#pragma once
// Two isolated UINT8 FCs. Host conversions use persistent buffers; NPU IO imports CMA.
namespace { namespace fused {
struct PinnedBuffer {
    cma_buffer_t value;
    explicit PinnedBuffer(unsigned bytes):value(bytes) {
        cma_mem_alloc(&value);
        if(!value.virt_addr||!value.phys_addr)throw std::runtime_error("Pinned buffer allocation");
    }
    ~PinnedBuffer(){cma_mem_free(&value);}
    PinnedBuffer(const PinnedBuffer&)=delete;
};
struct QuantizedFc {
    armnn::IRuntimePtr runtime{nullptr,armnn::IRuntime::Destroy};
    armnn::NetworkId networkId=0;
    bool loaded=false,hardware;
    std::string label;
    Quantization q;
    unsigned rows,ic,oc;
    std::vector<std::uint8_t> weights,inputCPU,outputCPU;
    std::vector<std::int32_t> biases;
    std::unique_ptr<PinnedBuffer> inputCma,outputCma;
    armnn::InputTensors inputs;
    armnn::OutputTensors outputs;
    std::ofstream auditInput,auditOutput;
    static bool allocatorRegistered;
    static armnn::TensorInfo qi(unsigned r,unsigned c,float scale,bool constant=false) {
        return armnn::TensorInfo(armnn::TensorShape({r,c}),armnn::DataType::QAsymmU8,scale,128,constant);
    }
    QuantizedFc(const Weights& source,const std::string& name,unsigned batch,bool useNpu,const std::string& result)
        :hardware(useNpu),label(name),q(quantization.at(name)),rows(batch),ic((q.inputs+7)/8*8),oc((q.outputs+7)/8*8),
         weights(ic*oc,128),inputCPU(rows*ic,128),outputCPU(rows*oc),biases(oc,0) {
        for(unsigned o=0;o<q.outputs;++o)for(unsigned i=0;i<q.inputs;++i)
            weights[o*ic+i]=static_cast<std::uint8_t>(std::max(0,std::min(255,int(std::round(source.data(name)[o*q.inputs+i]/q.sw))+128)));
        if(!hardware)return;
        armnn::IRuntime::CreationOptions creation;
        if(!allocatorRegistered){
            auto allocator=std::make_shared<armnn::CmaAllocator>();
            creation.m_CustomAllocatorMap["CpuRef"]=allocator;creation.m_CustomAllocatorMap["CpuAcc"]=allocator;
        }
        runtime=armnn::IRuntime::Create(creation);allocatorRegistered=true;
        auto network=armnn::INetwork::Create();
        auto inputInfo=qi(rows,ic,q.sx,true),outputInfo=qi(rows,oc,q.sy);
        auto* input=network->AddInputLayer(0,"pinned_input");input->GetOutputSlot(0).SetTensorInfo(inputInfo);
        auto* weight=network->AddConstantLayer(armnn::ConstTensor(qi(oc,ic,q.sw,true),weights.data()),name.c_str());
        weight->GetOutputSlot(0).SetTensorInfo(qi(oc,ic,q.sw,true));
        armnn::TensorInfo bi(armnn::TensorShape({oc}),armnn::DataType::Signed32,q.sx*q.sw,0,true);
        auto* bias=network->AddConstantLayer(armnn::ConstTensor(bi,biases.data()),"zero_bias");bias->GetOutputSlot(0).SetTensorInfo(bi);
        armnn::FullyConnectedDescriptor d;d.m_TransposeWeightMatrix=true;d.m_BiasEnabled=true;
        auto* fc=network->AddFullyConnectedLayer(d,"fused_head_fc");
        input->GetOutputSlot(0).Connect(fc->GetInputSlot(0));weight->GetOutputSlot(0).Connect(fc->GetInputSlot(1));bias->GetOutputSlot(0).Connect(fc->GetInputSlot(2));fc->GetOutputSlot(0).SetTensorInfo(outputInfo);
        fc->GetOutputSlot(0).Connect(network->AddOutputLayer(0)->GetInputSlot(0));
        armnn::OptimizerOptionsOpaque options;options.SetImportEnabled(true);options.SetExportEnabled(true);options.SetProfilingEnabled(true);
        auto optimized=armnn::Optimize(*network,{armnn::BackendId("Alnpu")},runtime->GetDeviceSpec(),options);
        if(!optimized)throw std::runtime_error("NPU FC Optimize failed");
        std::ostringstream dot;optimized->SerializeToDot(dot);
        if(dot.str().find("LayerType : ALHardNPU")==std::string::npos || dot.str().find("BackendID : Alnpu")==std::string::npos)throw std::runtime_error("FC not assigned to NPU");
        std::ofstream(result+"/"+label+".dot")<<dot.str();
        armnn::INetworkProperties properties(false,armnn::MemorySource::Malloc,armnn::MemorySource::Malloc,true);
        std::string error;
        if(runtime->LoadNetwork(networkId,std::move(optimized),error,properties)!=armnn::Status::Success)throw std::runtime_error(error);
        loaded=true;runtime->GetProfiler(networkId)->EnableProfiling(false);
        inputCma=std::make_unique<PinnedBuffer>(rows*ic);outputCma=std::make_unique<PinnedBuffer>(rows*oc);
        inputs.emplace_back(0,armnn::ConstTensor(inputInfo,inputCma->value.virt_addr));
        outputs.emplace_back(0,armnn::Tensor(outputInfo,outputCma->value.virt_addr));
        const char* verify=std::getenv("F3P_VERIFY_INT");
        if(verify && std::string(verify)=="1") {
            auditInput.open(result+"/"+label+"_input_u8.bin",std::ios::binary);
            auditOutput.open(result+"/"+label+"_output_u8.bin",std::ios::binary);
            std::ofstream wf(result+"/"+label+"_weights_u8.bin",std::ios::binary);
            wf.write(reinterpret_cast<const char*>(weights.data()),weights.size());
            std::ofstream meta(result+"/"+label+"_meta.txt");
            meta<<std::setprecision(17)<<rows<<' '<<q.inputs<<' '<<q.outputs<<' '<<ic<<' '<<oc<<' '<<q.sx<<' '<<q.sw<<' '<<q.sy<<'\n';
            if(!auditInput||!auditOutput||!wf||!meta)throw std::runtime_error("Cannot save integer-audit fixtures");
        }
    }
    int expected(unsigned row,unsigned channel)const {
        const auto* input=hardware?static_cast<const std::uint8_t*>(inputCma->value.virt_addr):inputCPU.data();
        std::int64_t acc=0;
        for(unsigned i=0;i<ic;++i)acc+=(int(input[row*ic+i])-128)*(int(weights[channel*ic+i])-128);
        const double multiplier=double(q.sx)*double(q.sw)/double(q.sy);
        return std::max(0,std::min(255,int(std::round(double(acc)*multiplier))+128));
    }
    void run(const float* input,float* output) {
        auto* quantized=hardware?static_cast<std::uint8_t*>(inputCma->value.virt_addr):inputCPU.data();
        std::fill_n(quantized,rows*ic,128);
        for(unsigned r=0;r<rows;++r)for(unsigned i=0;i<q.inputs;++i)
            quantized[r*ic+i]=static_cast<std::uint8_t>(std::max(0,std::min(255,int(std::round(input[r*q.inputs+i]/q.sx))+128)));
        auto* result=hardware?static_cast<std::uint8_t*>(outputCma->value.virt_addr):outputCPU.data();
        if(hardware){
            cma_mem_sync_for_device(inputCma->value.phys_addr,rows*ic);
            cma_mem_sync_for_device(outputCma->value.phys_addr,rows*oc);
            if(runtime->EnqueueWorkload(networkId,inputs,outputs)!=armnn::Status::Success)throw std::runtime_error("NPU enqueue failed");
            cma_mem_sync_for_cpu(outputCma->value.phys_addr,rows*oc);
        }else for(unsigned r=0;r<rows;++r)for(unsigned o=0;o<oc;++o)result[r*oc+o]=static_cast<std::uint8_t>(expected(r,o));
        for(unsigned r=0;r<rows;++r)for(unsigned o=0;o<q.outputs;++o)output[r*q.outputs+o]=(int(result[r*oc+o])-128)*q.sy;
    }
    unsigned audit(std::size_t sample,std::ostream& csv) {
        const auto* values=static_cast<const std::uint8_t*>(outputCma->value.virt_addr);
        auditInput.write(static_cast<const char*>(inputCma->value.virt_addr),rows*ic);
        auditOutput.write(reinterpret_cast<const char*>(values),rows*oc);
        if(!auditInput||!auditOutput)throw std::runtime_error("Integer-audit write failed");
        unsigned maximum=0;
        for(unsigned r=0;r<rows;++r)for(unsigned o=0;o<oc;++o){
            const int reference=expected(r,o),actual=values[r*oc+o];const unsigned error=std::abs(actual-reference);maximum=std::max(maximum,error);
            csv<<sample<<','<<label<<','<<r<<','<<o<<','<<actual<<','<<reference<<','<<error<<'\n';
        }
        return maximum;
    }
    void profile(bool enabled){if(hardware)runtime->GetProfiler(networkId)->EnableProfiling(enabled);}
    void saveProfile(const std::string& result){if(hardware){std::ofstream file(result+"/"+label+"_profile.json");runtime->GetProfiler(networkId)->Print(file);}}
    ~QuantizedFc(){if(loaded)runtime->UnloadNetwork(networkId);}
};
bool QuantizedFc::allocatorRegistered=false;
std::vector<std::weak_ptr<QuantizedFc>> quantizedFcs;
bool hybridMode=false;
std::string resultDirectory;
} } // namespace fused, anonymous
