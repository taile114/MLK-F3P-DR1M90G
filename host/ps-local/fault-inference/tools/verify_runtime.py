#!/usr/bin/env python3
"""校验封闭故障运行包；暂存时用 --aarch64 检查目标共享库架构。"""
import argparse
import hashlib
import json
from pathlib import Path
import re

CLASSES = ["normal", "overload", "undervoltage", "overtemperature", "mount_tilt"]
FILES = {"libmlk_fault.so", "preprocess.json", "weights.bin", "weights.tsv",
         "thresholds.bin", "qparams.tsv"}


def verify(root, aarch64=False):
    root = Path(root).resolve()
    manifest = json.loads((root / "model.json").read_text(encoding="utf-8"))
    expected = {"schema": "mlk.fault.runtime.v1", "task": "bench_fault_single_v1",
                "contract": "single_primary_fault_v1",
                "preprocess": "base6_native_blocks_short_context_v2", "classes": CLASSES,
                "input": {"layout": "base6_single_native4s_flat_v1", "floats": 27683}}
    for key, value in expected.items():
        if manifest.get(key) != value:
            raise ValueError("model.json field mismatch: " + key)
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", manifest.get("model", "")):
        raise ValueError("invalid model identity")
    runtime = manifest.get("runtime", {})
    backends = runtime.get("backends", [])
    if (runtime.get("abi") != 1 or runtime.get("library") != "libmlk_fault.so"
            or not isinstance(backends, list) or not backends
            or any(b not in ("cpu", "cpu_cnn_npu") for b in backends)
            or len(set(backends)) != len(backends)):
        raise ValueError("invalid runtime ABI, library or backends")
    plan = ("four_hard_npu_conv3x3_cpu_relu_pool" if "cpu_cnn_npu" in backends
            else "cpu_fp32_reference")
    if manifest.get("execution_plan") != plan:
        raise ValueError("execution plan mismatch")
    files = manifest.get("files", {})
    if set(files) != FILES:
        raise ValueError("runtime must seal all six payload files")
    for name, expected_hash in files.items():
        path = root / name
        if path.resolve().parent != root or not path.is_file():
            raise ValueError("missing or external asset: " + name)
        if not isinstance(expected_hash, str) or not re.fullmatch(r"[a-fA-F0-9]{64}", expected_hash):
            raise ValueError("invalid SHA-256: " + name)
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != expected_hash.lower():
            raise ValueError("asset checksum mismatch: " + name)
    if (root / "thresholds.bin").stat().st_size != 32:
        raise ValueError("thresholds.bin must hold eight float32 values")
    preprocess = json.loads((root / "preprocess.json").read_text(encoding="utf-8"))
    if (preprocess.get("schema") != "mlk.fault.preprocess.v1"
            or preprocess.get("preprocess") != manifest["preprocess"]):
        raise ValueError("preprocess identity mismatch")
    if aarch64:
        header = (root / runtime["library"]).read_bytes()[:20]
        if (len(header) < 20 or header[:6] != b"\x7fELF\x02\x01"
                or int.from_bytes(header[16:18], "little") != 3
                or int.from_bytes(header[18:20], "little") != 183):
            raise ValueError("library must be an AArch64 little-endian ELF shared object")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--aarch64", action="store_true")
    args = parser.parse_args()
    verify(args.root, args.aarch64)
    print("verified: " + str(args.root))
