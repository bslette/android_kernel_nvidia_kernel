/*
 * Copyright 2017 Red Hat Inc.
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
#include "vmm.h"

static const struct nvkm_vmm_func
gp10b_vmm = {
	.join = gp100_vmm_join,
	.part = gf100_vmm_part,
	.aper = gk20a_vmm_aper,
	.valid = gp100_vmm_valid,
	.flush = gp100_vmm_flush,
	.mthd = gp100_vmm_mthd,
	.invalidate_pdb = gp100_vmm_invalidate_pdb,
	.page = {
		{ 47, &gp100_vmm_desc_16[4], NVKM_VMM_PAGE_Sxxx },
		{ 38, &gp100_vmm_desc_16[3], NVKM_VMM_PAGE_Sxxx },
		{ 29, &gp100_vmm_desc_16[2], NVKM_VMM_PAGE_Sxxx },
		{ 21, &gp100_vmm_desc_16[1], NVKM_VMM_PAGE_SxHC },
		{ 16, &gp100_vmm_desc_16[0], NVKM_VMM_PAGE_SxHC },
		{ 12, &gp100_vmm_desc_12[0], NVKM_VMM_PAGE_SxHx },
		{}
	}
};

int
gp10b_vmm_new(struct nvkm_mmu *mmu, bool managed, u64 addr, u64 size,
	      void *argv, u32 argc, struct lock_class_key *key,
	      const char *name, struct nvkm_vmm **pvmm)
{
	return gp100_vmm_new_(&gp10b_vmm, mmu, managed, addr, size,
			      argv, argc, key, name, pvmm);
}

/* Tegra194 is IO-coherent: map all sysmem as coherent (aperture 2). */
static int
gv11b_vmm_aper(enum nvkm_memory_target target)
{
	switch (target) {
	case NVKM_MEM_TARGET_HOST:
	case NVKM_MEM_TARGET_NCOH: return 2;
	default:
		return -EINVAL;
	}
}

/*
 * GV11B: GP10B's sysmem-only layout, Volta instance blocks (subcontext PDBs),
 * and real sysmem apertures in PTEs: unlike earlier Tegra GPUs, GV11B faults
 * the VRAM aperture (MMU fault type 0xa, UNSUPPORTED_APERTURE, on the PMU's
 * first ACR code DMA).
 */
static const struct nvkm_vmm_func
gv11b_vmm = {
	.join = gv100_vmm_join,
	.part = gf100_vmm_part,
	.aper = gv11b_vmm_aper,
	.valid = gp100_vmm_valid,
	.flush = gp100_vmm_flush,
	.mthd = gp100_vmm_mthd,
	.invalidate_pdb = gp100_vmm_invalidate_pdb,
	.page = {
		{ 47, &gp100_vmm_desc_16[4], NVKM_VMM_PAGE_Sxxx },
		{ 38, &gp100_vmm_desc_16[3], NVKM_VMM_PAGE_Sxxx },
		{ 29, &gp100_vmm_desc_16[2], NVKM_VMM_PAGE_Sxxx },
		{ 21, &gp100_vmm_desc_16[1], NVKM_VMM_PAGE_SxHC },
		{ 16, &gp100_vmm_desc_16[0], NVKM_VMM_PAGE_SxHC },
		{ 12, &gp100_vmm_desc_12[0], NVKM_VMM_PAGE_SxHx },
		{}
	}
};

int
gv11b_vmm_new(struct nvkm_mmu *mmu, bool managed, u64 addr, u64 size,
	      void *argv, u32 argc, struct lock_class_key *key,
	      const char *name, struct nvkm_vmm **pvmm)
{
	return gp100_vmm_new_(&gv11b_vmm, mmu, managed, addr, size,
			      argv, argc, key, name, pvmm);
}
