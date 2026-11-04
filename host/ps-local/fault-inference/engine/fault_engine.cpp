/* 单主故障模型共享库：mlk_fault_abi / create / predict / destroy。
 *
 * 后端 1（CPU）用运行包中的 BN 折叠 FP32 权重逐窗前向，数值语义与
 * npu/fault-diagnosis/deploy/fault_test.cpp 的 cpu 模式一致：四层 3x3 卷积
 * （padding=1）后接 ReLU 与 2x2 MaxPool，再经 TF / 频谱 / 统计 / 姿态 /
 * 快状态 / 慢状态六个分支融合，机械与温度两条隔离路径在 CPU 合并。
 *
 * 后端 2（CPU+CNN NPU）在定义 MLK_FAULT_HAVE_ARMNN 时编译：四个 3x3 卷积
 * 各自成为仅允许 Alnpu 的独立图，CMA 持久输入输出；层间 ReLU（UINT8 零点
 * 128）与 MaxPool 仍在 CPU，与 fault_test.cpp 的 hybrid_split 计划一致。
 * 未启用该宏时后端 2 在 create 阶段返回明确错误，不做静默 CPU 回退。
 *
 * 输入是已按训练侧固定参数标准化的 base6_single_native4s_flat_v1：batch=1、
 * 小端 float32、27683 个值。库内不读取标签、参考结果或测试向量。
 */
#include "../../../common/fault_engine_api.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(MLK_FAULT_HAVE_ARMNN)
#include <armnn/ArmNN.hpp>
#include <armnn/Logging.hpp>
#include <armnnALUtils/cma_mem_init.hpp>
#endif

namespace {

constexpr unsigned kInputFloats = MLK_FAULT_INPUT_FLOATS; /* 27683 */
constexpr unsigned kTimeFrequencyFloats = 24576;          /* [8,64,48] */
constexpr unsigned kSpectrumOffset = 24576;               /* [3,769] */
constexpr unsigned kStatsOffset = 26883; /* [12] */
constexpr unsigned kPoseOffset = 26895;  /* [8] */
constexpr unsigned kFastOffset = 26903;  /* [6,40] */
constexpr unsigned kSlowOffset = 27143;  /* [9,60] */

constexpr unsigned kChannels[5] = {8, 16, 32, 64, 64};
constexpr unsigned kHeight = 64;
constexpr unsigned kWidth = 48;

constexpr const char *kWeightFile = "/weights.bin";
constexpr const char *kWeightTable = "/weights.tsv";
constexpr const char *kThresholdFile = "/thresholds.bin";
constexpr const char *kQuantFile = "/qparams.tsv";

/* 有界错误文本：始终以 NUL 结尾，超长截断。 */
void setError(char *error, size_t capacity, const std::string &text)
{
    if (!error || capacity == 0) return;
    const size_t limit = capacity - 1;
    const size_t length = text.size() < limit ? text.size() : limit;
    std::memcpy(error, text.data(), length);
    error[length] = '\0';
}

std::string joinPath(const std::string &root, const char *name)
{
    if (root.empty()) return std::string(name + 1);
    if (root.back() == '/' || root.back() == '\\') return root + (name + 1);
    return root + name;
}

template <class T>
std::vector<T> readBinary(const std::string &path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("missing file: " + path);
    const auto size = file.tellg();
    if (size <= 0 || size % static_cast<std::streamoff>(sizeof(T)))
        throw std::runtime_error("invalid file size: " + path);
    std::vector<T> values(static_cast<size_t>(size) / sizeof(T));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(T)));
    if (!file) throw std::runtime_error("read failed: " + path);
    return values;
}

/* weights.bin 与 weights.tsv：一维浮点存储 + 名称到区间的索引表。 */
struct WeightTable {
    std::vector<float> values;
    std::map<std::string, std::pair<size_t, size_t>> entries;

    void load(const std::string &root)
    {
        values = readBinary<float>(joinPath(root, kWeightFile));
        for (float value : values)
            if (!std::isfinite(value)) throw std::runtime_error("nonfinite weight");

        std::ifstream table(joinPath(root, kWeightTable));
        if (!table) throw std::runtime_error("missing file: weights.tsv");
        std::string name;
        unsigned rank = 0;
        while (table >> name >> rank) {
            if (rank > 4) throw std::runtime_error("invalid weight rank: " + name);
            size_t count = 1;
            for (unsigned axis = 0; axis < rank; ++axis) {
                size_t extent = 0;
                if (!(table >> extent) || !extent) throw std::runtime_error("invalid weight shape: " + name);
                count *= extent;
            }
            size_t declared = 0, offset = 0;
            if (!(table >> declared >> offset) || declared != count || offset > values.size()
                || count > values.size() - offset)
                throw std::runtime_error("invalid weight entry: " + name);
            if (!entries.emplace(name, std::make_pair(offset, count)).second)
                throw std::runtime_error("duplicate weight entry: " + name);
        }
        if (!table.eof() || entries.empty()) throw std::runtime_error("missing weight table");
    }

    const float *at(const char *name, size_t count) const
    {
        const auto found = entries.find(name);
        if (found == entries.end()) throw std::runtime_error(std::string("missing weight: ") + name);
        if (found->second.second != count)
            throw std::runtime_error(std::string("weight dimension mismatch: ") + name);
        return values.data() + found->second.first;
    }
};

/* 四层卷积的量化尺度：输入、权重、输出。 */
struct QuantParam {
    float sx = 0.f, sw = 0.f, sy = 0.f;
};

[[maybe_unused]] std::array<QuantParam, 4> readQuantParams(const std::string &root)
{
    std::ifstream plan(joinPath(root, kQuantFile));
    if (!plan) throw std::runtime_error("missing file: qparams.tsv");
    std::array<QuantParam, 4> params{};
    for (unsigned i = 0; i < 4; ++i) {
        unsigned row = 0;
        if (!(plan >> row >> params[i].sx >> params[i].sw >> params[i].sy) || row != i)
            throw std::runtime_error("invalid quantization parameters");
        const QuantParam &p = params[i];
        if (!std::isfinite(p.sx + p.sw + p.sy) || p.sx <= 0.f || p.sw <= 0.f || p.sy <= 0.f)
            throw std::runtime_error("invalid quantization scale");
        if (i && params[i - 1].sy != params[i].sx)
            throw std::runtime_error("quantization scales are not chained");
    }
    return params;
}

float dot(const float *left, const float *right, unsigned count)
{
    float sum = 0.f;
    unsigned index = 0;
    for (; index + 4 <= count; index += 4) {
        sum += left[index] * right[index];
        sum += left[index + 1] * right[index + 1];
        sum += left[index + 2] * right[index + 2];
        sum += left[index + 3] * right[index + 3];
    }
    for (; index < count; ++index) sum += left[index] * right[index];
    return sum;
}

/* 全连接 + 可选 ReLU，行主序权重，与 fault_test.cpp 的 linear 相同。 */
void linear(const WeightTable &weights, const std::string &name, const float *input, unsigned in,
            unsigned out, float *output, bool relu)
{
    const float *w = weights.at((name + ".weight").c_str(), static_cast<size_t>(in) * out);
    const float *b = weights.at((name + ".bias").c_str(), out);
    for (unsigned o = 0; o < out; ++o) {
        const float value = dot(input, w + static_cast<size_t>(o) * in, in) + b[o];
        output[o] = relu ? std::max(0.f, value) : value;
    }
}

struct Context {
    WeightTable weights;
    std::array<float, 8> thresholds{};
    std::vector<float> maps[5]; /* 卷积各层特征图 */
    std::vector<float> patch;   /* im2col 工作区 */
    std::vector<float> hidden;  /* 128 */
    std::array<float, 224> common{}; /* 五分支编码结果，供两次融合共用 */
    int backend = MLK_FAULT_CPU;
    std::mutex mutex;
#if defined(MLK_FAULT_HAVE_ARMNN)
    struct NpuBackend *npu = nullptr;
#endif
    ~Context();
};

/* 3x3 卷积（padding=1）后接 ReLU 与 2x2 MaxPool，输出写入下一层特征图。 */
void convReluPool(Context &context, unsigned layer, const float *input, float *output)
{
    const unsigned in_channels = kChannels[layer];
    const unsigned out_channels = kChannels[layer + 1];
    const unsigned height = kHeight >> layer;
    const unsigned width = kWidth >> layer;
    const unsigned kernel = in_channels * 9;
    const unsigned spatial = height * width;

    context.patch.resize(static_cast<size_t>(spatial) * kernel);
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            const size_t base = (static_cast<size_t>(y) * width + x) * kernel;
            for (unsigned c = 0; c < in_channels; ++c) {
                for (int ky = 0; ky < 3; ++ky) {
                    for (int kx = 0; kx < 3; ++kx) {
                        const int sy = static_cast<int>(y) + ky - 1;
                        const int sx = static_cast<int>(x) + kx - 1;
                        const bool inside = sy >= 0 && sx >= 0 && sy < static_cast<int>(height)
                                            && sx < static_cast<int>(width);
                        context.patch[base + c * 9 + ky * 3 + kx] =
                            inside ? input[(static_cast<size_t>(c) * height + sy) * width + sx] : 0.f;
                    }
                }
            }
        }
    }

    const std::string name = "backbone." + std::to_string(layer) + ".0";
    const float *weight = context.weights.at((name + ".weight").c_str(),
                                             static_cast<size_t>(out_channels) * kernel);
    const float *bias = context.weights.at((name + ".bias").c_str(), out_channels);
    const unsigned pooled_height = height / 2, pooled_width = width / 2;

    for (unsigned o = 0; o < out_channels; ++o) {
        const float *row = weight + static_cast<size_t>(o) * kernel;
        for (unsigned y = 0; y < height; y += 2) {
            for (unsigned x = 0; x < width; x += 2) {
                float best = 0.f; /* ReLU 后 MaxPool：与下界 0 取最大等价 */
                for (unsigned dy = 0; dy < 2; ++dy) {
                    for (unsigned dx = 0; dx < 2; ++dx) {
                        const size_t at = (static_cast<size_t>(y + dy) * width + x + dx) * kernel;
                        best = std::max(best, dot(context.patch.data() + at, row, kernel) + bias[o]);
                    }
                }
                output[(static_cast<size_t>(o) * pooled_height + y / 2) * pooled_width + x / 2] = best;
            }
        }
    }
}

const float *cnnForward(Context &context, float *firstMap)
{
    /* 第一层直接读取输入张量的 [8,64,48] 部分；调用方已把它放进 maps[0] */
    const float *current = firstMap;
    for (unsigned layer = 0; layer < 4; ++layer) {
        convReluPool(context, layer, current, context.maps[layer + 1].data());
        current = context.maps[layer + 1].data();
    }
    return current; /* 64 x 4 x 3 = 768 */
}

/* 六分支融合：机械路径使用全部公共编码，温度路径只使用慢状态中的温度通道。 */
std::array<float, 9> head(Context &context, const float *cnn, const float *input)
{
    float *common = context.common.data();
    linear(context.weights, "tf_encoder.1", cnn, 768, 64, common, true);
    linear(context.weights, "spectrum_encoder.1", input + kSpectrumOffset, 2307, 32, common + 64, true);
    linear(context.weights, "stats_encoder.0", input + kStatsOffset, 12, 16, common + 96, true);
    linear(context.weights, "pose_encoder.0", input + kPoseOffset, 8, 16, common + 112, true);
    linear(context.weights, "fast_encoder.1", input + kFastOffset, 240, 32, common + 128, true);

    std::array<float, 9> mechanical{}, thermal{};
    std::array<float, 224> fusion{};
    std::array<float, 540> state{};
    for (unsigned pass = 0; pass < 2; ++pass) {
        state.fill(0.f);
        for (unsigned channel = 0; channel < 9; ++channel) {
            /* 慢状态第 0/3/6 通道为温度相关量，其余为机械量 */
            const bool temperature = channel == 0 || channel == 3 || channel == 6;
            if (temperature == (pass == 1))
                std::copy_n(input + kSlowOffset + channel * 60, 60, state.data() + channel * 60);
        }
        fusion.fill(0.f);
        if (pass == 0) std::copy_n(context.common.data(), 160, fusion.data());
        linear(context.weights, "slow_encoder.1", state.data(), 540, 64, fusion.data() + 160, true);
        linear(context.weights, "fusion.0", fusion.data(), 224, 128, context.hidden.data(), true);
        linear(context.weights, "head", context.hidden.data(), 128, 9,
               pass ? thermal.data() : mechanical.data(), false);
    }
    /* 过热类别（索引 3）及其分级（索引 7）取自温度隔离路径 */
    mechanical[3] = thermal[3];
    mechanical[7] = thermal[7];
    return mechanical;
}

struct Decoded {
    int class_index;
    int grade;
    float confidence;
};

Decoded decode(const std::array<float, 9> &raw, const std::array<float, 8> &thresholds,
               uint32_t excluded)
{
    int best = -1;
    for (int i = 0; i < 5; ++i)
        if (!(excluded & (1u << i)) && (best < 0 || raw[i] > raw[best])) best = i;

    double sum = 0.0;
    for (unsigned i = 0; i < 5; ++i)
        if (!(excluded & (1u << i))) sum += std::exp(static_cast<double>(raw[i]) - raw[best]);

    int grade = 0;
    if (best != 0) {
        const float score = raw[4 + best];
        grade = 1 + (score >= thresholds[(best - 1) * 2])
                + (score >= thresholds[(best - 1) * 2 + 1]);
    }
    return {best, grade, static_cast<float>(1.0 / sum)};
}

void clearResult(mlk_fault_result *result, unsigned backend)
{
    if (!result) return;
    result->struct_size = 32;
    result->valid = 0;
    result->class_index = -1;
    result->grade = -1;
    result->confidence = -1.f;
    result->backend = backend;
    result->infer_us = 0;
    result->reserved = 0;
}

#if defined(MLK_FAULT_HAVE_ARMNN)

uint8_t quantize(double value)
{
    return static_cast<uint8_t>(std::max(0.0, std::min(255.0, std::round(value) + 128.0)));
}

/* ReLU 以 UINT8 零点 128 截断，再做 2x2 非重叠 MaxPool。 */
void reluPool(const uint8_t *input, uint8_t *output, unsigned channels, unsigned height,
              unsigned width)
{
    for (unsigned c = 0; c < channels; ++c) {
        for (unsigned y = 0; y < height; y += 2) {
            for (unsigned x = 0; x < width; x += 2) {
                const uint8_t *p = input + static_cast<size_t>(c) * height * width + y * width + x;
                output[(static_cast<size_t>(c) * (height / 2) + y / 2) * (width / 2) + x / 2] =
                    std::max<uint8_t>(128, std::max(std::max(p[0], p[1]), std::max(p[width], p[width + 1])));
            }
        }
    }
}

/* CMA 会话：本库自己初始化时才负责释放。 */
struct CmaSession {
    bool owned = false;
    CmaSession()
    {
        if (!cma_mem_is_init()) {
            cma_mem_init();
            owned = cma_mem_is_init();
        }
        if (!cma_mem_is_init()) throw std::runtime_error("CMA initialization failed");
    }
    ~CmaSession()
    {
        if (owned) cma_mem_deinit();
    }
};

struct CmaBuffer {
    cma_buffer_t value;
    explicit CmaBuffer(unsigned bytes) : value(bytes)
    {
        cma_mem_alloc(&value);
        if (!value.virt_addr || !value.phys_addr) throw std::runtime_error("CMA buffer allocation failed");
    }
    ~CmaBuffer() { cma_mem_free(&value); }
    CmaBuffer(const CmaBuffer &) = delete;
    CmaBuffer &operator=(const CmaBuffer &) = delete;
};

armnn::TensorInfo quantTensor(unsigned channels, unsigned height, unsigned width, float scale,
                              bool constant = false)
{
    return armnn::TensorInfo(armnn::TensorShape({1, channels, height, width}),
                             armnn::DataType::QAsymmU8, scale, 128, constant);
}

/* Arm NN 32.1 requires the read-only ConstTensor input binding to carry
 * IsConstant=true. It is still an InputLayer fed anew for every window;
 * this flag does not turn the model input into a ConstantLayer. */
armnn::TensorInfo inputTensorInfo(unsigned channels, unsigned height, unsigned width, float scale)
{
    return quantTensor(channels, height, width, scale, true);
}

struct NpuLayer {
    armnn::NetworkId id = 0;
    bool loaded = false;
    armnn::TensorInfo inputInfo, outputInfo;
    unsigned inputSize = 0, outputSize = 0;
    std::unique_ptr<CmaBuffer> input, output;
    armnn::InputTensors feeds;
    armnn::OutputTensors bindings;
};

/* 后端 2：四个仅允许 Alnpu 的独立卷积图共用 runtime 与 CMA 会话。 */
struct NpuBackend {
    CmaSession cma;
    armnn::IRuntimePtr runtime{nullptr, armnn::IRuntime::Destroy};
    std::array<QuantParam, 4> scales{};
    std::array<std::vector<uint8_t>, 4> weights;
    std::array<std::vector<int32_t>, 4> biases;
    std::array<NpuLayer, 4> layers;
    std::vector<uint8_t> upload; /* 当前层输入（主机侧） */

    NpuBackend(const WeightTable &table, const std::string &root)
    {
        scales = readQuantParams(root);
        for (unsigned layer = 0; layer < 4; ++layer) {
            const std::string name = "backbone." + std::to_string(layer) + ".0";
            const unsigned in = kChannels[layer], out = kChannels[layer + 1];
            const unsigned kernel = in * 9;
            const float *weight = table.at((name + ".weight").c_str(), static_cast<size_t>(out) * kernel);
            const float *bias = table.at((name + ".bias").c_str(), out);
            weights[layer].resize(static_cast<size_t>(out) * kernel);
            for (size_t k = 0; k < weights[layer].size(); ++k)
                weights[layer][k] = quantize(double(weight[k]) / scales[layer].sw);
            biases[layer].resize(out);
            for (unsigned k = 0; k < out; ++k) {
                const double value = std::round(double(bias[k]) / (double(scales[layer].sx) * scales[layer].sw));
                if (std::abs(value) >= 2147483648.0) throw std::runtime_error("Quantized bias overflow");
                biases[layer][k] = static_cast<int32_t>(value);
            }
        }

        armnn::ConfigureLogging(true, false, armnn::LogSeverity::Info);
        armnn::IRuntime::CreationOptions options;
        auto allocator = std::make_shared<armnn::CmaAllocator>();
        options.m_CustomAllocatorMap["CpuRef"] = allocator;
        options.m_CustomAllocatorMap["CpuAcc"] = allocator;
        runtime = armnn::IRuntime::Create(options);
        if (!runtime) throw std::runtime_error("Arm NN runtime creation failed");

        try {
            for (unsigned layer = 0; layer < 4; ++layer) buildLayer(layer);
        } catch (...) {
            /* 构造失败时按同样逆序撤销已加载的图，避免 runtime 残留网络。 */
            for (auto &layer : layers)
                if (layer.loaded && runtime) runtime->UnloadNetwork(layer.id);
            throw;
        }
    }

    ~NpuBackend()
    {
        for (auto &layer : layers)
            if (layer.loaded && runtime) runtime->UnloadNetwork(layer.id);
    }

    void buildLayer(unsigned layer)
    {
        NpuLayer &target = layers[layer];
        const unsigned in = kChannels[layer], out = kChannels[layer + 1];
        const unsigned height = kHeight >> layer, width = kWidth >> layer;
        const std::string prefix = "block" + std::to_string(layer);

        auto network = armnn::INetwork::Create();
        target.inputInfo = inputTensorInfo(in, height, width, scales[layer].sx);
        auto *previous = network->AddInputLayer(0, "cnn_input");
        previous->GetOutputSlot(0).SetTensorInfo(target.inputInfo);

        armnn::TensorInfo weightInfo(armnn::TensorShape({out, in, 3, 3}), armnn::DataType::QAsymmU8,
                                     scales[layer].sw, 128, true);
        armnn::TensorInfo biasInfo(armnn::TensorShape({out}), armnn::DataType::Signed32,
                                   scales[layer].sx * scales[layer].sw, 0, true);
        auto *weightLayer = network->AddConstantLayer(
            armnn::ConstTensor(weightInfo, weights[layer].data()), (prefix + "_weight").c_str());
        weightLayer->GetOutputSlot(0).SetTensorInfo(weightInfo);
        auto *biasLayer = network->AddConstantLayer(
            armnn::ConstTensor(biasInfo, biases[layer].data()), (prefix + "_bias").c_str());
        biasLayer->GetOutputSlot(0).SetTensorInfo(biasInfo);

        armnn::Convolution2dDescriptor descriptor;
        descriptor.m_BiasEnabled = true;
        descriptor.m_PadLeft = descriptor.m_PadRight = descriptor.m_PadTop = descriptor.m_PadBottom = 1;
        descriptor.m_DataLayout = armnn::DataLayout::NCHW;
        auto *convolution = network->AddConvolution2dLayer(descriptor, (prefix + "_conv3x3").c_str());
        previous->GetOutputSlot(0).Connect(convolution->GetInputSlot(0));
        weightLayer->GetOutputSlot(0).Connect(convolution->GetInputSlot(1));
        biasLayer->GetOutputSlot(0).Connect(convolution->GetInputSlot(2));
        target.outputInfo = quantTensor(out, height, width, scales[layer].sy);
        convolution->GetOutputSlot(0).SetTensorInfo(target.outputInfo);
        convolution->GetOutputSlot(0).Connect(network->AddOutputLayer(0, "cnn_output")->GetInputSlot(0));

        armnn::OptimizerOptionsOpaque options;
        options.SetImportEnabled(true);
        options.SetExportEnabled(true);
        auto optimized = armnn::Optimize(*network, {armnn::BackendId("Alnpu")},
                                         runtime->GetDeviceSpec(), options);
        if (!optimized) throw std::runtime_error("strict Alnpu optimization rejected");

        /* 计算节点必须全部落在 ANLOGIC NPU，且至少有一个硬核节点。 */
        std::ostringstream graph;
        optimized->SerializeToDot(graph);
        unsigned hard = 0, soft = 0;
        std::istringstream lines(graph.str());
        for (std::string line; std::getline(lines, line);) {
            const auto layerType = attribute(line, "LayerType");
            const auto backend = attribute(line, "BackendID");
            if (layerType.empty() || layerType == "Input" || layerType == "Output"
                || layerType == "Constant" || layerType == "MemCopy" || layerType == "MemImport"
                || layerType == "MemSync")
                continue;
            if (backend != "Alnpu" || (layerType != "ALHardNPU" && layerType != "ALSoftNPU"))
                throw std::runtime_error("unexpected CNN compute backend");
            hard += layerType == "ALHardNPU";
            soft += layerType == "ALSoftNPU";
        }
        if (!hard) throw std::runtime_error("CNN has no hard NPU workload");

        armnn::INetworkProperties properties(false, armnn::MemorySource::Malloc,
                                             armnn::MemorySource::Malloc, true);
        std::string error;
        if (runtime->LoadNetwork(target.id, std::move(optimized), error, properties) != armnn::Status::Success)
            throw std::runtime_error(error.empty() ? "NPU graph load failed" : error);
        target.loaded = true;

        target.inputSize = in * height * width;
        target.outputSize = out * height * width;
        target.input.reset(new CmaBuffer(target.inputSize));
        target.output.reset(new CmaBuffer(target.outputSize));
        target.feeds.emplace_back(0, armnn::ConstTensor(target.inputInfo, target.input->value.virt_addr));
        target.bindings.emplace_back(0, armnn::Tensor(target.outputInfo, target.output->value.virt_addr));
    }

    static std::string attribute(const std::string &line, const std::string &name)
    {
        auto start = line.find(name + " : ");
        if (start == std::string::npos) return "";
        start += name.size() + 3;
        return line.substr(start, line.find("\\l", start) - start);
    }

    /* 逐层：CMA 上传 → 硬件卷积 → CMA 回读 → CPU ReLU+MaxPool。 */
    const float *forward(Context &context, const float *input)
    {
        upload.resize(layers[0].inputSize);
        for (size_t i = 0; i < upload.size(); ++i)
            upload[i] = quantize(double(input[i]) / scales[0].sx);

        for (unsigned layer = 0; layer < 4; ++layer) {
            NpuLayer &target = layers[layer];
            std::memcpy(target.input->value.virt_addr, upload.data(), target.inputSize);
            std::memset(target.output->value.virt_addr, 0xa5, target.outputSize);
            cma_mem_sync_for_device(target.input->value.phys_addr, target.inputSize);
            cma_mem_sync_for_device(target.output->value.phys_addr, target.outputSize);
            if (runtime->EnqueueWorkload(target.id, target.feeds, target.bindings) != armnn::Status::Success)
                throw std::runtime_error("NPU EnqueueWorkload failed");
            cma_mem_sync_for_cpu(target.output->value.phys_addr, target.outputSize);
            const auto *output = static_cast<const uint8_t *>(target.output->value.virt_addr);

            const unsigned height = kHeight >> layer, width = kWidth >> layer;
            const unsigned count = kChannels[layer + 1] * (height / 2) * (width / 2);
            upload.resize(count);
            reluPool(output, upload.data(), kChannels[layer + 1], height, width);
        }
        for (size_t k = 0; k < upload.size(); ++k) {
            context.maps[4][k] = (int(upload[k]) - 128) * scales[3].sy;
        }
        return context.maps[4].data();
    }
};

#endif /* MLK_FAULT_HAVE_ARMNN */

Context::~Context()
{
#if defined(MLK_FAULT_HAVE_ARMNN)
    /* NpuBackend 的析构按“图 → CMA 缓冲 → runtime → CMA 会话”逆序释放。 */
    delete npu;
    npu = nullptr;
#endif
}

} // namespace

extern "C" {

int mlk_fault_abi(void) { return MLK_FAULT_ENGINE_ABI; }

void *mlk_fault_create(const char *root, int backend, char *error, size_t capacity)
{
    setError(error, capacity, "");
    if (!root || !*root) {
        setError(error, capacity, "runtime package root is empty");
        return nullptr;
    }
    if (backend != MLK_FAULT_CPU && backend != MLK_FAULT_CPU_CNN_NPU) {
        setError(error, capacity, "unsupported backend");
        return nullptr;
    }
#if !defined(MLK_FAULT_HAVE_ARMNN)
    if (backend == MLK_FAULT_CPU_CNN_NPU) {
        setError(error, capacity, "cpu_cnn_npu backend is not available in this library");
        return nullptr;
    }
#endif
    try {
        auto context = std::unique_ptr<Context>(new Context());
        context->backend = backend;
        context->weights.load(root);

        const auto thresholds = readBinary<float>(joinPath(root, kThresholdFile));
        if (thresholds.size() != 8) throw std::runtime_error("thresholds.bin must hold 8 floats");
        for (unsigned i = 0; i < thresholds.size(); ++i) {
            if (!std::isfinite(thresholds[i])) throw std::runtime_error("nonfinite threshold");
            context->thresholds[i] = thresholds[i];
        }

        context->hidden.resize(128);
        context->maps[0].resize(kChannels[0] * kHeight * kWidth);
        for (unsigned layer = 1; layer < 5; ++layer)
            context->maps[layer].resize(static_cast<size_t>(kChannels[layer]) * (kHeight >> layer)
                                        * (kWidth >> layer));
        if (context->maps[4].size() != 768) throw std::runtime_error("unexpected CNN output size");

        /* 预先解析全部张量，缺失或维度不符在创建阶段暴露，不在每窗推断中失败 */
        context->weights.at("backbone.0.0.weight", kChannels[1] * kChannels[0] * 9);
        context->weights.at("backbone.1.0.weight", kChannels[2] * kChannels[1] * 9);
        context->weights.at("backbone.2.0.weight", kChannels[3] * kChannels[2] * 9);
        context->weights.at("backbone.3.0.weight", kChannels[4] * kChannels[3] * 9);
        context->weights.at("tf_encoder.1.weight", 64 * 768);
        context->weights.at("spectrum_encoder.1.weight", 32 * 2307);
        context->weights.at("stats_encoder.0.weight", 16 * 12);
        context->weights.at("pose_encoder.0.weight", 16 * 8);
        context->weights.at("fast_encoder.1.weight", 32 * 240);
        context->weights.at("slow_encoder.1.weight", 64 * 540);
        context->weights.at("fusion.0.weight", 128 * 224);
        context->weights.at("head.weight", 9 * 128);

#if defined(MLK_FAULT_HAVE_ARMNN)
        if (backend == MLK_FAULT_CPU_CNN_NPU)
            context->npu = new NpuBackend(context->weights, root);
#else
        (void)root;
#endif
        return context.release();
    } catch (const std::exception &failure) {
        setError(error, capacity, std::string("cannot open runtime package: ") + failure.what());
        return nullptr;
    } catch (...) {
        setError(error, capacity, "cannot open runtime package: unknown failure");
        return nullptr;
    }
}

int mlk_fault_predict_masked(void *handle, const float *input, size_t count, uint32_t excluded,
                            mlk_fault_result *result, char *error, size_t capacity)
{
    setError(error, capacity, "");
    if (!result) return -1;
    /* 调用方在进入前填写的结构大小必须为 32，先取值再清零 */
    const uint32_t declared = result->struct_size;
    clearResult(result, MLK_FAULT_CPU);
    if (!handle) {
        setError(error, capacity, "context is null");
        return -1;
    }
    auto &context = *static_cast<Context *>(handle);
    if ((excluded & ~31u) || excluded == 31u) {
        setError(error, capacity, "class mask must leave at least one of five classes");
        return -1;
    }
    if (declared != 32) {
        setError(error, capacity, "result struct_size must be 32");
        return -1;
    }
    if (!input || count != kInputFloats) {
        setError(error, capacity, "input must hold 27683 floats");
        return -1;
    }
    for (unsigned i = 0; i < kInputFloats; ++i) {
        if (!std::isfinite(input[i])) {
            setError(error, capacity, "input contains non-finite value");
            return -1;
        }
    }
    try {
        const auto begin = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(context.mutex);
        const float *cnn = nullptr;
        if (context.backend == MLK_FAULT_CPU_CNN_NPU) {
#if defined(MLK_FAULT_HAVE_ARMNN)
            if (!context.npu) throw std::runtime_error("NPU backend is not initialized");
            cnn = context.npu->forward(context, input);
#else
            throw std::runtime_error("cpu_cnn_npu backend is not available in this library");
#endif
        } else {
            std::copy_n(input, kTimeFrequencyFloats, context.maps[0].data());
            cnn = cnnForward(context, context.maps[0].data());
        }
        const auto raw = head(context, cnn, input);
        for (unsigned k = 0; k < raw.size(); ++k)
            if (!std::isfinite(raw[k])) throw std::runtime_error("non-finite model output");
        const Decoded decoded = decode(raw, context.thresholds, excluded);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin);

        result->valid = 1;
        result->class_index = decoded.class_index;
        result->grade = decoded.grade;
        result->confidence = decoded.confidence;
        result->backend = static_cast<uint32_t>(context.backend);
        result->infer_us = static_cast<uint32_t>(std::max<long long>(0, elapsed.count()));
        return 0;
    } catch (const std::exception &failure) {
        clearResult(result, static_cast<uint32_t>(context.backend));
        setError(error, capacity, std::string("predict failed: ") + failure.what());
        return -1;
    } catch (...) {
        clearResult(result, static_cast<uint32_t>(context.backend));
        setError(error, capacity, "predict failed: unknown failure");
        return -1;
    }
}

int mlk_fault_predict(void *handle, const float *input, size_t count, mlk_fault_result *result,
                      char *error, size_t capacity)
{
    return mlk_fault_predict_masked(handle, input, count, 0u, result, error, capacity);
}

void mlk_fault_destroy(void *handle)
{
    if (!handle) return;
    auto *context = static_cast<Context *>(handle);
    {
        std::lock_guard<std::mutex> lock(context->mutex);
    }
    delete context; /* ARMNN 构建时由 Context 析构释放 NPU 后端 */
}

} // extern "C"
