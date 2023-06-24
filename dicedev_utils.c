#include "dicedev_types.h"
#include "dicedev_pt.h"

#include "dicedev_utils.h"


void dicedev_iow(struct dicedev_device *dev, uint32_t reg, uint32_t val) {
	iowrite32(val, dev->bar + reg);
}


uint32_t dicedev_ior(struct dicedev_device *dev, uint32_t reg) {
	return ioread32(dev->bar + reg);
}


void feed_cmd(struct dicedev_device *dev, uint32_t *cmd, size_t cmd_size) {
	unsigned long flags;
	spin_lock_irqsave(&dev->feed_lock, flags);

	for (size_t i = 0; i < cmd_size; i++) {
		while (dicedev_ior(dev, CMD_MANUAL_FREE) == 0) {
			// Wait for free slot.
		}

		dicedev_iow(dev, CMD_MANUAL_FEED, cmd[i]);
	}

	spin_unlock_irqrestore(&dev->feed_lock, flags);
}


int get_slot(struct dicedev_device *dev) {
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


void bind_slot(struct dicedev_device *dev, struct dicedev_buffer *buff, int slot) {
	uint32_t cmd[4];

	BUG_ON(dev->buff_slots[slot] != NULL);

	dev->free_slots--;
	dev->buff_slots[slot] = buff;
	buff->binded_slot = slot;
	buff->usage_count = 1;

	cmd[0] = DICEDEV_USER_CMD_BIND_SLOT_HEADER(slot, buff->seed);
	cmd[1] = buff->allowed;
	cmd[2] = buff->pt->pt.dma_handler;
	cmd[3] = buff->pt->pt.dma_handler >> 32;

	feed_cmd(dev, cmd, 4);
}


void unbind_slot(struct dicedev_device *dev, struct dicedev_buffer *buff) {
	uint32_t cmd;

	BUG_ON(buff->binded_slot == DICEDEV_BUFFER_NO_SLOT);

	cmd = DICEDEV_USER_CMD_UNBIND_SLOT_HEADER(buff->binded_slot);

	dev->buff_slots[buff->binded_slot] = NULL;
	buff->binded_slot = DICEDEV_BUFFER_NO_SLOT;
	buff->usage_count = 0;
	dev->free_slots++;

	feed_cmd(dev, &cmd, 1);
}


void restart_device(struct dicedev_device *dev) {
	dicedev_iow(dev, DICEDEV_ENABLE, 0);
	dicedev_iow(dev, DICEDEV_INTR_ENABLE, 0);
	dicedev_iow(dev, DICEDEV_INTR, DICEDEV_ACTIVE_INTR);
	dicedev_iow(dev, DICEDEV_INTR_ENABLE, DICEDEV_ACTIVE_INTR);
	dicedev_iow(dev, DICEDEV_ENABLE, 1);
}
