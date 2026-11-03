# a89-tl-raw — 逐帧打点：给 2 拍帧和 3 拍帧安上绝对时间戳和硬件刷新计数

2026-11-03 · Gate 7 / A-89。报告：`../../G7-A89报告.md`。

## 这份归档回答一个问题

**同一驱动、同一负载，为什么有些帧落在 2 拍（16.7 ms = 60 fps）、有些落在 3 拍（25 ms）？** 分界是相位，还是工作量？

答案：**都不是。** 每帧时间精确拆成五段之后，驱动内四段合计 10.8 ms，与帧时间的相关是 **−0.01**；
信号之后那一段 `G`（WindowServer 合成 + Present + `waitUntilScheduled`）14.1 ms，相关 **+0.95**、方差份额 **100%**。
两组帧的相位分布重叠（4.74 / 4.57 ms），arm 在刷新周期内的落点近似均匀随机 —— 相位不是分界。

## 文件

| 文件 | 作用 |
| --- | --- |
| `patch_a89_tl.py` | accel（`~/xegfx-log/A-89/tree-tl`）14 处锚点：环加 `sub`/`vcnt` 两列 + 新增 `debug.xegfxaccel_iop_tl_raw` |
| `a89-install-accel.sh` | 装 accel + 重建 AuxKC（特性串校验 + 备份到 `~/xegfx-log/A-89/backup/`） |
| `run-a89.sh` | 一轮：准入五条 → 25 s 拖动 + frame-times + sample → **五次环快照**（4 s 间隔，按 idx 去重后覆盖整场） |
| `classify.py` | 读原始列 → 用硬件计数器标签拍数 → 五段分解 + 各段对 `var(dt)` 的协方差份额 + 相位/成簇统计 |

## 为什么必须加这两个字段

| 字段 | 含义 | 缺了会怎样 |
| --- | --- | --- |
| `sub` | `performTransaction` 入口的 `mach_absolute_time()` | 环里原来的 `enter` 会被 flip worker 的 arm 覆盖（A-75 语义），**帧被交出来那一刻**从没被记过，相位无从谈起 |
| `vcnt` | 同一时刻读 BAR0 `0x70040` | 拍数只能靠"时间戳落在 8.33 的哪一档"去猜；有了它就是**硬件读数** |

`debug.xegfxaccel_iop_tl_raw` 是纯文本列（200 帧，约 18 KB），A-72 那张 µs 表原样保留，A-88 的 `stats.py` 照旧可用。
**零行为改动**：多的只是一次 MMIO 读（`nvXePlaneWatch` 本来就每 100 ms 读同一个寄存器）和 8 字节记录。

## 刷新周期是量出来的

`(sub_last − sub_first) / (vcnt_last − vcnt_first)` = **8.335 ms**（1464 计数跨 12.2 s）。
逐帧比值会被整数化带偏（p10 6.91 / p90 10.64），务必用端到端。120 Hz 别写死 8.3333。

## 装机注意（踩过一次）

`kmutil create` 在 kext 审批还没落盘时会静默产出一个**不含 accel 的 AuxKC**，开机 log 是
`tried to insert an invalid codeful kernel extension in the restricted lookup table`，系统照常起来但核显加速没了。
判据是 `sudo kmutil showloaded | grep watchthelight.XeGfxAccel`，不是 `kmutil inspect` 里有没有它。
