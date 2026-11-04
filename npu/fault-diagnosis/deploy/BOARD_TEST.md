# F3P 单故障模型离线板测

选定执行计划为 `hybrid_split`：四个 Conv3×3 在硬 NPU 执行，ReLU、MaxPool、其余分支与解码在 CPU 执行。独立测试包使用现有上位机 SD 系统，已完成 360 窗 test 回放、逐卷积整数复核及资源释放检查。

## 1. 交付与环境

| 项目 | 内容 |
|---|---|
| 归档 | [f3p-fault-test-v2.tar.gz](artifacts/f3p-fault-test-v2.tar.gz) |
| 板端目录 | `/mnt/fault-test/f3p-fault-test-v2` |
| 运行入口 | `sh run_test.sh hybrid_split 360`，默认模式同为 `hybrid_split` |
| 模型 | `fault_mtl_f3p_cnn2d_single_v3`，正常、过载、欠压、过热、倾斜五类，所选故障等级 1～3 |
| 输入 | 360 个冻结 test 窗口；已标准化 `base6`，每窗 27683 个小端 float32 |
| 内核 | `6.1.111-rt42 #8`，硬 NPU、软 NPU、CMA 设备均可用 |
| 运行库 | Arm NN 32.1.0，SHA-256 与构建配套库一致 |
| 上位机 | 测试时停止 `S99qtapp`；结束后服务已恢复，采集 ID `0x504c0103`、framebuffer `1920,1080` 可用 |
| 串口 | COM4，115200、8N1；调试结束已恢复原设置并释放端口 |

未重新训练或调整量化尺度。校准仍只使用 train 1063 窗；test 属于同批记录内留出，以下成绩不构成独立现场精度验收。板端使用冻结特征，不执行原生传感器预处理，不连接新的故障模型上位机接口。

## 2. 完整 test 结果

| 模式 | 分类正确 | 分类与所选等级同时正确 | 平均前向 / ms | P50 / ms | P95 / ms |
|---|---:|---:|---:|---:|---:|
| CPU FP32 | 360/360 | 356/360 | 76.186 | 76.103 | 76.446 |
| CPU 整数参考 | 360/360 | 355/360 | 60.265 | 60.233 | 60.359 |
| 四卷积硬 NPU | 360/360 | 356/360 | 3.988 | 3.946 | 4.132 |

NPU 对 FP32 CPU 的平均网络前向耗时比为 19.103，对 CPU 整数参考为 15.111。NPU 的 360 窗类别和选中等级与 FP32 判定逐窗相同，294 个异常窗口中类别与等级同时正确 290 个。

计时包含输入量化、NPU 传入传出及同步、CPU ReLU/MaxPool、其余 CPU 网络；不含原生预处理、资产读取、构图、三次预热、类别/等级解码及审计文件 I/O。NPU P95 为 4.132 ms，最大为 7.566 ms；首个计时窗口带工作负载 profiling。

浮点数值和量化误差分别核对：

| 对照 | 结果 |
|---|---|
| 板端 CPU FP32 对训练侧 FP32 | 最大完整原始输出差 `6.67572e-6`，容差检查通过 |
| 板端 CPU 整数对独立理想量化参考 | 276480 个 CNN 字节完全一致；完整原始输出最大差 `6.67572e-6` |
| 硬 NPU 对理想量化参考 | 最终 CNN 最大差 5 LSB，完整原始输出最大差 `0.361256` |
| 硬 NPU 对 FP32 | 完整原始输出最大差 `0.351259`；类别及所选等级不变 |

## 3. 硬件与整数证据

- 四份优化图的计算节点均为 `ALHardNPU / Alnpu`；没有 CPU 计算回退。
- 每个卷积有实际 `AlnpuALHardNPUWorkload_Execute` 事件。
- 完整回放硬 NPU IRQ 增加 1452，即四卷积 ×（三次预热 + 360 窗）；软 NPU IRQ 增加 0。
- 从 test 索引 0、84、150、200、246、300、359 捕获实际卷积输入、输出，以冻结权重、Signed32 bias 和尺度进行 int64 重算：28 组卷积、623616 个输出，最大差 1 LSB。
- 四层串联相对理想量化模型的 5 LSB 包含中间舍入误差的传播；单卷积检查使用该层的实际输入，不以理想上游张量替代。
- 模型 CMA 分配在全部计时窗口恒定为 360448 字节，卸载图和缓冲后为 0；完整回放未出现新的 NPU/CMA 内核错误。

机器可读摘要为 [summary.json](validation/board-v2/summary.json)，原始回收归档为 [raw-results.tar.gz](validation/board-v2/raw-results.tar.gz)。原始 CSV、图、profile、输入输出和独立复核 JSON 位于 [raw](validation/board-v2/raw)。

## 4. 融合表达限制

当前配套后端的 UINT8、zero-point=128 连续 Conv/ReLU/MaxPool 图被优化为四个硬 NPU 节点，但实际最终输出最小值为 41；ReLU 后应不低于量化零点 128。该图相对理想 CNN 的最大差为 87 LSB，不能作为数值正确的实现。

当前程序明确拒绝 `hybrid` 融合模式。选定计划将 ReLU/MaxPool 放在 CPU，保留全部四个卷积的硬 NPU 执行。限制只针对本模型在当前配套库下的这一量化表达，不扩展为所有类型或零点的全局能力结论。控制组原始数据保存在 [fused-control-v1.tar.gz](validation/board-v2/fused-control-v1.tar.gz)。

## 5. 重复测试与复核

在板端停止上位机后，使用已部署目录执行：

```sh
/etc/init.d/S99qtapp stop
cd /mnt/fault-test/f3p-fault-test-v2
sh run_test.sh cpu 360
sh run_test.sh hybrid_split 360
/etc/init.d/S99qtapp start
```

每次结果使用独立目录，保留失败，不自动重试硬件。复制结果到电脑后，先核对整数输出，再核对预测和硬件证据：

```powershell
.\.venv\Scripts\python.exe project\npu\fault-diagnosis\deploy\verify_integer.py --bundle project\npu\work\f3p-fault-test-v2 --results <NPU结果目录>
.\.venv\Scripts\python.exe project\npu\fault-diagnosis\deploy\verify_results.py --bundle project\npu\work\f3p-fault-test-v2 --results <NPU结果目录> --execution board
```

归档内部清单和用途标记保持冻结，实机验证状态以本页及板测摘要为准。当前结果覆盖独立模型回放；实时预处理、故障模型上位机接入和长期并发运行另行验收。
