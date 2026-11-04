"""Stage official NPU demo boot files and verified probes; never access a disk device."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import zlib

NPU = Path(__file__).resolve().parents[1]
WORKSPACE = NPU.parents[1]
VENDOR_FILES = {
    "BOOT.bin": "567e14124fa17137e964ebcadd4680d8349058beacd4070377ba0ac3fc8c2585",
    "boot.scr": "c48444ee51a8d26660807a94fb8ae7cd620e06622e9ea0f57b969422952d0d6c",
    "system.dtb": "e1ddd7405245d7b9f644c64066bda64369edbac0d8e8676564261d9cfc22b979",
    "uImage.lz4": "b570c65a8deef13bbf7770038a779a2b94c97000afb564896ccdd3e6bba7fab9",
    "uInitrd.lz4": "4dc1f9cfaadfa19cae2dd4b4cd9e0795e3289bbefd5f2d01af9adeada3ae074e",
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_uimage(path):
    raw = path.read_bytes()
    fields = struct.unpack(">7I4B32s", raw[:64])
    header = bytearray(raw[:64])
    header[4:8] = b"\0" * 4
    if fields[0] != 0x27051956 or zlib.crc32(header) & 0xffffffff != fields[1]:
        raise ValueError("Invalid uImage header: " + path.name)
    if len(raw) - 64 != fields[3] or zlib.crc32(raw[64:]) & 0xffffffff != fields[6]:
        raise ValueError("Invalid uImage payload: " + path.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vendor-boot", type=Path,
                        default=WORKSPACE / "ref/F3P/01_start/02_start_Linux/05_application_demo/05-5_NPU演示/demo/boot")
    parser.add_argument("--output", type=Path, default=NPU / "work/f3p-npu-sd-boot-v1")
    args = parser.parse_args()
    root = args.output.resolve()
    if root.exists():
        raise FileExistsError("Refusing to overwrite staging directory: " + str(root))
    for name, expected in VENDOR_FILES.items():
        path = args.vendor_boot / name
        if digest(path) != expected:
            raise ValueError("Vendor file differs from inspected version: " + name)
        if name in ["boot.scr", "uImage.lz4", "uInitrd.lz4"]:
            check_uimage(path)
    inventory = json.loads((NPU / "artifacts/manifest.json").read_text())
    payloads = ["f3p-rul-probe-arm64.tar.gz", "f3p-cpu-accel-arm64-v1.tar.gz"]
    for name in payloads:
        expected = next(e["sha256"] for e in inventory["archives"] if e["file"] == name)
        if digest(NPU / "artifacts" / name) != expected:
            raise ValueError("Probe package differs: " + name)
    card = root / "card"
    card.mkdir(parents=True)
    for name in VENDOR_FILES:
        shutil.copyfile(args.vendor_boot / name, card / name)
    for name in payloads:
        shutil.copyfile(NPU / "artifacts" / name, card / name)
    shutil.copyfile(NPU / "probe/check_npu.sh", card / "check_npu.sh")
    manifest = {"purpose": "Candidate standalone microSD boot with official NPU demo firmware and paired rootfs",
                "files": {p.name: {"bytes": p.stat().st_size, "sha256": digest(p)} for p in sorted(card.iterdir())},
                "uimage_crc_verified": True, "disk_written": False, "physical_board_boot_verified": False,
                "current_eMMC_modified": False,
                "scope": "Vendor boot files include their PL design; this is a separate SD test system, not the project's acquisition PL image"}
    (card / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    (card / "checksums.sha256").write_text("".join(f"{digest(p)}  {p.name}\n" for p in sorted(card.iterdir())),
                                            encoding="ascii", newline="\n")
    (root / "README.md").write_text('''# F3P NPU 备用 SD 启动候选

本目录只准备文件，不访问或写入任何磁盘设备。备用卡尚未格式化或烧写。

`card/` 内文件应放在已确认目标 microSD 的 FAT32 启动分区根目录，
包括 BOOT.bin、boot.scr、system.dtb、uImage.lz4、uInitrd.lz4 和诊断包。
不要把 `card` 目录本身作为启动目录复制。文件名保持原样。

该候选使用厂商完整 NPU 演示固件和 PL，包含硬核/软核 NPU 设备树节点，
内核中已发现 hard_npu、soft_npu、cma_mem 驱动代码，rootfs 带配套 Arm NN。
头部/载荷 CRC 与文件 SHA-256 已核对；尚未在本板实际启动，不保证驱动初始化成功。

下一步先把备用卡通过读卡器接到电脑，确认盘符、容量和可清空范围，
再选择复制到已有 FAT32 分区或重新准备启动分区。不在未识别目标盘时格式化。

卡准备完成后，关闭板卡电源，再把 SW1 从 eMMC 的 OFF-OFF-OFF
改为 SD 的 ON-OFF-OFF，插卡上电，仍通过 Windows PuTTY / COM4 观察日志。
此操作临时使用厂商 PL 配置，不是项目采集 PL。当前 eMMC 内容不由本工具改写。

SD 系统首次启动后，先检查：

    uname -a
    cat /proc/cmdline
    ls -l /dev/hard_npu /dev/soft_npu /dev/cma_mem
    df -h

收到驱动和设备状态后再继续运行库及 NPU 模型验证。
SD 候选使用 initramfs，/root 中的新文件可能随重启消失；卡上的归档可用于恢复。
回到现有 eMMC：断电、SW1 改回 OFF-OFF-OFF、移除备用卡后再上电。

此候选仅用于恢复硬件 NPU 验证环境。寿命模型当前已验证的是 CPU 混合推理，
尚未完成 NPU 量化、后端分配和硬件执行。
''', encoding="utf-8", newline="\n")
    print(json.dumps({"output": str(root), "card_files": len(list(card.iterdir())),
                      "bytes": sum(p.stat().st_size for p in card.iterdir()), "disk_written": False,
                      "uimage_crc_verified": True}, indent=2))


if __name__ == "__main__":
    main()
