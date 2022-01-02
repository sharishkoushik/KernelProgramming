#include<stdio.h>
#include<fcntl.h>
#include<unistd.h>
#include<string.h>
#include<sys/ioctl.h>


#define MAGIC 'z'
#define GETINV  _IOR(MAGIC, 1, int *)
#define SETLED  _IOW(MAGIC, 2, int *)
#define SETINV  _IOW(MAGIC, 3, int *)

#define LED_SCROLL      0x01
#define LED_NUML        0x02
#define LED_CAPSL       0x04
#define ALL_LEDS_ON     0x07

#define BLINKPLUS "/dev/blinkplus"

int main(int argc, char* argv[])
{
	int fd;
	int i=0;
	unsigned int led;
	unsigned long intv=1;
	int result;
	int cmd;
	
	fd = open(BLINKPLUS,O_RDWR);
	if(fd == -1)
	{
		printf("Error in open\n");
		return -1;
	}
	/* No error checking for failed ioctl is done. Demo only */
	while (intv<4){
	printf("Entered the while loop\n");
	printf("\n Setting SCROLL LOCK\n");
	led= LED_SCROLL;
	result = ioctl(fd, SETLED,&led );
	sleep(10);
	printf("\n Setting NUM LOCK\n");
	led= LED_NUML;
	result = ioctl(fd, SETLED,&led );
	sleep(10);
	printf("\n Setting CAPS LOCK\n");
	led= LED_CAPSL;
	result = ioctl(fd, SETLED,&led );
	sleep(10);
	printf("\n Setting ALL LEDS\n");
	led= ALL_LEDS_ON;
	result = ioctl(fd, SETLED,&led );
	sleep(10);
	printf("\nGETTING LED BLINK INTERVAL\n");
    result = ioctl(fd, GETINV, &cmd);
    printf("LED IS BLINKING AT INTERVAL: %d (sec)\n",cmd);
	sleep(10);
	intv++ ;
	printf("\n Setting LED BLINK INTERVAL to %d seconds\n",intv);
	result = ioctl(fd, SETINV,&intv );
	sleep(10);
	}
}
