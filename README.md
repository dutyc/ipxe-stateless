# ipxe-ferry

基于**上游 iPXE + 补丁（Patch）机制**的自动化固件构建仓库。

本仓库**不包含 iPXE 源码**，只保存：

- `patches/` —— 对上游源码的全部修改（git apply 可直接应用）
- `embed/` —— EMBED 引导脚本等构建资产
- `build/` —— 自动化构建流水线
- `dist/` —— 构建产物（不入库，由流水线生成）

## 设计动机

对 iPXE 源码的修改以**直接 fork 分支**维护的成本太高（上游升级需持续合并）。本仓库改为：

```
上游 ipxe 源码（固定基线 commit）
    +
patches/（差异文件，唯一事实来源）
    +
embed/（脚本资产）
    ↓ build/build.sh
dist/（五类固件 + SHA256SUMS）
```

补丁全部基于**固定上游基线**生成，升级上游时重新生成补丁即可（见 [patches/README.md](patches/README.md)）。

## 目录结构

```
ipxe-ferry/
├── patches/                 # 源码补丁集（按编号顺序应用）
│   ├── 0001-realtek-8125-adaptation.patch
│   └── 0002-makefile-ipxe-debug.patch
├── embed/
│   └── auto.ipxe            # EMBED 自动引导脚本
├── build/
│   └── build.sh             # 自动化构建流水线
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

## 与现有工作区的关系

- 旧工作区 `ipxe/`（含源码与行尾噪声）仅作为补丁来源的参照，不再承担构建职责
- 部署侧说明见 `ipxe-all-ready/` 文档（引导介质制作、控制面等）
