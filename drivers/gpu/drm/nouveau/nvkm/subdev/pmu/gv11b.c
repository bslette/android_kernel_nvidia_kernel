/* SPDX-License-Identifier: MIT
 *
 * GV11B (Tegra194) PMU.
 *
 * Same NVIDIA PMU RTOS command interface as GP10B (ACR bootstrap of the PMU,
 * FECS and GPCCS LS falcons), but the firmware descriptor is nvfw_ls_desc_v1
 * and the WPR uses the v1 loader config, as in R32 nvgpu
 * (gp106_pmu_populate_loader_cfg).
 */
#include "priv.h"

#include <subdev/acr.h>

#include <nvfw/flcn.h>
#include <nvfw/pmu.h>

static int
gv11b_pmu_acr_bootstrap_multiple_falcons_cb(void *priv,
					    struct nvfw_falcon_msg *hdr)
{
	struct nv_pmu_acr_bootstrap_multiple_falcons_msg *msg =
		container_of(hdr, typeof(*msg), msg.hdr);
	return msg->falcon_mask;
}

static int
gv11b_pmu_acr_bootstrap_multiple_falcons(struct nvkm_falcon *falcon, u32 mask)
{
	struct nvkm_pmu *pmu = container_of(falcon, typeof(*pmu), falcon);
	struct nv_pmu_acr_bootstrap_multiple_falcons_cmd cmd = {
		.cmd.hdr.unit_id = NV_PMU_UNIT_ACR,
		.cmd.hdr.size = sizeof(cmd),
		.cmd.cmd_type = NV_PMU_ACR_CMD_BOOTSTRAP_MULTIPLE_FALCONS,
		.flags = NV_PMU_ACR_BOOTSTRAP_MULTIPLE_FALCONS_FLAGS_RESET_YES,
		.falcon_mask = mask,
		.wpr_lo = 0,
		.wpr_hi = 0,
	};
	int ret;

	ret = nvkm_falcon_cmdq_send(pmu->hpq, &cmd.cmd.hdr,
				    gv11b_pmu_acr_bootstrap_multiple_falcons_cb,
				    &pmu->subdev, msecs_to_jiffies(1000));
	if (ret >= 0) {
		if (ret != cmd.falcon_mask)
			ret = -EIO;
		else
			ret = 0;
	}

	return ret;
}

/*
 * The GV11B LS PMU bootloader takes a flcn_bl_dmem_desc_v1 (as nvgpu R35
 * writes it: ctx_dma UCODE, code at app start, non_sec_code_size = resident
 * code size, data at app resident data, argc 1, argv = v6 args address),
 * not a loader_config.
 */
static void
gv11b_pmu_acr_bld_patch(struct nvkm_acr *acr, u32 bld, s64 adjust)
{
	struct flcn_bl_dmem_desc_v1 hdr;

	nvkm_robj(acr->wpr, bld, &hdr, sizeof(hdr));
	hdr.code_dma_base = hdr.code_dma_base + adjust;
	hdr.data_dma_base = hdr.data_dma_base + adjust;
	nvkm_wobj(acr->wpr, bld, &hdr, sizeof(hdr));
	flcn_bl_dmem_desc_v1_dump(&acr->subdev, &hdr);
	nvkm_debug(&acr->subdev, "\targc %d argv %#x\n",
		   nvkm_ro32(acr->wpr, bld + sizeof(hdr)), nvkm_ro32(acr->wpr, bld + sizeof(hdr) + 4));
}

#define GV11B_PMU_ARGS_SIZE 60

struct gv11b_pmu_bl_desc {
	struct flcn_bl_dmem_desc_v1 v1;
	u32 argc;
	u32 argv;
} __packed;

static void
gv11b_pmu_acr_bld_write(struct nvkm_acr *acr, u32 bld,
			struct nvkm_acr_lsfw *lsfw)
{
	const u64 base = lsfw->offset.img + lsfw->app_start_offset;
	const struct gv11b_pmu_bl_desc hdr = {
		.v1.ctx_dma = FALCON_DMAIDX_UCODE,
		.v1.code_dma_base = base + lsfw->app_resident_code_offset,
		.v1.non_sec_code_off = 0,
		.v1.non_sec_code_size = lsfw->app_resident_code_size,
		.v1.code_entry_point = lsfw->app_imem_entry,
		.v1.data_dma_base = base + lsfw->app_resident_data_offset,
		.v1.data_size = lsfw->app_resident_data_size,
		.argc = 1,
		.argv = lsfw->falcon->data.limit - GV11B_PMU_ARGS_SIZE,
	};

	nvkm_wobj(acr->wpr, bld, &hdr, sizeof(hdr));
}

static const struct nvkm_acr_lsf_func
gv11b_pmu_acr = {
	.flags = NVKM_ACR_LSF_DMACTL_REQ_CTX,
	.bld_size = sizeof(struct gv11b_pmu_bl_desc),
	.bld_write = gv11b_pmu_acr_bld_write,
	.bld_patch = gv11b_pmu_acr_bld_patch,
	.bootstrap_falcons = BIT_ULL(NVKM_ACR_LSF_PMU) |
			     BIT_ULL(NVKM_ACR_LSF_FECS) |
			     BIT_ULL(NVKM_ACR_LSF_GPCCS),
	.bootstrap_falcon = gm20b_pmu_acr_bootstrap_falcon,
	.bootstrap_multiple_falcons = gv11b_pmu_acr_bootstrap_multiple_falcons,
};

/*
 * Volta PMU falcon: as GM20B/GP10B, but the bind status is read directly
 * (0x0dc) as for GP102+ falcons, and reset goes through the falcon's own
 * ENGINE register (0x3c0) as nvgpu does from GP10x on
 * (gp106_pmu_engine_reset). With the PMC-only reset the core never left
 * reset: cpuctl stayed 0x12 (HALTED with STARTCPU pending).
 */
/*
 * Like gm200_pmu_flcn_bind_inst, but the physical DMA indices point at
 * system memory (gv11b_setup_apertures in nvgpu): GV11B has no VRAM and
 * faults the local-FB aperture, which left the HS ACR stuck once it started
 * copying the LS blob into the WPR.
 */
static void
gv11b_pmu_flcn_bind_inst(struct nvkm_falcon *falcon, int target, u64 addr)
{
	/* Values as programmed by nvgpu (R35 register trace). */
	nvkm_falcon_wr32(falcon, 0xe00, 6); /* DMAIDX_UCODE: physical, ncoh sysmem */
	nvkm_falcon_wr32(falcon, 0xe04, 0); /* DMAIDX_VIRT */
	nvkm_falcon_wr32(falcon, 0xe08, 6); /* DMAIDX_PHYS_VID: physical, ncoh sysmem */
	nvkm_falcon_wr32(falcon, 0xe0c, 5); /* DMAIDX_PHYS_SYS_COH */
	nvkm_falcon_wr32(falcon, 0xe10, 6); /* DMAIDX_PHYS_SYS_NCOH */
	nvkm_falcon_mask(falcon, 0x090, 0x00010000, 0x00010000);
	/* Tegra194 is IO-coherent (NVGPU_USE_COHERENT_SYSMEM in nvgpu). */
	if (target == 3)
		target = 2;
	nvkm_falcon_wr32(falcon, 0x480, (1 << 30) | (target << 28) | (addr >> 12));
}

static const struct nvkm_falcon_func
gv11b_pmu_flcn = {
	.disable = gm200_flcn_disable,
	.enable = gm200_flcn_enable,
	.reset_eng = gp102_flcn_reset_eng,
	.reset_wait_mem_scrubbing = gm200_flcn_reset_wait_mem_scrubbing,
	.debug = 0xc08,
	.bind_inst = gv11b_pmu_flcn_bind_inst,
	.bind_stat = gm200_flcn_bind_stat,
	.imem_pio = &gm200_flcn_imem_pio,
	.dmem_pio = &gm200_flcn_dmem_pio,
	.start = nvkm_falcon_v1_start,
	.cmdq = { 0x4a0, 0x4b0, 4 },
	.msgq = { 0x4c8, 0x4cc, 0 },
};

/*
 * GV11B PMU firmware (APP_VERSION_GV11B) uses the GV10x/TU10x interface:
 * pmu_cmdline_args_v6 (60 bytes) at the top of DMEM, as written by nvgpu
 * (R35 trace: cpu_freq 204 MHz, secure_mode 1). Interrupt setup as nvgpu.
 */
struct gv11b_pmu_args {
	u32 cpu_freq_hz;
	u32 trace_buf[3];
	u8 secure_mode;
	u8 raise_priv_sec;
	u8 pad[2];
	u32 gc6_ctx[3];
	u32 gc6_bsod_ctx[3];
	u32 super_surface[3];
	u32 flags;
} __packed;

static int
gv11b_pmu_init(struct nvkm_pmu *pmu)
{
	struct nvkm_falcon *falcon = &pmu->falcon;
	struct gv11b_pmu_args args = {
		.cpu_freq_hz = 204000000,
		.secure_mode = 1,
	};
	u32 addr_args = falcon->data.limit - sizeof(args);
	int ret;

	BUILD_BUG_ON(sizeof(args) != GV11B_PMU_ARGS_SIZE);
	ret = nvkm_falcon_get(&pmu->falcon, &pmu->subdev);
	if (ret)
		return ret;

	pmu->initmsg_received = false;

	nvkm_falcon_pio_wr(falcon, (u8 *)&args, 0, 0, DMEM, addr_args, sizeof(args), 0, false);
	nvkm_falcon_wr32(falcon, 0x014, 0xffffffff);	/* irqmclr */
	nvkm_falcon_wr32(falcon, 0x010, 0x000004f3);	/* irqmset */
	nvkm_falcon_wr32(falcon, 0x01c, 0x00010452);	/* irqdest */
	nvkm_falcon_start(falcon);
	return 0;
}

static const struct nvkm_pmu_func
gv11b_pmu = {
	.flcn = &gv11b_pmu_flcn,
	.init = gv11b_pmu_init,
	.fini = gm20b_pmu_fini,
	.intr = gt215_pmu_intr,
	.recv = gm20b_pmu_recv,
	.initmsg = gm20b_pmu_initmsg,
	.reset = gf100_pmu_reset,
};

static int
gv11b_pmu_load(struct nvkm_pmu *pmu, int ver, const struct nvkm_pmu_fwif *fwif)
{
	return nvkm_acr_lsfw_load_sig_image_desc_v1(&pmu->subdev, &pmu->falcon,
						    NVKM_ACR_LSF_PMU, "pmu/",
						    ver, fwif->acr);
}

MODULE_FIRMWARE("nvidia/gv11b/pmu/desc.bin");
MODULE_FIRMWARE("nvidia/gv11b/pmu/image.bin");
MODULE_FIRMWARE("nvidia/gv11b/pmu/sig.bin");

static const struct nvkm_pmu_fwif
gv11b_pmu_fwif[] = {
	{  0, gv11b_pmu_load, &gv11b_pmu, &gv11b_pmu_acr },
	{ -1, gm200_pmu_nofw, &gv11b_pmu },
	{}
};

int
gv11b_pmu_new(struct nvkm_device *device, enum nvkm_subdev_type type, int inst,
	      struct nvkm_pmu **ppmu)
{
	return nvkm_pmu_new_(gv11b_pmu_fwif, device, type, inst, ppmu);
}
