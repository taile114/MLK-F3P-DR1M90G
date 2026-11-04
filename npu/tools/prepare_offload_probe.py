"""Prepare a separate frozen-weight profiling/offload experiment; never train."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Expected one source anchor: {old[:100]!r}")
    return text.replace(old, new, 1)


def write(path, text):
    path.write_text(text, encoding="utf-8", newline="\n")


PROFILE_TYPES = r'''
bool profileOperations=false;
double geluTotalUs=0;
unsigned geluCalls=0;
struct OperationMeasurement {
    std::string kind,label;
    unsigned rows,inner,units;
    double totalUs=0,maximumUs=0;
    unsigned calls=0;
};
struct CapturedFc { std::string label; Value input,output; };
std::vector<CapturedFc> capturedFcs;
'''

PROFILE_METHODS = r'''
    std::vector<OperationMeasurement> measurements;
    template<class F> void append(const char* kind,const std::string& label,
                                 unsigned rows,unsigned inner,unsigned units,F operation) {
        operations.emplace_back(std::move(operation));
        measurements.push_back({kind,label,rows,inner,units,0,0,0});
    }
    void saveMeasurements(std::ostream& file,unsigned stage) const {
        for(std::size_t i=0;i<measurements.size();++i) {
            const auto& m=measurements[i];
            file<<stage<<'\t'<<i<<'\t'<<m.kind<<'\t'<<m.label<<'\t'<<m.rows<<'\t'
                <<m.inner<<'\t'<<m.units<<'\t'<<m.calls<<'\t'<<m.totalUs<<'\t'
                <<(m.calls?m.totalUs/m.calls:0)<<'\t'<<m.maximumUs<<'\n';
        }
    }
'''

BRANCH_MAIN = r'''
int runOffloadBranch(int argc,char** argv) {
    (void)argc;
    const std::string root=argv[2],capture=argv[3],label=argv[4],result=argv[5],layout=argv[6];
    if(layout!="fc" && layout!="conv")throw std::runtime_error("Invalid branch layout");
    setenv("F3P_QUANT_PLAN","qparams_all.tsv",1);readQuantization(root);
    if(layout=="conv")fused::convLabels.insert(label);
    npuRequested=true;CmaSession cma;
    armnn::ConfigureLogging(true,false,armnn::LogSeverity::Error);
    Weights weights(root);
    auto input=readFloats(capture+"/capture_"+label+"_input.bin");
    auto reference=readFloats(capture+"/capture_"+label+"_output.bin");
    const auto& q=quantization.at(label);
    if(input.size()%q.inputs)throw std::runtime_error("Invalid captured branch input");
    const unsigned rows=input.size()/q.inputs;
    if(reference.size()!=rows*q.outputs)throw std::runtime_error("Invalid captured output");
    std::vector<float> output(reference.size()),cpu(reference.size());
    const float* wf=weights.data(label);
    const auto beginCPU=std::chrono::steady_clock::now();
    for(unsigned repeat=0;repeat<20;++repeat)
        for(unsigned r=0;r<rows;++r)for(unsigned o=0;o<q.outputs;++o)
            cpu[r*q.outputs+o]=fused::dot(input.data()+r*q.inputs,wf+o*q.inputs,q.inputs,true);
    const double cpuUs=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-beginCPU).count()/20;
    fused::QuantizedFc kernel(weights,label,rows,true,result);
    for(unsigned i=0;i<5;++i)kernel.run(input.data(),output.data());
    double total=0;
    for(unsigned i=0;i<20;++i) {
        const auto begin=std::chrono::steady_clock::now();kernel.run(input.data(),output.data());
        total+=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count();
    }
    std::ofstream audit(result+"/integer_audit.csv");
    audit<<"sample,layer,row,channel,actual_u8,expected_u8,error_lsb\n";
    const unsigned integerError=kernel.audit(0,audit);
    kernel.profile(true);kernel.run(input.data(),output.data());kernel.profile(false);kernel.saveProfile(result);
    double squared=0,maximum=0,cpuError=0;unsigned inputClipped=0,outputSaturated=0;
    for(float v:input)if(std::round(v/q.sx)<-128 || std::round(v/q.sx)>127)++inputClipped;
    for(unsigned n=0;n<output.size();++n) {
        const double d=double(output[n])-reference[n];squared+=d*d;maximum=std::max(maximum,std::abs(d));
        cpuError=std::max(cpuError,std::abs(double(cpu[n])-reference[n]));
        if(output[n]<=-128*q.sy || output[n]>=127*q.sy)++outputSaturated;
    }
    fused::dumpValue(result+"/actual_f32.bin",{output.data(),{rows,q.outputs}});
    std::cout<<"result=branch_completed layer="<<label<<" layout="<<layout<<" rows="<<rows
        <<" input_features="<<q.inputs<<" output_features="<<q.outputs
        <<" cpu_neon_us="<<cpuUs<<" npu_boundary_us="<<total/20
        <<" max_integer_lsb="<<integerError<<" branch_rmse="<<std::sqrt(squared/output.size())
        <<" branch_max_abs="<<maximum<<" capture_cpu_error="<<cpuError
        <<" input_clipped="<<inputClipped<<" output_saturated="<<outputSaturated
        <<" cma_bytes="<<cma_mem_count()<<'\n';
    if(integerError>1 || cpuError>1e-4) return 70;
    return 0;
}
'''


def prepare(destination):
    if destination.exists():
        raise FileExistsError(destination)
    src = destination / "src"
    src.mkdir(parents=True)
    (destination / "bin").mkdir()
    # Include dependencies and math are copied; existing deployed sources stay untouched.
    dependencies = ["native_rul_cma.cpp", "native_rul_quantized.cpp", "allocation_probe.hpp", "npu_op_support.cpp"]
    for name in dependencies:
        shutil.copyfile(ROOT / "probe" / name, src / name)
    with tarfile.open(ROOT / "artifacts/f3p-fused-rul-arm64-v1.tar.gz") as frozen:
        (src / "model_config.hpp").write_bytes(frozen.extractfile("f3p-fused-rul-v1/src/model_config.hpp").read())

    header = (ROOT / "probe/fused_ops.hpp").read_text(encoding="utf-8")
    header = replace_once(header, "struct Graph {", PROFILE_TYPES + "\nstruct Graph {")
    header = replace_once(header, "    const Weights& weights;", PROFILE_METHODS + "\n    const Weights& weights;")
    # Assign labels at graph construction, with no per-forward string allocations.
    current = None
    transformed = []
    for line in header.splitlines():
        if line.startswith("    Value "):
            current = line.split("(")[0].split()[-1]
        if "operations.emplace_back(" in line and current != "append":
            if "std::move(operation)" in line:
                transformed.append(line)
                continue
            kind = current
            label, dims = ('""', "0,0,0")
            if current == "fc":
                label, dims = "label", "rows,inner,units"
            elif current == "kan":
                kind, label, dims = "spline_stem", "prefix", "batch,features,features*9"
            line = line.replace("operations.emplace_back(", f'append("{kind}",{label},{dims},')
        transformed.append(line)
    header = "\n".join(transformed) + "\n"
    header = replace_once(header, "            auto y=buffer({rows,units});\n            append", "            auto y=buffer({rows,units});\n            capturedFcs.push_back({label,x,y});\n            append")
    header = replace_once(header, "const bool vectorized=neon;", "const bool vectorized=neon;\n        capturedFcs.push_back({label,x,y});")
    header = replace_once(header, "        for(auto& execute:operations) execute();", r'''
        if(profileOperations)for(std::size_t i=0;i<operations.size();++i) {
            const auto begin=std::chrono::steady_clock::now();operations[i]();
            const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count();
            auto& m=measurements[i];m.totalUs+=us;m.maximumUs=std::max(m.maximumUs,us);++m.calls;
        } else for(auto& execute:operations)execute();''')
    write(src / "offload_ops.hpp", header)

    quantized = (ROOT / "probe/fused_quantized_fc.hpp").read_text(encoding="utf-8")
    quantized = quantized.replace("// Two isolated UINT8 FCs.", "// Configurable isolated UINT8 FC/Conv branches.")
    quantized = quantized.replace("namespace { namespace fused {", "namespace { namespace fused {\nstd::set<std::string> convLabels;\nstd::string offloadPhase=\"arguments\";", 1)
    quantized = replace_once(quantized, "bool loaded=false,hardware;", "bool loaded=false,hardware,convolution;")
    quantized = replace_once(quantized, ":hardware(useNpu),label(name)", ":hardware(useNpu),convolution(convLabels.count(name)!=0),label(name)")
    quantized = replace_once(quantized, "        if(!hardware)return;", "        if(!hardware)return;\n        offloadPhase=\"runtime\";")
    quantized = replace_once(quantized, "        auto inputInfo=qi(rows,ic,q.sx,true),outputInfo=qi(rows,oc,q.sy);", r'''
        offloadPhase="construct";
        auto inputInfo=qi(rows,ic,q.sx,true),outputInfo=qi(rows,oc,q.sy),weightInfo=qi(oc,ic,q.sw,true);
        if(convolution) {
            inputInfo=armnn::TensorInfo(armnn::TensorShape({1,ic,1,rows}),armnn::DataType::QAsymmU8,q.sx,128,true);
            outputInfo=armnn::TensorInfo(armnn::TensorShape({1,oc,1,rows}),armnn::DataType::QAsymmU8,q.sy,128);
            weightInfo=armnn::TensorInfo(armnn::TensorShape({oc,ic,1,1}),armnn::DataType::QAsymmU8,q.sw,128,true);
        }''')
    quantized = quantized.replace("qi(oc,ic,q.sw,true),weights.data()", "weightInfo,weights.data()")
    quantized = replace_once(quantized, "weight->GetOutputSlot(0).SetTensorInfo(qi(oc,ic,q.sw,true));", "weight->GetOutputSlot(0).SetTensorInfo(weightInfo);")
    quantized = replace_once(quantized, '        armnn::FullyConnectedDescriptor d;d.m_TransposeWeightMatrix=true;d.m_BiasEnabled=true;\n        auto* fc=network->AddFullyConnectedLayer(d,"fused_head_fc");', r'''
        armnn::IConnectableLayer* fc=nullptr;
        if(convolution) {
            armnn::Convolution2dDescriptor d;d.m_DataLayout=armnn::DataLayout::NCHW;
            d.m_StrideX=d.m_StrideY=1;d.m_BiasEnabled=true;
            fc=network->AddConvolution2dLayer(d,label.c_str());
        } else {
            armnn::FullyConnectedDescriptor d;d.m_TransposeWeightMatrix=true;d.m_BiasEnabled=true;
            fc=network->AddFullyConnectedLayer(d,label.c_str());
        }''')
    quantized = replace_once(quantized, "        auto optimized=armnn::Optimize", '        offloadPhase="optimize";\n        auto optimized=armnn::Optimize')
    quantized = replace_once(quantized, "        if(runtime->LoadNetwork", '        offloadPhase="load";\n        if(runtime->LoadNetwork')
    quantized = replace_once(quantized, "<<q.sy<<'\\n';", "<<q.sy<<' '<<(convolution?\"NCHW\":\"rows_features\")<<'\\n';")
    quantized = quantized.replace("input[row*ic+i]", "input[convolution?i*rows+row:row*ic+i]")
    quantized = quantized.replace("quantized[r*ic+i]", "quantized[convolution?i*rows+r:r*ic+i]")
    quantized = quantized.replace("result[r*oc+o]", "result[convolution?o*rows+r:r*oc+o]")
    quantized = replace_once(quantized, "            if(runtime->EnqueueWorkload", '            offloadPhase="inference";\n            if(runtime->EnqueueWorkload')
    quantized = replace_once(quantized, "actual=values[r*oc+o]", "actual=values[convolution?o*rows+r:r*oc+o]")
    write(src / "offload_quantized_fc.hpp", quantized)

    main = (ROOT / "probe/native_rul_fused.cpp").read_text(encoding="utf-8")
    main = main.replace('#include "allocation_probe.hpp"', '#include <set>\n#include "allocation_probe.hpp"')
    main = main.replace('"fused_quantized_fc.hpp"', '"offload_quantized_fc.hpp"').replace('"fused_ops.hpp"', '"offload_ops.hpp"')
    main = replace_once(main, 'void activate(const Value& x,float* y) {', 'void activate(const Value& x,float* y) {\n    const auto begin=profileOperations?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};')
    main = replace_once(main, 'y[i]=(x.data[i]*(1.0f+std::erf(x.data[i]/1.4142135623730951f)))*0.5f;\n}', 'y[i]=(x.data[i]*(1.0f+std::erf(x.data[i]/1.4142135623730951f)))*0.5f;\n    if(profileOperations){geluTotalUs+=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count();++geluCalls;}\n}')
    main = replace_once(main, "int main(int argc,char** argv)", BRANCH_MAIN + "\nint main(int argc,char** argv)")
    main = replace_once(main, "    try {", '    try {\n        if(argc==7 && std::string(argv[1])=="--branch")return runOffloadBranch(argc,argv);')
    main = replace_once(main, 'setenv("F3P_QUANT_PLAN","qparams_head.tsv",1);readQuantization(root);', 'if(!std::getenv("F3P_QUANT_PLAN"))setenv("F3P_QUANT_PLAN","qparams_head.tsv",1);readQuantization(root);')
    main = replace_once(main, '        Weights weights(root);', r'''
        if(const char* name=std::getenv("F3P_CONV_PLAN")) {
            std::ifstream conv(root+"/"+name);if(!conv)throw std::runtime_error("Cannot read convolution plan");
            for(std::string label;conv>>label;)if(!fused::convLabels.insert(label).second)throw std::runtime_error("Duplicate convolution label");
            for(const auto& label:fused::convLabels)if(!quantization.count(label))throw std::runtime_error("Convolution outside quantization plan");
        }
        Weights weights(root);''')
    main = replace_once(main, '        phase="inference";', '        const bool detailed=std::getenv("F3P_TRACE_OPS") && std::string(std::getenv("F3P_TRACE_OPS"))=="1";\n        phase="inference";')
    main = replace_once(main, "            allocationCalls=0;allocationBytes=0;countAllocations=true;", "            fused::profileOperations=detailed;\n            allocationCalls=0;allocationBytes=0;countAllocations=true;")
    main = replace_once(main, "            countAllocations=false;", "            countAllocations=false;fused::profileOperations=false;")
    main = replace_once(main, '            if(n==0){fused::dumpValue', r'''
            if(n==0 && std::getenv("F3P_CAPTURE_FC")) {
                for(const auto& f:fused::capturedFcs) {
                    fused::dumpValue(result+"/capture_"+f.label+"_input.bin",f.input);
                    fused::dumpValue(result+"/capture_"+f.label+"_output.bin",f.output);
                }
            }
            if(n==0){fused::dumpValue''')
    main = replace_once(main, '        std::cout<<"result=inference_completed', r'''
        if(detailed) {
            std::ofstream file(result+"/operations.tsv");file<<std::setprecision(17);
            file<<"stage\top\tkind\tlabel\trows\tinner\tunits\tcalls\ttotal_us\tmean_us\tmax_us\n";
            for(unsigned i=0;i<stages.size();++i)stages[i]->saveMeasurements(file,i);
            std::ofstream host(result+"/host_gelu.tsv");host<<std::setprecision(17)<<"calls\ttotal_us\tmean_us\n"<<fused::geluCalls<<'\t'<<fused::geluTotalUs<<'\t'<<(fused::geluCalls?fused::geluTotalUs/fused::geluCalls:0)<<'\n';
        }
        std::cout<<"result=inference_completed''')
    main = replace_once(main, '(fused::hybridMode?2:0)', '(fused::hybridMode?fused::quantizedFcs.size():0)')
    main = replace_once(main, '<<" integer_audit="<<integerAudit', '<<" integer_audit="<<integerAudit<<" operation_profile="<<detailed')
    main = replace_once(main, 'std::cerr<<"result=failed stage="<<phase<<" error="<<e.what()<<\'\\n\';return 2;', 'std::cerr<<"result=failed stage="<<phase<<" offload_stage="<<fused::offloadPhase<<" error="<<e.what()<<\'\\n\';return fused::offloadPhase=="optimize"?40:fused::offloadPhase=="load"?50:fused::offloadPhase=="inference"?60:2;')
    write(src / "native_rul_offload.cpp", main)

    calibration_path = ROOT / "results/calibration/rul-uint8-v1/calibration.json"
    calibration = json.loads(calibration_path.read_text(encoding="utf-8"))
    params = []
    for label, entry in sorted(calibration["layers"].items()):
        params.append(f"{label} {entry['input_features']} {entry['output_features']} " + " ".join(format(entry[k], ".17g") for k in ["input_scale", "weight_scale", "output_scale"]))
    write(destination / "qparams_all.tsv", "\n".join(params) + "\n")
    data = destination / "data"
    data.mkdir()
    with tarfile.open(ROOT / "artifacts/f3p-fused-rul-arm64-v1.tar.gz") as frozen:
        for name in ["qparams_head.tsv", "weights.sha256", "bin/timeout",
                     "data/input_f32.bin", "data/reference_f32.bin", "data/labels_f32.bin"]:
            (destination / name).write_bytes(frozen.extractfile("f3p-fused-rul-v1/" + name).read())
    with tarfile.open(ROOT / "artifacts/f3p-native-rul-arm64-v1.tar.gz") as frozen:
        for name in ["weights.bin", "weights.tsv"]:
            (destination / name).write_bytes(frozen.extractfile("f3p-native-rul-v1/" + name).read())
    shutil.copyfile(ROOT / "probe/run_offload_probe.sh", destination / "run.sh")
    shutil.copyfile(ROOT / "probe/benchmark_offload.sh", destination / "benchmark_offload.sh")
    shutil.copyfile(ROOT / "probe/query_npu_support.sh", destination / "support.sh")
    ff = [f"{kind}vgtformer_blocks.{block}.feed_forward_{suffix}.0.spline_weight"
          for kind, suffix in [("t", "temp"), ("s", "spat")] for block in [0, 1]]
    head = ["output_layer.0.spline_weight", "output_layer.2.spline_weight"]
    plans = {"ff4": ff, "ff6": ff+head, "input_ff5": ff+["linear_x.spline_weight"],
             "input_ff7": ff+["linear_x.spline_weight"]+head}
    for name, labels in plans.items():
        selected = []
        for label in labels:
            entry = calibration["layers"][label]
            selected.append(f"{label} {entry['input_features']} {entry['output_features']} " +
                            " ".join(format(entry[k], ".17g") for k in ["input_scale", "weight_scale", "output_scale"]))
        write(destination / ("qparams_"+name+".tsv"), "\n".join(selected)+"\n")
    write(destination / "conv_input.txt", "linear_x.spline_weight\n")
    write(destination / "candidate_plans.json", json.dumps(plans, indent=2)+"\n")
    write(destination / "build.sh", '''#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${CXX:?Set paired compiler}"
: "${ARMNN_ROOT:?Set paired runtime}"
for target in native_rul_offload npu_op_support; do
out=rul_offload
if [ "$target" = npu_op_support ]; then out=npu_op_support; fi
"$CXX" -std=c++17 -O3 -ffp-contract=off -Wall -Wextra -Wpedantic \\
 -I"$ARMNN_ROOT/include" -I"$root/src" "$root/src/$target.cpp" \\
 -L"$ARMNN_ROOT/lib" -Wl,-rpath-link,"$ARMNN_ROOT/lib" -larmnn -pthread -ldl -o "$root/bin/$out"
done
''')
    sources = {name: hashlib.sha256((ROOT / "probe" / name).read_bytes()).hexdigest() for name in dependencies + ["native_rul_fused.cpp", "fused_ops.hpp", "fused_quantized_fc.hpp"]}
    report = {"state": "source_prepared_hardware_not_tested", "source_sha256": sources,
              "checkpoint_sha256": calibration["checkpoint_sha256"], "calibration_sha256": hashlib.sha256(calibration_path.read_bytes()).hexdigest(),
              "quantized_branches_available": len(params), "training_executed": False,
              "test_dataset_opened": False, "existing_deployment_modified": False}
    write(destination / "preparation.json", json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    with tarfile.open(destination.with_suffix(".source.tar.gz"), "w:gz") as archive:
        archive.add(destination, arcname=destination.name)
    print(json.dumps({"directory": str(destination), "available_quantized_branches": len(params)}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "work/f3p-offload-v1")
    prepare(parser.parse_args().output.resolve())
