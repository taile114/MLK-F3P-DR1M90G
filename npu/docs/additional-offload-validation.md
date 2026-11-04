# 额外 NPU 分支与整模型对照

冻结 FKP-DVGT 的额外分支已完成独立层检查、1/10 窗整数对照及五种配置的三轮完整验证回放。五分支方案平均约 41.85 ms/窗，相对同轮融合 CPU 的约 44.52 ms/窗，耗时减少约 6.0%。四、六分支方案的收益较小，轮间范围重叠。额外 NPU 分配可用，但仍存在精度与边界开销的取舍。

选定部署计划为 `ff6`（FF + 原头部），对应 43.358 ms/窗、最大 FP32 预测差约 0.048573 轮。六分支独立模型与应用共享库均有板端验证证据，清单和显示标识使用 FF6；本地 HDMI 已验证有效 RUL 与窗口更新，见[NPU README](../README.md)与[应用运行包说明](../../host/ps-local/inference/README.md)。

六个分支及其余 CPU 算子的逐项位置、形状和选择理由见[CPU/NPU 分配说明](cpu-npu-allocation.md)。

## 环境与冻结范围

SD 使用厂商 NPU 启动文件，内核为 `6.1.111-rt42 #3`，三个 NPU/CMA 设备存在，运行库核心 SHA-256 为 `5def7ba75e4b59644be6f58deaf8b2bb3791c62ed38b52b6aea08b11a8f728ce`。此次恢复只替换 FAT32 启动分区中的文件；分区表、第二分区及 eMMC 不写入。写入及读回记录见[SD 恢复](../results/build/npu-sd-card-restoration-v3.json)。

训练检查点、训练集 543 窗的校准尺度及验证集 184 窗均保持冻结，没有训练、网格更新、归一化重拟合或独立测试集推理。输入为 `[1,23,110]`，输出单位为剩余观测老化试验轮次。实验程序与原融合程序分开生成，[准备工具](../tools/prepare_offload_probe.py)记录基础源文件哈希。

## 融合 CPU 热点

每个进程先预热五次，再逐窗回放验证集。诊断版本对 375 个回调及五处精确 GELU 分别计时；初始化、文件写出和量化整数审计在前向计时之外。以下比例来自带计时探针的完整回放，不是原生 ArmNN 首窗 profile。

| 类别 | 平均 ms/窗 | 占前向时间 |
|---|---:|---:|
| 样条基函数与 SiLU | 21.167 | 47.96% |
| 124 个 FC 分支 | 18.375 | 41.63% |
| 精确 GELU | 1.444 | 3.27% |
| 逐元素算术 | 1.150 | 2.60% |
| 动态矩阵乘法 | 0.777 | 1.76% |
| Softmax | 0.632 | 1.43% |
| 其他及计时/分派开销 | 0.590 | 1.34% |

FC 共 3501280 个逻辑 MAC/窗。按实际耗时筛选分支，再做单层 NPU 检查；节点数和 MAC 比例不能替代端到端测量。[分析工具](../tools/analyze_offload_profile.py)从原始 TSV/CSV 重新计算类别占比及分支排序。

## 支持与单层边界

[浮点支持查询](../probe/npu_op_support.cpp)对 ReLU、Sigmoid、Tanh、Softmax、Ceil、Add、Sub、Mul、Div 和动态 BatchMatMul 构建独立 Float32 图，只请求 Alnpu。十项均在优化阶段返回 40，没有加载或执行网络；该结论限定为当前库、类型与测试形状，不代表所有量化表达都不可用。样条递推和精确 GELU 仍在 CPU。

十二项真实权重单层对照使用同一完整 FP32 窗口捕获的输入与分支输出。CPU 为 NEON 点积；NPU 边界时间包含量化、补齐、布局转换、CMA 同步、执行及反量化。每项先预热五次，随后计时二十次，另保留一次 profile。所有输出以独立 int64 参考核对，允许误差 1 LSB。

| 分支/表达 | CPU ms | NPU 边界 ms | 选择 |
|---|---:|---:|---|
| `linear_x.spline_weight`，FC | 2.650 | 3.407 | 不采用 |
| 同一分支，补齐 1×1 Conv | 2.651 | 1.028 | 五分支候选 |
| `linear_x.base_weight`，FC | 0.413 | 1.615 | 不采用 |
| 同一分支，补齐 1×1 Conv | 0.413 | 0.231 | 未纳入组合；该夹具有 10 个输入值发生裁剪，已有隔离误差较大 |
| 四个 `feed_forward_*.0.spline_weight`，FC | 0.839–0.898 | 0.403–0.421 | 四分支候选 |
| 四个 `feed_forward_*.2.spline_weight`，FC | 0.775–0.846 | 0.895–0.988 | 保留 CPU |

物理 I/O 通道补到 8 的倍数，新增元素均表示实数零；UINT8 zero-point 保持 128。输入投影 Conv 将矩阵行映射到空间宽度，显式完成 NCHW 排列和恢复。未回退 CPU；计算图、工作负载事件和 HARD_NPU_IRQ 一并核对。SOFT_NPU_IRQ 未增长。

## 完整模型精度与耗时

配置中“头部”指两个原回归头样条分支；“FF”指两个时空块中的四个前馈首层样条分支；“输入”指 `linear_x.spline_weight` 的等价卷积。四个 FF 共占逻辑 FC MAC 的约 19.0%，再加输入分支约为 34.0%。基函数计算及其余算子仍在 CPU。

| 配置 | NPU 分支数 | 三轮均值 ms/窗 | 各轮均值范围 ms/窗 | 验证 RMSE | 相对 FP32 最大预测差 |
|---|---:|---:|---:|---:|---:|
| 融合 FP32 CPU | 0 | 44.520 | 44.076–45.405 | 2.465940 | 0.00000811 |
| 原头部 | 2 | 44.534 | 44.046–45.500 | 2.467977 | 0.030550 |
| FF | 4 | 43.917 | 43.010–44.424 | 2.465455 | 0.039159 |
| FF + 原头部 | 6 | 43.358 | 42.830–44.393 | 2.467897 | 0.048573 |
| 输入 + FF | 5 | 41.849 | 41.340–42.734 | 2.447859 | 0.575464 |

同一程序、相同验证序列、五窗预热，五种模式用三轮交错顺序执行。每轮中五分支候选均快于 CPU；速度比约 1.064，前向耗时减少约 6.0%。四、六分支及原头部的轮间范围重叠，不据此宣称稳定的微小收益。频率和温度没有受控固定，这些数值不是板卡极限性能或应用吞吐量验收。

FP32 保持原 `0.0001 + 0.0001*abs(reference)` 容差。量化候选超过该 FP32 容差的预测不能归类为浮点等价通过；整数误差另按 1 LSB 检查。五分支候选在验证集的 RMSE 略低，同时最大预测偏差显著增大；不能据此认定独立测试或实际任务的精度改善。应用允许偏差仍为 TBD。

三种新组合在 1/10 窗保存整数审计；结合独立层共核对 266344 个物理输出，最大差为 1 LSB。完整速度对照关闭逐输出审计，日志中的 `integer_audit=0` 表示该次没有进行整数复核，不能将对应 `max_integer_lsb=0` 解读为零误差证明。四、五、六分支的 CMA 分别恒定为 131072、192512、233472 字节；长期运行及反复加载/卸载尚未验收。

完整模型的补充轨迹中，四个 FF 分支的 NPU 边界约为 0.56–0.61 ms，输入卷积分支约为 1.21 ms，高于独立层反复执行的测值。[四分支轨迹](../results/board/offload-replay-v1-trace-ff4.json)与[五分支轨迹](../results/board/offload-replay-v1-trace-input_ff5.json)分别记录实际上下文。近半时间仍由 CPU 样条基函数承担，完整上下文的边界成本也抵消部分独立层收益。轨迹未区分缓存、DDR、同步和频率变化的因果贡献，不能据此认定内存带宽饱和。

## 交付与复现

原始证据及独立复核入口见[结果说明](../results/README.md)。[额外分支包](../artifacts/f3p-offload-rul-arm64-v1.tar.gz)包含程序、生成后的源码、验证输入、冻结尺度、超时工具及执行脚本；`weights.bin/weights.tsv` 从原生模型包提供后必须通过 `weights.sha256`。

电脑端准备与只读核对：

```powershell
python tools/prepare_offload_probe.py --output work/f3p-offload-new
python tools/analyze_offload_profile.py <CPU诊断结果目录>
python tools/verify_offload_replay.py results/board/recovered/offload-replay-v1
```

在 Ubuntu 中设置配套 `CXX` 与 `ARMNN_ROOT` 后执行新目录的 `sh build.sh`。板端使用新的 Linux 目录，依次校验和逐级执行；各步失败即停止：

```sh
sha256sum -c checksums.sha256
sha256sum -c weights.sha256
sh run.sh profile 1
sh run.sh profile 0
sh run.sh hybrid 1 qparams_ff4.tsv
sh run.sh hybrid 10 qparams_ff4.tsv
sh run.sh hybrid 1 qparams_input_ff5.tsv conv_input.txt
sh run.sh hybrid 10 qparams_input_ff5.tsv conv_input.txt
sh benchmark_offload.sh
```

上述为本地目录执行，不读取 U 盘。使用 U 盘部署前按[挂载规程](testing.md)创建 `/mnt/usb` 并挂载分区，再用 `gzip -dc <归档> | tar -xf -` 解压；配套 BusyBox 不支持 `tar -z`。COM4 传输使用分块 SHA-256 校验；下载期间仅临时降低控制台内核日志级别，退出块时恢复，内核环形日志保留。

当前选定六分支部署目标，应用默认模式尚未切换。后续完成共享库适配并确定允许预测偏差后，执行独立测试集、长期运行与应用整体性能验收。若继续降低耗时，样条基函数仍是最大 CPU 热点；本轮没有证明 DDR 带宽饱和或通过更换内存即可获得相同收益。
