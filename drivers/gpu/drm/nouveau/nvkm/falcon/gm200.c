/*
 * Copyright 2022 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "priv.h"

#include <core/memory.h>
#include <subdev/mc.h>
#include <subdev/timer.h>

void
gm200_flcn_tracepc(struct nvkm_falcon *falcon)
{
	u32 sctl = nvkm_falcon_rd32(falcon, 0x240);
	u32 tidx = nvkm_falcon_rd32(falcon, 0x148);
	int nr = (tidx & 0x00ff0000) >> 16, sp, ip;

	FLCN_ERR(falcon, "TRACEPC SCTL %08x TIDX %08x", sctl, tidx);
	for (sp = 0; sp < nr; sp++) {
		nvkm_falcon_wr32(falcon, 0x148, sp);
		ip = nvkm_falcon_rd32(falcon, 0x14c);
		FLCN_ERR(falcon, "TRACEPC: %08x", ip);
	}
}

static void
gm200_flcn_pio_dmem_rd(struct nvkm_falcon *falcon, u8 port, const u8 *img, int len)
{
	while (len >= 4) {
		*(u32 *)img = nvkm_falcon_rd32(falcon, 0x1c4 + (port * 8));
		img += 4;
		len -= 4;
	}

	/* Sigh.  Tegra PMU FW's init message... */
	if (len) {
		u32 data = nvkm_falcon_rd32(falcon, 0x1c4 + (port * 8));

		while (len--) {
			*(u8 *)img++ = data & 0xff;
			data >>= 8;
		}
	}
}

static void
gm200_flcn_pio_dmem_rd_init(struct nvkm_falcon *falcon, u8 port, u32 dmem_base)
{
	nvkm_falcon_wr32(falcon, 0x1c0 + (port * 8), BIT(25) | dmem_base);
}

static void
gm200_flcn_pio_dmem_wr(struct nvkm_falcon *falcon, u8 port, const u8 *img, int len, u16 tag)
{
	while (len >= 4) {
		nvkm_falcon_wr32(falcon, 0x1c4 + (port * 8), *(u32 *)img);
		img += 4;
		len -= 4;
	}

	WARN_ON(len);
}

static void
gm200_flcn_pio_dmem_wr_init(struct nvkm_falcon *falcon, u8 port, bool sec, u32 dmem_base)
{
	nvkm_falcon_wr32(falcon, 0x1c0 + (port * 8), BIT(24) | dmem_base);
}

const struct nvkm_falcon_func_pio
gm200_flcn_dmem_pio = {
	.min = 1,
	.max = 0x100,
	.wr_init = gm200_flcn_pio_dmem_wr_init,
	.wr = gm200_flcn_pio_dmem_wr,
	.rd_init = gm200_flcn_pio_dmem_rd_init,
	.rd = gm200_flcn_pio_dmem_rd,
};

static void
gm200_flcn_pio_imem_wr_init(struct nvkm_falcon *falcon, u8 port, bool sec, u32 imem_base)
{
	nvkm_falcon_wr32(falcon, 0x180 + (port * 0x10), (sec ? BIT(28) : 0) | BIT(24) | imem_base);
}

static void
gm200_flcn_pio_imem_wr(struct nvkm_falcon *falcon, u8 port, const u8 *img, int len, u16 tag)
{
	nvkm_falcon_wr32(falcon, 0x188 + (port * 0x10), tag);
	while (len >= 4) {
		nvkm_falcon_wr32(falcon, 0x184 + (port * 0x10), *(u32 *)img);
		img += 4;
		len -= 4;
	}
}

const struct nvkm_falcon_func_pio
gm200_flcn_imem_pio = {
	.min = 0x100,
	.max = 0x100,
	.wr_init = gm200_flcn_pio_imem_wr_init,
	.wr = gm200_flcn_pio_imem_wr,
};

int
gm200_flcn_bind_stat(struct nvkm_falcon *falcon, bool intr)
{
	if (intr && !(nvkm_falcon_rd32(falcon, 0x008) & 0x00000008))
		return -1;

	return (nvkm_falcon_rd32(falcon, 0x0dc) & 0x00007000) >> 12;
}

void
gm200_flcn_bind_inst(struct nvkm_falcon *falcon, int target, u64 addr)
{
	nvkm_falcon_mask(falcon, 0x604, 0x00000007, 0x00000000); /* DMAIDX_VIRT */
	nvkm_falcon_wr32(falcon, 0x054, (1 << 30) | (target << 28) | (addr >> 12));
	nvkm_falcon_mask(falcon, 0x090, 0x00010000, 0x00010000);
	nvkm_falcon_mask(falcon, 0x0a4, 0x00000008, 0x00000008);
}

int
gm200_flcn_reset_wait_mem_scrubbing(struct nvkm_falcon *falcon)
{
	nvkm_falcon_mask(falcon, 0x040, 0x00000000, 0x00000000);

	if (nvkm_msec(falcon->owner->device, 10,
		if (!(nvkm_falcon_rd32(falcon, 0x10c) & 0x00000006))
			break;
	) < 0)
		return -ETIMEDOUT;

	return 0;
}

int
gm200_flcn_enable(struct nvkm_falcon *falcon)
{
	struct nvkm_device *device = falcon->owner->device;
	int ret;

	if (falcon->func->reset_eng) {
		ret = falcon->func->reset_eng(falcon);
		if (ret)
			return ret;
	}

	if (falcon->func->select) {
		ret = falcon->func->select(falcon);
		if (ret)
			return ret;
	}

	if (falcon->func->reset_pmc)
		nvkm_mc_enable(device, falcon->owner->type, falcon->owner->inst);

	ret = falcon->func->reset_wait_mem_scrubbing(falcon);
	if (ret)
		return ret;

	nvkm_falcon_wr32(falcon, 0x084, nvkm_rd32(device, 0x000000));
	return 0;
}

int
gm200_flcn_disable(struct nvkm_falcon *falcon)
{
	struct nvkm_device *device = falcon->owner->device;
	int ret;

	if (falcon->func->select) {
		ret = falcon->func->select(falcon);
		if (ret)
			return ret;
	}

	nvkm_falcon_mask(falcon, 0x048, 0x00000003, 0x00000000);
	nvkm_falcon_wr32(falcon, 0x014, 0xffffffff);

	if (falcon->func->reset_pmc) {
		if (falcon->func->reset_prep) {
			ret = falcon->func->reset_prep(falcon);
			if (ret)
				return ret;
		}

		nvkm_mc_disable(device, falcon->owner->type, falcon->owner->inst);
	}

	if (falcon->func->reset_eng) {
		ret = falcon->func->reset_eng(falcon);
		if (ret)
			return ret;
	}

	return 0;
}

int
gm200_flcn_fw_boot(struct nvkm_falcon_fw *fw, u32 *pmbox0, u32 *pmbox1, u32 mbox0_ok, u32 irqsclr)
{
	struct nvkm_falcon *falcon = fw->falcon;
	u32 mbox0, mbox1;
	int ret = 0;

	nvkm_falcon_wr32(falcon, 0x040, pmbox0 ? *pmbox0 : 0xcafebeef);
	if (pmbox1)
		nvkm_falcon_wr32(falcon, 0x044, *pmbox1);

	nvkm_falcon_wr32(falcon, 0x104, fw->boot_addr);
	nvkm_falcon_wr32(falcon, 0x100, 0x00000002);
	{	/* XXX gv11b bring-up diagnostics: PC via ICD (RREG idx 21) */
		u32 pc[3], cpu[3];
		int i;
		for (i = 0; i < 3; i++) {
			cpu[i] = nvkm_falcon_rd32(falcon, 0x100);
			nvkm_falcon_wr32(falcon, 0x200, 0x0000150e);
			pc[i] = nvkm_falcon_rd32(falcon, 0x20c);
			if (i == 0) udelay(5); else mdelay(50);
		}
		FLCN_DBG(falcon, "start: bootvec %x cpuctl %08x/%08x/%08x pc %08x/%08x/%08x mbox0 %08x",
			 fw->boot_addr, cpu[0], cpu[1], cpu[2], pc[0], pc[1], pc[2],
			 nvkm_falcon_rd32(falcon, 0x040));
	}

	if (nvkm_msec(falcon->owner->device, 2000,
		if (nvkm_falcon_rd32(falcon, 0x100) & 0x00000010)
			break;
	) < 0)
		ret = -ETIMEDOUT;

	mbox0 = nvkm_falcon_rd32(falcon, 0x040);
	mbox1 = nvkm_falcon_rd32(falcon, 0x044);
	{	/* XXX gv11b bring-up diagnostics: PC trace buffer */
		u32 idx = nvkm_falcon_rd32(falcon, 0x148), n = (idx >> 16) & 0xff, i;
		struct nvkm_device *device = falcon->owner->device;
		FLCN_DBG(falcon, "trace: idx %08x", idx);
		FLCN_DBG(falcon, "flush: 100c10 %08x 100c40 %08x 70000 %08x 70004 %08x 70010 %08x "
			 "70014 %08x wpr 100cd4 %08x 100cd8 %08x 100cdc %08x 100ce0 %08x 100ce4 %08x",
			 nvkm_rd32(device, 0x100c10), nvkm_rd32(device, 0x100c40),
			 nvkm_rd32(device, 0x070000), nvkm_rd32(device, 0x070004),
			 nvkm_rd32(device, 0x070010), nvkm_rd32(device, 0x070014),
			 nvkm_rd32(device, 0x100cd4), nvkm_rd32(device, 0x100cd8),
			 nvkm_rd32(device, 0x100cdc), nvkm_rd32(device, 0x100ce0),
			 nvkm_rd32(device, 0x100ce4));
		FLCN_DBG(falcon, "pmc: boot0 %08x enable 200 %08x dev_enable 600 %08x intr 100 %08x 104 %08x "
			 "fecs cpuctl %08x gpccs cpuctl %08x",
			 nvkm_rd32(device, 0x000000), nvkm_rd32(device, 0x000200),
			 nvkm_rd32(device, 0x000600), nvkm_rd32(device, 0x000100),
			 nvkm_rd32(device, 0x000104), nvkm_rd32(device, 0x409100),
			 nvkm_rd32(device, 0x41a100));
		{	/* XXX diff against NVIDIA R35 pre-ACR register state */
			static const struct { u32 addr, val; } nvref[] = {
#include "gv11b_nvref.inc"
			};
			int j, diff = 0;
			for (j = 0; j < ARRAY_SIZE(nvref); j++) {
				u32 v = nvkm_rd32(device, nvref[j].addr);
				if (v != nvref[j].val) {
					FLCN_DBG(falcon, "nvref %06x nv %08x nouveau %08x",
						 nvref[j].addr, nvref[j].val, v);
					diff++;
				}
			}
			FLCN_DBG(falcon, "nvref: %d of %zu differ", diff, ARRAY_SIZE(nvref));
		}
		FLCN_DBG(falcon, "dma: trfbase %08x moffs %08x cmd %08x fboffs %08x base1 %08x "
			 "fbif_ctl %08x fbif_stat %08x mbox1 %08x",
			 nvkm_falcon_rd32(falcon, 0x110), nvkm_falcon_rd32(falcon, 0x114),
			 nvkm_falcon_rd32(falcon, 0x118), nvkm_falcon_rd32(falcon, 0x11c),
			 nvkm_falcon_rd32(falcon, 0x128), nvkm_falcon_rd32(falcon, 0x624),
			 nvkm_falcon_rd32(falcon, 0x620), nvkm_falcon_rd32(falcon, 0x044));
		/* Volta MMU fault registers (fb_mmu_fault_*) */
		FLCN_DBG(falcon, "mmu fault: addr %08x:%08x inst %08x:%08x info %08x status %08x "
			 "mmu_ctrl %08x pri_fault %08x",
			 nvkm_rd32(device, 0x100e50), nvkm_rd32(device, 0x100e4c),
			 nvkm_rd32(device, 0x100e58), nvkm_rd32(device, 0x100e54),
			 nvkm_rd32(device, 0x100e5c), nvkm_rd32(device, 0x100e60),
			 nvkm_rd32(device, 0x100c80), nvkm_rd32(device, 0x100a2c));
		for (i = 0; i < n && i < 16; i++) {
			nvkm_falcon_wr32(falcon, 0x148, i);
			FLCN_DBG(falcon, "trace[%2d] pc %08x", i, nvkm_falcon_rd32(falcon, 0x14c));
		}
	}
	/* XXX gv11b bring-up diagnostics */
	FLCN_DBG(falcon, "boot: cpuctl %08x sctl %08x exci %08x irqstat %08x os %08x "
		 "dmactl %08x hwcfg %08x engine %08x",
		 nvkm_falcon_rd32(falcon, 0x100), nvkm_falcon_rd32(falcon, 0x240),
		 nvkm_falcon_rd32(falcon, 0x16c), nvkm_falcon_rd32(falcon, 0x008),
		 nvkm_falcon_rd32(falcon, 0x080), nvkm_falcon_rd32(falcon, 0x10c),
		 nvkm_falcon_rd32(falcon, 0x108), nvkm_falcon_rd32(falcon, 0x3c0));
	if (FLCN_ERRON(falcon, ret || mbox0 != mbox0_ok, "mbox %08x %08x", mbox0, mbox1))
		ret = ret ?: -EIO;

	if (irqsclr)
		nvkm_falcon_mask(falcon, 0x004, 0xffffffff, irqsclr);

	return ret;
}

int
gm200_flcn_fw_load(struct nvkm_falcon_fw *fw)
{
	struct nvkm_falcon *falcon = fw->falcon;
	int ret;

	if (fw->inst) {
		int target;

		nvkm_falcon_mask(falcon, 0x048, 0x00000001, 0x00000001);

		switch (nvkm_memory_target(fw->inst)) {
		case NVKM_MEM_TARGET_VRAM: target = 0; break;
		case NVKM_MEM_TARGET_HOST: target = 2; break;
		case NVKM_MEM_TARGET_NCOH: target = 3; break;
		default:
			WARN_ON(1);
			return -EINVAL;
		}

		falcon->func->bind_inst(falcon, target, nvkm_memory_addr(fw->inst));

		if (nvkm_msec(falcon->owner->device, 10,
			if (falcon->func->bind_stat(falcon, falcon->func->bind_intr) == 5)
				break;
		) < 0) {
			/* XXX gv11b bring-up diagnostics */
			FLCN_ERR(falcon, "bind timeout: target %d inst %llx stat %d "
				 "0x008 %08x 0x048 %08x 0x054 %08x 0x090 %08x 0x0dc %08x 0x100 %08x "
				 "0x480 %08x 0x604 %08x 0xe00 %08x %08x %08x %08x %08x",
				 target, nvkm_memory_addr(fw->inst),
				 falcon->func->bind_stat(falcon, false),
				 nvkm_falcon_rd32(falcon, 0x008), nvkm_falcon_rd32(falcon, 0x048),
				 nvkm_falcon_rd32(falcon, 0x054), nvkm_falcon_rd32(falcon, 0x090),
				 nvkm_falcon_rd32(falcon, 0x0dc), nvkm_falcon_rd32(falcon, 0x100),
				 nvkm_falcon_rd32(falcon, 0x480), nvkm_falcon_rd32(falcon, 0x604),
				 nvkm_falcon_rd32(falcon, 0xe00), nvkm_falcon_rd32(falcon, 0xe04),
				 nvkm_falcon_rd32(falcon, 0xe08), nvkm_falcon_rd32(falcon, 0xe0c),
				 nvkm_falcon_rd32(falcon, 0xe10));
			return -ETIMEDOUT;
		}

		nvkm_falcon_mask(falcon, 0x004, 0x00000008, 0x00000008);
		nvkm_falcon_mask(falcon, 0x058, 0x00000002, 0x00000002);

		if (nvkm_msec(falcon->owner->device, 10,
			if (falcon->func->bind_stat(falcon, false) == 0)
				break;
		) < 0)
			return -ETIMEDOUT;
	} else {
		nvkm_falcon_mask(falcon, 0x624, 0x00000080, 0x00000080);
		nvkm_falcon_wr32(falcon, 0x10c, 0x00000000);
	}

	if (fw->boot) {
		ret = nvkm_falcon_pio_wr(falcon, fw->boot, 0, 0,
					 IMEM, falcon->code.limit - fw->boot_size, fw->boot_size,
					 fw->boot_addr >> 8, false);
		if (ret)
			return ret;

		return fw->func->load_bld(fw);
	}

	ret = nvkm_falcon_pio_wr(falcon, fw->fw.img + fw->nmem_base_img, fw->nmem_base_img, 0,
				 IMEM, fw->nmem_base, fw->nmem_size, fw->nmem_base >> 8, false);
	if (ret)
		return ret;

	ret = nvkm_falcon_pio_wr(falcon, fw->fw.img + fw->imem_base_img, fw->imem_base_img, 0,
				 IMEM, fw->imem_base, fw->imem_size, fw->imem_base >> 8, true);
	if (ret)
		return ret;

	ret = nvkm_falcon_pio_wr(falcon, fw->fw.img + fw->dmem_base_img, fw->dmem_base_img, 0,
				 DMEM, fw->dmem_base, fw->dmem_size, 0, false);
	if (ret)
		return ret;

	return 0;
}

int
gm200_flcn_fw_reset(struct nvkm_falcon_fw *fw)
{
	return nvkm_falcon_reset(fw->falcon);
}

int
gm200_flcn_fw_signature(struct nvkm_falcon_fw *fw, u32 *sig_base_src)
{
	struct nvkm_falcon *falcon = fw->falcon;
	u32 addr = falcon->func->debug;
	int ret = 0;

	if (addr) {
		ret = nvkm_falcon_enable(falcon);
		if (ret)
			return ret;

		if (nvkm_falcon_rd32(falcon, addr) & 0x00100000) {
			*sig_base_src = fw->sig_base_dbg;
			return 1;
		}
	}

	return ret;
}

const struct nvkm_falcon_fw_func
gm200_flcn_fw = {
	.signature = gm200_flcn_fw_signature,
	.reset = gm200_flcn_fw_reset,
	.load = gm200_flcn_fw_load,
	.boot = gm200_flcn_fw_boot,
};
