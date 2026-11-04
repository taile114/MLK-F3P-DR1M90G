# NixOS 下 TD 与 Questa 使用说明

安路 TD 与 Siemens Questa 以 NixOS FHS 沙箱方式安装，与常规 Linux 发行版的使用方式有差异。本文记录启动、编译、烧录、仿真与排障流程，供复用。

## 1. 安装形态与路径

运行环境为 NixOS 26.05 (Yarara)；TD 与 Questa 以 FHS 沙箱方式安装。

| 工具 | 版本 | 安装根目录 | GUI 启动器 | 命令行入口 |
|---|---|---|---|---|
| 安路 TangDynasty (TD) | Release 2026.2 (NL)，构建 6.2.200067 | `~/FHS/TD_Release_2026.2_NL` | `td` | `td-fhs`（进入 FHS bash） |
| Siemens Questa FPGA Edition | 25.1std | `~/FHS/altera/25.1std/questa_fse` | `questa` | `questa-fhs`（进入 FHS bash） |

- 两者由 `~/Projects/Nix/nixos-lf/system/programs/fhs/{td,questa}.nix` 定义为 `buildFHSEnvBubblewrap`。
- `td` / `questa` 直接拉起 GUI；`td-fhs` / `questa-fhs` 打开一个已注入厂商库路径（`LD_LIBRARY_PATH`、`PATH`、Qt 插件、`SALT_LICENSE_SERVER`）的 bash 沙箱，适合批处理。
- FHS 沙箱只暴露 vendor 库，不用系统 `LD_LIBRARY_PATH` 修补；缺库报错应加进对应 `.nix` 的 `targetPkgs`。

## 2. TD Tcl TCP 服务（供 fpga-mcp 驱动）

opencode 的 `fpga` MCP（anlogic 后端）通过 127.0.0.1:9997 的 JSON-over-TCP 服务驱动 TD。该服务由 TD 内部 `source` 一个 Tcl 脚本启动：

```tcl
# /tmp/opencode/td_anlogic_server.tcl
source /home/lfour/Projects/fpga-mcp/tcl/anlogic_server.tcl
```

### 2.1 常驻启动

普通 `&` 后台进程会随启动它的 shell 退出而被清理。必须用 systemd 用户级瞬态单元启动，才能在 agent 会话结束后继续存活：

```bash
systemd-run --user --unit=td-tcl-server --collect --no-block \
  td-fhs -c 'cd /home/lfour/FHS/TD_Release_2026.2_NL && exec ./bin/td.sh /tmp/opencode/td_anlogic_server.tcl'
# 等待端口
for i in $(seq 1 30); do ss -tlnp | grep -q 9997 && break; sleep 2; done
ss -tlnp | grep 9997
```

重启（TD 异常退出后）：

```bash
systemctl --user reset-failed td-tcl-server.service 2>/dev/null
# 再执行上面的 systemd-run
```

### 2.2 驱动方式

- fpga MCP 的 `a_*` 工具：仅部分命令在 TD 2026.2 存在，且 MCP 会缓存 TCP socket。TD 进程一旦异常退出，MCP 不会自动重连（`connect()` 在 `_sock` 非空时直接返回），后续工具全部报 `Broken pipe`。
- 直连脚本：直接用 JSON 协议连 9997，见 §7 的 `td.py`。协议：请求 `{"id":N,"cmd":"<tcl>","timeout":T}\n`，响应 `{"id":N,"ok":bool,"result":"...","error":"..."}\n`，连接后先收一行 `READY ...`。

## 3. 编译流程（Tcl）

在 TD Tcl 服务里按下列顺序执行。顺序与工作目录见下方要点：

```tcl
cd /path/to/fpga/<prj>                      ;# 必须先进入工程目录，见 §6
import_device dr1_90.db -package DR1M90GEG400 -speed 2
open_project <prj>.al
elaborate -top <top>
read_adc  uisrc/04_pin/fpga_pin.adc         ;# 必须在 elaborate 之后
read_sdc  uisrc/04_pin/timing.sdc
insert_debugger                             ;# 例化 ChipWatcher
optimize_rtl
optimize_gate
place
route
bitgen -bit <top>.bit
```

要点：

- 器件库名固定为 `dr1_90.db`，封装 `DR1M90GEG400`，速度 `-2`。对应厂商工程 `*_Runs/*/settings.cfg` 的 `device_name` / `package_name` / `speed`。
- `read_adc` / `read_sdc` 必须在 `elaborate` 之后：设计未建立时调用返回错误，IP 只落位一次。
- 不调用 `read_adc` 的直接后果是 `BIT-8123 ERROR: Bitgen is prohibited as there are unconstrained io(s)`。
- 产物：`<top>.bit`（在工程目录）、`final_timing.rpt`、`cw/`（ChipWatcher 中间文件）。
- 时序看 `final_timing.rpt` 的 `SWNS/STNS/HWNS/HTNS`，0 违例即可继续。

## 4. 下载与 ChipWatcher

### 4.1 启动 hwserver

JTAG 走本地 hwserver（FTDI 电缆 VID:PID `0403:6042`，监听 5555）。同样用 systemd 用户单元常驻：

```bash
systemd-run --user --unit=hwserver --collect --no-block \
  td-fhs -c 'cd /home/lfour/FHS/TD_Release_2026.2_NL && LD_LIBRARY_PATH=./lib:./lib/Qt/lib exec ./bin/hwserver -r'
ss -tlnp | grep 5555
```

hwserver 长时间运行会异常（CPU 满载、`PRG-8000 Connect server failed`），插拔 JTAG 后须重启该单元。

### 4.2 下载

```tcl
connect_cable -host_name 127.0.0.1:5555
download -mode jtag -bit /abs/path/<top>.bit -cable 0
```

成功标志：日志出现 `PRG-2014 : Chip validation success: DR1M90`。

### 4.3 ChipWatcher 抓取

```
get_hw_cws <实例名>                 ;# 返回句柄，仅在本次已重建设计的会话内有效
run_hw_cw  [get_hw_cws <实例名>] -cable 0
wait_on_hw_cw [get_hw_cws <实例名>]
write_hw_cw_data [get_hw_cws <实例名>] <out.csv>
```

注意：

- `get_hw_cws` 依赖当前会话里已 `bitgen` 的设计，重启 TD 后须重跑 §3。
- 不得调用 `report_cwc_status` / `export_chipwatcher`：在 TD 2026.2 上会触发原生断言（`ReportCwcStatus` / `xml_parser_error`），TD 进程异常退出。
- 只发生一次的事务（如某类仅上电执行的读）在自由触发抓取里通常抓不到，需以该事务的内部脉冲作触发，或把该读改为周期性后再抓。

## 5. Questa 仿真

RTL 无 `timescale`，时标由 TB 提供。批处理：

```bash
questa-fhs -c 'vlib work && vlog -sv rtl/*.v tb/*.v && vsim -c work.<tb_top> -do "run -all; quit -f"'
```

- `vsim` 输出中 `%0t` 默认按精度（ps）打印；`$time` 返回模块时间单位，勿混用。
- 选中信号用 `-voptargs=+acc`，否则 `$dumpvars` 只导出被保留的信号。
- 许可由 `SALT_LICENSE_SERVER` 指向 `~/FHS/altera/25.1std/questa_fse/license/*.dat`，由 `questa.nix` 在沙箱启动时自动导出。

## 6. TD 异常退出与规避

以下现象出现在 TD Release 2026.2 (NL)（构建 6.2.200067），运行于 NixOS 26.05 (Yarara) 的 FHS 沙箱；其他 TD 版本 / 平台未必相同。

| 现象 | 根因 | 规避 |
|---|---|---|
| `elaborate` 使 TD 段错误退出 | 未 `import_device`（`GlobalDesign.chip()==nullptr`）；ChipWatcher IP 模块未进设计（`HDL-8007 ... is a black box`）；或上一次编译的 `*_Runs` 残留 | 先 `import_device`；从工程目录 `open_project`（见下条）；必要时把 IP 的 RTL（`ui_cwc.v`、`ChipWatcher_ui_cwc.sv`）加入工程；删除残留的 `*_Runs` 后重跑 |
| `PRJ-9210 ERROR: The XML file al_ip/.../ui_cwc.xml does not exist` | TD 按**当前工作目录**（而非 `.al` 所在目录）解析 IP 路径 | 先 `cd` 到工程目录，再 `open_project <prj>.al` |
| `report_cwc_status` / `export_chipwatcher` 使 TD 异常退出 | TD 2026.2 原生缺陷 | 不调用；需要状态时读编译日志与 `cw/` 目录 |
| `run_hw_cw`（ChipWatcher 触发抓取）使 TD 段错误退出 | TD 2026.2 在 `WatcherInst::StartTrig` → `UpdateControlRegForMultiWindowVersion` 崩溃 | 用 TD GUI 抓取并导出 CSV；或换其他 TD 版本 / 平台执行 |
| MCP 工具全部 `Broken pipe` | MCP 缓存的 socket 指向已退出的 TD 进程 | 直接用 §7 脚本；或重启 opencode 使 MCP 重建连接 |
| Tcl 命令返回值被报成 error | fpga-mcp 的 omni 服务对脚本里的 `return` 处理异常 | 用 `set rc [catch {cmd} m]; list rc=$rc m=$m` 替代 `return` |
| 编译后 `place`/`route` 报 rc=1 但 `bitgen` 成功 | 对已布局设计重复 `place` | 每次改动后从 `elaborate` 起重跑，或在干净会话内一次跑完 |

## 7. 直连 TD 的 Tcl 客户端

`/tmp/opencode/td.py` 提供极简 JSON-over-TCP 客户端，绕过 MCP 的 socket 缓存问题：

```python
from td import TD
td = TD()                                   # 连 127.0.0.1:9997，读取 READY 行
print(td.cmd("pwd", timeout=30))            # 发一条 Tcl，收 JSON 响应
td.close()
```

配套常驻服务重启后，即可用该客户端执行 §3、§4 的所有 Tcl 命令。

## 8. 相关文件

| 内容 | 路径 |
|---|---|
| TD 定义 | `~/Projects/Nix/nixos-lf/system/programs/fhs/td.nix` |
| Questa 定义 | `~/Projects/Nix/nixos-lf/system/programs/fhs/questa.nix` |
| fpga-mcp Tcl 服务脚本 | `~/Projects/fpga-mcp/tcl/anlogic_server.tcl` |
| TD Tcl 命令权威手册 | `~/FHS/TD_Release_2026.2_NL/doc/SWUG/SWUG105_TCL_Command_User_Guide.pdf` |
| 调试工具手册 | `~/FHS/TD_Release_2026.2_NL/doc/SWUG/SWUG111_Debug_Tools_User_Guide.pdf` |
| 厂商参考工程（`.al` / `.adc` / `.sdc`、`settings.cfg`） | `MLK-F3P-DR1M90G-Resource/03_demo/3-1_ex_fpga/` |
