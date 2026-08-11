# 网卡支持矩阵

本仓库固件对网卡的支持情况（上游基线 `e6e51ccb` + 定制补丁），基于源码驱动设备表（`src/drivers/net/`）与本项目实测记录整理。上游升级或新增补丁后可能变化。

状态图例：

- **覆盖良好** —— 驱动成熟或实测通过，可放心使用
- **有条件** —— 驱动存在但有已知问题或限制
- **不支持** —— 基线无驱动（含本项目补丁范围）

## 覆盖良好

| 网卡 | 驱动 | 说明 |
|---|---|---|
| Intel e1000e 全系（82540 → I219 最新步进） | intel.c | **物理机实测通过**（direct-uefi ipxe.efi） |
| Intel I210 / I211 / I350 / 82576 | intel.c | 含服务器常见 I350 |
| **Intel I225 / I226**（2.5G，主流主板） | intel.c | 全步进覆盖：I225（0x15f2/15f3/0d9f/5502）、I226（0x125b/125c/125d） |
| Intel 82599 / X540 / X550 / X553 / X552（10G） | intelx.c | 含 X553-AT（10GBASE-T） |
| Intel X710 / XL710（10G / 40G） | intelxl.c | 服务器常见 |
| Intel E810 / E823（25G / 100G） | ice.c | 新驱动 |
| Realtek RTL8136 / 8139 / 8129 / 8167 / 8168 / 8169 | realtek.c | 基线原生 |
| **Realtek RTL8125 全系**（2.5G） | realtek.c（0001 补丁） | **物理机实测 DHCP 通过**；原生驱动适配 |
| Marvell/Aquantia AQC107 / 108 / 109 / 111（10G / 5G / 2.5G） | aqc1xx.c | 高端主板 / 工控常见 |
| Broadcom BCM57xx 千兆（tg3，82 设备） | tg3 | 覆盖广 |
| Broadcom BNX2（BCM5706/5708 等） | bnx2 | 1G/10G 旧系 |
| Broadcom NetXtreme-E（BCM957xxx） | bnxt | 服务器新系 |
| 虚拟化：virtio / vmxnet3 / ENA(AWS) / GVE(GCP) / netvsc(Hyper-V) | — | 云原生场景全覆盖 |
| USB：AX88179（axge）、LAN7800（lan78xx）、SMSC95xx/75xx、DM96xx、CDC ECM/NCM | — | USB 网卡仅此几类 |

## 不支持（重点风险）

| 网卡 | 场景 | 备注 |
|---|---|---|
| **Realtek RTL8126**（2024 新 2.5G） | 新主板 | 基线与本项目补丁均无 0x8126 |
| **Realtek USB RTL8152 / 8153 / 8156** | 笔记本 USB-C 转 RJ45（最常见） | iPXE 无此驱动 |
| **Mellanox ConnectX-4 / 5 / 6 / 7**（25G / 100G） | 服务器 | 无 mlx5；仅 ConnectX-3 及更早（arbel/hermon/golan/linda）且功能有限 |
| Broadcom bnx2x 系（BCM57710/57711/57712，10G） | 服务器 | 基线无 |
| 现代 WiFi 全系（Intel AX、Realtek 88 系列） | 笔记本 | 仅古董 prism2 / rtl818x |
| Aquantia AQC100 / AQC113 新版（0x12b1 系） | 10G | 不在 aqc1xx.c 设备表 |

## 有条件（已知问题）

- **Realtek RTL8168 部分版本**：native 驱动初始化挂起（本项目实证，研究终止）→ 需走 SNP 兜底，见 [8168-research-log.md](8168-research-log.md)
- **SNP 驱动 iSCSI 挂起缺陷**（本项目实证）：影响**所有没有 native 驱动、只能走 UEFI 网卡驱动**的网卡——即上方“不支持”列表中的卡若走 SNP 引导同样有此风险
- **Intel I219 系列**：带 `INTEL_PBSIZE_RST` / `NO_PHY_RST` 特殊处理（历史 bug 修复，实际影响小）
- **Marvell/Aquantia AQC 系列**：驱动为 Atheros 系代码改造，成熟度一般、无硬件卸载（iSCSI 卸载等）
- **82599 / X550（SFP+）**：兼容性依赖光模块质量

## 实测记录

| 日期 | 网卡 | 固件 | 结果 |
|---|---|---|---|
| 2026-08-07 | Realtek RTL8125 | SNP 固件 | iSCSI 挂载挂起（native 驱动接管） |
| 2026-08-09 | Realtek RTL8168 | native 驱动 | 部分版本初始化挂起（SNP 兜底） |
| 2026-08-10 | Intel e1000（82540EM 等） | direct-uefi ipxe.efi | 通过 |
| 2026-08-11 | Realtek RTL8125 | 定制固件（0001 补丁） | 正常 |
