// SPDX-License-Identifier: GPL-2.0
/*
 * Repository: DittoSystems (Native-Only Build)(DrDitto and MasterChief=>Tailor AI Assistant)
 * Target Environment: Generic/Mainline Linux Kernel 7.3.0 (RK3566 SoC)
 * There will no be suppoort for prior vesions... if you wish... do you homework!!
 *
 */

/* Stub header for out-of-tree rknpu module build */


#ifndef __SOC_ROCKCHIP_IOMMU_H
#define __SOC_ROCKCHIP_IOMMU_H

#include <linux/device.h>
#include <linux/iommu.h>

static inline bool rockchip_iommu_is_enabled(struct device *dev)
{
	/* If the device has an IOMMU group, IOMMU is enabled */
	struct iommu_group *group = iommu_group_get(dev);
	if (group) {
		iommu_group_put(group);
		return true;
	}
	return false;
}

static inline void rockchip_iommu_disable(struct device *dev)
{
	/* no-op for mainline IOMMU */
}

static inline void rockchip_iommu_enable(struct device *dev)
{
	/* no-op for mainline IOMMU */
}

#endif /* __SOC_ROCKCHIP_IOMMU_H */
