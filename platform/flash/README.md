# platform/flash — Build and flash

Linux/Buildroot build and SD flashing for MLK-F3P-DR1M90G.

| File | Purpose |
|---|---|
| `flake.nix` | build environment (host tools) |
| `apply_sdk_local.sh` | apply local patches to vendor/ |
| `build_rootfs.sh` | build the rootfs |
| `build_kernel_npu.sh` | rebuild `uImage.lz4` with the vendor NPU and CMA drivers built in |
| `stage_rul.sh` | install a sealed RUL model bundle and the ArmNN runtime into `rootfs-overlay/` |
| `stage_fault.sh` | verify an AArch64 fault runtime, stage versioned assets and retain the previous link; optional `ARMNN_RUNTIME_ROOT` supplies the paired runtime libraries |
| `stage_ps_local.sh` | cross-build the PS-local Qt app, `plctl` and `pl_ctrl.ko`, then stage them into `rootfs-overlay/`; calls `stage_rul.sh` when `RUL_BUNDLE` is set |
| `load_bit.sh` | build a device-tree overlay that loads a staged bitstream through `of-fpga-region` (no re-burn) |
| `burn_sd.sh` | flash the SD card |
| `rootfs-overlay/` | local additions packed into the rootfs |
| `vendor/` | extracted vendor build tree, not in git |

## Setup

```bash
cd platform/flash
mkdir -p vendor
tar xzf vendor/uisrc-lab-anlogicM-V4.0.1.tar.gz -C vendor --strip-components=1
./apply_sdk_local.sh
```

## Build

Prepare the vendor base rootfs / U-Boot / kernel once as described in Setup; skip this if `vendor/.../output/rootfs.tar.gz`, `boot.scr` and `uImage.lz4` already exist:

```bash
# first-time vendor base
cd vendor/scripts && source drcfg.sh
move_files.sh && make_uboot.sh && make_kernel.sh && make_rootfs.sh
printf '3\n' | create_image.sh        # 3 = parameter-tf-fat-ext4.txt
```

Then run the steps for the changed side:

```bash
# PS application / driver / rootfs
./stage_ps_local.sh
./build_rootfs.sh

# PL bitstream or DTB
./build_hdmi_dtb.sh
./build_boot_bin.sh
```

- `stage_ps_local.sh` cross-builds `mlk_ps_local`, `plctl` and the matching `pl_ctrl.ko`, then stages them into `rootfs-overlay/`; it needs the Qt runtime under `rootfs-overlay/opt/qt_play/qt_lib`.
- `MLK_FAULT_ROOT=/path/to/runtime` enables fault package staging after `stage_ps_local.sh`; `stage_fault.sh` can also run independently and accepts `ROOTFS_OVERLAY` for an isolated staging tree. It does not install the evaluation dataset, checker or Qt application. Required SDK files, separate deployment paths and validation commands are listed in [fault build and delivery](../../host/ps-local/fault-inference/BUILD.md).
- `build_rootfs.sh` overlays the staged files on the vendor base rootfs and writes `vendor/boards/DR1M/buildroot/images/rootfs/rootfs.tar.gz`.
- `build_hdmi_dtb.sh` writes `images/boot/system.dtb` with soft NPU, polling-only `pl-ctrl@80000000` and sensor reserved memory; it requires the matching NPU bitstream digest.
- `build_boot_bin.sh` assembles `images/boot/BOOT.bin` from `soc_hdmi.bit` and the HDMI FSBL, and copies `boot.scr` / `uImage.lz4`; if the vendor output lacks the latter two, run `move_files.sh && make_uboot.sh && make_kernel.sh` in `vendor/scripts` first.
- A PS-only change does not need `build_boot_bin.sh`; a PL-only change does not need `stage_ps_local.sh`.
- `load_bit.sh <name>.bit` builds `/tmp/.../name.bit.dtbo`; applying it on the board (`configfs` overlay) programs the PL from `/lib/firmware/`, so a bitstream can be tried without re-burning the SD card. Requires `dtc` (`nix shell nixpkgs#dtc`).
- `build_rootfs.sh` needs `bc`; if Buildroot reports `You must install 'bc'`, run `nix shell nixpkgs#bc -c bash ./build_rootfs.sh`.
- `rootfs-overlay/etc/init.d/S21setclock` seeds the clock the SoC cannot keep across power cycles: it restores `/var/lib/mlk/lasttime` written on the previous shutdown, else takes the newest payload modification time (`date -r`, the busybox build has no `stat` applet) as the image build baseline, and writes the stamp back on stop. Seeding is not a sync protocol - the seed is the pre-reboot instant, so boot time is lost until the host syncs. `S49mlktime` runs a client-only Chrony service over the current default route (WiFi or cellular). A healthy network source takes priority; `tools/sync_board_time.sh --interval N` supplies host UTC over the console UART only when network synchronisation is unavailable, and `tools/install_board_timesync.sh install` runs it as a systemd user service so a plugged-in console syncs automatically. The network `TIME_SYNC` frame is fixed in `docs/上位机-PS接口约定.md` §4 (command `0x0014`, 10-byte payload, `host/common/timesync.h`) and stays reserved on the PS side (`NOT_SUPPORTED`).
- The board has no tzdata and keeps UTC: `/etc/TZ`, `/etc/profile.d/10-timezone.sh` and `run_arm_app.sh` apply the fixed `CST-8` offset for console tools and the Qt dashboard.

## NPU: matching PL, kernel and device tree

The hard NPU is a PS peripheral at `0x63F00000`. The vendor ArmNN backend also
opens `/dev/soft_npu` during initialization, including the FF6 plan that executes
its six quantized FC branches on the hard NPU. Soft-NPU registers therefore need
a real PL implementation before its driver probes.

`fpga/soc/prepare_hdmi_soc.py` includes the encrypted `design_0_Video_NPU_0` RTL
and `AXI_Protocol_Converter_1` from the local vendor `05-5_NPU演示` package. GP1
provides its control window at `0x1F0000000` / 64 KiB. HDMI VDMA retains HP0;
soft-NPU reads use HP1, and soft-NPU/sensor writes share HP1 through a burst
arbiter that holds the owner until the B response is accepted. The NPU retains
all six AXI ID bits. Generic soft NN operators remain enabled; optional YOLO
postprocessing and image Resize are disabled through the vendor macro switches,
as FF6 uses neither. The encrypted RTL is unchanged. Both NPU clocks, GP1, HP1 and sensor DMA run at 100 MHz.
The soft-NPU interrupt is wired to `f2p_irq[15]` (SPI 97 / GIC 129), matching the
vendor demo. The combined image has booted on hardware: both NPU drivers probe, two 184-window FF6 hybrid replays exit successfully, and CMA returns to zero after each unload. HDMI, the sensor DMA ring and automatic hardware inference also run together. See [board results](../../fpga/uisrc/06_doc/验证结果.md) §5.6; prolonged stability and the soft-NPU datapath/IRQ remain separate checks.

Build in this order, starting at the repository root:

```bash
python3 fpga/soc/prepare_hdmi_soc.py
td-fhs -c 'cd /home/lfour/FHS/TD_Release_2026.2_NL && ./bin/td.sh /home/lfour/Projects/MLK-F3P-DR1M90G/fpga/uisrc/out/soc_hdmi/build_hdmi_soc.tcl'
cd platform/flash
nix shell nixpkgs#lz4 nixpkgs#bc -c bash ./build_kernel_npu.sh
./build_hdmi_dtb.sh
./build_boot_bin.sh
```

The TD installation and repository paths in the command are machine-specific.
`prepare_hdmi_soc.py` replaces its generated output directory; preserve any
bitstream needed for rollback before regenerating it. Successful TD bitgen writes
`soc_hdmi.npu.sha256`; `build_hdmi_dtb.sh` and `build_boot_bin.sh` verify that
this digest matches `soc_hdmi.bit` before packaging a soft-NPU image.

`build_kernel_npu.sh` enables `CONFIG_NPU_MODULE`, `CONFIG_HARD_NPU_DRIVER`,
`CONFIG_SOFT_NPU_DRIVER` and `CONFIG_CMA_MEM_DRIVER` as built-in, then republishes
`uImage.lz4` to `output/` and `images/boot/`. The release is `6.1.111-rt42`.
`build_hdmi_dtb.sh` preserves the vendor HDMI nodes, hard NPU and 128 MiB CMA,
adds the soft-NPU node, polling-only `pl_ctrl`, and the separate sensor DMA
`0x3EF00000` / 1 MiB `no-map` buffer. `build_boot_bin.sh` uses the HDMI FSBL with
HP0/HP1 enabled. `BOOT.bin` contains FSBL, PL and U-Boot; the kernel, DTB and rootfs
remain separate SD files/partitions.

A soft-NPU DT node must not be used with a bitstream lacking that IP: the vendor
driver performs MMIO writes during probe, which can reset the SoC on an undecoded
window. Online PL replacement also requires a matching design while NPU/HDMI/
acquisition drivers are active.

## NPU model bundle

`/opt/mlk_rul/` (engine `libmlk_rul.so`, weights, replay windows, `model.json`) and
`/opt/armnn_lib/lib/` (ArmNN 32.1 runtime) come from the `npu/` artifacts and are not
committed. Build the sealed bundle once:

```bash
cd platform/flash
python3 ../../host/ps-local/inference/prepare_runtime.py --output rul-runtime
CXX="$PWD/vendor/tools/aarch64-linux/bin/aarch64-linux-gnu-g++" \
ARMNN_ROOT="$PWD/../baremetal-sdk/sdk/app/npu/libs/armnn_lib" \
  sh rul-runtime/build.sh
python3 ../../host/ps-local/inference/prepare_runtime.py --seal --output rul-runtime
```

Then stage it with the PS-local application. `stage_ps_local.sh` calls `stage_rul.sh`
when `RUL_BUNDLE` is set, and `stage_rul.sh` verifies the SHA-256 manifest before
copying:

```bash
export RUL_BUNDLE="$PWD/rul-runtime"
export ARMNN_RUNTIME_ROOT="$PWD/../baremetal-sdk/sdk/app/npu/libs/armnn_lib"
./stage_ps_local.sh
./build_rootfs.sh
```

`verify_npu_rootfs.py` checks the sealed FF6 hashes, replay length, AArch64
executables and recursive shared-library dependencies before rootfs packing.
The optional checker, reference and environment probe are staged under
`/opt/mlk_rul-validation/`.

`rul-runtime/` is gitignored. The manifest is written by `--seal`, so rebuild and
re-seal after any engine change; an unsealed bundle is rejected.

At boot `S99qtapp` starts `mlk_ps_local`. The dataset replay does not start by itself:
`/opt/mlk_rul/model.json` only provides the default model directory. A replay starts
when the local panel's start button or a remote replay command is received. Backend 2
(`CPU/NPU`) needs `/dev/hard_npu`, `/dev/soft_npu` and `/dev/cma_mem`; a failed
selection is reported in the panel and is not switched automatically. Both panels
follow the reported execution backend, while preserving a manually edited selection
until the next command.

## Flash

```bash
cd platform/flash
lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS   # confirm the SD device, e.g. /dev/sdb
sudo env PATH="$PATH" ./burn_sd.sh /dev/sdb
```

`burn_sd.sh` wipes the target. The device argument is mandatory; identify the removable SD device before running it. It drops the partition table, formats FAT32 BOOT + ext4 rootfs at fixed offsets, populates them from `images/boot/` and `images/rootfs/rootfs.tar.gz`, then writes the MBR last.

The script needs `sfdisk` / `losetup` / `wipefs` / `partx` (util-linux), `mkfs.vfat` / `fsck.vfat` (dosfstools), `mkfs.ext4` / `e2fsck` (e2fsprogs), `rsync` and `tar`. Run it inside `nix develop` to provide the host tools.

On NixOS, `mkfs.ext4` / `e2fsck` come from e2fsprogs and are not on the default PATH:

```bash
nix shell nixpkgs#e2fsprogs nixpkgs#dosfstools nixpkgs#util-linux nixpkgs#rsync nixpkgs#gnutar \
  -c bash -c 'sudo env PATH="$PATH" bash ./burn_sd.sh /dev/sdb'
```

After flashing: `sync`, power off, set SW1 to SD (`ON-OFF-OFF`), insert the card, power on, serial `/dev/ttyACM0` @115200, log in as `root`/`root`.

### Flash targets

| Target | Medium / tool | Content |
|---|---|---|
| SD card | `burn_sd.sh` | `BOOT.bin`, `boot.scr`, `uImage.lz4`, `system.dtb` + rootfs |
| QSPI | FD `ProgramFlash`, SW1=`ON-ON-ON` | `BOOT.bin` (FSBL + PL + U-Boot); 32 MB, no rootfs |
| eMMC | `emmc_image/flash_emmc.sh` after SD boot | factory eMMC boot / rootfs |
| PL online | JTAG `Program FPGA`, SD boot `ON-OFF-OFF` | `.bit` (volatile, rootfs unchanged) |

JTAG online update only changes the PL; it does not write the SD/eMMC rootfs or the Qt application. Update the PS application by reflashing the SD card or over network/serial. TD/FD must be able to identify the DR1M90; this repository has not yet successfully run `Program FPGA` from an SD boot on hardware. Full steps: [`../../docs/PS-PL开发流程.md`](../../docs/PS-PL开发流程.md) §5.3.1.

## Local changes

`apply_sdk_local.sh` applies these to vendor/, safe to run repeatedly:

- dependency check `/usr/bin/file` becomes `file`, needed on NixOS
- enable vim in the buildroot defconfig
- script shebangs use the system bash

## rootfs overlay

`rootfs-overlay/` holds local additions that `build_rootfs.sh` copies over the staged rootfs before packing:

- `etc/init.d/S98plctrl` — loads `pl_ctrl.ko` at boot when the module is present.
- `etc/init.d/S99qtapp` — autostarts the PS-local Qt HMI when `/opt/qt_play/run_arm_app.sh` exists.
- `opt/qt_play/` — the PS-local Qt bundle (binary + `qt_lib` + `run_arm_app.sh`); large files are gitignored, only `.gitkeep` is committed.
- `usr/bin/plctl` and `lib/modules/<release>/extra/pl_ctrl.ko` — written by `stage_ps_local.sh`.

Run `./build_rootfs.sh` after adding files. The overlay is applied last, so it overwrites base-rootfs files at the same path.

PS-local input runs on the kernel's built-in drivers: a 2.5G USB wireless mouse receiver goes through `usbhid` + evdev (the USB HOST jumper still provides WiFi + 4G), with onboard KEY1/KEY2 as fallback. See [`../../host/ps-local/README.md`](../../host/ps-local/README.md).

## FD / FSBL

FSBL is built with FutureDynasty (FD), which is separate from TD. The local FD 2026.2 install, Platform / Application project steps, the NixOS toolchain shebang fix, the headless `make` command and BOOT.bin generation are documented in [`docs/PL-PS接入与上板交付.md`](../../docs/PL-PS接入与上板交付.md) §3.

Current artifacts (functional verification passed; production timing not closed):

- `fpga/uisrc/out/soc/fd_ws/soc_fsbl/build/FSBL.elf`
- `platform/flash/vendor/boards/DR1M/buildroot/output/BOOT.bin`

## Assembling BOOT.bin

Full bring-up steps, FD/FSBL, BOOT.bin composition, device-tree enablement and board checks are in [`docs/PL-PS接入与上板交付.md`](../../docs/PL-PS接入与上板交付.md).

For the merged HDMI + acquisition image, run `python3 fpga/soc/prepare_hdmi_soc.py` and `fpga/uisrc/out/soc_hdmi/build_hdmi_soc.tcl`, then `platform/flash/build_hdmi_dtb.sh` and `platform/flash/build_boot_bin.sh`. The merged bitstream is `fpga/uisrc/out/soc_hdmi/soc_hdmi.bit`; if the address or IRQ changes, update `docs/PL-PS接口约定.md` first. The HDMI image must use an FSBL whose HPF has `Slave_AXI_HP0_Interface=1`; the candidate FSBL has it cleared and stalls the VDMA. `build_boot_bin.sh` defaults to the vendor HDMI FSBL, override with `FSBL=<path>`. The merged image has booted on hardware; PS-local, HDMI 1080P/Qt, `/dev/pl_ctrl`, temperature/vibration and local records are verified. Production sign-off still requires 100% STA coverage, complete I/O delays and HDMI TMDS / sensor-board timing. During bring-up you can also load the `.bit` at runtime with fpga_manager instead of reflashing.

## WiFi and cellular networking

Build the matching kernel before packing rootfs:

```sh
./build_kernel_npu.sh
./stage_ps_local.sh
./build_rootfs.sh
```

The kernel enables cfg80211, USB option and QMI/CDC/MBIM/RNDIS networking.
`build_rootfs.sh` calls `stage_network.sh` to build wpa_supplicant/libnl and stage
AIC8800D80 firmware, matching vendor AIC modules, GobiNet and quectel-CM.
`NETWORK_ASSETS` selects the vendor boot-asset directory when needed.
`S40mlknetwork` starts wireless authentication and per-interface DHCP; ps-local
provides the WiFi configuration dialog. Cellular dialling is manual and remains
untested without a SIM. See [wireless and dual-end operation](../../docs/无线联网与双端协同.md).

声光告警交付包含 PL v1.3 合并码流、匹配 `pl_ctrl.ko`、ps-local 和 `/usr/bin/mlk-fault`，统一由 `stage_ps_local.sh` 暂存。模型接口和 U7 验证规程见[声光告警与模型故障 API](../../docs/声光告警与模型故障API.md)。
