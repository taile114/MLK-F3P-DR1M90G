# CpuAcc 与 CpuRef 混合执行

本文说明已验证的 CPU 后端限制、混合实现、新测试包和 NPU 驱动检查边界。

## 实机基础层结果

[原始串口输出](../results/board/f3p-cpuacc-layers-v2.log)来自冻结的基础层 v2 包，后端单独请求 CpuAcc。

| 类别 | 结果与解释 |
|---|---|
| Relu、逻辑与、二维/三维 BatchMatMul、FullyConnected、分解 L2 | 返回 0，已知数值核对通过 |
| Less、GreaterOrEqual、广播 Less | v2 返回 70；观察到真值字节为 255，检查程序将其当作数值与 1 比较 |
| BOOL→Float32 Cast | 优化通过，但加载时拒绝数据类型，返回 50 |
| 四个 Ceil 门控用例 | CpuAcc 无法优化一元 Ceil，返回 40 |

当前检查程序按 Boolean 的零/非零真值比较，并保留 `raw_boolean` 日志；没有将整数 255 当成普通浮点 1。新增 `comparison_and` 用例核对比较结果接逻辑运算的行为。更新后的三个比较及串联用例已在 QEMU 的 CpuAcc 中通过，也已在 F3P 混合后端复测通过。原 v2 退出码作为原始证据保留。

## 混合策略与验证

当前源码支持 `CpuRef`、`CpuAcc` 或 `CpuAcc,CpuRef`，仅允许 CPU 后端；不请求 NPU。权重、网格、输入和六段模型计算保持不变，优先分配 CpuAcc，不支持的层回退 CpuRef。完整模型避开 BOOL Cast，因此不会触发该已知加载错误。

后端支持查询及优化成功不保证加载成功。Cast 在混合请求下仍返回 50，而不是自动在加载失败后重新优化为 CpuRef；不能将后端列表理解为任意异常的通用回退机制。

QEMU AArch64 与厂商 Arm NN 32.1.0 的完整 165 窗结果如下：

| 项目 | 结果 |
|---|---|
| 参考不匹配 | 0 |
| 最大参考差 | `8.58306884765625e-06` |
| 标签 RMSE / MAE | `3.2562667131254135` / `2.771934077233979` |
| 五处 GELU 输入的首窗最大差 | 不超过 `4.254281520843506e-06` |
| 混合基础层用例 | 14 个正向用例通过；Cast 对照按预期返回 50 |
| CpuRef / CpuAcc / 混合三组基础层 | 共 45 项，按后端对应预期检查，无意外失败 |

优化图中，排除 Input、Output、Constant、MemCopy 等结构节点后，1724 个计算节点分配给 CpuAcc，60 个 Ceil 一元层分配给 CpuRef。首窗 profile 中存在 Neon 和 Ref 工作负载执行事件。节点数不等于计算量比例，也不代表实机加速比。详细证据见 [verification.json](../results/emulation/cpu-accel-v1/verification.json)。

## 新部署包

归档为 [f3p-cpu-accel-arm64-v1.tar.gz](../artifacts/f3p-cpu-accel-arm64-v1.tar.gz)，2428852 字节，SHA-256 为 `b7bb155dba6672d72792ecf82d7893a509e15f4dccda61d515cdacd5a284fb66`。内含两个 AArch64 程序、相同已训练权重、165 窗、QEMU 证据及只读系统检查脚本，193 项内部文件校验通过。

该包解压根目录为 `f3p-cpu-accel-v1`，复用现有 `/root/f3p-rul-probe/lib`；不覆盖 `/root/f3p-native-rul-v1`。经 U 盘传入后，在板端 PuTTY 执行：

```sh
cd /root
tar -xzf /mnt/usb/f3p-cpu-accel-arm64-v1.tar.gz
cd /root/f3p-cpu-accel-v1
sha256sum -c checksums.sha256
sh check_npu.sh
sh run_layers.sh
F3P_PROFILE=1 sh run_rul.sh 1
sh run_rul.sh 0
sh run_rul.sh 0 CpuRef
```

先确认单窗通过，再执行全部窗口。根目录包装脚本默认使用 `CpuAcc,CpuRef`，`native-rul/run_native_rul.sh` 自身默认仍为 CpuRef。首窗 profiling 单独运行，完整耗时对照关闭 profiling；`F3P_PROFILE=1` 只记录第一个窗口，不持续记录全部 165 窗。

输出位于 `native-rul/results_<后端>_<时间>_<PID>/`。每段的 `segment_N.dot`、优化器消息和可选 `segment_N_profile.json` 用于核对实际后端分配及执行，`predictions.csv` 用于精度与诊断耗时。电脑端可执行 `python tools/summarize_cpu_backends.py <结果目录>` 汇总六段图和执行事件。

## F3P 完整模型对照

[实机串口结果](../results/board/f3p-cpu-mixed-v1-result.json)确认两次完整回放均为 165 窗、退出码 0、不匹配 0；使用同一新包二进制，完整测试均关闭 profiling。

| 项目 | CpuAcc,CpuRef | CpuRef |
|---|---:|---:|
| 最大参考差 | `8.583068848e-06` | `7.629394531e-06` |
| 标签 RMSE | `3.256266713` | `3.256266531` |
| 标签 MAE | `2.771934077` | `2.771933881` |
| 平均正向耗时 / 微秒 | `199451.7223` | `928101.1201` |

本次观测的耗时比为 `4.6533`，平均耗时降低约 `78.51%`。这是两次数据集诊断回放的比较，未控制频率、温度和预热条件，也未测量时延分位数，不能作为实时性承诺。原始串口文本见 [日志](../results/board/f3p-cpu-mixed-v1.log)。

更新后的 14 个正向基础层用例在实机混合后端全部通过，Cast 对照按预期返回 50。[板端文件复核](../results/board/f3p-cpu-mixed-recovered-v1-verification.json)保存两次完整回放 CSV 的独立重算结果，逐窗参考与标签核对通过；原始 CSV、六段 DOT 及首窗 profile 位于 [回收目录](../results/board/recovered/cpu-accel-v1/)。两个返回包与板端生成的 SHA-256 一致，解包后的文件另有 [逐文件清单](../results/board/recovered-files-sha256.json)。

## 实机首窗性能分布

六段 profile 中非 Constant 的 `Workload_Execute` wall-clock 累计为 `206184.333` 微秒，同一首窗程序报告正向耗时 `239449.353` 微秒。主要工作负载如下：

| 工作负载 | 调用数 | 累计微秒 | 占工作负载累计时间 |
|---|---:|---:|---:|
| FullyConnected | 124 | 38208.361 | 18.53% |
| CpuRef Ceil 一元运算 | 60 | 35016.120 | 16.98% |
| Reshape | 243 | 31502.521 | 15.28% |
| Subtraction | 280 | 21482.010 | 10.42% |
| Division | 202 | 20929.621 | 10.15% |
| Slice | 191 | 16846.650 | 8.17% |

这些比例来自带 profiling 的第一个窗口，包含首次执行准备及测量开销，不是稳态端到端占比。全连接仅占该次工作负载累计时间的约 18.5%，因此不能由小卷积的硬件耗时比推断完整模型的大幅加速。后续应同时评估 KAN 基函数计算、图中大量小算子及形状变换的合并，并计入量化、转置、CPU/NPU 边界和调用开销。执行顺序见 [模型加速规划](npu-model-plan.md)。

冻结包内 `verification.json` 保留打包时的 QEMU 状态，后续实机结论单独保存，不改写已部署归档。

## 持久存储与 NPU 前提

实机 `/proc/mounts` 显示 `/dev/root / ext4 rw,relatime`，没有单独的 `/root` 挂载；`df -h` 显示根文件系统约 7.0G、可用 6.2G。`/root` 位于持久 ext4，`/tmp`、`/run`、`/dev/shm` 为 tmpfs。保留测试包及结果可继续使用 `/root`，正常写入后可执行 `sync`。

厂商 SDK 的 [只读检查记录](../results/build/sdk-npu-inspection.json)显示内核 release 为 `6.1.111-rt42`，当前 SDK `.config` 关闭 `CONFIG_NPU_MODULE`；三个子选项为 `CONFIG_HARD_NPU_DRIVER`、`CONFIG_SOFT_NPU_DRIVER`、`CONFIG_CMA_MEM_DRIVER`。源码中的设备树 compatible 分别为 `anlogic,hard_npu`、`anlogic,soft_npu`。CMA 内存池配置和厂商 CMA 字符设备是不同层次。

SDK 配置不等于板端当前内核的完整配置；SDK 也缺少 `Module.symvers`，仅 release 字符串相同不能证明模块 ABI 兼容。没有加载未知模块、改写内核、设备树或 PL。`check_npu.sh` 仅收集设备、模块、内核配置、设备树、存储和 CPU 信息，避免手工输入复杂 find 表达式。此前模块路径中 `input` 被 `npu` 子串误匹配，以及缺少星号的设备树命令，均不能作为 NPU 模块或节点存在/缺失的有效证明。

[eMMC 板端清单](../results/board/f3p-npu-inventory-v1.json)确认硬核节点 `hard_npu@63F00000` 已存在，软核节点未发现；三个字符设备、可加载 NPU/CMA 模块及注册驱动未发现。builtin 扫描中的三个 input 条目属于子串误匹配。CPU 基线位于 eMMC/ext4。

[配套 SD 实机结果](../results/board/f3p-npu-sd-boot-v1-result.json)确认三个 NPU/CMA 字符设备和 `Alnpu` 后端注册均可用，CpuRef Relu 对照通过。[独立量化卷积复核](../results/board/f3p-npu-smoke-recovered-v1-verification.json)确认 UINT8 小卷积已在硬核 NPU 执行。完整寿命模型的量化、算子映射及实际收益尚待验证；SD 环境和 eMMC CPU 基线分开保存。
