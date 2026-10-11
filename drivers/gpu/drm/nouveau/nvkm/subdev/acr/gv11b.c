/* SPDX-License-Identifier: MIT
 *
 * GV11B (Tegra194) ACR.
 *
 * The HS ACR ucode runs on the PMU falcon like on GM20B/GP10B, with the LS
 * blob in system memory and the WPR window programmed by the bootloader.
 * Unlike GP10B it takes the v1 descriptors used by GP102+: flcn_acr_desc_v1,
 * flcn_bl_dmem_desc_v1 for the HS bootloader, and v1 WPR/LSB headers with
 * 192-byte LS signatures. This mirrors NVIDIA's R32 nvgpu
 * (gv11b_acr_patch_wpr_info_to_ucode, gp106_prepare_ucode_blob).
 */
#include "priv.h"

#include <core/memory.h>

#include <nvfw/acr.h>
#include <nvfw/flcn.h>

static int
gv11b_acr_load_setup(struct nvkm_falcon_fw *fw)
{
	struct flcn_acr_desc_v1 *desc = (void *)&fw->fw.img[fw->dmem_base_img];
	struct nvkm_acr *acr = fw->falcon->owner->device->acr;

	desc->wpr_offset = 0;
	desc->regions.no_regions = 1;
	desc->ucode_blob_base = nvkm_memory_addr(acr->wpr);
	desc->ucode_blob_size = nvkm_memory_size(acr->wpr);
	flcn_acr_desc_v1_dump(&acr->subdev, desc);
	return 0;
}

static const struct nvkm_falcon_fw_func
gv11b_acr_load_0 = {
	.signature = gm200_flcn_fw_signature,
	.reset = gm200_flcn_fw_reset,
	.setup = gv11b_acr_load_setup,
	.load = gm200_flcn_fw_load,
	.load_bld = gm200_acr_hsfw_load_bld,
	.boot = gm200_flcn_fw_boot,
};

MODULE_FIRMWARE("nvidia/gv11b/acr/bl.bin");
MODULE_FIRMWARE("nvidia/gv11b/acr/ucode_load.bin");

static const struct nvkm_acr_hsf_fwif
gv11b_acr_load_fwif[] = {
	{ 0, gm200_acr_hsfw_ctor, &gv11b_acr_load_0, NVKM_ACR_HSF_PMU, 0, 0x00000010 },
	{}
};

static const struct nvkm_acr_func
gv11b_acr = {
	.load = gv11b_acr_load_fwif,
	.wpr_parse = gp102_acr_wpr_parse,
	.wpr_layout = gp102_acr_wpr_layout,
	.wpr_alloc = gm20b_acr_wpr_alloc,
	.wpr_build = gp102_acr_wpr_build,
	.wpr_patch = gp102_acr_wpr_patch,
	.wpr_check = gm200_acr_wpr_check,
	.init = gm200_acr_init,
};

static const struct nvkm_acr_fwif
gv11b_acr_fwif[] = {
	{  0, gm20b_acr_load, &gv11b_acr },
	{ -1, gm200_acr_nofw, &gm200_acr },
	{}
};

int
gv11b_acr_new(struct nvkm_device *device, enum nvkm_subdev_type type, int inst,
	      struct nvkm_acr **pacr)
{
	return nvkm_acr_new_(gv11b_acr_fwif, device, type, inst, pacr);
}
