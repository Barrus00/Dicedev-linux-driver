#include "dicedev_utils.h"
#include "dicedev_buffer.h"

#include "dicedev_wt.h"


#define DICEDEV_CMD_TYPE_MASK 0xF
#define DICEDEV_MAX_FENCE_VAL (1 << 28)

#define GET_DIE_SLOT_MASK (((1 << 28) - 1) ^ ((1 << 25) - 1))
#define GET_DIE_NUM_MASK  (((1 << 20) - 1) ^ ((1 << 5) - 1))
#define NEW_SET_SLOT_MASK (((1 << 8) - 1) ^ ((1 << 5) - 1))

static int dicedev_wt_fn(void *data);

void dicedev_wt_init(struct dicedev_device *dev) {
	dev->wt.running = 1;
	INIT_LIST_HEAD(&dev->wt.running_tasks);
	INIT_LIST_HEAD(&dev->wt.pending_tasks);
	init_waitqueue_head(&dev->wt.task_cond);
	init_waitqueue_head(&dev->wt.slot_cond);
	dev->wt.thread = kthread_run(dicedev_wt_fn, dev, "dicedev_wt");
}


void dicedev_wt_add_task(struct dicedev_context *ctx, struct dicedev_task *task) {
	unsigned long flags;
	struct dicedev_device *dev = ctx->dev;

	spin_lock_irqsave(&dev->slock, flags);
	list_add_tail(&task->lh, &dev->wt.pending_tasks);
	wake_up_interruptible(&dev->wt.task_cond);
	spin_unlock_irqrestore(&dev->slock, flags);
}


static inline uint32_t dicedev_task_get_next_word(struct dicedev_task *task) {
	struct dicedev_task_run *run_task;

	switch (task->type) {
		case DICEDEV_TASK_TYPE_WRITE:
			BUG_ON(task->write.cmd_size == task->write.it.offset);

			return task->write.cmd[task->write.it.offset++];
		case DICEDEV_TASK_TYPE_RUN:
			run_task = &task->run;

			if (run_task->it.curr_pg_off == PAGE_SIZE / 4) {
				run_task->it.curr_pg_off = 0;
				run_task->it.curr_pg = run_task->cBuff->pt->pages[++run_task->it.curr_pg_no].page;
			}

			run_task->it.bytes_left -= 4;

			return run_task->it.curr_pg[run_task->it.curr_pg_off++];
		default:
			BUG();
	}
}


static void dicedev_task_init_it(struct dicedev_task *task) {
	switch (task->type) {
	case DICEDEV_TASK_TYPE_WRITE:
		task->write.it.offset = 0;
		break;

	case DICEDEV_TASK_TYPE_RUN:
		task->run.it.curr_pg_no = task->run.offset / PAGE_SIZE;
		task->run.it.curr_pg_off = (task->run.offset % PAGE_SIZE) % 4;
		task->run.it.curr_pg = task->run.cBuff->pt->pages[task->run.it.curr_pg_no].page;
		task->run.it.bytes_left = task->run.size;
		break;

	default:
		BUG();
		break;
	}
}

static void __wt_run_task(struct dicedev_task *task) {
	struct dicedev_context *ctx = task->ctx;
	struct dicedev_device *dev = ctx->dev;
	struct dicedev_buffer *buff_out = task->buff_out;
	size_t bytes_left = task->type == DICEDEV_TASK_TYPE_WRITE ? task->write.cmd_size : task->run.size;
	size_t res_count;
	uint32_t cmd[2];
	unsigned long flags;

	spin_lock_irqsave(&dev->slock, flags);
	list_add_tail(&task->lh, &dev->wt.running_tasks);
	spin_unlock_irqrestore(&dev->slock, flags);

	BUG_ON(buff_out->binded_slot == DICEDEV_BUFFER_NO_SLOT);

	dicedev_task_init_it(task);

	while (bytes_left > 0) {
		cmd[0] = dicedev_task_get_next_word(task);

		switch (cmd[0] & DICEDEV_CMD_TYPE_MASK) {
		case DICEDEV_USER_CMD_TYPE_NOP:
			bytes_left -= 4;
			break;
		case DICEDEV_USER_CMD_TYPE_GET_DIE:
			if (bytes_left < 4) {
				goto err_ctx_fail;
			}

			cmd[1] = dicedev_task_get_next_word(task);

			if ((cmd[0] & GET_DIE_SLOT_MASK) != 0) {
				goto err_ctx_fail;
			}

			if ((cmd[1] & ~buff_out->allowed) != 0) {
				goto err_ctx_fail;
			}

			res_count = cmd[0] & GET_DIE_NUM_MASK;

			task->result_count += cmd[0] & GET_DIE_NUM_MASK;

			if (task->result_count >= buff_out->pt->max_size) {
				printk(KERN_ERR "Result count %d overflow\n", (int)task->result_count);
				printk(KERN_ERR "Max size %d\n", (int)buff_out->pt->max_size);
				printk(KERN_ERR "Result count overflow\n");
				goto err_ctx_fail;
			}

			cmd[0] |= buff_out->binded_slot << 24;

			feed_cmd(dev, cmd, 2);

			bytes_left -= 8;
			break;
		case DICEDEV_USER_CMD_TYPE_NEW_SET:
			if ((cmd[0] & NEW_SET_SLOT_MASK) != 0) {
				goto err_ctx_fail;
			}

			cmd[0] |= buff_out->binded_slot << 4;

			feed_cmd(dev, cmd, 1);

			bytes_left -= 4;
			break;
		default:
			goto err_ctx_fail;
		}
	}

//	printk(KERN_ERR "Task sent\n");

	return;

err_ctx_fail:

	ctx->failed = true;

	return;
}


static inline void __update_done_tasks(struct dicedev_device *dev) {
	struct dicedev_task *task;
	struct list_head *lh;
	uint32_t last_fence = dev->fence.last_handled;
	uint32_t current_fence;
	unsigned long flags;

	spin_lock_irqsave(&dev->slock, flags);
	dev->fence.reached = false;

	current_fence = dicedev_ior(dev, DICEDEV_CMD_FENCE_LAST);

	while (last_fence != current_fence) {
//		printk("TASK DONE!\n");
		lh = dev->wt.running_tasks.next;
		list_del(lh);
		task = container_of(lh, struct dicedev_task, lh);
		spin_lock_irqsave(&task->ctx->slock, flags);
		task->ctx->task_count--;
		task->buff_out->reader.result_count += task->result_count;

		spin_unlock_irqrestore(&task->ctx->slock, flags);
		wake_up_interruptible(&task->ctx->wq);

		if (task->type == DICEDEV_TASK_TYPE_RUN) {
			unbind_slot(dev, task->buff_out);
		}
		kfree(task);

		last_fence = (last_fence + 1) % DICEDEV_MAX_FENCE_VAL;
	}

	dev->fence.last_handled = last_fence;

	spin_unlock_irqrestore(&dev->slock, flags);
}


static int dicedev_wt_fn(void *data) {
	struct dicedev_device *dev = data;
	struct dicedev_task *task;
	struct list_head *task_lh;
	uint32_t fence_cmd;
	unsigned long flags;

	while (dev->wt.running) {
		__update_done_tasks(dev);

		if (list_empty(&dev->wt.pending_tasks)) {
			wait_event_interruptible(dev->wt.task_cond, !list_empty(&dev->wt.pending_tasks) || !dev->wt.running || dev->fence.reached);
			continue;
		}

		spin_lock_irqsave(&dev->slock, flags);

		/* Get next task */
		task_lh = dev->wt.pending_tasks.next;
		list_del(task_lh);
		task = container_of(task_lh, struct dicedev_task, lh);
//		printk(KERN_ERR "dicedev: task %p\n", task);

		while (dev->free_slots == 0 && task->buff_out->binded_slot == DICEDEV_BUFFER_NO_SLOT) {
			spin_unlock_irqrestore(&dev->slock, flags);
			wait_event_interruptible(dev->wt.slot_cond, dev->free_slots > 0);
			spin_lock_irqsave(&dev->slock, flags);
		}

		if (task->buff_out->binded_slot == DICEDEV_BUFFER_NO_SLOT) {
			bind_slot(dev, task->buff_out, get_slot(dev));
//			printk(KERN_ERR "Binded slot %d", task->buff_out->binded_slot);
			dicedev_buffer_init_reader(task->buff_out);
		}

		spin_unlock_irqrestore(&dev->slock, flags);

		__wt_run_task(task);


		spin_lock_irqsave(&dev->slock, flags);
		dev->fence.count = (dev->fence.count + 1) % DICEDEV_MAX_FENCE_VAL;
		fence_cmd = DICEDEV_USER_CMD_FENCE_HEADER(dev->fence.count);
		dicedev_iow(dev, DICEDEV_CMD_FENCE_WAIT, dev->fence.count);
		feed_cmd(dev, &fence_cmd, 1);
		spin_unlock_irqrestore(&dev->slock, flags);

//		printk(KERN_ERR "dicedev: fence %d\n", dev->fence.count);
	}

	return 0;
}