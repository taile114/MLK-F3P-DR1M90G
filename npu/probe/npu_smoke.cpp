// Independent quantized convolution controls for the paired vendor NPU image.
#include <armnn/ArmNN.hpp>
#include <armnn/IProfiler.hpp>
#include <armnn/Logging.hpp>
#include <armnnALUtils/cma_mem_init.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int channels = 8, height = 8, width = 8, kernel = 3;
constexpr int elements = channels * height * width;

armnn::TensorInfo info(std::initializer_list<unsigned> shape, armnn::DataType type,
                       float scale, int offset, bool constant = false) {
    return armnn::TensorInfo(armnn::TensorShape(static_cast<unsigned>(shape.size()), shape.begin()),
                             type, scale, offset, constant);
}

struct CmaGuard {
    bool owned = false;
    void initialize() {
        if (!cma_mem_is_init()) {
            cma_mem_init();
            owned = cma_mem_is_init();
            if (!owned) throw std::runtime_error("CMA initialization failed");
        }
    }
    ~CmaGuard() { if (owned) cma_mem_deinit(); }
};

struct CmaBuffer {
    cma_buffer_t value;
    explicit CmaBuffer(unsigned bytes) : value(bytes) {
        cma_mem_alloc(&value);
        if (!value.virt_addr || !value.phys_addr) throw std::runtime_error("CMA allocation failed");
    }
    ~CmaBuffer() { cma_mem_free(&value); }
    CmaBuffer(const CmaBuffer&) = delete;
    CmaBuffer& operator=(const CmaBuffer&) = delete;
};

std::ofstream create(const std::string& path) {
    if (std::ifstream(path).good()) throw std::runtime_error("Refusing to overwrite: " + path);
    std::ofstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot create: " + path);
    return stream;
}

int inputValue(int c, int h, int w) { return (c * 3 + h * 5 + w * 7) % 17 - 8; }
int weightValue(int o, int i, int h, int w) { return (o * 7 + i * 3 + h * 2 + w) % 5 - 2; }
int index(int c, int h, int w) { return (c * height + h) * width + w; }
int decoded(std::uint8_t value, bool signedValue) { return signedValue && value >= 128 ? int(value) - 256 : int(value); }
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf << std::setprecision(10);
    std::string stage = "arguments";
    int failure = 2;
    try {
        if (argc != 4) throw std::runtime_error("Usage: npu_smoke inspect|u8|u8_s8|s8 CpuRef|Alnpu RESULTS_DIR");
        const std::string name = argv[1], backend = argv[2], directory = argv[3];
        if (backend != "CpuRef" && backend != "Alnpu") throw std::runtime_error("Exactly one backend is required");
        if (name != "inspect" && name != "u8" && name != "u8_s8" && name != "s8")
            throw std::runtime_error("Unknown control");
        const bool hardware = backend == "Alnpu" && name != "inspect";
        armnn::ConfigureLogging(true, false, armnn::LogSeverity::Info);
        std::cout << "case=" << name << " requested_backend=" << backend
                  << " cpu_fallback_allowed=false onnx_parser_used=false\n";

        stage = "runtime"; failure = 30;
        CmaGuard cma;
        if (hardware) cma.initialize();
        armnn::IRuntime::CreationOptions creation;
        if (hardware) {
            auto allocator = std::make_shared<armnn::CmaAllocator>();
            creation.m_CustomAllocatorMap["CpuRef"] = allocator;
            creation.m_CustomAllocatorMap["CpuAcc"] = allocator;
        }
        auto runtime = armnn::IRuntime::Create(creation);
        bool supported = false;
        std::cout << "available_backends=";
        for (const auto& id : runtime->GetDeviceSpec().GetSupportedBackends()) {
            std::cout << id.Get() << ',';
            if (id.Get() == backend) supported = true;
        }
        std::cout << '\n';
        if (name == "inspect") { std::cout << "result=runtime_inspected inference_requested=false\n"; return 0; }
        if (!supported) throw std::runtime_error("Requested backend is not registered");

        stage = "construct"; failure = 20;
        const bool signedInput = name == "s8", signedWeights = name != "u8";
        const int offset = signedInput ? 0 : 128, weightOffset = signedWeights ? 0 : 128;
        const auto inputType = signedInput ? armnn::DataType::QAsymmS8 : armnn::DataType::QAsymmU8;
        const auto weightType = signedWeights ? armnn::DataType::QSymmS8 : armnn::DataType::QAsymmU8;
        auto inputInfo = info({1, channels, height, width}, inputType, 0.25f, offset, true);
        auto outputInfo = info({1, channels, height, width}, inputType, 0.25f, offset);
        auto weightInfo = info({channels, channels, kernel, kernel}, weightType, 0.125f, weightOffset, true);
        auto biasInfo = info({channels}, armnn::DataType::Signed32, 0.03125f, 0, true);
        std::vector<std::uint8_t> inputs(elements), outputs(elements), weights(channels * channels * kernel * kernel);
        std::vector<std::int32_t> biases(channels);
        std::vector<int> expected(elements);
        for (int c = 0; c < channels; ++c) {
            biases[c] = c - 3;
            for (int h = 0; h < height; ++h) for (int w = 0; w < width; ++w)
                inputs[index(c,h,w)] = static_cast<std::uint8_t>(inputValue(c,h,w) + offset);
            for (int i = 0; i < channels; ++i) for (int h = 0; h < kernel; ++h) for (int w = 0; w < kernel; ++w)
                weights[((c * channels + i) * kernel + h) * kernel + w] =
                    static_cast<std::uint8_t>(weightValue(c,i,h,w) + weightOffset);
        }
        // Separate integer convolution oracle: zero padding in real values,
        // accumulator scale 0.25 * 0.125, output scale 0.25, one-LSB tolerance.
        for (int o = 0; o < channels; ++o) for (int h = 0; h < height; ++h) for (int w = 0; w < width; ++w) {
            int accumulator = biases[o];
            for (int i = 0; i < channels; ++i) for (int kh = 0; kh < kernel; ++kh) for (int kw = 0; kw < kernel; ++kw) {
                const int ih = h + kh - 1, iw = w + kw - 1;
                if (ih >= 0 && ih < height && iw >= 0 && iw < width)
                    accumulator += inputValue(i,ih,iw) * weightValue(o,i,kh,kw);
            }
            const int quantized = static_cast<int>(std::round(accumulator / 8.0)) + offset;
            expected[index(o,h,w)] = std::max(signedInput ? -128 : 0, std::min(signedInput ? 127 : 255, quantized));
        }
        auto network = armnn::INetwork::Create();
        auto* input = network->AddInputLayer(0, "smoke_input");
        input->GetOutputSlot(0).SetTensorInfo(inputInfo);
        auto* constant = network->AddConstantLayer(armnn::ConstTensor(weightInfo, weights.data()), "smoke_weights");
        constant->GetOutputSlot(0).SetTensorInfo(weightInfo);
        auto* bias = network->AddConstantLayer(armnn::ConstTensor(biasInfo, biases.data()), "smoke_bias");
        bias->GetOutputSlot(0).SetTensorInfo(biasInfo);
        armnn::Convolution2dDescriptor descriptor;
        descriptor.m_DataLayout = armnn::DataLayout::NCHW;
        descriptor.m_BiasEnabled = true;
        descriptor.m_PadLeft = descriptor.m_PadRight = descriptor.m_PadTop = descriptor.m_PadBottom = 1;
        descriptor.m_StrideX = descriptor.m_StrideY = 1;
        auto* convolution = network->AddConvolution2dLayer(descriptor, "smoke_quantized_conv3x3");
        input->GetOutputSlot(0).Connect(convolution->GetInputSlot(0));
        constant->GetOutputSlot(0).Connect(convolution->GetInputSlot(1));
        bias->GetOutputSlot(0).Connect(convolution->GetInputSlot(2));
        convolution->GetOutputSlot(0).SetTensorInfo(outputInfo);
        convolution->GetOutputSlot(0).Connect(network->AddOutputLayer(0, "smoke_output")->GetInputSlot(0));
        std::cout << "stage=construct passed shape=1,8,8,8 layout=NCHW kernel=3,3 input_scale=0.25"
                  << " weight_scale=0.125 output_scale=0.25 output_offset=" << offset << '\n';

        stage = "optimize"; failure = 40;
        armnn::OptimizerOptionsOpaque optimizer;
        optimizer.SetImportEnabled(hardware);
        optimizer.SetExportEnabled(hardware);
        optimizer.SetProfilingEnabled(true);
        std::vector<std::string> messages;
        auto optimized = armnn::Optimize(*network, {armnn::BackendId(backend)}, runtime->GetDeviceSpec(), optimizer,
                                         armnn::Optional<std::vector<std::string>&>(messages));
        for (const auto& message : messages) std::cout << "optimizer_message=" << message << '\n';
        if (!optimized) throw std::runtime_error("Optimize returned null");
        std::ostringstream graph;
        if (optimized->SerializeToDot(graph) != armnn::Status::Success) throw std::runtime_error("Cannot serialize graph");
        const std::string dot = graph.str();
        create(directory + "/graph.dot") << dot;
        if (dot.find("BackendID : " + backend) == std::string::npos)
            throw std::runtime_error("Requested backend is absent from the optimized graph");
        std::cout << "stage=optimize passed graph_has_requested_backend=true\n";

        stage = "load"; failure = 50;
        armnn::NetworkId id;
        std::string error;
        const auto source = hardware ? armnn::MemorySource::Malloc : armnn::MemorySource::Undefined;
        armnn::INetworkProperties properties(false, source, source, true);
        if (runtime->LoadNetwork(id, std::move(optimized), error, properties) != armnn::Status::Success)
            throw std::runtime_error("LoadNetwork failed: " + error);
        auto profiler = runtime->GetProfiler(id);
        if (profiler) profiler->EnableProfiling(true);
        std::unique_ptr<CmaBuffer> inputCma, outputCma;
        if (hardware) { inputCma.reset(new CmaBuffer(elements)); outputCma.reset(new CmaBuffer(elements)); }
        void* inputPointer = hardware ? inputCma->value.virt_addr : inputs.data();
        void* outputPointer = hardware ? outputCma->value.virt_addr : outputs.data();
        std::memcpy(inputPointer, inputs.data(), inputs.size());
        armnn::InputTensors bindings{{0, armnn::ConstTensor(inputInfo, inputPointer)}};
        armnn::OutputTensors results{{0, armnn::Tensor(outputInfo, outputPointer)}};
        std::cout << "stage=load passed\n";

        stage = "inference"; failure = 60;
        auto csv = create(directory + "/output.csv");
        csv << "iteration,element,actual_quantized,expected_quantized,error_lsb\n";
        int maxError = 0, mismatches = 0;
        double totalUs = 0;
        for (int iteration = 0; iteration < 3; ++iteration) {
            std::memset(outputPointer, 0xa5, elements);
            if (hardware) {
                cma_mem_sync_for_device(inputCma->value.phys_addr, elements);
                cma_mem_sync_for_device(outputCma->value.phys_addr, elements);
            }
            const auto before = std::chrono::steady_clock::now();
            const auto status = runtime->EnqueueWorkload(id, bindings, results);
            const auto after = std::chrono::steady_clock::now();
            if (status != armnn::Status::Success) throw std::runtime_error("EnqueueWorkload failed");
            if (hardware) cma_mem_sync_for_cpu(outputCma->value.phys_addr, elements);
            const double us = std::chrono::duration<double, std::micro>(after-before).count();
            totalUs += us;
            int iterationError = 0, iterationMismatches = 0;
            const auto* bytes = static_cast<const std::uint8_t*>(outputPointer);
            for (int i = 0; i < elements; ++i) {
                const int value = decoded(bytes[i], signedInput), difference = std::abs(value - expected[i]);
                iterationError = std::max(iterationError, difference);
                if (difference > 1) ++iterationMismatches;
                csv << iteration << ',' << i << ',' << value << ',' << expected[i] << ',' << difference << '\n';
            }
            maxError = std::max(maxError, iterationError);
            mismatches += iterationMismatches;
            std::cout << "iteration=" << iteration << " output_elements=" << elements
                      << " max_error_lsb=" << iterationError << " mismatches=" << iterationMismatches << " enqueue_us=" << us << '\n';
        }
        if (profiler) { auto profile = create(directory + "/profile.json"); profiler->Print(profile); }
        create(directory + "/output_bytes.bin").write(static_cast<const char*>(outputPointer), elements);
        runtime->UnloadNetwork(id);
        std::cout << "result=" << (mismatches ? "numerical_mismatch" : "passed") << " case=" << name
                  << " backend=" << backend << " iterations=3 max_error_lsb=" << maxError
                  << " tolerance_lsb=1 mismatches=" << mismatches << " mean_enqueue_us=" << totalUs/3
                  << " npu_requested=" << (hardware ? "true" : "false") << '\n';
        return mismatches ? 70 : 0;
    } catch (const std::exception& error) {
        std::cerr << "result=failed stage=" << stage << " error=" << error.what() << '\n';
        return failure;
    }
}
