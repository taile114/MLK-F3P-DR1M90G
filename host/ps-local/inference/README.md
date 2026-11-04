# 寿命引擎运行包与六分支接入

本目录将冻结模型封装为 PS 本地应用可动态加载的 C ABI 引擎。选定部署目标为 `ff6`：四个前馈首层样条 FC 与两个原回归头样条 FC。应用引擎、运行包生成与封装、回放清单、部署脚本及两端界面均支持六分支；应用共享库已完成板端 CPU / FF6 两轮 184 窗核对，本地 HDMI 回放可用；合并镜像的 HDMI / 采集 DMA / 硬件回放基础共存及两轮 184 窗混合回放已实板核对，长期稳定性待定，见[合并镜像摘要](../../../npu/results/board/combined-soc-ff6-hybrid.json)。

## 1. 包与运行位置

| 交付物 | 内容与用途 |
|---|---|
| [f3p-offload-rul-arm64-v1.tar.gz](../../../npu/artifacts/f3p-offload-rul-arm64-v1.tar.gz) | 六分支已验证图、独立 ARM64 程序、`qparams_ff6.tsv`、184 窗验证数据；解压根为 `f3p-offload-v1`，用于模型复核和应用源码适配 |
| [f3p-native-rul-arm64-v1.tar.gz](../../../npu/artifacts/f3p-native-rul-arm64-v1.tar.gz) | 提供相同的 `weights.bin` 与 `weights.tsv`；只提取所需权重，不改变已训练模型 |
| 配套 ArmNN | 板端 `/opt/armnn_lib/lib`；核心库 SHA-256 为 `5def7ba75e4b59644be6f58deaf8b2bb3791c62ed38b52b6aea08b11a8f728ce` |
| 六分支应用运行目录 | 由本目录工具生成，包含 `model.json`、权重、六分支计划、输入与 `libmlk_rul.so`；部署到 `/opt/mlk_rul` |
| PS 本地上位机 | [`host/ps-local`](../README.md)，F3P 上执行推理，经 HDMI 显示并发送 UDP v3 |
| PC 上位机 | [`host/remote-pc`](../../remote-pc/README.md)，接收结果并控制回放；不加载 ARM64 模型库 |

独立诊断包不含 `libmlk_rul.so` 或 `model.json`，不能直接作为界面的模型目录。`f3p-fused-rul-arm64-v1.tar.gz` 是 CPU / 原两分支基线，不是本次六分支计划的来源。

## 2. 六分支计划

| 权重分支 | 逻辑输入维数 | 逻辑输出维数 |
|---|---:|---:|
| `tvgtformer_blocks.0.feed_forward_temp.0.spline_weight` | 108 | 140 |
| `tvgtformer_blocks.1.feed_forward_temp.0.spline_weight` | 108 | 140 |
| `svgtformer_blocks.0.feed_forward_spat.0.spline_weight` | 99 | 140 |
| `svgtformer_blocks.1.feed_forward_spat.0.spline_weight` | 99 | 140 |
| `output_layer.0.spline_weight` | 1188 | 64 |
| `output_layer.2.spline_weight` | 576 | 1 |

六个分支均为 UINT8 FullyConnected，物理输入 / 输出补到 8 的倍数，zero-point 为 128。尺度逐行读取冻结 `qparams_ff6.tsv`；不引入 `linear_x` 或 Conv 计划。其余算子使用融合 CPU，精确 GELU、样条网格与原数学定义保持不变。

六个分支的物理形状、KAN 层内的 CPU/NPU 边界及保留 CPU 的理由见[FF6 分配说明](../../../npu/docs/cpu-npu-allocation.md)。

已有独立程序结果为三轮 × 184 窗：43.358 ms/窗、验证 RMSE 2.467897、相对 FP32 最大预测差 0.048573 轮，CMA 恒定 233472 字节。该记录不代表共享库、Qt 和网络链路已按六分支验收。完整证据见[额外分支验证](../../../npu/docs/additional-offload-validation.md)。

## 3. 应用调用链与输入输出

`板端 input_f32.bin → ReplayWorker → npu_* → libmlk_rul.so → ReplayController → HDMI / UDP v3 → PC`

| 边界 | 约定 |
|---|---|
| 引擎 ABI | [`rul_engine_api.h`](../../common/rul_engine_api.h)，v1；创建、预测、释放，单个常驻上下文 |
| PS 初始化 | [`npu_init_backend(root, 2)`](../src/npu.h)显式选择混合；`npu_init(root)`仍是 CPU |
| 同步预测 | `npu_infer(input, 2530, &result)`；由 [`ReplayWorker`](../src/replaycontroller.cpp) 所在线程调用 |
| 输入 | little-endian float32 `[1,23,110]`；每窗 2530 个有限值、10120 字节；回放数据已归一化，不重复处理 |
| 标签 / 参考 | 不传入 `predict`；参考预测仅供独立验收使用 |
| 结果 | `valid & 1`、float32 `rul`、完整前向 `infer_us`、实际 `backend` |
| 单位 | `observed_aging_test_session`，剩余观测老化试验轮次，不换算小时 |
| 分类 / 置信度 / 分级 | 当前模型不提供 |
| PC 协议 | UDP v3：224 字节结果 / 状态、40 字节控制；格式与有效标志见[接口规程](../../../docs/寿命模型-上位机接入.md) §5 |

C ABI v1 和 UDP v3 的字段布局无需因分支数量变化而改变。`backend=2`仍表示 CPU/NPU 混合，具体 `ff6` 计划由运行包身份和文件校验确定；不能用这个枚举证明实际运行了六个 NPU 分支。两端界面显示 FF6 六分支，后端初始化失败时保持错误状态，不自动切换 CPU。

## 4. 运行包与校验

| 层次 | 实现 |
|---|---|
| 源码生成 | 从冻结卸载包提取实际执行图；移除 CLI、全局分配钩子、捕获、profile 和整数审计写盘，保留固定缓冲与前向数学实现 |
| 引擎计划 | 显式读取 `qparams_ff6.tsv`，逐行核对六个冻结权重名、维数、尺度，拒绝重复、缺行及额外字段；不读取环境变量中的候选计划 |
| 运行包身份 | `execution_plan=ff6`、`quantization_plan=qparams_ff6.tsv`、`npu_branches=6` |
| 必需哈希 | `weights.bin`、`weights.tsv`、`qparams_ff6.tsv`、`input_f32.bin`、`libmlk_rul.so`，恰含五项 |
| 加载与暂存 | ReplayWorker 和 stage_rul.sh 核对相同计划身份与 SHA-256；原两分支包不满足当前契约 |
| 封装 | 编译完成后 `--seal`，核对 little-endian AArch64 ELF 共享库，再生成最终 SHA-256 |
| 生命周期 | 单实例、互斥调用；图、权重释放后再关闭 CMA。混合模式要求构建六个指定 NPU 图 |

公共输入契约保持 `model_id=feature_version=motor_stable23_v1`、`shape=[1,23,110]`、`normalized=true`。标签和参考预测不进入推理；检查程序和参考预测作为验收附件，不暂存到正常应用目录。

[应用引擎验证](../../tests/results/ff6/summary.json)记录共享库哈希。QEMU CPU 完成两轮各 184 窗，共 368 次预测，最大参考差约 `8.11e-6`；缺行、重复、维数、尺度、未知分支、多字段与截断计划均拒绝。QEMU 不模拟硬核 NPU。板端共享库另完成 CPU / FF6 各两轮 184 窗，FF6 与冻结独立程序的逐窗结果完全一致，均值 43.386 ms/窗；稳态 CMA 233472 字节，每轮释放后计数为 0，硬核中断增加 18522，软核未执行。本地 HDMI 1920×1080 已观察到有效 RUL 与窗口持续更新。

## 5. 构建与部署顺序

生成与封装可在 Windows 仓库环境执行；编译在配套 Ubuntu 环境执行。输出目录须不存在，最终库编译完成后再封装。

1. 从仓库根生成新的 `val184` 运行目录，不能复用已封装目录：

   ```sh
   python host/ps-local/inference/prepare_runtime.py --output /home/uisrc/mlk-rul-ff6 --split val184
   ```

2. 在配套 Ubuntu 中交叉编译；仅构建引擎不需要 Qt SDK，Qt 应用本身另需厂商 Qt 5.13.2 / OpenSSL 环境：

   ```sh
   export CXX=/home/uisrc/uisrc-lab-anlogic/tools/aarch64-linux/bin/aarch64-linux-gnu-g++
   export ARMNN_ROOT=/home/uisrc/f3p-rul-probe/vendor/armnn
   sh /home/uisrc/mlk-rul-ff6/build.sh
   ```

3. 在包含仓库与最终库的环境中封装清单，得到 `model.json` 与 `checksums.sha256`：

   ```sh
   python host/ps-local/inference/prepare_runtime.py --output /home/uisrc/mlk-rul-ff6 --seal
   ```

4. 先在板端独立验收目录运行 `check_engine`，再部署到应用目录；验收目录需另含 `reference_f32.bin`：

   ```sh
   export LD_LIBRARY_PATH=/opt/armnn_lib/lib
   ./check_engine /path/to/ff6-validation cpu 2
   ./check_engine /path/to/ff6-validation hybrid 2
   ```

   `2` 表示两次完整加载 / 推理 / 释放，每次 184 窗。CPU 按原 FP32 容差检查；混合误差须与六分支独立结果比较，退出码 0 本身不代表满足应用精度阈值。

5. 配置已封装运行目录和配套库根目录，在仓库根暂存到 rootfs overlay：

   ```sh
   export RUL_BUNDLE=/home/uisrc/mlk-rul-ff6
   export ARMNN_RUNTIME_ROOT=/home/uisrc/f3p-rul-probe/vendor/armnn
   bash platform/flash/stage_ps_local.sh
   ```

   此步骤依赖已准备的 Qt SDK / 板端运行库。模型目标目录为 `/opt/mlk_rul`，应用为 `/opt/qt_play`，ArmNN 为 `/opt/armnn_lib/lib`。暂存不等于系统烧录，不替代内核、DTB 和码流核对。

6. 在本地回放面板设置模型目录并选择 CPU/NPU；启动时也可通过 `MLK_RUL_ROOT` 指定目录。PC 启动监听，配置 PS 的目标地址 / 端口，通过 UDP v3 开始、暂停、单步、停止、定位及查询。PC 不传模型路径或窗口张量，只控制板端已部署数据集。

应用默认在每次结果返回后等待 1000 ms，再调度下一次；模型约 43 ms 的前向时间不等同于界面刷新周期。手动选择的后端失败时保留错误状态。数据集回放不随开机自动启动：面板的「开始/继续」按钮或远端回放指令才发起调度，`CPU/NPU` 需要硬 NPU、软 NPU 与 CMA 三个设备，选择失败时显示错误，不自动切换。

## 6. 上板前提与验收

板端需配套 NPU 内核 / DTB / PL 和 `/dev/hard_npu`、`/dev/soft_npu`、`/dev/cma_mem`；六分支使用硬核，厂商后端初始化仍会打开软核设备。合并 `soc_hdmi` 工程含厂商 `Video_NPU`，接线见 [PL-PS 接口约定](../../../docs/PL-PS接口约定.md) §6.1。`run_arm_app.sh` 默认 linuxfb，另需 `/dev/fb0`、Qt 库及平台插件。PC 仅显示结果时，板端仍需推理应用运行；当前没有独立无 Qt 的推理服务入口。

项目 HDMI / 传感器 / NPU 合并镜像已完成基础共存验证，逐窗推理与 CMA 数据见[合并镜像结果](../../../npu/results/board/combined-soc-ff6-hybrid.json)；长期运行、双端显示和实采适配仍须分别验收。资源范围与顺序见[接入规程](../../../docs/寿命模型-上位机接入.md) §3 / §6。验收至少包括：

1. 共享库 184 窗输出与已保存 `ff6` 结果一致或处于独立说明的数值容差；确认六个 ALHardNPU 分配及实际执行。
2. 两轮加载 / 释放、长期回放、CMA 稳定性、异常和正常停止；独立 CLI 的结果不能直接替代共享库生命周期验收。
3. HDMI / PC 的浮点结果、单位、窗口与后端一致，验证控制确认、断网后结果失效和记录。
4. 在匹配镜像中验证 HDMI、采集、NPU 并行；实采数据适配不属于本次数据集回放。
5. 冻结应用允许误差与预测周期后，再执行独立 165 窗测试集验收。模型训练和独立测试集验收分别执行。
