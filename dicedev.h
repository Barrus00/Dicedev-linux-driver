#ifndef DICEDEV_H
#define DICEDEV_H

/* Section 1: PCI ids. */

#define DICEDEV_VENDOR_ID				0x0666
#define DICEDEV_DEVICE_ID				0x0011

/* Section 2: MMIO registers.  */

/* Interrupt status.  */
#define DICEDEV_INTR					0x0000
#define DICEDEV_INTR_ENABLE				0x0004
#define DICEDEV_ENABLE					0x0008
#define DICEDEV_INCREMENT_SEED				0x000c
#define DICEDEV_PROC_STATE				0x0010

#define CMD_MANUAL_FEED					0x008c
#define CMD_MANUAL_FREE					0x0094

#define DICEDEV_BAR_SIZE				0x1000

#define DICEDEV_CMD_FENCE_LAST				0x009c
#define DICEDEV_CMD_FENCE_WAIT				0x00A4

/* Section 3: misc constants, enums etc.  */

#define DICEDEV_SUM_INIT				1
#define DICEDEV_MOD					65521

#define DICEDEV_IOCTL_CREATE_SET 0x0
#define DICEDEV_IOCTL_RUN 0x1
#define DICEDEV_IOCTL_WAIT 0x3
#define DICEDEV_IOCTL_ENABLE_SEED_INCREMENT 0x4

struct dicedev_ioctl_create_set
{
	int size;
	uint64_t allowed;
};

struct dicedev_ioctl_run
{
	int cfd;
	uint32_t addr;
	uint32_t size;
	int bfd;
};

struct dicedev_ioctl_wait
{
	uint32_t cnt;
};

struct dicedev_ioctl_seed
{
	uint32_t seed;
};

struct dicedev_ioctl_seed_increment
{
	uint32_t enable_increment;
};

#define DICEDEV_INTR_FENCE_WAIT 0x1
#define DICEDEV_INTR_FEED_ERROR 0x2
#define DICEDEV_INTR_CMD_ERROR 0x4
#define DICEDEV_INTR_MEM_ERROR 0x8
#define DICEDEV_INTR_SLOT_ERROR 0x10

#define DICEDEV_PAGE_SHIFT				12
#define DICEDEV_PAGE_SIZE				0x1000

#define DICEDEV_USER_CMD_TYPE_NOP 0x0
#define DICEDEV_USER_CMD_TYPE_BIND_SLOT 0x1
#define DICEDEV_USER_CMD_TYPE_GET_DIE 0x2
#define DICEDEV_USER_CMD_TYPE_FENCE 0x3
#define DICEDEV_USER_CMD_TYPE_NEW_SET 0x4
#define DICEDEV_USER_CMD_TYPE_UNBIND_SLOT 0x5

#define DICEDEV_BUFFER_IOCTL_SEED 0x0

#define DICEDEV_USER_CMD_BIND_SLOT_HEADER(SLOT, SEED) (DICEDEV_USER_CMD_TYPE_BIND_SLOT | SLOT << 4 | SEED << 12)
#define DICEDEV_USER_CMD_GET_DIE_HEADER(NUM, OUTPUT_TYPE) (DICEDEV_USER_CMD_TYPE_GET_DIE | NUM << 4 | OUTPUT_TYPE << 20)
#define DICEDEV_USER_CMD_GET_DIE_HEADER_WSLOT(NUM, OUTPUT_TYPE, SLOT) (DICEDEV_USER_CMD_TYPE_GET_DIE | NUM << 4 | OUTPUT_TYPE << 20 | SLOT << 24)
#define DICEDEV_USER_CMD_FENCE_HEADER(NUM) (DICEDEV_USER_CMD_TYPE_FENCE | NUM << 4)
#define DICEDEV_USER_CMD_NEW_SET DICEDEV_USER_CMD_TYPE_NEW_SET
#define DICEDEV_USER_CMD_NEW_SET_WSLOT (DICEDEV_USER_CMD_TYPE_NEW_SET | SLOT << 4)
#define DICEDEV_USER_CMD_UNBIND_SLOT_HEADER(SLOT) (DICEDEV_USER_CMD_TYPE_UNBIND_SLOT | SLOT << 4)

struct dice
{
	uint32_t value;
	uint32_t type;
};

enum processing_state
{
	NONE,
	BIND_SLOT_0,
	BIND_SLOT_1,
	BIND_SLOT_2,
	GET_DIE_0,
};

enum output_type
{
	FAIR,
	SNAKE_EYES,
	CHEAT_DICE
};

#endif