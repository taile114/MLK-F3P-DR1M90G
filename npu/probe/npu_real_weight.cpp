// Real checkpoint-weight UINT8 FC probe; shared utilities remain byte-identical.
#define main synthetic_fc_probe_main
#include "npu_linear_padded.cpp"
#undef main

namespace {
template<class T> std::vector<T> readValues(const std::string& path, std::size_t count) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != std::streamoff(count * sizeof(T))) throw std::runtime_error("Invalid input size: " + path);
    std::vector<T> values(count); file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(values.data()), count * sizeof(T))) throw std::runtime_error("Cannot read " + path);
    return values;
}
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf << std::setprecision(10);
    std::string stage = "arguments"; int failure = 2;
    try {
        if (argc != 4) throw std::runtime_error("Usage: real_weight CASE_DIR CpuRef|Alnpu RESULTS_DIR");
        const std::string root = argv[1], backend = argv[2], directory = argv[3];
        if (backend != "CpuRef" && backend != "Alnpu") throw std::runtime_error("Single backend required");
        const bool hardware = backend == "Alnpu";
        unsigned rows, features, outputs, samples;
        float sx, sw, sy;
        std::ifstream meta(root + "/case.txt");
        if (!(meta >> rows >> features >> outputs >> samples >> sx >> sw >> sy)
            || rows < 1 || rows > 128 || features < 1 || features > 2048 || outputs < 1 || outputs > 256
            || samples != 3 || !std::isfinite(sx) || !std::isfinite(sw) || !std::isfinite(sy)
            || sx <= 0 || sw <= 0 || sy <= 0) throw std::runtime_error("Invalid real-weight metadata");
        const unsigned ic = (features + 7) / 8 * 8, oc = (outputs + 7) / 8 * 8;
        const unsigned inputBytes = rows * ic, outputBytes = rows * oc;
        auto weights = readValues<std::uint8_t>(root + "/weights_u8.bin", oc * ic);
        auto inputs = readValues<std::uint8_t>(root + "/inputs_u8.bin", samples * inputBytes);
        auto expected = readValues<std::uint8_t>(root + "/expected_u8.bin", samples * outputBytes);
        auto reference = readValues<float>(root + "/reference_f32.bin", samples * rows * outputs);
        std::cout << "requested_backend=" << backend << " cpu_fallback_allowed=false trained_weights_used=true"
                  << " rows=" << rows << " input_features=" << features << " output_features=" << outputs
                  << " physical_input_features=" << ic << " physical_output_features=" << oc
                  << " samples=" << samples << " input_scale=" << sx << " weight_scale=" << sw << " output_scale=" << sy << '\n';
        stage = "runtime"; failure = 30;
        armnn::ConfigureLogging(true, false, armnn::LogSeverity::Info);
        CmaGuard cma; if (hardware) cma.initialize();
        armnn::IRuntime::CreationOptions creation;
        if (hardware) {
            auto allocator = std::make_shared<armnn::CmaAllocator>();
            creation.m_CustomAllocatorMap["CpuRef"] = allocator;
            creation.m_CustomAllocatorMap["CpuAcc"] = allocator;
        }
        auto runtime = armnn::IRuntime::Create(creation);
        stage = "construct"; failure = 20;
        auto inputInfo = info({rows, ic}, armnn::DataType::QAsymmU8, sx, 128, true);
        auto outputInfo = info({rows, oc}, armnn::DataType::QAsymmU8, sy, 128);
        auto weightInfo = info({oc, ic}, armnn::DataType::QAsymmU8, sw, 128, true);
        auto biasInfo = info({oc}, armnn::DataType::Signed32, sx * sw, 0, true);
        std::vector<std::int32_t> bias(oc, 0);
        auto network = armnn::INetwork::Create();
        auto* input = network->AddInputLayer(0, "real_input"); input->GetOutputSlot(0).SetTensorInfo(inputInfo);
        auto* weight = network->AddConstantLayer(armnn::ConstTensor(weightInfo, weights.data()), "real_weights");
        weight->GetOutputSlot(0).SetTensorInfo(weightInfo);
        auto* biases = network->AddConstantLayer(armnn::ConstTensor(biasInfo, bias.data()), "zero_bias");
        biases->GetOutputSlot(0).SetTensorInfo(biasInfo);
        armnn::FullyConnectedDescriptor descriptor; descriptor.m_TransposeWeightMatrix = true; descriptor.m_BiasEnabled = true;
        auto* fc = network->AddFullyConnectedLayer(descriptor, "real_fc");
        input->GetOutputSlot(0).Connect(fc->GetInputSlot(0));
        weight->GetOutputSlot(0).Connect(fc->GetInputSlot(1)); biases->GetOutputSlot(0).Connect(fc->GetInputSlot(2));
        fc->GetOutputSlot(0).SetTensorInfo(outputInfo);
        fc->GetOutputSlot(0).Connect(network->AddOutputLayer(0)->GetInputSlot(0));
        stage = "optimize"; failure = 40;
        armnn::OptimizerOptionsOpaque options; options.SetImportEnabled(hardware); options.SetExportEnabled(hardware); options.SetProfilingEnabled(true);
        auto optimized = armnn::Optimize(*network, {armnn::BackendId(backend)}, runtime->GetDeviceSpec(), options);
        if (!optimized) throw std::runtime_error("Optimize returned null");
        std::ostringstream graph; optimized->SerializeToDot(graph); create(directory + "/graph.dot") << graph.str();
        std::istringstream lines(graph.str()); unsigned computes = 0;
        for (std::string line; std::getline(lines, line);) {
            const auto kind = attribute(line, "LayerType"), assigned = attribute(line, "BackendID");
            if (kind.empty() || kind == "Input" || kind == "Output" || kind == "Constant" || kind == "MemCopy" || kind == "MemImport" || kind == "MemSync") continue;
            if (assigned != backend || (hardware && kind != "ALHardNPU")) throw std::runtime_error("Unexpected compute backend");
            ++computes; std::cout << "compute_layer_type=" << kind << " actual_backend=" << assigned << '\n';
        }
        if (computes != 1) throw std::runtime_error("Expected one compute layer");
        stage = "load"; failure = 50;
        armnn::NetworkId id; std::string error;
        const auto source = hardware ? armnn::MemorySource::Malloc : armnn::MemorySource::Undefined;
        armnn::INetworkProperties properties(false, source, source, true);
        if (runtime->LoadNetwork(id, std::move(optimized), error, properties) != armnn::Status::Success) throw std::runtime_error(error);
        auto profiler = runtime->GetProfiler(id); if (profiler) profiler->EnableProfiling(true);
        std::vector<std::uint8_t> inputCpu(inputBytes), outputCpu(outputBytes);
        std::unique_ptr<CmaBuffer> inputCma, outputCma;
        if (hardware) { inputCma.reset(new CmaBuffer(inputBytes)); outputCma.reset(new CmaBuffer(outputBytes)); }
        void* inputPointer = hardware ? inputCma->value.virt_addr : inputCpu.data();
        void* outputPointer = hardware ? outputCma->value.virt_addr : outputCpu.data();
        armnn::InputTensors feeds{{0, armnn::ConstTensor(inputInfo, inputPointer)}};
        armnn::OutputTensors bindings{{0, armnn::Tensor(outputInfo, outputPointer)}};
        stage = "inference"; failure = 60;
        auto raw = create(directory + "/output_u8.bin");
        auto csv = create(directory + "/output.csv"); csv << "sample,row,channel,actual_u8,expected_u8,error_lsb\n";
        int maximum = 0; unsigned mismatches = 0; double squared = 0, maximumFloat = 0, totalUs = 0;
        for (unsigned sample = 0; sample < samples; ++sample) {
            std::memcpy(inputPointer, inputs.data() + sample * inputBytes, inputBytes); std::memset(outputPointer, 0xa5, outputBytes);
            if (hardware) { cma_mem_sync_for_device(inputCma->value.phys_addr, inputBytes); cma_mem_sync_for_device(outputCma->value.phys_addr, outputBytes); }
            const auto begin = std::chrono::steady_clock::now();
            if (runtime->EnqueueWorkload(id, feeds, bindings) != armnn::Status::Success) throw std::runtime_error("EnqueueWorkload failed");
            if (hardware) cma_mem_sync_for_cpu(outputCma->value.phys_addr, outputBytes);
            totalUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
            const auto* bytes = static_cast<const std::uint8_t*>(outputPointer);
            raw.write(reinterpret_cast<const char*>(bytes), outputBytes);
            for (unsigned row = 0; row < rows; ++row) for (unsigned o = 0; o < oc; ++o) {
                const auto position = row * oc + o;
                const int expectedValue = expected[sample * outputBytes + position], actual = bytes[position];
                const int difference = std::abs(actual - expectedValue);
                maximum = std::max(maximum, difference); if (difference > 1) ++mismatches;
                csv << sample << ',' << row << ',' << o << ',' << actual << ',' << expectedValue << ',' << difference << '\n';
                if (o < outputs) {
                    const double delta = (actual - 128) * double(sy) - reference[(sample * rows + row) * outputs + o];
                    squared += delta * delta; maximumFloat = std::max(maximumFloat, std::abs(delta));
                }
            }
        }
        if (profiler) { auto stream = create(directory + "/profile.json"); profiler->Print(stream); }
        runtime->UnloadNetwork(id);
        std::cout << "result=" << (mismatches ? "numerical_mismatch" : "passed") << " samples=" << samples
                  << " max_error_lsb=" << maximum << " mismatches=" << mismatches
                  << " fp32_branch_rmse=" << std::sqrt(squared / (samples * rows * outputs))
                  << " fp32_branch_max_abs=" << maximumFloat << " mean_enqueue_sync_us=" << totalUs / samples
                  << " full_rul_npu_verified=false\n";
        return mismatches ? 70 : 0;
    } catch (const std::exception& error) {
        std::cerr << "result=failed stage=" << stage << " error=" << error.what() << '\n'; return failure;
    }
}
