# 构建、部署与测试规程

本文规定已完成测试的执行方法、通过条件和证据位置。测试使用数据集回放，不需要采集外设，也不重新训练。

## 环境与执行位置

| 环境 | 用途 | 已验证配置 |
|---|---|---|
| Windows 电脑 | 处理数据、训练/导出、整理包、运行 PuTTY | Python 3.12.14，PyTorch 2.8.0+cu126，ONNX 1.19.1，ORT 1.23.2 |
| Ubuntu 开发虚拟机 | ARM64 交叉编译、QEMU 数值预验证 | 厂商 Ubuntu 18.04，Linaro GCC 7.5-2019.12 |
| F3P Linux | 加载实际运行库并预测 | AArch64，Buildroot 2022.02.6，Linux 6.1.111-rt42，约 1 GiB 内存 |

PuTTY 在 Windows 启动，连接板端 Linux 串口。当前已确认 COM4 为 PS 控制台，115200 波特率、8 数据位、无校验、1 停止位、无流控；COM 编号在其他电脑可能不同。串口连接后登录的是 F3P，提示符为 `#`。Ubuntu 虚拟机提示符 `uisrc@ubuntu:~$` 是另一个系统。

出厂板端和开发虚拟机的已用默认登录信息分别为 `root/root` 和 `uisrc/root`。虚拟机只在交叉编译或 QEMU 测试时需要；运行现成板端程序不需要虚拟机、板端 Python、板端编译器或互联网。

## 测试层次与通过条件

| 层次 | 测试方法 | 通过条件 / 已观察结果 |
|---|---|---|
| 处理数据 | 有限性、整机划分、输入排列、来源抽查 | 543/184/165 窗，检查见数据目录 `training_validation.json` |
| ONNX 导出 | CPU PyTorch 和 ORT 对全部 165 窗逐窗前向 | `abs(error) <= 1e-4 + 1e-4*abs(reference)`，原始/优化图均通过 |
| 厂商解析器 | 最小前缀、正负轴、17 个独立与完整对照 | 记录每项退出码，已知失败对照不要求返回 0 |
| 原生基础层 | 14 项已知数值用例 | 13 项返回 0，BOOL→FLOAT 对照返回 40 |
| 完整原生图 QEMU | 同一 AArch64 二进制、厂商库，先首窗再 165 窗 | 全部通过，最大参考差 `7.62939453125e-06` |
| 完整原生图 F3P | 与 QEMU 相同包，先首窗再 165 窗 | 全部通过，最大参考差 `7.629394531e-06` |
| NPU 最小卷积 | UINT8 优化分配、数值及中断对照 | ALHardNPU 通过，3 次硬核中断，误差不超过 1 LSB；完整寿命模型 NPU 待定 |

参考预测来自原 PyTorch CPU 模型；真实标签用于 RMSE/MAE。二者都不进入模型输入。单窗通过用于定位加载/结构问题，完整 165 窗用于数值核对，不代表任意输入范围均已覆盖。

## Windows：文件完整性检查

从项目仓库执行，Python 标准库即可：

```powershell
Set-Location E:\competion\code\project\npu
python tools/validate_delivery.py
```

工具检查冻结产物哈希、全部部署归档内部清单、原生二进制架构、源码语法、Shell 换行、文档相对链接和实机摘要，并从保存的 QEMU CSV 重算参考误差与标签指标。没有重新运行板端测试。

## Windows：生成新的源码包

使用 [训练与导出环境](../training/TRAINING_ENV.md) 的 Python，从 `npu` 目录执行：

```powershell
python probe/prepare_native_rul.py --output work/f3p-native-rul-new
python tools/prepare_parser_diagnostics.py --output work/parser-diagnostics-new
python tools/prepare_parser_compat_v2.py --output work/parser-compat-new
```

完整原生包生成会校验 `best.pt`，导出 209 条状态及位置编码，执行首窗原模型前向，并保存五个 GELU 输入用于边界核对。兼容包会重新执行全部完整变体的 165 窗 ORT 核对，检查 302 个原初始化器不变。已有目录/归档拒绝覆盖。

重新生成包的 gzip 元数据、目录名或环境路径可能不同；冻结部署包的 SHA-256 仍以 `artifacts/manifest.json` 为准，不能把新生成包标成原实机已验证的归档。

## Ubuntu：交叉编译完整模型

开发包应解压在虚拟机 Linux 文件系统中，避免 Windows 长路径与符号链接限制。已用开发包位置为 `/home/uisrc/uisrc-lab-anlogic`，编译器为：

```sh
export CXX=/home/uisrc/uisrc-lab-anlogic/tools/aarch64-linux/bin/aarch64-linux-gnu-g++
"$CXX" --version
"$CXX" -dumpmachine
```

目标必须为 AArch64 Linux。将基础部署包与新源码包复制进虚拟机。基础包同时带有编译用 `vendor/armnn.tar.gz`，在新工作目录执行：

```sh
mkdir -p ~/f3p-build
cd ~/f3p-build
tar -xzf /path/to/f3p-rul-probe-arm64.tar.gz
mkdir -p f3p-rul-probe/vendor/armnn
tar -xzf f3p-rul-probe/vendor/armnn.tar.gz -C f3p-rul-probe/vendor/armnn
export ARMNN_ROOT=$HOME/f3p-build/f3p-rul-probe/vendor/armnn
tar -xzf /path/to/f3p-native-rul-new.source.tar.gz
cd f3p-native-rul-new
sh build.sh
```

`/path/to/` 需替换为复制到虚拟机内的文件路径。生成 `bin/rul_native`；使用 `aarch64-linux-gnu-readelf -hW/-lW/-dW` 检查机器类型 AArch64、解释器 `/lib/ld-linux-aarch64.so.1` 以及动态依赖。原生完整模型不链接 ONNX Parser/protobuf。

基础层复编可解压 `f3p-native-layer-probe-arm64-v2.tar.gz` 到新的虚拟机工作目录，用仓库 `probe/native_layers.cpp` 覆盖包内对应源码，并把 `probe/build_native_layers.sh` 放到包根目录，保持同一 `CXX`/`ARMNN_ROOT` 设置后执行该脚本。

## Ubuntu：QEMU 预验证

已用 Ubuntu 官方归档中的 `qemu-user-static_6.2+dfsg-2ubuntu6.32_amd64.deb`，本地解包后使用 `qemu-aarch64-static`。包 SHA-256 为 `7a714bfa6e67ace04af7110ac28469639cb4194e4d4863f4b23784bd57a13ae5`。QEMU 不随仓库分发，可从 [Ubuntu 官方目录](https://archive.ubuntu.com/ubuntu/pool/universe/q/qemu/) 获取对应版本。

在编译后的完整模型包根目录运行，`QEMU` 指向实际可执行文件：

```sh
export QEMU=/home/uisrc/f3p-qemu-6.2/usr/bin/qemu-aarch64-static
sysroot=$("$CXX" -print-sysroot)
mkdir emulation_first
"$QEMU" -L "$sysroot" -E LD_LIBRARY_PATH="$ARMNN_ROOT/lib" \
  bin/rul_native "$PWD" 1 "$PWD/emulation_first" > emulation_first/probe.log 2>&1
echo $? > emulation_first/exit_code.txt
mkdir emulation_all
"$QEMU" -L "$sysroot" -E LD_LIBRARY_PATH="$ARMNN_ROOT/lib" \
  bin/rul_native "$PWD" 0 "$PWD/emulation_all" > emulation_all/probe.log 2>&1
echo $? > emulation_all/exit_code.txt
```

先检查首窗 `exit_code.txt` 为 0，再执行全部窗口。首窗五个 `pre_gelu_*.bin` 可和 `pc_reference/` 逐元素比较。已保存的 QEMU 逐窗输出与指标见 [结果目录](../results/README.md)。QEMU 只验证 ARM64 程序/运行库和数值，不能模拟板上 NPU，也不能代表实机性能。

完整回放通过后，用 [打包工具](../tools/README.md) 生成新归档，不覆盖冻结包。

## F3P：通过 U 盘部署

在 Windows 将以下文件直接复制到 U 盘根目录，不在 Windows 解压：

- `artifacts/f3p-rul-probe-arm64.tar.gz`：首次安装或运行库目录缺失时需要。
- `artifacts/f3p-native-rul-arm64-v1.tar.gz`：完整模型。
- `artifacts/f3p-native-layer-probe-arm64-v2.tar.gz`：基础层诊断时需要。
- `artifacts/f3p-parser-compat-v2.tar.gz`：解析器复现时需要。

安全弹出后插入 F3P USB HOST。当前 U 盘识别为 `/dev/sda1`；确认实际设备名后，在 PuTTY 的板端 `#` 执行：

```sh
mkdir -p /mnt/usb
mount -o ro /dev/sda1 /mnt/usb
cd /root
tar -xzf /mnt/usb/f3p-rul-probe-arm64.tar.gz
tar -xzf /mnt/usb/f3p-native-rul-arm64-v1.tar.gz
cd /root/f3p-rul-probe
sha256sum -c deployment.sha256
cd /root/f3p-native-rul-v1
sha256sum -c checksums.sha256
sh run_native_rul.sh 1
sh run_native_rul.sh 0
```

解压前确认目标目录未保存需保留的新文件。已有 `/root/f3p-rul-probe/lib` 且与基础包一致时，无需重复解压基础包。拔 U 盘前执行 `umount /mnt/usb`。USB 内核日志可能覆盖提示符，日志停止后按回车即可重新显示 `#`。

## F3P：基础层与解析器复现

解压对应包后，执行基础层数值测试：

```sh
cd /root/f3p-native-layer-probe-v2
sha256sum -c checksums.sha256
sh run_native_layers.sh CpuRef
```

已知 BOOL 转 FLOAT 对照 `cast_bool_float` 返回 40，其余 13 项应返回 0。独立请求 `CpuAcc` 的命令为 `sh run_native_layers.sh CpuAcc`；冻结 v2 包存在布尔比较检查限制，且 CpuAcc 不支持 Ceil 门控，不能要求该组全部返回 0。当前混合包及对应预期见 [CPU 加速说明](cpu-acceleration.md)。

解析器兼容性复现：

```sh
cd /root/f3p-parser-compat-v2
sha256sum -c checksums.sha256
sh run_diagnostics.sh
```

该包仅解析，不预测。17 个用例的预期结果见 [实机解析结果](../results/board/f3p-parser-compat-v2-board-result.json)，包含明确的失败对照。正轴 Unsqueeze 与常量 MatMul/Gemm 通过，完整模型停在 Less。

## 实机验收与结果解释

| 项目 | 首窗 | 全部窗口 |
|---|---:|---:|
| 窗口数 | 1 | 165 |
| 参考不匹配 | 0 | 0 |
| 最大参考差 | `1.907348633e-06` | `7.629394531e-06` |
| RMSE / MAE | `3.540578842` / `3.540578842` | `3.256266531` / `2.771933881` |
| 平均正向微秒 | `934595.05` | `923880.3768` |
| 退出码 | 0 | 0 |

本表全部窗口结果目录为 `/root/f3p-native-rul-v1/results_19700101_000452_295`。板端日期未校准。该 native-rul v1 实机证据来自串口输出摘要，原包逐窗板端 CSV 尚未回收。独立混合 CPU v1 包的两份 165 窗实机 CSV 已保存并 [重算通过](../results/board/f3p-cpu-mixed-recovered-v1-verification.json)，不能与 `emulation/` 的 QEMU CSV 混用。

参考差衡量移植一致性，RMSE/MAE 衡量与数据标签的误差。`mean_forward_us` 为诊断耗时，包含网络段和主机 GELU，首窗包含中间文件写入；不包含网络构建，不承诺稳态时延。控制台运行结束后显示日志，完整 165 窗需要等待。

## NPU 验证前提

eMMC CPU 基线系统中 `/dev/hard_npu`、`/dev/soft_npu`、`/dev/cma_mem` 均缺失。独立厂商 SD 镜像已在实机启动，三个节点存在，运行库注册 `CpuAcc,Alnpu,CpuRef,ALRef`。UINT8 最小卷积已通过硬核 NPU 实机图、执行事件、逐元素输出及中断复核，见 [SD 验证](npu-sd-validation.md)。完整寿命模型 NPU 推理待验证。SD 环境使用厂商配套 PL 和设备树，eMMC 系统保留。

验证硬件执行需同时具备：可用驱动和设备、匹配的 PL/设备树、受支持的数据类型/算子、优化图中明确的硬件分配、实际执行与数值/性能结果。只设置后端名称或程序输出预测值不足以证明 NPU 执行。

## CPU 混合后端与存储

基础层表的 14 项对应冻结 v2 包；当前新包增加比较串联用例，共 15 项。新包的 14 个正向用例和完整 165 窗已通过 QEMU 与 F3P 混合 CPU 验证，执行顺序和已知失败对照见 [CPU 加速说明](cpu-acceleration.md)。实机根文件系统已确认是可写 ext4，`/root` 不是 tmpfs。NPU 检查可直接执行新包内 `sh check_npu.sh`，不再依赖手工输入 find 表达式。

混合完整模型已完成 F3P 165 窗和同版本 CpuRef 对照，平均约 199.45 / 928.10 毫秒，详见 [CPU 加速说明](cpu-acceleration.md)。NPU 环境准备使用 [独立 SD 候选](npu-sd-validation.md)，当前 eMMC 保留。
