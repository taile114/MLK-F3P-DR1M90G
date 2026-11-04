"""Validate a newly built full replay and package it without overwriting archives."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import tarfile

from validate_delivery import replay_metrics, require, sha256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", required=True, type=Path)
    parser.add_argument("--results", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root, results, output = args.bundle.resolve(), args.results.resolve(), args.output.resolve()
    require(root.is_dir() and results.is_dir(), "Bundle and results must exist")
    require(not output.exists(), "Refusing to overwrite an existing archive")
    require(not output.is_relative_to(root), "Archive must be outside the bundle")
    for name in ["checksums.sha256", "replay_verification.json"]:
        require(not (root / name).exists(), "Refusing to change an already packaged bundle")
    require((results / "exit_code.txt").read_text().strip() == "0", "Replay did not exit successfully")
    report = replay_metrics(results / "predictions.csv", (root / "data/reference_f32.bin").read_bytes(),
                            (root / "data/labels_f32.bin").read_bytes())
    preparation = json.loads((root / "preparation.json").read_text(encoding="utf-8"))
    for relative, expected in preparation["provenance"].items():
        require(sha256(root / relative) == expected, "Prepared source changed: " + relative)
    binary = (root / "bin/rul_native").read_bytes()
    require(binary[:6] == b"\x7fELF\x02\x01" and struct.unpack_from("<H", binary, 18)[0] == 183,
            "Expected AArch64 ELF64 binary")
    report.update(status="saved_replay_passed", checkpoint_sha256=preparation["checkpoint_sha256"],
                  binary_sha256=sha256(root / "bin/rul_native"),
                  weights_sha256=sha256(root / "weights.bin"),
                  execution_environment="As supplied by the --results producer; this tool does not execute the model",
                  npu_verified=False)
    (root / "replay_verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    manifest = "".join(f"{sha256(p)}  {p.relative_to(root).as_posix()}\n"
                       for p in sorted(root.rglob("*")) if p.is_file())
    (root / "checksums.sha256").write_text(manifest, encoding="ascii", newline="\n")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(output, "w:gz", format=tarfile.GNU_FORMAT) as archive:
        def portable(member):
            member.mtime = 0
            member.mode = 0o755 if member.isdir() or member.name.endswith("/bin/rul_native") else 0o644
            return member
        archive.add(root, arcname=root.name, filter=portable)
    print(json.dumps({**report, "archive": str(output), "sha256": sha256(output)}, indent=2))


if __name__ == "__main__":
    main()
