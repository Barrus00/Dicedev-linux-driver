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
#define CMDS 500

int main() {
	int fd = do_open0();
	int bfd = do_create_buf(fd, CMDS * 5 * 4 * sizeof(struct dice), 0x20);
	int cmdfd = do_create_buf(fd, CMDS * 4 * sizeof(uint32_t), 0x20);
	
	char *buffer = (char *) mmap(0, CMDS * 4 * sizeof(struct dice), PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0);
	uint32_t *cmd = (uint32_t *) mmap(0, CMDS * 4 * sizeof(uint32_t), PROT_READ | PROT_WRITE, MAP_SHARED, cmdfd, 0);
	if (cmd == MAP_FAILED)
		syserr("mmap");

	for (int i = 0; i < CMDS; i++) {
		cmd[i * 4] = DICEDEV_USER_CMD_GET_DIE_HEADER(5, FAIR);
		cmd[i * 4 + 1] = 0x20;
		cmd[i * 4 + 2] = DICEDEV_USER_CMD_GET_DIE_HEADER(5, FAIR);
		cmd[i * 4 + 3] = 0x20;
	}
	do_run(fd, cmdfd, 0, sizeof(uint32_t) * 4 * CMDS, bfd);
	do_wait(fd, 0);
	
	int i;
	struct dice * buf_read = (struct dice *) buffer;
	assert(buf_read[1001].value == 3 && buf_read[1001].type == 5);
	
	do_close(bfd);
	do_close(cmdfd);
	do_munmap(cmd, CMDS * 4 * sizeof(uint32_t));
	do_close(fd);
	return 0;
}
