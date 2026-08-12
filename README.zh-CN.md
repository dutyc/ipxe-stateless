# iPXE-Stateless

[![License](https://img.shields.io/badge/License-GPL--2.0-green)](LICENSE)
[![GitHub stars](https://img.shields.io/github/stars/dutyc/ipxe-stateless)](https://github.com/dutyc/ipxe-stateless)
[![Version](https://img.shields.io/github/v/tag/dutyc/ipxe-stateless)](https://github.com/dutyc/ipxe-stateless/releases)
[![Platform](https://img.shields.io/badge/Platform-x86_64%20UEFI%2FBIOS-0f766e)](docs/network-support.zh-CN.md)
[![Upstream](https://img.shields.io/badge/Upstream-iPXE%20e6e51ccb-111111)](patches/README.zh-CN.md)
[![Patches](https://img.shields.io/badge/Patches-4-7c3aed)](docs/customizations.zh-CN.md)

[English](README.md) | [中文](README.zh-CN.md)

**无状态云原生 iPXE** —— 基于**上游 iPXE + 补丁（Patch）机制**的自动化固件构建仓库。

iPXE-Stateless 为无状态（Stateless）云原生环境提供统一引导固件：客户端不保存任何状态，启动即通过 DHCP 获取配置、链式加载引导脚本、无盘进入系统。仓库自身同样无状态——不包含 iPXE 源码，仅维护差异补丁与构建资产，可随时基于任意上游基线重建。

## 关联项目

- **[iPXE-All-Ready](https://github.com/dutyc/ipxe-all-ready)** —— 基于本仓库固件的完整无状态云原生无盘计算平台：中央 Controller（FastAPI 控制面 + DHCP/TFTP/HTTP 引导服务）、iSCSI Server 存储节点（stgt / LIO）、无盘 Worker 节点、Web 管理界面与文档站。

本仓库是 iPXE-All-Ready 的**固件底座**：其引导介质制作指南使用的固件即本仓库构建产物（`dist/`，RTL8125 驱动 + EMBED 自动引导版），引导 Worker 进入 iPXE 引导链后由平台接管（DHCP 下发脚本 + iSCSI 系统盘）。两个仓库职责严格分离——**ipxe-stateless 管“固件怎么构建”，iPXE-All-Ready 管“平台怎么运转”**。

## 特性

- **补丁机制、不 fork** —— 对上游 iPXE 的全部修改以 `git apply` 可直接应用的补丁维护在 `patches/` 下，基于固定上游基线（`e6e51ccb`）生成；升级上游只需重新生成补丁，无需合并 fork 分支。
- **native 网卡驱动适配** —— RTL8125（2.5G）与 RTL8126（5G）系列补丁，提供可靠的 native 驱动网络引导（详见 [docs/customizations.zh-CN.md](docs/customizations.zh-CN.md)）。
- **本地引导兜底** —— SNP 固件适配：主板无 PXE 启动选项时，可从本地介质（U 盘 / 磁盘 / GRUB2 链加载）引导并接管网络。
- **EMBED 自动引导脚本** —— `embed/auto.ipxe` 编译进固件，即插即用、无需手工配置。
- **一键可复现构建** —— 流水线自动拉取源码、应用补丁、构建并归档 10 个固件产物及 `SHA256SUMS`。

## 快速开始

```bash
# 完整构建（拉源码 -> 打补丁 -> 构建 -> 归档）
./build/build.sh

# 常用变量
UPSTREAM_COMMIT=<sha> ./build/build.sh    # 指定上游基线（默认 e6e51ccb）
JOBS=8 ./build/build.sh                   # 并行度（默认 nproc）
UPSTREAM_URL=<镜像地址> ./build/build.sh  # 更换源码源
```

环境要求：Linux + `git` / `make` / `gcc`，网络可访问上游仓库（默认 GitHub，支持镜像）。

## 使用

产物输出到 `dist/`：

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

`dist/SHA256SUMS` 为全部产物的 sha256 校验清单。

## 文档

| 文档 | 用途 |
|---|---|
| [docs/customizations.zh-CN.md](docs/customizations.zh-CN.md) | 设计动机与全部定制内容详解（0001-0004 补丁 + EMBED） |
| [docs/network-support.zh-CN.md](docs/network-support.zh-CN.md) | 网卡支持矩阵（覆盖良好 / 不支持 / 有条件 + 实测记录） |
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

## 工作机制

1. **获取源码** —— 浅克隆上游 `ipxe/ipxe`，检出固定基线，每次构建前 `clean` 保证干净树。
2. **应用补丁** —— 按文件名顺序 `git apply --check` 验证后应用；任一补丁失败立即中止。
3. **安装资产** —— 拷贝 `embed/auto.ipxe` 到源码 `src/embed/`。
4. **构建** —— 按清单逐个构建（同名 EFI 目标构建前强制删除，确保 EMBED 参数生效）。
5. **归档** —— 产物分类拷贝至 `dist/` 并生成 `SHA256SUMS`。

## 上游与许可

固件构建基于上游 **iPXE**（[github.com/ipxe/ipxe](https://github.com/ipxe/ipxe)，GPL-2.0-or-later / UBDL 双许可）。本仓库不包含 iPXE 源码，全部修改以补丁形式维护在固定基线 `e6e51ccb` 上（见 [patches/README.zh-CN.md](patches/README.zh-CN.md)）。

本仓库整体遵循 **GPL-2.0**（见 [LICENSE](LICENSE)），与上游 GPL-2.0-or-later / UBDL 许可兼容；参考第三方驱动的适配部分**仅按 GPL-2.0 授权**，不得以 UBDL 或更高版本许可再分发（见各补丁头部声明及 [docs/customizations.zh-CN.md](docs/customizations.zh-CN.md)）。
