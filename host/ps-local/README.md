# PS 本地实时采集看板

本目录实现 ARM Linux / HDMI 上的 Qt 数值看板与 `/dev/pl_ctrl` 用户态封装，接口以 [`../../docs/PL-PS接口约定.md`](../../docs/PL-PS接口约定.md) 为准。

单主故障模型已有原生块入口、预处理、工作线程、共享库加载和实时诊断面板源码，字段级[接口需求](../故障诊断模型接口需求.md)及[实现说明](../实时故障诊断实现.md)规定五类诊断 API 2、结构化工单与双端消息。四卷积硬 NPU 的独立测试包已板测；正式 `libmlk_fault.so`、兼容运行包和实时应用验收待完成，实施方法见[故障模型与上位机接入](../../docs/故障模型-上位机接入.md)。RUL 回放按钮和离线包目录不能直接启用实时故障诊断；API v1 保留粗分类语义。

| 项 | 行为 |
|---|---|
| 温度 | 读取 1 kHz 对齐帧，signed16 × 0.1 ℃；采样计数与时间戳实际更新后显示 |
| 振动 | 读取同一帧的 X/Y/Z 原始码，按 0.0039 g/LSB 显示三轴数值 |
| 电流 | ADS1115 + ACS712-20A，默认零点 20000 LSB、增益 1250 µA/LSB；启动恢复标定，归零值持久保存；无该通道时显示不可用 |
| 转速 | 读 §14 窗口（`0xB8`–`0xD0`，序号前后一致才接受）或 DMA `CH=3/4/5` 记录，0.001 rpm/LSB；正转正数、反转负数，停机回到 0；面板使用 signed32、0.1 rpm 量化，按安装方向对 PL 符号取反，数值与波形不施加 ±3276.7 rpm 限制；`online=0` 或 2 s 无新样本显示原因（如「编码器离线」/「无有效数据」），不用 0 冒充读数 |
| 通道停更 | 温度 6 s、振动 500 ms 未更新显示不可用；读失败立即显示不可用 |
| 驱动缺失 | 每秒重试，无演示数据回退 |
| IRQ | 本版不接传感器 IRQ，`plctl irq` 返回 polling-only 提示；驱动按轮询工作 |
| 波形 | 温度时域曲线；振动 X/Y/Z 三轴曲线；每个传感器面板可切换时域 / 频谱 |
| 频谱 | 码流带 §4.6 频谱窗口时显示 PL 逐 bin 功率（相对峰值 dB，约 1 s 刷新一次，读法为 `0x78` 序号读前后一致）；无该窗口时回退为本机 FFT。全谱 DMA 未实现 |
| 系统占用 | CPU（总 / 每核）、内存、网络 RX/TX、磁盘、NPU；NPU 显示硬核 / 软核 / CMA 驱动就绪状态，以及硬核、软核的中断速率与累计次数；驱动未提供百分比使用率 |
| 指标 | 读取周期 / 读取耗时实测；绝对采集→UI 延迟 TBD（PL/Linux 未硬同步）；传感器 DMA 经 HP1 环形缓冲由驱动映射后读取，指标区显示 DMA 块数 / 丢块 / 采集龄；显示 HDMI 1080P（固定模式，带宽未实测）；累计帧为 32 位，到 `0xFFFFFFFF` 自动回零 |
| 本地记录 | 顶栏「记录」按配置时长保存温度、电流、转速与振动 XYZ 的全部接收样本；始终加密为 MLKC，保留原生速率、时间与标定，支持七字段 CSV 与原生 CSV 导出；见 [记录与训练接口](../../docs/采集记录-回放与训练接口.md) |
| 配置入口 | 顶部「配置」或 F2 先验证配置密码；初始 `root`，可在配置内修改；QSettings 删除或重新部署后恢复 `root` |
| 时间同步 | 无 RTC，开机先播种；WiFi / 4G 联网后优先使用 Chrony NTP。后台每 10 s 检测网络时间源，链路刚连上与时间源可用时立即 `makestep`；顶栏「时间同步」手动触发同一动作。网络不可用时在配置中手动设置板端时间，网络时间可用时该项禁用。串口 `tools/sync_board_time.sh` 为回退。网络侧 `TIME_SYNC`（cmd `0x0014`）处理仍预留，独立于系统 NTP |
| 板端时间 | 标题下的时间是世界时钟，始终运行；模型窗口结果时间显示在「边缘模型检测」区块，暂停 / 停止后定格 |
| 转速工具 | `plctl speed [--json]` 读 §14 窗口并按序打印 raw / rpm / count / err_count / ts / seq / status；`online=0` 打印 `state=offline` 与 `rpm=-`，JSON 中 `rpm_milli` 为 `null`；`plctl dma --ring` 每块与原样字一起打印 `speed_words=` 并在首字属于 `CH=3/4/5` 时标注 `speed_record=wordN` |
| PL 特征读取 | `plctl features` 与本地 UI 均读取 X/Y/Z 时域特征（UI 显示 RMS/峰峰值）与 FFT 峰值/序号/功率摘要；`plctl spectrum` 读 §4.6 完整逐 bin 功率；实板读出待验证 |
| 传感器 IRQ | 本版不接传感器 IRQ，`/dev/pl_ctrl` 按轮询工作；`plctl irq` 会提示 `sensor IRQ not wired; polling only`，不纳入本版验收 |
| 输入 | 无物理键盘；USB 2.4G 接收器重新插入后，由 `/etc/udev/rules.d/60-input-hotplug.rules` 触发 `/usr/bin/mlk_input_replug` 自动重启看板，重新枚举输入设备；该重载机制已实板确认，期间界面及自动回放重新启动。数值配置项为纯文本编辑框 `mlk::NumberEdit`（点击定位、直接改写，不做整段选中）；文本输入弹出虚拟键盘；支持大小写英文、数字、常用符号 `. / : - _ @ * + = , ; #`、Shift / Caps、Backspace、Space、Enter；Shift/Caps 会同步切换字母键标签；键盘为独立顶层窗口，可拖出配置窗口，长按顶部拖动，点「收起」隐藏 |
| 资源 | 50 Hz 轮询；UI 刷新默认 2 fps（可设 2/5/10/30）；单线程，主要占用一个核；2 fps 下本应用实测约 4.5% 总 CPU / 单核 9.0%，另一核空闲 |
| 诊断 / 推理 | 已实现寿命数据集回放与融合 CPU / FF6 六分支 CPU/NPU 封装；合并镜像两轮 184 窗混合回放、CMA 释放及 HDMI / 采集 DMA 基础共存已验证，长期稳定性待定，见 [接入规程](../../docs/寿命模型-上位机接入.md) |

寿命模型选定部署目标为 `ff6`（四个前馈首层样条 FC + 两个原回归头样条 FC）。动态引擎严格校验六个分支的冻结计划，运行包加载与暂存均检查 FF6 身份；包来源、模型目录和 PC 接入方式见 [inference/README.md](inference/README.md)。

界面恢复为深色看板：四传感器小窗、时域/频谱、诊断、指标、系统占用，以及右上「时间同步」「记录」「回放」「配置」/ F2 设置面板（配置密码、通道标定与告警、记录管理、手动设置时间、重启核心板、FFT 点数、本地记录目录与加密口令、保留月数 / 最低剩余空间、加密模式 / PSK / ECDH / 密钥、可选 UDP 传输）。无物理键盘；文本输入弹出可拖动的独立顶层虚拟键盘；配置/密码窗口为 WindowModal，只阻塞主界面，不阻塞键盘。`src/npu.*` 动态加载冻结模型的 C ABI 引擎；配置与 DMA 的未实现操作不得作为已生效配置展示。

虚拟键盘保存最后选择的有效输入框，工具窗通过 transient parent 关联当前输入对话框；窗口焦点移到键盘时保留编辑内容、光标与选区。收起后仅在再次点击输入框或切换至其他输入框时弹出，输入窗口隐藏或销毁时解除目标。长按顶部 500 ms 后可拖动，移动请求按 33 ms 合并，松开时立即到达最终位置；linuxfb 后端对键盘让出的背景区域请求局部重绘，收起时取消待执行移动。独立键盘回归覆盖密码 / 数字输入、模态按钮、焦点恢复与拖动背景像素，见 [测试规程](../tests/README.md)。

## NPU 状态显示

ps-local 和 remote-pc 的「展开」抽屉共用 NPU 面板，以 CPU / 内存相同的标题、状态值、进度条和历史图表布局显示板端 NPU。使用率条禁用并显示驱动未提供；曲线显示最近 60 个有效样本的硬核 / 软核中断速率（次 / 秒），不换算为百分比。板端通过 UDP v4 type=7 的 `npu` 扩展同步状态，旧 `gpu` 字段保留兼容。

### 采集与实现

| 实现位置 | 行为 |
|---|---|
| [`src/systemmonitor.cpp`](src/systemmonitor.cpp) | 每秒检查 `/dev/hard_npu`、`/dev/soft_npu`、`/dev/cma_mem` 是否为字符设备，并读取 `/proc/interrupts`；仅执行状态查询，不打开 NPU 设备或调用推理 ioctl |
| [`src/npumonitor.cpp`](src/npumonitor.cpp) | 按 `HARD_NPU_IRQ`、`SOFT_NPU_IRQ` 驱动名称定位统计行，从 CPU 表头确定计数列，分别合计各 CPU 的中断数；不固定 Linux IRQ 编号，不计入后面的 GIC 中断编号 |
| [`src/npumonitor.h`](src/npumonitor.h) | 首个样本、计数回退、设备重现或统计恢复后重新建立基线 |
| [`../common/npustats.h`](../common/npustats.h) | 共用 NPU 状态与 JSON 扩展 v1；累计次数用十进制字符串保留 64 位精度，不可比较的速率为 null |
| [`../common/npupanel.cpp`](../common/npupanel.cpp) | 两端共用状态、使用率条和双曲线；统计失效清除对应曲线，远端状态超时清除全部曲线 |
| [`src/klocalwindow.cpp`](src/klocalwindow.cpp) | 更新共用面板并通过 BoardStatus 同步 `npu`、`network`，供 remote-pc 显示 |

中断速率为相邻有效样本的计数差除以实际采样间隔，单位为次 / 秒；累计值为系统启动以来的内核统计，可以包含其他进程的执行。统计不可读时显示原因，不以 0 代替。

| 条件 | 面板状态 |
|---|---|
| 任一所需字符设备缺失 | `未就绪（缺少 …）`，列出硬核、软核或 CMA |
| 三个设备齐备，任一中断计数有正增量 | `驱动就绪 · 检测到中断活动` |
| 三个设备齐备，统计不完整且未检测到正增量 | `驱动就绪 · 活动统计受限` |
| 三个设备齐备，统计可读但尚无可比较样本 | `驱动就绪 · 等待活动采样` |
| 三个设备齐备，两路有效计数均未增加 | `驱动就绪 · 本周期无中断` |

中断活动不能判定推理是否成功；无中断也不能判定硬件空闲。FF6 模型只使用硬核，软核计数为 0 属于正常情况。厂商驱动未提供 NPU 百分比使用率接口，面板显示 `使用率：驱动未提供`，不以中断速率或 CPU / NPU 混合推理总耗时换算占用百分比。

### 验证与交付

开发机回归覆盖硬核 / 软核活动、多核计数、设备缺失、统计恢复、计数回退、异常统计、64 位 JSON 往返及网络选择。窗口回归覆盖两端 1920×1080 侧栏布局、双曲线、顶栏两行字体与位置、加密 UDP 状态同步、断链清除及旧板端字段缺失。测试输入不代表实板读数。

ARM aarch64 应用及 SHA-256 更新包已构建并核对，包内仅包含 `opt/qt_play/mlk_ps_local`；构建与更新操作分别见本文「构建」和「部署」。板端应用 SHA-256 已与 ARM 构建产物匹配；三个 NPU 字符设备、应用中的 ArmNN / FF6 映射、硬核中断增长与 WiFi 默认路由已通过串口核对。NPU 的 sysfs 节点未提供使用率属性，devfreq 无设备。HDMI / PC 的实际画面、驱动缺失提示和长期运行仍待实板验收。

## 本地记录与现场回放

「记录」按 `recordSegmentSec`（10 / 30 / 60 / 300 s，默认 60）保存四传感器全部接收样本：温度、电流、转速、振动 XYZ。文件为 `capture_<startUs>_<endUs>_<uuid>.mlkc`，默认目录 `/data/mlk_f3p`，始终使用本机记录口令加密。各通道保留原生速率和样本，不以整个区间最后温度替代温度序列；记录源是应用成功收到的样本，不能补回 PL 或链路已丢失的数据。

「回放」打开独立二级窗口，默认显示异常区间清单，也可查看全部采集记录或故障工单。单击条目或选中后按回车才加载共用时间轴的温度、电流、转速、振动 XYZ；悬停、滚动和刷新清单保留当前波形。可播放、定位、查看数据表、标注并解密导出 CSV。实时看板和采集保持独立。模型工单按故障时间自动关联分段；异常定义与训练标签由模型 / 人工通过 API 给出。

采集与标签分别使用 MLKC 外层 v1 / 采集载荷 v2（兼容 v1）及 MLKL v1，AES-256-GCM + PBKDF2-HMAC-SHA256。保留月数、文件数和最低空间在保存前检查，异常记录同样受保留策略约束。配置（需密码）中「清理所有记录」删除本目录全部 MLKC / MLKF，「删除选定记录」通过文件选择器多选删除；两者都先确认并保留工单、标签和寿命结果。既有 MLKF 只能预览原始 CH0–CH3，不能恢复遗漏的传感器。

加密文件字节格式、字段单位、时间估算 / 对齐、模型标注 API、`mlk-capture` 解密 CSV 命令与实板验收步骤见 [采集记录、回放与训练接口](../../docs/采集记录-回放与训练接口.md)。ARM 工具由 `build_arm.sh` 构建至 `build-arm/capture-tool/mlk-capture`，暂存后位于 `/opt/qt_play/mlk-capture`。

## 验证状态

- 已实板验证：HDMI/Qt、`/dev/pl_ctrl`、温度/振动真实读数、配置密码入口与修改、自动保留、保留月数/最低剩余设置、虚拟键盘。记录管理与手动校时入口为主机验证，待实板验收。
- MLKC 四传感器记录、故障波形回放、标签 API 与 CSV 导出完成开发机验证；完整实采速率、磁盘写入压力和长期运行待实板验证。
- 虚拟键盘的输入目标保持、模态交互、收起与拖动背景像素检查通过主机 Qt 5.15 和 QEMU ARM Qt 5.13 验证；实际 linuxfb 输入与显示待实板验证。
- 已实板验证：`S99qtapp` 的 `stop` / `restart` / `status`；停止日志确认 DMA 环清理，独立混合推理后可以恢复看板。接收器重新插入时通过自动重启看板接管输入。
- 逻辑已实现但未在板跑到边界：累计帧到 `0xFFFFFFFF` 回零（需强制阈值才能验证）。
- 采集 DMA 环已实板运行，完整谱与参考源定标、长期并行压力及连续全速率记录仍须分别验收。传感器 IRQ 本版明确为轮询-only，不列为待实现。

## 构建

主机 Qt 5：

```bash
cd host
nix develop
mkdir -p ps-local/build-native
cd ps-local/build-native
qmake ../mlk_ps_local.pro
make -j4
./mlk_ps_local
```

ARM Qt 5.13.2：从仓库根执行，使用本地厂商 SDK；可用 `QT_CROSS_ROOT`、`CROSS_BIN`、`QT_OPENSSL_SYSROOT` 指定工具路径。完整界面链接 Qt Network 与 OpenSSL 1.1，交叉 OpenSSL 缺失时先运行 `host/ps-local/build_openssl_arm.sh`。

```bash
host/ps-local/build_arm.sh
file host/ps-local/build-arm/mlk_ps_local
file host/ps-local/build-arm/plctl
```

内核模块：

```bash
REPO="$PWD"
make -C host/ps-local/driver \
  KDIR="$REPO/platform/flash/vendor/sources/linux" ARCH=arm64 \
  CROSS_COMPILE="$REPO/platform/flash/vendor/tools/aarch64-linux/bin/aarch64-linux-gnu-"
```

设备树示例 [`driver/pl_ctrl.dtsi`](driver/pl_ctrl.dtsi) 使用 `0x8000_0000`，`status = "okay"`，不填 `interrupts`。GIC ID 仍为 REF；合并 HDMI 码流把 `f2p_irq[0]/[1]` 留给 VDMA，传感器 IRQ 未接线。驱动在 `platform_get_irq()` 失败时继续 probe，`read` / `pread` 正常，`poll()` 返回不可用；`plctl irq` 因此返回 ENODEV，`plctl sensors` 走轮询。字符设备节点由 miscdevice 注册，devtmpfs/mdev 负责创建。

## 无硬件验证

```bash
cd host
nix develop
mkdir -p ps-local/build-test
cd ps-local/build-test
qmake ../tests/unavailable.pro
make -j4
QT_QPA_PLATFORM=offscreen ./test_unavailable
```

该测试只验证缺失设备时不出现读数，不替代真实数据、IRQ 与 HDMI 验收。RTL 仿真数据仅存在于测试台，不进入应用。无 IRQ 的轮询模式只验证寄存器读数，不验证中断。

NPU 状态解析回归使用 [`tests/npu_monitor.pro`](tests/npu_monitor.pro)。在已启用 Qt 开发环境的仓库根目录执行：

```bash
mkdir -p host/ps-local/build-npu-monitor
cd host/ps-local/build-npu-monitor
qmake ../tests/npu_monitor.pro
make -j4
./test_npu_monitor
```

界面布局回归使用 [`../tests/windows.pro`](../tests/windows.pro) 的 `localNpuSidebar`。从仓库根目录执行：

```bash
mkdir -p host/ps-local/build-window-tests
cd host/ps-local/build-window-tests
qmake ../../tests/windows.pro
make -j4
QT_QPA_PLATFORM=offscreen ./test_windows localNpuSidebar
```

## 部署

### 应用更新

`build_arm.sh` 同时生成 `build-arm/ps-local-app-update.tar`、`.tar.gz` 与各自的 SHA-256 校验文件。板端 BusyBox tar 未启用 `-z`，应用更新使用未压缩 `.tar`。更新包只包含 `/opt/qt_play/mlk_ps_local`，用于已有匹配 Qt 运行库、驱动和启动脚本的板卡；不覆盖 `/var/lib/mlk/config` 或 `/data/mlk_f3p`。

完成当前记录后再更新。以下传输命令从仓库根执行，IP 使用板端实际地址：

```sh
scp -O host/ps-local/build-arm/ps-local-app-update.tar \
  host/ps-local/build-arm/ps-local-app-update.tar.sha256 root@192.168.211.14:/tmp/
```

在板端串口或 SSH shell 中执行：

```sh
cd /tmp
sha256sum -c ps-local-app-update.tar.sha256 && \
  /etc/init.d/S99qtapp stop && \
  tar xf ps-local-app-update.tar -C / && \
  /etc/init.d/S99qtapp start
```

`stop` 通过正常退出路径保存当前内存分段并停止 DMA；保存失败时应先核对磁盘和记录口令，避免替换进程造成未提交样本丢失。更新后核对配置密码、数字框、虚拟键盘收起、OK / Cancel，以及长按拖动后旧位置的背景恢复，再验证开启记录期间的输入操作。

NPU 面板验收：两端展开左上系统占用抽屉，核对 NPU 项、使用率条与硬核 / 软核曲线。板端执行以下只读命令，并在已配置的 CPU / NPU 混合回放运行期间比较间隔采样的中断计数与面板速率；暂停后若中断不再增加，不应据此判定硬件空闲。

```sh
ls -l /dev/hard_npu /dev/soft_npu /dev/cma_mem
grep -E 'HARD_NPU_IRQ|SOFT_NPU_IRQ' /proc/interrupts
```

### rootfs 构建

从仓库根执行：

```bash
platform/flash/stage_ps_local.sh
platform/flash/build_rootfs.sh
```

`stage_ps_local.sh` 构建并暂存 Qt 应用、`plctl` 与匹配内核版本的 `pl_ctrl.ko`；需要已准备的 `rootfs-overlay/opt/qt_play/qt_lib/`（厂商 Qt 库、linuxfb 插件、CJK 字体）。`build_rootfs.sh` 在厂商基础 rootfs 上叠加这些文件，完整 Buildroot 顺序另见 [`../../docs/PL-PS接入与上板交付.md`](../../docs/PL-PS接入与上板交付.md)。

板上启动：

```bash
insmod /lib/modules/"$(uname -r)"/extra/pl_ctrl.ko
plctl probe
plctl write 0x04 5
plctl sensors
plctl speed
plctl speed --json
plctl spectrum --top 8
# 本轮无传感器 IRQ：plctl irq 返回 ENODEV 属于预期
cd /opt/qt_play
./run_arm_app.sh
```

自启：`S98plctrl` 装载模块，`S99qtapp start` 启动看板；停止、重启与状态见「停止与重启」。设备树未启用或设备未绑定时仍显示不可用。HDMI 依赖匹配的 PL 显示通路、设备树和 `/dev/fb0`，见 [`../../docs/HDMI显示通路.md`](../../docs/HDMI显示通路.md)。合并码流 `fpga/uisrc/out/soc_hdmi/soc_hdmi.bit` 同时包含 HDMI 与 `0x8000_0000` 传感器 AXI，不再使用仅含采集的 `soc_candidate.bit` 作为显示镜像。

USB HOST 口的无线鼠标接收器由 usbhid / evdev 驱动。linuxfb 不会自动加载输入插件，`run_arm_app.sh` 设置 `QT_QPA_GENERIC_PLUGINS="evdevmouse,evdevkeyboard"` 后，Qt 才会打开 `/dev/input/event*` 并处理鼠标与键盘。串口为 115200 8N1，SD 启动模式为 SW1 ON-OFF-OFF。

当前软件构建可复现；合并码流已上板显示 HDMI 看板，鼠标输入、温度与振动三轴读数已观察。温度 / 振动未做参考源标定，PL 全速率频谱 DMA 仍待定；转速通道 PS 侧及实板正反转已接入，寄存器与 DMA 记录见 [接口约定](../../docs/PL-PS接口约定.md) §14。电流通道已接入 ADS1115 常驻显示与归零。两端回放后端默认选择 `CPU/NPU（FF6 六分支）`；开机自动回放先尝试混合后端，初始化或推理失败时回退融合 CPU。下拉框跟随板端实际后端，未提交的手动选择在状态刷新时保留。硬核、软核与 CMA 三个设备是混合后端的运行前提；合并 `soc_hdmi` 镜像已完成两轮 184 窗混合回放 / CMA 释放，并观察到 HDMI、采集 DMA 环与硬 NPU 中断增长同时存在；长期稳定性待定。构建与检查见 [开发流程](../../docs/PS-PL开发流程.md) §6.1，实测见 [验证结果](../../fpga/uisrc/06_doc/验证结果.md) §5.6。

## 停止与重启

看板由 `S99qtapp` 管理：

```bash
/etc/init.d/S99qtapp status
/etc/init.d/S99qtapp stop
/etc/init.d/S99qtapp restart
```

pidfile 为 `/var/run/mlk_ps_local.pid`；`/var/run` 不存在时脚本创建，创建失败则改用 `/tmp/mlk_ps_local.pid`。`run_arm_app.sh` 以 `exec` 启动 `mlk_ps_local`，脚本记录的 pid 即应用 pid。`stop` / `status` 先读 pidfile，pidfile 缺失、失效或进程名不符时扫描 `/proc/<pid>/comm`（`mlk_ps_local`），手工启动的实例同样可以停止；脚本不使用 `pgrep` / `pkill`。

| 命令 | 行为 |
|---|---|
| `start` | `/opt/qt_play/run_arm_app.sh` 不存在时静默返回 0；已在运行则输出 `already running` 并返回 0；否则后台启动并写 pidfile |
| `stop` | 发送 SIGTERM，最多等待 5 s（每秒 `kill -0` 轮询）；进程退出后删除 pidfile 并返回 0；超时才发送 SIGKILL，在标准错误输出 DMA 环的清理命令并返回 1 |
| `restart` | 先 `stop` 再 `start` |
| `status` | 运行中输出 `running (pid N)` 并返回 0；未运行输出 `not running` 并返回 3 |

### 信号路径

应用在 `main` 中安装 SIGTERM / SIGINT / SIGHUP 处理函数。处理函数只把信号编号写入自管道的一个字节（`write()` 与 `errno` 为 async-signal-safe 调用），`QSocketNotifier` 在事件循环中排空管道并调用 `QCoreApplication::quit()`。`app.exec()` 返回后 `main` 退出作用域，`KLocalWindow` 析构调用 `DriverSource::stop()`：`pl_dma_ring_stop()` 向 `DMA_CTRL`（`0x90`）写 `RING|STOP`（`0x18`），等待 BUSY 清零后清除 STOP 位，随后 `munmap` 环形缓冲并关闭 `/dev/mem` 与 `/dev/pl_ctrl`。`DriverSource` 析构函数同样调用 `stop()`，重复调用无副作用；SIGTERM / SIGINT / SIGHUP 与正常退出都走这条路径。

### 异常恢复

`SIGKILL`、应用崩溃或异常复位会绕过析构，PL 传感器 DMA 环保持启动，`DMA_CTRL`（`0x90`）的 BUSY 位保持置位（例如 `0x108`）。恢复步骤：

```bash
plctl write 0x90 0x18
plctl write 0x90 0x0
```

第一条写 `RING|STOP` 停止传输，第二条把 `RING` 与 `STOP` 清零；之后可重新 `start`。`plctl` 的读写以驱动放行 `0x90` 窗口为前提。

## 本地 HDMI / FF6 验证卡

本地显示与 RUL 数据集回放可使用厂商 NPU 演示的配套启动文件，叠加本仓库 Qt 应用和 FF6 运行包。此环境的现场采集显示不可用，验收不依赖 PC 上位机。镜像生成、SD 启动和板端检查见 [RUL-HDMI 启动与验收](../../platform/flash/RUL-HDMI启动与验收.md)；应用共享库及 SD 读回证据见 [FF6 验证摘要](../tests/results/ff6/summary.json)。

## 无线联网与双端协同

首次启动自动生成独立本机身份。首次配对时导出本机公钥、导入 PC 公钥并选择 ECDH；已配对后可自动发现同网段上位机。逐步操作与密钥更换见 [连接与密钥配置](../../docs/上位机连接与密钥配置.md)。

顶栏连接摘要上行显示核心板网络（未连接 / WiFi / 4G / 有线），下行显示上位机应用连接状态；字体与左侧 CPU / 内存一致。网络由板端有效默认路由和已连接接口确定，完整口径见[接口契约](../../docs/上位机-PS接口约定.md#板端状态type7)。

顶栏「展开」右侧「网络配置」按千兆网 / 4G / WiFi 分页显示配置：千兆网页拉起网口、获取地址并登录网络门户，也可勾选「使用 PPPoE 拨号」填入校园网账号密码后拨号；WiFi 页通过 wpa_supplicant 控制接口搜索并连接开放 / WPA/WPA2-PSK 网络；4G 页保存 APN 并启停 quectel-CM。链路优先级为 千兆网 > 4G > WiFi。板端保留虚拟键盘；remote-pc 使用物理键盘。内核无线 / USB 网络驱动与 rootfs 网络组件需配套部署，无 SIM 时 4G 组件仅集成，不自动拨号。

「网络配置」右侧显示核心板网络状态（千兆网 / 4G / WiFi / 未连接）与上位机连接状态；收到所配上位机的有效应用帧后显示明文 / 加密连接，5 秒无有效帧显示超时。启动脚本将本机配置固定到 `/var/lib/mlk/config/MLK-F3P/mlk_ps_local.conf`，并迁移已有 root 配置，避免开机与 SSH 启动读取不同设置。

配置中可选择 ps-local / remote-pc 共享参数控制权，默认 ps-local。四通道标定 / 告警参数由板端唯一保存：ps-local 优先时远端写入被拒绝；remote-pc 优先时板端仍可本地修改，上位机连接后以其保存的快照覆盖板端。共享参数与链路 / 密钥 / 本地存储独立，修改本地设置不要求先配置 IP 或密钥。两端配置密码、记录口令、目录、显示和网络设置独立。

模型面板「故障分级」右侧显示「故障时间」，无事件为 N/A。`KLocalWindow::reportFault` 先以本机记录口令保存加密工单，再通过加密链路重发至 PC，收到 PC 落盘确认后标记已确认。采集记录按钮与事件 API 独立，纯 RUL 回归不生成故障事件。

构建、API 字段、工单目录、WiFi 联调与实板验收边界见 [无线联网与双端协同](../../docs/无线联网与双端协同.md)。

## 声光告警与分类模型 API

`alarmcontroller` 使用共享通道配置和有效采样判定越界，每 250 ms 更新 PL v1.3 告警寄存器。`modelfaultserver` 在 `/run/mlk-fault.sock` 接收分类模型 JSON API v1，驱动模型优先的 U7 长响并生成加密工单。板端工具为 `mlk-fault`，C 客户端与 Python 示例、健康/不可用/分级定义见[声光告警与模型故障 API](../../docs/声光告警与模型故障API.md)。RUL 回归不会产生分类告警。

电流零点与增益在双端配置中共用板端确认值，默认对应 ACS712-20A 的 100 mV/A。电机停转且无电流时在 ps-local 点击「归零」，重启后恢复实际零点。采集使用 UDP v6，记录载荷使用 mlk.capture.v2；两端应用与 mlk-capture 需配套更新，读取兼容原有 UDP 与采集记录。PL 码流无需更新。
