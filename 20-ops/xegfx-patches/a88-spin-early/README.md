# a88-spin-early — 把"工作量 → 帧时间"量化，并直接测 0024 的 E2 提前信号

2026-11-03 · Gate 7 / A-88。报告：`../../G7-A88报告.md`。

## 这份归档回答两个问题

1. **往工作里加 X µs，帧时间什么时候跳？**（`patch_a88_spin.py`，插件侧可调自旋）
2. **把显示事务的完成从"等 latch"提前到"写完 PLANE_SURF"，能少一拍吗？**（`patch_a88_early.py`，accel 侧开关）

答案：**① 每帧多加 4.5 ms 工作，p50 一步不动（24.75 → 24.99），加到 13.5 ms 才跳 4 拍 ⇒ 余量 ≥4.5 ms；
② 不能，−0.07 ms。早信号每帧都发（796/826 次），`AccessComplete` 仍 3.01 ms 未动 —— 它等的不是我们这个 pipe。**

## 文件

| 文件 | 作用 |
| --- | --- |
| `patch_a88_spin.py` | 插件（`~/src/NvGfx-xe-2.6.2/plugin` 的 A-87 树）3 处锚点：自旋 helper + 提交路径 + ahead worker |
| `patch_a88_early.py` | accel（`~/src/a81-tree/kexts/NVRM`）9 处锚点：`gIopXeEarly` 开关 + 在 arming write 后完成事务 |
| `a88-install.sh` | 装插件（原子 rename + 特性串校验 + 备份到 `~/xegfx-log/A-88/backup/`） |
| `a88-install-accel.sh` | 装 accel + 重建 AuxKC（校验 `IGPUConsoleRange` 还在） |
| `run-a88.sh` | 一轮：设自旋 → 25 s 拖动 + frame-times + sample → 清自旋（防会话断了机器还在自旋） |
| `run-e2.sh` | 一轮：`debug.xegfxaccel_iop_xe_early=0/1` → 25 s 拖动 + frame-times + sample |
| `sweep1.sh` | 整条 P1 曲线（0/1000/2000/3000/5000/8000 + 一个 worker 对照，每点两轮） |
| `stats.py` | frame-times 原始间隔 → p50/p90/p99 + **8.33 ms 分桶**（双峰分布只有在分桶里才看得见） |

## 运行时开关（都不需要重启）

| 开关 | 位置 | 语义 |
| --- | --- | --- |
| 自旋 | `/private/tmp/nvmtl-spin`，内容 `"<µs>[m\|w]"` | `m` = 提交路径（主线程），`w` = submit-ahead worker；无文件/0 = 关 |
| E2 | `debug.xegfxaccel_iop_xe_early`（0/1） | 1 = 写完 `PLANE_SURF_1_A` 就完成事务，不等 latch |
| 计数 | `..._early_signals` | 早完成发生了多少次（应与帧数同量级） |

## 与历史结论的关系

* A-84 用**推算**否掉 E2（25.20 − 4.53 = 20.7 > 16.68）；本关用**开关 + 实测**否掉，且给出更硬的原因（打错等待对象）。
* A-87 砍掉 4.08 ms 类锁帧不动；本关的阶梯表从**正面**解释：4.5 ms 的工作量本来就在余量里。
* 两者合起来：**这条链是串行的，帧的落点是 WS/CA/CoreDisplay 的呈现节拍，不是驱动的忙碌程度。**
