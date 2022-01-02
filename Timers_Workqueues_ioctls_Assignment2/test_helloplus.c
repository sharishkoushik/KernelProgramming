#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/poll.h>
#include <linux/ioctl.h>

#define MAGIC 'z'
#define GET_DELAY      _IOR(MAGIC, 1, int *)
#define SET_DELAY      _IOW(MAGIC, 2, int *)

int ioctl(int fd, unsigned long request, ...);
int main(void)
{
 int fd, i=5;
 long Delay=1;
 int Value=0;
 int ret;

 if((fd = open("/dev/helloplus0", O_RDWR))<0) {
    perror("open error\n");
    exit(EXIT_FAILURE);
  }
 // Demo only. No error checking is performed. Use strace to see errors and errno
  while (1){
  printf("\nHit return..");
  getchar();
  ret = ioctl(fd,  GET_DELAY, &Value);
  printf("Delay is = %d\n", Value);
  Delay +=5;
  printf("\nSet Delay" );
  ret = ioctl( fd, SET_DELAY, &Delay);
  ret = ioctl( fd,  GET_DELAY, &Value);
  printf("\nAfter ioctl call Delay = %d\n",Value);
  }

  close(fd);
  return 0;
}
