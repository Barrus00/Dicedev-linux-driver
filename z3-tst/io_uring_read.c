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

#include <liburing.h>

#include "../dicedev.h"
#include "common.h"

#define SIZE 0x3000


int main()
{
	struct io_uring ring;
	io_uring_queue_init(8, &ring, 0);
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;

	int fd = do_open0();
	int bfd = do_create_buf(fd, SIZE, 0x20);

	char *buffer = (char *) mmap(0, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0);

	if (buffer == MAP_FAILED)
		syserr("mmap");

	do_munmap(buffer + 0x2000, 0x1000);

	uint32_t *cmd = (uint32_t*) (buffer + 0x1000);

	uint32_t buf[4];
	buf[0] = DICEDEV_USER_CMD_GET_DIE_HEADER(5, FAIR);
	buf[1] = 0x20;
	buf[2] = DICEDEV_USER_CMD_GET_DIE_HEADER(5, FAIR);
	buf[3] = 0x20;

	sqe = io_uring_get_sqe(&ring);
	sqe->flags |= IOSQE_IO_LINK;
	if (!sqe) {	syserr("sqe1"); }
	io_uring_prep_write(sqe, bfd, &buf[0], sizeof(uint32_t) * 2, 0);
	
	sqe = io_uring_get_sqe(&ring);
	if (!sqe) {	syserr("sqe2"); }
	io_uring_prep_write(sqe, bfd, &buf[2], sizeof(uint32_t) * 2, 0);

	io_uring_submit(&ring);
	for (int i = 0; i < 2; ++i)
	{
		if (io_uring_wait_cqe(&ring, &cqe) < 0)
		{
			syserr("wait_cqe");
		}
		if (cqe->res < 0)
		{
			syserr("async1");
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	struct dice d;

	for (int i = 0; i < 3; ++i)
	{
		sqe = io_uring_get_sqe(&ring);
		if (!sqe) { syserr("sqe3"); }
		io_uring_prep_read(sqe, bfd, &d, sizeof(struct dice), 0);
	}
	io_uring_submit(&ring);


	for (int i = 0; i < 3; i++) {
		if (io_uring_wait_cqe(&ring, &cqe) < 0)
		{
			syserr("wait_cqe");
		}
		if (cqe->res < 0)
		{
			printf("RES= %d \n", cqe->res);
			syserr("async2");
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	assert(d.value == 3 && d.type == 5);

	close(bfd);
	io_uring_queue_exit(&ring);

	do_munmap(buffer, SIZE - 0x1000);
	do_close(fd);
}
