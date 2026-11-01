> **⚠️ 本文已作废（v1）。** §4.2 用 `PlistBuddy ... data <base64>` 写 `scale-resolutions` 是错的 ——
> 已实测落盘为 24 字节 ASCII（`0x4141414d4d414141...`）而非 16 字节二进制。
> §6 的「尝试 2」语义也标错了（`1560x1040` 是背衬，不是逻辑点）。
> **请用 `G3-A15-HiDPI提示词-v3.md`。**

> ⚠️ **本文件已被 `G3-A15-HiDPI提示词-v2.md` 取代，请勿执行 v1。**
> v1 的 `PlistBuddy ... data <base64>` 写法会把 base64 文本二次编码（落盘变 `QUFBQU1NQUFB...`），必然失败；
> 另缺 `DisplayResolutionEnabled`，且"尝试 2 加 1560×1040"方向错误。详见 v2 开头的差异说明。

# Gate 3 · A-15 执行提示词（HiDPI：原生像素不变，UI 放大到 2×）

> 用法：把本文件全文作为一条消息发给执行会话。执行会话不需要任何前置上下文。
> **一句话：写一个显示器 override，让 macOS 提供 "看起来 1560×1040" 的 HiDPI 模式，背衬仍是原生 3120×2080。**

---

## 0. 现状与目标

A-14 已达成原生 3120×2080：`IOFBMemorySize = 25958400`、`PLANE_STRIDE 195`、`PIPESRC/SIZE` 都是 3120×2080、无缩放器参与。

**副作用（用户实机反馈）：画面铺满了，但 UI 元素偏小。** 这块面板 31.2×20.8 cm / 3120×2080 ≈ **254 DPI**，
按 1:1 显示时字就是"物理上很小"。正确解法不是降分辨率（那会丢掉原生像素），而是 **HiDPI**：
系统按 2× 渲染（1560×1040 逻辑点 → 3120×2080 背衬），扫描输出仍是原生 3120×2080。

---

## 1. 已核实的现场事实（不要重新探索）

| 项 | 值 |
| --- | --- |
| `DisplayVendorID` | `1970170734` = **`0x756e6b6e`**（ASCII "unkn"，即未被识别） |
| `DisplayProductID` | `1815` = **`0x717`** |
| 系统自带 override | `/System/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-756e6b6e/DisplayProductID-717`（**只读，但可读**） |
| 根卷 | `/dev/disk1s4s1 on / (apfs, **sealed**, read-only)` → `/System` 写不进去（实测 `Read-only file system`），**不要尝试破坏封存、不要关 SIP** |
| `/Library/Displays` | **不存在**（要自己建） |
| 当前模式 | `IOFBConfig` 里只有 **1 个模式**（`IOFB0Hz=Yes`），所以系统设置里没有比例选项 |
| 显示状态 | `Resolution: 3120 x 2080` / `UI Looks like: 3120 x 2080` / ARGB8888 / Main / Online |

---

## 2. 先例（已查，按这个做，别自己发明格式）

Big Sur 之后 `/System` 被封存，社区通行做法是写 **用户级 override**，系统会优先读它：
```
/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-<vid-hex>/DisplayProductID-<pid-hex>
```
（Apple 开发者论坛、one-key-hidpi 等项目均如此；Intel 机器无 ARM 的限制。）

**`scale-resolutions` 的二进制格式**：每个 `<data>` 是 **16 字节 = 4 个大端 UInt32**
```
width, height, 0x00000001, 0x00200000
```
主 agent 已用公开样例反推校验过，**编码器结果与社区样例一字不差**：
```
1920x1080 -> AAAHgAAABDgAAAABACAAAA==   （= 00000780 00000438 00000001 00200000）
3120x2080 -> AAAMMAAACCAAAAABACAAAA==
1560x1040 -> AAAGGAAABBAAAAABACAAAA==
```
目录名、文件名用**十六进制**，plist 内部的整数字段用**十进制** —— 这个混搭是 Apple 的规矩，照抄。

---

## 3. 铁律

- **不碰 `/System`**（写不进去，且会引战）。**不关 SIP、不破坏系统卷封存**。
- **不动显示相关的任何寄存器 / kext / OpenCore config**。本关只加一个 plist。
- 每个改动前先备份目标位置原有内容（若原本不存在，记下"原本为空"，回滚就是删掉那个目录）。
- 重启后若 HiDPI 没出现，**不要连着改别的**（比如去动 `Resolution` 或 `UIScale`）——一次一个变量。
- 收尾必须保证：屏幕仍是原生 3120×2080、`fbgeom` 自愈脚本仍工作、零新 panic。

---

## 4. 步骤

### 4.1 读系统自带的 override，以它为基底（别从零写）

```bash
SYS=/System/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-756e6b6e/DisplayProductID-717
plutil -p "$SYS" 2>&1 | head -30      # 先看清楚它本来有什么键（可能是二进制 plist）
```
若它是二进制 plist，用 `plutil -convert xml1 -o -` 看内容。

### 4.2 建用户级 override

```bash
D=/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-756e6b6e
sudo mkdir -p "$D"
# 以系统文件为底（保留它原有的键），转成 xml 再加工
sudo plutil -convert xml1 -o "$D/DisplayProductID-717" "$SYS"   # 若 SYS 不存在则跳过，用下面的模板
```
然后确保文件里包含 `scale-resolutions`，**先只加一条 3120×2080**：

```xml
<key>scale-resolutions</key>
<array>
    <data>AAAMMAAACCAAAAABACAAAA==</data>
</array>
```

用 PlistBuddy 加（不要手写整个 XML，避免破坏原有键）：
```bash
P="$D/DisplayProductID-717"
sudo /usr/libexec/PlistBuddy -c "Delete :scale-resolutions" "$P" 2>/dev/null
sudo /usr/libexec/PlistBuddy -c "Add :scale-resolutions array" "$P"
sudo /usr/libexec/PlistBuddy -c "Add :scale-resolutions:0 data AAAMMAAACCAAAAABACAAAA==" "$P"
sudo plutil -lint "$P"
```

同时把名字设成面板名，便于在设置里辨认：
```bash
sudo /usr/libexec/PlistBuddy -c "Set :DisplayProductName EDO14.55" "$P" 2>/dev/null || \
sudo /usr/libexec/PlistBuddy -c "Add :DisplayProductName string EDO14.55" "$P"
sudo chown root:wheel "$P" && sudo chmod 644 "$P"
```

### 4.3 重启并验证

HiDPI 模式表在 WindowServer 启动时读取，**必须重启**（`killall WindowServer` 在部分版本不足）。

```bash
touch /var/db/xegfx/armed && sudo reboot
```

回来之后：
```bash
system_profiler SPDisplaysDataType | grep -E "Resolution|UI Looks like|IOFB"
ioreg -lw0 -r -c IONDRVFramebuffer | grep -o "IOFB0Hz\"=[A-Za-z]*"
sudo /usr/local/bin/xenative-ctl status | grep -E "STRIDE|SIZE|PIPESRC|PS_CTRL"
cat /var/db/xegfx/fbgeom.log | tail -6
```

---

## 5. 判读

| 现象 | 含义 | 下一步 |
| --- | --- | --- |
| `UI Looks like: 1560 x 1040`，`Resolution` 仍 `3120 x 2080` | ✅ **完全成功**：原生背衬 + 2× UI | 收尾，报告成功（附桌面照片） |
| 设置 → 显示器里出现 "看起来像 1560×1040" 的选项，但当前未选中 | 成功了一半：模式表生效了，只是没自动选 | **手动在系统设置里选中它**，再读 `UI Looks like` |
| `UI Looks like` 仍是 3120×2080，且设置里没有任何新选项 | `/Library` 这个 override 在本版本没被采纳，或只加一条不够 | 见 §6 |
| 屏幕错位/异常 | 不该发生（本关没碰几何） | `restore`；若仍异常→查 `fbgeom.log` 是否没跑 |
| 起不来（180 s 不通） | 极端情况 | 等 3 分钟；护栏会回滚 kext；仍不行则用 U 盘 `D:\EFI` 启动，删 `/Library/Displays` 目录 |

---

## 6. 若第一次没生效（两次尝试上限）

**尝试 2（唯一变量：多加一条逻辑分辨率）**
在 `scale-resolutions` 里再加 1560×1040：
```bash
sudo /usr/libexec/PlistBuddy -c "Add :scale-resolutions:1 data AAAGGAAABBAAAAABACAAAA==" "$P"
sudo plutil -lint "$P"
```
重启，重复 §4.3 的读数。

**若尝试 2 仍无任何新选项 → 停手**，不要继续加条目、不要动 `Resolution`/`UIScale`、不要碰系统卷。
报告"`/Library` override 未被采纳"，并给出这条判断：剩下的路只有
① 让 framebuffer 自己发布多个 timing（`XeGfxIntelFB` 那条自建 IOFramebuffer 的重工程），
② 或退一步用 1920×1080 的 HiDPI 背衬（`Resolution` 改回显式 `1920x1080`，UI 变大但失去原生像素）。
由主 agent 决定走哪条。

---

## 7. 交付格式

| # | 检查 | 命令 | 期望 | 结果 | 实际输出 |
|---|---|---|---|---|---|
| 1 | override 文件就位 | `ls -l /Library/Displays/.../DisplayProductID-717` | root:wheel 644 | | |
| 2 | 格式合法 | `plutil -lint` | OK | | |
| 3 | 保留了原有键 | `plutil -p` 对比系统原件 | 原有键都在，只多 `scale-resolutions` | | |
| 4 | **UI 放大** | `system_profiler` | `UI Looks like: 1560 x 1040` | | |
| 5 | 原生未丢 | `system_profiler` / `ioreg` | `Resolution: 3120 x 2080`、`IOFBMemorySize 25958400` | | |
| 6 | 几何未变 | `xenative-ctl status` | `STRIDE 195`、`SIZE 0x081F0C2F`、`PIPESRC 0x0C2F081F` | | |
| 7 | 无回归 | `fbgeom.log` / panic / WindowServer | 脚本正常完成、零新 panic、WS 在 | | |

必须附：**桌面对比照片**（改前/改后各一张，能看出字号变化）、`plutil -p` 的完整输出。
一行结论：`A-15 成功：HiDPI 1560×1040 / 背衬 3120×2080` / `A-15 未生效：/Library override 未被采纳`。

---

## 验收锚点（主 agent 独立跑）

1. `system_profiler SPDisplaysDataType` → `Resolution: 3120 x 2080` **且** `UI Looks like: 1560 x 1040`。
2. `ioreg -lw0 -r -c IONDRVFramebuffer | grep IOFBMemorySize` → 仍 `25958400`。
3. `xenative-ctl status` → `STRIDE 195`、`SIZE 0x081F0C2F`、`PIPESRC 0x0C2F081F`（HiDPI 是合成层的放大，不应改扫描几何）。
4. `/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-756e6b6e/DisplayProductID-717` 存在、`plutil -lint` OK。
5. 零新 panic；`fbgeom.log` 有本次开机记录；屏幕内容正确无错位。
