# xegfx-patches — 让"装机跑的是哪份源码"这个问题永远有答案

2026-11-02 建立。起因：A-55 时发现**提示词指的源码文件根本不是我编译的那份**
（`NvGfx-5a19a73…` 里的 0.5 版 vs 装机的 `NvGfx-xe-2.6.2` 0.8 版），
而且装机的加速器二进制**无法由任何现存源码复现**。这两件事让当天的实验无法归因。

---

## 两条铁律

1. **动任何 kext 之前，先跑 `scripts/verify.sh`。**
   它重建每个 kext 并与装机二进制比对：`IN SYNC` 才说明"树 == 机器"。
   出现 `OUT OF SYNC` 就说明你看到的源码不是机器在跑的代码——**先修这个，再谈改动**。
2. **每次 build + install 之后，往 `ledger.tsv` 追加一行。**
   日期 / kext / 动作 / 源码 sha / 二进制 sha / 一句话说明。没有这一行，历史就断了。

---

## 树映射（★ 关键：**不是一棵树**）

| 组件 | 源码树 | 构建脚本 | 产物 |
| --- | --- | --- | --- |
| `XeGfxAccel` | `~/src/NvGfx-5a19a73f…` | `build/nvgfx/build_nvaccel.sh xegfxaccel` | `~/src/NvGfx-build/kexts/XeGfxAccel/26/…` |
| `XeGfx`（core） | `~/src/NvGfx-5a19a73f…` | `build/nvgfx/build_xegfx.sh` | `~/src/NvGfx-build/kexts/XeGfx/…` |
| `XeGfxIntelFB` | **`~/src/NvGfx-xe-2.6.2`** | `build/nvgfx/build_xegfxintelfb.sh` | `~/src/NvGfx-build/kexts/XeGfxIntelFB/…` |
| `NVMTLDriver`（插件） | **`~/src/NvGfx-xe-2.6.2`** | `build/nvgfx/build_plugin.sh` | `~/src/NvGfx-build/plugin/NVMTLDriver` |
| `NVK`（ICD） | `~/src/NvGfx-xe-2.6.2` | `build/nvgfx/build_nvk.sh` | `/Library/GPUBundles/nvmtl/…` |

**加速器来自 `5a19a73f`，面板 kext 和插件来自 `xe-2.6.2`。** 改之前先看清是哪一棵。
（`5a19a73f/kexts/XeGfxIntelFB/intelfb.cpp` 是一份 10-30 的旧副本 0.5，
`xe-2.6.2` 里那份才是装机的 0.8 —— A-55 就是在这里踩空的。）

---

## 已知缺口（2026-11-02 记录）

```
装机 XeGfxAccel  ac73184c0f74…  339648 B
现存任何源码重建 902cc946…        ——  不一致
```

原因不可追（可能是当时的 build 参数或另一份未保存的源码）。
**闭合方式**：下一次安装加速器时，从树里装它编出来的那份，ledger 记下两侧 sha，
此后 `verify.sh` 就会稳定报告 `IN SYNC`。

`XeGfxIntelFB` 没有这个缺口：`xe-2.6.2` 的 `intelfb.cpp` 重建后与装机 **逐字节相同**（`ce9baccb`）。

---

## 补丁系列

对加速器（`kexts/NVRM/accel/nvrm-accel.cpp`），基线 = `state/nvrm-accel.cpp.base`（`4f445170`）。

| # | 文件 | 内容 | 状态 |
| --- | --- | --- | --- |
| 0001 | `accel-scanout-12544.patch` | 在 GGTT 槽里分配一块 12544 行距的 scanout（只分配+映射，不动平面） | 实验过，**未装** |
| 0002 | `accel-flip-max-pages.patch` | `NV_XE_FLIP_MAX_PAGES` 4224→8192（16.5MB→32MB） | 同上 |
| 0003 | `accel-take-plane.patch` | 首帧拷入后接管平面（先 stride 后 surf）、`describeBootFramebuffer` 按新缓冲回答 | 同上 |
| 0004 | `accel-flip-map-cache.patch` | flip 槽映射缓存（97% 命中，内核开销降到 1/3） | 同上 |

四个补丁**按序应用后的结果 = `state/nvrm-accel.cpp.a53a54`，已验证逐字节一致**。

> 为什么"未装"：A-53/A-54 实测拖动只有 14 fps（拷贝路径 30–56）。
> 根因是 K9 的 worker 必须等 GPU 完全空闲才 arm，大面板下 GPU 一直忙 → worker 饿死。
> 补丁保留下来是因为 0002/0004 本身是**通用改进**（上游缺陷），值得单独回馈。

应用：`scripts/apply.sh`；回退：`git checkout` 不可用（非 git 树），用 `state/nvrm-accel.cpp.base` 覆盖回去即可。

---

## 目录

```
MANIFEST.md              本文
ledger.tsv               构建/安装账本（只追加）
patches/0001..0004       加速器补丁系列
scripts/verify.sh        重建并比对装机 → IN SYNC / OUT OF SYNC
scripts/apply.sh         按序应用补丁系列
state/nvrm-accel.cpp.base     加速器基线源码 4f445170
state/nvrm-accel.cpp.a53a54   四补丁应用后的成品（= A-53/A-54 全部工作）
state/build/                  拆分补丁用的中间态 s0..s4
```

---

## A-55 的三条教训（写死在规程里）

1. **先确认树**：`verify.sh` 报告 `IN SYNC` 之前，不要相信任何源码的位置。
2. **先确认模式**：判断"插件/驱动是否生效"前，先看 `who`（登录窗口下 WS 用软件合成，
   CPU 2000 ms/s 是正常的，不代表坏了）。
3. **一次只改一个变量**：装机二进制与源码不一致时，任何对照都不成立——
   A-55 因此没有装那个写好的 accel 补丁，这个判断是对的。

---

## 插件源码树（2026-11-03 补齐）

| 组件 | 源码树 | 构建 | 产物 |
| --- | --- | --- | --- |
| `NVMTLDriver`（插件） | **`plugin-A80-restored/src`**（= 机器上 `~/src/NvGfx-xe-2.6.2/plugin`） | `plugin-A80-restored/build.sh` | 1591904 B，UUID `0B790A57…` |

A-80 之后这棵树的构建入口（`~/nvmtl-build/LIVE/plugin`）消失，A-85 因此一行插件代码都改不了。
A-86 核实：`~/src/NvGfx-xe-2.6.2/plugin` **就是 A-80 的末态**（= A-79 backup + `patch_a80.py` 15 处 + 1 行手工声明，
5 个文件 md5 逐字节一致），并已归档到 `plugin-A80-restored/`（含 md5 清单与证据）。
详见 `plugin-A80-restored/README.md`。
