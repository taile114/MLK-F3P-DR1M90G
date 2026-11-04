# 测试证据

本目录保存模型导出、ARM64 模拟与 F3P 实机测试的来源和结果；执行步骤见 [测试规程](../docs/testing.md)。

| 路径 | 证据与范围 |
|---|---|
| [board/f3p-native-rul-board-v1-result.json](board/f3p-native-rul-board-v1-result.json) | 实机串口提供的完整模型首窗/165 窗摘要，CpuRef 数值通过 |
| [board/f3p-native-layer-board-v2-result.json](board/f3p-native-layer-board-v2-result.json) | 实机 13 个基础层数值通过，BOOL Cast 对照返回 40 |
| [board/f3p-parser-compat-v2-board-result.json](board/f3p-parser-compat-v2-board-result.json) | 实机 17 个解析对照的逐项结果 |
| `board/*v1.log` | 解析和基础层失败定位时的串口文本，保留错误上下文 |
| [emulation/native-rul/verification.json](emulation/native-rul/verification.json) | 原生完整模型 QEMU 验收、二进制/权重/检查点哈希 |
| [emulation/native-rul/emulation_all/predictions.csv](emulation/native-rul/emulation_all/predictions.csv) | QEMU 的 165 条预测、参考、标签及诊断耗时 |
| [emulation/native-rul/pc_reference/first_native_boundary_comparison.json](emulation/native-rul/pc_reference/first_native_boundary_comparison.json) | 首窗五个 GELU 输入的逐段核对 |
| `emulation/native-layers/` | 14 项 QEMU 基础层结果 |
| `build/` | 配套编译器、ELF 架构、解释器、动态依赖及构建日志 |
| [source-snapshot.json](source-snapshot.json) | 从原工作区收集文件的来源和原始哈希 |
| [repository-validation.json](repository-validation.json) | 仓库内归档校验、语法/换行/链接及保存指标的检查结果 |
| [repository-regeneration.json](repository-regeneration.json) | 仓库内路径的权重、ONNX 导出/优化与兼容对照重新生成结果 |

混合 CPU v1 及同包 CpuRef 的板端逐窗 CSV 已回收并独立重算，保存在 `board/recovered/`；`emulation/` 的 CSV 属于 QEMU。原始 native-rul v1 的实机证据仍限于串口摘要，不与同包 CpuRef 对照混用。

冻结归档中的 README/verification 记录打包时状态，可能仍写“实机待验证”。归档保持原始哈希，当前实机结论由 `board/` 独立记录。QEMU 报告中的 `physical_f3p_full_model_verified=false` 描述模拟验证范围，不覆盖后续实机摘要。

`source-snapshot.json` 中的路径以本目录上一层 `npu` 为根，`source` 表示收集时工作区相对来源。两个 `tools/prepare_parser_*.py` 已调整为仓库内路径；原始来源哈希仅说明拷贝来源，不是当前工具文件校验。其他冻结数据、模型、证据及归档保持原始字节。

CPU 正向测试通过的结论均不代表 NPU 执行已验证。混合 CPU 与同版本 CpuRef 的诊断回放平均约 0.199 / 0.928 秒/窗，正式预热性能、峰值内存及 NPU 加速比待定。

CpuAcc 独立实机及存储证据见 [原始串口文本](board/f3p-cpuacc-layers-v2.log) 与 [摘要](board/f3p-cpuacc-layers-v2-result.json)。混合 CPU 全部 165 窗的 QEMU 证据见 [验证报告](emulation/cpu-accel-v1/verification.json) 和 [逐窗 CSV](emulation/cpu-accel-v1/mixed-predictions.csv)。SDK NPU 配置见 [只读检查](build/sdk-npu-inspection.json)；该配置不能直接等同于板端配置。

完整混合版本的实机首窗、165 窗及同版本 CpuRef 对照见 [原始串口](board/f3p-cpu-mixed-v1.log)、[指标摘要](board/f3p-cpu-mixed-v1-result.json)。三个回放均通过，[板端 CSV 与 profile 复核](board/f3p-cpu-mixed-recovered-v1-verification.json)确认两次完整回放数值及首窗工作负载分布。[eMMC NPU 清单](board/f3p-npu-inventory-v1.json)记录该基线系统的硬核 DT 节点存在、软核节点和驱动未就绪，不能代表配套 SD 系统状态。

备用 SD 的 [文件准备记录](build/npu-sd-card-preparation.json)保存目标卡身份、原启动文件备份、10 个已写文件的哈希及缓存刷新状态，并引用独立实机启动证据。文件复制本身不作为 NPU 执行证据；厂商文件的静态检查见 [启动文件检查](build/vendor-npu-boot-inspection.json)。

[SD 启动与运行库实机摘要](board/f3p-npu-sd-boot-v1-result.json)及 [启动串口](board/f3p-npu-sd-boot-v1.log)、[运行库串口](board/f3p-npu-sd-runtime-v1.log)确认配套镜像已启动、三个字符设备存在、`Alnpu` 已注册，CpuRef Relu 对照通过。该启动记录自身不包含 NPU 计算验收，独立卷积证据见下文。

最小量化卷积的 [QEMU 验证](emulation/npu-smoke-v1/verification.json)覆盖 u8、u8_s8、s8 三种组合，每种 3 次、每次 512 个输出，全部与独立整数卷积期望值一致。该记录只证明 CPU 数值；[U 盘传输记录](build/npu-smoke-usb-transfer-v1.json)保存 61 KB 测试包的回读哈希，不证明已在板端执行。

[SD 镜像解压工具核对](build/sd-busybox-archive-tools.log)使用原镜像提取的 BusyBox，经 QEMU 确认独立 gzip 可用；`gzip -dc ... | tar -xf -` 解压同一部署包后，27 项 SHA-256 全部通过。该验证只针对部署工具兼容性，不涉及 NPU 计算。

[u8 最小卷积实机串口](board/f3p-npu-smoke-u8-board-v1.log)与[指标](board/f3p-npu-smoke-u8-board-v1-result.json)确认 ALHardNPU 数值通过、硬核中断 +3；[类型对照](board/f3p-npu-smoke-board-v1-result.json)记录 u8_s8 / s8 均在 Alnpu 优化阶段返回 40。完整寿命模型尚未在 NPU 验证。模型待测全连接尺寸见 [静态清单](build/rul-linear-shape-inventory.json)。

[返回包记录](board/f3p-board-return-v1.json)保存两个板端归档的长度和 SHA-256。解包后的 170 个原始文件见 [回收目录](board/recovered/)，每个文件的原始字节哈希见 [清单](board/recovered-files-sha256.json)。[NPU 独立复核](board/f3p-npu-smoke-recovered-v1-verification.json)覆盖图、执行 profile、CSV、输出字节、运行库哈希和中断快照；[完整 CPU 复核](board/f3p-cpu-mixed-recovered-v1-verification.json)覆盖两份 165 窗 CSV 与首窗 profile。当前证据可通过 `python tools/validate_delivery.py` 从保存文件重新核对，不重新执行模型。

[恢复后的 SD 实机摘要](board/f3p-npu-restored-sd-v1-result.json)记录重新写入后内核、设备、运行库和 UINT8 小卷积串口复查通过；该次原始图/CSV/profile 尚未回收，不替代前述完整文件证据。[写卡记录](build/npu-sd-card-restoration-v1.json)保存卡身份、无备份替换授权、12 个回读文件及安全弹出结果。

[线性尺寸 CPU 预验证](emulation/npu-linear-v1/verification.json)覆盖原生 FC、1×1 卷积与通道补齐映射，共 74 项、126768 个输出，最大整数参考差为 0；原始文件在对应部署归档的 `emulation/`。编译及配套 BusyBox 工具验证见 `build/npu-linear-v1-*`。此报告只描述 CPU 预验证范围，实机尺寸结论独立记录如下。

[原生 FC 控制摘要](board/npu-linear-control-v1-result.json)和[串口](board/npu-linear-control-v1.log)确认微型原生 FC 的 ALHardNPU 数值通过；timeout 兼容工具实机自检也通过。[六种模型尺寸摘要](board/npu-linear-smoke-v1-result.json)与[指标转录](board/npu-linear-smoke-v1-serial-summary.csv)记录六种原生 FC 全部通过、两种宽矩阵的卷积映射通过。[完整矩阵失败](board/npu-linear-matrix-v1-failure.json)定位 m06 原生 FC 的硬件中断超时，未完成余下尺寸。原始文件已与补齐结果一同回收，冻结归档中的 QEMU 标志不改写为实机结论。

[故障归档与恢复记录](board/npu-linear-matrix-v1-recovery.json)保存 SD 上诊断归档位置及 SHA-256、完整断电后内核和设备节点检查范围；计算恢复由补齐包控制组确认。[补齐 CPU 预验证](emulation/npu-fc-pad-v1/verification.json)核对 25 项、44544 个量化输出及裁剪等价性；[配套镜像工具检查](build/npu-fc-pad-v1-sd-tools.log)确认内置超时工具入口可用。

[补齐组摘要](board/npu-fc-pad-v1-staged-result.json)与[完整矩阵摘要](board/npu-fc-pad-v1-matrix-result.json)记录 control、first、attention、matrix 均通过，24 个补齐尺寸的 Alnpu 最大差均为 1 LSB，无不匹配。[1356 个回收文件哈希](board/linear-v1-recovered-files-sha256.json)及[原始文件复核](board/npu-linear-recovered-v1-verification.json)覆盖未补齐诊断、补齐数值、布局、实际后端、profile 与中断；无重新推理或训练。全部性能值仍是三次无预热、带 profiling 的诊断。

[训练集校准与验证快照](calibration/rul-uint8-v1/)保存 543 窗校准范围、124 分支局部及隔离误差、各候选的 184 窗预测，不读取测试集。[六个真实权重层复核](board/real-weight-v1-verification.json)覆盖六个实际 NPU 层，96 个原始文件由[哈希清单](board/real-weight-v1-files-sha256.json)保护。

[首窗完整模型串口](board/quantized-native-v1-first-serial.json)确认两个回归头样条分支实际映射 NPU，其余保留 CPU。184 窗的 FP32 与 CPU 量化完成，持续混合回放因 CMA 分配失败未完成；具体复现与接续工作见[量化与混合诊断](../docs/rul-quantization-validation.md)。冻结部署包只记录各自的夹具范围，不能用首窗通过覆盖持续运行失败。

[分段运行时复核](board/cma-replay-v1-verification.json)覆盖 1/10/184 窗混合及完整 CPU 对照，CMA 首窗后稳定为 1204224 字节。共享运行时增长与重复注册拒绝也保留在[原始文件哈希](board/cma-replay-v1-files-sha256.json)范围内。[SD 写入记录](build/npu-sd-card-restoration-v2.json)与[启动及最小卷积](board/restored-sd-v2-result.json)说明环境，验收边界见[量化规程](../docs/rul-quantization-validation.md)。

[融合复核](board/fused-replay-v1-verification.json)覆盖同输入四模式、三轮交错回放和 1/10/184 窗 NPU 审计；[457 个原始文件](board/fused-replay-v1-files-sha256.json)及[返回记录](board/fused-replay-v1-return.json)保护原始证据。方法和当前边界见[算子融合验证](../docs/operator-fusion-validation.md)。

[额外分支复核](board/offload-replay-v1-verification.json)覆盖 12 项真实层 FC/Conv、十项 Float32 支持查询、三种组合的 1/10 窗整数检查及五模式三轮完整回放。共独立核对 266344 个物理输出，最大差 1 LSB；[1513 个原始文件](board/offload-replay-v1-files-sha256.json)、[返回记录](board/offload-replay-v1-return.json)及[CPU 热点](board/offload-replay-v1-profile.json)保存完整范围。额外分支带来的耗时与预测偏差见[验证规程](../docs/additional-offload-validation.md)，接续状态见[检查点](rul-offload-checkpoint-v1.json)。
