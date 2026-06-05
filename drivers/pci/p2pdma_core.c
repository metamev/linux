// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Peer 2 Peer DMA support core, providing a bare-bones
 * pcim_p2pdma_provider() interface to drivers even if full P2PDMA
 * isn't present.  The full P2PDMA feature is in p2pdma.c (see
 * CONFIG_PCI_P2PDMA).
 *
 * Copyright (c) 2016-2018, Logan Gunthorpe
 * Copyright (c) 2016-2017, Microsemi Corporation
 * Copyright (c) 2017, Christoph Hellwig
 * Copyright (c) 2018, Eideticom Inc.
 */

#define pr_fmt(fmt) "pci-p2pdma: " fmt
#include <linux/ctype.h>
#include <linux/genalloc.h>
#include <linux/memremap.h>
#include <linux/pci-p2pdma.h>
#include <linux/xarray.h>

#include "p2pdma.h"

static void pci_p2pdma_release(void *data)
{
	struct pci_dev *pdev = data;
	struct pci_p2pdma *p2pdma;

	p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);
	if (!p2pdma)
		return;

	/* Flush and disable pci_alloc_p2p_mem() */
	pdev->p2pdma = NULL;
	pci_p2pdma_release_pool(pdev, p2pdma);
	xa_destroy(&p2pdma->map_types);
}

/**
 * pcim_p2pdma_init - Initialise peer-to-peer DMA providers
 * @pdev: The PCI device to enable P2PDMA for
 *
 * This function initializes the peer-to-peer DMA infrastructure
 * for a PCI device. It allocates and sets up the necessary data
 * structures to support P2PDMA operations, including mapping type
 * tracking.
 */
int pcim_p2pdma_init(struct pci_dev *pdev)
{
	struct pci_p2pdma *p2p;
	int i, ret;

	p2p = rcu_dereference_protected(pdev->p2pdma, 1);
	if (p2p)
		return 0;

	p2p = devm_kzalloc(&pdev->dev, sizeof(*p2p), GFP_KERNEL);
	if (!p2p)
		return -ENOMEM;

	xa_init(&p2p->map_types);
	/*
	 * Iterate over all standard PCI BARs and record only those that
	 * correspond to MMIO regions. Skip non-memory resources (e.g. I/O
	 * port BARs) since they cannot be used for peer-to-peer (P2P)
	 * transactions.
	 */
	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		if (!(pci_resource_flags(pdev, i) & IORESOURCE_MEM))
			continue;

		p2p->mem[i].owner = &pdev->dev;
		p2p->mem[i].bus_offset =
			pci_bus_address(pdev, i) - pci_resource_start(pdev, i);
	}

	ret = devm_add_action_or_reset(&pdev->dev, pci_p2pdma_release, pdev);
	if (ret)
		goto out_p2p;

	rcu_assign_pointer(pdev->p2pdma, p2p);
	return 0;

out_p2p:
	devm_kfree(&pdev->dev, p2p);
	return ret;
}
EXPORT_SYMBOL_GPL(pcim_p2pdma_init);

/**
 * pcim_p2pdma_provider - Get peer-to-peer DMA provider
 * @pdev: The PCI device to enable P2PDMA for
 * @bar: BAR index to get provider
 *
 * This function gets peer-to-peer DMA provider for a PCI device. The lifetime
 * of the provider (and of course the MMIO) is bound to the lifetime of the
 * driver. A driver calling this function must ensure that all references to the
 * provider, and any DMA mappings created for any MMIO, are all cleaned up
 * before the driver remove() completes.
 *
 * Since P2P is almost always shared with a second driver this means some system
 * to notify, invalidate and revoke the MMIO's DMA must be in place to use this
 * function. For example a revoke can be built using DMABUF.
 */
struct p2pdma_provider *pcim_p2pdma_provider(struct pci_dev *pdev, int bar)
{
	struct pci_p2pdma *p2p;

	if (!(pci_resource_flags(pdev, bar) & IORESOURCE_MEM))
		return NULL;

	p2p = rcu_dereference_protected(pdev->p2pdma, 1);
	if (WARN_ON(!p2p))
		/* Someone forgot to call to pcim_p2pdma_init() before */
		return NULL;

	return &p2p->mem[bar];
}
EXPORT_SYMBOL_GPL(pcim_p2pdma_provider);
