# Gate 3 · A-15 执行提示词 v3（HiDPI，最终版）

> **用这份，不要用 v1 / v2。**
> v1 的 `PlistBuddy ... data <base64>` 已实测确认写错（落盘变成 24 字节 ASCII，不是 16 字节二进制）。
> v2 三条修正方向都对，但 **§4.2 会丢掉系统原件里的 `tinf`/`tovr` 两张表**，**§6 的 heredoc 里留了 `...` 占位符**
> （照抄会把省略号写进文件）。v3 用 `python3 + plistlib` 一次解决：保留原件所有键、字节精确、无手写 XML。

---

## 0. 目标

现在：原生 3120×2080、`IOFBMemorySize 25958400`、`PLANE_STRIDE 195`、缩放器已被系统关闭。**画面铺满，但 UI 偏小**（254 DPI）。
目标：**HiDPI** —— 逻辑 1560×1040 点，背衬仍是原生 3120×2080。扫描几何、寄存器、kext 一律不动。

---

## 1. 已核实事实

| 项 | 值 |
| --- | --- |
| `DisplayVendorID` | `1970170734` = `0x756e6b6e`（ASCII "unkn"） |
| `DisplayProductID` | `1815` = `0x717` |
| 目标文件 | `/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-756e6b6e/DisplayProductID-717` |
| 系统原件（**只读，但可读**） | `/System/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-756e6b6e/DisplayProductID-717` |
| 原件内容（主 agent 已读） | `DisplayProductID=1815`、`DisplayVendorID=1970170734`、**`tinf`**（150/180/182 各 16B）、**`tovr`**（130…165 十余条 16B） |
| 根卷 | `apfs, sealed, read-only` → **不碰 `/System`、不关 SIP、不破封存** |
| `/Library/Displays` | 不存在，要建 |
| `com.apple.windowserver` | domain **不存在**（`defaults read` 报 does not exist） |
| `IOFBConfig` | 只有 1 个模式（`IOFB0Hz=Yes`）→ 设置里没有比例选项 |

`scale-resolutions` 的 `<data>` 是**大端 UInt32 序列**，前两个是**背衬**尺寸（不是逻辑点）：

| 字节 | 结构 | 含义 |
| --- | --- | --- |
| 16 | `w, h, 1, 0x00200000` | **HiDPI 档** → 系统显示为 `w/2 × h/2`「看起来像」 |
| 12 | `w, h, 1` | 普通档 |

本关要的两条（编码器已用社区样例 `1920x1080 → AAAHgAAABDgAAAABACAAAA==` 反推校验）：

| 用途 | 十六进制 | base64 |
| --- | --- | --- |
| **主条目**（要的就是这条） | `00000c30 00000820 00000001 00200000` | `AAAMMAAACCAAAAABACAAAA==` |
| 备用普通档（只在尝试 2 用） | `00000c30 00000820 00000001` | `AAAMMAAACCAAAAAB` |

**不要**加 `1560×1040` 的条目 —— 那声明的是"背衬 1560×1040 的 HiDPI 档"（看起来 780×520），是极小选项，不是放大。

---

## 2. 铁律

- 不碰 `/System`、不关 SIP、不改 OpenCore / boot-args / kext / 任何显示寄存器。
- 写之前备份目标文件；原本不存在就记"原本为空"，回滚 = `sudo rm -rf /Library/Displays`。
- 一次只改一个变量；没生效不要顺手动 `Resolution` / `UIScale`。
- **落盘校验不过就不许重启**（省一轮重启）。
- 收尾必须：屏幕仍原生 3120×2080、`fbgeom` 自愈脚本仍工作、零新 panic。

---

## 3. 执行

### 3.1 取证（不改动）

```bash
SYS=/System/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-756e6b6e/DisplayProductID-717
D=/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-756e6b6e
P="$D/DisplayProductID-717"

sudo plutil -p "$SYS" > /tmp/a15-sys-orig.txt 2>&1; head -20 /tmp/a15-sys-orig.txt
ls -laR /Library/Displays 2>&1 | head -5
system_profiler SPDisplaysDataType | grep -E "Resolution|UI Looks like"
```

### 3.2 用 python3 + plistlib 写（保留原件所有键，字节精确）

```bash
sudo mkdir -p "$D"
[ -e "$P" ] && sudo cp -p "$P" "$P.bak-$(date +%Y%m%d-%H%M%S)"

sudo python3 - "$SYS" "$P" <<'PY'
import plistlib, struct, sys, os
src, dst = sys.argv[1], sys.argv[2]
d = {}
if os.path.exists(src):                      # 以系统原件为基底，保留 tinf/tovr 等
    with open(src, 'rb') as f:
        d = plistlib.load(f)
d['DisplayProductID'] = 1815
d['DisplayVendorID'] = 1970170734
d.setdefault('DisplayProductName', 'EDO14.55')
d.pop('IODisplayEDID', None)                 # 上游 enable_hidpi() 也是主动删掉，抄错会改显示身份
d['scale-resolutions'] = [
    struct.pack('>IIII', 3120, 2080, 1, 0x00200000),   # HiDPI：看起来 1560x1040
]
d['target-default-ppmm'] = 10.0              # 3120px / 312mm = 10.0 px/mm，告诉系统这是高 DPI 面板
with open(dst, 'wb') as f:
    plistlib.dump(d, f, fmt=plistlib.FMT_XML)
print('written', dst)
PY

sudo chown -R root:wheel /Library/Displays && sudo chmod 755 /Library/Displays && sudo chmod 644 "$P"
```

> 为什么不用 PlistBuddy：**实测** `PlistBuddy -c "Add :d data AAAMM..."` 存进去的是
> `length=24, bytes=0x4141414d4d414141...`（base64 的 ASCII 原文），不是 16 字节二进制。已复现两次。

### 3.3 打开分辨率下拉

```bash
sudo defaults write /Library/Preferences/com.apple.windowserver DisplayResolutionEnabled -bool YES
sudo defaults read  /Library/Preferences/com.apple.windowserver      # read 也要 sudo，否则报 domain 不存在
```

### 3.4 落盘校验（**不过就不许重启**）

```bash
sudo plutil -lint "$P"
sudo plutil -p "$P" | grep -E "scale-resolutions|target-default|DisplayProduct"
sudo plutil -p "$P" | grep -o "bytes = 0x[0-9a-f]*" | head -3
# 期望 scale-resolutions 那段是  bytes = 0x00000c30000008200000000100200000   （16 字节）
# 若看到 0x4141414d4d...（ASCII "AAAM"）→ 写错了，重做 §3.2
sudo plutil -p "$P" | grep -c '"tinf"\|"tovr"'    # 期望 ≥ 1，证明原件键被保留
```

### 3.5 重启并验证

```bash
touch /var/db/xegfx/armed && sudo reboot
```
回来后：
```bash
system_profiler SPDisplaysDataType | grep -E "Resolution|UI Looks like|IOFBMemorySize"
sudo ioreg -lw0 -r -c IONDRVFramebuffer | grep -o 'IOFBMemorySize"=[0-9]*'
sudo /usr/local/bin/xenative-ctl status | grep -E "STRIDE|SIZE|PIPESRC|PS_CTRL"
tail -6 /var/db/xegfx/fbgeom.log
```

---

## 4. 判读

| 现象 | 含义 | 下一步 |
| --- | --- | --- |
| `UI Looks like: 1560 x 1040`，`Resolution` 仍 `3120 x 2080` | ✅ 成功 | 收尾，附对比照片 |
| 设置里出现「看起来像 1560×1040」但未选中 | 模式表生效 | 手动选中后重读 `UI Looks like` |
| 仍 3120×2080、无新选项 | override 没被采纳 | 尝试 2 |
| 屏幕错位 / 起不来 | 本关没碰几何，不该发生 | 查 `fbgeom.log`；异常则回滚 §5 |

**尝试 2（唯一变量：多一条普通档）** —— 用同样的 python 方式，把 `scale-resolutions` 改成两项：

```bash
sudo python3 - "$P" <<'PY'
import plistlib, struct, sys
p = sys.argv[1]
with open(p, 'rb') as f: d = plistlib.load(f)
d['scale-resolutions'] = [
    struct.pack('>IIII', 3120, 2080, 1, 0x00200000),
    struct.pack('>III',  3120, 2080, 1),
]
with open(p, 'wb') as f: plistlib.dump(d, f, fmt=plistlib.FMT_XML)
print('ok')
PY
sudo plutil -lint "$P" && sudo reboot
```

**两次都不行 → 停手**，报告"`/Library` override 未被采纳"。剩下的路只有：① 让 framebuffer 自己发布多个 timing（`XeGfxIntelFB` 自建 IOFramebuffer，重工程）；② 退回 1920×1080 背衬（UI 变大、丢原生像素）。由主 agent 定。

---

## 5. 回滚（一行）

```bash
sudo rm -rf /Library/Displays && sudo defaults delete /Library/Preferences/com.apple.windowserver DisplayResolutionEnabled; sudo reboot
```
几何不受影响（本关没碰它），回滚后回到 A-14 的原生 3120×2080。

---

## 6. 交付

| # | 检查 | 期望 |
|---|---|---|
| 1 | 文件就位 | `root:wheel 644` |
| 2 | `plutil -lint` | OK |
| 3 | **data 字节** | `0x00000c30000008200000000100200000`（16B），不是 `0x4141414d4d...` |
| 4 | 原件键保留 | `tinf` / `tovr` 仍在 |
| 5 | defaults | `DisplayResolutionEnabled = 1` |
| 6 | **UI 放大** | `UI Looks like: 1560 x 1040` |
| 7 | 原生未丢 | `Resolution: 3120 x 2080`、`IOFBMemorySize 25958400` |
| 8 | 几何未变 | `STRIDE 195`、`SIZE 0x081F0C2F`、`PIPESRC 0x0C2F081F` |
| 9 | 无回归 | `fbgeom.log` 正常、零新 panic、WindowServer 在 |

必须附：改前/改后**桌面照片各一张**（能看出字号）、`plutil -p "$P"` 完整输出、`plutil -lint` 输出。
一行结论：`A-15 成功：HiDPI 1560×1040 / 背衬 3120×2080` 或 `A-15 未生效：/Library override 未被采纳`。

---

## 验收锚点（主 agent 独立跑）

1. `system_profiler SPDisplaysDataType` → `Resolution: 3120 x 2080` **且** `UI Looks like: 1560 x 1040`。
2. `ioreg -lw0 -r -c IONDRVFramebuffer` → `IOFBMemorySize = 25958400`。
3. `xenative-ctl status` → `STRIDE 195`、`SIZE 0x081F0C2F`、`PIPESRC 0x0C2F081F`（HiDPI 是合成层放大，不该改扫描几何）。
4. `plutil -p "$P"` 里 `scale-resolutions[0]` 的 `bytes` 恰为 16 字节 `00000c30 00000820 00000001 00200000`。
5. 零新 panic；`fbgeom.log` 有本次开机记录；屏幕内容正确无错位。
