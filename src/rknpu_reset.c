// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 *
 * Target: RK3566/RK3568 | IOMMU | Linux Kernel >= 7.2.0
 */

#include <linux/delay.h>
#include <linux/iommu.h>

#include "rknpu_reset.h"

int rknpu_reset_get(struct rknpu_device *rknpu_dev)
{
	int i, num_srsts;

	num_srsts = of_count_phandle_with_args(rknpu_dev->dev->of_node,
					       "resets", "#reset-cells");
	if (num_srsts <= 0) {
		LOG_DEV_ERROR(rknpu_dev->dev,
			      "failed to get rknpu resets from dtb\n");
		return num_srsts;
	}

	rknpu_dev->srsts = devm_kcalloc(rknpu_dev->dev, num_srsts,
					sizeof(*rknpu_dev->srsts), GFP_KERNEL);
	if (!rknpu_dev->srsts)
		return -ENOMEM;

	for (i = 0; i < num_srsts; ++i) {
		rknpu_dev->srsts[i] = devm_reset_control_get_exclusive_by_index(
			rknpu_dev->dev, i);
		if (IS_ERR(rknpu_dev->srsts[i])) {
			rknpu_dev->num_srsts = i;
			return PTR_ERR(rknpu_dev->srsts[i]);
		}
	}

	rknpu_dev->num_srsts = num_srsts;
	return num_srsts;
}

static int rknpu_reset_assert(struct reset_control *rst)
{
	int ret;

	if (!rst)
		return -EINVAL;

	ret = reset_control_assert(rst);
	if (ret < 0) {
		LOG_ERROR("failed to assert rknpu reset: %d\n", ret);
		return ret;
	}
	return 0;
}

static int rknpu_reset_deassert(struct reset_control *rst)
{
	int ret;

	if (!rst)
		return -EINVAL;

	ret = reset_control_deassert(rst);
	if (ret < 0) {
		LOG_ERROR("failed to deassert rknpu reset: %d\n", ret);
		return ret;
	}
	return 0;
}

int rknpu_soft_reset(struct rknpu_device *rknpu_dev)
{
	struct iommu_domain *domain;
	int ret = 0, i;

	if (rknpu_dev->bypass_soft_reset) {
		LOG_WARN("bypass soft reset\n");
		return 0;
	}

	if (!mutex_trylock(&rknpu_dev->reset_lock))
		return 0;

	rknpu_dev->soft_reseting = true;
	msleep(100);

	wake_up(&rknpu_dev->subcore_data.job_done_wq);

	LOG_INFO("soft reset, num: %d\n", rknpu_dev->num_srsts);

	for (i = 0; i < rknpu_dev->num_srsts; ++i)
		ret |= rknpu_reset_assert(rknpu_dev->srsts[i]);

	udelay(10);

	for (i = 0; i < rknpu_dev->num_srsts; ++i)
		ret |= rknpu_reset_deassert(rknpu_dev->srsts[i]);

	udelay(10);

	if (ret) {
		LOG_DEV_ERROR(rknpu_dev->dev,
			      "failed to soft reset for rknpu: %d\n", ret);
		mutex_unlock(&rknpu_dev->reset_lock);
		return ret;
	}

	domain = iommu_get_domain_for_dev(rknpu_dev->dev);
	if (domain) {
		iommu_detach_device(domain, rknpu_dev->dev);
		iommu_attach_device(domain, rknpu_dev->dev);
	}

	rknpu_dev->soft_reseting = false;

	if (rknpu_dev->config->state_init)
		rknpu_dev->config->state_init(rknpu_dev);

	mutex_unlock(&rknpu_dev->reset_lock);
	return 0;
}
