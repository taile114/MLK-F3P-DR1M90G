"""Validate and archive an offline test bundle without changing model assets."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import tarfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def package(bundle, output):
    if output.exists():
        raise ValueError("Refusing to replace a frozen archive")
    manifest = json.loads((bundle / "model.json").read_text())
    if manifest["input_count"] != 27683 or manifest["test_samples"] != 360:
        raise ValueError("Unexpected test contract")
    for name in ("fault_test", "timeout"):
        data = (bundle / "bin" / name).read_bytes()
        if data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", data, 18)[0] != 183:
            raise ValueError("Expected little-endian AArch64 ELF")
    sizes = {"test_inputs.bin": 360 * 27683 * 4, "test_reference.bin": 360 * 9 * 4,
             "test_quantized.bin": 360 * 9 * 4, "test_cnn_u8.bin": 360 * 768}
    for name, size in sizes.items():
        if (bundle / "data" / name).stat().st_size != size:
            raise ValueError("Incomplete test vectors")
    files = sorted(p for p in bundle.rglob("*") if p.is_file() and not any(
        part.startswith(("results_", "emulation_", "graph_", "__pycache__")) for part in p.relative_to(bundle).parts)
        and p.name != "checksums.sha256")
    (bundle / "checksums.sha256").write_text("".join(f"{sha(p)}  {p.relative_to(bundle).as_posix()}\n" for p in files), encoding="ascii", newline="\n")
    files.append(bundle / "checksums.sha256")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(output, "w:gz", format=tarfile.PAX_FORMAT) as archive:
        for path in files:
            info = archive.gettarinfo(str(path), (Path(bundle.name) / path.relative_to(bundle)).as_posix())
            info.uid = info.gid = info.mtime = 0
            info.uname = info.gname = ""
            info.mode = 0o755 if path.parent.name == "bin" or path.suffix == ".sh" else 0o644
            with path.open("rb") as stream:
                archive.addfile(info, stream)
    report = {"archive": str(output.resolve()), "archive_bytes": output.stat().st_size,
              "archive_sha256": sha(output), "files": len(files), "architecture": "AArch64",
              "physical_npu_verified": bool(manifest.get("hardware_execution_verified", False))}
    output.with_suffix(output.suffix + ".json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    package(args.bundle, args.output)
