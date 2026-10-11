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
	{  0, gv11b_pmu_load, &gm20b_pmu, &gv11b_pmu_acr },
	{ -1, gm200_pmu_nofw, &gm20b_pmu },
	{}
};

int
gv11b_pmu_new(struct nvkm_device *device, enum nvkm_subdev_type type, int inst,
	      struct nvkm_pmu **ppmu)
{
	return nvkm_pmu_new_(gv11b_pmu_fwif, device, type, inst, ppmu);
}
