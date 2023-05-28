#include <linux/module.h>
#include <linux/pci.h>
#include <linux/list.h>
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/types.h>
#include <linux/cdev.h>


#include "dicedev.h"
#include "dicedev_pt.h"

#define DICEDEV_DEFAULT_SEED 42
#define DICEDEV_MAX_DEVICES 256
#define DICEDEV_NUM_SLOTS 16
#define DICEDEV_MAX_BUFF_SIZE (1 << 20)

#define DICEDEV_BUFFER_NO_SLOT -1

#define DICEDEV_MAX_ALLOWED (((uint64_t) 1 << 33) - 1

#define DICEDEV_CMD(first_byte, second_byte, third_byte, fourth_byte) \
	((uint32_t) (first_byte) << 24 | (uint32_t) (second_byte) << 16 | (uint32_t) (third_byte) << 8 | (uint32_t) (fourth_byte))

#define DICEDEV_CMD_TYPE_MASK 0xF

#defube GET_DIE_SLOT_MASK (((1 << 28) - 1) ^ ((1 << 25) - 1))
#define NEW_SET_SLOT_MASK (((1 << 8) - 1) ^ ((1 << 5) - 1))

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bartosz Ruszewski");
MODULE_DESCRIPTION("DiceDev driver");

static dev_t dicedev_devno;
static struct adlerdev_device *dicedev_devices[DICEDEV_MAX_DEVICES];
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

	struct list_head tasks;
};

struct dicedev_device {
	struct pci_dev *pdev;
	struct cdev cdev;
	int idx;
	struct device *dev;
	void _iomem *bar;
	spinlock_t slock;
	struct list_head pending_tasks;
	struct list_head running_tasks;
	struct dicedev_task *curr_proc_task;

	wait_queue_head_t slot_wq;

	struct dicedev_buffer *buff_slots[DICEDEV_NUM_SLOTS];
	size_t free_slots;
	bool increment_seeds;
};


struct devicedev_context {
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
	size_t seed;
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

/* IRQ handler */

static ireqreturn_t dicedev_isr(int irq, void *opaque)
{

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
	ctx->other_contexts = LIST_HEAD_INIT(&ctx->other_contexts);

	/* TODO: init them context */

	spin_lock(&dev->slock);

	spin_unlock(&dev->slock);

	filp->private_data = ctx;
	return nonseekable_open(inode, filp);
}


static int dicedev_release(struct inode *inode, struct file *filp) {
	struct dicedev_context *ctx = filp->private_data;

	kfree(ctx);

	return 0;
}


static struct file *dicedev_create_buffer(struct dicedev_context *ctx, int size, uint64_t allowed) {
	if (size < 0 || size > DICEDEV_MAX_BUFF_SIZE) {
		printk(KERN_ERR "dicedev_create_buffer: size too big\n");
		return ERR_PTR(-EINVAL);
	}

	if (allowed > DICEDEV_MAX_ALLOWED) {
		printk(KERN_ERR "dicedev_create_buffer: invalid allowed flag\n");
		return ERR_PTR(-EINVAL);
	}

	struct dicedev_buffer *buff = kzalloc(sizeof(struct dicedev_buffer), GFP_KERNEL);

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
	buff->context_buffers = LIST_HEAD_INIT(&buff->context_buffers);

	list_add(&buff->context_buffers, &ctx->allocated_buffers);

	// TODO: check the flags.
	return anon_inode_getfile("dicedev_buffer", &dicedev_buffer_fops, buff, O_RDWR | O_CLOEXEC);

err_free_buff:
	kfree(buff);
err:
	return ERR_PTR(-ENOMEM);
}

static int is_device_buffer(struct dicedev_device *dev, struct dicedev_buffer *buff) {
	struct list_head *pos;
	//TODO: need locking????? Does it skip head first element/????????
	list_for_each(pos, dev->allocated_buffers) {
		struct dicedev_buffer *b = container_of(pos, struct dicedev_buffer, context_buffers);

		if (b == buff) {
			return 1;
		}
	}

	return 0;
}

static void bind_slot(struct dicedev_device *dev, struct dicedev_buffer *buff, int slot) {
	BUG_ON(dev->buff_slots[slot] != NULL);
	dev->buff_slots[slot] = buff;

	dicedev_iow(dev, DICEDEV_USER_CMD_BIND_SLOT, DICEDEV_CMD(DICEDEV_USER_CMD_BIND_SLOT_HEADER(slot, buff->seed),
								 buff->allowed,
								 buff->pt->dma_handler,
								 buff->pt->dma_handler >> 32));
}

static void inline feed_cmd(struct dicedev_device *dev, uint32_t *cmd, size_t cmd_size) {
	for (size_t i = 0; i < cmd_size; i++) {
		while (dicedev_ior(dev, CMD_MANUAL_FREE) == 0) {
			// Wait for free slot.
		}

		dicedev_iow(dev, CMD_MANUAL_FREE, cmd[i]);
	}
}


static void dicedev_ctx_fail(struct dicedev_context *ctx) {
	spin_lock(&ctx->slock);
	ctx->failed = 1;
	spin_unlock(&ctx->slock);
}


static void dicedev_task_init_iterator(struct dicedev_task *task) {
	task->cBuff_state.curr_pg_no = task->offset / PAGE_SIZE;
	task->cBuff_state.curr_pg_off = task->offset % PAGE_SIZE;
	task->cBuff_state.curr_pg = task->cBuff->pt[task->cBuff_state.curr_pg_no];
	task->cBuff_state.bytes_left = task->size;
}


static uint32_t dicedev_task_get_next_word(struct dicedev_task *task) {
	BUG_ON(task->cBuff_state.bytes_left == 0);

	if (task->cBuff_state.curr_pg_off == PAGE_SIZE) {
		task->cBuff_state.curr_pg_off = 0;
		task->cBuff_state.curr_pg = task->cBuff[++task->cBuff_state.curr_pg_no];
	}

	task->cBuff_state.bytes_left -= 4;

	return task->cBuff_state.curr_pg[task->cBuff_state.curr_pg_off++];
}


static void run_task(struct dicedev_context *ctx) {
	struct dicedev_device *dev = ctx->dev;
	struct dicedev_task *task = dev->curr_proc_task;
	struct dicedev_buffer *data_buffer = task->dBuff;uint32_t cmd[2];
	uint32_t cmd[2];

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
				if (cmd[0] & NEW_SET_SLOT_MASK != 0) {
					goto err_ctx_fail;
				}

				cmd[0] |= data->buffer->binded_slot << 4;

				feed_cmd(dev, cmd, 1);

				break;
			default:
				goto err_ctx_fail;
		}
	}

	return;


err_ctx_fail:
	dicedev_ctx_fail(ctx);
	return;
}


/* Needs to be called under device lock */
static int get_slot(struct dicedev_device *dev) {
	int slot = -1;

	if (dev->free_slots > 0) {
		for (int i = 0; i < DICEDEV_NUM_SLOTS; i++) {
			if (dev->buff_slots[i] == NULL) {
				slot = i;
				dev->free_slots--;
				break;
			}
		}
	}

	return slot;
}


static int dicedev_ioctl_run(struct dicedev_context *ctx, struct dicedev_ioctl_run rCmd) {
	BUG_ON(ctx.failed);

	if (rCmd.addr % 4 !=  0 || rCmd.size % 4 != 0) {
		printk(KERN_ERR "dicedev_ioctl_run: invalid addr or size\n");
		return -EINVAL;
	}

	if ((uint64_t)rCmd.addr + (uint64_t) rCmd.size > buff->pt->size) {
		printk(KERN_ERR "dicedev_ioctl_run: buffer is not large enough to perform this operation.\n");
		return -EINVAL;
	}

	struct dicedev_buffer *buff = fdget(rCmd.cfd).file->private_data;
	struct dicedev_buffer *dBuff = fdget(rCmd.bfd).file->private_data;

	if (buff->ctx->dev != ctx->dev || dBuff->ctx->dev != ctx->dev) {
		printk(KERN_ERR "dicedev_ioctl_run: invalid device buffer\n");
		return -EINVAL;
	}

	struct dicedev_device *dev = ctx->dev;
	struct dicedev_task *task = kzalloc(sizeof(struct dicedev_task), GFP_KERNEL);

	if (!task) {
		goto err_task;
	}

	spin_lock(&dev->slock);

	if (dev->curr_proc_task != NULL) {
		spin_unlock(&dev->slock);
		wait_event_interruptible(dev->)
	}

	if (dev->free_slots == 0 && dBuff->) {
		/* Bind a slot */
		spin_unlock(&dev->slock);
		// TODO: atomic lock lock, and queue enter?
		wait_event_interruptible(dev->slot_wq, dev->free_slots > 0);
		spin_lock(&dev->slock);
		buff->binded_slot = get_slot(dev);
	}



	return 0;

err_task:
	return -ENOMEM;
}

static long dicedev_ioctl(struct file *filp, unsinged int cmd, unsigned long arg)
{
	struct dicedev_context *ctx = filp->private_data;
	struct dicedev_device *dev = ctx->dev;

	switch (cmd) {
	case DICEDEV_IOCTL_CREATE_SET:
		struct dicedev_ioctl_create_set *cs = (struct dicedev_ioctl_create_set *)arg;
		struct file *f;

		if (!cs) {
			printk(KERN_ERR "dicedev_ioctl: invalid argument\n");
			return -EINVAL;
		}

		f = dicedev_create_buffer(ctx, cs->size, cs->allowed);

		if (IS_ERR(f)) {
			return PTR_ERR(f);
		}

		return f;

	case DICEDEV_IOCTL_RUN:
		/* TODO: implement */
		break;
	case DICEDEV_IOCTL_WAIT:
		/* TODO: implement */
		break;
	case DICEDEV_IOCTL_ENABLE_SEED_INCREMENT:
		/* TODO: implement */
		break;

	}
}

static const struct file_operations dicedev_file_ops = {
	.owner = THIS_MODULE,
	.open = dicedev_open,
	.release = dicedev_release,
	.unlocked_ioctl = dicedev_ioctl,
	/* TODO: Czy to wszystko tutaj? */
};

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

static int dicedev_buffer_read(struct file *filp, char __user *buff, size_t count, loff_t *offp) {
	ssize_t ret;
	size_t page;
	struct dicedev_buffer *buff = filp->private_data;

	if (*offp >= buff->pt->max_size) {
		return 0;
	}

	page = *offp / PAGE_SIZE;

	count = min(count, PAGE_SIZE - (*offp % PAGE_SIZE));

	if (copy_to_user(buff, ((char *)buff->pt->pages[page].page) + (*offp % PAGE_SIZE), count)) {
		ret = -EFAULT;
		goto err;
	}

	*offp += count;

	return count;


err:
	return ret;
}


static ssize_t dicedev_buffer_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos) {
	ssize_t ret;
	size_t page;
	struct dicedev_buffer *buff = filp->private_data;

	if (*f_pos + count > buff->pt->max_size) {
		ret = -EINVAL;
		goto err;
	}

	page =  *f_pos / PAGE_SIZE;

	count = min(count, PAGE_SIZE - (*f_pos % PAGE_SIZE));

	if (copy_from_user(((char *)buff->pt->pages[page].page) + (*f_pos % PAGE_SIZE), buf, count)) {
		ret = -EFAULT;
		goto err;
	}

	*f_pos += count;

	return count;


err:
	return ret;
}


static long dicedev_buffer_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
	struct dicedev_buffer *buff = filp->private_data;

	switch (cmd) {
	case DICEDEV_BUFFER_IOCTL_SEED:
		/* TODO: implement */
		break;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations dicedev_file_ops = {
	.owner = THIS_MODULE,
	.mmap = dicedev_buffer_mmap,
	.read = dicedev_buffer_read,
	.write = dicedev_buffer_write,
	.unlocked_ioctl = dicedev_buffer_ioctl,
};


/* PCI driver. */

static struct pci_device_id dicedev_pci_ids[] = {
	{ PCI_DEVICE(DICEDEV_VENDOR_ID, DICEDEV_DEVICE_ID) },
	{ 0 }
};

static int dicedev_probe(struct pci_dev *pdev, const struct pci_device_id *pci_id);

static void dicedev_remove(struct pc_dev *pdev);

static int dicedev_suspend(struct pci_dev *pdev, pm_message_t state);

static int dicedev_resume(struct pci_dev *pdev);

static struct pci_driver dicedev_pci_driver = {
	.name = "dicedev",
	.id_table = dicedev_pci_ids,
	.probe = dicedev_probe,
	.remove = dicedev_remove,
	.suspend = dicedev_suspend,
	.resume = dicedev_resume,
};

/* Init and exit */

static int dicedev_init(void);

static void dicedev_exit(void);


module_init(dicedev_init);

module_exit(dicedev_exit);