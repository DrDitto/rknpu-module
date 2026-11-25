// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 *
 * Target: RK3566/RK3568 | Linux Kernel >= 7.2.0
 */

#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/proc_fs.h>
#include <linux/devfreq.h>
#include <linux/clk.h>
#include <asm/div64.h>

#include "rknpu_drv.h"
#include "rknpu_reset.h"
#include "rknpu_debugger.h"

#define RKNPU_DEBUGGER_ROOT_NAME "rknpu"

#if defined(CONFIG_ROCKCHIP_RKNPU_DEBUG_FS) || \
	defined(CONFIG_ROCKCHIP_RKNPU_PROC_FS)

static int rknpu_version_show(struct seq_file *m, void *data)
{
	seq_printf(m, "%s: v%d.%d.%d\n", DRIVER_DESC, DRIVER_MAJOR,
		   DRIVER_MINOR, DRIVER_PATCHLEVEL);
	return 0;
}

static int rknpu_load_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node = m->private;
	struct rknpu_device *rknpu_dev =
		container_of(node->debugger, struct rknpu_device, debugger);
	unsigned long flags;
	uint64_t total_busy_time, div_value;
	int load;

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	total_busy_time = rknpu_dev->subcore_data.timer.total_busy_time;
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	div_value = RKNPU_LOAD_INTERVAL / 100;
	do_div(total_busy_time, div_value);
	load = total_busy_time > 100 ? 100 : total_busy_time;

	seq_printf(m, "NPU load: %2d%%\n", load);
	return 0;
}

static int rknpu_power_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node = m->private;
	struct rknpu_device *rknpu_dev =
		container_of(node->debugger, struct rknpu_device, debugger);

	seq_puts(m, atomic_read(&rknpu_dev->power_refcount) > 0 ? "on\n" : "off\n");
	return 0;
}

static ssize_t rknpu_power_set(struct file *file, const char __user *ubuf,
			       size_t len, loff_t *offp)
{
	struct seq_file *priv = file->private_data;
	struct rknpu_debugger_node *node = priv->private;
	struct rknpu_device *rknpu_dev =
		container_of(node->debugger, struct rknpu_device, debugger);
	char buf[16];

	if (len == 0 || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	if (buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	if (strcmp(buf, "on") == 0) {
		atomic_inc(&rknpu_dev->cmdline_power_refcount);
		rknpu_power_get(rknpu_dev);
	} else if (strcmp(buf, "off") == 0) {
		if (atomic_read(&rknpu_dev->power_refcount) > 0 &&
		    atomic_dec_if_positive(&rknpu_dev->cmdline_power_refcount) >= 0) {
			atomic_sub(atomic_read(&rknpu_dev->cmdline_power_refcount),
				   &rknpu_dev->power_refcount);
			atomic_set(&rknpu_dev->cmdline_power_refcount, 0);
			rknpu_power_put(rknpu_dev);
		}
	} else {
		LOG_ERROR("invalid rknpu power param\n");
	}

	return len;
}

static int rknpu_power_put_delay_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node = m->private;
	struct rknpu_device *rknpu_dev =
		container_of(node->debugger, struct rknpu_device, debugger);

	seq_printf(m, "%lu\n", rknpu_dev->power_put_delay);
	return 0;
}

static ssize_t rknpu_power_put_delay_set(struct file *file,
					 const char __user *ubuf, size_t len,
					 loff_t *offp)
{
	struct seq_file *priv = file->private_data;
	struct rknpu_debugger_node *node = priv->private;
	struct rknpu_device *rknpu_dev =
		container_of(node->debugger, struct rknpu_device, debugger);
	char buf[16];
	unsigned long power_put_delay;

	if (len == 0 || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	if (buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	if (kstrtoul(buf, 10, &power_put_delay))
		return -EFAULT;

	rknpu_dev->power_put_delay = power_put_delay;
	return len;
}

static int rknpu_freq_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node = m->private;
	struct rknpu_device *rknpu_dev =
		container_of(node->debugger, struct rknpu_device, debugger);
	unsigned long current_freq;

	rknpu_power_get(rknpu_dev);
	current_freq = clk_get_rate(rknpu_dev->clks[0].clk);
	rknpu_power_put(rknpu_dev);

	seq_printf(m, "%lu\n", current_freq);
	return 0;
}

#ifdef CONFIG_PM_DEVFREQ
static ssize_t rknpu_freq_set(struct file *file, const char __user *ubuf,
			      size_t len, loff_t *offp)
{
	struct seq_file *priv = file->private_data;
	struct rknpu_debugger_node *node = priv->private;
	struct rknpu_device *rknpu_dev =
		container_of(node->debugger, struct rknpu_device, debugger);
	unsigned long current_freq, freq;
	char buf[16];

	if (len == 0 || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	if (buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	if (kstrtoul(buf, 10, &freq) || !rknpu_dev->devfreq)
		return -EFAULT;

	rknpu_power_get(rknpu_dev);
	current_freq = clk_get_rate(rknpu_dev->clks[0].clk);
	if (freq != current_freq) {
		rknpu_dev->ondemand_freq = freq;
		mutex_lock(&rknpu_dev->devfreq->lock);
		update_devfreq(rknpu_dev->devfreq);
		mutex_unlock(&rknpu_dev->devfreq->lock);
	}
	rknpu_power_put(rknpu_dev);

	return len;
}
#else
static ssize_t rknpu_freq_set(struct file *file, const char __user *ubuf,
			      size_t len, loff_t *offp)
{
	return -EFAULT;
}
#endif

static int rknpu_volt_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node = m->private;
	struct rknpu_device *rknpu_dev =
		container_of(node->debugger, struct rknpu_device, debugger);
	unsigned long current_volt = rknpu_dev->vdd ?
				     regulator_get_voltage(rknpu_dev->vdd) : 0;

	seq_printf(m, "%lu\n", current_volt);
	return 0;
}

static int rknpu_reset_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node = m->private;
	struct rknpu_device *rknpu_dev =
		container_of(node->debugger, struct rknpu_device, debugger);

	seq_puts(m, !rknpu_dev->bypass_soft_reset ? "on\n" : "off\n");
	return 0;
}

static ssize_t rknpu_reset_set(struct file *file, const char __user *ubuf,
			       size_t len, loff_t *offp)
{
	struct seq_file *priv = file->private_data;
	struct rknpu_debugger_node *node = priv->private;
	struct rknpu_device *rknpu_dev =
		container_of(node->debugger, struct rknpu_device, debugger);
	char buf[16];

	if (len == 0 || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	if (buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	if (strcmp(buf, "1") == 0 && atomic_read(&rknpu_dev->power_refcount) > 0)
		rknpu_soft_reset(rknpu_dev);
	else if (strcmp(buf, "on") == 0)
		rknpu_dev->bypass_soft_reset = 0;
	else if (strcmp(buf, "off") == 0)
		rknpu_dev->bypass_soft_reset = 1;

	return len;
}

static struct rknpu_debugger_list rknpu_debugger_root_list[] = {
	{ "version", rknpu_version_show, NULL, NULL },
	{ "load", rknpu_load_show, NULL, NULL },
	{ "power", rknpu_power_show, rknpu_power_set, NULL },
	{ "freq", rknpu_freq_show, rknpu_freq_set, NULL },
	{ "volt", rknpu_volt_show, NULL, NULL },
	{ "delayms", rknpu_power_put_delay_show, rknpu_power_put_delay_set, NULL },
	{ "reset", rknpu_reset_show, rknpu_reset_set, NULL },
};

static ssize_t rknpu_debugger_write(struct file *file, const char __user *ubuf,
				    size_t len, loff_t *offp)
{
	struct seq_file *priv = file->private_data;
	struct rknpu_debugger_node *node = priv->private;

	return node->info_ent->write ?
		       node->info_ent->write(file, ubuf, len, offp) : len;
}

static int rknpu_debugfs_open(struct inode *inode, struct file *file)
{
	struct rknpu_debugger_node *node = inode->i_private;

	return single_open(file, node->info_ent->show, node);
}

static const struct file_operations rknpu_debugfs_fops = {
	.owner = THIS_MODULE,
	.open = rknpu_debugfs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = rknpu_debugger_write,
};
#endif
#ifdef CONFIG_ROCKCHIP_RKNPU_DEBUG_FS
static int rknpu_debugfs_remove_files(struct rknpu_debugger *debugger)
{
	struct rknpu_debugger_node *pos, *q;

	mutex_lock(&debugger->debugfs_lock);
	list_for_each_entry_safe(pos, q, &debugger->debugfs_entry_list, list) {
		if (!pos->dent)
			continue;
		list_del(&pos->list);
		kfree(pos);
	}
	debugfs_remove_recursive(debugger->debugfs_dir);
	debugger->debugfs_dir = NULL;
	mutex_unlock(&debugger->debugfs_lock);
	return 0;
}

static int rknpu_debugfs_create_files(const struct rknpu_debugger_list *files,
				      int count, struct dentry *root,
				      struct rknpu_debugger *debugger)
{
	int i;
	struct dentry *ent;
	struct rknpu_debugger_node *tmp;

	for (i = 0; i < count; i++) {
		tmp = kmalloc(sizeof(*tmp), GFP_KERNEL);
		if (!tmp)
			goto err_alloc;

		tmp->info_ent = &files[i];
		tmp->debugger = debugger;

		ent = debugfs_create_file(files[i].name, 0444, root, tmp,
					  &rknpu_debugfs_fops);
		if (!ent) {
			kfree(tmp);
			goto err_alloc;
		}

		tmp->dent = ent;
		mutex_lock(&debugger->debugfs_lock);
		list_add_tail(&tmp->list, &debugger->debugfs_entry_list);
		mutex_unlock(&debugger->debugfs_lock);
	}
	return 0;

err_alloc:
	rknpu_debugfs_remove_files(debugger);
	return -ENOMEM;
}

static int rknpu_debugfs_remove(struct rknpu_debugger *debugger)
{
	return rknpu_debugfs_remove_files(debugger);
}

static int rknpu_debugfs_init(struct rknpu_debugger *debugger)
{
	debugger->debugfs_dir =
		debugfs_create_dir(RKNPU_DEBUGGER_ROOT_NAME, NULL);
	if (IS_ERR_OR_NULL(debugger->debugfs_dir)) {
		debugger->debugfs_dir = NULL;
		return -EIO;
	}

	if (rknpu_debugfs_create_files(rknpu_debugger_root_list,
				       ARRAY_SIZE(rknpu_debugger_root_list),
				       debugger->debugfs_dir, debugger)) {
		rknpu_debugfs_remove_files(debugger);
		return -ENOMEM;
	}
	return 0;
}
#endif

#ifdef CONFIG_ROCKCHIP_RKNPU_PROC_FS
static int rknpu_procfs_open(struct inode *inode, struct file *file)
{
	struct rknpu_debugger_node *node = pde_data(inode);

	return single_open(file, node->info_ent->show, node);
}

static const struct proc_ops rknpu_procfs_fops = {
	.proc_open = rknpu_procfs_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = rknpu_debugger_write,
};

static int rknpu_procfs_remove_files(struct rknpu_debugger *debugger)
{
	struct rknpu_debugger_node *pos, *q;

	mutex_lock(&debugger->procfs_lock);
	list_for_each_entry_safe(pos, q, &debugger->procfs_entry_list, list) {
		if (!pos->pent)
			continue;
		list_del(&pos->list);
		kfree(pos);
	}
	proc_remove(debugger->procfs_dir);
	debugger->procfs_dir = NULL;
	mutex_unlock(&debugger->procfs_lock);
	return 0;
}

static int rknpu_procfs_create_files(const struct rknpu_debugger_list *files,
				     int count, struct proc_dir_entry *root,
				     struct rknpu_debugger *debugger)
{
	int i;
	struct proc_dir_entry *ent;
	struct rknpu_debugger_node *tmp;

	for (i = 0; i < count; i++) {
		tmp = kmalloc(sizeof(*tmp), GFP_KERNEL);
		if (!tmp)
			goto err_alloc;

		tmp->info_ent = &files[i];
		tmp->debugger = debugger;

		ent = proc_create_data(files[i].name, 0444, root,
				       &rknpu_procfs_fops, tmp);
		if (!ent) {
			kfree(tmp);
			goto err_alloc;
		}

		tmp->pent = ent;
		mutex_lock(&debugger->procfs_lock);
		list_add_tail(&tmp->list, &debugger->procfs_entry_list);
		mutex_unlock(&debugger->procfs_lock);
	}
	return 0;

err_alloc:
	rknpu_procfs_remove_files(debugger);
	return -ENOMEM;
}

static int rknpu_procfs_remove(struct rknpu_debugger *debugger)
{
	return rknpu_procfs_remove_files(debugger);
}

static int rknpu_procfs_init(struct rknpu_debugger *debugger)
{
	debugger->procfs_dir = proc_mkdir(RKNPU_DEBUGGER_ROOT_NAME, NULL);
	if (IS_ERR_OR_NULL(debugger->procfs_dir)) {
		debugger->procfs_dir = NULL;
		return -EIO;
	}

	if (rknpu_procfs_create_files(rknpu_debugger_root_list,
				       ARRAY_SIZE(rknpu_debugger_root_list),
				       debugger->procfs_dir, debugger)) {
		rknpu_procfs_remove_files(debugger);
		return -ENOMEM;
	}
	return 0;
}
#endif

int rknpu_debugger_init(struct rknpu_device *rknpu_dev)
{
#ifdef CONFIG_ROCKCHIP_RKNPU_DEBUG_FS
	mutex_init(&rknpu_dev->debugger.debugfs_lock);
	INIT_LIST_HEAD(&rknpu_dev->debugger.debugfs_entry_list);
	rknpu_debugfs_init(&rknpu_dev->debugger);
#endif
#ifdef CONFIG_ROCKCHIP_RKNPU_PROC_FS
	mutex_init(&rknpu_dev->debugger.procfs_lock);
	INIT_LIST_HEAD(&rknpu_dev->debugger.procfs_entry_list);
	rknpu_procfs_init(&rknpu_dev->debugger);
#endif
	return 0;
}

int rknpu_debugger_remove(struct rknpu_device *rknpu_dev)
{
#ifdef CONFIG_ROCKCHIP_RKNPU_DEBUG_FS
	rknpu_debugfs_remove(&rknpu_dev->debugger);
#endif
#ifdef CONFIG_ROCKCHIP_RKNPU_PROC_FS
	rknpu_procfs_remove(&rknpu_dev->debugger);
#endif
	return 0;
}
