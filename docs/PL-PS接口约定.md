# PL ⇄ PS 接口约定（MLK-F3P-DR1M90G）

> PS 与 PL 之间的数字接口契约：地址映射、寄存器、中断、DMA、时钟 / 复位、数据格式、管脚。PL 与 PS 两侧实现均以本文件为准。
> 开发步骤见 [`PS-PL开发流程.md`](PS-PL开发流程.md)；PL 代码约束见 [`../fpga/README.md`](../fpga/README.md)。

| 项 | 值 |
|---|---|
| 平台 / 芯片 | 米联客 MLK-F3P-DR1M90G / 安路 DR1M90G FPSoC（PS=ARM，PL=FPGA） |
| PL 工具 | 安路 TD（+ design integrator / ChipWatcher） |
| PS 工具 | Linux / Buildroot；裸机用 FPSoc SDK |
| 接口版本 | v1.3（采集与声光告警 ABI；NPU 使用独立厂商 ABI） |
| 冻结日期 | TBD |
| SoC 工程 / 真值来源 | `soc_prj`；地址与中断真值取自 `.../system/Design.xml`（`DS*_BASEADDR0`、`P2F_IRQ_*`） |
| PS 实现位置 | `host/ps-local/driver/` / `host/ps-local/src/pl_host.*` |
| PL 实现位置 | `fpga/`（TD 工程 `uisrc/fpga.al`，顶层 `fpga_top`） |

---

## 1. 范围与职责

- **范围**：PS 与 PL 之间的数字接口——地址、寄存器、中断、DMA、时钟复位、数据格式、管脚。
- **不含**：板级电源与传感器板本体（见 [`传感器板-说明.md`](传感器板-说明.md)）、上位机 UI、算法。
- **单一来源**：本文件是接口的唯一权威定义。PL、PS 两侧的头文件 / 寄存器定义均据此生成或对照，不各自维护独立定义。

---

## 2. 冻结状态

状态取值：`TBD` 未填 · `REF` 例程参考值，待本工程实测确认 · `OK` 已冻结。

| # | 项 | 位置 | 状态 |
|---|---|---|---|
| 1 | 地址映射 | §3 | REF |
| 2 | 寄存器 | §4 | OK |
| 3 | 中断 | §5 | REF |
| 4 | DMA | §4.4 / §6 | TBD |
| 5 | 时钟 / 复位 | §7 | TBD |
| 6 | 数据格式 / 标定 | §8 | TBD |
| 7 | 物理层 / 管脚 | §9 | OK |
| — | PL 采集通道（温度 / 振动 / 电流） | §8 / §9 | OK |
| — | 转速采集 / PL 串口格式 | §8 / §9 | OK |
| — | 转速 SoC 寄存器 / DMA / PS 接入 | §14 / §4.4 / §15.5 | OK |
| — | 同步采集（共同时基 / 时间戳 / 对齐帧） | §4.3 / §8 | TBD |
| — | 新增传感器通道（接入路径 / 通道选择 / 预算） | §15 | OK |
| — | 最小连通测试（§11）通过 | — | TBD |

> §3 地址 `0x8000_0000` 已由实板 `plctl probe` 读回验证；`ID` 为 `0x504C0103`（v1.3，§16 声光告警；转速窗口见 §14，频谱功率定标见 §4.6）；§5 中断号仍为 `REF`。本版明确 **polling-only**：`f2p_irq[0]/[1]` 留给 HDMI VDMA，传感器 IRQ 未接线，`plctl irq` 返回 polling-only 提示；IRQ 不纳入本版验收。
> 温度、振动、电流三路采集已在 PL 侧实现并上板验证，管脚在 §9 冻结；三路采集与特征寄存器本体见 §4.3；SoC 接入（合并工程 `soc_hdmi`）已实现并上板运行，温度 / 振动 / 电流经 `sensor_capture` 进寄存器与 HP1 DMA；DMA 通道见 §4.4，PL 侧 RTL 与 PS 侧驱动 / CLI 已实现并上板验证：ramp 与真实采集流各 4 KB 传输通过，读回校验一致。环形缓冲已实现并上板验证：连续 16 块、块序号回绕与数据读出正确。采集基线实板未观察到 `f2p_irq[3]` 到达 GIC，`f2p_irq[0]/[1]` 由 VDMA 使用，块完成通知采用 `DMA_BLK_SEQ` 轮询（§4.4）。合并工程 `soc_hdmi` 的 PS 宏已导出 HP0/HP1（`Slave_AXI_HP1_Interface=1`、数据宽度 64），在用 HDMI FSBL 亦已使能 HP0/1，因此 HP1 通道不需重新生成 PS IP；仅采集工程 `soc` 的 PS 宏 HP0/HP1 均为 0。
> 电流换算在 PS：PL 只输出 ADS1115 原始码并存标定系数寄存器（§4.3）；霍尔模块资料未定，系数缺省 0（未标定）。
> 转速通道已经板载 RS485（PL 软 UART + DE）接入统一 `fpga_top`，非 J12。OID-R3808B-12S1S 使用 Modbus RTU、9600 8N1、站号 1；PL USB UART 的静止 / 正负转速 / 停止归零已实板验证。该通道已按接口 v1.2 接入 SoC 采集层：只读寄存器窗口 `0xB8～0xD0`（§14）、DMA 记录 `CH=3/4/5`（§4.4）、驱动与 `ps-local` 转速面板（§13）。转速不进入 §4.3 的 1 kHz 对齐帧与振动对齐记录，按自身事件（成功采样或错误事件）单独成帧。

---

## 3. 地址映射（PS 视角）

PL 的每个 IP 作为 AXI 从设备挂到 PS 地址空间；基地址由 design integrator 分配，真值取自 `Design.xml` 的 `DS*_BASEADDR0`。

| IP 名 | 基地址 | 大小 | 挂载总线 | 说明 |
|---|---|---|---|---|
| `pl_ctrl` | `0x8000_0000` | 64K | AXI4-Lite（M_AXI_GP0） | 控制 / 状态 / 采样（最小 IP） |
| `soft_npu` | `0x1_F000_0000` | 64K | GP1 → 厂商 AXI4/Lite 转换器 | `Video_NPU` 控制窗口；配套码流与设备树已实板 probe 成功，见 §6.1 |
| `pl_dma` | `0x1C00_0000` | 64K | AXI4-Lite | FDMA / VDMA 控制 |
| 新增 | TBD | TBD | TBD | TBD |

- 参考：`pl_ctrl` 缓存别名 `0xA000_0000`，`pl_dma` 别名 `0x1E00_0000`。
- 冻结后把真值写入本表；PS 侧宏 `PL_CTRL_BASE` 以此为准。

---

## 4. 寄存器

约定：`RO` 只读 · `RW` 读写 · `W1C` 写 1 清零。地址 = 基地址 + 偏移。未列出的位保留，写 0。

### 4.1 `pl_ctrl`

| 偏移 | 名 | 访问 | 复位值 | 说明 |
|---|---|---|---|---|
| `0x00` | `ID` | RO | `0x504C_0103` | 标识 / 版本 |
| `0x04` | `CTRL` | RW | `0x0` | 使能 / 复位 / 中断使能 / 模式 |
| `0x08` | `STAT` | RO / W1C | `0x0` | 状态 |
| `0x0C` | `DATA` | RO | `0x0` | 最新采样 + 计数 |
| `0x10` | `IRQ` | W1C | `0x0` | 中断标志 |

`ID` (0x00)

| 位 | 字段 | 访问 | 复位 | 值 | 说明 |
|---|---|---|---|---|---|
| `[31:16]` | `MAGIC` | RO | `0x504C` | `0x504C` | 固定标识 |
| `[15:0]` | `VER` | RO | `0x0103` | `0x0103` | 版本 v1.3 |

`CTRL` (0x04)

| 位 | 字段 | 访问 | 复位 | 取值 | 说明 |
|---|---|---|---|---|---|
| `[0]` | `EN` | RW | `0` | `0`=禁止 · `1`=使能 | 采集使能 |
| `[1]` | `RST` | RW | `0` | 写 `1` 复位（自清） | 软复位 |
| `[2]` | `IRQ_EN` | RW | `0` | `1`=使能 | done 中断使能 |
| `[7:4]` | `MODE` | RW | `0` | TBD | 工作模式 |
| `[31:8]` | 保留 | — | `0` | 写 `0` | — |

`STAT` (0x08)

| 位 | 字段 | 访问 | 复位 | 取值 | 说明 |
|---|---|---|---|---|---|
| `[0]` | `BUSY` | RO | `0` | `1`=采集中 | 忙 |
| `[1]` | `OVF` | RO / W1C | `0` | `1`=溢出，写 `1` 清 | 溢出 |
| `[15:8]` | `STATE` | RO | `0` | TBD | 状态机编码 |
| `[7:2]` `[31:16]` | 保留 | — | `0` | 写 `0` | — |

`DATA` (0x0C)

| 位 | 字段 | 访问 | 复位 | 说明 |
|---|---|---|---|---|
| `[15:0]` | `SAMPLE` | RO | `0` | 最新采样值 |
| `[31:16]` | `CNT` | RO | `0` | 采样计数（16 bit 回绕） |

`IRQ` (0x10)

| 位 | 字段 | 访问 | 复位 | 说明 |
|---|---|---|---|---|
| `[0]` | `DONE` | W1C | `0` | `1`=完成中断；写 `1` 清 |
| `[31:1]` | 保留 | — | `0` | 写 `0` |

- 大数据不逐点读 `DATA`，走 DMA（§4.4；FDMA / VDMA 参考参数见 §6）。
- `pl_ctrl_probe()` 校验 `ID` 的 `MAGIC` 与 `VER`。

### 4.2 其它 IP

| 偏移 | 名 | 访问 | 复位值 | 说明 |
|---|---|---|---|---|
| TBD | TBD | TBD | TBD | TBD |

### 4.3 采集与时域特征扩展

本节为独立 RTL 的窗口内偏移定义；实现为 [`axi_lite_reg.v`](../fpga/uisrc/01_rtl/axi_lite_reg.v)。`0x00～0x10` 的偏移、复位值及位域保持 §4.1 定义，`ID=0x504C0103`。SoC 接入见 §14；§3 基址、§5 IRQ 号仍为 REF。DMA 输出通道见 §4.4。

#### 总线与控制

| 项 | 规格 |
|---|---|
| 数据 / 地址 | 32 bit / 16 bit 窗口偏移；小端，32 bit 字对齐 |
| 时钟 / 复位 | 单一 `I_clk`，低有效同步 `I_rstn`；接入 SoC 时钟域为 TBD |
| AXI4-Lite | AW、W 独立单项缓存；读写各一个未完成事务；B/R 背压期间响应保持 |
| 响应 | 已定义读、可写寄存器写返回 OKAY；未映射、未对齐、RO 写返回 SLVERR |
| 写掩码 | WSTRB 按字节生效；控制、W1C、快照只看低字节；阈值只看低两字节；标定系数使用四字节 |
| `CTRL.EN` | 禁止时不更新数据、计数、故障与特征寄存器，不接收完成事件；不清历史值 |
| `CTRL.RST` | 单拍 `O_soft_reset`；清数据、计数、OVF、IRQ、特征、快照与序号；CTRL 与阈值保留；外部采集器复位接线为 TBD |
| `CTRL.MODE` | 位 `[7:4]` 保留读写，无模式切换功能；位 3 及 `[31:8]` 读 0 |
| `STAT` | BUSY / STATE 直接采样输入，OVF 为粘滞 W1C；新溢出优先于清除 |
| `DATA` | `I_sample_valid` 时锁存 signed16 原始样本至 SAMPLE，CNT 加一并按 16 bit 回绕；采样选择由接入层指定，TBD |

#### 扩展寄存器

除阈值外均复位为 0；阈值也复位为 0，表示禁止对应阈值事件。

| 偏移 | 名称 | 访问 | 位域 / 含义 |
|---|---|---|---|
| `0x14` | FEATURE_SNAPSHOT | W1P / RO | 写 bit0=1 原子复制四通道最新结果到快照；读 0 |
| `0x18` | FEATURE_AVAILABLE | RO | `[3:0]` 为 X/Y/Z/电流是否至少完成一个窗口；软复位清零 |
| `0x20` | TEMP | RO | signed16 温度，符号扩展到 32 bit，0.1 ℃ |
| `0x24 / 0x28 / 0x2C` | ACC_X / ACC_Y / ACC_Z | RO | signed16 原始加速度，符号扩展 |
| `0x30` | CURRENT | RO | signed16 ADS1115 原始值，符号扩展 |
| `0x34` | CHANNEL_COUNTS | RO | `[15:0]` 温度计数，`[31:16]` 振动向量计数，分别回绕 |
| `0x38` | CURRENT_COUNT | RO | `[15:0]` 电流采样计数，`[31:16]` 为 0 |
| `0x3C` | FAULTS | RO | `[7:0]` 为接入层 `I_faults`，采集使能时每拍更新；接入层通道位映射见 §14；`[31:8]` 为 0 |
| `0x40` | TEMP_THRESHOLD | RW | signed16，符号扩展；温度严格大于阈值产生事件；0 禁用 |
| `0x44` | ACC_THRESHOLD | RW | unsigned16；任一轴绝对值严格大于阈值产生事件；0 禁用 |
| `0x48` | CURRENT_THRESHOLD | RW | unsigned16；电流原始码绝对值严格大于阈值产生事件；0 禁用 |

阈值在对应通道 valid 时判定；单位为原始码，电流零点/灵敏度标定见下表。`abs(-32768)=32768`。阈值复位后需显式配置。

#### 电流标定（PL 原始码 + 系数寄存器，换算在 PS）

PL 只采样并输出 ADS1115 原始码（`0x30 CURRENT`，signed16，PGA=±4.096 V，1 LSB=125 µV），不参与电流换算。电流换算在 PS：读原始码与下列系数寄存器后按公式换算。

| 偏移 | 名称 | 访问 | 位域 / 含义 |
|---|---|---|---|
| `0x4C` | `CURRENT_ZERO` | RW | signed32；0 A 对应的 ADS1115 原始码（零点偏置），符号扩展；复位 0 |
| `0x50` | `CURRENT_GAIN` | RW | signed32；每 LSB 对应的电流，单位 µA/LSB；复位 0（0 = 未标定） |

换算关系：

| 物理量 | 关系 |
|---|---|
| ADS1115 输入电压 | `V = raw × 125 µV`（raw 为 `CURRENT` 的符号扩展值） |
| 电流 | `I[µA] = (raw − CURRENT_ZERO) × CURRENT_GAIN` |
| 系数与霍尔模块 | `CURRENT_ZERO = V₀ / 125 µV`，`CURRENT_GAIN[µA/LSB] = 125 / S[V/A]` |

- 当前霍尔模块为 ACS712-20A，5 V 供电，`S=0.100 V/A`、标称 `V₀=2.5 V`；PS 默认写入 `CURRENT_ZERO=20000`、`CURRENT_GAIN=1250 µA/LSB`，实际零点由停机归零校正。PL 硬复位值保持 0；PS 从持久配置恢复并回读核对。
- 示例：`raw=20800`、`CURRENT_ZERO=20000`、`CURRENT_GAIN=1250` → `V=2.6 V`、`I=1000 mA`。
- PS 用整数运算：`I[µA] = (raw − CURRENT_ZERO) × CURRENT_GAIN`，`I[mA] = I[µA] / 1000`；应用不将 ADC 饱和码作为有效电流；显示 ADC 饱和状态。
- 系数写入 PL 寄存器仅为随接口版本固化与按板设置；PL 不校验、不使用。PS 也可在软件内持有同一系数，两侧取值须一致。
- 若后续改为 PL 内换算，复用同一两个系数寄存器，在 PL 增加定点乘加。
- `axi_lite_reg` 实现两项 signed32 系数的读写与逐字节 WSTRB；硬复位清零，软复位保留。PL 不参与换算。

#### 同步采集（共同时基 + 时间戳 + 对齐帧）

实现为 [`timebase.v`](../fpga/uisrc/01_rtl/timebase.v)，由 `fpga_top` 与独立 `axi_lite_reg` 分别例化；SoC 集成时须选用同一采集域实例，跨域传输为 TBD。

| 项 | 规格 |
|---|---|
| 时钟 | 单一 100 MHz `I_clk`，不生成内部时钟；`CLK_FREQ_HZ` 须为 1000 的正整数倍 |
| `r_ts` | unsigned32 自由计数，每拍加一；10 ns/LSB，约 42.94967296 s 回绕 |
| `tick` | 每 100000 拍一次的单拍时钟使能，1 kHz；硬复位释放后第 100000 拍首次锁存帧 |
| `r_ms` / `FRAME_TS` | unsigned32 毫秒计数，每 tick 加一；帧锁存递增后的 `r_ms`，首帧为 1；约 49.71 天回绕 |
| 通道时间戳 | 对应 valid 被接收的上升沿，锁存该沿递增前的 `r_ts`；三轴共用 ACC 时间戳；表示 PL 接收时刻，器件实际转换时刻为 TBD |
| 帧锁存 | tick 所在上升沿同时锁存三路最新有效值、对应样本时间戳与帧毫秒数；同拍 valid 优先，纳入当前帧；无新样本沿用最近有效值与时间戳 |
| 初值 / 复位 | 硬复位清时基、分频相位、最新值与帧；未收到有效样本的通道值 / 时间戳为 0，0 不作为有效标志，可结合原通道计数判定 |
| 使能 / 软复位 | `CTRL.EN=0` 时不接收样本、不更新帧，保留历史；`CTRL.RST` 清最新值、样本时间戳与帧，时基与 tick 相位继续自由运行；顶层本地实例恒使能，无软复位 |
| 同步等级 | 振动 / 电流为 1 kHz 对齐锁存，器件 ODR 3200 Hz / DR 860 SPS 独立；温度为准同步，到达即打戳；均不保证物理同时采样 |
| 对齐触发 / 精度 | 振动读点、电流读取或单次转换的 tick 触发为 TBD，保持现有控制器节奏；tick→valid 时差的 ChipWatcher 实测为 TBD |
| 特征协调 | 时域窗口仍按原始 valid / ready 接受样本，不使用对齐帧重采样；`0x100` 起特征快照及 SEQUENCE 语义保持，特征窗口时间戳 / 特征窗口 DMA 为 TBD；FRAME_TS 不能解释为特征窗口结束时刻 |

下列寄存器均为 RO，硬复位 / 软复位值均为 0；写返回 SLVERR。ACC_TS / CURRENT_TS / TEMP_TS 是当前帧中样本的接收时间戳，在 tick 同拍从各通道最新时间戳复制，帧间保持，使其与帧值一一对应。

| 偏移 | 名称 | 访问 | 位域 / 含义 |
|---|---|---|---|
| `0x54` | FRAME_TS | RO | unsigned32，帧毫秒计数，1 ms/LSB |
| `0x58` | FRAME_TEMP | RO | signed16 温度，符号扩展到 32 bit，0.1 ℃ |
| `0x5C` | FRAME_ACC_X | RO | signed16 振动 X 原始码，符号扩展 |
| `0x60` | FRAME_ACC_Y | RO | signed16 振动 Y 原始码，符号扩展 |
| `0x64` | FRAME_ACC_Z | RO | signed16 振动 Z 原始码，符号扩展 |
| `0x68` | FRAME_CURRENT | RO | signed16 ADS1115 原始码，符号扩展；换算按电流标定公式 |
| `0x6C` | ACC_TS | RO | unsigned32，帧内三轴样本的 `r_ts` |
| `0x70` | CURRENT_TS | RO | unsigned32，帧内电流样本的 `r_ts` |
| `0x74` | TEMP_TS | RO | unsigned32，帧内温度样本的 `r_ts`，准同步 |

帧一致性读取步骤：

1. 读 FRAME_TS，记录 `t0`。
2. 读 `0x58～0x74` 的帧值与通道时间戳。
3. 再读 FRAME_TS，记录 `t1`；`t0=t1` 时接受，否则重读整帧。复位前后不得合并读取；单次读取区间须短于 FRAME_TS 回绕周期。

每笔 AXI 读独立采样并在响应背压期间保持，硬件不提供跨寄存器原子快照；FEATURE_SNAPSHOT 不冻结这些寄存器。PS 必须按设备内存顺序执行上述读取。`r_ts` 时间差按模 2³² 计算，需在一个回绕周期内解析；超过该周期的停更年龄仅凭 32 bit 时间戳无法确定。

#### 特征快照窗口

每通道占 `0x40` 字节。基偏移：X=`0x100`，Y=`0x140`，Z=`0x180`，电流=`0x1C0`。所有字段为 RO，复位为 0。64 bit 字段先读低 32 bit，再读高 32 bit；快照不随采样更新，避免半字撕裂。

| 通道内偏移 | 字段 | 格式 |
|---|---|---|
| `+0x00 / +0x04` | DC | signed64 Q16，窗口算术均值 |
| `+0x08 / +0x0C` | RMS | unsigned64 Q16，原始信号均方根 |
| `+0x10 / +0x14` | VARIANCE | unsigned64 Q16，总体方差 |
| `+0x18 / +0x1C` | PEAK_TO_PEAK | unsigned64 整数，max−min |
| `+0x20 / +0x24` | CREST_FACTOR | unsigned64 Q16，max(abs(x))/RMS |
| `+0x28 / +0x2C` | ZERO_CROSS_RATE | unsigned64 Q16，符号跨越数/(N−1) |
| `+0x30 / +0x34` | KURTOSIS | unsigned64 Q16，四阶中心矩/方差²，不减 3 |
| `+0x38` | SEQUENCE | unsigned32，通道每次 valid 加一并回绕；与快照同拍锁存 |
| `+0x3C` | FLAGS | bit0=零 RMS；bit1=零方差；其余位为 0 |

默认 `N=256`、步长 128；支持编译期 `N=512`。过零把零归为非负，窗口边界不参与相邻对计数。零 RMS 的峰值因子、零方差的峭度均返回 0，并置对应 FLAGS。精度、吞吐与输入握手见 [`时域与频域特征.md`](../fpga/uisrc/06_doc/时域与频域特征.md)。

读取步骤：写 FEATURE_SNAPSHOT.bit0=1，等待写响应，然后读取各通道的字段和 SEQUENCE。快照与特征 valid 同拍时包含新结果。独立寄存器本体仅在软件写快照时更新快照；`fpga_top` 的本地保留实例按特征 valid 自动快照，尚未挂 AXI。

### 4.4 DMA 通道（HP1 写 DDR）

PL 侧采集样本流经 AXI4 只写主口写入 PS DDR；控制面复用 §4.1 的同一 AXI-Lite 从机（基址 `0x8000_0000`，§3），偏移 `0x90～0xB4` 为新增，`0x00～0x88` 的偏移、复位值与位域保持，接口版本为 v1.1。本通道不经过 §3 的 `pl_dma` 窗口（`0x1C00_0000`）。

#### DMA 寄存器

| 偏移 | 名称 | 访问 | 复位值 | 说明 |
|---|---|---|---|---|
| `0x90` | `DMA_CTRL` | RW / RO | `0x0` | 控制与状态，位域见下表 |
| `0x94` | `DMA_BASE_LO` | RW | `0x0` | DDR 物理地址低 32 位；8 字节对齐 |
| `0x98` | `DMA_BASE_HI` | RW | `0x0` | 地址高 32 位；本板 DDR ≤ 1 GB，恒 0 |
| `0x9C` | `DMA_BYTES` | RW | `0x0` | 本次传输字节数；8 的倍数，范围 8～4096 |
| `0xA0` | `DMA_STATUS` | RO | `0x0` | 传输状态，位域见下表 |
| `0xA4` | `DMA_WRITTEN` | RO | `0x0` | 已写入字节数，按已返回 OKAY 写响应的字节累计 |
| `0xA8` | `DMA_BLK_SEQ` | RO | `0x0` | 自 `START` 起已完成的 4 KiB 块数（32 位，回绕） |
| `0xAC` | `DMA_IRQ` | RW / RO | `0x0` | `[0]` 块完成中断使能；`[8]` 写 1 清除中断挂起（W1C）；RO `[16]` 挂起标志 |
| `0xB0` | `PL_TS_NOW` | RO | - | 当前 PL 共同时基计数（10 ns/LSB，32 位回绕），用于端到端延迟测量 |
| `0xB4` | `DMA_BLOCKS` | RW | `0x0` | 环形模式环大小 N（1～256 块，每块 4 KiB）；单次模式忽略 |

`DMA_CTRL` (0x90)

| 位 | 字段 | 访问 | 复位 | 取值 | 说明 |
|---|---|---|---|---|---|
| `[0]` | `START` | RW | `0` | 写 `1` 启动（自清） | 启动一次传输，清 `DONE`、`DMA_BLK_SEQ` 与突发/溢出计数；`ERRCODE` 粘滞到复位 |
| `[1]` | `ABORT` | RW | `0` | 写 `1` 中止（自清） | 中止当前传输，`ERRCODE` 置 3 |
| `[2]` | `TEST` | RW | `0` | `0`=采集流 · `1`=内部 ramp | 数据源选择；ramp 用于通路自检，初值 / 步长为 TBD |
| `[8]` | `BUSY` | RO | `0` | `1`=传输中 | 忙 |
| `[9]` | `DONE` | RO | `0` | `1`=完成 | 全部写响应 OKAY 后置位 |
| `[10]` | `ERROR` | RO | `0` | `1`=错误 | `ERRCODE` 非 0 时置位 |
| `[3]` | `RING` | RW | `0` | 环形连续模式；`1` 时按 `DMA_BLOCKS` 循环写 4 KiB 块，`0` 为单次块传输 |
| `[4]` | `STOP` | RW | `0` | 写 `1` 优雅停止（自清）：完成当前突发（及正在收尾的块）后释放 `BUSY`，不置 `ERRCODE`；写入须同时带上当前 `TEST` / `RING` 位 |
| `[7:5]` `[31:11]` | 保留 | — | `0` | 写 `0` | — |

`DMA_STATUS` (0xA0)

| 位 | 字段 | 访问 | 复位 | 说明 |
|---|---|---|---|---|
| `[15:0]` | `BURSTS` | RO | `0` | 本次传输已完成突发数；写 `START` 时清 0 |
| `[16]` | `OVF` | RO | `0` | 内部缓冲溢出；置位后的传输行为为 TBD |
| `[19:17]` | `ERRCODE` | RO | `0` | `0`=无错 · `1`=参数非法（长度 / 对齐） · `2`=写响应非 OKAY · `3`=中止 |
| `[31:20]` | 保留 | — | `0` | 读 0 |

`BUSY=1` 期间写 `DMA_BASE_LO` / `DMA_BASE_HI` / `DMA_BYTES` 的行为为 TBD；PS 须在 `BUSY=0` 时配置。

#### 数据字格式

DDR 缓冲区按 64 bit 小端字排列，每字格式：

| 位 | 字段 | 说明 |
|---|---|---|
| `[63:32]` | `TS` | unsigned32，PL 共同时基时间戳，10 ns/LSB，模 2³² 回绕（§4.3） |
| `[31:16]` | `CH` | `0`=X · `1`=Y · `2`=Z · `3`/`4`/`5`=转速记录第 1/2/3 字（§14） · `0xFFFF`=pad |
| `[15:0]` | `SAMPLE` | signed16 原始码；振动 1 LSB = 3.9 mg |

- 低字节先写入 DDR；第 `n` 个字位于 `DMA_BASE + 8·n`。
- `TS` 沿用 §4.3 的 `r_ts` 口径：对应 valid 被接收的上升沿锁存，10 ns/LSB，模 2³² 回绕。
- 通道编码除振动三轴与 pad 外，还有转速记录三字（`CH=3/4/5`，各字 `SAMPLE` 字段承载的内容见 §14）；温度与电流仍经 §4.3 寄存器读取，不进入该流。
- 当前 RTL 不插入 pad 字；`CH=0xFFFF` 为预留编码，PS 侧解析时跳过该字并继续按 `CH` 分支。
- 块按 4 KiB 字节对齐，不按三字帧对齐：同一 X/Y/Z 帧的三个字可以跨块边界。每块 512 字不是 3 的倍数，PS 侧须保留块尾不足三字的残留字并与下一块拼接；不得假设每块从 `CH=0` 开始，也不得按块内 X/Y/Z 数组下标对齐。

#### 传输模型

| 项 | 规格 |
|---|---|
| 主口 | AXI4 只写（AW / W / B），无读通道；数据宽度 64 bit |
| PS 端口 | 接 `Slave_AXI_HP1_Interface`（须置 1）与 `Slave_AXI_HP1_DATA_WIDTH=64`；HP0 由 HDMI VDMA 使用（[`HDMI显示通路.md`](HDMI显示通路.md) §5） |
| 突发 | INCR；不超过 16 拍，每拍 8 B（单突发 ≤ 128 B）；单突发不跨 4 KB |
| Outstanding | 单 outstanding；上一突发的 B 响应返回后才发下一突发 |
| 发起条件 | 先攒满当前突发的全部拍数再拉高 `AWVALID`；`AW` 握手时 W 数据已全部就绪，事务不会因等待采样而长期占用 HP1。数据源无数据时模块停在采集侧等待（`O_s_ready` 保持高），不发起任何 AXI 事务；此时 `STOP` / `ABORT` 直接回到 `IDLE`，不产生半突发 |
| 地址 | `DMA_BASE_HI:DMA_BASE_LO` 为本板 DDR 物理地址，8 字节对齐；PL 不假设固定地址 |
| 长度 | `DMA_BYTES` 为 8 的倍数，范围 8～4096 |
| 完成 | 全部写响应 OKAY 后置 `DONE`；任一写响应非 OKAY 置 `ERROR` 与 `ERRCODE=2`，并停止后续突发 |
| 中断 | 中断线不可用，块完成通知按 `DMA_BLK_SEQ` 轮询。本 SoC 的 PS-PL fabric 中断只有 `f2p_irq[0]`/`[1]`（SPI 82/83，GIC 114/115），合并码流中已由厂商 VDMA 占用；`f2p_irq[2]` 及以上未引出，实板驱动 `f2p_irq[3]` 时 GIC 计数恒为 0，借用 VDMA 线路会使其驱动判为 spurious 并禁用该线。`DMA_IRQ[16]` 记录挂起，`[8]` W1C 清除 |

- 缓冲区物理地址由 PS 侧 `reserved-memory` 预留后写入 `DMA_BASE_HI` / `DMA_BASE_LO`；PL 不假设固定地址，也不访问未配置区域。本项目在合并设备树中预留 `0x3EF0_0000` 起 1 MiB 的 `no-map` 区间（节点 `sensor-dma-buffer@3ef00000`，避开 CMA 与 U-Boot 常驻区），`DMA_BASE_HI` 恒 0。
- `DONE` 须在全部写响应返回 OKAY 后置位，不得只依据内部计数或主口空闲发布。
- 样本来自 100 MHz 采集域（§14）；主口时钟域与跨域缓冲深度为 TBD。
- 基址按 8 字节对齐；单次传输跨 4 KB 边界时的突发拆分方式为 TBD。

#### 读取步骤

1. 写 `DMA_BASE_LO`、`DMA_BASE_HI` 与 `DMA_BYTES`。
2. 写 `DMA_CTRL.START=1`；该写清 `DONE` 与 `DMA_STATUS` 并启动传输，`ERROR` 仅由复位清除。
3. 轮询 `DMA_CTRL.DONE` 或 `DMA_STATUS.ERRCODE`；`DONE=1` 或 `ERROR=1` 时停止轮询。
4. 读 `DMA_WRITTEN` 与 `DMA_STATUS.ERRCODE`：`DMA_WRITTEN=DMA_BYTES` 且 `ERRCODE=0` 时数据有效；否则按错误码处理并丢弃该缓冲区内容。
5. 从 DDR 缓冲区读取 `DMA_WRITTEN` 字节，按本节数据字格式解析。
6. 需提前停止时写 `ABORT=1`；中止后 `ERRCODE=3`，`DMA_WRITTEN` 为已成功写入的字节数。

- 块完成通知：每完成一个 4 KiB 块递增 `DMA_BLK_SEQ` 并置 `DMA_IRQ[16]`，PS 轮询 `DMA_BLK_SEQ` 或读 `DMA_IRQ[16]` 获取进度；写 `DMA_IRQ[8]=1` 清除挂起，未清除时多次块完成合并为一次挂起。传感器事件线不接，其他事件按 §5 轮询。
- 缓冲区由 PS 以 `reserved-memory` 预留并按非缓存方式映射后读取；PL 侧不执行 cache 维护。
- 硬件超时机制为 TBD；PS 侧轮询须自备超时。`CTRL.RST` 对 DMA 通道的影响为 TBD。

- 中止语义：`ABORT` 立即释放 `BUSY`、不置 `DONE`、`ERRCODE=3`；`AWVALID` 已发出即保持地址请求直到握手，使用已攒满的突发缓冲完成剩余 W 拍与 B 响应（最多 16 拍），不撤回 AW、不改变背压期间的 W 数据。尚未发出 AW 时可直接结束。收尾期间忽略新 `START`，完成后才可重新启动。
- 时间戳：AXI 域不携带逐样本时间戳，同一 1 kHz 对齐帧间隔内的样本共用该帧的 `acc_ts`，因此同一帧内 `TS` 相同。

- 环形模式：`DMA_CTRL[3]=1` 时按 `DMA_BLOCKS` 连续写 4 KiB 块，块地址按 `BASE + (BLK_SEQ mod N) × 4096` 回绕，直到写 `ABORT`；每完成一块递增 `DMA_BLK_SEQ`，`DMA_IRQ[0]=1` 时同时触发一次 PL→PS 中断（`f2p_irq[3]`；本 SoC 上该线未接入 GIC，实际只能轮询，见传输模型·中断）。PS 按 `BLK_SEQ` 差值判断新块，避免与 PL 写同一块。停止方式：写 `DMA_CTRL[4]=1` 优雅停止（完成当前突发及正在收尾的块后释放 `BUSY`、不置 `ERRCODE`）；单次模式同样适用，PS 需把「自己发过 STOP」当正常结束而不是错误。写 `ABORT` 仍按中止处理并置 `ERRCODE=3`。
- 环形模式约定：基址须按 4 KiB 对齐；`DMA_BLOCKS` 取 1～256，越界或基址不满足 `ERRCODE=1`；`DMA_BYTES` 忽略；`DMA_WRITTEN` 自 `START` 起累计；环形模式不置 `DONE`；`DMA_BLK_SEQ` 只计已完成块，单次模式下恒 0；块完成中断不要求先 W1C。
- `PL_TS_NOW` 取自采集域 10 ns 时基，经格雷码同步到 AXI 域，读回滞后 2～3 个采集时钟（≤30 ns）。
- 延迟测量：`PL_TS_NOW` 给出当前 10 ns 计数，与样本字中的 `TS` 求差得到「采集→PS 可读」延迟；受 1 kHz 对齐帧量化限制，单次差值误差不超过 ±1 ms。
- PS 侧读取：第 `k` 块（`k` 从 1 计，等于读完该块后的 `BLK_SEQ`）位于 `BASE + ((k-1) mod N) × 4096`。读取最新已完成块时 PL 正在写第 `k+1` 块，两者不重叠；一次落后 N 块以上时中间块已被覆盖，`BLK_SEQ` 的跳变量即丢失块数，最新块仍然完整。`N=1` 没有读取余量，PS 侧应取 `N ≥ 2`。

上述 TBD 项须与实现方确认后冻结。HP1 的端口使能与 FSBL 配置已在合并工程与在用 HDMI FSBL 中确认（见本节传输模型），生产 HPF 正式导出时须保持一致；§2 中 DMA 状态保持 TBD。

---

## 5. 中断

| PL 源 | PS IRQ 号 | 触发 | 清方式 | 说明 |
|---|---|---|---|---|
| `pl_ctrl.done` | `114`（PL_INTER0） | 上升沿 | 写 `IRQ[0]=1`（W1C） | 一次采样 / 一帧完成 |
| 预留 | `115`（PL_INTER1） | TBD | TBD | 第二路 PL 中断 |
| 新增 | TBD | TBD | TBD | TBD |

- IRQ 号真值以 `Design.xml` 的 `P2F_IRQ_*` 为准；`114/115` 来自例程 `14_axi_gpio_pl_intr/soc_sdk/axi_intr/main.c`（`PL_IRQ_NUM0/1`）。SoC 候选只启用 `F2P_IRQ_Available=1`，`f2p_irq[0]` 接自研单拍 IRQ；实际 GIC ID 须在实板 `/proc/interrupts` 与设备树中确认，当前仍为 `REF`。
- 合并 HDMI + 采集码流 `soc_hdmi.bit` 的 `f2p_irq[0]/[1]` 已被厂商 VDMA mm2s/s2mm 占用；传感器 IRQ 在该码流不接线，`pl_ctrl` 走轮询，`plctl irq` 返回 ENODEV。独立采集候选 `soc_candidate.bit` 仍按 `f2p_irq[0]` 预留。
- PS 侧实现：Linux 用 `request_irq`；裸机用 FPSoc SDK 的 `AlIntr_RegHandler`。
- 独立 RTL 的输出为单个 `I_clk` 脉冲，由 `p2f_irq` 实现；尚未接入 PS。
- EN=1 时，`I_done`、任一特征 valid 或阈值超限置 IRQ.DONE；IRQ_EN=0 时保留标志，首次解除屏蔽补发一次脉冲。
- pending 未清期间合并后续事件；PS 写 `IRQ[0]=1` 后重新允许脉冲。清除与事件同拍时事件优先，并重新通知；软复位清除标志。硬件复位及软复位优先于事件。
- §5 IRQ 号与上升沿配置须在 SoC 接入后按 `Design.xml` 确认。
- §4.4 的 DMA 通道：单次模式用 `DMA_CTRL.DONE` / `DMA_STATUS.ERRCODE` 轮询；环形模式按 `DMA_BLK_SEQ` 轮询完成进度（fabric 中断线不可用，见 §4.4）。

---

## 6. DMA（FDMA / VDMA）

| 项 | 值 |
|---|---|
| 方向 | PL→DDR（S2MM） / DDR→PL（MM2S） |
| 缓冲区物理地址 | `0x3EF0_0000`，大小 1 MiB，`no-map` 预留区（节点 `sensor-dma-buffer@3ef00000`）；避开 CMA 与 U-Boot 常驻区，见 §4.4 |
| 帧长 / 对齐 | `4096 B` / 64 B 对齐 |
| 缓冲块数（环） | `8` |
| 描述符格式 | TBD（VDMA 寄存器 / FDMA 自定义寄存器） |
| 完成通知 | S2MM Done → 中断（§5） |
| 参考例程 | `04_培训demo/01_fdma_vdma_20251010/*` |

- 缓冲区地址必须与 PS 侧 Linux 的 reserved-memory / CMA 约定一致；否则 DMA 写入系统内存将导致异常。
- §4.4 定义本版 PL→DDR 的采集样本流通道（`pl_ctrl` 窗口 `0x90～0xB4`，HP1 写主口，块完成可中断或轮询）；上表为 FDMA / VDMA 参考参数，与 §4.4 不共用寄存器。

---

### 6.1 NPU 与采集共用 DDR

`soc_hdmi` 由 [`prepare_hdmi_soc.py`](../fpga/soc/prepare_hdmi_soc.py) 与 [`attach_npu.py`](../fpga/soc/attach_npu.py) 生成，软核使用厂商 NPU 演示工程的 `design_0_Video_NPU_0` 加密 RTL。软件模型 ABI、采集寄存器 ABI 与 DMA 记录格式各自独立；本接入不改变 v1.2 的采集字段。

| 通路 | 接入与规则 |
|---|---|
| HDMI VDMA | 独占 HP0，保持厂商显示通路 |
| 软 NPU 控制 | PS GP1 → 厂商 `AXI_Protocol_Converter_1` → AXI4-Lite，33 bit 地址、32 bit 数据 |
| HP1 读 | 软 NPU 直接接入，33 bit 地址、64 bit 数据、6 bit ID、最多 16 拍突发 |
| HP1 写 | 传感器 DMA 与软 NPU 经 [`npu_hp1_write_mux.sv`](../fpga/uisrc/01_rtl/soc/npu_hp1_write_mux.sv) 仲裁；一个未完成写事务，按突发轮转，AW 选中后持有来源直到 B 握手；W 不交错，BID/BRESP 返回原来源，保留 NPU 全部 6 bit ID |
| 时钟 / 复位 | GP1、转换器、软 NPU S/M、仲裁器及 HP1 共用 PLL0 100 MHz 与 `peripheral_aresetn`，不新增 AXI CDC |
| 软 NPU 中断 | `f2p_irq[15]`，设备树 `<0 97 4>`，SPI 97 / GIC 129，按厂商演示工程接线；实板已注册，FF6 回放时计数为 0，软算子触发待验收 |
| 内存隔离 | 传感器使用 `0x3EF00000` / 1 MiB `no-map`；NPU 使用低 512 MiB 内的 128 MiB CMA；HDMI 帧缓冲沿用厂商设备树 |
| 软件依赖 | `/dev/hard_npu`、`/dev/soft_npu`、`/dev/cma_mem` 全部存在后才自动选择混合后端；FF6 六分支在硬核执行，厂商 ArmNN 初始化仍需软核设备 |
| IP 配置 | 通用软算子 Pooling / Activate / Upsample / Route / Shortcut 保留；厂商可选的 YOLO 后处理与图像 Resize 关闭，FF6 不使用这两类图像算子；加密 RTL 原件不修改 |
| 配对校验 | TD 成功生成码流后写 `soc_hdmi.npu.sha256`；设备树与 BOOT 打包检查该摘要。禁止将启用软 NPU 的设备树与缺少该 IP 的码流组合 |

RTL 仲裁已通过双来源、背压、16 拍突发、ID、SLVERR 与复位自检。合并镜像已完成两轮 184 窗 FF6 混合回放、CMA 释放及 HDMI / 采集 DMA 基础共存验证，见 [实板结果](../fpga/uisrc/06_doc/验证结果.md) §5.6。FF6 分支使用硬核，软核 probe 成功但其数据面和中断未由该模型激励，仍须单独压力测试；长期并行稳定性待定。

## 7. 时钟 / 复位

| 项 | 值 |
|---|---|
| PL 主时钟源 | 底板 100 MHz → BANK33 GC 差分对（`PL_CLK_100M`） |
| PLL 输出 | TBD（如 100 / 50 MHz） |
| 复位 | 低有效；来源 TBD（PS 软复位 / 外部按键 `PS_SRST_B` / PL 自复位） |
| 跨时钟域 | PL↔PS 的 AXI 时钟域 TBD；CDC 用同步器 / 异步 FIFO |

---

## 8. 数据格式 / 标定

PL 侧四路采集的原生数据格式：

| 通道 | 数据 | 位宽 / 编码 | 标定 |
|---|---|---|---|
| 温度 | 温度值 | signed 16，单位 0.1 ℃ | 物理值 = 代码 × 0.1 ℃ |
| 振动 | X / Y / Z 三轴 | signed 16 × 3，器件原始补码 | 全分辨率 1 LSB = 3.9 mg |
| 电流 | ADS1115 连续转换采样 | signed 16，器件原始补码，DR 860 SPS | PL 只输出原始码；换算公式与系数寄存器见 §4.3；ACS712-20A 的灵敏度 / 标称零点为 100 mV/A / 2.5 V；实际零点需停机归零 |
| 时间戳 | 样本接收时间 / 对齐帧时间 | unsigned32；样本 10 ns/LSB，帧 1 ms/LSB；模 2³² 回绕 | 共同时基；温度准同步；读取与复位规则见 §4.3 |
| 转速 | OID-R3808B-12S1S，Modbus RTU `0x0020～0x0021` | `raw_speed` signed32；`rpm_milli` signed32，0.001 rpm/LSB | `rpm_milli=trunc_toward_zero(raw_speed×60000000/(4096×100))`；4096 计数/圈，测速采样时间 100 ms；准确度待参考源验证 |

- 统一寄存器与小端字段见 §4.3；配置采样率为振动 3200 SPS、电流 860 SPS。电流标定系数 `CURRENT_ZERO` / `CURRENT_GAIN`（§4.3）、霍尔模块为 ACS712-20A；实际采样准确度为 TBD。
- 转速总线为 9600 8N1、站号 1、查询模式；上电 FC06 写 `0x000A=100`（ms），随后 FC03 读取两字速度。事务间等待 100 ms，不等于完整采集周期。`raw_speed` 为测速采样时间内计数变化；换算向零截断，超出 signed32 时饱和并置 `overflow`。正负号沿用编码器计数方向。
- 转速独立遥测为 115200 8N1、20 字节、CRC16/Modbus，整数大端；帧表及错误码见 [`RS485转速通道接入.md`](RS485转速通道接入.md) §4 / §5。该串口协议独立于 PS 控制台和 AXI / DMA 小端格式，遥测版本为 1。
- 转速在接口 v1.2 有独立只读寄存器窗口 `0xB8～0xD0`（§14）、独立 DMA 记录 `CH=3/4/5`（§4.4）与 PL 共同时基接收时间戳；signed32 取值不截断、不复用 signed16 样本字段。错误事件清 `online`、保持最近有效速度，成功计数只在正确应答后增加；最近有效值在复位后清 0，不跨复位保留。
- 电流参考标定：霍尔模块灵敏度、零点见 [`传感器板-说明.md`](传感器板-说明.md)。
- DMA 样本流字格式（`{ts, ch, sample}`，64 bit 小端）见 §4.4。

---

## 9. 物理层 / 管脚（外设 → PL）

传感器经底板 `J12`（PL IO / BANK11，3.3 V）进 PL；网络与球号取自核心板 pinlist（`MLK-F3P-DR1M90G-Resource/…/PL-GPIO连接器管脚定义表.xlsx`）。`J12.1` = `+3V3`，`J12.2` = `GND`，`J12.3 ~ 26` = `CEP1_1P … CEP1_12N`（共 24 路 IO）。温度、振动、电流三路已实现，各路独立占用 J12 引脚：

| 通道 | 信号 | 方向(PL) | J12 脚 | 网络 | 球号 |
|---|---|---|---|---|---|
| 温度 GT-M601 | UART RX | 输入 | 5 | `CEP1_2P` | T9 |
| 振动 ADXL345 | SPI SCLK | 输出 | 3 | `CEP1_1P` | Y12 |
| 振动 ADXL345 | SPI SDIO | 双向 | 4 | `CEP1_1N` | Y13 |
| 振动 ADXL345 | SPI CS_N | 输出 | 6 | `CEP1_2N` | U10 |
| 振动 ADXL345 | INT1 | 输入 | 7 | `CEP1_3P` | T5 |
| 电流 ADS1115 | I2C SCL | 开漏 | 9 | `CEP1_4P` | V6 |
| 电流 ADS1115 | I2C SDA | 开漏 | 10 | `CEP1_4N` | W6 |

- 全部 J12 信号 `LVCMOS33`。双向线（SPI SDIO、I2C SCL / SDA）在 PL 侧关闭内部上下拉（`PULLTYPE = NONE`），高电平由模块侧上拉提供。
- **转速通道（RS485）不经 J12**，走板载 RS485 接口（PL，3.3 V，收发器 `SP3485EN` / `U21`，连接器 `J11`）。PL 侧信号与球号（`PL-RS485 管脚定义表`）：

| 信号 | 方向(PL) | 球号 | 说明 |
|---|---|---|---|
| `O_rs485_tx` | 输出 | L17 | 发往收发器 DI |
| `I_rs485_rx` | 输入 | N16 | 来自收发器 RO |
| `O_rs485_de` | 输出 | N15 | 方向：`1`=发送，`0`=接收 |
| `O_uart_tx` | 输出 | J16 | PL USB UART 遥测，115200 8N1，LVCMOS33 |

- 编码器红线接外部稳压 +5 V，黑线接 J11.4 GND、白线接 J11.5 A、绿线接 J11.6 B，黄线悬空绝缘；外部电源负极与板端共地。
- RS485 半双工：PL 用 `O_rs485_de` 切换收 / 发；板载 `R120` 已提供本端 120 Ω 终端，远端视传感器补 120 Ω。接入设计见 [`RS485转速通道接入.md`](RS485转速通道接入.md)。
- 板载 CAN（备用）为 PS 硬核，I/O 经 PL 引出：`CAN0TX_3V3`=F20、`CAN0RX_3V3`=F19（收发器 `SN65HVD232` / `U22`）。CAN 与 RS485 引脚不重叠、可共存；本方案转速走 RS485。
- 传感器载板 `P1` / `P2` 与 `J12` 逐脚并联，网络一致（`P1.3` / `P2.3` = `J12.3` = `CEP1_1P`，其余类推）。
- 约束：单端 3.3 V，短线；电机电流 / 高压不得引到这些脚（只走信号）。
- PL 基础引脚约束见 [`fpga_pin.adc`](../fpga/uisrc/04_pin/fpga_pin.adc)；独立采集顶层的 RS485 / PL UART 附加约束见 [`encoder_pin.adc`](../fpga/uisrc/04_pin/encoder_pin.adc)。SoC 尚未加载转速附加约束。
- 统一顶层 LED1～LED4 为心跳 / 温度在线 / 振动 DEVID 通过 / 电流错误；转速状态由 PL UART 输出。LED5 为 PS MIO51，不由独立 PL 顶层驱动。

---

## 10. 接口定义方式

- 地址 / 寄存器 / 位域 / 版本均以本文件为准；两侧头文件据此生成或对照。
- PS 侧头文件宏摘录（值以 §3/§4 为准，宏名可调整）：

```c
/* 寄存器宏摘录（示意；PS 侧按本文件定义） */
#define PL_CTRL_BASE     0x80000000u          /* design integrator 分配，以 Design.xml 为准 */
#define PL_CTRL_MAGIC    0x504Cu
#define PL_CTRL_VERSION  0x0100u
#define PL_CTRL_ID       (PL_CTRL_BASE + 0x00u)
#define PL_CTRL_CTRL     (PL_CTRL_BASE + 0x04u)
#define   PL_CTRL_CTRL_EN      (1u << 0)
#define   PL_CTRL_CTRL_RST     (1u << 1)
#define   PL_CTRL_CTRL_IRQ_EN  (1u << 2)
#define PL_CTRL_STAT     (PL_CTRL_BASE + 0x08u)
#define PL_CTRL_DATA     (PL_CTRL_BASE + 0x0Cu)
#define PL_CTRL_IRQ      (PL_CTRL_BASE + 0x10u)
#define PL_CTRL_CURRENT_ZERO  (PL_CTRL_BASE + 0x4Cu)   /* signed32, ADS1115 code at 0 A */
#define PL_CTRL_CURRENT_GAIN  (PL_CTRL_BASE + 0x50u)   /* signed32, uA per LSB */
```

- 改接口流程见 §12。

---

## 11. 最小连通测试

接口冻结前必须通过以下 3 项：

- [ ] PS 写 `CTRL[0]=1` → PL 有反应（点灯 / `STAT.BUSY` 变化）
- [ ] PL 端计数 → PS 读 `DATA.CNT` 递增
- [ ] PL 发一次 IRQ → PS 收到并计数 +1，且能写 `IRQ[0]=1` 清掉

联调顺序：

1. PL 单独验证（ChipWatcher 看波形）
2. PS 用 `devmem` 手工读写寄存器（系统不崩溃、寄存器值正确）
3. 写字符设备驱动（`mmap` 寄存器 / `request_irq`）
4. 连通 DMA（§4.4 HP1 通道或 §6 FDMA / VDMA），核对数据长度 / 内容
5. 应用层：读数据 → 处理 → UDP 发上位机 / HDMI 显示 / 存 SD
6. 上位机先用 Python `socket` 打点，再做 UI

---

## 12. 版本与变更规则

- 冻结后只准「加」：加寄存器、加位域、加 IP。
- §4.3 同步采集 `0x54～0x74` 与 §4.4 DMA 寄存器 `0x90～0xB4` 均为新增，属于「加」；已有偏移与语义保持。
- 本版接口版本 v1.3，`ID=0x504C0103`，新增 §16 的 `ALARM_STATE(0xD4)`、`ALARM_STATUS(0xD8)`。既有采集偏移、位域、频谱定标、转速窗口与 DMA 记录格式不变。新版驱动保留 v1.2 采集兼容，旧驱动不接受 v1.3；声光功能须 PL、驱动及应用同时更新。
- 不准改已有偏移或语义；破坏性改动 → 升版本（v1.x → v1.y 或 v2.0）→ 双方确认 → 两端同步实现。
- 变更记录：

| 版本 | 日期 | 改动 | PS | PL |
|---|---|---|---|---|
| v0.1 | | 初稿模板 | | |
| v1.0 | | 冻结（§11 三条通过） | | |
| v1.1 | | §4.6 `BIN_POWER` 定标 `>>16` → `>>8`，`ID` → `0x504C0101` | 同版本 | 同版本 |
| v1.2 | | §14 新增转速寄存器 `0xB8～0xD0`、§4.4 新增转速 DMA 记录 `CH=3/4/5`，`ID` → `0x504C0102` | 同版本 | 同版本 |

---

## 参考资料

| 主题 | 路径 |
|---|---|
| FPSoC SDK 入门 / 高级 | `docs/MLK-F3P-DR1M90G-Resource/03_demo/3-0_book_all/3_2_01_…入门篇.pdf` / `3_2_02_…高级篇.pdf` |
| PS↔PL 寄存器 + 中断例程 | `docs/MLK-F3P-DR1M90G-Resource/03_demo/3-2_ex_fpsoc/3_2_01_sdk_base/13_sdk_pl_intr`、`14_axi_gpio_pl_intr` |
| Linux 驱动开发 | `docs/MLK-F3P-DR1M90G-Resource/03_demo/3-4_ex_soc_linux/ebook/3-4-02_…Linux驱动开发篇.pdf` |
| FDMA / VDMA | `docs/MLK-F3P-DR1M90G-Resource/04_培训demo/01_fdma_vdma_20251010/*` |
| UDP 上位机通信 | `docs/MLK-F3P-DR1M90G-Resource/03_demo/3-0_book_all/3_1_02_…UDP以太网通信方案.pdf` |
| 硬件手册（外设 / 管脚 / 模式开关） | `docs/MLK-F3P-DR1M90G-Resource/02_hardware/hardware_MLK_F3P_CZ02_DR1_20250526/01_硬件手册/…硬件使用手册.pdf` |
| 传感器板 | `docs/传感器板-说明.md` |

## 13. Linux 字符设备接口（新增）

实现目录为 `../host/ps-local/driver/`，用户态声明为 `../host/ps-local/src/pl_host.h`。硬件地址与 IRQ 由设备树资源提供，禁止把 REF IRQ 当作 Linux 虚拟中断号。

| 操作 | 规格 |
|---|---|
| 节点 | `/dev/pl_ctrl`，权限 0600 |
| 寄存器读取 | `pread(fd, buf, size, offset)`；offset、size 为 4 的倍数；窗口 `0x000～0x1FF`（含 §14 转速窗口 `0xB8～0xD0`）与完整频谱窗口 `0x400～0x7FF`（`N=512` 构建为 `0x400～0xBFF`，`BIN_POWER[k]=(|X_k|²)>>8`）；未定义偏移返回 EINVAL；每字使用 readl |
| 版本匹配 | 新版驱动接受 v1.3 `0x504C0103` 与采集兼容 v1.2 `0x504C0102`；告警要求 v1.3，其他值返回 ENODEV |
| 寄存器写入 | `pwrite(fd, &word, 4, offset)`；仅 CTRL、STAT、IRQ、FEATURE_SNAPSHOT、三项阈值与两项标定；每字使用 writel，读回刷新 posted write |
| 中断通知 | poll(POLLIN)；`PL_CTRL_IRQ_COUNT` ioctl 返回本设备累计 IRQ 次数并更新当前文件的已读序号；每个打开文件独立消费 |
| IRQ 处理 | request_irq 使用设备树触发类型；读 IRQ.DONE 后 W1C 并读回，再更新计数并唤醒 poll |
| 设备移除 | 新操作返回 ENODEV，poll 返回 POLLHUP；打开句柄保留至 close |
| DMA | 未接入时返回 EOPNOTSUPP；寄存器读取不等同于 DMA 带宽验证 |

§16 的 `ALARM_STATE` 可写，`ALARM_STATUS` 只读；仅 v1.3 放行。

§4.4 的 DMA 寄存器已按「加」规则列入写白名单：`DMA_CTRL`、`DMA_BASE_LO` / `DMA_BASE_HI`、`DMA_BYTES`、`DMA_IRQ`、`DMA_BLOCKS` 可写，`DMA_STATUS` / `DMA_WRITTEN` / `DMA_BLK_SEQ` / `PL_TS_NOW` 只读；写入语义以 §4.4 为准。

Qt 本地显示只接受驱动实际读数；温度按 0.1 ℃，三轴按原始码与 0.0039 g/LSB 显示。通道计数须实际变化才判定有数据，温度 6 s、振动 500 ms 未更新显示不可用。电流默认按 ACS712-20A 系数换算，未标定或通信 / 采样异常时显示原因；转速面板显示寄存器窗口的最新值，`online=0` 或读取超时显示不可用；NPU 无有效推理结果时显示不可用。不得从演示、随机数、计数器推导物理测量或推理结果。

## 14. SoC 接入层

寄存器与互联在 PS `p2f_clk0`（模板请求 50 MHz）域运行；采集、时域、FFT、转速采集和唯一共同时基在板载 100 MHz 域运行。验证状态见 [`fpga/uisrc/06_doc/验证结果.md`](../fpga/uisrc/06_doc/验证结果.md) §1（仿真）、§3.2（合并码流时序与资源）、§5.5（上板）。

样本事件与特征通过 req/ack 邮箱传输；源端持有完整数据直至确认，忙期间新事件置溢出，不逐位同步二进制样本。1 kHz 帧作为完整记录跨域，寄存器本体使用外部帧模式；原独立单时钟模式语义保持。

| 新增偏移 | 名称 | 访问 | 规格 |
|---|---|---|---|
| `0x78` | FFT_SEQUENCE | RO | 每个完整振动 X 轴 FFT 窗口递增，硬 / 软复位清零 |
| `0x7C` | FFT_PEAK_BIN | RO | 单边谱 `1～N/2` 中功率最大 bin；相同功率取较小 bin；全零取 0 |
| `0x80 / 0x84` | FFT_PEAK_POWER | RO | unsigned64 Q16；`real²+imag²`，core 输出 X[k]/N、signed32 Q8；不作单边谱倍增 |
| `0x88` | FFT_FLAGS | RO | bit0=窗口去 DC 时发生 signed16 饱和；bit1=窗口 / 输出丢失；软复位清零 |

FFT 取振动 X 原始样本，连续 N 点组成不重叠窗口，编译期 N=256/512、WINDOW=0 矩形或 WINDOW=1 周期 Hann；先求 signed 算术均值（向负无穷取整），逐点减均值并饱和到 signed16，再输入现有 `fft_radix2`。核心数值语义与原始流、时域窗口保持。结果读法为 SEQUENCE → BIN/POWER/FLAGS → SEQUENCE，相同才接受。完整逐 bin 功率经 §4.6 的频谱窗口读出；DMA 输出未实现。

#### 转速通道（v1.2 新增）

`encoder_modbus` / `encoder_rpm` 例化在采集域（`I_sysclk`，100 MHz），输出 `raw_speed`、`rpm_milli`、`sample_count`、`online`、`overflow`、`error_valid`、`error_code`、`exception_code`。寄存器窗口与 DMA 记录在 AXI 域读取；跨域使用一次 `cdc_mailbox` 传递整条 192 bit 快照，源端在确认前持有，忙时新事件置溢出计数，不逐位同步二进制样本。

| 偏移 | 名称 | 访问 | 复位值 | 说明 |
|---|---|---|---|---|
| `0xB8` | SPEED_RAW | RO | 0 | signed32，最近一次成功采样的编码器计数变化，符号沿用编码器计数方向 |
| `0xBC` | SPEED_RPM_MILLI | RO | 0 | signed32，0.001 rpm/LSB，`trunc_toward_zero(raw×60_000_000/(4096×100))` |
| `0xC0` | SPEED_STATUS | RO | 0 | bit0 在线；bit1 换算饱和；bit2 最近事件为错误；bit3 FC06 采样时间已配置；`[7:4]` 错误码；`[15:8]` Modbus 异常码；其余位为 0 |
| `0xC4` | SPEED_COUNT | RO | 0 | unsigned32，成功采样累计数，只在该次应答正确后加一 |
| `0xC8` | SPEED_TS | RO | 0 | unsigned32，样本被采集域接收的时间戳，10 ns/LSB，共同时基 |
| `0xCC` | SPEED_SEQUENCE | RO | 0 | unsigned32，每收到一个转速事件（成功采样或错误事件）加一；快照与序号同拍锁存 |
| `0xD0` | SPEED_ERR_COUNT | RO | 0 | unsigned32，错误事件累计数 |

读取一致性：读 `SPEED_SEQUENCE` → 读其余字段 → 再读 `SPEED_SEQUENCE`，两次序号相同才接受；不同表示读取期间有新事件，重读。`[7:4]` / `[15:8]` 只在 `SPEED_STATUS.bit2=1` 时有效。

语义：

- 采集域的 Modbus 主站自由运行，只受硬复位控制；`CTRL.EN=0` 冻结寄存器窗口与 DMA 记录的更新，不冻结编码器轮询。`SPEED_COUNT` / `SPEED_ERR_COUNT` 是 PL 侧累计量，重新使能后的第一个事件带回使能期间继续累加的计数。轮询不随 `CTRL.EN=0` 停止，以保持 100 ms 交换节奏与在线判定连续，避免一次配置切换被记成超时事件。
- `CTRL.RST` 或软复位把寄存器快照清零（七个转速偏移读 0，`SPEED_STATUS=0`），PL 侧 Modbus 主站与累计计数不重新开始，因此复位后的第一个事件带回运行中的累计计数与在线状态。最近有效值不跨复位保留。
- 错误事件（超时 / CRC / 结构 / UART / Modbus 异常 / t1.5 / RX 溢出）清 `online`、保留最近一次成功的 `SPEED_RAW` / `SPEED_RPM_MILLI` / `SPEED_TS`，`SPEED_SEQUENCE` 与 `SPEED_ERR_COUNT` 加一，`SPEED_STATUS.bit2=1`。
- 换算饱和时 `SPEED_RAW` 仍为真实计数变化，`SPEED_RPM_MILLI` 饱和到 signed32 边界并置 `SPEED_STATUS.bit1`。

DMA 记录：每个转速事件写三个 64 bit 字，写成顺序固定为 `CH=3`、`4`、`5`。三字的 `TS` 字段复用为 32 bit 载荷（第 1 字仍是事件时间戳），`SAMPLE` 字段承载状态或保留字：

| 字 | `[63:32]` | `CH[31:16]` | `[15:0]` |
|---|---|---|---|
| 0 | 事件时间戳，10 ns/LSB | `0x0003` | `SPEED_STATUS` 的低 16 位（与寄存器同一位图）：bit0 在线；bit1 饱和；bit2 本次事件为错误（不携带新样本）；bit3 已配置；`[7:4]` 错误码；`[15:8]` 异常码 |
| 1 | `rpm_milli[31:0]` | `0x0004` | 0（保留） |
| 2 | `raw_speed[31:0]` | `0x0005` | 0（保留） |

- 一条记录固定三字，不与振动 X/Y/Z 帧交错：发送转速记录前须等当前振动帧三字发完，转速记录本身也不被振动样本打断；DMA 空闲时转速记录先落盘。
- PS 侧按 `CH=3` 起始、连续三字成一条记录；缺字、乱序或 `CH` 不匹配时丢弃该记录并计入丢弃计数，不把 `SAMPLE` 当振动样本处理。
- 错误事件也写记录（状态字 bit2=1，`rpm_milli` / `raw_speed` 仍为最近一次成功值），因此 `DMA_WRITTEN` 中的记录数等于事件数，不随错误事件跳过；PS 侧只把 bit2=0 的记录计入新样本。
- 状态字直接取 `SPEED_STATUS[15:0]`，寄存器与 DMA 共用一套位图，`[7:4]` 错误码、`[15:8]` 异常码在两个宽度下位置一致。
- 新事件到达时上一条记录仍未发完，本次事件不写记录，置粘滞丢弃位；该位并入 `DMA_STATUS.overflow`（与振动打包溢出同一计数），只在硬复位或软复位清除。`DMA_STATUS.overflow` 因此同时覆盖两路打包溢出，PS 侧按位判断「有记录丢失」即可。

验证状态：RTL 仿真（正 / 负 / 零速、错误恢复、跨域与时延、寄存器读一致、DMA 三字记录）见 `../fpga/uisrc/06_doc/验证结果.md`；合并码流的时序、资源与实板寄存器 / DMA 读数见同文件。绝对准确度仍需参考源。

### 4.6 完整频谱窗口（新增）

当前接口版本 v1.3，`ID` / `VER` 为 `0x504C0103` / `0x0103`；已有偏移与位域不变。

| 偏移 | 名称 | 访问 | 规格 |
|---|---|---|---|
| `0x400 + 4k` | BIN_POWER[k] | RO | `k = 0…N-1`；`(|X_k|²) >> 8`，unsigned32；未写入的 bin 读 0 |

- 与 `0x80` 的 `PEAK_POWER` 同源同定标（`real²+imag²`，core 输出 `X[k]/N` signed32 Q8），仅去掉低 8 位；还原 `|X_k|² = BIN_POWER[k] << 8`，`|X_k| = sqrt(BIN_POWER[k] << 8)`。量化步长为 `|X|²` 原始单位的 256，原始值低于 256 的 bin 读 0；静止时最强 bin 的 `|X|²` 约 2^11，对应读数约 8。
- 有效量程：ADXL345 ±16 g、256 LSB/g 时 `|X|²` 最大约 2^39，`>>8` 后约 2^31，unsigned32 不溢出；`0xFFFF_FFFF` 对应 `|X|² ≈ 2^40`。
- 当前构建 `N=256`（`LOGN=8`），窗口 `0x400～0x7FF`；`N=512` 构建为 `0x400～0xBFF`。窗口外偏移返回 EINVAL。
- 一窗 FFT 的 bin 在核心 `valid` 阶段逐拍写入；`0x78 FFT_SEQUENCE` 加一表示该窗 bin 已全部写完。读法：读 `0x78` → 读 bin 数组 → 再读 `0x78`，两次相同才采用，不同说明读取期间被新窗口覆盖，需重读；`0x88 FFT_FLAGS` 属于同一窗。
- 频谱窗口不经过 §4.4 的 DMA 通道，读带宽受 AXI-Lite 限制，适合每窗读取一次，不适合逐块连续搬运。

FAULTS 接入层分配：bit0 温度错误、bit1 ADXL345 自检不可用、bit2 ADS1115 错误、bit3 时域输入 overrun、bit4 CDC 传输丢失；其余保留。FAULTS 为当前状态，传输丢失在接入层保持至硬复位。

`CTRL.RST` 经握手送达采集域，清特征与帧；自由时基和 tick 相位保留；软件复位后应等 FRAME_TS 与通道计数重新变化再接受数据。地址 / 寄存器最小连通已在实板通过；IRQ、DMA（§4.4）与生产 HPF / FSBL 仍需分别完成并确认后冻结。

---

## 15. 扩展约定：新增传感器通道

新增一路传感器的接入路径、通道选择与资源预算由本规程固定；四路之外的通道按本规程加入，不改动既有窗口偏移与语义。

### 15.1 接入路径

| 步骤 | 承担内容 | 约束 |
|---|---|---|
| 1 PL 采集块 | 器件协议（UART / I²C / 计数 / 比较）、定长样本或整帧 | 自带慢时钟域；不进入 100 MHz 特征 / FFT 域 |
| 2 跨域 | 单 bit 翻转沿或 req/ack 邮箱（`cdc_mailbox.sv` / `cdc_event_fifo.sv`） | 源端持有数据至确认，忙时置溢出；同一存储不得跨两个时钟域读写（§4.6） |
| 3 寄存器块 | 新增只读窗口偏移与需要的标定 / 配置偏移 | 复用 §4.3 的写掩码、响应与 `CTRL.EN` / `CTRL.RST` 语义 |
| 4 驱动与用户态 | `pl_readable` 白名单放开新偏移，`pl_host` 增加读取函数，`plctl` 增加子命令 | 见 §13 |
| 5 显示 | `datasource` 增加通道；`KLocalWindow` 的四通道面板已建好 | 无数据时显示不可用，不从演示或计数器推导读数 |

### 15.2 通道选择

| 数据形态 | 通道 | 判据 |
|---|---|---|
| 采样率不高于 1 kHz，或按整帧读取 | 寄存器窗口 + 信箱 | 读取周期在 10 ms 量级时不需要块搬运 |
| 连续样本流（如振动 3.2 kHz × 3 轴） | §4.4 DMA 环 | 4 KiB 块，块周期由采样率决定 |

DMA 的 `BASE` / `BYTES` / `BLOCKS` 是运行时寄存器。新增第二路连续流时，可给第二路独立基址与环，或增加第二个 `sensor_dma` 例化（约 1k LUT 量级）。

### 15.3 预算

| 项 | 当前码流实测 | 说明 |
|---|---|---|
| DMA 带宽 | 75.05 KB/s | HP1 ramp 上限 406.35 MB/s，占用约 0.02% |
| 逻辑 | 20512 slices / 52480（39.09%） | 含 HDMI 显示、VDMA 与转速采集 |
| DSP | 171 / 240（71.25%） | 特征、FFT 与转速换算使用；UART / I²C 控制器不占 DSP |
| ERAM | 39 / 280（13.93%） | |
| 100 MHz 域余量 | SWNS 0.019 ns / HWNS 0.020 ns | 见 §15.4 |

上表为含转速通道的合并码流实测（`soc_hdmi`，STA 覆盖率 99.91%，0 违例端点）。相对接入转速前（18911 slices、169 DSP），转速采集与换算增加约 1601 slices 与 2 个 DSP；DMA 带宽增量来自每 100 ms 一条 24 B 转速记录，相对四路连续流可忽略。

UART、I²C 与计数控制本身不占 DSP / ERAM；转速数值换算须另计资源。独立四路 `fpga_top` 实测使用 DSP 2、ERAM 0，转速换算的 DSP 占用为 2。该数据不代表接入 HDMI / 特征后的 SoC 资源与时序，合并后须重新构建确认。

### 15.4 约束

- 不向 100 MHz 域增加组合逻辑：合并码流的建立裕量为 0.019 ns、保持裕量 0.020 ns，关键路径是特征单元的 DSP 乘法累加链。转速采集与换算已按寄存器切分放在该域内；新增传感器逻辑优先放自己的时钟域，按 §15.1 第 2 步跨域。
- 寄存器只加不改：新增偏移须在 §13 放行、在 §12 记录，并升 `VER`；`ID` 随接口版本更新（当前 `0x504C0103`）。
- 位流与 rootfs 同步：新码流要重建 `BOOT.bin`（`platform/flash/build_boot_bin.sh`）与 rootfs（`build_rootfs.sh`），码流同时放入 `rootfs-overlay/lib/firmware/`。

### 15.5 已有通道现状

| 通道 | PL | 缺口 |
|---|---|---|
| 温度 | 已实现（UART，`0x20`） | — |
| 振动 | 已实现（SPI，`0x24～0x2C`） | — |
| 电流 | 已实现（ADS1115，`0x30`） | 霍尔默认零点 / 灵敏度为 20000 / 1250，PS 启动恢复并回读核对；实测标定待验收 |
| 转速 | 统一 `fpga_top` 已实现并串口实板验证；SoC 采集层 / 寄存器 / DMA / 驱动 / 应用已按 v1.2 接入（§14） | 绝对准确度待参考源（编码器分辨率 4096 计数/圈）；正负号沿用编码器计数方向 |

电流通路使用 ACS712-20A 标定值：`CURRENT_ZERO = V₀ / 125 µV`、`CURRENT_GAIN = 125 / S`（µA/LSB），默认 20000 / 1250；停机归零写入并保存实际零点，不改 RTL 与寄存器接口。转速通道的 PL 侧设计见 [`RS485转速通道接入.md`](RS485转速通道接入.md)。

## 16. U7 声光告警（v1.3）

| 偏移 | 名称 | 访问 | 复位值 | 字段 |
|---|---|---|---|---|
| `0xD4` | ALARM_STATE | RW | 0 | [3:0] 温度/电流/振动/转速越界；[5:4] 模型告警等级 0～3；其余为 0 |
| `0xD8` | ALARM_STATUS | RO | 0 | [0] U7 电平、[1] 播放中、[2] 模型抑制、[3] 心跳超时；[11:8] 待播通道、[15:12] 当前通道；其余为 0 |

WSTRB[0] 有效的状态写入续期 2 秒，相同状态不重新触发。PS 以 250 ms 周期写入有效状态；超时、硬复位和 CTRL.RST 均清状态、队列与输出。通道序列为 1/2/3/4 次 1 秒高电平、间隔 1 秒；模型为 3/6/9 秒连续高电平并优先抢占。模型等级非 0 时抑制通道播放；健康解除及重触发规则见[声光告警与模型故障 API](声光告警与模型故障API.md)。

输出 `O_alarm` 约束至 J12.11/U7、LVCMOS33、高有效。计数使用 AXI 域时钟，合并码流为 100 MHz，SoC 寄存器演示为 50 MHz，参数 `AXI_CLK_FREQ_HZ` 随集成显式设置。独立 `fpga_top` 将该脚固定低电平。驱动仅放行状态寄存器写入，状态字只读；模型程序使用 PS 模型 API，不与应用争用寄存器。
