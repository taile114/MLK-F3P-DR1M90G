# F3P 模型解析与原生推理程序

本目录包含与厂商 Arm NN ABI 匹配的板端 C++ 实现和配套脚本。

| 文件 | 用途 | 当前验证 |
|---|---|---|
| [rul_probe.cpp](rul_probe.cpp) | ONNX 解析、CPU/混合后端诊断、优化图与性能事件 | 基础解析通过，完整模型受解析器限制 |
| [native_layers.cpp](native_layers.cpp) | 按布尔真值核对并保留原始字节 | 当前源码含 14 个正向用例及一个 Cast 对照，混合 QEMU 正向用例通过 |
| [native_rul.cpp](native_rul.cpp) | 完整 FKP-DVGT，六段网络与主机精确 GELU | CpuRef 基线已通过实机 165 窗；当前混合实现已通过 QEMU 和 F3P 165 窗 |
| [npu_smoke.cpp](npu_smoke.cpp) | 单一 CpuRef / Alnpu 的 8 位量化卷积，原生构图、CMA、DOT/profile 和数值对照 | 三种组合通过 QEMU CpuRef；u8 通过实机硬核 NPU，u8_s8 / s8 在优化阶段返回 40 |

## 生成与编译

从 `npu` 目录执行，使用已安装 PyTorch/ONNX 的 Python：

```powershell
python probe/prepare_native_rul.py
```

默认生成 `work/f3p-native-rul-v2/` 和 `.source.tar.gz` 源码包。输入来自仓库内 `training/outputs` 的检查点及 `artifacts/f3p-rul-probe-arm64.tar.gz` 的回放数据，不依赖工作区外的 `fkp-dvgt` 或 `out`。`--output` 指定新目录，已有目录或归档拒绝覆盖。只读权重导出及首窗 CPU 前向核对不进行训练。

在 Ubuntu 虚拟机中解压源码包，设置环境并编译：

```sh
export CXX=/home/uisrc/uisrc-lab-anlogic/tools/aarch64-linux/bin/aarch64-linux-gnu-g++
export ARMNN_ROOT=/home/uisrc/f3p-rul-probe/vendor/armnn
sh build.sh
```

包内 `build.sh` 来自 [build_native_rul.sh](build_native_rul.sh)，采用 C++17、`-O2`、`-ffp-contract=off`，只链接核心 Arm NN 和系统 C/C++ 库。基础层构建脚本为 [build_native_layers.sh](build_native_layers.sh)，ONNX 探针构建脚本为 [build.sh](build.sh)。路径和 SDK 准备见 [测试规程](../docs/testing.md)。

[prepare_bundle.py](prepare_bundle.py) 生成 ONNX 探针源码包，默认读取仓库内模型和处理数据；厂商归档可通过 `--vendor-archive` 指定。默认厂商路径为工作区资料目录，其 `package_runtime()` 保留 Linux 共享库符号链接。

## 执行与阶段

当前源码默认 CpuRef，第二参数可选择混合 CPU 后端；冻结的完整模型 v1 包仍固定 CpuRef：

```sh
sh run_native_rul.sh 1
sh run_native_rul.sh 0
sh run_native_rul.sh 1 CpuAcc,CpuRef
```

基础层可以分别请求 CPU 后端，不申请 NPU：

```sh
sh run_native_layers.sh CpuRef
sh run_native_layers.sh CpuAcc
sh run_native_layers.sh CpuAcc,CpuRef
```

脚本复用 `/root/f3p-rul-probe/lib`，可用 `F3P_PROBE_ROOT` 指定基础运行库包根目录。[check_board.sh](check_board.sh) 读取系统状态；[run.sh](run.sh) 提供 ONNX 探针的 `parse`、`cpu`、`mixed` 模式。当前完整 ONNX 无法解析，`mixed` 不是完整模型 NPU 验证入口。

完整模型比较条件为 `abs(pred-reference) <= 0.0001 + 0.0001*abs(reference)`。参考预测与标签只用于核对/统计，不进入网络。完整程序返回 0 表示数值通过，70 表示不匹配，2 表示异常并打印阶段。基础层及 ONNX 探针用 20/30/40/50/60 区分构图或解析、运行时、优化、加载、推理异常；ONNX 探针另有 21 表示契约错误。系统段错误退出码为 139。

`run_native_rul.sh` 执行完成后打印日志；运行期间无控制台输出不表示卡死。`forward_us` 包含六段网络和主机 GELU，首窗另含中间文件写入；不包含模型初始化，不是正式预热性能基准。

问题与处理见 [移植说明](../docs/porting.md)，实际证据见 [results](../results/README.md)。

当前源码为每段保存优化 DOT 和优化器消息；`F3P_PROFILE=1` 可记录首窗 profile。新打包版本与测试条件见 [CPU 加速说明](../docs/cpu-acceleration.md)。[check_npu.sh](check_npu.sh) 只读检查 NPU、设备树和持久存储，不加载模块。

NPU 最小卷积使用 [build_npu_smoke.sh](build_npu_smoke.sh) 编译、[run_npu_smoke.sh](run_npu_smoke.sh) 执行；包中分别命名为 `build.sh` 和 `run.sh`，源码位于 `src/npu_smoke.cpp`。`sh run.sh u8` 先运行 CpuRef，再强制单一 Alnpu；CPU 对照失败时不继续硬件测试。输入、权重和整数卷积期望值在程序中确定生成，不涉及训练数据或寿命模型。部署及证据条件见 [NPU SD 验证](../docs/npu-sd-validation.md)。

[npu_linear.cpp](npu_linear.cpp)、[build_npu_linear.sh](build_npu_linear.sh)和[run_npu_linear.sh](run_npu_linear.sh)用于模型实际矩阵尺寸的独立 UINT8 对照。每次输入变化，检查实际及补齐输出；CPU/Alnpu 分别独立运行，错误阶段与超时会保留证据并按规程停止。74 项 CPU 预验证通过；六种原生 FC 实机通过，但完整矩阵在 m06 硬核执行超时，当前停止重跑原包矩阵，见[线性尺寸验证](../docs/npu-linear-validation.md)。

[npu_linear_padded.cpp](npu_linear_padded.cpp)、[build_npu_fc_pad.sh](build_npu_fc_pad.sh)及[run_npu_fc_pad.sh](run_npu_fc_pad.sh)独立测试原生 FC 的实数零补齐。25 项 CPU 数值及裁剪等价性通过，全部 24 种模型尺寸的 Alnpu 补齐数值通过；包内自带超时工具，执行分为 control、first、attention、matrix，逐组审阅，见[补齐验证](../docs/npu-fc-padding-validation.md)。

配套镜像缺少 timeout applet。通过 [install_timeout_compat_v1.sh](install_timeout_compat_v1.sh)在测试目录安装独立工具，随后使用新增的 `run_compat.sh` 执行；原始包和校验保持不变。[timeout_compat.c](timeout_compat.c)为该工具源码，处理子进程退出码、进程组终止与 TERM/KILL 宽限期。

[npu_real_weight.cpp](npu_real_weight.cpp)通过包含冻结的补齐探针复用 CMA 与构图工具，使用六个真实权重夹具；构建/运行入口为 [build_npu_real_weight.sh](build_npu_real_weight.sh)、[run_npu_real_weight.sh](run_npu_real_weight.sh)。六项硬核数值通过。

[native_rul_quantized.cpp](native_rul_quantized.cpp)为独立诊断实现，保留已验证的原生 CPU 文件，按显式尺度计划插入补齐、Quantize、FC、Dequantize 和裁剪。[build_native_quantized.sh](build_native_quantized.sh)、[run_native_quantized.sh](run_native_quantized.sh)提供 FP32、CPU 量化及混合对照。首窗混合通过，持续混合出现 CMA 分配失败，限制见[诊断规程](../docs/rul-quantization-validation.md)。

[native_rul_cma.cpp](native_rul_cma.cpp)、[build_native_cma.sh](build_native_cma.sh)和[run_native_cma.sh](run_native_cma.sh)提供分段运行时实现。前四段使用普通 CPU 内存，两个 NPU 段复用一次进程全局 CMA 注册；逐段记录分配，184 窗回放首窗后稳定。权重、尺度和 GELU 不变，验收范围见[完整对照](../docs/rul-quantization-validation.md)。

[native_rul_fused.cpp](native_rul_fused.cpp)使用[融合 CPU 内核](fused_ops.hpp)和[持久量化 FC](fused_quantized_fc.hpp)。[参考程序](native_rul_reference_bench.cpp)保持原生图；[构建](build_native_fused.sh)、[逐组执行](run_native_fused.sh)和[交错回放](benchmark_native_fused.sh)用于同条件比较。分配统计只覆盖 C++ new/new[]，实验及限制见[融合验证](../docs/operator-fusion-validation.md)。

[run_offload_probe.sh](run_offload_probe.sh)执行独立生成的额外分支程序，包含 CPU 热点、真实层 FC/Conv 及混合候选入口；[benchmark_offload.sh](benchmark_offload.sh)按五种配置三轮交错回放。[npu_op_support.cpp](npu_op_support.cpp)及[查询脚本](query_npu_support.sh)仅优化 Float32 图，不加载或执行网络。生成后的完整源码位于独立部署包，生成入口为 [prepare_offload_probe.py](../tools/prepare_offload_probe.py)，当前范围见[额外分支验证](../docs/additional-offload-validation.md)。
