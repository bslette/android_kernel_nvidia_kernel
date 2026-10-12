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
#include "gf100.h"

#include <core/memory.h>

static const struct nvkm_bar_func
gm20b_bar_func = {
	.dtor = gf100_bar_dtor,
	.oneinit = gf100_bar_oneinit,
	.bar1.init = gf100_bar_bar1_init,
	.bar1.wait = gm107_bar_bar1_wait,
	.bar1.vmm = gf100_bar_bar1_vmm,
	.flush = g84_bar_flush,
};

int
gm20b_bar_new(struct nvkm_device *device, enum nvkm_subdev_type type, int inst,
	      struct nvkm_bar **pbar)
{
	int ret = gf100_bar_new_(&gm20b_bar_func, device, type, inst, pbar);
	if (ret == 0)
		(*pbar)->iomap_uncached = true;
	return ret;
}

/*
 * GV11B: the BAR1 block pointer needs its aperture (bits 29:28) like every
 * other instance pointer on this chip; with 0 (VRAM) BAR1 accesses (e.g.
 * FIFO-owned USERD, GP_PUT) never reach memory. nvgpu uses 3 (ncoh) with
 * uncached memory; nouveau's instmem is cached on this IO-coherent SoC.
 */
static void
gv11b_bar_bar1_init(struct nvkm_bar *base)
{
	struct nvkm_device *device = base->subdev.device;
	struct gf100_bar *bar = gf100_bar(base);
	const u32 addr = nvkm_memory_addr(bar->bar[1].inst) >> 12;

	nvkm_wr32(device, 0x001704, 0xa0000000 | addr);
}

/*
 * GV11B BAR2: GPU-internal address space (Volta CE method buffers live in
 * BAR2; with the block pointer unset the copy engine faulted with
 * UNBOUND_INST_BLOCK on every mid-draw preemption and the ctxsw hung).
 * nvgpu writes 0x1714 = valid | aperture | addr, like 0x1704.
 */
static void
gv11b_bar_bar2_init(struct nvkm_bar *base)
{
	struct nvkm_device *device = base->subdev.device;
	struct gf100_bar *bar = gf100_bar(base);
	const u32 addr = nvkm_memory_addr(bar->bar[0].inst) >> 12;

	nvkm_wr32(device, 0x001714, 0xa0000000 | addr);
}

static const struct nvkm_bar_func
gv11b_bar_func = {
	.dtor = gf100_bar_dtor,
	.oneinit = gf100_bar_oneinit,
	.bar1.init = gv11b_bar_bar1_init,
	.bar1.wait = gm107_bar_bar1_wait,
	.bar1.vmm = gf100_bar_bar1_vmm,
	.bar2.init = gv11b_bar_bar2_init,
	.bar2.fini = gf100_bar_bar2_fini,
	.bar2.wait = gm107_bar_bar2_wait,
	.bar2.vmm = gf100_bar_bar2_vmm,
	.flush = g84_bar_flush,
};

int
gv11b_bar_new(struct nvkm_device *device, enum nvkm_subdev_type type, int inst,
	      struct nvkm_bar **pbar)
{
	int ret = gf100_bar_new_(&gv11b_bar_func, device, type, inst, pbar);
	if (ret == 0)
		(*pbar)->iomap_uncached = true;
	return ret;
}
