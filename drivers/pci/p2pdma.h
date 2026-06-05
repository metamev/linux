/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PCI Peer 2 Peer DMA support.
 */

#ifndef _PCI_P2PDMA_H
#define _PCI_P2PDMA_H

#include <linux/genalloc.h>
#include <linux/pci-p2pdma.h>
#include <linux/xarray.h>

struct pci_p2pdma {
	struct gen_pool *pool;
	bool p2pmem_published;
	struct xarray map_types;
	struct p2pdma_provider mem[PCI_STD_NUM_BARS];
};

#ifdef CONFIG_PCI_P2PDMA
void pci_p2pdma_release_pool(struct pci_dev *pdev, struct pci_p2pdma *p2pdma);
#else
static inline void pci_p2pdma_release_pool(struct pci_dev *pdev, struct pci_p2pdma *p2pdma)
{
}
#endif

#endif

