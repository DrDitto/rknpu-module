Out-of-tree kernel module for the Rockchip RKNPU driver, ported from the [rockchip-linux/kernel](https://github.com/rockchip-linux/kernel).

Tested on kernel 7.3.4
- **Note:** This driver enables IOMMU support for the NPU, allowing safe, isolated, and efficient memory management for shared tensor buffers.

## 1. Tested Environment

| Item | Value |
|---|---|
| Board | BigTreeTech Pi2 v1.0.1 (RK3566) |
| OS | Armbian (Debian Trixie) |
| Kernel | 7.3.4 |
| RKNN Runtime | librknnrt v2.3.2 |

## 2. Devfreq (DVFS)

The driver supports dynamic voltage and frequency scaling via the standard Linux devfreq framework. When an OPP table is present in the DTB, the driver automatically registers a devfreq device at `/sys/class/devfreq/fde40000.npu/`.

Available frequencies (RK3566): 200 / 297 / 400 / 600 / 700 / 800 MHz.

```sh
# Check current frequency
cat /sys/class/devfreq/fde40000.npu/cur_freq

# List available frequencies
cat /sys/class/devfreq/fde40000.npu/available_frequencies

# Switch to manual frequency control
echo userspace > /sys/class/devfreq/fde40000.npu/governor
echo 800000000 > /sys/class/devfreq/fde40000.npu/min_freq
echo 800000000 > /sys/class/devfreq/fde40000.npu/max_freq

# Restore automatic governor
echo rknpu_ondemand > /sys/class/devfreq/fde40000.npu/governor
echo 200000000 > /sys/class/devfreq/fde40000.npu/min_freq
echo 800000000 > /sys/class/devfreq/fde40000.npu/max_freq
```
## 3. TODO
  IOCTL control over userpace=======>Work in prfogress!!!
