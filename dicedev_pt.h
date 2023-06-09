#ifndef DICEDEV_PT_H
#define DICEDEV_PT_H

#define DICEDEV_ADDR_BITS 40

struct dicedev_page {
	void *page;
	dma_addr_t dma_handler;
};


struct dicedev_page_table {
	struct dicedev_page pt;
	struct dicedev_page *pages;
	size_t num_pages;
	size_t max_size;
};


int dicedev_pt_init(struct pci_dev *pdev, struct dicedev_page_table *page_table, size_t size);


void dicedev_pt_free(struct pci_dev *pdev, struct dicedev_page_table *page_table);


char *dicedev_pt_read(struct dicedev_page_table *pt, size_t count, size_t offset);


#endif //DICEDEV_PT_H
