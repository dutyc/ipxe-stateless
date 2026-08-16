# 定制内容

[English](customizations.md) | [中文](customizations.zh-CN.md)

本仓库相对上游 iPXE 基线（默认 `e6e51ccb`）的全部定制 = **五个补丁** + **构建级 EMBED 定制**（`embed/auto.ipxe`，经 `EMBED=` 编译进固件，非补丁）：

| # | 补丁 | 内容 |
|---|---|---|
| 0001 | `0001-realtek-8125-adaptation.patch` | RTL8125（2.5G）全系 native 驱动适配 |
| 0002 | `0002-makefile-ipxe-debug.patch` | debug 构建修复 |
| 0003 | `0003-snponly-local-boot.patch` | snponly 本地引导支持 |
| 0004 | `0004-realtek-8126-adaptation.patch` | RTL8126（5G）native 驱动适配 |
| 0005 | `0005-device-info-collection.patch` | 设备信息采集：SMBIOS type 17 内存设置（`mem-total` / `mem-type` / `mem-speed`）+ PCI 设备表名经 `${net0/chip}` 暴露 |

## 设计动机

很多主板没有 PXE 网络启动选项，或设置繁琐（BIOS 无 Network Boot 入口、默认关闭 UEFI 网络栈、需逐台进 BIOS 配置且 Secure Boot 限制多）。与其依赖主板 PXE，不如提供可经本地介质（USB / 磁盘 / GRUB2 链加载）引导、启动即自动进入网络引导流程的固件——`embed/auto.ipxe`（EMBED 定制）与 `0003`（SNP 固件本地引导适配）即为此而作。

对这些定制的维护采用补丁机制而非直接 fork：fork 分支在上游升级时需要持续合并，成本太高。本仓库改为：

```
上游 ipxe 源码（固定基线 commit）
    +
patches/（差异文件，唯一事实来源）
    +
embed/（脚本资产）
    ↓ build/build.sh
dist/（十个固件产物 + SHA256SUMS）
```

补丁全部基于**固定上游基线**生成，升级上游时重新生成补丁即可（见 [patches/README.zh-CN.md](../patches/README.zh-CN.md)）。

## 定制详解

### 1. RTL8125 全系适配（`0001`）

- **背景**：RTL8125（2.5G）网卡必须由 iPXE native 驱动接管——固件 SNP 驱动在 iSCSI 挂载场景存在挂起缺陷，无法用于无盘引导；而上游 iPXE 对部分 8125 版本（XID 0x688 系列）支持不完整。
- **修改**：`src/drivers/net/realtek.c`、`realtek.h`
  - XID 0x688 版本表与设备识别
  - EPHY 初始化表（2.5G PHY 配置）
  - 32 位中断状态寄存器
  - FETCH/PAUSE_SLOT 配置
  - BAR 0x4808（2.5G 专用寄存器窗口）
  - TPPOLL_8125 轮询方式
- **许可**：8125 适配部分参考 Linux 内核 r8169 驱动（GPL-2.0-only），仅按 GPL-2.0 授权，不得以 UBDL 再分发（见 `patches/0001` 头部声明）。

### 2. debug 构建修复（`0002`）

- **背景**：`ipxe-debug.efi` 目标未定义驱动集，构建产物为空壳（无任何网卡驱动），无法用于故障定位。
- **修改**：`src/Makefile` 新增 `DRIVERS_ipxe-debug += $(DRIVERS_ipxe)`，debug 目标继承全驱动集。

### 3. snponly 本地引导支持（`0003`）

- **背景**：官方 `snponly.efi` 仅支持固件 PXE 链加载场景（只接管加载 iPXE 的那个设备）；从本地 UEFI（U 盘/磁盘）引导时找不到任何网卡，直接进入 shell。这是"主板无 PXE 网络启动选项"场景的配套适配——本地介质引导时同样需要网络接管能力。
- **修改**：`src/drivers/net/efi/snponly.c`——链加载定位失败时回退接管全部 SNP/NII/MNP 设备；PXE 链加载场景行为不变。
- **作用**：native 驱动不可用（如特定主板 RTL8168 初始化挂起）的机器，本地引导也有 SNP 兜底路径。

### 4. RTL8126 5GbE 适配（`0004`）

- **背景**：RTL8126（5G）为 2024 年新卡，上游 iPXE 基线无 0x8126 设备项；其 GPHY 需按 ICVerID 分三种 PHY 配置方法（静态寄存器表）初始化，且部分 PCIe 配置（ZRXDC 超时上报、ASPM 入口延迟）需经 CSI 机制访问扩展配置空间，基线驱动均不具备。
- **修改**：`src/drivers/net/realtek.c`、`realtek.h`
  - `0x8126` 设备项与 `realtek_detect_8126`（TxConfig 0x64800000 族检测 + ICVerID → mcfg 1/2/3 分派）
  - GPHY OCP 接口函数（`realtek_gphy_ocp_read/write/modify`，重构 0001 内联 MII 访问并复用）
  - CSI 扩展配置空间接口（`realtek_csi_read/write/modify`，Linux `rtl_csi_*` 同机制）
  - PHY 静态配置表 ×3（`realtek_8126a_1/2/3_phy`，共 367 项，源自官方 r8126 驱动；剔除 MCU 微码段；区分直接写与读改写语义，分别对应官方 `rtl8126_mdio_direct_write_phy_ocp` 与 `rtl8126_clear_and_set_eth_phy_ocp_bit`）与 `realtek_hw_phy_config_8126`
  - `realtek_hw_start_8126`：ZRXDC 关闭 + ASPM 默认入口延迟（CSI 路径）+ 8125 公共初始化 + PHY 配置
  - 挂载：`realtek_detect` / `realtek_open` / `realtek_probe` 按 `mac_ver == 70` 分派
- **验证**：完整构建 10 产物全部通过（含 `DEBUG=realtek:3` debug 目标）；待物理机实测。
- **审计**：双来源 PHY 表对照审计与 PHY MCU 微码轻量方案（版本检查）详见 [8126-porting-audit.md](8126-porting-audit.md)。
- **许可**：8126 适配部分参考 Realtek r8126 驱动（GPL-2.0-only，Copyright 2025 Realtek Semiconductor Corp.）与 Linux 内核 r8169（GPL-2.0-only），仅按 GPL-2.0 授权，不得以 UBDL 再分发（见 `patches/0004` 头部声明）。

### 5. 设备信息采集（`0005`）

- **背景**：固件侧设备信息采集（身份 / CPU / 内存 / 网卡）供 HTTP 上报。身份（SMBIOS type 1-3）与 CPU（CPUID）设置官方已有，缺口有二：SMBIOS type 17（内存设备，每插槽一条）无具名设置；PCI 网卡从不填充 `driver_name`，导致 `${net0/chip}` 在 PCI 上不可用。
- **修改**：`src/include/ipxe/smbios.h`、`src/interface/smbios/smbios_settings.c`、`src/drivers/bus/pci.c`
  - `struct smbios_memory_device`（type 17 布局经 dmidecode 三个版本交叉验证：Memory Type `0x12`、Speed `0x15`、Extended Size `0x1C`）+ `SMBIOS_TYPE_MEMORY_DEVICE 17`
  - `${mem-total}`（uint32 MB，全插槽聚合：`0xFFFF` 跳过、`0x7FFF` 回退 Extended Size、bit15=GB）、`${mem-type}`（首槽，映射为 `DDR5` 等字符串）、`${mem-speed}`（首槽，MT/s）——按名分派的自定义 fetch，复用现有 SMBIOS 设置 scope
  - `pci_probe` 现从匹配的设备表项填充 `driver_name`，使 `${net0/chip}`（如 `RTL8125`）对所有 PCI 网卡生效
- **用法**：设置项清单与上报 URL 模板见 [device-info-reporting.zh-CN.md](device-info-reporting.zh-CN.md)。
- **验证**：完整构建 10 产物全部通过；设置已编入（字符串验证）；真机行为待实测（空格 / 特殊字符的 URL 编码）。

## EMBED 自动引导脚本

`embed/auto.ipxe` 属于**配置资产**（非源码补丁）：改动无需重新生成补丁，直接修改文件后重新构建即可（见 [patches/README.zh-CN.md](../patches/README.zh-CN.md)）。
