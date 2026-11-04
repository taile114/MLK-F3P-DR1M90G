"""Prepare an application bundle from frozen, hash-checked model artifacts."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tarfile

REPO = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
PLAN = "qparams_ff6.tsv"
FILES = ["weights.bin", "weights.tsv", PLAN, "input_f32.bin", "libmlk_rul.so"]


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f"Frozen source anchor changed: {old[:80]}")
    return source.replace(old, new, 1)


def application_sources(offload):
    """Keep frozen forward kernels; remove CLI, captures and profiling paths."""
    graph = member(offload, "src/native_rul_offload.cpp").decode()
    graph = graph.split("void dumpValue(")[0]
    graph = replace_once(graph, '#include "allocation_probe.hpp"\n', '')
    begin = graph.index("    const auto begin=profileOperations?")
    end = graph.index("\n}", begin)
    graph = graph[:begin] + (
        "    for(std::size_t i=0;i<size(x);++i)"
        "y[i]=(x.data[i]*(1.0f+std::erf(x.data[i]/1.4142135623730951f)))*0.5f;"
    ) + graph[end:] + "} } // namespace fused, anonymous\n"

    native = member(offload, "src/native_rul_cma.cpp").decode().split("struct Value { Layer*")[0]
    begin, end = native.index("void readQuantization("), native.index("struct CmaSession {")
    native = native[:begin] + native[end:]
    begin, end = native.index("struct AllocationStats {"), native.index("using Shape =")
    native = native[:begin] + native[end:]
    native = native.replace("using Layer = armnn::IConnectableLayer;\n", "")
    begin, end = native.index("armnn::TensorInfo info("), native.index("std::vector<float> readFloats(")
    native = native[:begin] + native[end:] + "struct View { const float* data; std::size_t size; };\n} // namespace\n"

    ops = member(offload, "src/offload_ops.hpp").decode()
    begin, end = ops.index("bool profileOperations="), ops.index("struct Graph {")
    ops = ops[:begin] + ops[end:]
    begin, end = ops.index("    std::vector<OperationMeasurement>"), ops.index("    const Weights& weights;")
    ops = ops[:begin] + (
        "    template<class F> void append(const char*,const std::string&,unsigned,unsigned,unsigned,F operation) {\n"
        "        operations.emplace_back(std::move(operation));\n    }\n"
    ) + ops[end:]
    ops = "\n".join(line for line in ops.split("\n") if "capturedFcs.push_back" not in line)
    begin, end = ops.index("        if(profileOperations)"), ops.index("        return outputs;", ops.index("        if(profileOperations)"))
    ops = ops[:begin] + "        for(auto& execute:operations)execute();\n" + ops[end:]

    fc = member(offload, "src/offload_quantized_fc.hpp").decode()
    fc = replace_once(fc, "    std::ofstream auditInput,auditOutput;\n", "")
    fc = replace_once(fc, 'std::ofstream(result+"/"+label+".dot")<<dot.str();', '(void)result;')
    fc = replace_once(fc, "options.SetProfilingEnabled(true)", "options.SetProfilingEnabled(false)")
    begin, end = fc.index('        const char* verify='), fc.index("    int expected(")
    fc = fc[:begin] + "    }\n" + fc[end:]
    begin, end = fc.index("    unsigned audit("), fc.index("    ~QuantizedFc()")
    fc = fc[:begin] + fc[end:]
    return {"engine_graph.hpp": graph, "native_rul_cma.cpp": native,
            "offload_ops.hpp": ops, "offload_quantized_fc.hpp": fc}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def archive(name):
    folder = REPO / "npu/artifacts"
    checks = dict((line.split()[1].lstrip("*"), line.split()[0])
                  for line in (folder / "checksums.sha256").read_text().splitlines() if line.strip())
    data = (folder / name).read_bytes()
    if checks.get(name) != digest(data):
        raise ValueError(f"Frozen archive checksum mismatch: {name}")
    return tarfile.open(folder / name)


def member(tar, suffix):
    entries = [m for m in tar.getmembers() if m.isfile() and m.name.endswith("/" + suffix)]
    if len(entries) != 1:
        raise ValueError(f"Expected one archive entry: {suffix}")
    return tar.extractfile(entries[0]).read()


def seal(root):
    manifest = json.loads((root / "model.json").read_text())
    if (manifest.get("execution_plan"), manifest.get("quantization_plan"), manifest.get("npu_branches")) != ("ff6", PLAN, 6):
        raise ValueError("Expected the FF6 execution contract")
    names = FILES
    manifest["sha256"] = {name: digest((root / name).read_bytes()) for name in names}
    elf = (root / "libmlk_rul.so").read_bytes()
    if elf[:6] != b"\x7fELF\x02\x01" or int.from_bytes(elf[16:18], "little") != 3 or int.from_bytes(elf[18:20], "little") != 183:
        raise ValueError("Engine must be a little-endian AArch64 ELF shared library")
    (root / "model.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    (root / "checksums.sha256").write_text("".join(
        f"{digest((root / name).read_bytes())}  {name}\n" for name in names + ["model.json"]), encoding="ascii", newline="\n")


def prepare(root, split):
    root.mkdir(parents=True, exist_ok=False)
    (root / "src").mkdir()
    with archive("f3p-native-rul-arm64-v1.tar.gz") as native, archive("f3p-offload-rul-arm64-v1.tar.gz") as offload:
        for name in ["weights.bin", "weights.tsv"]:
            (root / name).write_bytes(member(native, name))
        (root / "src/model_config.hpp").write_bytes(member(offload, "src/model_config.hpp"))
        plan = member(offload, PLAN)
        (root / PLAN).write_bytes(plan)
        rows = [line.split() for line in plan.decode().splitlines() if line.strip()]
        if len(rows) != 6 or any(len(row) != 6 for row in rows):
            raise ValueError("Invalid frozen FF6 plan")
        header = 'namespace {\nstruct FrozenBranch { const char *name; Quantization q; };\nstatic const FrozenBranch ff6Plan[] = {\n'
        for name, inputs, outputs, sx, sw, sy in rows:
            header += f'    {{"{name}", {{{inputs}, {outputs}, {sx}f, {sw}f, {sy}f}}}},\n'
        (root / "src/ff6_plan.hpp").write_text(header + '};\n}\n', encoding="utf-8", newline="\n")
        data = offload if split == "val184" else native
        for name in ["input_f32.bin", "reference_f32.bin"]:
            (root / name).write_bytes(member(data, "data/" + name))
        for name, source in application_sources(offload).items():
            (root / "src" / name).write_text(source, encoding="utf-8", newline="\n")
    for name in ["engine.cpp", "check_engine.cpp"]:
        shutil.copyfile(HERE / name, root / "src" / name)
    shutil.copyfile(REPO / "host/common/rul_engine_api.h", root / "src/rul_engine_api.h")
    shutil.copyfile(HERE / "build.sh", root / "build.sh")
    count = 184 if split == "val184" else 165
    if (root / "input_f32.bin").stat().st_size != count * 2530 * 4:
        raise ValueError("Wrong replay input extent")
    manifest = {"schema": 1, "engine_abi": 1, "model_id": "motor_stable23_v1", "dataset_id": split,
                "feature_version": "motor_stable23_v1", "shape": [1, 23, 110],
                "dtype": "little_endian_float32", "normalized": True, "windows": count,
                "rul_unit": "observed_aging_test_session", "execution_plan": "ff6",
                "quantization_plan": PLAN, "npu_branches": 6,
                "source_archive_sha256": digest((REPO / "npu/artifacts/f3p-offload-rul-arm64-v1.tar.gz").read_bytes()),
                "sha256": {}}
    (root / "model.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--split", choices=["val184", "test165"], default="val184")
    parser.add_argument("--seal", action="store_true", help="Hash an already compiled bundle")
    args = parser.parse_args()
    if args.seal:
        seal(args.output)
    else:
        prepare(args.output, args.split)
    print(args.output.resolve())
