// Fixed-input FKP-DVGT inference using vendor Arm NN layers and host GELU.
// Six network segments preserve the original five exact GELU boundaries.
#include <armnn/ArmNN.hpp>
#include <armnn/Logging.hpp>
#include <armnnALUtils/cma_mem_init.hpp>
#include "model_config.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Quantization { unsigned inputs, outputs; float sx, sw, sy; };
std::map<std::string, Quantization> quantization;
bool npuRequested = false;
void readQuantization(const std::string& root) {
    const char* selected = std::getenv("F3P_QUANT_PLAN");
    if (!selected || std::string(selected) == "none") return;
    std::ifstream input(root + "/" + selected);
    if (!input) throw std::runtime_error("Cannot read quantization plan");
    std::string name; Quantization q;
    while (input >> name >> q.inputs >> q.outputs >> q.sx >> q.sw >> q.sy) {
        if (!std::isfinite(q.sx) || !std::isfinite(q.sw) || !std::isfinite(q.sy)
            || q.sx <= 0 || q.sw <= 0 || q.sy <= 0 || !quantization.emplace(name, q).second)
            throw std::runtime_error("Invalid quantization plan");
    }
    if (!input.eof() || quantization.empty()) throw std::runtime_error("Invalid/empty quantization plan");
}
struct CmaSession {
    bool owned = false;
    CmaSession() {
        if (npuRequested && !cma_mem_is_init()) { cma_mem_init(); owned = cma_mem_is_init();
            if (!owned) throw std::runtime_error("CMA initialization failed"); }
    }
    ~CmaSession() { if (owned) cma_mem_deinit(); }
};
using Shape = std::vector<unsigned>;
using Layer = armnn::IConnectableLayer;
std::size_t elements(const Shape& s) {
    return std::accumulate(s.begin(), s.end(), std::size_t(1), std::multiplies<std::size_t>());
}
armnn::TensorInfo info(const Shape& s) {
    return armnn::TensorInfo(armnn::TensorShape(static_cast<unsigned>(s.size()), s.data()), armnn::DataType::Float32);
}
std::vector<float> readFloats(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot open " + path);
    const auto n = f.tellg();
    if (n <= 0 || n % sizeof(float)) throw std::runtime_error("Invalid float file: " + path);
    std::vector<float> v(static_cast<std::size_t>(n) / sizeof(float));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(v.data()), n)) throw std::runtime_error("Cannot read " + path);
    for (float x : v) if (!std::isfinite(x)) throw std::runtime_error("Nonfinite data: " + path);
    return v;
}
struct Entry { Shape shape; std::size_t count, offset; };
struct Weights {
    std::vector<float> values;
    std::map<std::string, Entry> entries;
    explicit Weights(const std::string& root) : values(readFloats(root + "/weights.bin")) {
        std::ifstream file(root + "/weights.tsv");
        std::string name;
        unsigned rank;
        while (file >> name >> rank) {
            Entry e; e.shape.resize(rank);
            for (auto& d : e.shape) file >> d;
            file >> e.count >> e.offset;
            if (!file || elements(e.shape) != e.count || e.offset > values.size() || e.count > values.size() - e.offset)
                throw std::runtime_error("Invalid weight entry: " + name);
            if (!entries.emplace(name, e).second) throw std::runtime_error("Duplicate weight entry");
        }
        if (entries.empty()) throw std::runtime_error("No weights loaded");
    }
    const Entry& at(const std::string& name) const {
        auto it = entries.find(name);
        if (it == entries.end()) throw std::runtime_error("Missing weight " + name);
        return it->second;
    }
    const float* data(const std::string& name) const { return values.data() + at(name).offset; }
};
struct Value { Layer* layer; Shape shape; };
struct Graph {
    const Weights& weights;
    armnn::INetworkPtr network = armnn::INetwork::Create();
    std::list<std::vector<float>> constants;
    std::list<std::vector<std::uint8_t>> quantizedConstants;
    std::list<std::vector<std::int32_t>> quantizedBiases;
    unsigned quantizedLayers = 0;
    std::vector<armnn::TensorInfo> inputs, outputs;
    std::map<std::string, std::pair<Value, Value>> stems;
    unsigned nextName = 0;
    explicit Graph(const Weights& w) : weights(w) {}
    std::string name(const std::string& op) { return op + "_" + std::to_string(nextName++); }
    Value result(Layer* layer, Shape shape) {
        if (!layer) throw std::runtime_error("Null native layer");
        layer->GetOutputSlot(0).SetTensorInfo(info(shape));
        return {layer, std::move(shape)};
    }
    static void connect(Value x, Layer* to, unsigned slot = 0) { x.layer->GetOutputSlot(0).Connect(to->GetInputSlot(slot)); }
    Value input(Shape shape) {
        auto* layer = network->AddInputLayer(static_cast<armnn::LayerBindingId>(inputs.size()));
        inputs.push_back(info(shape));
        return result(layer, std::move(shape));
    }
    void output(Value x) {
        auto* layer = network->AddOutputLayer(static_cast<armnn::LayerBindingId>(outputs.size()));
        connect(x, layer);
        outputs.push_back(info(x.shape));
    }
    Value constant(Shape shape, const float* data, const std::string& label) {
        auto i = info(shape); i.SetConstant(true);
        return result(network->AddConstantLayer(armnn::ConstTensor(i, data), label.c_str()), std::move(shape));
    }
    Value owned(Shape shape, std::vector<float> data) {
        if (elements(shape) != data.size()) throw std::runtime_error("Constant size mismatch");
        constants.push_back(std::move(data));
        return constant(std::move(shape), constants.back().data(), name("constant"));
    }
    Value scalar(float x) { return owned({1}, {x}); }
    Value weight(const std::string& label) { return constant(weights.at(label).shape, weights.data(label), label); }
    Value reshape(Value x, Shape shape) {
        if (x.shape == shape) return x;
        if (elements(shape) != elements(x.shape)) throw std::runtime_error("Reshape element mismatch");
        armnn::ReshapeDescriptor d; d.m_TargetShape = info(shape).GetShape();
        auto* layer = network->AddReshapeLayer(d, name("reshape").c_str()); connect(x, layer);
        return result(layer, std::move(shape));
    }
    Value align(Value x, std::size_t rank) {
        Shape s(rank - x.shape.size(), 1); s.insert(s.end(), x.shape.begin(), x.shape.end());
        return reshape(x, std::move(s));
    }
    Value binary(Value a, Value b, armnn::BinaryOperation operation) {
        const auto rank = std::max(a.shape.size(), b.shape.size());
        a = align(a, rank); b = align(b, rank);
        Shape s(rank);
        for (std::size_t k = 0; k < rank; ++k) {
            if (a.shape[k] != b.shape[k] && a.shape[k] != 1 && b.shape[k] != 1)
                throw std::runtime_error("Invalid broadcast");
            s[k] = std::max(a.shape[k], b.shape[k]);
        }
        auto* layer = network->AddElementwiseBinaryLayer(armnn::ElementwiseBinaryDescriptor(operation), name("binary").c_str());
        connect(a, layer); connect(b, layer, 1); return result(layer, std::move(s));
    }
    Value add(Value a, Value b) { return binary(a, b, armnn::BinaryOperation::Add); }
    Value sub(Value a, Value b) { return binary(a, b, armnn::BinaryOperation::Sub); }
    Value mul(Value a, Value b) { return binary(a, b, armnn::BinaryOperation::Mul); }
    Value div(Value a, Value b) { return binary(a, b, armnn::BinaryOperation::Div); }
    Value activation(Value x, armnn::ActivationFunction fn, float a = 0, float b = 0) {
        auto* layer = network->AddActivationLayer(armnn::ActivationDescriptor(fn, a, b), name("activation").c_str());
        connect(x, layer); return result(layer, x.shape);
    }
    Value lessFloat(Value a, Value b) {
        auto difference = sub(b, a);
        auto* layer = network->AddElementwiseUnaryLayer(armnn::ElementwiseUnaryDescriptor(armnn::UnaryOperation::Ceil), name("ceil").c_str());
        connect(difference, layer);
        return activation(result(layer, difference.shape), armnn::ActivationFunction::BoundedReLu, 1, 0);
    }
    Value transpose(Value x, const Shape& permutation) {
        Shape s(permutation.size());
        for (std::size_t k = 0; k < s.size(); ++k) s[k] = x.shape.at(permutation[k]);
        armnn::TransposeDescriptor d(armnn::PermutationVector(permutation.data(), static_cast<unsigned>(permutation.size())));
        auto* layer = network->AddTransposeLayer(d, name("transpose").c_str()); connect(x, layer);
        return result(layer, std::move(s));
    }
    Value slice(Value x, unsigned axis, unsigned start, unsigned length) {
        Shape begin(x.shape.size(), 0), size = x.shape;
        begin.at(axis) = start; size.at(axis) = length;
        if (start + length > x.shape.at(axis)) throw std::runtime_error("Invalid slice");
        auto* layer = network->AddSliceLayer(armnn::SliceDescriptor(begin, size), name("slice").c_str());
        connect(x, layer); return result(layer, std::move(size));
    }
    Value concat(const std::vector<Value>& xs, unsigned axis) {
        if (xs.empty()) throw std::runtime_error("Empty concat");
        std::vector<armnn::TensorShape> shapes;
        Shape s = xs.front().shape; s.at(axis) = 0;
        for (auto x : xs) { shapes.push_back(info(x.shape).GetShape()); s.at(axis) += x.shape.at(axis); }
        auto d = armnn::CreateDescriptorForConcatenation(shapes.begin(), shapes.end(), axis);
        auto* layer = network->AddConcatLayer(d, name("concat").c_str());
        for (std::size_t k = 0; k < xs.size(); ++k) connect(xs[k], layer, static_cast<unsigned>(k));
        return result(layer, std::move(s));
    }
    Value reduce(Value x, unsigned axis, bool mean) {
        Shape s = x.shape; s.at(axis) = 1;
        armnn::ReduceDescriptor d; d.m_KeepDims = true; d.m_vAxis = {axis};
        d.m_ReduceOperation = mean ? armnn::ReduceOperation::Mean : armnn::ReduceOperation::Sum;
        auto* layer = network->AddReduceLayer(d, name("reduce").c_str()); connect(x, layer);
        return result(layer, std::move(s));
    }
    Value softmax(Value x) {
        armnn::SoftmaxDescriptor d; d.m_Axis = static_cast<int>(x.shape.size() - 1);
        auto* layer = network->AddSoftmaxLayer(d, name("softmax").c_str()); connect(x, layer);
        return result(layer, x.shape);
    }
    Value bmm(Value a, Value b) {
        if (a.shape.size() != 3 || b.shape.size() != 3 || a.shape[0] != b.shape[0] || a.shape[2] != b.shape[1])
            throw std::runtime_error("BMM dimensions");
        auto* layer = network->AddBatchMatMulLayer(armnn::BatchMatMulDescriptor(), name("bmm").c_str());
        connect(a, layer); connect(b, layer, 1);
        return result(layer, {a.shape[0], a.shape[1], b.shape[2]});
    }
    Value fc(Value x, const std::string& label) {
        const auto& e = weights.at(label);
        const unsigned units = e.shape[0];
        if (x.shape.size() != 2 || e.count != std::size_t(units) * x.shape[1]) throw std::runtime_error("FC dimensions: " + label);
        auto selected = quantization.find(label);
        if (selected != quantization.end()) {
            const auto q = selected->second;
            const unsigned rows = x.shape[0], ic = (q.inputs + 7) / 8 * 8, oc = (units + 7) / 8 * 8;
            if (q.inputs != x.shape[1] || q.outputs != units) throw std::runtime_error("Quantized FC shape mismatch");
            if (ic != q.inputs) x = concat({x, owned({rows, ic - q.inputs}, std::vector<float>(rows * (ic - q.inputs), 0))}, 1);
            auto qinfo = [](Shape shape, float scale, bool constant = false) {
                return armnn::TensorInfo(armnn::TensorShape(static_cast<unsigned>(shape.size()), shape.data()),
                                         armnn::DataType::QAsymmU8, scale, 128, constant);
            };
            auto* quantize = network->AddQuantizeLayer((label + "_quantize").c_str()); connect(x, quantize);
            quantize->GetOutputSlot(0).SetTensorInfo(qinfo({rows, ic}, q.sx));
            quantizedConstants.emplace_back(oc * ic, 128);
            auto& values = quantizedConstants.back();
            for (unsigned o = 0; o < units; ++o) for (unsigned i = 0; i < q.inputs; ++i) {
                const int value = static_cast<int>(std::round(weights.data(label)[o * q.inputs + i] / q.sw)) + 128;
                values[o * ic + i] = static_cast<std::uint8_t>(std::max(0, std::min(255, value)));
            }
            auto wi = qinfo({oc, ic}, q.sw, true);
            auto* w = network->AddConstantLayer(armnn::ConstTensor(wi, values.data()), (label + "_u8").c_str());
            w->GetOutputSlot(0).SetTensorInfo(wi);
            quantizedBiases.emplace_back(oc, 0);
            armnn::TensorInfo bi(armnn::TensorShape({oc}), armnn::DataType::Signed32, q.sx * q.sw, 0, true);
            auto* b = network->AddConstantLayer(armnn::ConstTensor(bi, quantizedBiases.back().data()));
            b->GetOutputSlot(0).SetTensorInfo(bi);
            armnn::FullyConnectedDescriptor descriptor; descriptor.m_TransposeWeightMatrix = true; descriptor.m_BiasEnabled = true;
            auto* linear = network->AddFullyConnectedLayer(descriptor, (label + "_quantized").c_str());
            quantize->GetOutputSlot(0).Connect(linear->GetInputSlot(0));
            w->GetOutputSlot(0).Connect(linear->GetInputSlot(1)); b->GetOutputSlot(0).Connect(linear->GetInputSlot(2));
            linear->GetOutputSlot(0).SetTensorInfo(qinfo({rows, oc}, q.sy));
            auto* dequantize = network->AddDequantizeLayer((label + "_dequantize").c_str());
            linear->GetOutputSlot(0).Connect(dequantize->GetInputSlot(0));
            ++quantizedLayers;
            auto value = result(dequantize, {rows, oc});
            return oc == units ? value : slice(value, 1, 0, units);
        }
        auto w = constant({units, x.shape[1]}, weights.data(label), label);
        armnn::FullyConnectedDescriptor d; d.m_TransposeWeightMatrix = true;
        auto* layer = network->AddFullyConnectedLayer(d, label.c_str());
        connect(x, layer); connect(w, layer, 1); return result(layer, {x.shape[0], units});
    }
    Value kan(Value x, const std::string& prefix) {
        const unsigned features = x.shape.back();
        const unsigned batch = static_cast<unsigned>(elements(x.shape) / features);
        const auto& gridInfo = weights.at(prefix + ".grid");
        if (gridInfo.shape != Shape{features, 13}) throw std::runtime_error("Unexpected KAN grid");
        const float* grid = weights.data(prefix + ".grid");
        std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(x.layer));
        key.append(reinterpret_cast<const char*>(grid), gridInfo.count * sizeof(float));
        auto found = stems.find(key);
        Value base, spline;
        if (found != stems.end()) { base = found->second.first; spline = found->second.second; }
        else {
            auto flat = reshape(x, {batch, features});
            base = mul(flat, activation(flat, armnn::ActivationFunction::Sigmoid));
            auto expanded = reshape(flat, {batch, features, 1});
            auto gridPart = [&](unsigned start, unsigned count, int otherStart = -1) {
                std::vector<float> values(features * count);
                for (unsigned f = 0; f < features; ++f) for (unsigned j = 0; j < count; ++j) {
                    float v = grid[f * 13 + start + j];
                    if (otherStart >= 0) v -= grid[f * 13 + static_cast<unsigned>(otherStart) + j];
                    values[f * count + j] = v;
                }
                return owned({1, features, count}, std::move(values));
            };
            auto lower = sub(scalar(1), lessFloat(expanded, gridPart(0, 12)));
            auto upper = lessFloat(expanded, gridPart(1, 12));
            auto bases = mul(lower, upper);
            for (unsigned k = 1; k <= 3; ++k) {
                const unsigned count = 12 - k;
                auto left = mul(div(sub(expanded, gridPart(0, count)), gridPart(k, count, 0)), slice(bases, 2, 0, count));
                auto right = mul(div(sub(gridPart(k + 1, count), expanded), gridPart(k + 1, count, 1)), slice(bases, 2, 1, count));
                bases = add(left, right);
            }
            spline = reshape(bases, {batch, features * 9});
            stems.emplace(std::move(key), std::make_pair(base, spline));
        }
        auto y = add(fc(base, prefix + ".base_weight"), fc(spline, prefix + ".spline_weight"));
        Shape s = x.shape; s.back() = y.shape.back(); return reshape(y, std::move(s));
    }
    Value pcc(Value x) {
        auto centered = sub(x, reduce(x, 2, true));
        auto dot = bmm(centered, transpose(centered, {0, 2, 1}));
        auto norm = activation(reduce(mul(centered, centered), 2, false), armnn::ActivationFunction::Sqrt);
        return div(dot, add(bmm(norm, transpose(norm, {0, 2, 1})), scalar(1e-8f)));
    }
    Value layerNorm(Value x, const std::string& prefix) {
        auto centered = sub(x, reduce(x, static_cast<unsigned>(x.shape.size() - 1), true));
        auto variance = reduce(mul(centered, centered), static_cast<unsigned>(x.shape.size() - 1), true);
        auto normalized = div(centered, activation(add(variance, scalar(1e-5f)), armnn::ActivationFunction::Sqrt));
        return add(mul(normalized, weight(prefix + ".weight")), weight(prefix + ".bias"));
    }
    std::pair<Value, Value> beforeGelu(Value x, Value adjacency, unsigned block, bool temporal) {
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
    Value afterGelu(Value gelu, Value residual, unsigned block, bool temporal) {
        const std::string kind = temporal ? "temp" : "spat";
        const std::string prefix = std::string(temporal ? "tvgtformer_blocks." : "svgtformer_blocks.") + std::to_string(block);
        return add(layerNorm(kan(gelu, prefix + ".feed_forward_" + kind + ".2"), prefix + ".layer_norm2_" + kind), residual);
    }
};

std::unique_ptr<Graph> buildStage(const Weights& weights, unsigned stage) {
    auto g = std::make_unique<Graph>(weights);
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
struct View { const float* data; std::size_t size; };
View view(const std::vector<float>& v) { return {v.data(), v.size()}; }
struct Stage {
    armnn::IRuntime& runtime;
    std::unique_ptr<Graph> graph;
    armnn::NetworkId id;
    std::vector<std::vector<float>> buffers;
    Stage(armnn::IRuntime& r, const Weights& w, unsigned index,
          const std::vector<armnn::BackendId>& preferences, const std::string& result, bool profile)
        : runtime(r), graph(buildStage(w, index)) {
        armnn::OptimizerOptionsOpaque options; options.SetImportEnabled(false); options.SetExportEnabled(false);
        options.SetProfilingEnabled(profile);
        std::vector<std::string> messages;
        auto optimized = armnn::Optimize(*graph->network, preferences, runtime.GetDeviceSpec(), options,
                                        armnn::Optional<std::vector<std::string>&>(messages));
        if (!optimized) throw std::runtime_error("Native Optimize returned null");
        std::ostringstream text; optimized->SerializeToDot(text);
        if (npuRequested) {
            unsigned npuLayers = 0;
            std::istringstream lines(text.str());
            for (std::string line; std::getline(lines, line);)
                if (line.find("LayerType : ALHardNPU") != std::string::npos && line.find("BackendID : Alnpu") != std::string::npos) ++npuLayers;
            if (npuLayers != graph->quantizedLayers) throw std::runtime_error("Quantized layers did not all map to NPU");
            std::cout << "segment=" << index << " physical_npu_layers=" << npuLayers << '\n';
        }
        std::ofstream dot(result + "/segment_" + std::to_string(index) + ".dot");
        if (!dot || optimized->SerializeToDot(dot) != armnn::Status::Success)
            throw std::runtime_error("Cannot write optimized segment graph");
        std::ofstream diagnostics(result + "/segment_" + std::to_string(index) + "_optimizer.txt");
        for (const auto& message : messages) diagnostics << message << '\n';
        std::cout << "segment=" << index << " optimizer_messages=" << messages.size() << '\n';
        std::string error;
        armnn::INetworkProperties properties(false, armnn::MemorySource::Undefined, armnn::MemorySource::Undefined, profile);
        if (runtime.LoadNetwork(id, std::move(optimized), error, properties) != armnn::Status::Success)
            throw std::runtime_error("Native LoadNetwork: " + error);
        if (profile) {
            auto profiler = runtime.GetProfiler(id);
            if (profiler) profiler->EnableProfiling(true);
        }
        for (const auto& out : graph->outputs) buffers.emplace_back(out.GetNumElements());
    }
    const std::vector<std::vector<float>>& run(const std::vector<View>& feeds) {
        if (feeds.size() != graph->inputs.size()) throw std::runtime_error("Input binding count mismatch");
        armnn::InputTensors inputs;
        for (std::size_t k = 0; k < feeds.size(); ++k) {
            auto i = graph->inputs[k]; i.SetConstant(true);
            if (feeds[k].size != i.GetNumElements()) throw std::runtime_error("Input element mismatch");
            inputs.emplace_back(static_cast<armnn::LayerBindingId>(k), armnn::ConstTensor(i, feeds[k].data));
        }
        armnn::OutputTensors outputs;
        for (std::size_t k = 0; k < buffers.size(); ++k)
            outputs.emplace_back(static_cast<armnn::LayerBindingId>(k), armnn::Tensor(graph->outputs[k], buffers[k].data()));
        if (runtime.EnqueueWorkload(id, inputs, outputs) != armnn::Status::Success) throw std::runtime_error("Native EnqueueWorkload failed");
        return buffers;
    }
    ~Stage() { runtime.UnloadNetwork(id); }
};
void gelu(const std::vector<float>& x, std::vector<float>& y) {
    y.resize(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float e = std::erf(x[i] / 1.4142135623730951f);
        y[i] = (x[i] * (1.0f + e)) * 0.5f;
    }
}
void dump(const std::string& path, const std::vector<float>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f || !f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size() * sizeof(float))))
        throw std::runtime_error("Cannot write " + path);
}
}  // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf << std::setprecision(10);
    std::string phase = "arguments";
    try {
        if (argc != 4) throw std::runtime_error("Usage: rul_native BUNDLE_ROOT COUNT RESULT_DIRECTORY (COUNT=0 means all)");
        const std::string root = argv[1], result = argv[3], countText = argv[2];
        if (countText.empty() || countText[0] == '-') throw std::runtime_error("Invalid count");
        std::size_t consumed = 0, count = std::stoul(countText, &consumed);
        if (consumed != countText.size()) throw std::runtime_error("Invalid count");
        const std::uint16_t endian = 1;
        if (sizeof(float) != 4 || *reinterpret_cast<const std::uint8_t*>(&endian) != 1) throw std::runtime_error("Little-endian float32 is required");
        const char* requested = std::getenv("F3P_CPU_BACKENDS");
        const std::string backend = requested ? requested : "CpuRef";
        std::vector<armnn::BackendId> preferences;
        if (backend == "CpuAcc,CpuRef") preferences = {armnn::BackendId("CpuAcc"), armnn::BackendId("CpuRef")};
        else if (backend == "Alnpu,CpuAcc,CpuRef") {
            npuRequested = true;
            preferences = {armnn::BackendId("Alnpu"), armnn::BackendId("CpuAcc"), armnn::BackendId("CpuRef")};
        }
        else if (backend == "CpuRef" || backend == "CpuAcc") preferences.emplace_back(backend);
        else throw std::runtime_error("F3P_CPU_BACKENDS must be CpuRef, CpuAcc or CpuAcc,CpuRef");
        const char* profileValue = std::getenv("F3P_PROFILE");
        const bool profile = profileValue && std::string(profileValue) == "1";
        armnn::ConfigureLogging(true, false, armnn::LogSeverity::Error);
        phase = "files";
        auto inputs = readFloats(root + "/data/input_f32.bin");
        auto references = readFloats(root + "/data/reference_f32.bin");
        auto labels = readFloats(root + "/data/labels_f32.bin");
        if (inputs.size() % 2530 || inputs.size() / 2530 != references.size() || labels.size() != references.size())
            throw std::runtime_error("Replay data lengths differ");
        count = count ? count : references.size();
        if (count > references.size()) throw std::runtime_error("Requested too many windows");
        Weights weights(root);
        readQuantization(root);
        if (npuRequested && quantization.empty()) throw std::runtime_error("NPU run requires an explicit quantization plan");
        phase = "runtime";
        CmaSession cma;
        armnn::IRuntime::CreationOptions creation;
        if (npuRequested) {
            auto allocator = std::make_shared<armnn::CmaAllocator>();
            creation.m_CustomAllocatorMap["CpuRef"] = allocator; creation.m_CustomAllocatorMap["CpuAcc"] = allocator;
        }
        auto runtime = armnn::IRuntime::Create(creation);
        std::cout << "requested_backend=" << backend << " onnx_parser_used=false host_gelu=std::erf samples=" << count
                  << " profile_first_window=" << profile << " available_backends=";
        for (const auto& id : runtime->GetDeviceSpec().GetSupportedBackends()) std::cout << id.Get() << ',';
        std::cout << '\n';
        std::vector<std::unique_ptr<Stage>> stages;
        for (unsigned i = 0; i < 6; ++i) {
            phase = "build_segment_" + std::to_string(i);
            std::cout << "stage=" << phase << " begin\n";
            stages.push_back(std::make_unique<Stage>(*runtime, weights, i, preferences, result, profile));
            std::cout << "stage=" << phase << " passed\n";
        }
        std::ofstream csv(result + "/predictions.csv");
        if (!csv) throw std::runtime_error("Cannot create prediction CSV");
        csv << std::setprecision(10) << "sample_index,prediction,reference,absolute_error,target,forward_us\n";
        phase = "inference";
        double maxError = 0, squared = 0, absolute = 0, totalUs = 0;
        std::size_t mismatches = 0;
        std::vector<float> activated;
        for (std::size_t n = 0; n < count; ++n) {
            const auto before = std::chrono::steady_clock::now();
            const auto& initial = stages[0]->run({{inputs.data() + n * 2530, 2530}});
            const std::vector<float>* pre = &initial[0];
            const std::vector<float>* residual = &initial[1];
            if (n == 0) { dump(result + "/a_temp.bin", initial[2]); dump(result + "/a_spat.bin", initial[3]); }
            for (unsigned i = 1; i <= 4; ++i) {
                if (n == 0) dump(result + "/pre_gelu_" + std::to_string(i - 1) + ".bin", *pre);
                gelu(*pre, activated);
                std::vector<View> feeds{view(activated), view(*residual)};
                if (i < 4) feeds.push_back(view(initial[i % 2 == 0 ? 2 : 3]));
                const auto& next = stages[i]->run(feeds);
                pre = &next[0];
                if (i < 4) residual = &next[1];
            }
            if (n == 0) dump(result + "/pre_gelu_4.bin", *pre);
            gelu(*pre, activated);
            const float prediction = stages[5]->run({view(activated)})[0][0];
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - before).count();
            if (n == 0 && profile) {
                for (unsigned k = 0; k < stages.size(); ++k) {
                    auto profiler = runtime->GetProfiler(stages[k]->id);
                    if (profiler) {
                        profiler->EnableProfiling(false);
                        std::ofstream output(result + "/segment_" + std::to_string(k) + "_profile.json");
                        if (!output) throw std::runtime_error("Cannot write execution profile");
                        profiler->Print(output);
                    }
                }
            }
            if (!std::isfinite(prediction)) throw std::runtime_error("Nonfinite prediction");
            const double error = std::abs(static_cast<double>(prediction) - references[n]);
            if (error > 1e-4 + 1e-4 * std::abs(references[n])) ++mismatches;
            maxError = std::max(maxError, error);
            const double difference = prediction - static_cast<double>(labels[n]);
            squared += difference * difference; absolute += std::abs(difference); totalUs += us;
            csv << n << ',' << prediction << ',' << references[n] << ',' << error << ',' << labels[n] << ',' << us << '\n';
            csv.flush();
            if (n == 0 || (n + 1) % 10 == 0 || n + 1 == count)
                std::cout << "sample=" << n << " prediction=" << prediction << " reference=" << references[n] << " absolute_error=" << error << '\n';
        }
        std::cout << "result=inference_completed samples=" << count << " max_reference_error=" << maxError
                  << " mismatches=" << mismatches << " rmse=" << std::sqrt(squared / count) << " mae=" << absolute / count
                  << " mean_forward_us=" << totalUs / count << "\nrul_unit=observed_aging_test_session\n";
        // Quantized deviations are reported separately; no application acceptance threshold is implied.
        std::cout << "quantized_branches=" << quantization.size() << " accuracy_acceptance=TBD\n";
        return quantization.empty() && mismatches ? 70 : 0;
    } catch (const std::exception& error) {
        std::cerr << "result=failed stage=" << phase << " error=" << error.what() << '\n';
        return 2;
    }
}
