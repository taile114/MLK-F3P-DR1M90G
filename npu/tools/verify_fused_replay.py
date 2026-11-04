"""Independently verify frozen fusion results, captured integer IO and paired timings."""
from __future__ import annotations

import csv
import hashlib
import json
from pathlib import Path
import re
import tarfile

import numpy as np

from verify_board_linear import execution_events, interrupts, require

ROOT = Path(__file__).resolve().parents[1]
LAYERS = ["output_layer.0.spline_weight", "output_layer.2.spline_weight"]


def read_csv(path, delimiter=","):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream, delimiter=delimiter))


def integer_audit(directory, count, frozen_weights):
    records = read_csv(directory / "integer_audit.csv")
    offset = 0
    maximum = 0
    for label in LAYERS:
        meta = (directory / (label + "_meta.txt")).read_text().split()
        rows, logical_i, logical_o, ic, oc = map(int, meta[:5])
        sx, sw, sy = map(float, meta[5:])
        calibration=json.loads((ROOT/"results/calibration/rul-uint8-v1/calibration.json").read_text())["layers"][label]
        require([np.float32(x) for x in [sx,sw,sy]]==[np.float32(calibration[k]) for k in ["input_scale","weight_scale","output_scale"]],"Frozen quantization scales differ")
        require(rows == 1 and ic == (logical_i + 7)//8*8 and oc == (logical_o + 7)//8*8, "Integer dimensions differ")
        x = np.fromfile(directory / (label + "_input_u8.bin"), np.uint8).reshape(count, rows, ic)
        w = np.fromfile(directory / (label + "_weights_u8.bin"), np.uint8).reshape(oc, ic)
        y = np.fromfile(directory / (label + "_output_u8.bin"), np.uint8).reshape(count, rows, oc)
        source = frozen_weights[label]
        require(source.shape == (logical_o, logical_i), "Frozen weight dimensions differ")
        divided = source / np.float32(sw)
        rounded = np.sign(divided)*np.floor(np.abs(divided)+np.float32(.5))
        expected_weights = np.full((oc, ic), 128, np.uint8)
        expected_weights[:logical_o, :logical_i] = np.clip(rounded+128, 0, 255).astype(np.uint8)
        require(np.array_equal(w, expected_weights), "Captured weights differ from checkpoint-derived weights")
        require(np.all(x[:, :, logical_i:] == 128), "Input padding is not real zero")
        acc = (x.astype(np.int64)-128) @ (w.astype(np.int64)-128).T
        scaled = acc.astype(np.float64)*(sx*sw/sy)
        rounded = np.sign(scaled)*np.floor(np.abs(scaled)+.5)
        expected = np.clip(rounded+128, 0, 255).astype(np.uint8)
        errors = np.abs(y.astype(np.int16)-expected.astype(np.int16))
        require(errors.max() <= 1, "NPU integer kernel exceeds 1 LSB")
        maximum = max(maximum, int(errors.max()))
        selected = [r for r in records if r["layer"] == label]
        require(len(selected) == count*rows*oc, "Integer CSV scope differs")
        for j, record in enumerate(selected):
            sample, position = divmod(j, rows*oc)
            row, channel = divmod(position, oc)
            require([int(record[k]) for k in ["sample", "row", "channel", "actual_u8", "expected_u8", "error_lsb"]]
                    == [sample, row, channel, int(y[sample,row,channel]), int(expected[sample,row,channel]), int(errors[sample,row,channel])],
                    "Integer CSV differs from independently recomputed captured bytes")
        offset += len(selected)
    require(offset == len(records), "Unexpected integer audit records")
    return {"checked_outputs": offset, "max_error_lsb": maximum, "independent_int64_oracle": True}


def frozen_weights():
    with tarfile.open(ROOT / "artifacts/f3p-native-rul-arm64-v1.tar.gz") as archive:
        raw = np.frombuffer(archive.extractfile("f3p-native-rul-v1/weights.bin").read(), "<f4")
        table = archive.extractfile("f3p-native-rul-v1/weights.tsv").read().decode("ascii")
    result = {}
    for line in table.splitlines():
        parts = line.split(); rank = int(parts[1])
        shape = tuple(map(int, parts[2:2+rank])); count, offset = map(int, parts[2+rank:])
        if parts[0] in LAYERS: result[parts[0]] = raw[offset:offset+count].reshape(shape[0], -1)
    require(set(result) == set(LAYERS), "Missing frozen weight rows")
    return result


def verify(root):
    inv = json.loads((ROOT / "results/board/fused-replay-v1-files-sha256.json").read_text())
    actual = {p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()}
    require(actual == {e["path"] for e in inv["files"]}, "Raw file inventory differs")
    for e in inv["files"]:
        p=root/e["path"]
        require(p.stat().st_size == e["bytes"] and hashlib.sha256(p.read_bytes()).hexdigest() == e["sha256"], "Raw file hash differs")
    with np.load(ROOT / "results/calibration/rul-uint8-v1/head-spline-validation.npz",allow_pickle=False) as data:
        reference, labels = data["baseline"], data["labels"]
    weights=frozen_weights()
    with tarfile.open(ROOT/"artifacts/f3p-fused-rul-arm64-v1.tar.gz") as archive:
        programs={name:hashlib.sha256(archive.extractfile("f3p-fused-rul-v1/bin/"+name).read()).hexdigest() for name in ["rul_fused","rul_reference_bench"]}
    groups={}
    full_predictions={}
    for directory in sorted((root/"fused-v3").glob("results_*")):
        mode=directory.name.split("_")[1]
        if "cpu_quantized" in directory.name: mode="cpu_quantized"
        require(int((directory/"exit_code.txt").read_text())==0,"Experiment returned nonzero")
        log=(directory/"probe.log").read_text()
        require((directory/"runtime.sha256").read_text().split()[0]=="5def7ba75e4b59644be6f58deaf8b2bb3791c62ed38b52b6aea08b11a8f728ce","Paired runtime differs")
        for line in (directory/"programs.sha256").read_text().splitlines():
            digest,name=line.split();require(digest==programs[Path(name).name],"Physical binary differs from frozen artifact")
        records=read_csv(directory/"predictions.csv"); count=len(records)
        require(count in {1,10,184} and [int(r["sample_index"]) for r in records]==list(range(count)),"Replay scope differs")
        p=np.array([float(r["prediction"]) for r in records],np.float32)
        refs=np.array([float(r["reference"]) for r in records],np.float32); target=np.array([float(r["target"]) for r in records],np.float32)
        require(np.array_equal(refs,reference[:count]) and np.array_equal(target,labels[:count]),"Validation provenance differs")
        delta=p.astype(np.float64)-refs; error=p.astype(np.float64)-target
        require(np.isfinite(p).all(),"Nonfinite prediction")
        if count==184:
            if mode in full_predictions:require(np.array_equal(p,full_predictions[mode]),"Repeated-round predictions differ")
            else:full_predictions[mode]=p.copy()
        if mode in {"reference","neon","scalar"}: require(np.all(np.abs(delta)<=1e-4+1e-4*np.abs(refs)),"FP32 tolerance exceeded")
        times=np.array([float(r["forward_us"]) for r in records]); require(np.all(times>0),"Invalid timing")
        result={"mode":mode,"samples":count,"rmse":float(np.sqrt(np.mean(error**2))),"mae":float(np.abs(error).mean()),
                "fp32_max_abs":float(np.abs(delta).max()),"mean_forward_us":float(times.mean()),"p50_forward_us":float(np.median(times)),"p95_forward_us":float(np.percentile(times,95))}
        warmup=5
        require("warmup=5" in log,"Warmup scope differs")
        if mode!="reference":
            calls=[int(r["allocation_calls"]) for r in records]; cma=[int(r["cma_bytes"]) for r in records]
            require(len(set(cma))==1,"CMA grows during fusion replay")
            if mode!="hybrid": require(sum(calls)==0 and cma[0]==0,"Fused CPU forward allocates")
            result.update({"timed_cpp_new_calls":sum(calls),"cma_bytes":cma[0]})
            plan=read_csv(directory/"plan.tsv",delimiter="\t")
            require(len(plan)==6 and sum(int(r["stems"]) for r in plan)==30,"Shared KAN stem scope differs")
            result["fused_plan_steps"]=sum(int(r["ops"]) for r in plan)
            result["tensor_workspace_bytes"]=sum(int(r["tensor_workspace_bytes"]) for r in plan)
        else:
            result["timed_cpp_new_calls"]=int(re.search(r"timed_allocation_calls=(\d+)",log)[1])
        before,after=[interrupts(directory/f"interrupts_{tag}.txt") for tag in ["before","after"]]
        irq={k:after[k]-before[k] for k in before}
        require(irq["SOFT_NPU_IRQ"]==0 and irq["HARD_NPU_IRQ"]==(3*(count+warmup) if mode=="hybrid" else 0),"Warmup/forward IRQ scope differs")
        result["interrupt_delta_including_warmup"]=irq
        if mode=="hybrid":
            events=0
            for label in LAYERS:
                dot=(directory/(label+".dot")).read_text()
                require(dot.count("LayerType : ALHardNPU")==1 and "BackendID : Alnpu" in dot,"Expected NPU-only FC graph")
                profile=json.loads((directory/(label+"_profile.json")).read_text())
                events+=sum(len(t) for name,t in execution_events(profile) if "AlnpuALHardNPU" in name)
            require(events==2,"First measured window NPU execution scope differs")
            if (directory/"integer_audit.csv").exists():result["integer_audit"]=integer_audit(directory,count,weights)
        result["application_accuracy_acceptance"]="TBD"
        groups[directory.name]=result
    bench=read_csv(root/"fused-v3/benchmark.tsv",delimiter="\t")
    require(len(bench)==12,"Need three paired rounds for four modes")
    paired=[]
    for round in [1,2,3]:
        trial={r["mode"]:groups[Path(r["directory"]).name] for r in bench if int(r["round"])==round}
        require(set(trial)=={"reference","neon","cpu_quantized","hybrid"},"Benchmark round differs")
        require(all(v["samples"]==184 for v in trial.values()),"Benchmark count differs")
        paired.append({"round":round,"mean_us":{k:v["mean_forward_us"] for k,v in trial.items()},
                       "reference_to_fused_cpu_ratio":trial["reference"]["mean_forward_us"]/trial["neon"]["mean_forward_us"],
                       "reference_to_fused_hybrid_ratio":trial["reference"]["mean_forward_us"]/trial["hybrid"]["mean_forward_us"]})
    require(sorted(v["samples"] for v in groups.values() if "integer_audit" in v)==[1,10,184],"Missing bounded hardware validation")
    stem=root/"fused-v2"
    stem_dirs=list(stem.glob("results_stem_*"));require(len(stem_dirs)==1,"Missing physical spline boundary control")
    stem_max=0.
    with tarfile.open(ROOT/"artifacts/f3p-fused-rul-arm64-v1.tar.gz") as archive:
        for name in ["base","spline"]:
            expected=np.frombuffer(archive.extractfile("f3p-fused-rul-v1/fixtures/"+name+".bin").read(),"<f4")
            actual=np.fromfile(stem_dirs[0]/(name+"_actual.bin"),"<f4")
            require(actual.shape==expected.shape and np.all(np.abs(actual-expected)<=1e-6+1e-6*np.abs(expected)),"Spline boundary mismatch")
            stem_max=max(stem_max,float(np.abs(actual-expected).max()))
    return {"status":"fusion_and_paired_replays_verified","raw_files_verified":len(inv["files"]),
            "runs":groups,"paired_benchmark":paired,"spline_boundary_max_abs":stem_max,
            "weights_and_quantization_frozen":True,"training_executed":False,"new_test_set_inference_executed":False,
            "NPU_branches":LAYERS,"CXX_new_instrumentation_is_not_a_complete_malloc_audit":True,
            "clock_and_temperature_controls_available":False,"application_accuracy_acceptance":"TBD"}


if __name__=="__main__":
    import argparse
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--root",type=Path,default=ROOT/"results/board/recovered/fused-replay-v1")
    print(json.dumps(verify(parser.parse_args().root),ensure_ascii=False,indent=2))
