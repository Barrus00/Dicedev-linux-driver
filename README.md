# Dicedev driver
Author: Bartosz Ruszewski

# Building

Make sure that KDIR variable in Makefile is set to the kernel source/headers directory.

Then run:

```
    make
    sudo make install
```

This will build `dicedev.ko` module which can be loaded into kernel using `insmod` command.


## Solution description

### Structure

Dicedev driver is separated into parts:

* 'dicedev' - device provided structures, enums and defines
* 'dicedev_base' - basic dicedev char device and pci driver operations implementation.
* 'dicedev_buffer' - dicedev buffer char device operations implementation.
* 'dicedev_pt' - utilities for dicedev page table operations.
* 'dicedev_utils' - dicedev utility functions to communicate with hardware.
* 'dicedev_wt' - dicedev work thread implementation.

### Overview

Dicedev buffers are created by dicedev ioctl interface. Using these buffers user can send commands to device,
and receive responses from devices. Commands are send via dicedev `ioctl(DICEDEV_IOCTL_RUN)` action, or via
dicedev buffer `write()` operation. Both of these operations create a new work task in dicedev driver. This task is
then sent to dicedev work thread, which executes task one by one in FIFO order.
There's at most one running task per device command queue, because of the way dice hardware works I think that it's not possible to be 100% sure
which context caused the device failure if there are multiple different context tasks in the queue (Because device still processes,
commands during the interrupt, and current task number could change during interrupt handle, so inserting FENCE after each operation wont work),
that's why I've decided to limit the number of running tasks to one.
After task is finished, dicedev work thread sends signals to waiting processes, restarts device if needed, and puts next task in the queue.
Dicedev device does not clear the command queue after device restart, so the restart will take place only after the current task is finished.

Waiting processes are hanging on the contexts wait queue, and are woken up by dicedev work thread when task is finished.

There are two types of tasks:

* run task - a task created by dicedev `ioctl(DICEDEV_IOCTL_RUN)` operation. When it's processed, dicedev binds a slot to the buffer,
    this slot is unbinded when the task is finished. I consider each run as a full non-extendable operation, so we can securely
    unbind the slot after the task is finished.
* write task - a task created by dicedev buffer `write()` operation. When it's processed, dicedev binds a slot to the buffer,
    this slot is later unbinded only when buffer is destroyed and the context is finished. That's a little odd, but that's due to
    the dice hardware limitations. I don`t consider each write as a full non-extendable operation (like run), because it's possible
    to write to the buffer multiple times, and then read the outputs. There's no special action that could be treated as
    a guarantee that "no more write operations will come", so we have to wait for the buffer to be destroyed.
    That could make device go out of slots fast, but I hope that every user is cautious enough to not create too many buffers!
    Maybe if the device supported some binding with begin buffer offset, that issue could be solved, but it's not supported.


#### Seeds
Seed incrementation is enabled instantly when the corresponsing ioctl operation is called.

Seed change is delayed until the current task is finished, and changed when the next task is started.
