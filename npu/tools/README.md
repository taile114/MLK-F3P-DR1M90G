# 兼容性与交付工具

从 `npu` 目录执行。`validate_delivery.py` 和 `package_native_rul.py` 只使用 Python 标准库；解析对照生成器需要 NumPy、ONNX、ONNX Runtime。

| 工具 | 输入与操作 |
|---|---|
| [validate_delivery.py](validate_delivery.py) | 检查冻结归档、源码、文档链接与 170 个板端文件哈希，重算 CPU CSV 指标，复核 NPU 图、profile、整数输出及 IRQ |
| [prepare_parser_diagnostics.py](prepare_parser_diagnostics.py) | 生成最小前缀、类型编码及完整形状对照，并用 ORT 核对 |
| [prepare_parser_compat_v2.py](prepare_parser_compat_v2.py) | 生成 17 个形状/正负轴/独立算子/完整模型对照，两个完整副本逐窗核对 |
| [package_native_rul.py](package_native_rul.py) | 检查新源码包与完整回放 CSV，生成有 LF 校验清单的新部署归档 |

```powershell
python tools/validate_delivery.py
python tools/prepare_parser_diagnostics.py --output work/parser-diagnostics-new
python tools/prepare_parser_compat_v2.py --output work/parser-compat-new
```

新完整模型包在 Ubuntu 中编译和回放后，保留 `exit_code.txt`、`predictions.csv` 以及生成包的 `preparation.json`。将编译包和结果复制回电脑，从 `npu` 执行：

```powershell
python tools/package_native_rul.py --bundle work/f3p-native-rul-new --results work/f3p-native-rul-new/emulation_all --output work/f3p-native-rul-arm64-new.tar.gz
```

该命令检查 AArch64 ELF、209 条状态生成来源、165 行参考/标签、有限预测和参考容差，计算 RMSE/MAE，再生成 `replay_verification.json` 和 `checksums.sha256`。不会重新运行模型，也不自动将输入结果标成实机或 NPU 证据。已有归档或已打包目录拒绝覆盖。

冻结部署包在 [artifacts](../artifacts/README.md)，当前实机证据在 [results](../results/README.md)。新打包结果不替代原始实机已验证包。

[summarize_cpu_backends.py](summarize_cpu_backends.py) 接收完整模型结果目录，汇总六段 DOT 的后端分配和可选 profile 的实际工作负载事件：`python tools/summarize_cpu_backends.py <结果目录>`。节点计数不代表 FLOP 比例或加速比。

[prepare_npu_sd_boot.py](prepare_npu_sd_boot.py) 校验并准备厂商 NPU 演示启动文件、诊断包和卡内清单，默认输出 `work/f3p-npu-sd-boot-v1/card/`。只复制到普通工作目录，绝不访问磁盘设备、不格式化、不写卡；已有目录拒绝覆盖。物理启动和卡准备见 [NPU SD 验证](../docs/npu-sd-validation.md)。

[prepare_npu_linear.py](prepare_npu_linear.py) 从已冻结尺寸清单生成三组线性层源码对照，不使用权重或训练数据。配套编译及 CPU 回放完成后，[verify_npu_linear.py](verify_npu_linear.py) 以标准库独立复核原始包 74 项对照的整数期望值、原始输出布局及反量化；不执行模型。详见[尺寸验证规程](../docs/npu-linear-validation.md)。

[prepare_npu_fc_pad.py](prepare_npu_fc_pad.py) 生成独立补齐 FC 源码包，原始矩阵尺寸保留为参考，物理输入和输出补到 8 的倍数。包内包含已验证的超时工具，已有输出拒绝覆盖。同一 `verify_npu_linear.py` 根据 preparation 的用例数核对补齐包 25 项结果；原始包默认仍为 74 项。板端只逐组验证，见[补齐规程](../docs/npu-fc-padding-validation.md)。

[verify_board_linear.py](verify_board_linear.py) 只读核对 1356 个回收文件哈希，从合成公式独立重算所有成功进程的整数输出，复核原始布局、裁剪反量化、实际后端、三次工作负载事件及 IRQ；保留原矩阵 m06 计算失败，不将失败计为支持。工具不执行模型或训练。

[calibrate_rul_uint8.py](calibrate_rul_uint8.py)仅用训练集 543 窗收集 124 分支范围，在验证集做隔离及组合误差对照，不修改训练代码或权重。[prepare_real_weight_probe.py](prepare_real_weight_probe.py)导出六个真实权重层及三个验证夹具；[prepare_quantized_native.py](prepare_quantized_native.py)生成两个回归头样条分支的完整模型诊断源码包。默认读取已保存的校准快照，详情见[量化规程](../docs/rul-quantization-validation.md)。

[verify_real_weight_results.py](verify_real_weight_results.py)复核真实层整数输出、图及执行事件；[verify_quantized_native.py](verify_quantized_native.py)重算完整回放或 CMA 失败前缀的精度与诊断时间，保留未完成状态。[f3p_serial.ps1](f3p_serial.ps1)按请求 JSON 执行 COM4 命令和分块传输，失败即停止、最终释放串口；不会自动重试 NPU 或重新启动板卡。

[verify_cma_replay.py](verify_cma_replay.py)只读核对共享运行时诊断、注册拒绝和分段运行时的原始文件，重算三种模式的 184 窗指标、分配轨迹、NPU 图、profile 和中断，不执行训练或推理。

[verify_fused_replay.py](verify_fused_replay.py)核对原始文件哈希、冻结程序/权重/尺度、预测及三轮耗时，从实际量化输入独立重算 int64 参考，复核样条边界、图、profile、预热与回放 IRQ 和恒定 CMA，不执行模型。

[prepare_offload_probe.py](prepare_offload_probe.py)从冻结融合源码生成独立的带标记执行计划和通用 FC/Conv 分支探针，校准尺度只读；[analyze_offload_profile.py](analyze_offload_profile.py)重新汇总 375 个回调及精确 GELU 的计时；[verify_offload_replay.py](verify_offload_replay.py)从实际 UINT8 字节独立重算 int64 输出、核对冻结权重和布局，并复核额外分支的完整回放、CMA、NPU 执行及三轮耗时。方法与边界见[额外分支验证](../docs/additional-offload-validation.md)。
