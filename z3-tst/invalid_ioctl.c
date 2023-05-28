#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <assert.h>

#include "../dicedev.h"
#include "common.h"


static void chk(int fd, int req, char *err) {
	if (ioctl(fd, req, 0x01) != -1) {
		fprintf(stderr, "ioctl %s succeeded\n", err);
		exit(-1);
	}
}

int main() {
	int fd = do_open0();
	chk(fd, DICEDEV_IOCTL_CREATE_SET, "buffer_resize");
	chk(fd, DICEDEV_IOCTL_RUN, "create_buffer");
	chk(fd, DICEDEV_IOCTL_WAIT, "run");
	
	do_close(fd);
	return 0;
}
