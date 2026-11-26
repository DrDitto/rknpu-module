Out-of-tree kernel module for the Rockchip RKNPU driver, ported from the [rockchip-linux/kernel](https://github.com/rockchip-linux/kernel).

Tested on kernel 7.3.6
- **Note:** This driver enables IOMMU support for the NPU, allowing safe, isolated, and efficient memory management for shared tensor buffers.

## 1. Tested Environment

| Item | Value |
|---|---|
| Board | BigTreeTech Pi2 v1.0.1 (RK3566) |
| OS | Armbian (Debian Trixie) |
| Kernel | 7.3.6silex-rockchip64 |
| RKNN Runtime | librknnrt v2.3.2 |
| This is a port from BSP and closed to RK35668/RK3566 only. All other HW implementation has been dropped
## 2. Devfreq (DVFS)

The driver supports dynamic voltage and frequency scaling via the standard Linux devfreq framework. When an OPP table is present in the DTB, the driver automatically registers a devfreq device at `/sys/class/devfreq/fde40000.npu/`.

Available frequencies (RK3566): 200 / 297 / 400 / 600 / 700 / 800 MHz.

```sh
# Check current frequency
cat /sys/class/devfreq/fde40000.npu/cur_freq

# List available frequencies
cat /sys/class/devfreq/fde40000.npu/available_frequencies

# Switch to manual frequency controlroot@rk356x:/ext_storage/usb/sda/otten/rknpu-module# lsmod |grep rknpu
rknpu                  53248  0
root@rk356x:/ext_storage/usb/sda/otten/rknpu-module# dmesg
[69046.125296] rknpu fde40000.npu: RKNPU [IRQ]: Asignado exitosamente IRQ virtual #106
[69046.126644] [drm] Initialized rknpu 1.0.0 for fde40000.npu on minor 2
[69046.130145] rknpu fde40000.npu: RKNPU: devfreq enabled, initial freq: 800000000 Hz, volt: 1000000 uV
[69046.131028] rknpu fde40000.npu: RKNPU [SUCCESS]: Inicializado con exito con DRM Render Node y Devfreq limpio
[69082.782876] RKNPU: job: 00000000fd89fdba, mask: 0x1, job iommu domain id: 0, dev iommu domain id: 0, wait_count: 1, continue wait: 0, commit elapse time: 10843500us, wait time: 10843503us, timeout: 10830000us
[69082.784047] RKNPU: failed to wait job, task counter: 0, flags: 0x1, ret = 0, elapsed time: 10844675us
[69082.886862] RKNPU: job timeout, flags: 0x0:
[69082.887484] RKNPU:   core 0 irq status: 0x0, raw status: 0x10000, require mask: 0x300, task counter: 0x0, elapsed time: 10948114us
[69082.990861] RKNPU: soft reset, num: 2
root@rk356x:/ext_storage/usb/sda/otten/rknpu-module# 


echo userspace > /sys/class/devfreq/fde40000.npu/governor
echo 800000000 > /sys/class/devfreq/fde40000.npu/min_freq
echo 800000000 > /sys/class/devfreq/fde40000.npu/max_freq

# Restore automatic governor
echo rknpu_ondemand > /sys/class/devfreq/fde40000.npu/governor
echo 200000000 > /sys/class/devfreq/fde40000.npu/min_freq
echo 800000000 > /sys/class/devfreq/fde40000.npu/max_freq
```
## 3. TODO
  IOCTL control over userpace=======>Work in progress!!!
  
root@rk356x:/ext_storage/usb/sda/otten/rknpu-module# lsmod |grep rknpu
rknpu                  53248  0
root@rk356x:/ext_storage/usb/sda/otten/rknpu-module# dmesg
[69046.125296] rknpu fde40000.npu: RKNPU [IRQ]: Asignado exitosamente IRQ virtual #106
[69046.126644] [drm] Initialized rknpu 1.0.0 for fde40000.npu on minor 2
[69046.130145] rknpu fde40000.npu: RKNPU: devfreq enabled, initial freq: 800000000 Hz, volt: 1000000 uV
[69046.131028] rknpu fde40000.npu: RKNPU [SUCCESS]: Inicializado con exito con DRM Render Node y Devfreq limpio
[69082.782876] RKNPU: job: 00000000fd89fdba, mask: 0x1, job iommu domain id: 0, dev iommu domain id: 0, wait_count: 1, continue wait: 0, commit elapse time: 10843500us, wait time: 10843503us, timeout: 10830000us
[69082.784047] RKNPU: failed to wait job, task counter: 0, flags: 0x1, ret = 0, elapsed time: 10844675us
[69082.886862] RKNPU: job timeout, flags: 0x0:
[69082.887484] RKNPU:   core 0 irq status: 0x0, raw status: 0x10000, require mask: 0x300, task counter: 0x0, elapsed time: 10948114us
[69082.990861] RKNPU: soft reset, num: 2
root@rk356x:/ext_storage/usb/sda/otten/rknpu-module# 

