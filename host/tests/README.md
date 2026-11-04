# 寿命模型接入测试

本文规定 Linux 开发机上的协议、状态、记录与窗口测试。Qt 需提供 Widgets、Network、TestLib，并配套 OpenSSL 开发库与 pkg-config。窗口测试在无 `/dev/pl_ctrl` 的开发机执行；不用于替代实板共存验收。

## 单主故障模型与评估

源码与厂商依赖文件清单、ARM/Qt 构建及运行包生成命令见
[故障模型构建与交付](../ps-local/fault-inference/BUILD.md)。

使用 `fault-inference` 的 CPU 构建和由正式离线包生成的运行包、360 窗评估集：

```sh
mkdir -p /tmp/mlk-fault-tests
cd /tmp/mlk-fault-tests
qmake /absolute/repo/host/tests/fault.pro
make -j4
MLK_TEST_FAULT_ROOT=/path/to/cpu-runtime \
MLK_TEST_FAULT_EVAL=/path/to/evalset MLK_TEST_OUTPUT="$PWD" \
QT_QPA_PLATFORM=offscreen ./test_fault
```

`test_fault` 使用真实共享库与冻结输入，核对分类 360/360、联合 356/360、报告导出、
取消后重启、缺失/不封闭资产、模型身份不符、错误等级、外部结果乱序/冲突/重复/过期、
评估切换实时诊断，以及评估期间拒绝启动寿命回放。采用生产主题检查 1920×1080 布局，
输出面板和完整窗口截图。评估页可纵向滚动，完整矩阵不增加主窗口最小高度。
证据见 [results/fault/summary.json](results/fault/summary.json)。

实时输入回归另覆盖 PL 计数回绕、轮询抖动、UTC 跳变、转速累计计数与单观测区分、
DMA 延迟轮询时按序取回保留块、覆写后的安全起点、序号回绕/重启和快照独立性，
以及存在少量真实缺块时的推断、TF 覆盖不足时的拒绝和诊断状态栏同步，
时间倒退后的窗口恢复，以及合成四通道原生流进入真实 CPU 模型、自动加密记录和
跨两段记录的模型窗口标签回看。记录策略检查正常分段清理、故障前后上下文、
质量不足、传感器告警和人工异常标签保护；模型告警检查二级状态位及三窗正常恢复清除。
范围与应用哈希见
[live-input.json](results/fault/live-input.json)，截图为模拟数据，不代表现场传感器验收。

`fixtures/fault-preprocess/` 包含两例从正常和过热原生记录量化到线上协议后的四秒输入，以及
训练侧 `NativeBlockFeatures.extract` / `FeatureNormalizer.transform` 冻结的六分支参考。
Qt 逐项检查全部 27683 个 float，并核对 23.6、35.0、35.1、41、46℃ 下的原模型类别和温度屏蔽结果，
另检查过载、欠压类别及等级不受影响。生成器 [generate_fault_preprocess_fixture.py](generate_fault_preprocess_fixture.py)
仅在重新冻结样例时需要训练环境；常规 Qt 回归直接读取这些文件，无需 PyTorch 或原始记录。
模型原始冻结评估与在线屏蔽策略分别验证，不用屏蔽后的指标替代原模型精度。

工单回归检查四条冷解密期间采集节拍仍可运行、单批次限流、重复重试、同长度密文篡改、错误口令、目录切换时旧批次失效及 ACK 清单更新。日志回归检查 MiB 剩余空间门槛下记录可读；窗口集成检查未确认低分候选不触发声光或工单。置信度回归覆盖 0.740415、0.683509、0.672537、0.799999 与 0.80 边界、低分打断连续确认及撤销已确认告警，重复窗口不能改变分数。

## 四传感器记录与训练导出

```sh
mkdir -p /tmp/mlk-capture-tests
cd /tmp/mlk-capture-tests
qmake /absolute/repo/host/tests/capture.pro
make -j4
./test_capture
```

覆盖温度、电流、转速、振动 XYZ 的原生样本保存，MLKC / MLKL 加密往返、错误口令与头 / 密文篡改、七字段 CSV、振动合成、前值保持与缺失 / 过期有效位、标签提前发布与更新、保存失败保留缓冲、保留策略及 v5 / 兼容波形时间转换。

`windows.pro` 的 `fourSensorCaptureAndFaultReplay` 使用本机 UDP 19500 / 19501，验证双端独立口令、300 点 XYZ 分包后的逐样本时间与数值一致性、四传感器完整存储、配置定时同步期间的周期分段提交、事件标签、工单关联、波形 / 数据二级窗口、无关联记录提示与本地标签 API。回放前后实时面板缓冲保持一致。指定 `MLK_TEST_OUTPUT` 后输出 `capture-pc.png` / `capture-ps.png`。这些输入仅为测试数据，不代表实采验收。

`captureReplayRequiresActivation` 使用临时加密记录，检查打开 / 筛选不自动加载，点击与回车确认后切换，悬停 / 滚轮 / 方向浏览不重建数据，以及目录变动 / 清单刷新保留波形、播放位置和滚动位置；重复点击当前记录复用已加载数据。双端窗口用例分别验证点击加载和工单直接打开。

字节格式、可执行 CSV 解密命令、模型标注字段和板端步骤见 [采集记录、回放与训练接口](../../docs/采集记录-回放与训练接口.md)。

## 接口与状态

从仓库根执行，构建目录可替换为其他独立目录：

```sh
mkdir -p /tmp/mlk-rul-tests
cd /tmp/mlk-rul-tests
c++ -std=c++17 -shared -fPIC -I/absolute/repo/host/common \
    /absolute/repo/host/tests/fake_engine.cpp -o libfake_rul.so
qmake /absolute/repo/host/tests/rul.pro
make -j4
MLK_TEST_ENGINE="$PWD/libfake_rul.so" QT_QPA_PLATFORM=offscreen ./test_rul
```

测试替身只用于运行时装载、校验失败、停止 / 定位与状态切换，不生成真实模型预测，也不进入应用运行包。数值核对使用 [应用检查程序](../ps-local/inference/check_engine.cpp) 和冻结权重。

覆盖 v3 浮点值与单位、截断 / 非法帧、v2 兼容、未知分类处理、加密记录与认证失败、模型缺失恢复、加载中停止、重复加载 / 定位，以及安全模式拒绝明文控制与重放。

`journalRetentionAndLargeDirectory` 建立 10000 个临时寿命文件，验证连续写入后的文件上限、按修改时间清理最旧记录、按 UTC 日历月到期及目录切换后重建索引，并输出首次索引与连续 25 次保存耗时。文件与数据均为测试夹具。

`defaultNpuAndReportedBackend` 检查两端面板默认请求后端 2、实际后端同步、CPU 回退显示及未提交手动选择在心跳期间的保持。

## 时间同步帧

[`../common/timesync.h`](../common/timesync.h) 只做 `TIME_SYNC` 帧编解码与偏移计算，仅依赖 QtCore，不调用系统接口、不修改系统时钟。

```sh
mkdir -p /tmp/mlk-timesync
cd /tmp/mlk-timesync
c++ -std=c++17 -O2 -fPIC -Wall -Wextra -Werror -I/absolute/repo/host/common \
    $(pkg-config --cflags Qt5Core) /absolute/repo/host/tests/test_timesync.cpp \
    -o test_timesync $(pkg-config --libs Qt5Core)
./test_timesync
```

Qt 以 `-reduce-relocations` 构建时必须加 `-fPIC`（不能用 `-fPIE`）。也可用 `qmake /absolute/repo/host/tests/timesync.pro && make -j4` 构建。通过时输出 `PASS=n FAIL=0` 与 `all n checks passed`，失败返回非零。

覆盖 10 字节往返、字段偏移与小端字节序、长度错误（空帧与 9 / 11 字节）、版本错误、`timeSyncOffsetMs` 的正负偏移，以及 `uncertainty_ms` 不参与偏移符号。

## 双端窗口

```sh
mkdir -p /tmp/mlk-window-tests
cd /tmp/mlk-window-tests
qmake /absolute/repo/host/tests/windows.pro
make -j4
QT_QPA_PLATFORM=offscreen MLK_TEST_OUTPUT="$PWD" ./test_windows
```

窗口测试使用临时配置目录和本机 UDP 18000 / 18001、19400 / 19401 端口，验证加密查询、模型缺失与停止确认、采集上报、双端独立记录口令、工单保存确认、共享控制权与 remote-pc 覆盖规则，并检查 1920×1080 下标题及窗口布局。WiFi 测试使用临时 Unix socket 模拟 wpa_supplicant，验证扫描、SSID、PSK 派生、认证 / IP 成功后保存，确认控制命令不携带原始密码。`MLK_TEST_FONT` 可指定开发机字体文件，仅用于截图；未设置时使用系统字体。

`communicationTimingAndReceiveStyle` 使用本机 UDP 19840 / 19841，验证应用往返只接受匹配 `request` 的回复、回复延时计入单调时钟耗时、两秒前的波形时间戳不污染往返指标，并检查连续 200 包接收不重复设置连接样式且原始样本全部进入缓冲。

系统状态回归：`ps-local/tests/npu_monitor.pro` 验证 NPU 解析、64 位累计计数的 JSON 往返、非法字段和板端网络路由选择。`windows.pro` 的 `localNpuSidebar` 检查状态 / 使用率条 / 双曲线与顶栏两行布局；`actualWindowsExchangeControl` 覆盖板端到 PC 的加密 NPU / 网络遥测、状态超时清除、重连及缺失扩展字段，输出 `local-npu-sidebar.png` 与 `remote-npu-sidebar.png`。测试状态不代表实板读数。

## 虚拟键盘与弹窗输入

```sh
mkdir -p /tmp/mlk-keyboard-tests
cd /tmp/mlk-keyboard-tests
qmake /absolute/repo/host/tests/keyboard.pro
make -j4
QT_QPA_PLATFORM=offscreen ./test_keyboard
# vnc 使用与 linuxfb 相同的 fbconvenience 合成路径；无需连接 VNC 客户端。
QT_QPA_PLATFORM=vnc:size=1920x1080:port=0 ./test_keyboard
```

测试通过 QWindow 鼠标事件验证 WindowModal / ApplicationModal 密码输入、OK / Cancel、收起后焦点恢复、数字校验框、光标位置、输入目标切换、Shift / Caps、输入窗口销毁和键盘工具窗取得焦点时的输入目标保持。拖动测试检查长按、密集移动、松开位置、输入目标保持以及收起后取消移动；vnc 模式逐像素检查快速拖动后旧位置的背景恢复，offscreen 跳过该像素检查。

测试不启用采集记录，可独立检查输入链路；板端 linuxfb 与实际鼠标操作仍需现场验证。Qt 5.13.2 的 ARM 测试可使用厂商 qmake 与同目录 QtTest 库交叉构建后通过 QEMU offscreen / vnc 运行。vnc 仅用于测试，不改变板端 linuxfb 启动配置。

## 数值证据与边界

[results/rul/summary.json](results/rul/summary.json) 记录已测试共享库 SHA-256、验证数据范围、硬核中断和 CMA 观测。配套 CSV 保存 QEMU 与实板各模式的两轮结果。CPU 最大参考差约 `8.11e-6`；混合最大参考差约 `0.03055`，应用允许精度退化阈值为 TBD。

当前证据不包含实板 HDMI / 采集 / NPU 共存、长期稳定性或实采泛化验收。系统 `CmaFree` 观测差为 32 KiB，原因与长期行为待核对，不能据此声明资源完全无泄漏。检查程序另提供初始化、稳态与释放后的分配计数，新增诊断读数需在后续实板验收中回收。

## FF6 应用共享库与本地 HDMI

[FF6 验证摘要](results/ff6/summary.json)记录当前六分支引擎哈希、QEMU 和板端结果。板端 CPU / FF6 各两轮、每轮 184 窗；CPU 最大参考差约 `8.11e-6`，FF6 最大 FP32 预测差约 `0.048573` 轮，与冻结 FF6 独立程序逐窗结果一致。FF6 均值约 `43.386 ms/窗`，稳态 CMA 为 `233472` 字节，每轮释放后计数为 0；硬核中断增加 `18522`，软核为 0。

本地 HDMI 为 1920×1080，生产运行包通过 Qt 加载、预热与连续回放，已观察到有效 RUL 和窗口编号更新。验证卡使用厂商配套 NPU / HDMI 启动环境，项目 PL 采集共存、长期稳定性和独立测试集验收保持各自范围。详见 [SD 启动与验收](../../platform/flash/RUL-HDMI启动与验收.md)。

双端协同、无线构建与验收规程见 [无线联网与双端协同](../../docs/无线联网与双端协同.md)。开发机模拟服务不代表无线实板联网验收通过。

## 已配对设备自动发现

使用 `qmake /absolute/repo/host/tests/discovery.pro && make -j4 && ./test_discovery` 构建与执行。测试使用独立回环地址和 UDP 19801–19803，覆盖地址变化、PSK 错误、ECDH 握手前的身份发现、篡改、响应重放、挑战过期与源地址绑定。窗口测试另覆盖身份生成、配置保存与应用启动自动监听。实板使用两端无效缓存 IP，验证取得真实地址后重新建立加密应用会话。


## WiFi 实板通信

[`hardware_probe.cpp`](hardware_probe.cpp)是显式启用的实板测试端，绑定 PC WiFi 地址的 UDP 8000；板端需配置同一地址 / 端口、对应密钥及默认 ps-local 优先。不得与其他 PC 监听实例同时运行。密钥文件权限应为 `0600`，字段为 `host`、`board`、`psk`、`selfPub`、`selfPriv`、`peerPub`、`mode`；密钥均为十六进制字符串，`mode` 为 1（PSK）或 2（ECDH）。文件不进入版本库。

```sh
mkdir -p /tmp/mlk-hardware-tests
cd /tmp/mlk-hardware-tests
qmake /absolute/repo/host/tests/hardware_probe.pro
make -j4
./hardware_probe /private/link.json 2 180
```

测试输出数据报 / 字节数、各类型帧与四通道采样计数、已认证流的序号缺口、解密 / 格式错误和异常包响应数。ECDH 先验证错误固定公钥，再使用正确身份建立会话；错误公钥阶段产生的握手前密文单独统计。网络序号缺口不等于 PL DMA 丢块计数，短时结果不替代长期采集验收。

实际 PC 窗口测试使用相同密钥文件，核对连接、四通道缓冲、故障时间与本机加密记录的正确 / 错误口令行为：

```sh
QT_QPA_PLATFORM=offscreen MLK_HW_CONFIG=/private/link.json \
  MLK_TEST_OUTPUT=/tmp/mlk-window-tests ./test_windows hardwareRemoteWindow
```

未提供 `MLK_HW_CONFIG` 时该用例跳过。实板通信摘要见 [`results/wireless/summary.json`](results/wireless/summary.json)。测试工单须明确标识为通信测试数据，验收后移出活动工单目录。

告警验证：`windows.pro` 的 `physicalAlarmPolicyAndModelApi` 覆盖物理单位、XYZ 任一轴越界、边界/禁用/失效样本、模型 Socket 上报、非法字段、工单 ID 冲突、未分级检测和模型心跳超时。PL 的 `tb_alarm_sequencer` 与 `tb_axi_lite_reg` 验证时长、抢占、重触发、WSTRB、读写背压与复位。模型 C 客户端和人工验证步骤见[声光告警与模型故障 API](../../docs/声光告警与模型故障API.md)。

板端时序测试程序为 `test_alarm_hardware.cpp`，使用板端 C++ 工具链、`-pthread -Ihost/ps-local/src` 与 `host/ps-local/src/pl_host.cpp` 一起编译。停止 ps-local 后执行 `test_alarm_hardware --run`，测试会暂时接管 U7，测量短响/长响时长、抢占和心跳保护，并在退出时清输出。该程序不属于无硬件回归，实板运行结果为待定。

## 电流标定与转速范围

`capture.pro` 覆盖 ACS712-20A / ADS1115 的零点、±1 A、5 A 换算，转速安装方向与 3277 / 12000 rpm、signed32 边界，UDP v6 编解码、v5 兼容、采集载荷 v2 加密往返和 v1 载荷读取。`windows.pro` 的 `actualWindowsExchangeControl` 通过数据源信号验证 4000 / 7000 / 12000 rpm、反转、停机在本地 / PC 的数值与波形一致，并核对 5 A 的板端到 PC 传输。

`test_sensor_source` 使用普通临时文件替代 `/dev/pl_ctrl`，通过链接器 open 包装拒绝 `/dev/mem`，不访问硬件。覆盖启动标定写入 / 回读、PL 系数清零后的恢复、归零保留增益、ADC 饱和、500 ms 采样停更、未标定停止发帧，以及 QSettings / 共享配置的保存与校验。

```sh
mkdir -p /tmp/mlk-sensor-source-tests
cd /tmp/mlk-sensor-source-tests
qmake /absolute/repo/host/tests/sensor_source.pro
make -j4
./test_sensor_source
```

这些测试使用合成输入，不替代实板电流参考值、转向与高速验收。实板 ADXL345 振动无数据时，先连续读取 `plctl sensors` 的 acc_count 与 `plctl read 0x3c` 的故障位；识别成功而计数不变时需检查 INT1 数据就绪信号（J12.7 / T5）、供电及初始化状态，不能将无新样本解释为电机未运行。

## 配置权限、记录删除与时间显示

`windows.pro` 的 `settingsSaveWithoutTransportCredentials` 验证 ECDH 未填对端公钥、IP 留空时仍可保存本地设置；`sharedAuthorityAndConflict` 验证 ps-local 优先拒绝远端写入、remote-pc 优先接受离线后的旧版本快照并覆盖、相同请求幂等且版本只递增一次。`replayWindowTimeFollowsModelState` 验证模型窗口时间在暂停 / 停止时定格、恢复运行后更新；`boardWallClockRunsWhileModelPaused` 验证标题下世界时钟不随模型暂停停表；`boardTimeNetworkDetection` 用 `MLK_TIME_STATUS_BIN` 指定外部命令，验证网络时间源检测与手动设置禁用条件。

`capture.pro` 的 `recordDeletionIsScopedToRecords` 验证按记录文件名删除、拒绝标签库与目录外路径。

记录管理、手动设置时间与重启入口需配置密码；主机测试不执行真实 `reboot`，实板行为待验收。
