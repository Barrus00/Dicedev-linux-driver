#!/bin/sh
TESTS="example get_die get_sets get_new_set overflow invalid_ioctl large_wait large_alloc invalid_cmd dumb_run crossmap large_op page_fault invalid_size get_set_tricks change_seed not_allowed io_uring io_uring_read"


for x in $TESTS; do
        echo === $x ===
        if ./$x ; then
                echo OK
        else
                echo Failed
        fi
done

