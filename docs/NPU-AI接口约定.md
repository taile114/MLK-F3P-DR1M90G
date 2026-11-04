# NPU AI 接口约定（MLK-F3P-DR1M90G）

本文规定寿命与单主故障模型的 PS 推理封装、输入 / 输出、后端和错误语义。采集寄存器见 [PL-PS 接口](PL-PS接口约定.md)，寿命模型部署、状态机与 UDP v3 逐字段定义见 [寿命模型接入](寿命模型-上位机接入.md)。

## 1. 范围与职责

| 项目 | 定义 |
|---|---|
| 应用 profile | `motor_stable23_v1` / RUL 回放 |
| 引擎 C ABI | v1，`host/common/rul_engine_api.h` |
| PS 封装 | `host/ps-local/src/npu.h`、`npu.cpp` |
| 推理线程 | `host/ps-local/src/replaycontroller.*` |
| 执行实现 | 冻结融合代码与原已训练权重，不通过厂商 ONNX 解析器加载完整网络 |
| 选定部署目标 | `ff6`，四个前馈首层样条 FC + 两个原回归头样条 FC；应用封装支持六分支，独立诊断程序硬件验证与应用共享库上板验收分别记录 |
| 应用验收 | CPU 数值一致性、混合误差、双端显示与资源稳定性分别验收 |

模型与预处理的权威来源为 [模型与数据说明](../npu/docs/model-and-data.md)。模型侧维护权重、张量排列、量化参数和数值基线；PS 侧维护输入校验、执行调度、结果状态、显示与传输。未来分类模型或实采模型使用独立 profile，不能复用本 profile 的单位和有效标志。

## 2. 输入

| 字段 | 定义 |
|---|---|
| 数据类型 | little-endian float32 |
| 形状 | `[1,23,110]`，2530 个元素 / 10120 字节 |
| 来源排列 | `[10,11,23]` → `[110,23]` → 转置为 `[23,110]`；不得用直接 reshape 代替转置 |
| 特征 | 11 传感器通道，每通道 23 个特征，10 次有效观测 |
| 归一化 | 使用训练集冻结参数；现有回放数据已归一化，不重复处理 |
| 有限性 | 输入全部为有限值，否则失败 |
| 标签 / 参考预测 | 不进入输入 |
| 当前来源 | 板端已部署的 `val184` / `test165` 回放文件 |

温度 / 电流 / 振动 / 转速四个设定值不满足当前模型输入。现场 PL 的通道、采样率、窗口、FFT 定标及特征公式与训练预处理尚未匹配；实采和 v2 四值注入不接入本模型。

## 3. PS API

```c
int npu_init(const char *model_directory);
int npu_init_backend(const char *model_directory, int backend);
int npu_infer(const float *input, int length, npu_result_t *output);
const char *npu_error(void);
void npu_deinit(void);
```

| 项目 | 语义 |
|---|---|
| `npu_init` | 显式使用融合 FP32 CPU；参数为运行包目录 |
| `backend=1` | 融合 CPU / NEON |
| `backend=2` | CPU + 四个前馈首层样条 FC 和两个回归头样条 FC 的硬核 NPU 执行 |
| 初始化 | 加载目录中的 `libmlk_rul.so`，核对 ABI，创建单个常驻上下文 |
| `npu_infer` | 同步调用，`length=2530`；由独立工作线程调用 |
| 返回码 | 0 成功；-1 失败，读取同一工作线程中的 `npu_error()` |
| 释放 | 等待当前调用完成后卸载图、释放缓冲、关闭 CMA；不可并行析构 |
| 后端失败 | 不静默回退 CPU；错误状态和显式后端选择分别处理 |

封装内串行化上下文访问。正常路径复用权重、执行计划与工作区，不写参考对照、中间张量或 profile 文件。预热与输入文件读取在前向计时之外。

`backend=2` 使用 FF6 六分支，保留后端枚举和 ABI 字段布局；厂商后端初始化要求 `/dev/hard_npu`、`/dev/soft_npu` 与 `/dev/cma_mem`。合并工程 `soc_hdmi` 实例化厂商 `Video_NPU` 以提供软核控制窗口，接线见 [PL-PS 接口约定](PL-PS接口约定.md) §6.1；合并镜像已完成两轮 184 窗混合回放、CMA 释放及 HDMI / 采集 DMA 基础共存核对，见 [实板结果](../fpga/uisrc/06_doc/验证结果.md) §5.6。引擎校验六行冻结名称、维数和尺度；回放和暂存检查执行计划身份及文件哈希。运行包契约见[应用运行包 README](../host/ps-local/inference/README.md)。

## 4. 输出

`npu_result_t` 保留原 `cls/conf/grade/rul` 成员，并增加 `infer_us`、`backend`、`valid`。所有消费者必须依据有效标志解释数据。

| 字段 | 当前 profile 的含义 |
|---|---|
| `valid & 1` | RUL 有效 |
| `rul` | 有限 float32，剩余观测老化试验轮次 |
| `infer_us` | 完整单窗前向耗时，包含混合方案的转换与同步 |
| `backend` | 实际执行后端 1 / 2 |
| `cls/conf/grade` | 不可用；清零不表示正常或 0% 置信度 |

当前模型只有确定性 RUL 点预测，没有分类头、故障分级或置信区间。RUL 不换算小时，不据此生成未经定义的分类和工单。UDP 使用 v3 float32 结果，不能塞入 v2 的整数 RUL 字段。

### 4.1 故障检测 / 故障分级（预留）

`cls`、`conf`、`grade` 是故障检测与故障分级的预留字段，当前 profile 不产出这两项能力。PS 封装在每次推理前清零整个结果结构，成功后只写 `rul`、`infer_us`、`backend` 与 `valid`，三个预留字段保持 0。

| 字段 | 类型 | 当前取值 | 界面显示 | 预留定义 |
|---|---|---|---|---|
| `cls` | `uint8_t` | 0 | 本地看板 `故障检测：N/A`；上位机「不可用」 | 故障检测类别；0 表示不可用，不表示「正常」 |
| `conf` | `float32` | 0.0 | 不显示 | 故障检测置信度；0 不表示「0% 置信度」 |
| `grade` | `uint8_t` | 0 | 本地看板 `故障分级：N/A`；上位机「不可用」 | 故障分级；0 表示不可用 |
| `valid` | `uint8_t` | 1 | — | 只有 bit0（`kValidRul`）有效；分类 / 分级位恒 0 |

消费规则：

- 只依据 `valid` 位判断字段是否可用，不得用零值推断；`cls` / `grade` 为 0 时不得据此判定故障、分级或生成工单。
- v3 推理帧只承载 RUL：`valid` 只允许 `kValidRul`，解析后 `hasClassification()` 为假，`faultText()` / `gradeText()` 返回 `N/A`（`host/common/diagnosisui.h`）；v1 / v2 帧的 `cls` / `grade` 仍按原语义显示。

后续模型接入时的 ABI 兼容规则：

1. 只追加：新能力只追加 `npu_result_t` 末尾字段，不改现有成员的顺序、类型与语义；PS 封装、引擎共享库与消费者同版本重建。
2. 能力位：新增能力占用 `valid` 的新位，bit0 始终表示 RUL；未置位的字段不得解释为输出。
3. 独立 profile：分类 / 分级使用独立 profile 与运行包，不复用本 profile 的输入形状、单位与验收基线，也不把分类结果写入 RUL 回放包。
4. ABI 版本：`host/common/rul_engine_api.h` 的 `MLK_RUL_ENGINE_ABI` 当前为 1。只追加预留字段、不改变引擎预测函数签名时该宏保持 1；改动函数签名、输出参数或既有字段语义属破坏性变更，须提升该宏，并同步 `libmlk_rul.so`、`npu.cpp` 与本文的引擎 C ABI 版本。
5. 文档同步：能力接入时先补齐本文的字段表、有效位与界面显示规则，再实现推理与显示。

## 单主故障模型（独立 ABI 与五类结果）

单主故障分类分级模型使用独立 profile 与运行包，不复用 RUL 的输入、单位、ABI 或输出结构。

| 项目 | 定义 |
|---|---|
| 架构 / 任务 / 契约 | `fault_mtl_f3p_cnn2d_single_v3` / `bench_fault_single_v1` / `single_primary_fault_v1` |
| 预处理 | `base6_native_blocks_short_context_v2`，四秒历史、两秒步长 |
| 引擎 C ABI | `MLK_FAULT_ENGINE_ABI=1`，`host/common/fault_engine_api.h`，动态库 `libmlk_fault.so` |
| 输入 | `[1,27683]`，float32 小端，110732 字节，调用前已按训练参数标准化 |
| 结果 | 固定 32 字节，唯一类别 0..4 与所选等级；不可用时不返回“正常” |
| 运行包 | `model.json`、`libmlk_fault.so`、`weights.bin`、`weights.tsv`、`preprocess.json`、`thresholds.bin`、`qparams.tsv` |
| 运行方式 | PS 工作线程单任务在途；实时诊断与评估默认 CPU+CNN NPU，允许显式选择 CPU；与寿命回放互斥 |

四个 C 导出符号以 [`fault_engine_api.h`](../host/common/fault_engine_api.h) 的函数指针类型为准：

| 符号 | 签名 / 返回值 |
|---|---|
| `mlk_fault_abi` | `int(void)`；固定返回 1 |
| `mlk_fault_create` | `void *(const char *root, int backend, char *error, size_t capacity)`；成功为常驻上下文，失败为 null |
| `mlk_fault_predict` | `int(void *context, const float *input, size_t count, mlk_fault_result *result, char *error, size_t capacity)`；成功 0，失败 -1；count=27683，调用前 result.struct_size=32 |
| `mlk_fault_predict_masked` | ABI 1 可选兼容扩展；在 count 后增加 `uint32_t excluded_classes`，其他参数及 32 字节结果不变；bit i 排除类别 i，禁止未知位及排除全部五类；mask=0 与原入口一致 |
| `mlk_fault_destroy` | `void(void *context)`；null 可接受；同一上下文调用结束后由所属线程释放 |

错误文本在 capacity>0 时以 NUL 结束；错误结果清除有效位，类别、等级与分数为 -1。

内置实时诊断要求库提供 masked 扩展。窗口内最近有效温度 `T≤35℃` 时传 `1u<<3`，
过热类不参与 argmax 和 softmax，其余类别及分级仍来自同次模型推断；`T>35℃` 时传 0。
有效温度只取窗口结束前的实际观测，缺失或过期时按质量规则输出不可用。
屏蔽不是把类别 3 改为类别 0；分数为允许类别上的 softmax。冻结数据集评估调用原入口，
不应用在线温度屏蔽。该策略不修改权重、量化尺度、训练标准化或分级阈值。
后端 1 执行 FP32 CPU；后端 2 对应离线 `hybrid_split`，执行计划
`four_hard_npu_conv3x3_cpu_relu_pool`：四个 Conv3×3 分别为只允许 Alnpu 的独立图，
每层后在 CPU 执行 UINT8 零点 128 的 ReLU 与 MaxPool，末层池化后反量化为 768 项，
其余分支与融合头在 CPU。创建时校验图计算节点归属，拒绝 CPU 回退；需配套
Arm NN 32.1.0、硬/软 NPU 与 CMA 设备。未编入 Arm NN 时创建后端 2 明确失败。

| 分支 | 形状 | float 偏移 | 数量 |
|---|---|---:|---:|
| `time_frequency` | `[8,64,48]` | 0 | 24576 |
| `spectrum` | `[3,769]` | 24576 | 2307 |
| `vibration_stats` | `[12]` | 26883 | 12 |
| `mount_pose` | `[8]` | 26895 | 8 |
| `fast_state` | `[6,40]` | 26903 | 240 |
| `slow_state` | `[9,60]` | 27143 | 540 |

| 字节偏移 | 字段 | 类型 | 含义 |
|---:|---|---|---|
| 0 | `struct_size` | uint32 | 固定 32，不匹配即拒绝调用 |
| 4 | `valid` | uint32 | 0 不可用、1 有效 |
| 8 | `class_index` | int32 | 0 正常、1 过载、2 欠压、3 过热、4 电机倾斜；无效 -1 |
| 12 | `grade` | int32 | 正常 0，故障 1..3；无效 -1 |
| 16 | `confidence` | float32 | 有效 0..1；无效 -1，JSON 序列化为 null |
| 20 | `backend` | uint32 | 1 CPU、2 CPU+CNN NPU |
| 24 | `infer_us` | uint32 | 引擎调用耗时，不含预处理与等待 |
| 28 | `reserved` | uint32 | 0 |

上位机加载时校验任务、架构、预处理版本与资产哈希、输入布局、类别顺序及 ABI；不符即报错，不静默回退到 RUL profile。五类结果的对外发布使用模型诊断 API 2（[模型接口需求](../host/故障诊断模型接口需求.md) §4），本地实时任务与外部发布者不能同时持有该接口。

## 5. 运行环境与交付

1. 模型目录提供 `model.json`、`weights.bin`、`weights.tsv`、`qparams_ff6.tsv`、`input_f32.bin` 与 `libmlk_rul.so`；初始化前逐项核对 SHA-256 和输入契约。
2. 板端使用配套厂商 ArmNN 库。混合方案另需硬核 NPU / CMA 设备及匹配的内核、设备树；不能由库注册后端名称推断硬件实际执行。
3. HDMI、PL 采集和 NPU 已完成配套镜像的基础共存验证；软核数据通路压力、软核中断触发及长期稳定性仍须单独验收。参考 [接入规程](寿命模型-上位机接入.md)。
4. CPU 使用已冻结 FP32 容差；混合方案允许精度退化、正式预测周期、长期稳定性和实采泛化验收均为 TBD。

## 分类模型的声光与工单接口

独立分类模型可通过 ps-local 的本地 Unix Socket API v1 上报有效性、类别、分级及 UTC 微秒故障时间。接口、C/C++ 客户端、Python 示例、心跳及故障解除规则见[声光告警与模型故障 API](声光告警与模型故障API.md)。模型上报每秒一次，缺失超过 10 秒变为不可用；不直接写 PL。该接口独立于 `npu_result_t` 和 `MLK_RUL_ENGINE_ABI=1`，不改变 RUL profile 的字段或共享库签名。
