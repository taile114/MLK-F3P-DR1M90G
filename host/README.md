# host — 上位机

本目录存放上位机（host computer）侧的代码与资料，按运行位置分两类：

- [`ps-local/`](ps-local/README.md) — 运行在板卡 PS（ARM Linux）上的本地上位机，经 HDMI 显示，Qt 实现。
- [`remote-pc/`](remote-pc/README.md) — 运行在与板卡以太网或 WiFi 相连的 PC 上的上位机，经 UDP 接收数据并显示。

两类上位机数据同源：板卡 PS 应用读 AXI 寄存器 / 收 DMA → 处理 → 本地显示与 / 或 UDP 发出。接口以 [`../docs/PL-PS接口约定.md`](../docs/PL-PS接口约定.md) 为准；PL 侧代码见 [`../fpga/README.md`](../fpga/README.md)，PS 系统构建见 [`../docs/SD卡启动与Linux构建指南.md`](../docs/SD卡启动与Linux构建指南.md)。

首次配对、公钥文件交换、自动发现与 PC 启动方式见 [上位机连接与密钥配置](../docs/上位机连接与密钥配置.md)。已配对设备默认自动发现受支持的局域网地址，PC 应用启动后自动监听并建立加密连接。

ps-local 与 remote-pc 共用 NPU 系统占用面板，沿用 CPU / 内存的状态、进度条及历史曲线布局；显示板端设备就绪状态、硬核 / 软核中断速率与累计次数，百分比使用率受驱动接口限制。顶栏应用连接状态上方显示核心板网络状态（千兆网 / 4G / WiFi / 未连接），两端字体与 CPU / 内存摘要一致；「网络配置」按千兆网 / 4G / WiFi 分页显示各自配置。实现方式、状态含义、测试和应用更新见 [NPU 状态显示](ps-local/README.md#npu-状态显示)，板端应用、NPU 设备及中断活动已通过串口核对，HDMI / PC 实际画面与长期运行仍待实板验收。

单主故障模型的实时链路运行在 ps-local 工作线程：本地显示唯一具体故障、所选等级与质量原因，确认后生成结构化工单并驱动声光，远端经 UDP v6/type8 显示完整故障状态。模块划分、数据流与验证状态见 [实时故障诊断实现](实时故障诊断实现.md)，接口见 [故障诊断模型接口需求](故障诊断模型接口需求.md)。

已板测模型的参数来源、四卷积硬 NPU 共享库提取、正式运行包与应用联调步骤见[故障模型与上位机接入](../docs/故障模型-上位机接入.md)。CPU 与 CPU+CNN NPU 两后端的共享库、运行包生成器与检查程序位于 [ps-local/fault-inference](ps-local/fault-inference/README.md)。AArch64 共享库、检查程序与 Qt 应用已构建；QEMU 的 CPU 后端 360 窗分类 360/360、联合 356/360，与 FP32 参考判定一致。Qt 评估与异常路径已完成开发机验证，证据见 [tests/results/fault](tests/results/fault/summary.json)。输入绑定修复库已更新至板端，NPU 两轮 360 窗均通过，见[板端证据](tests/results/fault/tensor-binding.json)；HDMI 界面操作、实采与长期共存待验收。两个诊断模式默认使用 `CPU+CNN NPU`。

## 目录

| 路径 | 内容 |
|---|---|
| [`ps-local/`](ps-local/README.md) | PS 端本地 Qt 上位机（HDMI / `/dev/fb0`），含 PL 寄存器 / DMA 读取、`plctl` CLI（`speed` 等）与采集看板（含转速面板）；接口见 [`../docs/PS本地应用接口约定.md`](../docs/PS本地应用接口约定.md) |
| [`remote-pc/`](remote-pc/README.md) | 通信电脑上位机（UDP / 以太网 / WiFi） |
| [`上位机需求与设计.md`](上位机需求与设计.md) | 需求 / 显示 / 链路 / 配置 / 接口索引 |
| [`故障诊断模型接口需求.md`](故障诊断模型接口需求.md) | 接口契约：原生块、六分支引擎 ABI、五类诊断 API 2、结构化工单及网络摘要 |
| [`故障诊断上位机改造清单.md`](故障诊断上位机改造清单.md) | 实施清单：现有差距、文件/函数修改位置、优先级、界面和部署改动 |
| [`实时故障诊断实现.md`](实时故障诊断实现.md) | 模块划分、数据流、质量原因码、API 2 所有权与时效、工单与标签、配置项与验证状态 |
| [`故障模型接入与评估模式.md`](故障模型接入与评估模式.md) | 运行包生成、共享库实现、实时诊断与数据集评估两个模式、寿命模型启动方式与验证状态 |
| [`故障模型接入待完成事项.md`](故障模型接入待完成事项.md) | 构建与离线验证结果、产物位置，以及板端 NPU 与实采剩余验收清单 |
| [`common/`](common/README.md) | 传输加密共享库（ECDH / PSK，AES-256-GCM） |
| [`flake.nix`](flake.nix) | 上位机开发环境（Python / PyQt5 / Qt5 / 抓包工具） |
| [`fetch_vendor.sh`](fetch_vendor.sh) | 从厂商资料包复制 `vendor/` |
| `vendor/` | 厂商资料（Qt 运行库 / 驱动 / 手册），未入库 |

## 厂商资料（vendor/）

`vendor/` 未入库，来源为 `../docs/MLK-F3P-DR1M90G-Resource/`，用脚本复制：

```bash
./host/fetch_vendor.sh
```

| 目标 | 来源（相对 `docs/MLK-F3P-DR1M90G-Resource/`） | 内容 |
|---|---|---|
| `vendor/qt/05-1_…QT环境搭建及demo演示.pdf` | `01_start/02_start_Linux/05_application_demo/05-1_QT环境搭建及demo演示/` | 交叉 Qt 环境搭建与 demo |
| `vendor/qt/qt_play.tar.gz` | 同上 `demo/QT/` | 预编译 Qt demo：`HelloQt` + Qt 5.13.2 运行库 + `run_arm_app.sh` |
| `vendor/qt/driver/…` | 同上 `demo/driver/` | HDMI / VDMA framebuffer 驱动源码；显示通路说明见 [`../docs/HDMI显示通路.md`](../docs/HDMI显示通路.md) |
| `vendor/udp/3_1_02_…UDP以太网通信方案.pdf` | `03_demo/3-0_book_all/` | UDP 以太网通信方案 |

## PS 端 Qt 交叉编译（`vendor/qt`）

PS 本地应用（[`ps-local/`](ps-local/README.md)）目标为 aarch64，用厂商交叉 Qt SDK 编译。SDK 不随仓库或 `docs/MLK-F3P-DR1M90G-Resource/` 提供，需按板卡系列（aarch64）另行获取后放入 `vendor/qt/`。

| 项 | 位置 |
|---|---|
| SDK 压缩包 | `vendor/qt/uisrc-lab-qt-aarch64.tar.gz` |
| SDK 解压目录 | `vendor/qt/uisrc-lab-qt-aarch64/uisrc-lab-qt/` |
| 交叉 Qt（主机工具 + 目标库） | `…/images/qt5-arm64-5.13.2/`，版本 5.13.2；`bin/qmake` 为 x86_64、`lib/` 为 aarch64 |
| SDK 路径 | `build_arm_app.sh` 直接设置 `QTDIR` / `QMAKESPEC`，不要求 `scripts/qt_cfg.sh` |
| 板端运行库 | `…/applications/qt_play/qt_lib/`，含平台插件 `platforms/libqlinuxfb.so` |
| 交叉工具链 | `../platform/flash/vendor/tools/aarch64-linux/bin`（Linaro GCC 7.5.0） |
| 交叉 OpenSSL | `vendor/openssl-aarch64/`（由 `ps-local/build_openssl_arm.sh` 生成） |

编译与部署脚本位于 [`ps-local/`](ps-local/README.md)：

| 脚本 | 作用 |
|---|---|
| `build_openssl_arm.sh` | 一次性：交叉编 OpenSSL 1.1 → `vendor/openssl-aarch64/` |
| `build_arm_app.sh` | 编 ps-local → `build-arm64/mlk_ps_local`（aarch64） |
| `deploy_arm_app.sh` | 二进制 + `qt_lib` + `run_arm_app.sh` 汇总到 rootfs overlay |
| `run_arm_app.sh` | 板端 linuxfb 启动脚本，随应用部署 |

### 常见问题与处理

| 现象 | 原因 | 处理 |
|---|---|---|
| `Could not find feature thread`，或 qmake 找不到 mkspecs / Qt 头 | SDK 内 `qmake -query QT_INSTALL_PREFIX` 硬编码 `/home/uisrc/uisrc-lab-qt`，解压到其他路径后失效 | 在 `images/qt5-arm64-5.13.2/bin/` 写 `qt.conf`：`[Paths]` 段加 `Prefix=<该目录绝对路径>`；`build_arm_app.sh` 自动写入 |
| `Project ERROR: openssl development package not found` | SDK 与工具链均不含交叉 OpenSSL，而 `.pro` 含 `PKGCONFIG += openssl` | 运行 `build_openssl_arm.sh`，并将 `PKG_CONFIG_PATH` 与 `PKG_CONFIG_LIBDIR` 指向 `vendor/openssl-aarch64/lib/pkgconfig` |
| `sh: line 1: --exists: command not found`，随后 openssl 报错 | 主机 PATH 无 pkg-config，或交叉 sysroot 未设置 | 在 PATH 中提供主机 pkg-config；`build_arm_app.sh` 检查工具并设置 sysroot，在构建目录生成已重定位的 OpenSSL `.pc` |
| `aarch64-linux-gnu-g++: No such file or directory` | 采用 buildroot 的 `output/host/bin/` 包装链，其内部软链指向厂商原机器的绝对路径 | 改用 `../platform/flash/vendor/tools/aarch64-linux/bin` |

工具链与 `qmake` 均为 x86_64 可执行文件，可在 NixOS 宿主直接运行（实测）。

## 开发环境

本目录自带 [`flake.nix`](flake.nix)，提供上位机侧工具：Python（numpy / scipy / matplotlib / pandas / pyserial）、PyQt5、Qt5，以及抓包与压测的 `tshark` / `iperf3` / `socat`。

```bash
cd host && nix develop
```

PS 端本地上位机的交叉 Qt 工具链用 `vendor/qt` 中的厂商 SDK，不在此 flake 内。

## 寿命模型接入

寿命模型在 F3P 的 PS 应用中执行，通过共享库 C ABI 回传给本地界面和 UDP v3；PC 只控制回放和显示。当前应用引擎支持融合 CPU / `ff6` 六分支混合，执行四个前馈首层样条 FC 与两个回归头样条 FC。独立模型包、六分支校验要求、运行目录与验收顺序见 [inference/README.md](ps-local/inference/README.md)和[NPU README](../npu/README.md)。六分支诊断包不能直接替代应用所需的 `libmlk_rul.so` / `model.json` 运行包。

无线联网、双端配置控制权与故障工单见 [无线联网与双端协同](../docs/无线联网与双端协同.md)。remote-pc 与 ps-local 共用主题、传感器面板、模型面板与设置组件，各端密码 / 存储独立。

声光输出与分类模型接入见[声光告警与模型故障 API](../docs/声光告警与模型故障API.md)。

单主故障模型要求的新版接口见[故障诊断模型接口需求](故障诊断模型接口需求.md)，具体改动位置见[上位机改造清单](故障诊断上位机改造清单.md)。方案新增正常、过载、欠压、过热、电机倾斜五类结果和所选分级，现有“正常/轻微/重度”仅作为旧端兼容摘要。接口层与实时链路已按清单实现，运行包与共享库见[故障模型接入与评估模式](故障模型接入与评估模式.md)。

两端四传感器加密记录、现场异常波形回放、模型标注 API 及 CSV 解密导出见 [采集记录、回放与训练接口](../docs/采集记录-回放与训练接口.md)。
