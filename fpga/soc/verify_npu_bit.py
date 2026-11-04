#!/usr/bin/env python3
"""Refuse to enable the soft-NPU DT node without its matching built bitstream."""
from pathlib import Path
import hashlib
import sys


def verify(out):
    bit = out / 'soc_hdmi.bit'
    stamp = out / 'soc_hdmi.npu.sha256'
    if not bit.is_file() or not stamp.is_file():
        raise SystemExit('NPU bitstream/stamp missing; rebuild fpga/soc/prepare_hdmi_soc.py output with TD')
    digest = hashlib.sha256(bit.read_bytes()).hexdigest()
    if stamp.read_text().strip() != digest:
        raise SystemExit('NPU bitstream digest mismatch; rebuild the merged image with TD')
    return digest


if __name__ == '__main__':
    root = Path(__file__).resolve().parents[2]
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'fpga/uisrc/out/soc_hdmi'
    print('Verified HDMI + sensors + soft NPU bitstream:', verify(out))
