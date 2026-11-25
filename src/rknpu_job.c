// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 *
 * Target: RK3566/RK3568 (Single-Core) | DRM GEM + IOMMU | Kernel >= 7.2.0
 */

#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/sync_file.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>

#include "rknpu_ioctl.h"
#include "rknpu_drv.h"
#include "rknpu_reset.h"
#include "rknpu_gem.h"
#include "rknpu_fence.h"
#include "rknpu_iommu.h"
#include "rknpu_job.h"

#define _REG_READ(base, offset)		readl(base + (offset))
#define _REG_WRITE(base, value, offset)	writel(value, base + (offset))

#define REG_READ(offset)		_REG_READ(rknpu_dev->base, offset)
#define REG_WRITE(value, offset)	_REG_WRITE(rknpu_dev->base, value, offset)

static void rknpu_job_free(struct rknpu_job *job)
{
	if (!job)
		return;

	if (job->args && job->args->task_number > 0) {
		struct rknpu_gem_object *task_obj = NULL;

		if (job->args->task_obj_addr <= 0x0000ffffffffffffULL)
			task_obj = (struct rknpu_gem_object *)(uintptr_t)
					   job->args->task_obj_addr;

		if (task_obj && virt_addr_valid(task_obj))
			rknpu_gem_object_put(&task_obj->base);
	}

	if (job->fence && virt_addr_valid(job->fence))
		dma_fence_put(job->fence);

	kfree(job);
}

static void rknpu_job_cleanup_work(struct work_struct *work)
{
	struct rknpu_job *job =
		container_of(work, struct rknpu_job, cleanup_work);

	rknpu_job_free(job);
}

static inline struct rknpu_job *rknpu_job_alloc(struct rknpu_device *rknpu_dev,
						struct rknpu_submit *args)
{
	struct rknpu_job *job;
	struct rknpu_gem_object *task_obj;
	bool need_copy = !!(args->flags & RKNPU_JOB_NONBLOCK);

	if (need_copy)
		job = kzalloc(sizeof(*job) + sizeof(*args), GFP_KERNEL);
	else
		job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return NULL;

	job->timestamp = ktime_get();
	job->rknpu_dev = rknpu_dev;
	job->use_core_num = 1;
	atomic_set(&job->run_count, 1);
	atomic_set(&job->interrupt_count, 1);
	atomic_set(&job->submit_count, 0);
	job->iommu_domain_id = args->iommu_domain_id;

	task_obj = (struct rknpu_gem_object *)(uintptr_t)args->task_obj_addr;
	if (task_obj)
		rknpu_gem_object_get(&task_obj->base);

	if (!need_copy) {
		job->args = args;
		job->args_owner = false;
		return job;
	}

	job->args = (struct rknpu_submit *)(job + 1);
	*job->args = *args;
	job->args_owner = true;
	INIT_WORK(&job->cleanup_work, rknpu_job_cleanup_work);

	return job;
}

static inline int rknpu_job_wait(struct rknpu_job *job)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_submit *args = job->args;
	struct rknpu_subcore_data *subcore_data = &rknpu_dev->subcore_data;
	struct rknpu_job *entry, *q;
	unsigned long flags;
	int wait_count = 0;
	bool continue_wait = false;
	int ret;

	do {
		ret = wait_event_timeout(subcore_data->job_done_wq,
					 (job->flags & RKNPU_JOB_DONE) ||
						 rknpu_dev->soft_reseting,
					 msecs_to_jiffies(args->timeout));

		if (++wait_count >= 3)
			break;

		if (ret == 0) {
			int64_t elapse_time_us;

			spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
			elapse_time_us = ktime_us_delta(ktime_get(),
							job->hw_commit_time);
			continue_wait =
				(job->hw_commit_time == 0) ?
					true :
					(elapse_time_us < args->timeout * 1000);
			spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

			LOG_ERROR(
				"job: %p, domain: %d/%d, wait_count: %d, continue: %d, commit_us: %lld, wait_us: %lld, timeout_us: %u\n",
				job, job->iommu_domain_id,
				rknpu_dev->iommu_domain_id, wait_count,
				continue_wait,
				(job->hw_commit_time == 0 ? 0 : elapse_time_us),
				ktime_us_delta(ktime_get(), job->timestamp),
				args->timeout * 1000);
		}
	} while (ret == 0 && continue_wait);

	if (!job->last_task) {
		spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
		list_for_each_entry_safe(entry, q, &subcore_data->todo_list, head) {
			if (entry == job) {
				list_del(&job->head);
				break;
			}
		}
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

		LOG_ERROR("job commit failed\n");
		return ret < 0 ? ret : -EINVAL;
	}

	job->last_task->int_status = job->int_status;

	if (ret <= 0) {
		args->task_counter = 0;
		if (args->flags & RKNPU_JOB_PC) {
			uint32_t task_status = REG_READ(
				rknpu_dev->config->pc_task_status_offset);
			args->task_counter =
				task_status &
				rknpu_dev->config->pc_task_number_mask;
		}

		LOG_ERROR(
			"failed to wait job, task counter: %d, flags: %#x, ret: %d, elapsed: %lldus\n",
			args->task_counter, args->flags, ret,
			ktime_us_delta(ktime_get(), job->timestamp));

		return ret < 0 ? ret : -ETIMEDOUT;
	}

	if (!(job->flags & RKNPU_JOB_DONE))
		return -EINVAL;

	args->task_counter = args->task_number;
	args->hw_elapse_time = job->hw_elapse_time;
	return 0;
}

static inline int rknpu_job_commit_pc(struct rknpu_job *job)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_submit *args = job->args;
	struct rknpu_gem_object *task_obj =
		(struct rknpu_gem_object *)(uintptr_t)args->task_obj_addr;
	struct rknpu_task *task_base, *first_task, *last_task;
	int task_start = args->task_start;
	int task_number = args->task_number;
	int task_end;
	int task_pp_en = (args->flags & RKNPU_JOB_PINGPONG) ? 1 : 0;
	int pc_data_amount_scale = rknpu_dev->config->pc_data_amount_scale;
	int pc_task_number_bits = rknpu_dev->config->pc_task_number_bits;
	int submit_index = atomic_read(&job->submit_count);
	int max_submit_number = rknpu_dev->config->max_submit_number;
	u32 base_dma_addr, regcmd_dma_addr;

	if (!task_obj) {
		job->ret = -EINVAL;
		return job->ret;
	}

	if (task_obj->dma_addr)
		base_dma_addr = task_obj->dma_addr;
	else if (task_obj->iova_start)
		base_dma_addr = task_obj->iova_start;
	else
		base_dma_addr = args->task_base_addr;

	args->task_base_addr = base_dma_addr;

	task_start += submit_index * max_submit_number;
	task_number -= submit_index * max_submit_number;
	if (task_number > max_submit_number)
		task_number = max_submit_number;
	task_end = task_start + task_number - 1;

	task_base = task_obj->kv_addr;
	if (!task_base) {
		LOG_ERROR("task_obj kv_addr is NULL!\n");
		job->ret = -EINVAL;
		return job->ret;
	}

	first_task = &task_base[task_start];
	last_task = &task_base[task_end];
	regcmd_dma_addr = args->task_base_addr +
			  ((void *)first_task - (void *)task_base);

	if (task_obj->flags & RKNPU_MEM_CACHEABLE) {
		dma_sync_single_range_for_device(
			rknpu_dev->dev, base_dma_addr,
			(unsigned long)((void *)first_task - (void *)task_base),
			(task_end - task_start + 1) * sizeof(struct rknpu_task),
			DMA_TO_DEVICE);
	}

	REG_WRITE(regcmd_dma_addr, RKNPU_OFFSET_PC_DATA_ADDR);
	REG_WRITE((first_task->regcfg_amount + RKNPU_PC_DATA_EXTRA_AMOUNT +
		   pc_data_amount_scale - 1) /
				  pc_data_amount_scale -
			  1,
		  RKNPU_OFFSET_PC_DATA_AMOUNT);
	REG_WRITE(last_task->int_mask, RKNPU_OFFSET_INT_MASK);
	REG_WRITE(first_task->int_mask, RKNPU_OFFSET_INT_CLEAR);
	REG_WRITE(((0x6 | task_pp_en) << pc_task_number_bits) | task_number,
		  RKNPU_OFFSET_PC_TASK_CONTROL);
	REG_WRITE(args->task_base_addr, RKNPU_OFFSET_PC_DMA_BASE_ADDR);

	job->first_task = first_task;
	job->last_task = last_task;
	job->int_mask = last_task->int_mask;

	REG_WRITE(0x1, RKNPU_OFFSET_PC_OP_EN);
	REG_WRITE(0x0, RKNPU_OFFSET_PC_OP_EN);

	return 0;
}
static void rknpu_job_commit(struct rknpu_job *job)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;

	REG_WRITE(0x1, RKNPU_OFFSET_PC_DATA_ADDR);

	if (!(job->args->flags & RKNPU_JOB_PC)) {
		job->ret = -EINVAL;
		return;
	}

	rknpu_job_commit_pc(job);
}

static void rknpu_job_next(struct rknpu_device *rknpu_dev)
{
	struct rknpu_job *job;
	struct rknpu_subcore_data *subcore_data = &rknpu_dev->subcore_data;
	unsigned long flags;

	if (rknpu_dev->soft_reseting)
		return;

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);

	if (subcore_data->job || list_empty(&subcore_data->todo_list)) {
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
		return;
	}

	job = list_first_entry(&subcore_data->todo_list, struct rknpu_job, head);
	list_del_init(&job->head);
	subcore_data->job = job;
	job->hw_commit_time = ktime_get();
	job->hw_recoder_time = job->hw_commit_time;

	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	if (atomic_dec_and_test(&job->run_count))
		rknpu_job_commit(job);
}

static void rknpu_job_done(struct rknpu_job *job, int ret)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_subcore_data *subcore_data = &rknpu_dev->subcore_data;
	ktime_t now;
	unsigned long flags;
	int max_submit_number = rknpu_dev->config->max_submit_number;

	if (atomic_inc_return(&job->submit_count) <
	    (job->args->task_number + max_submit_number - 1) / max_submit_number) {
		rknpu_job_commit(job);
		return;
	}

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	subcore_data->job = NULL;
	subcore_data->task_num -= job->args->task_number;
	now = ktime_get();
	job->hw_elapse_time = ktime_sub(now, job->hw_commit_time);
	subcore_data->timer.busy_time += ktime_sub(now, job->hw_recoder_time);
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	if (atomic_dec_and_test(&job->interrupt_count)) {
		rknpu_iommu_domain_put(rknpu_dev);

		job->flags |= RKNPU_JOB_DONE;
		job->ret = ret;

		if (job->fence)
			dma_fence_signal(job->fence);

		if (job->flags & RKNPU_JOB_ASYNC)
			schedule_work(&job->cleanup_work);

		wake_up(&subcore_data->job_done_wq);
	}

	rknpu_job_next(rknpu_dev);
}

static void rknpu_job_schedule(struct rknpu_job *job)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_subcore_data *subcore_data = &rknpu_dev->subcore_data;
	unsigned long flags;

	if (job->args->core_mask == RKNPU_CORE_AUTO_MASK)
		job->args->core_mask = RKNPU_CORE0_MASK;

	if (rknpu_iommu_domain_get_and_switch(rknpu_dev, job->iommu_domain_id)) {
		job->ret = -EINVAL;
		return;
	}

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	list_add_tail(&job->head, &subcore_data->todo_list);
	subcore_data->task_num += job->args->task_number;
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	rknpu_job_next(rknpu_dev);
}

static void rknpu_job_abort(struct rknpu_job *job)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_subcore_data *subcore_data = &rknpu_dev->subcore_data;
	unsigned long flags;

	rknpu_iommu_domain_put(rknpu_dev);
	msleep(100);

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	if (job == subcore_data->job && !job->irq_entry) {
		subcore_data->job = NULL;
		subcore_data->task_num -= job->args->task_number;
	}
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	if (job->ret == -ETIMEDOUT) {
		LOG_ERROR(
			"job timeout, flags: %#x, irq status: %#x, raw: %#x, mask: %#x, counter: %#x, elapsed: %lldus\n",
			job->flags, REG_READ(RKNPU_OFFSET_INT_STATUS),
			REG_READ(RKNPU_OFFSET_INT_RAW_STATUS), job->int_mask,
			(REG_READ(rknpu_dev->config->pc_task_status_offset) &
			 rknpu_dev->config->pc_task_number_mask),
			ktime_us_delta(ktime_get(), job->timestamp));
		rknpu_soft_reset(rknpu_dev);
	} else {
		LOG_ERROR("job abort, flags: %#x, ret: %d, elapsed: %lldus\n",
			  job->flags, job->ret,
			  ktime_us_delta(ktime_get(), job->timestamp));
	}

	rknpu_job_free(job);
}

static inline uint32_t rknpu_fuzz_status(uint32_t status)
{
	uint32_t fuzz_status = 0;

	if (status & 0x3)
		fuzz_status |= 0x3;
	if (status & 0xc)
		fuzz_status |= 0xc;
	if (status & 0x30)
		fuzz_status |= 0x30;
	if (status & 0xc0)
		fuzz_status |= 0xc0;
	if (status & 0x300)
		fuzz_status |= 0x300;
	if (status & 0xc00)
		fuzz_status |= 0xc00;

	return fuzz_status;
}

irqreturn_t rknpu_core0_irq_handler(int irq, void *data)
{
	struct rknpu_device *rknpu_dev = data;
	struct rknpu_subcore_data *subcore_data = &rknpu_dev->subcore_data;
	struct rknpu_job *job;
	uint32_t status;
	unsigned long flags;

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	job = subcore_data->job;
	if (!job) {
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
		REG_WRITE(RKNPU_INT_CLEAR, RKNPU_OFFSET_INT_CLEAR);
		rknpu_job_next(rknpu_dev);
		return IRQ_HANDLED;
	}
	job->irq_entry = true;
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	status = REG_READ(RKNPU_OFFSET_INT_STATUS);
	job->int_status = status;

	if (rknpu_fuzz_status(status) != job->int_mask) {
		LOG_ERROR(
			"invalid irq status: %#x, raw: %#x, mask: %#x, counter: %#x\n",
			status, REG_READ(RKNPU_OFFSET_INT_RAW_STATUS),
			job->int_mask,
			(REG_READ(rknpu_dev->config->pc_task_status_offset) &
			 rknpu_dev->config->pc_task_number_mask));
		REG_WRITE(RKNPU_INT_CLEAR, RKNPU_OFFSET_INT_CLEAR);
		return IRQ_HANDLED;
	}

	REG_WRITE(RKNPU_INT_CLEAR, RKNPU_OFFSET_INT_CLEAR);
	rknpu_job_done(job, 0);

	return IRQ_HANDLED;
}

irqreturn_t rknpu_platform_irq_handler(int irq, void *dev_id)
{
	return rknpu_core0_irq_handler(irq, dev_id);
}
EXPORT_SYMBOL(rknpu_platform_irq_handler);
static void rknpu_job_timeout_clean(struct rknpu_device *rknpu_dev)
{
	struct rknpu_subcore_data *subcore_data = &rknpu_dev->subcore_data;
	struct rknpu_job *job;
	unsigned long flags;

	job = subcore_data->job;
	if (!job ||
	    ktime_us_delta(ktime_get(), job->timestamp) < job->args->timeout)
		return;

	rknpu_soft_reset(rknpu_dev);

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	if (subcore_data->job == job)
		subcore_data->job = NULL;
	if (!list_empty(&job->head))
		list_del_init(&job->head);
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	schedule_work(&job->cleanup_work);

	do {
		spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
		if (!list_empty(&subcore_data->todo_list)) {
			job = list_first_entry(&subcore_data->todo_list,
					       struct rknpu_job, head);
			list_del_init(&job->head);
		} else {
			job = NULL;
		}
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

		if (job)
			schedule_work(&job->cleanup_work);
	} while (job);
}

static int rknpu_submit(struct rknpu_device *rknpu_dev,
			struct rknpu_submit *args)
{
	struct rknpu_job *job;
	int ret;

	if (args->task_number == 0) {
		LOG_ERROR("invalid rknpu task number!\n");
		return -EINVAL;
	}

	if (args->core_mask > RKNPU_CORE0_MASK) {
		LOG_ERROR("invalid rknpu core mask: %#x\n", args->core_mask);
		return -EINVAL;
	}

	job = rknpu_job_alloc(rknpu_dev, args);
	if (!job) {
		LOG_ERROR("failed to allocate rknpu job!\n");
		return -ENOMEM;
	}

	if (args->flags & RKNPU_JOB_FENCE_IN) {
#ifdef CONFIG_ROCKCHIP_RKNPU_FENCE
		struct dma_fence *in_fence = sync_file_get_fence(args->fence_fd);

		if (!in_fence) {
			LOG_ERROR("invalid fence in fd: %d\n", args->fence_fd);
			rknpu_job_free(job);
			return -EINVAL;
		}
		args->fence_fd = -1;

		ret = 0;
		if (!dma_fence_match_context(in_fence,
					     rknpu_dev->fence_ctx->context))
			ret = dma_fence_wait_timeout(in_fence, true,
						     args->timeout);
		dma_fence_put(in_fence);
		if (ret < 0) {
			rknpu_job_free(job);
			return ret;
		}
#else
		LOG_ERROR("rknpu fence config not enabled\n");
		rknpu_job_free(job);
		return -EINVAL;
#endif
	}

	if (args->flags & RKNPU_JOB_FENCE_OUT) {
#ifdef CONFIG_ROCKCHIP_RKNPU_FENCE
		ret = rknpu_fence_alloc(job);
		if (ret) {
			rknpu_job_free(job);
			return ret;
		}
		job->args->fence_fd = rknpu_fence_get_fd(job);
		args->fence_fd = job->args->fence_fd;
#else
		LOG_ERROR("rknpu fence config not enabled\n");
		rknpu_job_free(job);
		return -EINVAL;
#endif
	}

	if (args->flags & RKNPU_JOB_NONBLOCK) {
		job->flags |= RKNPU_JOB_ASYNC;
		rknpu_job_timeout_clean(rknpu_dev);
		rknpu_job_schedule(job);
		ret = job->ret;
		if (ret) {
			rknpu_job_abort(job);
			return ret;
		}
	} else {
		rknpu_job_schedule(job);
		if (args->flags & RKNPU_JOB_PC)
			job->ret = rknpu_job_wait(job);

		args->task_counter = job->args->task_counter;
		ret = job->ret;
		if (!ret)
			rknpu_job_free(job);
		else
			rknpu_job_abort(job);
	}

	return ret;
}

int rknpu_submit_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file_priv)
{
	struct rknpu_device *rknpu_dev = dev->dev_private;

	return rknpu_submit(rknpu_dev, (struct rknpu_submit *)data);
}

int rknpu_get_hw_version(struct rknpu_device *rknpu_dev, uint32_t *version)
{
	if (!version)
		return -EINVAL;

	*version = REG_READ(RKNPU_OFFSET_VERSION) +
		   (REG_READ(RKNPU_OFFSET_VERSION_NUM) & 0xffff);
	return 0;
}

int rknpu_get_bw_priority(struct rknpu_device *rknpu_dev, uint32_t *priority,
			  uint32_t *expect, uint32_t *tw)
{
	void __iomem *base = rknpu_dev->bw_priority_base;

	if (!base)
		return -EINVAL;

	spin_lock(&rknpu_dev->lock);
	if (priority)
		*priority = _REG_READ(base, 0x0);
	if (expect)
		*expect = _REG_READ(base, 0x8);
	if (tw)
		*tw = _REG_READ(base, 0xc);
	spin_unlock(&rknpu_dev->lock);

	return 0;
}

int rknpu_set_bw_priority(struct rknpu_device *rknpu_dev, uint32_t priority,
			  uint32_t expect, uint32_t tw)
{
	void __iomem *base = rknpu_dev->bw_priority_base;

	if (!base)
		return -EINVAL;

	spin_lock(&rknpu_dev->lock);
	if (priority)
		_REG_WRITE(base, priority, 0x0);
	if (expect)
		_REG_WRITE(base, expect, 0x8);
	if (tw)
		_REG_WRITE(base, tw, 0xc);
	spin_unlock(&rknpu_dev->lock);

	return 0;
}

int rknpu_clear_rw_amount(struct rknpu_device *rknpu_dev)
{
	const struct rknpu_config *config = rknpu_dev->config;

	if (!config->amount_top)
		return 0;

	spin_lock(&rknpu_dev->lock);
	REG_WRITE(0x80000101, config->amount_top->offset_clr_all);
	REG_WRITE(0x00000101, config->amount_top->offset_clr_all);
	if (config->amount_core) {
		REG_WRITE(0x80000101, config->amount_core->offset_clr_all);
		REG_WRITE(0x00000101, config->amount_core->offset_clr_all);
	}
	spin_unlock(&rknpu_dev->lock);

	return 0;
}

int rknpu_get_rw_amount(struct rknpu_device *rknpu_dev, uint32_t *dt_wr,
			uint32_t *dt_rd, uint32_t *wd_rd)
{
	const struct rknpu_config *config = rknpu_dev->config;
	int scale = config->pc_data_amount_scale;

	if (!config->amount_top)
		return 0;

	spin_lock(&rknpu_dev->lock);
	if (dt_wr) {
		*dt_wr = REG_READ(config->amount_top->offset_dt_wr) * scale;
		if (config->amount_core)
			*dt_wr += REG_READ(config->amount_core->offset_dt_wr) * scale;
	}
	if (dt_rd) {
		*dt_rd = REG_READ(config->amount_top->offset_dt_rd) * scale;
		if (config->amount_core)
			*dt_rd += REG_READ(config->amount_core->offset_dt_rd) * scale;
	}
	if (wd_rd) {
		*wd_rd = REG_READ(config->amount_top->offset_wt_rd) * scale;
		if (config->amount_core)
			*wd_rd += REG_READ(config->amount_core->offset_wt_rd) * scale;
	}
	spin_unlock(&rknpu_dev->lock);

	return 0;
}

int rknpu_get_total_rw_amount(struct rknpu_device *rknpu_dev, uint32_t *amount)
{
	uint32_t dt_wr = 0, dt_rd = 0, wd_rd = 0;
	int ret;

	if (!rknpu_dev->config->amount_top)
		return 0;

	ret = rknpu_get_rw_amount(rknpu_dev, &dt_wr, &dt_rd, &wd_rd);
	if (amount)
		*amount = dt_wr + dt_rd + wd_rd;

	return ret;
}
