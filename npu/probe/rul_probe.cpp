// Dataset-replay probe for the vendor Arm NN ABI. No acquisition or HMI dependency.
#include <armnn/ArmNN.hpp>
#include <armnn/IProfiler.hpp>
#include <armnn/Logging.hpp>
#include <armnnALUtils/cma_mem_init.hpp>
#include <armnnOnnxParser/IOnnxParser.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::size_t kInputFloats = 23 * 110;
struct Options {
    std::string model, input, reference, labels, csv, graph, profile;
    std::string backends = "CpuRef";
    std::size_t start = 0, count = 1;
    double atol = 1e-4, rtol = 1e-4;
    bool parseOnly = false;
};

std::size_t number(const std::string& value) {
    if (value.empty() || value[0] == '-') throw std::runtime_error("Invalid nonnegative integer");
    std::size_t consumed = 0;
    const auto parsed = std::stoull(value, &consumed);
    if (consumed != value.size() || parsed > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("Invalid integer: " + value);
    return static_cast<std::size_t>(parsed);
}

Options arguments(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--parse-only") { options.parseOnly = true; continue; }
        if (key == "--help") {
            std::cout << "rul_probe --model model.onnx [--parse-only]\n"
                         "  [--input input_f32.bin --reference reference_f32.bin --labels labels_f32.bin]\n"
                         "  [--backends CpuRef | Alnpu,CpuAcc,CpuRef] [--start 0 --count 1]\n"
                         "  [--csv results.csv --graph graph.dot --profile profile.json]\n"
                         "  [--atol 0.0001 --rtol 0.0001] (count=0: all remaining samples)\n";
            std::exit(0);
        }
        if (i + 1 == argc) throw std::runtime_error("Missing value for " + key);
        const std::string value = argv[++i];
        if (key == "--model") options.model = value;
        else if (key == "--input") options.input = value;
        else if (key == "--reference") options.reference = value;
        else if (key == "--labels") options.labels = value;
        else if (key == "--backends") options.backends = value;
        else if (key == "--csv") options.csv = value;
        else if (key == "--graph") options.graph = value;
        else if (key == "--profile") options.profile = value;
        else if (key == "--start") options.start = number(value);
        else if (key == "--count") options.count = number(value);
        else if (key == "--atol") options.atol = std::stod(value);
        else if (key == "--rtol") options.rtol = std::stod(value);
        else throw std::runtime_error("Unknown option: " + key);
    }
    if (options.model.empty()) throw std::runtime_error("--model is required");
    if (!options.parseOnly && options.input.empty()) throw std::runtime_error("--input is required for inference");
    if (!std::isfinite(options.atol) || !std::isfinite(options.rtol) || options.atol < 0 || options.rtol < 0)
        throw std::runtime_error("Tolerances must be finite and nonnegative");
    return options;
}

std::vector<float> readFloats(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open " + path);
    const auto length = file.tellg();
    if (length <= 0 || length % sizeof(float)) throw std::runtime_error("Invalid float32 file size: " + path);
    std::vector<float> values(static_cast<std::size_t>(length) / sizeof(float));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(values.data()), length)) throw std::runtime_error("Cannot read " + path);
    for (float value : values) if (!std::isfinite(value)) throw std::runtime_error("Nonfinite input in " + path);
    return values;
}

std::ofstream outputFile(const std::string& path) {
    if (std::ifstream(path).good()) throw std::runtime_error("Refusing to overwrite " + path);
    std::ofstream file(path);
    if (!file) throw std::runtime_error("Cannot create " + path);
    return file;
}

void printTensor(const std::string& direction, const std::string& name, const armnn::TensorInfo& info) {
    std::cout << direction << "=" << name << " dtype=" << static_cast<int>(info.GetDataType()) << " shape=[";
    for (unsigned i = 0; i < info.GetNumDimensions(); ++i) {
        if (i) std::cout << ',';
        std::cout << info.GetShape()[i];
    }
    std::cout << "] bytes=" << info.GetNumBytes() << '\n';
}

struct CmaGuard {
    bool active = false;
    void initialize() {
        if (!cma_mem_is_init()) {
            cma_mem_init();
            active = cma_mem_is_init();
            if (!active) throw std::runtime_error("CMA initialization failed; check /dev/cma_mem and permissions");
        }
    }
    ~CmaGuard() { if (active) cma_mem_deinit(); }
};

struct CmaBuffer {
    cma_buffer_t value;
    explicit CmaBuffer(std::uint32_t bytes) : value(bytes) {
        cma_mem_alloc(&value);
        if (!value.virt_addr || !value.phys_addr) throw std::runtime_error("CMA allocation failed");
    }
    ~CmaBuffer() { cma_mem_free(&value); }
    CmaBuffer(const CmaBuffer&) = delete;
    CmaBuffer& operator=(const CmaBuffer&) = delete;
};
}  // namespace

int main(int argc, char** argv) {
    std::string stage = "arguments";
    int failureCode = 2;
    std::cout << std::unitbuf << std::setprecision(10);
    try {
        const auto options = arguments(argc, argv);
        const std::uint16_t endian = 1;
        if (sizeof(float) != 4 || *reinterpret_cast<const unsigned char*>(&endian) != 1)
            throw std::runtime_error("Little-endian float32 platform required");
        armnn::ConfigureLogging(true, false, armnn::LogSeverity::Info);

        stage = "parse"; failureCode = 20;
        std::cout << "stage=parse begin model=" << options.model << '\n';
        auto parser = armnnOnnxParser::IOnnxParser::Create();
        auto network = parser->CreateNetworkFromBinaryFile(options.model.c_str());
        if (!network) throw std::runtime_error("Parser returned a null network");
        std::cout << "stage=parse passed\n";
        const auto inputNames = parser->GetInputTensorNames();
        const auto outputNames = parser->GetOutputTensorNames();
        for (const auto& name : inputNames) printTensor("input", name, parser->GetNetworkInputBindingInfo(name).second);
        for (const auto& name : outputNames) printTensor("output", name, parser->GetNetworkOutputBindingInfo(name).second);
        if (options.parseOnly) { std::cout << "result=parse_passed inference_not_requested\n"; return 0; }

        stage = "contract"; failureCode = 21;
        if (inputNames.size() != 1 || outputNames.size() != 1)
            throw std::runtime_error("This replay probe requires one input and one output");
        auto inputBinding = parser->GetNetworkInputBindingInfo(inputNames.front());
        auto outputBinding = parser->GetNetworkOutputBindingInfo(outputNames.front());
        const auto& inputInfo = inputBinding.second;
        const auto& outputInfo = outputBinding.second;
        if (inputInfo.GetDataType() != armnn::DataType::Float32 || inputInfo.GetNumDimensions() != 3 ||
            inputInfo.GetShape()[0] != 1 || inputInfo.GetShape()[1] != 23 || inputInfo.GetShape()[2] != 110 ||
            outputInfo.GetDataType() != armnn::DataType::Float32 || outputInfo.GetNumDimensions() != 2 ||
            outputInfo.GetShape()[0] != 1 || outputInfo.GetShape()[1] != 1)
            throw std::runtime_error("Expected float32 input [1,23,110] and output [1,1]");
        auto inputs = readFloats(options.input);
        if (inputs.size() % kInputFloats) throw std::runtime_error("Input file does not contain complete windows");
        const auto total = inputs.size() / kInputFloats;
        if (options.start >= total) throw std::runtime_error("Start index is outside the replay data");
        const auto count = options.count ? options.count : total - options.start;
        if (count > total - options.start) throw std::runtime_error("Requested count exceeds replay data");
        auto reference = options.reference.empty() ? std::vector<float>{} : readFloats(options.reference);
        auto labels = options.labels.empty() ? std::vector<float>{} : readFloats(options.labels);
        if ((!reference.empty() && reference.size() != total) || (!labels.empty() && labels.size() != total))
            throw std::runtime_error("Reference and labels must match the total window count");

        std::vector<armnn::BackendId> backends;
        std::stringstream backendStream(options.backends);
        std::string backend;
        bool usesAlnpu = false;
        while (std::getline(backendStream, backend, ',')) {
            if (backend != "CpuRef" && backend != "CpuAcc" && backend != "Alnpu")
                throw std::runtime_error("Unsupported requested backend: " + backend);
            usesAlnpu |= backend == "Alnpu";
            backends.emplace_back(backend);
        }
        if (backends.empty()) throw std::runtime_error("Backend list cannot be empty");
        CmaGuard cma;
        stage = "runtime"; failureCode = 30;
        if (usesAlnpu) cma.initialize();
        armnn::IRuntime::CreationOptions creation;
        if (usesAlnpu) {
            auto allocator = std::make_shared<armnn::CmaAllocator>();
            creation.m_CustomAllocatorMap["CpuRef"] = allocator;
            creation.m_CustomAllocatorMap["CpuAcc"] = allocator;
        }
        auto runtime = armnn::IRuntime::Create(creation);
        std::cout << "requested_backends=" << options.backends << " available_backends=";
        for (const auto& id : runtime->GetDeviceSpec().GetSupportedBackends()) std::cout << id.Get() << ',';
        std::cout << "\nbackend_preference_is_not_proof_of_npu_execution=true\n";

        stage = "optimize"; failureCode = 40;
        armnn::OptimizerOptionsOpaque optimizer;
        optimizer.SetImportEnabled(usesAlnpu);
        optimizer.SetExportEnabled(usesAlnpu);
        optimizer.SetProfilingEnabled(true);
        std::vector<std::string> messages;
        auto optimized = armnn::Optimize(*network, backends, runtime->GetDeviceSpec(), optimizer,
                                         armnn::Optional<std::vector<std::string>&>(messages));
        for (const auto& message : messages) std::cout << "optimizer_message=" << message << '\n';
        if (!optimized) throw std::runtime_error("Optimize returned a null network");
        if (!options.graph.empty()) {
            auto file = outputFile(options.graph);
            if (optimized->SerializeToDot(file) != armnn::Status::Success)
                throw std::runtime_error("Cannot serialize optimized graph");
        }

        stage = "load"; failureCode = 50;
        armnn::NetworkId networkId;
        std::string loadError;
        const auto memorySource = usesAlnpu ? armnn::MemorySource::Malloc : armnn::MemorySource::Undefined;
        armnn::INetworkProperties properties(false, memorySource, memorySource, true);
        if (runtime->LoadNetwork(networkId, std::move(optimized), loadError, properties) != armnn::Status::Success)
            throw std::runtime_error("LoadNetwork failed: " + loadError);
        auto profiler = runtime->GetProfiler(networkId);
        if (profiler) profiler->EnableProfiling(true);
        std::vector<float> inputStorage(kInputFloats), outputStorage(1);
        std::unique_ptr<CmaBuffer> inputCma, outputCma;
        if (usesAlnpu) {
            inputCma.reset(new CmaBuffer(static_cast<std::uint32_t>(kInputFloats * sizeof(float))));
            outputCma.reset(new CmaBuffer(sizeof(float)));
        }
        void* inputPointer = usesAlnpu ? inputCma->value.virt_addr : inputStorage.data();
        void* outputPointer = usesAlnpu ? outputCma->value.virt_addr : outputStorage.data();
        inputBinding.second.SetConstant(true);
        armnn::InputTensors inputTensors{{inputBinding.first, armnn::ConstTensor(inputBinding.second, inputPointer)}};
        armnn::OutputTensors outputTensors{{outputBinding.first, armnn::Tensor(outputBinding.second, outputPointer)}};
        std::ofstream csv;
        if (!options.csv.empty()) {
            csv = outputFile(options.csv);
            csv << std::setprecision(10) << "sample_index,prediction,reference,absolute_error,target,infer_us\n";
        }
        double maxError = 0, sumAbsTarget = 0, sumSqTarget = 0, totalUs = 0;
        std::size_t mismatches = 0;
        stage = "inference"; failureCode = 60;
        for (std::size_t i = options.start; i < options.start + count; ++i) {
            std::memcpy(inputPointer, inputs.data() + i * kInputFloats, kInputFloats * sizeof(float));
            if (usesAlnpu) cma_mem_sync_for_device(inputCma->value.phys_addr, kInputFloats * sizeof(float));
            const auto before = std::chrono::steady_clock::now();
            const auto status = runtime->EnqueueWorkload(networkId, inputTensors, outputTensors);
            const auto after = std::chrono::steady_clock::now();
            if (status != armnn::Status::Success) throw std::runtime_error("EnqueueWorkload failed");
            if (usesAlnpu) cma_mem_sync_for_cpu(outputCma->value.phys_addr, sizeof(float));
            const float prediction = *static_cast<const float*>(outputPointer);
            if (!std::isfinite(prediction)) throw std::runtime_error("Nonfinite prediction");
            const double us = std::chrono::duration<double, std::micro>(after - before).count();
            totalUs += us;
            const double error = reference.empty() ? 0 : std::abs(static_cast<double>(prediction) - reference[i]);
            if (!reference.empty()) {
                maxError = std::max(maxError, error);
                if (error > options.atol + options.rtol * std::abs(reference[i])) ++mismatches;
            }
            if (!labels.empty()) {
                const double residual = prediction - static_cast<double>(labels[i]);
                sumAbsTarget += std::abs(residual); sumSqTarget += residual * residual;
            }
            std::cout << "sample=" << i << " prediction=" << prediction << " infer_us=" << us;
            if (!reference.empty()) std::cout << " reference=" << reference[i] << " absolute_error=" << error;
            if (!labels.empty()) std::cout << " target=" << labels[i];
            std::cout << '\n';
            if (csv) {
                csv << i << ',' << prediction << ',';
                if (!reference.empty()) csv << reference[i];
                csv << ','; if (!reference.empty()) csv << error;
                csv << ','; if (!labels.empty()) csv << labels[i];
                csv << ',' << us << '\n';
            }
        }
        if (profiler && !options.profile.empty()) { auto file = outputFile(options.profile); profiler->Print(file); }
        runtime->UnloadNetwork(networkId);
        std::cout << "result=inference_completed samples=" << count << " mean_enqueue_us=" << totalUs / count;
        if (!reference.empty()) std::cout << " max_reference_error=" << maxError << " mismatches=" << mismatches;
        if (!labels.empty()) std::cout << " rmse=" << std::sqrt(sumSqTarget / count) << " mae=" << sumAbsTarget / count;
        std::cout << "\nrul_unit=observed_aging_test_session\n";
        return mismatches ? 70 : 0;
    } catch (const std::exception& error) {
        std::cerr << "result=failed stage=" << stage << " error=" << error.what() << '\n';
        return failureCode;
    }
}
