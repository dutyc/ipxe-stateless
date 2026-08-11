# 定制内容

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
dist/（六类固件 + SHA256SUMS）
```

补丁全部基于**固定上游基线**生成，升级上游时重新生成补丁即可（见 [patches/README.md](../patches/README.md)）。

## 定制清单

相对上游 iPXE 基线（默认 `e6e51ccb`）的全部修改，共三个补丁；另有**构建级 EMBED 定制**（`embed/auto.ipxe`，经 `EMBED=` 编译进固件，非补丁）：

### 1. RTL8125 全系适配（`0001`）

- **背景**：RTL8125（2.5G）网卡必须由 iPXE native 驱动接管——固件 SNP 驱动在 iSCSI 挂载场景存在挂起缺陷，无法用于无盘引导；而上游 iPXE 对部分 8125 版本（XID 0x688 系列）支持不完整。
- **修改**：`src/drivers/net/realtek.c`、`realtek.h`
  - XID 0x688 版本表与设备识别
  - EPHY 初始化表（2.5G PHY 配置）
  - 32 位中断状态寄存器
  - FETCH/PAUSE_SLOT 配置
  - BAR 0x4808（2.5G 专用寄存器窗口）
  - TPPOLL_8125 轮询方式

### 2. debug 构建修复（`0002`）

- **背景**：`ipxe-debug.efi` 目标未定义驱动集，构建产物为空壳（无任何网卡驱动），无法用于故障定位。
- **修改**：`src/Makefile` 新增 `DRIVERS_ipxe-debug += $(DRIVERS_ipxe)`，debug 目标继承全驱动集。

### 3. snponly 本地引导支持（`0003`）

- **背景**：官方 `snponly.efi` 仅支持固件 PXE 链加载场景（只接管加载 iPXE 的那个设备）；从本地 UEFI（U 盘/磁盘）引导时找不到任何网卡，直接进入 shell。这是“主板无 PXE 网络启动选项”场景的配套适配——本地介质引导时同样需要网络接管能力。
- **修改**：`src/drivers/net/efi/snponly.c`——链加载定位失败时回退接管全部 SNP/NII/MNP 设备；PXE 链加载场景行为不变。
- **作用**：native 驱动不可用（如特定主板 RTL8168 初始化挂起）的机器，本地引导也有 SNP 兜底路径。
