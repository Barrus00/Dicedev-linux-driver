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

#define SIZE 0x3000

#include "common.h"

int main() {
	int fd = do_open0();
	int bfd = do_create_buf(fd, SIZE, 0x20);

	char *buffer = (char *) mmap(0, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0);
	if (buffer == MAP_FAILED)
		syserr("mmap");

	uint32_t *cmd = (uint32_t*) (buffer + 0x1000);
	cmd[0] = DICEDEV_USER_CMD_GET_DIE_HEADER(5000, FAIR);

	md[1] = 0x20;
	cmd[2] = DICEDEV_USER_CMD_GET_DIE_HEADER(5000, FAIR);
	cmd[3] = 0x20;
	cmd[4] = DICEDEV_USER_CMD_GET_DIE_HEADER(5000, FAIR);
	cmd[5] = 0x20;
	
	do_run_and_wait_with_err(fd, bfd, 0x1000, sizeof(uint32_t) * 6, bfd);

	do_close(bfd);
	do_munmap(buffer, SIZE);
	do_close(fd);
	return 0;
}
