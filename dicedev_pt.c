#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/types.h>

#include "dicedev_pt.h"

#define DICEDEV_PT_PREFIX "[dicedev_pt] "

#define PAGE_SIZE_BITS 12
#define PAGE_SIZE (1 << PAGE_SIZE_BITS)

#define IS_PAGE_ALIGNED(x) (((x) & (PAGE_SIZE - 1)) == 0)

#define PRESENT_MASK 0x1
#define PRESENT(x) ((x) & PRESENT_MASK)
#define PA_MASK 0xFFFFFFF0 // TODO: check if this is correct for little endian

#define MAKE_PT_ENTRY(addr) (0x1 | ((addr) >> 4))


typedef uint32_t page_entry_t;


static int page_init(struct pci_dev *pdev, struct page *p) {
	if (!p) {
		printk(KERN_ERR "page_init: p is NULL\n");
		return -1; // TODO: error code
	}

	p->page = dma_alloc_coherent(pdev, PAGE_SIZE, &p->dma_handler, GFP_KERNEL | __GFP_ZERO);

	if (!p->page) {
		printk(KERN_ERR "page_init: dma_alloc_coherent failed\n");
		return -1; // TODO: error code + should we free something here?
	}

	return 0;
}


static void page_free(struct pci_dev *pdev, struct page *p) {
	if (!p) {
		printk(KERN_ERR "page_free: p is NULL\n");
		return;
	}

	dma_free_coherent(pdev, PAGE_SIZE, p->page, p->dma_handler);
}


int dicedev_pt_create(struct pci_dev *pdev, struct dicedev_page_table *page_table, size_t size) {
	size_t i, j;
	size_t num_pages;
	page_entry_t *page_entries;
	struct page *tmp_page;

	if (!p) {
		printk(KERN_ERR "dicedev_pt_create: p is NULL\n");
		return -1; // TODO: error code
	}

	if (size == 0) {
		printk(KERN_ERR "dicedev_pt_create: size is 0\n");
		return -1; // TODO: error code
	}

	if (page_init(pdev, &page_table->pt) < 0) {
		printk(KERN_ERR "dicedev_pt_create: page_init failed\n");
		return err; // TODO: error code
	}

	num_pages = (size + PAGE_SIZE - 1) >> PAGE_SIZE_BITS;
	page_entries = (page_entry_t *)p->page;
	page_table->max_size = size;

	if (!(page_table->entries = kzalloc(sizeof(struct page) * num_pages, GFP_KERNEL))) {
		printk(KERN_ERR "dicedev_pt_create: kmalloc failed\n");
		goto err_entries;
	}


	for (i = 0; i < num_pages; i++) {
		if (page_init(pdev, &page_table->pages[i]) < 0) {
			printk(KERN_ERR "dicedev_pt_create: page_init failed\n");
			goto err_pages;
		}

		page_entries[i] = MAKE_PT_ENTRY(page_table->pages[i]);
	}

	return 0;

err_pages:
	for (i = 0; i < num_pages; i++) {
		if (PRESENT(page_entries[i])) {
			page_free(pdev, &page_table->pages[i]
		} else {
			break;
		}
	}
	kfree(page_table->entries);

err_entries:
	page_free(pdev, &page_table->pt);

err:
	return 0;
}


static void dicedev_pt_free(dicedev_page_table *page_table) {
	if (!page_table) {
		printk(KERN_ERR "dicedev_pt_free: page_table is NULL\n");
		return;
	}

	for (i = 0; i < page_table->num_pages; i++) {
		if (PRESENT(page_table->entries[i])) {
			page_free(pdev, &page_table->pages[i]
		} else {
			break;
		}
	}

	kfree(page_table->entries);

	page_free(pdev, &page_table->pt);

	return;
}