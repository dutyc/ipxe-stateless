# 补丁集说明

所有补丁基于**同一上游基线**生成，应用顺序 = 文件名顺序。

## 基线

- 上游仓库：`https://github.com/ipxe/ipxe.git`（国内可换 `https://gitee.com/mirrors/ipxe.git`）
- 基线提交：`e6e51ccbf17ff40a899c8859fb4e95abd5cfcd57`（master）
- 重新生成补丁时，在构建缓存工作树（`.cache/ipxe-upstream`，已含补丁与修改）中：

  ```bash
  git -C .cache/ipxe-upstream diff > patches/NNNN-xxx.patch
  ```

## 补丁清单

| 补丁 | 修改文件 | 内容 |
|---|---|---|
| `0001-realtek-8125-adaptation.patch` | `src/drivers/net/realtek.c`、`realtek.h` | RTL8125 全系适配（XID 0x688 版本表、EPHY 初始化表、32 位中断寄存器、FETCH/PAUSE_SLOT、BAR 0x4808、TPPOLL_8125） |
| `0002-makefile-ipxe-debug.patch` | `src/Makefile` | 新增 `DRIVERS_ipxe-debug` 定义（debug 目标继承全驱动集，修复 `obj_ipxe_debug` 链接失败） |
| `0003-snponly-local-boot.patch` | `src/drivers/net/efi/snponly.c` | snponly 本地引导支持：链加载定位失败时（本地 UEFI 引导）回退接管全部 SNP/NII/MNP 设备，PXE 链加载场景行为不变 |

> RTL8168 相关研究已于 2026-08 终止，补丁不含 8168 过滤或修复代码（见 `../docs/8168-research-log.md`）。

## 升级上游基线流程

上游升级后补丁可能无法应用，按以下流程迁移：

1. 更新基线：

   ```bash
   git fetch https://github.com/ipxe/ipxe.git master
   git log FETCH_HEAD --oneline | head   # 确认新基线
   ```

2. 在构建缓存工作树（`.cache/ipxe-upstream`）中**在新基线上重新生成补丁**：

   ```bash
   # 在 .cache/ipxe-upstream 中：先检出新基线，再手动复现修改（或对照旧补丁），最后：
   git diff > patches/0001-realtek-8125-adaptation.patch
   ```

   注意：**先应用旧补丁到新基线 → 手动解决冲突 → 再重新生成**，比手工重写更可靠。

3. 更新 `build/build.sh` 中的 `UPSTREAM_COMMIT` 与本文档基线记录。

4. 运行 `./build/build.sh` 验证补丁可干净应用、产物功能正常（重点回归：8125 引导）。

## 注意事项

- `embed/auto.ipxe` 属于**配置资产**（非源码补丁），改动无需重新生成补丁，直接修改 `../embed/auto.ipxe` 后重新构建即可
- 补丁需保持 `git apply --check` 通过；补丁与基线提交强绑定，`UPSTREAM_COMMIT` 变更前务必走升级流程
