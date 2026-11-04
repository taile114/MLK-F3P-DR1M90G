# 备用 SD 卡验证 NPU 环境

本文规定独立 NPU 启动环境的准备、验证边界和上板顺序。配套 SD 系统的三个 NPU/CMA 设备及 `Alnpu` 后端可用，UINT8 最小卷积已通过硬核 NPU 实机数值与中断对照；完整寿命模型 NPU 推理尚未验证。eMMC CPU 基线保留。

## 采用的启动候选

使用资料包 `ref/F3P/01_start/02_start_Linux/05_application_demo/05-5_NPU演示/demo/boot/` 中的完整五件套，不将厂商 NPU 内核与当前项目的 PL 或设备树任意混配：

| 文件 | 内容 |
|---|---|
| `BOOT.bin` | 厂商 FSBL、PL 配置及 U-Boot |
| `boot.scr` | 从启动分区加载内核、DTB 和 initramfs 的脚本 |
| `uImage.lz4` | 包含 NPU 驱动代码的厂商 Linux 内核 |
| `system.dtb` | 含硬核/软核 NPU 和 CMA 配置的设备树 |
| `uInitrd.lz4` | 配套 Buildroot 根文件系统、Arm NN 库及示例资源 |

该环境使用厂商 NPU 演示 PL。电脑端暂存和复制到独立卡的步骤不改写 eMMC 系统；驱动初始化及 UINT8 最小卷积已通过实机验证，完整寿命模型另行验收。

## 文件检查结果

[检查摘要](../results/build/vendor-npu-boot-inspection.json)记录厂商五个文件的 SHA-256 与检查范围：

- `boot.scr`、内核和 initramfs 的 uImage 头部及载荷 CRC 均通过。
- 解压内核为 16611840 字节，版本字符串为 `6.1.111-rt42`，由 Linaro GCC 7.5 构建。
- 内核中存在硬核 NPU、软核 NPU 和 CMA 字符驱动的函数、注册名称和错误消息；嵌入式完整配置未提取到。
- 设备树包含 `hard_npu@63F00000`、`soft_npu@1f0000000` 和 128 MiB CMA，软核节点 `status="okay"`。
- initramfs 解压为 160361984 字节，包含 `/opt/armnn_lib` 与串口 getty 配置。

以上属于文件完整性和静态内容检查，不证明本板已成功启动，也不证明寿命模型已在 NPU 执行。原检查日志见 [vendor-npu-boot-inspection.log](../results/build/vendor-npu-boot-inspection.log)。

## 生成暂存目录

从 `npu` 目录使用 Python 标准库执行：

```powershell
python tools/prepare_npu_sd_boot.py
```

默认生成 `work/f3p-npu-sd-boot-v1/`，已有目录拒绝覆盖。工具校验所选厂商版本、uImage CRC 及现有诊断包，然后仅向普通工作目录复制文件；不调用磁盘分区、格式化或写卡命令。

目录结构：

```text
f3p-npu-sd-boot-v1/
  README.md
  card/
    BOOT.bin
    boot.scr
    system.dtb
    uImage.lz4
    uInitrd.lz4
    f3p-rul-probe-arm64.tar.gz
    f3p-cpu-accel-arm64-v1.tar.gz
    check_npu.sh
    manifest.json
    checksums.sha256
```

`card/` 内容约 99 MB，应放在目标卡 FAT32 启动分区的根目录；不是将整个 `card` 文件夹作为启动目录。厂商启动文件和当前已验证的诊断包保持原始字节。

## 当前启动卡状态

[写卡记录](../results/build/npu-sd-card-preparation.json)保存目标身份、备份范围和逐项文件哈希。

| 项目 | 结果 |
|---|---|
| 目标卡 | Windows 磁盘 2，USB 读卡器序列号 `121220160204`，31299993600 字节，MBR |
| 启动分区 | 分区 1，`F:` / `BOOT`，FAT32，分区大小 103809024 字节 |
| 原启动文件备份 | `work/f3p-sd-original-disk2-121220160204-v1/files/`，21 个文件、48479336 字节，原件与备份哈希全部一致 |
| 文件替换 | 经确认移除已备份的原文件，复制 10 个新文件；逐文件长度和 SHA-256 均通过，卡内 9 项清单回读通过 |
| 写入缓存 | 复制后已刷新；卡已移至板端并启动 |
| 剩余空间 | 3218432 字节 |
| 第二分区 | 31195136000 字节，准备启动卡时未修改；板端挂载为 `/mnt/mmcblk0p2`，可用约 26.7G |
| eMMC / 实机启动 | eMMC 未重刷；SD 启动和最小 u8 硬核卷积已通过，完整寿命模型 NPU 待验证 |

卡内 `manifest.json` 记录暂存生成阶段，写卡状态以本节链接的独立记录为准。备份与暂存目录属于 Git 忽略的工作产物，复制前后文件哈希保存于记录中。

## 写卡前提与物理启动

1. 将备用 microSD 经读卡器接入电脑，确认实际磁盘、盘符、容量、分区格式及需保留的数据；每次写入前重新核对身份，不能仅依据本次的磁盘号或盘符。
2. 已有合适 FAT32 分区时可在检查文件冲突后复制；需要重建分区或格式化时，先明确具体目标和清空范围。
3. 校验卡上的 `checksums.sha256`，安全弹出。
4. 在当前 eMMC 系统执行 `sync` 和 `poweroff`，关机后断开板卡电源，按项目 [SW1 模式表](../../docs/SD卡启动与Linux构建指南.md)将 eMMC 的 `OFF-OFF-OFF` 切换为 SD 的 `ON-OFF-OFF`，即 PIN1 为 ON、PIN2/PIN3 为 OFF。插卡后上电。
5. Windows PuTTY 继续使用已确认的 PS 串口 COM4、115200、8N1、无流控，观察完整启动日志。

首次登录后在板端执行：

```sh
uname -a
cat /proc/cmdline
ls -l /dev/hard_npu /dev/soft_npu /dev/cma_mem
df -h
```

三个设备可用后，再确认运行库实际注册的后端，执行厂商模型或最小量化模型的硬件分配和数值测试；成功后才评估寿命模型的量化与 NPU 分段。不能由设备节点出现或 CPU 预测通过直接判定 NPU 完成。

## 已验证的运行库入口

[实机结果](../results/board/f3p-npu-sd-boot-v1-result.json)记录内核 `6.1.111-rt42 #3`、配套启动命令行、三个字符设备，以及运行库后端 `CpuAcc,Alnpu,CpuRef,ALRef`。驱动日志包含 CMA 注册、硬核/软核 probe 与寄存器映射。

板端使用 `/opt/armnn_lib/lib` 加载 eMMC 中现有的基础层程序，单个 CpuRef Relu 的 6 个输出全部一致。该程序仍请求 CpuRef，没有执行 NPU 算子。SDK 核心库与配套 initramfs 核心库的 SHA-256 均为 `5def7ba75e4b59644be6f58deaf8b2bb3791c62ed38b52b6aea08b11a8f728ce`，见 [构建记录](../results/build/npu-smoke-v1-runtime-hashes.txt)；不能由后端名称推断 `ALRef` 的硬件属性。

## 最小量化卷积验证

新包 [f3p-npu-smoke-arm64-v1.tar.gz](../artifacts/f3p-npu-smoke-arm64-v1.tar.gz)为 61704 字节，默认使用 SD 镜像自带运行库，不需要重复部署基础库。它不使用寿命模型、训练数据或 ONNX 解析器。

| 条件 | 定义 |
|---|---|
| 原生计算图 | 单个带偏置的 3×3 Conv2d，NCHW，输入/输出 `[1,8,8,8]`，stride=1，padding=1 |
| 数值 | 输入/输出 scale=0.25，权重 scale=0.125，Signed32 偏置；确定生成非均匀整数输入和权重 |
| 类型对照 | u8：无符号激活及权重；u8_s8：无符号激活及对称有符号权重；s8：有符号激活及对称有符号权重 |
| 参考 | 独立整数卷积公式，每次 512 个输出，重复 3 次，容差 1 个量化步长 |
| 后端 | 先单一 CpuRef，再单一 Alnpu；Alnpu 不允许 CpuAcc/CpuRef 回退 |
| NPU 内存 | 厂商 CMA 分配器、Malloc 导入/导出和 cache 同步 |
| 保存证据 | 优化 DOT、profile、逐元素 CSV、动态依赖、运行库哈希、内核和中断前后快照 |
| 电脑预验证 | 配套 initramfs 核心库经 QEMU 执行，三种组合的 4608 个输出与整数参考完全一致 |

QEMU 数值通过不能模拟 NPU；最小卷积通过也不能直接推断完整 FKP-DVGT 的算子覆盖率或加速比。

将新包经 U 盘传入板端。U 盘识别与挂载是两个步骤，不能预设自动挂载已生效。当前 U 盘设备为 `/dev/sda1`，手动只读挂载到 `/mnt/usb`；每次插入后确认实际设备名。以下命令在 PuTTY 的 F3P shell 执行：

```sh
mkdir -p /mnt/usb
mount -o ro /dev/sda1 /mnt/usb
ls -lh /mnt/usb/f3p-npu-smoke-arm64-v1.tar.gz
mkdir -p /mnt/mmcblk0p2/npu-tests
cd /mnt/mmcblk0p2/npu-tests
gzip -dc /mnt/usb/f3p-npu-smoke-arm64-v1.tar.gz | tar -xf -
cd f3p-npu-smoke-v1
sha256sum -c checksums.sha256
sh run.sh inspect
sh run.sh u8
sync
```

该 SD 镜像的 BusyBox 1.35.0 不支持 `tar -z`，但包含独立的 gzip、gunzip、zcat。使用同一镜像中提取的 BusyBox，经 QEMU 验证上述解压管道与全部 27 项校验均通过，见 [工具兼容性日志](../results/build/sd-busybox-archive-tools.log)。帮助中出现 `-a` 本身不足以证明 gzip 集成已启用。冻结部署包保持不变。

文件不存在时先检查 `ls /mnt` 与 `dmesg | tail -n 20`，不继续解压。已挂载时不重复执行挂载命令；移除 U 盘前执行 `umount /mnt/usb`。`inspect` 仅核对运行库；u8 会先执行 CpuRef 对照，再执行 Alnpu，结果目录为 `results_u8_<时间>_<PID>`。CPU 对照失败时脚本不继续硬件测试。u8 类型在优化或加载阶段不支持时，可在核对日志后分别测试 `sh run.sh u8_s8` 和 `sh run.sh s8`。如超时或出现驱动错误，保留输出并检查板端状态，不持续重复调用。

硬件计算验收需结合以下证据：单一 Alnpu 优化与加载成功、图中实际计算层分配、实际 workload profile、输出数值、运行前后 NPU 中断变化。只有退出码 0、设备节点或注册成功均不足以单独确认硬件执行。完整寿命模型的量化和 NPU 分段另行验收。

## 实机量化类型与硬核执行

[u8 原始串口](../results/board/f3p-npu-smoke-u8-board-v1.log)、[u8 指标](../results/board/f3p-npu-smoke-u8-board-v1-result.json)和[三种类型摘要](../results/board/f3p-npu-smoke-board-v1-result.json)记录当前运行库的实机行为。

| 表示 | CpuRef | Alnpu | 硬核中断变化 |
|---|---|---|---:|
| QAsymmU8 激活/输出 + QAsymmU8 权重 | 3 次全部通过，误差 0 | 优化为 ALHardNPU，加载和 3 次计算通过，最大误差 1 LSB | +3 |
| QAsymmU8 激活/输出 + QSymmS8 权重 | 通过 | 优化返回 40：输入与权重类型不匹配 | 0 |
| QAsymmS8 激活/输出 + QSymmS8 权重 | 通过 | 优化返回 40：该卷积类型不支持 | 0 |

u8 模式只有 Alnpu 一个首选后端，不允许 CPU 回退；计算层明确报告 `ALHardNPU`，硬核 IRQ 从 0 增加到 3、软核 IRQ 保持 0，1536 个 NPU 输出均在 1 LSB 容差内。[回收文件复核](../results/board/f3p-npu-smoke-recovered-v1-verification.json)独立检查原始 DOT、profile、逐元素 CSV、最后一次输出字节及中断快照。实际执行事件为 `AlnpuALHardNPUWorkload_Execute_#22`，三次 wall-clock 分别为 49.05、34.20、24.12 微秒；图中唯一计算层分配给 Alnpu。运行库哈希与配套镜像一致。

本用例 CpuRef 与 Alnpu 的平均 enqueue 分别为 1513.07 / 218.94 微秒，观测耗时比约 6.91。它只比较小卷积的 3 次带 profiling 调用，不能用于推断完整模型或最优 CPU 后端的加速比。

后续沿已通过的 UINT8 表示验证模型实际层尺寸；这不是对所有有符号表示、所有算子或所有布局的全局不支持结论。[原生全连接清单](../results/build/rul-linear-shape-inventory.json)静态识别 124 次 FC、24 种输入/输出尺寸，每窗约 3501280 次乘加。直接 FC 或 1×1 Conv 映射、补齐通道、数据转换和调用开销均待实测，不能直接把整个 FP32 模型请求为 Alnpu。

## 返回结果与存储边界

两个板端结果包均已按板端清单核对 SHA-256，再复制到电脑并回读核对，见 [返回包记录](../results/board/f3p-board-return-v1.json)。解包得到 NPU 测试 66 个文件和 CPU 测试 104 个文件，原始内容保存在 [回收目录](../results/board/recovered/)，[文件清单](../results/board/recovered-files-sha256.json)记录长度与哈希。可在电脑运行 `python tools/validate_delivery.py` 复核，不需要重跑板端程序。

传输 U 盘为 aigo W381，FAT32。只读文件系统检查报告约 512 KB 丢失簇链；两个结果包的逐字节校验通过不代表整个 U 盘文件系统健康。当前保留该盘原状，不执行文件系统修复或新增写入，后续分析使用电脑副本。该 U 盘与正在启动 F3P 的 microSD 是不同设备，盘符不能作为介质身份。

候选根文件系统采用 initramfs，板端 `/root` 的新增文件可能随重启消失；卡上的归档可用于恢复。当前 eMMC 中的 CPU 混合基线另行保留。回退时断电，将 SW1 改回 `OFF-OFF-OFF` 并移除备用卡，再上电。
