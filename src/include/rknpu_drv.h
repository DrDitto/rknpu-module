/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 *
 * Target: RK3566/RK3568 | DRM GEM + IOMMU | Linux Kernel >= 7.2.0
 */

#ifndef __LINUX_RKNPU_DRV_H_
#define __LINUX_RKNPU_DRV_H_

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/kref.h>
#include <linux/irq.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/regulator/consumer.h>
#include <linux/version.h>
#include <linux/hrtimer.h>
#include <drm/drm_device.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 2, 0)
#error "This driver requires Linux Kernel 7.2.0 or newer"
#endif

#include "rknpu_job.h"
#include "rknpu_fence.h"
#include "rknpu_debugger.h"

#define DRIVER_NAME		"rknpu"
#define DRIVER_DESC		"RKNPU driver for RK3566/RK3568"
#define DRIVER_MAJOR		1
#define DRIVER_MINOR		0
#define DRIVER_PATCHLEVEL	0

#define LOG_TAG "RKNPU"

#define RKNPU_LOAD_INTERVAL 1000000000

#define LOG_INFO(fmt, args...)	pr_info(LOG_TAG ": " fmt, ##args)
#define LOG_WARN(fmt, args...)	pr_warn(LOG_TAG ": " fmt, ##args)
#define LOG_DEBUG(fmt, args...)	pr_devel(LOG_TAG ": " fmt, ##args)
#define LOG_ERROR(fmt, args...)	pr_err(LOG_TAG ": " fmt, ##args)

#define LOG_DEV_INFO(dev, fmt, args...)		dev_info(dev, LOG_TAG ": " fmt, ##args)
#define LOG_DEV_WARN(dev, fmt, args...)		dev_warn(dev, LOG_TAG ": " fmt, ##args)
#define LOG_DEV_DEBUG(dev, fmt, args...)	dev_dbg(dev, LOG_TAG ": " fmt, ##args)
#define LOG_DEV_ERROR(dev, fmt, args...)	dev_err(dev, LOG_TAG ": " fmt, ##args)

#define RKNPU_MAX_IOMMU_DOMAIN_NUM	16
#define RKNPU_CACHE_SG_TABLE_NUM	2

struct rknpu_amount_data {
	uint16_t offset_clr_all;
	uint16_t offset_dt_wr;
	uint16_t offset_dt_rd;
	uint16_t offset_wt_rd;
};

struct rknpu_config {
	__u32 bw_priority_addr;
	__u32 bw_priority_length;
	__u64 dma_mask;
	__u32 pc_data_amount_scale;
	__u32 pc_task_number_bits;
	__u32 pc_task_number_mask;
	__u32 pc_task_status_offset;
	__u32 pc_dma_ctrl;
	__u64 max_submit_number;
	__u32 core_mask;
	const struct rknpu_amount_data *amount_top;
	const struct rknpu_amount_data *amount_core;
	void (*state_init)(struct rknpu_device *rknpu_dev);
	int (*cache_sgt_init)(struct rknpu_device *rknpu_dev);
};

struct rknpu_timer {
	ktime_t busy_time;
	ktime_t total_busy_time;
};

struct rknpu_subcore_data {
	struct list_head todo_list;
	wait_queue_head_t job_done_wq;
	struct rknpu_job *job;
	int64_t task_num;
	struct rknpu_timer timer;
};

struct rknpu_device {
	void __iomem *base;
	struct device *dev;
	struct drm_device *drm_dev;
	atomic_t sequence;
	spinlock_t lock;
	spinlock_t irq_lock;
	struct mutex power_lock;
	struct mutex reset_lock;
	struct mutex domain_lock;
	struct rknpu_subcore_data subcore_data;
	const struct rknpu_config *config;
	void __iomem *bw_priority_base;
	struct rknpu_fence_context *fence_ctx;
	bool iommu_en;
	struct reset_control **srsts;
	int num_srsts;
	struct clk_bulk_data *clks;
	int num_clks;
	struct regulator *vdd;
	struct regulator *mem;
	struct thermal_cooling_device *devfreq_cooling;
	struct devfreq *devfreq;
	struct mutex devfreq_lock;
	unsigned long ondemand_freq;
	unsigned long current_freq;
	unsigned long current_volt;
	int bypass_irq_handler;
	int bypass_soft_reset;
	bool soft_reseting;
	atomic_t power_refcount;
	atomic_t cmdline_power_refcount;
	struct delayed_work power_off_work;
	struct workqueue_struct *power_off_wq;
	struct rknpu_debugger debugger;
	struct hrtimer timer;
	ktime_t kt;
	unsigned long power_put_delay;
	struct iommu_group *iommu_group;
	int iommu_domain_num;
	int iommu_domain_id;
	struct iommu_domain *iommu_domains[RKNPU_MAX_IOMMU_DOMAIN_NUM];
	struct sg_table *cache_sgt[RKNPU_CACHE_SG_TABLE_NUM];
	atomic_t iommu_domain_refcount;
};

struct rknpu_session {
	struct rknpu_device *rknpu_dev;
	struct list_head list;
};

int rknpu_power_get(struct rknpu_device *rknpu_dev);
int rknpu_power_put(struct rknpu_device *rknpu_dev);
int rknpu_power_put_delay(struct rknpu_device *rknpu_dev);

#endif /* __LINUX_RKNPU_DRV_H_ */
