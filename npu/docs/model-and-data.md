# 数据与模型基线

本文规定当前寿命模型的输入、标签、训练配置和部署版本对应关系。

## 数据划分与标签

数据位于 [run_to_failure_5motors_v1](../training/processed/run_to_failure_5motors_v1/README.md)。选择有运行至失效记录的 Motor2、3、5、6、7，使用 537 份稳态 CSV；原始 ZIP 和完整波形不重复入库，已处理特征、训练/验证/测试 NPZ、质量清单和回放输入均已保存。

| 集合 | 电机 | 有效信号片段 | 10 次观测组成的输入窗口 |
|---|---|---:|---:|
| 训练 | Motor2、Motor3、Motor5 | 570 | 543 |
| 验证 | Motor6 | 193 | 184 |
| 测试 | Motor7 | 174 | 165 |

按整台电机划分。每个 CSV 独立切成 10000 点片段，原始波形不跨 CSV；937 个有效片段生成窗口，137 个名义片段被质量规则隔离。窗口由同一电机最近十次有效观测组成，允许已记录的隔离缺口，不表示连续十秒。

标签定义为 `RUL = 最后观测老化试验批次编号 - 窗口末次观测批次编号`。原始 CSV 没有逐行寿命列或精确失效时间戳。末次记录置零是监督标签约定，单位为 `observed_aging_test_session`，不能直接换算成小时。

Motor2 有 28 个日期分组；Motor6 同一天的五份记录属于同一批；Motor7 最后文件日期的大间隔保持原样，不作为额外老化周期。隔离后仍保留原批次编号。完整规则见处理数据 README、`cycle_map.csv`、`segment_quality.csv` 和 `sample_index.csv`。

## 特征与输入排列

特征版本固定为 `motor_stable23_v1`。11 通道顺序为三相电流、三相电压、两个加速度、麦克风、转速、温度；每通道 23 个时域/频域特征。名称、公式和顺序以 [preprocessing.json](../training/processed/run_to_failure_5motors_v1/preprocessing.json) 为准。

频域采用去均值、单边 AC 功率谱和 Hz 频率，退化信号的未定义量采用稳定零值。该版本不同于恢复的全 FFT 特征，应与当前权重配套使用。

归一化只使用训练电机逐传感器、逐特征的最小/最大值，float64 计算后转 float32；验证、测试不重新拟合，也不裁剪到 `[0,1]`。训练近常量特征在全部集合置零。[normalization.npz](../training/processed/run_to_failure_5motors_v1/normalization.npz) 必须配套模型；已处理 NPZ 和回放二进制均已归一化。

| 阶段 | 形状 / 排列 |
|---|---|
| 保存的 NPZ `samples` | `[N,10,11,23]` |
| 展平时间与通道 | `[N,110,23]` |
| 转置后模型输入 | `[N,23,110]` |
| 板端单窗 | little-endian float32 `[1,23,110]`，10120 字节 |
| 单窗输出 | float32 `[1,1]` |

不能将 `[10,11,23]` 直接 reshape 为 `[23,110]` 替代中间转置。

## 检查点与预测方式

检查点为 [best.pt](../training/outputs/motor_stable23_v1_gpu/best.pt)，SHA-256：

```text
bb7cb39da623e95574e57a559f1df979e5ee5ddea75d71d079f818da108d16f2
```

| 项目 | 配置 |
|---|---|
| 模型 | 完整 FKP-DVGT，`variant=full` |
| 节点 / 历史长度 | 11 / 10 |
| 时空块 / 注意力头 | 2 / 3 |
| `d_model` / `d_ff` | `[21,21]` / `[140,140]` |
| 邻接混合系数 / dropout | 0.5 / 0.15，推理关闭 dropout |
| KAN | 三阶样条，grid_size=6，13 个网格点，9 个最终样条基 |
| 训练 | 150 epochs，batch=100，学习率 0.005，weight_decay=0.0001，seed=44，不打乱 |
| 最优检查点 | 验证选择 epoch 16 |
| 板端预测 | 确定性点预测，随机图采样概率 0 |

配置、训练记录与电脑测试结果在 `training/outputs/motor_stable23_v1_gpu/`、`motor_stable23_v1_gpu_test/`。板端没有移植 MC Dropout 多次采样、不确定性区间或训练。

## 模型产物

| 文件 | 用途 |
|---|---|
| `training/outputs/motor_stable23_v1_onnx_opset13/rul_fp32.onnx` | 原始导出，5125 节点 |
| `training/outputs/motor_stable23_v1_onnx/rul_fp32.onnx` | ORT basic 优化后标准 ONNX，1547 节点，opset 13 / IR 7 |
| `artifacts/f3p-native-rul-arm64-v1.tar.gz` 内 `weights.bin` / `weights.tsv` | 原生图已训练状态和位置编码，209 条记录 |
| 同包 `src/model_config.hpp` | 固定模型配置 |

优化 ONNX SHA-256 为 `b9eac7e3a7902931b638126a4e824b10472f4248c7f26e76ac1c0af1cde091b4`；原生权重 SHA-256 为 `0a61767a0b324fd032db6857424b0857dd9dff75979bee25384b928a75905f02`。

测试集只用于验证，不作为 NPU 量化校准集。末期高估等模型误差不会因数值移植一致而消失；当前结果不能外推为所有电机或全部有限输入均已验证。
