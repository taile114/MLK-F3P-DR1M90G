# tools — 辅助脚本

仓库的自动化脚本，都在仓库根目录下运行。

## sync_board_time.sh

通过调试串口把主机当前 UTC 时间写入板卡，不依赖网络；板卡无 RTC 时，运行一次即可完成本次上电后的对时，也可按固定间隔持续对时。记录文件的时间戳保存在本地，关机不要求 RTC 保持。

单次同步（默认）：

```bash
./tools/sync_board_time.sh --device /dev/ttyACM0 --timeout 120
```

周期同步，每 30 s 重写一次：

```bash
./tools/sync_board_time.sh --device /dev/ttyACM0 --interval 30
```

周期模式每轮重新打开串口、确认 Linux shell 后写入主机时间并校验，完成后关闭句柄。板卡重启、串口重新枚举或登录失效时重试，不会退出循环。脚本默认登录 `root`/`root`；可用 `BOARD_TTY` / `BOARD_USER` / `BOARD_PASSWORD` 覆盖。

U-Boot 与内核启动输出期间只接收，不向自动启动倒计时或 `=>` 提示符发送字符。识别到 Linux 登录提示后才发送用户名和密码；未知且安静的控制台等待至少 5 s 后只探测一次，避免回车中断 3 s 的自动启动倒计时。该行为已通过模拟串口测试及板卡重启验证。

| 参数 | 默认 | 说明 |
|---|---|---|
| `--device` | `/dev/ttyACM0` | 调试串口设备；`--timeout` 为登录超时秒数 |
| `--interval SECONDS` | `0` | `0` 表示单次同步；非 0 时每隔 SECONDS 秒重新对时，Ctrl-C 退出 |
| `--count N` | `0` | `0` 表示不限次数；周期模式下完成 N 次后退出 |

每轮执行 `date -u -s`（周期模式写作 `date -u -s @<epoch>`），若板上有 `/dev/rtc0` 再执行 `hwclock -w`，并校验板端 epoch 与主机相差不超过 5 s。失败语义：单次模式立即以非零码退出；周期模式单轮失败打印到标准错误、退避 1～5 s 后重连重试，直到 Ctrl-C（正常退出，打印已完成同步次数）或完成 `--count` 次数。板端时区按 `CST-8` 显示，脚本写入的一直是 UTC epoch。

## install_board_timesync.sh

把周期对时装成 systemd 用户服务，插上串口即自动同步，无需手动起命令。

```bash
./tools/install_board_timesync.sh install   # 写入 ~/.config/systemd/user 并 enable --now
./tools/install_board_timesync.sh status    # 查看状态与最近日志
./tools/install_board_timesync.sh remove     # 停用并删除单元
```

单元 `mlk-board-timesync.service` 以 `Restart=always` 运行 `sync_board_time.sh --interval 30`，设备默认 `/dev/ttyACM0`（用 `BOARD_TTY` 覆盖，间隔用 `INTERVAL`）。日志用 `journalctl --user -u mlk-board-timesync -f` 查看。

手动登录串口前先停止服务，关闭手动串口会话后再恢复服务，避免两个进程竞争串口数据：

```bash
systemctl --user stop mlk-board-timesync.service
# 运行手动串口会话，退出后恢复服务
systemctl --user start mlk-board-timesync.service
```

串口启动保护与周期重连测试：

```bash
python3 -m unittest discover -s tools/tests -p test_sync_board_time.py -v
```

## sync_lceda.sh

把正在编辑的 LCEDA 工程落盘成入库快照并提交。

- 来源：`~/Documents/LCEDA-Pro/projects/MLK-F3P.eprj2`
- 目标：`eda/MLK-F3P.eprj2`
- 前置：先在 LCEDA 内保存到磁盘（`eda.sch_Document.save()` / `eda.pcb_Document.save()`）。

```bash
./tools/sync_lceda.sh "描述这次改动"
```

无改动时直接退出，不产生空提交。

## make_arch_svg.py

生成 [`../docs/MLK-F3P-DR1M90G-方案B-系统架构图.svg`](../docs/MLK-F3P-DR1M90G-方案B-系统架构图.svg)。
用与 SVG 相同的字体实测每行文字宽度来定框宽，保证文字不出框、不叠压。

```bash
nix-shell -p python3Packages.pillow noto-fonts-cjk-sans fontconfig \
  --run "python3 tools/make_arch_svg.py"
```

## make_report_pptx.py

生成《MLK-F3P-DR1M90G 外设方案报告》PPTX（16:9）。
配图取 `~/Pictures/Screenshots/*.jpg`，系统架构图由脚本用 PIL 现画。

```bash
nix-shell -p python3Packages.python-pptx python3Packages.pillow noto-fonts-cjk-sans fontconfig \
  --run "python3 tools/make_report_pptx.py"
```

可选排版校验，需要 libreoffice + poppler-utils：先把 pptx 转成 pdf，再转 png 逐页检查。
