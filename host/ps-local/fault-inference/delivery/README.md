# F3P 实时诊断更新包使用说明

本目录用于在另一台 NixOS/Linux 或 Windows 电脑上传输和安装 ARM 应用及故障解码库。
无需复制 Ubuntu 虚拟机、编译环境或原始数据集；无需格式化 SD 卡。
安装器复用并校验卡上权重、量化参数、分级阈值和标准化资产，创建独立运行包版本后与应用一起切换。
训练权重、RUL、Qt/Arm NN、内核、DTB、码流、记录和配置保持。

## 1. 交付文件与接收电脑

便携 ZIP 只在交付电脑的 `out/mlk-fault-live/` 生成，不纳入 Git。可通过 U 盘复制到接收电脑，
解压至可写目录后使用；不必将 ZIP 加进仓库。仓库本目录已经包含相同的 `tar.gz` 更新归档、
校验文件、工具和说明，也可在另一台电脑的 dev 分支执行 `git pull --ff-only origin dev` 后直接使用。

| 文件 | 用途 |
|---|---|
| `fault-live-update.tar.gz` | 板端更新归档，含 ARM 应用、新解码库及其清单、安装脚本和校验文件 |
| `fault-live-update.tar.gz.sha256` | Windows 本地归档校验 |
| `upload_update.py` | Linux/NixOS 串口上传，仅用 Python 3 标准库，不需要 PowerShell 或 pyserial |
| `Upload-Update.ps1` | 本地校验、串口分块上传及传输报告；只传输，不自动安装 |
| `f3p_serial.ps1` | 便携 ZIP 携带的串口工具；仓库源文件在 `npu/tools/` |
| `README.md` | 本说明 |
| `build_update.py`、`install_update.sh` | 仓库中的更新包构建入口与可审查安装源码；接收电脑直接使用归档即可 |

Windows 接收电脑需安装 **PowerShell 7.2 或更高版本**（命令 `pwsh`，Windows 自带 PowerShell 5.1 不适用），
以及开发板 USB 串口驱动、PuTTY 等串口终端。串口传输不要求 Windows 与开发板在同一网络，
也不要求开发板联网。

本包应用 SHA-256：`64571005c0b4b4efc5eb3434e17c9e1700e58c18f65b8cc7bbb198b9e126c267`。
故障解码库 SHA-256：`90d403c06d26be4516148084fdf72d607e8c7c90bbca5623161044aee4d78d44`。
归档 SHA-256 以同目录 `.sha256` 为准。安装器接受原应用 `0df57fa0…` 或已交付修复应用
`023bd835…`、`71629c08…`、`409fbd7c…`、`4b12958b…`、`9b7039ab…`、`a7f3465e…`，遇到其他版本会拒绝替换；
固定资产哈希不一致时同样拒绝，不得删除检查绕过版本差异。

## 2. NixOS/Linux 串口传输

以下为 NixOS 操作，不使用 PowerShell。先进入从 U 盘复制并解压的目录，或仓库
`host/ps-local/fault-inference/delivery/`。

### 2.1 准备工具与串口权限

```sh
nix --extra-experimental-features 'nix-command flakes' shell nixpkgs#python3 nixpkgs#picocom
ls -l /dev/serial/by-id/
ls -l /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

已有 Python 3 和串口终端时可跳过临时环境。第一次下载 Nix 工具需要电脑联网；离线使用时
事先准备这些工具，便携 ZIP 不携带 Nix store 依赖。Python 上传器不依赖 pip 包。
串口通常为 `/dev/ttyACM0`，也可能为 `/dev/ttyUSB0`；优先使用 `/dev/serial/by-id/`
中的稳定路径，须实际确认 PS 控制台，不能仅凭编号选择。

普通用户对串口有读写权限时，直接执行下述命令；没有权限时使用示例中的 sudo 和
`command -v` 得到的绝对路径，避免 sudo 的 PATH 中没有临时 Nix 工具。
也可在 NixOS 用户配置中将实际用户加入 `dialout`，重建配置并重新登录后使用；
无需为了本次传输全局修改串口权限。

### 2.2 登录并释放串口

F3P 使用独立电源，从 SD 启动，数据 USB 线接板上 UART&JTAG Type-C；USB 设备连接到
这台 Linux 主机，不能仍被虚拟机接管。用实际端口替换示例：

```sh
sudo "$(command -v picocom)" --baud 115200 --flow n /dev/ttyACM0
```

115200、8N1、无流控。等待 Linux 启动完成，回车出现登录提示后，登录 `root`；
出厂密码为 `root`，已改密码时使用现用密码。确认是 Linux `#` 提示符，可执行 `uname -a`。
不要在 U-Boot 倒计时或 `=>` 下运行上传工具。

上传前在界面停止诊断、评估和 RUL 回放并保存记录，减少控制台输出对传输的干扰。
先按 **Ctrl+A**，再按 **Ctrl+X** 退出 picocom，释放串口。不是直接按 Ctrl+C，
也不要把终端留在后台；上传时只能有一个程序使用该端口。

### 2.3 上传并安装

```sh
sha256sum -c fault-live-update.tar.gz.sha256
sudo "$(command -v python3)" ./upload_update.py --port /dev/ttyACM0
```

有普通用户串口权限时可使用 `python3 ./upload_update.py --port /dev/ttyACM0`。
稳定设备路径同样通过 `--port /dev/serial/by-id/实际名称` 指定。
上传器设置 115200/8N1/无流控，逐块等待应答，最终核对板端完整 SHA-256；
成功后释放串口并恢复原串口参数，打印三条本次专用安装命令。约 0.6 MB 归档需要数分钟，
等待 `upload_bytes=` 进度结束。工具只上传，不自动安装、登录或重启。

再次打开 picocom，在板端 Linux `#` 下粘贴上传器打印的三条命令，随后按 §4/§5 安装与验收。
不要在 NixOS 主机终端执行打印的板端命令。日志默认位于 `logs/transfer-*.json`；
若使用 sudo，日志可能为 root 所有。可用 `--log-dir /可写目录` 更换日志位置。

失败即停止，不自动重试。先退出上传进程并重新打开 picocom，确认控制台处于 Linux `#`
再退出终端重新上传。失败时部分文件留在报告给出的 `/tmp/cdx-upl-*`，重新运行使用新路径，
不会覆盖原文件；未核验成功的文件不能安装。拔线或异常退出后若终端显示异常，可在 NixOS
对实际串口执行 `sudo stty -F /dev/ttyACM0 115200 cs8 -parenb -cstopb -crtscts` 后重新登录。

工具用法参考：[Nix flakes](https://wiki.nixos.org/wiki/Flakes)、[picocom 文档](https://github.com/npat-efault/picocom)。

## 3. Windows 连接并登录（可选）

1. F3P 保持 SD 启动，使用独立电源上电。数据 USB 线连接板上 **UART&JTAG Type-C** 与 Windows。
2. USB 设备连接到 Windows 主机；VMware 若接管设备，在可移动设备菜单将其断开并连接回主机。
3. 在设备管理器「端口（COM 和 LPT）」确定 PS 串口。已用板卡为 CH342 SERIAL-A，COM4；
   接收电脑的编号可能不同。SERIAL-B 通常为 PL 串口，FTDI 下载器端口不是 Linux 控制台。
4. PuTTY 选择 Serial、对应 COM、115200、8N1、无流控。等待 Linux 完成启动，回车后出现
   `buildroot login:`，登录 `root`，出厂密码 `root`；已修改密码时使用现用密码。
   密码输入没有回显。确认提示符为 `#`，可执行 `uname -a`。
5. **关闭 PuTTY 会话**，释放串口。上传工具需要独占端口。不要在 U-Boot 倒计时或 `=>` 提示符下上传。

没有 COM 分类表示尚未识别串口；先检查数据线、接口、驱动和 VMware 接管。
COM 能打开却无回应时，确认开发板已独立供电、Linux 已启动，以及选择的是 PS 串口。

### 3.1 Windows 串口上传

打开 PowerShell 7，进入解压目录，端口按实际编号替换：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\Upload-Update.ps1 -Port COM4
```

本次进程的执行策略参数不修改系统全局策略。脚本先检查本地归档 SHA-256，再确认板端为 root，
按 192 字节分块传输，核对完整远端 SHA-256 后才报告成功。115200 串口传输约 0.6 MB
压缩包需数分钟；出现 `upload_bytes=` 表示仍在传输，等待完成。

脚本使用唯一的 `/tmp/fault-live-update-<随机标识>.tar.gz`，不覆盖已有上传文件；
日志存于解压目录的 `logs/`。失败即停止并释放串口，不会自动安装或重启开发板。
超时后先查看 `logs/transfer-*.json` 的 `last_response/error`，用 PuTTY 确认回到 Linux `#`
后再关闭终端并重新上传。不要同时打开两个串口工具，也不要在 Linux 控制台正执行其他命令时上传。

## 4. 板端安装

上传成功后，脚本会打印**本次专用的三条安装命令**。重新用 PuTTY 打开同一 COM，
在板端 Linux `#` 下执行打印的命令；不要将这些命令粘贴到 NixOS 主机或 Windows PowerShell。
命令的形式如下，路径中的标识须使用上传脚本实际打印的值：

```sh
mkdir -p /tmp/fault-live-实际标识
gzip -dc /tmp/fault-live-update-实际标识.tar.gz | tar -xf - -C /tmp/fault-live-实际标识
sh /tmp/fault-live-实际标识/fault-live-update/install.sh
```

板端精简 tar 使用 `gzip -dc | tar -xf -`，不依赖 `tar -z` 或系统 `timeout`。
安装前先在界面停止诊断、评估和 RUL 回放，保存记录；安装过程会短时关闭 HDMI 应用，
校验内部文件并保留原应用和运行包，替换应用、切换 `/opt/mlk_fault` 后同步并恢复 `S99qtapp`。
`/opt/mlk_fault.previous` 指向安装前运行包，服务恢复失败时自动恢复应用和运行包链接。不必重烧系统。

成功后核对：

```sh
sha256sum /opt/qt_play/mlk_ps_local
sha256sum /opt/mlk_fault/libmlk_fault.so
/etc/init.d/S99qtapp status
```

应用与解码库哈希应与 §1 一致，服务应为 `running`，HDMI 看板恢复。
重复安装同一版本会提示已安装；版本不符、校验失败或服务停止失败时，保留错误输出进行核对。

## 5. 功能验收与操作

1. 在配置中确认记录目录、剩余空间和记录口令。空口令时不能启动诊断和加密记录。
2. 验证故障识别时保持「仅保留异常波形」关闭，保留误判分析所需的全部数据。
   如需故障发生前的波形，应先开始记录/诊断、取得正常基线，再植入故障。
3. 开始诊断后应显示「记录中」。完整四秒窗口和数据质量合格后产生分类结果；
   模型分数至少 0.80 且同类别、同等级连续两窗确认，启动约六秒后可满足确认条件。
   低于 0.80 只显示「低置信度候选」，撤销模型告警并清除累计确认，不生成故障工单；
   仍保留原始预测与波形，不能当作正常。0.80 是试用门槛，待实采标定，可能漏掉低分真故障，
   也不能排除高分误判；独立传感器阈值告警不受该门槛影响。数据不足不能当作正常。
   质量栏同时显示缺口数；少量缺块按冻结模型的实际时间格和有效率处理，不因单个缺口拒绝整窗。
   状态栏应随窗口更新为诊断中或窗口不可用；持续不可用时保存完整状态、TF/快慢状态有效率、
   缺口数、DMA 丢块数和应用 SHA-256，无须继续等待一分钟。
   窗口内最近有效温度不超过 35℃ 时，仅屏蔽过热类别，从其余四类真实分数中重新选择主故障；
   超过 35℃ 后允许模型选择过热。界面质量栏显示屏蔽状态，日志记录所用温度。
   缺失或过期温度仍按窗口质量规则判不可用，不把数据不足或被屏蔽类别直接改成正常。
4. 故障确认后从「回放 → 故障清单」选择工单，查看带类别、等级、模型窗口阴影的波形，
   跨分段时切换关联记录。停止诊断和打开回放均保存当前记录段；记录可从顶栏单独停止。
5. 日常运行可勾选「仅保留异常波形」。仅本次诊断的新记录中，完整被有效正常窗口覆盖的
   旧分段才清理；故障前后各至少 10 秒、异常标签、传感器告警和数据不足分段受保护。
6. 模型一级/二级/三级对应声光 3/6/9 秒，连续三窗正常解除模型告警；数据集评估不触发声光。
   需匹配的 PL 接口 v1.3、驱动及 J12.11/U7 接线。软件状态位已测，实际声光动作须现场验收。

未确认工单在后台按两秒重试；同一密文及口令复用已认证结果，不在采集事件循环重复执行 PBKDF2。
停止诊断和记录仍保留待确认工单；收到 ACK 后停止重发。逐窗日志的剩余空间参数使用 MiB。

本应用已通过 18 项离线回归、两例完整六分支训练侧对照、AArch64 构建和 QEMU 初始化。
温度对照覆盖 23.6、35.0、35.1、41、46℃，过载和欠压类别/等级保留。数据集评估不应用在线温度屏蔽，
冻结 360 窗分类 360/360、联合 356/360。本次交付未在接收电脑或开发板上
执行传输/安装；本次缺口修复后的实采推断、NPU 与加密记录并发及长期稳定性仍需更新后验收。
诊断启动后的自动记录、停止封存和记录页回看已有现场操作反馈，可作为记录链路可用的依据。

## 6. 回退

原应用保存在 `/opt/qt_play/mlk_ps_local.before-thermal-<原哈希前16位>`，原运行包由
`/opt/mlk_fault.previous` 保留。使用 `ls -l /opt/qt_play/mlk_ps_local.before-thermal-*` 查看备份。
先核对选定备份的完整 SHA-256，再同时恢复应用与运行包。以下以安装前 `9b7039ab…` 为例：

```sh
/etc/init.d/S99qtapp stop
cp -p /opt/qt_play/mlk_ps_local.before-thermal-9b7039ab2214b52d /opt/qt_play/mlk_ps_local
previous=$(readlink /opt/mlk_fault.previous)
test -n "$previous"
ln -sfn "$previous" /opt/mlk_fault
sync
/etc/init.d/S99qtapp start
```

回退恢复应用与运行包链接，记录、标签和配置保持。再次启动前确认服务状态和两项哈希。

## 7. 构建更新归档与便携 ZIP（交付方）

应用和共享库构建见 [BUILD.md](../BUILD.md)。`--runtime` 指向校验完整的七文件正式 ARM 包，
其中仅 `libmlk_fault.so` 和封闭它的 `model.json` 更新，其他五项须与既有卡上资产相同。
生成器只把应用、新库及其清单、固定资产校验和安装脚本放入归档，不携带新训练权重。

```sh
python host/ps-local/fault-inference/delivery/build_update.py \
  --app /path/to/qt-arm64/mlk_ps_local --runtime /path/to/runtime \
  --output host/ps-local/fault-inference/delivery/fault-live-update.tar.gz
```

仓库根目录使用 Python 标准库生成 ZIP；Linux 接收电脑的串口上传仍需 Python 3：

```sh
python host/ps-local/fault-inference/delivery/build_portable.py --output fault-live-update-portable.zip
```

生成的 ZIP 和同名 `.sha256` 一并复制到接收电脑。ZIP 内已包含实际串口工具；
直接使用仓库目录时，`Upload-Update.ps1` 自动定位 `npu/tools/f3p_serial.ps1`。

Linux 上传器的维护测试为 `python3 -m unittest -v test_upload_update`，在本目录执行。
已在 Linux 伪终端通过分块传输、本地/远端校验失败保护与串口参数恢复三项测试，
没有访问真实板卡；目标 NixOS 电脑的驱动、权限与实际连线仍需现场确认。

安装器的 Linux 离线测试使用隔离目录和模拟服务：设置 `MLK_TEST_OLD_APPLICATION` 为支持的旧应用、
`MLK_TEST_BASE_RUNTIME` 为含相同固定资产的旧运行包，再执行 `python3 -m unittest -v test_install_update`。
覆盖更新及重复安装、固定资产不符拒绝、服务恢复失败时回退应用和运行包，不访问真实板卡。
