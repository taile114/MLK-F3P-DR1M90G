# F3P 本地 HDMI 与 FF6 寿命回放

本文规定使用厂商 NPU 演示启动环境、PS 本地 Qt 应用和选定 FF6 模型的 SD 验证流程。

## 1. 启动组成

| 部分 | 来源与作用 |
|---|---|
| `BOOT.bin` | 厂商 NPU 演示配套 FSBL、PL 和 U-Boot |
| `uImage.lz4` | 同一演示的 Linux 6.1.111-rt42，含 HDMI framebuffer、硬核 NPU 和 CMA 驱动 |
| `system.dtb` | 同一演示的配套设备树 |
| `boot.scr` | 从 SD 的 FAT 分区加载内核与设备树，以 `/dev/mmcblk0p2` 为 ext4 根目录，不加载演示 initramfs |
| ext4 rootfs | 从配套 NPU initramfs 保留文件、设备节点和硬链接，叠加 Qt、本地应用、中文字体及已封装 FF6 运行包 |

该验证环境提供本地显示与数据集 RUL，不包含项目 `/dev/pl_ctrl` 采集驱动。现场传感器在界面上显示不可用；模型输入是已归一化的 184 窗验证数据。

独立采集合并码流须与对应 FSBL、设备树和驱动一并验收。TD 生成 `.bit` 后还须核对建立和保持时序，存在负时序裕量的产物不能作为已验收码流。

## 2. rootfs 准备

在配套 Ubuntu 中准备 overlay，至少包括：

| 目录 | 内容 |
|---|---|
| `opt/qt_play/` | ARM64 `mlk_ps_local`、`run_arm_app.sh`、厂商 `qt_lib`、兼容 OpenSSL 1.1 库 |
| `opt/qt_play/qt_lib/lib/fonts/` | Noto Sans CJK 等支持中文的字体 |
| `opt/mlk_rul/` | 已封装的 FF6 运行包，见 [模型运行包](../../host/ps-local/inference/README.md) |
| `etc/init.d/S99qtapp` | 本仓库的本地 Qt 服务脚本，须为可执行文件、LF 换行 |
| `opt/mlk_rul-validation/` | 独立 `check_engine`、参考预测及指向运行包数据的链接，仅用于验收 |

执行以下工具生成 rootfs 归档，不访问磁盘设备：

```sh
python3 platform/flash/prepare_rul_rootfs.py \
  --vendor-initrd /absolute/vendor/uInitrd.lz4 \
  --overlay /absolute/rootfs-overlay \
  --output /absolute/new/rootfs.tar.gz
```

工具仅使用 Python 标准库和开发机已有 `liblz4`。它校验冻结演示 initramfs、配套 ArmNN、FF6 身份、模型五项 SHA-256 及 AArch64 二进制，保留 CPIO 的设备节点与硬链接，重新打开最终 tar 复核模型和运行库。无需以 root 身份提取演示 rootfs。

FAT 分区包含四个启动文件，rootfs 归档放入镜像目录的 `rootfs/rootfs.tar.gz`。`burn_sd.sh` 支持 `IMAGES_ROOT` 指定该镜像目录，并要求显式传入已识别的 SD 设备。

启动脚本从 [rul_hdmi_boot.cmd](rul_hdmi_boot.cmd)生成：

```sh
mkimage -A arm64 -O linux -T script -C none -a 0 -e 0 \
  -n 'F3P HDMI FF6 SD rootfs' \
  -d platform/flash/rul_hdmi_boot.cmd /absolute/images/boot/boot.scr
```

## 3. 上板与本地操作

1. 烧录并读回核对后卸载分区，安全取出 SD；F3P 保持断电。
2. 插卡，SW1 设置为 SD 启动 `ON-OFF-OFF`，接好 HDMI 显示器、USB HOST 鼠标和 UART/JTAG Type-C，再上电。
3. 串口为 115200、8N1、无流控；出厂登录为 `root` / `root`。
4. 本地看板自动启动。底部寿命回放面板的模型目录为 `/opt/mlk_rul`。
5. 先选择 CPU，单步检查有效 RUL 和耗时；停止后选择 CPU/NPU（FF6 六分支），再次单步，再开始连续回放。
6. 输出单位为剩余观测老化试验轮次，分类、置信度和分级不可用。每次结果返回后默认等待 1000 ms，再调度下一窗。

## 4. 板端验收

先停止 Qt 服务，以独立检查程序验收，再恢复界面：

```sh
/etc/init.d/S99qtapp stop
ls -l /dev/fb0 /dev/hard_npu /dev/cma_mem
cd /opt/mlk_rul
sha256sum -c checksums.sha256
export LD_LIBRARY_PATH=/opt/armnn_lib/lib
mkdir -p /data/mlk_f3p/validation
/opt/mlk_rul-validation/check_engine /opt/mlk_rul-validation cpu 2 \
  > /data/mlk_f3p/validation/cpu.csv \
  2> /data/mlk_f3p/validation/cpu.log
cat /proc/interrupts > /data/mlk_f3p/validation/interrupts-before.txt
/opt/mlk_rul-validation/check_engine /opt/mlk_rul-validation hybrid 2 \
  > /data/mlk_f3p/validation/ff6.csv \
  2> /data/mlk_f3p/validation/ff6.log
cat /proc/interrupts > /data/mlk_f3p/validation/interrupts-after.txt
/etc/init.d/S99qtapp start
```

CPU 每轮 184 窗，容差为 `1e-4 + 1e-4*abs(reference)`。混合模式另检查六个硬核图、实际中断、有限输出、稳态 CMA 和重复加载释放，将结果与冻结 FF6 证据比较。混合检查程序返回 0 不代替应用预测误差阈值的冻结。

最终还需在 HDMI 界面观察有效 RUL、窗口变化、实际后端、暂停、停止和重新加载。串口数值验证与物理显示确认分别记录。
