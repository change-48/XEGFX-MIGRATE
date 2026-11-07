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
拖动帧率：p50 24.8ms（≈40fps）/ p90 34ms
GPU 频率：拖动时 1625MHz（max 2150）、拷贝路径只有 800MHz
```

开机 → 登录 → 约 2 分钟自动出桌面（脚本见 `20-ops/A-44/xegfx-bringup.sh`）。

### 帧率的正式结论（G7，2026-11-03 收关）

四个关卡、五次装机、跨三个版本，**p50 全程落在 24.6–25.3ms（≈40fps），没有一版越过 24.6**。
逐帧打点（A-89）把一帧精确拆成五段后，结论是确定的：

| 段 | p50 | 与帧时间的相关 |
| --- | --- | --- |
| 提交 → worker 取到 | 0.03ms | +0.04 |
| worker → GT idle → arm | 5.81ms | −0.05 |
| arm → 面板 latch（等 vblank） | 4.89ms | +0.02 |
| **驱动内合计** | **10.84ms** | **−0.01** |
| **信号之后（WS 合成 + Present + `AccessComplete`）** | **14.10ms** | **+0.95** |

三点：
1. **帧时间与驱动段零相关**——压驱动（锁、提交、latch、提前信号）实测四次全部无效，
   因为时间会被后面那 14ms 吸掉（`corr(L,G) = −0.3`，链上有半拍松弛）。
2. **面板不是限速器**：1 拍帧真实存在（4–5%，dt 11.6ms = 86fps），说明 display link 收得下 86 帧/秒。
3. **上限来自"一次只有一个 command buffer 在飞"的串行链**：`AccessComplete → waitUntilScheduled`
   要等这一帧的提交被调度，然后才轮到下一帧合成。**要让 60fps 成立，需要 CPU 合成与面板扫描重叠**
   —— 那要第二个 command buffer/第二队列，属整树换血（ArrowWhatever 同负载 82 翻/秒），不是本栈能改出来的。


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
| **帧率上限 ≈40fps** | 串行链 + 单槽提交（A-89 实测驱动段与帧时间相关 −0.01，天花板已确认）。60fps 需整树换血 |
| **开机要重启一次 WindowServer（本栈无解）** | WindowServer 在开机 ~20s 完成显示枚举，此时 display pipe 还不存在（"Unable to find display pipe for IOFB service … 0x1000004f4"），CoreDisplay 记下"没有 pipe"后再也不问第二次；脚本在 22s 后 kickstart 一次（**用户需重新登录，代价约 40 秒**）。A-90 试过三种修法全部证伪：显示器关/开（唤醒事件到了但不设模式）、加速器重送模式变更（`accelfb=3`，110 秒零帧）、FB 重放 `kIOFBNotifyOnlineChange`+`DisplayModeDidChange`（通知确实发出，CoreDisplay 无反应）。详见 `20-ops/G7-A90报告.md` §2 |
| **每个 App 启动都要多花约 2 s（冷热都一样）** | 不是着色器缓存：普通 App 在自己进程里**一次着色器都不翻译**（A-90 实测：把 1919 文件共享缓存整体搬走，两个 App 启动后缓存增量为 0）。真因是 `vkEnumeratePhysicalDevices -> 0 devices` → "NVK did not come up … refusing to be the Metal device"，**每次**启动白付 2.08 s，然后整个 App 走 CPU 渲染。见 `20-ops/G7-A90报告.md` §3；修法是下一关的 B0 |
| 首次遇到新的着色器变体会卡几秒 | **只发生在 WindowServer 与 iconservicesagent 这类真的拿到 GPU 的进程**（各自 `…/C/<progname>/nvmtl/spvcache`：1088 文件/290MB 与 1353 文件/42MB），之后永久命中 |
| 帧是 **翻转** 而非拷贝（G7 已达成） | A-84 起 flip/defer 是开机默认；拷贝路径下 GPU 只有 800MHz（RPn 地板） |
| HiDPI 上限 1560×1040 | 更高需要 pipe scaler（上游 issue #19 仍开放；A-55/58/60 三轮证伪：原生面板无 scaler） |
| 显式 `sleep` 不可触发 | `sleep 0` + `.Display_boot` preventer；显示器关/开可用（A-79 自愈链已验证） |


---

## 上游提交记录

| 日期 | 位置 | 内容 |
| --- | --- | --- |
| 2026-11-01 | [WTLIntelGfx#14](https://github.com/watchthelight-labs/WTLIntelGfx/issues/14) | 3120 宽面板的 `12480 / 12544` stride 失配、defer 队列不接帧（初版报告，描述的是当时的 copy 路径状态） |
| 2026-11-01 | [research PR #2](https://github.com/watchthelight-labs/research/pull/2) → [commit `c5395e6`](https://github.com/watchthelight-labs/research/commit/c5395e6) | 9 份日志集；上游代做脱敏并直接提交 main，PR 因此关闭（不是被拒） |
| 2026-11-07 | [#14 更新评论](https://github.com/watchthelight-labs/WTLIntelGfx/issues/14#issuecomment-6704448668) | 更正：flip 已是开机默认、39.6fps / p90 34ms；pitch 失配的真实绕过方式；40fps 上限归因 |
| 2026-11-07 | [ArrowWhatever#2](https://github.com/Mountainatic/ArrowWhatever/issues/2) | 把 Finding 1（patch 0008/0009 的 `q.pitch == P` gate）转给该栈作者 |

两篇 11-07 稿件原文见 `20-ops/upstream-*.md`。上游 README 里 "frames copied, not flipped" 那行由维护者自行更新，本项目不为其措辞开 PR。

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
