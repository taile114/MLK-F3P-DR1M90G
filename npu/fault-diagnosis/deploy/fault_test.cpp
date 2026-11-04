// Offline F3P single-primary fault inference. No host application or raw sensor API.
#include <armnn/ArmNN.hpp>
#include <armnn/IProfiler.hpp>
#include <armnn/Logging.hpp>
#include <armnnALUtils/cma_mem_init.hpp>
#include <arm_neon.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr unsigned inputCount = 27683;
const unsigned channels[] = {8, 16, 32, 64, 64};
struct Q { float sx, sw, sy; };
using Clock = std::chrono::steady_clock;

template<class T> std::vector<T> read(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f || f.tellg() <= 0 || f.tellg() % sizeof(T)) throw std::runtime_error("Invalid file: " + path);
    std::vector<T> v(static_cast<size_t>(f.tellg()) / sizeof(T));
    f.seekg(0); f.read(reinterpret_cast<char*>(v.data()), v.size() * sizeof(T));
    if (!f) throw std::runtime_error("Read failed: " + path);
    return v;
}
struct Weights {
    std::vector<float> values;
    std::map<std::string, std::pair<size_t, size_t>> entries;
    explicit Weights(const std::string& root) : values(read<float>(root + "/weights.bin")) {
        for (float v : values) if (!std::isfinite(v)) throw std::runtime_error("Nonfinite weight");
        std::ifstream f(root + "/weights.tsv"); std::string name; unsigned rank;
        while (f >> name >> rank) {
            if (rank > 4) throw std::runtime_error("Invalid tensor rank");
            size_t n = 1, d, count, offset;
            for (unsigned k = 0; k < rank; ++k) { f >> d; n *= d; }
            f >> count >> offset;
            if (!f || n != count || offset > values.size() || count > values.size() - offset || !entries.emplace(name, std::make_pair(offset, count)).second)
                throw std::runtime_error("Invalid weight entry");
        }
        if (!f.eof() || entries.empty()) throw std::runtime_error("Missing weight table");
    }
    const float* at(const std::string& name, size_t count) const {
        const auto& e = entries.at(name);
        if (e.second != count) throw std::runtime_error("Weight dimension mismatch: " + name);
        return values.data() + e.first;
    }
};
float dot(const float* a, const float* b, unsigned count) {
    float32x4_t v = vdupq_n_f32(0); unsigned k = 0;
    for (; k + 4 <= count; k += 4) v = vmlaq_f32(v, vld1q_f32(a + k), vld1q_f32(b + k));
    float sum = vaddvq_f32(v);
    for (; k < count; ++k) sum += a[k] * b[k];
    return sum;
}
int32_t idot(const uint8_t* a, const uint8_t* b, unsigned count) {
    int32x4_t acc = vdupq_n_s32(0); const auto zp = vdupq_n_s16(128); unsigned k = 0;
    for (; k + 8 <= count; k += 8) {
        auto av = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(a + k))), zp);
        auto bv = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(b + k))), zp);
        acc = vmlal_s16(acc, vget_low_s16(av), vget_low_s16(bv));
        acc = vmlal_s16(acc, vget_high_s16(av), vget_high_s16(bv));
    }
    int32_t sum = vaddvq_s32(acc);
    for (; k < count; ++k) sum += (int(a[k]) - 128) * (int(b[k]) - 128);
    return sum;
}
uint8_t quant(double v) { return static_cast<uint8_t>(std::max(0., std::min(255., std::round(v) + 128.))); }
void linear(const Weights& w, const std::string& name, const float* x, unsigned in, unsigned out, float* y, bool relu = true) {
    auto* weight = w.at(name + ".weight", in * out); auto* bias = w.at(name + ".bias", out);
    for (unsigned o = 0; o < out; ++o) { float v = dot(x, weight + o * in, in) + bias[o]; y[o] = relu ? std::max(0.f, v) : v; }
}
std::array<float, 9> head(const Weights& w, const float* cnn, const float* x) {
    std::array<float, 224> common{};
    linear(w, "tf_encoder.1", cnn, 768, 64, common.data());
    linear(w, "spectrum_encoder.1", x + 24576, 2307, 32, common.data() + 64);
    linear(w, "stats_encoder.0", x + 26883, 12, 16, common.data() + 96);
    linear(w, "pose_encoder.0", x + 26895, 8, 16, common.data() + 112);
    linear(w, "fast_encoder.1", x + 26903, 240, 32, common.data() + 128);
    std::array<float, 9> mechanical{}, thermal{};
    for (unsigned pass = 0; pass < 2; ++pass) {
        std::array<float, 540> state{};
        for (unsigned c = 0; c < 9; ++c) {
            const bool temperature = c == 0 || c == 3 || c == 6;
            if (temperature == bool(pass)) std::copy_n(x + 27143 + c * 60, 60, state.data() + c * 60);
        }
        std::array<float, 224> fusion{};
        if (!pass) std::copy_n(common.data(), 160, fusion.data());
        linear(w, "slow_encoder.1", state.data(), 540, 64, fusion.data() + 160);
        std::array<float, 128> hidden{};
        linear(w, "fusion.0", fusion.data(), 224, 128, hidden.data());
        linear(w, "head", hidden.data(), 128, 9, pass ? thermal.data() : mechanical.data(), false);
    }
    mechanical[3] = thermal[3]; mechanical[7] = thermal[7]; return mechanical;
}
struct Decoded { int c, g; float confidence; };
Decoded decode(const std::array<float, 9>& raw, const std::vector<float>& thresholds) {
    int c = int(std::max_element(raw.begin(), raw.begin() + 5) - raw.begin());
    double sum = 0; for (unsigned i = 0; i < 5; ++i) sum += std::exp(double(raw[i]) - raw[c]);
    int g = 0; if (c) g = 1 + (raw[4 + c] >= thresholds[(c - 1) * 2]) + (raw[4 + c] >= thresholds[(c - 1) * 2 + 1]);
    return {c, g, static_cast<float>(1 / sum)};
}
struct CmaSession {
    bool owned = false;
    void init() { if (!cma_mem_is_init()) { cma_mem_init(); owned = cma_mem_is_init(); } if (!cma_mem_is_init()) throw std::runtime_error("CMA initialization failed"); }
    ~CmaSession() { if (owned) cma_mem_deinit(); }
};
struct Buffer {
    cma_buffer_t value;
    explicit Buffer(unsigned size) : value(size) { cma_mem_alloc(&value); if (!value.virt_addr || !value.phys_addr) throw std::runtime_error("CMA buffer allocation failed"); }
    ~Buffer() { cma_mem_free(&value); }
    Buffer(const Buffer&) = delete;
};
armnn::TensorInfo info(unsigned c, unsigned h, unsigned width, float scale, bool constant = false) {
    return armnn::TensorInfo(armnn::TensorShape({1, c, h, width}), armnn::DataType::QAsymmU8, scale, 128, constant);
}
std::string attribute(const std::string& line, const std::string& name) {
    auto start = line.find(name + " : "); if (start == std::string::npos) return "";
    start += name.size() + 3; return line.substr(start, line.find("\\l", start) - start);
}
struct NpuGraph {
    armnn::IRuntime& runtime;
    armnn::NetworkId id = 0;
    bool loaded = false;
    armnn::TensorInfo inputInfo, outputInfo;
    unsigned inputSize, outputSize;
    std::unique_ptr<Buffer> in, out;
    armnn::InputTensors feeds;
    armnn::OutputTensors bindings;
    bool referenceBackend;
    std::vector<uint8_t> inputCpu, outputCpu;
    std::string tag;
    NpuGraph(armnn::IRuntime& rt, const std::array<Q, 4>& qs,
             const std::array<std::vector<uint8_t>, 4>& weights, const std::array<std::vector<int32_t>, 4>& bias,
             unsigned first, unsigned last, bool pool, bool graphOnly, const std::string& result, bool reference = false)
        : runtime(rt), referenceBackend(reference), tag("cnn_" + std::to_string(first) + "_" + std::to_string(last)) {
        auto network = armnn::INetwork::Create();
        unsigned h = 64 >> first, width = 48 >> first;
        inputSize = channels[first] * h * width;
        inputInfo = info(channels[first], h, width, qs[first].sx, true);
        auto* previous = network->AddInputLayer(0, "cnn_input"); previous->GetOutputSlot(0).SetTensorInfo(inputInfo);
        for (unsigned i = first; i < last; ++i) {
            auto prefix = "block" + std::to_string(i);
            armnn::TensorInfo wi(armnn::TensorShape({channels[i + 1], channels[i], 3, 3}), armnn::DataType::QAsymmU8, qs[i].sw, 128, true);
            armnn::TensorInfo bi(armnn::TensorShape({channels[i + 1]}), armnn::DataType::Signed32, qs[i].sx * qs[i].sw, 0, true);
            auto* w = network->AddConstantLayer(armnn::ConstTensor(wi, weights[i].data()), (prefix + "_weight").c_str()); w->GetOutputSlot(0).SetTensorInfo(wi);
            auto* b = network->AddConstantLayer(armnn::ConstTensor(bi, bias[i].data()), (prefix + "_bias").c_str()); b->GetOutputSlot(0).SetTensorInfo(bi);
            armnn::Convolution2dDescriptor cd; cd.m_BiasEnabled = true; cd.m_PadLeft = cd.m_PadRight = cd.m_PadTop = cd.m_PadBottom = 1; cd.m_DataLayout = armnn::DataLayout::NCHW;
            auto* conv = network->AddConvolution2dLayer(cd, (prefix + "_conv3x3").c_str());
            previous->GetOutputSlot(0).Connect(conv->GetInputSlot(0)); w->GetOutputSlot(0).Connect(conv->GetInputSlot(1)); b->GetOutputSlot(0).Connect(conv->GetInputSlot(2));
            outputInfo = info(channels[i + 1], h, width, qs[i].sy); conv->GetOutputSlot(0).SetTensorInfo(outputInfo); previous = conv;
            if (pool) {
                auto* relu = network->AddActivationLayer(armnn::ActivationDescriptor(armnn::ActivationFunction::ReLu), (prefix + "_relu").c_str());
                previous->GetOutputSlot(0).Connect(relu->GetInputSlot(0)); relu->GetOutputSlot(0).SetTensorInfo(outputInfo);
                armnn::Pooling2dDescriptor pd; pd.m_PoolType = armnn::PoolingAlgorithm::Max; pd.m_PoolWidth = pd.m_PoolHeight = pd.m_StrideX = pd.m_StrideY = 2; pd.m_DataLayout = armnn::DataLayout::NCHW;
                auto* p = network->AddPooling2dLayer(pd, (prefix + "_maxpool").c_str()); relu->GetOutputSlot(0).Connect(p->GetInputSlot(0)); h /= 2; width /= 2;
                outputInfo = info(channels[i + 1], h, width, qs[i].sy); p->GetOutputSlot(0).SetTensorInfo(outputInfo); previous = p;
            }
        }
        outputSize = channels[last] * h * width;
        previous->GetOutputSlot(0).Connect(network->AddOutputLayer(0, "cnn_output")->GetInputSlot(0));
        const std::string backend = reference ? "CpuRef" : "Alnpu";
        armnn::OptimizerOptionsOpaque options; options.SetImportEnabled(!reference); options.SetExportEnabled(!reference); options.SetProfilingEnabled(true);
        auto optimized = armnn::Optimize(*network, {armnn::BackendId(backend)}, runtime.GetDeviceSpec(), options);
        if (!optimized) throw std::runtime_error("Strict CNN backend optimization rejected");
        std::ostringstream graph; optimized->SerializeToDot(graph); std::ofstream(result + "/" + tag + ".dot") << graph.str();
        unsigned hard = 0, soft = 0;
        std::istringstream lines(graph.str());
        for (std::string line; std::getline(lines, line);) {
            auto kind = attribute(line, "LayerType"), backend = attribute(line, "BackendID");
            if (kind.empty() || kind == "Input" || kind == "Output" || kind == "Constant" || kind == "MemCopy" || kind == "MemImport" || kind == "MemSync") continue;
            if (backend != (reference ? "CpuRef" : "Alnpu") || (!reference && kind != "ALHardNPU" && kind != "ALSoftNPU")) throw std::runtime_error("Unexpected CNN compute backend");
            hard += kind == "ALHardNPU"; soft += kind == "ALSoftNPU";
        }
        if (!reference && !hard) throw std::runtime_error("CNN has no hard NPU workload");
        std::cout << "graph=" << tag << " hard_npu_nodes=" << hard << " soft_npu_nodes=" << soft << " cpu_fallback_allowed=false\n";
        if (graphOnly) return;
        auto memorySource = reference ? armnn::MemorySource::Undefined : armnn::MemorySource::Malloc;
        armnn::INetworkProperties properties(false, memorySource, memorySource, true); std::string error;
        if (runtime.LoadNetwork(id, std::move(optimized), error, properties) != armnn::Status::Success) throw std::runtime_error(error);
        loaded = true; runtime.GetProfiler(id)->EnableProfiling(false);
        if (reference) { inputCpu.resize(inputSize); outputCpu.resize(outputSize); }
        else { in = std::make_unique<Buffer>(inputSize); out = std::make_unique<Buffer>(outputSize); }
        feeds.emplace_back(0, armnn::ConstTensor(inputInfo, reference ? inputCpu.data() : in->value.virt_addr));
        bindings.emplace_back(0, armnn::Tensor(outputInfo, reference ? outputCpu.data() : out->value.virt_addr));
    }
    const uint8_t* run(const uint8_t* input) {
        if (referenceBackend) { std::memcpy(inputCpu.data(), input, inputSize); std::fill(outputCpu.begin(), outputCpu.end(), 0xa5); }
        else {
            std::memcpy(in->value.virt_addr, input, inputSize); std::memset(out->value.virt_addr, 0xa5, outputSize);
            cma_mem_sync_for_device(in->value.phys_addr, inputSize); cma_mem_sync_for_device(out->value.phys_addr, outputSize);
        }
        if (runtime.EnqueueWorkload(id, feeds, bindings) != armnn::Status::Success) throw std::runtime_error("NPU EnqueueWorkload failed");
        if (referenceBackend) return outputCpu.data();
        cma_mem_sync_for_cpu(out->value.phys_addr, outputSize); return static_cast<const uint8_t*>(out->value.virt_addr);
    }
    void profile(bool enabled) { if (loaded) runtime.GetProfiler(id)->EnableProfiling(enabled); }
    void saveProfile(const std::string& path) { if (loaded) { std::ofstream f(path + "/" + tag + "_profile.json"); runtime.GetProfiler(id)->Print(f); } }
    void saveAudit(const std::string& path, unsigned sample) {
        if (!loaded || referenceBackend) return;
        const auto prefix = path + "/audit_" + tag + "_sample" + std::to_string(sample);
        std::ofstream input(prefix + "_input.bin", std::ios::binary), output(prefix + "_output.bin", std::ios::binary);
        input.write(static_cast<const char*>(in->value.virt_addr), inputSize);
        output.write(static_cast<const char*>(out->value.virt_addr), outputSize);
        if (!input || !output) throw std::runtime_error("Cannot save actual NPU audit buffers");
    }
    ~NpuGraph() { if (loaded) runtime.UnloadNetwork(id); }
};

struct Cnn {
    const Weights& w;
    bool quantized;
    std::array<Q, 4> qs{};
    std::array<std::vector<uint8_t>, 4> qw;
    std::array<std::vector<int32_t>, 4> qb;
    std::array<std::vector<float>, 4> patches;
    std::array<std::vector<uint8_t>, 4> qpatches;
    std::array<std::vector<float>, 5> fv;
    std::array<std::vector<uint8_t>, 5> uv;
    std::vector<std::unique_ptr<NpuGraph>> graphs;
    explicit Cnn(const Weights& weights, const std::string& root, bool q) : w(weights), quantized(q) {
        std::ifstream plan(root + "/qparams.tsv"); unsigned row;
        for (unsigned i = 0; i < 4; ++i) {
            if (!(plan >> row >> qs[i].sx >> qs[i].sw >> qs[i].sy) || row != i || !std::isfinite(qs[i].sx + qs[i].sw + qs[i].sy) || qs[i].sx <= 0 || qs[i].sw <= 0 || qs[i].sy <= 0 || (i && qs[i - 1].sy != qs[i].sx)) throw std::runtime_error("Invalid quantization parameters");
            auto name = "backbone." + std::to_string(i) + ".0"; unsigned n = channels[i] * channels[i + 1] * 9;
            auto* weight = w.at(name + ".weight", n); auto* bias = w.at(name + ".bias", channels[i + 1]);
            qw[i].resize(n); qb[i].resize(channels[i + 1]);
            for (unsigned k = 0; k < n; ++k) qw[i][k] = quant(double(weight[k]) / qs[i].sw);
            for (unsigned k = 0; k < channels[i + 1]; ++k) { double b = std::round(double(bias[k]) / (double(qs[i].sx) * qs[i].sw)); if (std::abs(b) >= 2147483648.) throw std::runtime_error("Quantized bias overflow"); qb[i][k] = int32_t(b); }
            unsigned spatial = (64 >> i) * (48 >> i);
            if (q) qpatches[i].resize(channels[i] * 9 * spatial); else patches[i].resize(channels[i] * 9 * spatial);
        }
        for (unsigned i = 0; i < 5; ++i) { unsigned n = channels[i] * (64 >> i) * (48 >> i); fv[i].resize(n); uv[i].resize(n); }
    }
    static void pool(const uint8_t* in, uint8_t* out, unsigned c, unsigned h, unsigned width) {
        for (unsigned o = 0; o < c; ++o) for (unsigned y = 0; y < h; y += 2) for (unsigned x = 0; x < width; x += 2) {
            const auto* p = in + o * h * width + y * width + x;
            out[(o * (h / 2) + y / 2) * (width / 2) + x / 2] = std::max(uint8_t(128), std::max(std::max(p[0], p[1]), std::max(p[width], p[width + 1])));
        }
    }
    const float* run(const float* input) {
        if (quantized) for (unsigned i = 0; i < uv[0].size(); ++i) uv[0][i] = quant(double(input[i]) / qs[0].sx);
        else std::copy_n(input, fv[0].size(), fv[0].data());
        if (graphs.size() == 1) std::copy_n(graphs[0]->run(uv[0].data()), uv[4].size(), uv[4].data());
        else for (unsigned i = 0; i < 4; ++i) {
            unsigned h = 64 >> i, width = 48 >> i, spatial = h * width, ksize = channels[i] * 9;
            if (graphs.size() == 4) { pool(graphs[i]->run(uv[i].data()), uv[i + 1].data(), channels[i + 1], h, width); continue; }
            // im2col in NCHW kernel order; real-zero padding is 128 for UINT8.
            for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < width; ++x) for (unsigned c = 0; c < channels[i]; ++c) for (int ky = 0; ky < 3; ++ky) for (int kx = 0; kx < 3; ++kx) {
                int yy = int(y) + ky - 1, xx = int(x) + kx - 1; unsigned at = (y * width + x) * ksize + c * 9 + ky * 3 + kx;
                bool valid = yy >= 0 && xx >= 0 && yy < int(h) && xx < int(width);
                if (quantized) qpatches[i][at] = valid ? uv[i][(c * h + yy) * width + xx] : 128;
                else patches[i][at] = valid ? fv[i][(c * h + yy) * width + xx] : 0.f;
            }
            auto name = "backbone." + std::to_string(i) + ".0";
            auto* weight = w.at(name + ".weight", channels[i + 1] * ksize); auto* bias = w.at(name + ".bias", channels[i + 1]);
            for (unsigned o = 0; o < channels[i + 1]; ++o) for (unsigned y = 0; y < h; y += 2) for (unsigned x = 0; x < width; x += 2) {
                float best = 0.f; uint8_t ubest = 128;
                for (unsigned dy = 0; dy < 2; ++dy) for (unsigned dx = 0; dx < 2; ++dx) {
                    unsigned pos = ((y + dy) * width + x + dx) * ksize;
                    if (quantized) {
                        int64_t acc = int64_t(idot(qpatches[i].data() + pos, qw[i].data() + o * ksize, ksize)) + qb[i][o];
                        auto v = quant(double(acc) * (double(qs[i].sx) * qs[i].sw / qs[i].sy)); ubest = std::max(ubest, v);
                    } else best = std::max(best, dot(patches[i].data() + pos, weight + o * ksize, ksize) + bias[o]);
                }
                unsigned pos = (o * (h / 2) + y / 2) * (width / 2) + x / 2;
                if (quantized) uv[i + 1][pos] = ubest; else fv[i + 1][pos] = best;
            }
            (void)spatial;
        }
        if (quantized) for (unsigned k = 0; k < 768; ++k) fv[4][k] = (int(uv[4][k]) - 128) * qs[3].sy;
        return fv[4].data();
    }
};

long cmaFree() {
    std::ifstream f("/proc/meminfo"); for (std::string line; std::getline(f, line);) if (line.find("CmaFree:") == 0) { std::istringstream s(line.substr(8)); long v; if (s >> v) return v; } return -1;
}
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf << std::setprecision(10); std::string phase = "arguments";
    try {
        if (argc != 5 && argc != 6) throw std::runtime_error("Usage: fault_test ROOT cpu|quantized|armnn_ref|hybrid|hybrid_split COUNT RESULTS [--graph-only]");
        std::string root = argv[1], mode = argv[2], result = argv[4];
        bool hardware = mode == "hybrid" || mode == "hybrid_split", q = mode != "cpu", graphOnly = argc == 6 && std::string(argv[5]) == "--graph-only";
        bool armnnReference = mode == "armnn_ref";
        if (mode != "cpu" && mode != "quantized" && !hardware && !armnnReference) throw std::runtime_error("Unknown mode");
        if (mode == "hybrid") throw std::runtime_error("Fused quantized ReLU is numerically unsupported by the paired backend; use hybrid_split");
        if (argc == 6 && (!graphOnly || !hardware)) throw std::runtime_error("Graph-only requires an explicit NPU plan");
        std::string countText = argv[3]; size_t used; auto parsed = std::stoul(countText, &used);
        if (used != countText.size() || parsed > 360) throw std::runtime_error("COUNT must be 0..360");
        unsigned count = parsed;
        phase = "assets"; Weights weights(root); auto thresholds = read<float>(root + "/thresholds.bin");
        if (thresholds.size() != 8) throw std::runtime_error("Invalid ordinal thresholds");
        CmaSession cma; if (hardware && !graphOnly) cma.init();
        // All graphs share one runtime, one allocator registration, and persistent CMA IO.
        armnn::IRuntimePtr runtime{nullptr, armnn::IRuntime::Destroy};
        if (hardware || armnnReference) {
            armnn::ConfigureLogging(true, false, armnn::LogSeverity::Info);
            armnn::IRuntime::CreationOptions options;
            if (hardware && !graphOnly) { auto allocator = std::make_shared<armnn::CmaAllocator>(); options.m_CustomAllocatorMap["CpuRef"] = allocator; options.m_CustomAllocatorMap["CpuAcc"] = allocator; }
            phase = "runtime"; runtime = armnn::IRuntime::Create(options);
        }
        Cnn cnn(weights, root, q);
        if (hardware || armnnReference) {
            phase = "strict_npu_graph";
            if (mode == "hybrid" || armnnReference) cnn.graphs.emplace_back(new NpuGraph(*runtime, cnn.qs, cnn.qw, cnn.qb, 0, 4, true, graphOnly, result, armnnReference));
            else for (unsigned i = 0; i < 4; ++i) cnn.graphs.emplace_back(new NpuGraph(*runtime, cnn.qs, cnn.qw, cnn.qb, i, i + 1, false, graphOnly, result));
            if (graphOnly) { std::cout << "result=graph_accepted hardware_execution_verified=false\n"; return 0; }
        }
        phase = "test_vectors";
        auto inputs = read<float>(root + "/data/test_inputs.bin");
        auto reference = read<float>(root + "/data/test_reference.bin"); auto quantReference = read<float>(root + "/data/test_quantized.bin");
        auto expectedBytes = read<uint8_t>(root + "/data/test_cnn_u8.bin");
        if (inputs.size() != 360 * inputCount || reference.size() != 360 * 9 || quantReference.size() != 360 * 9 || expectedBytes.size() != 360 * 768) throw std::runtime_error("Incomplete frozen test set");
        for (float v : inputs) if (!std::isfinite(v)) throw std::runtime_error("Nonfinite test input");
        for (unsigned i = 0; i < 360; ++i) if (inputs[i * inputCount + 26901] != 1) throw std::runtime_error("Invalid test mount_pose");
        if (!count) count = 360;
        auto predict = [&](unsigned i) { return head(weights, cnn.run(inputs.data() + i * inputCount), inputs.data() + i * inputCount); };
        phase = "warmup"; for (unsigned k = 0; k < 3; ++k) (void)predict(0);
        const auto cmaSteady = hardware ? cma_mem_count() : 0;
        std::ofstream csv(result + "/predictions.csv"), outputs(result + "/raw_outputs.bin", std::ios::binary), bytes(result + "/cnn_output_u8.bin", std::ios::binary);
        csv << std::setprecision(10) << "sample,class_index,grade,confidence,forward_us,max_fp32_error,max_quantized_error,max_cnn_lsb,cma_bytes,cma_free_kib";
        for (unsigned k = 0; k < 9; ++k) csv << ",raw" << k;
        csv << '\n'; if (!csv || !outputs || !bytes) throw std::runtime_error("Cannot write result files");
        double maxFp = 0, maxQ = 0, elapsed = 0; unsigned mismatches = 0, maxLSB = 0;
        phase = "inference";
        for (unsigned i = 0; i < count; ++i) {
            if (!i) for (auto& g : cnn.graphs) g->profile(true);
            auto begin = Clock::now(); auto raw = predict(i); double us = std::chrono::duration<double, std::micro>(Clock::now() - begin).count();
            if (!i) for (auto& g : cnn.graphs) { g->profile(false); g->saveProfile(result); }
            if (hardware && (i == 0 || i == 84 || i == 150 || i == 200 || i == 246 || i == 300 || i == 359))
                for (auto& g : cnn.graphs) g->saveAudit(result, i);
            auto d = decode(raw, thresholds); double ef = 0, eq = 0; unsigned lsb = 0;
            for (unsigned k = 0; k < 9; ++k) {
                if (!std::isfinite(raw[k])) throw std::runtime_error("Nonfinite model output");
                ef = std::max(ef, std::abs(double(raw[k]) - reference[i * 9 + k])); eq = std::max(eq, std::abs(double(raw[k]) - quantReference[i * 9 + k]));
                float expected = q ? quantReference[i * 9 + k] : reference[i * 9 + k];
                if (!hardware && !armnnReference && std::abs(raw[k] - expected) > 1e-4f + 1e-4f * std::abs(expected)) ++mismatches;
            }
            if (q) for (unsigned k = 0; k < 768; ++k) lsb = std::max(lsb, unsigned(std::abs(int(cnn.uv[4][k]) - expectedBytes[i * 768 + k])));
            const auto allocated = hardware ? cma_mem_count() : 0;
            if (hardware && allocated != cmaSteady) throw std::runtime_error("CMA allocation changed during replay");
            csv << i << ',' << d.c << ',' << d.g << ',' << d.confidence << ',' << us << ',' << ef << ',' << eq << ',' << lsb << ',' << allocated << ',' << cmaFree();
            for (float v : raw) csv << ',' << v;
            csv << '\n'; outputs.write(reinterpret_cast<const char*>(raw.data()), sizeof(raw));
            if (q) bytes.write(reinterpret_cast<const char*>(cnn.uv[4].data()), 768);
            maxFp = std::max(maxFp, ef); maxQ = std::max(maxQ, eq); maxLSB = std::max(maxLSB, lsb); elapsed += us;
            if ((i + 1) % 30 == 0) std::cout << "progress=" << i + 1 << '/' << count << '\n';
        }
        csv.close(); outputs.close(); bytes.close();
        cnn.graphs.clear(); runtime.reset();
        auto afterRelease = hardware ? cma_mem_count() : 0;
        std::ofstream summary(result + "/summary.json");
        summary << std::setprecision(12) << "{\"mode\":\"" << mode << "\",\"samples\":" << count << ",\"warmup\":3,\"npu_execution_requested\":" << (hardware ? "true" : "false")
                << ",\"cpu_fallback_allowed\":false,\"max_fp32_error\":" << maxFp << ",\"max_quantized_error\":" << maxQ << ",\"max_cnn_lsb\":" << maxLSB
                << ",\"numeric_mismatches\":" << mismatches << ",\"mean_forward_us\":" << elapsed / count << ",\"steady_cma_bytes\":" << cmaSteady << ",\"after_release_cma_bytes\":" << afterRelease << "}\n";
        if (afterRelease) throw std::runtime_error("CMA buffers remain after graph release");
        std::cout << "result=" << (mismatches ? "numeric_mismatch" : "replay_completed") << " mode=" << mode << " samples=" << count << " max_fp32_error=" << maxFp << " max_quantized_error=" << maxQ << " mean_forward_us=" << elapsed / count << '\n';
        return mismatches ? 70 : 0;
    } catch (const std::exception& e) { std::cerr << "result=failed stage=" << phase << " error=" << e.what() << '\n'; return 2; }
}
