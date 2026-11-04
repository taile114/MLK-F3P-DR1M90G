# PS 本地应用接口约定（MLK-F3P-DR1M90G）

> ps-local（板卡 PS 上的 Qt 本地 HMI）读取什么数据，以及它与 PL、NPU、PC 上位机的通信接口（函数 / 消息）。
> 寄存器真值见 [`PL-PS接口约定.md`](PL-PS接口约定.md)；推理接口见 [`NPU-AI接口约定.md`](NPU-AI接口约定.md)；网络与注入见 [`上位机-PS接口约定.md`](上位机-PS接口约定.md)；功能与交互见 [`../host/ps-local/README.md`](../host/ps-local/README.md)、[`../host/上位机需求与设计.md`](../host/上位机需求与设计.md)。

| 项 | 值 |
|---|---|
| 运行位置 | 板卡 PS（ARM Linux / Buildroot），HDMI 输出 |
| 形态 | kiosk 单进程 Qt 应用（ps-local 即 PS 应用） |
| 接口版本 | v1.3（与 PL 接口版本同步；兼容 v1.2 采集） |
| PS 实现位置 | [`../host/ps-local/`](../host/ps-local/) |
| 冻结日期 | TBD |

---

## 1. 架构

ps-local 是 PS 侧单进程 Qt 应用：直接经 Linux 驱动读 PL 采集数据、调用 NPU 运行时推理、本地加密存储，并向 PC 上位机收发网络帧。

```
PL 采集 IP ─AXI/DMA─► Linux 驱动 (/dev/pl_ctrl) ─┐
                                                 ├─► ps-local (Qt, 单进程)
NPU 运行时 (npu_*) ◄─────────────────────────────┘        │
                                                          ├─ HDMI 显示 + 参数配置
PC 上位机 ◄──────── 以太网（UDP 数据 / TCP 控制）──────────┤
本地 SD ◄──────────────── 加密采集记录 (.mlkc) ────────────┘
```

- 数据源以 `DataSource` 抽象，本轮仅使用驱动，设备不可用时保留不可用状态；对 UI 输出统一 `Frame`（见 [`上位机-PS接口约定.md`](上位机-PS接口约定.md) §3）。
- 若后续要「UI 与采集解耦、连续运行 2 h」更稳，可拆出 PS 数据服务 `mlk_psd`，UI 与 PC 都作客户端（本地用 Unix socket，帧格式同网络）；此拆分 `TBD`。

---

## 2. 读取的数据

| 数据 | 来源 | 更新 |
|---|---|---|
| 温度 / 电流 / 振动三轴 | 驱动 `pl_read_sensors` / `pl_dma_read` | 采样率（如 10 kSps） |
| 转速（原始计数差 / rpm / 状态位图） | 驱动 `pl_read_speed` / DMA 转速记录（§4） | 100 ms |
| 状态 / 计数 / 溢出 | 驱动 `pl_status` / `pl_latest` | 周期 |
| done 中断 | 驱动 `pl_wait_irq` | 每帧 |
| 诊断结果 | `npu_infer` 返回值 | 每次推理 |
| 配置 | 本地设置 / PC 下发 | 变更时 |

---

## 3. PL ⇄ PS 驱动 API

PS 侧对 PL 采集 IP 的封装，实现 [`PL-PS接口约定.md`](PL-PS接口约定.md) §4 / §6 的寄存器与 DMA。设备节点默认 `/dev/pl_ctrl`。

```c
/* pl_host.h —— PS 侧封装（实现 PL-PS接口约定 §4/§6） */
int  pl_open(const char *dev);                       /* 打开，默认 /dev/pl_ctrl */
void pl_close(int fd);
int  pl_probe(int fd, uint32_t *id);                 /* 读 ID，校验 MAGIC/VER */
int  pl_configure(int fd, uint32_t ctrl);            /* CTRL：EN/RST/IRQ_EN/MODE */
int  pl_status(int fd, uint32_t *stat);              /* STAT：BUSY/OVF/STATE */
int  pl_latest(int fd, uint16_t *sample, uint16_t *cnt); /* DATA：最新采样+计数 */
int  pl_clear_irq(int fd);                           /* 写 IRQ[0]=1 */
int  pl_wait_irq(int fd, int timeout_ms);            /* 阻塞等一次 done */
int  pl_dma_start(int fd, unsigned frame_bytes, unsigned blocks); /* FDMA/VDMA */
int  pl_dma_read(int fd, void *buf, unsigned len, unsigned *got); /* 读一帧，阻塞 */
int  pl_dma_stop(int fd);
```

- 打开设备时用 `pl_probe` 校验 `ID.MAGIC/VER`；地址 / 中断真值以 `PL-PS接口约定.md` §3/§5 为准。
- 大数据不逐点读 `pl_latest`，走 `pl_dma_read` 环形缓冲。

---

## 4. 转速通道（接口 v1.2）

转速通道的寄存器、状态位图与 DMA 记录格式以 [`PL-PS接口约定.md`](PL-PS接口约定.md) §14 为准；本节只写 PS 侧的封装、读取协议与应用规则。PL 侧 Modbus 轮询与错误语义见 [`RS485转速通道接入.md`](RS485转速通道接入.md) §4。

### 4.1 读取接口

```c
/* pl_host.h */
typedef struct {
    int32_t  raw;       /* 采样周期内的编码器计数差 */
    int32_t  rpm_milli; /* 0.001 rpm/LSB，signed */
    uint32_t status;    /* 位图，见 4.2 */
    uint32_t count;     /* 成功样本累计数 */
    uint32_t ts;        /* 事件时间戳，10 ns/LSB，32 位回绕 */
    uint32_t seq;       /* 发布事件计数，含错误事件 */
    uint32_t err_count; /* 错误事件累计数 */
} pl_speed_t;

int pl_read_speed(int fd, pl_speed_t *out);
```

| 成员 | 类型 | 单位 / 取值 | 语义 |
|---|---|---|---|
| `raw` | `int32_t` | 编码器计数，signed | PL 侧 100 ms 采样周期内的计数变化量；反转时为负，正负号沿用编码器计数方向 |
| `rpm_milli` | `int32_t` | 0.001 rpm | 由 `raw` 按 `raw × 60 000 000 / (4096 × 100)` 向零截断换算；越界时饱和到 `0x7FFFFFFF` / `0x80000000` 并置 `SATURATED`，此时本字段非真实转速而 `raw` 仍为真实计数差 |
| `status` | `uint32_t` | 位图 | 在线 / 饱和 / 错误 / 已配置与错误码，逐位见 4.2；高 16 位读 0 |
| `count` | `uint32_t` | 个 | 成功样本累计数，只在正确帧上加一；错误事件不改 |
| `ts` | `uint32_t` | 10 ns/LSB | PL 事件时间戳，与 `pl_read_pl_ts()` 同一时基；32 位，约 42.9 s 回绕 |
| `seq` | `uint32_t` | 个 | 发布事件累计数，成功采样与错误事件各加一；用作读取一致性校验 |
| `err_count` | `uint32_t` | 个 | 错误事件累计数，只在错误事件上加一 |

`fd` 为已打开的 `/dev/pl_ctrl` 描述符，`out` 由调用方分配，函数内部整体覆盖。

| 返回值 | `errno` | 含义 |
|---|---|---|
| `0` | — | `out` 为一致快照，各字段可用 |
| `-1` | 系统错误码 | 寄存器读取失败；参数非法时为 `EINVAL`；`out` 不可用 |
| `-EAGAIN` | `EAGAIN` | 三轮重读仍未取到一致快照；`out` 各字段不可用，调用方应丢弃并在下一周期重试 |

### 4.2 状态位图 `status`

寄存器窗口与 DMA 记录共用同一套位图：DMA 记录 W0 的 `SAMPLE` 字段就是本寄存器的低 16 位。

| 位 | 宏 | 含义 | 有效条件 |
|---|---|---|---|
| `[0]` | `PL_CTRL_SPEED_STATUS_ONLINE` | 1：Modbus 通道在线；0：最近一次事务失败 | 恒有效 |
| `[1]` | `PL_CTRL_SPEED_STATUS_SATURATED` | 1：`rpm_milli` 换算饱和 | 恒有效 |
| `[2]` | `PL_CTRL_SPEED_STATUS_ERROR` | 1：最近一个事件为错误事件，不携带新样本 | 恒有效 |
| `[3]` | `PL_CTRL_SPEED_STATUS_CONFIGURED` | 1：PL 侧已完成 FC06 采样时间配置 | 恒有效 |
| `[7:4]` | `PL_CTRL_SPEED_STATUS_ERROR_CODE_MASK` / `_SHIFT` | 最近错误码，取值见下表 | `[2]=1` |
| `[15:8]` | `PL_CTRL_SPEED_STATUS_EXCEPTION_CODE_MASK` / `_SHIFT` | 最近 Modbus 异常码；错误码为 5 时有定义，其余取值不解释 | `[2]=1` |
| `[31:16]` | — | 保留，读 0 | — |

`[7:4]` 错误码取值（宏 `PL_CTRL_SPEED_ERR_*`，含义见 [`RS485转速通道接入.md`](RS485转速通道接入.md) §4）：

| 值 | 宏 | `plctl speed` 文本 | 含义 |
|---|---|---|---|
| 0 | `PL_CTRL_SPEED_ERR_NONE` | `none` | 最近事务正常 |
| 1 | `PL_CTRL_SPEED_ERR_TIMEOUT` | `timeout` | 应答超时 |
| 2 | `PL_CTRL_SPEED_ERR_CRC` | `crc` | CRC 错误 |
| 3 | `PL_CTRL_SPEED_ERR_FRAME` | `frame` | 长度 / 地址 / 功能码 / 字节数或初始化回显不符 |
| 4 | `PL_CTRL_SPEED_ERR_UART` | `uart` | UART 停止位错误或 BREAK |
| 5 | `PL_CTRL_SPEED_ERR_EXCEPTION` | `modbus_exception` | Modbus 异常应答，异常码见 `[15:8]` |
| 6 | `PL_CTRL_SPEED_ERR_GAP` | `inter_frame_gap` | 帧内间隔超过 t1.5 |
| 7 | `PL_CTRL_SPEED_ERR_RX_OVF` | `rx_overflow` | 应答超过接收缓冲区 |

其它取值在 CLI 文本中显示为 `reserved`。整窗 `0xb8..0xd0` 只读，PS 侧写入不被接受。

### 4.3 读取一致性协议

`pl_read_speed()` 保证返回的字段属于同一事件，不混合相邻两次发布：

1. 读 `SPEED_SEQUENCE`（`0xcc`）。
2. 一次 `pread` 读 `0xb8..0xd0` 共 7 个 32 bit 字，顺序为 `RAW` / `RPM_MILLI` / `STATUS` / `COUNT` / `TS` / `SEQ` / `ERR_COUNT`。
3. 再读 `SPEED_SEQUENCE`。
4. 两次序号相同、且与突发内读到的序号一致才接受；否则整轮重读，最多 3 轮，仍不一致返回 `-EAGAIN`。

序号只由 PL 侧事件推进，PS 侧读取不改变它。连续三轮被打断说明读取周期短于事件发布周期或 PL 未就绪，此时不得把拼凑的字段交给上层。

### 4.4 事件语义与离线判定

| 事件 | `seq` | `count` | `err_count` | `status` 位 | `raw` / `rpm_milli` / `ts` |
|---|---|---|---|---|---|
| 成功采样 | +1 | +1 | 不变 | `ONLINE=1`、`ERROR=0`，`CONFIGURED` 保持配置结果 | 更新为该样本 |
| 错误事件 | +1 | 不变 | +1 | `ONLINE=0`、`ERROR=1`，`[7:4]` 置错误码（Modbus 异常另置 `[15:8]`） | 保留最近一次成功值，不是该事件的新读数 |
| 换算饱和（成功采样的一种） | +1 | +1 | 不变 | 另置 `SATURATED=1` | `raw` 真实，`rpm_milli` 饱和 |

- `CTRL.EN=0` 冻结寄存器窗口与 DMA 更新，但不冻结 PL 侧 Modbus 轮询；置 1 后窗口给出新事件。`CTRL.RST` 清窗口，PL 计数不重置，最近有效值也不跨复位保留。
- DMA 记录的丢弃计入 `DMA_STATUS.overflow`，不在本状态字内（`pl_host.h` 的接口注释）。
- 应用层离线判定：`pl_read_speed()` 返回无效、`ONLINE=0`，或连续 2 s 没有新的成功样本时，显示不可用；不得显示 0 转速，也不得把保留值当作有效读数。错误事件使 `seq` 前进而 `count` 不变，因此判定依据是成功样本而不是 `seq` 本身。
- 驱动数据源 `DriverSource` 按上述规则取 2 s 超时（`kSpeedTimeoutMs`），原因文本取最近状态：`ONLINE=0` 为「编码器离线」，未配置为「编码器未配置」，无状态可依据时为「无有效数据」。错误事件只更新原因，不立即清空显示，避免单次 CRC 失败造成闪断。

### 4.5 DMA 记录

一个转速事件在环形缓冲中固定写三个字（24 B），与振动帧同宽：

| 字 | 字段 | 内容 |
|---|---|---|
| W0 | `[63:32]` | 事件时间戳，10 ns/LSB |
| W0 | `[31:16]` | `CH=3` |
| W0 | `[15:0]` | `SPEED_STATUS` 低 16 位（位图见 4.2） |
| W1 | `[63:32]` | `rpm_milli`，signed 32 bit |
| W1 | `[31:16]` | `CH=4` |
| W1 | `[15:0]` | 0 |
| W2 | `[63:32]` | `raw_speed`，signed 32 bit |
| W2 | `[31:16]` | `CH=5` |
| W2 | `[15:0]` | 0 |

- `CH` 是每个 64 bit 字 `[31:16]` 的通道号，不是整条记录的标识：一条记录三个字的 `CH` 各不相同，只有首字 `CH=3` 表示记录开始，`CH=4/5` 只表示该字承载的字段。
- 记录内不插入 `0xFFFF` 填充字（`CH=0xFFFF` 只是解析器的预留编码，当前 RTL 不产生）；三字连续写出，被 4 KiB 块边界切开时由解析器跨块保留余字，不丢失也不补齐。
- 错误事件同样写一条记录，`bit2=1`，其中的 `rpm_milli` / `raw_speed` 仍是最近一次成功值；只有 `bit2=0` 的记录计为新样本。
- 一条记录内三字共享 W0 的时间戳；W1 / W2 的高 32 位是数值而不是时间戳。
- `DmaFrameAssembler::push()` 接收 `std::vector<DmaSpeedRecord> *speeds` 的重载按 `CH=3,4,5` 配齐一条记录后追加；缺字、乱序、`CH` 不匹配时已收到部分作废并计入 `dropped`，随后由首字重新对齐，同时计入 `resync`。

```c
/* dma_frames.h */
struct DmaSpeedRecord {
    uint32_t ts;        /* W0 时间戳，10 ns/LSB */
    int32_t  rpm_milli; /* W1 高 32 位 */
    int32_t  raw_speed; /* W2 高 32 位 */
    uint16_t flags;     /* W0 低 16 位，SPEED_STATUS 低 16 位 */
};
```

### 4.6 两路取值分工

| 路径 | 提供 | 不提供 |
|---|---|---|
| 寄存器窗口 `pl_read_speed()` | 最近一次事件的 `raw` / `rpm_milli` / 完整 `status` / `count` / `ts` / `seq` / `err_count` | 历史；相邻事件之间的变化 |
| DMA 转速记录 | 每个事件一条带 PL 时间戳的记录（`ts` / `rpm_milli` / `raw_speed` / `flags`） | `count` / `err_count`；事务级计数 |

- 未开启 DMA 环形搬运时只用寄存器轮询；开启后两路并行，寄存器窗口给瞬时值，DMA 记录给带时间戳的波形。
- 两路按 `ts` 合并：时间戳不大于已发布样本、且落后不足 200 ms（`kSpeedDedupTicks` = 20 000 000 × 10 ns）的视为同一事件，只追加一次；32 位回绕后按新事件处理。
- 两路时延差超过该窗口时按两个独立事件处理，不互相配对；环形模式下 1 s 内已出现 DMA 记录时，不再把寄存器样本追加为波形点（`kSpeedDmaPreferMs`）。

### 4.7 命令行

```text
plctl speed [--json]
plctl speed --help
```

单次读取并打印一个事件；`--help` 在打开设备前处理，无设备节点也可用。命令行不提供周期监视参数，连续观察由外部循环调用 `plctl speed --json` 实现。

| 项目 | 文本输出 | `--json` 键（类型） | 说明 |
|---|---|---|---|
| 状态 | `state=online` / `state=offline` | `online`（bool） | `ONLINE` 位 |
| 原始计数差 | `raw=<int32>` | `raw`（int） | 离线时仍输出保留值 |
| 千分转速 | `rpm_milli=<int32>`；离线 `rpm_milli=-` | `rpm_milli`（int）；离线 `null` | 0.001 rpm/LSB |
| 转速 | `rpm=<%.3f>`；离线 `rpm=-` | `rpm`（number）；离线 `null` | `rpm_milli / 1000`，三位小数 |
| 成功样本数 | `count=<uint32>` | `count`（int） | |
| 错误事件数 | `err_count=<uint32>` | `err_count`（int） | |
| 时间戳 | `ts_ticks=<uint32>` `ts_ms=<%.3f>` | `ts_ticks`（int）、`ts_ms`（number） | 10 ns/LSB 与毫秒换算 |
| 序号 | `seq=<uint32>` | `seq`（int） | |
| 状态字 | `status=0x%08x`（十六进制） | `status`（十进制整数） | 完整 32 位 |
| 定位位 | `online=` `configured=` `saturated=` `error=`（0 / 1） | 同名 bool | 与状态字各位一致 |
| 错误码 | `error_code=<u>(<文本>)` | `error_code`（int） | 文本映射见 4.2 |
| 异常码 | `exception_code=<u>` | `exception_code`（int） | `[15:8]` |

- 成功时 `--json` 输出单行，含 `"ok":true` 与上表全部键；离线记录仍打印 `raw` / `count` / `ts` 等保留值，只把 `rpm_milli` 与 `rpm` 置 `null`。
- 读取失败时退出码为 1：文本模式在 stderr 打印 `speed: read 0xb8..0xd0: <原因>`；`--json` 另在 stdout 打印 `{"ok":false,"error":"<原因>"}`，不打印其余键。参数不是 `--json` 或 `--help` 时在 stderr 打印用法并返回 1。
- `plctl dma --ring` 每块输出追加 `speed_words=<本块 CH=3/4/5 的字数>`；块首字属于 `CH=3/4/5` 时再追加 `speed_record=word<N>`（N 为 1/2/3，表示首字在记录中的位置）；结束汇总行含 `speed_words=<全程字数>`。CLI 只计数，不解析记录内容。

### 4.8 本地看板

「转速」面板（单位 rpm）由数据源的 `speedChanged` / `speedAvailabilityChanged` 驱动：

- 读数先按安装方向对 PL 符号取反，再按 0.1 rpm 四舍五入量化；数值、波形、UDP 与记录使用 signed32，不施加 ±3276.7 rpm 限制。
- 波形只追加有效样本：`ONLINE=1` 且 `ERROR=0`；同一 PL 时间戳只追加一次，时间轴按 10 Hz 采样率记入。
- 首个有效样本到达前显示「未接入」；连续 2 s 没有新的可用样本时清空波形、数值区显示不可用原因（取最近状态：`编码器离线` / `编码器未配置` / `无有效数据`），不显示 0 rpm；恢复有效样本后重新显示。
- 提示条显示在线状态、成功样本数、事件时间戳（10 ns/LSB）、饱和标记与样本来源（DMA 记录 / 寄存器轮询）。
- 波形缓冲保留最近 2048 个样本（10 Hz 约 205 s），超出后丢弃最旧样本。

标题下方的覆盖层显示板端本地时间 `年-月-日 时:分:秒`（`yyyy-MM-dd HH:mm:ss`），是世界时钟，始终按墙钟刷新，不随数据集回放暂停、完成或停止。模型区块右侧显示窗口结果时间，运行 / 完成时更新，暂停、停止或推理失败后保持最后一次结果。采集方式（驱动名或 UDP 端点）与块统计移到指标区「PL 特征摘要」右侧，完整文本放入提示条。

### 4.9 推理输出中的故障字段

`npu_result_t` 的 `cls` / `conf` / `grade` 为故障检测与故障分级的预留字段，当前推理恒为 0；v3 推理帧不携带分类字段，本地看板显示 `故障检测：N/A` / `故障分级：N/A`，两端模型区块均显示 `N/A`。0 不表示「正常」或「0% 置信度」，不得据此判定故障或分级。字段定义、当前取值与后续接入的 ABI 规则见 [`NPU-AI接口约定.md`](NPU-AI接口约定.md) §4.1。

单主故障模型的五类结果不走 `npu_result_t`，而是通过独立引擎 ABI 与模型诊断 API 2 发布；本地看板在「实时故障诊断」区块显示唯一具体故障、所选等级、分数、窗口时间与质量原因，失败或数据不足显示 `N/A` 与原因码。API v1 的粗分类仅保留兼容摘要，不能用于推断具体故障类型。

---

## 5. PS ⇄ NPU API

调用板载 NPU 推理，接口见 [`NPU-AI接口约定.md`](NPU-AI接口约定.md) §3：

```c
int  npu_init(const char *model);
int  npu_infer(const float *in, int len, npu_result_t *out);
void npu_deinit(void);
```

- 当前 profile 在独立线程中处理已归一化的数据集窗口，RUL 使用 UDP v3 float32，单位为观测老化试验轮次；模型不输出分类、分级与置信度，接口不定义这些字段。合并镜像的 HDMI / 采集 DMA / 混合回放基础共存已实板验证；长期稳定性与实采适配另行验收，见 [接入规程](寿命模型-上位机接入.md)。
- 本地看板「边缘模型检测」显示剩余寿命、推理耗时、后端、状态、窗口索引，并在寿命右侧固定显示 `故障检测：N/A` 与 `故障分级：N/A`；无有效 RUL 时寿命项留空。分类占位只是后续模型的接口位置，不代表已实现故障判定。
- 电流通道由 `DriverSource` 换算为 signed32 mA，面板 scale=1；只在 `CURRENT_COUNT(0x38)` 变化时发布样本。ACS712-20A 默认零点 20000 LSB、增益 1250 µA/LSB，对应 ADS1115 ±4.096 V、125 µV/LSB 和霍尔 100 mV/A。启动 / 重新连接时从 QSettings 恢复系数至 PL 并回读核对，每秒检查及恢复寄存器。配置中的电流零点 / 增益沿用双端共享控制权，温度 / 振动使用 k/b；电流 / 转速单位固定。未标定、通信失败、ADC 饱和、500 ms 无新电流时显示原因并停止发布有效电流，不以 0 代替。「归零」只修改 `CURRENT_ZERO(0x4C)`，保留增益，回读核对后保存实际零点；电机停转且无电流时执行。
- 与 PC 不同，ps-local **不提供** `type=2` 模拟注入（该功能仅 PC 端）。

单主故障模型不使用上述 `npu_*` 入口，另有独立 ABI 与运行包（`MLK_FAULT_ENGINE_ABI=1`，`libmlk_fault.so`，输入 `[1,27683]`，输出固定 32 字节的唯一类别与等级）。封装位于 `faultruntime.*`（动态加载与资产校验）、`faultpreprocess.*`（四秒原生块预处理与标准化）、`faultworker.*`（工作线程，单窗口在途、积压只留最新）与 `faultlivecontroller.*`（模式、run、确认与时效）。两类模型在应用级互斥：启动一类前停止另一类并等待引擎释放；本地 UI 不阻塞推断，停止后的晚到结果按 run 失效。字段级定义见 [`NPU-AI接口约定.md`](NPU-AI接口约定.md) 的单主故障模型一节与 [`故障诊断模型接口需求.md`](../host/故障诊断模型接口需求.md) §2～§4。

---

## 6. PS ⇄ PC 上位机

走 [`上位机-PS接口约定.md`](上位机-PS接口约定.md)：

| 方向 | 帧 | 说明 |
|---|---|---|
| PS → PC | `type=0` 波形 | 四通道采样流 |
| PS → PC | `type=1`（v2，PC 模拟演示） | 结论 / 分级 / 置信度 / 剩余寿命 / 推理耗时 |
| PC → PS | `type=2` 注入 | 旧四值接口，不适用于当前寿命模型 |
| PS → PC | v3 `type=1` | 寿命结果 / 状态 / 命令确认 |
| PC → PS | v3 `type=3` | 已部署数据集的回放控制 |
| PS → PC | v4 `type=0` | 四通道采集，振动含 X/Y/Z |
| PS ↔ PC | v4 `type=4` | 共享标定 / 告警快照与版本校验 |
| PS → PC / PC → PS | v4 `type=5/6` | 故障工单 / 落盘确认 |
| PS → PC | v4 `type=7` | 板端系统占用、时间与 DMA 摘要 |
| PS → PC | v6 `type=8` | 完整故障状态（`mlk.fault.status.v1`），最长 1200 字节，独立于 `type=7` 的粗摘要 |
| PS ↔ PC | v4 `type=5/6` | 故障工单与落盘确认；v2 工单在 `diagnosis` 中携带结构化类型 |
| PS ↔ PC | TCP 命令（预留） | 文件、时间同步（见该文 §4） |

ps-local 本地看板即使不连 PC 也独立工作（本地存储 + HDMI 告警）。

---

## 7. 参数配置（本地 / HDMI）

规划的设置项与作用对象（本轮数值看板不展示未实现配置）：

| 参数 | 作用对象 | 接口 |
|---|---|---|
| 采样率 / 分频 | PL 采集 IP | 驱动 `pl_configure`（ioctl → PL 寄存器） |
| 通道使能 | PL | 同上 |
| 工作模式 | PL | `CTRL.MODE` |
| 每传感器告警限 | 本地看板 / 告警 | 本地应用 |
| 分级 / 剩余寿命阈值 | NPU 适配层 | 本地应用 |
| FFT 点数 / 窗 | 本地显示 | 本地应用 |
| 网络地址 / 端口 / 上报目标 | 网络 | 本地应用 |
| 存储目录 / 加密口令 / 保留月数 / 最低剩余空间 / 记录分段 | 本地 SD | `CaptureRecorder` / `CaptureStore`，MLKC 外层 v1 / 载荷 v2；分段 10/30/60/300 s |
| 配置密码 | 本地应用 | QSettings；初始 `root`，可在配置内修改 |
| 文本输入 | 本地应用 | 无物理键盘；点击 QLineEdit / QSpinBox 弹出独立顶层虚拟键盘；支持大小写英文、数字、常用符号 `. / : - _ @ * + = , ; #`、Shift/Caps、Backspace、Space、Enter；Shift/Caps 切换字母键标签；长按顶部拖动，可拖出配置窗口 |
| 记录管理 | 本地 SD | 配置（需密码）内「清理所有记录」或「删除选定记录」；删除 `capture_*.mlkc` / `fault_*.mlkf` 前确认，保留工单、标签、寿命结果 |
| 时间同步 | 本地 / 网络 | 顶栏「时间同步」立即校时；后台每 10 s 检测网络时间源，可用后自动步进。无网络时在配置中手动设置，网络时间可用时该项禁用；串口 `tools/sync_board_time.sh` 仍为网络不可用时的回退 |
| 重启 | 本地 | 配置（需密码）内「重启核心板」，确认后执行 `reboot` |

- 采集 / PL 相关参数经驱动落到寄存器；显示 / 阈值 / 存储 / 网络为应用本地。
- 配置持久化用各端独立的 QSettings。共享标定 / 告警参数由板端保存唯一生效副本，默认 ps-local 优先：远端写入被拒绝，板端可随时修改。切换 remote-pc 优先后，板端仍可修改本地值；上位机连接后以其保存的共享快照覆盖板端。共享参数与链路 / 密钥 / 本地存储相互独立，修改本地设置不要求先配置 IP 或密钥；空地址表示不绑定监听地址或不设置上报目标。
- ps-local 顶栏「展开」右侧提供「网络配置」，左列选择千兆网 / 4G / WiFi，右列显示对应配置与状态；网络连接状态为 千兆网 / 4G / WiFi / 未连接 四态。故障时间位于模型面板「故障分级」右侧；`KLocalWindow::reportFault` 接收事件 ID、UTC epoch 微秒、分类 / 分级及说明，先保存加密工单，再发送给 PC。接口约束与网络构建 / 验收见 [无线联网与双端协同](无线联网与双端协同.md)。

---

## 8. 交互（HDMI）

- kiosk 一屏：温度数值、振动三轴、电流与转速面板；转速面板的波形取最近窗口内的有效样本，离线或停更时显示不可用原因。数据集寿命回放独立运行；无有效结果时显示不可用。
- 导航：**2.5G USB 无线鼠标接收器（预选，内核 `usbhid`）**，或**板载 KEY1/KEY2**（上 / 下 / 确认 / 返回）；无鼠标时仅按键。
- 详见 [`../host/ps-local/README.md`](../host/ps-local/README.md)「交互（预选方案）」。

---

## 9. 版本与变更规则

与 [`PL-PS接口约定.md`](PL-PS接口约定.md) §12 一致：冻结后只准「加」；改语义升版本 + 双方确认 + 两端同步实现。

---

## 10. 未定（TBD）

- 是否拆 `mlk_psd` 数据服务与本地 IPC；ps-local 对 `type=2` 是否转发 NPU；非采集通道（CAN 编码器等）在 ps-local 的读取路径。

## 11. 本轮实现与可用性

- `pl_host.cpp` 通过 pread/pwrite 访问字符设备；`pl_wait_irq` 使用 poll 与 `PL_CTRL_IRQ_COUNT` ioctl。驱动实现见 [`../host/ps-local/driver/`](../host/ps-local/driver/)。
- `pl_read_sensors` 先读计数和 FRAME_TS，再读帧字段，重读 FRAME_TS 与计数；变化时最多重试 8 次，仍不一致返回 EAGAIN。时间戳为 PL 接收时基，不能直接减去 Linux 实时时钟计算端到端延迟。
- Qt 只显示真实读数（温度、电流、振动三轴、转速）；计数、样本时间戳实际变化后方可用。温度 / 振动停更门限为 6 s / 500 ms。
- 电流、转速均已接入面板：电流面板带零点校准，转速按 §4 的在线判定显示，离线或 2 s 无新样本时不可用；`npu_*` 已通过动态共享库提供 CPU / FF6 六分支数据集回放，加载前核对执行计划和文件 SHA-256，见[模型接入说明](../host/ps-local/inference/README.md)；无有效结果时显示不可用。DMA 环形缓冲经 `/dev/mem` 映射，`pl_dma_ring_open` / `pl_dma_ring_next` / `pl_dma_ring_stop` 按块序号校验并读出，驱动不再返回 EOPNOTSUPP。传感器 IRQ 本版不接，`plctl irq` 明确返回 polling-only。
- 无演示源、无随机读数、无模拟推理回退。驱动 / Qt 基础采集显示已实板运行；MLKC 四传感器记录、异常回放、标签与 CSV 导出已完成开发机验证，完整实采速率、磁盘写入压力与长时运行待板端验证。
- 部署与验证命令见 [`PL-PS接入与上板交付.md`](PL-PS接入与上板交付.md)。

## 声光告警与模型发布

板端使用 PL v1.3 的 `ALARM_STATE/ALARM_STATUS` 驱动 J12.11/U7。通道勾选、上下限与标定取已通过双端权限及版本检查的共享配置。模型故障优先抢占通道告警。`KLocalWindow::reportModelState(QJsonObject)` 和 `/run/mlk-fault.sock` 是分类模型上报入口；原 `reportFault` 只做工单持久化。字段、时长、超时、客户端及验证规程见[声光告警与模型故障 API](声光告警与模型故障API.md)。

## 四传感器加密记录与标注

记录文件契约 MLKC 外层 v1 / 采集载荷 v2（兼容 v1）、加密标签 MLKL v1 与本地标签 API v1 见 [采集记录、回放与训练接口](采集记录-回放与训练接口.md)。顶栏「回放」打开独立四传感器窗口，支持异常清单、工单关联、原始记录查看及解密 CSV。文件记录全部成功发布样本，时间采用首样本 UTC 估算；实板吞吐和长期记录待验证。

转速在 PS 应用入口按安装方向对 PL 值取反，再量化为 signed32、0.1 rpm/LSB；本地数值、波形、告警、UDP v6 与记录共用该值，不施加 ±3276.7 rpm 限制。PL 寄存器与 plctl 保留编码器原始方向。

## 单主故障运行包与预处理资产

默认运行目录 `/opt/mlk_fault`；资产生成器与校验器位于
[`fault-inference/tools`](../host/ps-local/fault-inference/tools/)。运行包含清单 `model.json`
及六个有效载荷：`libmlk_fault.so`、`weights.bin`、`weights.tsv`、`thresholds.bin`、
`qparams.tsv`、`preprocess.json`。测试输入、标签与参考预测位于独立评估目录。

| `model.json` 字段 | 必填值 / 约束 |
|---|---|
| `schema` | `mlk.fault.runtime.v1` |
| `model` | 模型身份，1..64 ASCII 字节；生成器接受字母、数字、点、下划线、连字符 |
| `task` / `contract` | `bench_fault_single_v1` / `single_primary_fault_v1` |
| `preprocess` | `base6_native_blocks_short_context_v2` |
| `input` | `layout=base6_single_native4s_flat_v1`，`floats=27683` |
| `classes` | 按顺序 `normal/overload/undervoltage/overtemperature/mount_tilt` |
| `runtime` | `abi=1`，`library=libmlk_fault.so`，`backends` 为 `cpu`、`cpu_cnn_npu` 的非空列表 |
| `execution_plan` | 含 NPU 后端时为 `four_hard_npu_conv3x3_cpu_relu_pool`；纯 CPU 为 `cpu_fp32_reference` |
| `checkpoint_sha256` | 原始训练 checkpoint 的 SHA-256 |
| `files` | 上述六个有效载荷的文件名到 SHA-256 映射；缺失或不符拒绝加载 |

`weights.tsv` 各行给出张量名称、秩、各维、元素数与 float 偏移；
`thresholds.bin` 为八个小端 float32，按四类故障各两个阈值排列；
`qparams.tsv` 为四行 `layer sx sw sy`，相邻层 `sy==sx`。

| `preprocess.json` 字段 | 必填值 / 约束 |
|---|---|
| `schema` / `preprocess` | `mlk.fault.preprocess.v1` / 上述冻结版本 |
| `model` / `task` | 模型身份 / 上述任务 |
| `frequencies` | `bands=64, tf=193, psd=769, maxHz=600` |
| `bands.weights` | 64×193 个冻结频带权重 |
| `norm.tf.mean/std` | 各 512 项，8 通道×64 带 |
| `norm.psd.mean/std` | 各 2307 项，3×769 |
| `norm.stats.mean/std` | 各 12 项 |
| `norm.pose.mean/std` | 各 5 项；有效位与年龄不另行标准化 |
| `norm.fast.mean/std` / `norm.slow.mean/std` | 各 6 / 9 项，通道维参数 |
| `pose.reference` | 3 项冻结水平重力向量，单位 g |
| `quality` | `minBlocks=12, minBlockSamples=64, minVibrationRateHz=1600, tfRatio=0.45, fastRatio=0.75, slowRatio=0.60, poseAgeMs=1500`；现场适用性待验收 |
| `layout` | `tf=[8,64,48], tfActiveColumns=47` |
| `source_preprocess_sha256` | 训练侧预处理资产的来源哈希 |

受控启动配置为 `/etc/default/mlk-inference`：`MLK_FAULT_ROOT` 缺省 `/opt/mlk_fault`，
`MLK_FAULT_EVAL_ROOT` 缺省 `/data/fault-eval`，`MLK_INFERENCE_MODE` 缺省 `off`；
进程显式环境值优先。模式值只记录意图，启动不自动运行模型。
[`stage_fault.sh`](../platform/flash/stage_fault.sh) 校验后暂存到
`/opt/mlk_fault-packages/<model>-<manifest-hash>`，以 `mlk_fault` 和 `mlk_fault.previous`
相对链接保存当前与上一版本。板端切换或回退前须停止任务并等待引擎释放。
