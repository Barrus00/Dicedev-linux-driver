TESTS=example get_die get_sets get_new_set overflow invalid_ioctl large_wait large_alloc invalid_cmd dumb_run crossmap large_op page_fault invalid_size get_set_tricks not_allowed change_seed 

all: $(TESTS) io_uring io_uring_read

io_uring:
	$(CC) io_uring.c -luring -o io_uring  

io_uring_read:
	$(CC) io_uring_read.c -luring -o io_uring_read

$(TESTS): %: %.o
	$(CC) -std=c11 -Wall -o $@ $< 

clean:
	rm -f $(TESTS) io_uring io_uring_read *.o

