# SD 卡启动与 Linux 构建指南（MLK-F3P-DR1M90G）

> 内容：用 SD 卡启动自编的 Linux 系统；不触碰 eMMC 出厂系统。本文只覆盖 PS 侧构建与烧录，PL 码流、FSBL 与 SoC 交付见 [`PL-PS接入与上板交付.md`](PL-PS接入与上板交付.md)。NPU 的配套码流、内核、设备树与运行包构建见 [`PS-PL开发流程.md`](PS-PL开发流程.md) §6.1。
> 平台：米联客 MLK-F3P-DR1M90G。
> 依据：`01_start/02_start_Linux/04_secondary_developmemt/04-1_…Buildroot篇.pdf`，以及构建工程 `platform/flash/vendor`（解压自 `uisrc-lab-anlogicM-V4.0.1.tar.gz`）。

---

## 0. 介质选择：无需刷 eMMC

SD 卡与 eMMC 是**两套独立介质**，只共用「启动模式选择」，由底板 **SW1** 决定从哪引导。所以：

- 把系统放到 SD 卡 → **SW1 拨到 SD 模式** → 插卡上电 → 从 SD 启动；
- **eMMC 出厂系统完全不动**，作为随时可回退的备份；
- 不需要「先刷 eMMC 的 boot」。

### SW1 模式对照（手册：`0 = ON`，`1 = OFF`）

| 模式 | PIN1 PIN2 PIN3 | 说明 |
|---|---|---|
| JTAG | ON-ON-ON (000) | TD 下载/调试 |
| QSPI | ON-ON-OFF (001) | QSPI 启动 |
| **SD** | **ON-OFF-OFF (011)** | **本节用** |
| **eMMC** | **OFF-OFF-OFF (111)** | 出厂系统所在 |

> 出厂默认从 eMMC（`OFF-OFF-OFF`）启动。切到 SD 启动时，**把 PIN1 拨到 ON**（`ON-OFF-OFF`）。若板上丝印与手册不一致，以核心板手册和丝印为准；JTAG 下载前先用 `ON-ON-ON` 确认 TD/FD 能识别 DR1M90。

### 选用构建工程而非出厂镜像的原因

| | `rst_to_factory_img/sdcard_image`（出厂镜像） | `uisrc-lab-anlogic`（构建工程，本文用） |
|---|---|---|
| 内容 | 只有预编译二进制 + `flash_sdcard.sh` | 内核 / U-Boot / Buildroot **源码** + 脚本 + 工具链 |
| 用途 | **恢复出厂** | **二次开发**（改了能重编） |
| 能否 patch | 不能：无源码 | 能：改 defconfig / dts / rootfs 后重编 |
| rootfs | 单 FAT32 分区 + ramdisk（`uInitrd.lz4`） | 可选 `parameter-tf-fat-ext4.txt`：FAT boot + **ext4 持久 rootfs** |

出厂镜像的 `flash_sdcard.sh` 只是把固定的 `BOOT/` 拷进 SD 的一个 FAT32 分区，**不提供源码与编译**；要编译、要定制就必须用构建工程。

---

## 1. 构建环境准备

构建工程根 `$LAB` 已带一个 `flake.nix`（**单 input `nixos-unstable`**，无 flake-utils 等第三方）。NixOS 下直接：

```bash
cd "$LAB"
nix develop            # 进入 devShell：自带 make/gcc/binutils/flex/bison/bc/
                       # ubootTools(mkimage)/dtc/ncurses/openssl/lz4/xz/zstd/
                       # rsync/python3/perl/parted/e2fsprogs/dosfstools/mtools/autotools …
```

进去后 `cd scripts && source drcfg.sh` 即可开始构建。flake 另提供 `formatter = nixfmt-rfc-style`；以后要把它变成真正构建内核/Buildroot 的 Nix 包，在 `outputs.packages` 里扩展。

非 NixOS / Ubuntu 路线（脚本按 **Ubuntu 18.04 ~ 23.04** 编写）：

```bash
sudo apt-get update
sudo apt-get install -y build-essential make gcc g++ flex bison bc \
  u-boot-tools liblz4-tool libncurses5-dev libssl-dev \
  rsync parted dosfstools mtools device-tree-compiler lz4 kmod \
  cpio unzip wget git python3 perl file sudo
```

- 交叉工具链：构建工程自带 `tools/aarch64-linux/`（**预编译外部工具链**），无需另装。
  - NixOS 上若运行它报缺 `/lib64/ld-linux-*`，说明宿主没提供 FHS 兼容层：启用 `programs.nix-ld`、或用 `steam-run`。
- `packages/` 是 Buildroot 的下载缓存，**首次构建需联网**。

目录约定（下文以 `$LAB` 表示构建工程根）：

```
$LAB = platform/flash/vendor        # 构建脚本在 platform/flash/（见其 README）
```

---

## 2. 准备 boot 输入文件

`create_image.sh` 生成 `BOOT.bin` 需要 `fsbl.elf` + `system.bit` + `u-boot.bin`。这些 **demo 里已有预编译件**，直接拷进 `boards/files` 即可：

```bash
DEMO=docs/MLK-F3P-DR1M90G-Resource/01_start/02_start_Linux/04_secondary_developmemt/demo

cp "$DEMO/soc_sdk/soc_fsbl/build/soc_fsbl.elf"        "$LAB/boards/files/"
cp "$DEMO/soc_sdk/soc_base/platform/system.bit"       "$LAB/boards/files/"
cp "$DEMO/soc_dts/kernel-dts/anlogic-dr1.dts"         "$LAB/boards/files/kernel-dts/"
cp "$DEMO/soc_dts/uboot-dts/anlogic-dr1.dts"          "$LAB/boards/files/uboot-dts/"
```

> `move_files.sh` 要求 `boards/files/` 顶层**恰好一个** `.elf` 和一个 `.bit`，并会自动改名为 `soc_fsbl.elf` / `system.bit`。
> 只改 PS 时，这两个文件保持 demo 版本即可。
>
> **`system.bit` 就是 PL 码流**：上面拷进去的是 **demo 的 PL 逻辑**。要运行自研 PL 的采集逻辑，就把 TD 导出的 `.bit` 覆盖到 `boards/files/system.bit` 再重新执行 `create_image.sh`；只改 PS、不改 PL 时保持 demo 版即可。
> 迭代 PL 时若不想每次重打 `BOOT.bin`/重写卡，可改用**内核在线加载**（`echo xxx.bit > /sys/class/fpga_manager/fpga0/firmware`），见 `PS-PL开发流程.md` §5.3。
> **`BOOT.bin` 只含 FSBL + PL bitstream + U-Boot**；内核/dtb/rootfs 是启动分区上的独立文件（见 §11），不放进 boot 镜像。
> `fpga/uisrc/out/soc/system.bit` 是仅采集的 SoC 候选 RTL 产物，不是当前 HDMI 镜像；当前 HDMI + 采集合并镜像使用 `fpga/uisrc/out/soc_hdmi/soc_hdmi.bit`，已实板启动并验证 HDMI/Qt/`/dev/pl_ctrl`。生产 HPF/FSBL 与 100% STA 仍未收口，不得把候选 `soc/system.bit` 当作已验收的 `boards/files/system.bit`。
> 显示构建：PL 码流与 `BOOT.bin` 取自厂商 QT demo（`…/05_application_demo/05-1_QT环境搭建及demo演示/demo/`），见 [`HDMI显示通路.md`](HDMI显示通路.md)。

---

## 3. 配置构建环境

```bash
cd "$LAB/scripts"
source drcfg.sh        # 必须 source；设置 ARCH=arm64 / CROSS_COMPILE / 输出目录等
```

`drcfg.sh` 关键变量：`MLK_BOARD_FAMILY=DR1M`、`ARCH=arm64`、`MLK_OUTPUT_DIR=$LAB/boards/DR1M/buildroot/output`、`MLK_IMAGES_DIR=$LAB/boards/DR1M/buildroot/images`。

---

## 4. 打补丁（PS 侧三类改法）

### 4.1 内核配置（config）

- 交互式：`make_kernel_menuconfig.sh`（等价 `make -C $LAB/sources/linux menuconfig`，`ARCH/CROSS_COMPILE` 已由环境提供）。
- 存回 defconfig：改完后 `save_kernel_defconfig.sh`，会把配置写回
  `boards/DR1M/configs/defconfigs/anlogic_dr1_kernel_defconfig`。
- 或直接编辑该 defconfig 文件（下次 `cfg_kernel.sh` 会拷进内核源码）。

### 4.2 设备树（dts）

- **内核 dts**：`$LAB/boards/files/kernel-dts/anlogic-dr1.dts`（→ 被 `move_files.sh` 拷到 `sources/linux/arch/arm64/boot/dts/anlogic/`）。
- **u-boot dts**：`$LAB/boards/files/uboot-dts/anlogic-dr1.dts`。
- 改完直接执行 `move_files.sh` 覆盖源码里的 dts。

> 例：要改某个外设使能/属性，就在这两个 dts 里对应节点改（kernel 用 kernel-dts，u-boot 用 uboot-dts，两边可能都要）。

### 4.3 根文件系统（rootfs / 应用）

- PS 本地 Qt 应用、`plctl` 与 `pl_ctrl.ko`：在仓库根执行 `platform/flash/stage_ps_local.sh`，再执行 `platform/flash/build_rootfs.sh` 打包。叠加内容与设备树启用前提见 [`../platform/flash/README.md`](../platform/flash/README.md)。
- Buildroot 配置：`make_rootfs_menuconfig.sh`（等价 `make -C $LAB/sources/buildroot menuconfig`）。
- 存回 defconfig：`save_rootfs_defconfig.sh` → `boards/DR1M/configs/defconfigs/anlogic_dr1_buildroot_defconfig`。
- **加文件/应用**：在 `$LAB/sources/buildroot/board/<board>/rootfs_overlay/` 下放内容，并在 buildroot 里选中对应 overlay 路径（NPU demo 即用此法，见 `05-5` 文档）。
- 交叉编译好的可执行文件，也可先 `scp`/SD 拷到板子 `/root` 试运行，稳定后再并入 rootfs。

---

## 5. 构建

```bash
# 已在 $LAB/scripts 且已 source drcfg.sh
move_files.sh        # 拷贝 fsbl/bit/dts 到 output 与源码树
make_uboot.sh        # 产出 u-boot.bin
make_kernel.sh       # 产出 uImage.lz4 + system.dtb
make_rootfs.sh       # 产出 rootfs.tar.gz (+ uInitrd.lz4)
create_image.sh      # 打包 BOOT.bin / boot.scr / uImage.lz4 / system.dtb (+ rootfs)
```

`create_image.sh` 会弹出启动参数选择：

- 选择 **`parameter-tf-fat-ext4.txt`** → SD 卡「FAT BOOT 分区 + ext4 持久 rootfs」（**开发推荐**）；
- 选择 `parameter-tf-fat.txt` → 纯 ramdisk（rootfs 打进 uInitrd.lz4）。

产物：`$LAB/boards/DR1M/buildroot/images/{boot,rootfs}/`

```
images/boot/     BOOT.bin  boot.scr  uImage.lz4  system.dtb  [uInitrd.lz4]
images/rootfs/   rootfs.tar.gz            # 选 ext4 参数时
```

### 构建顺序
```bash
move_files.sh && make_uboot.sh && make_kernel.sh && make_rootfs.sh && create_image.sh
```

---

## 6. 烧录到 SD 卡

本项目当前流程使用 `platform/flash/burn_sd.sh`：按固定偏移写 FAT32 BOOT + ext4 rootfs，最后写 MBR。厂商 `make_parted.sh` / `deploy_image.sh` 仍可用，但不要与 `burn_sd.sh` 混用；`rst_to_factory_img/sdcard_image/flash_sdcard.sh` 写的是预编译出厂镜像，不是本工程产物。

### 6.1 生成待写文件

首次使用先在 [`platform/flash/README_CN.md`](../platform/flash/README_CN.md) 准备厂商 base rootfs / U-Boot / kernel；已有 base 时直接执行：

```bash
cd platform/flash

# 先按 PS-PL 开发流程 §6.1 准备合并 NPU 码流及已封装的 rul-runtime
nix shell nixpkgs#lz4 nixpkgs#bc -c bash ./build_kernel_npu.sh
./build_hdmi_dtb.sh
./build_boot_bin.sh
export RUL_BUNDLE="$PWD/rul-runtime"
export ARMNN_RUNTIME_ROOT="$PWD/../baremetal-sdk/sdk/app/npu/libs/armnn_lib"
./stage_ps_local.sh
nix shell nixpkgs#bc -c bash ./build_rootfs.sh
```

- `stage_ps_local.sh` 交叉编译 `mlk_ps_local`、`plctl` 和匹配内核的 `pl_ctrl.ko`，写入 `rootfs-overlay/`；需要 `rootfs-overlay/opt/qt_play/qt_lib` 中的 Qt 运行库。
- `build_rootfs.sh` 在厂商 base rootfs 上叠加 overlay，产物为 `vendor/boards/DR1M/buildroot/images/rootfs/rootfs.tar.gz`。
- `build_hdmi_dtb.sh` 生成 `images/boot/system.dtb`：厂商 HDMI DTB 加 `pl-ctrl@80000000` polling 节点、传感器 DMA 保留区与 `soft_npu@1f0000000`。脚本先校验含厂商 `Video_NPU` 的合并码流摘要；软核节点必须与该码流配套，缺 IP 时 probe 访问未译码窗口可导致复位。
- `build_kernel_npu.sh` 把 `hard_npu`、`soft_npu`、`cma_mem` 三个驱动内置进内核并重写 `images/boot/uImage.lz4`。FF6 六分支在硬核执行，但厂商 ArmNN 初始化仍需要软核设备。`stage_rul.sh` 暂存封装模型与 ArmNN，`build_rootfs.sh` 校验模型和 AArch64 库依赖后打包；合并镜像启动、两轮 184 窗混合回放及 HDMI / 采集 DMA 基础共存已实板核对，长期稳定性待定；见 [验证结果](../fpga/uisrc/06_doc/验证结果.md) §5.6。
- `build_boot_bin.sh` 用 `soc_hdmi.bit` 与 HDMI FSBL 组装 `images/boot/BOOT.bin`，并复制 `boot.scr`、`uImage.lz4`；若厂商输出中缺少后两者，先在 `vendor/scripts` 执行 `move_files.sh && make_uboot.sh && make_kernel.sh`。
- 只改 PS 应用时不需要重跑 `build_boot_bin.sh`；只改 PL 时不需要重跑 `stage_ps_local.sh`。
- `build_rootfs.sh` 需要 `bc`；若报 `You must install 'bc'`，用 `nix shell nixpkgs#bc -c bash ./build_rootfs.sh`。
- 板端无 RTC：`rootfs-overlay/etc/init.d/S21setclock` 在启动时恢复 `/var/lib/mlk/lasttime`，否则以应用／`plctl`／模型权重的最晚修改时间作为镜像构建基线，停机时回写；网络侧 `TIME_SYNC` 的帧格式与 `host/common/timesync.h` 已固定（`0x0014`，10 字节载荷），PS 端处理仍未实现，按预留返回 `NOT_SUPPORTED`；串口侧可用 `tools/sync_board_time.sh --interval 30` 周期对时。

确认待写文件齐全：

```bash
ls -l vendor/boards/DR1M/buildroot/images/boot/{BOOT.bin,boot.scr,uImage.lz4,system.dtb}
ls -l vendor/boards/DR1M/buildroot/images/rootfs/rootfs.tar.gz
```

### 6.2 写卡

```bash
lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS   # 确认 SD 设备，例如 /dev/sdb
sudo env PATH="$PATH" ./burn_sd.sh /dev/sdb
```

`burn_sd.sh` 会清空目标盘；不要使用默认的 `/dev/sda`，也不要把系统盘作为目标。脚本先清分区表，按固定偏移格式化 FAT32 BOOT + ext4 rootfs，写入 `images/boot/` 与 `images/rootfs/rootfs.tar.gz`，最后写 MBR，避免桌面自动挂载干扰。完成后 `sync` 再拔卡。

NixOS 默认 PATH 不含 `mkfs.ext4` / `e2fsck`：

```bash
nix shell nixpkgs#e2fsprogs nixpkgs#dosfstools nixpkgs#util-linux nixpkgs#rsync nixpkgs#gnutar \
  -c bash -c 'sudo env PATH="$PATH" bash ./burn_sd.sh /dev/sdb'
```

`burn_sd.sh` 还需要 `sfdisk` / `losetup` / `wipefs` / `partx`（util-linux）、`mkfs.vfat` / `fsck.vfat`（dosfstools）、`rsync` 与 `tar`；在 `nix develop` 中执行可补齐宿主工具。

### 6.3 JTAG 在线更新（只更新 PL，不重烧 SD）

`ON-OFF-OFF` 是 SD 启动位，不是 JTAG 位；JTAG 位是 `ON-ON-ON`。SD 启动后 JTAG 通道仍可用，可在不写 SD 卡的情况下在线配置 PL 码流（FPGA 应用）；它不更新 rootfs、dtb 或 Qt 应用。前提是 TD/FD 能识别 DR1M90；本仓库尚未在 SD 启动下成功执行 `Program FPGA`。完整步骤、适用条件和掉电丢失说明见 [`PS-PL开发流程.md`](PS-PL开发流程.md) §5.3.1。

QSPI 固化是另一条路径：SW1=`ON-ON-ON`，FD `ProgramFlash` 写 `BOOT.bin`，完成后切 `ON-ON-OFF`(QSPI)；不要用 `ON-OFF-OFF` 执行 QSPI 固化。

| 目标 | 工具 / 开关 | 内容 |
|---|---|---|
| SD 卡 | `burn_sd.sh`；SW1=`ON-OFF-OFF` 启动 | `BOOT.bin`、`boot.scr`、`uImage.lz4`、`system.dtb` + rootfs |
| QSPI | FD `ProgramFlash`；SW1=`ON-ON-ON` | `BOOT.bin`（FSBL + PL + U-Boot）；32 MB，不含 rootfs |
| eMMC | SD 启动后 `emmc_image/flash_emmc.sh` | eMMC boot / rootfs；不经过 JTAG |
| PL 在线 | JTAG `Program FPGA`；SD 启动 `ON-OFF-OFF` | `.bit`；掉电丢失，不改 rootfs |

---

## 7. 启动与验证

串口在 U-Boot 倒计时期间保持只读，发送回车会取消自动启动。主机周期对时脚本会先确认 Linux 会话再发送命令；手动登录前停止 `mlk-board-timesync.service`，退出串口后恢复，命令见 [工具说明](../tools/README.md#install_board_timesyncsh)。控制台已停在 `=>` 时，停止主机对时服务后执行 `run an_bootcmd` 可继续从 SD 启动，不需要重新烧卡。

1. 取一张写好系统的 SD 卡插入核心板 TF 座；
2. **SW1 拨到 SD 模式 `ON-OFF-OFF`**；
3. 接 12V 上电；
4. 串口（`/dev/ttyACM0` @ 115200 8N1）看启动日志，登录 `root` / `root`（密码输入无回显）；
5. 验证：
   ```bash
   uname -a                 # 确认运行的是自编内核
   cat /proc/device-tree/model
   ifconfig eth1            # 以太网
   ls /dev/fb0                      # HDMI framebuffer（anlogic vdmafb）
   dmesg | grep -iE 'vdmafb|dynclk' # 确认显示驱动
   ```

---

## 8. 回退到出厂 eMMC

- 断电 → **SW1 拨回 `OFF-OFF-OFF`（eMMC）** → 拔掉 SD 卡 → 上电，即回到出厂系统。
- eMMC 从未被本流程修改，无需恢复操作。

---

## 9. 常用脚本速查

本项目脚本（`platform/flash/`）：

| 脚本 | 作用 |
|---|---|
| `stage_ps_local.sh` | 交叉编译 `mlk_ps_local` / `plctl` / `pl_ctrl.ko`，写入 overlay |
| `build_rootfs.sh` | 在厂商 base rootfs 上叠加 overlay，生成 `rootfs.tar.gz` |
| `build_hdmi_dtb.sh` | 生成带 `pl-ctrl@80000000` 的 `system.dtb` |
| `build_boot_bin.sh` | 组装 `BOOT.bin`（FSBL + PL + U-Boot）并复制 boot 文件 |
| `burn_sd.sh` | 写 SD：FAT32 BOOT + ext4 rootfs，最后写 MBR |

厂商脚本（`platform/flash/vendor/scripts/`，首次构建 base 或改用厂商流程时）：

| 脚本 | 作用 |
|---|---|
| `cd "$LAB" && nix develop` | 进入构建环境（NixOS / 单 input `nixos-unstable` flake） |
| `source drcfg.sh` | 设置环境变量（每个新终端都要先 source） |
| `other/move_files.sh` | 把 `boards/files` 的 fsbl/bit/dts 拷到 output 与源码 |
| `uboot/make_uboot.sh` | 编译 u-boot → `u-boot.bin` |
| `kernel/make_kernel.sh` | 编译内核 + dtb → `uImage.lz4` / `system.dtb` |
| `kernel/make_kernel_menuconfig.sh` | 内核 menuconfig |
| `kernel/save_kernel_defconfig.sh` | 存内核 defconfig |
| `rootfs/buildroot/make_rootfs.sh` | Buildroot → `rootfs.tar.gz` / `uInitrd.lz4` |
| `rootfs/buildroot/make_rootfs_menuconfig.sh` | Buildroot menuconfig |
| `rootfs/buildroot/save_rootfs_defconfig.sh` | 存 buildroot defconfig |
| `rootfs/buildroot/create_image.sh` | 打包 boot 镜像 + 选启动参数 |
| `rootfs/buildroot/make_parted.sh` | 分区/格式化 SD |
| `rootfs/buildroot/deploy_image.sh` | 写镜像到 SD |
| `other/make_all.sh` / `save_all.sh` | 全量构建 / 全部回存 defconfig |

---

## 10. 注意事项

- **必须先 `source drcfg.sh`**，否则脚本报 `Please source the configs/<BOARD NAME>.sh script first`。
- 首次构建耗时较长（Buildroot 要下载/编译大量包），需联网。
- 选启动参数只影响 `boot.scr` 里的内核命令行与分区：`ext4` 参数用 `root=/dev/mmcblk0p2`（SD 是 mmcblk0；eMMC 是 mmcblk1），不可混用。
- 只改内核/rootfs、不动 u-boot 与 PL 时，`BOOT.bin` 内容不变；但 `create_image.sh` 会按 `boards/files` 的 elf/bit 重新生成，保持与 demo 一致即可。
- `boards/files/` 顶层必须**只有一个** `.elf` 和一个 `.bit`，否则 `move_files.sh` 报错。
- 版本提示：工程 `version` 写的是 `Anlogic version: SDK_2025.1`，与 `platform/baremetal-sdk/sdk` 的官方 SDK 版本不同；本工程自带 `sources/`，正常构建不依赖官方 SDK。若同时使用官方 SDK 编 FSBL/裸机，注意版本匹配。
- 工程根 `flake.nix` 目前只提供 `devShells.default`（宿主构建依赖）与 `formatter`；它**不自动 source `drcfg.sh`**，进入后仍需手动 `cd scripts && source drcfg.sh`。
- 构建脚本的宿主依赖已尽量由 devShell 提供，但 `make_parted.sh` / `deploy_image.sh` 里的 `parted`/`mount` 等写盘操作仍需 `sudo` 与真实块设备权限（Nix 无法替代）。

---

## 11. 已构建镜像清单（含 WiFi / 4G / HDMI）

产物：`$LAB/boards/DR1M/buildroot/images/`

```
images/boot/
  BOOT.bin         # FSBL + PL bitstream + U-Boot（显示构建用厂商 QT demo 的 BOOT.bin）
  boot.scr         # 启动脚本（parameter-tf-fat-ext4.txt）
  uImage.lz4       # 自编 Linux 内核（含 vdmafb / dynclk 驱动）
  system.dtb       # 自编设备树（含 vdmafb / axi-vdma / dynclk 节点）
  aic_load_fw.ko   # WiFi 预编译模块（AIC8800）
  aic8800_fdrv.ko  # WiFi 预编译模块
  aic8800D80/      # WiFi 固件目录
  GobiNet.ko       # 4G 预编译模块
  quectel-CM       # 4G 拨号工具
images/rootfs/
  rootfs.tar.gz    # ext4 根文件系统（厂家 rootfs + 自编内核模块 + ps-local 应用与 Qt 运行库 / CJK 字体）
```

| 功能 | 落点 | 说明 |
|---|---|---|
| HDMI | PL 显示设计（VTG `0x8010_0000` / VDMA `0x8020_0000`）+ 内核 `anlogic,vdmafb` 驱动 → `/dev/fb0` | 完整说明见 [`HDMI显示通路.md`](HDMI显示通路.md) |
| WiFi | `aic_load_fw.ko` + `aic8800_fdrv.ko` + `aic8800D80/` | 预编译，与内核 6.1.111-rt42 匹配；运行时从 boot 分区 `insmod` |
| 4G | `GobiNet.ko` + `quectel-CM` | 同上；需 **4G 模块 + SIM** |

> **USB 跳线保持 HOST**（勿拨 OTG），WiFi/4G 才可用（4G 与 OTG 物理互斥，见 `接口冲突与可用性分析.md`）。

启动后验证（SD 启动时 boot 分区是 `/dev/mmcblk0p1`）：
```bash
uname -a
ls /dev/fb0 ; dmesg | grep -iE 'vdmafb|dynclk'             # HDMI framebuffer（anlogic vdmafb）
dmesg | grep -iE "stmmac|ethernet"                        # 以太网
cd /mnt/mmcblk0p1 && insmod aic_load_fw.ko && insmod aic8800_fdrv.ko && ifconfig   # wlan0
cd /mnt/mmcblk0p1 && insmod GobiNet.ko && ./quectel-CM    # usb0（4G）
```

---

## 12. NixOS 构建适配（NixOS 26.05 实测）

1. **宿主工具**：`nix shell nixpkgs#gnumake nixpkgs#gcc … nixpkgs#ubootTools nixpkgs#dtc nixpkgs#lz4 …`。（`$LAB/flake.nix` 也提供 devShell，但构建要**就地写盘**，不要用 `nix develop` 在只读 store 里编译。）
2. **shebang**：`scripts/**`、`tools/bootgen/**` 用 `#!/bin/bash`，NixOS 无 `/bin/bash` → 改成系统 bash：
   ```bash
   for f in $(grep -rl '^#!/bin/bash' "$LAB/scripts" "$LAB/tools/bootgen"); do
     sed -i "1s|^#!/bin/bash|$(command -v bash)|" "$f"; done
   ```
3. **sudo 垫片**：`make_kernel.sh` 硬编码 `sudo` 做 `modules_install`（实为写用户自己的 `output`）；无 TTY 会失败，透传即可：
   ```bash
   mkdir -p /tmp/shim; printf '#!/bin/sh\nexec "$@"\n' > /tmp/shim/sudo; chmod +x /tmp/shim/sudo
   export PATH="/tmp/shim:$PATH"
   ```
4. **路径必须 ASCII**：安路 `bootgen`(albg) 处理不了中文路径（报 `Unknown option on command line: …`）。目录已由 `板卡资料` 改为 `MLK-F3P-DR1M90G-Resource`，直接用即可。
5. **Buildroot**：会因 `You must install '/usr/bin/file'` 失败（依赖检查写死 `/usr/bin`）→ `make_rootfs.sh` 跳过、**沿用厂家预编译 rootfs**（功能可用）。要从源码编 rootfs，需修 `sources/buildroot/support/dependencies/dependencies.mk` 的 `file` 检测。

NixOS 26.05 实测构建顺序：
```bash
cd "$LAB/scripts" && source drcfg.sh
move_files.sh && make_uboot.sh && make_kernel.sh && make_rootfs.sh
printf '3\n' | create_image.sh          # 3 = parameter-tf-fat-ext4.txt
# 再把 WiFi/4G 模块拷进 images/boot（见 §11）
```

## WiFi / 4G 网络组件

无线联网需配套启用内核 cfg80211、USB 串口及 USB 网络驱动，并在 rootfs 集成 AIC8800 固件、wpa_supplicant / libnl 与厂商蜂窝组件。构建顺序、HOST 跳线、ps-local 配网与无 SIM 联调见 [无线联网与双端协同](无线联网与双端协同.md)。
