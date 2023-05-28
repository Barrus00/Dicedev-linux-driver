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

#define SIZE 0x1000

int main() {
	int fd = do_open0();
	int bfd = do_create_buf(fd, SIZE, 0x0);

	char *buffer = (char *) mmap(0, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0);
	if (buffer == MAP_FAILED)
		syserr("mmap");

	for (int i = 0; i < 0x10; i++)
		buffer[i] = i % 2 ? 0xab : 0xcd;

	do_run_and_wait_with_err(fd, bfd, 0, 0x10, bfd);

	do_close(bfd);
	do_close(fd);
	return 0;
}
