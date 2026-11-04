# 冻结部署包

本目录保存部署与诊断归档，保留包内原始清单与状态信息。当前实机结论以 [results/board](../results/README.md) 为准；包内可能仍记录打包时的“实机待验证”，不修改归档来更新测试结论。

## 文件与校验

| 归档 | 字节数 | SHA-256 |
|---|---:|---|
| [f3p-native-layer-probe-arm64-v2.tar.gz](f3p-native-layer-probe-arm64-v2.tar.gz) | 37136 | `205f6e4251e6eccf4f0560918d18858d0d3890d36d77a86e5cd136100609281a` |
| [f3p-native-rul-arm64-v1.tar.gz](f3p-native-rul-arm64-v1.tar.gz) | 2057636 | `5c940835bcbd428e9fb4509df26c34b5a2d40cdbf03ffb19667a14e3ba0d5e65` |
| [f3p-parser-compat-v2.tar.gz](f3p-parser-compat-v2.tar.gz) | 2611077 | `879aea9e283ef2da236dfbbea02e41c8680595a0efd3f695ba40a91c8959cfff` |
| [f3p-parser-diagnostics-v1.tar.gz](f3p-parser-diagnostics-v1.tar.gz) | 2605003 | `cc379fe69640fe0a329b2ca83a13021190f83774f60409dc701ae6c0f821426c` |
| [f3p-rul-probe-arm64.tar.gz](f3p-rul-probe-arm64.tar.gz) | 7203739 | `febd37b91d4a17aac6d8aca48faadf9ac9ddab106ee96146288a6a9e2ad9e5ba` |
| [f3p-cpu-accel-arm64-v1.tar.gz](f3p-cpu-accel-arm64-v1.tar.gz) | 2428852 | `b7bb155dba6672d72792ecf82d7893a509e15f4dccda61d515cdacd5a284fb66` |
| [f3p-npu-smoke-arm64-v1.tar.gz](f3p-npu-smoke-arm64-v1.tar.gz) | 61704 | `214e781c2eee4f19bf9d263d74c32ce045ad0c25fbf03a92576729a9b6d2a824` |
| [f3p-npu-linear-arm64-v1.tar.gz](f3p-npu-linear-arm64-v1.tar.gz) | 641031 | `a0d0238b6fa92817165ab314087c23ac2d891f9a79c9dfd84414a4e0018d2c70` |
| [f3p-npu-fc-pad-arm64-v1.tar.gz](f3p-npu-fc-pad-arm64-v1.tar.gz) | 266291 | `9434fb95d10d934738593677deebed5666d7c09d1d657eda5b8d1ef0fe292090` |
| [f3p-real-weight-arm64-v1.tar.gz](f3p-real-weight-arm64-v1.tar.gz) | 352766 | `33e307341cb0a99bfd50582da5d044c1b251b0bc16ecc5fac92ca7cbcc15286a` |
| [f3p-quantized-native-arm64-v1.tar.gz](f3p-quantized-native-arm64-v1.tar.gz) | 112058 | `406cd5ff90aa8cbfc0bd35b8c0497128a1d2c61e848c70c699677aa4029b681e` |
| [f3p-cma-diagnostic-arm64-v1.tar.gz](f3p-cma-diagnostic-arm64-v1.tar.gz) | 289819 | `fefabc7dc05ca1791b0a752f254440b2976fd560515f5b4d3fcd372878f8c3c5` |
| [f3p-cma-replay-arm64-v1.tar.gz](f3p-cma-replay-arm64-v1.tar.gz) | 291148 | `7263e3ad624216902ca8b0b59c886f6abefe81f100855d3af0b3aeb5267e801e` |
| [f3p-fused-rul-arm64-v1.tar.gz](f3p-fused-rul-arm64-v1.tar.gz) | 434607 | `282407c44cf97c6475d21f137096512032b51903cfeb14c655e5cbdc14f2a2db` |
| [f3p-offload-rul-arm64-v1.tar.gz](f3p-offload-rul-arm64-v1.tar.gz) | 393889 | `0bfcce79b8700d27ff08bd728b75bb2cbe027bee3ca8a468788aa6deea31c0cb` |

机器可读清单为 [manifest.json](manifest.json)，外层校验为 [checksums.sha256](checksums.sha256)。在 Linux 的本目录执行 `sha256sum -c checksums.sha256`；电脑可执行 `python ../tools/validate_delivery.py` 检查外层和包内全部文件清单。

## 用途与依赖

| 包 | 解压根目录 | 使用范围 |
|---|---|---|
| `f3p-rul-probe-arm64.tar.gz` | `f3p-rul-probe` | 基础运行库、ONNX 探针、模型、165 窗回放输入、头文件归档 |
| `f3p-parser-diagnostics-v1.tar.gz` | `f3p-parser-diagnostics-v1` | 解析段错误、最小前缀及中间形状的对照，需要基础包 |
| `f3p-parser-compat-v2.tar.gz` | `f3p-parser-compat-v2` | 正负轴与 17 项解析对照，需要基础包 |
| `f3p-native-layer-probe-arm64-v2.tar.gz` | `f3p-native-layer-probe-v2` | 13 个正向数值用例及一个已知失败 Cast 对照，复用基础包运行库 |
| `f3p-native-rul-arm64-v1.tar.gz` | `f3p-native-rul-v1` | 完整 FKP-DVGT CpuRef 回放，复用基础包运行库 |
| `f3p-cpu-accel-arm64-v1.tar.gz` | `f3p-cpu-accel-v1` | 完整模型混合 CPU 与基础层，复用基础包运行库 |
| `f3p-npu-smoke-arm64-v1.tar.gz` | `f3p-npu-smoke-v1` | 独立量化卷积，默认使用 SD 镜像 `/opt/armnn_lib/lib`；u8 实机硬核 NPU 通过，另两种组合在优化阶段受限 |
| `f3p-npu-linear-arm64-v1.tar.gz` | `f3p-npu-linear-v1` | 74 项 CPU 对照通过；六种原生 FC 实机通过，原始 matrix 在 m06 硬核执行超时停止，当前不重跑 |
| `f3p-npu-fc-pad-arm64-v1.tar.gz` | `f3p-npu-fc-pad-v1` | 原生 FC 实数零补齐；25 项 CPU 对照、24 种模型尺寸的 Alnpu 实机数值通过，内置超时工具 |
| `f3p-real-weight-arm64-v1.tar.gz` | `f3p-real-weight-v1` | 六个真实权重层、三个验证样本，75 项校验，CPU/NPU 数值通过 |
| `f3p-quantized-native-arm64-v1.tar.gz` | `f3p-quantized-native-v1` | 两个回归头样条分支混合诊断，12 项校验，依赖相同原生 CPU 权重；持续混合回放存在 CMA 分配失败 |
| `f3p-cma-diagnostic-arm64-v1.tar.gz` | `f3p-cma-diagnostic-v1` | 共享运行时分配增长诊断，11 项内部校验；已记录 1/10 窗，不用于完整回放 |
| `f3p-cma-replay-arm64-v1.tar.gz` | `f3p-cma-replay-v1` | 分段运行时和单次全局 CMA 注册，14 项校验；184 窗混合与首窗后内存稳定性通过，依赖相同原生 CPU 权重 |
| `f3p-fused-rul-arm64-v1.tar.gz` | `f3p-fused-rul-v1` | 分配复用及 CPU 融合，两分支 NPU 固定 IO；184 窗数值、独立整数审计及三轮预热回放通过，26 项校验，依赖相同原生 CPU 权重 |
| `f3p-offload-rul-arm64-v1.tar.gz` | `f3p-offload-v1` | 额外分支独立实验，29 项校验；真实层 FC/Conv、浮点支持查询和五模式三轮 184 窗对照，复用原生 CPU 权重；最大预测偏差需单独验收 |

只运行完整模型时需要基础包和完整模型包。基础包保留 `/root/f3p-rul-probe/lib`，它包含配套 Arm NN 32.1、ONNX Parser 24.6、protobuf 23；完整原生模型仅直接依赖核心 Arm NN 和系统 C/C++ 库。

基础包内 `vendor/armnn.tar.gz` 可在 Linux 解压得到交叉编译所需头文件和库，保留符号链接。厂商上游来源是 `ref/F3P/.../05-5_NPU演示/demo/sdk/face_detection.tar.gz`，原包 SHA-256 为 `33eb354f2cb41df81f42fe30d44898d99e2a4fcd8a69b8ae678c8622e473b82f`；具体成员路径见 [移植说明](../docs/porting.md)。配套文件保留其原有版权和 SPDX 标记。

这些包不包含原始波形数据集、Ubuntu 虚拟机镜像、完整 SDK、CUDA 环境或 QEMU 安装包。Windows 可通过 U 盘或 COM4 串口传输 tar.gz，在 F3P 的 Linux 文件系统中解压；步骤见 [测试规程](../docs/testing.md)与[串口规程](../docs/rul-quantization-validation.md)。

已编译的基础 ONNX 探针不支持完整模型，且不含当前源码的 CPU 内存策略调整；用途为已记录的解析诊断和运行库供应。完整模型使用独立原生包，不通过该解析入口。

新 [CPU 混合测试包](f3p-cpu-accel-arm64-v1.tar.gz) 解压为 `f3p-cpu-accel-v1`，复用基础运行库，包含两个更新后的程序及只读 NPU 检查脚本。完整 165 窗已通过 QEMU 和 F3P 混合 CPU 验证，详见 [使用说明](../docs/cpu-acceleration.md)。

NPU 最小卷积包内 27 项文件校验，包含源码、AArch64 程序和三种类型的 CPU 数值对照；Alnpu 不允许回退 CPU，u8 在实机上分配为 ALHardNPU 并通过数值与硬核中断对照。包内 verification 保留打包时的 QEMU 范围；当前实机结论见 [NPU SD 验证](../docs/npu-sd-validation.md)。

线性层尺寸包内 533 项校验，含原生源码、AArch64 程序和 74 个 CPU 对照的完整原始文件；独立核对的 126768 个输出误差均为 0。[使用说明](../docs/npu-linear-validation.md)记录尺寸相关支持及 m06 计算超时，当前停止重跑原始矩阵。

原生 FC 补齐包内 193 项校验，25 项 CPU 对照核对 44544 个输出；裁剪结果与未补齐 CpuRef 逐字节一致。内置独立 ARM64 超时工具，当前入口为[补齐验证规程](../docs/npu-fc-padding-validation.md)中的小控制组。

真实权重及完整混合包的校准范围、验证集索引、精度边界与 CMA 限制见[量化诊断](../docs/rul-quantization-validation.md)。完整混合包不重复包含 1933412 字节的 `weights.bin` 和 `weights.tsv`；从原生 CPU 包提供相同文件后，必须通过包内 `weights.sha256`。该包是可复现诊断，不是持续运行已验收版本。

分段运行时包复用相同冻结权重和量化尺度，修复持续回放的 CMA 增长。验证范围为两个回归头样条分支，应用精度阈值与正式性能基准尚未验收，详见[稳定性与精度对照](../docs/rul-quantization-validation.md)。

融合包保留相同训练检查点、数学定义及量化尺度；标量、NEON 与原生参考的范围、C++ 分配计数边界及性能结果见[融合验证](../docs/operator-fusion-validation.md)。

额外分支包从冻结融合源码生成独立执行计划，支持显式选择 FC/Conv 与量化计划。`ff4`、`ff6`、`input_ff5` 和原 `head2` 有完整实机回放；`input_ff7`、全部 124 分支计划仅为后续探索输入。CPU 仍承担样条基函数及其余算子，当前速度、量化偏差和复现入口见[额外分支验证](../docs/additional-offload-validation.md)。

当前选定 `ff6` 六分支部署目标，独立包须显式传入 `qparams_ff6.tsv`。上位机需要另行生成并编译共享库应用运行包，现有应用适配器仍为两分支；包来源、必需文件和适配流程见[运行包 README](../../host/ps-local/inference/README.md)。
