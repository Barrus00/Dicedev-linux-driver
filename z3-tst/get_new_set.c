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

#define SIZE 0x3000


int main()
{
		int fd = do_open0();
		int bfd = do_create_buf(fd, SIZE, 0x20);
		
		char *buffer = (char *) mmap(0, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0);

		if (buffer == MAP_FAILED)
			syserr("mmap");

		do_munmap(buffer + 0x2000, 0x1000);
		
		uint32_t *cmd = (uint32_t*) (buffer + 0x1000);
		cmd[0] = DICEDEV_USER_CMD_GET_DIE_HEADER(5, FAIR);
		cmd[1] = 0x20;
		cmd[2] = DICEDEV_USER_CMD_NEW_SET;
		cmd[3] = DICEDEV_USER_CMD_GET_DIE_HEADER(5, FAIR);
		cmd[4] = 0x20;

		do_run(fd, bfd, 0x1000, sizeof(uint32_t) * 5, bfd);
		do_wait(fd, 0);

		struct dice * buf_read = (struct dice *) buffer;
		int i;
		for (i = 0; i < 5; ++i)
		{
			assert(buf_read[i].value == buf_read[i + 5].value && buf_read[i].type == buf_read[i + 5].type);
		}

		do_close(bfd);
		do_munmap(buffer, SIZE - 0x1000);
		do_close(fd);
}
