# iPXE-Stateless

[![License](https://img.shields.io/badge/License-GPL--2.0-green)](LICENSE)
[![GitHub stars](https://img.shields.io/github/stars/dutyc/ipxe-stateless)](https://github.com/dutyc/ipxe-stateless)
[![Version](https://img.shields.io/github/v/tag/dutyc/ipxe-stateless)](https://github.com/dutyc/ipxe-stateless/releases)
[![Platform](https://img.shields.io/badge/Platform-x86_64%20UEFI%2FBIOS-0f766e)](docs/network-support.zh-CN.md)
[![Upstream](https://img.shields.io/badge/Upstream-iPXE%20e6e51ccb-111111)](patches/README.zh-CN.md)
[![Patches](https://img.shields.io/badge/Patches-4-7c3aed)](docs/customizations.zh-CN.md)

[English](README.md) | [中文](README.zh-CN.md)

## 项目定位

iPXE-Stateless 是一个面向无状态（Stateless）云原生计算环境的**网络引导固件构建仓库**，采用「上游 iPXE + 补丁（Patch）机制」的定制模式。本仓库不包含 iPXE 源码，仅维护差异补丁、构建脚本与构建资产；全部定制内容可审计、可复现，并可在任意上游基线之上重建。

本仓库遵循两类无状态约束：

- **客户端无状态** —— 引导客户端不保存任何状态：启动即通过 DHCP 获取配置、链式加载引导脚本、无盘进入操作系统；
- **仓库自身无状态** —— 不维护 fork 分支，全部修改以补丁形式存在，升级上游基线仅需重新生成补丁。

## 项目使命与适用范围

**使命**：为无状态云原生计算环境提供高质量、可复现、许可证边界清晰的统一网络引导固件，并作为 iPXE-All-Ready 平台的固件底座。

**适用范围**：本仓库聚焦 **2.5G / 5G / 10G 及以上速率高性能网卡**的驱动移植与适配，当前覆盖 RTL8125（2.5G）与 RTL8126（5G）系列。

**非目标（Out of Scope）**：普通千兆 / 百兆网卡仅维持上游基线既有支持，不进行新的适配与更新。

## 关联项目

- **[iPXE-All-Ready](https://github.com/dutyc/ipxe-all-ready)** —— 主仓库，一套把「无状态」贯彻到**算力层本身**的云原生实现——真正的云原生，是让算力不绑定任何具体硬件。计算节点不持有任何属于自己的持久状态——身份、系统、数据全部由网络与控制面在外部赋予：启动时 iPXE 写入 iBFT，控制面经动态变量链注入真实身份，盘与机器彻底解耦，节点插电即被识别与注册，可丢弃、可替换、可瞬间重建，全程零人工预注册。控制面与数据面严格分离，无盘只是其最外层形态。

本仓库与 iPXE-All-Ready 是同一理念的一体两面：**主仓库把「无状态」贯彻到算力层，本仓库把「无状态」贯彻到引导固件**。固件在本仓库构建、由主仓库消费，职责严格分离，同一理念贯穿始终。

## 核心特性

- **补丁化定制，无 fork** —— 对上游 iPXE 的全部修改以 `git apply` 可直接应用的补丁维护于 `patches/`，基于固定上游基线（`e6e51ccb`）生成；升级上游仅需重新生成补丁，无需合并 fork 分支。
- **高性能网卡原生驱动** —— RTL8125（2.5G）与 RTL8126（5G）系列原生驱动补丁（0001 / 0004），提供可靠的高速网络引导能力（详见 [docs/customizations.zh-CN.md](docs/customizations.zh-CN.md)）。
- **本地引导兜底（SNP 适配）** —— 针对主板缺失 PXE 启动选项的场景，支持从本地介质（U 盘 / 磁盘 / GRUB2 链式加载）引导并接管网络。
- **EMBED 自动引导脚本** —— `embed/auto.ipxe` 编译进固件，开箱即用，无需现场配置。
- **一键可复现构建** —— 流水线自动完成源码获取、补丁验证与应用、固件构建与归档，产出 10 种固件形态及 `SHA256SUMS` 校验清单。

## 快速开始

```bash
# 完整构建（拉取源码 -> 应用补丁 -> 构建 -> 归档）
./build/build.sh

# 常用变量
UPSTREAM_COMMIT=<sha> ./build/build.sh    # 指定上游基线（默认 e6e51ccb）
JOBS=8 ./build/build.sh                   # 并行度（默认 nproc）
UPSTREAM_URL=<镜像地址> ./build/build.sh  # 更换源码源
```

环境要求：Linux + `git` / `make` / `gcc`，网络可访问上游仓库（默认 GitHub，支持镜像）。

## 构建产物

产物分类输出至 `dist/`：

| 产物 | 目标形态 | 说明 |
|---|---|---|
| `dist/pxe-uefi/ipxe.efi` | PXE 网络启动（UEFI） | 无 EMBED，脚本由 DHCP 下发 |
| `dist/pxe-uefi/ipxe-debug.efi` | 同上（debug 版） | `DEBUG=realtek:3`，故障定位用 |
| `dist/pxe-uefi/snponly.efi` | PXE 网络启动（SNP 专用，UEFI） | 无 EMBED；固件 PXE 链加载时仅接管链加载设备，链加载定位失败时回退接管全部 SNP 设备 |
| `dist/pxe-uefi/snponly-debug.efi` | 同上（debug 版） | `DEBUG=realtek:3`，故障定位用 |
| `dist/direct-uefi/ipxe.efi` | UEFI 直接引导（含 EMBED） | 内置 auto.ipxe，启动即走引导链 |
| `dist/direct-uefi/ipxe-debug.efi` | 同上（debug 版） | `DEBUG=realtek:3`，故障定位用 |
| `dist/direct-uefi/snponly.efi` | UEFI 直接引导（SNP 专用，含 EMBED） | 使用固件 SNP 协议；native 驱动不可用的机器兜底 |
| `dist/grub-bios/ipxe.lkrn` | GRUB2 BIOS 引导（含 EMBED） | `linux16 /ipxe.lkrn` |
| `dist/undionly.kpxe` | PXE 网络启动（BIOS） | 无 EMBED；无 native 驱动，经网卡 ROM 的 UNDI 接口收发；兼容一切带 PXE ROM 的网卡 |
| `dist/usb/ipxe.usb` | BIOS 引导介质（含 EMBED） | 整盘写入 U 盘 |

`dist/SHA256SUMS` 为全部产物的 SHA-256 校验清单。

## 验证状态

- **RTL8125B（2.5G）** —— 已完成物理机实测：网卡初始化、DHCP 获取配置、进入 iPXE 引导菜单均正常。
- **RTL8126（5G）** —— 驱动适配完成，实机验证待进行（含 PHY MCU 固件版本一致性检查）。
- 完整支持矩阵与实测记录见 [docs/network-support.zh-CN.md](docs/network-support.zh-CN.md)。

## 文档

| 文档 | 用途 |
|---|---|
| [docs/customizations.zh-CN.md](docs/customizations.zh-CN.md) | 设计动机与全部定制内容详解（0001-0004 补丁 + EMBED） |
| [docs/network-support.zh-CN.md](docs/network-support.zh-CN.md) | 网卡支持矩阵（覆盖良好 / 不支持 / 有条件 + 实测记录） |
| [docs/8126-porting-audit.md](docs/8126-porting-audit.md) | RTL8126 双来源移植审计与修复记录 |
| [patches/README.zh-CN.md](patches/README.zh-CN.md) | 补丁集说明、授权、上游基线升级流程 |
| [docs/8168-research-log.md](docs/8168-research-log.md) | RTL8168 研究记录（已终止） |

## 目录结构

```
ipxe-stateless/
├── patches/                 # 源码补丁集（按文件名顺序应用）
├── embed/
│   └── auto.ipxe            # EMBED 自动引导脚本
├── build/
│   └── build.sh             # 自动化构建流水线
├── docs/                    # 设计、网卡支持、研究文档
├── reference/               # 补丁应用后的参考源码快照（构建不使用）
├── dist/                    # 构建产物（不入库）
└── README.md
```

构建缓存位于 `.cache/`（可删除重建，不影响仓库）。

## 构建流程

1. **获取源码** —— 浅克隆上游 `ipxe/ipxe`，检出固定基线，每次构建前 `clean` 保证干净树。
2. **应用补丁** —— 按文件名顺序 `git apply --check` 验证后应用；任一补丁失败立即中止。
3. **安装资产** —— 拷贝 `embed/auto.ipxe` 到源码 `src/embed/`。
4. **构建** —— 按清单逐个构建（同名 EFI 目标构建前强制删除，确保 EMBED 参数生效）。
5. **归档** —— 产物分类拷贝至 `dist/` 并生成 `SHA256SUMS`。

## 上游与许可

固件构建基于上游 **iPXE**（[github.com/ipxe/ipxe](https://github.com/ipxe/ipxe)，GPL-2.0-or-later / UBDL 双许可）。本仓库不包含 iPXE 源码，全部修改以补丁形式维护在固定基线 `e6e51ccb` 上（见 [patches/README.zh-CN.md](patches/README.zh-CN.md)）。

本仓库整体遵循 **GPL-2.0**（见 [LICENSE](LICENSE)），与上游 GPL-2.0-or-later / UBDL 许可兼容；参考第三方驱动的适配部分**仅按 GPL-2.0 授权**，不得以 UBDL 或更高版本许可再分发（见各补丁头部声明及 [docs/customizations.zh-CN.md](docs/customizations.zh-CN.md)）。
