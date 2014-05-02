/*
 * GM20B GPC MMU
 *
 * Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/types.h>

#include "gk20a/gk20a.h"
#include "gk20a/kind_gk20a.h"

#include "hw_fb_gm20b.h"
#include "hw_top_gm20b.h"
#include "hw_gmmu_gm20b.h"

static void fb_gm20b_init_fs_state(struct gk20a *g)
{
	gk20a_dbg_info("initialize gm20b fb");

	gk20a_writel(g, fb_fbhub_num_active_ltcs_r(),
			g->ltc_count);
}

static void gm20b_init_uncompressed_kind_map(void)
{
	gk20a_init_uncompressed_kind_map();

	gk20a_uc_kind_map[gmmu_pte_kind_s8_v()] =
	gk20a_uc_kind_map[gmmu_pte_kind_s8_2s_v()] =
		gmmu_pte_kind_s8_v();
}

static bool gm20b_kind_supported(u8 k)
{
	return k == gmmu_pte_kind_smsked_message_v()
		|| (k >= gmmu_pte_kind_s8_v() &&
		    k <= gmmu_pte_kind_s8_2s_v());
}

static bool gm20b_kind_z(u8 k)
{
	return (k >= gmmu_pte_kind_s8_v() &&
		 k <= gmmu_pte_kind_s8_2s_v());
}

static bool gm20b_kind_compressible(u8 k)
{
	return (k >= gmmu_pte_kind_s8_v() &&
		 k <= gmmu_pte_kind_s8_2s_v());
}

static bool gm20b_kind_zbc(u8 k)
{
	return (k >= gmmu_pte_kind_s8_v() &&
		 k <= gmmu_pte_kind_s8_2s_v());
}

void gm20b_init_kind_attr(void)
{
	u16 k;

	gk20a_init_kind_attr();

	for (k = 0; k < 256; k++) {
		if (gm20b_kind_supported((u8)k))
			gk20a_kind_attr[k] |= GK20A_KIND_ATTR_SUPPORTED;
		if (gm20b_kind_compressible((u8)k))
			gk20a_kind_attr[k] |= GK20A_KIND_ATTR_COMPRESSIBLE;
		if (gm20b_kind_z((u8)k))
			gk20a_kind_attr[k] |= GK20A_KIND_ATTR_Z;
		if (gm20b_kind_zbc((u8)k))
			gk20a_kind_attr[k] |= GK20A_KIND_ATTR_ZBC;
	}
}

void gm20b_init_fb(struct gpu_ops *gops)
{
	gops->fb.init_fs_state = fb_gm20b_init_fs_state;
	gm20b_init_uncompressed_kind_map();
	gm20b_init_kind_attr();
}
