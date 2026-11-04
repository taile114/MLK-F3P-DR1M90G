"""Reduce a verified ONNX graph, recheck real predictions, and inspect vendor parser evidence."""
from __future__ import annotations

import argparse
from collections import Counter
import csv
import json
from pathlib import Path
import re
import shutil
import struct
import tarfile
import traceback

from export_onnx import difference, save_json, sha256, tensor_shape
import numpy as np
import onnx
import onnxruntime as ort
import torch
from fkp_dvgt.data import load_dataset
from fkp_dvgt.metrics import regression_metrics


def parser_evidence(model: onnx.ModelProto, archive: Path) -> dict:
    prefix = "app/face_detection/libs/armnn_lib/"
    parser_member = prefix + "lib/libarmnnOnnxParser.so.24.6"
    with tarfile.open(archive, "r:gz") as package:
        binary = package.extractfile(parser_member).read()
        version = package.extractfile(prefix + "include/armnnOnnxParser/Version.hpp").read().decode("utf-8")
        main = package.extractfile("app/face_detection/src/npu_flow/yolo_demo_hdmi_camera/main.cpp").read().decode("utf-8")
        executor = package.extractfile("app/face_detection/src/npu_flow/utils/executor.cpp").read().decode("utf-8")
    strings = {item.decode("ascii") for item in re.findall(rb"[ -~]{2,}", binary)}
    operators = Counter(node.op_type for node in model.graph.node)
    evidence = []
    for operator, count in sorted(operators.items()):
        exact = operator in strings
        symbol = ("Parse" + operator).encode("ascii") in binary
        evidence.append({"operator": operator, "count": count, "exact_string_present": exact,
                         "parse_symbol_fragment_present": symbol,
                         "status": "presence_only_not_support_proof" if exact or symbol else "not_found_requires_runtime_check"})
    return {
        "method": "static binary string/symbol-fragment inspection; no parser or NPU execution",
        "limitations": "Presence does not prove support. Absence does not conclusively prove lack of support. "
                      "Type, rank, attributes, variable MatMul inputs and backend assignment require actual runtime checks.",
        "archive": str(archive.resolve()), "archive_sha256": sha256(archive),
        "parser_member": parser_member,
        "parser_sha256": __import__("hashlib").sha256(binary).hexdigest(),
        "elf_machine": struct.unpack_from("<H", binary, 18)[0],
        "elf_machine_description": "AArch64" if struct.unpack_from("<H", binary, 18)[0] == 183 else "unknown",
        "parser_version_components": re.findall(r"#define ONNX_PARSER_(?:MAJOR|MINOR|PATCH)_VERSION (\d+)", version),
        "onnx_loader_confirmed_in_source": "CreateNetworkFromBinaryFile" in executor,
        "default_backends_confirmed_in_source": "Alnpu,CpuAcc,CpuRef" in main,
        "operators": evidence,
        "operators_without_literal_or_symbol": [item["operator"] for item in evidence
                                                if item["status"] == "not_found_requires_runtime_check"],
        "f3p_runtime_verified": False, "alnpu_backend_assignment_verified": False,
    }


def run(args, report: dict) -> None:
    source_dir = args.export_dir.resolve()
    source = source_dir / "rul_fp32.onnx"
    source_report = json.loads((source_dir / "export_report.json").read_text(encoding="utf-8"))
    if source_report["status"] != "passed" or sha256(source) != source_report["onnx_sha256"]:
        raise ValueError("Input export must have a passed report and matching model hash")
    with np.load(source_dir / "predictions.npz", allow_pickle=False) as data:
        target = data["target"].reshape(-1)
        reference = data["pytorch_cpu"].reshape(-1)
    dataset = load_dataset(args.data)
    samples, labels = dataset.tensors
    if not np.array_equal(target, labels.numpy().reshape(-1)):
        raise ValueError("Verification labels do not match the original export")
    if sha256(args.data) != source_report["verification_data_sha256"]:
        raise ValueError("Verification inputs differ from the original export")
    protected = [Path(name) for name in source_report["protected_source_sha256"]]
    if not all(sha256(path) == source_report["protected_source_sha256"][str(path)] for path in protected):
        raise ValueError("Original model or data-preprocessing sources changed after export")
    torch.set_num_threads(1)
    output_model = args.output / "rul_fp32.onnx"
    options = ort.SessionOptions()
    options.intra_op_num_threads = 1
    options.inter_op_num_threads = 1
    options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_BASIC
    options.optimized_model_filepath = str(output_model)
    # Basic optimization is checked below to ensure it left only standard ONNX operators.
    session = ort.InferenceSession(str(source), sess_options=options, providers=["CPUExecutionProvider"])
    graph = onnx.load(str(output_model))
    if graph.functions or any(node.domain not in ("", "ai.onnx") for node in graph.graph.node):
        raise ValueError("Optimization introduced runtime-specific operators/functions")
    # ORT adds unused domain imports while serializing. Remove those imports for other parsers.
    used_domains = {node.domain for node in graph.graph.node}
    unused_imports = [item.domain for item in graph.opset_import if item.domain not in used_domains]
    kept_imports = [onnx.helper.make_opsetid(item.domain, item.version)
                    for item in graph.opset_import if item.domain in used_domains]
    del graph.opset_import[:]
    graph.opset_import.extend(kept_imports)
    onnx.checker.check_model(graph, full_check=True)
    onnx.save(graph, str(output_model))
    if graph.ir_version != source_report["onnx_ir_version"]:
        raise ValueError("Optimization unexpectedly changed the ONNX IR version")
    if len(graph.graph.input) != 1 or tensor_shape(graph.graph.input[0]) != [1, 23, 110]:
        raise ValueError("Optimization changed the fixed input contract")
    if len(graph.graph.output) != 1 or tensor_shape(graph.graph.output[0]) != [1, 1]:
        raise ValueError("Optimization changed the fixed output contract")
    counts = Counter(node.op_type for node in graph.graph.node)
    report.update(source_model=str(source), source_sha256=sha256(source),
                  optimization="ONNX Runtime ORT_ENABLE_BASIC; standard ONNX operators only",
                  onnx_ir_version=graph.ir_version,
                  opset_imports=[{"domain": item.domain, "version": item.version} for item in graph.opset_import],
                  original_node_count=source_report["node_count"], node_count=len(graph.graph.node),
                  operator_counts=dict(sorted(counts.items())), original_size_bytes=source.stat().st_size,
                  onnx_size_bytes=output_model.stat().st_size, onnx_sha256=sha256(output_model),
                  onnx_checker_passed=True, quantized=False, f3p_runtime_verified=False)
    report["unused_domain_imports_removed"] = unused_imports
    with (args.output / "node_inventory.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["index", "domain", "operator", "name"])
        writer.writerows([i, node.domain, node.op_type, node.name] for i, node in enumerate(graph.graph.node))
    # Reload the saved optimized model with further transformations disabled.
    options.optimized_model_filepath = ""
    options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
    saved_session = ort.InferenceSession(str(output_model), sess_options=options, providers=["CPUExecutionProvider"])
    predicted = []
    basic_predictions = []
    for i in range(len(dataset)):
        inputs = {"features": samples[i:i + 1].contiguous().numpy()}
        predicted.append(saved_session.run(["rul"], inputs)[0].item())
        basic_predictions.append(session.run(["rul"], inputs)[0].item())
        if (i + 1) % 50 == 0 or i + 1 == len(dataset):
            print(f"Verified optimized graph {i + 1}/{len(dataset)}", flush=True)
    predicted = np.asarray(predicted, dtype=np.float32)
    report["samples_verified"] = len(dataset)
    report["pytorch_vs_optimized_onnx"] = difference(reference, predicted, args.atol, args.rtol)
    report["pytorch_vs_basic_session"] = difference(reference, np.asarray(basic_predictions, dtype=np.float32), args.atol, args.rtol)
    report["onnx_metrics"] = regression_metrics(target, predicted)
    np.savez_compressed(args.output / "predictions.npz", target=target, mean=predicted, pytorch_cpu=reference)
    for name in ("normalization.npz", "preprocessing.json", "input_contract.json", "training_config.json"):
        shutil.copyfile(source_dir / name, args.output / name)
    if args.vendor_archive is not None:
        evidence = parser_evidence(graph, args.vendor_archive)
        save_json(args.output / "f3p_parser_static_check.json", evidence)
        report["f3p_parser_static_check"] = {
            "file": "f3p_parser_static_check.json",
            "operators_without_literal_or_symbol": evidence["operators_without_literal_or_symbol"],
            "method": evidence["method"], "conclusion": "Runtime compatibility remains unverified",
        }
    report["protected_sources_unchanged"] = all(
        sha256(path) == source_report["protected_source_sha256"][str(path)] for path in protected)
    if not report["protected_sources_unchanged"]:
        raise RuntimeError("Protected source files changed")
    if not report["pytorch_vs_optimized_onnx"]["allclose"] or not report["pytorch_vs_basic_session"]["allclose"]:
        raise RuntimeError("Optimized model verification exceeded tolerance")
    report["status"] = "passed"
    (args.output / "README.md").write_text(
        "# FKP-DVGT optimized FP32 ONNX\n\n"
        "rul_fp32.onnx is the verified fixed-input ONNX model after ORT basic graph optimization. "
        "Only standard ONNX operators remain; the source model is recorded in optimization_report.json.\n\n"
        "Input: float32 [1,23,110]. Output: float32 [1,1], measured in observed aging-test sessions. "
        "Normalization is external. The prepared replay binary is already normalized.\n\n"
        f"All {len(dataset)} verification windows were compared with the original PyTorch CPU output. "
        "The artifact remains FP32; quantization and actual F3P/Alnpu execution are not validated. "
        "f3p_parser_static_check.json contains static evidence only, not an operator-support guarantee.\n",
        encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--export-dir", type=Path, required=True)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--vendor-archive", type=Path)
    parser.add_argument("--atol", type=float, default=1e-4)
    parser.add_argument("--rtol", type=float, default=1e-4)
    args = parser.parse_args()
    if args.atol < 0 or args.rtol < 0:
        parser.error("Tolerances must be nonnegative")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    report = {"status": "running"}
    try:
        run(args, report)
    except Exception:
        report["status"] = "failed"
        report["error"] = traceback.format_exc()
        raise
    finally:
        save_json(args.output / "optimization_report.json", report)
    save_json(args.output / "artifact_manifest.json", {
        path.name: sha256(path) for path in sorted(args.output.iterdir()) if path.is_file()})
    print(json.dumps({key: report[key] for key in ("status", "node_count", "onnx_size_bytes", "onnx_metrics")}), flush=True)


if __name__ == "__main__":
    main()
