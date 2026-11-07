# 远程调试作业说明（给 AI 的作业手册）

> 面向：被要求在这台机器上**远程做内核/驱动类调试**的 AI 助手。
> 读法：第 3 节（铁律）和第 5 节（救援）**必须全部读完再动手**；其余按需查阅。
> 本文所有结论都来自 2026-10-31 至 11-01 在这台机器上的实际事故，出处可追。

---

## 0. 一句话

这台机器**已经有一套正在工作的图形栈**（核显 GPU 合成桌面）。
你做的每一次内核实验，都可能把使用者正在用的桌面弄黑、弄卡、弄到要长按电源。
**所以本文的重点不是"怎么做"，而是"什么时候不许做、做之前要准备什么、做完怎么证明"。**

---

## 1. 这台机器是什么

| 项 | 值 |
| --- | --- |
| 硬件 | HONOR MagicBook Art 14 2025，Intel Core Ultra 7 255H，32 GB |
| iGPU | Intel Arrow Lake Xe-LPG+ `8086:7D51` |
| 面板 | 3120 × 2080 @ 120 Hz（eDP），HiDPI 逻辑分辨率 1560 × 1040 |
| 系统 | macOS 26.5.2 (25F84)，OpenCore `DBG-108-2026-09-27` 引导 |
| SMBIOS | 伪装成 `MacBookPro16,2`（`Mac-5F9802EFE386AA28`）——**DMI/ACPI 全是伪装，别想从 macOS 读真实机型** |
| 无线 | Intel AX211（CNVi），`AirportItlwm` 驱动；蓝牙报 `BCM_4350C2`（伪装） |

### 1.1 当前工作状态（**这是必须保住的基线**）

```
WindowServer 在核显上合成：CA → Metal → NVMTL 插件 → ANV → XeGfxAccel 的 display pipe → 面板
Resolution 3120 x 2080 / UI Looks like 1560 x 1040 @ 120 Hz / Online: Yes
液态玻璃效果正常；负载下 30–56 fps；每帧由 CPU 拷贝 4–5 ms 到 scanout
开机自启：/usr/local/bin/xegfx-bringup.sh（LaunchDaemon org.xegfx.bringup）
```

**只要上面这几行还成立，使用者就能正常干活。任何让它不成立的操作，都必须事先准备回滚。**

### 1.2 已加载的 kext（顺序即依赖）

```
org.watchthelight.IGPUConsoleRange 0.4
org.watchthelight.XeGfxIntelFB     0.8      ← 持有面板（IONDRV 子类），head 4
org.watchthelight.XeGfxAGDC        0.1.1    ← IOP 找 display pipe 的入口
org.watchthelight.XeGfx            0.0.46   ← i915 over LinuxKPI
org.watchthelight.XeGfxDisplay     0.5      ← KMS 适配器（phase=%00 时惰性）
org.watchthelight.XeGfxAccel       0.1      ← 加速器 + display pipe 事务
org.xenative.XeNative              0.14     ← 诊断用，可直接读写平面寄存器
```

---

## 2. 连接与路径

```bash
# 从 Windows 侧（Git Bash）
ssh -i ~/.ssh/id_xegfx_remote change48@IP-1
```

| 位置 | 说明 |
| --- | --- |
| `~/src/NvGfx-5a19a73f36249efb99c44c3f9cb34603ef0c0eb8/` | **kext 源码树**（XeGfx / XeGfxAccel / XeGfxIntelFB 等）。装机二进制出自这里 |
| `~/src/NvGfx-xe-2.6.2/` | **plugin 与 translator** 的来源树（`plugin/`、`translator/`）。注意：它也有 `kexts/`，但**不是**装机 kext 的来源 |
| `~/src/deps/open-gpu-615.78.08` | 编 accel 需要的 `OGKM` 目录 |
| `~/xegfx-log/` | 每次实验的证据目录（`A-xx/`） |
| `/Library/Extensions/` | 装机的 kext |
| `/Library/GPUBundles/nvmtl/` | Metal 插件、translator、`nvrm610.conf`、`spvcache` |
| `/usr/local/bin/xegfx-bringup.sh` | 开机自启脚本（含让位逻辑，勿随意改） |
| `~/xenative/` | XeNative 源码与 `xenative-ctl` |

> **GitHub 通过 `git` 走 443 在这台机器上是不通的**（`Failed to connect ... port 443`），
> 但 `curl` 到 `raw.githubusercontent.com` / `api.github.com` 可用。要拉代码用 curl，别指望 `git clone`。

---

## 3. 铁律（每条都对应一次真实事故）

### 3.1 改之前先确认"你改的树就是装机的树"

同一台机器上有两棵看起来都能编 kext 的树。曾经照错的那棵编出 `NMXAccel` 命名的二进制（没有
`debug.xegfxaccel_*` 这套 sysctl），**装上去自启脚本全部 `unknown oid` → 帧流断 → 黑屏**。

**装机前必做**：

```bash
NEW=<刚编出来的 kext 里的 Mach-O>
strings -a $NEW | grep -c "debug.xegfxaccelfb"     # 期望 ≥1，否则是错树
size -m "$NEW" | grep -E "__text|__cstring"        # 与 /Library/Extensions 里那份对比，应相近
```

### 3.2 备份三件套，缺一不可

```bash
L=~/xegfx-log/<关卡号>; mkdir -p $L
cp -p <源文件> $L/<源文件>.before
cp -p /Library/Extensions/<X>.kext/Contents/MacOS/<X> $L/<X>.before
cp -p /Library/KernelCollections/AuxiliaryKernelExtensions.kc $L/aux-before-<关卡号>.kc
shasum -a 256 <源文件> /Library/Extensions/<X>.kext/Contents/MacOS/<X> | tee $L/sha.before
```

### 3.3 装 kext 的固定顺序（一步都不能跳）

```bash
sudo cp -f <new-binary> /Library/Extensions/<X>.kext/Contents/MacOS/<X>
sudo chown root:wheel /Library/Extensions/<X>.kext/Contents/MacOS/<X>
sudo codesign -f -s - /Library/Extensions/<X>.kext
codesign -dv /Library/Extensions/<X>.kext 2>&1 | grep Signature        # 必须 adhoc

sudo /usr/bin/kmutil create -n aux --volume-root / --allow-missing-kdk \
     -B /System/Library/KernelCollections/BootKernelExtensions.kc \
     -S /System/Library/KernelCollections/SystemKernelExtensions.kc \
     --repository /Library/Extensions -A /tmp/aux-new.kc -z
sudo cp -f /tmp/aux-new.kc /Library/KernelCollections/AuxiliaryKernelExtensions.kc

# 六个 id 必须都在（IGPUConsoleRange 少一个就开不了机）
sudo /usr/bin/kmutil inspect -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc -M --no-header \
  | awk 'NF{print $1}' | sort | grep -c watchthelight        # 期望 ≥6
```

**`IGPUConsoleRange` 缺了会怎样**：IONDRV 拿不到 stolen memory 里的 console → 面板点不亮。
这一步没核对就重启，代价是一次"黑屏 + 长按电源"。

### 3.4 一次只改一个变量

今天的记录里，凡是"顺手一起改了两处"的回合，最后都无法归因，只能整轮回滚重来。
**改一处 → 编 → 装 → 重启 → 量 → 记录**，然后再下一步。

### 3.5 不要在 boot 早期切呈现

开机阶段壳层（WindowServer）还没准备好。曾经在 boot 早期推 `stage 7` / 切呈现，
结果 WS 在 `push_surface` 断言里崩、进登录回环。

**正确做法**：等真人登录 + 桌面沉降 25 秒，再动手。`xegfx-bringup.sh` 已经实现了这个等待，**不要绕过它**。

### 3.6 改显示寄存器前，必须先有"一键还原"

存活的面板上改 `PLANE_SURF` / `PLANE_STRIDE` / `PLANE_SIZE`，**出错就是花屏/黑屏**（今天发生过）。

- `xenative-ctl restore` 会把五个寄存器放回**它启动时**的值，**但** `SURF` 会回到 `0xfe600000`
  （A-10 之前的老缓冲），而当前在用的是 `0xfd000000`（`ioreg`/`xenative-ctl status` 里能看到）。
  **所以 restore 之后必须补一次**：
  ```bash
  sudo /usr/local/bin/xenative-ctl regset 3 1 0x081f0c2f 2 195 0 0xfd000000
  ```
- 寄存器**单位**：`PLANE_STRIDE = 行字节 / 64`（12480 → 195）。
- 改完必须**读回**确认写进去了，并且**让使用者看一眼屏幕**（见 §6）。

### 3.7 冷启动后 `iop_ok = 0` 是正常的，别急着判定失败

WindowServer 比 display pipe 先起来时会握着过期的 display 注册，`iop_ok` 一直为 0。
`launchctl kickstart -k system/com.apple.WindowServer` 一次即可（自启脚本已内置，但 bootArm 接管时曾经漏掉）。

### 3.8 连续崩两次后"开机变 CPU 渲染"不是 kext 坏了

`XeGfxIntelFB` 自带护栏：NVRAM `xeintelfb-starts`，**2 次"启动 120 s 内没等到 WindowServer"就永久拒绝 probe**。

```
XeGfxIntelFB: boot guard tripped (two starts without WindowServer): declining
NMXAccel: boot-armed: FAILED -- no XeGfx framebuffer with an open start gate after 30000 ms
```

**解法**（先试这个，别换 kext）：

```bash
sudo nvram xeintelfb-starts=%00
sudo nvram nmintelfb-starts=%00
sudo shutdown -r now
```

### 3.9 会冻住整机的那条路，没有看门狗就不要碰

`XeGfxAccel` 的延迟 flip（`gIopXeFlip`/`gIopXeDefer`）在 `q.pitch == P` 不成立时会被拒；
放宽那道判定后，帧会被 `return kIOReturnSuccess` **认领**，但既不 flip 也不拷贝 →
**面板冻在最后一帧、WindowServer 卡住、整机失联**（今天发生两次，都要长按电源）。

**在装上 dead-man 看门狗之前，不要再试这条路。** 看门狗长这样（示例，未安装）：

```bash
# /usr/local/bin/xegfx-deadman.sh —— iop_ok 30 s 不前进就重启
a=$(sysctl -n debug.xegfxaccel_iop_ok); sleep 30; b=$(sysctl -n debug.xegfxaccel_iop_ok)
[ "$a" = "$b" ] && { logger -t xegfx "no frames in 30 s, rebooting"; shutdown -r now; }
```

### 3.10 不要替使用者做公开动作

发 issue / PR / 上传日志属于对外动作：**先问，得到明确同意再做**。
上传机器日志前**自己先脱敏**（序列号、硬件 UUID、UDID、MAC、SSID、token）——
别人的脱敏流程只覆盖他们仓库里的副本，**而你发的正文是公开且永久的**。

---

## 4. 四类任务的作业流程

### 4.1 只观察（默认、最安全）

```bash
sudo sysctl -a | grep -E 'debug\.xegfx'         # 计数（zsh 下不要用通配）
sudo ioreg -l -w0 -r -c XeGfxIntelFramebuffer   # 节点
sudo powermetrics --samplers tasks -n 1         # CPU ms/s（不是百分比）
sudo log show --last 5m --predicate 'eventMessage CONTAINS "NMXAccel"'
sudo sample $(pgrep -x WindowServer) 2 -file /tmp/ws.txt
```

不需要重启，不影响使用者。**优先把问题看清楚，再决定要不要动手。**

### 4.2 改 kext（可重）

按 §3.1 → §3.2 → §3.3 走，重启后按 §5 的判据验。回滚就是装回 `.before` + `aux-before-*.kc` 再重启。

### 4.3 改 EFI / boot-args（每次都要重启）

`config.plist` 在 `/Volumes/NO NAME/EFI/OC/config.plist`，`NVRAM/Add/<GUID>/boot-args` 是**字典**（不是数组）。

```bash
sudo diskutil mount disk0s1
sudo cp -p "$P" "$P.bak-$(date +%Y%m%d-%H%M%S)"
# 用 plistlib 改，改完 plutil -lint，并逐词比对"只多了预期的那个"
sudo diskutil unmount disk0s1
```

### 4.4 改 plugin / translator（不需要重启，只重启 WS）

`/Library/GPUBundles/nvmtl/` 下的 dylib 与 bundle 是用户态：

```bash
sudo codesign -f -s - <target>
sudo /usr/local/bin/xegfx-metal-fix.sh          # 平台位（CS_PLATFORM_BINARY），否则 WS 映射不进来
codesign -dv <target> 2>&1 | grep flags         # 期望含 0x4000002
sudo launchctl kickstart -k system/com.apple.WindowServer
```

---

## 5. 出事了怎么办（按严重度分级）

| 现象 | 第一步 | 第二步 |
| --- | --- | --- |
| 画面花屏/斜切/变色 | `xenative-ctl restore` + 补写 `0xfd000000`（§3.6） | 仍不对 → 重启 |
| 画面冻结、鼠标不动 | 直接 `sudo shutdown -r now`（SSH 还能进就还有救） | 进不去 → 长按电源 |
| 黑屏但 SSH 通 | 看 `log show --last 3m` 里 `boot-armed:` / `boot guard` | 重启 |
| 开机变 CPU 渲染、无 HiDPI | `nvram xeintelfb-starts=%00` + 重启（§3.8） | 仍不行 → 装回 KC 备份 |
| WS 反复重启 | `launchctl kickstart` 一次看是否自愈；查 `.ips` 的 `termination` | 回滚最近的 kext |
| 新 panic | 不要慌，先取 `.ips`，**看 backtrace 落在哪个 kext 哪个函数** | 按该函数决定回滚哪个 |

**救援时的取证优先于修复**：

```bash
L=~/xegfx-log/<关卡号>
sudo ioreg -l -w0 > $L/ioreg-final.txt
sudo log show --last 5m > $L/log-final.txt
ls -t /Library/Logs/DiagnosticReports/*.panic | head -3
```

---

## 6. 判据：哪些信号可信、哪些不可信

| 信号 | 可信度 | 说明 |
| --- | --- | --- |
| **使用者肉眼看到的画面** | 最高 | 很多问题 SSH 侧完全看不出来（花屏、撕裂、冻结） |
| `iop_ok` / `iop_fail` 等 sysctl 计数 | 高 | 但是"累计值"，要**取两次差分**，单次读数没有意义 |
| `powermetrics` 的 `CPU ms/s` | 高 | **是毫秒/秒，不是百分比**；1 个核 = 1000 ms/s |
| `log show` 的 kext 日志 | 高 | 注意 `<private>` 会吞掉参数 |
| `sample` 的调用栈 | 高 | **样本落在 `mach_msg` 说明它在空转等事件**，不是忙 |
| `screencapture` | **不可用** | SSH 下常报 `could not create image from display`，与显示状态无关 |
| `system_profiler SPDisplaysDataType` | 中 | 登录窗口期 `Displays:` 段可能整段消失，不代表显示坏了 |
| `xenative-ctl status` 的寄存器 | **低** | 打的是 kext 里的**缓存快照**，`FRMCOUNT` 三次读数一模一样就是证据 |
| 工具打印的 "filled/ok/success" | **低** | 今天的 `fill` 打印 `filled 1`，那个 1 其实是 `wb` 参数，图案根本没写进去。**必须独立读回验证** |

**三条硬规矩**：
1. 任何"成功"都要有**独立的第二证据**（读回、计数差分、肉眼）。
2. 任何"失败"都要先排除**采集方式**的问题（权限、缓存、时间窗）。
3. 改动的变量和观察的指标，**必须在同一时间窗内**。

---

## 7. 留档规范

每次实验一个目录 `~/xegfx-log/<关卡号>/`，至少包含：

```
<源文件>.before              改前源文件
<X>.before                   改前二进制
aux-before-<关卡号>.kc       改前内核集合
sha.before                   改前指纹
<测量>.txt                   原始输出（不要只留结论）
```

报告写清五件事：**改了什么（diff）、怎么测的（命令）、测出什么（原始数字）、
哪条判据通过/不通过、现在是什么状态（能不能正常用）**。
失败也要写——今天的记录里，最有价值的部分恰恰是一串被证伪的假设。

---

## 8. 什么时候必须停下来问人

- 需要**重启**（会打断使用者）
- 需要**改 EFI / boot-args**（改错可能开不了机）
- 需要**动显示寄存器**（可能花屏）
- 需要**碰延迟 flip 那条路**（可能整机冻结）
- 要**公开发布**任何东西（issue / PR / 日志）
- 同一关卡**连续两次失败**——先停下来重新看证据，不要"再试一个参数"

---

## 9. 已知陷阱速查

| 陷阱 | 现象 | 应对 |
| --- | --- | --- |
| 编错树 | 装完黑屏，sysctl `unknown oid` | §3.1 的 `strings` 检查 |
| 忘记平台位 | WS 映射不到插件，退回软件合成 | `xegfx-metal-fix.sh` + 查 `flags=0x4000002` |
| 登录窗口期误判 | `/dev/console` 属主是 `_windowserver` 不是 root | 用 `uid ≥ 500` + 排除名单 |
| `grep org.watchthelight.XeGfx` | 命中 IntelFB/AGDC/Display，误判"core 已加载" | 精确匹配 `org\.watchthelight\.XeGfx ` |
| 重复写 `accelfb=3` | `iop_ok` 停摆 1–2 分钟 | 先读 `iop`，已为 1 就跳过 |
| `xenative-ctl restore` | SURF 回到老缓冲 `0xfe600000` | 补写 `0xfd000000` |
| `system_profiler` 无 `Displays:` | 以为显示坏了 | 看 `ioreg -c IODisplayConnect` 数量 + 肉眼 |
| boot guard | 连崩两次后变 CPU 渲染 | §3.8 |
| 3120 宽面板 | 零拷贝 flip 永远不触发（`12544 ≠ 12480`） | 这是**已知未解**问题，见 issue #14，不要以为是自己改错了 |

---

## 10. 一句话总结给下一个 AI

> 这台机器上，**"看起来成功了"和"真的成功了"经常不是一回事**。
> 先量、再改、改完用第二种方式再量一次，然后请人看一眼屏幕。
> 想不清楚的时候，停下来问——修坏它的代价，比多问一句大得多。
