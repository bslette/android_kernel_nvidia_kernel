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

static void
gv11b_pmu_acr_bld_patch(struct nvkm_acr *acr, u32 bld, s64 adjust)
{
	struct loader_config_v1 hdr;

	nvkm_robj(acr->wpr, bld, &hdr, sizeof(hdr));
	hdr.code_dma_base = hdr.code_dma_base + adjust;
	hdr.data_dma_base = hdr.data_dma_base + adjust;
	hdr.overlay_dma_base = hdr.overlay_dma_base + adjust;
	nvkm_wobj(acr->wpr, bld, &hdr, sizeof(hdr));
	loader_config_v1_dump(&acr->subdev, &hdr);
}

static void
gv11b_pmu_acr_bld_write(struct nvkm_acr *acr, u32 bld,
			struct nvkm_acr_lsfw *lsfw)
{
	const u64 base = lsfw->offset.img + lsfw->app_start_offset;
	const struct loader_config_v1 hdr = {
		.dma_idx = FALCON_DMAIDX_UCODE,
		.code_dma_base = base + lsfw->app_resident_code_offset,
		.code_size_total = lsfw->app_size,
		.code_size_to_load = lsfw->app_resident_code_size,
		.code_entry_point = lsfw->app_imem_entry,
		.data_dma_base = base + lsfw->app_resident_data_offset,
		.data_size = lsfw->app_resident_data_size,
		.overlay_dma_base = base + lsfw->app_resident_code_offset,
		.argc = 1,
		.argv = lsfw->falcon->data.limit - sizeof(struct nv_pmu_args),
	};

	nvkm_wobj(acr->wpr, bld, &hdr, sizeof(hdr));
}

static const struct nvkm_acr_lsf_func
gv11b_pmu_acr = {
	.flags = NVKM_ACR_LSF_DMACTL_REQ_CTX,
	.bld_size = sizeof(struct loader_config_v1),
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
	nvkm_falcon_wr32(falcon, 0xe00, 5); /* DMAIDX_UCODE: physical, coh sysmem */
	nvkm_falcon_wr32(falcon, 0xe04, 0); /* DMAIDX_VIRT */
	nvkm_falcon_wr32(falcon, 0xe08, 5); /* DMAIDX_PHYS_VID: physical, coh sysmem */
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

static const struct nvkm_pmu_func
gv11b_pmu = {
	.flcn = &gv11b_pmu_flcn,
	.init = gm20b_pmu_init,
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
