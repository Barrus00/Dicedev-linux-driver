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

int main() {
	int fd = do_open0();
	int fd2 = do_open1();
	int bfd = do_create_buf(fd, SIZE, 0);
	do_run(fd, bfd, 0, 4, bfd);
	do_run_with_err(fd2, bfd, 0, 4,bfd);

	do_close(bfd);
	do_close(fd);
	do_close(fd2);

	return 0;
}