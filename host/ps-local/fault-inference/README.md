# 单故障模型运行包与共享库

本目录生成上位机可加载的单主故障模型运行包，并实现 `mlk_fault_*` 共享库。
引擎 ABI 以 [fault_engine_api.h](../../common/fault_engine_api.h) 为唯一定义；运行包
校验见 [faultruntime.cpp](../src/faultruntime.cpp)；接入步骤与验收条件见
[故障模型与上位机接入](../../../docs/故障模型-上位机接入.md)；上位机两个模式与实测
结果见[故障模型接入与评估模式](../../故障模型接入与评估模式.md)。
源码、厂商 SDK、工具链和交付文件清单，以及从构建到板端检查的完整命令见
[构建与交付](BUILD.md)。
另一台 Windows 电脑的串口上传、安装、校验与回退见[更新包使用说明](delivery/README.md)。

## 目录

| 路径 | 内容 |
|---|---|
| `engine/fault_engine.cpp` | 共享库实现：CPU 后端单窗口前向、五类与所选等级解码、ABI 导出 |
| `check/fault_check.cpp` | 检查程序：通过 ABI 回放冻结 test 集，核对分类/分级与参考结果 |
| `check/test_tensor_binding.cpp` | 配套 Arm NN 输入绑定回归，覆盖四层生产输入元数据，不访问 NPU/CMA |
| `tools/build_runtime.py` | 运行包生成器：离线包 → `mlk.fault.runtime.v1` 目录与评估集 |
| `tools/verify_runtime.py` | 封闭资产与 SHA-256 校验，`--aarch64` 检查目标 ELF 架构 |
| `build_arm.sh` | AArch64 交叉编译 `libmlk_fault.so` 与 `fault_check` |
| `build_check.ps1` | 桌面 MinGW 构建，用于在 PC 上核对数值 |

## 运行包

```text
<运行包目录>/
  model.json         # mlk.fault.runtime.v1，含 files 逐项 SHA-256
  preprocess.json    # mlk.fault.preprocess.v1，FaultPreprocessor 的加载格式
  weights.bin        # BN 折叠后的 FP32 权重
  weights.tsv        # 权重名称 → 偏移/长度
  thresholds.bin     # 8 个有序阈值，四类故障各两级
  qparams.tsv        # 四层量化尺度，CPU 后端不读取，保留给 NPU 计划
  libmlk_fault.so    # 共享库
```

生成命令（离线包可由 `deploy/prepare.py` 生成，或直接解压
`deploy/artifacts/f3p-fault-test-v2.tar.gz`）：

```powershell
tar -xzf project\npu\fault-diagnosis\deploy\artifacts\f3p-fault-test-v2.tar.gz -C tmp
.\.venv\Scripts\python.exe project\host\ps-local\fault-inference\tools\build_runtime.py `
  --offline tmp\f3p-fault-test-v2 `
  --output  out\f3p-fault-single-v1 `
  --library out\f3p-fault-single-v1\libmlk_fault.so `
  --model-id f3p-fault-single-v1 `
  --eval-output out\f3p-fault-eval
```

`--library` 指向 `build_arm.sh` 的产物；尚未交叉编译时可加
`--allow-missing-library` 只生成清单与预处理资产。

`build_runtime.py` 只转换表示与生成清单，不重新训练，也不改变权重、阈值、
标准化参数或频带权重；生成后按 `FaultRuntime::verifyPackage` 与
`FaultPreprocessor::loadAssets` 的条件自检字段、长度、哈希与目录内容，缺失或
不符直接报错。

## 构建

AArch64（板端）。`ARMNN_ROOT` 指向与离线板测相同的 Arm NN 头文件与运行库；设置后
启用后端 2（CPU+CNN NPU），不设置时只编译 CPU 后端：

```sh
CXX=aarch64-linux-gnu-g++ ARMNN_ROOT=/path/to/armnn OUT=build-aarch64 sh build_arm.sh
```

设置 `ARMNN_ROOT` 时还生成 `test_tensor_binding`。在相同 Arm NN 运行库的 AArch64
环境运行该程序，或通过 QEMU 指定目标 sysroot 与库路径运行。程序复现非 constant
元数据被 `ConstTensor` 拒绝的异常，再验证四层实际使用的输入和输出绑定。
四层输入为每窗更新的 `InputLayer`；其只读 `ConstTensor` 绑定须设置
`TensorInfo::IsConstant=true`，可写输出保持 false。该检查不创建 NPU 图，不替代板端推理验收。

桌面核对（Windows，MinGW `g++` 在 PATH 中）：只构建 CPU 后端。

```powershell
powershell -File build_check.ps1
```

## 检查程序

```text
fault_check ROOT [--backend cpu|npu] [--data DIR] [--library FILE] [--limit N] [--report FILE] [--predictions FILE]
```

`ROOT` 为运行包目录，`--data` 默认 `ROOT/data`，需含 `test_inputs.bin` 与
`test_index.csv`。程序逐窗调用 `mlk_fault_predict`，与 `class_target` /
`grade_target` 比对统计准确率，并与离线准备阶段的 `reference_class` /
`reference_grade` 逐窗比对；出现任何不一致即以退出码 70 结束。`--backend npu`
在启用 Arm NN 的板端验证四卷积硬 NPU 计划，未启用时 `create` 返回
`cpu_cnn_npu backend is not available in this library`。

报告同时保存单窗耗时的平均、中位、P95 与峰值；`--predictions` 可导出逐窗类别、等级、
分数、实际后端及耗时 CSV。结果结构、后端或数值范围不符、报告写入失败均返回非零。

桌面实测（360 窗冻结 test 集，Windows x86-64、MinGW `-O2`、CPU 后端）：分类
360/360，分类与所选等级同时正确 356/360，与 FP32 参考逐窗类别和等级一致（0 处
差异），单窗平均 4.6～5.2 ms。AArch64 共享库已用 Linaro 7.5.0 与 Arm NN 32.1.0
构建，`-Wl,-z,defs` 通过；QEMU CPU 后端复现 360/360、356/360 与零参考差异。
动态依赖的 SONAME 为 `libarmnn.so.32`，由配套 `libarmnn.so.32.1` 及其链接提供。
输入绑定修复库已在板端完成两轮 NPU 360 窗：每轮分类 360/360、联合 356/360，
与 FP32 参考逐窗判定一致；平均 4.956～5.188 ms/窗。每轮硬 NPU IRQ 增加 1440，
软 NPU IRQ 不变，进程退出后系统 CmaFree 回到相同基线。证据与库哈希见
[输入绑定验收](../../tests/results/fault/tensor-binding.json)。QEMU 耗时不作为实板性能。

## 后端

| 后端 | 状态 |
|---|---|
| 1 CPU | 已实现并桌面回放通过 |
| 2 CPU+CNN NPU | 板端两轮冻结集回放通过：四个 3x3 卷积为仅允许 Alnpu 的独立图，CMA 持久输入输出，四层各自的 ReLU（UINT8 零点 128）与 MaxPool 在 CPU，末层池化后反量化；与离线 `hybrid_split` 一致 |

四个卷积的 Arm NN 计划、CMA 生命周期与逐层整数证据以
[npu/fault-diagnosis/deploy](../../../npu/fault-diagnosis/deploy/README.md) 的独立
程序板测为准；共享库封装已有创建/推断/释放与逐窗判定证据，原始 9 维输出、逐层
整数对照、实采和长期共存仍须独立验收。
