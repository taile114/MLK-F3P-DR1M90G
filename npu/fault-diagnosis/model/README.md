# base6 多任务故障诊断模型

本目录实现六分支 PyTorch 联合网络、原生 NPZ 数据入口、因果预处理、训练、评估和 FP32 导出。当前应用为[单故障分类与分级](单故障训练与推断.md)：结构 `fault_mtl_f3p_cnn2d_single_v3`，任务 `bench_fault_single_v1`，输入配置 `base6`；正常加四类故障取唯一最高 logit，再输出所选故障的等级。多标签 v2 为对照配置。

当前单故障模型通过[独立 F3P 离线测试入口](../deploy/README.md)冻结 checkpoint、预处理与 test 特征，导出原生 FP32 和量化资产并交叉构建 AArch64 程序。[四卷积硬 NPU 板测](../deploy/BOARD_TEST.md)已完成 360 窗回放、逐层整数复核和 CMA 释放；类别及选中等级与 FP32 判定一致。该入口保留 `fault_mtl.export` 对短上下文 checkpoint 的限制，故障模型未接入 RUL 上位机。[F3P 移植与运行包规划](../F3P部署与量化设计.md)中的实时预处理、共享库和[实时上位机接入方案](../F3P实时故障诊断接入方案.md)为后续范围。本目录训练 Python、缓存、数据集及权重保留本地。

当前单故障缓存为 [active_selection_single_v1](cache/active_selection_single_v1/dataset_manifest.json)，train/valid/test 为 1063/360/360 窗，已完成 RTX 4060 上 25 轮开发训练。test 分类 360/360 正确，故障分级 290/294 正确；结果限于同批记录内留出。物理特征为四秒有效上下文、逐 DMA 块频谱及训练侧暖机参考，预处理标识 `base6_native_blocks_short_context_v2`。调用入口为 [predict.py](fault_mtl/predict.py)，当前权重为 [best.pt](runs/active_selection_single_v1_seed42/best.pt)。

第四类为安装倾斜异常 `mount_tilt`。网络从高通前原生 XYZ 生成姿态输入 `[B,8]`，经 8→16 编码后参与 224 维融合；不读取参考倾角作为输入。v1 的 `mount_looseness` 标签、五分支缓存及权重与本结构不兼容，加载时检查架构、任务、类别顺序、姿态通道、预处理版本、融合维度和标签契约 `fault_labels_v2_operator_v1`。

## 1. 网络与模块

单故障 v3 的分类头为 5 类、最终 Linear 128→9，参数量 257137，CNN/Linear MAC 为 12386304/259744。以下表格列出共用编码结构和连续波形多标签 v2 对照规格；单故障输出、标签及推断协议以[单故障说明](单故障训练与推断.md)为准。

| 项目 | 实现 |
|---|---|
| 参数总数 | 257008，含 720 个固定零补齐权重；非固定参数 256288 |
| Conv/Linear 权重 | 256288；卷积/全连接 MAC 12581536/窗，不含预处理 |
| 时频主干 | `[B,8,64,48]` → 四组 Conv3×3/BN/ReLU/MaxPool2×2 → `[B,64,4,3]` |
| 其他分支 | 长窗 PSD 2307→32、振动统计 12→16、姿态 8→16、快状态 240→32、慢状态 540→64 |
| 融合 | 时频 64、频谱 32、幅值 16、姿态 16、快状态 32、慢状态 64，按此顺序拼接 224→128 |
| 输出 | 4 个分类 logits、4 个严重度分数、每类两个严格有序阈值 |
| 标签 | `overload / undervoltage / overtemperature / mount_tilt`；多标签、每类 0～3 级 |
| 任务定义 | `bench_fault_sim_v2`；温度规则 `case_temp_35_40_45_v1`；倾斜数值候选 `mount_tilt_3_6_10_ref_v1` 为 REF，当前操作者确认档位单独使用 `operator_filename_grade_v1` |

前三个时频通道为 XYZ，后五个通道及最后一列为标准化实数零。首层对应补齐通道的权重和梯度固定为零。训练为一个联合模型；未来板端的 CNN/CPU 分区只改变执行位置。

| 文件 | 职责 |
|---|---|
| [model.py](fault_mtl/model.py) | 六分支网络、有序阈值、无效姿态解码、BN 折叠 |
| [mount_pose.py](fault_mtl/mount_pose.py) | 原生重力均值、相对基准倾角、源块覆盖及姿态有效性 |
| [preprocessing.py](fault_mtl/preprocessing.py) | 原生数据、因果滤波、STFT/Welch、幅值统计、状态对齐与标准化 |
| [prepare_dataset.py](fault_mtl/prepare_dataset.py) | session 分组、重复来源检查、质量审查、窗口索引与特征缓存 |
| [prepare_selection.py](fault_mtl/prepare_selection.py) | 当前选择清单适配、操作者确认的文件名等级、源块/哈希追溯和四秒候选索引 |
| [exploratory.py](fault_mtl/exploratory.py) | 原生块频谱、缺失感知短上下文、60/20/20 时间块划分和训练侧水平参考 |
| [prepare_single_class.py](fault_mtl/prepare_single_class.py) | 复用物理特征和划分，建立正常加四故障的唯一主类标签 |
| [predict.py](fault_mtl/predict.py) | 加载单故障权重，只输出一个类别及对应等级 |
| [dataset.py](fault_mtl/dataset.py) | 输入白名单、哈希/尺寸检查、混合 episode 批次 |
| [losses.py](fault_mtl/losses.py) | 单故障五类交叉熵与选中故障分级损失；多标签对照掩码 BCE |
| [train.py](fault_mtl/train.py) | AdamW、warmup/cosine、梯度裁剪、早停、验证选型 |
| [evaluate.py](fault_mtl/evaluate.py) | 冻结阈值、逐窗追溯、episode 等权指标、分组 bootstrap、拒绝计失败 |
| [export.py](fault_mtl/export.py) | TorchScript、固定形状 CNN ONNX、CPU 权重、参考输入输出与数值比对 |
| [validate.py](fault_mtl/validate.py) | 有界合成检查和首批真实温度子任务演练 |
| [adapt_legacy.py](fault_mtl/adapt_legacy.py) | 独立生成 v2 标签，显式区分水平确认负例与未知，不覆盖原始文件 |
| [base6.yaml](configs/base6.yaml) | 网络、预处理、质量和训练配置 |

## 2. 运行环境

已验证环境为 Python 3.12、PyTorch 2.11.0+cu128、CUDA 12.8，GPU 为 NVIDIA GeForce RTX 4060 Laptop 8GB，精确版本见 [requirements.lock.txt](requirements.lock.txt)。当前工作区解释器为 `E:\competion\code\.venv\Scripts\python.exe`。在本目录运行下列 PowerShell 命令：

```powershell
$python = 'E:\competion\code\.venv\Scripts\python.exe'
& $python -m pip install --index-url https://pypi.tuna.tsinghua.edu.cn/simple -r requirements-dev.txt
& $python -m pytest -q tests
& $python -m fault_mtl.validate --output runs/v2_validation_check --legacy-horizontal-confirmed
```

`requirements.txt` 指定清华源，`requirements-dev.txt` 增加 Pytest、ONNX 与 ONNX Runtime。复现本机 CUDA 环境使用 `pip install -r requirements.lock.txt`：其中 PyTorch 固定为带 SHA-256 的官方 CUDA wheel URL，其余依赖固定版本并使用清华源。CUDA 包来源、大小、哈希和兼容依赖记录在 [cuda_install.json](cuda_install.json)；官方轮包下载入口为 [PyTorch CUDA 12.8 仓库](https://download.pytorch.org/whl/cu128/torch/)。

GPU 训练使用 `--device cuda:0`；`--device auto` 在 CUDA 可用时自动选择 GPU，显式指定 CUDA 不可用时直接报错。模型、损失和批次均迁移到同一设备，CUDA 数据加载启用锁页内存和异步传输，保存权重时不搬动正在训练的模型。GPU 运行保持 FP32，未默认启用 AMP。

```powershell
& $python -m fault_mtl.check_gpu --output runs/v2_gpu_check --batch-size 32 --steps 5
& $python -m fault_mtl.train --dataset cache/base6_v2 `
  --device cuda:0 --batch-size 32 --num-workers 0 --output runs/base6_v2_gpu_seed_42
```

`check_gpu` 只用合成输入，检查 CPU/CUDA 输出、全部分支梯度、补齐权重、CPU checkpoint 重载及显存占用，不执行最终训练。GPU 检查结果写入 `gpu_verification.json`。本机检测到 RTX 4060 Laptop 8GB；批大小先用 32，增大批次需重新核对显存、BN 统计和验证指标。STFT、质量检查及特征缓存仍在 CPU 离线完成。

本机 v2 六分支 batch=32、5 步 FP32 检查已通过，PyTorch 峰值已分配张量显存约 79.58 MiB；该数不包含 CUDA 驱动、桌面和其他程序占用，也不是整个正式训练进程的显存上限。[verification.json](verification.json) 保存结构与 GPU 数值检查；[data_preparation_verification.json](data_preparation_verification.json) 保存连续波形配置的数据准备结果。

短上下文配置的数据划分、25 轮 GPU 开发训练、冻结 test、温度通道不变性和 88 项自动测试见 [exploratory_training_verification.json](exploratory_training_verification.json)。

当前单故障模型、唯一结果及选中等级校验、25 轮 GPU 训练和 96 项自动测试见 [single_class_verification.json](single_class_verification.json)。

`runs/`、`cache/` 和环境目录均被忽略。准备数据、训练、评估和导出要求新建或空的输出目录，避免覆盖冻结产物。

## 3. 首批数据验证范围

首批记录经操作者确认全程正常运行、水平放置。v2 派生清单的第四类为 `present=0,grade=0,class_mask=1,grade_mask=0`，依据为 `operator_confirmed_horizontal`；参考角度、测量误差和数值重力校准未被虚构。原 v1 JSONL、原生 NPZ 及哈希保留。记录只有 `session_001` 一个独立组；温度仍按项目模拟规则覆盖正常温度侧、一级、二级，没有三级或其他故障正例。[首批数据说明](../首批数据说明.md)和[缺口处理方案](../缺口处理方案.md)仍为质量依据。

独立 v2 清单为 [episodes.v2.jsonl](../data/segments/session_001_v2/episodes.v2.jsonl)，转换来源与原件未修改的哈希证据见 [adaptation.json](../data/segments/session_001_v2/adaptation.json)。

已运行的软件检查及真实数据演练结果见 [verification.json](verification.json)。本地 CPU 参考与首批数据输出为 `runs/v2_verification/validation_report.json`，GPU 输出为 `runs/v2_gpu/gpu_verification.json`：

- 完整网络使用三个固定种子，各进行 3 步合成输入反向传播，六分支均有有限非零梯度；补齐权重保持零。
- BN 折叠前后完整输出数值通过核对；CNN ONNX 固定为四组 Conv/Relu/MaxPool，经过 ONNX 检查与 CPU 数值比较。
- 首批 2191 个候选终点的完整六路合格数为 0；原始缺口、重复时间戳及审核缺项如实保留，没有生成补零、拼接或振动插值修复版。
- 从三个温区各取 12 个窗口，用真实温度 60 秒历史对现有慢分支、融合层和任务头做 20 步子任务演练；其他分支的嵌入显式置零，其他类别监督关闭。其损失仅说明训练流程可执行。

温度演练是同一 session 内的拟合检查，不构成独立验证或完整模型精度。输出命名为 `software_probe.pt`、`thermal_probe.pt`，均标记 `final_training=false`。本次没有进行最终训练，也没有生成可声称达到 90%/85% 的正式模型。

## 4. 原生数据与审核标签

数据入口采用现有工具生成的 NPZ，使用 `allow_pickle=False`；MLKC 解码继续使用上级 [prepare_capture.py](../tools/prepare_capture.py)，不在训练中处理口令。每路包含以下数组：

| 字段 | 规格 |
|---|---|
| `vibration_t_us` / `vibration_value` | 稳定排序 int64 UTC 微秒 / `[N,3]` 物理量 g |
| `vibration_block` | 与 XYZ 一一对应的真实源块 ID；姿态覆盖率结合块元数据的原生 `rate_hz` 计算 |
| `temperature_t_us` / `temperature_value` | 原生时间 / `[N]` ℃ |
| `current_t_us` / `current_value` | 原生时间 / `[N]` A |
| `speed_t_us` / `speed_value` | 原生时间 / `[N]` rpm |
| `*_raw` | 可选原生整数码，存在时附加饱和检查；不替换物理标定 |

同时间戳样本不去重；倒序时间拒绝。当前入口不将检查用 1 秒 CSV 作为高频振动来源。原生 CSV 需在保持原生频率、来源和逐通道时间戳的前提下转换为此 NPZ 契约。

`episodes.jsonl` 每行一个已合并的审核状态区间，沿用[设计方案 §9](../设计方案.md#9-区间标签与训练真值)。补充字段如下：

```json
{
  "episode_id": "run001-ep001",
  "session_id": "day01-mount01-cycle01",
  "group_id": "day01-mount01-cycle01",
  "source_data": "native_recording.npz",
  "start_us": "1793000000000000",
  "end_us": "1793000180000000",
  "input_profile": "base6",
  "schema": "mlk.fault_training.episode.v2",
  "task_definition": "bench_fault_sim_v2",
  "temperature_rule": "case_temp_35_40_45_v1",
  "tilt_rule": "mount_tilt_3_6_10_ref_v1",
  "tilt_rule_status": "REF",
  "source_block_metadata": "native_metadata.json",
  "gravity_reference_id": "level-cal01",
  "fault_order": ["overload", "undervoltage", "overtemperature", "mount_tilt"],
  "present": [0, 0, 1, 0],
  "grade": [0, 0, 1, 0],
  "class_mask": [1, 1, 1, 1],
  "grade_mask": [0, 0, 1, 0],
  "label_basis": ["operator_no_injection", "supply_protocol_proxy", "temperature_demo_threshold", "inclination_reference"],
  "theta_ref_deg": 1.0,
  "theta_ref_uncertainty_deg": 0.2,
  "quality_checks": {
    "vibration_dma": false,
    "current_calibrated": false,
    "timestamps_verified": false,
    "range_reviewed": false,
    "pose_mount_unchanged": false
  }
}
```

示例时间、角度、误差、校准 ID 和审核值需按真实记录填写；示例 1.0°、0.2°不代表本机测量。`source_data` 与 `source_block_metadata` 相对 JSONL 所在目录解析。五个 `quality_checks` 需对应来源、标定、时间轴、量程及传感器相对机身固定状态的审核依据；缺项视为未审核，软件缺口检查独立执行。

数值角度标签由独立参考角度和误差核验，跨 3°边界屏蔽分类，跨 6/10°屏蔽分级，重度还需记录 `theta_stop_deg`。操作者明确确认水平放置的负例可使用 `label_basis[3]=operator_confirmed_horizontal`、`operator_confirmed_horizontal=true`；不自动产生数值角度或校准参数。

当前选择的模拟故障目标使用操作者确认的文件名等级：`label_basis=operator_confirmed_filename_grade`、`operator_grade_rule=operator_filename_grade_v1`、`operator_filename_confirmed=true`。入口校验真实 `source_data` 文件名、目标类别、1/2/3 档位和监督掩码一致；没有显式确认时拒绝这种标签。倾斜的六段均启用分类和分级监督，数值参考角度保持为空。工艺档位不等价于 3/6/10°参考区间，两种倾斜分级依据不能混在同一数据集。数据、checkpoint、评估及导出清单保留 `label_policy`，防止将模拟档位成绩解释为物理角度分级。

共享热历史、装配、连续流及副本必须共用 `group_id`。程序拒绝同一 session、字节相同的原生文件或相同 `capture_ids` 被分配到不同组；不同编码格式的副本仍需依靠采集身份审核。重叠区间先合并/复核为四类向量，不自动取最大等级。

分类未知用 `present=null,class_mask=0`，等级未知用 `grade=null,grade_mask=0`。等级监督要求该类已确认存在。文件名、供电档位、参考仪表、`label_basis` 和实验 ID 不进入网络特征。

## 5. 数据准备与正式训练入口

当前单故障入口为 [prepare_single_class.py](fault_mtl/prepare_single_class.py)、[train.py](fault_mtl/train.py) 和 [predict.py](fault_mtl/predict.py)，完整命令见[单故障训练与推断](单故障训练与推断.md)。单故障使用五类交叉熵与 softmax argmax，无四类阈值校准和独立二值平滑；每窗只有一个主类和一个等级。以下保留物理特征、多标签及连续波形对照入口。

现有数据的快速训练、冻结 test 及温度覆盖使用[快速训练说明](快速训练说明.md)中的入口；[split_blocks.csv](cache/active_selection_exploratory_v2/split_blocks.csv) 记录三份时间块边界，[best.pt](runs/active_selection_exploratory_v2_seed42/best.pt) 为第 21 轮 valid 选出的开发权重。三个机械任务头在 `temperature_isolation=true` 时屏蔽温度值及其有效位/年龄，过热头单独读取温度；参数量不增加，Linear MAC 为 259488/窗。该分支使用已知标签掩码指标，不声称完成独立实验或正式验收。

当前 26 段的训练侧清单位于 [data/reviewed/active_selection_v2](../data/reviewed/active_selection_v2/preparation_manifest.json)，标签表为 [labels.csv](../data/reviewed/active_selection_v2/labels.csv)。暖机两段为四类正常负例，其余 24 段各按文件名启用目标类别和等级；未确认的非目标类别保持未知。26 个 NPZ、原选择索引和 230 个相关 MLKC 源文件的哈希均已核验且未修改。

四秒索引有 3939 个候选，其中 3266 个位于单一源文件。连续波形正式配置的结果为 [cache/active_selection_v2_operator/dataset_manifest.json](cache/active_selection_v2_operator/dataset_manifest.json)：2483 个候选、0 个合格。其姿态校准、质量门限及测量审核仍待补齐。快速配置独立拟合训练侧参考与标准化，生成 1783 个缓存并完成真实数据的开发训练；两种预处理和质量口径不混用。

正式配置的两个保守日期组保留为训练候选，[split_review.json](../data/reviewed/active_selection_v2/split_review.json) 声明尚无独立验证和测试集。快速配置的记录内时间块三分不宣称独立 session 测试；三份均覆盖全部源记录，窗口支持和 DMA 块在集合间不重叠。

先冻结组划分，再切窗和拟合标准化。组数少于 3 时默认全部归训练组，不生成伪独立验证集。正式数据推荐提供覆盖审查后的显式 `split-map.json`：

```json
{"session_A": "train", "session_B": "train", "session_C": "val", "session_D": "test"}
```

```powershell
# 首批数据：操作者已确认水平放置，生成独立 v2 清单
& $python -m fault_mtl.adapt_legacy `
  --episodes ../data/segments/session_001/episodes.jsonl `
  --output runs/session_001_v2_labels --confirmed-horizontal

# 对 v2 清单做质量审查
& $python -m fault_mtl.prepare_dataset `
  --episodes runs/session_001_v2_labels/episodes.v2.jsonl `
  --output runs/first_batch_v2_audit --audit-only

# 后续合格数据；路径替换为实际冻结清单
& $python -m fault_mtl.prepare_dataset `
  --episodes ../data/reviewed/episodes.jsonl `
  --split-map ../data/reviewed/split-map.json --output cache/base6_v2

# 正式训练命令；本次未在首批数据上执行
foreach ($seed in 42,43,44) {
  & $python -m fault_mtl.train --dataset cache/base6_v2 `
    --config configs/base6.yaml --seed $seed --output "runs/base6_v2_seed_$seed"
}
```

训练入口要求存在合格的训练和验证窗口、训练包含至少两个 episode，且组互不重叠。每批至少两个不同 episode，按组/episode 分层抽样，避免长记录主导。默认 AdamW、150 epoch 上限、5 epoch warmup、cosine、20 epoch 早停、梯度范数 1、分类加 0.5 倍有序损失。类别正权重仅从可信训练标签估计，截断上限 5；等级权重默认 1。

阈值只在验证集 0.05～0.95 网格按逐类 F1 选择，缺正/负例时保留 0.5 并记录原因。最佳候选先按验证 `A_set≥90%` 且 `A_grade_e2e≥85%` 筛选，再按 episode 等权的集合准确率、端到端分级与 macro-F1 均值选取；未达标时保留候选并如实报告。

产物包括 `best.pt`、`last.pt`、`config.yaml`、依赖锁、环境、训练日志、预处理、标签、组划分、数据清单、验证指标、逐窗预测与混淆矩阵。训练入口不执行测试集推理。消融、类别/等级覆盖、独立重复、供电/倾斜工艺和最终训练仍需按设计方案完成。

### 5.1 窗口数量与独立采集

窗口数量按连续、同审核状态的有效时长估算：`N=max(0,floor((D-60)/1)+1)`，D 为秒。下面是质量剔除前的参考数；不是准确率保证或通用最小样本量。

| 采集范围 | episode 数 | 每 episode 120～180 秒 | 约 60% 训练窗口 |
|---|---:|---:|---:|
| 先导：健康 + 四类×三级，各独立重复 3 次 | 39 | 2379～4719 窗 | 1400～2800 窗 |
| 正式参考：13 状态×3 工作点×10 次独立重复 | 390 | 23790～47190 窗 | 14000～28000 窗 |

120 秒对应 61 窗，180 秒对应 121 窗；热态同级驻留 3～5 分钟对应 121～241 窗。先导数据用于验证可分性、标签和实现，正式参考数量来自[设计方案 §7](../设计方案.md#7-试验组织与采样数量)。复合故障、暖机背景、干扰对照及发生/恢复事件需另外覆盖。

1 秒步长的相邻慢窗口重叠 59 秒，约 98%；窗口数不能当作独立试验数。应先保证多日期、多工作点和重复装配的独立 session，再按约 60/20/20 划分训练/验证/测试。完整 session 分组后比例可能与窗口数不完全一致。最终增采量根据独立验证/测试中的各类每级支持、学习曲线和误差决定，不能靠缩短步长增加相似窗口来补足。

首批 2191 个终点只是候选窗口；完整六路合格数仍为 0，且缺少其他故障正例与独立组，不能计入正式训练数量。

## 6. 预处理与质量约束

窗口为 `[t-60s,t)`，近期振动为 `[t-4s,t)`。完整 60 秒历史在同一审核状态区间内；发生/恢复事件的混合历史数据需单独完成事件标签实现和验证，当前主入口只生成稳态窗口。

振动为连续因果二阶 0.5 Hz 高通加椭圆抗混叠低通，通带 600 Hz、阻带 780 Hz、通带损失 0.5 dB、阻带抑制 60 dB；重置滤波状态后排除 10 秒。IIR 状态可能保留更早的输入，泄漏审查保守追溯到原生流起点，不能把 10 秒暂态排除长度当作有限滤波记忆。目标 1600 Hz，采用抗混叠之后的过去原生样本对齐，仅允许配置范围内的小时间抖动；不跨缺口插值。滤波群延迟随频率变化，DSP 参数、对齐误差及实板响应均为 REF，需要独立冻结，不能视作已通过硬件验收。

参考实现从源流首个初始化秒估计并固定滤波采样节拍，该区间位于已排除的暂态内；慢状态每格的节拍只依据该格过去样本估计。正在发生的缺口只按当前样本年龄判断，不利用未来样本的到达时间提前拒绝窗口。修改推理时刻及以后的值和时间戳时，既有时刻的特征保持不变。

STFT 为 512 点周期 Hann、128 步长、47 帧，单边密度 PSD 汇总到 64 个线性频带；固定频点单元与频带的交叠权重逐行归一并写入 `preprocess.json`。Welch 使用 2048/1024、去常数趋势、0～600 Hz 共 769 点。统计顺序为 `[X_RMS,X_p2p,X_crest,X_kurtosis,Y_...,Z_...]`，在物理 g 上计算，峭度为非超额形式，零方差置 0。

快状态为 `[I,n,valid_I,valid_n,age_I,age_n]×40`，电流经连续因果 4 Hz 低通，滤波重置后排除 2 秒。慢状态为 `[T,I,n,valid_T,valid_I,valid_n,age_T,age_I,age_n]×60`，I/n 使用过去一秒真实观察的均值，要求该秒覆盖率。对齐只读取网格时刻前的样本，终点及以后观察不可见。缺失物理值标准化后填 0，有效位置 0、归一化年龄置 1；有效位和年龄不参与均值/方差拟合。

保守质量条件包括 I/n 60 秒内不得出现 >0.5 秒缺口、温度新鲜度 6 秒、逐通道有效覆盖 ≥95%、近期振动不得出现 >20 ms 间隙/非正时间步，以及相邻原生步长偏差 ≤25%。这些是有版本的 REF 软件筛查，20 ms 和时间抖动门限不构成对较短 DMA 丢块的豁免，已知丢块仍须通过来源审核排除。

### 6.1 姿态输入与校准

`mount_pose` 顺序为 `[g_x,g_y,g_z,tilt_deg,g_norm,coverage,valid_pose,age_pose]`。校准采用 `(XYZ-offset_g)*gain`；前 3 项是同一 `[t-4s,t)` 原生样本的算术均值，倾角为 `atan2(norm(cross(g_ref,g_mean)),dot(g_ref,g_mean))` 转为度，模长为 `norm(g_mean)`。动态分支对同一校准后的 XYZ 高通，姿态分支保留重力，不使用高通数据、逐窗去均值或外部参考角度。

`preprocessing.mount_pose.gravity_references` 以 `gravity_reference_id` 为键保存 `reference_vector_g`、`offset_g`、`gain`、`calibration_id` 及水平/静态校准/传感器安装核验标记。[gravity_reference.example.json](configs/gravity_reference.example.json) 是未填写的字段模板，不会自动作为有效校准载入。参考向量为校准后的水平向量，不假设 Z 轴竖直；安装改变时重新核验，不在已倾斜启动时重新归零。

覆盖率按真实样本时间支持区间的并集计算，每点支持时长不超过其源块 `1/rate_hz`，限制在 4 秒窗口内；重复或重叠时间不增加覆盖率，来源块率低于输入 profile 时拒绝。年龄使用 `t` 前最后一个真实样本，按 `max_age_s` 归一化。统计稳定性检查窗口前后各 2 秒均值向量的差，不能据此排除全部持续外加加速度。

设计尚未冻结的 `min_coverage`、`max_age_s` 和 `max_half_mean_delta_g` 在默认配置中为 `null`。需按先导实采核定后填入，缺值时完整输入被拒绝；测试使用的数值仅属于合成检查条件。近零向量、缺少基准/来源块、缺口、重复时间、饱和、过期或统计不稳定均将姿态置为无效。前五项仅由训练集有效姿态拟合标准化，后三项保留质量语义；无效物理值标准化后填 0、有效位为 0、年龄为 1。模型解码会合并姿态有效位和外部质量标志，无效姿态不会报告健康。

## 7. 评估与导出

```powershell
& $python -m fault_mtl.evaluate --checkpoint runs/base6_v2_seed_42/best.pt `
  --dataset cache/base6_v2 --split test --bootstrap 2000 --output runs/test_v2_seed_42

& $python -m fault_mtl.export --checkpoint runs/base6_v2_seed_42/best.pt `
  --output runs/export_v2_seed_42
```

主指标按完整审核 episode 等权，窗口加权结果另列。分级分母只含真实存在且等级可信的故障，漏检计错；未知标签不作为健康。报告包括健康/单故障、复合故障、macro-F1、各类/各级支持与混淆、三级召回、3→1 错判、MAE、健康误报、平滑前后指标与数据可用率。不可用窗口在另一份端到端统计中计为失败，逐窗结果用 `valid=false` 和空预测表示，不报告为健康。

置信区间以最高声明独立层级 `group_id` 聚类 bootstrap；不足两个独立组时不给出伪精确区间。2 窗确认/3 窗恢复是 REF 稳定化，逐 episode、缺窗处重置；名义附加延迟分别为 1/2 秒，真实发生与恢复延迟仍需独立事件数据。缓存推理耗时不包含离线预处理，不能作为板端端到端时延。

导出包含 `cnn_fp32.onnx`、`cnn_fp32.ts`、`full_fp32.ts`、`partitions.pt`、`reference_io.npz`、训练预处理/标签/划分及带哈希的 `model.json`。完整 TorchScript 接收六个分支，输出 `[class_logits4,severity_scores4]`；有序阈值、姿态顺序、参考校准和预处理哈希随包冻结，调用端保留完整质量判定。CNN 固定 `B=1`、NCHW、输出 `[1,64,4,3]`；BN 折叠后只含 Conv/Relu/MaxPool。导出不执行量化，不宣称 NPU 已部署；PTQ/QAT、整数参考、Arm NN 解析和板端验收按 [F3P 部署设计](../F3P部署与量化设计.md)执行。

## 8. Python 前向接口

```python
import torch
from fault_mtl import FaultMTL, decode, INPUT_SHAPES

model = FaultMTL().eval()
inputs = {name: torch.zeros(1, *shape) for name, shape in INPUT_SHAPES.items()}
with torch.no_grad():
    output = model.forward_batch(inputs)
    prediction = decode(output, thresholds=[0.5] * 4, valid=torch.tensor([True]))
```

此例只说明接口；全零姿态的有效位为 0，结果为不可用。推理必须使用训练冻结的预处理、真实合格历史及阈值；未训练参数不能用于诊断。质量失败时 `valid=false`，解码的 `present/grade/overall_grade` 为 -1，由结果接口转换为空值。
