# F3P 单故障模型离线测试包

本目录实现单主故障五分类与分级模型的独立测试包生成、AArch64 构建和 test 集回放。运行不依赖上位机、Python 或 PyTorch。

参数来自 Python/PyTorch 训练生成的 `best.pt`，部署只进行 BN 折叠与训练集 PTQ。正式共享库运行包、预处理资产与 Qt 接入步骤见[故障模型与上位机接入](../../../docs/故障模型-上位机接入.md)。

## 执行计划

| 模式 | CNN | 其他分支、温度隔离、分类与等级 |
|---|---|---|
| `cpu` | 原生 FP32、NEON | CPU FP32 |
| `quantized` | CPU 整数参考，UINT8、zero-point=128 | CPU FP32 |
| `armnn_ref` | 同一连续量化 CNN 图，Arm NN CpuRef | CPU FP32 |
| `hybrid_split`（默认） | 四个仅允许 Alnpu 的 Conv3×3 图；CPU ReLU/MaxPool | CPU FP32 |
| `hybrid` | 禁用：当前 UINT8、zero-point=128 的融合 ReLU 数值不正确 | 不执行 |

当前 NPU 计划为 `hybrid_split`，四个卷积均强制 Alnpu，拒绝 CPU 回退。ReLU 按实数零点 128 截断，MaxPool 在 CPU 执行；不能将该计划描述为全部 CNN 算子均在 NPU 执行。配套运行时初始化仍需要 `/dev/hard_npu`、`/dev/soft_npu` 和 `/dev/cma_mem`。

连续图虽然被优化为四个硬 NPU 节点并产生实际中断，但融合输出含低于 128 的值，与 ReLU 的实数零点不符；该表达已禁用。`armnn_ref` 用于对照连续量化图的 CPU 参考语义。逐卷积正确性以实际 NPU 输入字节、冻结权重及尺度的 int64 重算为准，单卷积容差为 1 LSB；四层串联相对理想量化模型的误差另行报告。

输入为已冻结预处理及标准化的 `test` 缓存，每窗 27683 个小端 float32，六分支按 `model.json` 顺序排列。包内包含全部 360 窗和来源哈希；不在板端执行原生采集预处理。机械/温度两条融合路径和所选故障分级均保留。

CNN 的 BN 折叠、非零偏置、输入实数零补齐及姿态有效位保持训练语义。校准只读取 train 1063 窗，以激活最大范围和每层权重最大绝对值确定 UINT8 尺度；valid/test 不参与尺度拟合。完整 FP32 与 PTQ 的 valid/test 结果记录于 `preparation.json`。

## 已验证范围

现有上位机 SD 系统无需重新烧录。[完整板测](BOARD_TEST.md)已完成 360 窗：NPU 分类 360/360、分类与等级同时正确 356/360，平均网络前向 3.988 ms，对同板 FP32 CPU 的 76.186 ms 加速约 19.1 倍。28 组实际卷积的 int64 核验最大差 1 LSB，CMA 退出释放为 0；测试后原上位机服务已恢复。

当前执行计划的归档为仓库本目录的 `artifacts/f3p-fault-test-v2.tar.gz`，解压根目录为 `f3p-fault-test-v2`。使用与 RUL 相同的 Linaro GCC 7.5.0 和 Arm NN 32.1.0 构建。原生 FP32、整数参考和 Arm NN CpuRef 的冻结参考已完成 360 窗 QEMU 回放；v2 复用相同权重、量化尺度和参考输入，板端结果以仓库 `validation/board-v2/` 为准。

| 模式 | test 分类正确 | 分类与等级同时正确 | CNN 相对独立整数参考 |
|---|---:|---:|---|
| `cpu` | 360/360 | 356/360 | FP32，最大完整输出差 `6.68e-6` |
| `quantized` | 360/360 | 355/360 | 276480 个输出字节完全一致 |
| `armnn_ref` | 360/360 | 355/360 | 最大差 1 LSB |

PTQ 相对 FP32 唯一新增的等级变化位于 test 索引 246、窗口 `88fe59e47f094ddb692fa39c`：倾斜真值及 FP32 为二级，量化为三级，类别不变。全部目标、参考类别和等级见 `data/test_index.csv`。Arm NN CpuRef 的完整原始输出相对整数模型最大差 `6.27e-4`，分类与等级相同。

包内 `validation/` 保存当前程序身份和冻结参考摘要；仓库 `validation/aarch64-emulation.tar.gz` 保存参考实现的逐窗 CSV、原始输出、图及 profile。退出失败、截断输出、整数输出修改和虚假 NPU 归属均经拒绝检查。实机审计捕获 test 索引 0、84、150、200、246、300、359 的每个硬 NPU 卷积输入及输出，覆盖不同类别和量化幅值。

## 生成与构建

在工作区根目录执行：

```powershell
.\.venv\Scripts\python.exe project\npu\fault-diagnosis\deploy\prepare.py --output project\npu\work\f3p-fault-test-v2
```

Ubuntu 使用与 RUL 相同的厂商工具链：

```sh
export CXX=/home/uisrc/uisrc-lab-anlogic/tools/aarch64-linux/bin/aarch64-linux-gnu-g++
export ARMNN_ROOT=/home/uisrc/f3p-rul-probe/vendor/armnn
sh build.sh
```

部署归档还需要配套 RUL 已验证的 AArch64 `bin/timeout`；以包内 `checksums.sha256` 核验所有资产。动态运行库使用板上 `/opt/armnn_lib/lib`，构建环境的哈希保存在 `vendor-runtime.sha256`。更换运行库应重新核对后端、驱动和码流。

完成构建并将二进制放回生成目录后，在工作区根目录封包；输出归档必须不存在：

```powershell
.\.venv\Scripts\python.exe project\npu\fault-diagnosis\deploy\package.py --bundle project\npu\work\f3p-fault-test-v2 --output project\npu\fault-diagnosis\deploy\artifacts\f3p-fault-test-v2.tar.gz
```

## 板端测试顺序

将归档复制到板卡可写目录；与 RUL 使用同一套已验证的 NPU 启动镜像。以 root 运行，先验证小样本，成功后完成全量回放：

```sh
mkdir -p /mnt/fault-test
gzip -dc f3p-fault-test-v2.tar.gz | tar -xf - -C /mnt/fault-test
cd /mnt/fault-test/f3p-fault-test-v2
sh run_test.sh cpu 3
sh run_test.sh quantized 3
sh run_test.sh hybrid_split 3
sh run_test.sh cpu 360
sh run_test.sh hybrid_split 360
```

NPU 小样本完成后先核对实际逐卷积输出，通过后再运行 `360`。发生驱动超时或硬件错误时保留结果，处理板卡状态后再测试，不自动重试。脚本不加载 PL、不更换驱动、不修改启动镜像。当前精简系统的 tar 不支持 `-z`，解包使用上面的 gzip 管道。

每次运行新建结果目录，保存退出码、逐窗原始输出、类别、等级、置信度、完整推理耗时、CNN UINT8 输出、图、首个实测窗口的 profile、CMA、IRQ 及内核日志。预热三窗不计入耗时。CMA IO 和图仅初始化一次，退出释放。

在电脑解压结果后执行：

```powershell
python verify_integer.py --bundle . --results results_hybrid_split_...
python verify_results.py --bundle . --results results_hybrid_split_... --execution board
```

电脑端 `verify_integer.py` 依赖 NumPy，使用 int64 从实际输入重算卷积；板端无需 Python。`verification.json` 重算分类/等级精度、FP32 偏差、PTQ 偏差、P50/P95 耗时及资源记录。NPU 验收要求实际图分配、工作负载事件、硬 NPU IRQ 增量和整数审计；实机性能与同板 `cpu 360` 比较。测试集是同批记录内留出，当前权重为开发候选，不代表独立实验或现场精度验收。
