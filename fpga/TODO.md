# fpga — 现状、约束与注意点

本文件记录 PL 端（`fpga/`）的现状、关键约束与注意点。工程结构与接线见 [`README.md`](README.md)，编译仿真见 [`uisrc/06_doc/README.md`](uisrc/06_doc/README.md)。

## 1. 现状

| 项 | 状态 | 依据 |
|---|---|---|
| 温度 / 振动 / 电流 / RS485 转速四路统一到 `fpga_top` | 可用 | `uisrc/01_rtl/fpga_top.v`；统一码流 JTAG 下载、LED 状态及 PL 串口验证通过 |
| 电流通道连续转换（DR 860 SPS） | 可用 | `uisrc/01_rtl/ads1115_ctrl.v` |
| 四路仿真 | 可用 | 温度 `29+57+40` 项、振动 `10+4+2` 项、电流 `13+73` 项、转速 3523 项及解码 3 项，FAIL=0 |
| RS485 转速采集 / PL UART 读出 | 可用 | OID-R3808B-12S1S，Modbus RTU 9600 8N1；前 1000 帧成功计数连续，−78.955～+80.126 rpm，停止归零，无错误事件 / 溢出；长线抗干扰与长时间连续运行已验收 |
| TD 综合 / 布局布线 / 码流 | 可用 | `build_td.tcl` PASS，100 MHz、STA 100%，SWNS 1.674 ns / HWNS 0.051 ns；顶层无 CWC；未外接结果的特征逻辑可能被优化，资源与时序以 `validation_top` 为准 |
| 独立算法 / 寄存器 TD 验证壳 | 可用 | 100 MHz，SWNS 0.640 ns、HWNS 0.022 ns、STA 100%；LUT6 10754、寄存器 14347、DSP 170、ERAM 12 |
| J12 引脚分配（三路分离） | 已冻结 | [`../docs/PL-PS接口约定.md`](../docs/PL-PS接口约定.md) §9、`uisrc/04_pin/fpga_pin.adc` |
| SoC 候选码流 | 可用 | GUI 构建 `soc_candidate.bit` 成功，0 时序违例；`check_timing` 无效约束 0；STA coverage 99.91%（残差来自 PS 宏与厂商互联的分析覆盖限制，复现见 [`../docs/PL-PS接入与上板交付.md`](../docs/PL-PS接入与上板交付.md) §2.2），命令行 `build_soc.tcl` 按约束完整性判据通过并生成 `system.bit` |
| HDMI + 采集 + NPU 合并码流 | 可用（实板）/ 生产已收口 | `soc_hdmi.bit` 已实板启动；HDMI 1080P、Qt linuxfb、采集 DMA 与 FF6 硬件回放共存通过，2 h 连续运行已验收。SWNS 0.003 ns、HWNS 0.020 ns、STA coverage 99.90%，`no_clock=0`、`invalid_constraint=0`；I/O delay 与板级时序已按生产判据收口，见 [`uisrc/06_doc/验证结果.md`](uisrc/06_doc/验证结果.md) §3.2 / §5.6 |
| AXI 寄存器接入（`pl_ctrl`） | 可用（实板） | `soc_top` 与合并 `soc_hdmi` 均已接 AXI-Lite；合并码流挂 DS0 `0x80000000`；实板 `plctl probe` 读回 `0x504c0102`，温度 / 振动读取通过。高速 AXI / DMA 数据面由自研 `sensor_dma` 经 HP1 接入 |
| PL→PS 采集中断 | 可用 | 采集块完成按 `DMA_BLK_SEQ` 轮询通知，采集驱动保持 polling-only；合并设计保留 VDMA 位 0/1，软 NPU 使用位 15 / GIC 129，软核中断已注册并完成压力测试 |
| FIFO / DMA 数据搬运 | 可用（实板，含环形与长跑） | 自研 `sensor_dma` 经 PS 宏 HP1（`Slave_AXI_HP1_Interface=1`、数据宽度 64）写 DDR，寄存器窗口 `0x90～0xB4` 见 [`../docs/PL-PS接口约定.md`](../docs/PL-PS接口约定.md) §4.4；8 B/拍、突发 ≤16 拍、单 outstanding，突发缓冲为 16×64 移位队列，攒满整突发才拉高 AWVALID，W 数据取自队首寄存器，数据源断流时不开事务，避免半开事务挂住 HP 写通道；块完成通知按 `DMA_BLK_SEQ` 轮询。实板：ramp 4 KB / 10.1 µs（406.35 MB/s）、真实采集流 4 KB / 54.6 ms（75 KB/s，`OVF=0`）、环形 16 块连续读取 `jumps=0 lost=0`，采集龄 0.59～1.70 ms（中位 1.16 ms），读回校验通过；`ps-local` 采样走 DMA 环，长跑与 NPU 硬件回放并行已验收 |
| PL 时域特征提取 | 可用 | RTL、寄存器本体、仿真与 SoC 接入可用；实板精度与标定已对照通过 |
| PL 频域特征提取（FFT） | 可用 | 256/512 点矩形/Hann、摘要寄存器与 §4.6 完整谱窗口可用；实板摘要读数（`FFT_SEQUENCE` 递增、`FFT_PEAK_BIN` 随激励移动、`FFT_PEAK_POWER` 0.03～480.7）正常；谱峰与参考源的频率对照已验收 |
| 完整功率谱出口 | 可用（实板，v1.1 定标已复测） | 契约 §4.6 只读频谱窗口 `0x400～0x7FF`（N=256）/ `0x400～0xBFF`（N=512）：FFT 结果锁存时整窗拷贝到 PS 域快照，AXI 按 `0x400+4k` 顺序读出；PS 侧 `pl_read_spectrum`、`plctl spectrum` 与 ps-local 频谱视图已接。实板按 `>>16` 定标核对：晃动时 `BIN_POWER[peak_bin]` 与 `FFT_PEAK_POWER` 整数部分逐窗一致（480.7→480、159.1→159、73.6→73、41.5→41），bin 1 与 bin 255 镜像对称；v1.1 改为 `BIN_POWER[k]=(|X_k|²)>>8`（有效量程 `|X|²` 约 2^39，`>>8` 后约 2^31），静止读数约 8，新码流复测通过 |
| HDMI 显示通路 | 可用（实板） | HDMI 与采集已在 `soc_hdmi` 合并；HDMI + pl_ctrl 设备树、BOOT.bin、rootfs 已实板启动；`/dev/fb0` 与 Qt linuxfb 显示通过 |
| HPF / Design.xml | 已冻结 | `Design.xml` 已删除悬空复位块并补 ADC/SDC；`DS*_BASEADDR0` 与 `P2F_IRQ_*` 已回填契约；HPF 由 Design Integrator 正常导出，FSBL 与 HP0/1 端口使能一致 |
| FSBL / BOOT.bin | 可用（实板启动）/ 生产已收口 | HDMI 镜像使用厂商 HDMI FSBL（HP0/1=1）与含软 NPU 的 `soc_hdmi.bit` 重组（5,186,944 B，SHA256 `7fe00ab567682c2371e6ccc8444ccb2bae77bc010460b96f7f1cb3d149681c8a`）；生产 HPF 已由 Design Integrator 导出并重建 FSBL |
| 新增功能上板验证入口 | 可用（仿真 + 时序 + 实板） | `board_test_top` 以四组固定向量驱动时域块与 FFT，片上比较预期值，四只 LED 输出运行 / 特征通过 / FFT 通过 / 失败；独立 TB 与全量回归通过，100 MHz 布局布线 SWNS 0.642 ns / HWNS 0.025 ns，码流 `board_test.bit`。实板经 device tree overlay 加载（`platform/flash/load_bit.sh`），四只测试 LED 常亮即 `O_led[3:0]=1111`，与仿真一致；该顶层不含 AXI 出口，只以 LED 判定 |
| PL 内 AXI / P2F 自检 | 可用 | `validation_top` 覆盖寄存器、快照、W1C 与中断脉冲；新增引脚已先写入接口契约再写约束 |
| 转速 SoC / PS 接入 | 可用（仿真、构建与实板） | 接口 v1.2：`sensor_soc_axi` 在采集域例化 `encoder_modbus` / `encoder_rpm`，192 bit 快照经 `cdc_mailbox` 跨域，只读窗口 `0xB8～0xD0` 与 DMA 记录 `CH=3/4/5` 共用同一状态位图；驱动 `pl_read_speed` 以 seq-窗口-seq 取一致快照，`plctl speed` 与看板转速面板已接；signed32 转速不截断为 signed16，振动三轴帧语义不变。见 [`../docs/PL-PS接口约定.md`](../docs/PL-PS接口约定.md) §14 与 [`uisrc/06_doc/验证结果.md`](uisrc/06_doc/验证结果.md) §5.5 |
| 电流连续模式实板复测 | 可用 | 板测转换节奏约 862 Hz / 1160 us（设计 1163 us）；霍尔模块接入后电流幅值与链路已标定对照 |
| 各通道采样准确率对照 | 可用 | 温度、电流、转速、振动均已与参考源 / 万用表对照 |
| PS 侧驱动 / 应用 / 存储 / NPU | 可用（实板，含 2 h 共存） | 合并镜像包含 GP1 `0x1F0000000` 厂商 `Video_NPU`、HP1 写仲裁，以及配套内核 / 设备树 / rootfs。三个 NPU 设备创建、两轮各 184 窗 FF6 混合回放、CMA 卸载归零及 HDMI / 采集 DMA / 硬件自动回放同时运行已核对；均值 42.888 ms/窗，最大 FP32 预测差 0.048573 轮，2 h 连续运行验收通过。接线见接口契约 §6.1，实测见 [`uisrc/06_doc/验证结果.md`](uisrc/06_doc/验证结果.md) §5.6 |

## 2. 集成与验证记录

### 阶段 1：SoC 集成骨架与最小连通

目标：PL 采集接 AXI-Lite 寄存器、一次中断、一条 DMA 环，通过 [`../docs/PL-PS接口约定.md`](../docs/PL-PS接口约定.md) §11 的三条最小连通测试。

- [x] 新建 SoC 候选工程（`soc/prepare_soc.py` + `soc/build_soc.tcl` / GUI `soc.al`），以 `20_axi_lite_register` 为模板，接 `ARM_Processor_System + AXI_Matrix + sensor_soc_axi`；uiFDMA 未接入
- [x] 自研 `axi_lite_reg` 本体按接口约定 §4 实现 `ID / CTRL / STAT / DATA / IRQ`、通道数据、阈值、特征快照；SoC 挂接与 PS 寄存器读写已实板通过
- [x] 采集 / 时间戳 / 帧 / 特征 / FFT 摘要经 `sensor_capture` 与 `sensor_soc_axi` 接入 SoC 寄存器；`fpga_top` 不再是 SoC 顶层
- [x] P2F 单拍脉冲、W1C 清除、屏蔽保留与同拍事件 RTL；独立仿真与 SoC 中断映射、PS 联调通过
- [x] 写 DDR 通路由自研 `sensor_dma` 经 HP1 实现（8 B/拍、突发 ≤16 拍），未使用厂商 `uiFDMA` / VDMA；同时补齐环形缓冲、`DMA_BLK_SEQ`、`DMA_IRQ` 挂起标志与 `DMA_CTRL[4]` 优雅停止，`tb_sensor_dma` PASS=3699、`tb_dma_ring_irq` PASS=360
- [x] 基地址 `0x8000_0000` 由实板 `plctl probe` 验证；传感器 IRQ 接线与 GIC ID 已实测回填契约 §5
- [x] PS 侧寄存器 / 字符设备实板读写通过（`plctl probe` / `plctl sensors`）
- [x] 采集块完成通知采用 `DMA_BLK_SEQ` 轮询，`plctl irq` 对采集线返回 polling-only；VDMA 位 0/1 与软 NPU 位 15 的既有分配保持

判定：§11 三条通过（`CTRL[0]=1` 有反应、`DATA.CNT` 递增、一次 IRQ 收到并可清）。

### 阶段 2：PL 时域特征提取

目标：在 PL 内对滑窗样本实时算时域特征，对应选题五「PL 硬件特征提取」。

- [x] 定义 N=256/512、默认步长 128 及七项特征；见 [`uisrc/06_doc/时域与频域特征.md`](uisrc/06_doc/时域与频域特征.md)
- [x] 实现四路滚动和、中心矩扫描、分周期开方/除法；振动三轴与电流独立窗口
- [x] 实现特征保留、原子快照寄存器与特征就绪中断本体，并接 SoC 与 DMA 输出
- [x] 256/512 独立 TB 纳入 `run_sim.sh`；并入 `fpga_top`，四路结果与快照集成仿真可用
- [x] 采集共同时基：`timebase` 提供 100 MHz `r_ts`、1 kHz `tick` 与毫秒计数；三路 valid 接收拍锁存时间戳，独立与顶层仿真可用
- [x] 保持 1 kHz 最新值对齐锁存，不做器件读点触发。ADXL345 按自身 ODR 连续转换、不可命令采样，温度自主上报，两者无法对齐；唯一可对齐的电流改单次转换会压低 860 SPS 吞吐、且需解决「样本就绪」判定，收益低、风险高。器件 ODR / DR 与控制器节奏独立，不改变原始特征样本流
- [x] 样本时间戳随 1 kHz 对齐帧进入独立寄存器本体 `0x54～0x74`；温度按到达即时间戳，口径为「准同步」；帧值与对应时间戳共同锁存，首尾读 FRAME_TS 校验一致性。SoC 读出、特征窗口时间戳与 DMA 已接通，原特征快照语义保持，见 [`../docs/PL-PS接口约定.md`](../docs/PL-PS接口约定.md) §4.3
- [x] 板测量 `tick` → 各通道锁存时差（同步精度）：对齐帧周期 999.936 us（21 帧，帧计数严格 +1）、振动 320.2 us（约 3123 Hz，DATA_READY / `acc_vld` / 样本计数均为 66，无漏采）、电流约 1160 us（862 Hz）。见 [`uisrc/06_doc/验证结果.md`](uisrc/06_doc/验证结果.md) §5

以上完成项覆盖 RTL、仿真与实板。生产码流中的未外接特征逻辑已被优化，不能据此验证特征计算；特征资源与时序以 `validation_top` 为准。

### 阶段 3：频域与扩展

目标：频域特征（选题五「硬件实时频域」）与转速通道。

- [x] 时域与硬件实时 FFT 均在 PL；振动 X 轴 FFT 接入窗口、去 DC 与峰值寄存器；FFT 结果与 DMA 已实板读出
- [x] 采用自研 radix-2 DIT FFT，串行蝶形、同步 ERAM 缓存、旋转因子 ROM；见特征规格
- [x] 256/512 点 signed32 Q8、逐级 1/2 缩放、矩形/周期 Hann，四配置独立仿真可用
- [x] RS485 转速采集并入统一 `fpga_top`，板载 J11、Modbus RTU / CRC、signed32 速度及 rpm 换算、PL UART 遥测；统一码流与实板 LED / 正反转 / 归零验证通过，见 [`../docs/RS485转速通道接入.md`](../docs/RS485转速通道接入.md)
- [x] 转速接入 SoC 采集层、CDC、寄存器 / DMA、PS 驱动与应用；接口 v1.2 写入契约（只读窗口 `0xB8～0xD0`、DMA `CH=3/4/5`），驱动与看板已接，见 [`../docs/RS485转速通道接入.md`](../docs/RS485转速通道接入.md) §6～§7
- [x] 本地加密存储与 4G / 千兆网接入完成，NTP 网络对时可用

### 阶段 4：新增功能独立上板验证

目标：提供无需 SoC 的功能自检与结果观测，再验证真实传感器特征链路。

- [x] 实现独立板测顶层：固定向量驱动时域块与 FFT，片上比较预期结果，通过 LED 输出运行 / 通过 / 失败状态
- [x] 实现 PL 内 AXI 读写与 P2F 自检，核对寄存器、快照、W1C 与中断脉冲，不依赖 PS
- [x] 真实传感器特征增加结果保留与读出通路，数值、窗口序号及 overrun 已核对
- [x] 板测顶层完成独立 TB、全量回归与 100 MHz 布局布线检查，SWNS/HWNS 非负后生成板测码流
- [x] 下载板测码流，核对自检判定与真实传感器特征输出，结果见 [`uisrc/06_doc/验证结果.md`](uisrc/06_doc/验证结果.md)

`validation_top` 使用内部激励与签名负载，没有预期结果比较和通过 / 失败判定，不作为功能板测顶层。

当前交付边界：四路采集统一顶层、RS485 串口与 SoC / PS 通道、时域 / FFT / AXI / P2F RTL 与可复现仿真；转速并入合并码流 `soc_hdmi` 并已实板核对正反转、停机归零与看板波形，CAN 备用。TD 资源/时序见 [`uisrc/06_doc/验证结果.md`](uisrc/06_doc/验证结果.md)。SoC 候选接入层、PS 驱动、Qt 与 rootfs 交付状态见 [`../docs/PL-PS接入与上板交付.md`](../docs/PL-PS接入与上板交付.md)；合并镜像的 NPU 混合后端及 HDMI / 采集 DMA 共存已实板确认，生产时序与长期压力验收完成。

### 阶段 5：PL → PS 交接与转 FD

目标：PL 侧收口，产出转 PS/SoC（FD）所需交接物。

- [x] AXI 时钟域与 CDC：寄存器 / 互联跑 PS 的 AXI 时钟（50 MHz），采集跑 100 MHz；跨域用异步事件 FIFO / 握手邮箱；仿真与 SoC 布局布线通过
- [x] 时序收口：`fpga_top` STA 100%；SoC 候选 0 违例、无效约束 0；STA coverage 99.91% 为工具与厂商 IP 侧残差，命令行判据按约束完整性与非负裕量执行并通过
- [x] PL→PS 采样流：`sensor_dma` 经 HP1 写 DDR（单次块传输与环形缓冲均已实板验证：ramp 4 KiB 406.35 MB/s、真实采集流 75 KB/s、16 块环形测试 0 跳号；`tb_sensor_dma` 3699 项通过）；PS→PL 配置（阈值 / 标定系数 / 模式）已具备寄存器 RTL；块完成通知按 `DMA_BLK_SEQ` 轮询
- [x] 带宽 / 延迟：64 bit 数据宽度、16 拍突发、4 KiB 块按 HP1 定档，实测远低于通道容量；端到端延迟由 `PL_TS_NOW` 与样本 `TS` 同基求差（ps-local 显示采集龄），与 Linux 系统时钟对时及跨端换算完成
- [x] 交付物：生产 `system.bit`（约束完整性 + SWNS/HWNS 非负 + DRC 干净）、`Design.xml` 的 `DS*_BASEADDR0` 与 `P2F_IRQ_*` 已回填契约（`REF`→`OK`）、经 Design Integrator 正常导出的 `.hpf`；候选 `soc_candidate.bit`、合并 `soc_hdmi.bit`、HPF、FSBL 与 BOOT.bin 齐备

判定：契约 §11 三条最小连通通过，且上述交付物齐备，已转生产交付。

### 阶段 6：FD / BOOT.bin / 上板

目标：用候选 SoC 完成可复现的上板启动，再完成生产收口。

- [x] FD Platform 工程 `soc_base` 与 Application 工程 `soc_fsbl` 生成（生产 HPF）
- [x] FSBL 构建：`fpga/uisrc/out/soc/fd_ws/soc_fsbl/build/FSBL.elf`
- [x] HDMI + 采集合并码流：`fpga/uisrc/out/soc_hdmi/soc_hdmi.bit`
- [x] HDMI + pl_ctrl 设备树：`platform/flash/build_hdmi_dtb.sh`，节点 `pl-ctrl@80000000`，无 interrupts
- [x] `pl_ctrl` IRQ 可选：无 IRQ 时 probe 成功，read / pread 正常，poll 返回不可用
- [x] rootfs：`stage_ps_local.sh` + `build_rootfs.sh`，含 `pl_ctrl.ko`、`plctl`、Qt 与自启脚本
- [x] BOOT.bin 组装（`soc_hdmi.bit` + HDMI FSBL + U-Boot）：`platform/flash/build_boot_bin.sh`
- [x] 确认 HDMI FSBL 的 `Slave_AXI_HP0_Interface=1`；生产 HPF 的 HP0/1 使能一致
- [x] 写 SD 卡并按 SW1=SD 启动，确认串口 FSBL / U-Boot / Linux 输出
- [x] `plctl probe` 读回 `ID=0x504c0102`；`plctl sensors` 读取真实温度 / 振动
- [x] 验证 `/dev/fb0`、HDMI Qt linuxfb、`/dev/pl_ctrl` 绑定、真实温度 / 振动读数与停更处理
- [x] 核对 VDMA GIC 82/83 与显示中断：fabric 中断只有 SPI 82/83 两路且属 VDMA，`f2p_irq[3]` 未引出（实板驱动后 GIC 计数恒为 0），DMA 块完成改为轮询；契约 §4.4 与 §5 已按此更新
- [x] 合并 `soc_hdmi` 的 PS 宏已引出 `slave_hp0/1`，HP1 为 64-bit，采集 DMA 与软 NPU 写通路经仲裁接入

## 3. 关键约束

### 3.1 采样率决定特征目标

| 通道 | 器件 | 速率上限 | 可做特征 |
|---|---|---|---|
| 振动 | ADXL345 | 3200 Hz × 3 轴 | 时域 + 频域（唯一高速通道，DMA 与 FFT 的目标） |
| 电流 | ADS1115 | 连续转换，DR 860 SPS（器件上限） | 有效值 / 趋势 / 过载；见 `../docs/ADS1115/ads1115-3.pdf` |
| 温度 | GT-M601 串口板 | 主动上报，周期随模块设置 | 趋势与阈值 |
| 转速 | OID-R3808B-12S1S，RS485 / Modbus RTU | 测速采样时间 100 ms、事务间等待 100 ms；实测主机帧间隔中位 131 ms，含 USB / 调度 | 转速趋势；阈值与准确度已对照 |

- 电流：连续转换已把读取节奏提到器件上限 860 SPS；`ads1115_ctrl` 每 DR 周期读一次 Conversion，不再逐样本写 Config 与轮询 OS。代价是没有 ALERT/RDY 时无法用 OS 判定“新样本就绪”，只能定时读取；采样绝对精度已与参考表对照。
- 电流与温度通道的物理上限仍限制高频特征：ADS1115 上限 860 SPS，做不了 MCSA；温度受板上报周期限制。频域特征面向振动。
- 振动采样跟随器件 ODR，改采样率需同步改 `adxl345_ctrl` 的 `BW_RATE`。改 ADS1115 数据率改 `DATA_RATE`（3'b111 = 860 SPS）。

### 3.2 无厂商 PL FFT IP

- 资料包内只有 PS 侧 NMSIS / CMSIS-DSP 的 FFT；TD 内 `info commands *fft*` 为空、无 IP catalog 命令。
- 频域特征须自研 RTL，或放 PS / NPU。DR1M90G 资源（LUT 94464、DSP 240、ERAM 280×20K）足够 256 / 512 点 FFT。

### 3.3 硬件与电气

- J12 为 BANK11，固定 3.3 V；双向线（SPI SDIO、I2C SCL / SDA）PL 侧关闭内部上下拉。
- 工业总线归属：**CAN 为 PS 硬核**（`ARM_Processor_System` CAN0/1，I/O 经 PL BANK33 引出，Linux socketcan 驱动）；**RS485 为 PL 逻辑**（软 UART + DE 方向控制，板载 `SP3485EN`）。本方案转速走 RS485（PL）；CAN 备用，若用则在 PS 侧驱动，见 [`../docs/接口冲突与可用性分析.md`](../docs/接口冲突与可用性分析.md)。
- 传感器供电由传感器载板 `J2`（`+5V`）与 `H1`（`+3V3`）提供；霍尔模块电流端经 `U1` / `U2` 串入电机回路。见 [`../docs/传感器板-说明.md`](../docs/传感器板-说明.md)。
- 电机电流 / 高压不得引到 J12 信号脚。

### 3.4 接口契约

- 寄存器、地址、中断、DMA 以 [`../docs/PL-PS接口约定.md`](../docs/PL-PS接口约定.md) 为唯一权威；PL、PS 两侧据此实现。
- 冻结后只准「加」；改已有偏移或语义 → 升版本 + 双方确认 + 两端同步。
- DMA 缓冲区物理地址须与 PS 的 reserved-memory / CMA 约定一致，否则写系统内存会异常。

### 3.5 TD 环境（NixOS）

- TD Release 2026.2 (NL) 工程模式走 `open_project` → `elaborate` → `read_adc` / `read_sdc`；SoC 候选使用非工程 `read_hdl`，已能完成 elaborate / place / route。两条流程见 [`uisrc/06_doc/README.md`](uisrc/06_doc/README.md)。
- 集成顶层与 `build_td.tcl` 均不含 `ui_cwc` 或 `insert_debugger`；需要 ChipWatcher 时对调试副本单独插入，不进入交付码流。`run_hw_cw` 触发抓取在该版本会段错误，须用 GUI 抓取导出 CSV。
- `build_td.tcl` 检查 `sys_clk`、SWNS/HWNS 非负与 STA 覆盖率 100%；`build_soc.tcl` 另检查 `check_timing` 的 no_clock、invalid_constraint、combinational_loop 均为 0，并把 STA coverage 作为报告值记录而不设门槛（含 PS 宏的工程达不到 100%，来源与复现见 [`../docs/PL-PS接入与上板交付.md`](../docs/PL-PS接入与上板交付.md) §2.2）。PASS 不等于 DRC、板级 I/O 时序或实板功能通过。
- 设计整合器块设计无 Tcl API；避开 GUI 的做法是以厂商例程（如 `20_axi_lite_register`）为模板做文本改，再从工程模式构建。ChipWatcher 上板抓取只能 GUI。

## 4. 注意点

- 采集数据当前只在顶层内部，未接使用者，可能被综合优化；接入寄存器 / DMA 前不要以“已采集到数据”作为依据，需经 ChipWatcher 或寄存器读出。
- 生产 `fpga_top` 未接 AXI / DMA，四路时域与快照无对外读出，已被综合优化；生产码流不含可用特征逻辑，不能用于验证特征计算，特征资源与时序以独立 `validation_top` 为准。
- 厂商 I2C 已改为 `I_clk` 边沿使能，不再存在 `scl_clk` 内部时钟；`fpga_top` STA 覆盖率 100%。新增内部时钟仍须自行加 `create_generated_clock`。
- `02_sim` 不进入综合；`temp_test.v` / `adxl345_test.v` / `ads1115_test.v` 仅为各路独立验证顶层。
- `uisrc/01_rtl/axi_lite_reg.v` 为自研寄存器本体，与厂商同名模块不得同时加入 SoC 源文件；`uiFDMA` 接入前确认 TD 版本与许可。
- 中断必须可由 PS 清除，避免持续触发。
- 对接口的符合性见 [`../docs/选题符合性.md`](../docs/选题符合性.md)：PL 硬件功能、PS 系统 / 界面、SoC/AXI 联调、NPU 部署、工业总线与量化测试报告均已闭环。
- 三路器件保持独立节奏，PL 共同时基与 1 kHz 帧对齐锁存已板测；振动 / 电流为对齐锁存，温度为准同步。器件读点触发不采用，不声明物理同时采样。口径见 [`../docs/选题符合性.md`](../docs/选题符合性.md) §4。
- JTAG 线（FTDI `0403:6042`）已被主机识别；SD 启动下的 `Program FPGA` 按 [`../docs/PS-PL开发流程.md`](../docs/PS-PL开发流程.md) §5.3.1 的前置条件执行。

## 5. SoC / PS 交付现状

候选工程入口为 [`soc/prepare_soc.py`](soc/prepare_soc.py) 与 [`soc/build_soc.tcl`](soc/build_soc.tcl)。事件 FIFO、帧 / 特征邮箱、外部帧寄存器和 FFT 峰值寄存器经 `tb_sensor_soc_axi` 验证。

- CURRENT_ZERO / CURRENT_GAIN 可读写、四字节掩码、硬复位与软复位已覆盖。
- 顶层无 CWC；I2C 以 I_clk 边沿使能处理，不再用 scl_clk 作寄存器时钟。原采集验证顶层不作为生产 SoC。
- SoC 候选已有 GUI 码流 `soc_candidate.bit`、合并 `soc_hdmi.bit`、FSBL、BOOT.bin；命令行 `build_soc.tcl` 按约束完整性判据通过，`build_hdmi_soc.tcl` 已生成 0 违例码流。
- HPF / Design.xml 已由 Design Integrator 导出；§11 的 AXI-Lite 寄存器 / 字符设备已在实板通过；IRQ 冻结、HP 端口与生产 STA 已收口。
- 不得把独立采集码流或厂商 HDMI 码流改名为本项目生产 system.bit。

## 6. PS 时间同步

- 无 RTC 时：主机运行 `tools/sync_board_time.sh`，经 `/dev/ttyACM0` 设置 UTC；每次上电后执行一次，不依赖网络。
- 已接入 4G / WiFi：PS 启用 NTP/chrony 自动对时，串口对时保留为离线回退；记录文件名、`.mlkf` 时间戳与 mtime 随本地记录保存，关机不要求 RTC 保持。

## 7. 参考资料

| 主题 | 路径 |
|---|---|
| 选题要求（领域五、硬性任务、评分） | [`../docs/竞赛题目及要求.pdf`](../docs/竞赛题目及要求.pdf) |
| AXI-Lite 寄存器例程 | `../docs/MLK-F3P-DR1M90G-Resource/03_demo/3-2_ex_fpsoc/3_2_01_sdk_base/20_axi_lite_register/` |
| PL→PS 中断例程 | `../docs/MLK-F3P-DR1M90G-Resource/03_demo/3-2_ex_fpsoc/3_2_01_sdk_base/13_sdk_pl_intr/`、`14_axi_gpio_pl_intr/` |
| FDMA / VDMA 例程（含 ADC 流） | `../docs/MLK-F3P-DR1M90G-Resource/04_培训demo/01_fdma_vdma_20251010/fdma_vdma_adc_20251008/`、`.../3_2_01_sdk_base/17_axi_DMA/` |
| PL / PS 开发与构建流程 | [`../docs/PS-PL开发流程.md`](../docs/PS-PL开发流程.md) |
| 接口契约 | [`../docs/PL-PS接口约定.md`](../docs/PL-PS接口约定.md) |
| ADS1115 与 M601 模块资料 | `../docs/ADS1115/`、`../docs/M601/`（本地，未入库） |
