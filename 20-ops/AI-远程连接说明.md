# 远程调试：连接说明

给需要在这台机器上远程调试驱动（触控板、蓝牙、Wi-Fi、音频等）的 AI。

---

## 1. 连接

```bash
ssh -i ~/.ssh/id_xegfx_remote change48@IP-1
```

- 私钥在发起连接的机器上（Windows 走 Git Bash 即可，命令同上）
- 登录用户 `change48`，**`sudo` 免密**（NOPASSWD），可以直接写
- 机器常开，地址 `IP-1`（同一局域网）

> 这是使用者日常在用的 macOS，不是无人值守的测试机。**每次要重启，先问一声。**

---

## 2. 这台机器

```
HONOR MagicBook Art 14 2025 · Intel Core Ultra 7 255H · 32 GB
macOS 26.5.2 (25F84) · OpenCore DBG-108-2026-09-27 · SMBIOS 伪装 MacBookPro16,2
```

**当前跑着一套工作中的图形栈**（WindowServer 在核显上合成、3120×2080 HiDPI）。
它不是本次调试的目标，但很容易被顺带弄坏——见第 5 节。

---

## 3. 常用位置

| 路径 | 内容 |
| --- | --- |
| `/Library/Extensions/` | 已装 kext |
| `/Library/KernelCollections/AuxiliaryKernelExtensions.kc` | 辅助内核集合（改 kext 后要重建） |
| `/Library/Logs/DiagnosticReports/` | panic 与进程崩溃报告 `.ips` |
| `/var/log/xegfx-bringup.log` | 开机自启日志（图形栈，仅参考） |
| `~/src/` | 驱动源码 |
| `~/xegfx-log/` | 历次实验的证据目录 |

---

## 4. 常用命令

```bash
# kext 是否加载、版本
kmutil showloaded | grep -i <name>

# 设备节点（触控板 / 蓝牙 / HID / USB 都靠它）
ioreg -l -w0 -c <ClassName>
ioreg -l -p IOACPIPlane            # ACPI 侧（I2C / GPIO / 触控板节点）
ioreg -l -w0 -p IOService          # 服务树

# 日志（按 kext 或子系统过滤）
log show --last 10m --predicate 'senderImagePath CONTAINS "<KextName>"'
log stream --predicate 'eventMessage CONTAINS "<关键字>"'      # 实时

# 硬件枚举
system_profiler SPBluetoothDataType SPAirPortDataType SPUSBDataType

# 资源占用（毫秒/秒，不是百分比；1 核 = 1000）
powermetrics --samplers tasks -n 1

# 重启 / 关机
sudo shutdown -r now
```

---

## 5. 三条规矩（踩过坑，代价由使用者承担）

**① 改 kext 后必须重建内核集合，否则开机进不去系统。**

```bash
sudo /usr/bin/kmutil create -n aux --volume-root / --allow-missing-kdk \
     -B /System/Library/KernelCollections/BootKernelExtensions.kc \
     -S /System/Library/KernelCollections/SystemKernelExtensions.kc \
     --repository /Library/Extensions -A /tmp/aux.kc -z
sudo cp -f /tmp/aux.kc /Library/KernelCollections/AuxiliaryKernelExtensions.kc
```

改之前先把 `AuxiliaryKernelExtensions.kc` 和原 kext 各备份一份到 `~/xegfx-log/<日期>/`。
**核对 `kmutil inspect` 里 `org.watchthelight.IGPUConsoleRange` 还在**——少它开机会黑屏。

**② 别碰显示路径。** 以下三样属于图形栈，一旦改错会让整机冻结、只能长按电源：

- `/Library/Extensions/XeGfx*`（尤其 `XeGfxAccel`、`XeGfxIntelFB`）
- `/Library/GPUBundles/nvmtl/`
- 平面寄存器（`PLANE_SURF` / `PLANE_STRIDE`，工具 `xenative-ctl` 能写）

**③ 输入设备坏了比黑屏更麻烦**——没触控板没键盘就进不去桌面。
调试输入栈时保持 SSH 可用（它不依赖 HID），并准备好"装回备份 + 重启"这一条退路。

---

## 6. 出问题先取证，再动手

```bash
L=~/xegfx-log/$(date +%Y%m%d-%H%M)-<主题>; mkdir -p $L
sudo ioreg -l -w0 > $L/ioreg.txt
sudo log show --last 5m > $L/log.txt
sudo dmesg 2>/dev/null | tail -200 > $L/dmesg.txt
ls -t /Library/Logs/DiagnosticReports/*.panic | head -3
kmutil showloaded | grep -vi "com.apple" > $L/kexts.txt
```

**先拿到现状，再改。** 改完用第二种方式再验一次（读回、计数、或请人看一眼），
不要只信工具打印的 "ok/success"——今天已经出现过打印成功但实际没生效的情况。

---

## 7. 需要先问人的情况

- 要重启（会打断使用者）
- 要改 EFI / `boot-args`（改错开不了机）
- 要动显示相关（见规矩 ②）
- 要对外发布任何东西（issue / PR / 上传日志）
