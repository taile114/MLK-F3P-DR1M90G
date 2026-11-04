# MLK-F3P-DR1M90G · Intelligent Operation & Maintenance / Fault Diagnosis (Anlogic DR1M90G FPSoC)

> Topic: **Track 5 — Intelligent Operation & Maintenance / Fault Diagnosis.**
> A fault-diagnosis system on the Milianke **MLK-F3P-DR1M90G** board (Anlogic **DR1M90G**, PS = ARM cores + PL = FPGA).
> It covers four workstreams: **peripherals** (sensor carrier board + commercial motor set), **PL** (FPGA acquisition logic), **PS** (Linux/Buildroot drivers and applications), and **NPU** (board-side life-model inference; FF6 hybrid replay and basic HDMI/acquisition/NPU coexistence verified; prolonged stability remains pending).
> Chinese version: [`README_CN.md`](README_CN.md).

---

## 1. System overview

The motor under test is a purchased, self-driven set; the board only senses it. Temperature, vibration and current signals enter PL through J12; the OID encoder uses the on-board RS485 interface at J11. All four channels share the standalone `fpga_top` project. All four channels also have SoC register/DMA and PS-local integration; encoder speed has been verified through both the PL USB UART and PS-local. See [`docs/RS485转速通道接入.md`](docs/RS485转速通道接入.md).

```
sensor board ─P1/J12(PL I/O)─► PL: acquisition IP (SPI/I²C/counter/comparator)
                                   └─ AXI registers + FIFO/VDMA ─► PS (Linux driver)
PS: read registers / DMA → process → UDP to host / HDMI display / store to SD
```

- Architecture diagram: [`docs/MLK-F3P-DR1M90G-方案B-系统架构图.svg`](docs/MLK-F3P-DR1M90G-方案B-系统架构图.svg)
- Platform: Milianke MLK-F3P-DR1M90G; chip Anlogic DR1M90G (PS = ARM, PL = FPGA).
- PL toolchain: Anlogic **TD** (+ design integrator / ChipWatcher). PS: **Linux / Buildroot**.

### Workstreams

| Area | Content | Entry point |
|---|---|---|
| PL | Acquisition IP, AXI registers, IRQ, FIFO/VDMA | [`fpga/`](fpga/README.md) · [`docs/PL-PS接口约定.md`](docs/PL-PS接口约定.md) |
| PS | Linux/Buildroot, kernel drivers, applications (UDP / HDMI / storage) | [`docs/SD卡启动与Linux构建指南.md`](docs/SD卡启动与Linux构建指南.md) |
| NPU | On-chip NPU inference (fused CPU / CPU+NPU hybrid, FF6 six-branch life model); combined HDMI/acquisition/soft-NPU image verified for boot, basic coexistence and two 184-window FF6 replays | [`docs/PS-PL开发流程.md`](docs/PS-PL开发流程.md) §六 |
| Peripherals | Sensor carrier board (schematic/PCB complete) | [`docs/传感器板-说明.md`](docs/传感器板-说明.md) |

The motor under test runs independently and does not communicate with this board. Peripherals are complete; power / interfaces / grounding / Hall constraints are documented in [`docs/传感器板-说明.md`](docs/传感器板-说明.md).

> The old "motor board" (VNH5050A H-bridge / Buck / ACS725 / ADC / CAN) was archived as **`MLK-F3P-Motor`** ([`docs/Motor/`](docs/Motor/README.md), `eda/archive/`) and is no longer maintained. Its circuit was too complex; a sensor carrier board plus a commercial motor set covers this project's sensing needs, so a motor board is not drawn. Its component count / net names / pin assignment do not apply here.

---

## 2. Repository layout

| Path | Contents |
|---|---|
| [`fpga/`](fpga/README.md) | **PL** (FPGA) code: RTL / constraints / TD project |
| [`host/`](host/README.md) | Host apps: PS-local Qt HMI and remote PC UDP host; `vendor/` not in git |
| [`npu/`](npu/README.md) | Edge AI (NPU): models / training / runtime packaging; see `npu/README.md` |
| [`docs/PL-PS接口约定.md`](docs/PL-PS接口约定.md) | **Interface contract** (address / registers / IRQ / DMA / versioning) |
| [`docs/PS-PL开发流程.md`](docs/PS-PL开发流程.md) | PL / PS development steps and hand-off; build & flash overview (§5); NPU (§6) |
| [`docs/SD卡启动与Linux构建指南.md`](docs/SD卡启动与Linux构建指南.md) | **PS**: SD-card boot, kernel / U-Boot / Buildroot build and flashing |
| [`docs/接口冲突与可用性分析.md`](docs/接口冲突与可用性分析.md) | SW1 boot modes, USB HOST/OTG, pin-mux conflicts |
| [`docs/传感器板-说明.md`](docs/传感器板-说明.md) | Peripherals: sensor board (complete) |
| [`docs/Motor/`](docs/Motor/README.md) | Archived motor board `MLK-F3P-Motor` docs (no longer maintained) |
| `platform/flash/` | PS build and SD flashing workspace; see [`platform/flash/README.md`](platform/flash/README.md) |
| `platform/baremetal-sdk/`, `platform/factory-image/` | Vendor SDK and factory image; **not in git**, reference only |
| `docs/MLK-F3P-DR1M90G-Resource/` | Vendor resource pack (**~34 GB, not in git**) |
| `eda/` | LCEDA snapshot (peripherals, complete); `archive/` holds the motor board |
| `tools/` | Helper scripts; see [`tools/README.md`](tools/README.md) |
| `flake.nix` | Nix devShell: ngspice / xschem / verilator / iverilog / gtkwave / python |

---

## 3. Quick start

### 3.1 Dev environment (NixOS, recommended)

```bash
nix develop        # devShell: ngspice / xschem / verilator / iverilog / gtkwave / python
```

On non-NixOS (Ubuntu), install dependencies per [`docs/SD卡启动与Linux构建指南.md`](docs/SD卡启动与Linux构建指南.md) §1. Anlogic TD (Tang Dynasty) is closed-source and must be installed separately; it is not part of the flake.

### 3.2 Changing the PS ⇄ PL interface (standard three steps)

1. Edit [`docs/PL-PS接口约定.md`](docs/PL-PS接口约定.md) first
2. Both sides confirm and bump the version
3. PL and PS implement to the contract

---

## 4. PS ⇄ PL interface

- **Interface follows [`docs/PL-PS接口约定.md`](docs/PL-PS接口约定.md)**; PL and PS implement to it.
- **Minimal register set** (freeze these 5 first):

| Offset | Name | Access | Bits | Meaning |
|---|---|---|---|---|
| `0x00` | `ID` | RO | `[31:16] MAGIC` · `[15:0] VER` | ID/version (checked by `pl_ctrl_probe()`) |
| `0x04` | `CTRL` | RW | `[0] EN` · `[1] RST` · `[2] IRQ_EN` · `[7:4] MODE` | enable / reset / IRQ enable / mode |
| `0x08` | `STAT` | RO/W1C | `[0] BUSY` · `[1] OVF` · `[15:8] STATE` | status; `OVF` write-1-clear |
| `0x0C` | `DATA` | RO | `[15:0] SAMPLE` · `[31:16] CNT` | latest sample + count |
| `0x10` | `IRQ` | W1C | `[0] DONE` | IRQ flag, write-1-clear |

- **Base address** is assigned by TD design integrator (check `DS*_BASEADDR0` in `Design.xml`); the reference example uses `0x8000_0000`. PL register definitions carry offsets only, no base.
- **Rule**: additions only; changing an existing offset or semantics is a breaking change → bump the version + confirm on both sides + update both sides.
- Full contract: [`docs/PL-PS接口约定.md`](docs/PL-PS接口约定.md); PL code directory: [`fpga/README.md`](fpga/README.md).

---

## 5. Development flow (PL / PS / NPU)

| Line | Tool | Artifact | Hand-off |
|---|---|---|---|
| **PL** | TD (`fpga_prj.al`) | `fpga_prj.bit` → `system.bit` | ① IP base address ② PL→PS IRQ number ③ bitstream |
| **PS/SoC** | FD (FSBL / bootgen) | `soc_fsbl.elf` | — |
| **PS** | `$LAB` (linux/u-boot/buildroot) | `uImage.lz4`·`system.dtb`·`u-boot.bin`·`rootfs.tar.gz` | — |
| **NPU** | vendor ArmNN runtime + `hard_npu` / `soft_npu` / `cma_mem`; the combined PL includes vendor Video_NPU | FF6 six-branch quantized model + `libmlk_rul.so` + sealed run bundle | combined-image boot, two 184-window hybrid replays and basic HDMI/sensor-DMA coexistence verified; prolonged stability remains pending |

**Bring-up order**: get PL working alone (inspect with ChipWatcher) → get PS working alone (`devmem` read/write) → integrate and pass the minimal connectivity test → freeze interface v1.0.

**Minimal connectivity test (all 3 required to freeze)**:
1. PS writes `CTRL[0]=1` → PL reacts;
2. PL counter → PS reads `DATA.CNT` incrementing;
3. PL raises one IRQ → PS receives and counts +1, and can clear it by writing `IRQ[0]=1`.

See [`docs/PS-PL开发流程.md`](docs/PS-PL开发流程.md) for details.

---

## 6. Build & flash

### 6.1 `BOOT.bin` composition

```
BOOT.bin = fsbl.elf + system.bit(PL) + u-boot.bin     ← only these three
FAT(BOOT) partition = BOOT.bin + boot.scr + uImage.lz4 + system.dtb
ext4 partition      = unpacked rootfs.tar.gz
```

The board's QSPI is only **32 MB**, while `rootfs.tar.gz ≈ 63 MB`; the PS system does not fit in the boot image.

### 6.2 Three ways to load the PL bitstream

1. **Baked into BOOT.bin (production)**: overwrite `$LAB/boards/files/system.bit` → `create_image.sh`; FSBL configures it at power-on.
2. **Runtime kernel load (preferred for iteration)**:
   ```bash
   scp pl_ctrl.bit root@<board>:/lib/firmware/      # use TD's ASCII .bit
   echo pl_ctrl.bit > /sys/class/fpga_manager/fpga0/firmware
   cat /sys/class/fpga_manager/fpga0/state          # expect operating
   ```
3. **JTAG / U-Boot**: FD `Program FPGA`; for an online PL update while booted from SD, see [`docs/PS-PL开发流程.md`](docs/PS-PL开发流程.md) §5.3.1; or U-Boot `alfpga load`.

### 6.3 SD-card boot (PS side; leaves the factory eMMC untouched)

Prepare the vendor base rootfs / U-Boot / kernel once as described in [`platform/flash/README.md`](platform/flash/README.md). Then:

```bash
cd platform/flash
./stage_ps_local.sh && ./build_rootfs.sh        # PS application / driver
./build_hdmi_dtb.sh && ./build_boot_bin.sh      # when PL / DTB changes
lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS       # confirm the SD device
sudo env PATH="$PATH" ./burn_sd.sh /dev/sdX     # erases the target
```

After flashing, `sync`; SW1 = **`ON-OFF-OFF` (SD)** to boot; `OFF-OFF-OFF` (eMMC) to fall back. Serial `/dev/ttyACM0` @ 115200 8N1, login `root`/`root`.

Full commands and NixOS adaptation: [`docs/SD卡启动与Linux构建指南.md`](docs/SD卡启动与Linux构建指南.md) §6.

### 6.4 SW1 modes & interface conflicts

| Mode | PIN1 PIN2 PIN3 | Notes |
|---|---|---|
| JTAG | ON-ON-ON | TD download/debug, QSPI flashing |
| QSPI | ON-ON-OFF | QSPI boot |
| SD | ON-OFF-OFF | SD boot |
| eMMC | OFF-OFF-OFF | Factory system |

- `ON-OFF-OFF` is the SD boot position, not JTAG; JTAG is `ON-ON-ON`. Under SD boot, JTAG can configure the PL online without writing the rootfs; QSPI flashing still needs `ON-ON-ON` + FD `ProgramFlash`. The PS application is not updated over JTAG.
- **USB jumper = HOST** → WiFi + 4G + 2×USB HOST; **SLAVE** → OTG Type-C (loses the whole HOST branch).
- **SD / Ethernet / HDMI / CAN / RS485 are unaffected by this USB jumper**; the JTAG/UART Type-C can be plugged in freely without affecting 4G/WiFi.
- Details: [`docs/接口冲突与可用性分析.md`](docs/接口冲突与可用性分析.md).

---

## 7. Team collaboration

This is a GitHub **private repository**. Workflow:

```bash
git switch -c feat/xxx        # branch off main
git push -u origin feat/xxx
gh pr create --fill           # open a PR, merge after review
git switch main && git pull
```

- **Collaborators**: Settings → Collaborators (or `gh api -X PUT repos/<owner>/<repo>/collaborators/<user> -f permission=push`).
- **Branch protection**: Settings → Branches → protect `main` (require PR + review + status checks).
- **Gitignored assets**: `docs/MLK-F3P-DR1M90G-Resource/` (~34 GB vendor pack) and `platform/baremetal-sdk/` / `platform/factory-image/` are not in git; distribute separately.
- Mandatory checks before commit:

```bash
# Interface changed? Update docs/PL-PS接口约定.md (address / registers / version).
# EDA changed (peripherals are complete)? Save to disk, then ./tools/sync_lceda.sh "description"
```

---

## 8. Documentation index

| Topic | Document |
|---|---|
| PL code / interface constraints | [`fpga/README.md`](fpga/README.md), [`docs/PL-PS接口约定.md`](docs/PL-PS接口约定.md) |
| PL / PS / NPU development steps + build & flash overview | [`docs/PS-PL开发流程.md`](docs/PS-PL开发流程.md) |
| Interface contract (address / registers / IRQ / DMA / versioning) | [`docs/PL-PS接口约定.md`](docs/PL-PS接口约定.md) |
| Host requirements & design (displays / links / config) | [`host/上位机需求与设计.md`](host/上位机需求与设计.md) |
| Host ⇄ PS interface (network / serial frames, commands, encryption) | [`docs/上位机-PS接口约定.md`](docs/上位机-PS接口约定.md) |
| PS-local app interfaces (data source, PL driver / NPU / PC) | [`docs/PS本地应用接口约定.md`](docs/PS本地应用接口约定.md) |
| Transport encryption (ECDH / PSK, AES-256-GCM) usage | [`host/common/README.md`](host/common/README.md) |
| NPU AI interface (inference input / output / model) | [`docs/NPU-AI接口约定.md`](docs/NPU-AI接口约定.md) |
| SD-card boot / kernel / U-Boot / Buildroot | [`docs/SD卡启动与Linux构建指南.md`](docs/SD卡启动与Linux构建指南.md) |
| HDMI display path (PL design / dts / drivers / Qt linuxfb) | [`docs/HDMI显示通路.md`](docs/HDMI显示通路.md) |
| Interface conflicts & usability (SW1 / USB / pin mux) | [`docs/接口冲突与可用性分析.md`](docs/接口冲突与可用性分析.md) |
| Topic conformance (hard tasks vs current state, gaps) | [`docs/选题符合性.md`](docs/选题符合性.md) |
| Peripherals: board circuit / components / power / interfaces / grounding | [`docs/传感器板-说明.md`](docs/传感器板-说明.md) |
| Doc/report writing conventions | [`docs/文档与报告写作约定.md`](docs/文档与报告写作约定.md) |
| Motor board (archived, no longer maintained) | [`docs/Motor/README.md`](docs/Motor/README.md) |
| Partition / architecture diagrams | `docs/构建与烧录-分区图.svg`, `docs/MLK-F3P-DR1M90G-方案B-系统架构图.svg` |
