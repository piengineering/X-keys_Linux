/*************************************************
 X-Keys Interface
 
 LICENSE HERE
 
 P.I. Engineering
 Port to Linux by Alan Ott, Signal 11 Software
*************************************************/

#ifndef PIE_HID_H__
#define PIE_HID_H__

#ifdef _WIN32
	#define PIE_HID_CALL __stdcall
#else
	#define PIE_HID_CALL
	#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
	piNone = 0,
	piNewData = 1,
	piDataChange = 2
} EEventPI;

#define MAX_XKEY_DEVICES 128
#define PI_VID 0x5F3



// Enumerate() errors
#define PIE_HID_ENUMERATE_BAD_HID_DEVICE 101 /* Bad HID device information set handle */
#define PIE_HID_ENUMERATE_NO_DEVICES_FOUND 102 /* No devices found in device information set */
#define PIE_HID_ENUMERATE_OTHER_ENUM_ERROR 103 /* Other enumeration error */
#define PIE_HID_ENUMERATE_ERROR_GETTING_DEVICE_DETAIL 104 /* Error getting device interface detail (symbolic link name) */ 
#define PIE_HID_ENUMERATE_ERROR_GETTING_DEVICE_DETATIL2 105 /* Error getting device interface detail (symbolic link name) */
#define PIE_HID_ENUMERATE_UNABLE_TO_OPEN_HANDLE 106 /*Unable to open a handle. */
#define PIE_HID_ENUMERATE_GET_ATTRIBUTES_ERROR 107
#define PIE_HID_ENUMERATE_VENDOR_ID_ERROR 108
#define PIE_HID_ENUMERATE_GET_PREPARSED_DATA_ERROR 109
#define PIE_HID_ENUMERATE_GET_CAPS 110
#define PIE_HID_ENUMERATE_GET_MANUFACTURER_STRING 111
#define PIE_HID_ENUMERATE_GET_PRODUCT_STRING 112

// SetupInterface() errors
#define PIE_HID_SETUP_BAD_HANDLE 201 /* Bad interface handle */
#define PIE_HID_SETUP_CANNOT_ALLOCATE_MEM_FOR_RING 202 /* Cannot allocate memory for ring buffer */
#define PIE_HID_SETUP_CANNOT_CREATE_MUTEX 203 /* Cannot create mutex */
#define PIE_HID_SETUP_CANNOT_CREATE_READ_THREAD 204 /* Cannot create read thread */
#define PIE_HID_SETUP_CANNOT_OPEN_READ_HANDLE 205 /* Cannot open read handle */
#define PIE_HID_SETUP_CANNOT_OPEN_READ_HANDLE_ACCESS_DENIED 206 /* Cannot open read handle - Access Denied */
#define PIE_HID_SETUP_CANNOT_OPEN_READ_HANDLE_BAD_PATH 207 /* Cannot open read handle - bad DevicePath */
#define PIE_HID_SETUP_CANNOT_OPEN_WRITE_HANDLE 208 /* Cannot open write handle */
#define PIE_HID_SETUP_CANNOT_OPEN_WRITE_HANDLE_ACCESS_DENIED 209 /* Cannot open write handle - Access Denied */
#define PIE_HID_SETUP_CANNOT_OPEN_WRITE_HANDLE_BAD_PATH 210 /* Cannot open write handle - bad DevicePath */

// ReadData() errors
#define PIE_HID_READ_BAD_INTERFACE_HANDLE 301 /* Bad interface handle */
#define PIE_HID_READ_LENGTH_ZERO 302 /* Read length is zero */
#define PIE_HID_READ_CANNOT_ACQUIRE_MUTEX 303 /* Could not acquire data mutex */
#define PIE_HID_READ_INSUFFICIENT_DATA 304 /* Insufficient data (< readSize bytes) */
#define PIE_HID_READ_CANNOT_RELEASE_MUTEX 305 /* Could not release data mutex */
#define PIE_HID_READ_CANNOT_RELEASE_MUTEX2 306 /* Could not release data mutex */
#define PIE_HID_READ_INVALID_HANDLE 307 /* Handle Invalid or Device_Not_Found (probably device unplugged) */
#define PIE_HID_READ_DEVICE_DISCONNECTED 308
#define PIE_HID_READ_READ_ERROR 309
#define PIE_HID_READ_BYTES_NOT_EQUAL_READSIZE 310
#define PIE_HID_READ_BLOCKING_READ_DATA_TIMED_OUT 311

// Write() errors
#define PIE_HID_WRITE_BAD_HANDLE 401 /* Bad interface handle */
#define PIE_HID_WRITE_LENGTH_ZERO 402 /* Write length is zero */
#define PIE_HID_WRITE_FAILED 403 /* Write failed */
#define PIE_HID_WRITE_INCOMPLETE 404 /* Write incomplete */
#define PIE_HID_WRITE_UNABLE_TO_ACQUIRE_MUTEX 405 /* unable to acquire write mutex */
#define PIE_HID_WRITE_UNABLE_TO_RELEASE_MUTEX 406 /* unable to release write mutex */
#define PIE_HID_WRITE_HANDLE_INVALID 407 /* Handle Invalid or Device_Not_Found (probably device unplugged) (previous buffered write) */
#define PIE_HID_WRITE_BUFFER_FULL 408 /* Buffer full */
#define PIE_HID_WRITE_PREV_WRITE_FAILED 409 /* Previous buffered write failed. */
#define PIE_HID_WRITE_PREV_WRITE_WRONG_NUMBER 410 /* Previous buffered write sent wrong number of bytes */
#define PIE_HID_WRITE_TIMER_FAILED 411 /* timer failed */
#define PIE_HID_WRITE_PREV_WRITE_UNABLE_TO_RELEASE_MUTEX 412 /* previous buffered write count not release mutex */
#define PIE_HID_WRITE_BUFFER_FULL2 413 /* write buffer is full */
#define PIE_HID_WRITE_FAST_WRITE_ERROR 414 /* cannot write queue a fast write while slow writes are still pending */

// ReadLast errors
#define PIE_HID_READLAST_BAD_HANDLE 501 /* Bad interface handle */
#define PIE_HID_READLAST_LENGTH_ZERO 502 /* Read length is zero */
#define PIE_HID_READLAST_UNABLE_TO_ACQUIRE_MUTEX 503 /* Could not acquire data mutex */
#define PIE_HID_READLAST_INSUFFICIENT_DATA 504 /* Insufficient data (< readSize bytes) */
#define PIE_HID_READLAST_UNABLE_TO_RELEASE_MUTEX 505 /* Could not release data mutex */
#define PIE_HID_READLAST_UNABLE_TO_RELEASE_MUTEX2 506 /* Could not release data mutex */
#define PIE_HID_READLAST_INVALID_HANDLE 507 /* Handle Invalid or Device_Not_Found (probably device unplugged) */

// ClearBuffer() errors
#define PIE_HID_CLEARBUFFER_BAD_HANDLE 601 /* Bad interface handle */
#define PIE_HID_CLEARBUFFER_UNABLE_TO_RELEASE_MUTEX 602 /* Could not release data mutex */
#define PIE_HID_CLEARBUFFER_UNABLE_TO_ACQUIRE_MUTEX 603 /* Could not acquire data mutex */

// SetDataCallback() errors
#define PIE_HID_DATACALLBACK_BAD_HANDLE 701 /* Bad interface handle */
#define PIE_HID_DATACALLBACK_INVALID_INTERFACE 702
#define PIE_HID_DATACALLBACK_CANNOT_CREATE_CALLBACK_THREAD 703
#define PIE_HID_DATACALLBACK_CALLBACK_ALREADY_SET 704

// SetErrorCallback() errors
#define PIE_HID_ERRORCALLBACK_BAD_HANDLE 801 /* Bad interface handle */
#define PIE_HID_ERRORCALLBACK_INVALID_INTERFACE 802
#define PIE_HID_ERRORCALLBACK_CANNOT_CREATE_ERROR_THREAD 803
#define PIE_HID_ERRORCALLBACK_ERROR_THREAD_ALREADY_CREATED 1804



typedef struct  _HID_ENUM_INFO  {
    unsigned int   PID;
    unsigned int   Usage;
    unsigned int   UP;
    long    readSize;
    long    writeSize;
    char    DevicePath[256];
    unsigned int   Handle;
    unsigned int   Version;
    char   ManufacturerString[128];
    char   ProductString[128];
} TEnumHIDInfo;

#define MAX_XKEY_DEVICES		128
#define PI_VID					0x5F3

typedef unsigned int (PIE_HID_CALL *PHIDDataEvent)(unsigned char *pData, unsigned int deviceID, unsigned int error);
typedef unsigned int (PIE_HID_CALL *PHIDErrorEvent)( unsigned int deviceID,unsigned int status);

void PIE_HID_CALL GetErrorString(int errNumb,char* EString,int size);
unsigned int PIE_HID_CALL EnumeratePIE(long VID, TEnumHIDInfo *info, long *count);
unsigned int PIE_HID_CALL GetXKeyVersion(long hnd);
unsigned int PIE_HID_CALL SetupInterfaceEx(long hnd);
void  PIE_HID_CALL CloseInterface(long hnd);
void  PIE_HID_CALL CleanupInterface(long hnd);
unsigned int PIE_HID_CALL ReadData(long hnd, unsigned char *data);
unsigned int PIE_HID_CALL BlockingReadData(long hnd, unsigned char *data, int maxMillis);
unsigned int PIE_HID_CALL WriteData(long hnd, unsigned char *data);
unsigned int PIE_HID_CALL FastWrite(long hnd, unsigned char *data);
unsigned int PIE_HID_CALL ReadLast(long hnd, unsigned char *data);
unsigned int PIE_HID_CALL ClearBuffer(long hnd);
unsigned int PIE_HID_CALL GetReadLength(long hnd);
unsigned int PIE_HID_CALL GetWriteLength(long hnd);
unsigned int PIE_HID_CALL SetDataCallback(long hnd, PHIDDataEvent pDataEvent);
unsigned int PIE_HID_CALL SetErrorCallback(long hnd, PHIDErrorEvent pErrorCall);
#ifdef _WIN32
void PIE_HID_CALL DongleCheck2(int k0, int k1, int k2, int k3, int n0, int n1, int n2, int n3, int &r0, int &r1, int &r2, int &r3);
#endif
void PIE_HID_CALL SuppressDuplicateReports(long hnd,bool supp);
void PIE_HID_CALL DisableDataCallback(long hnd,bool disable);
bool PIE_HID_CALL IsDataCallbackDisabled(long hnd);
bool PIE_HID_CALL GetSuppressDuplicateReports(long hnd);


#ifdef __cplusplus
}
#endif


#endif /* PIE_HID_H__ */
