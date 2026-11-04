#!/usr/bin/env python3
"""由离线测试包生成上位机运行包（mlk.fault.runtime.v1）。

输入是 npu/fault-diagnosis/deploy/prepare.py 生成的离线目录（weights.bin、
weights.tsv、thresholds.bin、qparams.tsv、preprocess.json、model.json、data/）。
本脚本只做表示转换与清单生成，不重新训练、不改变权重、尺度、标准化参数或
频带权重；输出目录供 host/ps-local 的 FaultRuntime 校验并加载。

    python build_runtime.py --offline npu/work/f3p-fault-test-v2 \
        --output out/f3p-fault-single-v1 --library build-aarch64/libmlk_fault.so \
        --model-id f3p-fault-single-v1 --eval-output out/f3p-fault-eval

--backends 默认 cpu,cpu_cnn_npu；只有共享库用 ARMNN_ROOT 构建时才应声明 NPU。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path

SCHEMA_RUNTIME = "mlk.fault.runtime.v1"
SCHEMA_PREPROCESS = "mlk.fault.preprocess.v1"
SCHEMA_EVALSET = "mlk.fault.evalset.v1"
PREPROCESS_ID = "base6_native_blocks_short_context_v2"
TASK = "bench_fault_single_v1"
CONTRACT = "single_primary_fault_v1"
LAYOUT = "base6_single_native4s_flat_v1"
CLASSES = ["normal", "overload", "undervoltage", "overtemperature", "mount_tilt"]

BANDS = 64
TF_FREQS = 193
PSD_FREQS = 769
MAX_HZ = 600.0
TF_COLUMNS = 48
TF_ACTIVE_COLUMNS = 47
TF_CHANNELS = 8

QUALITY_KEYS = (
    ("minBlocks", "min_blocks_per_window", 12),
    ("minBlockSamples", "min_native_block_samples", 64),
    ("minVibrationRateHz", "vibration_rate_hz", 1600),
    ("tfRatio", "min_tf_valid_fraction", 0.45),
    ("fastRatio", "min_fast_valid_fraction", 0.75),
    ("slowRatio", "min_slow_valid_fraction", 0.60),
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def write_json(path: Path, document: dict) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(document, handle, ensure_ascii=False, indent=2, sort_keys=True)
        handle.write("\n")


def flat(values, name: str) -> list[float]:
    """一维或二维数组按行优先展开，校验非有限数。"""
    out: list[float] = []
    for row in values:
        if isinstance(row, list):
            out.extend(float(item) for item in row)
        else:
            out.append(float(row))
    for item in out:
        if item != item or item in (float("inf"), float("-inf")):
            raise ValueError(f"{name}: 含非有限数")
    return out


def checked(values, expected: int, name: str) -> list[float]:
    out = flat(values, name)
    if len(out) != expected:
        raise ValueError(f"{name}: 期望 {expected} 项，实际 {len(out)} 项")
    return out


def expand(values, expected: int, name: str, fill: float) -> list[float]:
    """前 len(values) 项保留训练值，其余通道取 fill（未参与标准化的有效位/年龄）。"""
    out = flat(values, name)
    if len(out) > expected:
        raise ValueError(f"{name}: 实际 {len(out)} 项超出 {expected}")
    return out + [fill] * (expected - len(out))


def expand_tf(values, name: str, fill: float) -> list[float]:
    """训练侧 TF 参数是 3×64（三轴×64 频带），布局要求 8×64；通道 3..7 不参与训练。"""
    source = checked(values, 3 * BANDS, name)
    out = [fill] * (TF_CHANNELS * BANDS)
    for axis in range(3):
        for band in range(BANDS):
            out[axis * BANDS + band] = source[axis * BANDS + band]
    return out


def convert_preprocess(offline: Path, model_id: str) -> dict:
    source = load_json(offline / "preprocess.json")
    version = source.get("preprocessing_version") or source.get("version")
    if version != PREPROCESS_ID:
        raise ValueError(f"preprocess.json 预处理版本不符：{version}")

    band_weights = source.get("band_weights")
    if not band_weights:
        raise ValueError("preprocess.json 缺少 band_weights")
    rows = []
    for row in band_weights:
        if len(row) != TF_FREQS:
            raise ValueError("band_weights 每行必须是 193 项")
        rows.append([float(item) for item in row])
    if len(rows) != BANDS:
        raise ValueError("band_weights 必须是 64 行")

    parameters = source.get("parameters", {})
    tf = parameters["time_frequency"]
    tf_mean = expand_tf(tf["mean"], "norm.tf.mean", 0.0)
    tf_std = expand_tf(tf["std"], "norm.tf.std", 1.0)
    psd = parameters["spectrum"]
    psd_mean = checked(psd["mean"], 3 * PSD_FREQS, "norm.psd.mean")
    psd_std = checked(psd["std"], 3 * PSD_FREQS, "norm.psd.std")
    stats = parameters["vibration_stats"]
    stats_mean = checked(stats["mean"], 12, "norm.stats.mean")
    stats_std = checked(stats["std"], 12, "norm.stats.std")
    pose = parameters["mount_pose"]
    pose_mean = checked(pose["mean"], 5, "norm.pose.mean")
    pose_std = checked(pose["std"], 5, "norm.pose.std")
    fast = parameters["fast_state"]
    fast_mean = expand(fast["mean"], 6, "norm.fast.mean", 0.0)
    fast_std = expand(fast["std"], 6, "norm.fast.std", 1.0)
    slow = parameters["slow_state"]
    slow_mean = expand(slow["mean"], 9, "norm.slow.mean", 0.0)
    slow_std = expand(slow["std"], 9, "norm.slow.std", 1.0)
    for name, values in (("norm.tf.std", tf_std), ("norm.psd.std", psd_std),
                         ("norm.stats.std", stats_std), ("norm.fast.std", fast_std),
                         ("norm.slow.std", slow_std)):
        if any(item <= 0.0 for item in values):
            raise ValueError(f"{name} 必须为正：" + name)

    dsp = source.get("dsp", {})
    reference = dsp.get("level_reference", {}).get("reference_vector_g")
    if not reference or len(reference) != 3:
        raise ValueError("缺少水平重力参考 reference_vector_g")

    quality = {}
    for key, dsp_key, default in QUALITY_KEYS:
        quality[key] = dsp.get(dsp_key, default)
    pose_max_age_s = dsp.get("pose_max_age_s", 1.5)
    quality["poseAgeMs"] = float(pose_max_age_s) * 1000.0

    document = {
        "schema": SCHEMA_PREPROCESS,
        "preprocess": PREPROCESS_ID,
        "model": model_id,
        "task": TASK,
        "frequencies": {"bands": BANDS, "tf": TF_FREQS, "psd": PSD_FREQS, "maxHz": MAX_HZ},
        "bands": {"weights": rows},
        "norm": {
            "tf": {"mean": tf_mean, "std": tf_std},
            "psd": {"mean": psd_mean, "std": psd_std},
            "stats": {"mean": stats_mean, "std": stats_std},
            "pose": {"mean": pose_mean, "std": pose_std},
            "fast": {"mean": fast_mean, "std": fast_std},
            "slow": {"mean": slow_mean, "std": slow_std},
        },
        "pose": {"reference": [float(item) for item in reference]},
        "quality": quality,
        "source_preprocess_sha256": sha256(offline / "preprocess.json"),
        "layout": {"tf": [TF_CHANNELS, BANDS, TF_COLUMNS], "tfActiveColumns": TF_ACTIVE_COLUMNS},
        "generated_by": "host/ps-local/fault-inference/tools/build_runtime.py",
    }
    total = (TF_CHANNELS * BANDS * TF_COLUMNS + 3 * PSD_FREQS + 12 + 8 + 6 * 40 + 9 * 60)
    if total != 27683:
        raise ValueError(f"输入布局长度 {total} 与 27683 不符")
    return document


def build_model_manifest(output: Path, model_id: str, checkpoint: str, backends: list[str],
                         require_library: bool) -> dict:
    files = {}
    for name in ("libmlk_fault.so", "weights.bin", "weights.tsv", "thresholds.bin",
                 "qparams.tsv", "preprocess.json"):
        path = output / name
        if path.exists():
            files[name] = sha256(path)
    if "libmlk_fault.so" not in files and require_library:
        raise ValueError("缺少 libmlk_fault.so：请用 --library 指定共享库")
    plan = "four_hard_npu_conv3x3_cpu_relu_pool" if "cpu_cnn_npu" in backends else "cpu_fp32_reference"
    return {
        "schema": SCHEMA_RUNTIME,
        "model": model_id,
        "task": TASK,
        "contract": CONTRACT,
        "preprocess": PREPROCESS_ID,
        "input": {"layout": LAYOUT, "floats": 27683},
        "classes": CLASSES,
        "runtime": {"abi": 1, "library": "libmlk_fault.so", "backends": backends},
        "execution_plan": plan,
        "checkpoint_sha256": checkpoint,
        "files": files,
    }


def build_eval_set(offline: Path, output: Path, model_id: str) -> None:
    data = offline / "data"
    inputs = data / "test_inputs.bin"
    index = data / "test_index.csv"
    if not inputs.exists() or not index.exists():
        raise ValueError("离线包缺少 data/test_inputs.bin 或 data/test_index.csv")
    output.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(inputs, output / inputs.name)
    shutil.copyfile(index, output / index.name)
    rows = sum(1 for _ in index.open(encoding="utf-8")) - 1
    document = {
        "schema": SCHEMA_EVALSET,
        "model": model_id,
        "task": TASK,
        "inputs": inputs.name,
        "index": index.name,
        "samples": rows,
        "inputFloats": 27683,
        "classes": CLASSES,
        "inputStage": "frozen_normalized_features",
        "generated_by": "host/ps-local/fault-inference/tools/build_runtime.py",
    }
    write_json(output / "evalset.json", document)


def verify_package(output: Path, model_id: str, backends: list[str], require_library: bool = True) -> None:
    """按 FaultRuntime::verifyPackage 与 FaultPreprocessor::loadAssets 的条件自检。"""
    manifest = load_json(output / "model.json")
    if manifest.get("schema") != SCHEMA_RUNTIME:
        raise ValueError("model.json schema 不符")
    for key, expected in (("task", TASK), ("contract", CONTRACT), ("preprocess", PREPROCESS_ID)):
        if manifest.get(key) != expected:
            raise ValueError(f"model.json {key} 不符")
    if manifest.get("model") != model_id:
        raise ValueError("model.json model 标识不符")
    if manifest.get("input", {}).get("layout") != LAYOUT:
        raise ValueError("model.json 输入布局不符")
    if manifest.get("input", {}).get("floats") != 27683:
        raise ValueError("model.json 输入长度不符")
    if manifest.get("classes") != CLASSES:
        raise ValueError("model.json 类别顺序不符")
    runtime = manifest.get("runtime", {})
    if runtime.get("abi") != 1:
        raise ValueError("model.json ABI 不符")
    if runtime.get("backends") != backends:
        raise ValueError("model.json 后端声明与预期不符")
    if "cpu" not in backends and "cpu_cnn_npu" not in backends:
        raise ValueError("model.json 至少声明一个后端")
    library = runtime.get("library", "")
    if not library or "/" in library or "\\" in library or ".." in library:
        raise ValueError("model.json 运行库名称无效")
    if require_library and not (output / library).exists():
        raise ValueError("缺少运行库 " + library)
    files = manifest.get("files", {})
    required = {"weights.bin", "weights.tsv", "thresholds.bin", "qparams.tsv", "preprocess.json"}
    if require_library or (output / library).exists():
        required.add(library)
    if set(files) != required:
        raise ValueError("model.json files 未完整封闭运行资产")
    for name, expected_hash in files.items():
        if "/" in name or "\\" in name or ".." in name:
            raise ValueError("资产名称无效：" + name)
        path = output / name
        if not path.exists():
            raise ValueError("缺少资产 " + name)
        if sha256(path) != expected_hash:
            raise ValueError("资产哈希不符：" + name)

    document = load_json(output / "preprocess.json")
    if document.get("schema") != SCHEMA_PREPROCESS or document.get("preprocess") != PREPROCESS_ID:
        raise ValueError("preprocess.json 身份不符")
    frequencies = document.get("frequencies", {})
    bands = int(frequencies.get("bands", 64))
    tf = int(frequencies.get("tf", 193))
    psd = int(frequencies.get("psd", 769))
    if (bands, tf, psd) != (BANDS, TF_FREQS, PSD_FREQS):
        raise ValueError("preprocess.json 频率网格与训练不一致")
    rows = document.get("bands", {}).get("weights", [])
    if len(rows) != bands or any(len(row) != tf for row in rows):
        raise ValueError("preprocess.json 频带权重形状不符")
    norm = document.get("norm", {})
    lengths = {
        ("tf", "mean"): TF_CHANNELS * bands, ("tf", "std"): TF_CHANNELS * bands,
        ("psd", "mean"): 3 * psd, ("psd", "std"): 3 * psd,
        ("stats", "mean"): 12, ("stats", "std"): 12,
        ("pose", "mean"): 5, ("pose", "std"): 5,
        ("fast", "mean"): 6, ("fast", "std"): 6,
        ("slow", "mean"): 9, ("slow", "std"): 9,
    }
    for (group, key), expected in lengths.items():
        values = norm.get(group, {}).get(key, [])
        if len(values) != expected:
            raise ValueError(f"preprocess.json norm.{group}.{key} 长度不符")
    reference = document.get("pose", {}).get("reference", [])
    if len(reference) != 3:
        raise ValueError("preprocess.json 水平参考无效")
    quality = document.get("quality", {})
    for key in ("minBlocks", "minBlockSamples", "minVibrationRateHz", "tfRatio",
                "fastRatio", "slowRatio", "poseAgeMs"):
        if key not in quality:
            raise ValueError("preprocess.json quality 缺少 " + key)
    print("verified:", output)


def prepare(args) -> None:
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", args.model_id):
        raise ValueError("model-id 必须为 1..64 个 ASCII 字母、数字、点、下划线或连字符")
    offline = args.offline.resolve()
    output = args.output.resolve()
    for name in ("weights.bin", "weights.tsv", "thresholds.bin", "qparams.tsv", "preprocess.json"):
        if not (offline / name).exists():
            raise ValueError(f"离线包缺少 {name}")
    manifest = load_json(offline / "model.json")
    if manifest.get("task_definition") != TASK:
        raise ValueError("离线包任务定义不符：" + str(manifest.get("task_definition")))
    if manifest.get("class_order") != CLASSES:
        raise ValueError("离线包类别顺序不符")
    checkpoint = manifest.get("checkpoint_sha256", "")
    if len(checkpoint) != 64:
        raise ValueError("离线包缺少 checkpoint_sha256")

    output.mkdir(parents=True, exist_ok=True)
    for name in ("weights.bin", "weights.tsv", "thresholds.bin", "qparams.tsv"):
        shutil.copyfile(offline / name, output / name)
    if args.library:
        source_library = args.library.resolve()
        destination_library = (output / "libmlk_fault.so").resolve()
        if source_library != destination_library:
            shutil.copyfile(source_library, destination_library)
    preprocess = convert_preprocess(offline, args.model_id)
    write_json(output / "preprocess.json", preprocess)
    write_json(output / "model.json",
               build_model_manifest(output, args.model_id, checkpoint, args.backends,
                                    args.require_library))
    verify_package(output, args.model_id, args.backends, args.require_library)
    print(f"runtime package: {output}")
    if args.eval_output:
        target = args.eval_output.resolve()
        build_eval_set(offline, target, args.model_id)
        print(f"eval set: {target}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--offline", type=Path, required=True, help="prepare.py 生成的离线包目录")
    parser.add_argument("--output", type=Path, required=True, help="运行包输出目录")
    parser.add_argument("--library", type=Path, help="libmlk_fault.so（AArch64 构建产物）")
    parser.add_argument("--model-id", default="f3p-fault-single-v1", help="运行包标识，ASCII ≤64 字节")
    parser.add_argument("--eval-output", type=Path, help="数据集评估目录（可选）")
    parser.add_argument("--backends", default="cpu,cpu_cnn_npu",
                        help="声明的后端，逗号分隔；默认 cpu,cpu_cnn_npu")
    parser.add_argument("--allow-missing-library", dest="require_library", action="store_false",
                        help="尚未交叉编译共享库时允许先生成清单")
    parser.set_defaults(require_library=True)
    args = parser.parse_args()
    backends = [name.strip() for name in args.backends.split(",") if name.strip()]
    allowed = {"cpu", "cpu_cnn_npu"}
    if not backends or set(backends) - allowed:
        parser.error("--backends 只接受 cpu 与 cpu_cnn_npu")
    args.backends = backends
    prepare(args)


if __name__ == "__main__":
    main()
