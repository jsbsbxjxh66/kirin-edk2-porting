#!/bin/bash
#
# Platform hook for Huawei Mate 10 Pro (BLA-AL00) / Kirin 970
#
# Mainline Linux kernel embedded in firmware volume
#
# To enable:
# 1. Uncomment MAINLINE_KERNEL_IN_FV=1 in configs/devices/bla-al00.conf
# 2. Set MAINLINE_KERNEL_IN_FV = 1 in Platform/HiSilicon/kirin970/kirin970.dsc [Defines]
# 3. Increase FD_SIZE and NumBlocks (see configs/kirin970.conf comments)
# 4. Place your files:
#      mainline/Image.lz4     - LZ4-compressed mainline kernel Image
#      mainline/kirin970.dtb  - Device tree blob
#      mainline/initrd.img    - Initial ramdisk
# 5. Uncomment timeout/default/mainline-linux in Platform/HiSilicon/kirin970/simpleinit.static.uefi.cfg
# 6. Build: ./build.sh -d bla-al00
#
# SimpleInit will show a boot menu with countdown, then auto-boot mainline Linux.
#
