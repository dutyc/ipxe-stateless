# iPXE-Stateless

[![License](https://img.shields.io/badge/License-GPL--2.0-green)](LICENSE)
[![GitHub stars](https://img.shields.io/github/stars/dutyc/ipxe-stateless)](https://github.com/dutyc/ipxe-stateless)
[![Version](https://img.shields.io/github/v/tag/dutyc/ipxe-stateless)](https://github.com/dutyc/ipxe-stateless/releases)
[![Platform](https://img.shields.io/badge/Platform-x86_64%20UEFI%2FBIOS-0f766e)](docs/network-support.zh-CN.md)
[![Upstream](https://img.shields.io/badge/Upstream-iPXE%20e6e51ccb-111111)](patches/README.zh-CN.md)
[![Patches](https://img.shields.io/badge/Patches-7-7c3aed)](docs/customizations.zh-CN.md)

[English](README.md) | [中文](README.zh-CN.md)

**iPXE-Stateless** 是面向无状态（Stateless）云原生计算环境的**网络引导固件构建仓库**：不包含 iPXE 源码，仅维护差异补丁与构建资产，可在任意上游基线之上重建。作为 iPXE-All-Ready 平台的固件底座——主仓库让算力无状态，本仓库让引导固件无状态。

> **分支：`research/nvme-of`** — NVMe-oF（NVMe over TCP）SAN 引导实验分支：nvmetcp 驱动、DH-HMAC-CHAP 认证、NBFT 消费种子模块与测试工具。探索性开发与 `main` 主干隔离，稳定后可能合入 `main`。

----

## 定制内容

对上游 iPXE 的全部修改以 `git apply` 可应用的补丁维护在固定基线（`e6e51ccb`）上，当前共七个：RTL8125（2.5G）与 RTL8126（5G）原生驱动、SNP 本地引导兜底、realtek 调试构建、设备信息采集（SMBIOS 内存 + 网卡芯片名）、NVMe-oF（NVMe over TCP）SAN 支持（含 DH-HMAC-CHAP 认证）与认证/状态机修复。设计动机与实现详见 **[docs/customizations.zh-CN.md](./docs/customizations.zh-CN.md)**；网卡支持矩阵与实测记录见 [docs/network-support.zh-CN.md](./docs/network-support.zh-CN.md)。

## 快速开始

```bash
./build/build.sh    # 完整构建：拉取源码 -> 应用补丁 -> 构建 -> 归档
```

环境要求 Linux + `git` / `make` / `gcc`。产物输出至 `dist/`（10 种形态 + SHA256SUMS），完整列表与选用指南见 [docs/build-artifacts.zh-CN.md](./docs/build-artifacts.zh-CN.md)。

## 文档

- [定制详解](./docs/customizations.zh-CN.md) — 每个补丁的设计动机与实现
- [网卡支持矩阵](./docs/network-support.zh-CN.md) — 覆盖情况与实测记录
- [设备信息上报](./docs/device-info-reporting.zh-CN.md) — 采集变量清单与用法
- [构建产物](./docs/build-artifacts.zh-CN.md) — 产物列表、校验与选用
- [NVMe-OF 使用指南](./docs/nvmeof-usage.md) — NVMe over TCP SAN 引导用法：nvmet 服务端配置（含 DH-HMAC-CHAP 认证）、`sanboot` 用法、QEMU 验证方法
- [能力实现参考（iPXE-All-Ready 集成用）](./docs/capability-reference.md) — 固件能力实现与接口契约，认证凭证注入链路详解
- [NVMe-OF 测试流程](./docs/nvmeof-test-procedure.md) — 端到端测试流程：`test/` 交付脚本、GRUB 引导盘、QEMU 轮次、pcap 分析
- [补丁集说明](./patches/README.zh-CN.md) — 授权边界与上游升级流程
- [RTL8126 移植审计](./docs/8126-porting-audit.md) — 双来源移植审计记录（中文）

## 主仓库

**[iPXE-All-Ready](https://github.com/dutyc/ipxe-all-ready)** —— 把「无状态」贯彻到算力层本身的云原生平台：算力不绑定任何具体硬件，节点插电即活、可丢弃、可瞬间重建。同一理念的一体两面：主仓库让算力无状态，本仓库让引导固件无状态。

## 社区与贡献

欢迎 Star / Watch / Issues / Pull Requests。与主仓库一致：**AI 可以写语法，架构必须由人脑理解**。

## 许可证

本仓库整体遵循 **[GPL-2.0](./LICENSE)**，与上游 iPXE（GPL-2.0-or-later / UBDL 双许可）兼容；参考第三方驱动的适配部分**仅按 GPL-2.0 授权**，不得以 UBDL 或更高版本许可再分发——详见 [patches/README.zh-CN.md](./patches/README.zh-CN.md)。
