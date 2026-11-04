# PS / PL 开发流程（MLK-F3P-DR1M90G）

> PS（Linux）与 PL（TD）两条线的开发步骤、交接物与构建 / 烧录流程；NPU / 寿命模型见 §六。
> 平台 / 芯片：米联客 MLK-F3P-DR1M90G / 安路 DR1M90G（PS=ARM，PL=FPGA）。
> 工具链：PL = 安路 TD（+ design integrator / ChipWatcher）；PS = Linux / Buildroot（+ 交叉工具链 / 内核驱动）。
> 接口定义见 [`PL-PS接口约定.md`](PL-PS接口约定.md)；PL 代码约束见 [`../fpga/README.md`](../fpga/README.md)。

```
        ┌─ PL 线（FPGA）──────────────┐        ┌─ 交接 ─┐        ┌─ PS 线（Linux ARM）──────────┐
        │ TD 写 RTL / 搭 SoC / 出 bit  │───►  地址 DS*_BASEADDR0  ──►│ 虚拟机 + 交叉编译 + 内核驱动  │
        │ JTAG 下载 + ChipWatcher      │        + PL→PS IRQ 号        │ + 应用 / 网络 / HDMI / 部署   │
        └─────────────────────────────┘                              └───────────────────────────────┘
```

---

## 一、PL 开发（TD）

前提：安装安路 TD（随板版本，如 `TD5.9.1`）并配置 License。

本仓库独立采集入口为 [`fpga.al`](../fpga/uisrc/fpga.al)，顶层 `fpga_top`，包含温度、振动、电流、RS485 转速四路。统一码流已完成 JTAG 与 PL 串口基础实板验证，接线及观测步骤见 [`RS485转速通道接入.md`](RS485转速通道接入.md)；SoC / HDMI 入口与该采集顶层分开构建，转速尚未接入 SoC / PS。

| # | 操作 | 命令 / 路径 | 产物 |
|---|---|---|---|
| 1 | 建 / 开工程 | 复制例程 `…/3_2_01_sdk_base/20_axi_lite_register/`，用 TD 打开 `soc_prj/fpga_prj.al` | TD 工程 |
| 2 | 写 RTL | `uisrc/01_rtl`；寄存器 / 地址按 `PL-PS接口约定.md` §3/§4 | RTL |
| 3 | 搭 SoC（含 PS 时） | TD → design integrator：ARM PS + AXI IP + 互联 / 中断，并分配地址 | `Design.xml`（`DS*_BASEADDR0`、IRQ 号） |
| 4 | 仿真 | ModelSim（例程 `…/00_for_starter/…/03_td_modelsim_sim`） | 波形 |
| 5 | 综合 / 布局布线 | TD：Synthesis → Place&Route | bitstream |
| 6 | 下载 | UART&JTAG 二合一 Type-C + TD downloader；SW1 拨 JTAG；SD 启动下在线配 PL 见 §5.3.1 | — |
| 7 | 板内调试 | ChipWatcher（例程 `…/00_for_starter/…/04_use_ChipWatcher`） | 波形 |
| 8 | 交底给 PS | 把 §3 的 IP 基地址与中断号写入 `PL-PS接口约定.md` §3/§5 | 接口真值 |
| 9 | 固化 | 覆盖 `boards/files/system.bit` → `create_image.sh`，或运行时由 PS 加载（§五） | `system.bit` |

- 器件：`DR1M90GEG400` / speed `-2`（GEG400 = 17×17mm / 0.8mm pitch）；`DR1M90MEG484` / `…GEG484` 为 19×19 的另一封装，不适用。TD 版本 5.9.1 以上。
- PL 侧寄存器定义只有偏移、无 base：AXI 从设备只见到窗口内的低位地址，基址由互联逻辑处理。

### 1.1 TD 工程目录与两种建法

例程 `3_2_01_sdk_base/20_axi_lite_register` 的目录：

```
<proj>/
├── soc_hw/fpga_prj.hpf        # 硬件平台（zip，内含 HPFs/Design.xml）→ PS+PL 的 SoC 描述
├── soc_prj/                   # ← TD 工程（PL 侧）
│   ├── fpga_prj.al            #   工程文件（TD 里打开这个）
│   ├── .design.prj            #   顶层 RTL 清单（例程 = uisrc/01_rtl/system）
│   ├── uisrc/{01_rtl,02_sim,03_ip,04_pin}   # RTL / 仿真 / IP / 管脚约束
│   ├── al_ip/                 #   安路 IP
│   └── fpga_prj_Runs/{syn_1,phy_1}          # 综合 / 布局布线
└── soc_sdk/                   # PS 侧（米联客 SDK / Eclipse）：axi_lite_register + soc_base
```

- 建法 1（复制例程）：复制 `20_axi_lite_register`（AXI 寄存器读写，与 `PL-PS接口约定.md` §4 同一套路）或 `14_axi_gpio_pl_intr`（PL→PS 中断）→ TD 打开 `soc_prj/fpga_prj.al` → 只改 `uisrc/01_rtl` 与 `uisrc/04_pin`。
- 建法 2（从零）：TD → `File → New Project` → 器件 `DR1M90GEG400` / speed `-2` → 加 RTL 与管脚 / 时序约束 → 需要 PS 时开 design integrator（ARM PS + AXI IP + 互联 / 中断）→ 由它分配地址 → 生成 `Design.xml` → 综合 → 布局布线 → bit → JTAG 下载（SW1 拨 JTAG）+ ChipWatcher。
- 教程：`00_for_starter/米联客-安路FPGA TD软件快速入门课程.pdf`（p24 建工程 / p32 加 IP / p50 编译 / p54 下载 / p67 ChipWatcher）；`3-0_book_all/3_2_01_米联客2024版FPSoc_SDK入门篇.pdf`。

---

## 二、PS 开发（Linux）

| # | 操作 | 命令 / 参考 | 产物 |
|---|---|---|---|
| 1 | 部署虚拟机环境 | 米联客 Ubuntu 虚拟机（预装交叉工具链 / SDK）→ `…/01_start/02_start_Linux/01_prepare/01-1_…部署虚拟机环境.pdf` | 构建主机 |
| 2 | 装串口终端 | PuTTY → `01-2_…PuTTY安装与使用.pdf` | — |
| 3 | 验证出厂系统 | SW1 拨 SD → 插出厂 SD → 上电 → 串口看 log → `02_test_board/02-1_…Linux出厂系统测试.pdf` | 基线确认 |
| 4 | 构建系统 | 厂家镜像（`03-1_…恢复出厂设置.pdf`）；要改走 Buildroot 二次开发（`04-1_…Buildroot篇.pdf`）；本仓库流程见 `SD卡启动与Linux构建指南.md` | rootfs / 内核 |
| 5 | 写内核驱动（主要工作） | `3-4-02_…Linux驱动开发篇.pdf`：`request_mem_region` + `ioremap` + `readl/writel` + `request_irq`；寄存器 / 中断按 `PL-PS接口约定.md` §4/§5 | 字符设备驱动 |
| 6 | 写应用 | 交叉编译 C/C++/Qt/Python → scp 到板子；读寄存器 / DMA → 处理 | 应用 |
| 7 | 通信上位机 | UDP（`3_1_02_…UDP以太网通信方案.pdf`）/ TCP / 串口 / USB | 数据链路 |
| 8 | 显示 | HDMI 1080p60：PL 显示设计 + 内核 `anlogic,vdmafb` 驱动 → `/dev/fb0`；见 `HDMI显示通路.md` | 显示 |
| 9 | 部署固化 | 烧 SD / eMMC；SW1 选 SD / QSPI 启动 | 启动介质 |
| 10 | 联调 | `pl_ctrl_probe()` 校验 base / 版本 → 执行 `PL-PS接口约定.md` §11 三条最小连通测试 | 接口冻结 |

---

## 三、交接与联调顺序

1. PL 单独通：ChipWatcher 看到波形 → 出 bitstream。
2. PS 单独通：出出厂系统 → 装驱动 → `devmem` 手工读写确认地址。
3. 合起来：按 `PL-PS接口约定.md` §11 三条 → 通过即接口冻结（v1.0）。
4. 增量：加 DMA → 加 UDP / HDMI → 加算法。

---

## 四、交接物

| 方向 | 内容 |
|---|---|
| PL → PS | ① `Design.xml` 的 `DS*_BASEADDR0`（地址） ② PL→PS 中断号 ③ bitstream |
| PS → PL | 无（PS 按地址访问；寄存器语义由 `PL-PS接口约定.md` 双方共同维护） |

---

## 五、构建与烧录

- PL 逻辑不经过内核：内核运行于 PS；PL 码流由 FSBL（开机）或运行时的内核 FPGA manager / JTAG（在线）配置。
- `$LAB` 构建工程只做「编译 PS + 把 FSBL / bit / u-boot 打成 `BOOT.bin`」。内核 / dtb / rootfs 不放进 `BOOT.bin`（见 5.2）。

### 5.1 两条线各自产出什么

```
PS 线（$LAB 编）                              PL 线（TD / FD 出）
  kernel   → uImage.lz4                          RTL/IP → .bit（采集逻辑）
  dtb      → system.dtb                          design integrator → 地址/中断(Design.xml)
  u-boot   → u-boot.bin                          FSBL   → soc_fsbl.elf（PS 初始化 + 配置 PL）
  buildroot→ rootfs.tar.gz                              |
        \___________________  合体（create_image.sh） ___/
                             BOOT.bin = FSBL + system.bit + u-boot.bin
                             + boot.scr / uImage.lz4 / system.dtb / rootfs.tar.gz
                                          ↓
                              1 个启动介质（SD / QSPI / eMMC）
```

| 线 | 工具 | 产物 | 落到 |
|---|---|---|---|
| PL | TD（`soc_prj/fpga_prj.al`） | `fpga_prj.bit` → 改名 `system.bit` | `$LAB/boards/files/system.bit` |
| PL | TD design integrator | 基地址 / 中断（`Design.xml` 的 `DS*_BASEADDR0`） | 写进 `PL-PS接口约定.md` §3/§5 |
| PS/SoC | FD（FSBL 模板 / bootgen） | `soc_fsbl.elf` | `$LAB/boards/files/soc_fsbl.elf` |
| PS | `$LAB`（linux/u-boot/buildroot） | `uImage.lz4`·`system.dtb`·`u-boot.bin`·`rootfs.tar.gz` | `$LAB/boards/DR1M/buildroot/images/` |

- 只改 PL 时 FSBL 一般不用重生成（它只把 bit 当数据搬进去）；只有改 PS 外设 / 时钟才要用 FD 重出 FSBL。
- FD 2026.2 的本机路径、Platform / Application 工程、NixOS 工具链 shebang、无界面 make 与 BOOT.bin 命令见 [`PL-PS接入与上板交付.md`](PL-PS接入与上板交付.md) §3。
- 各产物落到哪个分区，见 [`构建与烧录-分区图.svg`](构建与烧录-分区图.svg)。

### 5.2 `BOOT.bin` 的组成

```
BOOT.bin = fsbl.elf + system.bit(PL) + u-boot.bin     ← 只有这三样（create_dr1m_image.sh）
FAT(BOOT) 分区 = BOOT.bin + boot.scr + uImage.lz4 + system.dtb
ext4 分区      = rootfs.tar.gz 解包
```

- 内核 / dtb / rootfs 不在 `BOOT.bin` 里：由 U-Boot 按 `boot.scr` 从分区加载 kernel / dtb，再挂 ext4 rootfs。
- 本板 QSPI 只有 256 Mbit = 32 MB，而当前 `rootfs.tar.gz ≈ 63 MB`，放不下。

### 5.3 PL 码流进入 PL 的三种方式

1. 固化进 `BOOT.bin`（量产）：把 TD 的 `.bit` 覆盖 `$LAB/boards/files/system.bit` → `move_files.sh` → `create_image.sh`。上电由 FSBL 自动配置。
2. 运行时内核加载（迭代首选，不动 `BOOT.bin`、不重烧卡）：内核带 FPGA manager 与 `of-fpga-region`（`CONFIG_FPGA_MGR_DR1M90_FPGA=y`、`CONFIG_OF_FPGA_REGION=y`、`CONFIG_OF_CONFIGFS=y`、`CONFIG_CONFIGFS_FS=y`），节点 `anlogic,dr1m90-pcap-fpga` 与 `/soc/base_fpga_region`。厂商内核的 `fpga_manager` 只有 `name` / `state` / `status` 三个属性，没有 `firmware` 属性，用 device tree overlay 指定 `firmware-name`：

   ```bash
   # 主机：生成 overlay（码流须是 TD 的 ASCII .bit）
   nix shell nixpkgs#dtc -c bash platform/flash/load_bit.sh soc_hdmi.bit --out /tmp/lb
   # 板卡：把 /tmp/lb/soc_hdmi.bit.dtbo 送到板卡（串口 base64 或 scp）后
   mount -t configfs none /sys/kernel/config
   mkdir -p /sys/kernel/config/device-tree/overlays/pl
   cat /tmp/lb/soc_hdmi.bit.dtbo > /sys/kernel/config/device-tree/overlays/pl/dtbo
   cat /sys/class/fpga_manager/fpga0/state     # 期望 operating
   dmesg | tail -3                             # 期望 fpga prog successfully
   ```

   `firmware-name` 只写文件名，码流从板上 `/lib/firmware/` 读取；`stage_ps_local.sh` 已把 `soc_hdmi.bit` 与 `board_test.bit` 放到该目录。overlay 生效后 `firmware-name` 会残留在 `/soc/base_fpga_region`（内核日志 `memory leak will occur if overlay removed`）；换回其它码流或结束验证时重启，由 FSBL 重新加载 `BOOT.bin` 内的码流。无网络时用 `load_bit.sh ... --send <串口 FIFO>` 自动下发。
3. JTAG / U-Boot：FD `AnlogicTools → Program FPGA` 在线下载；SD 启动（SW1=`ON-OFF-OFF`）下也可在线配置 PL，见 §5.3.1；或 U-Boot `alfpga load <addr> <len>`（需 `CONFIG_CMD_ALFPGA=y`，工程 defconfig 默认未开）。

独立四路采集验证按以下顺序执行；本流程不写启动介质：

1. 在断电状态设 SW1=ON-ON-ON，接好温度 / 振动 / 电流与 J11 编码器，再上电。
2. 运行 [`build_td.tcl`](../fpga/uisrc/06_doc/build_td.tcl)，确认 100 MHz、STA 100% 与 SWNS / HWNS 非负；检查无效约束、缺失时钟与组合环。
3. 通过 UART&JTAG Type-C 下载生成的 [`fpga_top.bit`](../fpga/uisrc/out/td/fpga_top.bit)，确认芯片识别与下载完成。
4. 观察 LED1 闪烁、LED2 / LED3 常亮、LED4 熄灭；在 PL USB UART（实测 `/dev/ttyACM1`，115200 8N1）运行 `monitor_encoder.py`，核对静止、正负速度与停止归零。LED5 为 PS MIO51，不作为本次 PL 验收项。
5. 按 [`验证结果.md`](../fpga/uisrc/06_doc/验证结果.md) §5.4 保存样本序号、状态、原始计数、rpm、码流标识和时序结果。PL 串口读出不代表转速已进入 AXI / DMA 或 PS 应用。

#### 5.3.1 SD 启动下用 JTAG 在线更新 PL（SW1=ON-OFF-OFF）

本节把「核心板应用」分为 PL 码流（FPGA 逻辑）与 PS 应用（Linux rootfs / Qt）。JTAG 在线更新只适用于 **PL 码流**；PS 应用不通过 JTAG 更新。

`ON-OFF-OFF` 是 **SD 启动位**，不是 JTAG 位；JTAG 位是 `ON-ON-ON`。SD 启动后 JTAG 通道仍可用，可在线配置 PL；该操作不写 SD 卡，也不修改 rootfs、dtb 或 PS 应用。

适用条件：

- 只改 PL 逻辑；PS 内核、dtb、驱动、应用不变。
- 需要快速验证 `.bit`，不重烧 SD、不重打 `BOOT.bin`。
- PL 的寄存器偏移、AXI 基地址、中断号与当前 `PL-PS接口约定.md` 一致。

不适用：

- 更新 Qt / rootfs 应用：用 SD 重烧或网络 / 串口覆盖；JTAG 不写 ext4。PS 应用的正常更新路径是 `stage_ps_local.sh` → `build_rootfs.sh` → `burn_sd.sh`，或板上替换 `/opt/qt_play/mlk_ps_local` 后重启。
- 固化 QSPI 的 `BOOT.bin`：SW1=`ON-ON-ON`，FD `ProgramFlash`，见 §5.5。
- PL 改动涉及 AXI 地址 / 中断 / 时钟：先更新 dtb、驱动和 `BOOT.bin`，重启后再验证；不要在旧 dtb / 旧驱动上热换不兼容码流。

步骤：

1. 用 TD 生成 `.bit`，记录 SHA256；确认与当前 PS 侧寄存器 / 中断契约一致。合并 HDMI + 采集的码流为 `fpga/uisrc/out/soc_hdmi/soc_hdmi.bit`。
2. 连接 UART&JTAG 二合一 Type-C；主机识别 FTDI `0403:6042`；启动 hwserver（见 [`NixOS下TD与Questa使用.md`](NixOS下TD与Questa使用.md) §4.1）。
3. 保持 SW1=`ON-OFF-OFF`，从 SD 启动 Linux，确认串口可用；不要切换启动介质。
4. 用 TD Tcl 下载：
   ```tcl
   connect_cable -host_name 127.0.0.1:5555
   download -mode jtag -bit /abs/path/soc_hdmi.bit -cable 0
   ```
   或用 FD `AnlogicTools → Program FPGA` 选择同一个 `.bit` 后 Program。
   若日志出现 `PRG-8000 : Connect server failed`，先停止旧 hwserver，重新启动并确认 `ss -tlnp | grep 5555` 处于监听，再重试；不要在旧连接上重复 `download`。
5. 成功标志为 `PRG-2014 : Chip validation success: DR1M90`；板上执行 `plctl probe`，读回 `ID=0x504c0102`；`plctl sensors` 正常。
6. PL 重配置会复位 `pl_ctrl`、VDMA / HDMI 通路。若 `/dev/fb0` 无输出、HDMI 无画面或 `plctl` 异常，重启板卡后再验证；不要在旧显示 / 驱动状态下继续。
7. 该 `.bit` 只生效到断电；上电后 FSBL 仍从 `BOOT.bin` 加载旧码流。需要持久化时，把 `.bit` 放入 `BOOT.bin` / QSPI，或用内核 FPGA manager 在每次启动时加载。

验证状态：本仓库尚未在 SD 启动下执行 JTAG `Program FPGA`；TD/FD 能识别 DR1M90 是前提。若工具无法识别器件，改用 SW1=`ON-ON-ON` 后再下载，或检查 JTAG 线、FTDI 驱动与 hwserver。

### 5.4 烧录内容

- 交付：SD / eMMC 烧完整介质（PS 系统 + PL 码流）；QSPI 只固化 `BOOT.bin`，rootfs 仍在 SD/eMMC。
- 开发：可分开启用——PS 从 SD 启动；PL 用 5.3-2/3 单独在线加载，互不重烧。
- `UART&JTAG 二合一 Type-C` 只是把两路 UART 与 JTAG 合到一个物理口，不代表合并烧录：QSPI 固化用 FD `ProgramFlash`，PL 用 `Program FPGA`，走同一根 JTAG。
- SD 启动（`ON-OFF-OFF`）下 JTAG 只能在线改 PL，不改 SD/eMMC 的 rootfs 或 PS 应用；PS 应用仍按 §5.3-2 或 SD 重烧更新。

### 5.5 固化到板载 Flash

- QSPI：SW1=`ON-ON-ON`(JTAG) → FD `AnlogicTools → ProgramFlash` → 选 `BOOT.bin` → Program → 断电 → SW1 改 `ON-ON-OFF`(QSPI) 启动。
- QSPI 仅 32 MB，只放 `BOOT.bin`（FSBL + PL + U-Boot）；rootfs 不放 QSPI，仍需 SD/eMMC。`ON-OFF-OFF` 是 SD 启动位，不是 QSPI 固化位。
- eMMC：SD 启动后执行 `emmc_image/flash_emmc.sh`（板上 Linux 给 `/dev/mmcblk1` 分区、解 rootfs），再切 eMMC 启动；该路径不经过 JTAG。

完整 SD 构建 / 烧录命令见 [`SD卡启动与Linux构建指南.md`](SD卡启动与Linux构建指南.md) §5–§6。

---

## 六、NPU / 寿命模型

寿命模型已提供融合 CPU、FF6 六分支 CPU/NPU 的板端实现与回放验证。上位机封装、运行包及共存验收见 [接入规程](寿命模型-上位机接入.md)。厂商资料参考：

| 内容 | 路径 |
|---|---|
| NPU 演示 | `docs/MLK-F3P-DR1M90G-Resource/01_start/02_start_Linux/05_application_demo/05-5_NPU演示` |
| 模型简化 / 量化 / 推理脚本 | `platform/baremetal-sdk/sdk/app/npu`（`scripts/README.md`） |
| 综合应用示例（jpu + npu + camera + hdmi） | `platform/baremetal-sdk/sdk/app/face_detection` |

模型原件与证据保存在 `npu/`；当前 API 见 [NPU-AI 接口约定](NPU-AI接口约定.md)。合并镜像已完成启动、两轮 184 窗混合回放与 HDMI / 采集 DMA 基础共存验证，见 [实板结果](../fpga/uisrc/06_doc/验证结果.md) §5.6。长期稳定性与实采适配另行验收。

### 6.1 板端部署与烧录

硬核 NPU 是 PS 外设，寄存器窗口 `0x63F00000`；软核 `Video_NPU` 位于 PL，控制窗口 `0x1F0000000` / 64 KiB。FF6 的六个量化 FC 在硬核执行，厂商 ArmNN 初始化仍要求软核设备。合并工程通过 GP1 接软核控制口；HDMI 独占 HP0，软核读使用 HP1，软核与传感器 DMA 的写通路按完整突发仲裁，规则见 [PL-PS 接口约定](PL-PS接口约定.md) §6.1。

| 层 | 要求 |
|---|---|
| PL | `prepare_hdmi_soc.py` 包含厂商 `design_0_Video_NPU_0`、协议转换器和 HP1 写仲裁器；GP1/HP1/NPU 共用 100 MHz；软核 IRQ 接 `f2p_irq[15]` |
| 内核 | `CONFIG_NPU_MODULE`、`CONFIG_HARD_NPU_DRIVER`、`CONFIG_SOFT_NPU_DRIVER`、`CONFIG_CMA_MEM_DRIVER` 均 `=y` |
| 设备树 | 硬核、软核 `<0 97 4>` 中断、128 MiB `linux,cma`，以及独立的传感器 DMA 预留区；生成时校验 NPU 码流摘要 |
| rootfs | `/opt/mlk_rul/` 的封装引擎 / 权重 / FF6 量化表 / 回放输入 / 清单，`/opt/armnn_lib/lib/` 的配套 ArmNN，Qt 应用与驱动 |
| 验收 | 合并镜像启动、三个 NPU 设备、两轮 184 窗混合回放 / CMA 释放，以及 HDMI / 采集 DMA 基础共存已实板核对；长期稳定性待定 |

按顺序执行：

```bash
# 仓库根：生成并通过 TD 批处理构建合并码流
python3 fpga/soc/prepare_hdmi_soc.py
# TD 执行 fpga/uisrc/out/soc_hdmi/build_hdmi_soc.tcl
# 产物 soc_hdmi.bit 与 soc_hdmi.npu.sha256 必须成对保留
cd platform/flash
nix shell nixpkgs#lz4 nixpkgs#bc -c bash ./build_kernel_npu.sh
./build_hdmi_dtb.sh
./build_boot_bin.sh

# 运行包：仅首次构建或引擎变更后执行，输出目录须不存在
python3 ../../host/ps-local/inference/prepare_runtime.py --output rul-runtime
CXX="$PWD/vendor/tools/aarch64-linux/bin/aarch64-linux-gnu-g++" \
ARMNN_ROOT="$PWD/../baremetal-sdk/sdk/app/npu/libs/armnn_lib" sh rul-runtime/build.sh
python3 ../../host/ps-local/inference/prepare_runtime.py --seal --output rul-runtime

# 已封装的运行包与当前应用暂存后重建 rootfs
export RUL_BUNDLE="$PWD/rul-runtime"
export ARMNN_RUNTIME_ROOT="$PWD/../baremetal-sdk/sdk/app/npu/libs/armnn_lib"
./stage_ps_local.sh
nix shell nixpkgs#bc -c bash ./build_rootfs.sh

# 确认 SD 设备后烧录；该操作清空所选设备
lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS
nix shell nixpkgs#e2fsprogs nixpkgs#dosfstools nixpkgs#util-linux nixpkgs#rsync nixpkgs#gnutar \
  -c bash -c 'sudo env PATH="$PATH" bash ./burn_sd.sh /dev/sdX'
```

上电后确认 `/dev/hard_npu`、`/dev/soft_npu`、`/dev/cma_mem` 以及 `/opt/mlk_rul/libmlk_rul.so`。数据集回放不随开机自动启动，由本地面板的「开始/继续」按钮或远端回放指令发起；后端由用户选择，`CPU/NPU` 需要上述三个设备。选择混合后端失败时显示错误，不自动切换。诊断区显示实际后端与推理耗时。

独立核对时先停止 Qt，释放 NPU 设备，再执行：

```sh
/etc/init.d/S99qtapp stop
/opt/mlk_rul-validation/inspect_environment.sh hybrid
export LD_LIBRARY_PATH=/opt/armnn_lib/lib:/opt/qt_play/qt_lib/lib
/opt/mlk_rul-validation/check_engine /opt/mlk_rul-validation hybrid 2 > /tmp/ff6-hybrid.csv
/etc/init.d/S99qtapp start
```

确认两轮各 184 窗完成、释放前后 CMA 一致，并在 Qt 中继续核对 HDMI、采集与混合回放共存。软核设备树不得与不含该 IP 的码流组合，驱动 probe 会访问该窗口。RUL 单位为观测老化试验轮次，实采输入适配单独验收，见 [NPU-AI 接口约定](NPU-AI接口约定.md)。


---

## 七、本板（传感器板）落地

```
传感器板 ─P1/J12(PL IO)─► PL：采集 IP（SPI/I²C/计数/比较）
                              └─ AXI 寄存器 + FIFO/VDMA ─► PS(Linux 驱动)
PS：读寄存器/收 DMA → 处理 → UDP 发上位机 / HDMI 显示 / 存 SD
```

- PL 侧：采集 IP 的寄存器 / 中断按 `PL-PS接口约定.md` 实现，工程放 `fpga/`。
- PS 侧：读采样 / 收中断，开设备时用 ID / 版本校验。
- 接口契约：`PL-PS接口约定.md`；代码约束：`fpga/README.md`。

---

## 八、参考资料

| 主题 | 路径 |
|---|---|
| PL 工具（TD）快速入门 | `docs/MLK-F3P-DR1M90G-Resource/01_start/01_start_fpga/米联客-安路FPGA TD软件快速入门课程.pdf` |
| 开机测试 / 外设 demo | `…/01_board_tset/…开机测试.pdf`、`…/02_peripheral_demo/…演示demo教程.pdf` |
| Linux 部署 / 串口 / 出厂 / 恢复 / Buildroot | `docs/MLK-F3P-DR1M90G-Resource/01_start/02_start_Linux/**` |
| Linux 基础入门 / 驱动开发 | `docs/MLK-F3P-DR1M90G-Resource/03_demo/3-4_ex_soc_linux/ebook/3-4-01_…基础入门篇.pdf`、`3-4-02_…Linux驱动开发篇.pdf` |
| PS↔PL 寄存器 + 中断例程 | `docs/MLK-F3P-DR1M90G-Resource/03_demo/3-2_ex_fpsoc/3_2_01_sdk_base/13_sdk_pl_intr`、`14_axi_gpio_pl_intr`、`20_axi_lite_register` |
| FDMA / VDMA | `docs/MLK-F3P-DR1M90G-Resource/04_培训demo/01_fdma_vdma_20251010/*` |
| UDP 上位机通信 | `docs/MLK-F3P-DR1M90G-Resource/03_demo/3-0_book_all/3_1_02_…UDP以太网通信方案.pdf` |
| 硬件手册（外设 / 管脚 / 模式开关） | `docs/MLK-F3P-DR1M90G-Resource/02_hardware/hardware_MLK_F3P_CZ02_DR1_20250526/01_硬件手册/…硬件使用手册.pdf` |
| 传感器板 | `docs/传感器板-说明.md` |
