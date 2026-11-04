# 编译、仿真与调试

统一四路采集工程 `fpga_top` 的构建、仿真、PL 串口读出与 ChipWatcher 说明。

## 1. TD 工程

| 项 | 值 |
|---|---|
| 工程文件 | [`../fpga.al`](../fpga.al) |
| 顶层 | `fpga_top` |
| 器件 / 速度等级 | `DR1M90GEG400` / `-2` |
| 系统时钟 | `I_sysclk`，100 MHz |

GUI：在 TD 打开 `uisrc/fpga.al`，确认源文件与约束齐全后执行综合、布局布线、码流生成。

批处理（NixOS，见 [`../../../docs/NixOS下TD与Questa使用.md`](../../../docs/NixOS下TD与Questa使用.md)）：

```bash
td-fhs -c 'cd /home/lfour/FHS/TD_Release_2026.2_NL && ./bin/td.sh <绝对路径>/build_td.tcl'
```

`build_td.tcl` 在工程模式下 `open_project` → `elaborate` → `read_adc` / `read_sdc` → 综合布局布线 → 码流，输出 `uisrc/out/td/fpga_top.bit`、`route_timing_summary.rpt` 与 `route.area`。脚本加载基础 `fpga_pin.adc` / `timing.sdc` / `io_boundary.sdc` 以及转速附加 `encoder_pin.adc` / `encoder.sdc`；附加约束不进入尚未接入转速的 SoC 构建。顶层不含 CWC 或其他调试探针，脚本也不执行 `insert_debugger`。

SoC 候选工程使用非工程模式 `read_hdl`：`../soc/build_soc.tcl` 按 `sources.list` 读入 RTL，不打开 `.al` 工程，本机 TD Release 2026.2 可完成 elaborate / place / route。该流程输出 `uisrc/out/soc/system.bit`，产物只表示 RTL 构建通过，不代表 PS 连通或实板功能。工程模式与非工程模式共用同一器件库和约束语言，但工程模式不经过 `read_hdl`。

## 2. 仿真

```bash
./uisrc/02_sim/run_sim.sh
```

脚本使用 `iverilog` / `vvp`（Nix devShell 已提供），逐个编译并运行采集、特征、AXI 与集成顶层的测试平台，日志写入 `uisrc/out/sim/`。测试平台与被测范围：

| 通道 | 测试平台 | 覆盖 |
|---|---|---|
| 温度 | `tb_uart_rx`、`tb_temp_parser`、`tb_temp_test` | UART 收发、帧解析、集成链路 |
| 振动 | `tb_fast`、`tb_spi_l3`、`tb_spi_spec` | 三线 SPI 全流程、主机时序、位序列 |
| 电流 | `tb_ads1115`、`tb_ads1115_faults` | 正常采样、故障注入与恢复 |
| 转速 | `tb_encoder_uart`、`tb_encoder_rpm`、`tb_encoder_modbus`、`tb_encoder_top` | UART / BREAK、饱和换算、Modbus 故障恢复、统一顶层串口 CRC / 快照及原有 LED 语义 |
| 串口解码 | `test_monitor.py` | 分段输入、CRC 重同步、有符号值及 RTL 实际输出帧 |
| 同步采集 | `tb_timebase` | 100 MHz 计数、1 ms 周期、valid 时间戳、对齐帧、同拍 valid、保持、复位与回绕 |
| 时域 | `tb_time_features`，256/512 点 | 七项特征、重叠窗口、复位、拒收与延迟 |
| FFT | `tb_fft`，256/512 × 矩形/Hann | 独立数值参考、背压、自然序与帧尾 |
| AXI | `tb_axi_lite_reg` | AW/W 解耦、字节写、背压、阈值、计数、IRQ、特征快照与同步帧寄存器 |
| 顶层 | `tb_fpga_top` | 注入采集结果，验证四路特征、本地快照、三路时间戳及对齐帧 |
| SoC 接入 | `tb_cdc_soc`、`tb_sensor_soc_axi`、`tb_vibration_fft` | 异步事件 FIFO / 握手邮箱、50/100 MHz AXI 接入、软复位与 IRQ、FFT 窗口 / 功率 / 峰值 |

`02_sim` 中的 `temp_test.v`、`adxl345_test.v`、`ads1115_test.v` 是三路采集各自独立验证时的顶层，只用于仿真，不进入综合。它们的 `ui_cwc` 由 `ui_cwc_stub_*.v` 替身提供。`01_rtl` 的 `fpga_top.v` 是集成顶层，不含 CWC 实例，包含转速协议所需的样本与错误计数。

独立新增逻辑的综合验证：用同一 TD 命令运行 [`build_validation.tcl`](build_validation.tcl)。脚本在 `uisrc/out/validation/` 生成独立工程，例化四路时域、256/512 FFT 与 AXI/中断本体，避免未接外部输出导致算法优化。该工程只用于构建，不下载硬件。两个脚本均从全新工程打开，检查 100 MHz `sys_clk` 与非负 SWNS/HWNS；错误返回非零。

固定参考向量已纳入 `02_sim/vectors/`，HDL 仿真不需要重新生成向量；全量入口另使用 Python 3 运行串口解码测试。数值定义见 [`时域与频域特征.md`](时域与频域特征.md)，实测计数与资源见 [`验证结果.md`](验证结果.md)。

## 3. PL 串口读出

SW1=ON-ON-ON 上电，经 JTAG 下载 `uisrc/out/td/fpga_top.bit`。板载 PL USB UART 为 115200 8N1，实测主机枚举为 `/dev/ttyACM1`；PS 控制台为 `/dev/ttyACM0`，重插后须核对。码流为独立 PL 采集验证，不加载 PS 系统。

在仓库根目录运行，实时串口读取需要 `pyserial`：

```bash
python3 fpga/uisrc/06_doc/monitor_encoder.py --port /dev/ttyACM1 --csv fpga/uisrc/out/encoder.csv
```

遥测为 20 字节二进制帧，不能按普通文本读取。正负转速、错误事件、单位与接线见 [`RS485转速通道接入.md`](../../../docs/RS485转速通道接入.md)。LED1～LED4 继续表示心跳 / 温度 / 振动 / 电流错误，转速由串口判定。

## 4. ChipWatcher

生产顶层与 `build_td.tcl` 均不包含 ChipWatcher 探针，构建产物可直接作为交付码流。需要上板观测时，在 TD GUI 中对调试副本插入 ChipWatcher，单独综合、布局布线并下载；该调试工程不进入本仓库主线，也不作为交付依据。

各通道关键内部信号见 [`../01_rtl/fpga_top.v`](../01_rtl/fpga_top.v) 的网络命名，如温度 `S_temp_x10` / `S_temp_vld`，振动 `O_spi_cs_n` / `IO_spi_sdio` / `S_spi_busy`，电流 `S_adc_raw` / `S_data_vld` / `S_error_code`。

## 5. 产物

| 路径 | 内容 |
|---|---|
| `uisrc/out/td/` | TD 批处理编译产物（码流、时序、资源报告） |
| `uisrc/out/sim/` | 仿真日志与 `.vvp` |
| `uisrc/out/validation/` | 独立算法 / 寄存器本体的 TD 工程、时序与资源报告 |
| `uisrc/out/soc/` | SoC 候选工程：生成 RTL、`sources.list`、时序 / 资源 / 检查报告与 `system.bit` |
| `uisrc/out/rs485_hw/` | 统一码流串口实测：原始二进制、前 1000 帧记录与统计；不入库 |
| `uisrc/out/cwc/` | ChipWatcher 上板抓取导出的 CSV；不随工程提交 |

`uisrc/out/` 与 TD 的 `*_Runs/`、`log/`、`cw/` 由 [`../../.gitignore`](../../.gitignore) 排除。
