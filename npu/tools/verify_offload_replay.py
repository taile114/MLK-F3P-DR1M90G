"""Independently check extra offload evidence, captured bytes and paired replays."""
from __future__ import annotations

import argparse
from collections import defaultdict
import csv
import hashlib
import json
from pathlib import Path
import re
import tarfile

import numpy as np
from verify_board_linear import execution_events, interrupts

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = "5def7ba75e4b59644be6f58deaf8b2bb3791c62ed38b52b6aea08b11a8f728ce"
PROGRAM = "6bea234a783715ff87c991a20a8117696b118d3ae0a50675e54ce5b59aaedff6"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def rows(path, delimiter=","):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream, delimiter=delimiter))


def frozen_weights():
    with tarfile.open(ROOT / "artifacts/f3p-native-rul-arm64-v1.tar.gz") as archive:
        binary = archive.extractfile("f3p-native-rul-v1/weights.bin").read()
        table = archive.extractfile("f3p-native-rul-v1/weights.tsv").read().decode("ascii")
    require(hashlib.sha256(binary).hexdigest() == "0a61767a0b324fd032db6857424b0857dd9dff75979bee25384b928a75905f02", "Frozen weights changed")
    values = np.frombuffer(binary, "<f4")
    result = {}
    for line in table.splitlines():
        fields = line.split(); rank = int(fields[1])
        shape = tuple(map(int, fields[2:2+rank])); count, offset = map(int, fields[2+rank:])
        if fields[0].endswith(("base_weight", "spline_weight")):
            result[fields[0]] = values[offset:offset+count].reshape(shape[0], -1)
    return result


def integer_audit(directory, frozen, calibration):
    records = rows(directory / "integer_audit.csv")
    selected = defaultdict(list)
    for record in records:
        selected[record["layer"]].append(record)
    checked = 0; maximum = 0; details = []
    for meta in sorted(directory.glob("*_meta.txt")):
        label = meta.name.removesuffix("_meta.txt")
        fields = meta.read_text().split()
        batch, li, lo, ic, oc = map(int, fields[:5]); sx, sw, sy = map(float, fields[5:8]); layout = fields[8]
        require(layout in ["NCHW", "rows_features"], "Unknown captured layout")
        require(ic == (li+7)//8*8 and oc == (lo+7)//8*8, "Padding differs")
        cp = calibration[label]
        require([np.float32(s) for s in [sx,sw,sy]] == [np.float32(cp[k]) for k in ["input_scale","weight_scale","output_scale"]], "Train calibration changed")
        w = np.fromfile(directory / (label+"_weights_u8.bin"), np.uint8).reshape(oc, ic)
        expected_weights = np.full((oc,ic), 128, np.uint8)
        divided = frozen[label] / np.float32(sw)
        expected_weights[:lo,:li] = np.clip(np.sign(divided)*np.floor(np.abs(divided)+np.float32(.5))+128,0,255).astype(np.uint8)
        require(np.array_equal(w,expected_weights), "Quantized checkpoint weights differ")
        x = np.fromfile(directory / (label+"_input_u8.bin"),np.uint8)
        y = np.fromfile(directory / (label+"_output_u8.bin"),np.uint8)
        require(len(x) % (batch*ic) == 0 and len(x)>0, "Incomplete captured inputs")
        count = len(x)//(batch*ic)
        require(len(y) == count*batch*oc, "Incomplete captured outputs")
        if layout == "NCHW":
            x=x.reshape(count,ic,batch).transpose(0,2,1);y=y.reshape(count,oc,batch).transpose(0,2,1)
        else:
            x=x.reshape(count,batch,ic);y=y.reshape(count,batch,oc)
        require(np.all(x[:,:,li:] == 128), "Input padding is not real zero")
        acc=(x.astype(np.int64)-128) @ (w.astype(np.int64)-128).T
        scaled=acc.astype(np.float64)*(sx*sw/sy)
        expected=np.clip(np.sign(scaled)*np.floor(np.abs(scaled)+.5)+128,0,255).astype(np.uint8)
        error=np.abs(y.astype(np.int16)-expected.astype(np.int16))
        require(error.max() <= 1, "NPU arithmetic exceeds 1 LSB")
        require(len(selected[label]) == y.size, "Integer CSV length differs")
        for n, record in enumerate(selected[label]):
            sample, position=divmod(n,batch*oc);r,o=divmod(position,oc)
            require([int(record[k]) for k in ["sample","row","channel","actual_u8","expected_u8","error_lsb"]] ==
                    [sample,r,o,int(y[sample,r,o]),int(expected[sample,r,o]),int(error[sample,r,o])], "Integer CSV differs from bytes")
        dot=(directory/(label+".dot")).read_text()
        require("LayerType : ALHardNPU" in dot and "BackendID : Alnpu" in dot, "NPU graph missing")
        computes=[]
        for line in dot.splitlines():
            kind=re.search(r"LayerType : (\w+)",line)
            backend=re.search(r"BackendID : (\w+)",line)
            if kind and kind[1] not in {"Input","Output","Constant","MemCopy","MemImport","MemSync"}:
                require(kind[1]=="ALHardNPU" and backend and backend[1]=="Alnpu","Unexpected compute backend")
                computes.append(kind[1])
        require(len(computes)==1,"Expected one isolated hardware compute")
        events=execution_events(json.loads((directory/(label+"_profile.json")).read_text()))
        require(any("ALHardNPU" in key for key,_ in events),"NPU execution event missing")
        checked += y.size;maximum=max(maximum,int(error.max()))
        details.append({"layer":label,"samples":count,"rows":batch,"layout":layout,"outputs":int(y.size),"max_error_lsb":int(error.max())})
    require(details and checked == len(records), "Unexpected integer audit scope")
    return {"outputs":checked,"max_error_lsb":maximum,"layers":details}


def verify(directory):
    inventory=json.loads((ROOT/"results/board/offload-replay-v1-files-sha256.json").read_text())
    for item in inventory["files"]:
        path=directory/item["path"]
        require(path.is_file() and path.stat().st_size==item["bytes"] and hashlib.sha256(path.read_bytes()).hexdigest()==item["sha256"],
                "Raw evidence hash differs: "+item["path"])
    board_hashes={Path(line.split()[1]).name:line.split()[0]
                  for line in (directory/"board_inputs_programs.sha256").read_text().splitlines()}
    require(board_hashes["rul_offload"]==PROGRAM and board_hashes["libarmnn.so.32.1"]==RUNTIME,
            "Board executable/runtime differs")
    require(board_hashes["npu_op_support"]=="a7ef9e6955dc6750901af438a2b2dd1361a1bbfeb0d5e6f81524213b1b7c2a5a",
            "Support query executable differs")
    with tarfile.open(ROOT/"artifacts/f3p-native-rul-arm64-v1.tar.gz") as archive:
        for name in ["weights.bin","weights.tsv"]:
            require(board_hashes[name]==hashlib.sha256(archive.extractfile("f3p-native-rul-v1/"+name).read()).hexdigest(),"Board weights differ")
    calibration=json.loads((ROOT/"results/calibration/rul-uint8-v1/calibration.json").read_text())["layers"]
    frozen=frozen_weights()
    with tarfile.open(ROOT/"artifacts/f3p-fused-rul-arm64-v1.tar.gz") as archive:
        refs=np.frombuffer(archive.extractfile("f3p-fused-rul-v1/data/reference_f32.bin").read(),"<f4")
        labels=np.frombuffer(archive.extractfile("f3p-fused-rul-v1/data/labels_f32.bin").read(),"<f4")
        for name in ["input_f32.bin","reference_f32.bin","labels_f32.bin"]:
            require(board_hashes[name]==hashlib.sha256(archive.extractfile("f3p-fused-rul-v1/data/"+name).read()).hexdigest(),"Board validation inputs differ")
    audits=[]; all_metrics={}; irq_checks=[]
    for child in sorted(directory.iterdir()):
        if not child.is_dir():continue
        if (child/"integer_audit.csv").is_file():
            audits.append({"directory":child.name,**integer_audit(child,frozen,calibration)})
        if (child/"predictions.csv").is_file():
            require((child/"exit_code.txt").read_text().strip()=="0", "Replay failed")
            data=rows(child/"predictions.csv")
            require([int(r["sample_index"]) for r in data] == list(range(len(data))), "Incomplete sample sequence")
            prediction=np.array([float(r["prediction"]) for r in data]);reference=np.array([float(r["reference"]) for r in data]);target=np.array([float(r["target"]) for r in data])
            times=np.array([float(r["forward_us"]) for r in data]);cma={int(r["cma_bytes"]) for r in data}
            require(np.array_equal(reference.astype('<f4'),refs[:len(data)]) and np.array_equal(target.astype('<f4'),labels[:len(data)]),"Reference/label data changed")
            require(np.isfinite(prediction).all() and (times>0).all(), "Nonfinite result")
            require(len(cma)==1, "CMA changed during replay")
            log=(child/"probe.log").read_text()
            require(f"samples={len(data)} " in log and "warmup=5 " in log,"Replay log differs")
            if "mode=neon " in log:
                require(np.all(np.abs(prediction-reference)<=1e-4+1e-4*np.abs(reference)),"FP32 tolerance exceeded")
            if (child/"program.sha256").exists():
                require((child/"program.sha256").read_text().split()[0]==PROGRAM,"Unexpected executable")
                require((child/"runtime.sha256").read_text().split()[0]==RUNTIME,"Unexpected runtime")
            all_metrics[child.name]={"samples":len(data),"rmse":float(np.sqrt(np.mean((prediction-target)**2))),
                "mae":float(np.mean(np.abs(prediction-target))),"max_fp32_delta":float(np.max(np.abs(prediction-reference))),
                "mean_forward_us":float(times.mean()),"p50_forward_us":float(np.median(times)),"p95_forward_us":float(np.percentile(times,95)),
                "cma_bytes":next(iter(cma)),"prediction_sha256":hashlib.sha256(prediction.astype('<f4').tobytes()).hexdigest()}
        if (child/"interrupts_before.txt").is_file():
            before=interrupts(child/"interrupts_before.txt");after=interrupts(child/"interrupts_after.txt")
            delta={key:after[key]-before[key] for key in before}
            require(delta["SOFT_NPU_IRQ"]==0,"Unexpected software NPU interrupt")
            log=(child/"probe.log").read_text() if (child/"probe.log").exists() else ""
            if child.name.startswith("results_support_") or "mode=neon " in log:
                require(delta["HARD_NPU_IRQ"]==0,"CPU/support query executed NPU")
            else:
                expected=0
                for meta in child.glob("*_meta.txt"):
                    fields=meta.read_text().split();batch=int(fields[0]);layout=fields[8]
                    # The frozen large head FC uses two hardware transactions;
                    # previous fused head evidence records three IRQs for two heads.
                    factor=1 if layout=="NCHW" else (2 if meta.name=="output_layer.0.spline_weight_meta.txt" else batch)
                    count=all_metrics.get(child.name,{}).get("samples")
                    expected+=factor*(count+5 if count is not None else 26)
                # Benchmarks disable integer files, so infer rows/layout from its frozen plan.
                if expected==0 and child.name in all_metrics and "mode=hybrid " in log:
                    table=(child/"quantization_plan.tsv").read_text().splitlines() if (child/"quantization_plan.tsv").exists() else []
                    conv=set((child/"convolution_plan.txt").read_text().split()) if (child/"convolution_plan.txt").exists() else set()
                    for entry in table:
                        name=entry.split()[0]
                        factor=1 if name in conv else 2 if name=="output_layer.0.spline_weight" else 1 if name.startswith("output_layer.") else 11 if name.startswith("tvgtformer_") else 12
                        expected+=factor*(all_metrics[child.name]["samples"]+5)
                if expected:
                    require(delta["HARD_NPU_IRQ"]==expected,f"NPU interrupt scope differs: {child.name}: {delta['HARD_NPU_IRQ']} vs {expected}")
                else:
                    require(delta["HARD_NPU_IRQ"]>0,"Hardware NPU did not interrupt")
            irq_checks.append({"directory":child.name,**delta})
    support_dirs=list(directory.glob("results_support_*"))
    require(len(support_dirs)==1,"Unexpected support query scope")
    support={}
    for line in (support_dirs[0]/"summary.txt").read_text().splitlines():
        name,code=line.split();require(code=="40","Unexpected support result")
        log=(support_dirs[0]/name/"probe.log").read_text()
        require("dtype=Float32 requested_backend=Alnpu cpu_fallback_allowed=false inference_requested=false" in log,
                "Support query did not reach optimization")
        require("not supported" in log.lower() and "result=optimize_rejected" in log,"Not an operator support rejection")
        support[name]="optimize_rejected"
    require(len(support)==10,"Support query count differs")
    console=(directory/"benchmark-console.log").read_text()
    trials=[]; round_id=None; case=None
    for line in console.splitlines():
        if match:=re.match(r"trial=(\d+) case=(\w+)",line):
            round_id=int(match[1]);case=match[2]
        if line.startswith("exit_code="):
            match=re.match(r"exit_code=0 results=(\S+)",line)
            require(match is not None,"Benchmark failed")
            name=Path(match[1]).name
            metric=all_metrics[name]
            require(metric["samples"]==184,"Benchmark replay was partial")
            trials.append({"round":round_id,"case":case,"directory":name,**metric})
    require(len(trials)==15 and "result=paired_benchmark_completed" in console,"Paired benchmark incomplete")
    cases={t["case"] for t in trials}
    require(cases=={"neon","head2","ff4","ff6","input_ff5"},"Unexpected benchmark cases")
    summary={}
    for case in sorted(cases):
        values=[t for t in trials if t["case"]==case]
        require({t["round"] for t in values}=={1,2,3},"Missing benchmark round")
        require(len({t["prediction_sha256"] for t in values})==1,"Repeated predictions differ")
        summary[case]={"mean_us":float(np.mean([t["mean_forward_us"] for t in values])),
            "min_round_mean_us":min(t["mean_forward_us"] for t in values),"max_round_mean_us":max(t["mean_forward_us"] for t in values),
            "rmse":values[0]["rmse"],"mae":values[0]["mae"],"max_fp32_delta":values[0]["max_fp32_delta"],"cma_bytes":values[0]["cma_bytes"]}
    for value in summary.values():
        value["speedup_vs_neon"]=summary["neon"]["mean_us"]/value["mean_us"]
    return {"status":"offload_replays_verified","raw_files_verified":len(inventory["files"]),"validation_samples":184,"integer_audits":audits,
        "integer_outputs_checked":sum(a["outputs"] for a in audits),"integer_max_lsb":max(a["max_error_lsb"] for a in audits),
        "paired_trials":trials,"summary":summary,"application_accuracy_acceptance":"TBD",
        "interrupt_checks":irq_checks,
        "float32_support_queries":support,
        "test_dataset_opened":False,"training_executed":False}


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory",type=Path);parser.add_argument("--output",type=Path)
    args=parser.parse_args();result=verify(args.directory)
    if args.output:
        require(not args.output.exists(),"Refusing to overwrite verification report")
        args.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+"\n",encoding="utf-8",newline="\n")
    print(json.dumps({"status":result["status"],"integer_outputs_checked":result["integer_outputs_checked"],"summary":result["summary"]},ensure_ascii=False,indent=2))
