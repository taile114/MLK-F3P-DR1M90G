#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 当前模型为纯 RUL 回归：rul 是引擎实际产出，只在 valid bit0 置位时有效。
 * cls / conf / grade 为故障检测与故障分级的预留入口，当前恒为 0（界面显示 N/A）；
 * 下游不得把 0 当作模型输出，也不得据此判定故障。 */
typedef struct
{
    uint8_t cls;      /* 预留：故障检测结果，当前 0 = 不可用 */
    float conf;       /* 预留：故障检测置信度，当前 0 */
    uint8_t grade;    /* 预留：故障分级，当前 0 = 不可用 */
    float rul;
    uint32_t infer_us;
    uint8_t backend; /* 1 fused CPU, 2 CPU + FF6 hardware NPU branches */
    uint8_t valid;   /* bit0: RUL；分类/分级当前不由本模型提供 */
} npu_result_t;

int npu_init(const char *model);
int npu_init_backend(const char *model, int backend);
const char *npu_error(void);
int npu_infer(const float *in, int len, npu_result_t *out);
void npu_deinit(void);

#ifdef __cplusplus
}
#endif
