# F3P 边缘模型与寿命预测部署

本目录保存 FKP-DVGT 的训练与导出源码、处理后的运行至失效数据、板端实现、部署包及测试证据。电脑负责训练，F3P 负责数据集回放预测。

当前故障诊断采用[单主故障分类与分级](fault-diagnosis/model/单故障训练与推断.md)：`fault_mtl_f3p_cnn2d_single_v3`、`bench_fault_single_v1`，固定 `base6` 六路输入（XYZ、电流、转速、外壳温度）。正常、过载、欠压、过热、电机倾斜五类取最高 logit，仅输出所选故障的 1～3 级，正常为 0 级。当前权重使用四秒原生块短历史，已完成同批记录内开发训练与板端 test 回放；最终训练和独立现场精度验收另行实施。[总体设计](fault-diagnosis/设计方案.md)保留多标签对照与采集规格。

[F3P 独立测试包](fault-diagnosis/deploy/README.md)使用四个硬 NPU 卷积，ReLU/Pool 和其余分支留 CPU，拒绝 CPU 计算回退；携带全部 360 窗 test 输入和冻结参考。[实机复核](fault-diagnosis/deploy/BOARD_TEST.md)分类 360/360、分类与等级同时正确 356/360，平均网络前向由 CPU 的 76.186 ms 降至 3.988 ms，约 19.1 倍；623616 个实际卷积输出的 int64 复核在 1 LSB 内，CMA 释放通过。当前结果是同批记录内 test 回放，故障模型未接入上位机。[F3P 部署与量化规划](fault-diagnosis/F3P部署与量化设计.md)中的实时预处理、共享库及上位机运行包仍为规划；[实时故障诊断接入方案](fault-diagnosis/F3P实时故障诊断接入方案.md)规定后续适配。训练 Python、数据集和权重按忽略规则保留本地，独立部署工具与测试包另行保存。

现场记录、启动与暖机覆盖、故障等级驻留时长和独立重复要求见[采集执行规程](fault-diagnosis/采集执行规程.md)。

首批 SD 卡自然升温记录的原始归档、正常/轻度/中度切片及周期记录缺口见[首批数据说明](fault-diagnosis/首批数据说明.md)；这些数据尚未通过完整六路 60 秒连续性筛查。

按操作者时段整理的过载、欠压、过热和安装倾斜记录见[本批采集数据说明](fault-diagnosis/本批采集数据说明.md)，包含原件/认证解密、25 个原生运行片段、停机排除、源文件匹配和标签资格。两组轻度欠压的指定时段缺少 XYZ，独立倾角依据及其他数据质量项保持待核验。

补采归档与当前选择入口见[补采数据说明](fault-diagnosis/补采数据说明.md)：新两组轻度欠压均具有 XYZ，当前选择排除原来缺少 XYZ 的两组，并加入第二组重度过载。当前为 24 段故障候选与 2 段暖机片段，提供补采同文件 4 秒窗口索引；连续性与标签资格另行核验。

故障采集数据的当前入口、26 段状态对应、NPZ 读取示例及训练缓存准备方法见[数据使用说明](fault-diagnosis/data/README.md)。

## 寿命模型选定方案：六分支 CPU/NPU

部署目标为 `ff6`：四个前馈首层样条 FC，加两个原回归头样条 FC。六个分支均使用补齐 UINT8 FullyConnected，输入投影、样条基函数、GELU 和其他算子保留 CPU；不启用输入投影的 Conv 候选。

逐项计算位置、CPU/NPU 边界与分配理由见[FF6 分配说明](docs/cpu-npu-allocation.md)，其中区分当前表达不支持、迁移收益不合适及尚未验证的情况。

| 项目 | 选定值 |
|---|---|
| 独立模型包 | [f3p-offload-rul-arm64-v1.tar.gz](artifacts/f3p-offload-rul-arm64-v1.tar.gz)，解压根目录 `f3p-offload-v1` |
| 权重来源 | [f3p-native-rul-arm64-v1.tar.gz](artifacts/f3p-native-rul-arm64-v1.tar.gz)中的 `weights.bin`、`weights.tsv`；提供后核对 `weights.sha256` |
| 量化计划 | 包内 `qparams_ff6.tsv`，六行冻结尺度；不能使用默认的 `qparams_head.tsv` 代替 |
| 独立执行入口 | `sh run.sh hybrid 1 qparams_ff6.tsv`；不传 Conv 计划 |
| 已有实机范围 | 三轮 × 184 验证窗，平均 43.358 ms/窗，RMSE 2.467897，最大 FP32 预测差 0.048573 轮，CMA 恒定 233472 字节 |
| 应用接入状态 | `host` 已支持 FF6 共享库、回放清单、暂存和 UDP v3；板端共享库 CPU / FF6 各两轮 184 窗通过，HDMI 本地回放可用 |

上述归档是可执行诊断包，未包含上位机要求的 `libmlk_rul.so` 与 `model.json`。上位机应使用经过六分支适配、编译并封装校验的应用运行目录；不能将独立包目录直接填入界面的模型目录。现有包的字节与测试证据保持冻结。

当前 SD 上的六分支复核入口如下，程序与验证输入已在该目录，不读取 U 盘：

```sh
cd /mnt/mmcblk0p2/npu-tests/offload-v2/f3p-offload-v2
sha256sum -c weights.sha256
sh run.sh hybrid 1 qparams_ff6.tsv
sh run.sh hybrid 10 qparams_ff6.tsv
F3P_VERIFY_INT=0 sh run.sh hybrid 0 qparams_ff6.tsv
```

`0` 表示全部 184 窗，完整速度回放关闭整数写盘。数值边界及原始证据见[额外分支验证](docs/additional-offload-validation.md)。

## 当前状态

| 项目 | 状态与证据 |
|---|---|
| 数据 | `motor_stable23_v1`；训练 543 窗、验证 184 窗、测试 165 窗 |
| 已训练模型 | 两个时空块、三个注意力头的完整 FKP-DVGT，使用 `best.pt` |
| ONNX 导出 | FP32，固定输入 `[1,23,110]`，输出 `[1,1]`；电脑端 165 窗核对通过 |
| 厂商 ONNX 解析 | 受限，完整模型无法通过配套解析器 |
| 原生 Arm NN 实现 | 六段网络与五处主机 GELU；保留原已训练权重 |
| F3P 完整 CPU 推理 | 可用，`CpuRef`，165 窗全部通过参考容差，最大差约 `7.63e-06` |
| F3P 标签误差 | RMSE `3.256266531`，MAE `2.771933881`，单位为老化试验轮次 |
| 原生 ArmNN CPU 诊断耗时 | 混合后端约 `0.199` 秒/窗；同版本 CpuRef 约 `0.928` 秒/窗，未做正式预热性能测试 |
| 原生 CPU 后端对照 | `CpuAcc,CpuRef` 已通过 F3P 165 窗；最大参考差约 `8.58e-06`，本次对照耗时比约 `4.65` |
| NPU | 选定 `ff6` 六分支部署目标，独立程序完成 184 窗及三轮验证；原两分支基线保留，应用封装支持六分支，板端两轮 184 窗与冻结 FF6 逐窗结果一致 |
| 算子融合与分配复用 | 融合 FP32 CPU 完成 184 窗参考核对；三轮预热均值约 44.4 ms/窗，相对同条件原生参考约 4.4 倍，见[融合验证](docs/operator-fusion-validation.md) |
| 额外 NPU 分支 | 四个前馈首层样条 FC 可迁移；加入输入投影样条 Conv 的五分支候选在三轮验证回放中约 41.85 ms/窗，对照 CPU 约 44.52 ms；最大预测差约 0.575，取舍见[额外分支验证](docs/additional-offload-validation.md) |
| PS 应用集成 | `host/ps-local` 已实现动态引擎、独立回放线程及 UDP v3；六分支运行包与本地 HDMI 回放可用；合并镜像 HDMI / 采集 DMA / 硬件回放基础共存可用，长期稳定性待定，见[应用运行包说明](../host/ps-local/inference/README.md) |

CPU 移植数值一致性通过不等于寿命预测精度满足应用要求。模型输出单位为“剩余观测老化试验轮次”；数据集末次记录置零是端点约定，不能直接解释为小时或精确失效时间。

## 目录

| 路径 | 内容 |
|---|---|
| [docs/model-and-data.md](docs/model-and-data.md) | 数据划分、标签、特征、归一化、训练配置与模型来源 |
| [docs/porting.md](docs/porting.md) | 导出与移植方法、失败原因、算子处理及当前限制 |
| [docs/cpu-acceleration.md](docs/cpu-acceleration.md) | CpuAcc 真值编码、混合回退、执行证据与新测试包 |
| [docs/npu-sd-validation.md](docs/npu-sd-validation.md) | 厂商 NPU 启动、UINT8 硬核实测、结果返回与 eMMC 回退 |
| [docs/npu-model-plan.md](docs/npu-model-plan.md) | 原模型性能分布、FC 映射、训练集校准和完整模型验收规划 |
| [docs/npu-linear-validation.md](docs/npu-linear-validation.md) | 原模型 24 种线性尺寸的独立测试包、U 盘挂载与逐组验收 |
| [docs/npu-fc-padding-validation.md](docs/npu-fc-padding-validation.md) | m06 硬件超时后的原生 FC 零补齐对照、内置超时工具与分组验证 |
| [docs/rul-quantization-validation.md](docs/rul-quantization-validation.md) | 训练集校准、124 分支误差、真实权重及完整混合推理、CMA 修复验证与接续步骤 |
| [docs/operator-fusion-validation.md](docs/operator-fusion-validation.md) | 分配复用、样条/PCC/LayerNorm 融合、独立整数审计及三轮预热对照 |
| [docs/cpu-npu-allocation.md](docs/cpu-npu-allocation.md) | FF6 的 CPU/NPU 逐项分配、六分支形状、量化边界、选择理由与验证范围 |
| [docs/additional-offload-validation.md](docs/additional-offload-validation.md) | 融合 CPU 热点、浮点支持查询、真实层 FC/Conv 对照及额外分支的精度和速度 |
| [docs/testing.md](docs/testing.md) | Windows / Ubuntu 虚拟机 / 板端分工，编译、USB 部署及逐级测试 |
| [training/](training/README.md) | Python 模型与训练/评估/导出工具，处理数据和冻结模型产物 |
| [probe/](probe/README.md) | C++ 板端程序、交叉编译、数据包生成与执行脚本 |
| [tools/](tools/README.md) | ONNX 解析对照生成、交付物检查与原生模型打包工具 |
| [artifacts/](artifacts/README.md) | 已验证部署包，保持原始归档字节与 SHA-256 |
| [results/](results/README.md) | 实机 CSV/图/profile、摘要、QEMU 结果、构建与来源清单 |
| `work/` | 新生成的包、编译与测试目录，Git 忽略 |

## 原生 CPU 基线回放

不重新训练即可回放完整模型。将 [基础运行库包](artifacts/f3p-rul-probe-arm64.tar.gz) 和 [完整原生模型包](artifacts/f3p-native-rul-arm64-v1.tar.gz) 通过 U 盘复制到 F3P，在板端 Linux 文件系统中解压。保留 `/root/f3p-rul-probe/lib`，然后在 Windows PuTTY 所连接的 F3P shell 中执行：

```sh
cd /root/f3p-native-rul-v1
sha256sum -c checksums.sha256
sh run_native_rul.sh 1
sh run_native_rul.sh 0
```

单窗通过后再运行全部窗口。`0` 表示全部 165 窗；退出码 0 表示所有请求窗口通过参考容差。每次结果保存在独立目录，包含 `probe.log`、`predictions.csv`、动态依赖和首窗中间张量。详细安装和日志解释见 [测试规程](docs/testing.md)。

电脑端检查交付物，不运行训练或板端程序：

```powershell
Set-Location E:\competion\code\project\npu
python tools/validate_delivery.py
```

检查工具仅使用 Python 标准库；模型包生成及 ONNX 验证另需 [Python 环境](training/TRAINING_ENV.md)。

## 系统接口

上位机的接入链路为：`板端已归一化数据 → ReplayWorker → npu_* → libmlk_rul.so → ReplayController → HDMI / UDP v3 → PC`。模型在 F3P 的 PS 进程中执行；PC 负责回放控制、接收与显示结果，不加载 ARM64 引擎。当前输入使用数据集文件，不接入实采或四值模拟注入。

| 对端 | 契约 |
|---|---|
| PL / PS 数据 | [PL-PS 接口约定](../docs/PL-PS接口约定.md) |
| PS / NPU | [NPU-AI 接口约定](../docs/NPU-AI接口约定.md) |
| PC / PS 通信 | [上位机-PS 接口约定](../docs/上位机-PS接口约定.md) |
| PS 本地应用 | [PS 本地应用接口约定](../docs/PS本地应用接口约定.md) |
| 当前 PS 包装 | [npu.h](../host/ps-local/src/npu.h)与[npu.cpp](../host/ps-local/src/npu.cpp)，动态加载 C ABI v1 |
| 回放线程与校验 | [replaycontroller.cpp](../host/ps-local/src/replaycontroller.cpp)，单实例、五次预热、输入与文件 SHA-256 校验 |
| 六分支适配与运行包 | [host/ps-local/inference/README.md](../host/ps-local/inference/README.md) |

构建、部署和验收采用以下顺序，当前状态分别见[运行包说明](../host/ps-local/inference/README.md)：

1. 运行 `host/ps-local/inference/prepare_runtime.py`，从额外分支包提取已验证图与 `qparams_ff6.tsv`，生成应用源码；正常路径不含诊断 CLI、分配钩子和常规写盘。
2. 引擎严格核对六个指定分支的冻结名称、维数和尺度，保持单实例以及图、权重、运行时、CMA 的释放顺序；缺行、重复及修改尺度均拒绝。
3. 编译完成后封装 `model.json` 的 FF6 执行计划与文件 SHA-256；`ReplayWorker`、暂存脚本及两端界面使用相同方案。
4. 在配套 Ubuntu 中构建 `libmlk_rul.so` 与 `check_engine`，执行 `--seal` 封装校验；应用目录部署到 `/opt/mlk_rul`，配套库放 `/opt/armnn_lib/lib`。
5. 板端复核共享库的 CPU / 六分支混合结果和重复加载，再在本地回放面板设置模型目录、选择 CPU/NPU。PC 配置 PS 目标地址与端口并启动监听，通过 UDP v3 控制回放与接收结果。

输入仍为已归一化的 little-endian float32 `[1,23,110]`，每窗 2530 个值；标签与参考预测不进入推理。输出为 float32 RUL、有效标志和 `infer_us`，单位是剩余观测老化试验轮次，不换算小时。C ABI 和 UDP v3 的字段布局可继续使用；`backend=2` 表示混合执行，具体六分支计划必须由运行包身份与校验确定。

43.358 ms 是模型前向时间，界面默认在结果返回后等待 1000 ms 再调度，不等同于完整显示刷新周期。合并镜像已另行完成两轮 184 窗 FF6 混合回放，均值 42.888 ms/窗、CMA 释放后归零，并确认 HDMI / 采集 DMA 与硬件回放基础共存，见[合并镜像摘要](results/board/combined-soc-ff6-hybrid.json)；长期稳定性待定。Qt 启动依赖 `/dev/fb0`，仅使用 PC 显示也仍需板端推理应用实际运行。

## 后续工作

1. 使用已验证 FF6 应用运行包，保留显式 CPU 选项，补充长期运行与重复启停验收。
2. 复核应用引擎与六分支独立程序的 184 窗结果，完成重复加载与长期资源检查。
3. 明确应用允许预测偏差及周期，再对冻结方案进行独立 165 窗测试集验收。
4. 验收双端结果、控制命令及 HDMI / 项目 PL / NPU 长期共存；实采适配另行冻结输入契约。

CpuRef 完整预测的直接证据见 [实机结果](results/board/f3p-native-rul-board-v1-result.json)。混合 CPU 的 [板端 CSV 与性能复核](results/board/f3p-cpu-mixed-recovered-v1-verification.json)已通过。原两分支基线的 CMA 与应用验收范围见[量化与混合诊断](docs/rul-quantization-validation.md)；选定六分支的完整验证、误差与性能见[额外分支复核](results/board/offload-replay-v1-verification.json)，应用接入按本页及运行包 README 的部署和验收步骤执行。
