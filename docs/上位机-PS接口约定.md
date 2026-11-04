# 上位机 ⇄ PS 接口约定（MLK-F3P-DR1M90G）

> 上位机（PC 端 `host/remote-pc`、PS 端本地 `host/ps-local`）与板卡 PS 应用之间的接口契约：网络（以太网 / WiFi / 4G）与串口（UART&JTAG 二合一 Type-C）的帧、命令、加密、时间与延迟。
> PL ⇄ PS 见 [`PL-PS接口约定.md`](PL-PS接口约定.md)；NPU 见 [`NPU-AI接口约定.md`](NPU-AI接口约定.md)；上位机功能见 [`../host/上位机需求与设计.md`](../host/上位机需求与设计.md)。

| 项 | 值 |
|---|---|
| 接口版本 | 应用 UDP v2 / v3 / v4 / v5 / v6；安全封装 v2 |
| 冻结日期 | TBD |
| 上位机实现位置 | `host/remote-pc/`（PC）、`host/ps-local/`（板卡 PS） |
| PS 实现位置 | `host/ps-local/`，同一 Qt 进程内提供 UDP 服务 |

---

## 1. 范围与职责

- 范围：上位机 ⇄ PS 应用的传输层、数据帧、控制命令、加密、时间与延迟定义。
- 不含：PL ⇄ PS 的寄存器 / 中断 / DMA（见 [`PL-PS接口约定.md`](PL-PS接口约定.md)）、上层 UI。
- 单一来源：本文件是上位机与 PS 之间接口的唯一权威定义，两端按此实现。

---

## 2. 链路

| 链路 | 传输 | 用途 |
|---|---|---|
| 以太网（PS eth1） / WiFi | UDP（采集、推理、共享配置、工单）；TCP 控制为预留 | 本地网络联调 |
| 4G | 同以太网 | 远程 / 冗余 |
| UART | 串口帧（见 §5） | 无网接入 |
| JTAG | 仅 PL 配置 / 调试 | 非数据面 |

---

## 寿命模型应用帧

寿命模型使用 UDP v3 结果 / 状态帧（type=1）与回放控制帧（type=3），复用既有安全封装。逐字段定义、float32 RUL 单位、有效标志和命令确认见 [接入规程](寿命模型-上位机接入.md) §5。下文 v2 分类与四值注入不适用于当前寿命模型。

## 3. 网络数据帧（UDP，实时流）

小端。[frame.h](../host/remote-pc/src/frame.h) 解析 v2 采集 / 分类、v3 寿命回放、v4 协同 / 兼容采集与 v5 / v6 首样本时间采集帧。下表定义 v2，v4 扩展见 §3.3。

| 偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| 0 | 2 | `magic` | `0x4B4D`（`'M'` `'K'`，小端） |
| 2 | 1 | `ver` | 版本，当前 `2` |
| 3 | 1 | `type` | `0` = 波形流，`1` = 诊断结果，`2` = 模拟注入（上位机 → PS） |
| 4 | 4 | `seq` | 包序号（u32，小端） |
| 8 | 4 | `cnt` | 设备计数（对应 `DATA.CNT`） |
| 12 | 2 | `rate_hz` | 标称采样率 |
| 14 | 2 | `n` | 采样点数（`type=0`） |
| 16 | 8 | `ts_us` | v2 设备时间，微秒；旧单调时基不得直接与 PC UTC 相减。v4 / v5 / v6 规则见 §3.3、§7 |
| 24 | 1 | `ch` | 通道 `0`温度 / `1`电流 / `2`振动 / `3`转速（`type=0`） |
| 25 | 3 | 保留 | 写 0 |
| 28 | 2·n | `samples` | `int16` 采样，小端（`type=0`） |
| 24 | 12 | 诊断载荷 | `cls:u8` `grade:u8` `conf:u16` `rul:u32` `infer_us:u32`（`type=1`） |
| 28 | 8 | `setpoint[4]` | `int16`，四通道注入值（`type=2`） |
| 36 | 2 | `duration_ms` | 注入时长（`type=2`） |
| 38 | 2 | 保留 | 写 0 |

- `ch` 与物理量映射固定为四类；标定 k/b 见上位机配置。
- 大帧分片：`TBD`。

## 3.1 诊断帧（`type=1`）

| 偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| 24 | 1 | `cls` | `0` 正常 / `1` 轻微故障 / `2` 重度故障 |
| 25 | 1 | `grade` | 故障分级 |
| 26 | 2 | `conf` | 置信度 ×10（`900` = 90.0%） |
| 28 | 4 | `rul` | 剩余寿命（单位 `TBD`） |
| 32 | 4 | `infer_us` | NPU 推理耗时，微秒 |

## 3.2 注入帧（`type=2`，上位机 → PS）

| 偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| 28 | 8 | `setpoint[4]` | `int16`，四通道注入值（温度 / 电流 / 振动 / 转速，顺序同 §3 `ch`） |
| 36 | 2 | `duration_ms` | 注入时长 |
| 38 | 2 | 保留 | 写 0 |

PS 侧处理（采集与训练分离，用于演示与联调，避免损坏设备）：

1. 收到 `type=2` → 暂停采集路径（PL → PS → NPU），记录注入起点；
2. 按各通道标定把 `setpoint` 还原为物理量，构造 NPU 输入 → 推理；
3. 结果以 `type=1` 诊断帧回传上位机（`cls/grade/conf/rul/infer_us`）；
4. `duration_ms` 到点后恢复采集 → 推理链路；注入期间丢弃的采集数据不补传。

- 注入是临时路径切换，不改变上位机对正常 `type=0/1` 帧的解析。
- NPU 输入接口见 [`NPU-AI接口约定.md`](NPU-AI接口约定.md)。

---

## 3.3 UDP v4 协同与 v6 采集

v4 / v5 / v6 与 v2 共用 28 字节头。协同对象为 `ver=4`；发送波形使用 `ver=6`，接收兼容 v1 / v2 / v4 / v5 波形。v5 仅允许 type=0；v6 允许 type=0 与 type=8。应用数据报最多 1200 字节，波形每包最多 128 点；安全封装另占 44 字节，不依赖 IP 分片。

| type | 方向 | 头部 / 载荷 |
|---|---|---|
| `0`（v6；兼容 v4 / v5） | PS → PC | `ch` 为 0..3；偏移 25 为 `axes`（1 或 3），26..27 保留；从偏移 28 起为 n 个 X / 单通道值，三轴时再接 n 个 Y 与 n 个 Z，v6 电流与转速为小端 int32，温度与振动为小端 int16；v4 / v5 均为小端 int16 |
| `4` | 双向 | 偏移 24..27 保留为 0；偏移 28 起为 UTF-8 JSON 配置对象 |
| `5` | PS → PC | UTF-8 JSON 故障工单（v1 或 v2，见下） |
| `6` | PC → PS | UTF-8 JSON 工单确认 |
| `7` | PS → PC | UTF-8 JSON 板端资源与状态 |
| `8`（v6） | PS → PC | 28 字节头 + UTF-8 JSON 完整故障状态（`mlk.fault.status.v1`），整帧 ≤1200 字节 |

波形通道为温度 signed16 ×0.1 ℃、电流 signed32 mA、振动三轴 signed16 ×0.0039 g、转速 signed32 ×0.1 rpm。三轴只允许 `ch=2`，按 X、Y、Z 分块排列；`n` 为每轴点数。每通道 `seq` 独立递增。v5 / v6 时间戳为 PS 估算的首样本 UTC epoch 微秒；读取完成 UTC 减去 `(n-1)/rate_hz`，分包偏移 i 后加 `floor(i×1000000/rate_hz)`，两端共用该时间。v4 兼容帧时间戳按发送侧 epoch 估算首样本，v1 / v2 单调时基使用接收时间回退；兼容时间精度受限。PL 原始时基不能直接作为 Unix 时间使用。网络发送与本地显示使用同一有效采样源，不发送演示回退值。转速在 PS 应用入口按安装方向对 PL 符号取反，PC 沿用收到的方向；signed32 缓冲不施加 ±3276.7 rpm 限制，实际范围由 PL signed32、0.001 rpm/LSB 数据源决定。两端需配套升级；旧应用不支持 v6。

### 配置对象（type=4）

| 字段 | 类型 | 约束 |
|---|---|---|
| `op` | string | `get` 查询；`set` 提交；`snapshot` 板端响应 |
| `request` | string，可选 | set 请求 ID，确认返回原 ID；PC 固定携带，同一提交重试复用 ID；省略时不缓存结果 |
| `revision` | uint32 JSON number | 当前板端版本，初始 1；共享值成功修改后递增。remote-pc 优先时 set 不要求与板端一致：板端以远端快照覆盖并递增版本 |
| `owner` | string | `ps-local` 或 `remote-pc`；仅板端配置界面修改 |
| `channels` | object[4] | 每项 `k:number`、`b:number`、`lo:number`、`hi:number`、`enabled:bool` |
| `currentCalibration` | object | `zero`：−32768..32767 的整数 LSB；`gainUaPerLsb`：0..1000000 的整数 µA/LSB；0 表示未标定；默认 20000 / 1250（ACS712-20A）。兼容对象缺失时保留当前值 |
| `error` | string | 响应错误说明；空字符串表示无错误 |

`k` 在 0..1e6，`b`、`lo`、`hi` 在 ±1e9，全部要求有限数；使能告警时 `lo <= hi`。get 无需携带其他字段。set 需完整共享快照；板端仅在加密传输且 remote-pc 控制权时提交，ps-local 优先时拒绝。remote-pc 优先期间板端本地修改可被远端下一次快照覆盖；`currentCalibration` 例外，由板端采样与「归零」维护，PC 只读并在每次快照中采用板端值。拒绝时返回当前快照。两端配置密码、存储口令、密钥、界面刷新率与目录不属于共享对象。

remote-pc 每 2 秒查询快照。板端优先时 PC 采用板端值；remote-pc 优先时 PC 以本机保存的共享值为准，发现差异即提交，板端离线期间的本机修改在连接恢复后自动覆盖板端。首次取得板端快照时先采用板端值，之后才按上述规则提交。未获确认的 set 使用同一 ID 重试，最多 4 次；板端缓存最近 64 个请求的结果，重复请求不重复提交。

### 工单对象（type=5 / 6）

| 字段 | 类型 | type=5 约束 |
|---|---|---|
| `id` | string | 1–64 个 ASCII 字母、数字、下划线、连字符 |
| `faultTimeUs` | decimal string | Unix UTC epoch 微秒，正整数，最大 253402300799999999 |
| `class` | integer | 1 轻微、2 重度 |
| `grade` | integer | 0..255 |
| `message` | string | UTF-8 最多 512 字节 |

type=6 仅含 `id:string` 与 `status:"stored"`。工单与 ACK 只接受加密传输；PC 原子落盘成功后才能确认。同 ID 同内容可重复确认，不同内容拒绝。板端 API、加密文件格式和断线重发见 [无线联网与双端协同](无线联网与双端协同.md) §4。

### 工单对象 v2（结构化工单）

带 `schema:"mlk.fault.order.v2"` 的工单保留上述顶层字段，并追加结构化快照：

| 字段 | 类型 | 约束 |
|---|---|---|
| `schema` | string | 固定 `mlk.fault.order.v2` |
| `diagnosis` | object | 固定快照：`classIndex`、`fault`、`model`、`preprocess`、`run`、`seq`、`startUs`、`endUs`、`confidence` |
| `class` | integer | 兼容粗分类：`grade=1` 为 1，`grade=2/3` 为 2；新端不据此推断类型 |
| `grade` | integer | 所选异常等级 1..3 |
| `message` | string | 可读具体类型与等级，UTF-8 最多 128 字节；不作为机器类型来源 |
| `faultTimeUs` | decimal string | 等于 `diagnosis.endUs`，即首次确认序列首窗结束 |

`diagnosis.classIndex` 限定 1..4，`fault` 必须与类别一一对应；`run` 为 UUID，`seq`≥1，`endUs-startUs` 为 4000000 µs，`confidence` 为 0..1 的有限数。整单 JSON ≤1000 字节。无 `schema` 的旧工单按 v1 读取；接收端保存完整对象，不丢弃未知字段。首次确认时冻结快照，置信度与后续窗口变化不重写同 ID；类别或等级确认变更生成新 ID，心跳不新增工单。

### 板端状态（type=7）

| 字段 | 类型 / 单位 |
|---|---|
| `cpu`、`memory`、`disk` | number，百分比 |
| `cores` | number[]，各核百分比 |
| `memoryUsedKb`、`memoryTotalKb` | number，KiB |
| `net`、`netUp` | string 接口名、bool 链路状态 |
| `rxKbps`、`txKbps` | number，KB/s |
| `gpu`、`features` | string，板端 GPU / PL 特征摘要 |
| `npu` | object，可选，NPU 状态扩展 v1，字段见下表 |
| `network` | object，可选，核心板网络状态扩展 v1，字段见下表 |
| `dmaBlocks`、`dmaLost`、`dmaAge` | string，板端 DMA 统计 |
| `readCycle`、`readTime` | string，板端读取周期与耗时 |
| `time` | string，板端本地时间文本 |
| `faultTimeUs` | decimal string，最近故障事件的 epoch 微秒，0 表示无事件 |

remote-pc 侧栏显示这些板端指标；5 秒未收到有效数据时显示不可用。左上「展开」右侧连接状态区分未监听、等待配对、等待板卡、握手、明文 / 加密连接与超时，仅以所配目标的有效回包更新。

`npu` 为追加的兼容字段，不改变 UDP v4 type=7 的帧格式；旧接收端忽略未知字段，新接收端在缺失或非法时显示不可用并清除曲线。

| `npu` 字段 | 类型 / 约束 |
|---|---|
| `version` | integer，固定 1 |
| `hardReady`、`softReady`、`cmaReady` | bool，对应板端三个设备是否为字符设备 |
| `hardIrqAvailable`、`softIrqAvailable` | bool，对应设备就绪且中断统计可读 |
| `hardInterrupts`、`softInterrupts` | decimal string，0..18446744073709551615；各 CPU 合计的启动以来累计中断数 |
| `hardInterruptsPerSec`、`softInterruptsPerSec` | 非负 number，次 / 秒；无相邻有效样本时为 null，不能用 0 代替不可用 |

硬核与软核速率分别取相邻有效计数差除以板端实际采样间隔。该对象不提供百分比使用率或推理成功标志。FF6 使用硬核，软核为 0 属于正常情况。两端以同一组件显示标题、状态、禁用的使用率条和最近 60 个有效样本的硬核 / 软核活动曲线，曲线单位为次 / 秒。

| `network` 字段 | 类型 / 约束 |
|---|---|
| `version` | integer，固定 1 |
| `kind` | string：`disconnected` 未连接、`wifi` WiFi、`cellular` 4G、`ethernet` 千兆网、`other` 其他接口 |
| `interface` | string，所选板端接口名；未连接时为空 |

网络状态由板端每秒查询：接口须已启动、链路运行并具有非链路本地 IPv4 地址；优先选择有效默认路由中 metric 最低的已连接接口。无默认路由时，按 千兆网、4G、WiFi、其他接口顺序选择可用于局域网通信的接口。状态描述链路与地址，不代表互联网可达。WiFi 由无线接口标志或接口名识别；4G 由 `wwan` / `ppp` / `usb` / `rmnet` 接口名或 GobiNet / QMI / MBIM 驱动识别。

两端顶栏连接摘要采用上下两行：上行显示核心板网络状态，下行显示应用连接状态，字体与 CPU / 内存摘要一致。remote-pc 使用 `network`，不读取 PC 本机网络作为板端状态；字段缺失或非法时显示不可用，type=7 超时 5 秒后清除 NPU 曲线并将网络状态置为不可用。`net` / `netUp` 对应所选板端接口；RX/TX 仍为非回环接口的合计速率。

### 完整故障状态（v6 / type=8）

完整故障状态单独发包，不挤占 type=7 的 1200 字节预算；type=7 内的 `modelFault` 继续承载 API v1 粗分类摘要，仅兼容旧端。

| 头偏移 | 字段 | 取值 |
|---:|---|---|
| 0 / 2 / 3 | `magic` uint16 / `version` uint8 / `type` uint8 | `0x4B4D` / 6 / 8，小端 |
| 4 | `seq` uint32 | 每个状态包的传输序号，随心跳增加；不是模型窗口序号 |
| 8 / 12 / 14 | `cnt` uint32 / `rate` uint16 / `n` uint16 | 0 |
| 16 | `tsUs` uint64 | 最后已处理窗口的 `endUs`；无窗口为 0，心跳不改写为当前墙钟 |
| 24 | `reserved[4]` | 0 |
| 28 | JSON | `mlk.fault.status.v1`，整帧 ≤1200 字节 |

载荷字段为 `schema`、`task`、`model`、`preprocess`、`run`、`seq`、`startUs`、`endUs`、`state`、`valid`、`classIndex`、`fault`、`grade`、`confidence`、`backend`、`preprocessUs`、`inferUs`、`reason`、`ageMs`、`alarmConfirmed`；取值与校验规则同模型诊断 API 2（[模型接口需求](../host/故障诊断模型接口需求.md) §4.1）。`ageMs` 为板端结果年龄，无结果为 null；`alarmConfirmed` 表示确认后的告警状态。

接收端以 `run` + `seq` 判定窗口身份：新 run 或更大 `seq` 才更新显示，同窗口重发只续链路活跃时间。5 秒未收到状态或结果年龄超过 6 秒显示 N/A，不显示为正常；停止、错误、过期状态立即使同窗口结果失效。完整故障状态只由本消息控制，RUL v3 帧与 type=7 的粗摘要不覆盖具体故障类型。

## 4. 控制通道（TCP，命令 / 响应，预留）

请求：`cmd(u16) + len(u16) + payload`；响应：`cmd(u16) + status(u16) + len(u16) + payload`。除 `TIME_SYNC` 外，`cmd` 取值仍为 `TBD`。

| `cmd` | 名 | 说明 |
|---|---|---|
| `TBD` | `GET_ID` | 设备 ID / 版本 |
| `TBD` | `GET_STATUS` | 采集状态、速率、缓冲水位 |
| `TBD` | `SET_CONFIG` / `GET_CONFIG` | 写 / 读参数（见上位机设计 §6） |
| `TBD` | `START` / `STOP` | 采集启停 |
| `TBD` | `SUBSCRIBE` | 指定 UDP 数据流目标 |
| `TBD` | `LIST_FAULT` / `GET_FAULT` | 故障记录列表 / 下载 |
| `TBD` | `GET_NPU` | NPU 诊断结果查询 |
| `TBD` | `OTA` | 固件升级 |
| `0x0014` | `TIME_SYNC` | 网络时间同步（载荷见下；PS 端处理预留） |

`TIME_SYNC`（`0x0014`，PC → PS）请求载荷固定 10 字节：

| 偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| 0 | 1 | `ver` | 版本，`1` |
| 1 | 3 | `reserved` | 写 0 |
| 4 | 4 | `epoch_seconds` | 主机 UTC epoch，`i32`，小端 |
| 8 | 2 | `uncertainty_ms` | 主机时间不确定度，`u16`，小端 |

响应 `status = 0` 表示已应用；`status = 0x0005`（`NOT_SUPPORTED`）表示未支持。当前 PS 端未实现该命令，按预留返回 `NOT_SUPPORTED`。帧编解码与偏移计算见 [`../host/common/timesync.h`](../host/common/timesync.h)；PS 端收到帧后如何设置时钟不在本契约内。

---

## 5. 串口帧（UART）

- 物理：UART&JTAG 二合一 Type-C 的 UART（CH342K），主机侧 `/dev/ttyACM*`，`115200 8N1`（`TBD`）。
- 帧：帧界 + 长度 + 载荷（同 §3 数据帧或 §4 命令）+ CRC；转义方式 `TBD`。
- 用途：无网时数据 / 命令接入；JTAG 不在此通道。

---

## 6. 加密

实现见 [`../host/common/README.md`](../host/common/README.md)：帧之外再套一层安全数据报（`magic 0x4D53`）。

| 面 | 方案 | 状态 |
|---|---|---|
| 传输 | 应用层 AES-256-GCM；**ECDH(X25519) 为主、PSK 为后备** | 已实现（PC 与 ps-local 共用 `host/common`） |
| 握手 | ECDH：Hello / Reply + HMAC 认证；PSK：无握手 | 已实现 |
| 抗重放 | 按发送端 session 维护 64 包乱序窗口 | 已实现；不跨接收进程重启持久化 |
| 存储 | AES-256-GCM + PBKDF2（`.mlkf`、`.mlkr`、`.mlko`） | 两端独立口令与目录 |
| 密钥管理 | 串口带外配对 + 指纹核对（ECDH）；PSK hex 串口抄写 | 手工 |

安全数据报（与应用帧独立）：

| 偏移 | 大小 | 字段 |
|---|---|---|
| 0 | 2 | `magic` `0x4D53`（小端） |
| 2 | 1 | `ver` `2` |
| 3 | 1 | `kind` `1` Hello · `2` Reply · `3` Secure |
| 4 | — | 载荷：Hello=`eph_pub32+nonce16`；Reply=`eph_pub32+nonce16+tag32`；Secure=`session16+seq8+tag16+ct` |

- 上层应用帧（§3）原样放进 `kind=3` 的密文；接收端解出后再 `parseFrame`。
- 模式 `off | psk | ecdh`；ECDH 由 PC 监听后主动发 Hello，PS 回 Reply。
- ECDH 根密钥为 `HKDF(ECDH(eph)‖ECDH(身份), nonce_c‖nonce_s, "mlk-ecdh-v1")`；PSK 根密钥为 `HKDF(PSK, 空盐, "mlk-psk-v1")`。
- 每次配置通道生成随机 16 字节发送 session，方向密钥为 `HKDF(root, session, "mlk-direction-v2")`，`nonce = seq(小端8B)‖0⁴`。AAD 为完整安全头 `magic+ver+kind+session+seq`。两端及重启后的发送流使用不同 session / 密钥域。
- 接收端拒绝本端发送 session，认证成功后才更新防重放窗口；最多接受 64 个发送 session。安全封装 v1 不兼容，两端需同步更新。防重放状态不跨接收进程重启保存；需要跨重启的新鲜性保证时使用 ECDH 会话。
- 仅接收所配目标 IP / 源端口；切换 WiFi / 以太网或目标地址后需更新两端配置。

### 6.1 已配对设备自动发现

发现使用独立 UDP 8001，不改变应用帧与安全封装版本。两端需预先配置匹配的 PSK 或固定身份公钥；关闭传输加密时不启用发现。流程为探测请求、挑战响应校验、更新目标 IP / 数据端口、原协议握手与应用帧确认。

| 偏移 | 长度 | 字段 | 约束 |
|---|---|---|---|
| 0 | 4 | magic | ASCII `MLKD` |
| 4 | 1 | ver | `1` |
| 5 | 1 | kind | `1` 请求、`2` 响应 |
| 6 | 1 | role | `1` ps-local、`2` remote-pc；只接受相反角色 |
| 7 | 1 | mode | `1` PSK、`2` ECDH，须与本机相同 |
| 8 | 2 | dataPort | 大端，非零应用监听端口 |
| 10 | 16 | challenge | 发送方随机挑战；响应原样返回 |
| 26 | 4 | address | 大端 IPv4，须与数据报源 IP 一致 |
| 30 | 32 | tag | 对前 30 字节计算 HMAC-SHA256 |

PSK 发现密钥为 `HKDF(PSK, 空盐, "mlk-discovery-psk-v1")`。ECDH 发现密钥为 `HKDF(ECDH(本机身份私钥, 对端公钥), 按字节排序的两个身份公钥串联, "mlk-discovery-ecdh-v1")`。发现密钥与数据会话密钥域分离；发现过程不传输私钥，也不替代应用握手。

请求只触发同长度响应，不改变目标。响应必须匹配尚未消费的挑战及已探测地址，且在 5 秒内返回；校验成功后消费挑战，重复响应不再采用。当前只在启用的 WiFi / 以太网私有 IPv4 `/24` 至 `/30` 子网内探测，每 10 秒最多 1024 个地址，每 100 ms 发送最多 16 个请求；收到有效应用数据时暂停探测。PC 以主动单播请求接收回复，适配只允许既有 UDP 会话回包的防火墙。

自动发现与启动自动连接均为本机配置，不属于共享参数。4G 跨网、较大子网及 IPv6 使用手动可达端点；不提供公网广播或运营商 NAT 穿透。操作步骤见 [连接与密钥配置](上位机连接与密钥配置.md)。

---

## 7. 时间与延迟

- 当前 PS 采集上报、v3 推理和故障时间使用 Unix UTC epoch；PL 原始计数器为独立时基。
- remote-pc「应用往返」使用本机单调时钟：发送带唯一 `request` 的配置 `get` 查询后，收到匹配且字段有效的 `snapshot` 回复时取耗时。查询周期 2 s，同一时刻保留一笔探测，5 s 后过期；无匹配样本或最近样本过期时显示不可用。数值包含网络往返、板端处理和两端应用排队，不依赖两端 UTC 对齐，也不等同于纯网络 RTT 或单向时延。查询只读取配置，不修改板端参数。
- remote-pc「数据时间差」为电脑处理波形时的 UTC 减波形时间戳，使用指数平滑，最近 5 s 无有效时间戳时显示不可用；负值保留以暴露时钟偏差。v5 / v6 时间戳是包内首样本 UTC，因此该指标包含时钟差、采样攒块、发送及接收排队；v4 时间戳为发送 UTC。v1 / v2 旧单调时基不参与该指标。不得将此数值作为单向网络传输耗时；跨设备采集到显示时延还需要时基对齐和阶段打点。
- 板端网络校时优先：`S49mlktime` 启动 Chrony NTP 客户端，经当前默认路由访问 `/etc/chrony.conf` 的时间源，不绑定 WiFi 或 4G 接口。`mlk_time_status` 要求已选中网络源、最近有效采样不超过 300 s、剩余时钟修正小于 1 s。`/var/run/mlk-time-source` 表示 `network`、`serial` 或失去新鲜网络源后的 `holdover`；无标记表示尚未完成校时。
- 板端应用每 10 s 运行 `mlk_time_status`，链路刚连上时立即检测；网络时间源从不可用变为可用后执行 `chronyc -h /var/run/chrony/chronyd.sock makestep` 立即步进。顶栏「时间同步」手动触发同一检测；网络源不可用时先 `waitsync` 等待，成功后再步进。`mlk_time_status` 可用期间，配置中的手动设置时间禁用并显示原因；不可用时可通过 `mlk_set_time`（旧镜像退回 `date -u`）写入并回读核对。
- 串口周期对时为回退：主机执行 `tools/sync_board_time.sh --interval 30`，经 `/dev/ttyACM0` 调用板端 `mlk_set_time`。网络校时健康时保留网络时间；其他情况写入并校验主机 UTC，不依赖网络与 RTC。常驻运行用 `tools/install_board_timesync.sh install` 装成 systemd 用户服务（`mlk-board-timesync.service`，`Restart=always`），板卡重启或串口重新枚举后重新登录；`remove`、`status` 分别卸载与查看。旧镜像无 `mlk_set_time` 时兼容直接串口写时。
- 网络侧 `TIME_SYNC`（§4）的 10 字节帧格式与 [`../host/common/timesync.h`](../host/common/timesync.h) 已固定；PS 端处理仍为预留，当前返回 `NOT_SUPPORTED`。`timeSyncOffsetMs()` 只计算偏移，不在主机或板端改系统时钟。主机不得假定设备时钟已与主机对齐。
- 板端无 RTC，启动由 `/etc/init.d/S21setclock` 播种：优先恢复 `/var/lib/mlk/lasttime`，否则取应用、`plctl`、模型权重中最新的修改时间作为镜像构建时间基线；停机时回写。种子不代表准确当前时间，由网络校时或串口回退纠正。网络校时每分钟持久化一次，串口回退成功后也保存时间种子。
- 时区：板端不装 tzdata，系统时钟保持 UTC，显示按固定偏移 `CST-8`（UTC+8，无夏令时）。`/etc/TZ` 供 busybox 命令使用，`/etc/profile.d/10-timezone.sh` 供登录 shell，`/opt/qt_play/run_arm_app.sh` 给看板进程导出 `TZ`。
- 丢包按 `seq` 缺口统计；采集延迟、AI 耗时为量化报告必填项。

---

## 8. 版本与变更规则

- 与 [`PL-PS接口约定.md`](PL-PS接口约定.md) §12 一致：冻结后只准「加」；改已有语义为破坏性改动，升版本 + 双方确认 + 两端同步实现。

---

## 9. 未定（TBD）

- TCP 预留命令码与载荷格式；串口帧界与 CRC；网络侧 `TIME_SYNC` 的 PS 端处理；蜂窝公网 NAT / 中继接入。

## BoardStatus 的模型状态扩展

UDP v4 type=7 的 `modelFault` 对象包含 `api=1`、boolean `valid`、integer `class`、integer `grade`。健康为 `valid=true,class=0,grade=0`；不可用为 `valid=false,class=0,grade=0`；故障类别 1/2、等级 1/2/3，未分级为 0。该对象与独立 v3 RUL 帧并存，不从 RUL 数值推断分类。最近故障 UTC 微秒仍使用顶层字符串 `faultTimeUs`，不可变工单与 ACK 仍走 type=5/6。模型结果失效或断链时显示 N/A，历史故障时间保留。完整语义见[声光告警与模型故障 API](声光告警与模型故障API.md)。

## 采集存储与故障回看

两端本地记录使用 MLKC 外层 v1 / 采集载荷 v2（兼容 v1），标签使用 MLKL v1，格式与 CSV 解密命令见 [采集记录、回放与训练接口](采集记录-回放与训练接口.md)。type=5 只传输故障工单，不包含完整采集文件。PC 保存工单及事件标签后才发送 stored 确认；故障清单按时间关联 PC 记录，缺失时明确显示无关联采集记录。
