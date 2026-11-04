// Validate vendor Arm NN layer execution independently of the ONNX parser.
#include <armnn/ArmNN.hpp>
#include <armnn/Logging.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Layer = armnn::IConnectableLayer;

armnn::TensorInfo tensor(std::initializer_list<unsigned> shape,
                         armnn::DataType type = armnn::DataType::Float32) {
    return armnn::TensorInfo(armnn::TensorShape(static_cast<unsigned>(shape.size()), shape.begin()), type);
}

struct Data {
    armnn::TensorInfo info;
    std::vector<float> floats;
    std::vector<std::uint8_t> booleans;
    const void* pointer() const {
        return info.GetDataType() == armnn::DataType::Boolean
            ? static_cast<const void*>(booleans.data()) : static_cast<const void*>(floats.data());
    }
};

struct Test {
    armnn::INetworkPtr network = armnn::INetwork::Create();
    std::vector<Data> inputs;
    std::vector<float> weights;
    armnn::TensorInfo output;
    std::vector<float> expected;

    Layer* input(Data data) {
        const auto index = static_cast<armnn::LayerBindingId>(inputs.size());
        inputs.push_back(std::move(data));
        auto* layer = network->AddInputLayer(index);
        layer->GetOutputSlot(0).SetTensorInfo(inputs.back().info);
        return layer;
    }

    Layer* floating(std::initializer_list<unsigned> shape, std::initializer_list<float> values) {
        return input({tensor(shape), values, {}});
    }

    Layer* boolean(std::initializer_list<unsigned> shape, std::initializer_list<std::uint8_t> values) {
        return input({tensor(shape, armnn::DataType::Boolean), {}, values});
    }

    void finish(Layer* layer, const armnn::TensorInfo& info, std::initializer_list<float> values) {
        output = info;
        expected = values;
        layer->GetOutputSlot(0).SetTensorInfo(info);
        layer->GetOutputSlot(0).Connect(network->AddOutputLayer(0)->GetInputSlot(0));
    }
};

void connect(Layer* from, Layer* to, unsigned slot = 0) {
    from->GetOutputSlot(0).Connect(to->GetInputSlot(slot));
}

// For finite inputs, clamp(ceil(right - left), 0, 1) is a float mask
// for left < right. This avoids Boolean-to-Float32 casts entirely.
Layer* floatLess(Test& test, Layer* left, Layer* right, const armnn::TensorInfo& info) {
    auto* difference = test.network->AddElementwiseBinaryLayer(
        armnn::ElementwiseBinaryDescriptor(armnn::BinaryOperation::Sub));
    connect(right, difference); connect(left, difference, 1);
    difference->GetOutputSlot(0).SetTensorInfo(info);
    auto* ceiling = test.network->AddElementwiseUnaryLayer(
        armnn::ElementwiseUnaryDescriptor(armnn::UnaryOperation::Ceil));
    connect(difference, ceiling);
    ceiling->GetOutputSlot(0).SetTensorInfo(info);
    auto* bounded = test.network->AddActivationLayer(
        armnn::ActivationDescriptor(armnn::ActivationFunction::BoundedReLu, 1, 0));
    connect(ceiling, bounded);
    bounded->GetOutputSlot(0).SetTensorInfo(info);
    return bounded;
}

Test build(const std::string& name) {
    Test t;
    if (name == "relu") {
        auto* input = t.floating({2, 3}, {-2, -1, 0, 0.5f, 1, 2});
        auto* layer = t.network->AddActivationLayer(armnn::ActivationDescriptor(armnn::ActivationFunction::ReLu));
        connect(input, layer);
        t.finish(layer, tensor({2, 3}), {0, 0, 0, 0.5f, 1, 2});
    } else if (name == "less" || name == "greater_equal") {
        auto* a = t.floating({2, 3}, {-2, -1, 0, 0.5f, 1, 2});
        auto* b = t.floating({2, 3}, {-1, -1, 1, 1, 0, 2});
        auto operation = name == "less" ? armnn::ComparisonOperation::Less : armnn::ComparisonOperation::GreaterOrEqual;
        auto* layer = t.network->AddComparisonLayer(armnn::ComparisonDescriptor(operation));
        connect(a, layer); connect(b, layer, 1);
        if (name == "less") t.finish(layer, tensor({2, 3}, armnn::DataType::Boolean), {1, 0, 1, 1, 0, 0});
        else t.finish(layer, tensor({2, 3}, armnn::DataType::Boolean), {0, 1, 0, 0, 1, 1});
    } else if (name == "broadcast_less") {
        auto* a = t.floating({2, 2, 1}, {-1, 0.5f, 1, 2});
        auto* b = t.floating({1, 2, 3}, {-1, 0, 1, -2, 1, 2});
        auto* layer = t.network->AddComparisonLayer(armnn::ComparisonDescriptor(armnn::ComparisonOperation::Less));
        connect(a, layer); connect(b, layer, 1);
        t.finish(layer, tensor({2, 2, 3}, armnn::DataType::Boolean), {0, 1, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0});
    } else if (name == "comparison_and") {
        auto* a = t.floating({2, 3}, {-2, -1, 0, 0.5f, 1, 2});
        auto* b = t.floating({2, 3}, {-1, -1, 1, 1, 0, 2});
        auto* comparison = t.network->AddComparisonLayer(
            armnn::ComparisonDescriptor(armnn::ComparisonOperation::Less));
        connect(a, comparison); connect(b, comparison, 1);
        comparison->GetOutputSlot(0).SetTensorInfo(tensor({2, 3}, armnn::DataType::Boolean));
        auto* mask = t.boolean({2, 3}, {1, 1, 0, 1, 1, 1});
        armnn::LogicalBinaryDescriptor descriptor;
        descriptor.m_Operation = armnn::LogicalBinaryOperation::LogicalAnd;
        auto* layer = t.network->AddLogicalBinaryLayer(descriptor);
        connect(comparison, layer); connect(mask, layer, 1);
        t.finish(layer, tensor({2, 3}, armnn::DataType::Boolean), {1, 0, 0, 1, 0, 0});
    } else if (name == "logical_and") {
        auto* a = t.boolean({2, 3}, {0, 0, 1, 1, 0, 1});
        auto* b = t.boolean({2, 3}, {0, 1, 0, 1, 0, 1});
        armnn::LogicalBinaryDescriptor descriptor;
        descriptor.m_Operation = armnn::LogicalBinaryOperation::LogicalAnd;
        auto* layer = t.network->AddLogicalBinaryLayer(descriptor);
        connect(a, layer); connect(b, layer, 1);
        t.finish(layer, tensor({2, 3}, armnn::DataType::Boolean), {0, 0, 0, 1, 0, 1});
    } else if (name == "cast_bool_float") {
        auto* input = t.boolean({2, 3}, {0, 0, 1, 1, 0, 1});
        auto* layer = t.network->AddCastLayer();
        connect(input, layer);
        t.finish(layer, tensor({2, 3}), {0, 0, 1, 1, 0, 1});
    } else if (name == "float_less_via_ceil" || name == "float_greater_equal_via_ceil") {
        auto* a = t.floating({2, 3}, {-2, -1, 0, 0.5f, 1, 2});
        auto* b = t.floating({2, 3}, {-1, -1, 1, 1, 0, 2});
        auto* mask = floatLess(t, a, b, tensor({2, 3}));
        if (name == "float_less_via_ceil") {
            t.finish(mask, tensor({2, 3}), {1, 0, 1, 1, 0, 0});
        } else {
            auto* ones = t.floating({2, 3}, {1, 1, 1, 1, 1, 1});
            auto* complement = t.network->AddElementwiseBinaryLayer(
                armnn::ElementwiseBinaryDescriptor(armnn::BinaryOperation::Sub));
            connect(ones, complement); connect(mask, complement, 1);
            t.finish(complement, tensor({2, 3}), {0, 1, 0, 0, 1, 1});
        }
    } else if (name == "float_less_boundaries") {
        auto* a = t.floating({2, 4}, {-1, std::nextafter(-1.0f, 0.0f), std::nextafter(-1.0f, -2.0f),
                                     0, std::nextafter(0.0f, 1.0f), std::nextafter(0.0f, -1.0f), 1, 2});
        auto* b = t.floating({2, 4}, {-1, -1, -1, 0, 0, 0, 1, 1});
        t.finish(floatLess(t, a, b, tensor({2, 4})), tensor({2, 4}), {0, 0, 1, 0, 0, 1, 0, 0});
    } else if (name == "spline_gate_float") {
        auto* input = t.floating({2, 3}, {-2, -1, 0, 0.5f, 1, 2});
        auto* lower = t.floating({2, 3}, {-1, -1, -1, -1, -1, -1});
        auto* upper = t.floating({2, 3}, {1, 1, 1, 1, 1, 1});
        auto* ones = t.floating({2, 3}, {1, 1, 1, 1, 1, 1});
        auto* below = floatLess(t, input, lower, tensor({2, 3}));
        auto* insideLower = t.network->AddElementwiseBinaryLayer(
            armnn::ElementwiseBinaryDescriptor(armnn::BinaryOperation::Sub));
        connect(ones, insideLower); connect(below, insideLower, 1);
        insideLower->GetOutputSlot(0).SetTensorInfo(tensor({2, 3}));
        auto* insideUpper = floatLess(t, input, upper, tensor({2, 3}));
        auto* product = t.network->AddElementwiseBinaryLayer(
            armnn::ElementwiseBinaryDescriptor(armnn::BinaryOperation::Mul));
        connect(insideLower, product); connect(insideUpper, product, 1);
        t.finish(product, tensor({2, 3}), {0, 1, 1, 1, 0, 0});
    } else if (name == "batch_matmul_2d" || name == "batch_matmul_3d") {
        const bool batched = name == "batch_matmul_3d";
        auto* a = batched ? t.floating({1, 2, 3}, {1, 2, 3, 4, 5, 6})
                          : t.floating({2, 3}, {1, 2, 3, 4, 5, 6});
        auto* b = batched ? t.floating({1, 3, 2}, {1, 2, 0, 1, -1, 3})
                          : t.floating({3, 2}, {1, 2, 0, 1, -1, 3});
        auto* layer = t.network->AddBatchMatMulLayer(armnn::BatchMatMulDescriptor());
        connect(a, layer); connect(b, layer, 1);
        t.finish(layer, batched ? tensor({1, 2, 2}) : tensor({2, 2}), {-2, 13, -2, 31});
    } else if (name == "einsum_via_fc") {
        auto* input = t.floating({2, 2, 2}, {1, 2, 3, 4, 5, 6, 7, 8});
        armnn::ReshapeDescriptor reshape;
        reshape.m_TargetShape = tensor({2, 4}).GetShape();
        auto* flat = t.network->AddReshapeLayer(reshape);
        connect(input, flat);
        flat->GetOutputSlot(0).SetTensorInfo(tensor({2, 4}));
        t.weights = {1, 0, 0, 1, 0, 1, 1, 0, 1, -1, 1, -1};
        auto weightInfo = tensor({3, 4});
        weightInfo.SetConstant(true);
        auto* weights = t.network->AddConstantLayer(armnn::ConstTensor(weightInfo, t.weights.data()));
        weights->GetOutputSlot(0).SetTensorInfo(weightInfo);
        armnn::FullyConnectedDescriptor descriptor;
        descriptor.m_TransposeWeightMatrix = true;
        auto* layer = t.network->AddFullyConnectedLayer(descriptor);
        connect(flat, layer); connect(weights, layer, 1);
        t.finish(layer, tensor({2, 3}), {5, 5, -2, 13, 13, -2});
    } else if (name == "reduce_l2_decomposed") {
        auto* input = t.floating({2, 3}, {3, 4, 0, 0, 5, 12});
        auto* square = t.network->AddElementwiseBinaryLayer(armnn::ElementwiseBinaryDescriptor(armnn::BinaryOperation::Mul));
        connect(input, square); connect(input, square, 1);
        square->GetOutputSlot(0).SetTensorInfo(tensor({2, 3}));
        armnn::ReduceDescriptor descriptor;
        descriptor.m_KeepDims = true;
        descriptor.m_vAxis = {1};
        descriptor.m_ReduceOperation = armnn::ReduceOperation::Sum;
        auto* reduce = t.network->AddReduceLayer(descriptor);
        connect(square, reduce);
        reduce->GetOutputSlot(0).SetTensorInfo(tensor({2, 1}));
        auto* layer = t.network->AddActivationLayer(armnn::ActivationDescriptor(armnn::ActivationFunction::Sqrt));
        connect(reduce, layer);
        t.finish(layer, tensor({2, 1}), {5, 13});
    } else throw std::runtime_error("Unknown test case: " + name);
    return t;
}
}  // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf << std::setprecision(10);
    std::string stage = "arguments";
    int failure = 2;
    try {
        if (argc < 2 || argc > 3) throw std::runtime_error("Usage: native_layers CASE [CpuRef|CpuAcc|CpuAcc,CpuRef]");
        const std::string name = argv[1];
        const std::string backend = argc == 3 ? argv[2] : "CpuRef";
        std::vector<armnn::BackendId> preferences;
        if (backend == "CpuAcc,CpuRef") preferences = {armnn::BackendId("CpuAcc"), armnn::BackendId("CpuRef")};
        else if (backend == "CpuRef" || backend == "CpuAcc") preferences.emplace_back(backend);
        else throw std::runtime_error("Only CPU backends are allowed in this capability probe");
        armnn::ConfigureLogging(true, false, armnn::LogSeverity::Info);
        std::cout << "case=" << name << " requested_backend=" << backend << " onnx_parser_used=false\n";
        stage = "construct"; failure = 20;
        auto test = build(name);
        std::cout << "stage=construct passed\n";
        stage = "runtime"; failure = 30;
        auto runtime = armnn::IRuntime::Create(armnn::IRuntime::CreationOptions());
        std::cout << "available_backends=";
        for (const auto& id : runtime->GetDeviceSpec().GetSupportedBackends()) std::cout << id.Get() << ',';
        std::cout << '\n';
        stage = "optimize"; failure = 40;
        std::vector<std::string> messages;
        armnn::OptimizerOptionsOpaque options;
        options.SetImportEnabled(false);
        options.SetExportEnabled(false);
        auto optimized = armnn::Optimize(*test.network, preferences, runtime->GetDeviceSpec(), options,
                                        armnn::Optional<std::vector<std::string>&>(messages));
        for (const auto& message : messages) std::cout << "optimizer_message=" << message << '\n';
        if (!optimized) throw std::runtime_error("Optimize returned a null network");
        if (const char* path = std::getenv("F3P_LAYER_GRAPH")) {
            std::ofstream graph(path);
            if (!graph || optimized->SerializeToDot(graph) != armnn::Status::Success)
                throw std::runtime_error("Cannot write optimized graph");
        }
        std::cout << "stage=optimize passed\n";
        stage = "load"; failure = 50;
        armnn::NetworkId id;
        std::string error;
        // Undefined selects ordinary copy-based CPU buffers and must match the
        // disabled import/export optimizer options above. Malloc means import.
        armnn::INetworkProperties properties(false, armnn::MemorySource::Undefined, armnn::MemorySource::Undefined);
        if (runtime->LoadNetwork(id, std::move(optimized), error, properties) != armnn::Status::Success)
            throw std::runtime_error("LoadNetwork failed: " + error);
        std::cout << "stage=load passed\n";
        armnn::InputTensors inputs;
        for (std::size_t i = 0; i < test.inputs.size(); ++i) {
            auto info = test.inputs[i].info;
            info.SetConstant(true);
            inputs.emplace_back(static_cast<armnn::LayerBindingId>(i), armnn::ConstTensor(info, test.inputs[i].pointer()));
        }
        const bool boolean = test.output.GetDataType() == armnn::DataType::Boolean;
        std::vector<float> floats(test.expected.size());
        std::vector<std::uint8_t> booleans(test.expected.size());
        void* destination = boolean ? static_cast<void*>(booleans.data()) : static_cast<void*>(floats.data());
        armnn::OutputTensors outputs{{0, armnn::Tensor(test.output, destination)}};
        stage = "inference"; failure = 60;
        if (runtime->EnqueueWorkload(id, inputs, outputs) != armnn::Status::Success)
            throw std::runtime_error("EnqueueWorkload failed");
        double maximum = 0;
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < test.expected.size(); ++i) {
            // Boolean tensors encode truth, not an integer magnitude. The
            // observed CPU backends use 1 and 255 for true. Retain the raw code.
            const double actual = boolean ? (booleans[i] != 0 ? 1.0 : 0.0) : floats[i];
            const double difference = std::abs(actual - test.expected[i]);
            if (!std::isfinite(actual) || difference > 1e-5 + 1e-5 * std::abs(test.expected[i])) ++mismatches;
            maximum = std::max(maximum, difference);
            std::cout << "element=" << i << " actual=" << actual << " expected=" << test.expected[i];
            if (boolean) std::cout << " raw_boolean=" << static_cast<unsigned>(booleans[i]);
            std::cout << '\n';
        }
        runtime->UnloadNetwork(id);
        std::cout << "result=" << (mismatches ? "mismatch" : "passed") << " case=" << name
                  << " max_absolute_error=" << maximum << " mismatches=" << mismatches << '\n';
        return mismatches ? 70 : 0;
    } catch (const std::exception& error) {
        std::cerr << "result=failed stage=" << stage << " error=" << error.what() << '\n';
        return failure;
    }
}
