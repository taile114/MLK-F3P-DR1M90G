# 原模型线性层尺寸的 NPU 对照

本文规定模型实际线性层尺寸的独立 UINT8 对照、传输和板端执行顺序。六种代表性原生 FC 的实机数值通过；完整矩阵在 m06 的实际执行阶段停止，尚不能确认全部尺寸可用。

当前停止执行本包的原始 `matrix` 和未补齐 m06。后续入口为[原生 FC 补齐验证](npu-fc-padding-validation.md)，在完整断电恢复后依次核对控制组和单一补齐尺寸。以下原包命令用于说明既有诊断包与结果结构，不作为重新启动原矩阵的指令。

## 内容与预验证

[f3p-npu-linear-arm64-v1.tar.gz](../artifacts/f3p-npu-linear-arm64-v1.tar.gz)大小 641031 字节，SHA-256 为 `a0d0238b6fa92817165ab314087c23ac2d891f9a79c9dfd84414a4e0018d2c70`，解压根目录为 `f3p-npu-linear-v1`，内部 533 项校验。使用 SD 系统的 `/opt/armnn_lib/lib`，不使用原模型权重、训练数据或 ONNX 解析器。

| 组 | 内容 | 每组映射数 |
|---|---|---:|
| `control` | B=8、I=8、O=8 的原生 FC 和 1×1 卷积 | 2 |
| `smoke` | 6 个代表性模型尺寸，各测试 FC、1×1 卷积和通道补齐卷积 | 18 |
| `matrix` | 全部 24 种模型尺寸，各测试三种表达；包含 smoke 子集 | 72 |

`B/I/O` 为输入矩阵行数、输入维数和输出维数。`fc` 使用原生 FullyConnected；`conv` 使用 NCHW `[1,I,1,B]` 的等价 1×1 卷积；`conv_pad8` 将输入/输出通道补齐到 8 的倍数，补齐权重、输入及偏置的实数值为零。补齐规则是待验证候选，不代表已经确定的硬件要求。

全部表达使用 QAsymmU8 输入、权重和输出，zero-point=128，输入 scale=0.25，权重 scale=0.125，Signed32 零偏置 scale=0.03125。输出 scale 根据输入维数选择；具体公式见包内 README 和源码。每次重复改变输入，独立整数矩阵乘法核对全部输出和补齐通道，允许 1 LSB 舍入差。

[CPU 预验证](../results/emulation/npu-linear-v1/verification.json)覆盖 control 与 matrix，共 74 个 CpuRef 对照、126768 个量化输出，全部最大误差为 0；布局恢复与反量化输出也通过独立检查。完整 CPU 原始文件保存在部署归档的 `emulation/`。配套 SD 镜像的原始 BusyBox 经 QEMU 解压本包、533 项校验及 shell 语法检查通过，见 [工具验证](../results/build/npu-linear-v1-sd-tools.log)。这些结果不代表 Alnpu 支持或完整模型 NPU 验收。

## 实机结果与执行限制

[control 串口](../results/board/npu-linear-control-v1.log)及[摘要](../results/board/npu-linear-control-v1-result.json)确认 B=8、I=8、O=8 的原生 FC 分配为 ALHardNPU，三次计算最大差 1 LSB，硬核 IRQ 从 3 增至 27。相同矩阵的 NCHW `[1,8,1,8]` 卷积表达在优化阶段返回 40。该微型 FC 的 NPU 诊断耗时高于 CpuRef，不能据此宣称加速。

[smoke 结构化摘要](../results/board/npu-linear-smoke-v1-result.json)及[串口指标转录](../results/board/npu-linear-smoke-v1-serial-summary.csv)覆盖 18 个映射、36 个后端进程：28 个通过、8 个优化拒绝。全部 6 个原生 FC 尺寸在 Alnpu 上数值通过，实际计算层均为 ALHardNPU；B=110 的两种尺寸，原始及补齐通道的 1×1 卷积也通过。

以下微秒值包含量化、排列、enqueue、同步及输出恢复，来自三次无预热、带 profiling 的调用：

| B / I / O | CpuRef 原生 FC | Alnpu 原生 FC | Alnpu 1×1 Conv | Alnpu 补齐 Conv |
|---|---:|---:|---:|---:|
| 1 / 576 / 1 | 325.74 | 294.37 | 优化拒绝 | 优化拒绝 |
| 1 / 1188 / 64 | 4444.380333 | 335.98 | 优化拒绝 | 优化拒绝 |
| 10 / 207 / 1 | 402.71 | 447.53 | 优化拒绝 | 优化拒绝 |
| 11 / 1260 / 12 | 3390.60 | 939.96 | 优化拒绝 | 优化拒绝 |
| 110 / 23 / 23 | 1288.48 | 1810.00 | 439.03 | 408.08 |
| 110 / 207 / 23 | 6934.33 | 3343.12 | 1044.28 | 1042.38 |

CPU 对照为 CpuRef，并非部署模型的 CpuAcc/CpuRef 基线；不能把本表比例直接解释为完整模型加速比。卷积支持与具体尺寸有关，不能由小尺寸失败推导所有 1×1 卷积不可用。图、CSV 和 profile 原始文件的[回收复核](../results/board/npu-linear-recovered-v1-verification.json)确认已保存的数值、实际后端、执行事件及中断，串口转录与文件证据分别保留。

[完整矩阵失败摘要](../results/board/npu-linear-matrix-v1-failure.json)定位到 m06：B=11、I=12、O=21，CpuRef 通过，Alnpu 优化为 ALHardNPU 并加载成功，随后约 1029.83 毫秒出现 `filter_OC = 21`、硬核等待中断超时及 `CRASHED`，程序返回 60。外部 30 秒超时未触发。硬件状态未确认恢复前停止全部 NPU 调用；保存结果到 SD、正常关机并完整断电后再进行独立对照。

输出通道、输入通道与运行库实现的具体因果尚未确定。后续单独核对原生 FC 实数零补齐：物理输入从 12 增至 16、物理输出从 21 增至 24，再裁回 21 个输出；不能仅修改尺寸而新增非零输入/权重，也不能将优化通过作为可执行证明。

## 超时工具兼容入口

配套 SD 镜像 BusyBox 未编入 `timeout` applet，见 [功能检查](../results/build/sd-timeout-applet-check.log)。原包 `run.sh` 在此镜像中会打印 `A timeout command is required for hardware controls` 并在推理前退出；该退出不属于模型或 NPU 算子失败。

[兼容安装脚本](../probe/install_timeout_compat_v1.sh)包含独立 ARM64 超时工具的压缩编码。将该文件的全部文本粘贴到 PuTTY 执行；脚本包含自己的 `sh` 子 shell，不更改登录 shell 的错误处理设置。默认目录为已确认的 `/mnt/mmcblk0p2/npu-tests/linear-v1-19700101_002235-357/f3p-npu-linear-v1`。其他目录应先设置环境变量 `F3P_LINEAR_DIR` 为实际包目录。

脚本先复核原包 533 项校验，仅新增 `compat_timeout_v1/timeout` 和 `run_compat.sh`，不改写冻结的 `run.sh`、程序或校验清单。二进制和兼容入口均经过 SHA-256 检查。随后验证成功退出、保留退出码 40、超时返回 124，再自动运行 control；自检中的 `deadline_reached` 为预期输出。该安装器及 control 已在实机通过，无需为补齐包重新安装；补齐包自带同一超时工具。

独立工具仅实现本测试所需的 `timeout [-k KILL_SECONDS] SECONDS COMMAND`，使用单调时钟；到时向自有子进程组发送 SIGTERM，宽限期结束后发送 SIGKILL。正常退出码、信号退出和执行错误分别保留，超时返回 124。9 项 QEMU 对照见 [构建与运行日志](../results/build/timeout-compat-v1-build.log)和[摘要](../results/build/timeout-compat-v1-verification.json)，内嵌解码与配套 BusyBox 检查见 [安装脚本检查](../results/build/timeout-installer-v1-check.log)。该工具不等同于支持全部 GNU timeout 选项。

## 介质与挂载

Windows 将完整 `.tar.gz` 文件复制到本次传输 U 盘根目录，不在 Windows 上解压。此前要求保留现状的 aigo U 盘不能默认新增文件；继续使用时先明确本次新增文件授权，文件系统修复是独立操作。

U 盘安全弹出后插入 F3P USB HOST。以下操作在 Windows PuTTY 连接的 F3P shell 中执行。先核对设备名：

```sh
dmesg | tail -n 20
ls -l /dev/sd*
```

确认本次分区为 `/dev/sda1` 后，执行只读挂载。若实际名称不同，应使用实际分区，不将 SD 的 `mmcblk*` 作为 U 盘设备：

```sh
mkdir -p /mnt/usb
if ! grep -q '^/dev/sda1 /mnt/usb ' /proc/mounts; then
    mount -o ro /dev/sda1 /mnt/usb
fi
ls -lh /mnt/usb/f3p-npu-linear-arm64-v1.tar.gz
```

任一步出错时停止后续操作，保留错误输出。挂载成功不证明文件一定存在，文件存在也不替代哈希校验。

## 解压与包校验

以下脚本在子 shell 中顺序执行，失败即停止，不覆盖已有目录或结果：

```sh
sh <<'SH'
set -eu
printf '%s  %s\n' a0d0238b6fa92817165ab314087c23ac2d891f9a79c9dfd84414a4e0018d2c70 /mnt/usb/f3p-npu-linear-arm64-v1.tar.gz | sha256sum -c -
mkdir -p /mnt/mmcblk0p2/npu-tests
test_dir=/mnt/mmcblk0p2/npu-tests/linear-v1-$(date +%Y%m%d_%H%M%S)-$$
mkdir "$test_dir"
cd "$test_dir"
gzip -dc /mnt/usb/f3p-npu-linear-arm64-v1.tar.gz | tar -xf -
cd f3p-npu-linear-v1
sha256sum -c checksums.sha256 > package-check.log || { cat package-check.log; exit 1; }
echo 'package_check=533_items_passed'
sync
echo "test_directory=$PWD"
SH
```

校验通过后，按上文设置实际包目录并粘贴兼容安装脚本，自动执行第一轮 control。该组先运行两个表达，每个表达先 CpuRef，再仅请求 Alnpu。`control` 完成不自动进入模型尺寸矩阵；应检查输出后再推进。程序结果放在包目录下的 `results_control_*`，按表达、后端分别保存日志、图、profile、CSV 和中断快照。

## 后续组与结果解释

原包 control、smoke 已完成；matrix 在 m06 硬核执行阶段返回 60 并停止。当前不再启动原包的 smoke/matrix 或单独重试未补齐 m06。诊断归档已保存并回收，正常关机及完整断电后的内核、三个设备节点检查通过，补齐包控制组确认计算已恢复；全部 24 种补齐尺寸通过，见[恢复记录](../results/board/npu-linear-matrix-v1-recovery.json)和[补齐验证](npu-fc-padding-validation.md)。

每个进程超时 30 秒；CPU 失败、加载失败、数值不匹配、崩溃或超时会停止整组。Alnpu 优化阶段拒绝返回 40 时，记录为兼容性限制，继续下一项。运行库内部硬件超时可能先于外部超时发生，退出码 60 不能解释为外部计时器触发。

`result=capability_survey_completed` 只表示计划内探测完成，不能解释为全部尺寸受支持。需逐项核对 `summary.tsv`、实际计算层的后端、执行事件、数值及硬核/软核中断变化；原生 FC 可能走不同硬件单元，不能预先认定均为硬核。

程序分别记录 enqueue 时间和包含量化、布局转换、同步、输出恢复与反量化的时间；两者均排除参考计算与文件写入。全部 3 次调用启用 profiling、无预热，只用于定位，不作为稳态时延或完整模型加速比。输出中的原始通道饱和计数应与尺寸、尺度一同分析。

结果保存在 SD 第二分区；当前系统的 `/root` 是 initramfs，不用于持久保存。完成文件读取后可卸载 U 盘：

```sh
cd /
umount /mnt/usb
```

## 重新准备与编译

Windows 从 `npu` 目录执行 `python tools/prepare_npu_linear.py --output work/f3p-npu-linear-new`，生成独立源码包；已有目录拒绝覆盖。在 Ubuntu 中使用配套 CXX、ARMNN_ROOT 执行包内 `sh build.sh`。完成全部 CPU 用例后，可运行 `python tools/verify_npu_linear.py --bundle <结果包目录>`，独立复核整数期望值、CSV、原始输出布局与反量化文件；该工具不执行推理或训练。
