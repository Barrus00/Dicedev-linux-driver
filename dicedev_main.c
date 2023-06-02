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

#include "dicedev.h"
#include "dicedev_pt.h"

#define DICEDEV_DEFAULT_SEED 42
#define DICEDEV_MAX_DEVICES 256
#define DICEDEV_NUM_SLOTS 16
#define DICEDEV_MAX_BUFF_SIZE (1 << 20)

#define DICEDEV_FENCE_DONE_NUM 1

#define DICEDEV_BUFFER_NO_SLOT -1

#define DICEDEV_MAX_ALLOWED (((uint64_t) 1 << 33) - 1)

#define DICEDEV_CMD(first_byte, second_byte, third_byte, fourth_byte) \
	((uint32_t) (first_byte) << 24 | (uint32_t) (second_byte) << 16 | (uint32_t) (third_byte) << 8 | (uint32_t) (fourth_byte))

#define DICEDEV_CMD_TYPE_MASK 0xF

#define GET_DIE_SLOT_MASK (((1 << 28) - 1) ^ ((1 << 25) - 1))
#define NEW_SET_SLOT_MASK (((1 << 8) - 1) ^ ((1 << 5) - 1))

static dev_t dicedev_devno;
static struct dicedev_device *dicedev_devices[DICEDEV_MAX_DEVICES];
static DEFINE_MUTEX(dicedev_devices_lock);
static struct class dicedev_class = {
	.name = "dicedev",
	.owner = THIS_MODULE,
};

struct dicedev_buffer;
struct dicedev_context;


struct dicedev_task {
	struct dicedev_context *ctx;
	struct dicedev_buffer *cBuff;
	struct dicedev_buffer *dBuff;
	size_t offset;
	size_t size;

	struct {
		uint32_t *curr_pg;
		size_t curr_pg_no;
		size_t curr_pg_off;
		size_t bytes_left;
	} cBuff_state;

	struct list_head lh;
};


struct dicedev_device {
	struct pci_dev *pdev;
	struct cdev cdev;
	int idx;
	struct device *dev;
	void __iomem *bar;
	spinlock_t slock;

	wait_queue_head_t slot_wq;

	struct dicedev_buffer *buff_slots[DICEDEV_NUM_SLOTS];
	size_t free_slots;
	bool increment_seeds;

	/* Work thread structure, responsible for sending commands to device */
	struct {
		bool running;
		struct task_struct *thread;
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
	size_t running_tasks_no;
	wait_queue_head_t wq;
	struct list_head allocated_buffers;
	struct list_head other_contexts;
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
};


/* Hardware handling */
static inline void dicedev_iow(struct dicedev_device *dev, uint32_t reg, uint32_t val)
{
	iowrite32(val, dev->bar + reg);
}


static inline uint32_t dicedev_ior(struct dicedev_device *dev, uint32_t reg)
{
	return ioread32(dev->bar + reg);
}


static void dicedev_ctx_fail(struct dicedev_context *ctx)
{
	spin_lock(&ctx->slock);
	ctx->failed = 1;
	spin_unlock(&ctx->slock);
}


static void inline feed_cmd(struct dicedev_device *dev, uint32_t *cmd, size_t cmd_size)
{
	for (size_t i = 0; i < cmd_size; i++) {
		while (dicedev_ior(dev, CMD_MANUAL_FREE) == 0) {
			// Wait for free slot.
		}

		dicedev_iow(dev, CMD_MANUAL_FREE, cmd[i]);
	}
}


/* Should be called with slock held */
static void bind_slot(struct dicedev_device *dev, struct dicedev_buffer *buff, int slot)
{
	uint32_t cmd[4];

	BUG_ON(dev->buff_slots[slot] != NULL);

	buff->binded_slot = slot;
	buff->usage_count = 1; /* No need to use a lock here, only one thread process commands */
	dev->buff_slots[slot] = buff;
	dev->free_slots--;

	cmd[0] = DICEDEV_USER_CMD_BIND_SLOT_HEADER(slot, buff->seed);
	cmd[1] = buff->allowed;
	cmd[2] = buff->pt->pt.dma_handler;
	cmd[3] = buff->pt->pt.dma_handler >> 32;

	feed_cmd(dev, cmd, 4);
}


/* Should be called with slock held */
static void unbind_slot(struct dicedev_device *dev, struct dicedev_buffer *buff)
{
	uint32_t cmd;

	BUG_ON(buff->binded_slot == DICEDEV_BUFFER_NO_SLOT);

	cmd = DICEDEV_USER_CMD_UNBIND_SLOT_HEADER(buff->binded_slot);

	feed_cmd(dev, &cmd, 1);

	dev->buff_slots[buff->binded_slot] = NULL;
	buff->binded_slot = DICEDEV_BUFFER_NO_SLOT;
	buff->usage_count = 0;
	dev->free_slots++;
}

/* IRQ handler */

static irqreturn_t dicedev_isr(int irq, void *opaque)
{
	struct dicedev_device *dev = opaque;
	unsigned long flags;
	uint32_t istatus;

	spin_lock_irqsave(&dev->slock, flags);
	istatus = dicedev_ior(dev, DICEDEV_INTR) & dicedev_ior(dev, DICEDEV_INTR_ENABLE);

	if (istatus) {
		dicedev_iow(dev, DICEDEV_INTR, istatus);

		if (istatus & DICEDEV_INTR_FENCE_WAIT) {
			struct list_head *lh;
			struct dicedev_task *task;

			BUG_ON(list_empty(&dev->wt.running_tasks));

			lh = dev->wt.running_tasks.next;
			list_del(lh);
			task = container_of(lh, struct dicedev_task, lh);

			task->ctx->running_tasks_no--;
			wake_up_interruptible(&task->ctx->wq);

			unbind_slot(dev, task->dBuff);
			wake_up_interruptible(&dev->wt.slot_cond);

			kfree(task);
		}

		if (istatus & DICEDEV_INTR_FEED_ERROR) {
			// TODO: Propably nothing, and should simply disable this interrupt.
		}

		if ((istatus & DICEDEV_INTR_CMD_ERROR) | (istatus & DICEDEV_INTR_MEM_ERROR)) {
			struct list_head *lh;
			struct dicedev_task *task;

			BUG_ON(list_empty(&dev->wt.running_tasks));

			lh = dev->wt.running_tasks.next;
			task = container_of(lh, struct dicedev_task, lh);

			dicedev_ctx_fail(task->ctx);
		}


		BUG_ON(istatus & DICEDEV_INTR_SLOT_ERROR);
	}

	spin_unlock_irqrestore(&dev->slock, flags);
	return IRQ_RETVAL(istatus);
}

/* Main device node handling */

static int dicedev_open(struct inode *inode, struct file *filp)
{
	struct dicedev_device *dev = container_of(inode->i_cdev, struct dicedev_device, cdev);
	struct dicedev_context *ctx = kzalloc(sizeof(struct dicedev_context), GFP_KERNEL);

	if (!ctx) {
		printk(KERN_ERR "dicedev_open: kzalloc failed\n");
		return -ENOMEM;
	}

	ctx->dev = dev;

	spin_lock_init(&ctx->slock);
	init_waitqueue_head(&ctx->wq);

	INIT_LIST_HEAD(&ctx->other_contexts);
	INIT_LIST_HEAD(&ctx->allocated_buffers);

	filp->private_data = ctx;

	return nonseekable_open(inode, filp);
}


static int dicedev_release(struct inode *inode, struct file *filp)
{
	struct dicedev_context *ctx = filp->private_data;
	struct dicedev_device *dev = ctx->dev;
	struct list_head *lh;
	unsigned long flags;

	list_for_each(lh, &ctx->allocated_buffers) {
		struct dicedev_buffer *buff = container_of(lh, struct dicedev_buffer, context_buffers);

		spin_lock_irqsave(&dev->slock, flags);
		buff->destroyed = true;
		spin_unlock_irqrestore(&dev->slock, flags);
	}

	spin_lock_irqsave(&dev->slock, flags);

	while (ctx->running_tasks_no > 0) {
		spin_unlock_irqrestore(&dev->slock, flags);
		wait_event_interruptible(ctx->wq, ctx->running_tasks_no == 0);
		spin_lock_irqsave(&dev->slock, flags);
	}

	spin_unlock_irqrestore(&dev->slock, flags);

	list_for_each(lh, &ctx->allocated_buffers) {
		struct dicedev_buffer *buff = container_of(lh, struct dicedev_buffer, context_buffers);

		spin_lock_irqsave(&dev->slock, flags);

		while (buff->usage_count > 0) {
			spin_unlock_irqrestore(&dev->slock, flags);
			wait_event_interruptible(ctx->wq, buff->usage_count == 0);
			spin_lock_irqsave(&dev->slock, flags);
		}

		if (buff->binded_slot != DICEDEV_BUFFER_NO_SLOT) {
			unbind_slot(dev, buff);
		}

		dicedev_pt_free(dev->pdev, buff->pt);
		kfree(buff);

		spin_unlock_irqrestore(&dev->slock, flags);
	}

	kfree(ctx);

	return 0;
}


/* Buffer node handling */
static vm_fault_t dicedev_buffer_fault(struct vm_fault *vmf) {
	struct vm_area_struct *vma = vmf->vma;
	struct dicedev_buffer *buff = vma->vm_private_data;
	struct page *page;

	if (vmf->pgoff > buff->pt->num_pages) {
		return VM_FAULT_SIGBUS;
	}

	page = virt_to_page(buff->pt->pages[vmf->pgoff].page);

	get_page(page);

	vmf->page = page;

	return 0;
}

static struct vm_operations_struct dicedev_buffer_vms = {
	.fault = dicedev_buffer_fault,
};


static int dicedev_buffer_mmap(struct file *filp, struct vm_area_struct *vma) {
	struct dicedev_buffer *buff = filp->private_data;

	vma->vm_private_data = buff;
	vma->vm_ops = &dicedev_buffer_vms;

	return 0;
}

static ssize_t dicedev_buffer_read(struct file *filp, char __user *buff, size_t count, loff_t *offp) {
	/* FIXME: add read support */

	return 0;
//	ssize_t ret;
//	size_t page;
//	struct dicedev_buffer *buff = filp->private_data;
//
//	if (*offp >= buff->pt->max_size) {
//		return 0;
//	}
//
//	page = *offp / PAGE_SIZE;
//
//	count = min(count, PAGE_SIZE - (*offp % PAGE_SIZE));
//
//	if (copy_to_user(buff, ((char *)buff->pt->pages[page].page) + (*offp % PAGE_SIZE), count)) {
//		ret = -EFAULT;
//		goto err;
//	}
//
//	*offp += count;
//
//	return count;
//
//
//err:
//	return ret;
}


static ssize_t dicedev_buffer_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos) {
	/* FIXME: add write support */
	return 0;
//	ssize_t ret;
//	size_t page;
//	struct dicedev_buffer *buff = filp->private_data;
//
//	if (*f_pos + count > buff->pt->max_size) {
//		ret = -EINVAL;
//		goto err;
//	}
//
//	page =  *f_pos / PAGE_SIZE;
//
//	count = min(count, PAGE_SIZE - (*f_pos % PAGE_SIZE));
//
//	if (copy_from_user(((char *)buff->pt->pages[page].page) + (*f_pos % PAGE_SIZE), buf, count)) {
//		ret = -EFAULT;
//		goto err;
//	}
//
//	*f_pos += count;
//
//	return count;
//
//
//err:
//	return ret;
}


static long dicedev_buffer_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
	struct dicedev_buffer *buff = filp->private_data;
	struct dicedev_ioctl_seed *sCmd;

	if (!buff) {
		return -EINVAL;
	}

	switch (cmd) {
	case DICEDEV_BUFFER_IOCTL_SEED:
		sCmd = (struct dicedev_ioctl_seed *) arg;

		if (!sCmd) {
			return -EINVAL;
		}

		buff->seed_chg = 1;
		buff->seed = sCmd->seed;

		return 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations dicedev_buffer_fops = {
	.owner = THIS_MODULE,
	.mmap = dicedev_buffer_mmap,
	.read = dicedev_buffer_read,
	.write = dicedev_buffer_write,
	.unlocked_ioctl = dicedev_buffer_ioctl,
};


static struct file *dicedev_create_buffer(struct dicedev_context *ctx, int size, uint64_t allowed)
{
	struct dicedev_buffer *buff;

	if (size < 0 || size > DICEDEV_MAX_BUFF_SIZE) {
		printk(KERN_ERR "dicedev_create_buffer: size too big\n");
		return ERR_PTR(-EINVAL);
	}

	if (allowed > DICEDEV_MAX_ALLOWED) {
		printk(KERN_ERR "dicedev_create_buffer: invalid allowed flag\n");
		return ERR_PTR(-EINVAL);
	}

	buff = kzalloc(sizeof(struct dicedev_buffer), GFP_KERNEL);

	if (!buff) {
		goto err;
	}

	if (dicedev_pt_create(ctx->dev->pdev, buff->pt, size) < 0) {
		goto err_free_buff;
	}

	buff->ctx = ctx;
	buff->binded_slot = DICEDEV_BUFFER_NO_SLOT;
	buff->seed = DICEDEV_DEFAULT_SEED;
	buff->allowed = allowed;
	INIT_LIST_HEAD(&buff->context_buffers);

	list_add(&buff->context_buffers, &ctx->allocated_buffers);

	// TODO: check the flags.
	return anon_inode_getfile("dicedev_buffer", &dicedev_buffer_fops, buff, O_RDWR | O_CLOEXEC);

err_free_buff:
	kfree(buff);
err:
	return ERR_PTR(-ENOMEM);
}


static void dicedev_task_init_iterator(struct dicedev_task *task)
{
	task->cBuff_state.curr_pg_no = task->offset / PAGE_SIZE;
	task->cBuff_state.curr_pg_off = (task->offset % PAGE_SIZE) / 4;
	task->cBuff_state.curr_pg = task->cBuff->pt->pages[task->cBuff_state.curr_pg_no].page;
	task->cBuff_state.bytes_left = task->size;
}


static inline uint32_t dicedev_task_get_next_word(struct dicedev_task *task)
{
	BUG_ON(task->cBuff_state.bytes_left == 0);

	if (task->cBuff_state.curr_pg_off == PAGE_SIZE / 4) {
		task->cBuff_state.curr_pg_off = 0;
		task->cBuff_state.curr_pg = task->cBuff->pt->pages[++task->cBuff_state.curr_pg_no].page;
	}

	task->cBuff_state.bytes_left -= 4;

	return task->cBuff_state.curr_pg[task->cBuff_state.curr_pg_off++];
}


/* Needs to be called under device lock */
static int get_slot(struct dicedev_device *dev)
{
	int slot = -1;

	if (dev->free_slots > 0) {
		for (int i = 0; i < DICEDEV_NUM_SLOTS; i++) {
			if (dev->buff_slots[i] == NULL) {
				slot = i;
				break;
			}
		}
	}

	return slot;
}


static void __add_task(struct dicedev_device *dev, struct dicedev_task *task)
{
	spin_lock(&dev->slock);
	list_add_tail(&task->lh, &dev->wt.pending_tasks);
	wake_up_interruptible(&dev->wt.task_cond);
	spin_unlock(&dev->slock);
}


static int dicedev_ioctl_run(struct dicedev_context *ctx, struct dicedev_ioctl_run rCmd) {
	struct dicedev_buffer *cBuff, *dBuff;
	struct dicedev_device *dev;
	struct dicedev_task *task;

	if (rCmd.addr % 4 !=  0 || rCmd.size % 4 != 0) {
		printk(KERN_ERR "dicedev_ioctl_run: invalid addr or size\n");
		return -EINVAL;
	}

	cBuff = fdget(rCmd.cfd).file->private_data;
	dBuff = fdget(rCmd.bfd).file->private_data;
	dev = ctx->dev;

	if (cBuff == NULL || dBuff == NULL) {
		printk(KERN_ERR "dicedev_ioctl_run: invalid file descriptor\n");
		return -EINVAL;
	}

	if ((uint64_t)rCmd.addr + (uint64_t) rCmd.size > cBuff->pt->max_size) {
		printk(KERN_ERR "dicedev_ioctl_run: buffer is not large enough to perform this operation.\n");
		return -EINVAL;
	}

	if (cBuff->ctx->dev != ctx->dev || dBuff->ctx->dev != ctx->dev) {
		printk(KERN_ERR "dicedev_ioctl_run: invalid device buffer\n");
		return -EINVAL;
	}

	//TODO: maybe we dont need this.
	spin_lock(&dev->slock);
	if (ctx->failed) {
		spin_unlock(&dev->slock);
		return -EIO;
	}
	spin_unlock(&dev->slock);

	task = kzalloc(sizeof(struct dicedev_task), GFP_KERNEL);

	if (!task) {
		return -ENOMEM;
	}

	__add_task(dev, task);

	return 0;
}


static int dicedev_ioctl_wait(struct dicedev_context *ctx, struct dicedev_ioctl_wait *wCmd) {
	unsigned long flags;

	spin_lock_irqsave(&ctx->dev->slock, flags);
	while (wCmd->cnt >= ctx->running_tasks_no) {
		spin_unlock_irqrestore(&ctx->dev->slock, flags);
		wait_event_interruptible(ctx->wq, wCmd->cnt >= ctx->running_tasks_no);
		spin_lock_irqsave(&ctx->wq.lock, flags);
	}

	if (ctx->failed) {
		spin_unlock_irqrestore(&ctx->dev->slock, flags);
		return -EIO;
	}

	spin_unlock_irqrestore(&ctx->dev->slock, flags);

	return 0;
}


static int dicedev_ioctl_seed_inc(struct dicedev_context *ctx, uint32_t inc) {
	if (inc != 0 && inc != 1) {
		printk(KERN_ERR "dicedev_ioctl_seed_inc: invalid increment value\n");
		return -EINVAL;
	}

	spin_lock(&ctx->dev->slock);

	dicedev_iow(ctx->dev, DICEDEV_INCREMENT_SEED, inc);

	spin_unlock(&ctx->dev->slock);

	return 0;
}


static long dicedev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct dicedev_context *ctx = filp->private_data;
	struct dicedev_ioctl_create_set *cs;
	struct dicedev_ioctl_run *rCmd;
	struct dicedev_ioctl_wait *wCmd;
	struct dicedev_ioctl_seed_inc *siCmd;
	struct file *f;

	if (!ctx) {
		printk(KERN_ERR "dicedev_ioctl: invalid context\n");
		return -EINVAL;
	}

	switch (cmd) {
	case DICEDEV_IOCTL_CREATE_SET: ;
		cs = (struct dicedev_ioctl_create_set *)arg;

		if (!cs) {
			printk(KERN_ERR "dicedev_ioctl: invalid argument\n");
			return -EINVAL;
		}

		f = dicedev_create_buffer(ctx, cs->size, cs->allowed);

		if (IS_ERR(f)) {
			return PTR_ERR(f);
		}

		return (long) f;

	case DICEDEV_IOCTL_RUN:
		rCmd = (struct dicedev_ioctl_run *) arg;

		if (!rCmd) {
			printk(KERN_ERR "dicedev_ioctl: invalid argument\n");
			return -EINVAL;
		}

		return dicedev_ioctl_run(ctx, *rCmd);

	case DICEDEV_IOCTL_WAIT:
		wCmd = (struct dicedev_ioctl_wait *) arg;

		if (!wCmd) {
			printk(KERN_ERR "dicedev_ioctl: invalid argument\n");
			return -EINVAL;
		}

		return dicedev_ioctl_wait(ctx, wCmd);

	case DICEDEV_IOCTL_ENABLE_SEED_INCREMENT:
		siCmd = (struct dicedev_ioctl_seed_increment *) arg;

		if (!siCmd) {
			printk(KERN_ERR "dicedev_ioctl: invalid argument\n");
			return -EINVAL;
		}

		return dicedev_ioctl_seed_inc(ctx, siCmd->enable_increment);
	default:
		printk(KERN_ERR "dicedev_ioctl: invalid command\n");
		return -EINVAL;
	}
}

static const struct file_operations dicedev_file_ops = {
	.owner = THIS_MODULE,
	.open = dicedev_open,
	.release = dicedev_release,
	.unlocked_ioctl = dicedev_ioctl,
	/* TODO: Czy to wszystko tutaj? */
};


/* Worker thread */

static void __wt_run_task(struct dicedev_task *task) {
	struct dicedev_context *ctx = task->ctx;
	struct dicedev_device *dev = ctx->dev;
	struct dicedev_buffer *data_buffer = task->dBuff;
	uint32_t cmd[2];

	spin_lock(&task->ctx->dev->slock);
	list_add_tail(&task->lh, &task->ctx->dev->wt.running_tasks);
	spin_unlock(&task->ctx->dev->slock);

	BUG_ON(data_buffer->binded_slot == DICEDEV_BUFFER_NO_SLOT);

	dicedev_task_init_iterator(task);

	while (task->cBuff_state.bytes_left > 0) {
		cmd[0] = dicedev_task_get_next_word(task);

		switch (cmd[0] & DICEDEV_CMD_TYPE_MASK) {
			case DICEDEV_USER_CMD_TYPE_NOP:
				break;
			case DICEDEV_USER_CMD_TYPE_GET_DIE:
				if (task->cBuff_state.bytes_left < 4) {
					goto err_ctx_fail;
				}

				cmd[1] = dicedev_task_get_next_word(task);

				if ((cmd[0] & GET_DIE_SLOT_MASK) != 0) {
					goto err_ctx_fail;
				}

				cmd[0] |= data_buffer->binded_slot << 24;

				feed_cmd(dev, cmd, 2);

				break;
			case DICEDEV_USER_CMD_TYPE_NEW_SET:
				if ((cmd[0] & NEW_SET_SLOT_MASK) != 0) {
					goto err_ctx_fail;
				}

				cmd[0] |= data_buffer->binded_slot << 4;

				feed_cmd(dev, cmd, 1);

				break;
			default:
				goto err_ctx_fail;
		}
	}

	cmd[0] =  DICEDEV_USER_CMD_FENCE_HEADER(DICEDEV_FENCE_DONE_NUM);

	feed_cmd(dev, cmd, 1);

	return;

err_ctx_fail:
	cmd[0] =  DICEDEV_USER_CMD_FENCE_HEADER(DICEDEV_FENCE_DONE_NUM);

	feed_cmd(dev, cmd, 1);

	dicedev_ctx_fail(ctx);
	return;
}


static int dicedev_work_thread_fn(void *data) {
	struct dicedev_device *dev = data;
	struct dicedev_task *task;
	struct list_head *task_lh;

	while (dev->wt.running) {
		spin_lock(&dev->slock);

		if (list_empty(&dev->wt.pending_tasks)) {
			spin_unlock(&dev->slock);
			wait_event_interruptible(dev->wt.task_cond, !list_empty(&dev->wt.pending_tasks));
			spin_lock(&dev->slock);
		}

		task_lh = dev->wt.pending_tasks.next;
		list_del(task_lh);
		task = container_of(task_lh, struct dicedev_task, lh);


		while (dev->free_slots == 0 && task->dBuff->binded_slot == DICEDEV_BUFFER_NO_SLOT) {
			spin_unlock(&dev->slock);
			wait_event_interruptible(dev->wt.slot_cond, dev->free_slots > 0);
			spin_lock(&dev->slock);
		}

		if (task->dBuff->binded_slot == DICEDEV_BUFFER_NO_SLOT) {
			dev->free_slots--;
			bind_slot(dev, task->dBuff, get_slot(dev));
		}

		spin_unlock(&dev->slock);

		__wt_run_task(task);
	}

	return 0;
}


/* PCI driver. */

static struct pci_device_id dicedev_pci_ids[] = {
	{ PCI_DEVICE(DICEDEV_VENDOR_ID, DICEDEV_DEVICE_ID) },
	{ 0 }
};

static void dicedev_init_wt(struct dicedev_device *dev) {
	dev->wt.running = 1;
	INIT_LIST_HEAD(&dev->wt.running_tasks);
	INIT_LIST_HEAD(&dev->wt.pending_tasks);
	init_waitqueue_head(&dev->wt.task_cond);
	init_waitqueue_head(&dev->wt.slot_cond);
	dev->wt.thread = kthread_run(dicedev_work_thread_fn, dev, "dicedev_work_thread");
}


static int dicedev_probe(struct pci_dev *pdev, const struct pci_device_id *pci_id)
{
	int err, i;
	const uint32_t enabled_intr = DICEDEV_INTR_FENCE_WAIT
				      | DICEDEV_INTR_FEED_ERROR
				      | DICEDEV_INTR_CMD_ERROR
				      | DICEDEV_INTR_MEM_ERROR
				      | DICEDEV_INTR_SLOT_ERROR;
	/* Allocate device structure */
	struct dicedev_device *dev = kzalloc(sizeof(struct dicedev_device), GFP_KERNEL);
	if (!dev) {
		err = -ENOMEM;
		goto err_alloc_dev;
	}
	pci_set_drvdata(pdev, dev);
	dev->pdev = pdev;

	spin_lock_init(&dev->slock);
	mutex_lock(&dicedev_devices_lock);
	for (i = 0; i < DICEDEV_MAX_DEVICES; i++) {
		if (!dicedev_devices[i]) {
			break;
		}
	}
	if (i == DICEDEV_MAX_DEVICES) {
		err = -ENOSPC;
		mutex_unlock(&dicedev_devices_lock);
		goto err_max_dev;
	}
	dicedev_devices[i] = dev;
	dev->idx = i;
	mutex_unlock(&dicedev_devices_lock);

	if ((err = pci_enable_device(pdev))) {
		goto err_enable_dev;
	}

	if ((err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(DICEDEV_ADDR_BITS)))) {
		goto err_dma_mask;
	}

	pci_set_master(dev->pdev);

	if ((err = pci_request_regions(pdev, "dicedev"))) {
		goto err_request_regions;
	}

	/* Map BAR */
	if (!(dev->bar = pci_iomap(pdev, 0, 0))) {
		err = -ENOMEM;
		goto err_iomap;
	}

	/* Connect IRQ line */
	if ((err = request_irq(pdev->irq, dicedev_isr, IRQF_SHARED, "dicedev", dev))) {
		goto err_request_irq;
	}

	dicedev_init_wt(dev);

	dicedev_iow(dev, DICEDEV_INTR, enabled_intr);
	dicedev_iow(dev, DICEDEV_INTR_ENABLE, enabled_intr);



	cdev_init(&dev->cdev, &dicedev_file_ops);

	if ((err = cdev_add(&dev->cdev, dicedev_devno + dev->idx, 1))) {
		goto err_cdev_add;
	}

	dev->dev = device_create(&dicedev_class,
				 &dev->pdev->dev, dicedev_devno + dev->idx, dev,
				 "dicedev%d", dev->idx);

	if (IS_ERR(dev->dev)) {
		printk(KERN_ERR "dicedev: device_create failed\n");
		dev->dev = 0;
	}

	return 0;


err_cdev_add:
	dicedev_iow(dev, DICEDEV_INTR_ENABLE, 0);
	free_irq(pdev->irq, dev);
err_request_irq:
	pci_iounmap(pdev, dev->bar);
err_iomap:
	pci_release_regions(pdev);
err_request_regions:
err_dma_mask:
	pci_disable_device(pdev);
err_enable_dev:
	mutex_lock(&dicedev_devices_lock);
	dicedev_devices[dev->idx] = 0;
	mutex_unlock(&dicedev_devices_lock);
err_max_dev:
	kfree(dev);
err_alloc_dev:
	return err;
}


static void dicedev_remove(struct pci_dev *pdev) {
	struct dicedev_device *dev = pci_get_drvdata(pdev);
	if (dev->dev) {
		device_destroy(&dicedev_class, dicedev_devno + dev->idx);
	}
	cdev_del(&dev->cdev);
	dicedev_iow(dev, DICEDEV_INTR_ENABLE, 0);
	free_irq(pdev->irq, dev);
	pci_iounmap(pdev, dev->bar);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	mutex_lock(&dicedev_devices_lock);
	dicedev_devices[dev->idx] = 0;
	mutex_unlock(&dicedev_devices_lock);
	kfree(dev);
}


static int dicedev_suspend(struct pci_dev *pdev, pm_message_t state) {
	struct dicedev_device *dev = pci_get_drvdata(pdev);
	dicedev_iow(dev, DICEDEV_INTR_ENABLE, 0);
	dicedev_iow(dev, DICEDEV_INTR, 0);
	free_irq(pdev->irq, dev);
	pci_disable_device(pdev);
	return 0;
}

static int dicedev_resume(struct pci_dev *pdev) {
	struct dicedev_device *dev = pci_get_drvdata(pdev);

	dicedev_iow(dev, DICEDEV_INTR, DICEDEV_INTR_FENCE_WAIT
				| DICEDEV_INTR_FEED_ERROR
				| DICEDEV_INTR_CMD_ERROR
				| DICEDEV_INTR_MEM_ERROR
				| DICEDEV_INTR_SLOT_ERROR);
	dicedev_iow(dev, DICEDEV_INTR_ENABLE, DICEDEV_INTR_FENCE_WAIT
				| DICEDEV_INTR_FEED_ERROR
				| DICEDEV_INTR_CMD_ERROR
				| DICEDEV_INTR_MEM_ERROR
				| DICEDEV_INTR_SLOT_ERROR);

	return 0;
}

static struct pci_driver dicedev_pci_driver = {
	.name = "dicedev",
	.id_table = dicedev_pci_ids,
	.probe = dicedev_probe,
	.remove = dicedev_remove,
	.suspend = dicedev_suspend,
	.resume = dicedev_resume,
};

/* Init and exit */

static int dicedev_init(void) {
	int err;
	if ((err = alloc_chrdev_region(&dicedev_devno, 0, DICEDEV_MAX_DEVICES, "dicedev")))
		goto err_chrdev;
	if ((err = class_register(&dicedev_class)))
		goto err_class;
	if ((err = pci_register_driver(&dicedev_pci_driver)))
		goto err_pci;
	return 0;

err_pci:
	class_unregister(&dicedev_class);
err_class:
	unregister_chrdev_region(dicedev_devno, DICEDEV_MAX_DEVICES);
err_chrdev:
	return err;
}


static void dicedev_exit(void) {
	pci_unregister_driver(&dicedev_pci_driver);
	class_unregister(&dicedev_class);
	unregister_chrdev_region(dicedev_devno, DICEDEV_MAX_DEVICES);
}


module_init(dicedev_init);

module_exit(dicedev_exit);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bartosz Ruszewski");
MODULE_DESCRIPTION("DiceDev driver");
