#include "dicedev_wt.h"
#include "dicedev_utils.h"

#include "dicedev_buffer.h"

int dicedev_buffer_init(struct dicedev_context *ctx,
			struct dicedev_buffer *buff,
			size_t size,
			uint64_t allowed) {
	int err;
	BUG_ON(!ctx);
	BUG_ON(!buff);

	buff->pt = kzalloc(sizeof(struct dicedev_page_table), GFP_KERNEL);

	if (!buff->pt) {
		printk(KERN_ERR "dicedev_buffer_init: failed to allocate page table\n");
		err = -ENOMEM;
		goto error;
	}

	if (dicedev_pt_init(ctx->dev->pdev, buff->pt, size) <  0) {
		printk(KERN_ERR "dicedev_buffer_init: failed to initialize page table\n");
		err = -ENOMEM;
		goto err_pt;
	}

	buff->ctx = ctx;
	INIT_LIST_HEAD(&buff->context_buffers);
	buff->destroyed = 0;
	buff->seed = DICEDEV_DEFAULT_SEED;
	buff->seed_chg = 0;
	buff->allowed = allowed;
	buff->binded_slot = DICEDEV_BUFFER_NO_SLOT;

	buff->reader.result_count = 0;
	buff->reader.offset = 0;

	return 0;

err_pt:
	kfree(buff->pt);
error:
	return err;
}


void dicedev_buffer_init_reader(struct dicedev_buffer *buff) {
	BUG_ON(!buff);

	buff->reader.result_count = 0;
	buff->reader.offset = 0;
}


static vm_fault_t dicedev_buffer_vm_fault(struct vm_fault *vmf) {
	struct vm_area_struct *vma = vmf->vma;
	struct dicedev_buffer *buff = vma->vm_private_data;
	struct page *page;

	if (vmf->pgoff >= buff->pt->num_pages) {
		return VM_FAULT_SIGBUS;
	}

	page = virt_to_page(buff->pt->pages[vmf->pgoff].page);

	get_page(page);

	vmf->page = page;

	return 0;
}


static struct vm_operations_struct dicedev_buffer_vm_ops = {
	.fault = dicedev_buffer_vm_fault,
};


static int dicedev_buffer_mmap(struct file *filp, struct vm_area_struct *vma) {
	struct dicedev_buffer *buff = filp->private_data;

	if (buff == NULL) {
		printk(KERN_ERR "dicedev_buffer_mmap: buffer is NULL\n");
		return -EINVAL;
	}

	vma->vm_private_data = buff;
	vma->vm_ops = &dicedev_buffer_vm_ops;

	return 0;
}


static ssize_t dicedev_buffer_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	struct dicedev_buffer *buff = filp->private_data;
	struct dicedev_context *ctx = buff->ctx;
	struct dicedev_task *task;
	uint32_t *cmd;
	unsigned long flags;

	if (count % 4 != 0) {
		printk(KERN_ERR "dicedev_buffer_write: count is not a multiple of 4\n");
		return -EINVAL;
	}

	cmd = kmalloc(count, GFP_KERNEL);

	if (!cmd) {
		printk(KERN_ERR "dicedev_buffer_write: failed to allocate memory\n");
		return -ENOMEM;
	}

	if (copy_from_user(cmd, buf, count)) {
		printk(KERN_ERR "dicedev_buffer_write: failed to copy from user\n");
		return -EFAULT;
	}

	task = kmalloc(sizeof(struct dicedev_task), GFP_KERNEL);

	if (!task) {
		kfree(cmd);
		printk(KERN_ERR "dicedev_buffer_write: failed to allocate memory\n");
		return -ENOMEM;
	}

	spin_lock_irqsave(&ctx->slock, flags);
	ctx->task_count++;
	spin_unlock_irqrestore(&ctx->slock, flags);

	task->type = DICEDEV_TASK_TYPE_WRITE;
	task->ctx = ctx;
	INIT_LIST_HEAD(&task->lh);
	task->result_count = 0;
	task->buff_out = buff;

	task->write.cmd = cmd;
	task->write.cmd_size = count;

	dicedev_wt_add_task(ctx, task);

	return count;
}


static int __buff_read_result(struct dicedev_buffer *buffer, struct dice *res) {
	struct dice *result_page;
	size_t page_no;
	BUG_ON(buffer->reader.offset >= buffer->reader.result_count);

	page_no = buffer->reader.offset / (DICEDEV_PAGE_SIZE / sizeof(struct dice));

	if (page_no >= buffer->pt->num_pages) {
		return -EINVAL;
	}

	result_page = (struct dice *)buffer->pt->pages[page_no].page;

	*res = result_page[buffer->reader.offset % (DICEDEV_PAGE_SIZE / sizeof(struct dice))];

	buffer->reader.offset++;

	return 0;
}


static ssize_t dicedev_buffer_read(struct file *filp, char __user *buff, size_t count, loff_t *offp) {
	struct dicedev_buffer *res_buff = filp->private_data;
	struct dicedev_context *ctx = res_buff->ctx;
	struct dice result;
	unsigned long flags;

	if (count != 8) {
		printk(KERN_ERR "dicedev_buffer_read: count is not 8\n");
		return -EINVAL;
	}

	spin_lock_irqsave(&ctx->slock, flags);
	while (res_buff->reader.result_count == 0 && ctx->task_count > 0) {
		spin_unlock_irqrestore(&ctx->slock, flags);
		wait_event_interruptible(ctx->wq, res_buff->reader.result_count > 0 || ctx->task_count == 0);
		spin_lock_irqsave(&ctx->slock, flags);
	}

	if (res_buff->reader.offset >= res_buff->reader.result_count) {
		return 0;
	}

	if (__buff_read_result(res_buff, &result)) {
		spin_unlock_irqrestore(&ctx->slock, flags);
		printk(KERN_ERR "dicedev_buffer_read: failed to read result\n");
		return -EFAULT;
	}

	if (copy_to_user(buff, &result, sizeof(struct dice))) {
		spin_unlock_irqrestore(&ctx->slock, flags);
		printk(KERN_ERR "dicedev_buffer_read: failed to copy to user\n");
		return -EFAULT;
	}

	if (res_buff->reader.offset >= res_buff->reader.result_count && ctx->task_count == 0 && res_buff->binded_slot != DICEDEV_BUFFER_NO_SLOT) {
		unbind_slot(ctx->dev, res_buff);
	}

	spin_unlock_irqrestore(&ctx->slock, flags);
	return 8;
}



static long dicedev_buffer_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
	struct dicedev_buffer *buff = filp->private_data;
	struct dicedev_ioctl_seed sCmd;
	char __user *user_buff;

	if (!buff) {
		return -EINVAL;
	}

	switch (cmd) {
	case DICEDEV_BUFFER_IOCTL_SEED:
		user_buff = (char __user *) arg;

		if (copy_from_user(&sCmd, user_buff, sizeof(struct dicedev_ioctl_seed))) {
			return -EFAULT;
		}

		buff->seed_chg = 1;
		buff->seed = sCmd.seed;

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



int dicedev_buffer_get_fd(struct dicedev_buffer *buff) {
	BUG_ON(!buff);

	return anon_inode_getfd("dicedev_buffer", &dicedev_buffer_fops, buff, O_RDWR);
}
