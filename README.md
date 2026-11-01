# XEGFX-MIGRATE

在 macOS 26 上把 **Intel Xe-LPG（Arrow Lake，`8086:7D51`）核显**从"只能点亮"推进到
**桌面由核显 GPU 合成**的全过程记录：每个关卡的目标、实测读数、被证伪的假设、以及最终可用的开机自启链路。

> 这不是一份"教程"。它记录的是**一次真实的迁移过程**——包括走错的路。
> 每个 `G*` / `A*` 文件都是当时写给执行方（另一台 Mac 上的会话）的提示词或执行报告，
> 中间穿插 `20-ops/STATE-*.md` 作为当日的实测基线总账。

---

## 硬件与系统

| 项 | 值 |
| --- | --- |
| 机器 | MacBookPro16,2（黑苹果，OpenCore 引导） |
| CPU / iGPU | Intel Core Ultra 7 255H / Intel Xe-LPG+ `8086:7D51` |
| 面板 | 3120 × 2080（254 DPI，eDP） |
| 系统 | macOS 26.5.2（Tahoe） |
| 上游 | [XeGfx](https://github.com/watchthelight-labs/XeGfx) / NvGfx `xe` 分支（i915 over LinuxKPI、Mesa ANV、NullMoth Metal 插件） |

---

## 最终结果

| 关卡 | 内容 | 状态 |
| --- | --- | --- |
| Gate 0 | 快照 / 回滚脚本 / 开机护栏 / panic 自动重启 | ✅ |
| Gate 1 | 内核 core `stage 1–7` 全绿（含 `pci_dev_put` / stage-loop 补丁） | ✅ |
| Gate 2 | Vulkan 全链路（`vkQueueSubmit` / `vkWaitForFences` 全 0） | ✅ |
| Gate 3 | 系统原生 **3120×2080**，不再依赖缩放 | ✅ |
| Gate 4 | **桌面跑在核显上**：CA→Metal→NVMTL→ANV，帧经 accel 的 display pipe 拷到 scanout | ✅ |

当前可用状态：

```
Resolution: 3120 x 2080
UI Looks like: 1560 x 1040 @ 120.00Hz     ← HiDPI（2×）
Online: Yes / Main Display: Yes
液态玻璃（Liquid Glass）效果正常
```

开机 → 登录 → 约 2 分钟自动出桌面（脚本见 `20-ops/A-44/xegfx-bringup.sh`）。

---

## 关键结论（按发现顺序）

1. **`-28` 不是无害的。** `reserve_bootfb` 返回 `-28`（ENOSPC）意味着 i915 的 GGTT 分配器
   **不知道那块 26 MB 帧缓冲被占用**，加速器一分配就把它覆盖掉。
2. **平面几何寄存器是双缓冲的**：只写 `STRIDE/SIZE/PIPESRC` 不生效，必须以一次 `PLANE_SURF` 更新锁存。
3. **`AGDC_FB_CLASS` 必须与 framebuffer 的真实类名一致。** 上游是 `"IONDRVFramebuffer"`；
   把它换成别的类名会让 AGDC 静默失配（`pci 0x0 fb 0x0`），进而 IOP 找不到 display pipe。
4. **`frame` 到位的关键是 `gNvIop`**：`performTransaction()` 第一行 `if (!gNvIop) return 0xe00002c7;`
   （静默返回，不打日志）。本 build 里它的开关是 `debug.xegfxaccel_iop=1`。
5. **`debug.xegfxaccelfb` 必须写 3**（`how >= 3` 才置 `gNvScanout`）；读回值恒为 1 是读另一个变量，不代表没生效。
6. **冷启动必须重启一次 WindowServer**：WS 早于 pipe 存在时会握着过期的 display 注册，
   `iop_ok` 会一直为 0；`kickstart` 一次后帧流立刻起来。
7. **着色器翻译是卡顿的根**：CA 的 Uber 着色器一次翻译 700 KB SPIR-V、耗时 7.8–9.6 秒；
   缓存命中后正常，所以"越用越快"。

---

## 目录

```
20-ops/                     全部工作记录
  STATE-2026-10-31.md       当日实测基线总账（最重要的一份，建议先读）
  G0..G4-执行提示词.md       各关卡发给执行方的提示词
  G*-报告.md                 执行方回报（含原始日志摘录）
  20-ops/A-*/               每次实验的原始证据（ioreg 全量、日志、崩溃报告）
  A-44/xegfx-bringup.sh     最终的开机自启脚本
10-diag/                    只读寄存器窗口 dumper（XeDiag）
11-native/                  XeNative：用户态寄存器/页表工具（最终方案未依赖它）
```

---

## 怎么复现（摘要）

```bash
# 1) 内核 core 与加速器（需先按上游 XeGfx 文档构建）
kmutil load -b org.watchthelight.XeGfx
sysctl -w debug.xegfx_wdog=60
sysctl -w debug.xegfx_stage=7            # 期望 results = 0 1 1 1 1 1 1 1

# 2) 加速器 + scanout pipe + 事务钩子
kmutil load -b org.watchthelight.XeGfxAccel
sysctl -w debug.xegfxaccelfb=3           # scanout-backed display pipe
sysctl -w debug.xegfxaccel_iop=1         # 允许 display pipe 事务（帧才会真的搬上屏）

# 3) 冷启动后重启一次 WindowServer
launchctl kickstart -k system/com.apple.WindowServer

# 4) 验证：这个计数在增长就说明帧在往面板上搬
sysctl debug.xegfxaccel_iop_ok
```

自动化版本见 `20-ops/A-44/xegfx-bringup.sh`（含"等真人登录""任一步失败即停"的守卫）。

---

## 已知限制

| 限制 | 说明 |
| --- | --- |
| 需要真人登录 | 脚本靠 `/dev/console` 判定会话，未开 auto-login |
| 首次遇到新的着色器变体会卡约 8 秒 | 之后永久命中 `spvcache` |
| 帧是 **拷贝** 而非 **翻转** | 上游自己也是（"frames are still copied rather than flipped"），零拷贝属下一阶段 |
| HiDPI 上限 1560×1040 | 更高需要 pipe scaler（上游 issue #19 仍开放） |

---

## 许可与致谢

本仓库只包含**本项目的记录、脚本与自研诊断工具**，不包含上游驱动源码，也不包含任何 Apple 或第三方的驱动分发物。

所有实际驱动能力来自：

- [watchthelight-labs/XeGfx](https://github.com/watchthelight-labs/XeGfx) 与 [NvGfx](https://github.com/watchthelight-labs/NvGfx) `xe` 分支
- [NullMoth 的 NVIDIA Metal 驱动](https://github.com/nullmoth/nvidia-macos-driver)（PolyForm Noncommercial）
- [freebsd/drm-kmod](https://github.com/freebsd/drm-kmod)、[Mesa ANV](https://gitlab.freedesktop.org/mesa/mesa)、
  [metal2vulkan](https://github.com/steelbrain/metal2vulkan)
- 以及 XeGfx 文档里致谢的各个 Hackintosh 项目（WhateverXe、VedCloud、nmx-intel 等）

本仓库的文字与脚本采用 CC BY-NC 4.0；引用的上游组件遵循其各自许可。
