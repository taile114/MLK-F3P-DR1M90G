#pragma once

#include <stddef.h>
#include <stdint.h>

/* 单主故障模型的独立共享库 ABI。与 RUL（rul_engine_api.h，2530 输入、
 * 单值输出）和告警 API v1 分属不同接口，二者不能共用结构或检查器。
 *
 * 输入为 base6_single_native4s_flat_v1：batch=1、float32、小端、C 连续，
 * 共 27683 个 float（110732 字节），调用前已完成训练侧固定标准化。 */
#define MLK_FAULT_ENGINE_ABI 1
#define MLK_FAULT_INPUT_FLOATS 27683
#define MLK_FAULT_INPUT_BYTES (MLK_FAULT_INPUT_FLOATS * 4)

enum { MLK_FAULT_CPU = 1, MLK_FAULT_CPU_CNN_NPU = 2 };
enum { MLK_FAULT_MASK_OVERTEMPERATURE = 1u << 3 };

/* 固定 32 字节结果，IEEE float32，字段按字节偏移排列；不含指针或 C++ 类型。
 * 调用方先填写 struct_size=32；结构大小不匹配即拒绝调用。错误返回时仍清
 * valid/class_index/grade/confidence。 */
typedef struct mlk_fault_result
{
    uint32_t struct_size;  /* +0  固定 32 */
    uint32_t valid;        /* +4  0 不可用、1 有效 */
    int32_t class_index;   /* +8  有效 0..4；无效 -1 */
    int32_t grade;         /* +12 正常 0，故障 1..3；无效 -1 */
    float confidence;      /* +16 有效 0..1；无效 -1 */
    uint32_t backend;      /* +20 1 CPU，2 CPU+CNN NPU */
    uint32_t infer_us;     /* +24 引擎调用耗时，不含预处理/等待 */
    uint32_t reserved;     /* +28 0 */
} mlk_fault_result;

typedef int (*mlk_fault_abi_fn)(void);
typedef void *(*mlk_fault_create_fn)(const char *root, int backend, char *error, size_t capacity);
typedef int (*mlk_fault_predict_fn)(void *context, const float *input, size_t count,
                                    mlk_fault_result *result, char *error, size_t capacity);
/* ABI 1 可选扩展 mlk_fault_predict_masked：屏蔽类别不参与 argmax/softmax，
 * 其余类别及所选分级仍来自同一次真实推断。mask=0 与原入口一致。
 * 原入口保持不变，冻结数据集评估不应用在线温度策略。 */
typedef int (*mlk_fault_predict_masked_fn)(void *context, const float *input, size_t count,
        uint32_t excluded_classes, mlk_fault_result *result, char *error, size_t capacity);
typedef void (*mlk_fault_destroy_fn)(void *context);

#ifdef __cplusplus
extern "C" {
#endif
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(mlk_fault_result) == 32, "mlk_fault_result must stay 32 bytes");
#endif
#ifdef __cplusplus
}
static_assert(sizeof(mlk_fault_result) == 32, "mlk_fault_result must stay 32 bytes");
static_assert(offsetof(mlk_fault_result, valid) == 4, "valid offset");
static_assert(offsetof(mlk_fault_result, class_index) == 8, "class_index offset");
static_assert(offsetof(mlk_fault_result, grade) == 12, "grade offset");
static_assert(offsetof(mlk_fault_result, confidence) == 16, "confidence offset");
static_assert(offsetof(mlk_fault_result, backend) == 20, "backend offset");
static_assert(offsetof(mlk_fault_result, infer_us) == 24, "infer_us offset");
static_assert(offsetof(mlk_fault_result, reserved) == 28, "reserved offset");
#endif
