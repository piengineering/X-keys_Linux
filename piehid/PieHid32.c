/****************************************
    X-Keys Interface

    Alan Ott
    Signal 11 Software
    under contract to P.I. Engineering
    2011-08-10

    This file is part of the X-Keys Library.

    The X-Keys library is free software: you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This software is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with The X-Keys library.  If not, see
    <http://www.gnu.org/licenses/>.
****************************************/

#include "PieHid32.h"
#include "hidapi.h"
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <signal.h>

#define BUFFER_LENGTH 5 /* number of reports in the buffer */
#define REPORT_SIZE 80   /* max size of a single report */

struct report {
	int length;
	char buffer[REPORT_SIZE];
};

struct pie_device {
	int handle;

	/* HIDAPI objects */
	hid_device *dev;
	char *path;
	
	/* PieHid Configuration Options */
	int suppress_duplicate_reports;
	int disable_data_callback;
	
	/* Thread Objects and data */
	pthread_t read_thread;
	pthread_t callback_thread;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	volatile int shutdown;
	
	/* Data Ring Buffer */
	struct report *buffer;
	int front_of_buffer; /* next report to give to the application */
	int back_of_buffer;  /* points to one slot after the last report read from hardware */

	/* The last report received */
	struct report last_report;

	/* Callbacks */
	PHIDDataEvent data_event_callback;
	PHIDErrorEvent error_event_callback;
};

static struct pie_device pie_devices[MAX_XKEY_DEVICES];

static int cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime, const struct pie_device *pd);
static int return_data(struct pie_device *pd, unsigned char *data);

struct device_map_entry {
    unsigned short vid;
    unsigned short pid;
    int interface_number;
    unsigned short usage_page;
    unsigned short usage;
};

static bool get_usage(unsigned short vid, unsigned short pid,
                      int interface_number,
                      unsigned short *usage_page,
                      unsigned short *usage);


unsigned int PIE_HID_CALL EnumeratePIE(long VID, TEnumHIDInfo *info, long *count)
{
	struct hid_device_info *cur;
	struct hid_device_info *hi;
	int i;

	/* Clear out the devices array */
	for (i = 0; i < MAX_XKEY_DEVICES; i++) {
		struct pie_device *pd = &pie_devices[i];
		if (pd->dev) {
			CloseInterface(i);
			hid_close(pd->dev);
		}
		free(pd->path);		
	}
	memset(&pie_devices, 0, sizeof(pie_devices));
	for (i = 0; i < MAX_XKEY_DEVICES; i++) {
		struct pie_device *pd = &pie_devices[i];
		pd->handle = i;
	}
	
		
	hi = hid_enumerate(0x0, 0x0);
	
	*count = 0;

	/* Pack the return data, and set up our pie_devices array. */
	cur = hi;
	while (cur && *count < MAX_XKEY_DEVICES) {
		if (cur->vendor_id != PI_VID) {
			printf("Continuing, vid: %hx pivid: %hx\n", cur->vendor_id, (unsigned short)PI_VID);
			cur = cur->next;
			continue;
		}
		
		/* Get the Usage and Usage Page from a table. This is because
		   it's not possible to get this information on all recent
		   versions of Linux without claiming an interface, and thus
		   severely disrupting the system. */
		unsigned short usage_page = -1;
		unsigned short usage = -1;
		bool res = get_usage(PI_VID, cur->product_id,
		                     cur->interface_number,
		                     &usage_page, &usage);
		if (!res) {
			usage_page = -1;
			usage = -1;
		}

		TEnumHIDInfo *inf = &info[*count];
		inf->PID = cur->product_id;
		inf->Usage = usage;
		inf->UP = usage_page;
		inf->readSize = 33;
		inf->writeSize = 36;
		strncpy(inf->DevicePath, cur->path, sizeof(inf->DevicePath));
		inf->DevicePath[sizeof(inf->DevicePath)-1] = '\0';
		inf->Handle = *count;
		inf->Version = cur->release_number;
		inf->ManufacturerString[0] = '\0';
		inf->ProductString[0] = '\0';

		struct pie_device *pd = &pie_devices[*count];
		pd->path = cur->path;
		(*count)++;
		cur = cur->next;
	}

	return 0;
}


unsigned int PIE_HID_CALL GetXKeyVersion(long hnd)
{
	return 0;
}


/* Make a timespec for the specified number of milliseconds in the future. */
static void make_timeout(struct timespec *ts, int milliseconds)
{
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_sec += milliseconds / 1000;
	ts->tv_nsec += (milliseconds % 1000) * 1000000; //convert to ns
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_nsec -= 1000000000L;
		ts->tv_sec += 1;
	}
}

static void cleanup_mutex(void *param)
{
	struct pie_device *pd = param;
	pthread_mutex_unlock(&pd->mutex);
}

static void *read_thread(void *param)
{
	struct pie_device *pd = param;
	char buf[80];
	buf[0] = 0x0;
	
	while (!pd->shutdown) {
		int res = hid_read(pd->dev, (unsigned char*)buf, sizeof(buf));
		if (res > 0) {
			int is_empty = 0;
			int wake_up_waiters = 0;
			int skip = 0;
			
			pthread_mutex_lock(&pd->mutex);
			pthread_cleanup_push(&cleanup_mutex, pd)
			
			/* Check if this is the same as the last report
			   received (ie: if it's a duplicate). */
			if (res == pd->last_report.length &&
			    memcmp(buf, pd->last_report.buffer, res) == 0)
			{
				if (pd->suppress_duplicate_reports)
					skip = 1;
			}
			
			if (!skip) {
				/* See if this is going into an empty buffer */
				if (pd->front_of_buffer == pd->back_of_buffer)
					wake_up_waiters = 1;
					
				/* Put this report at the end of the buffer
				   Add an extra byte at the beginning for the
				   report number. */
				int new_position = pd->back_of_buffer;
				struct report *rpt = &pd->buffer[new_position];
				memcpy(rpt->buffer+1, buf, res);
				rpt->length = res+1;

				/* Increment the back-of-buffer pointer, moving
				   the front-of-buffer pointer if we've overflowed. */
				new_position += 1;
				new_position %= BUFFER_LENGTH;
				if (new_position == pd->front_of_buffer) {
					/* Buffer is full. Lose the first one, and
					   consider the next one the front. */
					pd->front_of_buffer++;
					pd->front_of_buffer %= BUFFER_LENGTH;
				}
				pd->back_of_buffer = new_position;
				
				/* If the buffer was empty, wake up any waiting
				   threads which may be waiting on data. */
				if (wake_up_waiters) {
					pthread_cond_signal(&pd->cond);
				}
				
				/* Save this report as the last one received. */
				memcpy(pd->last_report.buffer, buf, res);
				pd->last_report.length = res;
			}

			pthread_mutex_unlock(&pd->mutex);
			pthread_cleanup_pop(0);
			
		}
		else if (res < 0) {
			/* An error occurred, possibly a device disconnect,
			   or the handle was closed from a different thread.
			   Break out of this loop and end this thread. */
			
			if (pd->error_event_callback) {
				pd->error_event_callback(pd->handle, PIE_HID_READ_BAD_INTERFACE_HANDLE);
			}
			
			/* Break out of this loop. */
			pd->shutdown = 1;
		}
	}

	/* Wake up anyone waiting on data. Do this under a mutex so that
	   any thread which may be about to sleep will actually go to sleep
	   before the broadcast is called here to wake them up. */
	pthread_mutex_lock(&pd->mutex);
	pthread_cond_broadcast(&pd->cond);
	pthread_mutex_unlock(&pd->mutex);
	
	return NULL;
}

static void *callback_thread(void *param)
{
	struct pie_device *pd = param;
	char buf[80];
	
	while (!pd->shutdown) {
		/* Wait for data to become available. */
		pthread_mutex_lock(&pd->mutex);
		pthread_cleanup_push(&cleanup_mutex, pd);
		while (pd->front_of_buffer == pd->back_of_buffer) {
			/* No data available. Sleep until there is. */
			int res = pthread_cond_wait(&pd->cond, &pd->mutex);
			if (res != 0) {
				if (pd->error_event_callback)
					pd->error_event_callback(pd->handle, PIE_HID_WRITE_UNABLE_TO_ACQUIRE_MUTEX);
				
				/* Something failed. Re-acquire the
				   mutex and try again. This is a pretty
				   serious error and will probably never
				   happen. */
				pthread_mutex_lock(&pd->mutex);
			}
			
			if (pd->shutdown)
				break;

			/* If we're here, then there is either data, or
			   there was a spurious wakeup, or there was an
			   error. Either way, try to run the loop again.
			   The loop will fall out if there is data. */
		}

		/* We came out of the wait, so there either data
		   available or a shutdown was called for. */

		if (!pd->shutdown && pd->data_event_callback && !pd->disable_data_callback) {
			if (pd->front_of_buffer != pd->back_of_buffer) {
				/* There is data available. Copy it to buf. */
				return_data(pd, buf);

				/* Call the callback. */
				pd->data_event_callback(buf, pd->handle, 0);
			}
		}

		pthread_mutex_unlock(&pd->mutex);
		pthread_cleanup_pop(0);
	}
	
	return NULL;
}

unsigned int PIE_HID_CALL SetupInterfaceEx(long hnd)
{
	int res;
	int ret_val = 0;
	
	if (hnd >= MAX_XKEY_DEVICES)
		return PIE_HID_SETUP_BAD_HANDLE;
	
	struct pie_device *pd = &pie_devices[hnd];

	/* Open the device */
	pd->dev = hid_open_path(pd->path);
	if (!pd->dev) {
		ret_val = PIE_HID_SETUP_CANNOT_OPEN_READ_HANDLE;
		goto err_open_path;
	}
	
	/* Create the buffer */
	pd->buffer = calloc(BUFFER_LENGTH, sizeof(struct report));
	if (!pd->buffer) {
		ret_val = PIE_HID_SETUP_CANNOT_ALLOCATE_MEM_FOR_RING;
		goto err_alloc_buffer;
	}
	
	/* Create the mutex */
	res = pthread_mutex_init(&pd->mutex, NULL);
	if (res != 0) {
		ret_val = PIE_HID_SETUP_CANNOT_CREATE_MUTEX;
		goto err_create_mutex;
	}
	
	/* Create the condition */
	res = pthread_cond_init(&pd->cond, NULL);
	if (res != 0) {
		ret_val = PIE_HID_SETUP_CANNOT_CREATE_MUTEX;
		goto err_create_cond;
	}
	
	/* Start the Read thread */
	res = pthread_create(&pd->read_thread, NULL, &read_thread, pd);
	if (res != 0) {
		ret_val = PIE_HID_SETUP_CANNOT_CREATE_READ_THREAD;
		goto err_create_read_thread;
	}

	/* Start the Callback thread */
	res = pthread_create(&pd->callback_thread, NULL, &callback_thread, pd);
	if (res != 0) {
		ret_val = PIE_HID_SETUP_CANNOT_CREATE_READ_THREAD;
		goto err_create_callback_thread;
	}
	
	/* Set Default parameters */
	pd->suppress_duplicate_reports = true;
	pd->disable_data_callback = false;
	
	return ret_val;
	
	
err_create_callback_thread:
	pd->shutdown = 1;
	pthread_join(pd->read_thread, NULL);
err_create_read_thread:
	pthread_cond_destroy(&pd->cond);
err_create_cond:
	pthread_mutex_destroy(&pd->mutex);
err_create_mutex:
	free(pd->buffer);
err_alloc_buffer:
	hid_close(pd->dev);
err_open_path:

	return ret_val;
}

void  PIE_HID_CALL CloseInterface(long hnd)
{
	if (hnd >= MAX_XKEY_DEVICES)
		return;
	
	struct pie_device *pd = &pie_devices[hnd];

	/* Stop the threads. pthread_cancel will stop the threads once
	   they get to a cancellation point. In this case the
	   pthread_cond_*wait() function (inside hid-libusb.c) is where
	   they will be cancelled. Once stopped, they will call their
	   respective cancellation handlers which will release the mutexes
	   if necessary. */
	pd->shutdown = 1;
	pthread_cancel(pd->callback_thread);
	pthread_cancel(pd->read_thread);

	/* Wait for the threds to stop */
	pthread_join(pd->callback_thread, NULL);
	pthread_join(pd->read_thread, NULL);
	
	/* Destroy the condition */
	pthread_cond_destroy(&pd->cond);

	/* Destroy the mutex */
	pthread_mutex_destroy(&pd->mutex);
	
	/* Close the device handle */
	hid_close(pd->dev);
	pd->dev = NULL;

	/* Free the buffer */
	free(pd->buffer);
	pd->buffer = NULL;
}

void  PIE_HID_CALL CleanupInterface(long hnd)
{
	CloseInterface(hnd);
}

static int return_data(struct pie_device *pd, unsigned char *data)
{
	/* Return the first report in the queue. */
	struct report *rpt = &pd->buffer[pd->front_of_buffer];
	memcpy(data, rpt->buffer, rpt->length);
	
	/* Increment the front of buffer pointer. */
	pd->front_of_buffer++;
	if (pd->front_of_buffer >= BUFFER_LENGTH)
		pd->front_of_buffer = 0;
}

unsigned int PIE_HID_CALL ReadData(long hnd, unsigned char *data)
{
	if (hnd >= MAX_XKEY_DEVICES)
		return PIE_HID_READ_BAD_INTERFACE_HANDLE;
	
	struct pie_device *pd = &pie_devices[hnd];
	
	pthread_mutex_lock(&pd->mutex);
	
	/* Return early if there is no data available */
	if (pd->front_of_buffer == pd->back_of_buffer) {
		/* No data available. */
		pthread_mutex_unlock(&pd->mutex);
		return PIE_HID_READ_INSUFFICIENT_DATA;
	}

	return_data(pd, data);

	pthread_mutex_unlock(&pd->mutex);
	
	return 0;	
}

static int cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime, const struct pie_device *pd)
{
	while (pd->front_of_buffer == pd->back_of_buffer && !pd->shutdown) {
		int res = pthread_cond_timedwait(cond, mutex, abstime);
		if (res == ETIMEDOUT)
			return res;
		if (res != 0)
			return res;
		/* A res of 0 means we may have been signaled or it may
		   be a spurious wakeup. Check to see that there's acutally
		   data in the queue before returning, and if not, go back
		   to sleep. See the pthread_cond_timedwait() man page for
		   details. */
	}
	
	return 0;
}

unsigned int PIE_HID_CALL BlockingReadData(long hnd, unsigned char *data, int maxMillis)
{
	if (hnd >= MAX_XKEY_DEVICES)
		return PIE_HID_READ_BAD_INTERFACE_HANDLE;
	
	struct pie_device *pd = &pie_devices[hnd];
	
	pthread_mutex_lock(&pd->mutex);
	
	/* Go to sleep if there is no data available */
	if (pd->front_of_buffer == pd->back_of_buffer) {
		/* No data available. Sleep until there is. */
		struct timespec abstime;
		make_timeout(&abstime, maxMillis);
		
		int res = cond_timedwait(&pd->cond, &pd->mutex, &abstime, pd);
		if (res == ETIMEDOUT) {
			pthread_mutex_unlock(&pd->mutex);
			return PIE_HID_READ_INSUFFICIENT_DATA;
		}
		else if (res != 0) {
			return PIE_HID_READ_CANNOT_ACQUIRE_MUTEX;
		}
	}

	/* If we got to this point, there is either data here or there was
	   an error. In either case, this thread is holding the mutex. */
	   
	if (pd->front_of_buffer != pd->back_of_buffer)
		return_data(pd, data);
        
	pthread_mutex_unlock(&pd->mutex);
	
	return 0;
}

unsigned int PIE_HID_CALL WriteData(long hnd, unsigned char *data)
{
	if (hnd >= MAX_XKEY_DEVICES)
		return PIE_HID_WRITE_BAD_HANDLE;
	
	struct pie_device *pd = &pie_devices[hnd];
	
	int res = hid_write(pd->dev, data, GetWriteLength(hnd));
	if (res < 0)
		return PIE_HID_WRITE_FAILED;
	if (res != GetWriteLength(hnd))
		return PIE_HID_WRITE_INCOMPLETE;
	
	return 0;
}

unsigned int PIE_HID_CALL FastWrite(long hnd, unsigned char *data)
{
	return WriteData(hnd, data);
}

unsigned int PIE_HID_CALL ReadLast(long hnd, unsigned char *data)
{
	if (hnd >= MAX_XKEY_DEVICES)
		return PIE_HID_READ_BAD_INTERFACE_HANDLE;

	struct pie_device *pd = &pie_devices[hnd];
	
	pthread_mutex_lock(&pd->mutex);

	/* If the buffer is empty, return insufficient data. */
	if (pd->front_of_buffer == pd->back_of_buffer) {
		pthread_mutex_unlock(&pd->mutex);
		return PIE_HID_READ_INSUFFICIENT_DATA;
	}
	
	/* Find the last item in the buffer. */
	int last = pd->back_of_buffer - 1;
	if (last < 0)
		last = BUFFER_LENGTH -1;
	
	/* Return the first report in the queue. */
	struct report *rpt = &pd->buffer[last];
	memcpy(data, rpt->buffer, rpt->length);
	
	pthread_mutex_unlock(&pd->mutex);
	
	return 0;
}

unsigned int PIE_HID_CALL ClearBuffer(long hnd)
{
	if (hnd >= MAX_XKEY_DEVICES)
		return PIE_HID_CLEARBUFFER_BAD_HANDLE;

	struct pie_device *pd = &pie_devices[hnd];

	pthread_mutex_lock(&pd->mutex);
	pd->front_of_buffer = 0;
	pd->back_of_buffer = 0;
	pthread_mutex_unlock(&pd->mutex);
}


unsigned int PIE_HID_CALL GetReadLength(long hnd)
{
	return 33;
}

unsigned int PIE_HID_CALL GetWriteLength(long hnd)
{
	return 36;
}
unsigned int PIE_HID_CALL SetDataCallback(long hnd, PHIDDataEvent pDataEvent)
{
	if (hnd >= MAX_XKEY_DEVICES)
		return PIE_HID_DATACALLBACK_BAD_HANDLE;
	
	struct pie_device *pd = &pie_devices[hnd];
	
	pd->data_event_callback = pDataEvent;	
	
	return 0;
}

unsigned int PIE_HID_CALL SetErrorCallback(long hnd, PHIDErrorEvent pErrorCall)
{
	if (hnd >= MAX_XKEY_DEVICES)
		return PIE_HID_ERRORCALLBACK_BAD_HANDLE;
	
	struct pie_device *pd = &pie_devices[hnd];
	
	pd->error_event_callback = pErrorCall;

	return 0;
}

void PIE_HID_CALL SuppressDuplicateReports(long hnd,bool supp)
{
	if (hnd >= MAX_XKEY_DEVICES)
		return;

	struct pie_device *pd = &pie_devices[hnd];
	
	pd->suppress_duplicate_reports = supp;
}

void PIE_HID_CALL DisableDataCallback(long hnd,bool disable)
{
	if (hnd >= MAX_XKEY_DEVICES)
		return;

	struct pie_device *pd = &pie_devices[hnd];
	
	pd->disable_data_callback = disable;
}

bool PIE_HID_CALL IsDataCallbackDisabled(long hnd)
{
	if (hnd >= MAX_XKEY_DEVICES)
		return false;

	struct pie_device *pd = &pie_devices[hnd];
	
	return pd->disable_data_callback;
}

bool PIE_HID_CALL GetSuppressDuplicateReports(long hnd)
{
	if (hnd >= MAX_XKEY_DEVICES)
		return false;
	
	struct pie_device *pd = &pie_devices[hnd];
	
	return pd->suppress_duplicate_reports;
}


void PIE_HID_CALL GetErrorString(int err, char* out_str, int size)
{
	const char *str = NULL;

	switch (err) {
	case PIE_HID_ENUMERATE_BAD_HID_DEVICE:
		str = "101 Bad HID device information set handle";
		break;
	case PIE_HID_ENUMERATE_NO_DEVICES_FOUND:
		str = "102 No devices found.";
		break;
	case PIE_HID_ENUMERATE_OTHER_ENUM_ERROR:
		str = "103 Bad error";
		break;
	case PIE_HID_ENUMERATE_ERROR_GETTING_DEVICE_DETAIL:
		str = "104 Error interface detail (required size)";
		break;
	case PIE_HID_ENUMERATE_ERROR_GETTING_DEVICE_DETATIL2:
		str = "105 Error getting device interface detail.";
		break;
	case PIE_HID_ENUMERATE_UNABLE_TO_OPEN_HANDLE:
		str = "106 CreateFile error.";
		break;
	case PIE_HID_ENUMERATE_GET_ATTRIBUTES_ERROR:
		str = "107 HidD_GetAttributes error";
		break;
	case PIE_HID_ENUMERATE_VENDOR_ID_ERROR:
		str = "108 VendorID not VID";
		break;
	case PIE_HID_ENUMERATE_GET_PREPARSED_DATA_ERROR:
		str = "109 HidD_GetPreparsedData error";
		break;
	case PIE_HID_ENUMERATE_GET_CAPS:
		str = "110 HidP_GetCaps error";
		break;
	case PIE_HID_ENUMERATE_GET_MANUFACTURER_STRING:
		str = "111 HidD_GetManufacturerString error";
		break;
	case PIE_HID_ENUMERATE_GET_PRODUCT_STRING:
		str = "112 HidD_GetProductString error";
		break;
	case PIE_HID_SETUP_BAD_HANDLE:
		str = "201 Bad interface handle";
		break;
	case PIE_HID_SETUP_CANNOT_ALLOCATE_MEM_FOR_RING:
		str = "202 Interface Already Set";
		break;
	case PIE_HID_SETUP_CANNOT_CREATE_MUTEX:
		str = "203 Cannot Create Mutex";
		break;
	case PIE_HID_SETUP_CANNOT_CREATE_READ_THREAD:
		str = "204 Cannot Create Read Thread";
		break;
	case PIE_HID_SETUP_CANNOT_OPEN_READ_HANDLE:
		str = "205 Cannot open read handle";
		break;
	case PIE_HID_SETUP_CANNOT_OPEN_READ_HANDLE_ACCESS_DENIED:
		str = "206 No read handle - Access Denied";
		break;
	case PIE_HID_SETUP_CANNOT_OPEN_READ_HANDLE_BAD_PATH:
		str = "207 No read handle - bad DevicePath"; 
		break;
	case PIE_HID_SETUP_CANNOT_OPEN_WRITE_HANDLE:
		str = "208 Cannot open write handle";
		break;
	case PIE_HID_SETUP_CANNOT_OPEN_WRITE_HANDLE_ACCESS_DENIED:
		str = "209 No write handle - Access Denied";
		break;
	case PIE_HID_SETUP_CANNOT_OPEN_WRITE_HANDLE_BAD_PATH:
		str = "210 No write handle - bad DevicePath"; 
		break;
	case PIE_HID_READ_BAD_INTERFACE_HANDLE:
		str = "301 Bad interface handle";
		break;
	case PIE_HID_READ_LENGTH_ZERO:
		str = "302 readSize is zero";
		break;
	case PIE_HID_READ_CANNOT_ACQUIRE_MUTEX:
		str = "303 Interface not valid";
		break;
	case PIE_HID_READ_INSUFFICIENT_DATA:
		str = "304 Ring buffer empty.";
		break;
	case PIE_HID_READ_CANNOT_RELEASE_MUTEX:
		str = "305 Cannot Release Mutex.";
		break;
	case PIE_HID_READ_CANNOT_RELEASE_MUTEX2:
		str = "306 Cannot Release Mutex.";
		break;
	case PIE_HID_READ_INVALID_HANDLE:
		str = "307 Invalid Handle.";
		break;
	case PIE_HID_READ_DEVICE_DISCONNECTED:
		str = "308 Device disconnected";
		break;
	case PIE_HID_READ_READ_ERROR:
		str = "309 Read error. ( unplugged )";
		break;
	case PIE_HID_READ_BYTES_NOT_EQUAL_READSIZE:
		str = "310 Bytes read not equal readSize";
		break;
	case PIE_HID_READ_BLOCKING_READ_DATA_TIMED_OUT:
		str = "311 BlockingReadData timed out.";
		break;
	case PIE_HID_WRITE_BAD_HANDLE:
		str = "401 Bad interface handle";
		break;
	case PIE_HID_WRITE_LENGTH_ZERO:
		str = "402 Write length is zero";
		break;
	case PIE_HID_WRITE_FAILED:
		str = "403 Bad internal interface handle ";
		break;
	case PIE_HID_WRITE_INCOMPLETE:
		str = "404 Write Incomplete";
		break;
	case PIE_HID_WRITE_UNABLE_TO_ACQUIRE_MUTEX:
		str = "405 No write buffer";
		break;
	case PIE_HID_WRITE_UNABLE_TO_RELEASE_MUTEX:
		str = "406 Write size equals zero";
		break;
	case PIE_HID_WRITE_HANDLE_INVALID:
		str = "407 No writeBuffer";
		break;
	case PIE_HID_WRITE_BUFFER_FULL:
		str = "408 Write buffer full";
		break;
	case PIE_HID_WRITE_PREV_WRITE_FAILED:
		str = "409 Previous Write Failed";
		break;
	case PIE_HID_WRITE_PREV_WRITE_WRONG_NUMBER:
		str = "410 byteCount != writeSize";
		break;
	case PIE_HID_WRITE_TIMER_FAILED:
		str = "411 Timed out in write.";
		break;
	case PIE_HID_WRITE_PREV_WRITE_UNABLE_TO_RELEASE_MUTEX:
		str = "412 Unable to Release Mutex";
		break;
	case PIE_HID_WRITE_BUFFER_FULL2:
		str = "413 Write Buffer Full";
		break;
	case PIE_HID_WRITE_FAST_WRITE_ERROR:
		str = "414 Fast Write Error";
		break;
	case PIE_HID_READLAST_BAD_HANDLE:
		str = "501 Bad interface handle";
		break;
	case PIE_HID_READLAST_LENGTH_ZERO:
		str = "502 Read length is zero";
		break;
	case PIE_HID_READLAST_UNABLE_TO_ACQUIRE_MUTEX:
		str = "503 Unable to acquire mutex";
		break;
	case PIE_HID_READLAST_INSUFFICIENT_DATA:
		str = "504 No data yet.";
		break;
	case PIE_HID_READLAST_UNABLE_TO_RELEASE_MUTEX:
		str = "505 Unable to release Mutex";
		break;
	case PIE_HID_READLAST_UNABLE_TO_RELEASE_MUTEX2:
		str = "506 Unable to release Mutex";
		break;
	case PIE_HID_READLAST_INVALID_HANDLE:
		str = "507 ReadLast() Invalid Handle";
		break;
	case PIE_HID_CLEARBUFFER_BAD_HANDLE:
		str = "601 Bad interface handle";
		break;
	case PIE_HID_CLEARBUFFER_UNABLE_TO_RELEASE_MUTEX:
		str = "602 Unable to release mutex";
		break;
	case PIE_HID_CLEARBUFFER_UNABLE_TO_ACQUIRE_MUTEX:
		str = "603 Unable to acquire mutex.";
		break;
	case PIE_HID_DATACALLBACK_BAD_HANDLE:
		str = "701 Bad interface handle";
		break;
	case PIE_HID_DATACALLBACK_INVALID_INTERFACE:
		str = "702 Interface not valid";
		break;
	case PIE_HID_DATACALLBACK_CANNOT_CREATE_CALLBACK_THREAD:
		str = "703 Could not create event thread.";
		break;
	case PIE_HID_DATACALLBACK_CALLBACK_ALREADY_SET:
		str = "704 Callback already set.";
		break;
	case PIE_HID_ERRORCALLBACK_BAD_HANDLE:
		str = "801 Bad interface handle";
		break;
	case PIE_HID_ERRORCALLBACK_INVALID_INTERFACE:
		str = "802 Interface not valid";
		break;
	case PIE_HID_ERRORCALLBACK_CANNOT_CREATE_ERROR_THREAD:
		str = "803 Could not create error thread.";
		break;
	case PIE_HID_ERRORCALLBACK_ERROR_THREAD_ALREADY_CREATED:
		str = "1804 Error thread already created";
		break;
	default:
		str = "Unknown error code";
		break;
	}

	strncpy(out_str, str, size);
	out_str[size-1] = '\0';
}


#define DEVICE_MAP_ENTRY(vid, pid, interface, usage_page, usage) \
    { vid, pid, interface, usage_page, usage,},

static const struct device_map_entry device_map[] = {
	DEVICE_MAP_ENTRY(PI_VID, 0x0405, 0, 0x000c, 0x0001) /* XK-24 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0405, 1, 0x0001, 0x0006) /* XK-24 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0405, 2, 0x0001, 0x0002) /* XK-24 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0404, 0, 0x000c, 0x0001) /* XK-24 splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0404, 1, 0x0001, 0x0006) /* XK-24 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0404, 2, 0x0001, 0x0004) /* XK-24 joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0404, 3, 0x0001, 0x0002) /* XK-24 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0403, 0, 0x000c, 0x0001) /* XK-24 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0403, 1, 0x0001, 0x0006) /* XK-24 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0403, 2, 0x0001, 0x0004) /* XK-24 joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0406, 0, 0x000c, 0x0001) /* Pi3 Matrix Board splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0406, 1, 0x0001, 0x0006) /* Pi3 Matrix Board keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0406, 2, 0x0001, 0x0002) /* Pi3 Matrix Board mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0407, 0, 0x000c, 0x0001) /* Pi3 Matrix Board splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0407, 1, 0x0001, 0x0006) /* Pi3 Matrix Board keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0407, 2, 0x0001, 0x0004) /* Pi3 Matrix Board joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0407, 3, 0x0001, 0x0002) /* Pi3 Matrix Board mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0408, 0, 0x000c, 0x0001) /* Pi3 Matrix Board splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0408, 1, 0x0001, 0x0006) /* Pi3 Matrix Board keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0408, 2, 0x0001, 0x0004) /* Pi3 Matrix Board joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0410, 0, 0x000c, 0x0001) /* MultiBoard 192 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0410, 1, 0x0001, 0x0006) /* MultiBoard 192 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0410, 2, 0x0001, 0x0002) /* MultiBoard 192 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0411, 0, 0x000c, 0x0001) /* MultiBoard 192 splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0411, 1, 0x0001, 0x0006) /* MultiBoard 192 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0411, 2, 0x0001, 0x0004) /* MultiBoard 192 joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0411, 3, 0x0001, 0x0002) /* MultiBoard 192 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0412, 0, 0x000c, 0x0001) /* MultiBoard 192 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0412, 1, 0x0001, 0x0006) /* MultiBoard 192 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0412, 2, 0x0001, 0x0004) /* MultiBoard 192 joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0413, 0, 0x000c, 0x0001) /* MultiBoard 256 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0413, 1, 0x0001, 0x0006) /* MultiBoard 256 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0413, 2, 0x0001, 0x0002) /* MultiBoard 256 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0414, 0, 0x000c, 0x0001) /* MultiBoard 256 splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0414, 1, 0x0001, 0x0006) /* MultiBoard 256 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0414, 2, 0x0001, 0x0004) /* MultiBoard 256 joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0414, 3, 0x0001, 0x0002) /* MultiBoard 256 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0415, 0, 0x000c, 0x0001) /* MultiBoard 256 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0415, 1, 0x0001, 0x0006) /* MultiBoard 256 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0415, 2, 0x0001, 0x0004) /* MultiBoard 256 joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0419, 0, 0x000c, 0x0001) /* XK-16 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0419, 1, 0x0001, 0x0006) /* XK-16 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0419, 2, 0x0001, 0x0002) /* XK-16 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x041A, 0, 0x000c, 0x0001) /* XK-16 splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x041A, 1, 0x0001, 0x0006) /* XK-16 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x041A, 2, 0x0001, 0x0004) /* XK-16 joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x041A, 3, 0x0001, 0x0002) /* XK-16 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x041B, 0, 0x000c, 0x0001) /* XK-16 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x041B, 1, 0x0001, 0x0006) /* XK-16 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x041B, 2, 0x0001, 0x0004) /* XK-16 joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x041F, 0, 0x000c, 0x0001) /* ShipDriver read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x041F, 1, 0x0001, 0x0006) /* ShipDriver keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x041F, 2, 0x0001, 0x0004) /* ShipDriver joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0423, 0, 0x000c, 0x0001) /* XK-128 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0423, 1, 0x0001, 0x0006) /* XK-128 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0423, 2, 0x0001, 0x0002) /* XK-128 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0424, 0, 0x000c, 0x0001) /* XK-128 splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0424, 1, 0x0001, 0x0006) /* XK-128 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0424, 2, 0x0001, 0x0004) /* XK-128 joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0424, 3, 0x0001, 0x0002) /* XK-128 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0425, 0, 0x000c, 0x0001) /* XK-128 read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0425, 1, 0x0001, 0x0006) /* XK-128 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0425, 2, 0x0001, 0x0004) /* XK-128 joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0426, 0, 0x000c, 0x0001) /* XK-12 Jog & Shuttle splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0426, 1, 0x0001, 0x0006) /* XK-12 Jog & Shuttle keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0426, 2, 0x0001, 0x0002) /* XK-12 Jog & Shuttle mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0427, 0, 0x000c, 0x0001) /* XK-12 Jog & Shuttle splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0427, 1, 0x0001, 0x0006) /* XK-12 Jog & Shuttle keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0427, 2, 0x0001, 0x0004) /* XK-12 Jog & Shuttle joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0427, 3, 0x0001, 0x0002) /* XK-12 Jog & Shuttle mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0428, 0, 0x000c, 0x0001) /* XK-12 Jog & Shuttle read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0428, 1, 0x0001, 0x0006) /* XK-12 Jog & Shuttle keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0428, 2, 0x0001, 0x0004) /* XK-12 Jog & Shuttle joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0429, 0, 0x000c, 0x0001) /* XK-12 Joystick splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0429, 1, 0x0001, 0x0006) /* XK-12 Joystick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0429, 2, 0x0001, 0x0004) /* XK-12 Joystick joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x042A, 0, 0x000c, 0x0001) /* XK-12 Joystick splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042A, 1, 0x0001, 0x0006) /* XK-12 Joystick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042A, 2, 0x0001, 0x0004) /* XK-12 Joystick joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042A, 3, 0x0001, 0x0002) /* XK-12 Joystick mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x042B, 0, 0x000c, 0x0001) /* XK-12 Joystick read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042B, 1, 0x0001, 0x0006) /* XK-12 Joystick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042B, 2, 0x0001, 0x0002) /* XK-12 Joystick mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x042C, 0, 0x000c, 0x0001) /* XK-3 Front Hinged Footpedal splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042C, 1, 0x0001, 0x0006) /* XK-3 Front Hinged Footpedal keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042C, 2, 0x0001, 0x0002) /* XK-3 Front Hinged Footpedal mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x042D, 0, 0x000c, 0x0001) /* XK-3 Front Hinged Footpedal splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042D, 1, 0x0001, 0x0006) /* XK-3 Front Hinged Footpedal keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042D, 2, 0x0001, 0x0004) /* XK-3 Front Hinged Footpedal joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042D, 3, 0x0001, 0x0002) /* XK-3 Front Hinged Footpedal mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x042E, 0, 0x000c, 0x0001) /* XK-3 Front Hinged Footpedal read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042E, 1, 0x0001, 0x0006) /* XK-3 Front Hinged Footpedal keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042E, 2, 0x0001, 0x0004) /* XK-3 Front Hinged Footpedal joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0432, 0, 0x000c, 0x0001) /* XK-12 Touch splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0432, 1, 0x0001, 0x0006) /* XK-12 Touch keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0432, 2, 0x0001, 0x0002) /* XK-12 Touch mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0433, 0, 0x000c, 0x0001) /* XK-12 Touch splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0433, 1, 0x0001, 0x0006) /* XK-12 Touch keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0433, 2, 0x0001, 0x0004) /* XK-12 Touch joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0433, 3, 0x0001, 0x0002) /* XK-12 Touch mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0434, 0, 0x000c, 0x0001) /* XK-12 Touch read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0434, 1, 0x0001, 0x0006) /* XK-12 Touch keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0434, 2, 0x0001, 0x0004) /* XK-12 Touch joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0435, 0, 0x000c, 0x0001) /* XK-12 Trackball splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0435, 1, 0x0001, 0x0006) /* XK-12 Trackball keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0435, 2, 0x0001, 0x0002) /* XK-12 Trackball mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0436, 0, 0x000c, 0x0001) /* XK-12 Trackball splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0436, 1, 0x0001, 0x0006) /* XK-12 Trackball keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0436, 2, 0x0001, 0x0004) /* XK-12 Trackball joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0436, 3, 0x0001, 0x0002) /* XK-12 Trackball mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0437, 0, 0x000c, 0x0001) /* XK-12 Trackball read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0437, 1, 0x0001, 0x0006) /* XK-12 Trackball keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0437, 2, 0x0001, 0x0004) /* XK-12 Trackball joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0438, 0, 0x000c, 0x0001) /* XK-3 Rear Hinged Footpedal splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0438, 1, 0x0001, 0x0006) /* XK-3 Rear Hinged Footpedal keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0438, 2, 0x0001, 0x0002) /* XK-3 Rear Hinged Footpedal mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0439, 0, 0x000c, 0x0001) /* XK-3 Rear Hinged Footpedal splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0439, 1, 0x0001, 0x0006) /* XK-3 Rear Hinged Footpedal keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0439, 2, 0x0001, 0x0004) /* XK-3 Rear Hinged Footpedal joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0439, 3, 0x0001, 0x0002) /* XK-3 Rear Hinged Footpedal mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x043A, 0, 0x000c, 0x0001) /* XK-3 Rear Hinged Footpedal splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x043A, 1, 0x0001, 0x0006) /* XK-3 Rear Hinged Footpedal keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x043A, 2, 0x0001, 0x0004) /* XK-3 Rear Hinged Footpedal joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x043B, 0, 0x000c, 0x0001) /* ADC-888 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x043B, 1, 0x0001, 0x0006) /* ADC-888 Touch keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x043B, 2, 0x0001, 0x0004) /* ADC-888 Touch joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x043C, 0, 0x000c, 0x0001) /* HiRes splat read and write*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0440, 0, 0x000c, 0x0001) /* Foxboro splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0440, 1, 0x0001, 0x0006) /* Foxboro keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0440, 2, 0x0001, 0x0002) /* Foxboro mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0441, 0, 0x000c, 0x0001) /* XK-80/60 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0441, 1, 0x0001, 0x0006) /* XK-80/60 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0441, 2, 0x0001, 0x0002) /* XK-80/60 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0442, 0, 0x000c, 0x0001) /* XK-80/60 splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0442, 1, 0x0001, 0x0006) /* XK-80/60 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0442, 2, 0x0001, 0x0004) /* XK-80/60 joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0442, 3, 0x0001, 0x0002) /* XK-80/60 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0443, 0, 0x000c, 0x0001) /* XK-80/60 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0443, 1, 0x0001, 0x0006) /* XK-80/60 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0443, 2, 0x0001, 0x0004) /* XK-80/60 joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0444, 0, 0x000c, 0x0001) /* LGZ splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0444, 1, 0x0001, 0x0006) /* LGZ keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0444, 2, 0x0001, 0x0002) /* LGZ mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0445, 0, 0x000c, 0x0001) /* LGZ splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0445, 1, 0x0001, 0x0006) /* LGZ keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0445, 2, 0x0001, 0x0004) /* LGZ joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0445, 3, 0x0001, 0x0002) /* LGZ mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0446, 0, 0x000c, 0x0001) /* LGZ splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0446, 1, 0x0001, 0x0006) /* LGZ keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0446, 2, 0x0001, 0x0004) /* LGZ joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0447, 0, 0x000c, 0x0001) /* PushPull splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0447, 1, 0x0001, 0x0006) /* PushPull keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0447, 2, 0x0001, 0x0002) /* PushPull mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0448, 0, 0x000c, 0x0001) /* PushPull splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0448, 1, 0x0001, 0x0006) /* PushPull keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0448, 2, 0x0001, 0x0004) /* PushPull joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0448, 3, 0x0001, 0x0002) /* PushPull mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0449, 0, 0x000c, 0x0001) /* PushPull splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0449, 1, 0x0001, 0x0006) /* PushPull keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0449, 2, 0x0001, 0x0004) /* PushPull joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x044A, 0, 0x000c, 0x0001) /* Bluetooth Encoder splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x044A, 1, 0x0001, 0x0006) /* Bluetooth Encoder keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x044A, 2, 0x0001, 0x0002) /* Bluetooth Encoder mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x044B, 0, 0x000c, 0x0001) /* Bluetooth Encoder splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x044B, 1, 0x0001, 0x0006) /* Bluetooth Encoder keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x044B, 2, 0x0001, 0x0004) /* Bluetooth Encoder joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x044B, 3, 0x0001, 0x0002) /* Bluetooth Encoder mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x044C, 0, 0x000c, 0x0001) /* Bluetooth Encoder splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x044C, 1, 0x0001, 0x0006) /* Bluetooth Encoder keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x044C, 2, 0x0001, 0x0004) /* Bluetooth Encoder joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x00F6, 0, 0x000c, 0x0001) /* VEC splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x00F7, 0, 0x000c, 0x0001) /* VEC Audiotranskription.de footpedal splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x00F8, 0, 0x000c, 0x0001) /* VEC sbs footpedal splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x00F9, 0, 0x000c, 0x0001) /* VEC dwx footpedal splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x00FA, 0, 0x000c, 0x0001) /* VEC spx footpedal splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x00FB, 0, 0x000c, 0x0001) /* VEC dac footpedal splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x00FC, 0, 0x000c, 0x0001) /* VEC srw footpedal splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x00FD, 0, 0x000c, 0x0001) /* VEC dictation (VIS) splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x00FE, 0, 0x000c, 0x0001) /* VEC dvi footpedal splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x00FF, 0, 0x000c, 0x0001) /* VEC footpedal splat read and write*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0268, 0, 0x000c, 0x0001) /* Footpedal SE splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0261, 0, 0x000c, 0x0001) /* Matrix/Footpedal/SI SE splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0271, 0, 0x000c, 0x0001) /* Stick SE splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0281, 0, 0x000c, 0x0001) /* Desktop SE splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0291, 0, 0x000c, 0x0001) /* Professional SE splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0241, 0, 0x000c, 0x0001) /* Jog & Shuttle SE splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0251, 0, 0x000c, 0x0001) /* Joystick Pro SE splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0261, 0, 0x000c, 0x0001) /* Pendant SE splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0269, 0, 0x000c, 0x0001) /* Switch Interface SE splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0304, 0, 0x000c, 0x0001) /* Button Panel SE splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x026A, 0, 0x000c, 0x0001) /* Matrix Board SE splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0305, 0, 0x000c, 0x0001) /* 128 w Mag Strip splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0306, 0, 0x000c, 0x0001) /* 128 no reader splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0307, 0, 0x000c, 0x0001) /* 128 w Bar Code splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0308, 0, 0x000c, 0x0001) /* 84 w Mag Strip splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0309, 0, 0x000c, 0x0001) /* 84 no reader splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x030A, 0, 0x000c, 0x0001) /* 84 w Bar Code splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0301, 0, 0x000c, 0x0001) /* LCD w Mag Strip splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0302, 0, 0x000c, 0x0001) /* LCD no reader splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0303, 0, 0x000c, 0x0001) /* LCD w Bar Code splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x00D9, 0, 0x000c, 0x0001) /* ReDAC IO Module splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x00D2, 0, 0x000c, 0x0001) /* Raildriver splat read and write*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x02B5, 0, 0x000c, 0x0001) /* Stick MWII splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02B6, 1, 0x0001, 0x0006) /* Stick MWII keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02B6, 2, 0x0001, 0x0002) /* Stick MWII mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x02A5, 0, 0x000c, 0x0001) /* Desktop MWII splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02A6, 1, 0x0001, 0x0006) /* Desktop MWII keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02A6, 2, 0x0001, 0x0002) /* Desktop MWII mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x02A7, 0, 0x000c, 0x0001) /* Professional MWII splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02A8, 1, 0x0001, 0x0006) /* Professional MWII keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02A8, 2, 0x0001, 0x0002) /* Professional MWII mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x02B1, 0, 0x000c, 0x0001) /* Jog & Shuttle MWII splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02B2, 1, 0x0001, 0x0006) /* Jog & Shuttle MWII keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02B2, 2, 0x0001, 0x0002) /* Jog & Shuttle MWII mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x02B7, 0, 0x000c, 0x0001) /* Switch Interface MWII splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02B8, 1, 0x0001, 0x0006) /* Switch Interface MWII keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02B8, 2, 0x0001, 0x0002) /* Switch Interface MWII mouse*/
};

static bool get_usage(unsigned short vid, unsigned short pid,
                      int interface_number,
                      unsigned short *usage_page,
                      unsigned short *usage)
{
	size_t num = sizeof(device_map) / sizeof(*device_map);
	size_t i;

	for (i = 0; i < num; i++) {
		const struct device_map_entry *dev = &device_map[i];
		
		if (dev->vid == vid &&
		    dev->pid == pid &&
		    dev->interface_number == interface_number)
		{
			*usage_page = dev->usage_page;
			*usage = dev->usage;
			return true;
		}
	}
	
	return false;
}
