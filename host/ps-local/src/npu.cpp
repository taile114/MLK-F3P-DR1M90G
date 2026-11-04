#include "npu.h"
#include "rul_engine_api.h"
#include <QLibrary>
#include <QString>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

namespace {
std::mutex mutex;
std::unique_ptr<QLibrary> library;
void *context = nullptr;
mlk_rul_predict_fn predict = nullptr;
mlk_rul_destroy_fn destroy = nullptr;
char error[512] = {};
int activeBackend = 0;
void release()
{
    if (context && destroy) destroy(context);
    context = nullptr;
    predict = nullptr;
    destroy = nullptr;
    library.reset();
    activeBackend = 0;
}
}

int npu_init(const char *model)
{
    return npu_init_backend(model, MLK_RUL_CPU);
}

int npu_init_backend(const char *model, int backend)
{
    std::lock_guard<std::mutex> guard(mutex);
    release();
    error[0] = 0;
    if (!model || !*model || (backend != MLK_RUL_CPU && backend != MLK_RUL_HYBRID)) {
        std::strcpy(error, "Invalid model directory/backend");
        return -1;
    }
    library.reset(new QLibrary(QString::fromUtf8(model) + QStringLiteral("/libmlk_rul.so")));
    auto abi = reinterpret_cast<mlk_rul_abi_fn>(library->resolve("mlk_rul_abi"));
    auto create = reinterpret_cast<mlk_rul_create_fn>(library->resolve("mlk_rul_create"));
    predict = reinterpret_cast<mlk_rul_predict_fn>(library->resolve("mlk_rul_predict"));
    destroy = reinterpret_cast<mlk_rul_destroy_fn>(library->resolve("mlk_rul_destroy"));
    if (!abi || !create || !predict || !destroy || abi() != MLK_RUL_ENGINE_ABI) {
        const auto detail = library->errorString().toUtf8();
        std::snprintf(error, sizeof(error), "Engine ABI/load failure: %s", detail.constData());
        release();
        return -1;
    }
    context = create(model, backend, error, sizeof(error));
    if (!context) { release(); return -1; }
    activeBackend = backend;
    return 0;
}

int npu_infer(const float *in, int len, npu_result_t *out)
{
    std::lock_guard<std::mutex> guard(mutex);
    if (out) std::memset(out, 0, sizeof(*out));
    if (!context || !in || !out || len != MLK_RUL_INPUT_FLOATS) {
        std::strcpy(error, "Engine not initialized or wrong input extent");
        return -1;
    }
    for (int i = 0; i < len; ++i) if (!std::isfinite(in[i])) {
        std::strcpy(error, "Nonfinite model input");
        return -1;
    }
    if (predict(context, in, size_t(len), &out->rul, &out->infer_us, error, sizeof(error)) != 0
        || !std::isfinite(out->rul)) return -1;
    out->backend = uint8_t(activeBackend);
    out->valid = 1;
    error[0] = 0;
    return 0;
}

const char *npu_error(void) { return error; } // Read by the owning worker thread.

void npu_deinit(void)
{
    std::lock_guard<std::mutex> guard(mutex);
    release();
}
