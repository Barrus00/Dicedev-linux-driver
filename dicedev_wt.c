#include "dicedev_utils.h"
#include "dicedev_buffer.h"

#include "dicedev_wt.h"


#define DICEDEV_CMD_TYPE_MASK 0xF
#define DICEDEV_MAX_FENCE_VAL (1 << 28)
#define DICEDEV_TASK_FENCE_VAL 42

#define GET_DIE_SLOT_MASK (((1 << 28) - 1) ^ ((1 << 25) - 1))
#define GET_DIE_NUM_MASK  (((1 << 20) - 1) ^ ((1 << 5) - 1))
#define NEW_SET_SLOT_MASK (((1 << 8) - 1) ^ ((1 << 5) - 1))

static int dicedev_wt_fn(void *data);

void dicedev_wt_init(struct dicedev_device *dev) {
	dev->wt.running_task = NULL;

	init_waitqueue_head(&dev->wt.event_cond);
	INIT_LIST_HEAD(&dev->wt.pending_tasks);

	dev->wt.thread = kthread_run(dicedev_wt_fn, dev, "dicedev_wt");
}


void dicedev_wt_add_task(struct dicedev_context *ctx, struct dicedev_task *task) {
	unsigned long flags;
	struct dicedev_device *dev = ctx->dev;

	spin_lock_irqsave(&dev->slock, flags);
	list_add_tail(&task->lh, &dev->wt.pending_tasks);
	wake_up_interruptible(&dev->wt.event_cond);
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


static int numbers_of_bits_set(uint32_t i)
{
	/* Source:
	 * https://stackoverflow.com/questions/109023/count-the-number-of-set-bits-in-a-32-bit-integer
	 */
	i = i - ((i >> 1) & 0x55555555);        // add pairs of bits
	i = (i & 0x33333333) + ((i >> 2) & 0x33333333);  // quads
	i = (i + (i >> 4)) & 0x0F0F0F0F;        // groups of 8
	return (i * 0x01010101) >> 24;          // horizontal sum of bytes
}


static void __wt_run_task(struct dicedev_task *task) {
	struct dicedev_context *ctx = task->ctx;
	struct dicedev_device *dev = ctx->dev;
	struct dicedev_buffer *buff_out = task->buff_out;
	size_t bytes_left = task->type == DICEDEV_TASK_TYPE_WRITE ? task->write.cmd_size : task->run.size;
	size_t res_count;
	uint32_t cmd[2];

	dev->wt.running_task = task;

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

			/* Dice device always produce num * (number of allowed dices) results */
			res_count *= numbers_of_bits_set(cmd[1]);

			task->result_count += cmd[0] & GET_DIE_NUM_MASK;

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

	return;

err_ctx_fail:

	ctx->failed = true;

	return;
}


enum wt_event {
	WT_NO_EVENT,
	WT_EVENT_TASK_GET_NEXT,
	WT_EVENT_TASK_BIND_SLOT,
	WT_EVENT_TASK_RUN,
	WT_EVENT_TASK_FINISH,
	WT_PARK,
	WT_STOP
};


struct wt_state {
	struct dicedev_device *dev;
	struct dicedev_task *next_task;
};


static inline enum wt_event __wt_get_event(struct wt_state *wtState) {
	unsigned long flags;

	if (kthread_should_stop()) {
		return WT_STOP;
	}

	if (kthread_should_park()) {
		return WT_PARK;
	}

	if (wtState->dev->fence_state == DICEDEV_FENCE_STATE_REACHED) {
		return WT_EVENT_TASK_FINISH;
	}

	if (wtState->next_task == NULL) {
		return !list_empty(&wtState->dev->wt.pending_tasks) ? WT_EVENT_TASK_GET_NEXT : WT_NO_EVENT;
	}

	if (wtState->dev->fence_state != DICEDEV_FENCE_STATE_NONE) {
		return WT_NO_EVENT;
	}

	spin_lock_irqsave(&wtState->dev->slock, flags);
	if (wtState->next_task->buff_out->binded_slot == DICEDEV_BUFFER_NO_SLOT) {
		spin_unlock_irqrestore(&wtState->dev->slock, flags);

		return wtState->dev->free_slots > 0 ? WT_EVENT_TASK_BIND_SLOT : WT_NO_EVENT;
	}
	spin_unlock_irqrestore(&wtState->dev->slock, flags);

	return WT_EVENT_TASK_RUN;
}


static inline void __task_destroy(struct dicedev_task *task) {
	struct dicedev_device *dev = task->ctx->dev;
	unsigned long flags;

	spin_lock_irqsave(&dev->slock, flags);

	if (task->buff_out->binded_slot != DICEDEV_BUFFER_NO_SLOT) {
		if (task->type == DICEDEV_TASK_TYPE_RUN || task->ctx->failed) {
			unbind_slot(dev, task->buff_out);
		}
	}

	spin_unlock_irqrestore(&dev->slock, flags);


	spin_lock_irqsave(&task->buff_out->dev->slock, flags);
	task->ctx->task_count--;
	task->buff_out->reader.result_count += task->result_count;

	wake_up_interruptible(&task->ctx->wq);
	spin_unlock_irqrestore(&task->buff_out->dev->slock, flags);

	kfree(task);
}


static int dicedev_wt_fn(void *data) {
	struct dicedev_device *dev = data;
	struct wt_state wtState = {
		.dev = dev,
		.next_task = NULL
	};
	struct dicedev_task *task;
	struct list_head *task_lh, *tmp;
	uint32_t fence_cmd;
	unsigned long flags;

	while (!kthread_should_stop()) {
		switch (__wt_get_event(&wtState)) {
		case WT_STOP:
			/* In this case, kthread_should_stop() is true,
			 * so the loop will be broken anyway */
			continue;

		case WT_PARK:
			kthread_parkme();
			continue;

		case WT_NO_EVENT:
			wait_event_interruptible(dev->wt.event_cond,
						 __wt_get_event(&wtState) != WT_NO_EVENT);
			continue;

		case WT_EVENT_TASK_GET_NEXT:
			spin_lock_irqsave(&dev->slock, flags);
			task_lh = dev->wt.pending_tasks.next;
			list_del(task_lh);
			spin_unlock_irqrestore(&dev->slock, flags);

			task = container_of(task_lh, struct dicedev_task, lh);

			if (task->ctx->failed) {
				__task_destroy(task);
				continue;
			}

			wtState.next_task = task;
			break;

		case WT_EVENT_TASK_BIND_SLOT:
			task = wtState.next_task;

			spin_lock_irqsave(&dev->slock, flags);
			bind_slot(dev, task->buff_out, get_slot(dev));
			dicedev_buffer_init_reader(task->buff_out);
			spin_unlock_irqrestore(&dev->slock, flags);

			break;

		case WT_EVENT_TASK_RUN:
			dev->fence_state = DICEDEV_FENCE_STATE_WAITING;

			__wt_run_task(wtState.next_task);

			wtState.next_task = NULL;

			/* Task sent, set fence */
			spin_lock_irqsave(&dev->slock, flags);

			fence_cmd = DICEDEV_USER_CMD_FENCE_HEADER(DICEDEV_TASK_FENCE_VAL);
			dicedev_iow(dev, DICEDEV_CMD_FENCE_WAIT, DICEDEV_TASK_FENCE_VAL);
			feed_cmd(dev, &fence_cmd, 1);
			spin_unlock_irqrestore(&dev->slock, flags);
			break;

		case WT_EVENT_TASK_FINISH:
			BUG_ON(dev->wt.running_task == NULL);

			task = dev->wt.running_task;

			__task_destroy(task);

			if (dev->failed) {
				restart_device(dev);
				dev->failed = false;
			}

			dev->fence_state = DICEDEV_FENCE_STATE_NONE;
			dev->wt.running_task = NULL;
			break;

		default:
			BUG();
		}
	}

	spin_lock_irqsave(&dev->slock, flags);

	list_for_each_safe(task_lh, tmp, &dev->wt.pending_tasks) {
		task = container_of(task_lh, struct dicedev_task, lh);

		list_del(task_lh);

		__task_destroy(task);
	}

	if (dev->wt.running_task != NULL) {
		task = dev->wt.running_task;
		dev->wt.running_task = NULL;
		__task_destroy(task);
	}

	spin_unlock_irqrestore(&dev->slock, flags);

	return 0;
}