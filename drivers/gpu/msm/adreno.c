/* Copyright (c) 2002,2007-2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/ioctl.h>
#include <linux/sched.h>

#include <mach/socinfo.h>

#include "kgsl.h"
#include "kgsl_pwrscale.h"
#include "kgsl_cffdump.h"
#include "kgsl_sharedmem.h"

#include "adreno.h"
#include "adreno_pm4types.h"
#include "adreno_debugfs.h"
#include "adreno_postmortem.h"

#include "a200_reg.h"

#define DRIVER_VERSION_MAJOR   3
#define DRIVER_VERSION_MINOR   1

/* Adreno MH arbiter config*/
#define ADRENO_CFG_MHARB \
	(0x10 \
		| (0 << MH_ARBITER_CONFIG__SAME_PAGE_GRANULARITY__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__L1_ARB_ENABLE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__L1_ARB_HOLD_ENABLE__SHIFT) \
		| (0 << MH_ARBITER_CONFIG__L2_ARB_CONTROL__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__PAGE_SIZE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__TC_REORDER_ENABLE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__TC_ARB_HOLD_ENABLE__SHIFT) \
		| (0 << MH_ARBITER_CONFIG__IN_FLIGHT_LIMIT_ENABLE__SHIFT) \
		| (0x8 << MH_ARBITER_CONFIG__IN_FLIGHT_LIMIT__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__CP_CLNT_ENABLE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__VGT_CLNT_ENABLE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__TC_CLNT_ENABLE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__RB_CLNT_ENABLE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__PA_CLNT_ENABLE__SHIFT))

#define ADRENO_MMU_CONFIG						\
	(0x01								\
	 | (MMU_CONFIG << MH_MMU_CONFIG__RB_W_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__CP_W_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__CP_R0_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__CP_R1_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__CP_R2_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__CP_R3_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__CP_R4_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__VGT_R0_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__VGT_R1_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__TC_R_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__PA_W_CLNT_BEHAVIOR__SHIFT))

/* max msecs to wait for gpu to finish its operation(s) */
#define MAX_WAITGPU_SECS (HZ + HZ/2)

static const struct kgsl_functable adreno_functable;

static struct adreno_device device_3d0 = {
	.dev = {
		.name = DEVICE_3D0_NAME,
		.id = KGSL_DEVICE_3D0,
		.ver_major = DRIVER_VERSION_MAJOR,
		.ver_minor = DRIVER_VERSION_MINOR,
		.mmu = {
			.config = ADRENO_MMU_CONFIG,
			/* turn off memory protection unit by setting
			   acceptable physical address range to include
			   all pages. */
			.mpu_base = 0x00000000,
			.mpu_range =  0xFFFFF000,
		},
		.pwrctrl = {
			.regulator_name = "fs_gfx3d",
			.irq_name = KGSL_3D0_IRQ,
			.src_clk_name = "grp_src_clk",
		},
		.mutex = __MUTEX_INITIALIZER(device_3d0.dev.mutex),
		.state = KGSL_STATE_INIT,
		.active_cnt = 0,
		.iomemname = KGSL_3D0_REG_MEMORY,
		.ftbl = &adreno_functable,
		.display_off = {
#ifdef CONFIG_HAS_EARLYSUSPEND
			.level = EARLY_SUSPEND_LEVEL_STOP_DRAWING,
			.suspend = kgsl_early_suspend_driver,
			.resume = kgsl_late_resume_driver,
#endif
		},
	},
	.gmemspace = {
		.gpu_base = 0,
		.sizebytes = SZ_256K,
	},
	.pfp_fw = NULL,
	.pm4_fw = NULL,
	.mharb  = ADRENO_CFG_MHARB,
};

static int adreno_gmeminit(struct adreno_device *adreno_dev)
{
	struct kgsl_device *device = &adreno_dev->dev;
	union reg_rb_edram_info rb_edram_info;
	unsigned int gmem_size;
	unsigned int edram_value = 0;

	/* make sure edram range is aligned to size */
	BUG_ON(adreno_dev->gmemspace.gpu_base &
				(adreno_dev->gmemspace.sizebytes - 1));

	/* get edram_size value equivalent */
	gmem_size = (adreno_dev->gmemspace.sizebytes >> 14);
	while (gmem_size >>= 1)
		edram_value++;

	rb_edram_info.val = 0;

	rb_edram_info.f.edram_size = edram_value;
	if (!adreno_is_a220(adreno_dev))
		rb_edram_info.f.edram_mapping_mode = 0; /* EDRAM_MAP_UPPER */

	/* must be aligned to size */
	rb_edram_info.f.edram_range = (adreno_dev->gmemspace.gpu_base >> 14);

	adreno_regwrite(device, REG_RB_EDRAM_INFO, rb_edram_info.val);

	return 0;
}

static int adreno_gmemclose(struct kgsl_device *device)
{
	adreno_regwrite(device, REG_RB_EDRAM_INFO, 0x00000000);

	return 0;
}

irqreturn_t adreno_isr(int irq, void *data)
{
	irqreturn_t result;
	struct kgsl_device *device = data;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	result = adreno_dev->gpudev->irq_handler(adreno_dev);

	if (device->requested_state == KGSL_STATE_NONE) {
		if (device->pwrctrl.nap_allowed == true) {
			device->requested_state = KGSL_STATE_NAP;
			queue_work(device->work_queue, &device->idle_check_ws);
		} else if (device->pwrscale.policy != NULL) {
			queue_work(device->work_queue, &device->idle_check_ws);
		}
	}

	/* Reset the time-out in our idle timer */
	mod_timer(&device->idle_timer,
		jiffies + device->pwrctrl.interval_timeout);
	return result;
}

static int adreno_cleanup_pt(struct kgsl_device *device,
			struct kgsl_pagetable *pagetable)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_ringbuffer *rb = &adreno_dev->ringbuffer;

	kgsl_mmu_unmap(pagetable, &rb->buffer_desc);

	kgsl_mmu_unmap(pagetable, &rb->memptrs_desc);

	kgsl_mmu_unmap(pagetable, &device->memstore);

	kgsl_mmu_unmap(pagetable, &device->mmu.dummyspace);

	return 0;
}

static int adreno_setup_pt(struct kgsl_device *device,
			struct kgsl_pagetable *pagetable)
{
	int result = 0;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_ringbuffer *rb = &adreno_dev->ringbuffer;

	BUG_ON(rb->buffer_desc.physaddr == 0);
	BUG_ON(rb->memptrs_desc.physaddr == 0);
	BUG_ON(device->memstore.physaddr == 0);
#ifdef CONFIG_MSM_KGSL_MMU
	BUG_ON(device->mmu.dummyspace.physaddr == 0);
#endif
	result = kgsl_mmu_map_global(pagetable, &rb->buffer_desc,
				     GSL_PT_PAGE_RV);
	if (result)
		goto error;

	result = kgsl_mmu_map_global(pagetable, &rb->memptrs_desc,
				     GSL_PT_PAGE_RV | GSL_PT_PAGE_WV);
	if (result)
		goto unmap_buffer_desc;

	result = kgsl_mmu_map_global(pagetable, &device->memstore,
				     GSL_PT_PAGE_RV | GSL_PT_PAGE_WV);
	if (result)
		goto unmap_memptrs_desc;

	result = kgsl_mmu_map_global(pagetable, &device->mmu.dummyspace,
				     GSL_PT_PAGE_RV | GSL_PT_PAGE_WV);
	if (result)
		goto unmap_memstore_desc;

	return result;

unmap_memstore_desc:
	kgsl_mmu_unmap(pagetable, &device->memstore);

unmap_memptrs_desc:
	kgsl_mmu_unmap(pagetable, &rb->memptrs_desc);

unmap_buffer_desc:
	kgsl_mmu_unmap(pagetable, &rb->buffer_desc);

error:
	return result;
}

static void adreno_setstate(struct kgsl_device *device, uint32_t flags)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	unsigned int link[32];
	unsigned int *cmds = &link[0];
	int sizedwords = 0;
	unsigned int mh_mmu_invalidate = 0x00000003; /*invalidate all and tc */

	if (!kgsl_mmu_enabled() || !flags)
		return;

	/* If possible, then set the state via the command stream to avoid
	   a CPU idle.  Otherwise, use the default setstate which uses register
	   writes */

	if (adreno_dev->drawctxt_active) {
		if (flags & KGSL_MMUFLAGS_PTUPDATE) {
			/* wait for graphics pipe to be idle */
			*cmds++ = pm4_type3_packet(PM4_WAIT_FOR_IDLE, 1);
			*cmds++ = 0x00000000;

			/* set page table base */
			*cmds++ = pm4_type0_packet(MH_MMU_PT_BASE, 1);
			*cmds++ = device->mmu.hwpagetable->base.gpuaddr;
			sizedwords += 4;
		}

		if (flags & KGSL_MMUFLAGS_TLBFLUSH) {
			if (!(flags & KGSL_MMUFLAGS_PTUPDATE)) {
				*cmds++ = pm4_type3_packet(PM4_WAIT_FOR_IDLE,
								1);
				*cmds++ = 0x00000000;
				sizedwords += 2;
			}
			*cmds++ = pm4_type0_packet(MH_MMU_INVALIDATE, 1);
			*cmds++ = mh_mmu_invalidate;
			sizedwords += 2;
		}

		if (flags & KGSL_MMUFLAGS_PTUPDATE &&
			!adreno_is_a220(adreno_dev)) {
			/* HW workaround: to resolve MMU page fault interrupts
			* caused by the VGT.It prevents the CP PFP from filling
			* the VGT DMA request fifo too early,thereby ensuring
			* that the VGT will not fetch vertex/bin data until
			* after the page table base register has been updated.
			*
			* Two null DRAW_INDX_BIN packets are inserted right
			* after the page table base update, followed by a
			* wait for idle. The null packets will fill up the
			* VGT DMA request fifo and prevent any further
			* vertex/bin updates from occurring until the wait
			* has finished. */
			*cmds++ = pm4_type3_packet(PM4_SET_CONSTANT, 2);
			*cmds++ = (0x4 << 16) |
				(REG_PA_SU_SC_MODE_CNTL - 0x2000);
			*cmds++ = 0;	  /* disable faceness generation */
			*cmds++ = pm4_type3_packet(PM4_SET_BIN_BASE_OFFSET, 1);
			*cmds++ = device->mmu.dummyspace.gpuaddr;
			*cmds++ = pm4_type3_packet(PM4_DRAW_INDX_BIN, 6);
			*cmds++ = 0;	  /* viz query info */
			*cmds++ = 0x0003C004; /* draw indicator */
			*cmds++ = 0;	  /* bin base */
			*cmds++ = 3;	  /* bin size */
			*cmds++ = device->mmu.dummyspace.gpuaddr; /* dma base */
			*cmds++ = 6;	  /* dma size */
			*cmds++ = pm4_type3_packet(PM4_DRAW_INDX_BIN, 6);
			*cmds++ = 0;	  /* viz query info */
			*cmds++ = 0x0003C004; /* draw indicator */
			*cmds++ = 0;	  /* bin base */
			*cmds++ = 3;	  /* bin size */
			/* dma base */
			*cmds++ = device->mmu.dummyspace.gpuaddr;
			*cmds++ = 6;	  /* dma size */
			*cmds++ = pm4_type3_packet(PM4_WAIT_FOR_IDLE, 1);
			*cmds++ = 0x00000000;
			sizedwords += 21;
		}

		if (flags & (KGSL_MMUFLAGS_PTUPDATE | KGSL_MMUFLAGS_TLBFLUSH)) {
			*cmds++ = pm4_type3_packet(PM4_INVALIDATE_STATE, 1);
			*cmds++ = 0x7fff; /* invalidate all base pointers */
			sizedwords += 2;
		}

		adreno_ringbuffer_issuecmds(device, KGSL_CMD_FLAGS_PMODE,
					&link[0], sizedwords);
	} else
		kgsl_default_setstate(device, flags);
}

static unsigned int
adreno_getchipid(struct kgsl_device *device)
{
	unsigned int chipid = 0;
	unsigned int coreid, majorid, minorid, patchid, revid;

	adreno_regread(device, REG_RBBM_PERIPHID1, &coreid);
	adreno_regread(device, REG_RBBM_PERIPHID2, &majorid);
	adreno_regread(device, REG_RBBM_PATCH_RELEASE, &revid);

	/*
	* adreno 22x gpus are indicated by coreid 2,
	* but REG_RBBM_PERIPHID1 always contains 0 for this field
	*/
	if (cpu_is_msm8960() || cpu_is_msm8x60())
		chipid = 2 << 24;
	else
		chipid = (coreid & 0xF) << 24;

	chipid |= ((majorid >> 4) & 0xF) << 16;

	minorid = ((revid >> 0)  & 0xFF);

	patchid = ((revid >> 16) & 0xFF);

	/* 8x50 returns 0 for patch release, but it should be 1 */
	if (cpu_is_qsd8x50())
		patchid = 1;
	/* userspace isn't prepared to deal with patch id for these chips yet */
	else if (cpu_is_msm8960() || cpu_is_msm8x60())
		patchid = 0;

	chipid |= (minorid << 8) | patchid;

	return chipid;
}

/* all chipid fields are 8 bits wide so 256 won't occur in a real chipid */
#define DONT_CARE 256
static const struct {
	unsigned int core;
	unsigned int major;
	unsigned int minor;
	enum adreno_gpurev gpurev;
} gpurev_table[] = {
	/* major and minor may be DONT_CARE, but core must not be */
	{0, 2, DONT_CARE, ADRENO_REV_A200},
	{0, 1, 0, ADRENO_REV_A205},
	{2, 1, DONT_CARE, ADRENO_REV_A220},
	{2, 2, DONT_CARE, ADRENO_REV_A225},
};

static inline bool _rev_match(unsigned int id, unsigned int entry)
{
	return (entry == DONT_CARE || entry == id);
}
#undef DONT_CARE

static void
adreno_identify_gpu(struct adreno_device *adreno_dev)
{
	enum adreno_gpurev gpurev = ADRENO_REV_UNKNOWN;
	unsigned int i, core, major, minor;

	adreno_dev->chip_id = adreno_getchipid(&adreno_dev->dev);

	core = (adreno_dev->chip_id >> 24) & 0xff;
	major = (adreno_dev->chip_id >> 16) & 0xff;
	minor = (adreno_dev->chip_id >> 8) & 0xff;

	for (i = 0; i < ARRAY_SIZE(gpurev_table); i++) {
		if (core == gpurev_table[i].core &&
		    _rev_match(major, gpurev_table[i].major) &&
		    _rev_match(minor, gpurev_table[i].minor)) {
			gpurev = gpurev_table[i].gpurev;
			break;
		}
	}

	adreno_dev->gpurev = gpurev;
	adreno_dev->gpudev = &adreno_a2xx_gpudev;
}

static int __devinit
adreno_probe(struct platform_device *pdev)
{
	struct kgsl_device *device;
	struct adreno_device *adreno_dev;
	int status = -EINVAL;

	device = (struct kgsl_device *)pdev->id_entry->driver_data;
	adreno_dev = ADRENO_DEVICE(device);
	device->parentdev = &pdev->dev;

	init_completion(&device->recovery_gate);

	status = adreno_ringbuffer_init(device);
	if (status != 0)
		goto error;

	status = kgsl_device_platform_probe(device, adreno_isr);
	if (status)
		goto error_close_rb;

	adreno_debugfs_init(device);

	kgsl_pwrscale_init(device);
	kgsl_pwrscale_attach_policy(device, ADRENO_DEFAULT_PWRSCALE_POLICY);

	device->flags &= ~KGSL_FLAGS_SOFT_RESET;
	return 0;

error_close_rb:
	adreno_ringbuffer_close(&adreno_dev->ringbuffer);
error:
	device->parentdev = NULL;
	return status;
}

static int __devexit adreno_remove(struct platform_device *pdev)
{
	struct kgsl_device *device;
	struct adreno_device *adreno_dev;

	device = (struct kgsl_device *)pdev->id_entry->driver_data;
	adreno_dev = ADRENO_DEVICE(device);

	kgsl_pwrscale_detach_policy(device);
	kgsl_pwrscale_close(device);

	adreno_ringbuffer_close(&adreno_dev->ringbuffer);
	kgsl_device_platform_remove(device);

	return 0;
}

static int adreno_start(struct kgsl_device *device, unsigned int init_ram)
{
	int status = -EINVAL;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	int init_reftimestamp = 0x7fffffff;

	device->state = KGSL_STATE_INIT;
	device->requested_state = KGSL_STATE_NONE;

	/* Power up the device */
	kgsl_pwrctrl_enable(device);

	/* Identify the specific GPU */
	adreno_identify_gpu(adreno_dev);

	if (kgsl_mmu_start(device))
		goto error_clk_off;

	/*We need to make sure all blocks are powered up and clocked before
	*issuing a soft reset.  The overrides will then be turned off (set to 0)
	*/
	adreno_regwrite(device, REG_RBBM_PM_OVERRIDE1, 0xfffffffe);
	adreno_regwrite(device, REG_RBBM_PM_OVERRIDE2, 0xffffffff);

	/* Only reset CP block if all blocks have previously been reset */
	if (!(device->flags & KGSL_FLAGS_SOFT_RESET) ||
		!adreno_is_a220(adreno_dev)) {
		adreno_regwrite(device, REG_RBBM_SOFT_RESET, 0xFFFFFFFF);
		device->flags |= KGSL_FLAGS_SOFT_RESET;
	} else
		adreno_regwrite(device, REG_RBBM_SOFT_RESET, 0x00000001);

	/* The core is in an indeterminate state until the reset completes
	 * after 30ms.
	 */
	msleep(60);

	adreno_regwrite(device, REG_RBBM_SOFT_RESET, 0x00000000);

	adreno_regwrite(device, REG_RBBM_CNTL, 0x00004442);

	adreno_regwrite(device, REG_MH_ARBITER_CONFIG,
				adreno_dev->mharb);

	/* Remove 1k boundary check in z470 to avoid GPU hang.
	   Notice that, this solution won't work if both EBI and SMI are used */
	if (adreno_is_a220(adreno_dev)) {
		adreno_regwrite(device, REG_MH_CLNT_INTF_CTRL_CONFIG1,
				 0x00032f07);
	}

	adreno_regwrite(device, REG_SQ_VS_PROGRAM, 0x00000000);
	adreno_regwrite(device, REG_SQ_PS_PROGRAM, 0x00000000);

	adreno_regwrite(device, REG_RBBM_PM_OVERRIDE1, 0xFFFFFFFE);
	if (!adreno_is_a220(adreno_dev))
		adreno_regwrite(device, REG_RBBM_PM_OVERRIDE2, 0xFFF);
	else
		adreno_regwrite(device, REG_RBBM_PM_OVERRIDE2, 0x80);

	kgsl_sharedmem_writel(&device->memstore,
			      KGSL_DEVICE_MEMSTORE_OFFSET(ref_wait_ts),
			      init_reftimestamp);

	adreno_regwrite(device, REG_RBBM_DEBUG, 0x00080000);

	/* Make sure interrupts are disabled */

	adreno_regwrite(device, REG_RBBM_INT_CNTL, 0);
	adreno_regwrite(device, REG_CP_INT_CNTL, 0);
	adreno_regwrite(device, REG_SQ_INT_CNTL, 0);

	if (adreno_is_a220(adreno_dev))
		adreno_dev->gmemspace.sizebytes = SZ_512K;
	else
		adreno_dev->gmemspace.sizebytes = SZ_256K;
	adreno_gmeminit(adreno_dev);

	kgsl_pwrctrl_irq(device, KGSL_PWRFLAGS_ON);

	status = adreno_ringbuffer_start(&adreno_dev->ringbuffer, init_ram);
	if (status != 0)
		goto error_irq_off;
	
	KGSL_PWR_WARN(device, "device started status %d\n", status);

	mod_timer(&device->idle_timer, jiffies + FIRST_TIMEOUT);
	return status;

error_irq_off:
	kgsl_pwrctrl_irq(device, KGSL_PWRFLAGS_OFF);
error_clk_off:
	kgsl_pwrctrl_disable(device);
	kgsl_mmu_stop(device);

	return status;
}

static int adreno_stop(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	KGSL_PWR_WARN(device, "device stopped state %x\n", device->state);
	if (device->state == KGSL_STATE_SLEEP)
		return 0;
	
	adreno_dev->drawctxt_active = NULL;

	adreno_ringbuffer_stop(&adreno_dev->ringbuffer);

	adreno_gmemclose(device);

	kgsl_mmu_stop(device);

	kgsl_pwrctrl_irq(device, KGSL_PWRFLAGS_OFF);
	del_timer_sync(&device->idle_timer);

	/* Power down the device */
	kgsl_pwrctrl_disable(device);
	KGSL_PWR_WARN(device, "device stopped\n");

	return 0;
}

static int
adreno_recover_hang(struct kgsl_device *device)
{
	int ret;
	unsigned int *rb_buffer;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_ringbuffer *rb = &adreno_dev->ringbuffer;
	unsigned int timestamp;
	unsigned int num_rb_contents;
	unsigned int bad_context;
	unsigned int reftimestamp;
	unsigned int enable_ts;
	unsigned int soptimestamp;
	unsigned int eoptimestamp;
	struct adreno_context *drawctxt;

	KGSL_DRV_ERR(device, "Starting recovery from 3D GPU hang....\n");
	rb_buffer = vmalloc(rb->buffer_desc.size);
	if (!rb_buffer) {
		KGSL_MEM_ERR(device,
			"Failed to allocate memory for recovery: %x\n",
			rb->buffer_desc.size);
		return -ENOMEM;
	}
	/* Extract valid contents from rb which can stil be executed after
	 * hang */
	ret = adreno_ringbuffer_extract(rb, rb_buffer, &num_rb_contents);
	if (ret)
		goto done;
	timestamp = rb->timestamp;
	KGSL_DRV_ERR(device, "Last issued timestamp: %x\n", timestamp);
	kgsl_sharedmem_readl(&device->memstore, &bad_context,
				KGSL_DEVICE_MEMSTORE_OFFSET(current_context));
	kgsl_sharedmem_readl(&device->memstore, &reftimestamp,
				KGSL_DEVICE_MEMSTORE_OFFSET(ref_wait_ts));
	kgsl_sharedmem_readl(&device->memstore, &enable_ts,
				KGSL_DEVICE_MEMSTORE_OFFSET(ts_cmp_enable));
	kgsl_sharedmem_readl(&device->memstore, &soptimestamp,
				KGSL_DEVICE_MEMSTORE_OFFSET(soptimestamp));
	kgsl_sharedmem_readl(&device->memstore, &eoptimestamp,
				KGSL_DEVICE_MEMSTORE_OFFSET(eoptimestamp));
	/* Make sure memory is synchronized before restarting the GPU */
	mb();
	KGSL_CTXT_ERR(device,
		"Context that caused a GPU hang: %x\n", bad_context);
	/* restart device */
	ret = adreno_stop(device);
	if (ret)
		goto done;
	ret = adreno_start(device, true);
	if (ret)
		goto done;
	KGSL_DRV_ERR(device, "Device has been restarted after hang\n");
	/* Restore timestamp states */
	kgsl_sharedmem_writel(&device->memstore,
			KGSL_DEVICE_MEMSTORE_OFFSET(soptimestamp),
			soptimestamp);
	kgsl_sharedmem_writel(&device->memstore,
			KGSL_DEVICE_MEMSTORE_OFFSET(eoptimestamp),
			eoptimestamp);
	kgsl_sharedmem_writel(&device->memstore,
			KGSL_DEVICE_MEMSTORE_OFFSET(soptimestamp),
			soptimestamp);
	if (num_rb_contents) {
		kgsl_sharedmem_writel(&device->memstore,
			KGSL_DEVICE_MEMSTORE_OFFSET(ref_wait_ts),
			reftimestamp);
		kgsl_sharedmem_writel(&device->memstore,
			KGSL_DEVICE_MEMSTORE_OFFSET(ts_cmp_enable),
			enable_ts);
	}
	/* Make sure all writes are posted before the GPU reads them */
	wmb();
	/* Mark the invalid context so no more commands are accepted from
	 * that context */

	drawctxt = (struct adreno_context *) bad_context;

	KGSL_CTXT_ERR(device,
		"Context that caused a GPU hang: %x\n", bad_context);

	drawctxt->flags |= CTXT_FLAGS_GPU_HANG;

	/* Restore valid commands in ringbuffer */
	adreno_ringbuffer_restore(rb, rb_buffer, num_rb_contents);
	rb->timestamp = timestamp;
done:
	vfree(rb_buffer);
	return ret;
}

static int
adreno_dump_and_recover(struct kgsl_device *device)
{
	static int recovery;
	int result = -ETIMEDOUT;

	if (device->state == KGSL_STATE_HUNG)
		goto done;
	if (device->state == KGSL_STATE_DUMP_AND_RECOVER && !recovery) {
		mutex_unlock(&device->mutex);
		wait_for_completion(&device->recovery_gate);
		mutex_lock(&device->mutex);
		if (!(device->state & KGSL_STATE_HUNG))
			/* recovery success */
			result = 0;
	} else {
		INIT_COMPLETION(device->recovery_gate);
		/* Detected a hang - trigger an automatic dump */
		adreno_postmortem_dump(device, 0);
		if (!recovery) {
			recovery = 1;
			result = adreno_recover_hang(device);
			if (result) {
				device->state = KGSL_STATE_HUNG;
				//panic("[%s] GPU hang !!! (1)\n", __func__);
			}
			recovery = 0;
			complete_all(&device->recovery_gate);
		} else {
			KGSL_DRV_ERR(device,
				"Cannot recover from another hang while "
				"recovering from a hang\n");
			//panic("[%s] GPU hang !!! (2)\n", __func__);
		}

	}
done:
	return result;
}

static int adreno_getproperty(struct kgsl_device *device,
				enum kgsl_property_type type,
				void *value,
				unsigned int sizebytes)
{
	int status = -EINVAL;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	switch (type) {
	case KGSL_PROP_DEVICE_INFO:
		{
			struct kgsl_devinfo devinfo;

			if (sizebytes != sizeof(devinfo)) {
				status = -EINVAL;
				break;
			}

			memset(&devinfo, 0, sizeof(devinfo));
			devinfo.device_id = device->id+1;
			devinfo.chip_id = adreno_dev->chip_id;
			devinfo.mmu_enabled = kgsl_mmu_enabled();
			devinfo.gpu_id = adreno_dev->gpurev;
			devinfo.gmem_gpubaseaddr = adreno_dev->gmemspace.
					gpu_base;
			devinfo.gmem_sizebytes = adreno_dev->gmemspace.
					sizebytes;

			if (copy_to_user(value, &devinfo, sizeof(devinfo)) !=
					0) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	case KGSL_PROP_DEVICE_SHADOW:
		{
			struct kgsl_shadowprop shadowprop;

			if (sizebytes != sizeof(shadowprop)) {
				status = -EINVAL;
				break;
			}
			memset(&shadowprop, 0, sizeof(shadowprop));
			if (device->memstore.hostptr) {
				/*NOTE: with mmu enabled, gpuaddr doesn't mean
				 * anything to mmap().
				 */
				shadowprop.gpuaddr = device->memstore.physaddr;
				shadowprop.size = device->memstore.size;
				/* GSL needs this to be set, even if it
				   appears to be meaningless */
				shadowprop.flags = KGSL_FLAGS_INITIALIZED;
			}
			if (copy_to_user(value, &shadowprop,
				sizeof(shadowprop))) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	case KGSL_PROP_MMU_ENABLE:
		{
#ifdef CONFIG_MSM_KGSL_MMU
			int mmuProp = 1;
#else
			int mmuProp = 0;
#endif
			if (sizebytes != sizeof(int)) {
				status = -EINVAL;
				break;
			}
			if (copy_to_user(value, &mmuProp, sizeof(mmuProp))) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	case KGSL_PROP_INTERRUPT_WAITS:
		{
			int int_waits = 1;
			if (sizebytes != sizeof(int)) {
				status = -EINVAL;
				break;
			}
			if (copy_to_user(value, &int_waits, sizeof(int))) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	default:
		status = -EINVAL;
	}

	return status;
}

static inline void adreno_poke(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	adreno_regwrite(device, REG_CP_RB_WPTR, adreno_dev->ringbuffer.wptr);
}

/* Caller must hold the device mutex. */
int adreno_idle(struct kgsl_device *device, unsigned int timeout)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_ringbuffer *rb = &adreno_dev->ringbuffer;
	unsigned int rbbm_status;
	unsigned long wait_time = jiffies + MAX_WAITGPU_SECS;

	kgsl_cffdump_regpoll(device->id, REG_RBBM_STATUS << 2,
		0x00000000, 0x80000000);
	/* first, wait until the CP has consumed all the commands in
	 * the ring buffer
	 */
retry:
	if (rb->flags & KGSL_FLAGS_STARTED) {
		do {
			adreno_poke(device);
			GSL_RB_GET_READPTR(rb, &rb->rptr);
			if (time_after(jiffies, wait_time)) {
				KGSL_DRV_ERR(device, "rptr: %x, wptr: %x\n",
					rb->rptr, rb->wptr);
				goto err;
			}
		} while (rb->rptr != rb->wptr);
	}

	/* now, wait for the GPU to finish its operations */
	wait_time = jiffies + MAX_WAITGPU_SECS;
	if (device->state & (KGSL_STATE_SLEEP | KGSL_STATE_SUSPEND))
		KGSL_DRV_ERR(device, "device state %d\n", device->state);
	while (time_before(jiffies, wait_time)) {
		adreno_regread(device, REG_RBBM_STATUS, &rbbm_status);
		if (rbbm_status == 0x110)
			return 0;
	}

err:
	KGSL_DRV_ERR(device, "spun too long waiting for RB to idle\n");
	if (!adreno_dump_and_recover(device)) {
		wait_time = jiffies + MAX_WAITGPU_SECS;
		goto retry;
	}
	return -ETIMEDOUT;
}

static unsigned int adreno_isidle(struct kgsl_device *device)
{
	int status = false;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_ringbuffer *rb = &adreno_dev->ringbuffer;
	unsigned int rbbm_status;

	if (rb->flags & KGSL_FLAGS_STARTED) {
		/* Is the ring buffer is empty? */
		GSL_RB_GET_READPTR(rb, &rb->rptr);
		if (!device->active_cnt && (rb->rptr == rb->wptr)) {
			/* Is the core idle? */
			adreno_regread(device, REG_RBBM_STATUS,
					    &rbbm_status);
			if (rbbm_status == 0x110)
				status = true;
		}
	} else {
		KGSL_DRV_ERR(device, "ringbuffer not started\n");
		BUG();
	}
	return status;
}

/* Caller must hold the device mutex. */
static int adreno_suspend_context(struct kgsl_device *device)
{
	int status = 0;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	/* switch to NULL ctxt */
	if (adreno_dev->drawctxt_active != NULL) {
		adreno_drawctxt_switch(adreno_dev, NULL, 0);
		status = adreno_idle(device, KGSL_TIMEOUT_DEFAULT);
	}

	return status;
}

const struct kgsl_memdesc *adreno_find_region(struct kgsl_device *device,
                                               unsigned int pt_base,
                                               unsigned int gpuaddr,
                                               unsigned int size)
{
	struct kgsl_memdesc *result = NULL;
	struct kgsl_mem_entry *entry;
	struct kgsl_process_private *priv;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_ringbuffer *ringbuffer = &adreno_dev->ringbuffer;

	if (kgsl_gpuaddr_in_memdesc(&ringbuffer->buffer_desc, gpuaddr, size))
		return &ringbuffer->buffer_desc;

	if (kgsl_gpuaddr_in_memdesc(&ringbuffer->memptrs_desc, gpuaddr, size))
		return &ringbuffer->memptrs_desc;

	if (kgsl_gpuaddr_in_memdesc(&device->memstore, gpuaddr, size))
		return &device->memstore;

	mutex_lock(&kgsl_driver.process_mutex);
	list_for_each_entry(priv, &kgsl_driver.process_list, list) {
		if (pt_base != 0
			&& priv->pagetable
			&& priv->pagetable->base.gpuaddr != pt_base) {
			continue;
		}

		spin_lock(&priv->mem_lock);
		entry = kgsl_sharedmem_find_region(priv, gpuaddr,
						sizeof(unsigned int));
		if (entry) {
			result = &entry->memdesc;
			spin_unlock(&priv->mem_lock);
			mutex_unlock(&kgsl_driver.process_mutex);
			return result;
		}
		spin_unlock(&priv->mem_lock);
	}
	mutex_unlock(&kgsl_driver.process_mutex);

	BUG_ON(!mutex_is_locked(&device->mutex));
	list_for_each_entry(entry, &device->memqueue, list) {
		if (kgsl_gpuaddr_in_memdesc(&entry->memdesc, gpuaddr, size)) {
			result = &entry->memdesc;
			break;
		}

	}
	return result;
}

uint8_t *adreno_convertaddr(struct kgsl_device *device, unsigned int pt_base,
                           unsigned int gpuaddr, unsigned int size)
{
	const struct kgsl_memdesc *memdesc;

	memdesc = adreno_find_region(device, pt_base, gpuaddr, size);

	return memdesc ? kgsl_gpuaddr_to_vaddr(memdesc, gpuaddr) : NULL;
}
 
void adreno_regread(struct kgsl_device *device, unsigned int offsetwords,
				unsigned int *value)
{
	unsigned int *reg;
	BUG_ON(offsetwords*sizeof(uint32_t) >= device->regspace.sizebytes);
	reg = (unsigned int *)(device->regspace.mmio_virt_base
				+ (offsetwords << 2));

	if (!in_interrupt())
		kgsl_pre_hwaccess(device);

	/*ensure this read finishes before the next one.
	 * i.e. act like normal readl() */
	*value = __raw_readl(reg);
	rmb();
}

void adreno_regwrite(struct kgsl_device *device, unsigned int offsetwords,
				unsigned int value)
{
	unsigned int *reg;

	BUG_ON(offsetwords*sizeof(uint32_t) >= device->regspace.sizebytes);

	if (!in_interrupt())
		kgsl_pre_hwaccess(device);

	kgsl_cffdump_regwrite(device->id, offsetwords << 2, value);
	reg = (unsigned int *)(device->regspace.mmio_virt_base
				+ (offsetwords << 2));

	/*ensure previous writes post before this one,
	 * i.e. act like normal writel() */
	wmb();
	__raw_writel(value, reg);
}

static int kgsl_check_interrupt_timestamp(struct kgsl_device *device,
					unsigned int timestamp)
{
	int status;
	unsigned int ref_ts, enableflag;

	status = kgsl_check_timestamp(device, timestamp);
	if (!status) {
		mutex_lock(&device->mutex);
		kgsl_sharedmem_readl(&device->memstore, &enableflag,
			KGSL_DEVICE_MEMSTORE_OFFSET(ts_cmp_enable));
		mb();

		if (enableflag) {
			kgsl_sharedmem_readl(&device->memstore, &ref_ts,
				KGSL_DEVICE_MEMSTORE_OFFSET(ref_wait_ts));
			mb();
			if (timestamp_cmp(ref_ts, timestamp)) {
				kgsl_sharedmem_writel(&device->memstore,
				KGSL_DEVICE_MEMSTORE_OFFSET(ref_wait_ts),
				timestamp);
				wmb();
			}
		} else {
			unsigned int cmds[2];
			kgsl_sharedmem_writel(&device->memstore,
				KGSL_DEVICE_MEMSTORE_OFFSET(ref_wait_ts),
				timestamp);
			enableflag = 1;
			kgsl_sharedmem_writel(&device->memstore,
				KGSL_DEVICE_MEMSTORE_OFFSET(ts_cmp_enable),
				enableflag);
			wmb();
			/* submit a dummy packet so that even if all
			* commands upto timestamp get executed we will still
			* get an interrupt */
			cmds[0] = pm4_type3_packet(PM4_NOP, 1);
			cmds[1] = 0;
			adreno_ringbuffer_issuecmds(device, 0, &cmds[0], 2);
		}
		mutex_unlock(&device->mutex);
	}

	return status;
}

/*
 wait_io_event_interruptible_timeout checks for the exit condition before
 placing a process in wait q. For conditional interrupts we expect the
 process to already be in its wait q when its exit condition checking
 function is called.
*/
#define kgsl_wait_io_event_interruptible_timeout(wq, condition, timeout)\
({									\
	long __ret = timeout;						\
	__wait_io_event_interruptible_timeout(wq, condition, __ret);	\
	__ret;								\
})

/* MUST be called with the device mutex held */
static int adreno_waittimestamp(struct kgsl_device *device,
				unsigned int timestamp,
				unsigned int msecs)
{
	long status = 0;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	int retries;
	unsigned int msecs_first;
	unsigned int msecs_part;
#if defined(CONFIG_MACH_GEIM) || defined(CONFIG_MACH_TREBON)
	__s64 t1, t2;

	t1 = ktime_to_us(ktime_get());
#endif

	if (timestamp != adreno_dev->ringbuffer.timestamp &&
		timestamp_cmp(timestamp,
		adreno_dev->ringbuffer.timestamp)) {
		KGSL_DRV_ERR(device, "Cannot wait for invalid ts: %x, "
			"rb->timestamp: %x\n",
			timestamp, adreno_dev->ringbuffer.timestamp);
		status = -EINVAL;
		goto done;
	}
	/* Keep the first timeout as 100msecs before rewriting
	 * the WPTR. Less visible impact if the WPTR has not
	 * been updated properly.
	 */
	msecs_first = (msecs <= 100) ? ((msecs + 4) / 5) : 100;
	msecs_part = (msecs - msecs_first + 3) / 4;
	for (retries = 0; retries < 5; retries++) {
		if (!kgsl_check_timestamp(device, timestamp)) {
			adreno_poke(device);
			mutex_unlock(&device->mutex);
			/* We need to make sure that the process is
			 * placed in wait-q before its condition is called
			 */
			status = kgsl_wait_io_event_interruptible_timeout(
					device->wait_queue,
					kgsl_check_interrupt_timestamp(device,
						timestamp),
					msecs_to_jiffies(retries ?
						msecs_part : msecs_first));
			mutex_lock(&device->mutex);
			if (status > 0) {
				/*completed before the wait finished */
				status = 0;
				goto done;
			} else if (status < 0) {
				/*an error occurred*/
				goto done;				
			}
			/*this wait timed out*/
		}
	}
	if (!kgsl_check_timestamp(device, timestamp)) {
		status = -ETIMEDOUT;
		KGSL_DRV_ERR(device,
			"Device hang detected while waiting "
			"for timestamp: %x, last "
			"submitted(rb->timestamp): %x, wptr: "
			"%x\n", timestamp,
			adreno_dev->ringbuffer.timestamp,
			adreno_dev->ringbuffer.wptr);
		if (!adreno_dump_and_recover(device)) {
			/* wait for idle after recovery as the
			 * timestamp that this process wanted
			 * to wait on may be invalid */
			if (!adreno_idle(device,
				KGSL_TIMEOUT_DEFAULT))
				status = 0;
		}
	} else {
		status = 0;
	}

done:
#if defined(CONFIG_MACH_GEIM) || defined(CONFIG_MACH_TREBON)
	t2 = ktime_to_us(ktime_get());
	if ((t2 - t1) >= 1000000)
		KGSL_DRV_ERR(device, "adreno_waittimestamp took %lld", t2 - t1);
#endif
	return (int)status;
}

static unsigned int adreno_readtimestamp(struct kgsl_device *device,
			     enum kgsl_timestamp_type type)
{
	unsigned int timestamp = 0;

	if (type == KGSL_TIMESTAMP_CONSUMED)
		adreno_regread(device, REG_CP_TIMESTAMP, &timestamp);
	else if (type == KGSL_TIMESTAMP_RETIRED)
		kgsl_sharedmem_readl(&device->memstore, &timestamp,
				 KGSL_DEVICE_MEMSTORE_OFFSET(eoptimestamp));
	rmb();

	return timestamp;
}

static long adreno_ioctl(struct kgsl_device_private *dev_priv,
			      unsigned int cmd, void *data)
{
	int result = 0;
	struct kgsl_drawctxt_set_bin_base_offset *binbase;
	struct kgsl_context *context;

	switch (cmd) {
	case IOCTL_KGSL_DRAWCTXT_SET_BIN_BASE_OFFSET:
		binbase = data;

		context = kgsl_find_context(dev_priv, binbase->drawctxt_id);
		if (context) {
			adreno_drawctxt_set_bin_base_offset(
				dev_priv->device, context, binbase->offset);
		} else {
			result = -EINVAL;
			KGSL_DRV_ERR(dev_priv->device,
				"invalid drawctxt drawctxt_id %d "
				"device_id=%d\n",
				binbase->drawctxt_id, dev_priv->device->id);
		}
		break;

	default:
		KGSL_DRV_INFO(dev_priv->device,
			"invalid ioctl code %08x\n", cmd);
		result = -EINVAL;
		break;
	}
	return result;

}

static inline s64 adreno_ticks_to_us(u32 ticks, u32 gpu_freq)
{
	gpu_freq /= 1000000;
	return ticks / gpu_freq;
}

static void adreno_power_stats(struct kgsl_device *device,
				struct kgsl_power_stats *stats)
{
	unsigned int reg;
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;

	/* In order to calculate idle you have to have run the algorithm *
	 * at least once to get a start time. */
	if (pwr->time != 0) {
		s64 tmp;
		/* Stop the performance moniter and read the current *
		 * busy cycles. */
		adreno_regwrite(device,
			REG_CP_PERFMON_CNTL,
			REG_PERF_MODE_CNT |
			REG_PERF_STATE_FREEZE);
		adreno_regread(device, REG_RBBM_PERFCOUNTER1_LO, &reg);
		tmp = ktime_to_us(ktime_get());
		stats->total_time = tmp - pwr->time;
		pwr->time = tmp;
		stats->busy_time = adreno_ticks_to_us(reg, device->pwrctrl.
				pwrlevels[device->pwrctrl.active_pwrlevel].
				gpu_freq);

		adreno_regwrite(device,
			REG_CP_PERFMON_CNTL,
			REG_PERF_MODE_CNT |
			REG_PERF_STATE_RESET);
	} else {
		stats->total_time = 0;
		stats->busy_time = 0;
		pwr->time = ktime_to_us(ktime_get());
	}

	/* re-enable the performance moniters */
	adreno_regread(device, REG_RBBM_PM_OVERRIDE2, &reg);
	adreno_regwrite(device, REG_RBBM_PM_OVERRIDE2, (reg | 0x40));
	adreno_regwrite(device, REG_RBBM_PERFCOUNTER1_SELECT, 0x1);
	adreno_regwrite(device,
		REG_CP_PERFMON_CNTL,
		REG_PERF_MODE_CNT | REG_PERF_STATE_ENABLE);
}

void adreno_irqctrl(struct kgsl_device *device, int state)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	adreno_dev->gpudev->irq_control(adreno_dev, state);
}

static const struct kgsl_functable adreno_functable = {
	/* Mandatory functions */
	.regread = adreno_regread,
	.regwrite = adreno_regwrite,
	.idle = adreno_idle,
	.isidle = adreno_isidle,
	.suspend_context = adreno_suspend_context,
	.start = adreno_start,
	.stop = adreno_stop,
	.getproperty = adreno_getproperty,
	.waittimestamp = adreno_waittimestamp,
	.readtimestamp = adreno_readtimestamp,
	.issueibcmds = adreno_ringbuffer_issueibcmds,
	.ioctl = adreno_ioctl,
	.setup_pt = adreno_setup_pt,
	.cleanup_pt = adreno_cleanup_pt,
	.power_stats = adreno_power_stats,
	.irqctrl = adreno_irqctrl,
	/* Optional functions */
	.setstate = adreno_setstate,
	.drawctxt_create = adreno_drawctxt_create,
	.drawctxt_destroy = adreno_drawctxt_destroy,
};

static struct platform_device_id adreno_id_table[] = {
	{ DEVICE_3D0_NAME, (kernel_ulong_t)&device_3d0.dev, },
	{ },
};
MODULE_DEVICE_TABLE(platform, adreno_id_table);

static struct platform_driver adreno_platform_driver = {
	.probe = adreno_probe,
	.remove = __devexit_p(adreno_remove),
	.suspend = kgsl_suspend_driver,
	.resume = kgsl_resume_driver,
	.id_table = adreno_id_table,
	.driver = {
		.owner = THIS_MODULE,
		.name = DEVICE_3D_NAME,
		.pm = &kgsl_pm_ops,
	}
};

static int __init kgsl_3d_init(void)
{
	return platform_driver_register(&adreno_platform_driver);
}

static void __exit kgsl_3d_exit(void)
{
	platform_driver_unregister(&adreno_platform_driver);
}

module_init(kgsl_3d_init);
module_exit(kgsl_3d_exit);

MODULE_DESCRIPTION("3D Graphics driver");
MODULE_VERSION("1.2");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:kgsl_3d");
