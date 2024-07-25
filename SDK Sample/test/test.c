
#include "PieHid32.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>


void print_buf(char *data, int len)
{
	int i;
	for (i = 0; i < len; i++) {
		printf("%02hhx ", data[i]);
		if ((i+1) % 8 == 0)
			printf("  ");
		if ((i+1) % 16 == 0)
			printf("\n");
	}
	printf("\n\n");

}

int main(void)
{
	TEnumHIDInfo info[128];
	long count;
	int i;
	long handle = -1;
	
	unsigned res = EnumeratePIE(PI_VID, info, &count);
	
	for (i = 0; i < count; i++) 
	{
		TEnumHIDInfo *dev = &info[i];
		printf("Found XKeys Device:\n");
		printf("\tPID: %04x\n", dev->PID);
		printf("\tUsage Page: %04x\n", dev->UP);
		printf("\tUsage:      %04x\n", dev->Usage);
		printf("\tVersion: %d\n\n", dev->Version);
		if (dev->UP == 0x000c && dev->writeSize > 0) 
		{
			/* This is the splat interface, */

			handle = dev->Handle;
			unsigned int res = SetupInterfaceEx(handle);
			if (res != 0) 
			{
				printf("Unable to open device. err: %d\n", res);
			}
			break;
         	}
	}
	
	if (handle < 0) {
		printf("Unable to open device\n");
		exit(1);
	}
	
	char data[80];
	while (1) {
		
		unsigned int res = 0;

		res  = ReadLast(handle, data);
		if (res == 0) {
			printf("LAST: \n");
			print_buf(data, 33);
			printf("ENDLAST\n\n");
		}

		res = 0;
		
		while (res == 0) {
			res = BlockingReadData(handle, data, 20);
			if (res == 0) {
				print_buf(data, 33);
			}
			else if (res == PIE_HID_READ_INSUFFICIENT_DATA) {
				printf(".");
				fflush(stdout);
			}	
			else {
				printf("Error Reading\n");
			}
		}
		
		printf("Sleeping\n");
		#if 1
		if (res != 0) {
			//usleep(10*1000); // Sleep 10 milliseconds.
			sleep(2); // 2 seconds
		}
		#endif
		
		//ClearBuffer(handle);
		
	}


	return 0;
}
