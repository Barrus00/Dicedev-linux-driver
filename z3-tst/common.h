#include <stdint.h>

void syserr(const char *fmt) {
	fprintf(stderr,"ERROR %s (%d; %s)\n", fmt, errno, strerror(errno));
	exit(1);
}

int do_open(char *path) {
	int fd;
	if ((fd = open(path, O_RDWR)) < 0)
		syserr("open");
	return fd;
}

int do_open0() {
	return do_open("/dev/dice0");
}

int do_open1() {
	return do_open("/dev/dice1");
}

int do_create_buf(int fd, int size, uint64_t allowed) {
	struct dicedev_ioctl_create_set cb = { size, allowed };

	int bfd;
	if ((bfd = ioctl(fd, DICEDEV_IOCTL_CREATE_SET, &cb)) < 0)
		syserr("create_buffer");
	return bfd;
}

void do_close(int fd) {
	if (close(fd) < 0)
		syserr("close");
}

void do_munmap(void *addr, size_t len) {
	if (munmap(addr, len) < 0)
		syserr("munmap");
}

void do_run(int fd, int cfd, uint32_t addr, uint32_t size, int bfd) {
	struct dicedev_ioctl_run run = {cfd, addr, size, bfd};
	int i;
	if (ioctl(fd, DICEDEV_IOCTL_RUN, &run))
		syserr("run");
}

void do_run_with_err(int fd, int cfd, uint32_t addr, uint32_t size, int bfd) {
	struct dicedev_ioctl_run run = {cfd, addr, size, bfd};
	int i;
	if (ioctl(fd, DICEDEV_IOCTL_RUN, &run) != -1)
		syserr("run should fail");
}

void do_wait(int fd, uint32_t cnt) {
	struct dicedev_ioctl_wait wait = {cnt};
	if (ioctl(fd, DICEDEV_IOCTL_WAIT, &wait))
		syserr("wait");
}

void do_wait_for_err(int fd, uint32_t cnt) {
	struct dicedev_ioctl_wait wait = {cnt};
	if (ioctl(fd, DICEDEV_IOCTL_WAIT, &wait) != -1)
		syserr("wait should fail");
}


void do_run_and_wait_with_err(int fd, int cfd, uint32_t addr, uint32_t size, int bfd) {
	struct dicedev_ioctl_run run = {cfd, addr, size, bfd};
	int i;
	if(ioctl(fd, DICEDEV_IOCTL_RUN, &run))
		syserr("run failed");
	do_wait_for_err(fd, 0);
}
