// Test fixture only. Never included by application packaging scripts.
#include "rul_engine_api.h"
#include <cstdio>
#include <new>
extern "C" int mlk_rul_abi() { return 1; }
extern "C" void *mlk_rul_create(const char *, int backend, char *, size_t) { return new int(backend); }
extern "C" int mlk_rul_predict(void *handle, const float *input, size_t count, float *rul,
                                uint32_t *us, char *error, size_t size) {
    if (!handle || count != 2530) return -1;
    if (input[0] == -999) { std::snprintf(error, size, "fixture failure"); return -1; }
    *rul = input[0] + 0.125f; *us = 44000; return 0;
}
extern "C" void mlk_rul_destroy(void *handle) { delete static_cast<int *>(handle); }
