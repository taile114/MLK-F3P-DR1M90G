#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MLK_RUL_ENGINE_ABI 1
#define MLK_RUL_INPUT_FLOATS 2530
enum { MLK_RUL_CPU = 1, MLK_RUL_HYBRID = 2 };
typedef int (*mlk_rul_abi_fn)(void);
typedef void *(*mlk_rul_create_fn)(const char *, int, char *, size_t);
typedef int (*mlk_rul_predict_fn)(void *, const float *, size_t, float *, uint32_t *, char *, size_t);
typedef void (*mlk_rul_destroy_fn)(void *);
#ifdef __cplusplus
}
#endif
