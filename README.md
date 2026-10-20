# Kirin EDK2 Porting

面向 **海思麒麟（HiSilicon Kirin）系列 SoC** 的 EDK2 UEFI 固件移植项目，基于
[Renegade Project](https://github.com/edk2-porting/) 的 edk2-msm 框架，并集成
[SimpleInit](https://github.com/BigfootACA/simple-init) 图形化启动菜单。

本仓库仅保留麒麟 / 海思平台，按 SoC 分层组织代码，便于后续逐步扩展至更多麒麟
处理器。当前已完成 **Kirin 970** 的移植，可用于引导 mainline Linux、Android，以及
作为多系统启动管理器。

## 麒麟 SoC 支持

图例：✅ 已支持　🚧 开发中　⏳ 计划中　❌ 未支持

### 旗舰

| SoC | 发布年份 | 架构 | 状态 | 备注 |
| --- | --- | --- | --- | --- |
| Kirin 970 | 2017 | AArch64 | ✅ 已支持 | 本仓库当前实现 |
| Kirin 960 | 2016 | AArch64 | ⏳ 计划中 | |
| Kirin 955 | 2016 | AArch64 | ⏳ 计划中 | |
| Kirin 950 | 2015 | AArch64 | ⏳ 计划中 | |
| Kirin 980 | 2018 | AArch64 | ⏳ 计划中 | |
| Kirin 985 | 2020 | AArch64 | ⏳ 计划中 | |
| Kirin 990 4G | 2019 | AArch64 | ⏳ 计划中 | |
| Kirin 990 5G | 2019 | AArch64 | ⏳ 计划中 | |
| Kirin 9000 / 9000E | 2020 | AArch64 | ⏳ 计划中 | |
| Kirin 9000S | 2023 | AArch64 | ⏳ 计划中 | |
| Kirin 9010 | 2024 | AArch64 | ⏳ 计划中 | |

### 中端 / 入门

| SoC | 发布年份 | 架构 | 状态 | 备注 |
| --- | --- | --- | --- | --- |
| Kirin 710 / 710F | 2018 | AArch64 | ⏳ 计划中 | |
| Kirin 810 | 2019 | AArch64 | ⏳ 计划中 | |
| Kirin 820 | 2020 | AArch64 | ⏳ 计划中 | |
| Kirin 658 / 659 | 2017 | AArch64 | ⏳ 计划中 | |
| Kirin 650 / 655 | 2016 | AArch64 | ⏳ 计划中 | |
| Kirin 620 | 2014 | AArch64 | ⏳ 计划中 | |

> 目前仓库内只有 `Silicon/HiSilicon/kirin970`（含其共享包 `KirinPkg`）的实现，
> 上表中除 Kirin 970 外的处理器均**尚未开始适配**。新增 SoC 的方式见
> [新增 SoC / 机型](#新增-soc--机型)。

## 目前支持的机型

| 机型 | 型号 | SoC | 厂商 | 底包 | 平台目录 | 状态 |
| --- | --- | --- | --- | --- | --- | --- |
| Huawei Mate 10 Pro | `BLA-AL00` | Kirin 970 | Huawei | Android EMUI 9.1 | `Platform/Huawei/kirin970` | ✅ 已支持 |
| Honor Play | `COR-AL00` | Kirin 970 | Honor | Android EMUI 9.1 | `Platform/Honor/kirin970` | ✅ 已支持 |

两台机型均为 Kirin 970，且均基于 **Android EMUI 9.1 底包（原厂固件）** 适配，
共用 `Platform/HiSilicon/kirin970` 与 `Silicon/HiSilicon/kirin970/KirinPkg`。

## 功能支持（Kirin 970）

图例：✅ 已实现并可用　⚠️ 已实现，待验证或不稳定　❌ 未支持

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| 显示 / 帧缓冲 | ✅ | `SimpleFbDxe`，1080 × 2160 |
| UFS 存储 | ✅ | `KirinUfsHcDxe` + `UfsPassThruDxe` |
| USB 主机（XHCI） | ✅ | `KirinDwc3Dxe` + `XhciDxe` |
| GPIO 按键（电源 / 音量） | ✅ | `KirinGpioKeyDxe` |
| 硬件看门狗 | ✅ | `KirinWdtDxe`（ARM SP805） |
| RTC | ✅ | `VirtualRealTimeClockLib` |
| PMIC / 充电 | ✅ | `KirinHi6523Lib` |
| 重启到 recovery / bootloader / fastboot | ✅ | `KirinRebootModeDxe` + Android BCB |
| SimpleInit 启动菜单 | ✅ | 支持鼠标、键盘操作 |
| USB 大容量存储 / OTG / USB 主机应用 | ⚠️ | 已集成，待设备验证 |
| USB 设备模式（内置 fastboot） | ⚠️ | 枚举不稳定，热插拔时可能出现「设备描述符请求失败」 |
| ADB | ❌ | 尚未启用 |
| 触摸屏 | ❌ | 无驱动 |
| ACPI | ❌ | 当前仅提供设备树（DT）路径 |

## 新增 SoC / 机型

框架按 SoC 与厂商分层，新增支持时通常需要：

1. **SoC 层**：`Silicon/HiSilicon/<soc>/`（驱动、库）与 `configs/<soc>.conf`（FD 基址 / 大小）。
2. **公共包**：`Silicon/HiSilicon/<soc>/KirinPkg/` 放置跨 SoC 复用的驱动（UFS、DWC3、GPIO、看门狗、帧缓冲等）。
3. **平台层**：`Platform/<厂商>/<soc>/`（DSC / FDF / DTB / 平台库）。
4. **机型配置**：`configs/devices/<型号>.conf`，设置 `SOC_PLATFORM`、`SOC_VENDOR`、`VENDOR_NAME`、`PLATFORM_NAME`。
5. 在 `build.sh` 中通过 `-d <型号>` 构建。

## 已知问题

- **USB 设备模式（DWC3 gadget）**：内置 fastboot 依赖该通路，热插拔时设备枚举
  可能失败，表现为主机侧「设备描述符请求失败」。相关 DWC3 行为已按内核实现对齐，
  仍在设备上验证中。
- **触摸屏**：驱动未移植，启动菜单需使用物理按键 / 鼠标 / 键盘操作。
- **ADB**：`KirinUsbAdb` 尚未加入构建。

## 构建

依赖：`clang`、`llvm`、`iasl`、`nasm`、`gcc-aarch64-linux-gnu`、`uuid-dev`、
`python3`、`gettext` 等（Ubuntu 下可直接参考 `.github/workflows`）。

```bash
# Huawei Mate 10 Pro
./build.sh -d bla-al00

# Honor Play
./build.sh -d cor-al00
```

产物为 `boot-<device>.img`，可通过 `fastboot flash kernel boot-<device>.img` 启动。

常用选项：

| 选项 | 说明 |
| --- | --- |
| `--skip-rootfs-gen` | 跳过 SimpleInit rootfs 生成，加快构建 |
| `-u` / `--uart` | 开启 UART 调试输出 |
| `-r DEBUG` | 构建 DEBUG 版本（默认 RELEASE） |
| `-a` / `--all` | 构建所有机型 |
| `-C` / `--clean` | 清理 workspace 与输出 |

## 目录结构

```
Platform/Huawei/kirin970/    Huawei Mate 10 Pro（BLA-AL00）平台描述
Platform/Honor/kirin970/     Honor Play（COR-AL00）平台描述
Platform/HiSilicon/kirin970/ Kirin 970 公共平台描述（DSC/FDF 公共部分）
Silicon/HiSilicon/kirin970/          Kirin 970 芯片驱动与库
Silicon/HiSilicon/kirin970/KirinPkg/ Kirin 公共包（UFS、DWC3、GPIO、看门狗、帧缓冲等）
configs/devices/             机型配置（bla-al00、cor-al00）
configs/kirin970.conf        Kirin 970 FD 基址 / 大小
```

## 用户文档

参见 [Renegade Project Wiki](https://wiki.renegade-project.cn/)。

## 致谢

- Gustave Monce and his [SurfaceDuoPkg](https://github.com/WOA-Project/SurfaceDuoPkg)
- [DuoWoa Project](https://github.com/WOA-Project)
- [EFIDroid](https://github.com/efidroid)
- [Ben (Bingxing) Wang](https://github.com/imbushuo/)
- [Lumia950XLPkg](https://github.com/WOA-Project/Lumia950XLPkg)
- BigfootACA and his [SimpleInit](https://github.com/BigfootACA/simple-init) and [LinuxSimpleMassStorage](https://github.com/BigfootACA/linux-simple-mass-storage)
- fxsheep and his original edk2-sagit
- All the developers and members of [Renegade Project](https://github.com/edk2-porting/) for offering efforts, equipments, valuable documents and more

## License

All code except drivers in the `GPLDrivers` directory are licensed under BSD 2-Clause.
GPL Drivers are licensed under GPLv2 license.
