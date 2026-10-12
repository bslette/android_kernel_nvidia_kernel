/*
 * Copyright 2018 Red Hat Inc.
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
#include "chan.h"
#include "chid.h"
#include "cgrp.h"
#include "runl.h"
#include "runq.h"

#include <engine/gr.h>
#include <subdev/mc.h>

#include <core/gpuobj.h>
#include <subdev/mmu.h>

#include <nvif/class.h>

static u32
gv100_chan_doorbell_handle(struct nvkm_chan *chan)
{
	return chan->id;
}

static int
gv100_chan_ramfc_write(struct nvkm_chan *chan, u64 offset, u64 length, u32 devm, bool priv)
{
	const u64 userd = nvkm_memory_addr(chan->userd.mem) + chan->userd.base;
	const u32 limit2 = ilog2(length / 8);

	nvkm_kmap(chan->inst);
	nvkm_wo32(chan->inst, 0x008, lower_32_bits(userd));
	nvkm_wo32(chan->inst, 0x00c, upper_32_bits(userd));
	nvkm_wo32(chan->inst, 0x010, 0x0000face);
	nvkm_wo32(chan->inst, 0x030, 0x7ffff902);
	nvkm_wo32(chan->inst, 0x048, lower_32_bits(offset));
	nvkm_wo32(chan->inst, 0x04c, upper_32_bits(offset) | (limit2 << 16));
	nvkm_wo32(chan->inst, 0x084, 0x20400000);
	nvkm_wo32(chan->inst, 0x094, 0x30000000 | devm);
	nvkm_wo32(chan->inst, 0x0e4, priv ? 0x00000020 : 0x00000000);
	nvkm_wo32(chan->inst, 0x0e8, chan->id);
	nvkm_wo32(chan->inst, 0x0f4, 0x00001000 | (priv ? 0x00000100 : 0x00000000));
	nvkm_wo32(chan->inst, 0x0f8, 0x10003080);
	nvkm_mo32(chan->inst, 0x218, 0x00000000, 0x00000000);
	nvkm_done(chan->inst);
	return 0;
}

const struct nvkm_chan_func_ramfc
gv100_chan_ramfc = {
	.write = gv100_chan_ramfc_write,
	.devm = 0xfff,
	.priv = true,
};

const struct nvkm_chan_func_userd
gv100_chan_userd = {
	.bar = -1,
	.size = 0x200,
	.clear = gf100_chan_userd_clear,
};

static const struct nvkm_chan_func
gv100_chan = {
	.inst = &gf100_chan_inst,
	.userd = &gv100_chan_userd,
	.ramfc = &gv100_chan_ramfc,
	.bind = gk104_chan_bind_inst,
	.unbind = gk104_chan_unbind,
	.start = gk104_chan_start,
	.stop = gk104_chan_stop,
	.preempt = gk110_chan_preempt,
	.doorbell_handle = gv100_chan_doorbell_handle,
};

void
gv100_ectx_bind(struct nvkm_engn *engn, struct nvkm_cctx *cctx, struct nvkm_chan *chan)
{
	u64 addr = 0ULL;

	if (cctx) {
		addr  = cctx->vctx->vma->addr;
		addr |= 4ULL;
	}

	nvkm_kmap(chan->inst);
	nvkm_wo32(chan->inst, 0x210, lower_32_bits(addr));
	nvkm_wo32(chan->inst, 0x214, upper_32_bits(addr));
	nvkm_mo32(chan->inst, 0x0ac, 0x00010000, cctx ? 0x00010000 : 0x00000000);
	nvkm_done(chan->inst);
}

const struct nvkm_engn_func
gv100_engn = {
	.chsw = gk104_engn_chsw,
	.cxid = gk104_engn_cxid,
	.ctor = gk104_ectx_ctor,
	.bind = gv100_ectx_bind,
};

void
gv100_ectx_ce_bind(struct nvkm_engn *engn, struct nvkm_cctx *cctx, struct nvkm_chan *chan)
{
	const u64 bar2 = cctx ? nvkm_memory_bar2(cctx->vctx->inst->memory) : 0ULL;

	nvkm_kmap(chan->inst);
	nvkm_wo32(chan->inst, 0x220, lower_32_bits(bar2));
	nvkm_wo32(chan->inst, 0x224, upper_32_bits(bar2));
	nvkm_mo32(chan->inst, 0x0ac, 0x00020000, cctx ? 0x00020000 : 0x00000000);
	nvkm_done(chan->inst);
}

int
gv100_ectx_ce_ctor(struct nvkm_engn *engn, struct nvkm_vctx *vctx)
{
	if (nvkm_memory_bar2(vctx->inst->memory) == ~0ULL)
		return -EFAULT;

	return 0;
}

const struct nvkm_engn_func
gv100_engn_ce = {
	.chsw = gk104_engn_chsw,
	.cxid = gk104_engn_cxid,
	.ctor = gv100_ectx_ce_ctor,
	.bind = gv100_ectx_ce_bind,
};

static bool
gv100_runq_intr_1_ctxnotvalid(struct nvkm_runq *runq, int chid)
{
	struct nvkm_fifo *fifo = runq->fifo;
	struct nvkm_device *device = fifo->engine.subdev.device;
	struct nvkm_chan *chan;
	unsigned long flags;

	RUNQ_ERROR(runq, "CTXNOTVALID chid:%d", chid);

	chan = nvkm_chan_get_chid(&fifo->engine, chid, &flags);
	if (WARN_ON_ONCE(!chan))
		return false;

	nvkm_chan_error(chan, true);
	nvkm_chan_put(&chan, flags);

	nvkm_mask(device, 0x0400ac + (runq->id * 0x2000), 0x00030000, 0x00030000);
	nvkm_wr32(device, 0x040148 + (runq->id * 0x2000), 0x80000000);
	return true;
}

const struct nvkm_runq_func
gv100_runq = {
	.init = gk208_runq_init,
	.intr = gk104_runq_intr,
	.intr_0_names = gk104_runq_intr_0_names,
	.intr_1_ctxnotvalid = gv100_runq_intr_1_ctxnotvalid,
	.idle = gk104_runq_idle,
};

void
gv100_runl_preempt(struct nvkm_runl *runl)
{
	nvkm_wr32(runl->fifo->engine.subdev.device, 0x002638, BIT(runl->id));
}

void
gv100_runl_insert_chan(struct nvkm_chan *chan, struct nvkm_memory *memory, u64 offset)
{
	const u64 user = nvkm_memory_addr(chan->userd.mem) + chan->userd.base;
	const u64 inst = chan->inst->addr;

	nvkm_wo32(memory, offset + 0x0, lower_32_bits(user) | chan->runq << 1);
	nvkm_wo32(memory, offset + 0x4, upper_32_bits(user));
	nvkm_wo32(memory, offset + 0x8, lower_32_bits(inst) | chan->id);
	nvkm_wo32(memory, offset + 0xc, upper_32_bits(inst));
}

/* XXX bring-up: TSG timeslice = 128 << scale us (default 3 = 1 ms, as nvgpu) */
static int gv100_tslice_scale = 3;
module_param_named(tslice_scale, gv100_tslice_scale, int, 0644);

void
gv100_runl_insert_cgrp(struct nvkm_cgrp *cgrp, struct nvkm_memory *memory, u64 offset)
{
	nvkm_wo32(memory, offset + 0x0, (128 << 24) | ((gv100_tslice_scale & 0xf) << 16) | 0x00000001);
	nvkm_wo32(memory, offset + 0x4, cgrp->chan_nr);
	nvkm_wo32(memory, offset + 0x8, cgrp->id);
	nvkm_wo32(memory, offset + 0xc, 0x00000000);
}

static const struct nvkm_runl_func
gv100_runl = {
	.runqs = 2,
	.size = 16,
	.update = nv50_runl_update,
	.insert_cgrp = gv100_runl_insert_cgrp,
	.insert_chan = gv100_runl_insert_chan,
	.commit = gk104_runl_commit,
	.wait = nv50_runl_wait,
	.pending = gk104_runl_pending,
	.block = gk104_runl_block,
	.allow = gk104_runl_allow,
	.preempt = gv100_runl_preempt,
	.preempt_pending = gf100_runl_preempt_pending,
};

const struct nvkm_enum
gv100_fifo_mmu_fault_gpcclient[] = {
	{ 0x00, "T1_0" },
	{ 0x01, "T1_1" },
	{ 0x02, "T1_2" },
	{ 0x03, "T1_3" },
	{ 0x04, "T1_4" },
	{ 0x05, "T1_5" },
	{ 0x06, "T1_6" },
	{ 0x07, "T1_7" },
	{ 0x08, "PE_0" },
	{ 0x09, "PE_1" },
	{ 0x0a, "PE_2" },
	{ 0x0b, "PE_3" },
	{ 0x0c, "PE_4" },
	{ 0x0d, "PE_5" },
	{ 0x0e, "PE_6" },
	{ 0x0f, "PE_7" },
	{ 0x10, "RAST" },
	{ 0x11, "GCC" },
	{ 0x12, "GPCCS" },
	{ 0x13, "PROP_0" },
	{ 0x14, "PROP_1" },
	{ 0x15, "PROP_2" },
	{ 0x16, "PROP_3" },
	{ 0x17, "GPM" },
	{ 0x18, "LTP_UTLB_0" },
	{ 0x19, "LTP_UTLB_1" },
	{ 0x1a, "LTP_UTLB_2" },
	{ 0x1b, "LTP_UTLB_3" },
	{ 0x1c, "LTP_UTLB_4" },
	{ 0x1d, "LTP_UTLB_5" },
	{ 0x1e, "LTP_UTLB_6" },
	{ 0x1f, "LTP_UTLB_7" },
	{ 0x20, "RGG_UTLB" },
	{ 0x21, "T1_8" },
	{ 0x22, "T1_9" },
	{ 0x23, "T1_10" },
	{ 0x24, "T1_11" },
	{ 0x25, "T1_12" },
	{ 0x26, "T1_13" },
	{ 0x27, "T1_14" },
	{ 0x28, "T1_15" },
	{ 0x29, "TPCCS_0" },
	{ 0x2a, "TPCCS_1" },
	{ 0x2b, "TPCCS_2" },
	{ 0x2c, "TPCCS_3" },
	{ 0x2d, "TPCCS_4" },
	{ 0x2e, "TPCCS_5" },
	{ 0x2f, "TPCCS_6" },
	{ 0x30, "TPCCS_7" },
	{ 0x31, "PE_8" },
	{ 0x32, "PE_9" },
	{ 0x33, "TPCCS_8" },
	{ 0x34, "TPCCS_9" },
	{ 0x35, "T1_16" },
	{ 0x36, "T1_17" },
	{ 0x37, "T1_18" },
	{ 0x38, "T1_19" },
	{ 0x39, "PE_10" },
	{ 0x3a, "PE_11" },
	{ 0x3b, "TPCCS_10" },
	{ 0x3c, "TPCCS_11" },
	{ 0x3d, "T1_20" },
	{ 0x3e, "T1_21" },
	{ 0x3f, "T1_22" },
	{ 0x40, "T1_23" },
	{ 0x41, "PE_12" },
	{ 0x42, "PE_13" },
	{ 0x43, "TPCCS_12" },
	{ 0x44, "TPCCS_13" },
	{ 0x45, "T1_24" },
	{ 0x46, "T1_25" },
	{ 0x47, "T1_26" },
	{ 0x48, "T1_27" },
	{ 0x49, "PE_14" },
	{ 0x4a, "PE_15" },
	{ 0x4b, "TPCCS_14" },
	{ 0x4c, "TPCCS_15" },
	{ 0x4d, "T1_28" },
	{ 0x4e, "T1_29" },
	{ 0x4f, "T1_30" },
	{ 0x50, "T1_31" },
	{ 0x51, "PE_16" },
	{ 0x52, "PE_17" },
	{ 0x53, "TPCCS_16" },
	{ 0x54, "TPCCS_17" },
	{ 0x55, "T1_32" },
	{ 0x56, "T1_33" },
	{ 0x57, "T1_34" },
	{ 0x58, "T1_35" },
	{ 0x59, "PE_18" },
	{ 0x5a, "PE_19" },
	{ 0x5b, "TPCCS_18" },
	{ 0x5c, "TPCCS_19" },
	{ 0x5d, "T1_36" },
	{ 0x5e, "T1_37" },
	{ 0x5f, "T1_38" },
	{ 0x60, "T1_39" },
	{}
};

const struct nvkm_enum
gv100_fifo_mmu_fault_hubclient[] = {
	{ 0x00, "VIP" },
	{ 0x01, "CE0" },
	{ 0x02, "CE1" },
	{ 0x03, "DNISO" },
	{ 0x04, "FE" },
	{ 0x05, "FECS" },
	{ 0x06, "HOST" },
	{ 0x07, "HOST_CPU" },
	{ 0x08, "HOST_CPU_NB" },
	{ 0x09, "ISO" },
	{ 0x0a, "MMU" },
	{ 0x0b, "NVDEC" },
	{ 0x0d, "NVENC1" },
	{ 0x0e, "NISO" },
	{ 0x0f, "P2P" },
	{ 0x10, "PD" },
	{ 0x11, "PERF" },
	{ 0x12, "PMU" },
	{ 0x13, "RASTERTWOD" },
	{ 0x14, "SCC" },
	{ 0x15, "SCC_NB" },
	{ 0x16, "SEC" },
	{ 0x17, "SSYNC" },
	{ 0x18, "CE2" },
	{ 0x19, "XV" },
	{ 0x1a, "MMU_NB" },
	{ 0x1b, "NVENC0" },
	{ 0x1c, "DFALCON" },
	{ 0x1d, "SKED" },
	{ 0x1e, "AFALCON" },
	{ 0x1f, "DONT_CARE" },
	{ 0x20, "HSCE0" },
	{ 0x21, "HSCE1" },
	{ 0x22, "HSCE2" },
	{ 0x23, "HSCE3" },
	{ 0x24, "HSCE4" },
	{ 0x25, "HSCE5" },
	{ 0x26, "HSCE6" },
	{ 0x27, "HSCE7" },
	{ 0x28, "HSCE8" },
	{ 0x29, "HSCE9" },
	{ 0x2a, "HSHUB" },
	{ 0x2b, "PTP_X0" },
	{ 0x2c, "PTP_X1" },
	{ 0x2d, "PTP_X2" },
	{ 0x2e, "PTP_X3" },
	{ 0x2f, "PTP_X4" },
	{ 0x30, "PTP_X5" },
	{ 0x31, "PTP_X6" },
	{ 0x32, "PTP_X7" },
	{ 0x33, "NVENC2" },
	{ 0x34, "VPR_SCRUBBER0" },
	{ 0x35, "VPR_SCRUBBER1" },
	{ 0x36, "DWBIF" },
	{ 0x37, "FBFALCON" },
	{ 0x38, "CE_SHIM" },
	{ 0x39, "GSP" },
	{}
};

const struct nvkm_enum
gv100_fifo_mmu_fault_reason[] = {
	{ 0x00, "PDE" },
	{ 0x01, "PDE_SIZE" },
	{ 0x02, "PTE" },
	{ 0x03, "VA_LIMIT_VIOLATION" },
	{ 0x04, "UNBOUND_INST_BLOCK" },
	{ 0x05, "PRIV_VIOLATION" },
	{ 0x06, "RO_VIOLATION" },
	{ 0x07, "WO_VIOLATION" },
	{ 0x08, "PITCH_MASK_VIOLATION" },
	{ 0x09, "WORK_CREATION" },
	{ 0x0a, "UNSUPPORTED_APERTURE" },
	{ 0x0b, "COMPRESSION_FAILURE" },
	{ 0x0c, "UNSUPPORTED_KIND" },
	{ 0x0d, "REGION_VIOLATION" },
	{ 0x0e, "POISONED" },
	{ 0x0f, "ATOMIC_VIOLATION" },
	{}
};

static const struct nvkm_enum
gv100_fifo_mmu_fault_engine[] = {
	{ 0x01, "DISPLAY" },
	{ 0x03, "PTP" },
	{ 0x04, "BAR1", NULL, NVKM_SUBDEV_BAR },
	{ 0x05, "BAR2", NULL, NVKM_SUBDEV_INSTMEM },
	{ 0x06, "PWR_PMU" },
	{ 0x08, "IFB", NULL, NVKM_ENGINE_IFB },
	{ 0x09, "PERF" },
	{ 0x1f, "PHYSICAL" },
	{ 0x20, "HOST0" },
	{ 0x21, "HOST1" },
	{ 0x22, "HOST2" },
	{ 0x23, "HOST3" },
	{ 0x24, "HOST4" },
	{ 0x25, "HOST5" },
	{ 0x26, "HOST6" },
	{ 0x27, "HOST7" },
	{ 0x28, "HOST8" },
	{ 0x29, "HOST9" },
	{ 0x2a, "HOST10" },
	{ 0x2b, "HOST11" },
	{ 0x2c, "HOST12" },
	{ 0x2d, "HOST13" },
	{}
};

const struct nvkm_enum
gv100_fifo_mmu_fault_access[] = {
	{ 0x0, "VIRT_READ" },
	{ 0x1, "VIRT_WRITE" },
	{ 0x2, "VIRT_ATOMIC" },
	{ 0x3, "VIRT_PREFETCH" },
	{ 0x4, "VIRT_ATOMIC_WEAK" },
	{ 0x8, "PHYS_READ" },
	{ 0x9, "PHYS_WRITE" },
	{ 0xa, "PHYS_ATOMIC" },
	{ 0xb, "PHYS_PREFETCH" },
	{}
};

static const struct nvkm_fifo_func_mmu_fault
gv100_fifo_mmu_fault = {
	.recover = gf100_fifo_mmu_fault_recover,
	.access = gv100_fifo_mmu_fault_access,
	.engine = gv100_fifo_mmu_fault_engine,
	.reason = gv100_fifo_mmu_fault_reason,
	.hubclient = gv100_fifo_mmu_fault_hubclient,
	.gpcclient = gv100_fifo_mmu_fault_gpcclient,
};

static void
gv100_fifo_intr_ctxsw_timeout(struct nvkm_fifo *fifo, u32 engm)
{
	struct nvkm_runl *runl;
	struct nvkm_engn *engn;

	nvkm_runl_foreach(runl, fifo) {
		nvkm_runl_foreach_engn_cond(engn, runl, engm & BIT(engn->id))
			nvkm_runl_rc_engn(runl, engn);
	}
}

static const struct nvkm_fifo_func
gv100_fifo = {
	.chid_nr = gm200_fifo_chid_nr,
	.chid_ctor = gk110_fifo_chid_ctor,
	.runq_nr = gm200_fifo_runq_nr,
	.runl_ctor = gk104_fifo_runl_ctor,
	.init = gk104_fifo_init,
	.init_pbdmas = gk104_fifo_init_pbdmas,
	.intr = gk104_fifo_intr,
	.intr_ctxsw_timeout = gv100_fifo_intr_ctxsw_timeout,
	.mmu_fault = &gv100_fifo_mmu_fault,
	.nonstall = &gf100_fifo_nonstall,
	.runl = &gv100_runl,
	.runq = &gv100_runq,
	.engn = &gv100_engn,
	.engn_ce = &gv100_engn_ce,
	.cgrp = {{ 0, 0, KEPLER_CHANNEL_GROUP_A  }, &gk110_cgrp, .force = true },
	.chan = {{ 0, 0,  VOLTA_CHANNEL_GPFIFO_A }, &gv100_chan },
};

int
gv100_fifo_new(struct nvkm_device *device, enum nvkm_subdev_type type, int inst,
	       struct nvkm_fifo **pfifo)
{
	return nvkm_fifo_new_(&gv100_fifo, device, type, inst, pfifo);
}

/*
 * GV11B: Tegra instmem has no BAR2 mappings yet, so the CE method buffer
 * pointer (ram_in_eng_method_buffer_addr, inst+0x220) is left at 0. It is
 * only used to save CE methods on a fault.
 * XXX implement BAR2 for Tegra instmem and point this at a real buffer.
 */
static void
gv11b_ectx_ce_bind(struct nvkm_engn *engn, struct nvkm_cctx *cctx, struct nvkm_chan *chan)
{
	nvkm_kmap(chan->inst);
	nvkm_wo32(chan->inst, 0x220, 0x00000000);
	nvkm_wo32(chan->inst, 0x224, 0x00000000);
	nvkm_mo32(chan->inst, 0x0ac, 0x00020000, cctx ? 0x00020000 : 0x00000000);
	nvkm_done(chan->inst);
}

static const struct nvkm_engn_func
gv11b_engn_ce = {
	.chsw = gk104_engn_chsw,
	.cxid = gk104_engn_cxid,
	.bind = gv11b_ectx_ce_bind,
};

/*
 * GV11B: the channel instance pointer (ccsr_channel_inst) needs its aperture
 * (bits 29:28); gk104_chan_bind_inst leaves it 0 (VRAM), and the channel then
 * never answers a preempt (10 s nvkm_runl_preempt_wait timeouts).
 */
/*
 * GV11B: RAMFC's USERD address carries its aperture in bits 1:0
 * (pbdma_userd_target_*); gv100 leaves 0 (VRAM). Use coherent sysmem.
 */
static int
gv11b_chan_ramfc_write(struct nvkm_chan *chan, u64 offset, u64 length, u32 devm, bool priv)
{
	int ret = gv100_chan_ramfc_write(chan, offset, length, devm, priv);

	if (ret)
		return ret;

	nvkm_kmap(chan->inst);
	nvkm_mo32(chan->inst, 0x008, 0x00000003, 0x00000002);
	nvkm_done(chan->inst);
	return 0;
}

static const struct nvkm_chan_func_ramfc
gv11b_chan_ramfc = {
	.write = gv11b_chan_ramfc_write,
	.devm = 0xfff,
	.priv = true,
};

static void
gv11b_chan_bind_inst(struct nvkm_chan *chan)
{
	struct nvkm_device *device = chan->cgrp->runl->fifo->engine.subdev.device;
	u32 aper = 0;

	/*
	 * Tegra194 is IO-coherent and the DMA API gives nouveau cached memory
	 * without cache maintenance, so the GPU must snoop: use the coherent
	 * aperture. With non-coherent (3) the PBDMA read stale RAMFC
	 * (PBDMA intr 0x01000000 storm).
	 */
	switch (nvkm_memory_target(chan->inst->memory)) {
	case NVKM_MEM_TARGET_HOST:
	case NVKM_MEM_TARGET_NCOH: aper = 0x20000000; break;
	default:
		break;
	}

	nvkm_wr32(device, 0x800000 + (chan->id * 8), 0x80000000 | aper | chan->inst->addr >> 12);
}

/* GV11B (Tegra194): no VRAM, so USERD is FIFO-owned in BAR1 as on GK104/GP10B. */
static const struct nvkm_chan_func
gv11b_chan = {
	.inst = &gf100_chan_inst,
	.userd = &gk104_chan_userd,
	.ramfc = &gv11b_chan_ramfc,
	.bind = gv11b_chan_bind_inst,
	.unbind = gk104_chan_unbind,
	.start = gk104_chan_start,
	.stop = gk104_chan_stop,
	.preempt = gk110_chan_preempt,
	.doorbell_handle = gv100_chan_doorbell_handle,
};

/*
 * nvgpu initialises GR (incl. its engine reset) at power-on, before any
 * channel exists. nouveau inits GR lazily on the first GR object, after the
 * DRM's kernel channel is already resident, and the reset then hits the
 * loaded PBDMA (ENG_RESET, intr 0x01000000) and the golden save times out.
 * Take a permanent GR reference here, before channels can be created.
 */
static void
gv11b_fifo_init(struct nvkm_fifo *fifo)
{
	struct nvkm_device *device = fifo->engine.subdev.device;
	static bool gr_ref;

	if (!gr_ref && device->gr) {
		struct nvkm_engine *gr = nvkm_engine_ref(&device->gr->engine);

		if (IS_ERR(gr))
			nvkm_error(&fifo->engine.subdev, "early GR init failed: %ld\n", PTR_ERR(gr));
		else
			gr_ref = true;
	}

	gk104_fifo_init(fifo);
	nvkm_wr32(device, 0x002528, 0x80000000); /* channel non-stall -> intr tree 1 */
}

/*
 * GV11B (as nvgpu does): channel non-stall interrupts (fences) are routed
 * with FIFO intr_en_1 (0x2528) to MC leaf 1 (PFIFO bit 8), which Tegra
 * raises on its own "nonstall" line. intr_en_0 bit 31, as used on dGPUs,
 * never fired here.
 */
/*
 * The routing bit is left on permanently (set in gv11b_fifo_init, as nvgpu
 * does): a NON_STALL_INTERRUPT executed while 0x2528 bit 31 was clear is
 * dropped, not latched, so enabling it lazily on the first wait lost the
 * first fence's interrupt (30 s wait). Notifying with no listener is cheap.
 */
static void
gv11b_fifo_nonstall_allow(struct nvkm_event *event, int type, int index)
{
}

static void
gv11b_fifo_nonstall_block(struct nvkm_event *event, int type, int index)
{
}

static const struct nvkm_event_func
gv11b_fifo_nonstall = {
	.init = gv11b_fifo_nonstall_allow,
	.fini = gv11b_fifo_nonstall_block,
};

static irqreturn_t
gv11b_fifo_nonstall_intr(struct nvkm_inth *inth)
{
	struct nvkm_runl *runl = container_of(inth, typeof(*runl), nonstall.inth);
	struct nvkm_fifo *fifo = runl->fifo;
	struct nvkm_device *device = fifo->engine.subdev.device;

	if (!(nvkm_rd32(device, 0x002100) & 0x80000000))
		return IRQ_NONE;

	nvkm_wr32(device, 0x002100, 0x80000000);
	nvkm_event_ntfy(&fifo->nonstall.event, 0, NVKM_FIFO_NONSTALL_EVENT);
	return IRQ_HANDLED;
}

static int
gv11b_fifo_nonstall_ctor(struct nvkm_fifo *fifo)
{
	struct nvkm_runl *runl = list_first_entry(&fifo->runls, typeof(*runl), head);
	int ret;

	/* MC leaf 1 (non-stall), PFIFO bit 8. */
	ret = nvkm_inth_add(&fifo->engine.subdev.device->mc->intr, 32 + 8,
			    NVKM_INTR_PRIO_NORMAL, &fifo->engine.subdev,
			    gv11b_fifo_nonstall_intr, &runl->nonstall.inth);
	if (ret)
		return ret;

	nvkm_inth_allow(&runl->nonstall.inth);
	return 1;
}

/*
 * GV11B: FIFO ctxsw timeout (intr bit 1). Same registers as Turing
 * (0x2a30 engine mask, 0x3200 + 4*engn info, nvgpu fifo_intr_ctxsw_timeout*),
 * which gk104_fifo_intr does not handle: it masked the interrupt and left
 * 0x2a30 latched, and the GPU stopped scheduling (Lineage boot, and three
 * concurrent long-draw GL contexts on Ubuntu).
 */
static void
gv11b_fifo_intr_ctxsw_timeout(struct nvkm_fifo *fifo)
{
	struct nvkm_subdev *subdev = &fifo->engine.subdev;
	struct nvkm_device *device = subdev->device;
	struct nvkm_runl *runl;
	struct nvkm_engn *engn;
	u32 engm = nvkm_rd32(device, 0x002a30);

	nvkm_runl_foreach(runl, fifo) {
		nvkm_runl_foreach_engn_cond(engn, runl, engm & BIT(engn->id)) {
			u32 info = nvkm_rd32(device, 0x003200 + (engn->id * 4));
			static const char *const state[] = { "?", "load", "save", "switch" };
			static const char *const status[] = { "awaiting ack", "eng was reset",
							      "ack received", "dropped timeout" };
			struct nvkm_cgrp *cgrp = NULL;
			unsigned long flags;
			int id = -1;

			switch (info & 0x0000c000) {
			case 0x00004000: id = (info >> 16) & 0x3fff; break;	/* LOAD: next */
			case 0x00008000:					/* SAVE */
			case 0x0000c000: id = info & 0x3fff; break;		/* SWITCH: prev */
			default: break;
			}

			nvkm_warn(subdev, "ctxsw timeout engn %d: %s, prev tsg %d next tsg %d, %s\n",
				  engn->id, state[(info >> 14) & 3], info & 0x3fff,
				  (info >> 16) & 0x3fff, status[info >> 30]);

			{	/* XXX GV11B bring-up: one-shot snapshot of GR/FECS at the timeout */
				static int dumped;
				if (!dumped++) {
					int m;
					nvkm_warn(subdev, "XXX gr_status %08x gr_engine_status %08x eng_status %08x pgraph intr %08x\n",
						  nvkm_rd32(device, 0x400700), nvkm_rd32(device, 0x40060c),
						  nvkm_rd32(device, 0x002640 + engn->id * 8), nvkm_rd32(device, 0x400100));
					nvkm_warn(subdev, "XXX fecs cur_ctx %08x new_ctx %08x host_int %08x cpuctl %08x irqstat %08x\n",
						  nvkm_rd32(device, 0x409b00), nvkm_rd32(device, 0x409b04),
						  nvkm_rd32(device, 0x409c18), nvkm_rd32(device, 0x409100),
						  nvkm_rd32(device, 0x409008));
					for (m = 0; m < 16; m += 4)
						nvkm_warn(subdev, "XXX fecs mbox %d-%d: %08x %08x %08x %08x\n", m, m + 3,
							  nvkm_rd32(device, 0x409800 + m * 4), nvkm_rd32(device, 0x409804 + m * 4),
							  nvkm_rd32(device, 0x409808 + m * 4), nvkm_rd32(device, 0x40980c + m * 4));
					nvkm_warn(subdev, "XXX gpccs cpuctl %08x mbox0 %08x\n",
						  nvkm_rd32(device, 0x41a100), nvkm_rd32(device, 0x41a040));
				}
			}

			/* ACK_RECEIVED/DROPPED_TIMEOUT: the switch completed, nothing stuck. */
			if (info & 0xc0000000 || id < 0)
				continue;

			cgrp = nvkm_runl_cgrp_get_cgid(runl, id, &flags);
			if (!WARN_ON(!cgrp)) {
				nvkm_runl_rc_cgrp(cgrp);
				nvkm_cgrp_put(&cgrp, flags);
			}
		}
	}

	nvkm_wr32(device, 0x002a30, engm);
}

static irqreturn_t
gv11b_fifo_intr(struct nvkm_inth *inth)
{
	struct nvkm_fifo *fifo = container_of(inth, typeof(*fifo), engine.subdev.inth);
	struct nvkm_device *device = fifo->engine.subdev.device;

	if (nvkm_rd32(device, 0x002100) & nvkm_rd32(device, 0x002140) & 0x00000002) {
		gv11b_fifo_intr_ctxsw_timeout(fifo);
		nvkm_wr32(device, 0x002100, 0x00000002);
	}

	return gk104_fifo_intr(inth);
}

static const struct nvkm_fifo_func
gv11b_fifo = {
	.chid_nr = gm200_fifo_chid_nr,
	.chid_ctor = gk110_fifo_chid_ctor,
	.runq_nr = gm200_fifo_runq_nr,
	.runl_ctor = gk104_fifo_runl_ctor,
	.init = gv11b_fifo_init,
	.init_pbdmas = gk104_fifo_init_pbdmas,
	.intr = gv11b_fifo_intr,
	.intr_ctxsw_timeout = gv100_fifo_intr_ctxsw_timeout,
	.mmu_fault = &gv100_fifo_mmu_fault,
	.nonstall_ctor = gv11b_fifo_nonstall_ctor,
	.nonstall = &gv11b_fifo_nonstall,
	.runl = &gv100_runl,
	.runq = &gv100_runq,
	.engn = &gv100_engn,
	.engn_ce = &gv11b_engn_ce,
	.cgrp = {{ 0, 0, KEPLER_CHANNEL_GROUP_A  }, &gk110_cgrp, .force = true },
	.chan = {{ 0, 0,  VOLTA_CHANNEL_GPFIFO_A }, &gv11b_chan },
};

int
gv11b_fifo_new(struct nvkm_device *device, enum nvkm_subdev_type type, int inst,
	       struct nvkm_fifo **pfifo)
{
	return nvkm_fifo_new_(&gv11b_fifo, device, type, inst, pfifo);
}
