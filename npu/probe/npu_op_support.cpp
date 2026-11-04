// Query the registered vendor backend for actual Float32 model operations.
// This program optimizes graphs only: it never loads or enqueues a network.
#include <armnn/ArmNN.hpp>
#include <armnn/Logging.hpp>
#include <armnnALUtils/cma_mem_init.hpp>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

armnn::TensorInfo fp32(std::initializer_list<unsigned> shape) {
    return armnn::TensorInfo(armnn::TensorShape(shape),armnn::DataType::Float32);
}

int main(int argc,char** argv) {
    bool cma=false;
    try {
        if(argc!=3)throw std::runtime_error("Usage: npu_op_support CASE RESULTS");
        const std::string name=argv[1],directory=argv[2];
        cma_mem_init();cma=cma_mem_is_init();if(!cma)throw std::runtime_error("CMA init");
        armnn::ConfigureLogging(true,false,armnn::LogSeverity::Warning);
        {
            armnn::IRuntime::CreationOptions options;
            auto allocator=std::make_shared<armnn::CmaAllocator>();
            options.m_CustomAllocatorMap["CpuRef"]=allocator;options.m_CustomAllocatorMap["CpuAcc"]=allocator;
            auto runtime=armnn::IRuntime::Create(options);
            auto graph=armnn::INetwork::Create();
            auto info=fp32({1,11,12});
            auto* input=graph->AddInputLayer(0);input->GetOutputSlot(0).SetTensorInfo(info);
            armnn::IConnectableLayer* compute=nullptr;
            if(name=="relu"||name=="sigmoid"||name=="tanh") {
                const auto fn=name=="relu"?armnn::ActivationFunction::ReLu:name=="sigmoid"?armnn::ActivationFunction::Sigmoid:armnn::ActivationFunction::TanH;
                compute=graph->AddActivationLayer(armnn::ActivationDescriptor(fn,1,1));
            } else if(name=="softmax") {
                armnn::SoftmaxDescriptor descriptor;descriptor.m_Axis=-1;
                compute=graph->AddSoftmaxLayer(descriptor);
            } else if(name=="ceil") {
                compute=graph->AddElementwiseUnaryLayer(armnn::ElementwiseUnaryDescriptor(armnn::UnaryOperation::Ceil));
            } else if(name=="add"||name=="sub"||name=="mul"||name=="div") {
                const auto op=name=="add"?armnn::BinaryOperation::Add:name=="sub"?armnn::BinaryOperation::Sub:name=="mul"?armnn::BinaryOperation::Mul:armnn::BinaryOperation::Div;
                compute=graph->AddElementwiseBinaryLayer(armnn::ElementwiseBinaryDescriptor(op));
                auto* second=graph->AddInputLayer(1);second->GetOutputSlot(0).SetTensorInfo(info);
                second->GetOutputSlot(0).Connect(compute->GetInputSlot(1));
            } else if(name=="batch_matmul") {
                input->GetOutputSlot(0).SetTensorInfo(fp32({1,11,21}));
                auto* second=graph->AddInputLayer(1);second->GetOutputSlot(0).SetTensorInfo(fp32({1,21,11}));
                compute=graph->AddBatchMatMulLayer(armnn::BatchMatMulDescriptor());
                second->GetOutputSlot(0).Connect(compute->GetInputSlot(1));info=fp32({1,11,11});
            } else throw std::runtime_error("Unknown case");
            input->GetOutputSlot(0).Connect(compute->GetInputSlot(0));
            compute->GetOutputSlot(0).SetTensorInfo(info);
            compute->GetOutputSlot(0).Connect(graph->AddOutputLayer(0)->GetInputSlot(0));
            std::cout<<"case="<<name<<" dtype=Float32 requested_backend=Alnpu cpu_fallback_allowed=false inference_requested=false\n";
            auto optimized=armnn::Optimize(*graph,{armnn::BackendId("Alnpu")},runtime->GetDeviceSpec(),armnn::OptimizerOptionsOpaque());
            if(!optimized)throw std::runtime_error("Optimize returned null");
            std::ofstream file(directory+"/graph.dot");optimized->SerializeToDot(file);
            std::cout<<"result=optimize_accepted hardware_execution_verified=false\n";
        }
        cma_mem_deinit();return 0;
    } catch(const std::exception& error) {
        if(cma)cma_mem_deinit();
        std::cerr<<"result=optimize_rejected error="<<error.what()<<'\n';return 40;
    }
}
