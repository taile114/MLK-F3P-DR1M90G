#include "rul_engine_api.h"
#include "engine_graph.hpp"
#include "ff6_plan.hpp"
#include <cstdio>
#include <limits>
#include <mutex>

namespace {
std::mutex engineMutex;
bool occupied = false;

struct Context {
    // Destruction is reverse declaration order: graphs, weights, then CMA session.
    CmaSession cma;
    Weights weights;
    std::vector<std::unique_ptr<fused::Graph>> stages;
    std::vector<float> activated = std::vector<float>(1680);
    explicit Context(const std::string &root) : weights(root) {
        for (unsigned i = 0; i < 6; ++i) stages.push_back(fused::buildStage(weights, i, true));
        if (fused::quantizedFcs.size() != (npuRequested ? 6u : 0u))
            throw std::runtime_error("Unexpected number of FF6 NPU graphs");
    }
    float run(const float *input) {
        const auto &initial = stages[0]->run({{input, MLK_RUL_INPUT_FLOATS}});
        const auto *pre = &initial[0]; const auto *residual = &initial[1];
        for (unsigned i = 1; i <= 4; ++i) {
            fused::activate(*pre, activated.data());
            const auto &next = i < 4
                ? stages[i]->run({{activated.data(), fused::size(*pre)}, {residual->data, fused::size(*residual)},
                                  {initial[i%2==0?2:3].data, fused::size(initial[i%2==0?2:3])}})
                : stages[i]->run({{activated.data(), fused::size(*pre)}, {residual->data, fused::size(*residual)}});
            pre = &next[0]; if (i < 4) residual = &next[1];
        }
        fused::activate(*pre, activated.data());
        return stages[5]->run({{activated.data(), fused::size(*pre)}})[0].data[0];
    }
};
void resetGlobals() {
    quantization.clear(); fused::quantizedFcs.clear();
    fused::QuantizedFc::allocatorRegistered = false;
    fused::hybridMode = false; npuRequested = false;
    fused::convLabels.clear(); fused::resultDirectory.clear(); fused::offloadPhase = "arguments";
}
void fail(char *error, size_t size, const char *message) {
    if (error && size) std::snprintf(error, size, "%s", message);
}
}

extern "C" int mlk_rul_abi() { return MLK_RUL_ENGINE_ABI; }
extern "C" uint64_t mlk_rul_cma_bytes() {
    std::lock_guard<std::mutex> guard(engineMutex);
    return cma_mem_is_init() ? cma_mem_count() : 0;
}
extern "C" void *mlk_rul_create(const char *root, int backend, char *error, size_t size) {
    std::lock_guard<std::mutex> guard(engineMutex);
    if (occupied) { fail(error, size, "Only one inference context is supported"); return nullptr; }
    try {
        resetGlobals();
        if (!root || (backend != MLK_RUL_CPU && backend != MLK_RUL_HYBRID))
            throw std::runtime_error("Invalid engine arguments");
        npuRequested = fused::hybridMode = backend == MLK_RUL_HYBRID;
        std::ifstream plan(std::string(root) + "/qparams_ff6.tsv");
        if (!plan) throw std::runtime_error("FF6 quantization plan missing");
        std::map<std::string, Quantization> selected;
        std::string line;
        while (std::getline(plan, line)) {
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            std::istringstream fields(line);
            std::string name, extra; Quantization q{};
            if (!(fields >> name >> q.inputs >> q.outputs >> q.sx >> q.sw >> q.sy) || (fields >> extra))
                throw std::runtime_error("Invalid FF6 plan row");
            const FrozenBranch *frozen = nullptr;
            for (const auto &branch : ff6Plan) if (name == branch.name) frozen = &branch;
            if (!frozen || q.inputs != frozen->q.inputs || q.outputs != frozen->q.outputs
                || q.sx != frozen->q.sx || q.sw != frozen->q.sw || q.sy != frozen->q.sy
                || !selected.emplace(name, q).second) throw std::runtime_error("FF6 plan differs from frozen calibration");
        }
        if (plan.bad() || selected.size() != 6) throw std::runtime_error("Incomplete FF6 quantization plan");
        if (npuRequested) quantization = std::move(selected);
        armnn::ConfigureLogging(true, false, armnn::LogSeverity::Error);
        auto context = std::make_unique<Context>(root);
        occupied = true;
        return context.release();
    } catch (const std::exception &e) { resetGlobals(); fail(error, size, e.what()); return nullptr; }
}
extern "C" int mlk_rul_predict(void *handle, const float *input, size_t count,
                                float *rul, uint32_t *us, char *error, size_t size) {
    std::lock_guard<std::mutex> guard(engineMutex);
    try {
        if (!handle || !occupied || !input || count != MLK_RUL_INPUT_FLOATS || !rul || !us)
            throw std::runtime_error("Invalid prediction arguments");
        for (size_t i = 0; i < count; ++i) if (!std::isfinite(input[i])) throw std::runtime_error("Nonfinite input");
        auto begin = std::chrono::steady_clock::now();
        *rul = static_cast<Context *>(handle)->run(input);
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
        *us = static_cast<uint32_t>(std::min<int64_t>(elapsed, std::numeric_limits<uint32_t>::max()));
        if (!std::isfinite(*rul)) throw std::runtime_error("Nonfinite prediction");
        return 0;
    } catch (const std::exception &e) { fail(error, size, e.what()); return -1; }
}
extern "C" void mlk_rul_destroy(void *handle) {
    std::lock_guard<std::mutex> guard(engineMutex);
    delete static_cast<Context *>(handle);
    occupied = false;
    resetGlobals();
}
