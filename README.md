# iPXE-Stateless

**无状态云原生 iPXE** —— 基于**上游 iPXE + 补丁（Patch）机制**的自动化固件构建仓库。

定位：为无状态（Stateless）云原生环境提供统一引导固件——客户端不保存任何状态，启动即通过 DHCP 获取配置、链式加载引导脚本、无盘进入系统。固件自身亦无状态：仓库不直接包含 iPXE 源码，仅维护差异补丁与构建资产，可随时基于新基线重建。

本仓库**不包含 iPXE 源码**，只保存：

- `patches/` —— 对上游源码的全部修改（git apply 可直接应用）
- `embed/` —— EMBED 引导脚本等构建资产
- `build/` —— 自动化构建流水线
- `dist/` —— 构建产物（不入库，由流水线生成）

## 设计动机

定制源于两类需求：

**业务动机**——部分主板**没有 PXE 网络启动选项，或设置繁琐**（BIOS 无 Network Boot 入口、默认关闭 UEFI 网络栈、需逐台进 BIOS 配置且 Secure Boot 限制多）。为绕开主板 PXE 支持，提供可经本地介质（USB / 磁盘 / GRUB2 链加载）引导、启动即自动进入网络引导流程的定制固件：`embed/auto.ipxe`（EMBED 定制）与 `0003`（SNP 固件本地引导适配）即为此服务。

**维护动机**——对 iPXE 源码的修改以**直接 fork 分支**维护的成本太高（上游升级需持续合并）。本仓库改为：

```
上游 ipxe 源码（固定基线 commit）
    +
patches/（差异文件，唯一事实来源）
    +
embed/（脚本资产）
    ↓ build/build.sh
dist/（六类固件 + SHA256SUMS）
```

补丁全部基于**固定上游基线**生成，升级上游时重新生成补丁即可（见 [patches/README.md](patches/README.md)）。

## 定制内容

相对上游 iPXE 基线（默认 `e6e51ccb`）的全部修改，共三个补丁；另有**构建级 EMBED 定制**（`embed/auto.ipxe`，经 `EMBED=` 编译进固件，非补丁）——为无 PXE 启动选项的主板提供本地介质自动网络引导：

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

## 上游依赖与许可

本仓库的固件构建与定制基于两个上游项目：

- **iPXE**（[github.com/ipxe/ipxe](https://github.com/ipxe/ipxe)，GPL-2.0-or-later / UBDL 双许可）——固件基础源码。本仓库不包含其源码，仅以补丁形式维护对其的全部修改；补丁基于固定基线 `e6e51ccb` 生成，升级上游时重新生成（见 [patches/README.md](patches/README.md)）。

- **Linux 内核 r8169 驱动**（`drivers/net/ethernet/realtek/r8169_main.c`，GPL-2.0）——`0001` 补丁中 RTL8125 适配（XID 版本表、EPHY 初始化、电源管理等）的寄存器级参考实现，原生驱动行为与 Linux 对齐。

本仓库整体遵循 **GPL-2.0**（见 [LICENSE](LICENSE)），覆盖对上游 iPXE 的全部修改（含参考 Linux 内核代码的部分），与上游 GPL-2.0-or-later 及 Linux 内核 GPL-2.0 许可兼容。其中参考 Linux 内核的 8125 适配部分**仅按 GPL-2.0 授权**，不得以 UBDL 或更高版本许可再分发。

补丁应用后的完整源码快照见 [reference/](reference/)（含 `0001` 的 realtek.c 与 `0003` 的 snponly.c），便于直接阅读修改内容；构建不直接使用该目录，仍由 `build.sh` 从上游基线 + 补丁现场生成，升级补丁时须同步更新快照。

## 目录结构

```
ipxe-stateless/
├── patches/                 # 源码补丁集（按编号顺序应用）
│   ├── 0001-realtek-8125-adaptation.patch
│   ├── 0002-makefile-ipxe-debug.patch
│   └── 0003-snponly-local-boot.patch
├── embed/
│   └── auto.ipxe            # EMBED 自动引导脚本
├── build/
│   └── build.sh             # 自动化构建流水线
├── reference/               # 补丁应用后的参考源码快照（构建不使用）
│   └── src/drivers/net/
│       ├── realtek.c        # 0001 应用后：含 RTL8125 全系适配
│       └── efi/snponly.c    # 0003 应用后：snponly 本地引导支持
├── dist/                    # 构建产物（.gitignore）
└── README.md
```

## 快速开始

```bash
# 完整构建（拉源码 -> 打补丁 -> 构建 -> 归档）
./build/build.sh

# 常用变量
UPSTREAM_COMMIT=<sha> ./build/build.sh   # 指定上游基线
JOBS=8 ./build/build.sh                  # 并行度（默认 nproc）
UPSTREAM_URL=<镜像地址> ./build/build.sh # 更换源码源
```

产物输出到 `dist/`：

| 产物 | 目标形态 | 说明 |
|---|---|---|
| `dist/pxe-uefi/ipxe.efi` | PXE 网络启动（UEFI） | 无 EMBED，脚本由 DHCP 下发 |
| `dist/direct-uefi/ipxe.efi` | UEFI 直接引导（含 EMBED） | 内置 auto.ipxe，启动即走引导链 |
| `dist/direct-uefi/ipxe-debug.efi` | 同上（debug 版） | `DEBUG=realtek:3`，故障定位用 |
| `dist/direct-uefi/snponly.efi` | UEFI 直接引导（SNP 专用，含 EMBED） | 无 native 驱动，用固件 SNP 协议；固件 PXE 链加载时仅接管链加载设备，本地引导（U 盘/磁盘）时回退接管全部 SNP 设备，native 驱动不可用的机器兜底 |
| `dist/grub-bios/ipxe.lkrn` | GRUB2 BIOS 引导（含 EMBED） | `linux16 /ipxe.lkrn` |
| `dist/usb/ipxe.usb` | BIOS 引导介质（含 EMBED） | 整盘写入 U 盘 |

`dist/SHA256SUMS` 为全部产物的 sha256 校验清单。

## 工作机制

1. **获取源码**：浅克隆上游 `ipxe/ipxe`，检出固定基线（默认 `e6e51ccb`），每次构建前 `clean` 保证干净树
2. **应用补丁**：按文件名顺序 `git apply --check` 验证后应用；任一补丁失败立即中止并提示
3. **安装资产**：拷贝 `embed/auto.ipxe` 到源码 `src/embed/`
4. **构建**：按清单逐个构建（同名 EFI 目标构建前强制删除，确保 EMBED 参数生效）
5. **归档**：产物分类拷贝至 `dist/` 并生成 `SHA256SUMS`

源码与构建缓存位于 `.cache/`（可删除重建，不影响仓库）。

## 环境要求

- Linux + `git` / `make` / `gcc`（iPXE x86_64 构建工具链）
- 网络可访问上游仓库（默认 GitHub，可换镜像）
