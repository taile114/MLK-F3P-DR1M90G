# PL / PS 接入与上板交付

本文规定同步采集、Linux 字符设备、Qt 数值看板、FD / rootfs / SD 的构建与验证步骤。接口唯一权威为 [`PL-PS接口约定.md`](PL-PS接口约定.md)，产物未通过实板验证前不得冻结接口版本。

## 1. 交付状态

| 阶段 | 实现 | 验证边界 |
|---|---|---|
| P0 | CURRENT_ZERO / CURRENT_GAIN，signed32、四字节 WSTRB、硬复位清零、软复位保留 | AXI 测试通过 |
| P1 | 时域与 FFT 保持 PL 实现 | 无 PS / NPU FFT 替代 |
| P2 / P3 | 100 MHz 采集域、AXI 域、事件异步 FIFO、帧 / 特征握手邮箱、P2F 单拍 | 接入层仿真通过；`soc_hdmi.bit` 已实板启动；AXI-Lite 寄存器、HDMI / Qt、`/dev/pl_ctrl`、温度 / 振动读取通过；传感器 DMA（HP1 写主口）已接入，ramp 与真实采集流实板读回一致 |
| P4 | PS→PL 寄存器配置；DMA 单次与环形搬运 | 传感器 DMA 经 HP1 写 DDR 实板验证：ramp 4 KB 校验通过（406.4 MB/s），真实采集流 75 KB/s、无溢出；uiFDMA 未接入 |
| P5 | 振动 X 窗口调度、去 DC、矩形 / Hann、逐 bin 功率与峰值寄存器 | 256/512 点四配置仿真通过；`plctl features` 已提供 X/Y/Z 时域特征与 FFT 摘要读出，实板待测；完整谱经契约 §4.6 窗口读出（`plctl spectrum`、ps-local 频谱视图），实板待测；DMA 已接入并实板验证 |
| P6 | I2C 单时钟使能、CDC 与片内 I/O 布线预算；`build_td` 要求 STA 100%，SoC 构建要求 no_clock / invalid_constraint / combinational_loop 均为 0 且 SWNS / HWNS 非负 | `fpga_top` STA 100%；合并 HDMI `soc_hdmi.bit` no_clock 0 / invalid 0 / 0 违例，STA coverage 99.90%（残差见 §2.2）；1 ns I/O 预算为占位值，板线 / 器件时序待测 |
| P7 | 规程与验证命令齐备 | 合并 `soc_hdmi.bit`、FSBL、BOOT.bin、HDMI + pl_ctrl dtb、rootfs 已实板启动；AXI-Lite 寄存器 / 字符设备实板通过；DMA 已实板验证、块完成中断不可用（改轮询）、接口冻结与生产 STA 判据收口 / Design.xml / HPF 未完成 |
| PS | 字符设备驱动（IRQ 可选）、寄存器封装、温度 / 振动数值看板、缺失数据处理 | ARM 编译通过；实板已运行 Qt linuxfb、`/dev/pl_ctrl`、真实温度 / 振动与本地加密记录；生产时序另见 P6 |

`../fpga/uisrc/out/soc/Design.xml` 是文本整合候选文件，尚未完成 design integrator 的自定义组件、外部端口和 HPF 一致性校验。不得将该文件当作已冻结硬件平台，也不得用原厂 HPF 或码流替代本工程验收。合并工程 `fpga/uisrc/out/soc_hdmi` 同时包含 HDMI 显示逻辑与 `0x8000_0000` 传感器 AXI；`soc_candidate.bit` 保留为仅采集候选，不再作为 HDMI 镜像。合并码流仍是功能候选，不能据此宣称生产验收通过。

仅采集工程 `soc` 的 PS 宏 `design_0_ARM_Processor_System_0` 只导出 `master_gp0/gp1` AXI 主口；其内部 `slave_hp0/1_axi_*` 在包装层以常量 0 绑定，未引到顶层，该工程不能经 HP 端口写 DDR。合并工程 `soc_hdmi` 的 PS 宏 `system_ARM_Processor_System_0` 已引出 `slave_hp0/1_axi_*`，其 `VDMA_0` 与 `uiFDMA_1` 已综合、布线并通过实板 1080P 显示，HP 的 DDR 通路可用。传感器采样流接入应在此基础上增加 AXI 流写通道与 DDR 环形缓冲，不需要为本工程先在 Design Integrator 重新生成 PS IP。显示通路验证的是 VDMA 的 mm2s 读方向，传感器方向为 s2mm 写，端口宽度与地址映射须在接入时确认。

当前没有本轮实板带宽或端到端延迟记录。测量表：

| 测量 | 数值 | 条件 |
|---|---|---|
| 传感器 DMA 写入带宽 | 406.4 MB/s（ramp，4 KiB）| `plctl dma --test` 实板；真实采集流受 3.2 kHz × 3 轴 × 8 B 采样率限制，为 75 KB/s，不是总线上限 |
| 采集至 DDR 周期 | 54.6 ms / 4 KiB 块 | 环形模式实板；真实流按采样率填满一块的时间，端到端延迟需 PL 时间戳与 Linux 时钟对齐，未测 |
| 采集至 Qt 刷新延迟 | TBD | PL `r_ts` 与 Linux 时钟未硬同步，不能直接相减；当前只显示 `/dev/pl_ctrl` 读取周期 / 耗时实测值 |
| 读取周期 / 读取耗时 | 20 ms / 25 µs（本轮启动观察） | `mlk_ps_local` 指标页；随系统负载浮动，不是固定保证值 |
| HDMI 显示模式 | 1080P@60Hz（配置值） | 固定显示模式；带宽未实测，不把像素率计算值当作测量值 |
| IRQ 接收与清除 | 不可用 | 采集基线实测 `f2p_irq[3]` 的 GIC 计数未增长，`f2p_irq[0]/[1]` 属 VDMA；块完成通知采用 `DMA_BLK_SEQ` 轮询 |

## 2. PL 构建与候选 SoC

从仓库根执行：

```bash
nix develop .#fpga
bash fpga/uisrc/02_sim/run_sim.sh
python3 fpga/soc/prepare_soc.py
```

`prepare_soc.py` 从已安装的 `20_axi_lite_register` 复制 PS、互联、协议转换与复位 RTL，排除全部厂商同名 `axi_lite_reg`，连接自研寄存器和 `sensor_capture`。未使用窗口以 DECERR 结束事务。输出位于 `fpga/uisrc/out/soc/`，不入库；本地厂商资料包为必需输入。

TD 2026.2：

```bash
REPO="$PWD"
td-fhs -c "cd /home/lfour/FHS/TD_Release_2026.2_NL && ./bin/td.sh $REPO/fpga/uisrc/06_doc/build_td.tcl"
td-fhs -c "cd /home/lfour/FHS/TD_Release_2026.2_NL && ./bin/td.sh $REPO/fpga/uisrc/06_doc/build_validation.tcl"
td-fhs -c "cd /home/lfour/FHS/TD_Release_2026.2_NL && ./bin/td.sh $REPO/fpga/soc/build_soc.tcl"
```

### 2.1 HDMI + 采集 + NPU 合并码流

显示与采集合并工程由 `prepare_hdmi_soc.py` 生成，不替代上面的仅采集 `soc` 工程：

```bash
python3 fpga/soc/prepare_hdmi_soc.py
td-fhs -c "cd /home/lfour/Projects/MLK-F3P-DR1M90G/fpga/uisrc/out/soc_hdmi && /home/lfour/FHS/TD_Release_2026.2_NL/bin/td.sh build_hdmi_soc.tcl"
```

`prepare_hdmi_soc.py` 复制厂商 QT demo 的 HDMI 工程，并由 `attach_npu.py` 接入厂商 `Video_NPU` 与 GP1 协议转换器。HDMI VDMA 保留 HP0；软 NPU 读走 HP1，写与采集 DMA 按完整突发仲裁，NPU 软核中断接 `f2p_irq[15]`，规则见 [接口契约](PL-PS接口约定.md) §6.1。加密 RTL 原件不修改，可选 YOLO / Resize 关闭。采集侧把 `soc_ctrl_port` 挂到空闲 DS0（`0x8000_0000`），并生成 `sources_readhdl.list`、`timing_hdmi.sdc` 与 `build_hdmi_soc.tcl`。RS485 三根端口由脚本从 `04_pin/encoder_pin.adc` 注入 `system.v` / wrapper / `fpga_pin.adc`（`N16`/`L17`/`N15`），并在 `timing_hdmi.sdc` 生成对应的 10 ns 片内预算；wrapper 未接 `O_rs485_tx`、adc 缺 `LOCATION = N16`、`system.v` 缺 `X_sensor` 或 HP1 / `f2p_irq[3]` 连接时，脚本与构建脚本均拒绝继续。`timing_hdmi.sdc` 先用 PS `p2f_clk0`（50 MHz）建钟，再 `derive_clocks` 推导 PLL0 clk0/clk1/clk2、VTG 与 PLL1 时钟，最后按异步域分组。厂商 IP `.sdc` 只有 `set_ip_property`，其层级路径在扁平 `read_hdl` 网表中不存在，读取会产生无效约束，因此不读取。

已实板验证的 HDMI / 采集基线：码流 4,052,208 B，SHA256 `0a7400ddd106cf6217c5ccd3d5ceac9f4c898613cdd62ebd90e230a09aadd1e4`；SWNS 0.019 ns、HWNS 0.020 ns，0 违例端点；`check_timing` no_clock 0、invalid_constraint 0、no path 0、combinational loop 0；STA coverage 99.91%，残差来源与复现步骤见 §2.2。该码流含 §4.4 传感器 DMA 通道（HP1 写主口）与 §14 转速通道，资源 20512 slices（39.09%）/ 19096 LUT / 27118 寄存器 / 171 DSP / 39 RAM。含 NPU 的合并产物与验收边界见 [`../fpga/uisrc/06_doc/验证结果.md`](../fpga/uisrc/06_doc/验证结果.md)；上述基线的功能与性能数据不替代合并 NPU 镜像的实板验收。PLL 检查仅报告 3 处频率舍入（148.456 vs 148.500、742.400 vs 742.500、148.500 vs 148.485 MHz），不是缺失时钟约束；6 个输入 / 15 个输出仍无 input/output delay（MEDIUM），其中 RS485 的 `I_rs485_rx`、`O_rs485_tx`、`O_rs485_de` 以 10 ns `set_max_delay -datapath_only` 计预算，HDMI TMDS 与传感器板级时序为 TBD。该码流是功能候选，不能据此宣称生产验收通过。

`f2p_irq[0]/[1]` 在厂商 HDMI 工程中属于 VDMA mm2s/s2mm，合并时保持原接线。采集基线实板驱动 `f2p_irq[3]` 时未观察到 GIC 计数增长，驱动 `f2p_irq[0]` 会使 VDMA 驱动判为 spurious 并禁用该线。因此 `pl_ctrl` 保持轮询，设备树不填采集 `interrupts`。软 NPU 的 `f2p_irq[15]` 与 `<0 97 4>` 沿用厂商 NPU 演示接线，合并镜像已注册 GIC 129，FF6 回放时该软核计数为 0；软算子触发仍待验收；位 3 的采集实验不证明其余 fabric 线均不可用。

GUI 构建时，`soc.al` 必须把 `.sv` 放在 `<System_Verilog>` 分组，并在工程中包含 `fpga_pin.adc`、`soc/timing.sdc` 与 `io_boundary.sdc`；`prepare_soc.py` 已按此生成。否则 GUI 会把 `.sv` 标为 `AutoExcluded`，综合报 `black box`，或在 bitgen 阶段报 `BIT-8123 unconstrained io`。

`ps_reset_clk` 不再约束：`p2f_clk1` 的唯一负载是厂商模板中所有复位输出均为 open 的 `Processor_System_Reset_0`。`prepare_soc.py` 删除该复位块与 `Constant_0`，并同步删除其 RTL net 与 Design.xml 连接；`timing.sdc` 因此不再为 `p2f_clk1` 建钟，也没有用 `set_false_path` 掩盖。

`build_soc.tcl` 在 `check_timing` 后检查 no_clock、invalid_constraint、combinational_loop 三项均为 0，并输出 I/O 预算说明：传感器侧 UART / SPI / I2C 与 LED 均为异步接口，`timing.sdc` 以 `sys_clk` 为参照给输入 / 输出各加 1 ns 占位预算，使每个 I/O 端点都有可计时的参照；`io_boundary.sdc` 另以 10 ns `set_max_delay -datapath_only` 约束片内 pad↔寄存器 datapath。1 ns 不是实测板线或器件延迟，板线延迟、外设建立 / 保持与采样相位仍为 TBD。

`build_td` 是独立采集验证工程，不是生产 SoC。`build_soc` 的 `soc_top` 不包含 `ui_cwc`、诊断探针或模拟采样计数源；CTRL.EN 直接决定采集数据接收，DATA.CNT 只由实际振动 valid 递增。LED1 为采集时钟域心跳，LED2/3/4 分别表示温度在线、ADXL345 自检通过和电流错误。SoC 构建使用 `sources.list` 的非工程流程，`.al` 用于源文件查阅；导出生产 HPF 前仍需 design integrator 校验。

时域快照来自原始样本流；对齐帧只跨域传输，不在 AXI 域重建 100 MHz 时间戳。FFT 核数值语义保持，预处理与新增寄存器见契约 §14。谱峰适用于 X 轴，不能标为三轴完整谱。

uiFDMA 接入前需确认实际 TD / IP 许可、HP 端口宽度、读写突发及 BRESP 语义，并匹配 Linux 内存所有权。参考 4096 B × 8 块仅为契约 REF 参数，不能在未设置 reserved-memory / CMA 时写 DDR；现有 CMA 区域可能占用示例地址。完成块必须在全部写响应成功后发布，不得只依据 FDMA busy 下降沿发布。

### 2.2 STA coverage 残差对照

覆盖率残差由两组构建对照确认：厂商自带示例（未修改任何源文件）与本工程。下面是复现步骤，以及每一步应读的文件与字段。

**步骤 1：生成厂商示例工作区**

```bash
python3 fpga/soc/vendor_coverage_control.py
```

脚本从 `docs/MLK-F3P-DR1M90G-Resource/03_demo/3-2_ex_fpsoc/3_2_01_sdk_base/20_axi_lite_register/soc_prj` 读取 IP-XACT 清单，取出其中列出的 RTL（跳过各处 `define.v`），加上 `hdl/system.v`、`hdl/system_wrapper.v`，汇总为 `fpga/uisrc/out/vendor_ctrl/sources.list`，并写出：

- `timing.sdc`：`create_clock -name axi_clk -period 20.000 [get_nets {system/w_port__ARM_Processor_System_0__p2f_clk0}]` 加 `derive_clocks`
- `build_vendor_ctrl.tcl`：`read_hdl -top system_top` → `read_adc`（厂商 `pin.adc`）→ `read_sdc timing.sdc` → `place` / `route` → `update_timing -mode final` → 输出 `route_timing_summary.rpt` 与 `check_timing.rpt`

脚本只写 `fpga/uisrc/out/`，不改资料包。

**步骤 2：构建厂商示例**

```bash
td-fhs -c "cd /home/lfour/FHS/TD_Release_2026.2_NL && ./bin/td.sh $PWD/fpga/uisrc/out/vendor_ctrl/build_vendor_ctrl.tcl"
```

**步骤 3：读数**

| 读什么 | 文件 | 字段 | 预期 |
|---|---|---|---|
| 覆盖率 | `fpga/uisrc/out/vendor_ctrl/route_timing_summary.rpt` | 头部 `STA coverage` | 88.98% |
| 约束完整性 | `fpga/uisrc/out/vendor_ctrl/check_timing.rpt` | 目录中 `no clock(0)`、`invalid constraint(0)`、`combinational loops(0)` | 三项均为 0 |
| 逻辑实例数 | 构建日志 | `SYN-3001 ... logic instances` | 3183 |
| 时序裕量 | `route_timing_summary.rpt` | `SWNS` / `HWNS` | +13.829 / +0.021 ns |

**步骤 4：构建本工程作对照（命令行）**

```bash
python3 fpga/soc/prepare_soc.py
td-fhs -c "cd /home/lfour/FHS/TD_Release_2026.2_NL && ./bin/td.sh $PWD/fpga/soc/build_soc.tcl"
```

读 `fpga/uisrc/out/soc/route_timing_summary.rpt` 与 `check_timing.rpt`：覆盖率 99.91%，`no clock` / `invalid constraint` / `combinational loops` 均为 0，SWNS 0.756 ns、HWNS 0.020 ns，逻辑实例 35928，产物 `system.bit` 4,052,203 B。`build_soc.tcl` 的判据是这三项为 0 加 SWNS / HWNS 非负，覆盖率只打印、不设门槛。

**步骤 5：无 PS 宏对照**

```bash
td-fhs -c "cd /home/lfour/FHS/TD_Release_2026.2_NL && ./bin/td.sh $PWD/fpga/uisrc/06_doc/build_td.tcl"
```

`fpga/uisrc/out/td/route_timing_summary.rpt` 的 `STA coverage` 为 100.00%。

**对照结果**

| 工程 | 逻辑实例 | `check_timing` 三项 | STA coverage |
|---|---|---|---|
| 厂商 `20_axi_lite_register`（未改） | 3183 | 0 / 0 / 0 | 88.98% |
| 本工程 `soc`（命令行） | 35928 | 0 / 0 / 0 | 99.91% |
| 本工程 `soc_hdmi`（合并 HDMI） | - | 0 / 0 / 0 | 99.90% |
| `fpga_top`（`build_td.tcl`） | - | 0 / 0 / 0 | 100.00% |

**复现前提：时钟网命名**

厂商示例的顶层是 `system_top`，PS 时钟网位于 `system/` 层级下，必须写 `system/w_port__ARM_Processor_System_0__p2f_clk0`。写成不带层级的 `w_port__…` 或 `w_net__…`（`assign` 别名）都会使时钟无法建立：`check_timing` 报 6 个时钟网络无时钟源，同一构建的覆盖率降到 83.97%。

**结论**

含 PS 宏的工程在约束全部有效时覆盖率仍达不到 100%，且厂商自带示例低于本工程。残差属 TD 与厂商 IP 一侧的时序端点分析覆盖限制，不是本项目 RTL 或约束的缺失。

### 2.3 传感器 DMA 通道

PL 侧采样流经自研 AXI4 只写主口写入 PS DDR，主口接 PS 宏的空闲 HP1（`Slave_AXI_HP1_Interface=1`、数据宽度 64）。寄存器窗口 `0x90～0xA4` 与数据字格式见 [`PL-PS接口约定.md`](PL-PS接口约定.md) §4.4。实现要点：

- 突发不超过 16 拍、单 outstanding、每突发收到写响应才发下一个；`awlen` 切片到 PS 宏的 4 位端口，`awsize` 恒 `2'b11`，`awid/awcache/awlock/awprot/awqos` 接 0，读通道保持常量 0。
- 时钟与传感器 AXI 从机、PS master GP0 同为 `w_net__PLL_0__clk0_out`（100 MHz）；复位用 `w_net__Processor_System_Reset_0__peripheral_aresetn`，本版 PS 宏没有 `slave_hp1_axi_aresetn` 端口。
- `DMA_CTRL.TEST=1` 时由内部 ramp 产生数据，先把链路跑通；真实样本流取采集模块的 CDC 流与 1 kHz 对齐帧时间戳，不经过特征单元。
- 仿真：`tb_sensor_dma` 3699 项检查通过（单突发、4 KB 多突发、非法长度、写响应非 OKAY、STOP / ABORT 背压保持、外部流反压）。

PS 侧：`pl_ctrl` 驱动放行 `0x90～0xA4` 窗口（`0xA0` / `0xA4` 只读），`plctl dma --base <物理地址> --bytes <n> [--test] [--abort]` 负责配置、启动、轮询与读回校验；设备树在 `0x3EF0_0000` 预留 1 MiB `no-map` 区间并在 `pl-ctrl` 节点挂 `memory-region`。实板实测：ramp 4 KB / 10.1 µs（406.35 MB/s，`verify=PASS`）、真实采集流 4 KB / 54.6 ms（75 KB/s，`OVF=0`），读回校验通过。

环形缓冲与块完成中断：`DMA_CTRL[3]=1` 进入环形模式，按 `DMA_BLOCKS` 循环写 4 KiB 块，`DMA_BLK_SEQ` 累计完成块；`DMA_IRQ[16]` 记录块完成挂起，写 `DMA_IRQ[8]=1` 清除；中断线不可用（见本节 fabric 中断限制），PS 按 `DMA_BLK_SEQ` 轮询进度，未清除时多次块完成合并为一次挂起。`PL_TS_NOW` 提供 10 ns 计数用于延迟测量。PS 侧 `plctl dma --ring --blocks N --count K [--wait-irq] [--latency]` 按 `DMA_BLK_SEQ` 差值读块；设备树 `interrupts` 由 `pl_irq=auto|off|<gic>` 控制，默认 `off`（轮询）。仿真：`tb_sensor_dma` 3699 项、`tb_dma_ring_irq` 360 项、`tb_sensor_soc_axi` 228 项全部通过。

时序余量说明：合并码流 100 MHz 域的建立裕量仅 0.003 ns、保持裕量 0.021 ns，关键路径是特征单元的 DSP 乘法累加链（数据 9.345 ns / 10 ns 预算）。接入 DMA 的过程中曾出现 −12 ps 保持时间违例，位置在特征单元存储的写数据输入端（数据路径 0.282 ns、时钟偏斜 0.199 ns），TD 报 `Failed to fix hold` 且无法在该网插延时；改用当前 RTL 结构后该违例未再复现。该域没有设计余量，继续增加 100 MHz 逻辑前应先降低该域负载或把特征 / FFT 移入独立时钟域。

## 3. 从 HPF 到 FD FSBL ELF

前提：生产 SoC 完成布局布线及接口校验，FD 与 TD 版本匹配。当前候选 Design.xml 不能直接用于生产 FSBL。

### 3.1 本机 FD 环境

FD（FutureDynasty）与 TD 分开安装。本机路径：

```bash
/home/lfour/FHS/FD_Release_2026.2/FutureDynasty/fd
```

FD 是 Eclipse GUI，没有完整的无头建平台接口。`tools/asct/asct` 可用于查询和修改已有工程，但 `create_platform_project` 只生成 `.fd` 元数据，`generate_bsp_sources` 等在本版本是空实现，不能替代 GUI。

两个已知问题：

1. **HPF 器件名**：FD 2026.2 只接受 `DR1M90`，HPF 若写 `DR1M90GEG400` 会报
   `but the selected HPF is based on DR1M90GEG400`。厂商 `20_axi_lite_register` 的 HPF 与该 SoC 的 PS 配置一致（DDR3L 1.35 V、`DDRC_DELAY_B0=0`、32 bit），可复制后把 `HPFs/hpf.info` 和 `HPFs/Design.xml` 的器件名改为 `DR1M90`，作为候选平台 HPF。该修改只影响器件名校验，不改 PS 配置；最终生产 HPF 仍应由 Design Integrator 正常导出。
2. **NixOS 工具链 shebang**：FD 工具链 wrapper 以 `#!/bin/bash` 开头，NixOS 没有 `/bin/bash`，直接 `make` 会报
   `aarch64-none-elf-gcc: No such file or directory`。把 `FutureDynasty/toolchain` 下 wrapper 的首行改为 `#!/usr/bin/env bash` 后再构建。该修改属于本机安装环境，不在仓库内。

厂商手册：

- [`FD_DR1_User_Guide_V1.1-米联客.pdf`](MLK-F3P-DR1M90G-Resource/02_hardware/hardware_MLK_F3P_CZ02_DR1_20250526/06_芯片手册/01_DR1/FD_DR1_User_Guide_V1.1-米联客.pdf) §4 Platform Project、§5 Application Project
- [`FPSoC应用开发说明_alpha1.2.pdf`](MLK-F3P-DR1M90G-Resource/02_hardware/hardware_MLK_F3P_CZ02_DR1_20250526/06_芯片手册/01_DR1/FPSoC应用开发说明_alpha1.2.pdf) §5 Platform 工程、§7 FSBL / BOOT.bin

### 3.2 FD GUI 建 Platform 与 FSBL

1. 启动 FD，选择空 workspace，例如 `fpga/uisrc/out/soc/fd_ws`。
2. `File → New → Platform Project`：
   - Name `soc_base`
   - HPF 选择候选平台 HPF（例如 `soc_hw/fpga_prj_dr1m90.hpf`）
   - Processor `apu-0`
   - OS `standalone`
   - Finish。Platform 工程是静态库工程，默认 Build Target 为空，单独点 Build 可能没有任何输出，这是正常现象。
3. `File → New → Application Project`：
   - Name `soc_fsbl`
   - Platform `soc_base`
   - Processor `apu-0`
   - Language `C`，Bit Width `64-bit`
   - Template `FSBL_V1_0`（列表可能显示 `FSBL`）
   - 不勾 `Create Linux App`，不勾 `ManagedBuildOn`
   - Finish。
4. 选中 `soc_fsbl`，Build。成功产物为 `soc_fsbl/build/FSBL.elf`（部分版本为 `soc_fsbl.elf`）。

### 3.3 无界面构建 FSBL

FD 工程是 Makefile 工程；GUI 生成工程后，可在仓库根直接构建：

```bash
FD=/home/lfour/FHS/FD_Release_2026.2/FutureDynasty
make -C fpga/uisrc/out/soc/fd_ws/soc_fsbl   SDK_ROOT=$PWD/fpga/uisrc/out/soc/fd_ws/soc_base   CHIP=dr1m90   COMPILE_PREFIX=$FD/toolchain/gcc/aarch64/bin/aarch64-none-elf-   TARGET=FSBL
```

输出 `fpga/uisrc/out/soc/fd_ws/soc_fsbl/build/FSBL.elf`。本机产物为 AArch64 静态 ELF，大小约 1.71 MB。构建成功只表示 FSBL 可链接；实际 HDMI 镜像使用厂商 HDMI FSBL 并已实板启动，生产仍需重新导出 HP0/1 使能的 HPF 后重建 FSBL。

### 3.4 组装 BOOT.bin

把 FSBL 与候选码流放到 vendor 输出目录（保留 `.bak`），再用现有 `boot.bif` 重新生成：

```bash
platform/flash/build_boot_bin.sh
```

该脚本把 `fpga/uisrc/out/soc/fd_ws/soc_fsbl/build/FSBL.elf` 与 `fpga/uisrc/out/soc_hdmi/soc_hdmi.bit` 复制到 vendor 输出目录，再用 `albg` 重新生成并复制到启动分区。当前产物：

| 文件 | 大小 | SHA256 |
|---|---|---|
| `platform/flash/vendor/boards/DR1M/buildroot/images/boot/BOOT.bin` | 5,186,944 B | `14aca0915f7b7f633cb62837edeb277393b3e48e537acb024890419754737b1b` |
| `.../output/system.bit` | 4,052,208 B | `d818a44ad6c22273726256885ce4f69a82d493a3ab9b1427da0d1ebe9bf3dc34` |
| `.../output/fsbl.elf` | 2,168,256 B | `ea8231f9ff5d7549b83e4725c14f6f62b505a89f8cb00dad68e294c359666922` |

BOOT.bin 只含 FSBL + PL 码流 + U-Boot；内核、dtb、rootfs 是启动分区独立文件。合并码流的 STA coverage 为 99.90%，残差来源与复现步骤见 §2.2；该码流是功能候选，生产判据见 §7。

HDMI 显示通路经 VDMA 从 DDR 取帧，数据走 PS HP0。候选 FSBL 的 HPF 为 `Slave_AXI_HP0_Interface=0`，会使 VDMA 停住、HDMI TX 保持复位、屏幕无输出（传感器走 `M_AXI_GP0`，读数仍正常）。HDMI 镜像必须使用 HP0/1 使能的 FSBL：本镜像使用厂商 HDMI 工程的 `soc_sdk/soc_fsbl/build/soc_fsbl.elf`；`build_boot_bin.sh` 已默认选择该 FSBL，可用 `FSBL=<path>` 覆盖。生产交付应在 Design Integrator 导出 HP0/1 使能的 HPF 后重建 FSBL，不得直接使用 `Slave_AXI_HP0_Interface=0` 的候选 FSBL。

1. 在 TD 打开生产 SoC 工程；design integrator 校验 PS 外设、时钟、地址和 IRQ，生成系统 RTL。将 `DS*_BASEADDR0` 与实际 `f2p_irq` 连线回填契约 §3 / §5；`P2F_IRQ_*_Available` 不是 Linux 虚拟 IRQ 号。
2. 在 TD 执行 `Project → Export Hardware Platform`，导出 `soc_hw/fpga_prj.hpf`，并保留同一次构建的 `system.bit` 和 `Design.xml`。HPF 含 PS 初始化参数，不可只重命名原厂文件。
3. 启动 FD，选择独立 workspace。执行 `File → New → Platform Project`，名称 `soc_base`，选择上述 HPF、ARM / APU0、standalone。生成 BSP 与 `platform` 初始化代码。
4. 在 `soc_base` 上执行 Build。更新平台时用 `Change Reference HPF` 后重新生成 BSP，再构建。
5. 执行 `File → New → Application Project`，名称 `soc_fsbl`，关联 `soc_base`，CPU APU0 / AArch64，模板 `FSBL_V1_0`。执行 Build，检查 Console 中无错误。
6. 在 FD 配置的 make 与裸机工具链环境中，可用同一工程的 Makefile 复现。以下变量取实际 FD workspace 与 AArch64 裸机工具链目录；Linux 的 `aarch64-linux-gnu-` 不是 FSBL 工具链。

```bash
FD_WS=/absolute/path/to/fd-workspace
BM_BIN=/absolute/path/to/aarch64-baremetal/bin
export PATH="$BM_BIN:$PATH"
BM_PREFIX=aarch64-none-elf-
# 如实际工具名为 aarch64-elf-gcc，则使用 BM_PREFIX=aarch64-elf-
"${BM_PREFIX}gcc" --version
make -C "$FD_WS/soc_base" SDK_ROOT="$FD_WS/soc_base" \
  CHIP=dr1m90 COMPILE_PREFIX="$BM_PREFIX"
make -C "$FD_WS/soc_fsbl" SDK_ROOT="$FD_WS/soc_base" \
  CHIP=dr1m90 COMPILE_PREFIX="$BM_PREFIX" TARGET=soc_fsbl
file "$FD_WS/soc_fsbl/build/soc_fsbl.elf"
```

Makefile 的 SDK_ROOT、COMPILE_PREFIX、TARGET、APP_TAR_DIR 以本地厂商生成工程为准；若 FD Console 使用不同参数，保存该完整构建命令。不同 FD 版本的 HPF→BSP 生成没有在本仓库验证过的统一无界面命令，必须先完成步骤 3–5；不得跳过 BSP 生成而只修改 ELF 名称。

## 4. BOOT.bin 与 Linux 构建

从仓库根进入 Nix 构建环境，准备实际产物：

```bash
REPO="$PWD"
LAB="$REPO/platform/flash/vendor"
cp "$FD_WS/soc_fsbl/build/soc_fsbl.elf" "$LAB/boards/files/soc_fsbl.elf"
cp "$REPO/fpga/uisrc/out/soc_hdmi/soc_hdmi.bit" "$LAB/boards/files/system.bit"
find "$LAB/boards/files" -maxdepth 1 -type f \( -name '*.elf' -o -name '*.bit' \)
```

只在 SoC 构建与 HPF 验收完成后执行复制。`boards/files/` 顶层必须恰好一个 ELF 和一个 bit；内核 / U-Boot dts 与该 PS 配置一致。BOOT.bin 仅由 FSBL + system.bit + u-boot.bin 组成，内核、dtb、rootfs 为独立分区文件。

```bash
cd "$REPO/platform/flash"
nix develop
./apply_sdk_local.sh
cd vendor/scripts
source drcfg.sh
move_files.sh
make_uboot.sh
make_kernel.sh
make_rootfs.sh
create_image.sh
```

`create_image.sh` 选择 `parameter-tf-fat-ext4.txt`；按菜单中的实际名称选择，菜单编号不能作为固定接口。产物为 `boards/DR1M/buildroot/images/boot/` 与 `images/rootfs/`。实际组装命令在 `create_dr1m_image.sh` 中使用 `albg -w -log info -image boot.bif -arch dr1m90 -o BOOT.bin`；BIF 只列 FSBL、PL bit、U-Boot。

### 本地记录（PS → SD）

`host/remote-pc/src/faultstore.cpp` 已链接进 ps-local。顶部「记录」按钮按 `recordSegmentSec`（10/30/60/300 s，默认 60）记录 `(t, t+a)` 的轮询样本，写入 `/data/mlk_f3p/fault_<UTC时间>_<us>.mlkf`；目录可在「配置 → 本地记录目录」修改。默认 AES-256-GCM + PBKDF2 加密，口令来自配置；文件头 `MLKF` 的 `encrypt` 标志可用于核对。通道映射为 X / Y / Z / Temp，与 PC 端旧的 Temp/Current/Vibration/Speed 映射不同。当前为轮询速率分段记录，不是 NPU 故障触发，也不是 PL 全速率；连续全速率需 DMA/流式写盘，未实现。

保存前自动执行保留策略：默认最长 3 个月、最低剩余 512 MB，按文件修改时间删除最旧样本；同时以 `months × 30 × 24 × 3600 / recordSegmentSec` 个文件作为硬上限，避免未设 RTC 时时间判断失效。配置（需密码）中的「清理所有记录」删除当前记录目录下全部 `capture_*.mlkc` / `fault_*.mlkf`，「删除选定记录」在文件选择器中多选删除；两者都先确认并保留工单与标签。自动保留策略仍在保存前执行。配置项为 `retentionMonths`、`minFreeMb` 与 `recordSegmentSec`，`0` 表示对应限制关闭。旧 256 点周期记录、自动保留、手动清理与配置密码修改均已实板验证；新增的分段记录、虚拟键盘符号行、UI/CLI 特征读出与可读文件名尚未板测。目录位于 SD 卡 ext4 rootfs。

### 时间同步

板卡未设 RTC 时，上电后从主机运行 [`../tools/sync_board_time.sh`](../tools/sync_board_time.sh)，经 `/dev/ttyACM0` 写入当前 UTC；不依赖网络，可加 `--interval 30` 按固定间隔持续对时（复用同一串口会话）。记录文件名、`.mlkf` 内 `tsUs` 与文件 mtime 随本地记录保存，关机后不要求 RTC 保持。后续接入 4G/WiFi 时改用 NTP/chrony，串口对时保留为离线回退。

### PL 特征读取与 IRQ 口径

`plctl features` 写 `FEATURE_SNAPSHOT` 后读取 X/Y/Z 时域特征（DC/RMS/方差/峰峰值/峰值因子/过零率/峭度）、FFT 峰值 bin/功率/序号/标志；本地 UI 的「PL 特征摘要」显示 X/Y/Z 的 RMS/峰峰值与 FFT 峰值 bin/功率。完整功率谱与 DMA 仍未实现；UI 与 CLI 的实板读出待验证。

传感器 IRQ 本版不纳入验收：`f2p_irq[0]/[1]` 留给 HDMI VDMA，`/dev/pl_ctrl` 按轮询工作；`plctl irq` 在无 IRQ 时明确输出 `sensor IRQ not wired; polling only`，不再写成“待上板”。

### 应用与驱动叠加

在内核构建完成后，从仓库根执行：

```bash
platform/flash/stage_ps_local.sh
platform/flash/build_rootfs.sh
```

这是“厂商基础 rootfs + vim/ncurses + 本地 overlay”路径，不是完整 Buildroot 源码重建；完整源码构建仍按上面的 `make_rootfs.sh` 执行。重新运行 `create_image.sh` 可能从 output 覆盖 images 中的 rootfs，应用叠加应在最终镜像组装后执行。

本机产物：

| 文件 | 大小 | 说明 |
|---|---|---|
| `platform/flash/vendor/boards/DR1M/buildroot/images/rootfs/rootfs.tar.gz` | 133,197,675 B | SHA256 `189dc805edf32c139977987371e09bba4e2891603d5271590884f13bb8109310` |
| `platform/flash/vendor/boards/DR1M/buildroot/images/boot/uImage.lz4` | 8,502,292 B | SHA256 `e5e6aa34a1547d4e17a0955a0ae1aa79624a51f5d88cac5a96b1cf15d0192e5e`；含 `CONFIG_PPP`、`CONFIG_PPPOE` |
| `platform/flash/rootfs-overlay/lib/modules/6.1.111-rt42/extra/pl_ctrl.ko` | 436,504 B | SHA256 `4f8f94b2eef507e55590a4f431e69a397474c5d828b0f1454a638d8a3d380e98` |
| `platform/flash/rootfs-overlay/usr/bin/plctl` | — | ARM aarch64 |
| `platform/flash/rootfs-overlay/opt/qt_play/` | — | `mlk_ps_local`、`qt_lib`、`run_arm_app.sh` |

rootfs 内已核对存在 `/lib/modules/6.1.111-rt42/extra/pl_ctrl.ko`、`/usr/bin/plctl`、`/usr/bin/mlk_pppoe`、`/usr/sbin/pppd`、`/usr/sbin/pppoe-discovery`、`/usr/lib/pppd/2.4.9/pppoe.so`、`/opt/qt_play/mlk_ps_local`、`/opt/qt_play/run_arm_app.sh`、`/etc/init.d/S40mlknetwork`、`/etc/init.d/S98plctrl`、`/etc/init.d/S99qtapp`。

### 板上应用的启动、停止与重启

```bash
/etc/init.d/S99qtapp start|stop|restart|status
```

pidfile 为 `/var/run/mlk_ps_local.pid`（不可建时退到 `/tmp`）；脚本按 `/proc/<pid>/comm` 校验进程名，不使用 BusyBox 未提供的 `pgrep` / `pkill`。`stop` 先发 SIGTERM：应用在事件循环里退出，`KLocalWindow` 析构调用 `DriverSource::stop()`，`pl_dma_ring_stop()` 向 `DMA_CTRL`（`0x90`）写 `RING|STOP` 并等待 BUSY 清零，随后 `munmap` 并关闭 `/dev/mem`、`/dev/pl_ctrl`。超过 5 s 未退出才发 SIGKILL。

两条路径结束后脚本都用 `plctl read 0x90` 核对 `DMA_CTRL`：BUSY（bit 8）仍置位时写 `0x18`（`RING|STOP`），确认清零后再写 `0x0` 回到 idle，并把结果打印到标准输出。该核对覆盖应用未自行释放环的情形：上一次实例被强杀后环停在 BUSY 时，新实例打不开环会退化为寄存器轮询，其自身析构不再写 `DMA_CTRL`。

### 设备树与 IRQ

合并码流的设备树以厂商 QT demo 的 `.../demo/boot/system.dtb` 为基线，由 `platform/flash/build_hdmi_dtb.sh` 反编译、插入 polling-only 节点并重新编译：

```bash
platform/flash/build_hdmi_dtb.sh
```

输出：`platform/flash/vendor/boards/DR1M/buildroot/images/boot/system.dtb`，SHA256 `73b2d30849b1b8325ca9f5e1e7d7fbdd76f139c6fc1d65d48412a8568f2186f4`。脚本会用 dtc 反编译校验 `pl-ctrl@80000000`、`vdma@80200000`、`vdmafb@80100000` 均存在。

```dts
pl-ctrl@80000000 {
    compatible = "mlk,pl-ctrl-1.0";
    reg = <0x0 0x80000000 0x0 0x10000>;
    status = "okay";
};
```

节点不填 `interrupts`。合并码流把 `f2p_irq[0]/[1]` 留给 VDMA mm2s/s2mm；传感器 IRQ 未接线。例程 GIC 硬件 ID 114 对应 SPI 编号 82，仍为 REF，不得写入设备树或当作 Linux 虚拟 IRQ。驱动在 `platform_get_irq()` 失败时继续 probe，`read` / `pread` 正常，`poll()` 返回不可用。

## 5. SD 烧录与启动

先用 `lsblk` 确认 SD 读卡器，以下命令会清空选定介质；不得将系统盘作为目标。仓库当前流程：

```bash
lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS
sudo env PATH="$PATH" bash platform/flash/burn_sd.sh /dev/sdX
sync
```

NixOS 默认 PATH 不含 `mkfs.ext4` / `e2fsck`（e2fsprogs）。若脚本报 `missing tool: mkfs.ext4`：

```bash
nix shell nixpkgs#e2fsprogs nixpkgs#dosfstools nixpkgs#util-linux nixpkgs#rsync nixpkgs#gnutar \
  -c bash -c 'sudo env PATH="$PATH" bash platform/flash/burn_sd.sh /dev/sdX'
```

`burn_sd.sh` 还需要 `sfdisk` / `losetup` / `wipefs` / `partx`（util-linux）、`mkfs.vfat` / `fsck.vfat`（dosfstools）、`rsync` 与 `tar`；在 `nix develop` 中执行可补齐宿主工具。厂商 `make_parted.sh` / `deploy_image.sh` 仍可用作备选，但不要与 `burn_sd.sh` 混用。`burn_sd.sh` 会先清分区表，按固定偏移写 FAT32 BOOT + ext4 rootfs，最后写 MBR。

SD 插入板卡后，断电设置 SW1=SD（PIN1/PIN2/PIN3 为 ON-OFF-OFF），HDMI 接显示器，温度模块与 ADXL345 接好，串口 115200 8N1，上电并查看启动日志。该流程不写 eMMC。

`ON-OFF-OFF` 是 SD 启动位，不是 JTAG 位；JTAG 位是 `ON-ON-ON`。SD 启动后若只需在线更新 PL 码流，可保持 `ON-OFF-OFF`，用 JTAG `Program FPGA` 下载 `.bit`；前提是 TD/FD 能识别 DR1M90。该操作不写 SD rootfs，掉电后仍由 `BOOT.bin` 加载旧码流。步骤和限制见 [`PS-PL开发流程.md`](PS-PL开发流程.md) §5.3.1。QSPI 固化需 `ON-ON-ON` + FD `ProgramFlash`。

### 配置入口与输入

「配置」/ F2 先验证配置密码，初始 `root`，可在配置内修改；QSettings 删除或重新部署后恢复 `root`。无物理键盘：点击文本输入弹出独立顶层虚拟键盘，支持大小写英文、数字、常用符号 `. / : - _ @ * + = , ; #`、Shift/Caps、Backspace/Space/Enter；Shift/Caps 同步切换字母键标签，长按顶部可拖动，可拖出配置窗口。配置/密码窗口使用 WindowModal，只阻塞主界面，不阻塞键盘。

## 6. PS 最小连通与真实数据验证

### 手工寄存器验证

仅在基地址确认为 `0x80000000`、已加载匹配 SoC 后执行；此阶段不加载驱动，避免与驱动同时操作 IRQ / CTRL。

```sh
devmem 0x80000000 32             # ID 应为 0x504c0102
devmem 0x80000004 32 0x1        # 使能，IRQ 暂不使能
devmem 0x80000008 32             # STAT.BUSY / EN 反应
devmem 0x8000000c 32
sleep 1
devmem 0x8000000c 32             # 高 16 bit 由实际振动 valid 递增
devmem 0x8000004c 32 0xffff8000
devmem 0x80000050 32 0x0000007d
devmem 0x8000004c 32
devmem 0x80000050 32
devmem 0x8000004c 32 0           # 恢复未标定
devmem 0x80000050 32 0
```

电流系数仅验证 RW，不据此显示电流，示例系数不代表实际标定。

### 驱动与中断

```sh
uname -r
insmod /lib/modules/"$(uname -r)"/extra/pl_ctrl.ko
ls -l /dev/pl_ctrl
dmesg | tail -30
plctl probe
plctl write 0x04 5
plctl sensors
plctl read 0x10
plctl write 0x10 1
plctl read 0x10
```

本轮合并 HDMI 码流不接传感器 IRQ：`plctl irq 3000` 返回 ENODEV，`cat /proc/interrupts | grep pl` 没有传感器条目，属于预期。驱动 probe 不会因 `platform_get_irq()` 失败而退出；`read` / `pread` 与 `/dev/pl_ctrl` 正常，`poll()` 返回不可用。VDMA 仍使用 `f2p_irq[0]/[1]`，其 GIC 82/83 由显示设备树声明。

带 IRQ 的版本保留原行为：handler 读 DONE、W1C、读回、累加计数并唤醒 poll。启用 IRQ 前必须先核对实际 GIC ID 与触发类型；不得用累计多次中断冒充“一次 IRQ”验收。

### DMA 通道验证

前提：位流含契约 §4.4 的 DMA 通道，设备树含 `sensor-dma-buffer@3ef00000`（`0x3EF0_0000` 起 1 MiB、`no-map`），驱动为放行 `0x90～0xA4` 的版本。

```bash
insmod /lib/modules/$(uname -r)/extra/pl_ctrl.ko
plctl probe                                  # 应为 ID=0x504c0102
plctl dma --base 0x3ef00000 --bytes 4096 --test
plctl dma --base 0x3ef00000 --bytes 4096     # 真实采集流，校验前打印前 4 个 64 位字
```

判读：

| 输出 | 含义 |
|---|---|
| `BURSTS=32`、`WRITTEN=4096`、`verify=PASS` | 4 KB 写成 32 个 128 B 突发，ramp 校验通过 |
| `elapsed_us` / `bandwidth_mbs` | 含 syscall 与驱动读写的折算值，不是 PL 主口峰值 |
| `ERRCODE=2` | 写响应非 OKAY：检查 HP1 连接与基址是否落在 DDR |
| `ERRCODE=1` | 参数非法：长度非 8 的倍数、超过 4096、基址未 8 字节对齐 |
| 超时 `DONE not set` | 位流不含 DMA、驱动为旧版（写 `0x90` 返回 EINVAL）、或 HP1 未使能 |

实板实测（同一码流、同一驱动）：

| 模式 | 命令 | 结果 |
|---|---|---|
| ramp | `plctl dma --base 0x3ef00000 --bytes 4096 --test` | 32 突发、4096 B、`verify=PASS`、`ramp_start=0`、10.1 µs、406.35 MB/s |
| 采集流 | `plctl dma --base 0x3ef00000 --bytes 4096` | 32 突发、4096 B、`OVF=0`、54.6 ms、0.08 MB/s |

采集流首块解码：`word[0..2]` 的 `ts` 同为 `0x28c56602`，`ch` 依次 0 / 1 / 2，`sample` 为 66 / -158 / -200，与 `plctl sensors` 的 `acc_raw`（68 / -156 / -192）同量级同轴向，确认搬的是真实振动样本。4096 B / 54.6 ms = 75 KB/s，与振动 3.2 kHz × 3 轴 × 8 B 的设计值一致；ramp 的 406.35 MB/s 反映主口 + HP1 + DDR 上限，两者相差三个数量级属预期。

PS 侧消费（ps-local 的 `DriverSource`）：

- 环大小 4 块（`kDmaBlocks=4`），20 ms 轮询；每轮读 `DMA_BLK_SEQ`，只取最新完成块，`BLK_SEQ` 跳变量减一即丢失块数。
- 4 KiB 块 = 512 字 = 170 个三字帧，块边界不按三字帧对齐：块尾 0～2 个字保留，与下一块开头拼接；发包时 `samples` / `samplesY` / `samplesZ` 等长。
- 样本率由块内首末帧的 `TS` 求差估计（实测约 3.1 kHz，与 170 帧 / 55 ms 一致），不取标称值。
- 采集龄 = `PL_TS_NOW` − 块内末帧 `TS`，10 ns/LSB，指数平滑；状态栏显示「DMA 环 · 块 / 丢块 / 溢出 / 采集龄」。
- 采集方式：环打开成功为 `DMA 环`，`/dev/mem` 映射失败退回 `寄存器轮询`；`ERRCODE` 非零时显示 `DMA 错误`。

两条与 PL 实现绑定的行为：

- ramp 计数器只在 PL 复位时清零，重复运行 `--test` 时首字不为 0；工具按「相邻字递增」校验并打印 `ramp_start`。要严格核对「第 i 字 == i」，先重载码流。
- `ERROR` 置位后只能由 PL 复位清除，`START` 不清；`CTRL.RST` 软复位不复位 DMA 引擎，只会再触发一次 abort。

### Qt 与物理来源确认

```sh
ls -l /dev/fb0
cd /opt/qt_play
./run_arm_app.sh
```

上板验收顺序：

```sh
ls -l /dev/fb0
ls -l /dev/pl_ctrl
plctl probe
plctl sensors
```

振动面板在同一行显示 X / Y / Z / 合成 四个数值；波形图第 4 条曲线为逐样本 `sqrt(x²+y²+z²)`（原始计数域，按 `setScale` 换算与三轴同单位）；合成值不参与告警阈值判定。曲线有两种显示方式，由「频谱」按钮左侧的按钮切换，默认叠加：叠加为四条曲线共用一条纵轴，分道为四条曲线各占一条等高泳道（基线间距 = 绘图区高度 / 4，各道按自身中点居中但共用同一 px/LSB 增益，幅度可直接比较，虚线为零线）。

预期 `/dev/fb0` 存在且 HDMI 出现 Qt 界面，`/dev/pl_ctrl` 存在，`plctl probe` 读回 `ID=0x504c0102`，`plctl sensors` 的温度与振动三轴随传感器变化。任一设备缺失时先查设备树节点、内核日志与模块加载，不要用固定常数或演示数据代替。

1. 连接温度 UART 与振动模块；确认 `plctl sensors` 的对应计数持续变化，FRAME_TS 与通道时间戳按契约单位递增。
2. 对温度模块施加可控温度变化并与参考温度计对照；Qt 温度应随硬件变化，未接入前或 6 s 停更后显示不可用。
3. 改变振动模块姿态 / 施加振动；三轴原始码与 Qt 三轴数值应对应变化。停止 DATA_READY 或采集使能并确认振动计数停更，500 ms 后显示不可用；仅断开 SDIO 不保证控制器停止报 valid，应另验收器件错误检测。
4. 电流、转速始终显示“未接入 / 不可用”；NPU 诊断显示不可用。不得从 DATA.CNT、系统时间、固定常数或随机数产生物理读数。
5. FFT 读取 `0x78` 序号、`0x7C` 谱峰 bin、`0x80/0x84` 功率、`0x88` 标志，首尾校验序号。使用已知振动源核对频率与量程；实际 ODR 与频率误差须记录，不能用标称 3200 Hz 代替测量。

6. 完整频谱按 §4.6 读：`plctl spectrum --top 8`（默认 3200 Hz 折算频率，实际约 3123 Hz），或用 ps-local 的「频谱」视图。窗口字为 `BIN_POWER[k]=(|X_k|²)>>8`，有效量程覆盖 `|X|²` 约 2^39（`>>8` 后约 2^31）。判据为读取期间 `SEQUENCE` 单调、峰值 bin 的 `BIN_POWER` 与 `FFT_PEAK_POWER>>8` 一致、单音频点能量集中在对应 bin；`plctl spectrum` 只读快照，不干扰 DMA 环形运行。

### 板测顶层（PL 自检）

`soc_hdmi.bit` 之外的独立码流 `board_test.bit`（随 rootfs 放在 `/lib/firmware/`）在没有 PS 参与的情况下自检时域块与 FFT：

```sh
echo board_test.bit > /sys/class/fpga_manager/fpga0/firmware
cat /sys/class/fpga_manager/fpga0/state     # 期望 operating
```

LED 语义：`O_led[3]`（M20）done、`O_led[2]`（J15）特征通过、`O_led[1]`（R19）FFT 通过、`O_led[0]`（T19）状态（运行中慢闪 / 全通过常亮 / 失败快闪）；期望运行结束后 `O_led[3:0] = 4'b1111`。验收完成后用 `echo soc_hdmi.bit > /sys/class/fpga_manager/fpga0/firmware` 恢复显示码流。

## 7. 冻结条件

仅在契约 §11 三项、DMA 完成 / 数据内容、FFT 实际读出与生产 STA 判据均通过后冻结。生产 STA 判据为 `check_timing` 的 no_clock、invalid_constraint、combinational_loop 均为 0，SWNS / HWNS 非负，DRC 无 SEVERE 项；STA coverage 作为报告值记录，不设 100% 门槛（来源与复现见 §2.2）。交付目录须包含同一 SoC 的 system.bit、Design.xml、HPF、soc_fsbl.elf、内核 / dtb / rootfs、构建日志、版本 / 校验和与实际测试条件；生产码流与独立采集验证码流分目录保存。缺失的测试填写 TBD / 待测确认，不能填入估计的实测值。
