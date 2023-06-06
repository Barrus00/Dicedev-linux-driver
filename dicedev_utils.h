#ifndef DICEDEV_UTILS_H
#define DICEDEV_UTILS_H

#include "dicedev_types.h"

/* NOTE: This function should be called with slock held */
void dicedev_iow(struct dicedev_device *dev, uint32_t reg, uint32_t val);

/* NOTE: This function should be called with slock held */
uint32_t dicedev_ior(struct dicedev_device *dev, uint32_t reg);

/* Sends command to dice device, blocks until the command is sent.
 * NOTE: This function should be called with slock held */
void feed_cmd(struct dicedev_device *dev, uint32_t *cmd, size_t size);


/* NOTE: This function should be called with slock held */
int get_slot(struct dicedev_device *dev);


/* NOTE: This function should be called with slock held */
void bind_slot(struct dicedev_device *dev, struct dicedev_buffer *buff, int slot);


/* NOTE: This function should be called with slock held */
void unbind_slot(struct dicedev_device *dev, struct dicedev_buffer *buff);

#endif //DICEDEV_UTILS_H
