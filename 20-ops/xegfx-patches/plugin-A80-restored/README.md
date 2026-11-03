# plugin-A80-restored — 装机插件的源码树（A-80 末态）

> 2026-11-03 重建。**这是 `NVMTLDriver.bundle` 里那份二进制对应的源码**，
> 从 A-85 起"插件源码树不见了、一行代码都改不了"的坑，到此为止。

## 一句话结论

`~/src/NvGfx-xe-2.6.2/plugin` **就是 A-80 的构建树**（没被 A-81 污染）。
它是 `A-80/backup/*.before`（A-79 态）+ `patch_a80.py` 的 15 处 + **1 行手工前向声明**，
逐字节可复现（下面有 md5 证据）；用 `build.sh` 能编出 1591904 B 的 bundle，
装上去的二进制（`e5bb42b0…`，1622624 B 签名后）UUID `0B790A57-6AE0-3484-B592-6F0D4112D031`
与 `A-80/build-plugin/NVMTLDriver` **完全相同**。

## 重建路径（路线 1：现树直接可用）

```bash
# 1) 现树 == A-80 末态？用"重打一遍"来证明：
cp -a ~/src/NvGfx-xe-2.6.2/plugin ~/xegfx-log/A-86/tree-from-a79
cp -f ~/xegfx-log/A-80/backup/{NVMTLDevice.m,NVMTLEventSync.m,NVMTLObjects.m,NVMTLObjects.h,nvmtl_vk.c,nvmtl_vk.h}.before \
     ~/xegfx-log/A-86/tree-from-a79/          # 6 个文件回到 A-79 态
python3 patch_a80.py ~/xegfx-log/A-86/tree-from-a79/     # 15 处全部命中 → ALL_OK
diff -rq tree-from-a79 ~/src/NvGfx-xe-2.6.2/plugin      # 只有 NVMTLDevice.m 差 1 行
```

差的那 1 行（A-80 当时手工补的，`patch_a80.py` 里没有）：

```c
int nvmtl_po_denied(IOSurfaceRef s);      // 在 nvmtl_po_prepare 的声明之后
```

除它之外 5 个文件 md5 **完全一致**：

```
2daac40e009eb60cd96733074270cef0  NVMTLEventSync.m
fb65115da73711114dadf85958f02ff7  NVMTLObjects.m
1b5d58daa585ebcd4eb0a4e83b8a32a1  NVMTLObjects.h
bc5ff0dc48aba23a5f054f7ddd41560f  nvmtl_vk.c
5cf436eaad101a0c6a674898bf404062  nvmtl_vk.h
```

## 构建

```bash
bash build.sh /tmp/NVMTLDriver
# built /tmp/NVMTLDriver  1591904 B  uuid 6B8DD91C-…   （与装机件同尺寸、同 5032 个符号）
```

- **体积与符号完全一致**：1591904 B，`nm -a` 5032 条、与 A-80 构建件 **零差异**；
- **不是逐字节相同**：与 `A-80/build-plugin/NVMTLDriver` 差 25117 B（1.6%），段偏移/段大小完全相同，
  差异是 `__cstring` 与引用位移的排布不同（同一棵树连编两次只差 16 B 的 LC_UUID，
  所以这不是编译随机性，是当时那次构建的环境差异，未能追出）。**判行为等价以实测为准**：
  A-86 装机实测 p50 24.59 ms、page-off 日志在、marks 增长，与 A-80 基线一致。

## 安装

```bash
bash a86-install.sh <NVMTLDriver>     # 原子替换（rename，不覆盖运行中的 inode）+ ad-hoc 签名 + CS_PLATFORM_BINARY
# 换插件后要重启整机（不要 kickstart WS，见 A-85 §7）
```

## 目录

```
src/                  A-80 末态源码（32 个 .m/.h/.c/.metal，不含 *.a73before 旧备份）
md5.txt               上面每个文件的 md5（与机器上 ~/src/NvGfx-xe-2.6.2/plugin 一一对应）
build.sh              A-80 实际用的编译命令
a86-install.sh        装机脚本（含特征串门禁）
patch_a80.py          A-79 → A-80 的 15 处锚点补丁（可重打，逐处断言）
patch_a86_diag.py     A-86 探针：fc 缓存每次查表的 census → /private/tmp/nvmtl-fc.log
patch_a86_knobs.py    A-86 运行时开关：/private/tmp/nvmtl-a86 里写 nogsub / cap=N
```

## 缺失/注意

- `*.a73before` 三个文件（A-73 之前的旧备份）没有归档，它们是历史垃圾，不在构建输入里；
- 这棵树**不是 git 仓库**，`build_plugin.sh` 的 clean-tree 检查无法用，直接用 `build.sh`；
- 插件日志：WS 的在 `$TMPDIR/WindowServer/nvmtl.log`（`/private/var/folders/zz/…/T/WindowServer/nvmtl.log`），
  但 `nvlog` 按**格式串指针**去重（前 8 次 + 每 4096 次），**不能用日志行数判断频率**——A-86 就是在这里差点误判。
