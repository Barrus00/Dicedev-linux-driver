#include <linux/types.h>
#include <linux/delay.h>

#include "dicedev.h"
#include "dicedev_types.h"
#include "dicedev_utils.h"
#include "dicedev_buffer.h"
#include "dicedev_wt.h"

#define DICEDEV_MAX_DEVICES 256
#define DICEDEV_MAX_BUFFER_SIZE (DICEDEV_PAGE_SIZE * 1024)

#define DICEDEV_MAX_ALLOWED (((uint64_t) 1 << 33) - 1)


static dev_t dicedev_devno;
static struct dicedev_device *dicedev_devices[DICEDEV_MAX_DEVICES];
static DEFINE_MUTEX(dicedev_devices_lock);
static struct class dicedev_class = {
	.name = "dicedev",
	.owner = THIS_MODULE,
};

/* Dicedev chardriver file operations */

static int dicedev_open(struct inode *inode, struct file *filp) {
	struct dicedev_device *dev = container_of(inode->i_cdev, struct dicedev_device, cdev);
	struct dicedev_context *ctx = kzalloc(sizeof(struct dicedev_context), GFP_KERNEL);

	if (!ctx) {
		return -ENOMEM;
	}

	ctx->dev = dev;
	spin_lock_init(&ctx->slock);
	ctx->failed = false;
	ctx->task_count = 0;
	init_waitqueue_head(&ctx->wq);
	INIT_LIST_HEAD(&ctx->allocated_buffers);

	filp->private_data = ctx;

	return nonseekable_open(inode, filp);
}


static int dicedev_release(struct inode *inode, struct file *filp) {
	struct dicedev_context *ctx = filp->private_data;
	struct dicedev_device *dev = ctx->dev;
	struct list_head *lh, *tmp;
	unsigned long flags;

	/* Mark all buffers as destroyed */
	spin_lock_irqsave(&dev->slock, flags); /* This lock is to ensure safe buff->destroyed read on buffer operations */
	list_for_each(lh, &ctx->allocated_buffers) {
		struct dicedev_buffer *buff = container_of(lh, struct dicedev_buffer, context_buffers);

		buff->destroyed = true;
	}
	spin_unlock_irqrestore(&dev->slock, flags);

	/* Wait for all tasks to finish */
	while (ctx->task_count > 0) {
		wait_event_interruptible(ctx->wq, ctx->task_count == 0);
	}

	/* Close all allocated buffers */
	list_for_each_safe(lh, tmp, &ctx->allocated_buffers) {
		struct dicedev_buffer *buff = container_of(lh, struct dicedev_buffer, context_buffers);

		spin_lock_irqsave(&dev->slock, flags);

		if (buff->binded_slot != DICEDEV_BUFFER_NO_SLOT) {
			unbind_slot(dev, buff);
		}

		spin_unlock_irqrestore(&dev->slock, flags);

		list_del(lh);

		/* Decrease reference count buffer reference count
		 * Buffer is not freed here, because it could cause a fatal error
		 * if process is still using it.
		 * It will be freed in buffer release action */
		fput(buff->file);
	}

	kfree(ctx);

	return 0;
}


static long dicedev_ioctl_create_set(struct dicedev_context *ctx, unsigned long arg);

static long dicedev_ioctl_run(struct dicedev_context *ctx, unsigned long arg);

static long dicedev_ioctl_wait(struct dicedev_context *ctx, unsigned long arg);

static long dicedev_ioctl_seed_increment(struct dicedev_context *ctx, unsigned long arg);


static long dicedev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct dicedev_context *ctx = filp->private_data;

	switch (cmd) {
	case DICEDEV_IOCTL_CREATE_SET:
		return dicedev_ioctl_create_set(ctx, arg);

	case DICEDEV_IOCTL_RUN:
		return dicedev_ioctl_run(ctx, arg);

	case DICEDEV_IOCTL_WAIT:
		return dicedev_ioctl_wait(ctx, arg);

	case DICEDEV_IOCTL_ENABLE_SEED_INCREMENT:
		return dicedev_ioctl_seed_increment(ctx, arg);

	default:
		printk(KERN_ERR "dicedev_ioctl: invalid command\n");
		return -EINVAL;
	}
}


static const struct file_operations dicedev_file_ops = {
	.owner = THIS_MODULE,
	.open = dicedev_open,
	.release = dicedev_release,
	.unlocked_ioctl = dicedev_ioctl
};

/* ioctl handlers */

static long dicedev_ioctl_create_set(struct dicedev_context *ctx, unsigned long arg) {
	char __user *argp = (char __user *)arg;
	struct dicedev_ioctl_create_set cs;
	struct dicedev_buffer *buff;
	int err;
	int fd;

	if (copy_from_user(&cs, argp, sizeof(struct dicedev_ioctl_create_set))) {
		err = -EFAULT;
		goto error;
	}

	if (cs.size < 0 || cs.size > DICEDEV_MAX_BUFFER_SIZE) {
		err = -EINVAL;
		goto error;
	}

	if (cs.allowed > DICEDEV_MAX_ALLOWED) {
		err = -EINVAL;
		goto error;
	}

	buff = kzalloc(sizeof(struct dicedev_buffer), GFP_KERNEL);

	if (!buff) {
		err = -ENOMEM;
		goto error;
	}

	if (dicedev_buffer_init(ctx, buff, cs.size, cs.allowed) < 0) {
		err = -ENOMEM;
		goto err_buff;
	}

	if ((err = dicedev_buffer_get_fd(buff)) < 0) {
		goto err_fd;
	}

	fd = err;

	list_add(&buff->context_buffers, &ctx->allocated_buffers);

	return fd;

err_fd:
	dicedev_buffer_destroy(buff);

err_buff:
	kfree(buff);

error:
	return err;
}


static long dicedev_ioctl_run(struct dicedev_context *ctx, unsigned long arg) {
	char __user *argp = (char __user *)arg;
	struct dicedev_ioctl_run rCmd;
	struct dicedev_buffer *cBuff, *dBuff;
	struct dicedev_task *task;
	unsigned long flags;

	if (copy_from_user(&rCmd, argp, sizeof(struct dicedev_ioctl_run))) {
		return -EFAULT;
	}

	if (rCmd.addr % 4 !=  0 || rCmd.size % 4 != 0) {
		return -EINVAL;
	}

	cBuff = fdget(rCmd.cfd).file->private_data;
	dBuff = fdget(rCmd.bfd).file->private_data;

	if (cBuff == NULL || dBuff == NULL) {
		return -EINVAL;
	}

	if ((uint64_t)rCmd.addr + (uint64_t) rCmd.size > cBuff->pt->max_size) {
		return -EINVAL;
	}

	if (cBuff->ctx != ctx || dBuff->ctx != ctx) {
		return -EINVAL;
	}

	if (ctx->failed) {
		return -EIO;
	}

	task = kzalloc(sizeof(struct dicedev_task), GFP_KERNEL);

	if (!task) {
		return -ENOMEM;
	}

	spin_lock_irqsave(&dBuff->dev->slock, flags);
	/* We dont have to perform safe task add procedure as in buffer_write,
	 * as there's no way to call ioctl when releasing context */
	ctx->task_count++;
	spin_unlock_irqrestore(&dBuff->dev->slock, flags);

	task->type = DICEDEV_TASK_TYPE_RUN;
	task->ctx = ctx;
	INIT_LIST_HEAD(&task->lh);
	task->result_count = 0;
	task->buff_out = dBuff;

	task->run.cBuff = cBuff;
	task->run.offset = rCmd.addr;
	task->run.size = rCmd.size;

	dicedev_wt_add_task(ctx, task);

	return 0;
}


static long dicedev_ioctl_wait(struct dicedev_context *ctx, unsigned long arg) {
	char __user *argp = (char __user *)arg;
	struct dicedev_ioctl_wait wCmd;

	if (copy_from_user(&wCmd, argp, sizeof(struct dicedev_ioctl_wait))) {
		return -EFAULT;
	}

	while (wCmd.cnt < ctx->task_count) {
		wait_event_interruptible(ctx->wq, wCmd.cnt >= ctx->task_count);
	}

	if (ctx->failed) {
		return -EIO;
	}

	return 0;
}


static long dicedev_ioctl_seed_increment(struct dicedev_context *ctx, unsigned long arg) {
	char __user *argp = (char __user *)arg;
	struct dicedev_ioctl_seed_increment siCmd;
	unsigned long flags;

	if (copy_from_user(&siCmd, argp, sizeof(struct dicedev_ioctl_seed_increment))) {
		return -EFAULT;
	}

	if (siCmd.enable_increment != 0 && siCmd.enable_increment != 1) {
		return -EINVAL;
	}

	spin_lock_irqsave(&ctx->dev->slock, flags);

	dicedev_iow(ctx->dev, DICEDEV_INCREMENT_SEED, siCmd.enable_increment);

	spin_unlock_irqrestore(&ctx->dev->slock, flags);

	return 0;
}


static inline void dicedev_ctx_fail(struct dicedev_context *ctx) {
	ctx->failed = true;
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

		if (istatus & (DICEDEV_INTR_CMD_ERROR | DICEDEV_INTR_MEM_ERROR)) {
			BUG_ON(dev->wt.running_task == NULL);
			dicedev_ctx_fail(dev->wt.running_task->ctx);
		}

		if (istatus & DICEDEV_INTR_FENCE_WAIT) {
			BUG_ON(dev->wt.running_task == NULL);

			dev->fence_state = DICEDEV_FENCE_STATE_REACHED;
			wake_up_interruptible(&dev->wt.event_cond);
		}
	}

	spin_unlock_irqrestore(&dev->slock, flags);
	return IRQ_RETVAL(istatus);
}

/* PCI driver */

static struct pci_device_id dicedev_pci_ids[] = {
	{ PCI_DEVICE(DICEDEV_VENDOR_ID, DICEDEV_DEVICE_ID) },
	{ 0 }
};


static int dicedev_probe(struct pci_dev *pdev, const struct pci_device_id *pci_id)
{
	int err, i;

	/* Allocate device structure */
	struct dicedev_device *dev = kzalloc(sizeof(struct dicedev_device), GFP_KERNEL);
	if (!dev) {
		err = -ENOMEM;
		goto err_alloc_dev;
	}

	pci_set_drvdata(pdev, dev);
	dev->pdev = pdev;

	/* Locks etc. */
	spin_lock_init(&dev->slock);
	spin_lock_init(&dev->feed_lock);

	/* Allocate a free index. */
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

	/* Enable hardware resources */
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


	/* Initialize device defaults */
	dev->fence_state = DICEDEV_FENCE_STATE_NONE;
	dev->failed = false;

	for (i = 0; i < DICEDEV_NUM_SLOTS; i++) {
		dev->buff_slots[i] = NULL;
	}

	dev->free_slots = DICEDEV_NUM_SLOTS;

	dicedev_wt_init(dev);

	dicedev_iow(dev, DICEDEV_INTR, DICEDEV_ALL_INTR); /* Clear interrupts */
	dicedev_iow(dev, DICEDEV_INTR_ENABLE, DICEDEV_ACTIVE_INTR); /* Enable interrupts */
	dicedev_iow(dev, DICEDEV_CMD_FENCE_WAIT, 1); /* Init fence */
	dicedev_iow(dev, DICEDEV_CMD_FENCE_LAST, 0);
	dicedev_iow(dev, DICEDEV_ENABLE, 1); /* Enable device */

	/* Device is running now. */

	for (i = 0; i < DICEDEV_NUM_SLOTS; i++) {
		uint32_t cmd;
		cmd = DICEDEV_USER_CMD_UNBIND_SLOT_HEADER(i);

		feed_cmd(dev, &cmd, 1);
	}

	cdev_init(&dev->cdev, &dicedev_file_ops);

	if ((err = cdev_add(&dev->cdev, dicedev_devno + dev->idx, 1))) {
		goto err_cdev_add;
	}

	dev->dev = device_create(&dicedev_class,
				 &dev->pdev->dev, dicedev_devno + dev->idx, dev,
				 "dice%d", dev->idx);

	if (IS_ERR(dev->dev)) {
		printk(KERN_ERR "dicedev: device_create failed\n");
		dev->dev = 0;
		goto err_dev_create;
	}

	return 0;

err_dev_create:
err_cdev_add:
	dicedev_iow(dev, DICEDEV_ENABLE, 0);
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
	kthread_stop(dev->wt.thread);
	dicedev_iow(dev, DICEDEV_ENABLE, 0);

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
	kthread_park(dev->wt.thread);
	dicedev_iow(dev, DICEDEV_ENABLE, 0);
	return 0;
}


static int dicedev_resume(struct pci_dev *pdev) {
	struct dicedev_device *dev = pci_get_drvdata(pdev);
	kthread_unpark(dev->wt.thread);

	dicedev_iow(dev, DICEDEV_INTR_ENABLE, DICEDEV_ACTIVE_INTR);
	dicedev_iow(dev, DICEDEV_ENABLE, 1);
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
