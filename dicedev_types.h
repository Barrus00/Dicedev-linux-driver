#ifndef DICEDEV_MAIN_H
#define DICEDEV_MAIN_H

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/list.h>
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>

#include "dicedev.h"
#include "dicedev_pt.h"


#define DICEDEV_NUM_SLOTS 16

#define DICEDEV_BUFFER_NO_SLOT -1
#define DICEDEV_DEFAULT_SEED 42

#define DICEDEV_FENCE_DONE_NUM 1


struct dicedev_buffer;

struct dicedev_device {
	struct pci_dev *pdev;
	struct cdev cdev;
	int idx;
	struct device *dev;
	void __iomem *bar;
	spinlock_t slock;

	size_t fence_count;

	struct dicedev_buffer *buff_slots[DICEDEV_NUM_SLOTS];
	size_t free_slots;
	bool increment_seeds;

	/* Work thread structure, responsible for sending commands to device */
	struct {
		struct task_struct *thread;

		bool running;
		wait_queue_head_t task_cond;
		wait_queue_head_t slot_cond;
		struct list_head pending_tasks;
		struct list_head running_tasks;
	} wt;
};


struct dicedev_context {
	struct dicedev_device *dev;
	spinlock_t slock;
	bool failed;
	size_t task_count;
	wait_queue_head_t wq;
	struct list_head allocated_buffers;
};


struct dicedev_buffer {
	struct dicedev_page_table *pt;
	struct dicedev_context *ctx;
	struct list_head context_buffers;

	bool destroyed;
	size_t seed;
	bool seed_chg;
	uint32_t allowed;
	size_t usage_count;
	int binded_slot;

	struct {
		size_t result_count;
		size_t offset;
	} reader;
};


enum dicedev_task_type {
	DICEDEV_TASK_TYPE_WRITE,
	DICEDEV_TASK_TYPE_RUN
};


struct dicedev_task_write {
	uint32_t *cmd;
	size_t cmd_size;

	struct {
		size_t offset;
	} it;
};


struct dicedev_task_run {
	struct dicedev_buffer *cBuff;
	size_t offset;
	size_t size;

	struct {
		uint32_t *curr_pg;
		size_t curr_pg_no;
		size_t curr_pg_off;
		size_t bytes_left;
	} it;
};


struct dicedev_task {
	enum dicedev_task_type type;

	struct dicedev_context *ctx;
	struct list_head lh;
	size_t result_count;
	struct dicedev_buffer *buff_out;

	union {
		struct dicedev_task_write write;
		struct dicedev_task_run run;
	};
};

#endif //DICEDEV_MAIN_H
