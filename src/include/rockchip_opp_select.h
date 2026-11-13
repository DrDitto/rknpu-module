// SPDX-License-Identifier: GPL-2.0
/*
 * Repository: DittoSystems (Native-Only Build)(DrDitto and MasterChief=>Tailor AI Assistant)
 * Target Environment: Generic/Mainline Linux Kernel 7.3.0 (RK3566 SoC)
 * There will no be suppoort for prior vesions... if you wish... do you homework!!
 *
 */

/* Stub header for out-of-tree rknpu module build */

#ifndef __SOC_ROCKCHIP_OPP_SELECT_H
#define __SOC_ROCKCHIP_OPP_SELECT_H

#include <linux/of.h>

struct rockchip_opp_info {
	int dummy;
};

static inline int rockchip_nvmem_cell_read_u8(struct device_node *np,
					      const char *cell_id, u8 *val)
{
	return -ENOSYS;
}

#endif /* __SOC_ROCKCHIP_OPP_SELECT_H */
