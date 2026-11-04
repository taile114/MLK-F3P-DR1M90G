# 原生全连接实数零补齐验证

本文规定 m06 原生 FC 硬件超时后的独立补齐对照、部署步骤和逐组验收。补齐的数值等价性已通过 CPU 验证，全部 24 种模型尺寸的补齐 FC 已通过 Alnpu 实机数值检查；真实权重量化与完整模型推理待验证。

## 条件与映射

[原始矩阵诊断](../results/board/npu-linear-matrix-v1-failure.json)中，m06 的 `B=11,I=12,O=21` 在 CpuRef 通过、Alnpu 优化及加载通过后发生硬核中断超时，返回 60。输出日志包含 `filter_OC = 21`；该信息不足以确定通道数是根因，已有输出 23 通道的其他尺寸通过。当前不重新执行该未补齐表达或原包 matrix。

本包只测试 `fc_pad8`：I、O 分别补到 8 的倍数，B 保持不变。新增输入、权重和偏置均为实数零；QAsymmU8 的实数零编码是 128。m06 物理输入为 `[11,16]`、权重为 `[24,16]`、输出为 `[11,24]`，恢复时逐行取前 21 个输出。整数参考仍按原始 I/O 计算，并检查补齐输出为实数零。

补齐到 8 在本轮全部尺寸上可用，尚未确定为普遍硬件要求或原超时的根因。源码未使用训练权重、校准样本或标签，不进行训练；本轮不测量完整寿命模型的量化误差。

## 部署包与预验证

[f3p-npu-fc-pad-arm64-v1.tar.gz](../artifacts/f3p-npu-fc-pad-arm64-v1.tar.gz)为 266291 字节，SHA-256 为 `9434fb95d10d934738593677deebed5666d7c09d1d657eda5b8d1ef0fe292090`，解压根目录 `f3p-npu-fc-pad-v1`，内部 193 项校验。使用 SD 系统 `/opt/armnn_lib/lib`；包内 `bin/timeout` 已内置并由 `run.sh` 加入 PATH，无须安装器或更改系统目录。

[CPU 预验证](../results/emulation/npu-fc-pad-v1/verification.json)覆盖 1 个控制尺寸和 24 个模型尺寸，共 25 项、44544 个量化输出，整数差均为 0。全部 25 项裁剪、反量化输出与原始未补齐 CpuRef 文件逐字节一致。原始 CSV、字节输出、图与 profile 在归档 `emulation/` 中。[配套 BusyBox 检查](../results/build/npu-fc-pad-v1-sd-tools.log)覆盖 gzip/tar、193 项校验、shell 语法及内置超时工具入口。

上述预验证只针对 CPU。三次调用输入均不同，启用 profiling、无预热，时间仅用于诊断；CpuRef 也不是完整模型的混合 CPU 性能基线。

## 实机结果

[控制组、m06 与四种输出 21 通道的摘要](../results/board/npu-fc-pad-v1-staged-result.json)确认恢复后的控制计算及补齐对照通过。[完整矩阵摘要](../results/board/npu-fc-pad-v1-matrix-result.json)覆盖全部 24 种补齐 FC，48 个 CPU/NPU 进程全部通过，CpuRef 最大差 0、Alnpu 最大差 1 LSB，均无不匹配、无原始输出饱和、无优化拒绝，硬核 IRQ 从 195 增至 1482，软核保持 0。

[回收文件清单](../results/board/linear-v1-recovered-files-sha256.json)保留原尺寸与补齐轮次的 1356 个原始文件，归档 SHA-256 已与板端一致；独立数值、图、profile 和 IRQ 的复核见[文件验证](../results/board/npu-linear-recovered-v1-verification.json)。原始尺寸 matrix 的 m06 超时与补齐成功分别保留，不重跑已知失败表达。

本轮只证明合成 UINT8 数据下的硬件尺寸可用。m09 在 attention 与 matrix 中耗时差异较大，部分小尺寸和 B=110 的 FC 也可能慢于 CpuRef；三次无预热的诊断不能决定稳态收益。后续应使用训练权重、训练集范围进行分层量化，在验证集比较精度，并与混合 CPU 基线核对端到端耗时。

## 板端部署与控制组

前提为正常关机后完整断电再上电，SD 模式启动，三个 NPU/CMA 设备节点存在。恢复证据及原诊断包位置见[恢复记录](../results/board/npu-linear-matrix-v1-recovery.json)。

Windows 将完整归档新增到已授权的传输 U 盘根目录，安全弹出后插入 F3P USB HOST。以下全部在 Windows PuTTY 连接的板端 Linux `#` 命令行执行。先检查本次设备名：

```sh
dmesg | tail -n 20
ls -l /dev/sd*
```

确认分区为 `/dev/sda1` 后执行；名称不同时使用实际分区。脚本只读挂载 U 盘，结果保存在 SD 数据分区；任一步失败即停止：

```sh
sh <<'SH'
set -eu
ls -l /dev/sda1
mkdir -p /mnt/usb
if ! grep -q '^/dev/sda1 /mnt/usb ' /proc/mounts; then
    mount -o ro /dev/sda1 /mnt/usb
fi
printf '%s  %s\n' 9434fb95d10d934738593677deebed5666d7c09d1d657eda5b8d1ef0fe292090 /mnt/usb/f3p-npu-fc-pad-arm64-v1.tar.gz | sha256sum -c -
mkdir -p /mnt/mmcblk0p2/npu-tests
test_dir=/mnt/mmcblk0p2/npu-tests/fc-pad-v1-$(date +%Y%m%d_%H%M%S)-$$
mkdir "$test_dir"
cd "$test_dir"
gzip -dc /mnt/usb/f3p-npu-fc-pad-arm64-v1.tar.gz | tar -xf -
cd f3p-npu-fc-pad-v1
sha256sum -c checksums.sha256 > package-check.log || {
    cat package-check.log
    exit 1
}
echo 'package_check=193_items_passed'
echo "test_directory=$PWD"
sh run.sh control
sync
SH
```

首先只执行 control。核对 CpuRef 与 Alnpu 均通过、Alnpu 实际计算层为 ALHardNPU、误差不超过 1 LSB、没有不匹配及硬件错误，再进入下一组。

## 后续组与停止条件

在打印的实际 `test_directory` 中逐组执行 `sh run.sh GROUP`；每组审阅后再进入下一组，不连续自动运行：

| 顺序 | GROUP | 范围 |
|---|---|---|
| 1 | `control` | B=8、I=8、O=8，确认冷启动后的计算状态 |
| 2 | `first` | 仅 m06 补齐 FC，物理 I/O 为 16/24 |
| 3 | `attention` | 四种原始输出 21 通道的补齐 FC |
| 4 | `matrix` | 全部 24 种补齐 FC；仅在前组通过后进行 |

每个尺寸先单一 CpuRef，再单一 Alnpu，禁止 CPU 回退；进程外部限时 30 秒、TERM 宽限期 5 秒。优化拒绝 40 记录为不支持；CPU 失败、加载失败、计算失败、误差超限、信号或外部超时停止整组。出现 `CRASHED` 或硬核中断超时后停止全部 NPU 调用，保存日志并正常关机、完整断电恢复。

每个 `results_GROUP_*` 保留 `summary.tsv`、运行库哈希、每个后端的图、CSV、原始输出、profile 和中断/内存/内核快照。完整组完成不等于全部尺寸支持；必须核对逐项状态，最终量化与全模型回放另行验收。

结果持久保存在 SD 第二分区。U 盘不再使用后，在板端执行 `cd /`、`sync`、`umount /mnt/usb`，无报错后再拔下。

## 重新生成

从 `npu` 目录执行 `python tools/prepare_npu_fc_pad.py --output work/f3p-npu-fc-pad-new`，生成独立目录和 `.source.tar.gz`，已有输出拒绝覆盖。在配套 Ubuntu 中使用 [build_npu_fc_pad.sh](../probe/build_npu_fc_pad.sh) 交叉编译，完成 CPU 用例后使用 `python tools/verify_npu_linear.py --bundle <目录>` 只读复核。生成及验证不调用训练。
