"""Check frozen artifacts, saved numerical evidence and repository portability."""
from __future__ import annotations

import ast
import base64
from collections import Counter
import csv
import gzip
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
import struct
import tarfile
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def floats(raw):
    require(len(raw) % 4 == 0, "Invalid float32 file size")
    return [x[0] for x in struct.iter_unpack("<f", raw)]


def replay_metrics(csv_path, reference_raw, labels_raw):
    reference, labels = floats(reference_raw), floats(labels_raw)
    with csv_path.open(encoding="utf-8-sig", newline="") as stream:
        rows = list(csv.DictReader(stream))
    require(len(rows) == len(reference) == len(labels) == 165, "Expected 165 replay rows")
    errors, differences = [], []
    for i, row in enumerate(rows):
        require(int(row["sample_index"]) == i, "Replay index order differs")
        # C++ prints 10 significant digits, enough to recover the float32 value.
        prediction = struct.unpack("<f", struct.pack("<f", float(row["prediction"])))[0]
        ref = struct.unpack("<f", struct.pack("<f", float(row["reference"])))[0]
        label = struct.unpack("<f", struct.pack("<f", float(row["target"])))[0]
        require(math.isfinite(prediction), "Nonfinite prediction")
        require(ref == reference[i] and label == labels[i], "Replay reference/label differs")
        error = abs(prediction - reference[i])
        require(error <= 1e-4 + 1e-4 * abs(reference[i]), "Reference tolerance exceeded")
        errors.append(error)
        differences.append(prediction - labels[i])
    return {"samples": len(rows), "mismatches": 0, "max_reference_error": max(errors),
            "rmse": math.sqrt(sum(x * x for x in differences) / len(rows)),
            "mae": sum(abs(x) for x in differences) / len(rows)}


def workload_events(path):
    events = []

    def walk(value):
        if isinstance(value, dict):
            for name, child in value.items():
                if "Workload_Execute" in name and "Constant" not in name and isinstance(child, dict):
                    times = [measurement["raw"] for key, measurement in child.items()
                             if key.startswith("Wall clock time_") and isinstance(measurement, dict)]
                    require(len(times) == 1, "Missing workload wall-clock measurement: " + name)
                    events.append({"name": name, "kind": re.sub(r"_Execute_#\d+$", "", name),
                                   "wall_clock_us": times[0]})
                walk(child)
        elif isinstance(value, list):
            for child in value:
                walk(child)

    walk(load_json(path))
    return events


def verify_recovered_board(reference, labels):
    board = ROOT / "results/board"
    files = load_json(board / "recovered-files-sha256.json")["files"]
    expected_paths = set()
    for entry in files:
        path = ROOT / entry["path"]
        require(path.resolve().is_relative_to((board / "recovered").resolve()), "Unsafe recovered path")
        require(path.stat().st_size == entry["bytes"] and sha256(path) == entry["sha256"],
                "Recovered board file differs: " + entry["path"])
        expected_paths.add(path.resolve())
    actual_paths = {path.resolve() for name in ["cpu-accel-v1", "npu-smoke-v1"]
                    for path in (board / "recovered" / name).rglob("*") if path.is_file()}
    require(expected_paths == actual_paths and len(files) == len(expected_paths) == 170,
            "Recovered board inventory differs")

    cpu = load_json(board / "f3p-cpu-mixed-recovered-v1-verification.json")
    for run in cpu["full_runs"]:
        metrics = replay_metrics(ROOT / run["directory"] / "predictions.csv", reference, labels)
        for key, value in metrics.items():
            require(abs(value - run[key]) < 1e-10, "Recovered CPU metric differs: " + key)
    require(len(cpu["full_runs"]) == 2 and not cpu["rul_model_npu_verified"], "Recovered CPU scope differs")
    profile = cpu["profile"]
    totals, counts = Counter(), Counter()
    for segment in profile["segments"]:
        root = ROOT / profile["directory"]
        events = workload_events(root / f"segment_{segment['segment']}_profile.json")
        for event in events:
            totals[event["kind"]] += sum(event["wall_clock_us"])
            counts[event["kind"]] += len(event["wall_clock_us"])
        dot = (root / f"segment_{segment['segment']}.dot").read_text()
        require("BackendID : CpuAcc" in dot and "BackendID : CpuRef" in dot,
                "Recovered mixed CPU graph lacks both backends")
    require(len(profile["segments"]) == 6 and counts["NeonFullyConnectedWorkload"] == 124,
            "Recovered CPU profile coverage differs")
    total = sum(totals.values())
    require(abs(total - profile["nonconstant_workload_sum_us"]) < 1e-7, "CPU profile total differs")
    for entry in profile["workloads"]:
        kind = entry["kind"]
        require(abs(totals[kind] - entry["us"]) < 1e-7 and counts[kind] == entry["calls"]
                and abs(entry["share_of_summed_workload_percent"] - totals[kind] / total * 100) < 1e-8,
                "CPU workload summary differs: " + kind)

    npu = load_json(board / "f3p-npu-smoke-recovered-v1-verification.json")
    for case, entry in npu["cases"].items():
        root = ROOT / entry["directory"]
        with (ROOT / f"results/emulation/npu-smoke-v1/{case}/output.csv").open(newline="") as stream:
            oracle = list(csv.DictReader(stream))
        require(len(oracle) == 1536, "Invalid saved integer reference")
        for backend, run in entry["backends"].items():
            folder = root / backend
            require(int((folder / "exit_code.txt").read_text()) == run["exit_code"]
                    and sha256(folder / "probe.log") == run["probe_log_sha256"], "NPU recovered log differs")
            if run["exit_code"]:
                require(backend == "Alnpu" and case in {"u8_s8", "s8"} and run["exit_code"] == 40
                        and "stage=optimize" in (folder / "probe.log").read_text(), "Unexpected NPU failure")
                continue
            with (folder / "output.csv").open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            require(len(rows) == run["checked_outputs"] == 1536, "NPU output count differs")
            errors = []
            for index, row in enumerate(rows):
                require((int(row["iteration"]), int(row["element"])) == divmod(index, 512),
                        "NPU output order differs")
                require(int(row["expected_quantized"]) == int(oracle[index]["expected_quantized"]),
                        "Physical integer reference differs from saved CPU control")
                error = abs(int(row["actual_quantized"]) - int(row["expected_quantized"]))
                require(error == int(row["error_lsb"]) and error <= 1, "NPU numerical tolerance exceeded")
                errors.append(error)
            require(max(errors) == run["max_error_lsb"] and run["mismatches"] == 0,
                    "NPU numerical summary differs")
            require((folder / "output_bytes.bin").read_bytes()
                    == bytes(int(row["actual_quantized"]) % 256 for row in rows[-512:]), "NPU output bytes differ")
            graph = [{"layer_type": kind, "backend": assigned} for kind, assigned in
                     re.findall(r"LayerType : ([^\\]+)\\lBackendID : ([^\\]+)\\l", (folder / "graph.dot").read_text())]
            require(graph == run["graph_assignments"] and workload_events(folder / "profile.json")
                    == run["execution_events"], "NPU graph/profile summary differs")
        for kind, irq in entry["interrupts"].items():
            values = {}
            for moment in ["before", "after"]:
                raw = (root / f"interrupts_{moment}.txt").read_text()
                match = re.search(r"^\s*\d+:\s+((?:\d+\s+)+)GICv3.*\b" + kind + r"\s*$", raw, re.MULTILINE)
                require(match is not None, "Missing NPU interrupt snapshot")
                values[moment] = sum(int(value) for value in match[1].split())
            require(values["before"] == irq["before"] and values["after"] == irq["after"]
                    and values["after"] - values["before"] == irq["delta"], "NPU interrupt delta differs")
        require((root / "runtime.sha256").read_text().split()[0] == entry["runtime_sha256"]
                == "5def7ba75e4b59644be6f58deaf8b2bb3791c62ed38b52b6aea08b11a8f728ce", "NPU runtime differs")
    u8 = npu["cases"]["u8"]
    hard = u8["backends"]["Alnpu"]
    compute = [row for row in hard["graph_assignments"] if row["layer_type"] not in {"Input", "Output", "Constant"}]
    require(compute == [{"layer_type": "ALHardNPU", "backend": "Alnpu"}]
            and any(event["kind"] == "AlnpuALHardNPUWorkload" and len(event["wall_clock_us"]) == 3
                    for event in hard["execution_events"])
            and u8["interrupts"]["HARD_NPU_IRQ"]["delta"] == 3
            and u8["interrupts"]["SOFT_NPU_IRQ"]["delta"] == 0
            and npu["physical_hard_npu_u8_verified"] and not npu["rul_model_on_npu_verified"],
            "Incomplete physical hard NPU evidence")
    return {"files_verified": len(files), "full_cpu_replays_recomputed": len(cpu["full_runs"]),
            "profiled_first_window_fc_share_percent": profile["fully_connected_share_percent"],
            "npu_cases_verified": list(npu["cases"])}


def validate():
    inventory = load_json(ROOT / "artifacts/manifest.json")
    checksums = 0
    for entry in inventory["archives"]:
        path = ROOT / "artifacts" / entry["file"]
        require(path.stat().st_size == entry["bytes"], "Archive size differs: " + path.name)
        require(sha256(path) == entry["sha256"], "Archive hash differs: " + path.name)
        with tarfile.open(path) as archive:
            for member in archive.getmembers():
                parts = PurePosixPath(member.name)
                require(not parts.is_absolute() and ".." not in parts.parts,
                        "Unsafe archive member: " + member.name)
            raw = archive.extractfile(entry["checksum_manifest"]).read()
            require(b"\r" not in raw, "Checksum manifest must use LF")
            base = PurePosixPath(entry["checksum_manifest"]).parent
            for line in raw.decode("utf-8").splitlines():
                expected, relative = line.split("  ", 1)
                payload = archive.extractfile((base / relative).as_posix()).read()
                require(hashlib.sha256(payload).hexdigest() == expected,
                        "Internal archive checksum differs: " + relative)
                checksums += 1

    snapshot = load_json(ROOT / "results/source-snapshot.json")
    integrated_tools = {"tools/prepare_parser_diagnostics.py", "tools/prepare_parser_compat_v2.py"}
    unchanged = 0
    for entry in snapshot["files"]:
        if entry["path"] in integrated_tools:
            continue
        require(sha256(ROOT / entry["path"]) == entry["sha256"],
                "Frozen source copy differs: " + entry["path"])
        unchanged += 1

    checked_python = checked_shell = checked_links = 0
    for path in ROOT.rglob("*"):
        if not path.is_file() or "work" in path.relative_to(ROOT).parts or "__pycache__" in path.parts or ".venv" in path.parts:
            continue
        if path.suffix == ".py":
            ast.parse(path.read_text(encoding="utf-8-sig"), filename=str(path))
            checked_python += 1
        elif path.suffix == ".sh":
            require(b"\r" not in path.read_bytes(), "Shell script must use LF: " + str(path))
            checked_shell += 1
        elif path.suffix == ".json":
            load_json(path)
        elif path.suffix == ".md":
            content = path.read_text(encoding="utf-8-sig")
            for target in re.findall(r"\[[^\]]*\]\(([^)]+)\)", content):
                if re.match(r"[a-zA-Z][a-zA-Z0-9+.-]*:", target) or target.startswith("#"):
                    continue
                target = unquote(target.split("#", 1)[0].strip("<>"))
                require((path.parent / target).exists(), "Broken relative link in " + str(path) + ": " + target)
                checked_links += 1

    with tarfile.open(ROOT / "artifacts/f3p-native-rul-arm64-v1.tar.gz") as archive:
        prefix = "f3p-native-rul-v1/"
        binary = archive.extractfile(prefix + "bin/rul_native").read()
        require(binary[:6] == b"\x7fELF\x02\x01" and struct.unpack_from("<H", binary, 18)[0] == 183,
                "Native model binary must be little-endian AArch64 ELF64")
        require(archive.getmember(prefix + "bin/rul_native").mode & 0o111, "Native binary lacks executable mode")
        baseline_preparation = json.loads(archive.extractfile(prefix + "preparation.json").read())
        baseline_sources = {key.replace("\\", "/"): value
                            for key, value in baseline_preparation["provenance"].items()}
        require(hashlib.sha256(archive.extractfile(prefix + "src/native_rul.cpp").read()).hexdigest()
                == baseline_sources["src/native_rul.cpp"],
                "Frozen native source differs from its recorded preparation")
        result = replay_metrics(ROOT / "results/emulation/native-rul/emulation_all/predictions.csv",
                                archive.extractfile(prefix + "data/reference_f32.bin").read(),
                                archive.extractfile(prefix + "data/labels_f32.bin").read())
    saved = load_json(ROOT / "results/emulation/native-rul/verification.json")
    for key in ["max_reference_error", "rmse", "mae"]:
        require(abs(result[key] - saved[key]) < 1e-10, "Saved QEMU metric differs: " + key)
    board = load_json(ROOT / "results/board/f3p-native-rul-board-v1-result.json")
    require(board["backend"] == "CpuRef" and not board["npu_execution_tested"], "Invalid backend scope")
    require(board["full_replay"]["samples"] == 165 and board["full_replay"]["exit_code"] == 0
            and board["full_replay"]["mismatches"] == 0, "Invalid full-board replay summary")
    mixed_path = ROOT / "artifacts/f3p-cpu-accel-arm64-v1.tar.gz"
    with tarfile.open(mixed_path) as archive:
        prefix = "f3p-cpu-accel-v1/"
        for source, member in {
            "native_rul.cpp": "native-rul/src/native_rul.cpp",
            "native_layers.cpp": "native-layers/src/native_layers.cpp",
            "check_npu.sh": "check_npu.sh",
            "run_native_rul.sh": "native-rul/run_native_rul.sh",
            "run_native_layers.sh": "native-layers/run_native_layers.sh",
        }.items():
            require((ROOT / "probe" / source).read_bytes() == archive.extractfile(prefix + member).read(),
                    "Current source differs from compiled mixed test package: " + source)
        mixed = replay_metrics(ROOT / "results/emulation/cpu-accel-v1/mixed-predictions.csv",
                               archive.extractfile(prefix + "native-rul/data/reference_f32.bin").read(),
                               archive.extractfile(prefix + "native-rul/data/labels_f32.bin").read())
        report = json.loads(archive.extractfile(prefix + "verification.json").read())
        for key in ["max_reference_error", "rmse", "mae"]:
            require(abs(mixed[key] - report["full_replay"][key]) < 1e-10, "Mixed metric differs: " + key)
        require(not report["physical_f3p_mixed_verified"] and not report["npu_requested"], "Mixed scope differs")
        for stage in range(6):
            dot = archive.extractfile(prefix + f"emulation_mixed_first/segment_{stage}.dot").read().decode()
            require("BackendID : CpuAcc" in dot and "BackendID : CpuRef" in dot,
                    "Expected both CPU backends in each mixed segment")
    physical = load_json(ROOT / "results/board/f3p-cpu-mixed-v1-result.json")
    serial_path = ROOT / "results/board/f3p-cpu-mixed-v1.log"
    require(sha256(serial_path) == physical["source_log_sha256"], "Physical serial log differs")
    require(sha256(mixed_path) == physical["host_archive_sha256"], "Physical package hash differs")
    serial = serial_path.read_text(encoding="utf-8-sig")
    require(len(re.findall(r"^.+: OK$", serial, re.MULTILINE))
            == physical["board_internal_checksum_ok"] == 193, "Physical checksum count differs")
    starts = list(re.finditer(r"^requested_backend=(\S+).*$", serial, re.MULTILINE))
    run_names = ["first_window_profiled", "mixed_full_replay", "reference_full_replay"]
    require(len(starts) == len(run_names), "Expected three physical model runs")
    for i, name in enumerate(run_names):
        run = physical[name]
        block = serial[starts[i].start():starts[i + 1].start() if i + 1 < len(starts) else len(serial)]
        match = re.search(r"^result=inference_completed .+$", block, re.MULTILINE)
        require(match is not None, "Missing physical completion: " + name)
        fields = dict(item.split("=", 1) for item in match[0].split())
        require(starts[i][1] == run["backend"], "Physical backend differs: " + name)
        for key in ["samples", "mismatches", "max_reference_error", "rmse", "mae", "mean_forward_us"]:
            require(float(fields[key]) == run[key], "Physical serial metric differs: " + name + "/" + key)
        require(run["samples"] == (1 if i == 0 else 165) and run["mismatches"] == 0
                and run["exit_code"] == 0 and run["mean_forward_us"] > 0,
                "Invalid physical run: " + name)
        require("exit_code=0 results=" + run["board_result_directory"] in block,
                "Physical run output directory differs: " + name)
    for case, code in physical["layer_controls"]["raw_exit_codes"].items():
        require(f"--- {case} exit_code={code} ---" in serial, "Physical layer summary differs: " + case)
    ratio = physical["reference_full_replay"]["mean_forward_us"] / physical["mixed_full_replay"]["mean_forward_us"]
    require(abs(ratio - physical["comparison"]["observed_reference_to_mixed_time_ratio"]) < 1e-12,
            "Physical time ratio differs")
    require(physical["physical_f3p_mixed_verified"] and physical["board_csv_and_profile_retrieved"]
            and not physical["quantized"] and not physical["npu_execution_verified"],
            "Physical evidence scope differs")
    with tarfile.open(mixed_path) as archive:
        recovered = verify_recovered_board(
            archive.extractfile("f3p-cpu-accel-v1/native-rul/data/reference_f32.bin").read(),
            archive.extractfile("f3p-cpu-accel-v1/native-rul/data/labels_f32.bin").read())
    sd = load_json(ROOT / "results/board/f3p-npu-sd-boot-v1-result.json")
    for name, expected in sd["source_logs"].items():
        require(sha256(ROOT / "results/board" / name) == expected, "SD serial log differs: " + name)
    require(sd["physical_sd_boot_verified"] and "Alnpu" in sd["runtime"]["registered_backends"]
            and sd["runtime"]["cpu_relu_control"]["passed"] and not sd["hardware_npu_workload_verified"],
            "SD boot evidence scope differs")
    npu_board = load_json(ROOT / "results/board/f3p-npu-smoke-u8-board-v1-result.json")
    npu_serial_path = ROOT / "results/board/f3p-npu-smoke-u8-board-v1.log"
    require(sha256(npu_serial_path) == npu_board["source_log_sha256"], "NPU physical serial log differs")
    npu_serial = npu_serial_path.read_text(encoding="utf-8-sig")
    require("|Alnpu   |ALHardNPU" in npu_serial and npu_board["hardware_hard_npu_convolution_verified"]
            and not npu_board["rul_model_on_npu_verified"], "NPU physical scope differs")
    for backend in ["CpuRef", "Alnpu"]:
        run = npu_board["runs"][backend]
        require(run["exit_code"] == 0 and run["mismatches"] == 0 and len(run["iterations"]) == 3
                and run["max_error_lsb"] <= 1 and not run["cpu_fallback_allowed"], "Invalid NPU control: " + backend)
    require(npu_board["interrupts"]["HARD_NPU_IRQ"]["total_delta"] == 3
            and npu_board["interrupts"]["SOFT_NPU_IRQ"]["total_delta"] == 0, "NPU interrupt evidence differs")
    npu_types = load_json(ROOT / "results/board/f3p-npu-smoke-board-v1-result.json")
    for name in ["u8_s8", "s8"]:
        require(npu_types["cases"][name]["CpuRef"]["exit_code"] == 0
                and npu_types["cases"][name]["Alnpu"]["exit_code"] == 40
                and npu_types["cases"][name]["Alnpu"]["stage"] == "optimize",
                "NPU representation control differs: " + name)
    with tarfile.open(ROOT / "artifacts/f3p-npu-smoke-arm64-v1.tar.gz") as archive:
        prefix = "f3p-npu-smoke-v1/"
        for source, member in {"npu_smoke.cpp": "src/npu_smoke.cpp", "build_npu_smoke.sh": "build.sh",
                               "run_npu_smoke.sh": "run.sh"}.items():
            require((ROOT / "probe" / source).read_bytes() == archive.extractfile(prefix + member).read(),
                    "NPU smoke package/source differs: " + source)
        smoke = json.loads(archive.extractfile(prefix + "verification.json").read())
        require(not smoke["physical_npu_convolution_verified"] and not smoke["rul_model_used"]
                and not smoke["cpu_fallback_allowed_for_Alnpu"], "NPU smoke scope differs")
        for name in ["u8", "u8_s8", "s8"]:
            saved_csv = ROOT / f"results/emulation/npu-smoke-v1/{name}/output.csv"
            require(saved_csv.read_bytes() == archive.extractfile(prefix + f"emulation_{name}/output.csv").read(),
                    "NPU control CSV differs: " + name)
            with saved_csv.open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            require(len(rows) == 1536, "Expected three 512-element quantized outputs")
            errors = [abs(int(row["actual_quantized"]) - int(row["expected_quantized"])) for row in rows]
            require(max(errors) == smoke["cpu_controls"][name]["max_error_lsb"] == 0,
                    "NPU control CPU evidence differs: " + name)
    with tarfile.open(ROOT / "artifacts/f3p-npu-linear-arm64-v1.tar.gz") as archive:
        prefix = "f3p-npu-linear-v1/"
        for source, member in {"npu_linear.cpp": "src/npu_linear.cpp", "build_npu_linear.sh": "build.sh",
                               "run_npu_linear.sh": "run.sh"}.items():
            require((ROOT / "probe" / source).read_bytes() == archive.extractfile(prefix + member).read(),
                    "Linear control package/source differs: " + source)
        linear = json.loads(archive.extractfile(prefix + "verification.json").read())
        require(linear == load_json(ROOT / "results/emulation/npu-linear-v1/verification.json"),
                "Saved linear CPU report differs")
        require(linear["status"] == "passed" and linear["cpu_controls"] == len(linear["cases"]) == 74
                and linear["checked_outputs"] == sum(case["checked_outputs"] for case in linear["cases"]) == 126768
                and all(case["max_error_lsb"] == 0 and case["exit_code"] == 0 for case in linear["cases"])
                and not linear["physical_linear_npu_execution_verified"] and not linear["full_rul_npu_verified"]
                and not linear["training_executed"], "Linear CPU evidence scope differs")
        require(sha256(ROOT / "results/build/rul-linear-shape-inventory.json") == linear["shape_inventory_sha256"],
                "Linear shape inventory differs")
        binary = archive.extractfile(prefix + "bin/npu_linear").read()
        require(binary[:6] == b"\x7fELF\x02\x01" and struct.unpack_from("<H", binary, 18)[0] == 183
                and hashlib.sha256(binary).hexdigest() == linear["binary_sha256"], "Linear executable differs")
    linear_tools = (ROOT / "results/build/npu-linear-v1-sd-tools.log").read_text()
    require(len(re.findall(r"^.+: OK$", linear_tools, re.MULTILINE)) == 533
            and "result=SD_busybox_gzip_tar_checksums_and_shell_syntax_passed" in linear_tools,
            "Linear package SD tools check differs")
    timeout_report = load_json(ROOT / "results/build/timeout-compat-v1-verification.json")
    installer_path = ROOT / "probe/install_timeout_compat_v1.sh"
    installer = installer_path.read_text()
    require(sha256(installer_path) == timeout_report["installer_sha256"]
            and sha256(ROOT / "probe/timeout_compat.c") == timeout_report["source_sha256"],
            "Timeout compatibility source/installer differs")
    encoded = installer.split("base64 -d <<'F3P_TIMEOUT_PAYLOAD' | gzip -dc > \"$compat/timeout\"\n", 1)[1]
    payload = gzip.decompress(base64.b64decode(encoded.split("\nF3P_TIMEOUT_PAYLOAD", 1)[0]))
    require(hashlib.sha256(payload).hexdigest() == timeout_report["binary_sha256"]
            and len(payload) == timeout_report["binary_bytes"] and payload[:6] == b"\x7fELF\x02\x01"
            and struct.unpack_from("<H", payload, 18)[0] == 183
            and timeout_report["cases"] == 9 and timeout_report["unexpected_failures"] == 0
            and not timeout_report["physical_board_helper_verified"], "Timeout compatibility evidence differs")
    linear_control = load_json(ROOT / "results/board/npu-linear-control-v1-result.json")
    require(sha256(ROOT / "results/board/npu-linear-control-v1.log") == linear_control["source_log_sha256"]
            and linear_control["timeout_helper_self_checks_passed"]
            and linear_control["physical_native_u8_fc_control_verified_from_serial"]
            and [run["exit_code"] for run in linear_control["runs"]] == [0, 0, 0, 40],
            "Physical linear control evidence differs")
    linear_smoke = load_json(ROOT / "results/board/npu-linear-smoke-v1-result.json")
    require(sha256(ROOT / "results/board" / linear_smoke["serial_summary_csv"])
            == linear_smoke["serial_summary_csv_sha256"]
            and len(linear_smoke["runs"]) == 18 and linear_smoke["native_fc_model_shapes_passed"] == 6
            and sum(run["npu_exit_code"] == 0 for run in linear_smoke["runs"]) == 10
            and sum(run["npu_exit_code"] == 40 for run in linear_smoke["runs"]) == 8
            and not linear_smoke["full_rul_npu_verified"], "Physical linear smoke evidence differs")
    failure = load_json(ROOT / "results/board/npu-linear-matrix-v1-failure.json")
    require(failure["exit_code"] == 60 and failure["failed_case"]["shape"] == "m06"
            and failure["failed_case"]["failure_stage"] == "inference"
            and not failure["external_timeout_triggered"] and not failure["all_24_model_shapes_supported"],
            "Known raw matrix failure scope differs")
    with tarfile.open(ROOT / "artifacts/f3p-npu-fc-pad-arm64-v1.tar.gz") as archive:
        prefix = "f3p-npu-fc-pad-v1/"
        for source, member in {"npu_linear_padded.cpp": "src/npu_linear_padded.cpp",
                               "timeout_compat.c": "src/timeout_compat.c",
                               "build_npu_fc_pad.sh": "build.sh", "run_npu_fc_pad.sh": "run.sh"}.items():
            require((ROOT / "probe" / source).read_bytes() == archive.extractfile(prefix + member).read(),
                    "Padded FC package/source differs: " + source)
        padded = json.loads(archive.extractfile(prefix + "verification.json").read())
        require(padded == load_json(ROOT / "results/emulation/npu-fc-pad-v1/verification.json")
                and padded["cpu_controls"] == len(padded["cases"]) == 25
                and padded["checked_outputs"] == sum(case["checked_outputs"] for case in padded["cases"]) == 44544
                and padded["unpadded_CpuRef_cropped_output_exact_matches"] == 25
                and padded["includes_timeout_supervisor"] and not padded["training_executed"]
                and not padded["physical_linear_npu_execution_verified"] and not padded["full_rul_npu_verified"],
                "Padded FC CPU evidence scope differs")
        require(archive.extractfile(prefix + "bin/timeout").read() == payload,
                "Padded FC bundled timeout differs")
        with tarfile.open(ROOT / "artifacts/f3p-npu-linear-arm64-v1.tar.gz") as original:
            for case in padded["cases"]:
                require(case["mode"] == "fc_pad8" and case["max_error_lsb"] == case["exit_code"] == 0
                        and case["physical_input_features"] == (case["input_features"] + 7) // 8 * 8
                        and case["physical_output_features"] == (case["output_features"] + 7) // 8 * 8,
                        "Padded FC dimensions differ: " + case["name"])
                folder = prefix + "emulation/" + case["name"] + "/"
                raw = archive.extractfile(folder + "output.csv").read()
                require(hashlib.sha256(raw).hexdigest() == case["output_csv_sha256"],
                        "Padded FC CPU CSV differs: " + case["name"])
                old_folder = "f3p-npu-linear-v1/emulation/" + case["name"].replace("_fc_pad8", "_fc") + "/"
                require(archive.extractfile(folder + "dequantized_output_f32.bin").read()
                        == original.extractfile(old_folder + "dequantized_output_f32.bin").read(),
                        "Padded FC cropped output differs from original: " + case["name"])
    pad_tools = (ROOT / "results/build/npu-fc-pad-v1-sd-tools.log").read_text()
    require(len(re.findall(r"^.+: OK$", pad_tools, re.MULTILINE)) == 193
            and "result=SD_busybox_gzip_tar_checksums_shell_and_bundled_timeout_preflight_passed" in pad_tools,
            "Padded FC SD tools check differs")
    staged = load_json(ROOT / "results/board/npu-fc-pad-v1-staged-result.json")
    require(sha256(ROOT / "results/board" / staged["serial_summary_csv"])
            == staged["serial_summary_csv_sha256"] and staged["board_package_checks_passed"] == 193
            and staged["physical_control_computation_recovered"]
            and staged["physical_m06_padded_fc_verified_from_serial"]
            and staged["physical_four_output21_shapes_verified_from_serial"]
            and not staged["full_rul_npu_verified"], "Padded FC staged serial scope differs")
    for name, expected_runs in [("control", 2), ("first", 2), ("attention", 8)]:
        group = staged["groups"][name]
        require(group["exit_code"] == 0 and len(group["runs"]) == expected_runs
                and group["passed_backend_runs"] == expected_runs and group["optimize_rejections"] == 0,
                "Padded FC serial group differs: " + name)
        for run in group["runs"]:
            require(run["exit_code"] == run["mismatches"] == run["saturated_original_outputs"] == 0
                    and run["max_error_lsb"] <= 1 and run["iterations"] == 3,
                    "Padded FC serial numerical result differs")
    pad_matrix = load_json(ROOT / "results/board/npu-fc-pad-v1-matrix-result.json")
    require(sha256(ROOT / "results/board" / pad_matrix["serial_summary_csv"])
            == pad_matrix["serial_summary_csv_sha256"]
            and pad_matrix["all_24_padded_shapes_verified_from_serial"]
            and pad_matrix["exit_code"] == pad_matrix["optimize_rejections"] == 0
            and pad_matrix["passed_backend_runs"] == 48 and len(pad_matrix["runs"]) == 24
            and not pad_matrix["full_rul_npu_verified"] and not pad_matrix["training_executed"],
            "Padded FC physical matrix scope differs")
    for run, case in zip(pad_matrix["runs"], padded["cases"][1:], strict=True):
        require(run["shape"] == case["name"].removesuffix("_fc_pad8")
                and all(run[key] == case[key] for key in ["rows", "input_features", "output_features",
                                                         "physical_input_features", "physical_output_features"])
                and run["cpu_exit_code"] == run["npu_exit_code"] == run["cpu_mismatches"] == run["npu_mismatches"] == 0
                and run["cpu_max_error_lsb"] == 0 and run["npu_max_error_lsb"] <= 1,
                "Padded FC matrix dimensions/numerical scope differs")
    from verify_board_linear import verify as verify_linear_board_files
    linear_recovered = verify_linear_board_files()
    require(linear_recovered == load_json(ROOT / "results/board/npu-linear-recovered-v1-verification.json"),
            "Saved linear board-file verification differs")
    return {"status": "passed", "archives": len(inventory["archives"]),
            "internal_checksums": checksums, "unchanged_source_copies": unchanged,
            "python_files": checked_python, "shell_files": checked_shell, "relative_links": checked_links,
            "saved_qemu_replay": result, "saved_mixed_qemu_replay": mixed,
            "saved_mixed_board_replay": physical["mixed_full_replay"],
            "observed_board_reference_to_mixed_time_ratio": ratio,
            "physical_sd_boot_verified": True, "Alnpu_backend_registered": True,
            "npu_smoke_qemu_cpu_controls": smoke["cpu_controls"],
            "saved_linear_cpu_prechecks": {"cases": linear["cpu_controls"],
                                          "outputs": linear["checked_outputs"], "max_error_lsb": 0,
                                          "physical_linear_npu_verified": False},
            "timeout_compatibility": {"qemu_cases": 9, "installer_payload_verified": True,
                                      "physical_helper_verified_from_serial": True},
            "physical_raw_fc_model_shapes_passed_from_serial": 6,
            "physical_raw_matrix_failed_case": {"shape": "m06", "exit_code": 60, "stage": "inference"},
            "saved_padded_fc_cpu_prechecks": {"cases": 25, "outputs": 44544, "max_error_lsb": 0,
                                              "cropped_outputs_equal_unpadded_CpuRef": 25},
            "physical_padded_fc_shapes_passed_from_serial": 24,
            "physical_padded_fc_max_error_lsb": 1,
            "new_linear_raw_board_files_retrieved": True,
            "recovered_linear_files_verified": linear_recovered["files_verified"],
            "physical_padded_fc_shapes_verified_from_files": 24,
            "physical_padded_fc_npu_outputs_recomputed": linear_recovered["padded_matrix_npu_checked_outputs"],
            "physical_hard_npu_u8_convolution_verified": True, "rul_model_npu_verified": False,
            "recovered_physical_board_files": recovered,
            "board_evidence": "Recovered physical CPU CSV metrics and graphs/profiles verified; hard NPU u8 graph, execution events, integer outputs and IRQ snapshots verified",
            "training_executed": False, "board_test_rerun": False, "npu_verified": True,
            "npu_verification_scope": "Recovered physical output bytes, graphs, execution events and IRQs verify u8 3x3 convolution, six raw FC sizes, selected 1x1 convolutions and all 24 zero-padded FC sizes. Original raw matrix fails m06. Trained-weight quantization and full RUL NPU inference not verified"}


if __name__ == "__main__":
    print(json.dumps(validate(), ensure_ascii=False, indent=2))
