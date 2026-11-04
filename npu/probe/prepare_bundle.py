"""Package the verified model, replay data, probe source and matching vendor library."""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import shutil
import struct
import tarfile

import numpy as np

HERE = Path(__file__).resolve().parent
WORKSPACE = HERE.parents[2]
NPU_ROOT = HERE.parent
MODEL_ROOT = NPU_ROOT / "training"


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def elf_dependencies(binary: bytes) -> list[str]:
    if binary[:6] != b"\x7fELF\x02\x01":
        raise ValueError("Expected a little-endian ELF64 runtime library")
    offset = struct.unpack_from("<Q", binary, 40)[0]
    size, count = struct.unpack_from("<HH", binary, 58)
    sections = [struct.unpack_from("<IIQQQQIIQQ", binary, offset + i * size) for i in range(count)]
    result = []
    for section in sections:
        if section[1] != 6:
            continue
        strings = sections[section[6]]
        table = binary[strings[4]:strings[4] + strings[5]]
        for position in range(section[4], section[4] + section[5], section[9] or 16):
            tag, value = struct.unpack_from("<qQ", binary, position)
            if tag == 1:
                result.append(table[value:table.index(b"\0", value)].decode("ascii"))
    return result


def package_runtime(source: Path, destination: Path) -> dict:
    prefix = "app/face_detection/libs/armnn_lib/"
    inventory = []
    dependencies = {}
    with tarfile.open(source, "r:gz") as original, tarfile.open(destination, "w:gz", dereference=False) as output:
        for member in original.getmembers():
            if not member.name.startswith(prefix) or member.name == prefix:
                continue
            relative = member.name[len(prefix):].rstrip("/")
            if not relative:
                continue
            parts = PurePosixPath(relative)
            if parts.is_absolute() or ".." in parts.parts:
                raise ValueError("Unsafe runtime archive path")
            if not (member.isfile() or member.isdir() or member.issym()):
                raise ValueError("Unexpected archive member type: " + relative)
            if member.issym() and (PurePosixPath(member.linkname).is_absolute() or ".." in PurePosixPath(member.linkname).parts):
                raise ValueError("Unsafe runtime symlink")
            copied = tarfile.TarInfo(relative)
            copied.mode = member.mode
            copied.mtime = member.mtime
            copied.type = member.type
            copied.linkname = member.linkname
            copied.size = member.size if member.isfile() else 0
            payload = original.extractfile(member).read() if member.isfile() else None
            output.addfile(copied, io.BytesIO(payload) if payload is not None else None)
            inventory.append(relative)
            if payload and payload.startswith(b"\x7fELF") and ".so." in relative:
                dependencies[relative] = elf_dependencies(payload)
    return {"source_archive": str(source.resolve()), "source_sha256": digest(source),
            "packaged_sha256": digest(destination), "member_count": len(inventory),
            "elf_needed": dependencies,
            "license_note": "Vendor Arm NN headers retain their original copyright and SPDX notices."}


def prepare(args) -> None:
    output = args.output.resolve()
    archive_path = output.with_suffix(".tar.gz")
    if output.exists() or archive_path.exists():
        raise FileExistsError("Bundle directory/archive already exists; choose a new --output")
    model_dir = args.model_dir.resolve()
    data_dir = args.data_dir.resolve()
    report = json.loads((model_dir / "optimization_report.json").read_text(encoding="utf-8"))
    if report["status"] != "passed" or digest(model_dir / "rul_fp32.onnx") != report["onnx_sha256"]:
        raise ValueError("The model must match the passed optimization report")
    with np.load(data_dir / "test.npz", allow_pickle=False) as data:
        raw = data["samples"]
        if raw.ndim != 4 or tuple(raw.shape[1:]) != (10, 11, 23):
            raise ValueError("Expected processed samples [N,10,11,23]")
        inputs = raw.reshape(len(raw), 110, 23).transpose(0, 2, 1).astype("<f4", order="C")
        labels = data["labels"].reshape(-1).astype("<f4")
    with np.load(model_dir / "predictions.npz", allow_pickle=False) as data:
        if not np.array_equal(labels, data["target"].reshape(-1)):
            raise ValueError("Labels do not match the verified model's predictions")
        reference = data["pytorch_cpu"].reshape(-1).astype("<f4")
    if len(reference) != len(inputs) or not all(np.isfinite(values).all() for values in (inputs, labels, reference)):
        raise ValueError("Invalid replay data")
    original_replay = data_dir / "board_replay" / "test_input_f32.bin"
    if inputs.tobytes() != original_replay.read_bytes():
        raise ValueError("Replay layout differs from the validated dataset binary")
    for directory in ("src", "vendor", "models", "data", "contracts"):
        (output / directory).mkdir(parents=True)
    shutil.copyfile(HERE / "rul_probe.cpp", output / "src" / "rul_probe.cpp")
    for name in ("build.sh", "check_board.sh", "run.sh"):
        shutil.copyfile(HERE / name, output / name)
    shutil.copyfile(model_dir / "rul_fp32.onnx", output / "models" / "rul_fp32.onnx")
    for name in ("input_contract.json", "normalization.npz", "preprocessing.json", "optimization_report.json",
                 "f3p_parser_static_check.json"):
        shutil.copyfile(model_dir / name, output / "contracts" / name)
    inputs.tofile(output / "data" / "input_f32.bin")
    reference.tofile(output / "data" / "reference_f32.bin")
    labels.tofile(output / "data" / "labels_f32.bin")
    with (data_dir / "sample_index.csv").open(encoding="utf-8-sig", newline="") as stream:
        rows = [row for row in csv.DictReader(stream) if row["split"] == "test"]
    if len(rows) != len(inputs):
        raise ValueError("Sample metadata count does not match the replay data")
    with (output / "data" / "samples.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    runtime = package_runtime(args.vendor_archive.resolve(), output / "vendor" / "armnn.tar.gz")
    write_json(output / "vendor" / "runtime_manifest.json", runtime)
    write_json(output / "contracts" / "replay.json", {
        "samples": len(inputs), "input_shape": [1, 23, 110], "input_bytes_per_sample": 10120,
        "dtype": "little_endian_float32", "input_normalization_already_applied": True,
        "reference": "PyTorch CPU predictions from the verified export", "labels_are_not_inputs": True,
        "rul_unit": "observed_aging_test_session", "binary_built": False, "board_tested": False,
    })
    (output / "README.md").write_text(
        "# F3P RUL compatibility probe\n\n"
        "Source, verified FP32 ONNX model, 165 normalized dataset windows, PyTorch reference predictions "
        "and matching vendor Arm NN headers/libraries. No compiled executable is included.\n\n"
        "## Build in the vendor Linux development environment\n\n"
        "```sh\nexport CXX=/home/uisrc/uisrc-lab-anlogic/tools/aarch64-linux/bin/aarch64-linux-gnu-g++\n"
        "sh build.sh\n```\n\n"
        "Set CXX to the actual matching compiler path. Build uses only the supplied Arm NN SDK and a GNU C++17 "
        "compiler; OpenCV, cameras, Qt and Python are not required on the board.\n\n"
        "## Run on the F3P Linux board\n\n"
        "```sh\nsh check_board.sh > board_env.txt 2>&1\nsh run.sh parse\n"
        "# After parsing succeeds:\nsh run.sh cpu 1\nsh run.sh cpu 0\n"
        "# After NPU/soft-NPU/CMA driver and PL configuration are confirmed:\nsh run.sh mixed 1\n```\n\n"
        "The parse mode does not create an Arm NN runtime, allocate CMA memory or request inference. "
        "Runtime shared libraries must still load successfully. CPU mode requests CpuRef. Mixed mode requests "
        "Alnpu,CpuAcc,CpuRef; a successful result alone does not prove NPU usage. The model is FP32, not quantized.\n\n"
        "Logs and optional graph/profiling files are written under results/. "
        "Exit codes: 2 arguments/files, 20 ONNX parser, 21 input contract, 30 runtime/CMA, "
        "40 optimizer, 50 network load, 60 inference, 70 reference mismatch. "
        "Do not continue to later modes when the preceding mode fails.\n\n"
        "All data files use little-endian float32. Labels are only used for reporting errors; "
        "they are never fed into the model. RUL is measured in observed aging-test sessions, not hours. "
        "The reported enqueue timing is a diagnostic measurement, not a warmed-up production benchmark.\n",
        encoding="utf-8")
    write_json(output / "manifest.json", {
        "files": {str(path.relative_to(output)).replace("\\", "/"): digest(path)
                  for path in sorted(output.rglob("*")) if path.is_file()},
        "binary_built": False, "board_tested": False,
    })
    with tarfile.open(archive_path, "w:gz") as package:
        def modes(member):
            if member.name.endswith(".sh"):
                member.mode = 0o755
            return member
        package.add(output, arcname=output.name, filter=modes)
    print(json.dumps({"bundle": str(output), "archive": str(archive_path), "samples": len(inputs),
                      "archive_bytes": archive_path.stat().st_size, "binary_built": False,
                      "board_tested": False}, ensure_ascii=False))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-dir", type=Path, default=MODEL_ROOT / "outputs/motor_stable23_v1_onnx")
    parser.add_argument("--data-dir", type=Path, default=MODEL_ROOT / "processed/run_to_failure_5motors_v1")
    parser.add_argument("--vendor-archive", type=Path, default=WORKSPACE / "ref/F3P/01_start/02_start_Linux/05_application_demo/05-5_NPU演示/demo/sdk/face_detection.tar.gz")
    parser.add_argument("--output", type=Path, default=NPU_ROOT / "work/f3p-rul-probe")
    prepare(parser.parse_args())


if __name__ == "__main__":
    main()
