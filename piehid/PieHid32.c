/****************************************
 X-Keys Interface

 TODO LICENSE HERE
 
 Alan Ott
 Signal 11 Software
 under contract to P.I. Engineering
 2011-08-10
 2017-10-24 v2 Patti
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
	int pid; //patti
	int interfacenumber; //patti

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
    int readlength;
    int writelength;
};

static bool get_usage(unsigned short vid, unsigned short pid,
                      int interface_number,
                      unsigned short *usage_page,
                      unsigned short *usage,
                      int *readlength,
		      int *writelength);


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
		   severely disrupting the system. Patti adding Read and Write sizes to this lookup.*/
		unsigned short usage_page = -1;
		unsigned short usage = -1;
		int readlength = -1;
		int writelength = -1;
		bool res = get_usage(PI_VID, cur->product_id,
		                     cur->interface_number,
		                     &usage_page, &usage, &readlength, &writelength);
		if (!res) {
			usage_page = -1;
			usage = -1;
    			readlength = -1;
			writelength = -1;
		}

		TEnumHIDInfo *inf = &info[*count];
		inf->PID = cur->product_id;
		inf->Usage = usage;
		inf->UP = usage_page;
		inf->readSize = readlength; //37; 
		inf->writeSize = writelength; //36;
		strncpy(inf->DevicePath, cur->path, sizeof(inf->DevicePath));
		inf->DevicePath[sizeof(inf->DevicePath)-1] = '\0';
		inf->Handle = *count;
		inf->Version = cur->release_number;
		inf->ManufacturerString[0] = '\0';
		inf->ProductString[0] = '\0';

		const char *str = NULL;
		str = "P.I. Engineering";
		strncpy(inf->ManufacturerString, str, 128);
		inf->ManufacturerString[128-1] = '\0';
		
		GetProductString(inf->PID, inf->ProductString);
		inf->ProductString[128-1] = '\0';

		struct pie_device *pd = &pie_devices[*count];
		pd->path = cur->path;
		pd->pid = cur->product_id; //patti
		pd->interfacenumber = cur->interface_number; //patti
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
	hid_close(pd->dev); //this causes crash if no input endpoint
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

unsigned int PIE_HID_CALL GetReadLength(long hnd) //patti changed 10/24/17
{
	struct pie_device *pd = &pie_devices[hnd];
        int pid = pd->pid;
	int interface1 = pd->interfacenumber;

	unsigned short usage_page = -1;
	unsigned short usage = -1;
	int readlength = -1;
	int writelength = -1;
	bool res = get_usage(PI_VID, pid,
		                     interface1,
		                     &usage_page, &usage, &readlength, &writelength);
	if (!res) {
		usage_page = -1;
		usage = -1;
    		readlength = -1;
		writelength = -1;
	}
	
	return readlength; //depends on product. For XK-24 this is 33
}

unsigned int PIE_HID_CALL GetWriteLength(long hnd) //patti changed 10/24/17
{
	struct pie_device *pd = &pie_devices[hnd];
        int pid = pd->pid;
	int interface1 = pd->interfacenumber;

	unsigned short usage_page = -1;
	unsigned short usage = -1;
	int readlength = -1;
	int writelength = -1;
	bool res = get_usage(PI_VID, pid,
		                     interface1,
		                     &usage_page, &usage, &readlength, &writelength);
	if (!res) {
		usage_page = -1;
		usage = -1;
    		readlength = -1;
		writelength = -1;
	}
        
	return writelength;
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

void PIE_HID_CALL GetProductString(int Pid, char* out_str)
{
	const char *str = NULL;

	switch (Pid) {
	case 0x0405:
		str = "XK-24";
		break;
	case 0x0404:
		str = "XK-24";
		break;
	case 0x0403:
		str = "XK-24";
		break;
	case 0x04E1:
		str = "XK-24";
		break;
	case 0x0406:
		str = "Matrix Encoder Board";
		break;
	case 0x0407:
		str = "Matrix Encoder Board";
		break;
	case 0x0408:
		str = "Matrix Encoder Board";
		break;
	case 0x04E7:
		str = "Matrix Encoder Board";
		break;
	case 0x0419:
		str = "XK-16 Stick";
		break;
	case 0x041A:
		str = "XK-16 Stick";
		break;
	case 0x041B:
		str = "XK-16 Stick";
		break;
	case 0x04E3:
		str = "XK-16 Stick";
		break;
	case 0x0467:
		str = "XK-4 Stick";
		break;
	case 0x0468:
		str = "XK-4 Stick";
		break;
	case 0x0469:
		str = "XK-4 Stick";
		break;
	case 0x04E5:
		str = "XK-4 Stick";
		break;
	case 0x046A:
		str = "XK-8 Stick";
		break;
	case 0x046B:
		str = "XK-8 Stick";
		break;
	case 0x046C:
		str = "XK-8 Stick";
		break;
	case 0x04E4:
		str = "XK-8 Stick";
		break;
	case 0x04CB:
		str = "XK-128";
		break;
	case 0x04CC:
		str = "XK-128";
		break;
	case 0x04CD:
		str = "XK-128";
		break;
	case 0x04CE:
		str = "XK-128";
		break;
	case 0x0426:
		str = "XK-12 Jog & Shuttle";
		break;
	case 0x0427:
		str = "XK-12 Jog & Shuttle";
		break;
	case 0x0428:
		str = "XK-12 Jog & Shuttle";
		break;
	case 0x045A:
		str = "XK-68 Jog & Shuttle";
		break;
	case 0x045B:
		str = "XK-68 Jog & Shuttle";
		break;
	case 0x045C:
		str = "XK-68 Jog & Shuttle";
		break;
	case 0x0429:
		str = "XK-12 Joystick";
		break;
	case 0x042A:
		str = "XK-12 Joystick";
		break;
	case 0x042B:
		str = "XK-12 Joystick";
		break;
	case 0x045D:
		str = "XK-68 Joystick";
		break;
	case 0x045E:
		str = "XK-68 Joystick";
		break;
	case 0x045F:
		str = "XK-68 Joystick";
		break;
	case 0x042C:
		str = "XK-3 Footpedal";
		break;
	case 0x042D:
		str = "XK-3 Footpedal";
		break;
	case 0x042E:
		str = "XK-3 Footpedal";
		break;
	case 0x0432:
		str = "XK-12 Touch";
		break;
	case 0x0433:
		str = "XK-12 Touch";
		break;
	case 0x0434:
		str = "XK-12 Touch";
		break;
	case 0x0438:
		str = "XK-3 Footpedal";
		break;
	case 0x0439:
		str = "XK-3 Footpedal";
		break;
	case 0x043A:
		str = "XK-3 Footpedal";
		break;
	case 0x04E8:
		str = "XK-3 Footpedal";
		break;
	case 0x0441:
		str = "XK-80";
		break;
	case 0x0442:
		str = "XK-80";
		break;
	case 0x0443:
		str = "XK-80";
		break;
	case 0x04E2:
		str = "XK-80";
		break;
	case 0x0461:
		str = "XK-60";
		break;
	case 0x0462:
		str = "XK-60";
		break;
	case 0x0463:
		str = "XK-60";
		break;
	case 0x04E6:
		str = "XK-60";
		break;
	case 0x0268:
		str = "Footpedal SE";
		break;
	case 0x026A:
		str = "Matrix SE";
		break;
	case 0x0271:
		str = "Stick SE";
		break;
	case 0x0281:
		str = "Desktop SE";
		break;
	case 0x0291:
		str = "Professional SE";
		break;
	case 0x0241:
		str = "Jog and Shuttle SE";
		break;
	case 0x0251:
		str = "Joystick Pro SE";
		break;
	case 0x0269:
		str = "Switch Interface SE";
		break;
	case 0x0305:
		str = "128 w Mag Strip";
		break;
	case 0x0306:
		str = "128 w No Reader";
		break;
	case 0x0307:
		str = "128 w Bar Code";
		break;
	case 0x0308:
		str = "84 w Mag Strip";
		break;
	case 0x0309:
		str = "84 w No Reader";
		break;
	case 0x030A:
		str = "84 w Bar Code";
		break;
	case 0x0301:
		str = "LCD w Mag Strip";
		break;
	case 0x0302:
		str = "LCD w No Reader";
		break;
	case 0x0303:
		str = "LCD w Bar Code";
		break;
	case 0x00D2:
		str = "RailDriver";
		break;
	case 0x02B5:
		str = "Stick MWII";
		break;
	case 0x02B6:
		str = "Stick MWII";
		break;
	case 0x02A5:
		str = "Desktop MWII";
		break;
	case 0x02A6:
		str = "Desktop MWII";
		break;
	case 0x02A7:
		str = "Professional MWII";
		break;
	case 0x02A8:
		str = "Professional MWII";
		break;
	case 0x02B1:
		str = "Jog and Shuttle MWII";
		break;
	case 0x02B2:
		str = "Jog and Shuttle MWII";
		break;
	case 0x02B7:
		str = "Switch Interface MWII";
		break;
	case 0x02B8:
		str = "Switch Interface MWII";
		break;
	case 0x04C5:
		str = "XK-3 Switch Interface";
		break;
	case 0x04C6:
		str = "XK-3 Switch Interface";
		break;
	case 0x04C7:
		str = "XK-3 Switch Interface";
		break;
	case 0x04C8:
		str = "XK-3 Switch Interface";
		break;
	case 0x04A8:
		str = "XK-12 Switch Interface";
		break;
	case 0x04A9:
		str = "XK-12 Switch Interface";
		break;
	case 0x04AA:
		str = "XK-12 Switch Interface";
		break;
	case 0x04AB:
		str = "XK-12 Switch Interface";
		break;
	case 0x04FF:
		str = "XKR-32 Rack Mount";
		break;
	case 0x0500:
		str = "XKR-32 Rack Mount";
		break;
	case 0x0501:
		str = "XKR-32 Rack Mount";
		break;
	case 0x0502:
		str = "XKR-32 Rack Mount";
		break;
	case 0x04AC:
		str = "Pi4 Matrix Board";
		break;
	case 0x04AD:
		str = "Pi4 Matrix Board";
		break;
	case 0x04AE:
		str = "Pi4 Matrix Board";
		break;
	case 0x04AF:
		str = "Pi4 Matrix Board";
		break;
	case 0x04B0:
		str = "Pi4 Foot Pedal";
		break;
	case 0x04B1:
		str = "Pi4 Foot Pedal";
		break;
	case 0x04B2:
		str = "Pi4 Foot Pedal";
		break;
	case 0x04B3:
		str = "Pi4 Foot Pedal";
		break;
	case 0x04B4:
		str = "RS485";
		break;
	case 0x04B5:
		str = "RS485";
		break;
	case 0x04B6:
		str = "RS485";
		break;
	case 0x04B7:
		str = "RS485";
		break;
	case 0x049C:
		str = "XK-24 Android";
		break;
	case 0x049D:
		str = "XK-24 Android";
		break;
	case 0x049E:
		str = "XK-24 Android";
		break;
	case 0x049F:
		str = "XK-24 Android";
		break;
	case 0x04C1:
		str = "XK-80 Android";
		break;
	case 0x04C2:
		str = "XK-80 Android";
		break;
	case 0x04C3:
		str = "XK-80 Android";
		break;
	case 0x04C4:
		str = "XK-80 Android";
		break;
	case 0x04CF:
		str = "XK-60 Android";
		break;
	case 0x04D0:
		str = "XK-60 Android";
		break;
	case 0x04D1:
		str = "XK-60 Android";
		break;
	case 0x04D2:
		str = "XK-60 Android";
		break;
	case 0x04BD:
		str = "XK-16 Stick Android";
		break;
	case 0x04BE:
		str = "XK-16 Stick Android";
		break;
	case 0x04BF:
		str = "XK-16 Stick Android";
		break;
	case 0x04C0:
		str = "XK-16 Stick Android";
		break;
	case 0x04DC:
		str = "IAB-HD15-Wire Interface";
		break;
	case 0x04DD:
		str = "IAB-HD15-Wire Interface";
		break;
	case 0x04DE:
		str = "IAB-HD15-Wire Interface";
		break;
	case 0x04DF:
		str = "IAB-HD15-Wire Interface";
		break;
	case 0x04FB:
		str = "XK-124 Tbar";
		break;
	case 0x04FC:
		str = "XK-124 Tbar";
		break;
	case 0x04FD:
		str = "XK-124 Tbar";
		break;
	case 0x04FE:
		str = "XK-124 Tbar";
		break;
	case 0x04E9:
		str = "XC-RS232-DB9";
		break;
	case 0x04EA:
		str = "XC-RS232-DB9";
		break;
	case 0x04EB:
		str = "XC-RS232-DB9";
		break;
	case 0x04EC:
		str = "XC-RS232-DB9";
		break;
	case 0x04C9:
		str = "XC-DMX512-RJ45";
		break;
	case 0x052C:
		str = "XC-DMX512-ST";
		break;
	case 0x04D3:
		str = "XK-24 KVM";
		break;
	case 0x04D4:
		str = "XK-24 KVM";
		break;
	case 0x04D5:
		str = "XK-80 KVM";
		break;
	case 0x04D6:
		str = "XK-80 KVM";
		break;
	case 0x04D7:
		str = "XK-60 KVM";
		break;
	case 0x04D8:
		str = "XK-60 KVM";
		break;
	case 0x04F5:
		str = "XK-16 KVM";
		break;
	case 0x04F6:
		str = "XK-16 KVM";
		break;
	case 0x0503:
		str = "XKR-32 Rack Mount KVM";
		break;
	case 0x0504:
		str = "XKR-32 Rack Mount KVM";
		break;
	case 0x0514:
		str = "XK-3 Switch Interface KVM";
		break;
	case 0x0515:
		str = "XK-3 Switch Interface KVM";
		break;
	case 0x0516:
		str = "XK-12 Switch Interface KVM";
		break;
	case 0x0517:
		str = "XK-12 Switch Interface KVM";
		break;
	case 0x050A:
		str = "XK-128 KVM";
		break;
	case 0x050B:
		str = "XK-128 KVM";
		break;
	case 0x0524:
		str = "XK-16 LCD";
		break;
	case 0x0525:
		str = "XK-16 LCD";
		break;
	case 0x0526:
		str = "XK-16 LCD";
		break;
	case 0x0527:
		str = "XK-16 LCD";
		break;
	case 0x0528:
		str = "XK-16 LCD";
		break;
	case 0x0529:
		str = "XK-16 LCD";
		break;
	case 0x052A:
		str = "XK-16 LCD";
		break;
	case 0x052B:
		str = "XK-16 LCD (KVM)";
		break;
	default:
		str = "Unknown product";
		break;
	}

	strncpy(out_str, str, 128);
	out_str[128-1] = '\0';
}


#define DEVICE_MAP_ENTRY(vid, pid, interface, usage_page, usage, readlength, writelength) \
    { vid, pid, interface, usage_page, usage, readlength, writelength,},

static const struct device_map_entry device_map[] = {
	DEVICE_MAP_ENTRY(PI_VID, 0x0405, 0, 0x000c, 0x0001, 0x0021, 0x0024) /* XK-24 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0405, 1, 0x0001, 0x0006, 0x0009, 0x0002) /* XK-24 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0405, 2, 0x0001, 0x0002, 0x0006, 0x0000) /* XK-24 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0404, 0, 0x000c, 0x0001, 0x0000, 0x0024) /* XK-24 splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0404, 1, 0x0001, 0x0006, 0x0009, 0x0002) /* XK-24 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0404, 2, 0x0001, 0x0004, 0x000C, 0x0000) /* XK-24 joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0404, 3, 0x0001, 0x0002, 0x0006, 0x0000) /* XK-24 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0403, 0, 0x000c, 0x0001, 0x0021, 0x0024) /* XK-24 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0403, 1, 0x0001, 0x0006, 0x0009, 0x0002) /* XK-24 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0403, 2, 0x0001, 0x0004, 0x000C, 0x0000) /* XK-24 joystick*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04E1, 0, 0x000c, 0x0001, 0, 36) /* XK-24 splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E1, 1, 0x0001, 0x0006, 9, 2) /* XK-24 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E1, 2, 0x000c, 0x0001, 3, 0) /* XK-24 multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E1, 3, 0x0001, 0x0080, 2, 0) /* XK-24 multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E1, 4, 0x0001, 0x0002, 6, 0) /* XK-24 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0406, 0, 0x000c, 0x0001, 33, 36) /* Pi3 Matrix Board splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0406, 1, 0x0001, 0x0006, 9, 2) /* Pi3 Matrix Board keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0406, 2, 0x0001, 0x0002, 6, 0) /* Pi3 Matrix Board mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0407, 0, 0x000c, 0x0001, 0, 36) /* Pi3 Matrix Board splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0407, 1, 0x0001, 0x0006, 9, 2) /* Pi3 Matrix Board keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0407, 2, 0x0001, 0x0004, 12, 0) /* Pi3 Matrix Board joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0407, 3, 0x0001, 0x0002, 6, 0) /* Pi3 Matrix Board mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0408, 0, 0x000c, 0x0001, 33, 36) /* Pi3 Matrix Board splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0408, 1, 0x0001, 0x0006, 9, 2) /* Pi3 Matrix Board keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0408, 2, 0x0001, 0x0004, 12, 0) /* Pi3 Matrix Board joystick*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04E7, 0, 0x000c, 0x0001, 0, 36) /* Pi3 Matrix Board splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E7, 1, 0x0001, 0x0006, 9, 2) /* Pi3 Matrix Board keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E7, 2, 0x000c, 0x0001, 3, 0) /* Pi3 Matrix Board multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E7, 3, 0x0001, 0x0080, 2, 0) /* Pi3 Matrix Board multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E7, 4, 0x0001, 0x0002, 6, 0) /* Pi3 Matrix Board mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0419, 0, 0x000c, 0x0001, 33, 36) /* XK-16 Stick splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0419, 1, 0x0001, 0x0006, 9, 2) /* XK-16 Stick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0419, 2, 0x0001, 0x0002, 6, 0) /* XK-16 Stick mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x041A, 0, 0x000c, 0x0001, 0, 36) /* XK-16 Stick splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x041A, 1, 0x0001, 0x0006, 9, 2) /* XK-16 Stick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x041A, 2, 0x0001, 0x0004, 12, 0) /* XK-16 Stick joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x041A, 3, 0x0001, 0x0002, 6, 0) /* XK-16 Stick mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x041B, 0, 0x000c, 0x0001, 33, 36) /* XK-16 Stick splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x041B, 1, 0x0001, 0x0006, 9, 2) /* XK-16 Stick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x041B, 2, 0x0001, 0x0004, 12, 0) /* XK-16 Stick joystick*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04E3, 0, 0x000c, 0x0001, 0, 36) /* XK-16 Stick splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E3, 1, 0x0001, 0x0006, 9, 2) /* XK-16 Stick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E3, 2, 0x000c, 0x0001, 3, 0) /* XK-16 Stick multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E3, 3, 0x0001, 0x0080, 2, 0) /* XK-16 Stick multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E3, 4, 0x0001, 0x0002, 6, 0) /* XK-16 Stick mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x0467, 0, 0x000c, 0x0001, 33, 36) /* XK-4 Stick splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0467, 1, 0x0001, 0x0006, 9, 2) /* XK-4 Stick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0467, 2, 0x0001, 0x0002, 6, 0) /* XK-4 Stick mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0468, 0, 0x000c, 0x0001, 0, 36) /* XK-4 Stick splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0468, 1, 0x0001, 0x0006, 9, 2) /* XK-4 Stick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0468, 2, 0x0001, 0x0004, 12, 0) /* XK-4 Stick joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0468, 3, 0x0001, 0x0002, 6, 0) /* XK-4 Stick mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0469, 0, 0x000c, 0x0001, 33, 36) /* XK-4 Stick splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0469, 1, 0x0001, 0x0006, 9, 2) /* XK-4 Stick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0469, 2, 0x0001, 0x0004, 12, 0) /* XK-4 Stick joystick*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04E5, 0, 0x000c, 0x0001, 0, 36) /* XK-4 Stick splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E5, 1, 0x0001, 0x0006, 9, 2) /* XK-4 Stick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E5, 2, 0x000c, 0x0001, 3, 0) /* XK-4 Stick multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E5, 3, 0x0001, 0x0080, 2, 0) /* XK-4 Stick multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E5, 4, 0x0001, 0x0002, 6, 0) /* XK-4 Stick mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x046A, 0, 0x000c, 0x0001, 33, 36) /* XK-8 Stick splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x046A, 1, 0x0001, 0x0006, 9, 2) /* XK-8 Stick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x046A, 2, 0x0001, 0x0002, 6, 0) /* XK-8 Stick mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x046B, 0, 0x000c, 0x0001, 0, 36) /* XK-8 Stick splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x046B, 1, 0x0001, 0x0006, 9, 2) /* XK-8 Stick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x046B, 2, 0x0001, 0x0004, 12, 0) /* XK-8 Stick joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x046B, 3, 0x0001, 0x0002, 6, 0) /* XK-8 Stick mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x046C, 0, 0x000c, 0x0001, 33, 36) /* XK-8 Stick splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x046C, 1, 0x0001, 0x0006, 9, 2) /* XK-8 Stick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x046C, 2, 0x0001, 0x0004, 12, 0) /* XK-8 Stick joystick*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04E4, 0, 0x000c, 0x0001, 0, 36) /* XK-8 Stick splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E4, 1, 0x0001, 0x0006, 9, 2) /* XK-8 Stick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E4, 2, 0x000c, 0x0001, 3, 0) /* XK-8 Stick multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E4, 3, 0x0001, 0x0080, 2, 0) /* XK-8 Stick multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E4, 4, 0x0001, 0x0002, 6, 0) /* XK-8 Stick mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x04CB, 0, 0x000c, 0x0001, 37, 36) /* XK-128 read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04CB, 1, 0x0001, 0x0004, 12, 0) /* XK-128 joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04CB, 2, 0x0001, 0x0002, 7, 0) /* XK-128 mouse*/
		 
	DEVICE_MAP_ENTRY(PI_VID, 0x04CC, 0, 0x000c, 0x0001, 0, 36) /* XK-128 splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04CC, 1, 0x0001, 0x0006, 9, 2) /* XK-128 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04CC, 2, 0x000c, 0x0001, 3, 0) /* XK-128 multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04CC, 3, 0x0001, 0x0080, 2, 0) /* XK-128 multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04CC, 4, 0x0001, 0x0002, 7, 0) /* XK-128 mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04CD, 0, 0x000c, 0x0001, 0, 36) /* XK-128 splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04CD, 1, 0x0001, 0x0004, 12, 0) /* XK-128 joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04CD, 2, 0x000c, 0x0001, 3, 0) /* XK-128 multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04CD, 3, 0x0001, 0x0080, 2, 0) /* XK-128 multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04CD, 4, 0x0001, 0x0002, 7, 0) /* XK-128 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x04CE, 0, 0x000c, 0x0001, 37, 36) /* XK-128 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04CE, 1, 0x0001, 0x0006, 9, 2) /* XK-128 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04CE, 2, 0x0001, 0x0002, 7, 0) /* XK-128 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0426, 0, 0x000c, 0x0001, 33, 36) /* XK-12 Jog & Shuttle splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0426, 1, 0x0001, 0x0006, 9, 2) /* XK-12 Jog & Shuttle keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0426, 2, 0x0001, 0x0002, 6, 0) /* XK-12 Jog & Shuttle mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0427, 0, 0x000c, 0x0001, 0, 36) /* XK-12 Jog & Shuttle splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0427, 1, 0x0001, 0x0006, 9, 2) /* XK-12 Jog & Shuttle keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0427, 2, 0x0001, 0x0004, 12, 0) /* XK-12 Jog & Shuttle joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0427, 3, 0x0001, 0x0002, 6, 0) /* XK-12 Jog & Shuttle mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0428, 0, 0x000c, 0x0001, 33, 36) /* XK-12 Jog & Shuttle read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0428, 1, 0x0001, 0x0006, 9, 2) /* XK-12 Jog & Shuttle keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0428, 2, 0x0001, 0x0004, 12, 0) /* XK-12 Jog & Shuttle joystick*/

	DEVICE_MAP_ENTRY(PI_VID, 0x045A, 0, 0x000c, 0x0001, 33, 36) /* XK-68 Jog & Shuttle splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x045A, 1, 0x0001, 0x0006, 9, 2) /* XK-68 Jog & Shuttle keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x045A, 2, 0x0001, 0x0002, 6, 0) /* XK-68 Jog & Shuttle mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x045B, 0, 0x000c, 0x0001, 0, 36) /* XK-68 Jog & Shuttle splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x045B, 1, 0x0001, 0x0006, 9, 2) /* XK-68 Jog & Shuttle keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x045B, 2, 0x0001, 0x0004, 12, 0) /* XK-68 Jog & Shuttle joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x045B, 3, 0x0001, 0x0002, 6, 0) /* XK-68 Jog & Shuttle mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x045C, 0, 0x000c, 0x0001, 33, 36) /* XK-68 Jog & Shuttle read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x045C, 1, 0x0001, 0x0006, 9, 2) /* XK-68 Jog & Shuttle keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x045C, 2, 0x0001, 0x0004, 12, 0) /* XK-68 Jog & Shuttle joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0429, 0, 0x000c, 0x0001, 33, 36) /* XK-12 Joystick splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0429, 1, 0x0001, 0x0006, 9, 2) /* XK-12 Joystick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0429, 2, 0x0001, 0x0004, 12, 0) /* XK-12 Joystick joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x042A, 0, 0x000c, 0x0001, 33, 36) /* XK-12 Joystick splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042A, 1, 0x0001, 0x0006, 9, 2) /* XK-12 Joystick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042A, 2, 0x0001, 0x0004, 12, 0) /* XK-12 Joystick joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042A, 3, 0x0001, 0x0002, 6, 0) /* XK-12 Joystick mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x042B, 0, 0x000c, 0x0001, 33, 36) /* XK-12 Joystick read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042B, 1, 0x0001, 0x0006, 9, 2) /* XK-12 Joystick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042B, 2, 0x0001, 0x0002, 6, 0) /* XK-12 Joystick mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x045D, 0, 0x000c, 0x0001, 33, 36) /* XK-68 Joystick splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x045D, 1, 0x0001, 0x0006, 9, 2) /* XK-68 Joystick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x045D, 2, 0x0001, 0x0004, 12, 0) /* XK-68 Joystick joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x045E, 0, 0x000c, 0x0001, 33, 36) /* XK-68 Joystick splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x045E, 1, 0x0001, 0x0006, 9, 2) /* XK-68 Joystick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x045E, 2, 0x0001, 0x0004, 12, 0) /* XK-68 Joystick joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x045E, 3, 0x0001, 0x0002, 6, 0) /* XK-68 Joystick mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x045F, 0, 0x000c, 0x0001, 33, 36) /* XK-68 Joystick read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x045F, 1, 0x0001, 0x0006, 9, 2) /* XK-68 Joystick keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x045F, 2, 0x0001, 0x0002, 6, 0) /* XK-68 Joystick mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x042C, 0, 0x000c, 0x0001, 33, 36) /* XK-3 Front Hinged Footpedal splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042C, 1, 0x0001, 0x0006, 9, 2) /* XK-3 Front Hinged Footpedal keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042C, 2, 0x0001, 0x0002, 6, 0) /* XK-3 Front Hinged Footpedal mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x042D, 0, 0x000c, 0x0001, 0, 36) /* XK-3 Front Hinged Footpedal splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042D, 1, 0x0001, 0x0006, 9, 2) /* XK-3 Front Hinged Footpedal keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042D, 2, 0x0001, 0x0004, 12, 0) /* XK-3 Front Hinged Footpedal joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042D, 3, 0x0001, 0x0002, 6, 0) /* XK-3 Front Hinged Footpedal mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x042E, 0, 0x000c, 0x0001, 33, 36) /* XK-3 Front Hinged Footpedal read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042E, 1, 0x0001, 0x0006, 9, 2) /* XK-3 Front Hinged Footpedal keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x042E, 2, 0x0001, 0x0004, 12, 0) /* XK-3 Front Hinged Footpedal joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0432, 0, 0x000c, 0x0001, 33, 36) /* XK-12 Touch splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0432, 1, 0x0001, 0x0006, 9, 2) /* XK-12 Touch keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0432, 2, 0x0001, 0x0002, 6, 0) /* XK-12 Touch mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0433, 0, 0x000c, 0x0001, 0, 36) /* XK-12 Touch splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0433, 1, 0x0001, 0x0006, 9, 2) /* XK-12 Touch keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0433, 2, 0x0001, 0x0004, 12, 0) /* XK-12 Touch joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0433, 3, 0x0001, 0x0002, 6, 0) /* XK-12 Touch mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0434, 0, 0x000c, 0x0001, 33, 36) /* XK-12 Touch read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0434, 1, 0x0001, 0x0006, 9, 2) /* XK-12 Touch keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0434, 2, 0x0001, 0x0004, 12, 0) /* XK-12 Touch joystick*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0438, 0, 0x000c, 0x0001, 33, 36) /* XK-3 Rear Hinged Footpedal splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0438, 1, 0x0001, 0x0006, 9, 2) /* XK-3 Rear Hinged Footpedal keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0438, 2, 0x0001, 0x0002, 6, 0) /* XK-3 Rear Hinged Footpedal mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0439, 0, 0x000c, 0x0001, 0, 36) /* XK-3 Rear Hinged Footpedal splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0439, 1, 0x0001, 0x0006, 9, 2) /* XK-3 Rear Hinged Footpedal keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0439, 2, 0x0001, 0x0004, 12, 0) /* XK-3 Rear Hinged Footpedal joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0439, 3, 0x0001, 0x0002, 6, 0) /* XK-3 Rear Hinged Footpedal mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x043A, 0, 0x000c, 0x0001, 33, 36) /* XK-3 Rear Hinged Footpedal splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x043A, 1, 0x0001, 0x0006, 9, 2) /* XK-3 Rear Hinged Footpedal keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x043A, 2, 0x0001, 0x0004, 12, 0) /* XK-3 Rear Hinged Footpedal joystick*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04E8, 0, 0x000c, 0x0001, 0, 36) /* XK-3 Rear Hinged Footpedal splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E8, 1, 0x0001, 0x0006, 9, 2) /* XK-3 Rear Hinged Footpedal keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E8, 2, 0x000c, 0x0001, 3, 0) /* XK-3 Rear Hinged Footpedal multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E8, 3, 0x0001, 0x0080, 2, 0) /* XK-3 Rear Hinged Footpedal multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E8, 4, 0x0001, 0x0002, 6, 0) /* XK-3 Rear Hinged Footpedal mouse*/
	
	DEVICE_MAP_ENTRY(PI_VID, 0x0441, 0, 0x000c, 0x0001, 33, 36) /* XK-80 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0441, 1, 0x0001, 0x0006, 9, 2) /* XK-80 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0441, 2, 0x0001, 0x0002, 6, 0) /* XK-80 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0442, 0, 0x000c, 0x0001, 0, 36) /* XK-80 splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0442, 1, 0x0001, 0x0006, 9, 2) /* XK-80 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0442, 2, 0x0001, 0x0004, 12, 0) /* XK-80 joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0442, 3, 0x0001, 0x0002, 6, 0) /* XK-80 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0443, 0, 0x000c, 0x0001, 33, 36) /* XK-80 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0443, 1, 0x0001, 0x0006, 9, 2) /* XK-80 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0443, 2, 0x0001, 0x0004, 12, 0) /* XK-80 joystick*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04E2, 0, 0x000c, 0x0001, 0, 36) /* XK-80 splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E2, 1, 0x0001, 0x0006, 9, 2) /* XK-80 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E2, 2, 0x000c, 0x0001, 3, 0) /* XK-80 multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E2, 3, 0x0001, 0x0080, 2, 0) /* XK-80 multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E2, 4, 0x0001, 0x0002, 6, 0) /* XK-80 mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x0461, 0, 0x000c, 0x0001, 33, 36) /* XK-60 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0461, 1, 0x0001, 0x0006, 9, 2) /* XK-60 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0461, 2, 0x0001, 0x0002, 6, 0) /* XK-60 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0462, 0, 0x000c, 0x0001, 0, 36) /* XK-60 splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0462, 1, 0x0001, 0x0006, 9, 2) /* XK-60 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0462, 2, 0x0001, 0x0004, 12, 0) /* XK-60 joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0462, 3, 0x0001, 0x0002, 6, 0) /* XK-60 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0463, 0, 0x000c, 0x0001, 33, 36) /* XK-60 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0463, 1, 0x0001, 0x0006, 9, 2) /* XK-60 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0463, 2, 0x0001, 0x0004, 12, 0) /* XK-60 joystick*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04E6, 0, 0x000c, 0x0001, 0, 36) /* XK-60 splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E6, 1, 0x0001, 0x0006, 9, 2) /* XK-60 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E6, 2, 0x000c, 0x0001, 3, 0) /* XK-60 multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E6, 3, 0x0001, 0x0080, 2, 0) /* XK-60 multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E6, 4, 0x0001, 0x0002, 6, 0) /* XK-60 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0268, 0, 0x000c, 0x0001, 19, 9) /* Footpedal SE splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x026A, 0, 0x000c, 0x0001, 19, 9) /* Matrix SE splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0271, 0, 0x000c, 0x0001, 12, 9) /* Stick SE splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0281, 0, 0x000c, 0x0001, 12, 9) /* Desktop SE splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0291, 0, 0x000c, 0x0001, 12, 9) /* Professional SE splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0241, 0, 0x000c, 0x0001, 15, 9) /* Jog & Shuttle SE splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0251, 0, 0x000c, 0x0001, 15, 9) /* Joystick Pro SE splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0269, 0, 0x000c, 0x0001, 19, 9) /* Switch Interface SE splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0305, 0, 0x000c, 0x0001, 32, 9) /* 128 w Mag Strip splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0306, 0, 0x000c, 0x0001, 32, 9) /* 128 no reader splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0307, 0, 0x000c, 0x0001, 32, 9) /* 128 w Bar Code splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0308, 0, 0x000c, 0x0001, 32, 9) /* 84 w Mag Strip splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0309, 0, 0x000c, 0x0001, 32, 9) /* 84 no reader splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x030A, 0, 0x000c, 0x0001, 32, 9) /* 84 w Bar Code splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0301, 0, 0x000c, 0x0001, 32, 9) /* LCD w Mag Strip splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0302, 0, 0x000c, 0x0001, 32, 9) /* LCD no reader splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0303, 0, 0x000c, 0x0001, 32, 9) /* LCD w Bar Code splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x00D2, 0, 0x000c, 0x0001, 15, 9) /* Raildriver splat read and write*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x02B5, 0, 0x000c, 0x0001, 32, 8) /* Stick MWII splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02B6, 1, 0x0001, 0x0006, 9, 2) /* Stick MWII keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02B6, 2, 0x0001, 0x0002, 6, 0) /* Stick MWII mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x02A5, 0, 0x000c, 0x0001, 32, 8) /* Desktop MWII splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02A6, 1, 0x0001, 0x0006, 9, 2) /* Desktop MWII keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02A6, 2, 0x0001, 0x0002, 6, 0) /* Desktop MWII mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x02A7, 0, 0x000c, 0x0001, 32, 8) /* Professional MWII splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02A8, 1, 0x0001, 0x0006, 9, 2) /* Professional MWII keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02A8, 2, 0x0001, 0x0002, 6, 0) /* Professional MWII mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x02B1, 0, 0x000c, 0x0001, 32, 8) /* Jog & Shuttle MWII splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02B2, 1, 0x0001, 0x0006, 9, 2) /* Jog & Shuttle MWII keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02B2, 2, 0x0001, 0x0002, 6, 0) /* Jog & Shuttle MWII mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x02B7, 0, 0x000c, 0x0001, 32, 8) /* Switch Interface MWII splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02B8, 1, 0x0001, 0x0006, 9, 2) /* Switch Interface MWII keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x02B8, 2, 0x0001, 0x0002, 6, 0) /* Switch Interface MWII mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04A8, 0, 0x000c, 0x0001, 0x0025, 0x0024) /* XK-12 Switch Interface read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04A8, 1, 0x0001, 0x0004, 0x000C, 0x0000) /* XK-12 Switch Interface joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04A8, 2, 0x0001, 0x0002, 0x0007, 0x0000) /* XK-12 Switch Interface mouse*/
		 
	DEVICE_MAP_ENTRY(PI_VID, 0x04A9, 0, 0x000c, 0x0001, 0x0000, 0x0024) /* XK-12 Switch Interface splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04A9, 1, 0x0001, 0x0006, 0x0009, 0x0002) /* XK-12 Switch Interface keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04A9, 2, 0x000c, 0x0001, 0x0003, 0x0000) /* XK-12 Switch Interface multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04A9, 3, 0x0001, 0x0080, 0x0002, 0x0000) /* XK-12 Switch Interface multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04A9, 4, 0x0001, 0x0002, 0x0007, 0x0000) /* XK-12 Switch Interface mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04AA, 0, 0x000c, 0x0001, 0x0000, 0x0024) /* XK-12 Switch Interface splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04AA, 1, 0x0001, 0x0004, 0x000C, 0x0000) /* XK-12 Switch Interface joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04AA, 2, 0x000c, 0x0001, 0x0003, 0x0000) /* XK-12 Switch Interface multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04AA, 3, 0x0001, 0x0080, 0x0002, 0x0000) /* XK-12 Switch Interface multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04AA, 4, 0x0001, 0x0002, 0x0007, 0x0000) /* XK-12 Switch Interface mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x04AB, 0, 0x000c, 0x0001, 0x0025, 0x0024) /* XK-12 Switch Interface splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04AB, 1, 0x0001, 0x0006, 0x0009, 0x000C) /* XK-12 Switch Interface keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04AB, 2, 0x0001, 0x0002, 0x0007, 0x0000) /* XK-12 Switch Interface mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04C5, 0, 0x000c, 0x0001, 0x0025, 0x0024) /* XK-3 Switch Interface read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C5, 1, 0x0001, 0x0004, 0x000C, 0x0000) /* XK-3 Switch Interface joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C5, 2, 0x0001, 0x0002, 0x0007, 0x0000) /* XK-3 Switch Interface mouse*/
		 
	DEVICE_MAP_ENTRY(PI_VID, 0x04C6, 0, 0x000c, 0x0001, 0x0000, 0x0024) /* XK-3 Switch Interface splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C6, 1, 0x0001, 0x0006, 0x0009, 0x0002) /* XK-3 Switch Interface keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C6, 2, 0x000c, 0x0001, 0x0003, 0x0000) /* XK-3 Switch Interface multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C6, 3, 0x0001, 0x0080, 0x0002, 0x0000) /* XK-3 Switch Interface multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C6, 4, 0x0001, 0x0002, 0x0007, 0x0000) /* XK-3 Switch Interface mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04C7, 0, 0x000c, 0x0001, 0x0000, 0x0024) /* XK-3 Switch Interface splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C7, 1, 0x0001, 0x0004, 0x000C, 0x0000) /* XK-3 Switch Interface joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C7, 2, 0x000c, 0x0001, 0x0003, 0x0000) /* XK-3 Switch Interface multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C7, 3, 0x0001, 0x0080, 0x0002, 0x0000) /* XK-3 Switch Interface multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C7, 4, 0x0001, 0x0002, 0x0007, 0x0000) /* XK-3 Switch Interface mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x04C8, 0, 0x000c, 0x0001, 0x0025, 0x0024) /* XK-3 Switch Interface splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C8, 1, 0x0001, 0x0006, 0x0009, 0x000C) /* XK-3 Switch Interface keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C8, 2, 0x0001, 0x0002, 0x0007, 0x0000) /* XK-3 Switch Interface mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04FF, 0, 0x000c, 0x0001, 37, 36) /* XK-32 Rack Mount read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04FF, 1, 0x0001, 0x0004, 12, 0) /* XK-32 Rack Mount joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04FF, 2, 0x0001, 0x0002, 7, 0) /* XK-32 Rack Mount mouse*/
		 
	DEVICE_MAP_ENTRY(PI_VID, 0x0500, 0, 0x000c, 0x0001, 0, 36) /* XK-32 Rack Mount splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0500, 1, 0x0001, 0x0006, 9, 2) /* XK-32 Rack Mount keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0500, 2, 0x000c, 0x0001, 3, 0) /* XK-32 Rack Mount multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0500, 3, 0x0001, 0x0080, 2, 0) /* XK-32 Rack Mount multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0500, 4, 0x0001, 0x0002, 7, 0) /* XK-32 Rack Mount mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x0501, 0, 0x000c, 0x0001, 0, 36) /* XK-32 Rack Mount splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0501, 1, 0x0001, 0x0004, 12, 0) /* XK-32 Rack Mount joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0501, 2, 0x000c, 0x0001, 3, 0) /* XK-32 Rack Mount multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0501, 3, 0x0001, 0x0080, 2, 0) /* XK-32 Rack Mount multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0501, 4, 0x0001, 0x0002, 7, 0) /* XK-32 Rack Mount mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x0502, 0, 0x000c, 0x0001, 37, 36) /* XK-32 Rack Mount splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0502, 1, 0x0001, 0x0006, 9, 2) /* XK-32 Rack Mount keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0502, 2, 0x0001, 0x0002, 7, 0) /* XK-32 Rack Mount mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x049C, 0, 0x000c, 0x0001, 37, 36) /* XK-24 Android read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x049C, 1, 0x0001, 0x0004, 12, 0) /* XK-24 Android joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x049C, 2, 0x0001, 0x0002, 7, 0) /* XK-24 Android mouse*/
		 
	DEVICE_MAP_ENTRY(PI_VID, 0x049D, 0, 0x000c, 0x0001, 0, 36) /* XK-24 Android splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x049D, 1, 0x0001, 0x0006, 9, 2) /* XK-24 Android keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x049D, 2, 0x000c, 0x0001, 3, 0) /* XK-24 Android multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x049D, 3, 0x0001, 0x0080, 2, 0) /* XK-24 Android multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x049D, 4, 0x0001, 0x0002, 7, 0) /* XK-24 Android mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x049E, 0, 0x000c, 0x0001, 0, 36) /* XK-24 Android splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x049E, 1, 0x0001, 0x0004, 12, 0) /* XK-24 Android joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x049E, 2, 0x000c, 0x0001, 3, 0) /* XK-24 Android multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x049E, 3, 0x0001, 0x0080, 2, 0) /* XK-24 Android multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x049E, 4, 0x0001, 0x0002, 7, 0) /* XK-24 Android mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x049F, 0, 0x000c, 0x0001, 37, 36) /* XK-24 Android splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x049F, 1, 0x0001, 0x0006, 9, 2) /* XK-24 Android keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x049F, 2, 0x0001, 0x0002, 7, 0) /* XK-24 Android mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04C1, 0, 0x000c, 0x0001, 37, 36) /* XK-80 Android read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C1, 1, 0x0001, 0x0004, 12, 0) /* XK-80 Android joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C1, 2, 0x0001, 0x0002, 7, 0) /* XK-80 Android mouse*/
		 
	DEVICE_MAP_ENTRY(PI_VID, 0x04C2, 0, 0x000c, 0x0001, 0, 36) /* XK-80 Android splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C2, 1, 0x0001, 0x0006, 9, 2) /* XK-80 Android keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C2, 2, 0x000c, 0x0001, 3, 0) /* XK-80 Android multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C2, 3, 0x0001, 0x0080, 2, 0) /* XK-80 Android multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C2, 4, 0x0001, 0x0002, 7, 0) /* XK-80 Android mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04C3, 0, 0x000c, 0x0001, 0, 36) /* XK-80 Android splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C3, 1, 0x0001, 0x0004, 12, 0) /* XK-80 Android joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C3, 2, 0x000c, 0x0001, 3, 0) /* XK-80 Android multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C3, 3, 0x0001, 0x0080, 2, 0) /* XK-80 Android multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C3, 4, 0x0001, 0x0002, 7, 0) /* XK-80 Android mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x04C4, 0, 0x000c, 0x0001, 37, 36) /* XK-80 Android splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C4, 1, 0x0001, 0x0006, 9, 2) /* XK-80 Android keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C4, 2, 0x0001, 0x0002, 7, 0) /* XK-80 Android mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04CF, 0, 0x000c, 0x0001, 37, 36) /* XK-60 Android read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04CF, 1, 0x0001, 0x0004, 12, 0) /* XK-60 Android joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04CF, 2, 0x0001, 0x0002, 7, 0) /* XK-60 Android mouse*/
		 
	DEVICE_MAP_ENTRY(PI_VID, 0x04D0, 0, 0x000c, 0x0001, 0, 36) /* XK-60 Android splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04D0, 1, 0x0001, 0x0006, 9, 2) /* XK-60 Android keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04D0, 2, 0x000c, 0x0001, 3, 0) /* XK-60 Android multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04D0, 3, 0x0001, 0x0080, 2, 0) /* XK-60 Android multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04D0, 4, 0x0001, 0x0002, 7, 0) /* XK-60 Android mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04D1, 0, 0x000c, 0x0001, 0, 36) /* XK-60 Android splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04D1, 1, 0x0001, 0x0004, 12, 0) /* XK-60 Android joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04D1, 2, 0x000c, 0x0001, 3, 0) /* XK-60 Android multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04D1, 3, 0x0001, 0x0080, 2, 0) /* XK-60 Android multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04D1, 4, 0x0001, 0x0002, 7, 0) /* XK-60 Android mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x04D2, 0, 0x000c, 0x0001, 37, 36) /* XK-60 Android splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04D2, 1, 0x0001, 0x0006, 9, 2) /* XK-60 Android keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04D2, 2, 0x0001, 0x0002, 7, 0) /* XK-60 Android mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04BD, 0, 0x000c, 0x0001, 37, 36) /* XK-16 Android read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04BD, 1, 0x0001, 0x0004, 12, 0) /* XK-16 Android joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04BD, 2, 0x0001, 0x0002, 7, 0) /* XK-16 Android mouse*/
		 
	DEVICE_MAP_ENTRY(PI_VID, 0x04BE, 0, 0x000c, 0x0001, 0, 36) /* XK-16 Android splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04BE, 1, 0x0001, 0x0006, 9, 2) /* XK-16 Android keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04BE, 2, 0x000c, 0x0001, 3, 0) /* XK-16 Android multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04BE, 3, 0x0001, 0x0080, 2, 0) /* XK-16 Android multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04BE, 4, 0x0001, 0x0002, 7, 0) /* XK-16 Android mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04BF, 0, 0x000c, 0x0001, 0, 36) /* XK-16 Android splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04BF, 1, 0x0001, 0x0004, 12, 0) /* XK-16 Android joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04BF, 2, 0x000c, 0x0001, 3, 0) /* XK-16 Android multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04BF, 3, 0x0001, 0x0080, 2, 0) /* XK-16 Android multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04BF, 4, 0x0001, 0x0002, 7, 0) /* XK-16 Android mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x04C0, 0, 0x000c, 0x0001, 37, 36) /* XK-16 Android splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C0, 1, 0x0001, 0x0006, 9, 2) /* XK-16 Android keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C0, 2, 0x0001, 0x0002, 7, 0) /* XK-16 Android mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04AC, 0, 0x000c, 0x0001, 37, 36) /* Pi4 Matrix Board read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04AC, 1, 0x0001, 0x0004, 12, 0) /* Pi4 Matrix Board joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04AC, 2, 0x0001, 0x0002, 7, 0) /* Pi4 Matrix Board mouse*/
		 
	DEVICE_MAP_ENTRY(PI_VID, 0x04AD, 0, 0x000c, 0x0001, 0, 36) /* Pi4 Matrix Board splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04AD, 1, 0x0001, 0x0006, 9, 2) /* Pi4 Matrix Board keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04AD, 2, 0x000c, 0x0001, 3, 0) /* Pi4 Matrix Board multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04AD, 3, 0x0001, 0x0080, 2, 0) /* Pi4 Matrix Board multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04AD, 4, 0x0001, 0x0002, 7, 0) /* Pi4 Matrix Board mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04AE, 0, 0x000c, 0x0001, 0, 36) /* Pi4 Matrix Board splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04AE, 1, 0x0001, 0x0004, 12, 0) /* Pi4 Matrix Board joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04AE, 2, 0x000c, 0x0001, 3, 0) /* Pi4 Matrix Board multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04AE, 3, 0x0001, 0x0080, 2, 0) /* Pi4 Matrix Board multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04AE, 4, 0x0001, 0x0002, 7, 0) /* Pi4 Matrix Board mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x04AF, 0, 0x000c, 0x0001, 37, 36) /* Pi4 Matrix Board splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04AF, 1, 0x0001, 0x0006, 9, 2) /* Pi4 Matrix Board keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04AF, 2, 0x0001, 0x0002, 7, 0) /* Pi4 Matrix Board mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04B0, 0, 0x000c, 0x0001, 37, 36) /* Pi4 Footpedal read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B0, 1, 0x0001, 0x0004, 12, 0) /* Pi4 Footpedal joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B0, 2, 0x0001, 0x0002, 7, 0) /* Pi4 Footpedal mouse*/
		 
	DEVICE_MAP_ENTRY(PI_VID, 0x04B1, 0, 0x000c, 0x0001, 0, 36) /* Pi4 Footpedal splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B1, 1, 0x0001, 0x0006, 9, 2) /* Pi4 Footpedal keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B1, 2, 0x000c, 0x0001, 3, 0) /* Pi4 Footpedal multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B1, 3, 0x0001, 0x0080, 2, 0) /* Pi4 Footpedal multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B1, 4, 0x0001, 0x0002, 7, 0) /* Pi4 Footpedal mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04B2, 0, 0x000c, 0x0001, 0, 36) /* Pi4 Footpedal splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B2, 1, 0x0001, 0x0004, 12, 0) /* Pi4 Footpedal joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B2, 2, 0x000c, 0x0001, 3, 0) /* Pi4 Footpedal multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B2, 3, 0x0001, 0x0080, 2, 0) /* Pi4 Footpedal multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B2, 4, 0x0001, 0x0002, 7, 0) /* Pi4 Footpedal mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x04B3, 0, 0x000c, 0x0001, 37, 36) /* Pi4 Footpedal splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B3, 1, 0x0001, 0x0006, 9, 2) /* Pi4 Footpedal keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B3, 2, 0x0001, 0x0002, 7, 0) /* Pi4 Footpedal mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04B4, 0, 0x000c, 0x0001, 37, 36) /* RS485 read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B4, 1, 0x0001, 0x0004, 12, 0) /* RS485 joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B4, 2, 0x0001, 0x0002, 7, 0) /* RS485 mouse*/
		 
	DEVICE_MAP_ENTRY(PI_VID, 0x04B5, 0, 0x000c, 0x0001, 0, 36) /* RS485 splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B5, 1, 0x0001, 0x0006, 9, 2) /* RS485 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B5, 2, 0x000c, 0x0001, 3, 0) /* RS485 multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B5, 3, 0x0001, 0x0080, 2, 0) /* RS485 multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B5, 4, 0x0001, 0x0002, 7, 0) /* RS485 mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04B6, 0, 0x000c, 0x0001, 0, 36) /* RS485 splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B6, 1, 0x0001, 0x0004, 12, 0) /* RS485 joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B6, 2, 0x000c, 0x0001, 3, 0) /* RS485 multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B6, 3, 0x0001, 0x0080, 2, 0) /* RS485 multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B6, 4, 0x0001, 0x0002, 7, 0) /* RS485 mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x04B7, 0, 0x000c, 0x0001, 37, 36) /* RS485 splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B7, 1, 0x0001, 0x0006, 9, 2) /* RS485 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04B7, 2, 0x0001, 0x0002, 7, 0) /* RS485 mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04DC, 0, 0x000c, 0x0001, 37, 36) /* IAB-HD15-Wire Interface read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04DC, 1, 0x0001, 0x0004, 12, 0) /* IAB-HD15-Wire Interface joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04DC, 2, 0x0001, 0x0002, 7, 0) /* IAB-HD15-Wire Interface mouse*/
		 
	DEVICE_MAP_ENTRY(PI_VID, 0x04DD, 0, 0x000c, 0x0001, 0, 36) /* IAB-HD15-Wire Interface splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04DD, 1, 0x0001, 0x0006, 9, 2) /* IAB-HD15-Wire Interface keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04DD, 2, 0x000c, 0x0001, 3, 0) /* IAB-HD15-Wire Interface multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04DD, 3, 0x0001, 0x0080, 2, 0) /* IAB-HD15-Wire Interface multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04DD, 4, 0x0001, 0x0002, 7, 0) /* IAB-HD15-Wire Interface mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04DE, 0, 0x000c, 0x0001, 0, 36) /* IAB-HD15-Wire Interface splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04DE, 1, 0x0001, 0x0004, 12, 0) /* IAB-HD15-Wire Interface joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04DE, 2, 0x000c, 0x0001, 3, 0) /* IAB-HD15-Wire Interface multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04DE, 3, 0x0001, 0x0080, 2, 0) /* IAB-HD15-Wire Interface multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04DE, 4, 0x0001, 0x0002, 7, 0) /* IAB-HD15-Wire Interface mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x04DF, 0, 0x000c, 0x0001, 37, 36) /* IAB-HD15-Wire Interface splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04DF, 1, 0x0001, 0x0006, 9, 2) /* IAB-HD15-Wire Interface keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04DF, 2, 0x0001, 0x0002, 7, 0) /* IAB-HD15-Wire Interface mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04E9, 0, 0x000c, 0x0001, 37, 36) /* RS232 read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E9, 1, 0x0001, 0x0004, 12, 0) /* RS232 joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04E9, 2, 0x0001, 0x0002, 7, 0) /* RS232 mouse*/
		 
	DEVICE_MAP_ENTRY(PI_VID, 0x04EA, 0, 0x000c, 0x0001, 0, 36) /* RS232 splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04EA, 1, 0x0001, 0x0006, 9, 2) /* RS232 keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04EA, 2, 0x000c, 0x0001, 3, 0) /* RS232 multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04EA, 3, 0x0001, 0x0080, 2, 0) /* RS232 multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04EA, 4, 0x0001, 0x0002, 7, 0) /* RS232 mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04EB, 0, 0x000c, 0x0001, 0, 36) /* RS232 splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04EB, 1, 0x0001, 0x0004, 12, 0) /* RS232 joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04EB, 2, 0x000c, 0x0001, 3, 0) /* RS232 multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04EB, 3, 0x0001, 0x0080, 2, 0) /* RS232 multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04EB, 4, 0x0001, 0x0002, 7, 0) /* RS232 Interface mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x04EC, 0, 0x000c, 0x0001, 37, 36) /* RS232 Interface splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04EC, 1, 0x0001, 0x0006, 9, 2) /* RS232 Interface keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04EC, 2, 0x0001, 0x0002, 7, 0) /* RS232 mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04FB, 0, 0x000c, 0x0001, 37, 36) /* XK-124 Tbar read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04FB, 1, 0x0001, 0x0004, 12, 0) /* XK-124 Tbar joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04FB, 2, 0x0001, 0x0002, 7, 0) /* XK-124 Tbar mouse*/
		 
	DEVICE_MAP_ENTRY(PI_VID, 0x04FC, 0, 0x000c, 0x0001, 0, 36) /* XK-124 Tbar splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04FC, 1, 0x0001, 0x0006, 9, 2) /* XK-124 Tbar keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04FC, 2, 0x000c, 0x0001, 3, 0) /* XK-124 Tbar multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04FC, 3, 0x0001, 0x0080, 2, 0) /* XK-124 Tbar multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04FC, 4, 0x0001, 0x0002, 7, 0) /* XK-124 Tbar mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04FD, 0, 0x000c, 0x0001, 0, 36) /* XK-124 Tbar splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04FD, 1, 0x0001, 0x0004, 12, 0) /* XK-124 Tbar joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04FD, 2, 0x000c, 0x0001, 3, 0) /* XK-124 Tbar multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04FD, 3, 0x0001, 0x0080, 2, 0) /* XK-124 Tbar multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04FD, 4, 0x0001, 0x0002, 7, 0) /* XK-124 Tbar Interface mouse*/
	 
	DEVICE_MAP_ENTRY(PI_VID, 0x04FE, 0, 0x000c, 0x0001, 37, 36) /* XK-124 Tbar Interface splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04FE, 1, 0x0001, 0x0006, 9, 2) /* XK-124 Tbar Interface keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04FE, 2, 0x0001, 0x0002, 7, 0) /* XK-124 Tbar mouse*/
	
	DEVICE_MAP_ENTRY(PI_VID, 0x04C9, 0, 0x000c, 0x0001, 37, 36) /* XC-DMX512-RJ45  splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04C9, 1, 0x0001, 0x0006, 9, 2) /* XC-DMX512-RJ45 Interface keyboard*/

	DEVICE_MAP_ENTRY(PI_VID, 0x052C, 0, 0x000c, 0x0001, 37, 36) /* XC-DMX512 Screw Terminal splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x052C, 1, 0x0001, 0x0006, 9, 2) /* XC-DMX512 Screw Terminal keyboard*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04D3, 0, 0x000c, 0x0001, 37, 36) /* XK-24 KVM splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04D3, 1, 0x0001, 0x0006, 9, 2) /* XK-24 KVM keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04D3, 2, 0x0001, 0x0004, 12, 0) /* XK-24 KVM joystick*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04D4, 0, 0x0001, 0x0006, 9, 2) /* XK-24 KVM keyboard*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04D5, 0, 0x000c, 0x0001, 37, 36) /* XK-80 KVM splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04D5, 1, 0x0001, 0x0006, 9, 2) /* XK-80 KVM keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04D5, 2, 0x0001, 0x0002, 6, 0) /* XK-80 KVM mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04D6, 0, 0x0001, 0x0006, 9, 2) /* XK-80 KVM keyboard*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04D7, 0, 0x000c, 0x0001, 37, 36) /* XK-60 KVM splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04D7, 1, 0x0001, 0x0006, 9, 2) /* XK-60 KVM keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04D7, 2, 0x0001, 0x0002, 6, 0) /* XK-16 KVM mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04D8, 0, 0x0001, 0x0006, 9, 2) /* XK-60 KVM keyboard*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04F5, 0, 0x000c, 0x0001, 37, 36) /* XK-16 KVM splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04F5, 1, 0x0001, 0x0006, 9, 2) /* XK-16 KVM keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x04F5, 2, 0x0001, 0x0002, 6, 0) /* XK-16 KVM mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x04F6, 0, 0x0001, 0x0006, 9, 2) /* XK-16 KVM keyboard*/

	DEVICE_MAP_ENTRY(PI_VID, 0x0503, 0, 0x000c, 0x0001, 37, 36) /* XK-32 Rack Mount KVM splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0503, 1, 0x0001, 0x0006, 9, 2) /* XK-32 Rack Mount KVM keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0503, 2, 0x0001, 0x0002, 7, 0) /* XK-32 Rack Mount KVM mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x0504, 0, 0x0001, 0x0006, 9, 2) /* XK-32 Rack Mount KVM keyboard*/

	DEVICE_MAP_ENTRY(PI_VID, 0x050A, 0, 0x000c, 0x0001, 37, 36) /* XK-128 KVM splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x050A, 1, 0x0001, 0x0006, 9, 2) /* XK-128 KVM keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x050A, 2, 0x0001, 0x0002, 7, 0) /* XK-128 KVM mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x050B, 0, 0x0001, 0x0006, 9, 2) /* XK-128 KVM keyboard*/

	DEVICE_MAP_ENTRY(PI_VID, 0x0514, 0, 0x000c, 0x0001, 37, 36) /* XK-3 Switch Interface KVM splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0514, 1, 0x0001, 0x0006, 9, 2) /* XK-3 Switch Interface KVM keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0514, 2, 0x0001, 0x0002, 7, 0) /* XK-3 Switch Interface KVM mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x0515, 0, 0x0001, 0x0006, 9, 2) /* XK-3 Switch Interface KVM keyboard*/

	DEVICE_MAP_ENTRY(PI_VID, 0x0516, 0, 0x000c, 0x0001, 37, 36) /* XK-12 Switch Interface KVM splat read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0516, 1, 0x0001, 0x0006, 9, 2) /* XK-12 Switch Interface KVM keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0516, 2, 0x0001, 0x0002, 7, 0) /* XK-12 Switch Interface KVM mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x0517, 0, 0x0001, 0x0006, 9, 2) /* XK-12 Switch Interface KVM keyboard*/

	DEVICE_MAP_ENTRY(PI_VID, 0x0524, 0, 0x000c, 0x0001, 37, 36) /* XK-16 LCD read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0524, 1, 0x0001, 0x0006, 9, 2) /* XK-16 LCD keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0524, 2, 0x000c, 0x0001, 3, 0) /* XK-16 LCD multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0524, 3, 0x0001, 0x0080, 2, 0) /* XK-16 LCD multimedia*/

	DEVICE_MAP_ENTRY(PI_VID, 0x0525, 0, 0x000c, 0x0001, 37, 36) /* XK-16 LCD read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0525, 1, 0x0001, 0x0006, 9, 2) /* XK-16 LCD keyboard boot*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0525, 2, 0x000c, 0x0001, 3, 0) /* XK-16 LCD multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0525, 3, 0x0001, 0x0080, 2, 0) /* XK-16 LCD multimedia*/
		 
	DEVICE_MAP_ENTRY(PI_VID, 0x0526, 0, 0x000c, 0x0001, 37, 36) /* XK-16 LCD read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0526, 1, 0x0001, 0x0006, 9, 2) /* XK-16 LCD keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0526, 2, 0x0001, 0x0004, 12, 0) /* XK-16 LCD joystick*/
	
	DEVICE_MAP_ENTRY(PI_VID, 0x0527, 0, 0x000c, 0x0001, 37, 36) /* XK-16 LCD read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0527, 1, 0x0001, 0x0002, 7, 0) /* XK-16 LCD mouse*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0527, 2, 0x0001, 0x0004, 12, 0) /* XK-16 LCD joystick*/

	DEVICE_MAP_ENTRY(PI_VID, 0x0528, 0, 0x000c, 0x0001, 37, 36) /* XK-16 LCD read and write*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0528, 1, 0x0001, 0x0006, 9, 2) /* XK-16 LCD keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x0528, 2, 0x0001, 0x0002, 7, 0) /* XK-16 LCD mouse*/

	DEVICE_MAP_ENTRY(PI_VID, 0x0529, 0, 0x000c, 0x0001, 37, 36) /* XK-16 LCD read and write*/

	DEVICE_MAP_ENTRY(PI_VID, 0x052A, 0, 0x000c, 0x0001, 0, 36) /* XK-16 LCD splat write only*/
	DEVICE_MAP_ENTRY(PI_VID, 0x052A, 1, 0x0001, 0x0006, 9, 2) /* XK-16 LCD keyboard*/
	DEVICE_MAP_ENTRY(PI_VID, 0x052A, 2, 0x0001, 0x0004, 12, 0) /* XK-16 LCD joystick*/
	DEVICE_MAP_ENTRY(PI_VID, 0x052A, 3, 0x0001, 0x0002, 7, 0) /* XK-16 LCD mouse*/
	DEVICE_MAP_ENTRY(PI_VID, 0x052A, 4, 0x000c, 0x0001, 3, 0) /* XK-16 LCD multimedia*/
	DEVICE_MAP_ENTRY(PI_VID, 0x052A, 5, 0x0001, 0x0080, 2, 0) /* XK-16 LCD multimedia*/
	
	DEVICE_MAP_ENTRY(PI_VID, 0x052B, 0, 0x0001, 0x0006, 9, 2) /* XK-16 LCD keyboard boot (KVM)*/
};

static bool get_usage(unsigned short vid, unsigned short pid,
                      int interface_number,
                      unsigned short *usage_page,
                      unsigned short *usage, 
		      int *readlength,
  		      int *writelength)
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
			*readlength = dev->readlength;
			*writelength = dev->writelength;
			return true;
		}
	}
	
	return false;
}
