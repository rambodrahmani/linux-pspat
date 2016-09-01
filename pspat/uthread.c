#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>


int main()
{
    const char *devname = "/dev/pspat";
    int ret;
    int fd;

    fd = open(devname, O_RDWR);
    if (fd < 0) {
        perror("open()");
        return -1;
    }

    ret = ioctl(fd, 70, NULL);
    printf("ioctl() --> %d\n", ret);

    close(fd);

    return 0;
}
