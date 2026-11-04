// Independent native-FC zero-padding controls; keep the original graph dimensions for the integer oracle.
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
constexpr int zero = 128, iterations = 3;
constexpr float inputScale = 0.25f, weightScale = 0.125f;

unsigned dimension(const char* text, unsigned maximum) {
    const std::string value(text);
    std::size_t used = 0;
    const auto result = std::stoul(value, &used);
    if (used != value.size() || result < 1 || result > maximum)
        throw std::runtime_error("Dimension outside bounded control range");
    return static_cast<unsigned>(result);
}

armnn::TensorInfo info(std::initializer_list<unsigned> shape, armnn::DataType type,
                       float scale, int offset, bool constant = false) {
    return armnn::TensorInfo(armnn::TensorShape(static_cast<unsigned>(shape.size()), shape.begin()),
                             type, scale, offset, constant);
}

std::ofstream create(const std::string& path) {
    if (std::ifstream(path).good()) throw std::runtime_error("Refusing to overwrite: " + path);
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot create: " + path);
    return out;
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
    explicit CmaBuffer(unsigned size) : value(size) {
        cma_mem_alloc(&value);
        if (!value.virt_addr || !value.phys_addr) throw std::runtime_error("CMA allocation failed");
    }
    ~CmaBuffer() { cma_mem_free(&value); }
    CmaBuffer(const CmaBuffer&) = delete;
    CmaBuffer& operator=(const CmaBuffer&) = delete;
};

int inputValue(unsigned iteration, unsigned row, unsigned i) {
    return static_cast<int>((iteration * 11 + row * 13 + i * 7 + (i / 17) * 3) % 17) - 8;
}
int weightValue(unsigned output, unsigned i) {
    return static_cast<int>((output * 11 + i * 5 + (i / 13) * 2) % 7) - 3;
}
int requantize(int accumulator, unsigned divisor) {
    const int rounded = static_cast<int>(std::round(static_cast<double>(accumulator) / divisor));
    return std::max(0, std::min(255, rounded + zero));
}

std::string attribute(const std::string& line, const std::string& name) {
    const auto start = line.find(name + " : ");
    if (start == std::string::npos) return "";
    const auto from = start + name.size() + 3;
    return line.substr(from, line.find("\\l", from) - from);
}
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf << std::setprecision(10);
    std::string stage = "arguments";
    int failure = 2;
    try {
        if (argc != 7) throw std::runtime_error("Usage: npu_linear fc|fc_pad8|conv|conv_pad8 B I O CpuRef|Alnpu RESULTS_DIR");
        const std::string mode = argv[1], backend = argv[5], directory = argv[6];
        if (mode != "fc" && mode != "fc_pad8" && mode != "conv" && mode != "conv_pad8") throw std::runtime_error("Unknown mapping");
        if (backend != "CpuRef" && backend != "Alnpu") throw std::runtime_error("Exactly one backend required");
        const unsigned rows = dimension(argv[2], 128), features = dimension(argv[3], 2048);
        const unsigned outputs = dimension(argv[4], 256);
        const bool convolution = mode == "conv" || mode == "conv_pad8", hardware = backend == "Alnpu";
        const unsigned ic = (mode == "conv_pad8" || mode == "fc_pad8") ? (features + 7) / 8 * 8 : features;
        const unsigned oc = (mode == "conv_pad8" || mode == "fc_pad8") ? (outputs + 7) / 8 * 8 : outputs;
        const unsigned divisor = features <= 64 ? 8 : features <= 256 ? 16 : features <= 512 ? 32 : 64;
        const float outputScale = inputScale * weightScale * divisor;
        const unsigned inputBytes = rows * ic, outputBytes = rows * oc;
        std::cout << "mode=" << mode << " rows=" << rows << " input_features=" << features
                  << " output_features=" << outputs << " physical_input_features=" << ic
                  << " physical_output_features=" << oc << " requested_backend=" << backend
                  << " cpu_fallback_allowed=false trained_model_weights_used=false\n";

        stage = "runtime"; failure = 30;
        armnn::ConfigureLogging(true, false, armnn::LogSeverity::Info);
        CmaGuard cma;
        if (hardware) cma.initialize();
        armnn::IRuntime::CreationOptions creation;
        if (hardware) {
            auto allocator = std::make_shared<armnn::CmaAllocator>();
            creation.m_CustomAllocatorMap["CpuRef"] = allocator;
            creation.m_CustomAllocatorMap["CpuAcc"] = allocator;
        }
        auto runtime = armnn::IRuntime::Create(creation);
        bool registered = false;
        std::cout << "available_backends=";
        for (const auto& id : runtime->GetDeviceSpec().GetSupportedBackends()) {
            std::cout << id.Get() << ',';
            if (id.Get() == backend) registered = true;
        }
        std::cout << '\n';
        if (!registered) throw std::runtime_error("Requested backend is not registered");

        stage = "construct"; failure = 20;
        auto inputInfo = convolution ? info({1, ic, 1, rows}, armnn::DataType::QAsymmU8, inputScale, zero, true)
                                     : info({rows, ic}, armnn::DataType::QAsymmU8, inputScale, zero, true);
        auto outputInfo = convolution ? info({1, oc, 1, rows}, armnn::DataType::QAsymmU8, outputScale, zero)
                                      : info({rows, oc}, armnn::DataType::QAsymmU8, outputScale, zero);
        auto weightInfo = convolution ? info({oc, ic, 1, 1}, armnn::DataType::QAsymmU8, weightScale, zero, true)
                                      : info({oc, ic}, armnn::DataType::QAsymmU8, weightScale, zero, true);
        auto biasInfo = info({oc}, armnn::DataType::Signed32, inputScale * weightScale, 0, true);
        std::vector<std::uint8_t> weights(oc * ic, zero), packedInput(inputBytes, zero), packedOutput(outputBytes);
        std::vector<std::int32_t> biases(oc, 0);
        for (unsigned o = 0; o < outputs; ++o) for (unsigned i = 0; i < features; ++i)
            weights[o * ic + i] = static_cast<std::uint8_t>(weightValue(o, i) + zero);
        auto network = armnn::INetwork::Create();
        auto* input = network->AddInputLayer(0, "linear_input");
        input->GetOutputSlot(0).SetTensorInfo(inputInfo);
        auto* constant = network->AddConstantLayer(armnn::ConstTensor(weightInfo, weights.data()), "linear_weights");
        constant->GetOutputSlot(0).SetTensorInfo(weightInfo);
        auto* bias = network->AddConstantLayer(armnn::ConstTensor(biasInfo, biases.data()), "zero_bias");
        bias->GetOutputSlot(0).SetTensorInfo(biasInfo);
        armnn::IConnectableLayer* compute = nullptr;
        if (convolution) {
            armnn::Convolution2dDescriptor descriptor;
            descriptor.m_DataLayout = armnn::DataLayout::NCHW;
            descriptor.m_StrideX = descriptor.m_StrideY = 1;
            descriptor.m_BiasEnabled = true;
            compute = network->AddConvolution2dLayer(descriptor, "linear_as_conv1x1");
        } else {
            armnn::FullyConnectedDescriptor descriptor;
            descriptor.m_TransposeWeightMatrix = true;
            descriptor.m_BiasEnabled = true;
            compute = network->AddFullyConnectedLayer(descriptor, "linear_fc");
        }
        input->GetOutputSlot(0).Connect(compute->GetInputSlot(0));
        constant->GetOutputSlot(0).Connect(compute->GetInputSlot(1));
        bias->GetOutputSlot(0).Connect(compute->GetInputSlot(2));
        compute->GetOutputSlot(0).SetTensorInfo(outputInfo);
        compute->GetOutputSlot(0).Connect(network->AddOutputLayer(0, "linear_output")->GetInputSlot(0));
        std::cout << "stage=construct passed quantization=QAsymmU8 weight_zero_point=128 input_zero_point=128"
                  << " input_scale=" << inputScale << " weight_scale=" << weightScale
                  << " output_scale=" << outputScale << " requantization_divisor=" << divisor
                  << " zero_bias=true physical_input_bytes=" << inputBytes << " physical_output_bytes=" << outputBytes << '\n';

        stage = "optimize"; failure = 40;
        armnn::OptimizerOptionsOpaque options;
        options.SetImportEnabled(hardware); options.SetExportEnabled(hardware); options.SetProfilingEnabled(true);
        std::vector<std::string> messages;
        auto optimized = armnn::Optimize(*network, {armnn::BackendId(backend)}, runtime->GetDeviceSpec(), options,
                                         armnn::Optional<std::vector<std::string>&>(messages));
        for (const auto& message : messages) std::cout << "optimizer_message=" << message << '\n';
        if (!optimized) throw std::runtime_error("Optimize returned null");
        std::ostringstream graph;
        if (optimized->SerializeToDot(graph) != armnn::Status::Success) throw std::runtime_error("Cannot serialize graph");
        create(directory + "/graph.dot") << graph.str();
        std::istringstream graphLines(graph.str());
        unsigned computeLayers = 0;
        for (std::string line; std::getline(graphLines, line);) {
            const auto kind = attribute(line, "LayerType"), assigned = attribute(line, "BackendID");
            if (kind.empty() || kind == "Input" || kind == "Output" || kind == "Constant" ||
                kind == "MemCopy" || kind == "MemImport" || kind == "MemSync") continue;
            if (assigned != backend) throw std::runtime_error("Unexpected compute backend: " + assigned);
            ++computeLayers;
            std::cout << "compute_layer_type=" << kind << " actual_backend=" << assigned << '\n';
        }
        if (!computeLayers) throw std::runtime_error("No compute layer in optimized graph");
        std::cout << "stage=optimize passed compute_layers=" << computeLayers << '\n';

        stage = "load"; failure = 50;
        armnn::NetworkId id;
        std::string error;
        const auto memory = hardware ? armnn::MemorySource::Malloc : armnn::MemorySource::Undefined;
        armnn::INetworkProperties properties(false, memory, memory, true);
        if (runtime->LoadNetwork(id, std::move(optimized), error, properties) != armnn::Status::Success)
            throw std::runtime_error("LoadNetwork failed: " + error);
        auto profiler = runtime->GetProfiler(id);
        if (profiler) profiler->EnableProfiling(true);
        std::unique_ptr<CmaBuffer> inputCma, outputCma;
        if (hardware) { inputCma.reset(new CmaBuffer(inputBytes)); outputCma.reset(new CmaBuffer(outputBytes)); }
        void* inputPointer = hardware ? inputCma->value.virt_addr : packedInput.data();
        void* outputPointer = hardware ? outputCma->value.virt_addr : packedOutput.data();
        armnn::InputTensors feeds{{0, armnn::ConstTensor(inputInfo, inputPointer)}};
        armnn::OutputTensors bindings{{0, armnn::Tensor(outputInfo, outputPointer)}};
        std::cout << "stage=load passed\n";

        stage = "inference"; failure = 60;
        auto csv = create(directory + "/output.csv");
        csv << "iteration,row,output_feature,is_padding,actual_quantized,expected_quantized,error_lsb\n";
        std::vector<float> canonicalInput(rows * features), canonicalOutput(rows * outputs);
        int maximum = 0, mismatches = 0;
        unsigned saturated = 0;
        double enqueueTotal = 0, completeTotal = 0;
        const auto physicalIndex = [&](unsigned row, unsigned channel, unsigned channels) {
            return convolution ? channel * rows + row : row * channels + channel;
        };
        for (unsigned iteration = 0; iteration < iterations; ++iteration) {
            for (unsigned row = 0; row < rows; ++row) for (unsigned i = 0; i < features; ++i)
                canonicalInput[row * features + i] = inputValue(iteration, row, i) * inputScale;
            const auto begin = std::chrono::steady_clock::now();
            std::fill(packedInput.begin(), packedInput.end(), zero);
            for (unsigned row = 0; row < rows; ++row) for (unsigned i = 0; i < features; ++i)
                packedInput[physicalIndex(row, i, ic)] = static_cast<std::uint8_t>(
                    std::lround(canonicalInput[row * features + i] / inputScale) + zero);
            if (hardware) std::memcpy(inputPointer, packedInput.data(), inputBytes);
            std::memset(outputPointer, 0xa5, outputBytes);
            if (hardware) {
                cma_mem_sync_for_device(inputCma->value.phys_addr, inputBytes);
                cma_mem_sync_for_device(outputCma->value.phys_addr, outputBytes);
            }
            const auto beforeEnqueue = std::chrono::steady_clock::now();
            const auto status = runtime->EnqueueWorkload(id, feeds, bindings);
            const auto afterEnqueue = std::chrono::steady_clock::now();
            if (status != armnn::Status::Success) throw std::runtime_error("EnqueueWorkload failed");
            if (hardware) cma_mem_sync_for_cpu(outputCma->value.phys_addr, outputBytes);
            const auto* bytes = static_cast<const std::uint8_t*>(outputPointer);
            for (unsigned row = 0; row < rows; ++row) for (unsigned o = 0; o < outputs; ++o)
                canonicalOutput[row * outputs + o] = (int(bytes[physicalIndex(row, o, oc)]) - zero) * outputScale;
            const auto end = std::chrono::steady_clock::now();
            const double enqueueUs = std::chrono::duration<double, std::micro>(afterEnqueue - beforeEnqueue).count();
            const double completeUs = std::chrono::duration<double, std::micro>(end - begin).count();
            enqueueTotal += enqueueUs; completeTotal += completeUs;
            int iterationMaximum = 0, iterationMismatches = 0;
            for (unsigned row = 0; row < rows; ++row) for (unsigned o = 0; o < oc; ++o) {
                int accumulator = 0;
                if (o < outputs) for (unsigned i = 0; i < features; ++i)
                    accumulator += inputValue(iteration, row, i) * weightValue(o, i);
                const int expected = requantize(accumulator, divisor);
                const int actual = bytes[physicalIndex(row, o, oc)], difference = std::abs(actual - expected);
                if (o < outputs && (expected == 0 || expected == 255)) ++saturated;
                iterationMaximum = std::max(iterationMaximum, difference);
                if (difference > 1) ++iterationMismatches;
                csv << iteration << ',' << row << ',' << o << ',' << (o >= outputs) << ','
                    << actual << ',' << expected << ',' << difference << '\n';
            }
            maximum = std::max(maximum, iterationMaximum); mismatches += iterationMismatches;
            std::cout << "iteration=" << iteration << " max_error_lsb=" << iterationMaximum
                      << " mismatches=" << iterationMismatches << " enqueue_us=" << enqueueUs
                      << " quantize_pack_enqueue_sync_restore_us=" << completeUs << '\n';
        }
        if (profiler) { auto stream = create(directory + "/profile.json"); profiler->Print(stream); }
        create(directory + "/output_bytes.bin").write(static_cast<const char*>(outputPointer), outputBytes);
        create(directory + "/dequantized_output_f32.bin").write(reinterpret_cast<const char*>(canonicalOutput.data()),
                                                                  canonicalOutput.size() * sizeof(float));
        runtime->UnloadNetwork(id);
        std::cout << "result=" << (mismatches ? "numerical_mismatch" : "passed") << " mode=" << mode
                  << " backend=" << backend << " iterations=" << iterations << " max_error_lsb=" << maximum
                  << " tolerance_lsb=1 mismatches=" << mismatches << " saturated_original_outputs=" << saturated
                  << " mean_enqueue_us=" << enqueueTotal / iterations
                  << " mean_quantize_pack_enqueue_sync_restore_us=" << completeTotal / iterations
                  << " profiling_enabled=true timing_is_diagnostic=true\n";
        return mismatches ? 70 : 0;
    } catch (const std::exception& error) {
        std::cerr << "result=failed stage=" << stage << " error=" << error.what() << '\n';
        return failure;
    }
}
