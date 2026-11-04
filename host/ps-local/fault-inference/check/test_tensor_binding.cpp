/* Exercise production input metadata against the paired Arm NN runtime.
 * No NPU graph, CMA allocator or physical device is created by this check. */
#include "../engine/fault_engine.cpp"
#include <iostream>

int main()
{
#if !defined(MLK_FAULT_HAVE_ARMNN)
    std::cerr << "paired Arm NN build required\n";
    return 2;
#else
    // Reproduce the SDK rejection before checking all four production bindings.
    bool rejected = false;
    uint8_t byte = 0;
    try {
        const armnn::ConstTensor invalid(quantTensor(1, 1, 1, 1.f), &byte);
        (void)invalid;
    } catch (const std::exception &error) {
        rejected = std::string(error.what()).find("non-constant TensorInfo") != std::string::npos;
    }
    if (!rejected) return 3;
    for (unsigned layer = 0; layer < 4; ++layer) {
        const unsigned in = kChannels[layer], out = kChannels[layer + 1];
        const unsigned height = kHeight >> layer, width = kWidth >> layer;
        const auto input = inputTensorInfo(in, height, width, 0.25f);
        const auto output = quantTensor(out, height, width, 0.5f);
        std::vector<uint8_t> inBytes(in * height * width), outBytes(out * height * width);
        armnn::InputTensors feeds;
        armnn::OutputTensors bindings;
        feeds.emplace_back(0, armnn::ConstTensor(input, inBytes.data()));
        bindings.emplace_back(0, armnn::Tensor(output, outBytes.data()));
        if (!input.IsConstant() || output.IsConstant()) return 4;
    }
    std::cout << "PASS: SDK rejection reproduced; four production input/output bindings accepted\n";
    return 0;
#endif
}
