// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2025, NVIDIA CORPORATION & AFFILIATES.
 */
#include <linux/dma-buf-mapping.h>
#include <linux/pci-p2pdma.h>
#include <linux/dma-resv.h>

#include "vfio_pci_priv.h"

MODULE_IMPORT_NS("DMA_BUF");

#ifdef CONFIG_VFIO_PCI_DMABUF
static int vfio_pci_dma_buf_attach(struct dma_buf *dmabuf,
				   struct dma_buf_attachment *attachment)
{
	struct vfio_pci_dma_buf *priv = dmabuf->priv;

	if (!attachment->peer2peer)
		return -EOPNOTSUPP;

	if (priv->status != VFIO_PCI_DMABUF_OK)
		return -ENODEV;

	if (!dma_buf_attach_revocable(attachment))
		return -EOPNOTSUPP;

	return 0;
}

static int vfio_pci_dma_buf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct vfio_pci_dma_buf *priv = dmabuf->priv;
	u64 req_len, req_start;

	if (priv->status != VFIO_PCI_DMABUF_OK)
		return -ENODEV;
	if ((vma->vm_flags & VM_SHARED) == 0)
		return -EINVAL;

	req_len = vma->vm_end - vma->vm_start;
	req_start = vma->vm_pgoff << PAGE_SHIFT;
	if (req_start + req_len > priv->size)
		return -EINVAL;

	if (priv->attrs == VFIO_DEVICE_FEATURE_DMA_BUF_ATTR_WC)
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	else
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vma->vm_page_prot = pgprot_decrypted(vma->vm_page_prot);

	/* See comments in vfio_pci_core_mmap() re VM_ALLOW_ANY_UNCACHED. */
	vm_flags_set(vma, VM_ALLOW_ANY_UNCACHED | VM_IO | VM_PFNMAP |
				  VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_private_data = priv;
	vma->vm_ops = &vfio_pci_mmap_ops;

	return 0;
}
#endif /* CONFIG_VFIO_PCI_DMABUF */

static void vfio_pci_dma_buf_done(struct kref *kref)
{
	struct vfio_pci_dma_buf *priv =
		container_of(kref, struct vfio_pci_dma_buf, kref);

	complete(&priv->comp);
}

static struct sg_table *
vfio_pci_dma_buf_map(struct dma_buf_attachment *attachment,
		     enum dma_data_direction dir)
{
	struct vfio_pci_dma_buf *priv = attachment->dmabuf->priv;
	struct sg_table *ret;

	dma_resv_assert_held(priv->dmabuf->resv);

	if (priv->status != VFIO_PCI_DMABUF_OK)
		return ERR_PTR(-ENODEV);

	ret = dma_buf_phys_vec_to_sgt(attachment, priv->provider,
				      priv->phys_vec, priv->nr_ranges,
				      priv->size, dir);
	if (IS_ERR(ret))
		return ret;

	kref_get(&priv->kref);
	return ret;
}

static void vfio_pci_dma_buf_unmap(struct dma_buf_attachment *attachment,
				   struct sg_table *sgt,
				   enum dma_data_direction dir)
{
	struct vfio_pci_dma_buf *priv = attachment->dmabuf->priv;

	dma_resv_assert_held(priv->dmabuf->resv);

	dma_buf_free_sgt(attachment, sgt, dir);
	kref_put(&priv->kref, vfio_pci_dma_buf_done);
}

static void vfio_pci_dma_buf_release(struct dma_buf *dmabuf)
{
	struct vfio_pci_dma_buf *priv = dmabuf->priv;

	/*
	 * Either this or vfio_pci_dma_buf_cleanup() will remove from the list.
	 * The refcount prevents both.
	 */
	if (priv->vdev) {
		down_write(&priv->vdev->memory_lock);
		list_del_init(&priv->dmabufs_elm);
		up_write(&priv->vdev->memory_lock);
		vfio_device_put_registration(&priv->vdev->vdev);
	}
	if (priv->vfile)
		fput(priv->vfile);
	kfree(priv->phys_vec);
	kfree(priv);
}

static const struct dma_buf_ops vfio_pci_dmabuf_ops = {
#ifdef CONFIG_VFIO_PCI_DMABUF
	.attach = vfio_pci_dma_buf_attach,
	.mmap = vfio_pci_dma_buf_mmap,
#endif
	.map_dma_buf = vfio_pci_dma_buf_map,
	.unmap_dma_buf = vfio_pci_dma_buf_unmap,
	.release = vfio_pci_dma_buf_release,
};

int vfio_pci_dma_buf_find_pfn(struct vfio_pci_dma_buf *vpdmabuf,
			      struct vm_area_struct *vma,
			      unsigned long address,
			      unsigned int order,
			      unsigned long *out_pfn)
{
	/*
	 * Given a VMA (start, end, pgoffs) and a fault address,
	 * search the corresponding DMABUF's phys_vec[] to find the
	 * range representing the address's offset into the VMA, and
	 * its PFN.
	 *
	 * The phys_vec[] ranges represent contiguous spans of VAs
	 * upwards from the buffer offset 0; the actual PFNs might be
	 * in any order, overlap/alias, etc.  Calculate an offset of
	 * the desired page given VMA start/pgoff and address, then
	 * search upwards from 0 to find which span contains it.
	 *
	 * On success, a valid PFN for a page sized by 'order' is
	 * returned into out_pfn.
	 *
	 * Failure occurs if:
	 * - The page would cross the edge of the VMA
	 * - The page isn't entirely contained within a range
	 * - We find a range, but the final PFN isn't aligned to the
	 *   requested order.
	 *
	 * (Upon failure, the caller is expected to try again with a
	 * smaller order; the tests above will always succeed for
	 * order=0 as the limit case.)
	 *
	 * It's suboptimal if DMABUFs are created with neigbouring
	 * ranges that are physically contiguous, since hugepages
	 * can't straddle range boundaries.  (The construction of the
	 * ranges vector should merge such ranges.)
	 */

	const unsigned long pagesize = PAGE_SIZE << order;
	unsigned long rounded_page_addr = address & ~(pagesize - 1);
	unsigned long rounded_page_end = rounded_page_addr + pagesize;
	unsigned long buf_page_offset;
	unsigned long buf_offset = 0;
	unsigned int i;

	if (rounded_page_addr < vma->vm_start || rounded_page_end > vma->vm_end)
		return -EAGAIN;

	if (unlikely(check_add_overflow(rounded_page_addr - vma->vm_start,
					vma->vm_pgoff << PAGE_SHIFT, &buf_page_offset)))
		return -EFAULT;

	for (i = 0; i < vpdmabuf->nr_ranges; i++) {
		unsigned long range_len = vpdmabuf->phys_vec[i].len;
		unsigned long range_start = vpdmabuf->phys_vec[i].paddr;

		if (buf_page_offset >= buf_offset &&
		    buf_page_offset + pagesize <= buf_offset + range_len) {
			/*
			 * The faulting page is wholly contained
			 * within the span represented by the range.
			 * Validate PFN alignment for the order:
			 */
			unsigned long pfn = (range_start >> PAGE_SHIFT) +
				((buf_page_offset - buf_offset) >> PAGE_SHIFT);

			if (IS_ALIGNED(pfn, 1 << order)) {
				*out_pfn = pfn;
				return 0;
			}
			/* Retry with smaller order */
			return -EAGAIN;
		}
		buf_offset += range_len;
	}

	/*
	 * If we get here, the address fell outside of the span
	 * represented by the (concatenated) ranges.  Setup of a
	 * mapping must ensure that the VMA is <= the total size of
	 * the ranges, so this should never happen.  But, if it does,
	 * force SIGBUS for the access and warn.
	 */
	WARN_ONCE(1, "No range for addr 0x%lx, order %d: VMA 0x%lx-0x%lx pgoff 0x%lx, %d ranges, size 0x%lx\n",
		  address, order, vma->vm_start, vma->vm_end, vma->vm_pgoff,
		  vpdmabuf->nr_ranges, vpdmabuf->size);

	return -EFAULT;
}

static int vfio_pci_dmabuf_export(struct vfio_pci_core_device *vdev,
				  struct vfio_pci_dma_buf *priv, uint32_t flags,
				  size_t size, bool status_ok)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);

	if (!vfio_device_try_get_registration(&vdev->vdev))
		return -ENODEV;

	exp_info.ops = &vfio_pci_dmabuf_ops;
	exp_info.size = size;
	exp_info.flags = flags;
	exp_info.priv = priv;

	priv->dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(priv->dmabuf)) {
		vfio_device_put_registration(&vdev->vdev);
		return PTR_ERR(priv->dmabuf);
	}

	kref_init(&priv->kref);
	init_completion(&priv->comp);

	/* dma_buf_put() now frees priv */
	INIT_LIST_HEAD(&priv->dmabufs_elm);
	down_write(&vdev->memory_lock);
	dma_resv_lock(priv->dmabuf->resv, NULL);
	priv->status = status_ok ? VFIO_PCI_DMABUF_OK :
		VFIO_PCI_DMABUF_TEMP_REVOKED;
	list_add_tail(&priv->dmabufs_elm, &vdev->dmabufs);
	dma_resv_unlock(priv->dmabuf->resv);
	up_write(&vdev->memory_lock);

	return 0;
}

#ifdef CONFIG_VFIO_PCI_DMABUF
/*
 * This is a temporary "private interconnect" between VFIO DMABUF and iommufd.
 * It allows the two co-operating drivers to exchange the physical address of
 * the BAR. This is to be replaced with a formal DMABUF system for negotiated
 * interconnect types.
 *
 * If this function succeeds the following are true:
 *  - There is one physical range and it is pointing to MMIO
 *  - When move_notify is called it means revoke, not move, vfio_dma_buf_map
 *    will fail if it is currently revoked
 */
int vfio_pci_dma_buf_iommufd_map(struct dma_buf_attachment *attachment,
				 struct phys_vec *phys)
{
	struct vfio_pci_dma_buf *priv;

	dma_resv_assert_held(attachment->dmabuf->resv);

	if (attachment->dmabuf->ops != &vfio_pci_dmabuf_ops)
		return -EOPNOTSUPP;

	priv = attachment->dmabuf->priv;
	if (priv->status != VFIO_PCI_DMABUF_OK)
		return -ENODEV;

	/* More than one range to iommufd will require proper DMABUF support */
	if (priv->nr_ranges != 1)
		return -EOPNOTSUPP;

	*phys = priv->phys_vec[0];
	return 0;
}
EXPORT_SYMBOL_FOR_MODULES(vfio_pci_dma_buf_iommufd_map, "iommufd");

int vfio_pci_core_fill_phys_vec(struct phys_vec *phys_vec,
				struct vfio_region_dma_range *dma_ranges,
				size_t nr_ranges, phys_addr_t start,
				phys_addr_t len)
{
	phys_addr_t max_addr;
	unsigned int i;

	max_addr = start + len;
	for (i = 0; i < nr_ranges; i++) {
		phys_addr_t end;

		if (!dma_ranges[i].length)
			return -EINVAL;

		if (check_add_overflow(start, dma_ranges[i].offset,
				       &phys_vec[i].paddr) ||
		    check_add_overflow(phys_vec[i].paddr,
				       dma_ranges[i].length, &end))
			return -EOVERFLOW;
		if (end > max_addr)
			return -EINVAL;

		phys_vec[i].len = dma_ranges[i].length;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(vfio_pci_core_fill_phys_vec);

int vfio_pci_core_get_dmabuf_phys(struct vfio_pci_core_device *vdev,
				  struct p2pdma_provider **provider,
				  unsigned int region_index,
				  struct phys_vec *phys_vec,
				  struct vfio_region_dma_range *dma_ranges,
				  size_t nr_ranges)
{
	struct pci_dev *pdev = vdev->pdev;

	*provider = pcim_p2pdma_provider(pdev, region_index);
	if (!*provider)
		return -EINVAL;

	return vfio_pci_core_fill_phys_vec(
		phys_vec, dma_ranges, nr_ranges,
		pci_resource_start(pdev, region_index),
		pci_resource_len(pdev, region_index));
}
EXPORT_SYMBOL_GPL(vfio_pci_core_get_dmabuf_phys);

static int validate_dmabuf_input(struct vfio_device_feature_dma_buf *dma_buf,
				 struct vfio_region_dma_range *dma_ranges,
				 size_t *lengthp)
{
	size_t length = 0;
	u32 i;

	if ((dma_buf->flags != 0) &&
	    ((dma_buf->flags & ~VFIO_DEVICE_FEATURE_DMA_BUF_ATTR_MASK) ||
	     ((dma_buf->flags & VFIO_DEVICE_FEATURE_DMA_BUF_ATTR_MASK) !=
	      VFIO_DEVICE_FEATURE_DMA_BUF_ATTR_WC)))
		return -EINVAL;

	for (i = 0; i < dma_buf->nr_ranges; i++) {
		u64 offset = dma_ranges[i].offset;
		u64 len = dma_ranges[i].length;

		if (!len || !PAGE_ALIGNED(offset) || !PAGE_ALIGNED(len))
			return -EINVAL;

		if (check_add_overflow(length, len, &length))
			return -EINVAL;
	}

	/*
	 * dma_iova_try_alloc() will WARN on if userspace proposes a size that
	 * is too big, eg with lots of ranges.
	 */
	if ((u64)(length) & DMA_IOVA_USE_SWIOTLB)
		return -EINVAL;

	*lengthp = length;
	return 0;
}

int vfio_pci_core_feature_dma_buf(struct vfio_pci_core_device *vdev, u32 flags,
				  struct vfio_device_feature_dma_buf __user *arg,
				  size_t argsz)
{
	struct vfio_device_feature_dma_buf get_dma_buf = {};
	struct vfio_region_dma_range *dma_ranges;
	struct vfio_pci_dma_buf *priv;
	size_t length;
	int ret;

	if (!vdev->pci_ops || !vdev->pci_ops->get_dmabuf_phys)
		return -EOPNOTSUPP;

	ret = vfio_check_feature(flags, argsz, VFIO_DEVICE_FEATURE_GET,
				 sizeof(get_dma_buf));
	if (ret != 1)
		return ret;

	if (copy_from_user(&get_dma_buf, arg, sizeof(get_dma_buf)))
		return -EFAULT;

	if (!get_dma_buf.nr_ranges)
		return -EINVAL;

	/*
	 * For PCI the region_index is the BAR number like everything else.
	 */
	if (get_dma_buf.region_index >= VFIO_PCI_ROM_REGION_INDEX)
		return -ENODEV;

	dma_ranges = memdup_array_user(&arg->dma_ranges, get_dma_buf.nr_ranges,
				       sizeof(*dma_ranges));
	if (IS_ERR(dma_ranges))
		return PTR_ERR(dma_ranges);

	ret = validate_dmabuf_input(&get_dma_buf, dma_ranges, &length);
	if (ret)
		goto err_free_ranges;

	priv = kzalloc_obj(*priv);
	if (!priv) {
		ret = -ENOMEM;
		goto err_free_ranges;
	}
	priv->phys_vec = kzalloc_objs(*priv->phys_vec, get_dma_buf.nr_ranges);
	if (!priv->phys_vec) {
		ret = -ENOMEM;
		goto err_free_priv;
	}

	/*
	 * Just like the vfio_pci_core_mmap() path, we need to ensure
	 * PCI regions have been requested before returning DMABUFs
	 * that reference them.  It's possible to create a DMABUF for
	 * a BAR without the BAR having already been mmap()ed.  The
	 * barmap setup requests the regions for us:
	 */
	ret = vfio_pci_core_setup_barmap(vdev, get_dma_buf.region_index);
	if (ret)
		goto err_free_phys;

	priv->vdev = vdev;
	priv->nr_ranges = get_dma_buf.nr_ranges;
	priv->size = length;
	priv->attrs = get_dma_buf.flags & VFIO_DEVICE_FEATURE_DMA_BUF_ATTR_MASK;
	ret = vdev->pci_ops->get_dmabuf_phys(vdev, &priv->provider,
					     get_dma_buf.region_index,
					     priv->phys_vec, dma_ranges,
					     priv->nr_ranges);
	if (ret)
		goto err_free_phys;

	kfree(dma_ranges);
	dma_ranges = NULL;

	ret = vfio_pci_dmabuf_export(vdev, priv, get_dma_buf.open_flags,
				     priv->size,
				     __vfio_pci_memory_enabled(vdev));
	if (ret)
		goto err_free_phys;
	/*
	 * dma_buf_fd() consumes the reference, when the file closes the dmabuf
	 * will be released.
	 */
	ret = dma_buf_fd(priv->dmabuf, get_dma_buf.open_flags);
	if (ret >= 0)
		return ret;

	dma_buf_put(priv->dmabuf);
	vfio_device_put_registration(&vdev->vdev);
err_free_phys:
	kfree(priv->phys_vec);
err_free_priv:
	kfree(priv);
err_free_ranges:
	kfree(dma_ranges);
	return ret;
}
#endif /* CONFIG_VFIO_PCI_DMABUF */

int vfio_pci_core_mmap_prep_dmabuf(struct vfio_pci_core_device *vdev,
				   struct vm_area_struct *vma,
				   u64 phys_start,
				   u64 pgoff,
				   u64 req_len)
{
	struct vfio_pci_dma_buf *priv;
	const unsigned int nr_ranges = 1;
	int ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->phys_vec = kcalloc(nr_ranges, sizeof(*priv->phys_vec),
				 GFP_KERNEL);
	if (!priv->phys_vec) {
		ret = -ENOMEM;
		goto err_free_priv;
	}

	priv->vdev = vdev;
	priv->nr_ranges = nr_ranges;
	priv->size = req_len;
	priv->attrs = 0;
	priv->phys_vec[0].paddr = phys_start + (pgoff << PAGE_SHIFT);
	priv->phys_vec[0].len = req_len;

	/*
	 * Creates a DMABUF, adds it to vdev->dmabufs list for
	 * tracking (meaning cleanup or revocation will zap them), and
	 * registers with vfio_device:
	 */
	ret = vfio_pci_dmabuf_export(vdev, priv, O_CLOEXEC, priv->size, true);
	if (ret)
		goto err_free_phys;

	/*
	 * The VMA gets the DMABUF file so that other users can locate
	 * the DMABUF via a VA.  Ownership of the original VFIO device
	 * file being mmap()ed transfers to priv, and is put when the
	 * DMABUF is released.
	 */
	priv->vfile = vma->vm_file;
	vma->vm_file = priv->dmabuf->file;
	vma->vm_private_data = priv;

	return 0;

err_free_phys:
	kfree(priv->phys_vec);
err_free_priv:
	kfree(priv);
	return ret;
}

static void __vfio_pci_dma_buf_revoke(struct vfio_pci_dma_buf *priv, bool revoked,
				      bool permanently)
{
	if ((priv->status == VFIO_PCI_DMABUF_PERM_REVOKED) ||
	    (priv->status == VFIO_PCI_DMABUF_OK && !revoked) ||
	    (priv->status == VFIO_PCI_DMABUF_TEMP_REVOKED && revoked && !permanently)) {
		return;
	}

	dma_resv_lock(priv->dmabuf->resv, NULL);
	if (revoked)
		priv->status = permanently ?
			VFIO_PCI_DMABUF_PERM_REVOKED : VFIO_PCI_DMABUF_TEMP_REVOKED;
	dma_buf_invalidate_mappings(priv->dmabuf);
	dma_resv_wait_timeout(priv->dmabuf->resv,
			      DMA_RESV_USAGE_BOOKKEEP, false,
			      MAX_SCHEDULE_TIMEOUT);
	dma_resv_unlock(priv->dmabuf->resv);
	if (revoked) {
		kref_put(&priv->kref, vfio_pci_dma_buf_done);
		wait_for_completion(&priv->comp);
		unmap_mapping_range(priv->dmabuf->file->f_mapping,
				    0, priv->size, 1);
	} else {
		/*
		 * Kref is initialize again, because when revoke
		 * was performed the reference counter was decreased
		 * to zero to trigger completion.
		 */
		kref_init(&priv->kref);
		/*
		 * There is no need to wait as no mapping was
		 * performed when the previous status was
		 * priv->status == *REVOKED.
		 */
		reinit_completion(&priv->comp);
		dma_resv_lock(priv->dmabuf->resv, NULL);
		priv->status = VFIO_PCI_DMABUF_OK;
		dma_resv_unlock(priv->dmabuf->resv);
	}
}

void vfio_pci_dma_buf_move(struct vfio_pci_core_device *vdev, bool revoked)
{
	struct vfio_pci_dma_buf *priv;
	struct vfio_pci_dma_buf *tmp;

	lockdep_assert_held_write(&vdev->memory_lock);
	/*
	 * Holding memory_lock ensures a racing VMA fault observes
	 * priv->status properly.
	 */

	list_for_each_entry_safe(priv, tmp, &vdev->dmabufs, dmabufs_elm) {
		if (!get_file_active(&priv->dmabuf->file))
			continue;
		__vfio_pci_dma_buf_revoke(priv, revoked, false);
		fput(priv->dmabuf->file);
	}
}

void vfio_pci_dma_buf_cleanup(struct vfio_pci_core_device *vdev)
{
	struct vfio_pci_dma_buf *priv;
	struct vfio_pci_dma_buf *tmp;

	down_write(&vdev->memory_lock);
	list_for_each_entry_safe(priv, tmp, &vdev->dmabufs, dmabufs_elm) {
		if (!get_file_active(&priv->dmabuf->file))
			continue;

		dma_resv_lock(priv->dmabuf->resv, NULL);
		list_del_init(&priv->dmabufs_elm);
		priv->vdev = NULL;
		priv->status = VFIO_PCI_DMABUF_PERM_REVOKED;
		dma_buf_invalidate_mappings(priv->dmabuf);
		dma_resv_wait_timeout(priv->dmabuf->resv,
				      DMA_RESV_USAGE_BOOKKEEP, false,
				      MAX_SCHEDULE_TIMEOUT);
		dma_resv_unlock(priv->dmabuf->resv);
		kref_put(&priv->kref, vfio_pci_dma_buf_done);
		wait_for_completion(&priv->comp);
		unmap_mapping_range(priv->dmabuf->file->f_mapping,
				    0, priv->size, 1);
		vfio_device_put_registration(&vdev->vdev);
		fput(priv->dmabuf->file);
	}
	up_write(&vdev->memory_lock);
}

#ifdef CONFIG_VFIO_PCI_DMABUF
int vfio_pci_dma_buf_revoke(struct vfio_pci_core_device *vdev, int dmabuf_fd)
{
	struct vfio_pci_core_device *db_vdev;
	struct dma_buf *dmabuf;
	struct vfio_pci_dma_buf *priv;
	int ret = 0;

	dmabuf = dma_buf_get(dmabuf_fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	/*
	 * The DMABUF is a user-provided fd, so sanity-check it's
	 * really a vfio_pci_dma_buf _and_ relates to the VFIO device
	 * that it was provided for:
	 */
	if (dmabuf->ops != &vfio_pci_dmabuf_ops) {
		ret = -ENODEV;
		goto out_put_buf;
	}

	priv = dmabuf->priv;
	db_vdev = READ_ONCE(priv->vdev);

	if (!db_vdev || db_vdev != vdev) {
		ret = -ENODEV;
		goto out_put_buf;
	}

	scoped_guard(rwsem_read, &vdev->memory_lock) {
		if (priv->status == VFIO_PCI_DMABUF_PERM_REVOKED) {
			ret = -EBADFD;
			break;
		}
		__vfio_pci_dma_buf_revoke(priv, true, true);
	}

 out_put_buf:
	dma_buf_put(dmabuf);

	return ret;
}
#endif /* CONFIG_VFIO_PCI_DMABUF */
