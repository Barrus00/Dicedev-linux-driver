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

#define SIZE 4096
#define COUNT 1000

int bfds[COUNT];
char *bufs[COUNT];

int main() {

	int fd = do_open0();

	for (int i = 0; i < COUNT; i++) {
		bfds[i] = do_create_buf(fd, SIZE, 0x20);
		bufs[i] = (char *) mmap(0, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, bfds[i], 0);
		if (bufs[i] == MAP_FAILED)
			syserr("mmap");
	}
	
	uint32_t *cmd = (uint32_t*) bufs[0];
	cmd[0] = DICEDEV_USER_CMD_GET_DIE_HEADER(5, FAIR);
	cmd[1] = 0x20;
	cmd[2] = DICEDEV_USER_CMD_GET_DIE_HEADER(5, FAIR);
	cmd[3] = 0x20;

	for (int i = 1; i < COUNT; i++) {
		do_run(fd, bfds[0], 0, sizeof(uint32_t) * 4, bfds[i]);
		do_wait(fd, 0);
	}
	
	for (int i = 1; i < COUNT; i++) {
		struct dice * buf_read = (struct dice *) bufs[i];
		assert(buf_read[7].value == 5 && buf_read[9].type == 5);
	}

	for (int i = 0; i < COUNT; i++) {
		do_munmap(bufs[i], SIZE);
		do_close(bfds[i]);
	}

	do_close(fd);	
	
	return 0;
}
