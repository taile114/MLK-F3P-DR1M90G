# fpga — PL 端（FPGA）

本目录是 MLK-F3P-DR1M90G 的 PL 统一工程：把温度、振动、电流、RS485 转速四路传感器采集 RTL 集结在一个 TD 工程（顶层 `fpga_top`）下，共用底板 100 MHz 时钟与上电复位。独立顶层把转速经 PL USB UART 输出，四路统一码流已完成 JTAG 与串口实板验证；SoC 版本（`soc_top` 与 HDMI 合并 `soc_hdmi_top`）另把转速经只读寄存器窗口 `0xB8～0xD0` 与 DMA 记录 `CH=3/4/5` 接入 PS，接口 v1.3（转速格式保持 v1.2），见 [`../docs/PL-PS接口约定.md`](../docs/PL-PS接口约定.md) §14。

`soc/prepare_hdmi_soc.py` 生成包含 HDMI、采集与厂商 `Video_NPU` 的合并工程，GP1 接软 NPU 控制口，HP1 写通路由软 NPU 与采集 DMA 按完整突发仲裁。FF6 不使用的 YOLO 后处理与图像 Resize 配置关闭；通用软算子保留。接线、内存与中断约定见接口契约 §6.1；内核、设备树、模型运行库与 rootfs 配套构建见 [`../platform/flash/README.md`](../platform/flash/README.md)。该合并镜像的启动与三类通路共存须实板验收。

参考：
- 接口契约（地址 / 寄存器 / 中断 / DMA / 版本）：[`../docs/PL-PS接口约定.md`](../docs/PL-PS接口约定.md)
- 开发与交接流程：[`../docs/PS-PL开发流程.md`](../docs/PS-PL开发流程.md)
- 本板硬件与管脚：[`../docs/传感器板-说明.md`](../docs/传感器板-说明.md)
- NixOS 下 TD / Questa：[`../docs/NixOS下TD与Questa使用.md`](../docs/NixOS下TD与Questa使用.md)

协作约定：
- 改接口先改 `docs/PL-PS接口约定.md`，双方确认后升版本。
- 寄存器表与地址以接口契约为准，PL、PS 两端各自实现。

## 1. 工程结构

```
fpga/
├── README.md
├── TODO.md
├── .gitignore                   TD 编译产物与仿真产物
├── .gitattributes               保留厂商 UART 核原始字节
└── uisrc/
    ├── fpga.al                  TD 工程，顶层 fpga_top，器件 DR1M90GEG400 / -2
    ├── 01_rtl/
    │   ├── fpga_top.v           集成顶层：复位、四路采集、时基与帧、特征、LED、遥测
    │   ├── timebase.v           100 MHz 共同时基、接收时间戳、1 kHz 对齐帧
    │   ├── uart_rx_guard.v      温度：UART 同步与错误处理
    │   ├── temp_frame_parser.v  温度：ASCII 帧解析，输出 0.1 ℃
    │   ├── spi_adxl345.v        振动：三线 SPI 主机
    │   ├── adxl345_ctrl.v       振动：器件配置与逐样本读取
    │   ├── encoder_*.v        转速：UART、Modbus RTU、rpm 换算、遥测
    │   ├── features/            四路时域、开方/除法、快照保留
    │   ├── fft/                 radix-2 FFT 与系数 ROM（独立块）
    │   ├── axi_lite_reg.v       自研 AXI-Lite 本体（独立验证 / SoC 候选两用）
    │   ├── p2f_irq.v            W1C 完成标志与单拍中断
    │   └── ads1115_ctrl.v       电流：ADS1115 命令层与采样
    ├── 02_sim/
    │   ├── run_sim.sh           Icarus Verilog 仿真入口
    │   ├── *_test.v             三路各自的独立验证顶层（仅仿真）
    │   ├── tb_*.v / *_model.v   测试平台与从机模型
    │   └── ui_cwc_stub_*.v      ChipWatcher 仿真替身
    ├── 03_ip/
    │   ├── uiuart_rx.v  ui_mspi_tx.v  ui_spi_rx.v  uii2c.v
    │   └── README.md            厂商 IP 来源与用法
    ├── 04_pin/
    │   ├── fpga_pin.adc         顶层引脚约束
    │   ├── encoder_pin.adc      RS485 与 PL USB UART 附加引脚
    │   ├── timing.sdc           100 MHz 时钟约束
    │   └── encoder.sdc          RS485 RX 同步与串口输出布线约束
    ├── 05_boot/                 预留
    ├── 06_doc/
    │   ├── README.md            编译、仿真与 ChipWatcher 说明
    │   ├── build_td.tcl         TD 批处理构建
    │   └── monitor_encoder.py   PL 串口二进制帧解码
    └── out/                     编译与仿真产物（不入库）
```

`02_sim` 不进入综合。集成顶层与生产构建均不含 CWC 或其他调试探针，见 [`uisrc/06_doc/README.md`](uisrc/06_doc/README.md)。`soc/` 保存 PS + AXI 接入候选工程；其 RTL 构建与实板验证状态分开记录。

## 2. 通道与状态

| 通道 | 传感器 | PL 侧接口 | 状态 |
|---|---|---|---|
| 温度 | GT-M601 串口采集板 | UART 115200 8N1，主动上报 | 可用：仿真 126 项检查通过，实板 CWC 读回 23.7 ℃ |
| 振动 | ADXL345 三轴加速度 | 三线 SPI，5 MHz，3200 Hz | 可用：仿真通过，实板 DEVID 自检与三轴读回正常 |
| 电流 | 霍尔模块 → ADS1115 | I2C 100 kHz，连续转换，DR 860 SPS | 可用：仿真通过（13 项 + 故障 73 项）；连续转换节奏已板测（约 862 Hz），未接霍尔，幅值 / 链路未验证 |
| 转速 | OID-R3808B-12S1S 编码器 | RS485 / Modbus RTU，9600 8N1，站号 1 | 可用：3523 项 HDL 检查与 3 项解码测试；统一顶层实板 1000 帧连续，正负转速与停止归零通过 |

受限与未完成项：

| 通道 | 项 | 状态 |
|---|---|---|
| 温度 | 温度准确度对照、上报周期、TX 电平 | 待定 |
| 振动 | 四路时域与本地滑窗保留可用；FFT 独立块可用；频率估计、对外数据输出待定 | 受限 |
| 电流 | 采样绝对精度校准；连续模式未接 ALERT/RDY，按数据率周期读取，无“新样本就绪”判定 | 待定 |
| 转速 | SoC 采集层、寄存器 / DMA、PS 驱动与应用已接入并实板核对（正反转、停机归零、看板波形）；参考转速准确度及长跑 | 待定 |
| 全部 | AXI-Lite 寄存器、PS 连通、HDMI/Qt、温度/振动与本地记录已实板验证；传感器 DMA、完整谱、IRQ 与生产 STA 待收口 | 受限 |

各通道的详细设计、仿真与实板记录见 `02_sim` 内对应测试平台，以及本目录下各通道的接线与说明（见 §3）。

## 3. 接线与 LED

传感器信号经底板 J12（PL IO / BANK11，固定 3.3 V）进 PL。J12.1 = `+3V3`，J12.2 = `GND`，J12.3 ~ J12.26 为 `CEP1_1P … CEP1_12N`。网络与球号取自核心板 pinlist（`docs/MLK-F3P-DR1M90G-Resource/…/PL-GPIO连接器管脚定义表.xlsx`）。

各路单独分配 J12 引脚，互不共用信号：

| 通道 | 信号（PL） | 方向 | J12 脚 | 网络 | 球号 | IOSTANDARD |
|---|---|---|---|---|---|---|
| 温度 | `I_uart_rx` | 输入 | 5 | `CEP1_2P` | T9 | LVCMOS33，上拉 |
| 振动 | `O_spi_sclk` | 输出 | 3 | `CEP1_1P` | Y12 | LVCMOS33 |
| 振动 | `IO_spi_sdio` | 双向 | 4 | `CEP1_1N` | Y13 | LVCMOS33，无上下拉 |
| 振动 | `O_spi_cs_n` | 输出 | 6 | `CEP1_2N` | U10 | LVCMOS33 |
| 振动 | `I_int1` | 输入 | 7 | `CEP1_3P` | T5 | LVCMOS33 |
| 电流 | `IO_iic_scl` | 开漏 | 9 | `CEP1_4P` | V6 | LVCMOS33，无上下拉 |
| 电流 | `IO_iic_sda` | 开漏 | 10 | `CEP1_4N` | W6 | LVCMOS33，无上下拉 |

说明：

- 振动与电流不共用 J12 引脚：振动用 J12.3 / J12.4（`Y12` / `Y13`），电流用 J12.9 / J12.10（`V6` / `W6`）。
- `IO_spi_sdio` 与两路 I2C 均为双向线，PL 侧关闭内部上下拉（`PULLTYPE = NONE`），高电平由模块侧上拉提供，避免分压。
- 温度通道只接收模块上报，`I_uart_rx` 配弱上拉，未接时保持 UART 空闲高电平；模块 RX 不接。
- 全部 J12 信号为单端 3.3 V；电机电流与高压不得引到这些脚（只走信号）。

转速不经 J12，接板载 J11：编码器黑线→J11.4 GND、白线→J11.5 A、绿线→J11.6 B，红线→外部稳压 +5 V，黄线悬空绝缘。电源负极与板端共地。PL TX/RX/DE=L17/N16/N15，PL USB UART TX=J16，均为 LVCMOS33，约束见 [`encoder_pin.adc`](uisrc/04_pin/encoder_pin.adc)。协议、单位与串口帧见 [`RS485转速通道接入.md`](../docs/RS485转速通道接入.md)。

板上 LED（PL 可驱动 4 只）：

| LED | 球号 | IOSTANDARD | 含义 |
|---|---|---|---|
| LED1 | T19 | LVCMOS18 | 系统心跳 |
| LED2 | R19 | LVCMOS18 | 温度在线（收到有效温度帧后常亮） |
| LED3 | J15 | LVCMOS33 | 振动 DEVID 自检通过（读回 `0xE5`） |
| LED4 | M20 | LVCMOS33 | 电流采集错误 |

LED1 / LED2 上拉至 `VCC_ADJ`（BANK32，发货默认 1.8 V），取 LVCMOS18；LED3 / LED4 上拉至 `VCC_3V3`（BANK33），取 LVCMOS33。硬件手册把 LED1 / LED2 标为 LVCMOS33，与原理图不一致，以原理图为准。

统一码流正常状态为 LED1 闪烁、LED2 / LED3 常亮、LED4 熄灭。转速通过 PL USB UART 观测，不复用上述 LED。LED5 为 PS MIO51，不属于独立 PL 顶层。

温度、振动、电流模块供电由传感器载板 `J2`（`+5V` / `GND`）与 `H1`（`+3V3`）提供，见 [`../docs/传感器板-说明.md`](../docs/传感器板-说明.md)。霍尔模块的电流端经 `U1` / `U2` 串入电机电源回路，其信号端 OUT 接 ADS1115 AIN0。

## 4. 编译、仿真与抓取

见 [`uisrc/06_doc/README.md`](uisrc/06_doc/README.md)。要点：

- TD 打开 `uisrc/fpga.al`（顶层 `fpga_top`），或批处理运行 `uisrc/06_doc/build_td.tcl`。
- 仿真：`./uisrc/02_sim/run_sim.sh`，日志在 `uisrc/out/sim/`。
- 码流生成后用 Program Device 下载。`fpga_top.v` 不含 CWC 实例，`build_td.tcl` 不执行 `insert_debugger`；需要 ChipWatcher 时对调试副本单独插入。

## 5. 交接与后续

- 转速采集已在独立 `fpga_top` 验证；后续接入 SoC 采集层、CDC、`pl_ctrl` 寄存器 / DMA 及 PS 应用。现有温度、振动、电流的 SoC 接入与转速分开验收；PS 视角的数据格式与标定按 [`../docs/PL-PS接口约定.md`](../docs/PL-PS接口约定.md) 定义。
- 接口改动只准「加」；破坏性改动升版本并双方确认。

## 6. 特征与寄存器 RTL

`fpga_top` 已连接振动 X/Y/Z 与电流四路时域特征和本地快照，默认 256 点窗口、128 点步长。特征为 DC、RMS、总体方差、峰峰值、峰值因子、过零率与 Pearson 峭度，支持编译期 512 点。FFT 为 256/512 点独立 radix-2 DIT 块，提供矩形/周期 Hann 窗。

自研 `axi_lite_reg`、`p2f_irq`、`feature_bank` 完成寄存器 / 中断本体；SoC 候选经 `soc/sensor_soc_axi` 例化，独立 `fpga_top` 不接 AXI。冻结寄存器 `0x00～0x10` 与引脚保持；扩展偏移和读取步骤以接口契约 §4.3 为准。

`fpga_top` 是独立采集验证顶层，不含 CWC 或 PS 接口；时基 / 对齐帧与特征的结果尚未外接，相关逻辑可能被综合优化，不能用于验证这些功能。完整寄存器与算法的资源、时序以独立 `validation_top` 为准；PS 接入以 `soc/build_soc.tcl` 的 `soc_top` 为准，见验证结果。`build_td.tcl` 的 PASS 判 `sys_clk` 存在、SWNS/HWNS 非负与 STA 覆盖率 100%，不替代 DRC / 板级 I/O 时序确认。

- 特征与握手规格：[`uisrc/06_doc/时域与频域特征.md`](uisrc/06_doc/时域与频域特征.md)
- 仿真、时序、资源与遗留项：[`uisrc/06_doc/验证结果.md`](uisrc/06_doc/验证结果.md)
- 仿真入口仍为 `./uisrc/02_sim/run_sim.sh`，包含现有采集、时域、FFT、AXI 与生产顶层测试。

## 7. 共同时基与对齐帧

`timebase.v` 以 100 MHz 自由计数提供 unsigned32 `r_ts`（10 ns/LSB），每 100000 拍产生一次 `tick`，并递增 unsigned32 `r_ms`。三路 valid 接收拍锁存时间戳；tick 同拍锁存温度、振动三轴、电流的最新有效值与对应时间戳，含同拍 valid。无新样本时重复最近有效值。

振动 / 电流属于 1 kHz 对齐锁存，温度属于到达即打戳的准同步；器件实际采样时刻与 PL 接收时刻不同，读点对齐触发和同步精度板测为 TBD。特征仍按原始 valid / ready 形成独立窗口，对齐帧不改变其采样流；特征窗口时间戳为 TBD。

独立 `axi_lite_reg` 内部例化同一模块，提供 RO `0x54～0x74`；读取时首尾校验 FRAME_TS，寄存器不提供跨字段原子快照。`fpga_top` 实例不接 PS，SoC 候选经 `sensor_soc_axi` 接入；后续集成须共用同一采集域时基。已有字段与 v1.0 保持，完整语义见 [`../docs/PL-PS接口约定.md`](../docs/PL-PS接口约定.md) §4.3。

转速尚未接入本节的时间戳与对齐帧，按独立成功样本计数及串口帧读出。

`tb_timebase` 核对真实 1 ms 周期、逐拍计数、各路时间戳、同拍 valid、帧间保持、使能、复位及 32 bit 回绕；AXI 与顶层 TB 覆盖寄存器读回和实际连接，均纳入 `run_sim.sh`。

声光告警：合并 SoC 的 PL ABI v1.3 使用 J12.11/U7 高有效输出，寄存器为 `0xD4/0xD8`。`alarm_sequencer.sv` 实现通道短响、模型长响抢占和 2 秒心跳保护；独立采集顶层将 U7 固定低。模型及 PS 接口见[声光告警与模型故障 API](../docs/声光告警与模型故障API.md)。
