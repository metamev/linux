// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2025, NVIDIA CORPORATION & AFFILIATES.
 */
#include <linux/dma-buf-mapping.h>
#include <linux/pci-p2pdma.h>
#include <linux/dma-resv.h>
#include <uapi/linux/dma-buf.h>

#include "vfio_pci_priv.h"

MODULE_IMPORT_NS("DMA_BUF");

#ifdef CONFIG_VFIO_PCI_DMABUF
static int vfio_pci_dma_buf_attach(struct dma_buf *dmabuf,
				   struct dma_buf_attachment *attachment)
{
	struct vfio_pci_dma_buf *priv = dmabuf->priv;

	if (!attachment->peer2peer)
		return -EOPNOTSUPP;

	if (priv->revoked)
		return -ENODEV;

	if (!dma_buf_attach_revocable(attachment))
		return -EOPNOTSUPP;

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

	if (priv->revoked)
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
#endif
	.map_dma_buf = vfio_pci_dma_buf_map,
	.unmap_dma_buf = vfio_pci_dma_buf_unmap,
	.release = vfio_pci_dma_buf_release,
};

int vfio_pci_dma_buf_find_pfn(struct vfio_pci_dma_buf *priv,
			      struct vm_area_struct *vma,
			      unsigned long fault_addr,
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
	 * - A hugepage would cross the edge of the VMA,
	 * - A hugepage isn't entirely contained within a range
	 *   (including where it straddles the boundary between
	 *   ranges),
	 * - We find a range, but the final PFN isn't aligned to the
	 *   requested order.
	 *
	 * Upon failure, -EAGAIN is returned and the caller is
	 * expected to try again with a smaller order, which will
	 * eventually succeed (order=0 will always work).
	 *
	 * It's suboptimal if DMABUFs are created with neighbouring
	 * ranges that are physically contiguous, since hugepages
	 * can't straddle range boundaries.  (The construction of the
	 * ranges should merge them in this case.)
	 *
	 * Finally, vma_pgoff_adjust is used with a DMABUF created for
	 * a VFIO BAR mmap: a BAR mapped with vm_pgoff > 0 creates a
	 * DMABUF such that byte 0 of the VMA corresponds to byte 0 of
	 * the DMABUF and byte 'vm_pgoff << PAGE_SHIFT' into the BAR.
	 * To avoid double-offsetting in this scenario, subtracting
	 * vma_pgoff_adjust from this (non-zero) vm_pgoff generates
	 * the effective offset.
	 */

	const unsigned long pagesize = PAGE_SIZE << order;
	unsigned long vma_off = ((vma->vm_pgoff - priv->vma_pgoff_adjust) <<
				 PAGE_SHIFT) & VFIO_PCI_OFFSET_MASK;
	unsigned long rounded_page_addr = ALIGN_DOWN(fault_addr, pagesize);
	unsigned long rounded_page_end = rounded_page_addr + pagesize;
	unsigned long fault_offset;
	unsigned long fault_offset_end;
	unsigned long range_start_offset = 0;
	unsigned int i;
	int ret;

	if (rounded_page_addr < vma->vm_start || rounded_page_end > vma->vm_end) {
		if (order > 0)
			return -EAGAIN;

		/* A fault address outside of the VMA is absurd. */
		dev_warn_ratelimited(
			&priv->vdev->pdev->dev,
			"Fault addr 0x%lx outside VMA 0x%lx-0x%lx\n",
			fault_addr, vma->vm_start, vma->vm_end);
		return -EFAULT;
	}

	/*
	 * fault_offset[_end] is the span within the DMABUF
	 * corresponding to the faulting page:
	 */
	if (unlikely(check_add_overflow(rounded_page_addr - vma->vm_start,
					vma_off, &fault_offset) ||
		     check_add_overflow(fault_offset, pagesize,
					&fault_offset_end)))
		return -EFAULT;

	/*
	 * Iterate over ranges in the buffer, summing their lengths:
	 * range_start_offset represents the current range's starting
	 * offset in the buffer (from 0 upwards).
	 *
	 * A failure for order == 0 is unexpected, and triggers a
	 * fault/warn.
	 */
	ret = (order == 0) ? -EFAULT : -EAGAIN;

	for (i = 0; i < priv->nr_ranges; i++) {
		size_t range_len = priv->phys_vec[i].len;

		/* Early exit if range starts after the page end */
		if (fault_offset_end <= range_start_offset)
			break;

		if (fault_offset >= range_start_offset &&
		    fault_offset_end <= range_start_offset + range_len) {
			/*
			 * The faulting page is wholly contained
			 * within the span represented by this range,
			 * so validate PFN alignment for the order.
			 * The if() condition ensures the pfn
			 * arithmetic won't overflow.
			 */
			unsigned long pfn =
				((fault_offset - range_start_offset) +
				 priv->phys_vec[i].paddr) >> PAGE_SHIFT;

			if (IS_ALIGNED(pfn, 1 << order)) {
				*out_pfn = pfn;
				ret = 0;
			}
			/*
			 * Else order > 0; EAGAIN retries with smaller
			 * order
			 */
			break;
		}
		range_start_offset += range_len;
	}

	if (order == 0 && ret != 0)
		/*
		 * The address fell outside of the span represented by
		 * the (concatenated) ranges.  As setup of a mapping
		 * ensures that the VMA is <= the total size of the
		 * ranges this should never happen.  If it does, warn
		 * and SIGBUS.
		 */
		dev_warn_ratelimited(
			&priv->vdev->pdev->dev,
			"No range for addr 0x%lx, order %d: VMA 0x%lx-0x%lx pgoff 0x%lx, %u ranges, size 0x%zx\n",
			fault_addr, order, vma->vm_start, vma->vm_end,
			vma->vm_pgoff, priv->nr_ranges, priv->size);

	return ret;
}

/*
 * Create a DMABUF corresponding to priv, add it to vdev->dmabufs list
 * for tracking (meaning cleanup or revocation will zap it), and take
 * a vfio_device registration.
 */
static int vfio_pci_dmabuf_export(struct vfio_pci_core_device *vdev,
				  struct vfio_pci_dma_buf *priv, u32 flags)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);

	if (!vfio_device_try_get_registration(&vdev->vdev))
		return -ENODEV;

	exp_info.ops = &vfio_pci_dmabuf_ops;
	exp_info.size = priv->size;
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
	priv->revoked = !__vfio_pci_memory_enabled(vdev);
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
	if (priv->revoked)
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

	if (!get_dma_buf.nr_ranges || get_dma_buf.flags)
		return -EINVAL;

	/*
	 * For PCI the region_index is the BAR number like everything
	 * else.  Check that PCI resources have been claimed for it.
	 */
	if (get_dma_buf.region_index >= VFIO_PCI_ROM_REGION_INDEX ||
	    vfio_pci_core_setup_barmap(vdev, get_dma_buf.region_index))
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

	priv->vdev = vdev;
	priv->nr_ranges = get_dma_buf.nr_ranges;
	priv->size = length;
	ret = vdev->pci_ops->get_dmabuf_phys(vdev, &priv->provider,
					     get_dma_buf.region_index,
					     priv->phys_vec, dma_ranges,
					     priv->nr_ranges);
	if (ret)
		goto err_free_phys;

	kfree(dma_ranges);
	dma_ranges = NULL;

	ret = vfio_pci_dmabuf_export(vdev, priv, get_dma_buf.open_flags);
	if (ret)
		goto err_free_phys;
	/*
	 * dma_buf_fd() consumes the reference, when the file closes the dmabuf
	 * will be released.
	 */
	ret = dma_buf_fd(priv->dmabuf, get_dma_buf.open_flags);
	if (ret < 0)
		dma_buf_put(priv->dmabuf);

	return ret;

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
				   u64 phys_start, u64 req_len,
				   unsigned int res_index)
{
	struct vfio_pci_dma_buf *priv;
	unsigned long vma_pgoff = vma->vm_pgoff & (VFIO_PCI_OFFSET_MASK >> PAGE_SHIFT);
	char *bufname;
	int ret;

	priv = kzalloc_obj(*priv);
	if (!priv)
		return -ENOMEM;

	priv->phys_vec = kzalloc_obj(*priv->phys_vec);
	if (!priv->phys_vec) {
		ret = -ENOMEM;
		goto err_free_priv;
	}

	bufname = kasprintf(GFP_KERNEL, "%s:%s/%x",
			    dev_name(&vdev->vdev.device), pci_name(vdev->pdev),
			    res_index);

	if (!bufname) {
		ret = -ENOMEM;
		goto err_free_phys;
	}

	/*
	 * The DMABUF begins from the mmap()'s BAR offset, i.e. the
	 * start of the VMA corresponds to byte 0 of the DMABUF and
	 * byte (vma_pgoff << PAGE_SHIFT) of the BAR.
	 *
	 * vfio_pci_dma_buf_find_pfn() reverses this offset using
	 * vma_pgoff_adjust, so that ultimately a fault's offset from
	 * the start of the _VMA_ has a consistent usage whether the
	 * VMA originates from an mmap() of the VFIO device here or a
	 * direct DMABUF mmap().
	 */
	priv->vdev = vdev;
	priv->size = req_len;
	priv->nr_ranges = 1;
	priv->vma_pgoff_adjust = vma_pgoff;
	priv->provider = pcim_p2pdma_provider(vdev->pdev, res_index);
	if (!priv->provider) {
		ret = -EINVAL;
		goto err_free_name;
	}

	priv->phys_vec[0].paddr = phys_start + ((u64)vma_pgoff << PAGE_SHIFT);
	priv->phys_vec[0].len = priv->size;

	ret = vfio_pci_dmabuf_export(vdev, priv, O_CLOEXEC | O_RDWR);
	if (ret)
		goto err_free_name;

	/*
	 * Ownership of the DMABUF file transfers to the VMA so that
	 * other users can locate the DMABUF via a VA.  Ownership of
	 * the original VFIO device file being mmap()ed transfers to
	 * priv, and is put when the DMABUF is released.  This
	 * intentionally does not use get_file()/vma_set_file()
	 * because the references are already held, and ownership
	 * moves.
	 */
	priv->vfile = vma->vm_file;
	vma->vm_file = priv->dmabuf->file;
	vma->vm_private_data = priv;

	spin_lock(&priv->dmabuf->name_lock);
	kfree(priv->dmabuf->name);
	priv->dmabuf->name = bufname;
	spin_unlock(&priv->dmabuf->name_lock);

	return 0;

err_free_name:
	kfree(bufname);
err_free_phys:
	kfree(priv->phys_vec);
err_free_priv:
	kfree(priv);
	return ret;
}

void vfio_pci_dma_buf_move(struct vfio_pci_core_device *vdev, bool revoked)
{
	struct vfio_pci_dma_buf *priv;
	struct vfio_pci_dma_buf *tmp;

	/*
	 * Holding memory_lock ensures a racing VMA fault observes
	 * priv->revoked properly.
	 */
	lockdep_assert_held_write(&vdev->memory_lock);

	list_for_each_entry_safe(priv, tmp, &vdev->dmabufs, dmabufs_elm) {
		if (!get_file_active(&priv->dmabuf->file))
			continue;

		if (priv->revoked != revoked) {
			dma_resv_lock(priv->dmabuf->resv, NULL);
			if (revoked)
				priv->revoked = true;
			dma_buf_invalidate_mappings(priv->dmabuf);
			dma_resv_wait_timeout(priv->dmabuf->resv,
					      DMA_RESV_USAGE_BOOKKEEP, false,
					      MAX_SCHEDULE_TIMEOUT);
			dma_resv_unlock(priv->dmabuf->resv);
			if (revoked) {
				kref_put(&priv->kref, vfio_pci_dma_buf_done);
				wait_for_completion(&priv->comp);
				unmap_mapping_range(priv->dmabuf->file->f_mapping,
						    0, priv->size, true);
				/*
				 * Re-arm the registered kref reference and the
				 * completion so the post-revoke state matches the
				 * post-creation state.  An un-revoke followed by a
				 * new mapping needs the kref to be non-zero before
				 * kref_get(), and vfio_pci_dma_buf_cleanup()
				 * delegates its drain back through this revoke
				 * path on a possibly-already-revoked dma-buf.
				 */
				kref_init(&priv->kref);
				reinit_completion(&priv->comp);
			} else {
				dma_resv_lock(priv->dmabuf->resv, NULL);
				priv->revoked = false;
				dma_resv_unlock(priv->dmabuf->resv);
			}
		}
		fput(priv->dmabuf->file);
	}
}

void vfio_pci_dma_buf_cleanup(struct vfio_pci_core_device *vdev)
{
	struct vfio_pci_dma_buf *priv;
	struct vfio_pci_dma_buf *tmp;

	down_write(&vdev->memory_lock);

	/*
	 * Drain any active mappings via the revoke path.  The move is
	 * idempotent for dma-bufs already in the revoked state and
	 * leaves every priv with the kref re-armed and the completion
	 * ready, so cleanup itself does not need to participate in kref
	 * bookkeeping.
	 */
	vfio_pci_dma_buf_move(vdev, true);

	list_for_each_entry_safe(priv, tmp, &vdev->dmabufs, dmabufs_elm) {
		if (!get_file_active(&priv->dmabuf->file))
			continue;

		list_del_init(&priv->dmabufs_elm);
		priv->vdev = NULL;
		vfio_device_put_registration(&vdev->vdev);
		fput(priv->dmabuf->file);
	}
	up_write(&vdev->memory_lock);
}
