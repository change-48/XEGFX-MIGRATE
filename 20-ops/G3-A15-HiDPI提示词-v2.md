> **⚠️ 本文被 v3 取代（三条修正方向都对，但实现细节要改）。**
> §4.2 从零写 XML 会**丢掉系统原件的 `tinf`/`tovr` 两张表**（主 agent 已读出原件确有这两项）；
> §6 的 heredoc 里留着 `...（同上…）` 占位符，照抄会把省略号写进文件。
> **请用 `G3-A15-HiDPI提示词-v3.md`（python3 + plistlib，保留原件键、字节精确）。**

# Gate 3 · A-15 执行提示词 v2（HiDPI：原生像素不变，UI 放大到 2×）

> 用法：把本文件全文作为一条消息发给执行会话。执行会话不需要任何前置上下文。
> **一句话：写一个用户级显示器 override，让 macOS 提供 "看起来 1560×1040" 的 HiDPI 模式，扫描输出仍是原生 3120×2080。**

> **v2 相比 v1 改了 3 处（v1 会直接失败，别用 v1）**
> 1. **致命**：`PlistBuddy -c "Add :scale-resolutions:0 data AAAMM..."` **写不对**。PlistBuddy 的 `data`
>    不按 base64 解析，它会把整串 base64 文本再编码成 data（落盘变成 `QUFBQU1NQUFBQ0...`）。
>    必须改成**整份 XML 用 heredoc 写**（§4.2）。已在本机用 base64 双向验证过。
> 2. **漏了一步**：必须 `defaults write /Library/Preferences/com.apple.windowserver DisplayResolutionEnabled -bool YES`
>    ——one-key-hidpi 的 `end()` 里就是这一步，缺了它新档可能不进"缩放"列表。
> 3. **尝试 2 方向错了**：`1560x1040` 那条不是"逻辑分辨率"，而是 **2× 的背衬尺寸**。
>    加 `1560x1040` 会声明"背衬 1560×1040 的 HiDPI 档"（看起来 780×520），只是多一个怪选项。
>    尝试 2 改成加 12 字节的裸 3120×2080 档（§6）。

---

## 0. 现状与目标

A-14 已达成原生 3120×2080：`IOFBMemorySize = 25958400`、`PLANE_STRIDE 195`、`PIPESRC/SIZE` 都是 3120×2080、缩放器已由系统关闭。

**副作用（用户实机反馈）：画面铺满了，但 UI 元素偏小。** 面板 31.2×20.8 cm / 3120×2080 ≈ **254 DPI（10 px/mm）**，
1:1 显示时字就是物理上很小。正确解法不是降分辨率，而是 **HiDPI**：系统按 2× 渲染（1560×1040 逻辑点 → 3120×2080 背衬），扫描输出不变。

**为什么这次应该能成**：override 声明的 HiDPI 档，背衬尺寸正好等于当前原生时序，framebuffer 不需要新 timing，
只是同一块像素换个"逻辑点"解释。所以本关不涉及寄存器、kext、OpenCore。

---

## 1. 已核实的现场事实（不要重新探索）

| 项 | 值 |
| --- | --- |
| `DisplayVendorID` | `1970170734` = **`0x756e6b6e`**（ASCII "unkn"） |
| `DisplayProductID` | `1815` = **`0x717`** |
| 目标文件 | `/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-756e6b6e/DisplayProductID-717` |
| 系统原件（只读参考） | `/System/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-756e6b6e/DisplayProductID-717` |
| 根卷 | `apfs, sealed, read-only` → `/System` 写不进去，**不要破坏封存、不要关 SIP** |
| `/Library/Displays` | 不存在，自己建（one-key-hidpi 也只建这一层，不拷整棵树） |
| 当前模式表 | `IOFBConfig` 只有 1 个模式（`IOFB0Hz=Yes`）→ 设置里没有比例选项 |

---

## 2. 编码事实（已按上游源码核对，照抄即可）

`scale-resolutions` 每个 `<data>` 的字节 = **大端 UInt32 序列**，前两个是 **背衬（framebuffer）宽高，不是逻辑点**：

| 字节数 | 结构 | 含义 |
| --- | --- | --- |
| 8 | `w, h` | 裸档（少见） |
| 12 | `w, h, 0x1` | 普通档 |
| **16** | `w, h, 0x1, 0x00200000` | **HiDPI 档**：`0x00200000` 是 HiDPI 位，系统显示为 `w/2 × h/2` "看起来像" |

依据：`xzhih/one-key-hidpi` 的 `create_res_2()` 对逻辑 1280×800 写的是 `2560,1600,1,0x00200000`
（即"看起来 1280×800"），`init()` 只 `mkdir -p /Library/Displays/Contents/Resources/Overrides`，`end()` 才 `defaults write ...DisplayResolutionEnabled`。

本关要的三条（本机 base64 双向验证过，与社区样例 `1920x1080 -> AAAHgAAABDgAAAABACAAAA==` 同规则）：

| 尺寸 | 十六进制 | base64 |
| --- | --- | --- |
| 3120×2080 HiDPI（**主条目，要的就是这条**） | `00000c30 00000820 00000001 00200000` | `AAAMMAAACCAAAAABACAAAA==` |
| 3120×2080 普通档（备用，尝试 2 用） | `00000c30 00000820 00000001` | `AAAMMAAACCAAAAAB` |
| 1560×1040 HiDPI（**不要用**，见 §6） | `00000618 00000410 00000001 00200000` | `AAAGGAAABBAAAAABACAAAA==` |

目录名、文件名用**十六进制小写不补零**（`0x717` → `717`）；plist 内部 `DisplayVendorID/ProductID` 用**十进制**。这是 Apple 的规矩。

---

## 3. 铁律

- **不碰 `/System`**，不关 SIP，不破坏封存。
- **不动任何寄存器 / kext / OpenCore config**。本关只加一个 plist + 一个 defaults 键。
- 写之前先备份目标位置原有内容；原本不存在就记"原本为空"，回滚 = `sudo rm -rf /Library/Displays` + `defaults delete`。
- 一次只改一个变量；没生效**不要**顺手去动 `Resolution` / `UIScale`。
- 收尾必须保证：屏幕仍是原生 3120×2080、`fbgeom` 自愈脚本仍工作、零新 panic。

---

## 4. 步骤

### 4.1 现状取证（不改动）

```bash
SYS=/System/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-756e6b6e/DisplayProductID-717
D=/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-756e6b6e
P="$D/DisplayProductID-717"

ls -l "$SYS" 2>&1
sudo plutil -p "$SYS" 2>&1 | head -20        # 看清原件有哪些键（可能是二进制 plist）
ls -laR /Library/Displays 2>&1 | head -20    # 预期"不存在"
defaults read /Library/Preferences/com.apple.windowserver 2>&1
system_profiler SPDisplaysDataType | grep -E "Resolution|UI Looks like"
```
把这两段输出留着，交付时要对比。

### 4.2 写 override（整份 XML heredoc，**不要用 PlistBuddy 写 data**）

```bash
sudo mkdir -p "$D"
[ -e "$P" ] && sudo cp -p "$P" "$P.bak-$(date +%Y%m%d-%H%M%S)"

sudo tee "$P" >/dev/null <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>DisplayProductID</key>
	<integer>1815</integer>
	<key>DisplayVendorID</key>
	<integer>1970170734</integer>
	<key>DisplayProductName</key>
	<string>EDO14.55</string>
	<key>scale-resolutions</key>
	<array>
		<data>AAAMMAAACCAAAAABACAAAA==</data>
	</array>
	<key>target-default-ppmm</key>
	<real>10.0</real>
</dict>
</plist>
EOF

sudo chown root:wheel "$P" && sudo chmod 644 "$P"
sudo chown root:wheel /Library/Displays && sudo chmod 755 /Library/Displays
```

> 关于 `IODisplayEDID`：上游 `enable_hidpi()` 会主动删掉这个键（`sed "/.*IODisplayEDID/d"`），
> 说明纯 HiDPI 路径不需要它。**不要**从原件抄 EDID，抄错反而会改掉显示身份。原件里的其它键若想保留，
> 在 §4.1 的 `plutil -p` 输出里看清楚再决定，但**不要为此阻塞本关**。

### 4.3 打开分辨率下拉（漏了这步可能看不到新档）

```bash
sudo defaults write /Library/Preferences/com.apple.windowserver DisplayResolutionEnabled -bool YES
defaults read /Library/Preferences/com.apple.windowserver
```

### 4.4 落盘校验（**这一步不过就不要重启**）

```bash
sudo plutil -lint "$P"
sudo plutil -p "$P" | grep -A3 scale-resolutions
# 期望看到 data 的十六进制是：00000c30 00000820 00000001 00200000
# 若看到 "QUFBQU1NQUFB..." 这种，说明又用了 PlistBuddy base64 写法 → 重做 §4.2
sudo ls -l "$P"
```

### 4.5 重启并验证

HiDPI 模式表在 WindowServer 启动时读取，必须重启。

```bash
touch /var/db/xegfx/armed && sudo reboot
```

回来之后：

```bash
system_profiler SPDisplaysDataType | grep -E "Resolution|UI Looks like|Main Display"
ioreg -lw0 -r -c IONDRVFramebuffer | grep -o "IOFBMemorySize\"=[0-9]*"
sudo /usr/local/bin/xenative-ctl status | grep -E "STRIDE|SIZE|PIPESRC|PS_CTRL"
cat /var/db/xegfx/fbgeom.log | tail -6
```

---

## 5. 判读

| 现象 | 含义 | 下一步 |
| --- | --- | --- |
| `UI Looks like: 1560 x 1040`，`Resolution` 仍 `3120 x 2080` | ✅ 完全成功 | 收尾，报告成功（附桌面照片） |
| 设置 → 显示器里出现"看起来像 1560×1040"，但没选中 | 模式表已生效 | 手动选中，再读 `UI Looks like` |
| 仍是 3120×2080，设置里也没有新选项 | override 没被采纳 | 见 §6 |
| 屏幕错位/异常 | 不该发生（本关没碰几何） | 查 `fbgeom.log`；仍异常则回滚 §7 |
| 起不来（180 s 不通） | 极端情况 | 等 3 分钟；护栏会回滚 kext；仍不行用 U 盘 `D:\EFI` 启动，删 `/Library/Displays` |

---

## 6. 若第一次没生效（**两次尝试上限**）

**尝试 2（唯一变量：additionally 声明一条普通档）**

```bash
# 12 字节 = 3120,2080,0x1 的普通档；与 HiDPI 档并存
sudo tee "$P" >/dev/null <<'EOF'
...（同上，只把 <array> 换成下面两行）
		<data>AAAMMAAACCAAAAABACAAAA==</data>
		<data>AAAMMAAACCAAAAAB</data>
EOF
sudo plutil -lint "$P" && sudo reboot
```

**不要**加 `1560x1040` 那条：它的语义是"背衬 1560×1040 的 HiDPI 档"（看起来 780×520），
只会多一个怪的极小选项，不会得到你要的放大效果。

**若尝试 2 仍无任何新选项 → 停手**，不要再加条目、不要动 `Resolution`/`UIScale`、不要碰系统卷。
报告"`/Library` override 未被采纳"，并附这条判断（剩下的路只有两条）：

1. 让 framebuffer 自己发布多个 timing（`XeGfxIntelFB` 自建 IOFramebuffer 的重工程）；
2. 退一步用 1920×1080 的 HiDPI 背衬（丢原生像素，UI 变大）。

由主 agent 决定走哪条。

---

## 7. 回滚（一行）

```bash
sudo rm -rf /Library/Displays && sudo defaults delete /Library/Preferences/com.apple.windowserver DisplayResolutionEnabled 2>/dev/null; sudo reboot
```
回滚后预期回到 A-14 的 3120×2080 原生状态（几何不受影响，因为本关没碰它）。

---

## 8. 交付格式

| # | 检查 | 命令 | 期望 | 结果 | 实际输出 |
|---|---|---|---|---|---|
| 1 | 文件就位 | `ls -l "$P"` | root:wheel 644 | | |
| 2 | 格式合法 | `plutil -lint` | OK | | |
| 3 | data 字节正确 | `plutil -p` | `00000c30 00000820 00000001 00200000` | | |
| 4 | defaults 已开 | `defaults read` | `DisplayResolutionEnabled = 1` | | |
| 5 | **UI 放大** | `system_profiler` | `UI Looks like: 1560 x 1040` | | |
| 6 | 原生未丢 | `system_profiler` / `ioreg` | `Resolution: 3120 x 2080`、`IOFBMemorySize 25958400` | | |
| 7 | 几何未变 | `xenative-ctl status` | `STRIDE 195`、`SIZE 0x081F0C2F`、`PIPESRC 0x0C2F081F` | | |
| 8 | 无回归 | `fbgeom.log` / panic / WS | 脚本正常完成、零新 panic、WS 在 | | |

必须附：**桌面对比照片**（改前/改后各一张，能看出字号变化）、`plutil -p` 完整输出、`plutil -lint` 输出。
一行结论：`A-15 成功：HiDPI 1560×1040 / 背衬 3120×2080` / `A-15 未生效：/Library override 未被采纳`。

---

## 验收锚点（主 agent 独立跑）

1. `system_profiler SPDisplaysDataType` → `Resolution: 3120 x 2080` **且** `UI Looks like: 1560 x 1040`。
2. `ioreg -lw0 -r -c IONDRVFramebuffer | grep IOFBMemorySize` → 仍 `25958400`。
3. `xenative-ctl status` → `STRIDE 195`、`SIZE 0x081F0C2F`、`PIPESRC 0x0C2F081F`（HiDPI 是合成层放大，不应改扫描几何）。
4. `/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-756e6b6e/DisplayProductID-717` 存在、`plutil -lint` OK、
   `scale-resolutions[0]` 的 base64 恰为 `AAAMMAAACCAAAAABACAAAA==`。
5. 零新 panic；`fbgeom.log` 有本次开机记录；屏幕内容正确无错位。
