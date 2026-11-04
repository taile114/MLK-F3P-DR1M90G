# 故障模型构建与交付

本文规定从仓库源码生成 AArch64 故障共享库、Qt 应用、正式运行包与独立评估集所需的文件、环境和检查步骤。命令均从仓库根目录执行，构建文件放在 Linux 文件系统中，以保留厂商库的符号链接。仓库通过 `.gitattributes` 固定 Shell 脚本和板端配置模板为 LF 换行。

## 1. 文件与依赖

| 文件或目录 | 来源与用途 |
|---|---|
| [engine/fault_engine.cpp](engine/fault_engine.cpp)、[fault_engine_api.h](../../common/fault_engine_api.h) | 共享库实现与 ABI；仅复编模型库时无需 Qt |
| [build_arm.sh](build_arm.sh)、[check](check/) | 生成 AArch64 `libmlk_fault.so`、`fault_check`；启用 Arm NN 时同时生成 `test_tensor_binding` |
| [tools/build_runtime.py](tools/build_runtime.py)、[tools/verify_runtime.py](tools/verify_runtime.py) | Python 标准库工具，转换冻结资产、生成评估集并检查封闭清单与 ELF 架构 |
| [f3p-fault-test-v2.tar.gz](../../../npu/fault-diagnosis/deploy/artifacts/f3p-fault-test-v2.tar.gz) | 仓库内冻结资产包，根目录 `f3p-fault-test-v2`；包含权重、量化参数、原始预处理参数和 360 窗特征/真值/参考判定 |
| [mlk_ps_local.pro](../mlk_ps_local.pro)、[src](../src/)、[common](../../common/)、[remote-pc/src](../../remote-pc/src/) | Qt 应用构建输入；工程引用共享组件和远端绘图/存储等实现，不能仅复制 `ps-local/src` |
| [build_arm_app.sh](../build_arm_app.sh)、[build_openssl_arm.sh](../build_openssl_arm.sh) | 厂商 Qt SDK 的应用构建入口与 ARM OpenSSL 准备入口 |
| [fault.pro](../../tests/fault.pro)、[test_fault.cpp](../../tests/test_fault.cpp)、[windows.pro](../../tests/windows.pro)、[rul.pro](../../tests/rul.pro) | 开发机 Qt 集成测试；工程逐层 include，共同参与构建 |
| [fixtures/fault-preprocess](../../tests/fixtures/fault-preprocess/) | 两例四秒原生块 JSON 与 27683 个 float32 参考；Qt 测试时须与测试源码一起复制 |
| [delivery/build_update.py](delivery/build_update.py)、[install_update.sh](delivery/install_update.sh) | 应用和类别屏蔽库的联合更新归档生成及安装入口；校验并复用既有训练资产 |
| [stage_fault.sh](../../../platform/flash/stage_fault.sh)、[mlk-inference](../../../platform/flash/rootfs-overlay/etc/default/mlk-inference)、[run_arm_app.sh](../run_arm_app.sh) | 运行包暂存、默认路径配置和板端 Qt 启动 |

不重新训练即可复编和评估。上述流程不需要 CUDA、PyTorch、ONNX、原始波形数据集或模型训练目录的 `requirements*.txt`。只有重新训练或重新生成离线资产时才进入训练侧环境。

以下厂商开发文件需另外准备，`host/vendor/` 和 `platform/flash/vendor/` 不纳入 Git：

| 依赖 | 必需内容与已验证配置 |
|---|---|
| Linux 主机构建工具 | Bash、GNU make、C/C++ 编译器、awk、tar、gzip、file、pkg-config；资产工具使用 Python 3.10 及以上，已用 Python 3.12.14 |
| AArch64 Linux 工具链 | Linaro GCC 7.5-2019.12；`bin/aarch64-linux-gnu-gcc`、`g++`、`readelf` 及配套 sysroot，来自厂商 `uisrc-lab-anlogic` 开发环境 |
| 厂商 Arm NN SDK | 32.1.0，须同时提供 `include/armnn/ArmNN.hpp`、`include/armnn/Logging.hpp`、`include/armnnALUtils/cma_mem_init.hpp` 及 `lib/libarmnn.so*`。来源为厂商资料 `05_application_demo/05-5_NPU演示/demo/sdk/face_detection.tar.gz` 中的 `app/face_detection/libs/armnn_lib/` |
| 厂商交叉 Qt SDK | 5.13.2，`QT_SDK_ROOT/images/qt5-arm64-5.13.2/` 内需有 `bin/qmake`、`moc`、`rcc`、`uic`、`mkspecs/linux-aarch64-gnu-g++`、头文件和 ARM 库；应用使用 Core、Gui、Widgets、Network |
| ARM OpenSSL | 1.1.1q 的头文件、`libcrypto.so.1.1`、`libssl.so.1.1` 和 `lib/pkgconfig/*.pc`；可由厂商 Buildroot 的 `dl/libopenssl/openssl-1.1.1q.tar.gz` 生成 |
| 开发机 Qt（可选） | 原生 Qt 5，另含 TestLib、OpenSSL 开发库；用于 CPU 数值与 Qt 集成测试，不能用 ARM qmake 生成主机测试 |
| QEMU（可选） | `qemu-aarch64` / `qemu-aarch64-static`，已用 6.2；须配套目标 sysroot、Arm NN 和目标 C++ 运行库；只检查无硬件路径 |

配套 `libarmnn.so.32.1` 的 SHA-256 为
`5def7ba75e4b59644be6f58deaf8b2bb3791c62ed38b52b6aea08b11a8f728ce`。
`libarmnn.so -> libarmnn.so.32 -> libarmnn.so.32.1` 的链接须保留。
仓库中的 `npu/artifacts/f3p-rul-probe-arm64.tar.gz` 只携带部署运行库，不含编译所需的 Arm NN 头文件；不能用该归档代替完整 SDK。

## 2. 构建故障共享库

以下路径对应已验证 Ubuntu 环境；在其他目录部署 SDK 时替换相应变量。`FAULT_BUILD_ROOT` 选择新的输出目录，避免混入桌面 DLL 或其他后端的产物。

```sh
export FAULT_REPO_ROOT="$PWD"
export FAULT_BUILD_ROOT="$PWD/../out/mlk-fault-rebuild"
export CXX=/home/uisrc/uisrc-lab-anlogic/tools/aarch64-linux/bin/aarch64-linux-gnu-g++
export ARMNN_ROOT=/home/uisrc/f3p-rul-probe/vendor/armnn
mkdir -p "$FAULT_BUILD_ROOT"

"$CXX" --version
"$CXX" -dumpmachine
test -f "$ARMNN_ROOT/include/armnn/ArmNN.hpp"
test -f "$ARMNN_ROOT/include/armnn/Logging.hpp"
test -f "$ARMNN_ROOT/include/armnnALUtils/cma_mem_init.hpp"
printf '%s  %s\n' \
  5def7ba75e4b59644be6f58deaf8b2bb3791c62ed38b52b6aea08b11a8f728ce \
  "$ARMNN_ROOT/lib/libarmnn.so.32.1" | sha256sum -c -

OUT="$FAULT_BUILD_ROOT/build-aarch64" \
  sh host/ps-local/fault-inference/build_arm.sh
"${CXX%g++}readelf" -hW "$FAULT_BUILD_ROOT/build-aarch64/libmlk_fault.so"
"${CXX%g++}readelf" -dW "$FAULT_BUILD_ROOT/build-aarch64/libmlk_fault.so"
```

没有已解压的 Arm NN SDK 时，可先从厂商 NPU 归档提取并使用对应目录，再执行上述构建：

```sh
mkdir -p "$FAULT_BUILD_ROOT/vendor-npu"
tar -xzf /path/to/face_detection.tar.gz -C "$FAULT_BUILD_ROOT/vendor-npu" \
  app/face_detection/libs/armnn_lib
export ARMNN_ROOT="$FAULT_BUILD_ROOT/vendor-npu/app/face_detection/libs/armnn_lib"
```

构建入口检查目标为 AArch64 Linux，使用 C++17、`-ffp-contract=off` 和 `-Wl,-z,defs`。正式库须为 ELF64 little-endian AArch64，共享库依赖 `libarmnn.so.32`；检查程序的解释器为 `/lib/ld-linux-aarch64.so.1`。不设置 `ARMNN_ROOT` 时只生成 CPU 后端，运行包必须相应声明 `--backends cpu`。

## 3. 生成运行包与评估集

```sh
printf '%s  %s\n' \
  d806f5f4670e059e536d19662afe9f85ed10684e203bf4ca02c14fbfa49594df \
  npu/fault-diagnosis/deploy/artifacts/f3p-fault-test-v2.tar.gz | sha256sum -c -
mkdir -p "$FAULT_BUILD_ROOT/offline"
tar -xzf npu/fault-diagnosis/deploy/artifacts/f3p-fault-test-v2.tar.gz \
  -C "$FAULT_BUILD_ROOT/offline"
python3 host/ps-local/fault-inference/tools/build_runtime.py \
  --offline "$FAULT_BUILD_ROOT/offline/f3p-fault-test-v2" \
  --output "$FAULT_BUILD_ROOT/runtime" \
  --library "$FAULT_BUILD_ROOT/build-aarch64/libmlk_fault.so" \
  --model-id f3p-fault-single-v1 --backends cpu,cpu_cnn_npu \
  --eval-output "$FAULT_BUILD_ROOT/evalset"
python3 host/ps-local/fault-inference/tools/verify_runtime.py \
  "$FAULT_BUILD_ROOT/runtime" --aarch64
```

`runtime/` 恰有七个文件：`model.json` 和其 `files` 字段封闭的六项资产 `libmlk_fault.so`、`preprocess.json`、`weights.bin`、`weights.tsv`、`thresholds.bin`、`qparams.tsv`。`evalset/` 包含 `evalset.json`、`test_inputs.bin`、`test_index.csv`；360 窗输入占 `360 × 27683 × 4 = 39863520` 字节。

离线包的 `preprocess.json` 须经生成器转换，不能直接复制为正式运行包。库每次重编后重新生成 `model.json`，不能沿用其他二进制的哈希。`--allow-missing-library` 只适用于准备资产，产物不可部署为正式包。清单生成不证明 NPU 后端已执行，仍需板端检查。

## 4. 构建 Qt 应用

已有 ARM OpenSSL sysroot 时跳过第一段。生成 OpenSSL 的入口会重新建立其工作和输出目录，应为本次构建指定专用路径。

```sh
export QT_TOOLCHAIN_BIN=/home/uisrc/uisrc-lab-anlogic/tools/aarch64-linux/bin
export QT_SDK_ROOT=/home/uisrc/mlk-host-ff6/qt-sdk
export QT_OPENSSL_SYSROOT=/home/uisrc/mlk-host-ff6/repo/host/vendor/openssl-aarch64

# 仅在没有 ARM OpenSSL 时执行；OPENSSL_TAR 指向已准备的厂商源码归档。
# OPENSSL_TAR=/path/to/openssl-1.1.1q.tar.gz \
# OPENSSL_OUT="$FAULT_BUILD_ROOT/openssl-aarch64" \
# OPENSSL_WORK="$FAULT_BUILD_ROOT/openssl-work" \
#   bash host/ps-local/build_openssl_arm.sh
# export QT_OPENSSL_SYSROOT="$FAULT_BUILD_ROOT/openssl-aarch64"

QT_BUILD_DIR="$FAULT_BUILD_ROOT/qt-arm64" bash host/ps-local/build_arm_app.sh
```

产物为 `$FAULT_BUILD_ROOT/qt-arm64/mlk_ps_local`。脚本使用交叉 SDK 的 qmake，重定位 OpenSSL 的 pkg-config 前缀并清除构建主机的 Qt 路径变量；主机 `pkg-config` 必须在 PATH。SDK 的 `bin/qt.conf` 不存在时自动创建，已有文件须指向该 SDK 的实际 `qt5-arm64-5.13.2` 目录。SDK 工具本身的主机依赖也须可加载。

完整 rootfs 暂存另用 [stage_ps_local.sh](../../../platform/flash/stage_ps_local.sh)，它调用 [build_arm.sh](../build_arm.sh) 而非上述应用入口，并同时需要匹配内核构建树、`pl_ctrl.ko`、`plctl`、采集工具、PL 码流及 Qt 运行目录。其变量为 `QT_CROSS_ROOT=$QT_SDK_ROOT/images/qt5-arm64-5.13.2`、`CROSS_BIN=$QT_TOOLCHAIN_BIN`、`QT_OPENSSL_SYSROOT`；两种应用构建入口的输出目录不同，不得混用。完整镜像要求见 [platform/flash](../../../platform/flash/README.md)。单独修复模型库不要求重编 Qt、内核或码流。

## 5. 无硬件检查

`test_tensor_binding` 使用生产输入元数据和配套 Arm NN 实现，复现非 constant 输入被 `ConstTensor` 拒绝的异常，再检查四层输入和可写输出。它不创建 NPU 图，也不打开 CMA。已验证的 QEMU 调用为：

```sh
export FAULT_QEMU=/home/uisrc/f3p-qemu-6.2/usr/bin/qemu-aarch64-static
export FAULT_SYSROOT=/home/uisrc/uisrc-lab-anlogic/tools/aarch64-linux/aarch64-linux-gnu/libc
export FAULT_TARGET_LIBS="$ARMNN_ROOT/lib:$FAULT_SYSROOT/lib:$FAULT_SYSROOT/usr/lib:/home/uisrc/uisrc-lab-anlogic/tools/aarch64-linux/aarch64-linux-gnu/lib64"
"$FAULT_QEMU" -L "$FAULT_SYSROOT" -E LD_LIBRARY_PATH="$FAULT_TARGET_LIBS" \
  "$FAULT_BUILD_ROOT/build-aarch64/test_tensor_binding"
"$FAULT_QEMU" -L "$FAULT_SYSROOT" -E LD_LIBRARY_PATH="$FAULT_TARGET_LIBS" \
  "$FAULT_BUILD_ROOT/build-aarch64/fault_check" "$FAULT_BUILD_ROOT/runtime" \
  --backend cpu --data "$FAULT_BUILD_ROOT/evalset" --limit 3 \
  --report "$FAULT_BUILD_ROOT/qemu-cpu-smoke.json"
```

四层输入仍为每窗重新提交的 `InputLayer`，只读 `ConstTensor` 的元数据设置 `IsConstant=true`，输出为 false。QEMU 中不执行 NPU 检查，耗时不作为实板性能。

`mlk_fault_predict_masked` 是 ABI 1 的兼容扩展，使用原模型输出完成类别排除，不改变四层 NPU 图或训练权重。
实时诊断需匹配的应用与扩展库；数据集评估仍调用 `mlk_fault_predict`，按原始五类判定验证。

Qt 开发机测试需另编原生 CPU `.so`，按第 3 节为它生成独立 `runtime-native/`，并传入 `--backends cpu`；不可把 ARM 库加载到 x86-64 测试进程。原生库构建示例：

```sh
mkdir -p "$FAULT_BUILD_ROOT/build-native"
g++ -O3 -std=c++17 -ffp-contract=off -fPIC -shared -pthread \
  host/ps-local/fault-inference/engine/fault_engine.cpp \
  -o "$FAULT_BUILD_ROOT/build-native/libmlk_fault.so"
```

测试的原生 qmake / make 命令与环境变量见 [host/tests](../../tests/README.md#单主故障模型与评估)。不得把 CPU 声明的测试包用于 NPU 正式部署。

Qt 六分支对照直接读取冻结 fixture，不需要训练环境。仅重新生成 fixture 时才使用训练环境运行
`host/tests/generate_fault_preprocess_fixture.py`，正常样例默认生成，过热样例增加 `--class-index 3`。
该过程只读取原始样例与冻结 checkpoint，不拟合标准化、不训练或保存新权重。

## 6. 交付与板端验收

| 交付物 | 板端位置与要求 |
|---|---|
| 正式 `runtime/` 全部七项文件 | `/opt/mlk_fault` 指向版本目录；保留 `/opt/mlk_fault.previous` |
| 独立 `evalset/` 三项文件 | `/data/fault-eval`；测试标签和参考判定不进入正式运行包 |
| `build-aarch64/fault_check` | `/usr/bin/fault_check`，执行权限 755 |
| `qt-arm64/mlk_ps_local` | `/opt/qt_play/mlk_ps_local`，执行权限 755；只更换库时可保留应用 |
| `run_arm_app.sh`、`mlk-inference` | `/opt/qt_play/run_arm_app.sh`、`/etc/default/mlk-inference`；后者记录路径和模式意图，不自动运行模型 |
| 板端既有环境 | Qt 5.13.2、linuxfb/evdev 插件、CJK 字体、OpenSSL 1.1、配套 Arm NN 与目标 C/C++ 运行库；NPU、CMA、HDMI 的内核/DTB/码流须匹配 |

`stage_fault.sh` 校验正式 ARM 包并写入 rootfs overlay，不直接修改在线开发板；可通过 `ROOTFS_OVERLAY` 指定独立暂存目录：

```sh
MLK_FAULT_ROOT="$FAULT_BUILD_ROOT/runtime" \
ARMNN_RUNTIME_ROOT="$ARMNN_ROOT" \
ROOTFS_OVERLAY="$FAULT_BUILD_ROOT/rootfs-overlay" \
  bash platform/flash/stage_fault.sh
```

该入口不部署评估集、检查程序或 Qt 应用，它们按上表另行暂存。重建镜像按 [rootfs 构建规程](../../../platform/flash/README.md)进行；在线更新须先停止诊断/评估/RUL 并等待释放，再停止 `S99qtapp`、备份现用版本、校验新包和切换链接，完成后 `sync` 并恢复服务。用户态更新不要求格式化 SD 卡。

板端要求 `/dev/hard_npu`、`/dev/soft_npu`、`/dev/cma_mem` 可用；HDMI 另需 `/dev/fb0`，实采另需匹配的 `/dev/pl_ctrl`。在独占 NPU 的情况下先做三窗，再跑完整 360 窗并重复一次，保存报告、CSV、四图硬件归属、IRQ 与释放前后 CMA。命令见 [最小验收顺序](../../故障模型接入待完成事项.md#3-板端最小验收顺序)。板端精简环境没有系统 `timeout`，不能直接套用依赖该命令的主机脚本。

库与运行包哈希、两轮板端结果和证据范围见 [tensor-binding.json](../../tests/results/fault/tensor-binding.json)。CPU 全量/Qt 离线证据见 [summary.json](../../tests/results/fault/summary.json)；每项结果只对应其登记的二进制，重编产物须重新验收。冻结特征回放不替代原始实采预处理、界面操作和长期共存验证。
