// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2020 Rockchip Electronics Co., Ltd.
 * Refactored for Kernel 7.3.0 DRM Render Node integration & 850mV stability fix.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/pm_runtime.h>
#include <linux/pm_domain.h>
#include <linux/slab.h>
#include <linux/io.h>a
#include <linux/regulator/consumer.h>
#include <linux/clk.h>
#include <linux/version.h>
#include <linux/wait.h>
#include <linux/sched.h>

#include "rknpu_drv.h"
#include "rknpu_reset.h"
#include "rknpu_gem.h"
#include "rknpu_fence.h"
#include "rknpu_devfreq.h"
#include "rknpu_iommu.h"
#include "rknpu_ioctl.h"

#ifdef CONFIG_ROCKCHIP_RKNPU_DRM_GEM
#include <drm/drm_device.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_file.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#endif

#ifdef CONFIG_ROCKCHIP_RKNPU_DMA_HEAP
#include <linux/rk-dma-heap.h>
#include "rknpu_mem.h"
#endif

static bool bypass_irq_handler = false;
module_param(bypass_irq_handler, bool, 0644);
MODULE_PARM_DESC(bypass_irq_handler, "Bypass irq handler");

static bool bypass_soft_reset = false;
module_param(bypass_soft_reset, bool, 0644);
MODULE_PARM_DESC(bypass_soft_reset, "Bypass soft reset");

/* Declaraciones externas */
extern irqreturn_t rknpu_platform_irq_handler(int irq, void *dev_id);
extern int rknpu_get_hw_version(struct rknpu_device *rknpu_dev, uint32_t *version);
extern int rknpu_get_bw_priority(struct rknpu_device *rknpu_dev, uint32_t *priority, uint32_t *expect, uint32_t *tw);
extern int rknpu_set_bw_priority(struct rknpu_device *rknpu_dev, uint32_t priority, uint32_t expect, uint32_t tw);
extern int rknpu_clear_rw_amount(struct rknpu_device *rknpu_dev);
extern int rknpu_get_rw_amount(struct rknpu_device *rknpu_dev, uint32_t *dt_wr, uint32_t *dt_rd, uint32_t *wd_rd);
extern int rknpu_get_total_rw_amount(struct rknpu_device *rknpu_dev, uint32_t *amount);

int rknpu_power_on(struct rknpu_device *rknpu_dev);
int rknpu_power_off(struct rknpu_device *rknpu_dev);
int rknpu_power_get(struct rknpu_device *rknpu_dev);
int rknpu_power_put(struct rknpu_device *rknpu_dev);
int rknpu_power_put_delay(struct rknpu_device *rknpu_dev);
int rknpu_action(struct rknpu_device *rknpu_dev, struct rknpu_action *args);

/* Configuración de plataformas RK356x / RK3588 */
static const struct rknpu_config rk3566_rknpu_config = {
	.core_mask = RKNPU_CORE0_MASK,
	.bw_priority_addr = 0xFE880000,
	.bw_priority_length = 0x100,
	.dma_mask = DMA_BIT_MASK(32),
	.pc_data_amount_scale = 1,
	.pc_task_number_bits = 12,
	.pc_task_number_mask = 0xfff,
	.pc_task_status_offset = 0x3c,
	.max_submit_number = (1 << 12) - 1,
	.nbuf_size = 0,
	.num_irqs = 1,
};

static const struct rknpu_config rk3568_rknpu_config = {
	.core_mask = RKNPU_CORE0_MASK,
	.bw_priority_addr = 0xFE880000,
	.bw_priority_length = 0x100,
	.dma_mask = DMA_BIT_MASK(32),
	.pc_data_amount_scale = 1,
	.pc_task_number_bits = 12,
	.pc_task_number_mask = 0xfff,
	.pc_task_status_offset = 0x3c,
	.max_submit_number = (1 << 12) - 1,
	.nbuf_size = 0,
	.num_irqs = 1,
};

static const struct of_device_id rknpu_of_match[] = {
	{
		.compatible = "rockchip,rk3566-npu",
		.data = &rk3566_rknpu_config,
	},
	{
		.compatible = "rockchip,rk3568-npu",
		.data = &rk3568_rknpu_config,
	},
	{
		.compatible = "rockchip,rk3568-rknpu",
		.data = &rk3568_rknpu_config,
	},
	{
		.compatible = "rockchip,rknpu",
		.data = &rk3568_rknpu_config,
	},
	{},
};
MODULE_DEVICE_TABLE(of, rknpu_of_match);

static int rknpu_is_iommu_enable(struct device *dev)
{
	struct device_node *np = dev->of_node;
	return of_find_property(np, "iommus", NULL) != NULL;
}

static int rknpu_register_irq(struct platform_device *pdev, struct rknpu_device *rknpu_dev)
{
	int ret;
	int irq = -1;

	/* 1. Resolución directa desde el dominio de interrupciones GICv3 usando OpenFirmware */
	if (pdev->dev.of_node) {
		irq = of_irq_get_byname(pdev->dev.of_node, "npu_irq");
		if (irq < 0) {
			irq = of_irq_get(pdev->dev.of_node, 0);
		}
	}

	/* 2. Fallback de API de platform si OF devolviera error */
	if (irq < 0) {
		irq = platform_get_irq_byname(pdev, "npu_irq");
	}

	if (irq < 0) {
		dev_err(&pdev->dev, "RKNPU [ERROR]: No se pudo traducir el IRQ virtual del GICv3\n");
		return irq;
	}

	dev_info(&pdev->dev, "RKNPU [IRQ]: Asignado exitosamente IRQ virtual #%d\n", irq);

	ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
					rknpu_platform_irq_handler,
					IRQF_SHARED,
					dev_name(&pdev->dev),
					&rknpu_dev->subcore_datas[0]);
	return ret;
}

static int rknpu_get_drv_version(uint32_t *version)
{
	*version = RKNPU_GET_DRV_VERSION_CODE(DRIVER_MAJOR, DRIVER_MINOR, DRIVER_PATCHLEVEL);
	return 0;
}

static void rknpu_power_off_delay_work(struct work_struct *power_off_work)
{
	int ret = 0;
	struct rknpu_device *rknpu_dev =
		container_of(to_delayed_work(power_off_work),
			     struct rknpu_device, power_off_work);
	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_dec_if_positive(&rknpu_dev->power_refcount) == 0) {
		ret = rknpu_power_off(rknpu_dev);
		if (ret)
			atomic_inc(&rknpu_dev->power_refcount);
	}
	mutex_unlock(&rknpu_dev->power_lock);

	if (ret)
		rknpu_power_put_delay(rknpu_dev);
}

int rknpu_power_get(struct rknpu_device *rknpu_dev)
{
	int ret = 0;

	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_inc_return(&rknpu_dev->power_refcount) == 1) {
		ret = rknpu_power_on(rknpu_dev);
		if (ret)
			atomic_dec(&rknpu_dev->power_refcount);
	}
	mutex_unlock(&rknpu_dev->power_lock);

	return ret;
}

int rknpu_power_put(struct rknpu_device *rknpu_dev)
{
	int ret = 0;

	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_dec_if_positive(&rknpu_dev->power_refcount) == 0) {
		ret = rknpu_power_off(rknpu_dev);
		if (ret)
			atomic_inc(&rknpu_dev->power_refcount);
	}
	mutex_unlock(&rknpu_dev->power_lock);

	if (ret)
		rknpu_power_put_delay(rknpu_dev);

	return ret;
}

int rknpu_power_put_delay(struct rknpu_device *rknpu_dev)
{
	if (rknpu_dev->power_put_delay == 0)
		return rknpu_power_put(rknpu_dev);

	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_read(&rknpu_dev->power_refcount) == 1)
		queue_delayed_work(
			rknpu_dev->power_off_wq, &rknpu_dev->power_off_work,
			msecs_to_jiffies(rknpu_dev->power_put_delay));
	else
		atomic_dec_if_positive(&rknpu_dev->power_refcount);
	mutex_unlock(&rknpu_dev->power_lock);

	return 0;
}

int rknpu_power_on(struct rknpu_device *rknpu_dev)
{
	struct device *dev = rknpu_dev->dev;
	int ret;

	if (rknpu_dev->vdd) {
		ret = regulator_enable(rknpu_dev->vdd);
		if (ret)
			return ret;
	}

	ret = clk_bulk_prepare_enable(rknpu_dev->num_clks, rknpu_dev->clks);
	if (ret) {
		if (rknpu_dev->vdd)
			regulator_disable(rknpu_dev->vdd);
		return ret;
	}

	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		pm_runtime_put_noidle(dev);
		clk_bulk_disable_unprepare(rknpu_dev->num_clks, rknpu_dev->clks);
		if (rknpu_dev->vdd)
			regulator_disable(rknpu_dev->vdd);
		return ret;
	}

	return 0;
}

int rknpu_power_off(struct rknpu_device *rknpu_dev)
{
	struct device *dev = rknpu_dev->dev;

	pm_runtime_put_sync(dev);
	clk_bulk_disable_unprepare(rknpu_dev->num_clks, rknpu_dev->clks);
	if (rknpu_dev->vdd)
		regulator_disable(rknpu_dev->vdd);

	return 0;
}

int rknpu_action(struct rknpu_device *rknpu_dev, struct rknpu_action *args)
{
	int ret = -EINVAL;

	switch (args->flags) {
	case RKNPU_GET_HW_VERSION:
		ret = rknpu_get_hw_version(rknpu_dev, &args->value);
		break;
	case RKNPU_GET_DRV_VERSION:
		ret = rknpu_get_drv_version(&args->value);
		break;
	case RKNPU_GET_FREQ:
		args->value = clk_get_rate(rknpu_dev->clks[0].clk);
		ret = 0;
		break;
	case RKNPU_GET_VOLT:
		if (rknpu_dev->vdd)
			args->value = regulator_get_voltage(rknpu_dev->vdd);
		else
			args->value = 850000;
		ret = 0;
		break;
	case RKNPU_ACT_RESET:
		ret = rknpu_soft_reset(rknpu_dev);
		break;
	case RKNPU_GET_BW_PRIORITY:
		ret = rknpu_get_bw_priority(rknpu_dev, &args->value, NULL, NULL);
		break;
	case RKNPU_SET_BW_PRIORITY:
		ret = rknpu_set_bw_priority(rknpu_dev, args->value, 0, 0);
		break;
	case RKNPU_ACT_CLR_TOTAL_RW_AMOUNT:
		ret = rknpu_clear_rw_amount(rknpu_dev);
		break;
	case RKNPU_GET_DT_WR_AMOUNT:
		ret = rknpu_get_rw_amount(rknpu_dev, &args->value, NULL, NULL);
		break;
	case RKNPU_GET_DT_RD_AMOUNT:
		ret = rknpu_get_rw_amount(rknpu_dev, NULL, &args->value, NULL);
		break;
	case RKNPU_GET_WT_RD_AMOUNT:
		ret = rknpu_get_rw_amount(rknpu_dev, NULL, NULL, &args->value);
		break;
	case RKNPU_GET_TOTAL_RW_AMOUNT:
		ret = rknpu_get_total_rw_amount(rknpu_dev, &args->value);
		break;
	case RKNPU_GET_IOMMU_EN:
		args->value = rknpu_dev->iommu_en;
		ret = 0;
		break;
	case RKNPU_SET_PROC_NICE:
		set_user_nice(current, *(int32_t *)&args->value);
		ret = 0;
		break;
	case RKNPU_GET_TOTAL_SRAM_SIZE:
	case RKNPU_GET_FREE_SRAM_SIZE:
		args->value = 0;
		ret = 0;
		break;
	case RKNPU_GET_IOMMU_DOMAIN_ID:
		args->value = rknpu_dev->iommu_domain_id;
		ret = 0;
		break;
	case RKNPU_SET_IOMMU_DOMAIN_ID:
		ret = rknpu_iommu_domain_get_and_switch(rknpu_dev, *(int32_t *)&args->value);
		if (!ret)
			rknpu_iommu_domain_put(rknpu_dev);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}
EXPORT_SYMBOL(rknpu_action);

#ifdef CONFIG_ROCKCHIP_RKNPU_DRM_GEM
static int rknpu_action_ioctl(struct drm_device *dev, void *data,
			      struct drm_file *file_priv)
{
	struct rknpu_device *rknpu_dev = dev->dev_private;
	return rknpu_action(rknpu_dev, (struct rknpu_action *)data);
}

#define RKNPU_IOCTL(func)                                                   \
	static int __##func(struct drm_device *dev, void *data,             \
			    struct drm_file *file_priv)                     \
	{                                                                   \
		struct rknpu_device *rknpu_dev = dev->dev_private;          \
		int ret = -EINVAL;                                          \
		ret = rknpu_power_get(rknpu_dev);                           \
		if (ret)                                                    \
			return ret;                                         \
		ret = func(dev, data, file_priv);                           \
		rknpu_power_put_delay(rknpu_dev);                           \
		return ret;                                                 \
	}

RKNPU_IOCTL(rknpu_action_ioctl);
RKNPU_IOCTL(rknpu_submit_ioctl);
RKNPU_IOCTL(rknpu_gem_create_ioctl);
RKNPU_IOCTL(rknpu_gem_map_ioctl);
RKNPU_IOCTL(rknpu_gem_destroy_ioctl);
RKNPU_IOCTL(rknpu_gem_sync_ioctl);

static const struct drm_ioctl_desc rknpu_ioctls[] = {
	DRM_IOCTL_DEF_DRV(RKNPU_ACTION, __rknpu_action_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_SUBMIT, __rknpu_submit_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_MEM_CREATE, __rknpu_gem_create_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_MEM_MAP, __rknpu_gem_map_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_MEM_DESTROY, __rknpu_gem_destroy_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_MEM_SYNC, __rknpu_gem_sync_ioctl, DRM_RENDER_ALLOW),
};

DEFINE_DRM_GEM_FOPS(rknpu_drm_driver_fops);

static struct drm_driver rknpu_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER,
	.dumb_create = rknpu_gem_dumb_create,
	.dumb_map_offset = drm_gem_dumb_map_offset,
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_import = drm_gem_prime_import,
	.gem_prime_import_sg_table = rknpu_gem_prime_import_sg_table,
	.ioctls = rknpu_ioctls,
	.num_ioctls = ARRAY_SIZE(rknpu_ioctls),
	.fops = &rknpu_drm_driver_fops,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
};

static int rknpu_drm_probe(struct rknpu_device *rknpu_dev)
{
	struct device *dev = rknpu_dev->dev;
	struct drm_device *drm_dev = NULL;
	int ret = -EINVAL;

	drm_dev = drm_dev_alloc(&rknpu_drm_driver, dev);
	if (IS_ERR(drm_dev))
		return PTR_ERR(drm_dev);

	drm_dev->dev_private = rknpu_dev;
	rknpu_dev->drm_dev = drm_dev;

	ret = drm_dev_register(drm_dev, 0);
	if (ret < 0)
		goto err_free_drm;

	return 0;

err_free_drm:
	drm_dev_put(drm_dev);
	rknpu_dev->drm_dev = NULL;
	return ret;
}

static void rknpu_drm_remove(struct rknpu_device *rknpu_dev)
{
	struct drm_device *drm_dev = rknpu_dev->drm_dev;
	if (!drm_dev)
		return;

	drm_dev_unregister(drm_dev);
	drm_dev_put(drm_dev);
	rknpu_dev->drm_dev = NULL;
}
#endif /* CONFIG_ROCKCHIP_RKNPU_DRM_GEM */

static int rknpu_probe(struct platform_device *pdev)
{
	struct resource *res = NULL;
	struct rknpu_device *rknpu_dev = NULL;
	struct device *dev = &pdev->dev;
	const struct of_device_id *match = NULL;
	const struct rknpu_config *config = NULL;
	int ret = -EINVAL, i = 0;

	if (!pdev->dev.of_node)
		return -ENODEV;

	match = of_match_device(rknpu_of_match, dev);
	if (!match)
		return -ENODEV;

	rknpu_dev = devm_kzalloc(dev, sizeof(*rknpu_dev), GFP_KERNEL);
	if (!rknpu_dev)
		return -ENOMEM;

	config = of_device_get_match_data(dev);
	if (!config)
		return -EINVAL;

	rknpu_dev->config = config;
	rknpu_dev->dev = dev;
	platform_set_drvdata(pdev, rknpu_dev);

	rknpu_dev->iommu_en = rknpu_is_iommu_enable(dev);
	if (rknpu_dev->iommu_en) {
		rknpu_dev->iommu_group = iommu_group_get(dev);
		if (!rknpu_dev->iommu_group)
			return -EINVAL;
	} else {
		ret = of_reserved_mem_device_init(dev);
	}

	rknpu_dev->bypass_irq_handler = bypass_irq_handler;
	rknpu_dev->bypass_soft_reset = bypass_soft_reset;

	rknpu_reset_get(rknpu_dev);

	rknpu_dev->num_clks = devm_clk_bulk_get_all(dev, &rknpu_dev->clks);
	if (rknpu_dev->num_clks < 1) {
		ret = -ENODEV;
		goto err_release_iommu;
	}

	rknpu_dev->vdd = devm_regulator_get_optional(dev, "rknpu");
	if (IS_ERR(rknpu_dev->vdd)) {
		if (PTR_ERR(rknpu_dev->vdd) != -ENODEV) {
			ret = PTR_ERR(rknpu_dev->vdd);
			goto err_release_iommu;
		}
		rknpu_dev->vdd = NULL;
	}

	rknpu_dev->mem = devm_regulator_get_optional(dev, "mem");
	if (IS_ERR(rknpu_dev->mem)) {
		if (PTR_ERR(rknpu_dev->mem) != -ENODEV) {
			ret = PTR_ERR(rknpu_dev->mem);
			goto err_release_iommu;
		}
		rknpu_dev->mem = NULL;
	}

	/* Fijar 850mV para evitar cuelgues */
	if (rknpu_dev->vdd) {
		ret = regulator_set_voltage(rknpu_dev->vdd, 850000, 850000);
		if (ret)
			goto err_release_iommu;
		ret = regulator_enable(rknpu_dev->vdd);
		if (ret)
			goto err_release_iommu;
	}

	spin_lock_init(&rknpu_dev->lock);
	spin_lock_init(&rknpu_dev->irq_lock);
	mutex_init(&rknpu_dev->power_lock);
	mutex_init(&rknpu_dev->reset_lock);
	mutex_init(&rknpu_dev->domain_lock);
	mutex_init(&rknpu_dev->devfreq_lock);

	for (i = 0; i < config->num_irqs; i++) {
		INIT_LIST_HEAD(&rknpu_dev->subcore_datas[i].todo_list);
		init_waitqueue_head(&rknpu_dev->subcore_datas[i].job_done_wq);
		rknpu_dev->subcore_datas[i].task_num = 0;
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res) {
			ret = -ENXIO;
			goto err_release_iommu;
		}

		rknpu_dev->base[i] = devm_ioremap_resource(dev, res);
		if (PTR_ERR(rknpu_dev->base[i]) == -EBUSY) {
			rknpu_dev->base[i] = devm_ioremap(dev, res->start,
							  resource_size(res));
		}

		if (IS_ERR(rknpu_dev->base[i])) {
			ret = PTR_ERR(rknpu_dev->base[i]);
			goto err_release_iommu;
		}
	}

	if (!rknpu_dev->bypass_irq_handler) {
		ret = rknpu_register_irq(pdev, rknpu_dev);
		if (ret)
			goto err_release_iommu;
	}

#ifdef CONFIG_ROCKCHIP_RKNPU_DRM_GEM
	ret = rknpu_drm_probe(rknpu_dev);
	if (ret) {
		dev_err(dev, "Error al registrar dispositivo DRM: %d\n", ret);
		goto err_release_iommu;
	}
#endif

#ifdef CONFIG_ROCKCHIP_RKNPU_FENCE
	ret = rknpu_fence_context_alloc(rknpu_dev);
	if (ret)
		goto err_remove_drm;
#endif

	pm_runtime_enable(dev);

	ret = rknpu_power_on(rknpu_dev);
	if (ret)
		goto err_disable_pm;

	rknpu_devfreq_init(rknpu_dev);

	rknpu_power_off(rknpu_dev);
	atomic_set(&rknpu_dev->power_refcount, 0);

	rknpu_dev->power_put_delay = 3000;
	rknpu_dev->power_off_wq = create_freezable_workqueue("rknpu_power_off_wq");
	if (rknpu_dev->power_off_wq)
		INIT_DEFERRABLE_WORK(&rknpu_dev->power_off_work, rknpu_power_off_delay_work);

	dev_info(dev, "RKNPU [SUCCESS]: Inicializado con exito con DRM Render Node\n");
	return 0;

err_disable_pm:
	pm_runtime_disable(dev);
#ifdef CONFIG_ROCKCHIP_RKNPU_FENCE
err_remove_drm:
#endif
#ifdef CONFIG_ROCKCHIP_RKNPU_DRM_GEM
	rknpu_drm_remove(rknpu_dev);
#endif
err_release_iommu:
	if (rknpu_dev->iommu_en) {
		if (rknpu_dev->iommu_group)
			iommu_group_put(rknpu_dev->iommu_group);
	} else {
		of_reserved_mem_device_release(dev);
	}
	return ret;
}

static void rknpu_remove(struct platform_device *pdev)
{
	struct rknpu_device *rknpu_dev = platform_get_drvdata(pdev);
	if (!rknpu_dev)
		return;

	if (rknpu_dev->power_off_wq) {
		cancel_delayed_work_sync(&rknpu_dev->power_off_work);
		destroy_workqueue(rknpu_dev->power_off_wq);
	}

#ifdef CONFIG_ROCKCHIP_RKNPU_DRM_GEM
	rknpu_drm_remove(rknpu_dev);
#endif

	rknpu_devfreq_remove(rknpu_dev);

	pm_runtime_disable(&pdev->dev);

	if (rknpu_dev->iommu_en) {
		iommu_group_put(rknpu_dev->iommu_group);
	} else {
		of_reserved_mem_device_release(&pdev->dev);
	}
}

static struct platform_driver rknpu_driver = {
	.driver = {
		.name = "rknpu",
		.of_match_table = rknpu_of_match,
	},
	.probe = rknpu_probe,
	.remove = rknpu_remove,
};

module_platform_driver(rknpu_driver);

MODULE_AUTHOR("Rockchip Electronics");
MODULE_DESCRIPTION("RKNPU Driver with DRM Render Node for RK356x");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
