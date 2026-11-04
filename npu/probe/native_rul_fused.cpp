// Frozen-weight execution with preallocated fused CPU kernels.
#include "allocation_probe.hpp"
#define main legacy_armnn_diagnostic_main
#include "native_rul_cma.cpp"
#undef main
#include "fused_quantized_fc.hpp"
#include "fused_ops.hpp"
namespace { namespace fused {
std::pair<Value, Value> Graph::beforeGelu(Value x, Value adjacency, unsigned block, bool temporal) {
        const std::string kind = temporal ? "temp" : "spat";
        const std::string prefix = std::string(temporal ? "tvgtformer_blocks." : "svgtformer_blocks.") + std::to_string(block);
        const unsigned modelDim = temporal ? native_config::kTemporalDim : native_config::kSpatialDim;
        auto graphAttention = softmax(activation(adjacency, armnn::ActivationFunction::ReLu));
        std::vector<Value> heads;
        // The explicit Q/K/V paths share spline stems through kan()'s cache.
        for (unsigned h = 0; h < native_config::kHeads; ++h) {
            const std::string index = std::to_string(h);
            auto q = kan(x, prefix + ".linears_Q_" + kind + "." + index);
            auto k = kan(x, prefix + ".linears_K_" + kind + "." + index);
            auto v = kan(x, prefix + ".linears_V_" + kind + "." + index);
            auto scores = div(bmm(q, transpose(k, {0, 2, 1})), scalar(std::sqrt(static_cast<float>(modelDim))));
            auto attention = add(mul(softmax(scores), scalar(native_config::kOneMinusLambda)),
                                 mul(graphAttention, scalar(native_config::kLambda)));
            heads.push_back(bmm(softmax(attention), v));
        }
        auto projected = kan(concat(heads, 2), prefix + ".W_O_" + kind);
        auto residual = add(layerNorm(projected, prefix + ".layer_norm1_" + kind), x);
        return {kan(residual, prefix + ".feed_forward_" + kind + ".0"), residual};
    }
Value Graph::afterGelu(Value gelu, Value residual, unsigned block, bool temporal) {
        const std::string kind = temporal ? "temp" : "spat";
        const std::string prefix = std::string(temporal ? "tvgtformer_blocks." : "svgtformer_blocks.") + std::to_string(block);
        return add(layerNorm(kan(gelu, prefix + ".feed_forward_" + kind + ".2"), prefix + ".layer_norm2_" + kind), residual);
    }
std::unique_ptr<Graph> buildStage(const Weights& weights, unsigned stage, bool vectorized) {
    auto g = std::make_unique<Graph>(weights, vectorized);
    if (stage == 0) {
        auto features = g->input({1, 23, 110});
        auto expanded = g->reshape(g->kan(g->transpose(features, {0, 2, 1}), "linear_x"), {1, 10, 11, 23});
        std::vector<Value> sensors;
        for (unsigned j = 0; j < 11; ++j) {
            auto sample = g->reshape(g->slice(expanded, 2, j, 1), {10, 23});
            sensors.push_back(g->reshape(g->kan(sample, "linear_sample." + std::to_string(j)), {1, 10, 1}));
        }
        auto x = g->concat(sensors, 2);
        x = g->concat({x, g->weight("t_v")}, 1);
        x = g->concat({x, g->weight("x_v")}, 2);
        auto aTemp = g->pcc(x);
        auto aSpat = g->pcc(g->transpose(x, {0, 2, 1}));
        x = g->add(x, g->weight("__positional_encoding"));
        auto pair = g->beforeGelu(x, aTemp, 0, true);
        g->output(pair.first); g->output(pair.second); g->output(aTemp); g->output(aSpat);
    } else if (stage <= 4) {
        const unsigned previous = stage - 1;
        const bool temporal = previous % 2 == 0;
        const unsigned length = temporal ? 11 : 12;
        const unsigned features = temporal ? 12 : 11;
        const unsigned ff = temporal ? native_config::kTemporalFF : native_config::kSpatialFF;
        auto gelu = g->input({1, length, ff});
        auto residual = g->input({1, length, features});
        auto x = g->transpose(g->afterGelu(gelu, residual, previous / 2, temporal), {0, 2, 1});
        if (stage < 4) {
            const bool nextTemporal = stage % 2 == 0;
            auto adjacency = g->input(nextTemporal ? Shape{1, 11, 11} : Shape{1, 12, 12});
            auto pair = g->beforeGelu(x, adjacency, stage / 2, nextTemporal);
            g->output(pair.first); g->output(pair.second);
        } else g->output(g->kan(g->reshape(x, {1, 132}), "output_layer.0"));
    } else if (stage == 5) g->output(g->kan(g->input({1, 64}), "output_layer.2"));
    else throw std::runtime_error("Invalid stage");
    return g;
}

void activate(const Value& x,float* y) {
    for(std::size_t i=0;i<size(x);++i)y[i]=(x.data[i]*(1.0f+std::erf(x.data[i]/1.4142135623730951f)))*0.5f;
}
void dumpValue(const std::string& path,Value v) {
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(v.data),size(v)*sizeof(float));
    if(!file)throw std::runtime_error("Dump failed");
}
} } // namespace fused, anonymous
int main(int argc,char** argv) {
    std::cout<<std::unitbuf<<std::setprecision(10);
    std::string phase="arguments";
    try {
        if(argc==4 && std::string(argv[1])=="--stem-check") {
            const std::string root=argv[2],out=argv[3];unsigned rows,features;
            std::ifstream meta(root+"/meta.txt");if(!(meta>>rows>>features)||rows>10000||features>256)throw std::runtime_error("Stem fixture shape");
            auto x=readFloats(root+"/input.bin"),grid=readFloats(root+"/grid.bin"),baseRef=readFloats(root+"/base.bin"),splineRef=readFloats(root+"/spline.bin");
            if(x.size()!=rows*features||grid.size()!=features*13||baseRef.size()!=x.size()||splineRef.size()!=x.size()*9)throw std::runtime_error("Stem fixture extent");
            std::vector<float> base(x.size()),spline(x.size()*9);fused::splineStem(x.data(),grid.data(),rows,features,base.data(),spline.data());
            double maximum=0;unsigned bad=0;
            for(unsigned i=0;i<base.size();++i){const double d=std::abs(double(base[i])-baseRef[i]);maximum=std::max(maximum,d);if(d>1e-6+1e-6*std::abs(baseRef[i]))++bad;}
            for(unsigned i=0;i<spline.size();++i){const double d=std::abs(double(spline[i])-splineRef[i]);maximum=std::max(maximum,d);if(d>1e-6+1e-6*std::abs(splineRef[i]))++bad;}
            fused::dumpValue(out+"/base_actual.bin",{base.data(),{rows,features}});fused::dumpValue(out+"/spline_actual.bin",{spline.data(),{rows,features*9}});
            std::cout<<"result=stem_checked rows="<<rows<<" features="<<features<<" max_error="<<maximum<<" mismatches="<<bad<<'\n';return bad?70:0;
        }
        if(argc!=5)throw std::runtime_error("Usage: rul_fused ROOT COUNT RESULTS scalar|neon|cpu_quantized|hybrid");
        const std::string root=argv[1],result=argv[3],mode=argv[4];
        if(mode!="scalar"&&mode!="neon"&&mode!="cpu_quantized"&&mode!="hybrid")throw std::runtime_error("Unknown mode");
        const bool vectorized=mode!="scalar";
        fused::hybridMode=mode=="hybrid";fused::resultDirectory=result;
        npuRequested=fused::hybridMode;
        CmaSession cma;
        armnn::ConfigureLogging(true,false,armnn::LogSeverity::Error);
        if(mode=="cpu_quantized"||mode=="hybrid"){setenv("F3P_QUANT_PLAN","qparams_head.tsv",1);readQuantization(root);}
        auto input=readFloats(root+"/data/input_f32.bin"),reference=readFloats(root+"/data/reference_f32.bin"),labels=readFloats(root+"/data/labels_f32.bin");
        std::size_t used=0,count=std::stoul(argv[2],&used);
        if(used!=std::strlen(argv[2])||std::string(argv[2])[0]=='-')throw std::runtime_error("Bad count");
        if(!count)count=reference.size();
        if(input.size()!=reference.size()*2530||labels.size()!=reference.size()||count>reference.size())throw std::runtime_error("Replay lengths");
        Weights weights(root);
        std::vector<std::unique_ptr<fused::Graph>> stages;
        std::ofstream plan(result+"/plan.tsv");plan<<"stage\tops\ttensor_workspace_bytes\tstems\n";
        for(unsigned i=0;i<6;++i){phase="build_segment_"+std::to_string(i);stages.push_back(fused::buildStage(weights,i,vectorized));plan<<i<<'\t'<<stages.back()->operations.size()<<'\t'<<stages.back()->workspaceBytes<<'\t'<<stages.back()->stems.size()<<'\n';}
        std::vector<float> activated(1680);
        std::array<const fused::Value*,5> preGelu;
        auto predict=[&](std::size_t n){
            const auto& initial=stages[0]->run({{input.data()+n*2530,2530}});
            const auto* pre=&initial[0];const auto* residual=&initial[1];
            for(unsigned i=1;i<=4;++i){
                preGelu[i-1]=pre;fused::activate(*pre,activated.data());
                const auto& next=i<4?stages[i]->run({{activated.data(),fused::size(*pre)},{residual->data,fused::size(*residual)},{initial[i%2==0?2:3].data,fused::size(initial[i%2==0?2:3])}}):stages[i]->run({{activated.data(),fused::size(*pre)},{residual->data,fused::size(*residual)}});
                pre=&next[0];if(i<4)residual=&next[1];
            }
            preGelu[4]=pre;fused::activate(*pre,activated.data());
            return stages[5]->run({{activated.data(),fused::size(*pre)}})[0].data[0];
        };
        const unsigned warmup=5;
        for(unsigned n=0;n<warmup;++n)(void)predict(0);
        std::ofstream csv(result+"/predictions.csv");csv<<std::setprecision(10)<<"sample_index,prediction,reference,absolute_error,target,forward_us,allocation_calls,allocation_bytes,cma_bytes\n";
        double squared=0,absolute=0,maximum=0,totalUs=0;unsigned mismatches=0;
        std::size_t calls=0,bytes=0;
        const bool integerAudit=fused::hybridMode && std::getenv("F3P_VERIFY_INT") && std::string(std::getenv("F3P_VERIFY_INT"))=="1";
        std::ofstream audit;unsigned maxIntegerLSB=0;
        if(integerAudit){audit.open(result+"/integer_audit.csv");audit<<"sample,layer,row,channel,actual_u8,expected_u8,error_lsb\n";}
        phase="inference";
        for(std::size_t n=0;n<count;++n){
            if(n==0)for(auto& weak:fused::quantizedFcs)if(auto f=weak.lock())f->profile(true);
            allocationCalls=0;allocationBytes=0;countAllocations=true;
            const auto begin=std::chrono::steady_clock::now();const float prediction=predict(n);
            const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count();
            countAllocations=false;
            const auto ac=allocationCalls.load(),ab=allocationBytes.load();calls+=ac;bytes+=ab;
            if(n==0)for(auto& weak:fused::quantizedFcs)if(auto f=weak.lock()){f->profile(false);f->saveProfile(result);}
            if(integerAudit)for(auto& weak:fused::quantizedFcs)if(auto f=weak.lock())maxIntegerLSB=std::max(maxIntegerLSB,f->audit(n,audit));
            if(maxIntegerLSB>1)throw std::runtime_error("NPU integer tolerance exceeded");
            if(!std::isfinite(prediction))throw std::runtime_error("Nonfinite prediction");
            const double error=std::abs(double(prediction)-reference[n]),delta=double(prediction)-labels[n];
            squared+=delta*delta;absolute+=std::abs(delta);maximum=std::max(maximum,error);totalUs+=us;
            if(error>1e-4+1e-4*std::abs(reference[n]))++mismatches;
            csv<<n<<','<<prediction<<','<<reference[n]<<','<<error<<','<<labels[n]<<','<<us<<','<<ac<<','<<ab<<','<<(npuRequested?cma_mem_count():0)<<'\n';
            if(n==0){fused::dumpValue(result+"/a_temp.bin",stages[0]->outputs[2]);fused::dumpValue(result+"/a_spat.bin",stages[0]->outputs[3]);for(unsigned k=0;k<5;++k)fused::dumpValue(result+"/pre_gelu_"+std::to_string(k)+".bin",*preGelu[k]);}
        }
        std::cout<<"result=inference_completed mode="<<mode<<" samples="<<count<<" warmup="<<warmup<<" max_reference_error="<<maximum<<" mismatches="<<mismatches<<" rmse="<<std::sqrt(squared/count)<<" mae="<<absolute/count<<" mean_forward_us="<<totalUs/count<<" timed_allocation_calls="<<calls<<" timed_allocation_bytes="<<bytes<<" NPU_layers="<<(fused::hybridMode?2:0)<<" max_integer_lsb="<<maxIntegerLSB<<" integer_audit="<<integerAudit<<"\n";
        return quantization.empty()&&mismatches?70:0;
    }catch(const std::exception& e){countAllocations=false;std::cerr<<"result=failed stage="<<phase<<" error="<<e.what()<<'\n';return 2;}
}
