# 电脑端数据、训练与导出

本目录为当前 F3P 寿命模型的可复现 Python 源码和冻结数据/模型基线。内容从工作区 `fkp-dvgt` 按字节复制，来源与哈希见 [source-snapshot.json](../results/source-snapshot.json)。原始波形 ZIP、解压 CSV 和 `.venv` 不入库；现有处理数据可直接用于评估、导出或后续训练。

## 目录

| 路径 | 内容 |
|---|---|
| `fkp_dvgt/`、`main.py` | 原模型、数据加载、训练、评估、不确定性和绘图入口 |
| `tools/prepare_motor_dataset.py` | 原始 CSV 的稳定 23 特征处理 |
| `tools/validate_motor_dataset.py` | 数据/模型训练适配验收，包含反向及短训练检查 |
| `tools/check_training_env.py` | CUDA 环境验收，包含参数更新和短训练检查 |
| `tools/export_onnx.py`、`tools/optimize_onnx.py` | 只读检查点导出、ORT 优化与逐窗前向核对 |
| `processed/run_to_failure_5motors_v1/` | 处理后的全部特征、NPZ、质量清单和回放输入 |
| `outputs/motor_stable23_v1_gpu/` | 已训练 `best.pt`、配置和训练记录 |
| `outputs/motor_stable23_v1_gpu_test/` | 电脑预测、指标与曲线 |
| `outputs/motor_stable23_v1_onnx_opset13/` | 原始 ONNX 导出和检查报告 |
| `outputs/motor_stable23_v1_onnx/` | 优化 ONNX、预处理参数、逐窗参考、检查报告 |

冻结输出目录不覆盖，新结果写到 `../work/`。执行环境见 [TRAINING_ENV.md](TRAINING_ENV.md)，模型/标签定义见 [数据与模型](../docs/model-and-data.md)。

## 仅评估现有权重

从本目录执行，输出目录需不存在：

```powershell
.\.venv\Scripts\python.exe main.py evaluate --checkpoint outputs/motor_stable23_v1_gpu/best.pt --data processed/run_to_failure_5motors_v1/test.npz --output ../work/evaluate-new --device cpu
```

没有 `--uq` 时采用确定性点预测，与当前板端基线一致。`validate_motor_dataset.py` 和 `check_training_env.py` 包含训练步骤，不是单纯的只读环境查看命令；只做推理/导出时无需运行它们。

## 导出与简化

在本目录执行：

```powershell
.\.venv\Scripts\python.exe tools/export_onnx.py --checkpoint outputs/motor_stable23_v1_gpu/best.pt --example-data processed/run_to_failure_5motors_v1/train.npz --data processed/run_to_failure_5motors_v1/test.npz --preprocessing processed/run_to_failure_5motors_v1/preprocessing.json --reference-predictions outputs/motor_stable23_v1_gpu_test/predictions.npz --output ../work/onnx-export-new --opset 13
.\.venv\Scripts\python.exe tools/optimize_onnx.py --export-dir ../work/onnx-export-new --data processed/run_to_failure_5motors_v1/test.npz --output ../work/onnx-optimized-new
```

导出固定 batch=1，优化仅使用 ORT basic 和标准 ONNX 算子，不需要 onnxsim/CMake。两步都拒绝覆盖输出，执行全部 165 窗前向核对，不训练。完整解析失败与原生替代方法见 [移植说明](../docs/porting.md)。

## 训练复现

以下命令只用于需要重新训练时，训练输出必须与冻结基线分开：

```powershell
.\.venv\Scripts\python.exe main.py train --train processed/run_to_failure_5motors_v1/train.npz --val processed/run_to_failure_5motors_v1/val.npz --output ../work/train-new --device cuda --epochs 150 --batch-size 100 --learning-rate 0.005
```

新权重即使模型结构相同，也需要重新进行导出、完整数值比较和板端验证，不能沿用已有部署包的验收结论。
