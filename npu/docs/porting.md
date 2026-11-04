# FKP-DVGT 移植方法与兼容问题

本文说明已训练 PyTorch 模型如何在 F3P 上完成 FP32 预测，以及厂商解析器和运行时限制的处理方法。

## 部署路径

电脑读取原检查点，以固定 `[1,23,110]` 输入导出 ONNX，用 ONNX Runtime basic 优化后核对全部测试窗。厂商 ARM64 ONNX 解析器无法接受完整模型；可用实现通过 `armnn::INetwork` 原生层重建模型，使用相同权重和输入，五处 GELU 在 C++ 中计算，六段网络依次执行。

```text
best.pt ──只读导出──► weights.bin + 固定配置
                            │
数据集 ──特征/归一化──► [1,23,110]
                            │
               六段 Arm NN CpuRef 网络
                  + 五处 std::erf GELU
                            │
                         [1,1] RUL
```

保留两个时空块、三个注意力头、KAN 样条递推、PCC 邻接、位置编码、残差与 LayerNorm。未通过删减模型或重新训练取得兼容性，参考预测和标签不输入网络。

## 厂商依据与限制边界

本地资料为工作区 `ref/F3P/01_start/02_start_Linux/05_application_demo/05-5_NPU演示/`；`demo/sdk/face_detection.tar.gz` 包含运行库和示例。

| 归档内路径 | 核对内容 |
|---|---|
| `app/face_detection/libs/armnn_lib/` | Arm NN 32.1.0、ONNX Parser 24.6、protobuf 23 和头文件 |
| `app/face_detection/src/npu_flow/utils/executor.cpp` | ONNX 加载与接收 `armnn::INetworkPtr` 的入口 |
| `app/face_detection/src/npu_flow/yolo_demo_hdmi_camera/main.cpp` | 示例请求 `Alnpu,CpuAcc,CpuRef` |

配套运行库是 AArch64 Linux 动态库，不能在 Windows Python 直接加载。Linaro GCC 7.5-2019.12 在 Ubuntu 虚拟机中交叉编译，程序在板端 Linux 或 QEMU AArch64 中运行。

“ONNX 解析器接受模型”“原生层接口存在”“后端可优化/加载/计算”需分别验证。请求 CpuRef 不能补上解析器缺失算子；头文件接口也不能证明指定类型/形状可执行。静态字符串/符号检查只用于定位测试方向。

## 问题与处理

| 问题 / 现象 | 验证 | 当前处理 |
|---|---|---|
| 完整 ONNX 解析段错误，退出 139 | 最小前缀显示中间张量元数据影响错误表现，不能单独认定 Reshape 不支持 | shape inference 补齐元数据，再以独立模型定位错误 |
| Unsqueeze 的 `-1` 被拒绝 | 正轴对照通过，配套解析器拒绝负轴 | 按输出秩转正轴，共享轴常量另建专用初始化器，不改共享参数 |
| 正轴完整图停在 `/linear_x/Less` | Less、GreaterOrEqual、And、Einsum、Erf、ReduceL2 独立解析失败 | 完整模型采用原生构图入口 |
| 双动态输入 MatMul 解析失败 | 二维/三维要求一侧常量；常量 MatMul/Gemm 通过 | 原生 BatchMatMul 表达注意力及 PCC 动态矩阵乘法 |
| BOOL 转 Float32 | ONNX 拒绝 BOOL，原生 Cast 在 CpuRef 优化返回 40 | 有限输入使用浮点门控，避开 BOOL→Float32 |
| CPU 网络加载退出 50 | 优化器关闭 import/export 与 Malloc 内存源不匹配 | 普通复制路径关闭 import/export，输入输出均 `MemorySource::Undefined` |
| 原生 API 没有 Erf/GELU 枚举 | 配套头文件没有可用原生 Erf 入口 | 五处 GELU 将模型分成六段，主机 `std::erf` 计算原精确公式 |
| Windows 清单 CRLF 导致校验找不到文件 | 文件名末尾 `\r`，不是模型文件必然损坏 | 清单强制 LF；已有清单可 `tr -d '\r'` 修复，仓库声明 LF |
| 板端时钟 1970，解包提示未来时间 | 归档时间超前于板端时钟 | 诊断与原生归档 mtime=0，目录时间仅作唯一标识 |
| eMMC 系统 NPU/CMA 三个设备节点缺失 | 128 MiB CMA 已预留，但字符设备不存在；配套 SD 镜像的三个节点及 Alnpu 注册已通过实机检查 | CPU 基线不申请 CMA；NPU 硬件计算在 SD 环境中单独验证 |

补形状和正轴的两个完整 ONNX 副本在电脑端通过 165 窗，最大差约 `7.63e-06`，302 个原初始化器不变。该结果不代表厂商解析器或 NPU 已接受模型。

## 原生表达与验证

| 原操作 | 原生表达 | 验证 |
|---|---|---|
| `x < y` | `clamp(ceil(y-x),0,1)` | 相等端点、相邻可表示浮点数、次正规数等对照 |
| `x >= y` / 区间与 | 浮点补集及两个 0/1 掩码乘积 | 门控对照及完整测试集 |
| KAN `bik,oik->bo` | 展平样条基和原权重 FullyConnected | 已知数值及完整模型 |
| L2 范数 | 平方、ReduceSum、Sqrt，保留原 PCC epsilon | 已知数值及完整模型 |
| 两个动态矩阵乘 | BatchMatMul | 二维、三维及完整模型 |
| GELU | `0.5*x*(1+erf(x/sqrt(2)))`，主机 float32 | 首窗五个分段边界与 PyTorch 核对 |

浮点掩码适用于当前验证的有限输入 CPU 路径；不推断 NaN、量化或硬件后端具有相同行为。GELU 未采用 tanh 近似。编译使用 `-ffp-contract=off`，控制浮点融合的影响。相同输入的 KAN SiLU/样条基可共享，保留原权重与递推次序。首窗五处 GELU 输入最大差不超过 `3.814697265625e-06`。

## 当前结论

F3P CpuRef 实机完成 165 窗，参考不匹配 0，最大差 `7.629394531e-06`。证据为 [实机摘要](../results/board/f3p-native-rul-board-v1-result.json) 和 [QEMU 逐窗 CSV](../results/emulation/native-rul/emulation_all/predictions.csv)。

混合 `CpuAcc,CpuRef` 也已通过实机全部 165 窗，最大参考差约 `8.58e-06`，同版本 CPU 对照平均耗时约 `199.45 / 928.10` 毫秒。

NPU 的有效加速能力仍待定。运行时列出 `CpuRef,ALRef,CpuAcc`，不能凭 `ALRef` 名称推断硬件实现。当前完整模型仅验证 CPU 后端，未完成量化、NPU 硬件分配或 NPU 加速比验证。

CpuAcc 的布尔真值编码、Cast 加载约束、Ceil 回退与完整混合图验证见 [CPU 加速说明](cpu-acceleration.md)。当前源码允许 CPU 后端偏好选择，冻结 CpuRef v1 包保持原样。
