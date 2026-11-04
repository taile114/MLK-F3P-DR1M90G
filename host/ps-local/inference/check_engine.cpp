#include "rul_engine_api.h"
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>
#include <string>
#include <cstdlib>

// Standalone numerical/lifecycle check; reference and labels never enter predict().
int main(int argc, char **argv) {
    if (argc != 4) { std::fprintf(stderr, "Usage: check_engine ROOT cpu|hybrid RELOADS\n"); return 2; }
    const std::string root = argv[1], mode = argv[2];
    if (mode != "cpu" && mode != "hybrid") return 2;
    const int reloads = std::atoi(argv[3]); if (reloads < 1 || reloads > 100) return 2;
    auto lib = dlopen((root + "/libmlk_rul.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "%s\n", dlerror()); return 2; }
    auto abi = reinterpret_cast<mlk_rul_abi_fn>(dlsym(lib, "mlk_rul_abi"));
    auto create = reinterpret_cast<mlk_rul_create_fn>(dlsym(lib, "mlk_rul_create"));
    auto predict = reinterpret_cast<mlk_rul_predict_fn>(dlsym(lib, "mlk_rul_predict"));
    auto destroy = reinterpret_cast<mlk_rul_destroy_fn>(dlsym(lib, "mlk_rul_destroy"));
    auto memory = reinterpret_cast<uint64_t (*)(void)>(dlsym(lib, "mlk_rul_cma_bytes"));
    if (!abi || !create || !predict || !destroy || abi() != 1) return 2;
    auto read = [](const std::string &path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f || f.tellg() <= 0 || size_t(f.tellg()) % 4) return std::vector<float>{};
        std::vector<float> v(size_t(f.tellg()) / 4); f.seekg(0);
        f.read(reinterpret_cast<char *>(v.data()), v.size() * 4); return v;
    };
    const auto input = read(root + "/input_f32.bin"), reference = read(root + "/reference_f32.bin");
    if (reference.empty() || input.size() != reference.size() * MLK_RUL_INPUT_FLOATS) return 2;
    std::printf("reload,window,rul,reference,error,infer_us\n");
    char error[512] = {}; unsigned mismatches = 0;
    for (int r = 0; r < reloads; ++r) {
        const uint64_t before = memory ? memory() : 0;
        auto ctx = create(root.c_str(), mode == "cpu" ? 1 : 2, error, sizeof(error));
        if (!ctx) { std::fprintf(stderr, "%s\n", error); return 2; }
        float result; uint32_t us;
        // Same five warmup forwards as the frozen benchmark.
        for (int i = 0; i < 5; ++i) if (predict(ctx, input.data(), 2530, &result, &us, error, sizeof(error))) return 2;
        const uint64_t stable = memory ? memory() : 0;
        for (size_t n = 0; n < reference.size(); ++n) {
            if (predict(ctx, input.data() + n * 2530, 2530, &result, &us, error, sizeof(error))) {
                std::fprintf(stderr, "%s\n", error); destroy(ctx); return 2;
            }
            const auto delta = std::abs(result - reference[n]);
            if (memory && memory() != stable) { std::fprintf(stderr, "CMA changed during replay\n"); destroy(ctx); return 71; }
            if (delta > 1e-4 + 1e-4 * std::abs(reference[n])) ++mismatches;
            std::printf("%d,%zu,%.10g,%.10g,%.10g,%u\n", r, n, result, reference[n], delta, us);
        }
        destroy(ctx);
        const uint64_t after = memory ? memory() : 0;
        std::fprintf(stderr, "reload=%d cma_before=%llu cma_stable=%llu cma_after=%llu\n", r,
            static_cast<unsigned long long>(before), static_cast<unsigned long long>(stable), static_cast<unsigned long long>(after));
        if (after != before) return 71;
    }
    dlclose(lib);
    // Hybrid differences are reported; application acceptance threshold remains TBD.
    return mode == "cpu" && mismatches ? 70 : 0;
}
