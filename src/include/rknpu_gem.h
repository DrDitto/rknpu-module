/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 *
 * Target: RK3566/RK3568 | DRM GEM + IOMMU | Linux Kernel >= 7.2.0
 */

#ifndef __LINUX_RKNPU_GEM_H
#define __LINUX_RKNPU_GEM_H

#include <linux/mm_types.h>
#include <linux/version.h>
#include <drm/drm_device.h>
#include <drm/drm_vma_manager.h>
#include <drm/drm_gem.h>
#include <drm/drm_mode.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 2, 0)
#error "This driver requires Linux Kernel 7.2.0 or newer"
#endif

#define to_rknpu_obj(x) container_of(x, struct rknpu_gem_object, base)

struct rknpu_gem_object {
	struct drm_gem_object base;
	unsigned int flags;
	unsigned long size;
	dma_addr_t iova_start;
	unsigned long iova_size;
	void *cookie;
	void __iomem *kv_addr;
	dma_addr_t dma_addr;
	unsigned long dma_attrs;
	unsigned long num_pages;
	struct page **pages;
	struct sg_table *sgt;
	int iommu_domain_id;
	unsigned int core_mask;
};

struct rknpu_gem_object *
rknpu_gem_object_create(struct drm_device *dev, unsigned int flags,
			unsigned long size, int iommu_domain_id,
			unsigned int core_mask);

void rknpu_gem_object_destroy(struct rknpu_gem_object *rknpu_obj);

int rknpu_gem_create_ioctl(struct drm_device *dev, void *data,
			   struct drm_file *file_priv);
int rknpu_gem_map_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv);
int rknpu_gem_destroy_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file_priv);
int rknpu_gem_sync_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file_priv);

static inline void rknpu_gem_object_get(struct drm_gem_object *obj)
{
	drm_gem_object_get(obj);
}

static inline void rknpu_gem_object_put(struct drm_gem_object *obj)
{
	drm_gem_object_put(obj);
}

static inline struct rknpu_gem_object *
rknpu_gem_object_find(struct drm_file *filp, unsigned int handle)
{
	struct drm_gem_object *obj = drm_gem_object_lookup(filp, handle);

	if (!obj)
		return NULL;

	rknpu_gem_object_put(obj);
	return to_rknpu_obj(obj);
}

void rknpu_gem_free_object(struct drm_gem_object *obj);

int rknpu_gem_dumb_create(struct drm_file *file_priv, struct drm_device *dev,
			  struct drm_mode_create_dumb *args);

int rknpu_gem_mmap_obj(struct drm_gem_object *obj, struct vm_area_struct *vma);
int rknpu_gem_mmap(struct file *filp, struct vm_area_struct *vma);

struct sg_table *rknpu_gem_prime_get_sg_table(struct drm_gem_object *obj);
struct drm_gem_object *
rknpu_gem_prime_import_sg_table(struct drm_device *dev,
				struct dma_buf_attachment *attach,
				struct sg_table *sgt);
int rknpu_gem_prime_vmap(struct drm_gem_object *obj, struct iosys_map *map);
void rknpu_gem_prime_vunmap(struct drm_gem_object *obj, struct iosys_map *map);
int rknpu_gem_prime_mmap(struct drm_gem_object *obj,
			 struct vm_area_struct *vma);

static inline void *rknpu_gem_alloc_page(size_t nr_pages)
{
	return kvmalloc_array(nr_pages, sizeof(struct page *),
			      GFP_KERNEL | __GFP_ZERO);
}

static inline void rknpu_gem_free_page(void *pages)
{
	kvfree(pages);
}

#endif /* __LINUX_RKNPU_GEM_H */
