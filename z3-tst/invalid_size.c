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

int main() {
	int fd = do_open0();
	int bfd = do_create_buf(fd, 0x1000, 0);

	do_run_with_err(fd, bfd, 0x1000, 1, bfd);
	do_close(fd);
	return 0;
}
