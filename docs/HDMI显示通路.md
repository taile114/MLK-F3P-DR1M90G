# HDMI 显示通路（MLK-F3P-DR1M90G）

> HDMI 本地显示的实现：PL 设计、设备树、内核驱动与启动链，以及本地 Qt 应用（[`../host/ps-local/README.md`](../host/ps-local/README.md)）如何经 `/dev/fb0` 输出。
> 相关：系统构建与烧录见 [`SD卡启动与Linux构建指南.md`](SD卡启动与Linux构建指南.md)；PL ⇄ PS 寄存器契约见 [`PL-PS接口约定.md`](PL-PS接口约定.md)；开发步骤见 [`PS-PL开发流程.md`](PS-PL开发流程.md)。

## 1. 通路

```
ps-local (Qt linuxfb) ─ /dev/fb0 ─ DDR 帧缓冲
        ▲                              │
        │ anlogic vdmafb 驱动            ▼
   VTG(0x8010_0000) · VDMA(0x8020_0000) ─► hdmi_tx ─► 显示器

sensor_capture ─ sensor_soc_axi ─ AXI DS0(0x8000_0000) ─► /dev/pl_ctrl ─► Qt
```

- Qt 以 `linuxfb` 平台插件把界面画到 `/dev/fb0`。
- 内核 `anlogic,vdmafb` 驱动按设备树时序配置 PL 的 VTG 与 VDMA；VDMA 扫描 DDR 帧缓冲，经 `hdmi_tx` 输出。
- 像素时钟由 PL 内 PLL 提供，`anlogic,dynclk` 驱动配置（`axi_dynclk@8010_1000`）。
- 温度 / 振动采集经 `sensor_capture`、`sensor_soc_axi` 挂到空闲 DS0；Qt 通过 `/dev/pl_ctrl` 轮询寄存器。

## 2. PL 设计

显示逻辑与传感器接入合并在 `fpga/uisrc/out/soc_hdmi/`，由 `fpga/soc/prepare_hdmi_soc.py` 从厂商 QT demo 的 PL 工程复制 HDMI 部分并挂接本地采集 IP：

| 项 | 内容 |
|---|---|
| HDMI | `VDMA`、`video_timing_generator`、`hdmi_tx`、`PLL_0/PLL_1` 保持厂商配置，1920×1080@60 |
| 传感器接入 | `soc_ctrl_port` → `sensor_soc_axi` → `sensor_capture`，挂 DS0 `0x8000_0000` |
| 地址 | DS0 `0x8000_0000`（传感器）、DS1 `0x8010_0000`（VTG）、DS2 `0x8020_0000`（VDMA） |
| IRQ | `f2p_irq[0]/[1]` 保留给 VDMA mm2s/s2mm；本码流不接传感器 IRQ，`pl_ctrl` 走轮询 |

构建与约束：

```bash
python3 fpga/soc/prepare_hdmi_soc.py
td-fhs -c "cd /home/lfour/Projects/MLK-F3P-DR1M90G/fpga/uisrc/out/soc_hdmi && /home/lfour/FHS/TD_Release_2026.2_NL/bin/td.sh build_hdmi_soc.tcl"
```

`prepare_hdmi_soc.py` 生成 `timing_hdmi.sdc`：先用 PS `p2f_clk0`（50 MHz）建钟，再 `derive_clocks` 从 PLL/VTG IP 参数推导 clk0=100 MHz、clk1=120 MHz、clk2=150 MHz、VTG 148.5 MHz、PLL1 742.5 MHz，最后按异步域分组。厂商 IP `.sdc` 只有 `set_ip_property` 且路径属于 GUI 工程层级，扁平 `read_hdl` 下读取会产生无效约束，故不读取。

本机构建结果：

| 项 | 结果 |
|---|---|
| 码流 | `fpga/uisrc/out/soc_hdmi/soc_hdmi.bit`，4,052,208 B，SHA256 `cade7ea60805ad66d1f1e7947e480f497ecca5e0820e4f88372549ce6373a09e` |
| 时序违例 | SWNS 0.004 ns、HWNS 0.020 ns；0 违例 |
| 时钟约束 | `check_timing` no_clock 0、invalid_constraint 0、unexpandable 0 |
| STA coverage | 99.91%，未达到生产 100% 门槛 |
| PLL 检查 | 仅 3 处频率舍入：148.456 vs 148.500、742.400 vs 742.500、148.500 vs 148.485 MHz |
| I/O 约束 | 5 个输入 / 13 个输出无 input/output delay（MEDIUM）；HDMI TMDS 与传感器板级时序仍为 TBD |

该码流已在实板启动，HDMI 1080P、Qt linuxfb、`/dev/fb0`、`/dev/pl_ctrl` 与温度/振动读数已验证；功能可用。生产交付仍要求 STA coverage 100%、完整 I/O delay 与 HDMI TMDS/传感器板级时序签核，因此当前仍是生产候选而非生产 PASS。

`prepare_hdmi_soc.py` 每次重建都会重写顶层 wrapper，并校验 `system.v` 中的 `soc_ctrl_port` 与 DS0 连接；`build_hdmi_soc.tcl` 在 `read_hdl` 前再次检查 `O_led` 与 `soc_ctrl_port`。若用厂商 DI 重新生成 RTL，必须重新运行 `prepare_hdmi_soc.py`，否则 wrapper 可能变成拉低 `O_led`、不接 System 的 stub，表现为全部 LED 常亮且无 HDMI / 传感器数据。

## 3. 设备树

基线为厂商 QT demo 的 `.../05-1_QT环境搭建及demo演示/demo/boot/system.dtb`，其中已有 `vdma@80200000`、`vdmafb@80100000`、`axi_dynclk@80101000`。`platform/flash/build_hdmi_dtb.sh` 反编译该 dtb，在 `/soc` 下插入：

```dts
pl-ctrl@80000000 {
    compatible = "mlk,pl-ctrl-1.0";
    reg = <0x0 0x80000000 0x0 0x10000>;
    status = "okay";
};
```

再编译为 `platform/flash/vendor/boards/DR1M/buildroot/images/boot/system.dtb`，并用 dtc 反编译校验 `pl-ctrl`、`vdma`、`vdmafb` 节点均存在。节点不填 `interrupts`；GIC ID 仍为 REF。

## 4. 内核配置与驱动

显示驱动源码取自厂商 `.../demo/driver/`：`anlogic-vdmafb.c`、`anlogic-dynclk.c`、`anlogic-vtc.h`，位于 `platform/flash/vendor/sources/linux/drivers/video/fbdev/`。VDMA 控制器与通道由 `platform/flash/vendor/sources/linux/drivers/dma/anlogic/`（`anlogic_dma.c`、`axi-vdma.c`）提供。

`boards/DR1M/configs/defconfigs/anlogic_dr1_kernel_defconfig` 启用：

| 配置 | 项 |
|---|---|
| `CONFIG_ANLOGIC_DMA` | AXI DMA / VDMA 控制器 |
| `CONFIG_AXI_VDMA` | VDMA 通道后端（`anlogic_vdma_channel_set_config`） |
| `CONFIG_FB_ANLOGIC_VDMAFB` | `anlogic,vdmafb` 帧缓冲 |
| `CONFIG_ANLOGIC_DYNCLK` | `anlogic,dynclk` 像素时钟 |
| `CONFIG_VIDEOMODE_HELPERS` | `of_get_videomode` |

驱动就绪时输出 `/dev/fb0`（`anlogic` 帧缓冲）。`pl_ctrl` 字符设备驱动见 `host/ps-local/driver/pl_ctrl.c`，与显示驱动独立；本轮不启用传感器 IRQ。

## 5. 启动链

`BOOT.bin` = FSBL + `soc_hdmi.bit` + U-Boot，由 `platform/flash/build_boot_bin.sh` 调用 `albg` 生成；内核 `uImage.lz4`、`system.dtb`、`boot.scr` 放在 FAT 启动分区，`boot.scr` 使用 ext4 根文件系统参数（`root=/dev/mmcblk0p2 rootfstype=ext4`）。PL 码流只在 BOOT.bin 中。`0x8000_0000` 现在是传感器 AXI，启动参数文件不得向该地址写显示寄存器。

VDMA 从 DDR 取帧缓冲数据必须经 PS HP0。FSBL 的 HPF 若 `Slave_AXI_HP0_Interface=0`，VDMA 不会前进，AXI4S Video Out 无法 lock，HDMI TX 保持复位，屏幕无输出；此时传感器（`M_AXI_GP0`）仍可读。HDMI 镜像使用厂商 HDMI 工程的 FSBL（HP0/1=1），`build_boot_bin.sh` 默认选择它。

## 6. Qt 显示与字体

- 平台插件 `linuxfb`，`QT_QPA_PLATFORM="linuxfb:fb=/dev/fb0"`；鼠标与键盘由 `QT_QPA_GENERIC_PLUGINS="evdevmouse,evdevkeyboard"` 加载 Qt 的 evdev 输入插件。运行脚本与环境变量见 [`../host/ps-local/README.md`](../host/ps-local/README.md)。
- CJK 字体：`Maple Mono NF CN`（Regular / Medium / Bold）放入 Qt 字体目录 `qt_lib/lib/fonts/`（对应 `QT_QPA_FONTDIR`），避免中文显示为方框。
- 数据显示只来自 `/dev/pl_ctrl`；设备缺失、读取失败或通道停更时显示不可用，不使用演示数据回退。

## 7. 验证

```bash
dmesg | grep -iE 'vdmafb|dynclk|pl_ctrl'
ls -l /dev/fb0 /dev/pl_ctrl
plctl probe
plctl sensors
cat /proc/device-tree/model
```

预期：`/dev/fb0` 存在并有 Qt 界面；`/dev/pl_ctrl` 存在；`plctl probe` 读回 `ID=0x504c0102`；`plctl sensors` 的温度与三轴随传感器变化；`cat /proc/interrupts | grep anlogic` 的 `anlogic-dma-controller` 计数持续增长。`plctl irq` 在本轮返回 ENODEV，属于无 IRQ 的预期行为。若 HDMI 无输出且 VDMA 中断计数为 0，先核对 FSBL 的 HP0 配置。

## 8. 未定（TBD）

- 分辨率切换（`display-timings` 其它时序）的实测；双缓冲 / 多缓冲；
- 与 PL 采集共用时 VDMA 带宽与 DMA 通道分配；
- 生产 STA coverage 100%、I/O delay、HDMI TMDS/传感器板级时序与 GIC ID 回填。
