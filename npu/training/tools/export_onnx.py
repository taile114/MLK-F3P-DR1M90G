"""Export an existing checkpoint and verify fixed-batch ONNX inference on real data."""
from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.metadata
import json
from pathlib import Path
import shutil
import sys
import traceback
import warnings

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import numpy as np
import onnx
import onnxruntime as ort
import torch

from fkp_dvgt.data import load_dataset
from fkp_dvgt.engine import load_checkpoint, seed_everything
from fkp_dvgt.metrics import regression_metrics


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def save_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def tensor_shape(value) -> list[int | str]:
    return [dim.dim_value if dim.HasField("dim_value") else dim.dim_param for dim in value.type.tensor_type.shape.dim]


def difference(reference: np.ndarray, actual: np.ndarray, atol: float, rtol: float) -> dict:
    if reference.shape != actual.shape or not np.isfinite([reference, actual]).all():
        raise ValueError("Predictions must have matching shapes and finite values")
    error = actual.astype(np.float64) - reference.astype(np.float64)
    worst = int(np.argmax(np.abs(error)))
    return {
        "allclose": bool(np.allclose(reference, actual, atol=atol, rtol=rtol)),
        "atol": atol,
        "rtol": rtol,
        "max_absolute_error": float(np.max(np.abs(error))),
        "mean_absolute_error": float(np.mean(np.abs(error))),
        "rmse_between_predictions": float(np.sqrt(np.mean(error ** 2))),
        "worst_sample_index": worst,
        "worst_reference": float(reference[worst]),
        "worst_actual": float(actual[worst]),
    }


def export_and_check(args, report: dict) -> None:
    checkpoint = args.checkpoint.resolve()
    data_path = args.data.resolve()
    example_path = args.example_data.resolve()
    preprocessing = args.preprocessing.resolve()
    normalization = preprocessing.parent / "normalization.npz"
    protected = list(sorted((ROOT / "fkp_dvgt").glob("*.py"))) + [checkpoint, preprocessing, normalization]
    hashes_before = {str(path): sha256(path) for path in protected}
    report.update(
        checkpoint=str(checkpoint),
        checkpoint_sha256=hashes_before[str(checkpoint)],
        verification_data=str(data_path),
        verification_data_sha256=sha256(data_path),
        example_data=str(example_path),
        versions={name: importlib.metadata.version(name) for name in ("torch", "numpy", "onnx", "onnxruntime")},
        python=sys.version,
        export_settings={"opset": args.opset, "dynamo": False, "device": "cpu", "fixed_batch_size": 1,
                         "training": False, "graph_probability": 0.0, "atol": args.atol, "rtol": args.rtol},
    )
    seed_everything(44)
    torch.set_num_threads(1)
    model, config = load_checkpoint(checkpoint, "cpu")
    model.eval()
    example = load_dataset(example_path)[0][0].unsqueeze(0).contiguous()
    dataset = load_dataset(data_path)
    samples, labels = dataset.tensors
    target = labels.numpy().reshape(-1)
    model_path = args.output / "rul_fp32.onnx"
    with warnings.catch_warnings(record=True) as recorded:
        warnings.simplefilter("always")
        with torch.inference_mode():
            torch.onnx.export(
                model, (example,), str(model_path),
                input_names=["features"], output_names=["rul"],
                opset_version=args.opset, dynamo=False,
                export_params=True, keep_initializers_as_inputs=False,
                dynamic_axes=None, training=torch.onnx.TrainingMode.EVAL,
            )
    report["export_warnings"] = sorted({f"{type(w.message).__name__}: {w.message}" for w in recorded})

    graph = onnx.load(str(model_path))
    preprocessing_config = json.loads(preprocessing.read_text(encoding="utf-8"))
    if preprocessing_config.get("feature_version") != "motor_stable23_v1":
        raise ValueError("This export contract requires motor_stable23_v1 preprocessing")
    onnx.helper.set_model_props(graph, {
        "source_checkpoint_sha256": report["checkpoint_sha256"],
        "inference_mode": "deterministic; graph_probability=0; dropout disabled",
        "input_preprocessing": "motor_stable23_v1; normalization applied outside model",
        "rul_unit": "observed_aging_test_session",
        "target_runtime_status": "F3P Arm NN / Alnpu not validated",
    })
    onnx.checker.check_model(graph, full_check=True)
    onnx.save(graph, str(model_path))
    report["onnx_checker_passed"] = True
    report["onnx_ir_version"] = graph.ir_version
    report["opset_imports"] = [{"domain": item.domain, "version": item.version} for item in graph.opset_import]
    report["inputs"] = [{"name": value.name, "shape": tensor_shape(value),
                          "dtype": onnx.TensorProto.DataType.Name(value.type.tensor_type.elem_type)}
                         for value in graph.graph.input]
    report["outputs"] = [{"name": value.name, "shape": tensor_shape(value),
                           "dtype": onnx.TensorProto.DataType.Name(value.type.tensor_type.elem_type)}
                          for value in graph.graph.output]
    counts = {}
    with (args.output / "node_inventory.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["index", "domain", "operator", "name"])
        for i, node in enumerate(graph.graph.node):
            operator = f"{node.domain}::{node.op_type}" if node.domain else node.op_type
            counts[operator] = counts.get(operator, 0) + 1
            writer.writerow([i, node.domain, node.op_type, node.name])
    report["operator_counts"] = dict(sorted(counts.items()))
    report["node_count"] = len(graph.graph.node)
    report["onnx_size_bytes"] = model_path.stat().st_size
    report["onnx_sha256"] = sha256(model_path)
    if len(graph.graph.input) != 1 or tensor_shape(graph.graph.input[0]) != [1, 23, 110]:
        raise ValueError("Export did not produce exactly one fixed [1,23,110] input")
    if len(graph.graph.output) != 1 or tensor_shape(graph.graph.output[0]) != [1, 1]:
        raise ValueError("Export did not produce exactly one fixed [1,1] output")
    if graph.functions or any(node.domain not in ("", "ai.onnx") for node in graph.graph.node):
        raise ValueError("Unexpected custom operators or local ONNX functions")

    options = ort.SessionOptions()
    options.intra_op_num_threads = 1
    options.inter_op_num_threads = 1
    # Verify the exported graph itself as well as the usual optimized ORT graph.
    options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
    session = ort.InferenceSession(str(model_path), sess_options=options, providers=["CPUExecutionProvider"])
    cpu_predictions = []
    onnx_predictions = []
    with torch.inference_mode():
        for i in range(len(dataset)):
            x = samples[i:i + 1].contiguous()
            cpu_predictions.append(float(model(x).item()))
            result = session.run(["rul"], {"features": x.numpy()})[0]
            if result.shape != (1, 1):
                raise ValueError(f"Unexpected ONNX result shape: {result.shape}")
            onnx_predictions.append(float(result[0, 0]))
            if (i + 1) % 50 == 0 or i + 1 == len(dataset):
                print(f"Verified {i + 1}/{len(dataset)} samples", flush=True)
    cpu = np.asarray(cpu_predictions, dtype=np.float32)
    predicted = np.asarray(onnx_predictions, dtype=np.float32)
    report["samples_verified"] = len(dataset)
    report["pytorch_vs_onnx"] = difference(cpu, predicted, args.atol, args.rtol)
    report["pytorch_metrics"] = regression_metrics(target, cpu)
    report["onnx_metrics"] = regression_metrics(target, predicted)
    arrays = {"target": target, "pytorch_cpu": cpu, "mean": predicted}

    options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    optimized = ort.InferenceSession(str(model_path), sess_options=options, providers=["CPUExecutionProvider"])
    optimized_predictions = np.asarray([
        optimized.run(["rul"], {"features": samples[i:i + 1].contiguous().numpy()})[0].item()
        for i in range(len(dataset))
    ], dtype=np.float32)
    report["pytorch_vs_onnx_optimized"] = difference(cpu, optimized_predictions, args.atol, args.rtol)
    arrays["onnx_optimized"] = optimized_predictions

    if args.reference_predictions is not None:
        with np.load(args.reference_predictions, allow_pickle=False) as reference:
            if not np.array_equal(reference["target"].reshape(-1), target):
                raise ValueError("Saved reference targets do not match verification data")
            original = reference["mean"].reshape(-1)
        report["saved_reference_vs_pytorch_cpu"] = difference(original, cpu, args.atol, args.rtol)
        arrays["saved_reference"] = original
    np.savez_compressed(args.output / "predictions.npz", **arrays)
    with (args.output / "predictions.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["sample_index", "target", "pytorch_cpu", "onnx", "onnx_minus_pytorch"])
        for i, (truth, ref, actual) in enumerate(zip(target, cpu, predicted)):
            writer.writerow([i, float(truth), float(ref), float(actual), float(actual - ref)])

    for path in (preprocessing, normalization):
        shutil.copyfile(path, args.output / path.name)
    save_json(args.output / "training_config.json", config)
    save_json(args.output / "input_contract.json", {
        "version": "motor_stable23_v1", "input_name": "features", "input_shape": [1, 23, 110],
        "input_dtype": "little_endian_float32", "input_bytes": 10120,
        "source_sample_shape": [10, 11, 23],
        "layout_conversion": "reshape [10,11,23] to [110,23], then transpose to [23,110]",
        "normalization_inside_model": False, "normalization_file": "normalization.npz",
        "already_normalized_inputs_must_not_be_normalized_again": True,
        "output_name": "rul", "output_shape": [1, 1], "output_dtype": "float32",
        "rul_unit": "observed_aging_test_session", "labels_are_not_inputs": True,
        "preprocessing_config": "preprocessing.json", "quantized": False,
        "f3p_runtime_verified": False,
    })
    report["protected_sources_unchanged"] = all(sha256(path) == hashes_before[str(path)] for path in protected)
    report["protected_source_sha256"] = hashes_before
    if not report["protected_sources_unchanged"]:
        raise RuntimeError("A protected source, checkpoint, or preprocessing file changed")
    if not report["pytorch_vs_onnx"]["allclose"] or not report["pytorch_vs_onnx_optimized"]["allclose"]:
        raise RuntimeError("ONNX verification exceeded the selected numerical tolerance")
    if args.reference_predictions is not None and not report["saved_reference_vs_pytorch_cpu"]["allclose"]:
        raise RuntimeError("CPU predictions differ from the saved reference beyond tolerance")
    report["status"] = "passed"
    report["f3p_runtime_verified"] = False
    report["quantized"] = False
    (args.output / "README.md").write_text(
        "# FKP-DVGT FP32 ONNX\n\n"
        "rul_fp32.onnx contains the existing trained model with fixed float32 input [1,23,110] "
        "and output [1,1]. Dropout and stochastic graph sampling are disabled. "
        "Normalization is external; already normalized replay inputs must not be normalized again.\n\n"
        f"Verified on {len(dataset)} real windows with PyTorch CPU and ONNX Runtime CPU "
        "(graph optimization both disabled and enabled). Exact results are in export_report.json.\n\n"
        "RUL is measured in observed aging-test sessions, not hours. "
        "This export preserves the trained model's existing prediction errors.\n\n"
        "F3P Arm NN parsing, Alnpu execution, quantization and board performance have not been validated. "
        "node_inventory.csv lists the operators requiring compatibility checks.\n",
        encoding="utf-8",
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--example-data", type=Path, required=True, help="Training data for the example input")
    parser.add_argument("--data", type=Path, required=True, help="Dataset to verify, one window per inference")
    parser.add_argument("--preprocessing", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="A new directory; existing results are never overwritten")
    parser.add_argument("--reference-predictions", type=Path)
    parser.add_argument("--opset", type=int, default=13)
    parser.add_argument("--atol", type=float, default=1e-4)
    parser.add_argument("--rtol", type=float, default=1e-4)
    args = parser.parse_args()
    if args.atol < 0 or args.rtol < 0:
        parser.error("Tolerances must be nonnegative")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    report = {"status": "running"}
    try:
        export_and_check(args, report)
    except Exception:
        report["status"] = "failed"
        report["error"] = traceback.format_exc()
        raise
    finally:
        save_json(args.output / "export_report.json", report)
    save_json(args.output / "artifact_manifest.json", {
        path.name: sha256(path) for path in sorted(args.output.iterdir()) if path.is_file()
    })
    print(json.dumps({"status": report["status"], "output": str(args.output),
                      "max_absolute_error": report["pytorch_vs_onnx"]["max_absolute_error"],
                      "onnx_metrics": report["onnx_metrics"]}, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
