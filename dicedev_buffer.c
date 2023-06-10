#include "dicedev_wt.h"
#include "dicedev_utils.h"

#include "dicedev_buffer.h"

#define READS_AVAILABLE(reader) (reader.result_count - reader.offset)

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
		return -EINVAL;
	}

	vma->vm_private_data = buff;
	vma->vm_ops = &dicedev_buffer_vm_ops;

	return 0;
}


static ssize_t dicedev_buffer_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	struct dicedev_buffer *buff = filp->private_data;
	struct dicedev_context *ctx;
	ssize_t err;
	struct dicedev_task *task;
	uint32_t *cmd;
	unsigned long flags;

	if (count % 4 != 0) {
		err = -EINVAL;
		goto err;
	}

	spin_lock_irqsave(&buff->dev->slock, flags);

	if (buff->destroyed) {
		spin_unlock_irqrestore(&buff->dev->slock, flags);
		err = -EPERM;
		goto err;
	}

//	spin_lock_irqsave(&buff->ctx->slock, flags2);

	ctx = buff->ctx;
	ctx->task_count++;

	spin_unlock_irqrestore(&buff->dev->slock, flags);
//	spin_unlock_irqrestore(&buff->ctx->slock, flags2);

	cmd = kmalloc(count, GFP_KERNEL);

	if (!cmd) {
		err = -ENOMEM;
		goto err_task;
	}

	if (copy_from_user(cmd, buf, count)) {
		err = -EFAULT;
		goto err_copy;
	}

	task = kmalloc(sizeof(struct dicedev_task), GFP_KERNEL);

	if (!task) {
		err = -ENOMEM;
		goto err_copy;
	}


	task->type = DICEDEV_TASK_TYPE_WRITE;
	task->ctx = ctx;
	INIT_LIST_HEAD(&task->lh);
	task->result_count = 0;
	task->buff_out = buff;

	task->write.cmd = cmd;
	task->write.cmd_size = count;

	dicedev_wt_add_task(ctx, task);

	return count;

err_copy:
	kfree(cmd);
err_task:
	spin_lock_irqsave(&buff->dev->slock, flags);
	ctx->task_count--;
	wake_up_interruptible(&task->ctx->wq);
	spin_unlock_irqrestore(&buff->dev->slock, flags);
err:
	return err;
}


static int __buff_read_result(struct dicedev_buffer *buffer, struct dice *res) {
	size_t page_no;
	struct dice *result_page;
	size_t off = buffer->reader.offset++;

	page_no = off / (DICEDEV_PAGE_SIZE / sizeof(struct dice));

	if (page_no >= buffer->pt->num_pages) {
		return -EINVAL;
	}

	result_page = (struct dice *)buffer->pt->pages[page_no].page;

	*res = result_page[off % (DICEDEV_PAGE_SIZE / sizeof(struct dice))];

	return 0;
}


static ssize_t dicedev_buffer_read(struct file *filp, char __user *buff, size_t count, loff_t *offp) {
	struct dicedev_buffer *res_buff = filp->private_data;
	struct dice result;
	unsigned long flags;

	if (count != sizeof(struct dice)) {
		printk(KERN_ERR "dicedev_buffer_read: count is not 8\n");
		return -EINVAL;
	}

	spin_lock_irqsave(&res_buff->dev->slock, flags);
	while (READS_AVAILABLE(res_buff->reader) == 0 && !res_buff->destroyed && res_buff->ctx->task_count > 0) {
		spin_unlock_irqrestore(&res_buff->dev->slock, flags);
		wait_event_interruptible(res_buff->ctx->wq, READS_AVAILABLE(res_buff->reader) > 0 || res_buff->destroyed || res_buff->ctx->task_count == 0);
		spin_lock_irqsave(&res_buff->dev->slock, flags);
	}

	if (READS_AVAILABLE(res_buff->reader) == 0) {
		printk(KERN_ERR "dicedev_buffer_read: no reads available\n");
		spin_unlock_irqrestore(&res_buff->dev->slock, flags);
		return 0;
	}

	if (__buff_read_result(res_buff, &result)) {
		spin_unlock_irqrestore(&res_buff->dev->slock, flags);
		printk(KERN_ERR "dicedev_buffer_read: failed to read result\n");
		return -EFAULT;
	}

	if (copy_to_user(buff, &result, sizeof(struct dice))) {
		spin_unlock_irqrestore(&res_buff->dev->slock, flags);
		printk(KERN_ERR "dicedev_buffer_read: failed to copy to user\n");
		return -EFAULT;
	}

	spin_unlock_irqrestore(&res_buff->dev->slock, flags);

	return sizeof(struct dice);
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


static int dicedev_buffer_release(struct inode* inode, struct file *filp) {
	struct dicedev_buffer *buff = filp->private_data;

	dicedev_buffer_destroy(buff);
	kfree(buff);

	return 0;
}


static const struct file_operations dicedev_buffer_fops = {
	.owner = THIS_MODULE,
	.mmap = dicedev_buffer_mmap,
	.read = dicedev_buffer_read,
	.write = dicedev_buffer_write,
	.unlocked_ioctl = dicedev_buffer_ioctl,
	.release = dicedev_buffer_release
};


int dicedev_buffer_init(struct dicedev_context *ctx,
			struct dicedev_buffer *buff,
			size_t size,
			uint64_t allowed) {
	int err;
	BUG_ON(!ctx);
	BUG_ON(!buff);

	buff->pt = kzalloc(sizeof(struct dicedev_page_table), GFP_KERNEL);

	if (!buff->pt) {
		err = -ENOMEM;
		goto error;
	}

	if (dicedev_pt_init(ctx->dev->pdev, buff->pt, size) <  0) {
		err = -ENOMEM;
		goto err_pt;
	}

	buff->ctx = ctx;
	buff->dev = ctx->dev;
	INIT_LIST_HEAD(&buff->context_buffers);
	buff->destroyed = 0;
	buff->seed = DICEDEV_DEFAULT_SEED;
	buff->seed_chg = 0;
	buff->allowed = allowed;
	buff->binded_slot = DICEDEV_BUFFER_NO_SLOT;
	buff->destroyed = 0;

	buff->reader.result_count = 0;
	buff->reader.offset = 0;
	buff->file = anon_inode_getfile("dicedev_buffer", &dicedev_buffer_fops, buff, O_RDWR);

	if (IS_ERR(buff->file)) {
		err = PTR_ERR(buff->file);
		goto err_fil;
	}

	get_file(buff->file);

	return 0;

err_fil:
	dicedev_pt_free(ctx->dev->pdev, buff->pt);

err_pt:
	kfree(buff->pt);
error:
	return err;
}


void dicedev_buffer_destroy(struct dicedev_buffer *buff) {
	unsigned long flags;

	dicedev_pt_free(buff->dev->pdev, buff->pt);
	spin_lock_irqsave(&buff->dev->slock, flags);

	if (buff->binded_slot != DICEDEV_BUFFER_NO_SLOT) {
		unbind_slot(buff->dev, buff);
	}

	spin_unlock_irqrestore(&buff->dev->slock, flags);
	kfree(buff->pt);
}


int dicedev_buffer_get_fd(struct dicedev_buffer *buff) {
	int error, fd;

	error = get_unused_fd_flags(O_RDWR);

	if (error < 0) {
		return error;
	}

	fd = error;

	fd_install(fd, buff->file);

	return fd;
}
