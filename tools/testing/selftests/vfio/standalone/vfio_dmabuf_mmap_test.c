/*
 * Tests for VFIO DMABUF userspace mmap()
 *
 * As well as the basics (mmap() a BAR resource to userspace), test
 * shutdown/unmapping, aliasing, and DMABUF revocation scenarios.
 *
 * This test relies on being attached to a bochs-display device, which
 * has a simple known MMIO layout and a large plain-memory BAR.
 * Example invocation, assuming function 0000:00:03.0 is the target:
 *
 *  # lspci -n -s 00:03.0
 *  00:03.0 0380: 1234:1111 (rev 02)
 *
 *  # readlink /sys/bus/pci/devices/0000\:00\:03.0/iommu_group
 *  ../../../../../kernel/iommu_groups/3
 *
 *  (if there's a driver already attached)
 *  # echo 0000:00:03.0 > /sys/bus/pci/devices/0000:00:03.0/driver/unbind
 *
 *  (and, might need)
 *  # echo 1 > /sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts
 *
 *  Attach to VFIO:
 *  # echo 1234 1111 > /sys/bus/pci/drivers/vfio-pci/new_id
 *
 *  There should be only one thing in the group:
 *  # ls /sys/bus/pci/devices/0000:00:03.0/iommu_group/devices
 *
 *  Then given above an invocation would be:
 *  # this_test -r 0000:00:03.0 -g 3
 *
 * Such a device can be attached to a qemu-system machine using
 * '-device bochs-display'.  Adding the 'vgamem=64M' property can be
 * used to create a larger memory BAR (current default is 16MB).
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/dma-buf.h>
#include <linux/vfio.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define ROUND_UP(x, to) (((x) + (to) - 1) & ~((to) - 1))
#define MiB(x)		((x) * 1024ULL * 1024)


#define FAIL_IF(cond, msg...)                  \
	do {                                   \
		if (cond) {                    \
			printf("\n\nFAIL:\t"); \
			printf(msg);           \
			exit(1);               \
		}                              \
	} while (0)

static int vfio_setup(int groupnr, char *rid_str,
		      struct vfio_region_info *out_mappable_regions,
		      int nr_regions, int *out_nr_regions, int *out_vfio_cfd,
		      int *out_vfio_devfd)
{
	/* Create a new container, add group to it, open device, read
	 * resource, reset, etc.  Based on the example code in
	 * Documentation/driver-api/vfio.rst
	 */

	int container = open("/dev/vfio/vfio", O_RDWR);

	int r = ioctl(container, VFIO_GET_API_VERSION);

	if (r != VFIO_API_VERSION) {
		/* Unknown API version */
		printf("-E- Unknown API ver %d\n", r);
		return 1;
	}

	if (ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) != 1) {
		printf("-E- Doesn't support type 1\n");
		return 1;
	}

	char devpath[PATH_MAX];

	snprintf(devpath, PATH_MAX - 1, "/dev/vfio/%d", groupnr);
	/* Open the group */
	int group = open(devpath, O_RDWR);

	if (group < 0) {
		printf("-E- Can't open VFIO device (group %d)\n", groupnr);
		return 1;
	}

	/* Test the group is viable and available */
	struct vfio_group_status group_status = { .argsz = sizeof(
							  group_status) };

	if (ioctl(group, VFIO_GROUP_GET_STATUS, &group_status)) {
		perror("-E- Can't get group status");
		return 1;
	}

	if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		/* Group is not viable (ie, not all devices bound for vfio) */
		printf("-E- Group %d is not viable!\n", groupnr);
		return 1;
	}

	/* Add the group to the container */
	if (ioctl(group, VFIO_GROUP_SET_CONTAINER, &container)) {
		perror("-E- Can't add group to container");
		return 1;
	}

	/* Enable the IOMMU model we want */
	if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU)) {
		perror("-E- Can't select T1");
		return 1;
	}

	/* Get addition IOMMU info */
	struct vfio_iommu_type1_info iommu_info = { .argsz = sizeof(
							    iommu_info) };

	if (ioctl(container, VFIO_IOMMU_GET_INFO, &iommu_info)) {
		perror("-E- Can't get VFIO info");
		return 1;
	}

	/* Get a file descriptor for the device */
	int device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, rid_str);

	if (device < 0) {
		perror("-E- Can't get device fd");
		return 1;
	}
	close(group);

	/* Test and setup the device */
	struct vfio_device_info device_info = { .argsz = sizeof(device_info) };

	if (ioctl(device, VFIO_DEVICE_GET_INFO, &device_info)) {
		perror("-E- Can't get device info");
		return 1;
	}
	printf("-i- %d device regions, flags 0x%x\n", device_info.num_regions,
	       device_info.flags);

	/* Regions are BAR0-5 then ROM, config, VGA */
	int out_region = 0;

	for (int i = 0; i < device_info.num_regions; i++) {
		struct vfio_region_info reg = { .argsz = sizeof(reg) };

		reg.index = i;
		if (ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &reg)) {
			/* We expect EINVAL if there's no VGA region */
			printf("-W- Region %d: ERROR %d\n", i, errno);
		} else {
			printf("-i- Region %d(%d): flags 0x%08x (%c%c%c), cap_offs %d, size 0x%llx, offs 0x%llx\n",
			       i, reg.index, reg.flags,
			       (reg.flags & VFIO_REGION_INFO_FLAG_READ) ? 'R' :
									  '-',
			       (reg.flags & VFIO_REGION_INFO_FLAG_WRITE) ? 'W' :
									   '-',
			       (reg.flags & VFIO_REGION_INFO_FLAG_MMAP) ? 'M' :
									  '-',
			       reg.cap_offset, reg.size, reg.offset);

			if ((reg.flags & VFIO_REGION_INFO_FLAG_MMAP) &&
			    (out_region < nr_regions))
				out_mappable_regions[out_region++] = reg;
		}
	}
	*out_nr_regions = out_region;

#ifdef THERE_ARE_NO_IRQS_YET
	for (i = 0; i < device_info.num_irqs; i++) {
		struct vfio_irq_info irq = { .argsz = sizeof(irq) };

		irq.index = i;

		ioctl(device, VFIO_DEVICE_GET_IRQ_INFO, &irq);

		/* Setup IRQs... eventfds, VFIO_DEVICE_SET_IRQS */
	}
#endif
	/* Gratuitous device reset and go... */
	if (ioctl(device, VFIO_DEVICE_RESET))
		perror("-W- Can't reset device (continuing)");

	*out_vfio_cfd = container;
	*out_vfio_devfd = device;

	return 0;
}

static int vfio_feature_present(int dev_fd, uint32_t feature)
{
	struct vfio_device_feature probeftr = {
		.argsz = sizeof(probeftr),
		.flags = VFIO_DEVICE_FEATURE_PROBE |
			 feature,
	};
	return ioctl(dev_fd, VFIO_DEVICE_FEATURE, &probeftr) == 0;
}

static int vfio_create_dmabuf(int dev_fd, uint32_t region, uint64_t offset,
			      uint64_t length)
{
	uint64_t ftrbuf
		[ROUND_UP(sizeof(struct vfio_device_feature) +
				  sizeof(struct vfio_device_feature_dma_buf) +
				  sizeof(struct vfio_region_dma_range),
			  8) /
		 8];

	struct vfio_device_feature *f = (struct vfio_device_feature *)ftrbuf;
	struct vfio_device_feature_dma_buf *db =
		(struct vfio_device_feature_dma_buf *)f->data;
	struct vfio_region_dma_range *range =
		(struct vfio_region_dma_range *)db->dma_ranges;

	f->argsz = sizeof(ftrbuf);
	f->flags = VFIO_DEVICE_FEATURE_GET | VFIO_DEVICE_FEATURE_DMA_BUF;
	db->region_index = region;
	db->open_flags = O_RDWR | O_CLOEXEC;
	db->flags = 0;
	db->nr_ranges = 1;
	range->offset = offset;
	range->length = length;

	return ioctl(dev_fd, VFIO_DEVICE_FEATURE, &ftrbuf);
}

/* As above, but try multiple ranges in one dmabuf */
static int vfio_create_dmabuf_dual(int dev_fd, uint32_t region,
				   uint64_t offset0, uint64_t length0,
				   uint64_t offset1, uint64_t length1)
{
	uint64_t ftrbuf
		[ROUND_UP(sizeof(struct vfio_device_feature) +
				  sizeof(struct vfio_device_feature_dma_buf) +
				  (sizeof(struct vfio_region_dma_range) * 2),
			  8) /
		 8];

	struct vfio_device_feature *f = (struct vfio_device_feature *)ftrbuf;
	struct vfio_device_feature_dma_buf *db =
		(struct vfio_device_feature_dma_buf *)f->data;
	struct vfio_region_dma_range *range =
		(struct vfio_region_dma_range *)db->dma_ranges;

	f->argsz = sizeof(ftrbuf);
	f->flags = VFIO_DEVICE_FEATURE_GET | VFIO_DEVICE_FEATURE_DMA_BUF;
	db->region_index = region;
	db->open_flags = O_RDWR | O_CLOEXEC;
	db->flags = 0;
	db->nr_ranges = 2;
	range[0].offset = offset0;
	range[0].length = length0;
	range[1].offset = offset1;
	range[1].length = length1;

	return ioctl(dev_fd, VFIO_DEVICE_FEATURE, &ftrbuf);
}

static volatile uint32_t *mmap_resource_aligned(size_t size,
						unsigned long align, int fd,
						unsigned long offset)
{
	void *v;

	if (align <= getpagesize()) {
		v = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			 offset);
		FAIL_IF(v == MAP_FAILED,
			"Can't mmap fd %d (size 0x%lx, offset 0x%lx), %d\n", fd,
			size, offset, errno);
	} else {
		size_t resv_size = size + align;
		void *resv =
			mmap(0, resv_size, 0, MAP_PRIVATE | MAP_ANON, -1, 0);
		FAIL_IF(resv == MAP_FAILED,
			"Can't mmap reservation, size 0x%lx, %d\n", resv_size,
			errno);

		uintptr_t pos = ((uintptr_t)resv + (align - 1)) & ~(align - 1);

		v = mmap((void *)pos, size, PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_FIXED, fd, offset);
		FAIL_IF(v == MAP_FAILED,
			"Can't mmap-fixed fd %d (size 0x%lx, offset 0x%lx), %d\n",
			fd, size, offset, errno);
		madvise((void *)v, size, MADV_HUGEPAGE);

		/* Tidy */
		if (pos > (uintptr_t)resv)
			munmap(resv, pos - (uintptr_t)resv);
		if (pos + size < (uintptr_t)resv + resv_size)
			munmap((void *)pos + size,
			       (uintptr_t)resv + resv_size - (pos + size));
	}

	return (volatile uint32_t *)v;
}

static volatile uint32_t *mmap_resource(size_t size, int fd,
					unsigned long offset)
{
	return mmap_resource_aligned(size, getpagesize(), fd, offset);
}

#define BOCHS_VBE_OFFSET	0x500
#define BOCHS_VBE_ID		(BOCHS_VBE_OFFSET + 0)
#define  BOCHS_VBE_ID5 		0xb0c5
#define BOCHS_VBE_VIRT_HEIGHT	(BOCHS_VBE_OFFSET + 7*2)
#define BOCHS_QEXT_OFFSET	0x600
#define BOCHS_QEXT_BYTEORDER	(BOCHS_QEXT_OFFSET + 4)
#define BOCHS_QEXT_LE		0x1e1e1e1e
#define BOCHS_QEXT_BE		0xbebebebe

static void check_mmio(volatile uint32_t *base)
{
	volatile uint16_t *shortbase = (volatile uint16_t *)base;
	uint32_t v;

	printf("-i- MMIO check: ");

	/* Trivial MMIO: check an ID word */
	v = shortbase[BOCHS_VBE_ID / 2];
	FAIL_IF(v != BOCHS_VBE_ID5,
		"Magic value %08x incorrect, BAR map bad?\n", v);

	/* Write the endian field with a) a correct value (it changes)
	 * then b) a broken value (it doesn't change).  This helps to
	 * differentiate the region from plain memory.
	 */
	base[BOCHS_QEXT_BYTEORDER / 4] = BOCHS_QEXT_LE;
	v = base[BOCHS_QEXT_BYTEORDER / 4];
	FAIL_IF(v != BOCHS_QEXT_LE, "Magic value %08x bad (should be %08x)\n",
		v, BOCHS_QEXT_LE);
	base[BOCHS_QEXT_BYTEORDER / 4] = BOCHS_QEXT_BE;
	v = base[BOCHS_QEXT_BYTEORDER / 4];
	FAIL_IF(v != BOCHS_QEXT_BE, "Magic value %08x bad (should be %08x)\n",
		v, BOCHS_QEXT_BE);
	base[BOCHS_QEXT_BYTEORDER / 4] = (BOCHS_QEXT_BE - 7); /* Rejected! */
	v = base[BOCHS_QEXT_BYTEORDER / 4];
	FAIL_IF(v != BOCHS_QEXT_BE, "Magic value %08x bad (should be %08x)\n",
		v, BOCHS_QEXT_BE);
	printf("OK\n");
}

/* Verify that base & alternate appear to point to aliasing pages/the
 * same registers
 */
static void check_mmio_alias(volatile uint32_t *base, volatile uint32_t *alternate)
{
	volatile uint16_t *shortbase = (volatile uint16_t *)base;
	volatile uint16_t *shortalt = (volatile uint16_t *)alternate;
	uint16_t r = random();
	uint16_t v;

	printf("-i- MMIO alias check: ");

	shortbase[BOCHS_VBE_VIRT_HEIGHT / 2] = r;
	v = shortalt[BOCHS_VBE_VIRT_HEIGHT / 2];
	FAIL_IF(v != r,
		"Alias initial MMIO value %08x bad (should be %08x)\n", v, r);
	r = ~r;
	shortalt[BOCHS_VBE_VIRT_HEIGHT / 2] = r;
	v = shortbase[BOCHS_VBE_VIRT_HEIGHT / 2];
	FAIL_IF(v != r,
		"Alias MMIO value %08x bad (should be %08x)\n", v, r);
	printf("OK\n");
}

static int revoke_dmabuf(int dev_fd, int dmabuf_fd)
{
	uint64_t ftrbuf
		[ROUND_UP(sizeof(struct vfio_device_feature) +
				  sizeof(struct vfio_device_feature_dma_buf_revoke),
			  8) /
		 8];

	struct vfio_device_feature *f = (struct vfio_device_feature *)ftrbuf;
	struct vfio_device_feature_dma_buf_revoke *dbr =
		(struct vfio_device_feature_dma_buf_revoke *)f->data;

	f->argsz = sizeof(ftrbuf);
	f->flags = VFIO_DEVICE_FEATURE_SET | VFIO_DEVICE_FEATURE_DMA_BUF_REVOKE;
	dbr->dmabuf_fd = dmabuf_fd;

	return ioctl(dev_fd, VFIO_DEVICE_FEATURE, &ftrbuf);
}

static jmp_buf jmpbuf;

static void sighandler(int sig)
{
	printf("*** Signal %d ***\n", sig);
	siglongjmp(jmpbuf, sig);
}

static void setup_signals(void)
{
	struct sigaction sa = {
		.sa_handler = sighandler,
		.sa_flags = 0,
	};

	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);
}

#define BAR_REGION_REGS 1
#define BAR_REGION_MEM 0

static int vfio_dmabuf_test_membar(int dev_fd, unsigned long long membar_region_offset,
				   size_t membar_region_size, int membar_region_index)
{
	uint32_t v;

	printf("\nTEST: Second BAR: test overlapping+offset DMABUF\n");

	/* Make sure we can mmap via device_fd with a non-zero offset */
	unsigned long vfio_membar_offset = 0x30000;  // FIXME ASSERT

	if (membar_region_size < 2*vfio_membar_offset) {
		printf("-W- SKIPPING: Memory BAR size 0x%lx is not big enough (want at least 0x%lx)\n",
		       membar_region_size, vfio_membar_offset);
		return 1;
	}

	volatile uint32_t *vfio_membar = mmap_resource(
		membar_region_size - vfio_membar_offset, dev_fd,
		membar_region_offset + vfio_membar_offset);

	printf("-i- Mapped VFIO reg BAR +0x%lx at %p+0x%lx\n", vfio_membar_offset, vfio_membar,
	       membar_region_size - vfio_membar_offset);

	printf("-i- Mem BAR DMABUF: offset 0x%llx, size 0x%lx\n",
	       membar_region_offset, membar_region_size);
	int membar_db_fd =
		vfio_create_dmabuf(dev_fd, membar_region_index, 0, membar_region_size);

	FAIL_IF(membar_db_fd < 0, "Can't create DMABUF, %d\n", errno);

	volatile uint32_t *db_membar = mmap_resource_aligned(
		membar_region_size, MiB(32), membar_db_fd, 0);
	printf("-i- Mapped DMABUF mem BAR at %p+0x%lx\n", db_membar,
	       membar_region_size);

	/* Init with known values */
	for (unsigned long i = 0; i < (membar_region_size);
	     i += getpagesize())
		db_membar[i / 4] = 0xca77face ^ i;

	v = db_membar[0];
	FAIL_IF(v != 0xca77face,
		"DB mem BAR read: Magic value %08x incorrect\n", v);
	printf("-i- DB mem BAR read: Magic: 0x%08x\n", v);

	v = vfio_membar[0];
	FAIL_IF(v != (0xca77face ^ vfio_membar_offset),
		"VFIO mem BAR read: Magic value %08x incorrect\n", v);
	printf("-i- VFIO mem BAR read: Magic: 0x%08x\n", v);

	/* munmap a page in the middle of the VFIO mem BAR to split the VMA */
	void *membar_hole = (void *)vfio_membar + getpagesize()*3;
	size_t membar_hole_size = getpagesize();
	printf("-i- Splitting VFIO VMA (unmapping %p +0x%lx)\n", membar_hole, membar_hole_size);
	munmap(membar_hole, membar_hole_size);

	unsigned long vfio_membar_end_offs = membar_region_size - vfio_membar_offset - 0x4000;
	v = vfio_membar[vfio_membar_end_offs / 4];
	printf("-i- End offs 0x%lx: read 0x%08x\n", vfio_membar_end_offs, v);
	FAIL_IF(v != (0xca77face ^ (vfio_membar_offset + vfio_membar_end_offs)),
		"VFIO mem BAR read 2: Magic value %08x incorrect\n", v);
	printf("-i- VFIO mem BAR read 2: Magic: 0x%08x\n", v);

	munmap((void *)vfio_membar, membar_region_size - vfio_membar_offset);

	if (sigsetjmp(jmpbuf, 1) == 0) {
		v = vfio_membar[vfio_membar_end_offs / 4];
		FAIL_IF(true,
			"Expecting VFIO bar map to fault at end after unmap!\n");
	}
	printf("-i- End of VFIO bar map inaccessible (expected)\n");

	close(membar_db_fd);

	/* TEST: Overlap/aliasing; map same BAR with a range
	 * offset > 0.  Also test disjoint/multi-range DMABUFs
	 * by creating a second range.  This appears as one
	 * contiguous VA range mapped to a first BAR range
	 * (starting from range0_offset), then skipping a few
	 * physical pages, then a second range (starting at
	 * range1_offset).
	 */
	unsigned long range0_offset = getpagesize() * 3;
	unsigned long range1_skip_pages = 5;
	unsigned long range1_skip = getpagesize() * range1_skip_pages;
	unsigned long range_size =
		(membar_region_size - range0_offset - range1_skip) / 2;
	unsigned long range1_offset =
		range0_offset + range_size + range1_skip;
	unsigned long map_size = range_size * 2;

	printf("\nTEST: Second BAR aliasing mapping, two ranges size 0x%lx:\n\t\t0x%lx-0x%lx, 0x%lx-0x%lx\n",
	       range_size, range0_offset, range0_offset + range_size,
	       range1_offset, range1_offset + range_size);

	int membar_2_db_fd = vfio_create_dmabuf_dual(
		dev_fd, membar_region_index, range0_offset, range_size, range1_offset,
		range_size);
	FAIL_IF(membar_2_db_fd < 0, "Can't create DMABUF, %d\n", errno);

	volatile uint32_t *db_membar_2 =
		mmap_resource(map_size, membar_2_db_fd, 0);

	printf("-i- Mapped DMABUF mem BAR alias at %p+0x%lx\n",
	       db_membar_2, map_size);
	FAIL_IF(db_membar_2[0] != db_membar[range0_offset / 4],
		"slice2 value mismatch\n");

	db_membar[(range0_offset + 4) / 4] = 0xfacef00d;
	/* Check we can see the value written above at +offset
	 * from offset 0 of this mapping (since the DMABUF
	 * itself is offsetted):
	 */
	v = db_membar_2[4 / 4];
	FAIL_IF(v != 0xfacef00d,
		"DB mem BAR alias read: Magic value %08x incorrect\n",
		v);
	printf("-i- DB mem BAR alias read: Magic 0x%08x, OK\n", v);

	/* Read back the known values across the two
	 * sub-ranges of the db_membar_2 mapping, accounting for
	 * the physical pages skipped between them
	 */
	for (unsigned long i = 0; i < range_size; i += getpagesize()) {
		unsigned long t = i + range0_offset;
		uint32_t want = (0xca77face ^ t);

		v = db_membar_2[i / 4];
		FAIL_IF(v != want,
			"Expected %08x (got %08x) from range0 +%08lx (real %08lx)\n",
			want, v, i, t);
	}
	for (unsigned long i = range_size; i < (range_size * 2);
	     i += getpagesize()) {
		unsigned long t = i + range1_offset - range_size;
		uint32_t want = (0xca77face ^ t);

		v = db_membar_2[i / 4];
		FAIL_IF(v != want,
			"Expected %08x (got %08x) from range1 +%08lx (real %08lx)\n",
			want, v, i, t);
	}

	printf("\nTEST: Third BAR aliasing mapping, testing mmap() non-zero offset:\n");

	unsigned long smaller = range_size - 0x1000;
	volatile uint32_t *db_membar_3 = mmap_resource_aligned(
		smaller, MiB(32), membar_2_db_fd, range_size);
	printf("-i- Mapped DMABUF mem BAR range 1 alias at %p+0x%lx\n",
	       db_membar_3, smaller);

	for (unsigned long i = 0; i < smaller; i += getpagesize()) {
		unsigned long t = i + range1_offset;
		uint32_t want = (0xca77face ^ t);

		v = db_membar_3[i / 4];
		FAIL_IF(v != want,
			"Expected %08x (got %08x) from 3rd range1 +%08lx (real %08lx)\n",
			want, v, i, t);
	}
	printf("-i- mmap offset OK\n");

	/* TODO: If we can observe hugepages (mechanically,
	 * rather than human reading debug), we can test
	 * interesting alignment cases for the PFN search:
	 *
	 * - Deny hugepages at start/end of an mmap() that
	 *   starts/ends at non-HP-aligned addresses
	 *   (e.g. first pages are small, middle is fully
	 *   aligned in VA and PFN so 2M, and buffer finishes
	 *   before 2M boundary, so last pages are small).
	 *
	 * - Everything aligned nicely except the mmap() size
	 *   is <2MB, so hugepage denied due to straddling
	 *   end.
	 *
	 * - Buffer offsets into BAR not aligned, so no huge
	 *   mappings even if mmap() is perfectly aligned.
	 */

	/* Check that access after DMABUF fd close still works
	 * (VMA still holds refcount, obvs!)
	 */
	close(membar_2_db_fd);
	if (sigsetjmp(jmpbuf, 1) == 0)
		v = db_membar_2[0x4 / 4];
	else
		FAIL_IF(true,
			"Expecting original DMABUF mapping to still work!\n");
	printf("-i- DB mem BAR alias read 2: Magic 0x%08x, OK\n", v);
	printf("-i- Offset check OK\n");

	return 0;
}

static int vfio_dmabuf_test(int groupnr, char *rid_str)
{
	/* Only expecting one or two regions */
	struct vfio_region_info bar_region[2];
	int num_regions = 0;
	int container_fd, dev_fd;
	int r = vfio_setup(groupnr, rid_str, &bar_region[0], 2, &num_regions,
			   &container_fd, &dev_fd);

	FAIL_IF(r, "VFIO setup failed\n");

	printf("-i- Container fd %d, device fd %d, and got %d mappable regions\n",
	       container_fd, dev_fd, num_regions);

	setup_signals();

	/* If no DMABUF feature, still do "something" with the regular VFIO BAR */
	if (!vfio_feature_present(dev_fd, VFIO_DEVICE_FEATURE_DMA_BUF)) {
		printf("\n-W- No VFIO_DEVICE_FEATURE_DMA_BUF, skipping all other tests\n");
		printf("\nTEST: Map the regular VFIO BAR\n");
		volatile uint32_t *vfio_regbar =
			mmap_resource(bar_region[BAR_REGION_REGS].size, dev_fd, bar_region[BAR_REGION_REGS].offset);
		printf("-i- Mapped VFIO reg BAR at %p+0x%llx\n", vfio_regbar,
		       bar_region[BAR_REGION_REGS].size);
		check_mmio(vfio_regbar);
		munmap((void *)vfio_regbar, bar_region[BAR_REGION_REGS].size);
		return 2; /* Different to regular failure */
	}

	for (int i = 0; i < num_regions; i++) {
		printf("Region %d: Index 0x%x, offset 0x%llx +0x%llx\n", i,
		       bar_region[i].index, bar_region[i].offset,
		       bar_region[i].size);
	}
	////////////////////////////////////////////////////////////////////////////////

	/* Real basics:	 create DMABUF, and mmap it, and access MMIO through it.
	 * Do this for 2nd BAR if present, too (just plain memory).
	 */
	printf("\nTEST: Create DMABUF, map it\n");
	int bar_db_fd = vfio_create_dmabuf(dev_fd, /* region */ bar_region[BAR_REGION_REGS].index,
					   /* offset */ 0, bar_region[BAR_REGION_REGS].size);
	FAIL_IF(bar_db_fd < 0, "Can't create DMABUF, %d\n", errno);

	volatile uint32_t *db_regbar =
		mmap_resource(bar_region[BAR_REGION_REGS].size, bar_db_fd, 0);

	printf("-i- Mapped DMABUF reg BAR at %p+0x%llx\n", db_regbar,
	       bar_region[BAR_REGION_REGS].size);
	check_mmio(db_regbar);

	/* Basic mapping tests:  can't map off the end */
	void *dbbar_fail = mmap(0, bar_region[BAR_REGION_REGS].size + 0x1000, PROT_READ | PROT_WRITE,
				MAP_SHARED, bar_db_fd, 0);
	FAIL_IF(dbbar_fail != MAP_FAILED, "DB mmap(): too large should fail\n");

	dbbar_fail = mmap(0, bar_region[BAR_REGION_REGS].size, PROT_READ | PROT_WRITE,
			  MAP_SHARED, bar_db_fd, 0x1000);
	FAIL_IF(dbbar_fail != MAP_FAILED, "DB mmap(): offset off end should fail\n");
	printf("-i- DB mmap() span checks OK\n");


	/* TEST: Map the traditional VFIO one _second_; it should still work. */
	printf("\nTEST: Map the regular VFIO BAR\n");
	volatile uint32_t *vfio_regbar =
		mmap_resource(bar_region[BAR_REGION_REGS].size, dev_fd, bar_region[BAR_REGION_REGS].offset);

	printf("-i- Mapped VFIO reg BAR at %p+0x%llx\n", vfio_regbar,
	       bar_region[BAR_REGION_REGS].size);
	check_mmio(vfio_regbar);

	void *vfio_regbar_fail = mmap(0, bar_region[BAR_REGION_REGS].size + 0x1000, PROT_READ | PROT_WRITE,
				      MAP_SHARED, dev_fd, bar_region[BAR_REGION_REGS].offset);
	FAIL_IF(vfio_regbar_fail != MAP_FAILED, "VFIO mmap(): too large should fail\n");

	vfio_regbar_fail = mmap(0, bar_region[BAR_REGION_REGS].size, PROT_READ | PROT_WRITE,
				MAP_SHARED, dev_fd, bar_region[BAR_REGION_REGS].offset + 0x1000);
	FAIL_IF(vfio_regbar_fail != MAP_FAILED, "VFIO mmap(): offset off end should fail\n");
	printf("-i- VFIO mmap() span checks OK\n");


	/* Test plan:
	 *
	 * - Revoke the first DMABUF, check for fault
	 * - Check VFIO BAR access still works
	 * - Revoke first DMABUF fd again: -EBADFD
	 * - create new DMABUF for same (previously-revoked) region: accessible
	 *
	 * - Create overlapping DMABUFs: map success, maps alias OK
	 * - Create a second mapping of the second DMABUF, maps alias OK
	 * - Destroy one by revoking through a dup()ed fd: check mapping revoked
	 * - Check original is still accessible
	 *
	 * If we have a larger (>4K of accessible stuff!) second BAR resource:
	 * - Map it, create an overlapping alias with offset != 0
	 * - Check alias/offset is sane
	 *
	 * Last:
	 * - close container_fd and dev_fd: check DMABUF mapping revoked
	 * - try revoking a non-DMABUF fd: -EINVAL
	 */

	printf("\nTEST: Revocation of first DMABUF\n");
	r = revoke_dmabuf(dev_fd, bar_db_fd);
	FAIL_IF(r != 0, "Can't revoke: %d\n", errno);

	if (sigsetjmp(jmpbuf, 1) == 0) {
		// Try an access: expect BOOM
		check_mmio(db_regbar);
		FAIL_IF(true, "Expecting fault after revoke!\n");
	}
	printf("-i- Revoked OK\n");

	printf("\nTEST: Access through VFIO-mapped region still works\n");
	if (sigsetjmp(jmpbuf, 1) == 0)
		check_mmio(vfio_regbar);
	else
		FAIL_IF(true, "Expecting VFIO-mapped BAR to still work!\n");

	printf("\nTEST: Double-revoke\n");
	r = revoke_dmabuf(dev_fd, bar_db_fd);
	FAIL_IF(r != -1 || errno != EBADFD,
		"Expecting 2nd revoke to give EBADFD, got %d errno %d\n", r,
		errno);
	printf("-i- Correctly failed second revoke\n");

	printf("\nTEST: Can't mmap() revoked DMABUF\n");
	void *dbfail = mmap(0, bar_region[BAR_REGION_REGS].size, PROT_READ | PROT_WRITE,
			    MAP_SHARED, bar_db_fd, 0);
	FAIL_IF(dbfail != MAP_FAILED, "mmap() should fail\n");
	printf("-i- OK\n");

	printf("\nTEST: Recreate new DMABUF for previously-revoked region\n");
	int bar_db_fd_2 = vfio_create_dmabuf(
		dev_fd, /* region */ bar_region[BAR_REGION_REGS].index, /* offset */ 0, bar_region[BAR_REGION_REGS].size);
	FAIL_IF(bar_db_fd_2 < 0, "Can't create DMABUF, %d\n", errno);

	volatile uint32_t *db_regbar_2 =
		mmap_resource(bar_region[BAR_REGION_REGS].size, bar_db_fd_2, 0);

	printf("-i- Mapped 2nd DMABUF reg BAR at %p+0x%llx\n", db_regbar_2,
	       bar_region[BAR_REGION_REGS].size);
	check_mmio(db_regbar_2);

	munmap((void *)db_regbar, bar_region[BAR_REGION_REGS].size);
	close(bar_db_fd);

	printf("\nTEST: Create aliasing/overlapping DMABUF\n");
	int bar_db_fd_3 = vfio_create_dmabuf(
		dev_fd, /* region */ bar_region[BAR_REGION_REGS].index, /* offset */ 0, bar_region[BAR_REGION_REGS].size);
	FAIL_IF(bar_db_fd_3 < 0, "Can't create DMABUF, %d\n", errno);

	volatile uint32_t *db_regbar_3 =
		mmap_resource(bar_region[BAR_REGION_REGS].size, bar_db_fd_3, 0);

	printf("-i- Mapped 3rd DMABUF reg BAR at %p+0x%llx\n", db_regbar_3,
	       bar_region[BAR_REGION_REGS].size);
	check_mmio(db_regbar_3);

	/* Basic aliasing check: Write value through 2nd, read back through 3rd */
	check_mmio_alias(db_regbar_2, db_regbar_3);
	printf("-i- Aliasing DMABUF OK\n");

	printf("\nTEST: Create a double-mapping of DMABUF\n");
	/* Create another mmap of the existing aliasing DMABUF fd */
	volatile uint32_t *db_regbar_3_2 =
		mmap_resource(bar_region[BAR_REGION_REGS].size, bar_db_fd_3, 0);

	printf("-i- Mapped 3rd DMABUF reg BAR _again_ at %p+0x%llx\n", db_regbar_3_2,
	       bar_region[BAR_REGION_REGS].size);
	check_mmio(db_regbar_3_2);
	check_mmio_alias(db_regbar_3, db_regbar_3_2);

	printf("\nTEST: revoke aliasing DMABUF through dup()ed fd\n");
	int dup_dbfd3 = dup(bar_db_fd_3);

	r = revoke_dmabuf(dev_fd, dup_dbfd3);
	FAIL_IF(r != 0, "Can't revoke: %d\n", errno);

	/* Both of the mmap()s made should now be gone */
	if (sigsetjmp(jmpbuf, 1) == 0) {
		check_mmio(db_regbar_3);
		FAIL_IF(true, "Expecting fault on 1st mmap after revoke!\n");
	}

	if (sigsetjmp(jmpbuf, 1) == 0) {
		check_mmio(db_regbar_3_2);
		FAIL_IF(true, "Expecting fault on 2nd mmap after revoke!\n");
	}
	printf("-i- Both aliasing DMABUF mappings revoked OK\n");

	/* Close the dup'ed fd.  Leave the revoked DB bar_db_fd_3
	 * open, so that VFIO device_fd cleanup cleans up an
	 * already-revoked object.
	 */
	close(dup_dbfd3);
	munmap((void *)db_regbar_3, bar_region[BAR_REGION_REGS].size);
	munmap((void *)db_regbar_3_2, bar_region[BAR_REGION_REGS].size);

	/* And finally, although the aliasing DMABUF is gone, access
	 * through the original one should still work:
	 */
	if (sigsetjmp(jmpbuf, 1) == 0)
		check_mmio(db_regbar_2);
	else
		FAIL_IF(true,
			"Expecting original DMABUF mapping to still work!\n");
	printf("-i- Aliasing DMABUF removal OK, original still accessible\n");


	if (num_regions >= 2) {
		/* If we're attached to a hacked/extended QEMU EDU device with
		 * a large memory mem BAR then we can test things like
		 * offsets/aliasing.
		 */
		vfio_dmabuf_test_membar(dev_fd,
					bar_region[BAR_REGION_MEM].offset,
					bar_region[BAR_REGION_MEM].size,
					bar_region[BAR_REGION_MEM].index);
	}

	printf("\nTEST: Shutdown: close VFIO container/device fds, check DMABUF gone\n");

	/* Final use of dev_fd: use it to try to revoke a non-DMABUF fd: */
	r = revoke_dmabuf(dev_fd, 1);
	FAIL_IF(r != -1 || errno != EINVAL,
		"Expecting revoke of stdout to give EINVAL, got %d errno %d\n",
		r, errno);
	printf("-i- Correctly failed final revoke\n");

	/* Closing all uses of dev_fd (including the VFIO BAR mmap()!)
	 * will revoke the DMABUF; even though the DMABUF fd might
	 * remain open, the mapping itself is zapped. Start with a
	 * plain close (before unmapping the VFIO BAR mapping):
	 */
	close(dev_fd);
	close(container_fd);
	printf("-i- VFIO fds closed\n");

	if (sigsetjmp(jmpbuf, 1) == 0)
		check_mmio(db_regbar_2);
	else
		FAIL_IF(true,
			"Expecting DMABUF mapping to still work if VFIO mapping still live!\n");

	if (sigsetjmp(jmpbuf, 1) == 0)
		check_mmio(vfio_regbar);
	else
		FAIL_IF(true,
			"Expecting VFIO BAR mapping to still work after fd close!\n");

	munmap((void *)vfio_regbar, bar_region[BAR_REGION_REGS].size);
	printf("-i- VFIO BAR unmapped\n");

	/* The final reference via VFIO should now be gone, and the
	 * DMABUF should now be destroyed.  The mapping of it should
	 * be inaccessible:
	 */
	if (sigsetjmp(jmpbuf, 1) == 0) {
		check_mmio(db_regbar_2);
		FAIL_IF(true,
			"Expecting DMABUF mapping to fault after VFIO fd shutdown!\n");
	}
	printf("-i- DMABUF mappings inaccessible (expected)\n");

	/* Ensure we can't mmap() DMABUF for closed device */
	void *dbfail2 = mmap(0, bar_region[BAR_REGION_MEM].size, PROT_READ | PROT_WRITE,
			     MAP_SHARED, bar_db_fd_2, 0);
	FAIL_IF(dbfail2 != MAP_FAILED, "mmap() should fail\n");
	printf("-i- Can't mmap DMABUF for closed device, OK\n");

	munmap((void *)db_regbar_2, bar_region[BAR_REGION_REGS].size);
	close(bar_db_fd_2);

	printf("\nPASS\n");

	return 0;
}

static void usage(char *me)
{
	printf("Usage:\t%s -g <group_number> -r <RID/BDF>\n"
	       "\n"
	       "\t\tGroup is found via device path, e.g. cat /sys/bus/pci/devices/0000:03:1d.0/iommu_group\n"
	       "\t\tRID is of the form 0000:03:1d.0\n"
	       "\n",
	       me);
}

int main(int argc, char *argv[])
{
	/* Get args: IOMMU group and BDF/path */
	int groupnr = -1;
	char *rid_str = NULL;
	int arg;

	while ((arg = getopt(argc, argv, "g:r:h")) != -1) {
		switch (arg) {
		case 'g':
			groupnr = atoi(optarg);
			break;

		case 'r':
			rid_str = strdup(optarg);
			break;
		case 'h':
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (rid_str == NULL || groupnr == -1) {
		usage(argv[0]);
		return 1;
	}

	printf("-i- Using group number %d, RID '%s'\n", groupnr, rid_str);

	return vfio_dmabuf_test(groupnr, rid_str);
}
