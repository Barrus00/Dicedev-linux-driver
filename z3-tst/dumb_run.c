#include <signal.h>
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
	int bfd = do_create_buf(fd, SIZE, 0);
	do_run(fd, bfd, 0, 4, bfd);
	do_wait(fd, 0);
	do_run_with_err(fd, 1, 0, 4, bfd);
	int crap_fd = 1;
	do_run_with_err(fd, bfd, 0, 4, crap_fd);
	do_close(bfd);
	do_close(fd);
	return 0;
}