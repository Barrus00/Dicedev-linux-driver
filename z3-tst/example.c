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
#include "../dicedev.h"

#include "common.h"

#define SIZE 0x3000


int main()
{
		int fd0 = do_open0();
		int fd1 = do_open1();
		int bfd = do_create_buf(fd0, SIZE, 0x20);
		
		char *buffer = (char *) mmap(0, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0);

		if (buffer == MAP_FAILED)
			syserr("mmap");

		for (int i = 0; i < SIZE; i++)
			buffer[i] = 0x13;

		for (int i = 0; i < SIZE; ++i)
			if (buffer[i] != 0x13) return -1;

		do_munmap(buffer, SIZE);
		do_close(bfd);
}
