# platform/flash — 构建与烧录

MLK-F3P-DR1M90G 的 Linux/Buildroot 构建与 SD 烧录工作区。

| 文件 | 作用 |
|---|---|
| `flake.nix` | 构建环境（宿主工具） |
| `apply_sdk_local.sh` | 给 vendor 打本地补丁 |
| `build_rootfs.sh` | 编译 rootfs |
| `build_kernel_npu.sh` | 内置硬 NPU、软 NPU、CMA 驱动并重建内核 |
| `stage_rul.sh` | 校验并暂存 FF6 封装模型及厂商 ArmNN 运行库 |
| `verify_npu_rootfs.py` | 打包前检查模型哈希、码流摘要和 AArch64 动态库依赖 |
| `burn_sd.sh` | 烧录 SD |
| `rootfs-overlay/` | 叠加进 rootfs 的本地内容 |
| `vendor/` | 解压的厂商构建工程，未入库 |

## 准备

```bash
cd platform/flash
mkdir -p vendor
tar xzf vendor/uisrc-lab-anlogicM-V4.0.1.tar.gz -C vendor --strip-components=1
./apply_sdk_local.sh
```

## 构建

首次从厂商工程准备 base rootfs / U-Boot / kernel 时，按上面的准备步骤解压并 `apply_sdk_local.sh`；已有 `vendor/.../output/rootfs.tar.gz`、`boot.scr`、`uImage.lz4` 时可跳过：

```bash
# 首次 base（仅一次）
cd vendor/scripts && source drcfg.sh
move_files.sh && make_uboot.sh && make_kernel.sh && make_rootfs.sh
printf '3\n' | create_image.sh        # 3 = parameter-tf-fat-ext4.txt
```

随后按改动范围选择：

```bash
# PS 应用 / 驱动 / rootfs 变更
./stage_ps_local.sh
./build_rootfs.sh

# PL 码流或 DTB 变更
./build_hdmi_dtb.sh
./build_boot_bin.sh
```

- `stage_ps_local.sh` 交叉编译 `mlk_ps_local`、`plctl` 和匹配内核的 `pl_ctrl.ko`，写入 `rootfs-overlay/`；需要 `rootfs-overlay/opt/qt_play/qt_lib` 中的 Qt 运行库。
- `build_rootfs.sh` 在厂商 base rootfs 上叠加 overlay，生成 `vendor/boards/DR1M/buildroot/images/rootfs/rootfs.tar.gz`。
- `build_hdmi_dtb.sh` 生成带 `pl-ctrl@80000000` polling 节点的 `images/boot/system.dtb`。
- `build_boot_bin.sh` 用 `soc_hdmi.bit` 与 HDMI FSBL 组装 `images/boot/BOOT.bin`，并复制 `boot.scr`、`uImage.lz4`；若厂商输出中缺少后两者，先在 `vendor/scripts` 执行 `move_files.sh && make_uboot.sh && make_kernel.sh`。
- 只改 PS 应用时不需要重跑 `build_boot_bin.sh`；只改 PL 时不需要重跑 `stage_ps_local.sh`。
- `build_rootfs.sh` 需要 `bc`；若 Buildroot 报 `You must install 'bc'`，用 `nix shell nixpkgs#bc -c bash ./build_rootfs.sh`。

## 合并 NPU 镜像

合并 `soc_hdmi` 工程含厂商 `Video_NPU`，通过 GP1 提供软核窗口，HP1 写通路与采集 DMA 仲裁；HDMI 保留 HP0。FF6 六个量化 FC 使用硬核，但厂商 ArmNN 初始化仍要求 `/dev/soft_npu`。通用软算子保留，FF6 不使用的 YOLO 后处理与图像 Resize 关闭。

完整顺序为 PL 码流及摘要、NPU 内核、设备树、BOOT、应用与封装运行包暂存、rootfs 打包；命令见 [NPU 构建说明](README.md#npu-matching-pl-kernel-and-device-tree) 与 [PS-PL 开发流程](../../docs/PS-PL开发流程.md) §6.1。暂存模型须设置 `RUL_BUNDLE` 与 `ARMNN_RUNTIME_ROOT`。启用软核的设备树必须与含该 IP 的码流配套，打包脚本检查 `soc_hdmi.npu.sha256`。三个 NPU 设备齐备后自动回放选择混合后端，合并镜像启动、两轮 184 窗混合回放 / CMA 释放及 HDMI / 采集 DMA 基础共存已实板核对，见 [验证结果](../../fpga/uisrc/06_doc/验证结果.md) §5.6；长期稳定性待定。

## 烧录

```bash
cd platform/flash
lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS   # 确认 SD 设备，例如 /dev/sdb
sudo env PATH="$PATH" ./burn_sd.sh /dev/sdb
```

`burn_sd.sh` 会清空目标盘，必须显式传入已识别的 SD 设备，不得将系统盘作为目标。脚本先清分区表，按固定偏移格式化 FAT32 BOOT + ext4 rootfs，写入 `images/boot/` 与 `images/rootfs/rootfs.tar.gz`，最后写 MBR，避免桌面自动挂载干扰。完成后 `sync` 再拔卡。

脚本需要 `sfdisk` / `losetup` / `wipefs` / `partx`（util-linux）、`mkfs.vfat` / `fsck.vfat`（dosfstools）、`mkfs.ext4` / `e2fsck`（e2fsprogs）、`rsync` 与 `tar`。在 `nix develop` 中执行可补齐宿主工具。

NixOS 默认 PATH 不含 `mkfs.ext4` / `e2fsck`：

```bash
nix shell nixpkgs#e2fsprogs nixpkgs#dosfstools nixpkgs#util-linux nixpkgs#rsync nixpkgs#gnutar \
  -c bash -c 'sudo env PATH="$PATH" bash ./burn_sd.sh /dev/sdb'
```

烧完 `sync`，断电，SW1 拨到 SD（`ON-OFF-OFF`），插卡上电，串口 `/dev/ttyACM0` @115200 登录 `root`/`root`。

### 烧录对象

| 目标 | 介质 / 工具 | 内容 |
|---|---|---|
| SD 卡 | `burn_sd.sh` | `BOOT.bin`、`boot.scr`、`uImage.lz4`、`system.dtb` + rootfs |
| QSPI | FD `ProgramFlash`，SW1=`ON-ON-ON` | `BOOT.bin`（FSBL + PL + U-Boot）；QSPI 仅 32 MB，不含 rootfs |
| eMMC | SD 启动后 `emmc_image/flash_emmc.sh` | 出厂 eMMC 的 boot / rootfs |
| PL 在线 | JTAG `Program FPGA`，SD 启动 `ON-OFF-OFF` | `.bit`（掉电丢失，不改 rootfs） |

JTAG 在线更新只改 PL，不改 SD/eMMC 的 rootfs 或 Qt 应用；PS 应用仍通过 SD 重烧或网络/串口更新。前提是 TD/FD 能识别 DR1M90；本仓库尚未在 SD 启动下成功执行 `Program FPGA`。完整步骤见 [`../../docs/PS-PL开发流程.md`](../../docs/PS-PL开发流程.md) §5.3.1。

### NPU 状态面板验证

ps-local 和 remote-pc 的系统占用侧栏采用 CPU / 内存相同的状态、进度条及历史图表布局，显示板端 NPU 驱动就绪状态和中断活动。顶栏网络状态位于应用连接状态上方，remote-pc 使用板端上报值。此应用变更使用已有合并 `soc_hdmi` 的 PL、内核和设备树，重新暂存应用并打包 rootfs 即可。从仓库根执行：

```bash
nix develop ./host --command bash platform/flash/stage_ps_local.sh
nix develop ./platform/flash --command bash platform/flash/build_rootfs.sh
```

rootfs 产物为 `vendor/boards/DR1M/buildroot/images/rootfs/rootfs.tar.gz`。按本文「烧录」将 `images/boot/` 和该 rootfs 写入 SD 卡；整卡烧录会清除卡内配置、配对信息和采集数据，需提前备份。已有匹配合并镜像时，可使用[应用更新包](../../host/ps-local/README.md#应用更新)保留现有配置与数据。

SD 启动后按以下顺序验收：

1. 在串口核对应用、显示、采集与 NPU 设备：

   ```sh
   /etc/init.d/S99qtapp status
   ls -l /dev/fb0 /dev/pl_ctrl /dev/hard_npu /dev/soft_npu /dev/cma_mem
   grep -E 'HARD_NPU_IRQ|SOFT_NPU_IRQ' /proc/interrupts
   ```

2. HDMI 看板左上点击「展开」，核对 NPU 项显示「驱动就绪」。在寿命回放面板核对模型目录 `/opt/mlk_rul` 和实际后端 `CPU/NPU`；手动切换后端时先「停止」，再选择 `CPU/NPU（FF6 六分支）` 并「开始/继续」。窗口编号和有效 RUL 应持续更新，现场采集保持独立运行。
3. 混合回放期间间隔采样中断计数：

   ```sh
   grep -E 'HARD_NPU_IRQ|SOFT_NPU_IRQ' /proc/interrupts
   sleep 5
   grep -E 'HARD_NPU_IRQ|SOFT_NPU_IRQ' /proc/interrupts
   ```

4. 同时运行更新后的 remote-pc，核对两端顶栏上行均显示核心板网络，下行为各自的应用连接状态。两端「展开」中的 NPU 均须出现使用率条与硬核 / 软核历史曲线。核对硬核累计次数增长，侧栏出现「检测到中断活动」及硬核次 / 秒。FF6 使用硬核，软核计数为 0 属于正常情况。暂停回放，待当前窗口完成后比较中断增量；若无其他 NPU 任务，面板应显示「本周期无中断」。中断活动不代表推理成功，须同时检查实际后端和有效预测。

PC 端从仓库根执行 `host/remote-pc/run_host_app.sh`。断开应用链路 5 秒后，PC 的核心板网络与 NPU 状态应显示不可用，NPU 曲线清除；重新连接后恢复板端状态。

厂商驱动未提供百分比使用率，面板应显示 `使用率：驱动未提供`。设备缺失或统计不可读的提示及实现口径见 [NPU 状态显示](../../host/ps-local/README.md#npu-状态显示)。

## 本地改动

`apply_sdk_local.sh` 会把这几处改动打进 vendor，可重复执行：

- 依赖检查 `/usr/bin/file` 改成 `file`，NixOS 需要
- buildroot defconfig 打开 vim
- 脚本 shebang 换成系统 bash

## rootfs overlay

`rootfs-overlay/` 存放 `build_rootfs.sh` 在打包前叠加进 rootfs 的本地内容：

- `etc/init.d/S99qtapp` — `/opt/qt_play/run_arm_app.sh` 存在时开机自动运行 PS 端本地 Qt 上位机。
- `opt/qt_play/` — 放 PS 端 Qt 产物（可执行文件 + `qt_lib` + `run_arm_app.sh`）；大文件不入库，只提交 `.gitkeep`。

加入文件后重跑 `./build_rootfs.sh`。overlay 最后叠加，会覆盖基础 rootfs 中同路径文件。

PS 端本地交互走内核内置驱动：2.5G USB 无线鼠标接收器经 `usbhid` + evdev（USB HOST 跳线仍提供 WiFi + 4G），板载 KEY1/KEY2 作后备。见 [`../../host/ps-local/README.md`](../../host/ps-local/README.md)。

## 合体 BOOT.bin

PL 的 .bit 导出后，覆盖 `vendor/boards/files/system.bit`；地址或中断有变化就同步 `docs/PL-PS接口约定.md`，然后重跑 `create_image.sh`。调试期也可以用 fpga_manager 在线加载 .bit，不必重烧卡。

## 本地 HDMI 与 FF6 寿命回放

使用厂商 NPU 演示配套启动环境的独立验证卡，见 [RUL-HDMI 启动与验收](RUL-HDMI启动与验收.md)。`prepare_rul_rootfs.py` 从冻结 initramfs 与本地 overlay 生成完整 rootfs 归档，保留设备节点和硬链接；`burn_sd.sh` 通过 `IMAGES_ROOT` 接受镜像目录，必须显式传入已识别的目标设备。
