# 声光告警与模型故障 API

## 1. 范围与接线

声光输出为核心板 J12.11 的 `CEP1_5P`，芯片球号 U7，约束为 LVCMOS33、高电平有效。LED 与有源蜂鸣器共用 `O_alarm`。健康、复位、应用正常退出和 PS 心跳超时均拉低输出。引脚定义依据厂商 `PL-GPIO连接器管脚定义表.xlsx` 的 DR1M90GEG400 列；表中连接器标题的 BANK 编号沿用 Zynq，对应 DR1M 的 BANK11。

PL 负责定时、排队、抢占和心跳保护；PS 负责物理量换算、共享配置及模型结果有效性。remote-pc 通过既有加密共享配置协议修改参数，PS 是物理告警的唯一写入方。配置优先级与覆盖规则见[无线联网与双端协同](无线联网与双端协同.md)。

独立采集 `fpga_top` 没有 PS 配置通路，U7 固定低电平；声光告警使用 `soc_hdmi` 合并码流、PL 接口 v1.3、匹配的 `pl_ctrl.ko` 和 ps-local。

## 2. 告警规则

| 来源 | 触发条件 | 输出 |
|---|---|---|
| 温度 | 已勾选告警，物理量低于下限或高于上限 | 1 次短响 |
| 电流 | 同上，单位 mA | 2 次短响 |
| 振动 | 同上，X/Y/Z 任一轴越界 | 3 次短响 |
| 转速 | 同上，单位 rpm | 4 次短响 |
| 模型检测，未分级 | 有效、非健康，`grade=0` | 连续 3 秒 |
| 模型一级 | 有效、非健康，`grade=1` | 连续 3 秒 |
| 模型二级 | 有效、非健康，`grade=2` | 连续 6 秒 |
| 模型三级 | 有效、非健康，`grade=3` | 连续 9 秒 |

短响高电平为 1 秒，相邻短响以及通道告警组之间低电平为 1 秒。模型恢复后也保留 1 秒低电平，再播放仍越界的通道。等于边界不报警。温度和振动采用配置中的 `k × raw + b`；电流已经在数据源换算为 mA，转速为 `0.1 × raw` rpm，与两端面板一致。振动合成值只用于显示，不用于阈值判断。判定使用每批最近一个样本，与面板数值一致；不承诺捕获两批之间的瞬态。

1. 通道由正常进入越界时产生一组告警；持续越界不重复播放。恢复正常或取消勾选立即取消该通道的输出及排队项，随后再次越界可重新触发。
2. 多个通道同时越界时按温度、电流、振动、转速顺序排队，每通道至多一个待播放项。
3. 模型故障立即抢占通道告警并清除通道队列。模型仍处于故障状态时，即使长响已经结束，也抑制通道告警。模型恢复健康或变为不可用后，仍越界的通道重新排队。
4. 同一连续模型故障只播放一次。等级提升重新开始完整的 3/6/9 秒长响；等级下降不延长或重启当前长响。故障类别或工单 ID 改变本身不重启声音。恢复健康或不可用立即结束模型输出。
5. 通道超过 2 秒没有有效新样本，不再作为越界依据。模型发布者超过 10 秒没有有效上报，状态变为不可用，界面显示 N/A，撤销模型告警；不可用不表示健康。
6. PS 每 250 ms 更新告警状态；PL 连续 2 秒收不到更新，清状态、队列和输出。正常退出、驱动卸载及软复位也清输出。

`告警` 复选框只控制对应传感器，不屏蔽模型故障。当前 FF6/RUL profile 只有寿命回归，没有分类或分级输出；RUL 数值不会被自动转换为故障、工单或蜂鸣器告警。

内置单主故障诊断须连续两个不同窗口出现相同具体类别与等级后才调用模型告警，
连续三个有效正常窗口解除；质量不足、推理错误、结果过期或停止诊断撤销模型告警。
四秒首窗加两秒步长意味着数据持续合格时，启动约六秒后可满足首次确认条件。
数据集评估不驱动声光。软件回归可检查模型告警等级位及正常恢复清除，
J12.11/U7 上的实际声光时序另按板端规程验收。

## 3. 模型发布接口

### 3.1 进程与传输

模型进程通过板端 Unix Stream Socket `/run/mlk-fault.sock` 上报，不直接写 PL 寄存器。Socket 由 ps-local 创建，权限仅应用运行用户可读写，默认板端为 root。环境变量 `MLK_FAULT_SOCKET` 可覆盖路径；应用与客户端须使用相同路径。该服务不是网络监听端口。

每次连接发送一行 UTF-8 JSON，末尾为 LF；包含 LF 总长不超过 1000 字节，每连接只接受一个请求。最多同时处理 8 个客户端，连接超时 2 秒。收到一行 JSON 响应后关闭连接。发布者建议每秒更新一次状态，故障持续期间保持相同事件字段。

| 字段 | 类型 | 约束 |
|---|---|---|
| `api` | integer | 固定 1 |
| `valid` | boolean | `true` 表示分类结果有效；`false` 表示模型结果不可用 |
| `class` | integer | 0 健康；1 轻微故障；2 重度故障。不可用时必须为 0 |
| `grade` | integer | 健康或不可用为 0；故障为 1/2/3；只检测而未分级时为 0，按一级时长输出 |
| `id` | string | 故障必填；1～64 个 ASCII 字母、数字、下划线或连字符；建议 UUID |
| `faultTimeUs` | string | 故障必填；Unix UTC 微秒十进制字符串，范围 1～253402300799999999；不使用相对推理时间 |
| `message` | string | 故障必填；UTF-8 不超过 512 字节；受整条请求长度限制 |

`class` 是检测类别，`grade` 独立决定声光时长，不根据类别推测等级。后续新增类别需升接口版本并同步 UI 映射。

故障事件 ID 与时间在事件首次发生时生成，心跳重报保持不变。等级或类别变化需要生成新 ID 和事件时间，形成新的不可变工单；同一 ID 携带不同事件内容会被拒绝。时间源优先网络 NTP，WiFi 与 4G 使用同一机制；离线回退规则见[无线联网与双端协同](无线联网与双端协同.md)。

### 3.2 请求和响应

二级故障请求：

```json
{"api":1,"valid":true,"class":1,"grade":2,"id":"model-episode-001","faultTimeUs":"1792843200000000","message":"模型检测到异常"}
```

健康请求：

```json
{"api":1,"valid":true,"class":0,"grade":0}
```

模型停止或当前结果无效：

```json
{"api":1,"valid":false,"class":0,"grade":0}
```

成功响应为 `{"ok":true,"error":""}`。字段无效、ID 内容冲突或工单持久化失败返回 `ok=false`。持久化失败时仍执行有效的物理告警，调用方须保留事件并使用相同 ID 重试，不能把失败响应当作工单已保存。

接受故障后，ps-local 更新故障检测、分级和故障时间；工单按板端记录密码加密保存并重试发送，remote-pc 按自己的记录密码保存后 ACK。健康/不可用只更新当前状态，不删除历史工单或最近故障时间。独立 RUL 回放继续更新寿命与窗口，不覆盖有效分类状态。

只有一个分类发布者可以拥有该接口。多个模型需先在模型进程中融合成一个有效结果；API v1 不提供多发布者竞争与融合仲裁。网络控制权限不改变本地模型接口的所有权。

### 3.3 C / C++ 与 Python

无 Qt 依赖的 C 客户端为 [`mlk_fault_client.h`](../host/common/mlk_fault_client.h) 和 [`mlk_fault_client.c`](../host/common/mlk_fault_client.c)：

```c
#include "mlk_fault_client.h"
char reply[1024];
int rc = mlk_fault_publish_json(NULL,
    "{\"api\":1,\"valid\":true,\"class\":0,\"grade\":0}",
    reply, sizeof reply, 2000);
/* rc==0 仅表示收到完整响应，仍须解析 reply 中的 ok。 */
```

与模型程序一起编译该 `.c` 文件；使用板端工具链时与模型采用相同 sysroot。传输超时参数覆盖连接、发送和接收全过程，范围 1～10000 ms；错误返回 -1 并设置 errno。该同步函数应在外部进程或工作线程调用，不能在 ps-local 的 UI 线程中同步调用自身服务。

板端已提供 `/usr/bin/mlk-fault`：

```sh
mlk-fault '{"api":1,"valid":true,"class":0,"grade":0}'
```

Python 发布与分级验证示例见 [`model_fault_example.py`](../host/ps-local/tools/model_fault_example.py)。脚本在已有 Python 3 的环境使用；rootfs 不要求安装 Python，板端模型可直接链接 C 客户端。示例保持事件心跳，退出时上报健康。

同进程 Qt 集成入口为：

```cpp
QMetaObject::invokeMethod(window, "reportModelState", Qt::QueuedConnection,
                          Q_ARG(QJsonObject, state));
```

`KLocalWindow::reportModelState(const QJsonObject &)` 与 Socket 使用相同字段和校验。直接调用应位于 UI 线程；工作线程使用排队调用。需要确认持久化成功时，在非 UI 线程使用阻塞排队调用并取得 bool 返回值，或使用 Socket 响应。原 `reportFault(id,time,class,grade,message)` 仅保存工单，不代表模型当前健康状态，也不驱动声音。

### 3.4 模型诊断 API 2 与结构化工单

单主故障模型使用 API 2（schema `mlk.fault.result.v2`），与 API v1 共用 `/run/mlk-fault.sock`，但校验器、长度上限与确认规则独立。api=2 的每行请求上限为 4096 字节，api=1 与采集接口保持 1000 字节；同一服务实例按请求内的 `api` 分派。

| 字段 | 类型 / 上限 | 要求 |
|---|---|---|
| `api` / `schema` | int / string | 固定 2 / `mlk.fault.result.v2` |
| `task` | string | 固定 `bench_fault_single_v1` |
| `model` / `preprocess` | string，各 ≤64 ASCII 字节 | 运行包身份 / 冻结预处理版本 |
| `run` | UUID string | 本次模型运行身份 |
| `seq` | uint32 | 新窗口严格递增；尚无窗口为 0 |
| `startUs` / `endUs` | decimal string | 窗口 UTC 微秒；有窗口时差 4000000 µs |
| `state` | string | `stopped/loading/waiting/running/data_insufficient/stale/error/stopping` |
| `valid` | bool | 仅当前质量合格且未过期结果为 true |
| `classIndex` / `fault` | int / string | 0 正常、1 过载、2 欠压、3 过热、4 电机倾斜；无效 -1 / `unavailable` |
| `grade` | int | 正常 0，异常 1..3，无效 -1 |
| `confidence` | number 或 null | 有效为 0..1；无效必须 null |
| `backend` | string | `cpu` / `cpu_cnn_npu`，加载前可为 `unknown` |
| `preprocessUs` / `inferUs` | uint32 | 分开计时，未知为 0 |
| `reason` | string，≤64 ASCII 字节 | 有效为空；无效为有界原因码 |
| `quality` | object，可选 | 本地诊断详情，不作为模型物理输入 |

响应为 `{"ok":true,"error":"","acceptedSeq":N}`；拒绝返回 `ok=false` 与 `unsupported_api`、`invalid_result`、`identity_mismatch`、`publisher_busy`、`stale_result`、`window_conflict` 之一。响应只表示原始状态已接收，不表示故障已确认或工单已落盘。

所有权与时效：内置实时任务运行时拒绝其他分类发布者；同一 `run` 同一 `seq` 的分类、等级与窗口时间不可改变，改变返回 `window_conflict`；不同 run 的晚到结果与倒序窗口拒绝。有效结果的最大年龄为 6000 ms，同时约束窗口结束时间与本地单调时钟；无效、过期、停止立即撤销当前模型告警，“N/A”不等于正常。旧十秒发布者超时只处理 API v1。

确认与工单：故障/恢复确认由上位机控制器执行，不更改原始 argmax；异常分数至少 0.80 且相同类别与等级连续两窗确认，恢复正常连续三窗。低于 0.80 仅作为候选显示，清除模型告警和累计确认，不生成工单，原始状态与日志保持；该试用门槛待实采标定，可能漏掉低分真故障，也不能排除高分误判。确认后生成 `mlk.fault.order.v2` 工单，`diagnosis` 固定快照记录类别、运行身份与窗口，`grade` 取所选等级，`class` 按 `grade=1→1、2/3→2` 兼容映射，`message` 为可读类型与等级。首次确认时冻结快照，同 ID 内容不可变；类别或等级确认变更生成新 ID。声光直接使用确认后的等级：无效/低置信度为 0，正常确认恢复后为 0，确认异常为 1/2/3，保留既有 3/6/9 秒与抢占规则；工单保存失败与物理告警有效分别展示。传感器独立阈值告警保持原规则，不受模型置信度影响。

同一 run/seq 的相同结果可重复接收，但不累计为新的确认窗口。数据集评估只统计冻结特征
的预测与真值，不调用本 API，不生成声光告警、故障工单、记录标签或 UDP 故障状态；
与实时诊断及寿命回放互斥，须等待旧上下文释放后才加载新引擎。

## 4. PL 与 PS 底层接口

PL ABI v1.3，`ID=0x504C0103`，基址仍为 `0x80000000`，新增：

| 偏移 | 名称 | 访问 | 定义 |
|---|---|---|---|
| `0xD4` | ALARM_STATE | RW | [0] 温度越界、[1] 电流、[2] 振动、[3] 转速；[5:4] 模型有效告警等级 0～3；其余位保留为 0 |
| `0xD8` | ALARM_STATUS | RO | [0] U7 驱动电平；[1] 正在播放；[2] 模型抑制状态；[3] 心跳超时；[11:8] 通道待播放位图；[15:12] 当前通道位图；其余为 0 |

ALARM_STATE 的 WSTRB[0] 为 1 时锁存低 6 位并续期 2 秒；相同状态写入只续期，不重新触发。WSTRB[0] 为 0 时不改变状态且不续期。硬件复位和 CTRL.RST 清状态，其他采集偏移、DMA 记录和 NPU ABI 不变。合并码流告警计数时钟为 100 MHz；SoC 寄存器演示工程为 50 MHz，由 `AXI_CLK_FREQ_HZ` 明确传入。

用户态封装为 `pl_set_alarm(fd,state)`、`pl_read_alarm(fd,&state,&status)`，见 [`pl_host.h`](../host/ps-local/src/pl_host.h)。新版驱动和应用兼容 v1.2 的采集，但告警调用要求 v1.3，否则返回 ENOTSUP。旧驱动不识别 v1.3，必须与码流一起更新。模型发布者不应另行调用这些函数，以免与 ps-local 争用硬件状态。

双端同步沿用加密 UDP v4 的 BoardStatus（type=7），新增 `modelFault` 对象，包含 `api/valid/class/grade` 四项摘要；`faultTimeUs` 保留原字段；`alarm` 为板端硬件告警可用性文本。工单仍走 type=5/6。网络断开超过 5 秒后 remote-pc 将当前模型状态显示为 N/A，历史故障时间保留。

## 5. 验证规程

1. 构建合并码流，检查 U7 引脚约束和非负 setup/hold slack；构建 ps-local、pl_ctrl、mlk-fault，更新板端与 rootfs。构建及加载入口见[PS-PL 开发流程](PS-PL开发流程.md)。
2. 打开 ps-local 或具备配置权限的 remote-pc，在“通道标定与告警”启用单一通道，设置当前读数位于范围外；观察对应短响次数和 1 秒高电平。恢复范围后再次越界，确认可重新触发；取消勾选后不再报警。
3. 振动分别使 X/Y/Z 任一轴越界，转速验证负值和上下限；采集不可用超过 2 秒后不使用旧数值维持告警。
4. 通过模型 API 保持一级、二级、三级心跳，分别验证连续 3/6/9 秒；相同状态不重复。通道正在短响时提交模型故障，确认立即变为连续高电平。模型恢复健康后，仍越界的通道重新播放。
5. 发送健康和不可用，检查当前声音取消；停止模型心跳超过 10 秒，检查两端分类显示 N/A；停止 ps-local 后检查 U7 低电平。异常终止应用时，PL 最迟 2 秒拉低。
6. 检查板端工单、remote-pc 加密保存与 ACK；同 ID 重报不新增工单，同 ID 改级被拒绝，新 ID 改级产生新工单。恢复实际配置并归档测试工单。

自检入口为 `fpga/uisrc/02_sim/run_sim.sh`、`host/tests/windows.pro`。板端寄存器读回只能证明输出逻辑的状态与时序；LED 亮灭、蜂鸣器实际响声和负载电平需现场观察或仪器测量。

## 6. 验证范围

本地验证覆盖告警状态机自检、AXI 寄存器读写、完整采集 RTL 回归、Qt 双端加密状态同步、模型 Socket/C 客户端、工单加密保存和超时解除。U7 负载电平、LED/蜂鸣器实际动作、板端时长和断电重启后的验收已在板端完成。可复核结果见 [`host/tests/results/alarm/summary.json`](../host/tests/results/alarm/summary.json)。

TD 合并码流已生成，SWNS 为 0.004 ns、HWNS 为 0.000 ns，STA coverage 为 99.90%；no_clock、invalid_constraint、combinational_loop 均为 0。HDMI PLL 有 3 项约束频率与 IP 设置不匹配的检查项，分别涉及 148.456057/148.500000 MHz、742.400024/742.500000 MHz、148.500000/148.484848 MHz；详见验证记录的 `check_timing.rpt`。该结果用于功能候选，不满足生产冻结要求。U7 输出的 10 ns datapath-only 约束有效，未被忽略或覆盖。
