// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 *
 * Target: RK3566/RK3568 | Linux Kernel >= 7.2.0
 */

#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/devfreq-governor.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include "rknpu_drv.h"
#include "rknpu_devfreq.h"

static int devfreq_rknpu_ondemand_func(struct devfreq *df,
				       unsigned long *freq)
{
	struct rknpu_device *rknpu_dev = df->data;

	if (rknpu_dev && rknpu_dev->ondemand_freq)
		*freq = rknpu_dev->ondemand_freq;
	else
		*freq = df->previous_freq;

	return 0;
}

static int devfreq_rknpu_ondemand_handler(struct devfreq *devfreq,
					  unsigned int event, void *data)
{
	return 0;
}

static struct devfreq_governor devfreq_rknpu_ondemand = {
	.name = "rknpu_ondemand",
	.get_target_freq = devfreq_rknpu_ondemand_func,
	.event_handler = devfreq_rknpu_ondemand_handler,
};

static int npu_devfreq_target(struct device *dev, unsigned long *freq,
			      u32 flags)
{
	struct rknpu_device *rknpu_dev = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	unsigned long opp_volt;
	int ret;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	opp_volt = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);

	if (*freq == rknpu_dev->current_freq)
		return 0;

	mutex_lock(&rknpu_dev->devfreq_lock);
	ret = dev_pm_opp_set_rate(dev, *freq);
	if (!ret) {
		rknpu_dev->current_freq = *freq;
		if (rknpu_dev->devfreq)
			rknpu_dev->devfreq->last_status.current_frequency =
				*freq;
		rknpu_dev->current_volt = opp_volt;
		LOG_DEV_DEBUG(dev, "set rknpu freq: %lu, volt: %lu\n",
			      rknpu_dev->current_freq,
			      rknpu_dev->current_volt);
	}
	mutex_unlock(&rknpu_dev->devfreq_lock);

	return ret;
}

static int npu_devfreq_get_dev_status(struct device *dev,
				      struct devfreq_dev_status *stat)
{
	struct rknpu_device *rknpu_dev = dev_get_drvdata(dev);
	unsigned long flags;
	ktime_t busy;

	stat->current_frequency = rknpu_dev->current_freq;

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	busy = rknpu_dev->subcore_data.timer.total_busy_time;
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	stat->busy_time = ktime_to_us(busy);
	stat->total_time = RKNPU_LOAD_INTERVAL / 1000;

	return 0;
}

static int npu_devfreq_get_cur_freq(struct device *dev, unsigned long *freq)
{
	struct rknpu_device *rknpu_dev = dev_get_drvdata(dev);

	*freq = rknpu_dev->current_freq;
	return 0;
}

static struct devfreq_dev_profile npu_devfreq_profile = {
	.polling_ms = 50,
	.target = npu_devfreq_target,
	.get_dev_status = npu_devfreq_get_dev_status,
	.get_cur_freq = npu_devfreq_get_cur_freq,
};

void rknpu_devfreq_lock(struct rknpu_device *rknpu_dev)
{
	if (rknpu_dev->devfreq)
		mutex_lock(&rknpu_dev->devfreq_lock);
}
EXPORT_SYMBOL(rknpu_devfreq_lock);

void rknpu_devfreq_unlock(struct rknpu_device *rknpu_dev)
{
	if (rknpu_dev->devfreq)
		mutex_unlock(&rknpu_dev->devfreq_lock);
}
EXPORT_SYMBOL(rknpu_devfreq_unlock);

int rknpu_devfreq_init(struct rknpu_device *rknpu_dev)
{
	struct device *dev = rknpu_dev->dev;
	struct devfreq_dev_profile *dp = &npu_devfreq_profile;
	struct dev_pm_opp *opp;
	int ret;
	static const char * const reg_names[] = { "rknpu", NULL };

	ret = devm_pm_opp_set_regulators(dev, reg_names);
	if (ret) {
		LOG_DEV_ERROR(dev, "failed to set OPP regulators: %d\n", ret);
		return ret;
	}

	ret = devm_pm_opp_set_clkname(dev, "scmi_clk");
	if (ret) {
		LOG_DEV_ERROR(dev, "failed to set OPP clkname: %d\n", ret);
		return ret;
	}

	ret = devm_pm_opp_of_add_table(dev);
	if (ret) {
		LOG_DEV_ERROR(dev, "failed to add OPP table: %d\n", ret);
		return ret;
	}

	rknpu_dev->current_freq = clk_get_rate(rknpu_dev->clks[0].clk);

	opp = devfreq_recommended_opp(dev, &rknpu_dev->current_freq, 0);
	if (IS_ERR(opp)) {
		ret = PTR_ERR(opp);
		LOG_DEV_ERROR(dev, "failed to get recommended OPP: %d\n", ret);
		return ret;
	}
	dev_pm_opp_put(opp);

	dp->initial_freq = rknpu_dev->current_freq;

	ret = devfreq_add_governor(&devfreq_rknpu_ondemand);
	if (ret) {
		LOG_DEV_ERROR(dev, "failed to add rknpu_ondemand governor: %d\n",
			      ret);
		return ret;
	}

	rknpu_dev->devfreq = devm_devfreq_add_device(dev, dp,
						      "rknpu_ondemand",
						      (void *)rknpu_dev);
	if (IS_ERR(rknpu_dev->devfreq)) {
		LOG_DEV_ERROR(dev, "failed to add devfreq device\n");
		ret = PTR_ERR(rknpu_dev->devfreq);
		rknpu_dev->devfreq = NULL;
		goto err_remove_governor;
	}

	rknpu_dev->current_freq = clk_get_rate(rknpu_dev->clks[0].clk);
	rknpu_dev->ondemand_freq = rknpu_dev->current_freq;
	if (rknpu_dev->vdd)
		rknpu_dev->current_volt = regulator_get_voltage(rknpu_dev->vdd);

	rknpu_dev->devfreq->previous_freq = rknpu_dev->current_freq;
	if (rknpu_dev->devfreq->suspend_freq)
		rknpu_dev->devfreq->resume_freq = rknpu_dev->current_freq;
	rknpu_dev->devfreq->last_status.current_frequency =
		rknpu_dev->current_freq;
	rknpu_dev->devfreq->last_status.total_time = 1;
	rknpu_dev->devfreq->last_status.busy_time = 1;

	LOG_DEV_INFO(dev, "devfreq enabled, initial freq: %lu Hz, volt: %lu uV\n",
		     rknpu_dev->current_freq, rknpu_dev->current_volt);

	return 0;

err_remove_governor:
	devfreq_remove_governor(&devfreq_rknpu_ondemand);
	return ret;
}
EXPORT_SYMBOL(rknpu_devfreq_init);

void rknpu_devfreq_remove(struct rknpu_device *rknpu_dev)
{
	if (rknpu_dev->devfreq)
		devfreq_remove_governor(&devfreq_rknpu_ondemand);
}
EXPORT_SYMBOL(rknpu_devfreq_remove);

int rknpu_devfreq_runtime_suspend(struct device *dev)
{
	return 0;
}
EXPORT_SYMBOL(rknpu_devfreq_runtime_suspend);

int rknpu_devfreq_runtime_resume(struct device *dev)
{
	return 0;
}
EXPORT_SYMBOL(rknpu_devfreq_runtime_resume);
