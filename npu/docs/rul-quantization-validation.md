# 真实权重量化校准与混合推理验证

本文规定冻结 FKP-DVGT 权重的训练后量化、验证集误差归因和 CPU/NPU 诊断边界。当前六个真实权重单层及两个回归头样条分支的完整模型混合推理通过；分段运行时实现完成 184 窗验证集回放，CMA 在首窗后保持稳定。应用精度验收、正式性能基准及全部分支 NPU 化尚未完成。

## 模型与数据

检查点为 [best.pt](../training/outputs/motor_stable23_v1_gpu/best.pt)，SHA-256 `bb7cb39da623e95574e57a559f1df979e5ee5ddea75d71d079f818da108d16f2`。权重、样条网格、归一化参数及五处精确 GELU 保持不变。

| 范围 | 内容 |
|---|---|
| 校准 | 训练集 543 窗，电机 2/3/5；只收集激活范围，不使用标签拟合 |
| 逐层与方案比较 | 验证集 184 窗，电机 6 |
| 真实层夹具 | 验证窗口索引 0、92、183 |
| 测试集 | 本轮不读取电机 7 的 165 窗；方案冻结后再独立评估 |
| 单位 | 剩余观测老化测试周期，非小时或精确失效时间 |

[校准参数](../results/calibration/rul-uint8-v1/calibration.json)保存数据文件及检查点哈希。实现使用 `eval()`、禁用梯度和图随机采样，不创建优化器、不执行反向传播，执行结束逐张量确认状态未改变。

## 量化定义

124 个实际执行的线性分支分别收集 SiLU 基函数输入、样条输入和各自线性输出的训练集绝对最大值。未使用的 `linear_t` 不计入。每分支按张量量化：

`scale = max(train_absmax / 127, 2^-30)`，保存为 float32；输入、权重、输出均为 QAsymmU8、zero-point=128。权重范围取冻结权重绝对最大值。取整为绝对值加 0.5 后向下取整，再恢复符号；裁剪到 UINT8 范围。CPU 模拟使用整数值 float64 矩阵乘法，再按输入 scale × 权重 scale / 输出 scale 重量化。

物理 I/O 通道补到 8 的倍数，新增输入、权重及偏置为实数零，恢复后裁回原始输出。C++ 原生量化使用 ArmNN Quantize/Dequantize 边界，可能存在与电脑模拟或 NPU 固定点计算不同的舍入；两者的偏差单独报告，不假定完全相等。

## 电脑验证结果

[完整报告](../results/calibration/rul-uint8-v1/verification.json)、[局部误差](../results/calibration/rul-uint8-v1/local-layer-errors.json)及[逐分支隔离验证](../results/calibration/rul-uint8-v1/isolated-validation.json)覆盖 124 个分支。隔离验证每次只量化一个分支，其余保留 FP32；再检查组合误差，避免将局部误差简单相加。

| 配置 | 分支数 | 验证 RMSE | 相对 FP32 的预测 RMSE | 相对 FP32 最大偏差 |
|---|---:|---:|---:|---:|
| FP32 | 0 | 2.46594035 | 0 | 0 |
| 全部分支 | 124 | 2.49143025 | 0.20739476 | 0.65286064 |
| 样条分支 | 62 | 2.45583412 | 0.09986001 | 0.39180088 |
| 隔离预测 RMSE ≤0.005 的候选 | 64 | 2.46501136 | 0.01449381 | 0.04300499 |
| 两个回归头样条分支 | 2 | 2.46570941 | 0.00460599 | 0.01163232 |

FP32 线性/样条重构与原 PyTorch 实现的验证预测逐位相同。候选阈值只用于工程筛选，不是应用验收门槛，也不以验证 RMSE 的偶然下降宣称量化提升了泛化精度。最终允许退化量为 TBD。

## 真实权重单层实机

[真实层探针](../probe/npu_real_weight.cpp)使用原生补齐 FC 和 Signed32 零偏置。六项分别覆盖时间 Q 的基函数/样条、输入投影的基函数/样条、两个回归头样条；每项使用三个不同验证窗口。

[原始文件复核](../results/board/real-weight-v1-verification.json)确认六项均分配为 ALHardNPU，最大整数差为 1 LSB，无不匹配；CpuRef 最大整数差为 0。96 个原始文件的哈希见[清单](../results/board/real-weight-v1-files-sha256.json)。该结论限于六个真实层夹具，不自动覆盖全部 124 个真实层的任意尺度。

部署包为 [f3p-real-weight-arm64-v1.tar.gz](../artifacts/f3p-real-weight-arm64-v1.tar.gz)，内部 75 项校验。包含数据、源码、程序、超时工具及六项 CPU 对照，不进行训练。

## 共享运行时诊断

[native_rul_quantized.cpp](../probe/native_rul_quantized.cpp)保留六段网络，仅量化 `output_layer.0.spline_weight`、`output_layer.2.spline_weight`。第 4、5 段各包含一个 ALHardNPU；其余计算保留 CPU，五个主机 `std::erf` GELU 边界不变。每段核对实际 NPU 计算层数量，避免回退 CPU 后误报 NPU 通过。

首窗 FP32 最大参考差约 `1.91e-06`；CPU 量化相对 FP32 偏差约 0.00843；混合约 0.01894。首窗诊断时间分别约 234.07、234.69、246.27 毫秒，未证明混合方案有性能收益。

184 窗的 FP32 与 CPU 量化回放均完成：RMSE 分别约 2.46593998、2.46570998，平均约 201.25、200.28 毫秒。混合回放完成 91/184 窗后出现 `cma_mem alloc failed`，返回 2；不能将其前缀 RMSE 与完整验证集直接比较。混合前缀相对 FP32 最大预测差约 0.03055，平均约 212.26 毫秒，未证明性能收益。[完整文件复核](../results/board/quantized-native-v1-verification.json)保留已完成和未完成状态。

共享运行时实现对所有 CPU 后端配置 CMA allocator，重复推理出现内存累积。已核对配套头文件：`CmaAllocator::free` 存在并调用 `cma_mem_search_buffer`/`cma_mem_free`，不能简单归因于缺少 free 实现。逐段跟踪进一步确认工作内存反复分配及未配对释放，见后述 CMA 对照。

内核失败记录的申请量为 2260992 字节，当前 fd 管理量为 587657216 字节。进程退出后的只读检查为 `MemAvailable=808108 kB`、`CmaFree=122992 kB`、`CmaTotal=131072 kB`，资源已大幅回收；该失败版本不继续完整回放。原始回收和内存证据见[串口记录](../results/board/quantized-native-v1-return-serial.json)。硬核 IRQ 首窗为 3、91 窗为 273；图层数为 2，硬件分块导致 IRQ 数不等于图层数。

原生输出字段 `mismatches` 沿用 FP32 容差超限计数。在量化运行中，该字段表示相对 FP32 的偏差，不是单层整数核对失败；量化允许误差仍为 TBD。退出 0 只表示该诊断前向完成。FP32 模式仍严格使用原参考容差，未放宽。

[混合诊断部署包](../artifacts/f3p-quantized-native-arm64-v1.tar.gz)内部 12 项校验，仅附三个验证夹具；权重依赖冻结原生 CPU 包中校验一致的 `weights.bin`、`weights.tsv`，以 `weights.sha256` 核对，不重新传入一份不同权重。当前不用于无人值守持续推理。

## 分段运行时与 CMA 稳定性

[native_rul_cma.cpp](../probe/native_rul_cma.cpp)保持相同图、冻结权重、两个量化分支、尺度和精确 GELU。六段网络各持有独立运行时，每个运行时只加载一个网络，避免每窗在同一运行时切换网络 ID 时反复分配工作内存。前四段在 CMA 注册前以普通 CPU 内存加载。厂商分配器按进程全局注册，第 4 段前只注册一次，第 5 段复用；重复注册会在构图阶段返回 `CpuAcc already has an allocator associated with it`。

实现通过派生 `TrackedCmaAllocator` 记录 allocate/free 调用、尚存块和字节数，并在每段前后保存 `cma_mem_count()` 与 `/proc/meminfo`。跟踪确认现有分配路径的增长及规避效果，不将问题泛化为所有 ArmNN 版本的缺陷。

| 对照 | 第 1 窗结束 CMA 字节 | 第 10 窗结束 CMA 字节 | 分配调用：1 窗 / 10 窗 | 结论 |
|---|---:|---:|---:|---|
| 共享运行时、全局 CPU CMA | 6569984 | 64667648 | 50 / 500 | 每增加一窗增长 6455296 字节，尚存块增长 12 个 |
| 独立段运行时、仅 NPU 段 CMA | 1204224 | 1204224 | 10 / 10 | 首窗后无新增分配 |

修复配置的 184 窗结束 CMA 仍为 1204224 字节；前四段没有调用自定义 CMA 分配器。1 窗、10 窗及 184 窗的预测前缀一致，短序列也与共享运行时诊断逐位一致。内核、运行库哈希和三个设备节点已通过恢复后的 SD 实测，见[启动与最小卷积串口](../results/board/restored-sd-v2-smoke-serial.json)。

| 184 窗配置 | 验证 RMSE | 验证 MAE | 相对 FP32 最大偏差 | 平均诊断耗时/窗 |
|---|---:|---:|---:|---:|
| FP32 CPU | 2.46593998 | 1.97711485 | 0.00000763 | 197.50 ms |
| 两分支 CPU 量化 | 2.46570998 | 1.97699601 | 0.01163208 | 197.35 ms |
| 两分支 CPU/NPU 混合 | 2.46795107 | 1.97947107 | 0.03054810 | 196.31 ms |

性能含内存跟踪，首窗另含 profile 与中间输出写入，没有独立预热或多轮统计，不能将约 1 ms 的差异解释为已证实的加速。两个样条 FC 的实际图、首窗 NPU 执行事件和硬核中断均在原始文件中复核；完整验证集混合回放硬核 IRQ 增量为 552，软核为 0。

网络卸载后、CMA 会话清理前仍有 4 个自定义分配块，`cma_mem_count()` 为 843776 字节；程序保持 `CmaSession` 在所有段运行时析构后执行 `cma_mem_deinit()`。该清理顺序是实现的一部分，接入应用时必须保留。当前验证单次加载后连续 184 窗稳定，跨多轮加载/卸载与长时间压力测试尚未验收。

[独立复核](../results/board/cma-replay-v1-verification.json)重新计算全部预测指标、分配记录、图、profile 和中断；[文件哈希](../results/board/cma-replay-v1-files-sha256.json)保护原始字节。[冻结部署包](../artifacts/f3p-cma-replay-arm64-v1.tar.gz)含全部 184 窗验证数据、源码、ARM64 程序、超时工具及 14 项内部校验；依赖原生 CPU 包中相同的 `weights.bin`、`weights.tsv`，必须通过 `weights.sha256`。

在 F3P 包的解压根目录执行；下列步骤直接读取 SD 数据分区，不使用 U 盘：

```sh
sha256sum -c checksums.sha256
sha256sum -c weights.sha256
sh run.sh hybrid 1
sh run.sh hybrid 10
sh run.sh hybrid 0
sh run.sh baseline 0
sh run.sh cpu_quantized 0
sync
```

每组结束核对退出码及 `memory.csv` 后继续；`0` 表示全部验证窗口。量化模式保留原 FP32 容差计数语义，应用阈值仍为 TBD。电脑只读复核入口为 `python tools/verify_cma_replay.py`，不执行模型推理。

## 串口与复现入口

[f3p_serial.ps1](../tools/f3p_serial.ps1)独占 COM4，115200、8N1、无流控。PuTTY 会话应关闭。上传按 192 字节分块、逐块应答并最终 SHA-256 校验；下载使用 64 KiB 有界块并核对长度，最终核对完整哈希；格式异常的只读块最多重读一次，保留异常原文。命令长度保守限制为 500 字符，较长步骤应先上传脚本。串口只控制运行中的板端 Linux，不替代 Ubuntu 交叉编译器。

请求 JSON 指定 `report_path` 和 `steps`；步骤支持 `command`、`upload`、`download`。命令非零即停止，超时后不自动重试硬件。大归档不通过单次无限长控制台输出下载，以免触及命令读取期限。`reset_owned_continuation` 只用于取消已确认由本工具遗留的续行，不应对未知前台任务使用。

从 `npu` 目录执行只读校准与导出：

```powershell
python tools/calibrate_rul_uint8.py --output work/rul-uint8-calibration-new
python tools/prepare_real_weight_probe.py --output work/f3p-real-weight-new
python tools/prepare_quantized_native.py
```

导出默认使用已保存的校准快照；已有输出拒绝覆盖。使用配套 Ubuntu 的 CXX、ARMNN_ROOT 编译。验证先 CPU，再单层 NPU，最后完整模型，禁止在单层失败后直接扩大范围。

## 接续工作

1. 明确寿命输出允许误差与预测周期；当前 RMSE 变化及最大预测偏差只作为验证证据，不替代应用验收。
2. 使用独立预热和多轮回放测量端到端收益。先验证跨多轮加载/卸载的会话清理，再做长时间压力测试。
3. 按电脑逐层误差及实际端到端收益选择额外 NPU 分支；新增真实层仍需单层、短序列和全验证集对照，不直接将 124 分支全部交给 NPU。
4. 冻结方案和允许退化量后，执行尚未用于选择的 165 窗测试集，并接入 PS 应用推理接口。当前不重新训练。

当前融合 CPU/混合实现的内存复用和三轮预热结果见[算子融合验证](operator-fusion-validation.md)；本页保留分段 ArmNN 与量化定义的对照范围。
