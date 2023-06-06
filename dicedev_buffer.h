#ifndef DICEDEV_BUFFER_H
#define DICEDEV_BUFFER_H

#include "dicedev_types.h"

int dicedev_buffer_init(struct dicedev_context *ctx,
			struct dicedev_buffer *buff,
			size_t size,
			uint64_t allowed);


int dicedev_buffer_get_fd(struct dicedev_buffer *buff);


void dicedev_buffer_init_reader(struct dicedev_buffer *buff);


#endif //DICEDEV_BUFFER_H
