# AGENTS.md — MLK-F3P-DR1M90G（安路 DR1M90G FPSoC / 领域 5 故障诊断）

本仓库的项目入口与协作约定。

## 这是什么

基于米联客 **MLK-F3P-DR1M90G**（芯片 **安路 DR1M90G**，PS = ARM + PL = FPGA）的故障诊断系统，覆盖四个方向：

| 方向 | 内容 | 入口 |
|---|---|---|
| PL | 采集 IP、AXI 寄存器、中断、FIFO/VDMA | `fpga/` · `docs/PL-PS接口约定.md` |
| PS | Linux/Buildroot、内核驱动、应用（UDP / HDMI / 存储） | `docs/SD卡启动与Linux构建指南.md` |
| NPU | 板端寿命模型推理与回放 | `npu/` · `docs/NPU-AI接口约定.md` · `docs/PS-PL开发流程.md` §六 |
| 外设 | 传感器载板（原理图/PCB 完成） | `docs/传感器板-说明.md` |

```
传感器板 ─P1/J12(PL IO)─► PL：采集 IP（SPI/I²C/计数/比较）
                              └─ AXI 寄存器 + FIFO/VDMA ─► PS(Linux 驱动)
PS：读寄存器/收 DMA → 处理 → UDP 上位机 / HDMI 显示 / 存 SD
```

被测电机（市售整套：电源适配器 + 调速器 + 755 电机）独立运行、与本板不通信，系统只做电流/传感器采集。

外设（传感器载板）已完成，电源 / 接口 / 接地 / 霍尔电流等全部约束见 `docs/传感器板-说明.md`；改动后在 LCEDA 落盘并按 `tools/README.md` 快照。

> 旧「电机板」（VNH5050A 桥 / Buck / ACS725 / ADC / CAN）已归档为 `MLK-F3P-Motor`（`docs/Motor/`、`eda/archive/`），不再维护。归档原因是该板电路复杂：采用市售整套电机（电源适配器 + 调速器 + 755 电机）+ 传感器载板即可覆盖本选题的采集需求，无须自绘电机板。其器件数 / 网络名 / 引脚分配只适用于电机板。

## 目录

| 路径 | 内容 |
|---|---|
| `fpga/` | **PL**（FPGA）代码：RTL / 约束 / TD 工程 |
| `host/` | 上位机：PS 端本地 Qt（HDMI）与通信电脑（UDP）；`vendor/` gitignored；见 `host/README.md` |
| `npu/` | 边缘 AI（NPU）：模型 / 训练 / 部署与推理封装；见 `npu/README.md` |
| `docs/PL-PS接口约定.md` | PL ⇄ PS 接口契约（地址 / 寄存器 / 中断 / DMA / 版本规则） |
| `docs/PS-PL开发流程.md` | PL / PS 开发步骤与交接、构建烧录总览（§五）、NPU（§六） |
| `docs/SD卡启动与Linux构建指南.md` | **PS**：SD 卡启动、内核 / U-Boot / Buildroot 构建与烧录 |
| `docs/HDMI显示通路.md` | HDMI 本地显示：PL 设计 / dts / 内核驱动 / 启动链 / Qt |
| `docs/接口冲突与可用性分析.md` | SW1 启动模式、USB HOST/OTG、引脚复用冲突 |
| `docs/选题符合性.md` | 选题四·应用方向 5：硬性任务 / 评分项对照、现状与缺口 |
| `docs/上位机-PS接口约定.md` · `docs/PS本地应用接口约定.md` · `docs/NPU-AI接口约定.md` | 上位机 ⇄ PS（网络 / 串口）、PS 本地应用（PL 驱动 / NPU / PC）、PS ⇄ NPU 推理接口；见 `host/上位机需求与设计.md` |
| `platform/flash/` | PS 构建/烧录工作区（脚本 tracked；`vendor/` = 解压的 Buildroot SDK，gitignored）；见 `platform/flash/README.md` |
| `platform/baremetal-sdk/` · `platform/factory-image/` | 官方 SDK（FSBL/裸机）/ 原厂镜像（**gitignored**） |
| `docs/MLK-F3P-DR1M90G-Resource/` | 厂商资料包（原理图 / pinlist / 教程 / SDK，**gitignored**） |
| `docs/传感器板-说明.md` | 外设：传感器载板全部说明（已完成） |
| `docs/Motor/` | 已归档的电机板 `MLK-F3P-Motor` 文档（不再维护） |
| `eda/` | LCEDA 工程快照（传感器外设） |
| `tools/` | 辅助脚本：EDA 快照 / 架构图 / 报告；见 `tools/README.md` |

## 关键约束

- **PS 系统不放入 `BOOT.bin`**：`BOOT.bin` 只含 `FSBL + PL bitstream + U-Boot`；内核/dtb/rootfs 放 FAT/ext4 分区（QSPI 仅 32MB，放不下）。PL 逻辑不进内核，迭代期用内核 FPGA manager 在线加载 `.bit`。详见 `docs/PS-PL开发流程.md` §五。

## PL ⇄ PS 接口

- 开发流程（PS/Linux + PL/TD 两条线）：`docs/PS-PL开发流程.md`（**§五 构建与烧录总览**：两线产物、`BOOT.bin` 组成、PL 码流三种加载方式）
- 接口契约：`docs/PL-PS接口约定.md`（地址/寄存器/中断/DMA/版本规则）
- 改接口 = 改契约文档 + 双方确认 + 升版本；PL、PS 两端各自实现
- PL 代码放 `fpga/`；协作细节见 `fpga/README.md`

## NPU / 寿命推理

板端融合 CPU 与选定 FF6 六分支模型已有数据集回放证据；上位机支持 FF6 封装、UDP v3 和运行包校验，共享库上板验收见 `docs/寿命模型-上位机接入.md`。合并镜像已完成两轮 184 窗混合回放、CMA 释放及 HDMI / 采集 DMA 基础共存验证，结果见 `fpga/uisrc/06_doc/验证结果.md` §5.6；实采适配与长期稳定性另行验收。

## 文档 / 报告写作

写文档、README、报告、提交说明时：

- 中立陈述，对事不对人；范围句描述内容，不用「你 / 队友」，也不写「给……看的」限定读者。
- 不用 emoji / 状态符号（`⚠️`、`✅`、`❌`、`✓`、`✗` 等）；状态用词：可用 / 受限 / 不可用 / 已修 / 待定；未定值用 `TBD` / `REF` / `OK`。
- 不用口语、教学 / FAQ 语气（「坑 / 搞定 / 打嗝 / 别把 / 为什么……」）。
- 不写历史：不写自更正、文档整理过程，也不写带日期的变更 / 修复 / 审计记录；只保留当前结论与事实。
- 接口契约、开发流程用规格 / 规程样式（字段级表格、步骤按执行顺序），不用报告体。
- 只写与仓库事实一致的内容；路径写成相对本文文件的正确路径并核对。
- 讲清是什么、为什么、怎么做。

详见 [`docs/文档与报告写作约定.md`](docs/文档与报告写作约定.md)。
