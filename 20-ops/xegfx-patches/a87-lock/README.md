# a87-lock · 提交路径"锁内只 claim、锁外干活"

G7 A-87 的实验件：把 submit-ahead pass 的 451 µs/次提交从类锁
`@synchronized ([NVMTLCommandBuffer class])` 里搬出去。

**结果：锁等待 4.08 → 0.01 ms/帧，p50 25.30 → 25.00/25.12 ms（帧没变）。**
靶子打空了——详见 `20-ops/G7-A87报告.md`。下面的东西留着，是因为它是"证据"，也是下次改这条路径的起点。

## 为什么不是"给 ahead 一把独立的锁"

A-87 P0 锁边界表（报告 §2）的结论：ahead 与 dealloc/enqueue/submit/surface_in 共享的**不只是**
`_nextSubmit` / `part->inflight`，还有 `nvmtl_pending_buffers` 和 `queue->_reservations` 两张
NSMutableArray。换锁对象 = 边枚举边被改写 = 堆破坏。所以只能按**阶段**拆锁，不能按**锁对象**拆。

## 内容

| 文件 | 说明 |
| --- | --- |
| `patch_a87_lock.py` | 10 处锚点（断言唯一命中），对 `plugin-A80-restored/src` 的末态打补丁 |
| `NVMTLEventSync.m.diff` / `NVMTLObjects.h.diff` | 打完之后的样子（相对 A-80 末态） |
| `a87-install.sh` | 原子装机（rename + ad-hoc 签名 + CS_PLATFORM_BINARY + 特征串门禁） |
| `run-a87.sh` | 一轮 25 s 拖动 + frame-times + 6 s sample，按参数切 `oollock` 开关 |

## 复现

```bash
cp -a ~/src/NvGfx-xe-2.6.2/plugin ~/xegfx-log/A-87/tree-lock
cd ~/xegfx-log/A-87 && python3 patch_a87_lock.py tree-lock/     # ALL_OK 10 edits
cp build.sh tree-lock/ && cd tree-lock && bash build.sh ./NVMTLDriver
bash ../a87-install.sh ./NVMTLDriver                            # 装机后需重启生效
bash ../run-a87.sh base off                                     # 基线
echo oollock | sudo tee /private/tmp/nvmtl-a87 >/dev/null       # 200 ms 生效，无需重启
bash ../run-a87.sh ool on
```

构建：`0 error / 481 warning`，1596560 B，UUID `85CFEEAA-…`，装机后 shasum `3017d7650cc102a8`（1627280 B）。

## 计数（自洽门）

`nvlog` 按格式串去重，**不要数日志行**。看这两条（每 2 的幂打一次）：

```
A87 out-of-lock: %llu parts claimed, %llu landed, %llu failed
A87 out-of-lock: %llu parts landed, %llu claimed, %llu failed, %u in flight
```

A-87 实测：16384 次提交、0 失败、peak in flight 3、`claimed == landed + failed + in-flight`。

## 回滚

```bash
sudo rm -f /private/tmp/nvmtl-a87          # 回 A-80 行为（秒级，不用重启）
bash ~/xegfx-log/A-81/use-a80.sh           # 插件回 A-86 件 df1b5083，之后重启整机
```
