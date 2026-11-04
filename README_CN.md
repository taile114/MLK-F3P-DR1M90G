# MLK-F3P-DR1M90G · 设备智能运维 / 故障诊断（安路 DR1M90G FPSoC）

> 选题：**领域 5 — 设备智能运维 / 故障诊断**。
> 基于米联客 **MLK-F3P-DR1M90G**（芯片 **安路 DR1M90G**，PS = ARM + PL = FPGA）的故障诊断系统。
> 覆盖四个方向：**外设**（传感器载板 + 市售电机整套）、**PL**（FPGA 采集逻辑）、**PS**（Linux/Buildroot 驱动与应用）、**NPU**（FF6 六分支寿命推理；合并镜像基础共存及两轮 184 窗回放已实板验证，长期稳定性待定）。
> 英文版见 [`README.md`](README.md)。

---

## 1. 系统概览

被测电机是市售整套、独立运行，本系统只对它做采集。温度、振动、电流经 J12 接入 PL，OID 编码器经板载 J11 的 RS485 接入；四路采集统一在 `fpga_top` 工程。四路均已接入 SoC 寄存器 / DMA 与 PS 本地应用；转速已通过 PL USB UART 及看板实板验证，见 [`docs/RS485转速通道接入.md`](docs/RS485转速通道接入.md)。

```
传感器板 ─P1/J12(PL IO)─► PL：采集 IP（SPI/I²C/计数/比较）
                              └─ AXI 寄存器 + FIFO/VDMA ─► PS(Linux 驱动)
PS：读寄存器/收 DMA → 处理 → UDP 上位机 / HDMI 显示 / 存 SD
```

- 架构图：[`docs/MLK-F3P-DR1M90G-方案B-系统架构图.svg`](docs/MLK-F3P-DR1M90G-方案B-系统架构图.svg)
- 平台：米联客 MLK-F3P-DR1M90G；芯片 安路 DR1M90G（PS = ARM，PL = FPGA）。
- PL 工具链：安路 **TD**（+ design integrator / ChipWatcher）；PS：**Linux / Buildroot**。

### 四个方向

| 方向 | 内容 | 入口 |
|---|---|---|
| PL | 采集 IP、AXI 寄存器、中断、FIFO/VDMA | [`fpga/`](fpga/README.md) · [`docs/PL-PS接口约定.md`](docs/PL-PS接口约定.md) |
| PS | Linux/Buildroot、内核驱动、应用（UDP / HDMI / 存储） | [`docs/SD卡启动与Linux构建指南.md`](docs/SD卡启动与Linux构建指南.md) |
| NPU | 板上融合 CPU / FF6 六分支 CPU/NPU；合并镜像启动、基础共存及两轮 184 窗混合回放可用 | [`docs/PS-PL开发流程.md`](docs/PS-PL开发流程.md) §六 |
| 外设 | 传感器载板（原理图/PCB 完成） | [`docs/传感器板-说明.md`](docs/传感器板-说明.md) |

被测电机独立运行，与本板不通信。外设（传感器载板）已完成，电源 / 接口 / 接地 / 霍尔电流等全部约束见 [`docs/传感器板-说明.md`](docs/传感器板-说明.md)。

> 旧「电机板」（VNH5050A 桥 / Buck / ACS725 / ADC / CAN）已归档为 **`MLK-F3P-Motor`**（[`docs/Motor/`](docs/Motor/README.md)、`eda/archive/`），不再维护。其电路复杂；采用市售整套电机 + 传感器载板即可覆盖本选题的采集需求，因此不绘电机板。其器件数 / 网络名 / 引脚分配不适用于本板。

---

## 2. 仓库结构

| 路径 | 内容 |
|---|---|
| [`fpga/`](fpga/README.md) | **PL**（FPGA）代码：RTL / 约束 / TD 工程 |
| [`host/`](host/README.md) | 上位机：PS 端本地 Qt（HDMI）与通信电脑（UDP）；`vendor/` 未入库 |
| [`npu/`](npu/README.md) | 边缘 AI（NPU）部署：模型 / 训练 / 运行时封装；见 `npu/README.md` |
| [`docs/PL-PS接口约定.md`](docs/PL-PS接口约定.md) | **接口契约**（地址 / 寄存器 / 中断 / DMA / 版本规则） |
| [`docs/PS-PL开发流程.md`](docs/PS-PL开发流程.md) | PL / PS 开发步骤与交接、构建烧录总览（§五）、NPU（§六） |
| [`docs/SD卡启动与Linux构建指南.md`](docs/SD卡启动与Linux构建指南.md) | **PS**：SD 卡启动、内核 / U-Boot / Buildroot 构建与烧录 |
| [`docs/接口冲突与可用性分析.md`](docs/接口冲突与可用性分析.md) | SW1 启动模式、USB HOST/OTG、引脚复用冲突 |
| [`docs/传感器板-说明.md`](docs/传感器板-说明.md) | 外设：传感器载板（已完成） |
| [`docs/Motor/`](docs/Motor/README.md) | 已归档的电机板 `MLK-F3P-Motor` 文档（不再维护） |
| `platform/flash/` | PS 构建与 SD 烧录工作区；见 [`platform/flash/README.md`](platform/flash/README.md) |
| `platform/baremetal-sdk/`、`platform/factory-image/` | 厂商 SDK 与原厂镜像；**未入库**，仅作参考 |
| `docs/MLK-F3P-DR1M90G-Resource/` | 厂商资料包（**~34 GB，未入库**） |
| `eda/` | LCEDA 工程快照（外设，已完成）；`archive/` 内为电机板 |
| `tools/` | 辅助脚本；见 [`tools/README.md`](tools/README.md) |
| `flake.nix` | Nix devShell：ngspice / xschem / verilator / iverilog / gtkwave / python |

---

## 3. 快速开始

### 3.1 开发环境（NixOS，推荐）

```bash
nix develop        # 进入 devShell：ngspice / xschem / verilator / iverilog / gtkwave / python
```

非 NixOS（Ubuntu）按 [`docs/SD卡启动与Linux构建指南.md`](docs/SD卡启动与Linux构建指南.md) §1 安装依赖。安路 TD（Tang Dynasty）为闭源工具，需单独安装，不在 flake 内。

### 3.2 PL⇄PS 接口改动的标准三步

1. 先改 [`docs/PL-PS接口约定.md`](docs/PL-PS接口约定.md)
2. 双方确认后升版本
3. PL、PS 两端按契约各自实现

---

## 4. PS ⇄ PL 接口

- **接口以 [`docs/PL-PS接口约定.md`](docs/PL-PS接口约定.md) 为准**，PL、PS 两端各自实现。
- **最小寄存器集**（先冻结这 5 个）：

| 偏移 | 名 | 访问 | 位域 | 含义 |
|---|---|---|---|---|
| `0x00` | `ID` | RO | `[31:16] MAGIC` · `[15:0] VER` | 标识/版本（`pl_ctrl_probe()` 校验） |
| `0x04` | `CTRL` | RW | `[0] EN` · `[1] RST` · `[2] IRQ_EN` · `[7:4] MODE` | 使能 / 复位 / 中断使能 / 模式 |
| `0x08` | `STAT` | RO/W1C | `[0] BUSY` · `[1] OVF` · `[15:8] STATE` | 状态；`OVF` 写 1 清 |
| `0x0C` | `DATA` | RO | `[15:0] SAMPLE` · `[31:16] CNT` | 最新采样 + 计数 |
| `0x10` | `IRQ` | W1C | `[0] DONE` | 中断标志，写 1 清 |

- **基地址**由 TD design integrator 分配（核对 `Design.xml` 的 `DS*_BASEADDR0`），例程实测 `0x8000_0000`；PL 侧只有偏移、无 base。
- **规则**：只准「加」寄存器/位域/IP；改已有偏移或语义是破坏性改动 → 升版本 + 双方确认 + 两端同步实现。
- 完整契约：[`docs/PL-PS接口约定.md`](docs/PL-PS接口约定.md)；PL 代码目录：[`fpga/README.md`](fpga/README.md)。

---

## 5. 开发流程（PL / PS / NPU）

| 线 | 工具 | 产物 | 交接 |
|---|---|---|---|
| **PL** | TD（`fpga_prj.al`） | `fpga_prj.bit` → `system.bit` | ① IP 基地址 ② PL→PS 中断号 ③ bitstream |
| **PS/SoC** | FD（FSBL / bootgen） | `soc_fsbl.elf` | — |
| **PS** | `$LAB`（linux/u-boot/buildroot） | `uImage.lz4`·`system.dtb`·`u-boot.bin`·`rootfs.tar.gz` | — |
| **NPU** | 厂商 ArmNN 与硬核 / 软核 / CMA 驱动；合并 PL 含 `Video_NPU` | FF6 六分支量化模型、`libmlk_rul.so` 与封装运行包 | 合并镜像启动、两轮 184 窗混合回放及 HDMI / 采集 DMA 基础共存已实板验证；长期稳定性待定 |

**联调顺序**：PL 先单独通（ChipWatcher 看波形）→ PS 先单独通（`devmem` 读写）→ 合起来执行最小连通测试 → 接口冻结 v1.0。

**最小连通测试（3 条全过才算冻结）**：
1. PS 写 `CTRL[0]=1` → PL 有反应；
2. PL 端计数 → PS 读 `DATA.CNT` 递增；
3. PL 发一次 IRQ → PS 收到并计数 +1，且能写 `IRQ[0]=1` 清掉。

详细步骤见 [`docs/PS-PL开发流程.md`](docs/PS-PL开发流程.md)。

---

## 6. 构建与烧录

### 6.1 `BOOT.bin` 组成

```
BOOT.bin = fsbl.elf + system.bit(PL) + u-boot.bin     ← 只有这三样
FAT(BOOT) 分区 = BOOT.bin + boot.scr + uImage.lz4 + system.dtb
ext4 分区      = rootfs.tar.gz 解包
```

本板 QSPI 仅 **32 MB**，而 `rootfs.tar.gz ≈ 63 MB`，PS 系统放不进 boot 镜像。

### 6.2 PL 码流进入 PL 的三种方式

1. **固化进 BOOT.bin（量产）**：覆盖 `$LAB/boards/files/system.bit` → `create_image.sh`，上电由 FSBL 配置。
2. **运行时内核加载（迭代首选）**：
   ```bash
   scp pl_ctrl.bit root@<板卡>:/lib/firmware/      # 用 TD 的 ASCII .bit
   echo pl_ctrl.bit > /sys/class/fpga_manager/fpga0/firmware
   cat /sys/class/fpga_manager/fpga0/state         # 期望 operating
   ```
3. **JTAG / U-Boot**：FD `Program FPGA`；SD 启动下在线更新 PL 见 [`docs/PS-PL开发流程.md`](docs/PS-PL开发流程.md) §5.3.1；或 U-Boot `alfpga load`。

### 6.3 SD 卡启动（PS 侧，不改动 eMMC 出厂系统）

首次准备厂商 base rootfs / U-Boot / kernel 见 [`platform/flash/README_CN.md`](platform/flash/README_CN.md)。随后：

```bash
cd platform/flash
./stage_ps_local.sh && ./build_rootfs.sh        # PS 应用 / 驱动
./build_hdmi_dtb.sh && ./build_boot_bin.sh      # PL / DTB 变更时
lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS       # 确认 SD 设备
sudo env PATH="$PATH" ./burn_sd.sh /dev/sdX     # 会清空目标盘
```

烧完 `sync`；SW1 = **`ON-OFF-OFF`(SD)** 启动；`OFF-OFF-OFF`(eMMC) 回退。串口 `/dev/ttyACM0` @ 115200 8N1，登录 `root`/`root`。

完整命令与 NixOS 适配见 [`docs/SD卡启动与Linux构建指南.md`](docs/SD卡启动与Linux构建指南.md) §6。

### 6.4 SW1 模式与接口冲突

| 模式 | PIN1 PIN2 PIN3 | 说明 |
|---|---|---|
| JTAG | ON-ON-ON | TD 下载/调试、QSPI 固化 |
| QSPI | ON-ON-OFF | QSPI 启动 |
| SD | ON-OFF-OFF | SD 启动 |
| eMMC | OFF-OFF-OFF | 出厂系统 |

- `ON-OFF-OFF` 是 SD 启动位，不是 JTAG 位；JTAG 位是 `ON-ON-ON`。SD 启动下 JTAG 可用于在线配置 PL，不写 rootfs；QSPI 固化仍需 `ON-ON-ON` + FD `ProgramFlash`。PS 应用不通过 JTAG 更新。
- **USB 跳线 HOST** → 提供 WiFi + 4G + 2×USB HOST；**SLAVE** → 提供 OTG Type-C（失去整条 HOST 分支）。
- **SD / 网口 / HDMI / CAN / RS485 不受这条 USB 影响**；JTAG/UART Type-C 可自由插拔，不影响 4G/WiFi。
- 详见 [`docs/接口冲突与可用性分析.md`](docs/接口冲突与可用性分析.md)。

---

## 7. 团队协作

本仓库为 GitHub **私有仓库**。协作流程：

```bash
git switch -c feat/xxx        # 从 main 开分支
git push -u origin feat/xxx
gh pr create --fill           # 开 PR，评审后合并
git switch main && git pull
```

- **协作者**：仓库 Settings → Collaborators（或 `gh api -X PUT repos/<owner>/<repo>/collaborators/<user> -f permission=push`）。
- **分支保护**：Settings → Branches → 保护 `main`（要求 PR + 评审 + 状态检查）。
- **未入库资产**：`docs/MLK-F3P-DR1M90G-Resource/`（~34 GB 厂商资料包）与 `platform/baremetal-sdk/` / `platform/factory-image/` 不在 git 内，需另行分发。
- 提交前必做核对：

```bash
# 若改了接口：更新 docs/PL-PS接口约定.md（地址/寄存器/版本）
# 若动了 EDA（外设已完成）：先落盘，再 ./tools/sync_lceda.sh "描述"
```

---

## 8. 文档索引

| 主题 | 文档 |
|---|---|
| PL 代码 / 接口约束 | [`fpga/README.md`](fpga/README.md)、[`docs/PL-PS接口约定.md`](docs/PL-PS接口约定.md) |
| PL / PS / NPU 开发步骤 + 构建烧录总览 | [`docs/PS-PL开发流程.md`](docs/PS-PL开发流程.md) |
| 接口契约（地址/寄存器/中断/DMA/版本） | [`docs/PL-PS接口约定.md`](docs/PL-PS接口约定.md) |
| 上位机需求与设计（显示/链路/配置） | [`host/上位机需求与设计.md`](host/上位机需求与设计.md) |
| 上位机 ⇄ PS 接口（网络/串口帧、命令、加密） | [`docs/上位机-PS接口约定.md`](docs/上位机-PS接口约定.md) |
| PS 本地应用接口（数据源、PL 驱动 / NPU / PC） | [`docs/PS本地应用接口约定.md`](docs/PS本地应用接口约定.md) |
| 传输加密（ECDH / PSK，AES-256-GCM）使用说明 | [`host/common/README.md`](host/common/README.md) |
| NPU AI 接口（推理输入/输出/模型） | [`docs/NPU-AI接口约定.md`](docs/NPU-AI接口约定.md) |
| SD 卡启动 / 内核 / U-Boot / Buildroot | [`docs/SD卡启动与Linux构建指南.md`](docs/SD卡启动与Linux构建指南.md) |
| HDMI 本地显示通路（PL 设计 / dts / 内核驱动 / Qt） | [`docs/HDMI显示通路.md`](docs/HDMI显示通路.md) |
| 接口冲突与可用性（SW1 / USB / 引脚复用） | [`docs/接口冲突与可用性分析.md`](docs/接口冲突与可用性分析.md) |
| 外设：本板电路 / 器件 / 电源 / 接口 / 接地 | [`docs/传感器板-说明.md`](docs/传感器板-说明.md) |
| 文档 / 报告写作规范 | [`docs/文档与报告写作约定.md`](docs/文档与报告写作约定.md) |
| 电机板（已归档，不再维护） | [`docs/Motor/README.md`](docs/Motor/README.md) |
| 分区图 / 架构图 | `docs/构建与烧录-分区图.svg`、`docs/MLK-F3P-DR1M90G-方案B-系统架构图.svg` |
