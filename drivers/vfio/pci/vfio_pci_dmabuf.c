// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2025, NVIDIA CORPORATION & AFFILIATES.
 */
#include <linux/dma-buf-mapping.h>
#include <linux/pci-p2pdma.h>
#include <linux/dma-resv.h>

#include "vfio_pci_priv.h"

MODULE_IMPORT_NS("DMA_BUF");

struct vfio_pci_dma_buf {
	struct dma_buf *dmabuf;
	struct vfio_pci_core_device *vdev;
	struct list_head dmabufs_elm;
	size_t size;
	struct dma_buf_phys_vec *phys_vec;
	struct p2pdma_provider *provider;
	u32 nr_ranges;
	u8 revoked : 1;
};

static int vfio_pci_dma_buf_pin(struct dma_buf_attachment *attachment)
{
	return -EOPNOTSUPP;
}

static void vfio_pci_dma_buf_unpin(struct dma_buf_attachment *attachment)
{
	/* Do nothing */
}

static int vfio_pci_dma_buf_attach(struct dma_buf *dmabuf,
				   struct dma_buf_attachment *attachment)
{
	struct vfio_pci_dma_buf *priv = dmabuf->priv;

	if (!attachment->peer2peer)
		return -EOPNOTSUPP;

	if (priv->revoked)
		return -ENODEV;

	return 0;
}

static struct sg_table *
vfio_pci_dma_buf_map(struct dma_buf_attachment *attachment,
		     enum dma_data_direction dir)
{
	struct vfio_pci_dma_buf *priv = attachment->dmabuf->priv;

	dma_resv_assert_held(priv->dmabuf->resv);

	if (priv->revoked)
		return ERR_PTR(-ENODEV);

	return dma_buf_phys_vec_to_sgt(attachment, priv->provider,
				       priv->phys_vec, priv->nr_ranges,
				       priv->size, dir);
}

static void vfio_pci_dma_buf_unmap(struct dma_buf_attachment *attachment,
				   struct sg_table *sgt,
				   enum dma_data_direction dir)
{
	dma_buf_free_sgt(attachment, sgt, dir);
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
	kfree(priv->phys_vec);
	kfree(priv);
}

static int vfio_pci_dma_buf_find_pfn(struct device *dev,
				     struct vfio_pci_dma_buf *vpdmabuf,
				     struct vm_area_struct *vma,
				     unsigned long address,
				     unsigned int order,
				     unsigned long *out_pfn)
{
	/*
	 * Given a VMA (start, end, pgoffs) and a fault address,
	 * search phys_vec[] to find the range representing the
	 * address's offset into the VMA (and so a PFN).
	 *
	 * The phys_vec ranges represent contiguous spans of VAs
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

	unsigned long rounded_page_addr = address & ~((PAGE_SIZE << order) - 1);
	unsigned long rounded_page_end = rounded_page_addr + (PAGE_SIZE << order);
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
		    buf_page_offset + (PAGE_SIZE << order) <= buf_offset + range_len) {
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
	 * represented by the (concatenated) ranges.  This can
	 * never happen because vfio_pci_dma_buf_mmap() checks that
	 * the VMA is <= the total size of the ranges.
	 *
	 * But if it does, force SIGBUS for the access, and warn.
	 */
	WARN_ONCE(1, "No range for addr 0x%lx, order %d: VMA 0x%lx-0x%lx pgoff 0x%lx, %d ranges, size 0x%lx\n",
		  address, order, vma->vm_start, vma->vm_end, vma->vm_pgoff,
		  vpdmabuf->nr_ranges, vpdmabuf->size);

	return -EFAULT;
}

static vm_fault_t vfio_pci_dma_buf_mmap_huge_fault(struct vm_fault *vmf,
						   unsigned int order)
{
	struct vm_area_struct *vma = vmf->vma;
	struct vfio_pci_dma_buf *priv = vma->vm_private_data;
	struct vfio_pci_core_device *vdev;
	unsigned long pfn;
	vm_fault_t ret = VM_FAULT_FALLBACK;

	vdev = READ_ONCE(priv->vdev);

	/*
	 * A fault for an existing mmap might occur after
	 * vfio_pci_dma_buf_cleanup() has revoked and destroyed the
	 * vdev's DMABUFs, and annulled vdev.  After creation, vdev is
	 * only ever written in cleanup.
	 */
	if (!vdev)
		return VM_FAULT_SIGBUS;

	int r = vfio_pci_dma_buf_find_pfn(&vdev->pdev->dev, priv, vma,
					  vmf->address, order, &pfn);

	if (r == 0) {
		scoped_guard(rwsem_read, &vdev->memory_lock) {
			/* Deal with the possibility of a fault racing
			 * with vfio_pci_dma_buf_move() revoking and
			 * then unmapping the buffer.  The
			 * revocation/unmap and status change occurs
			 * whilst holding memory_lock.
			 */
			if (priv->revoked)
				ret = VM_FAULT_SIGBUS;
			else
				ret = vfio_pci_vmf_insert_pfn(vdev, vmf, pfn, order);
		}
	} else if (r != -EAGAIN) {
		ret = VM_FAULT_SIGBUS;
	}

	dev_dbg_ratelimited(&vdev->pdev->dev,
			    "%s(order = %d) PFN 0x%lx, VA 0x%lx, pgoff 0x%lx: 0x%x\n",
			    __func__, order, pfn, vmf->address, vma->vm_pgoff, (unsigned int)ret);

	return ret;
}

static vm_fault_t vfio_pci_dma_buf_mmap_page_fault(struct vm_fault *vmf)
{
	return vfio_pci_dma_buf_mmap_huge_fault(vmf, 0);
}

static const struct vm_operations_struct vfio_pci_dma_buf_mmap_ops = {
	.fault = vfio_pci_dma_buf_mmap_page_fault,
#ifdef CONFIG_ARCH_SUPPORTS_HUGE_PFNMAP
	.huge_fault = vfio_pci_dma_buf_mmap_huge_fault,
#endif
};

static bool vfio_pci_dma_buf_is_mappable(struct dma_buf *dmabuf)
{
	struct vfio_pci_dma_buf *priv = dmabuf->priv;

	/*
	 * Sanity checks at mmap() time; alignment has already been
	 * asserted by validate_dmabuf_input().
	 *
	 * Although the revoked state is transient, refuse to map a
	 * revoked buffer to flag early that something odd is going
	 * on: for example, users should not be mmap()ing a buffer
	 * that's being moved [by a user-triggered activity].
	 */
	if (priv->revoked)
		return false;

	return true;
}

/*
 * Similar to vfio_pci_core_mmap() for a regular VFIO device fd, but
 * differs by pre-checks performed and ultimately the vm_ops installed.
 */
static int vfio_pci_dma_buf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct vfio_pci_dma_buf *priv = dmabuf->priv;
	u64 req_len, req_start;

	if (!vfio_pci_dma_buf_is_mappable(dmabuf))
		return -ENODEV;
	if ((vma->vm_flags & VM_SHARED) == 0)
		return -EINVAL;

	req_len = vma->vm_end - vma->vm_start;
	req_start = vma->vm_pgoff << PAGE_SHIFT;

	if (req_start + req_len > priv->size)
		return -EINVAL;

	vma->vm_private_data = priv;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vma->vm_page_prot = pgprot_decrypted(vma->vm_page_prot);

	/*
	 * See comments in vfio_pci_core_mmap() re VM_ALLOW_ANY_UNCACHED.
	 *
	 * FIXME: get mapping attributes from dmabuf?
	 */
	vm_flags_set(vma, VM_ALLOW_ANY_UNCACHED | VM_IO | VM_PFNMAP |
		     VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_ops = &vfio_pci_dma_buf_mmap_ops;

	return 0;
}

static const struct dma_buf_ops vfio_pci_dmabuf_ops = {
	.pin = vfio_pci_dma_buf_pin,
	.unpin = vfio_pci_dma_buf_unpin,
	.attach = vfio_pci_dma_buf_attach,
	.map_dma_buf = vfio_pci_dma_buf_map,
	.unmap_dma_buf = vfio_pci_dma_buf_unmap,
	.release = vfio_pci_dma_buf_release,
	.mmap = vfio_pci_dma_buf_mmap,
};

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
				 struct dma_buf_phys_vec *phys)
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

int vfio_pci_core_fill_phys_vec(struct dma_buf_phys_vec *phys_vec,
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
				  struct dma_buf_phys_vec *phys_vec,
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
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
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

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto err_free_ranges;
	}
	priv->phys_vec = kcalloc(get_dma_buf.nr_ranges, sizeof(*priv->phys_vec),
				 GFP_KERNEL);
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
	ret = vdev->pci_ops->get_dmabuf_phys(vdev, &priv->provider,
					     get_dma_buf.region_index,
					     priv->phys_vec, dma_ranges,
					     priv->nr_ranges);
	if (ret)
		goto err_free_phys;

	kfree(dma_ranges);
	dma_ranges = NULL;

	if (!vfio_device_try_get_registration(&vdev->vdev)) {
		ret = -ENODEV;
		goto err_free_phys;
	}

	exp_info.ops = &vfio_pci_dmabuf_ops;
	exp_info.size = priv->size;
	exp_info.flags = get_dma_buf.open_flags;
	exp_info.priv = priv;

	priv->dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(priv->dmabuf)) {
		ret = PTR_ERR(priv->dmabuf);
		goto err_dev_put;
	}

	/* dma_buf_put() now frees priv */
	INIT_LIST_HEAD(&priv->dmabufs_elm);
	down_write(&vdev->memory_lock);
	dma_resv_lock(priv->dmabuf->resv, NULL);
	priv->revoked = !__vfio_pci_memory_enabled(vdev);
	list_add_tail(&priv->dmabufs_elm, &vdev->dmabufs);
	dma_resv_unlock(priv->dmabuf->resv);
	up_write(&vdev->memory_lock);

	/*
	 * dma_buf_fd() consumes the reference, when the file closes the dmabuf
	 * will be released.
	 */
	ret = dma_buf_fd(priv->dmabuf, get_dma_buf.open_flags);
	if (ret < 0)
		goto err_dma_buf;
	return ret;

err_dma_buf:
	dma_buf_put(priv->dmabuf);
err_dev_put:
	vfio_device_put_registration(&vdev->vdev);
err_free_phys:
	kfree(priv->phys_vec);
err_free_priv:
	kfree(priv);
err_free_ranges:
	kfree(dma_ranges);
	return ret;
}

void vfio_pci_dma_buf_move(struct vfio_pci_core_device *vdev, bool revoked)
{
	struct vfio_pci_dma_buf *priv;
	struct vfio_pci_dma_buf *tmp;

	lockdep_assert_held_write(&vdev->memory_lock);
	/*
	 * Holding memory_lock ensures a racing
	 * vfio_pci_dma_buf_mmap_*_fault() observes priv->revoked
	 * properly.
	 */

	list_for_each_entry_safe(priv, tmp, &vdev->dmabufs, dmabufs_elm) {
		if (!get_file_active(&priv->dmabuf->file))
			continue;

		if (priv->revoked != revoked) {
			dma_resv_lock(priv->dmabuf->resv, NULL);
			priv->revoked = revoked;
			dma_buf_move_notify(priv->dmabuf);
			dma_resv_unlock(priv->dmabuf->resv);

			/*
			 * Unmap any possible userspace mappings for a
			 * now-revoked DMABUF:
			 */
			if (revoked)
				unmap_mapping_range(priv->dmabuf->file->f_mapping,
						    0, priv->size, 1);
		}
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
		priv->revoked = true;
		dma_buf_move_notify(priv->dmabuf);
		dma_resv_unlock(priv->dmabuf->resv);
		unmap_mapping_range(priv->dmabuf->file->f_mapping,
				    0, priv->size, 1);
		vfio_device_put_registration(&vdev->vdev);
		fput(priv->dmabuf->file);
	}
	up_write(&vdev->memory_lock);
}
