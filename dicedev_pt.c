#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/types.h>

#include "dicedev.h"

#include "dicedev_pt.h"

#define PRESENT_MASK 0x1
#define PRESENT(x) ((x) & PRESENT_MASK)

#define MAKE_PT_ENTRY(addr) (0x1 | ((addr) >> 8))


typedef uint32_t page_entry_t;


static int page_init(struct pci_dev *pdev, struct dicedev_page *p) {
	if (!p) {
		return -EINVAL;
	}

	p->page = dma_alloc_coherent(&pdev->dev, DICEDEV_PAGE_SIZE, &p->dma_handler, GFP_KERNEL | __GFP_ZERO);

	if (!p->page) {
		return -ENOMEM;
	}

	return 0;
}


static void page_free(struct pci_dev *pdev, struct dicedev_page *p) {
	if (!p) {
		return;
	}

	dma_free_coherent(&pdev->dev, DICEDEV_PAGE_SIZE, p->page, p->dma_handler);
}


int dicedev_pt_init(struct pci_dev *pdev, struct dicedev_page_table *page_table, size_t size) {
	int err;
	size_t i;
	size_t num_pages;
	page_entry_t *page_entries;

	if (!page_table) {
		err = -EINVAL;
		goto error;
	}

	if (page_init(pdev, &page_table->pt) < 0) {
		err = -ENOMEM;
		goto error;
	}

	num_pages = (size + DICEDEV_PAGE_SIZE - 1) / DICEDEV_PAGE_SIZE;
	page_entries = (page_entry_t *)page_table->pt.page;
	page_table->max_size = size;
	page_table->num_pages = num_pages;

	if (!(page_table->pages = kzalloc(sizeof(struct dicedev_page) * num_pages, GFP_KERNEL))) {
		err = -ENOMEM;
		goto err_entries;
	}

	for (i = 0; i < num_pages; i++) {
		if (page_init(pdev, &page_table->pages[i]) < 0) {
			err = -ENOMEM;
			goto err_pages;
		}

		page_entries[i] = MAKE_PT_ENTRY(page_table->pages[i].dma_handler);
	}

	return 0;

err_pages:
	for (i = 0; i < num_pages; i++) {
		if (PRESENT(page_entries[i])) {
			page_free(pdev, &page_table->pages[i]);
		} else {
			break;
		}
	}
	kfree(page_table->pages);

err_entries:
	page_free(pdev, &page_table->pt);

error:
	return err;
}


void dicedev_pt_free(struct pci_dev *pdev, struct dicedev_page_table *page_table) {
	size_t i;
	page_entry_t *page_entries;

	if (!page_table) {
		return;
	}

	page_entries = (page_entry_t *)page_table->pt.page;

	for (i = 0; i < page_table->num_pages; i++) {
		if (PRESENT(page_entries[i])) {
			page_free(pdev, &page_table->pages[i]);
		} else {
			break;
		}
	}

	kfree(page_table->pages);

	page_free(pdev, &page_table->pt);

	return;
}