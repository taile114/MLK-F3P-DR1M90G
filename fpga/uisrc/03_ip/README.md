# 厂商 IP

本目录为从米联客参考工程复制的总线 IP。`uiuart_rx.v`、`ui_mspi_tx.v`、`ui_spi_rx.v` 保持厂商源文件不变；`uii2c.v` 按本工程需要改为单时钟使能实现，差异见下文。四个文件均自带 `timescale`；本工程自写 RTL 不声明 `timescale`，仿真时标由测试平台提供。

| 文件 | 模块名 | 来源参考工程 | 用途 |
|---|---|---|---|
| `uiuart_rx.v` | `ui_uart_rx` | `03_demo/3-1_ex_fpga/.../11_uart_rx_driver/uisrc/03_ip/uiuart_rx.v` | UART 8 倍采样接收，供温度通道 |
| `ui_mspi_tx.v` | `ui_mspi_tx` | `.../14_spi_master_tx_driver/` | SPI 发送，供振动通道 |
| `ui_spi_rx.v` | `ui_spi_rx` | `.../15_spi_rx_driver/` | SPI 接收，供振动通道 |
| `uii2c.v` | `uii2c` | `.../19_i2c_eeprom/uisrc/03_ip/uii2c.v` | I2C 主机，供电流通道 |

## 使用说明

- `uiuart_rx` 的文件名与模块名不一致，模块实际为 `ui_uart_rx`。厂商核在停止位中段产生字节有效脉冲，`uart_rx_guard.v` 在其上增加输入同步、停止位检查、持续低电平检测和空闲恢复。
- `ui_mspi_tx` / `ui_spi_rx` 由 `spi_adxl345.v` 例化，收发均由系统时钟分频，接收侧对 SCLK 与片选做多级寄存消亚稳态。
- `uii2c` 配置为最大写 4 字节、读 2 字节。厂商 SCL 输出在顶层转换为开漏；SDA 使用厂商三态输出。控制器不支持时钟拉伸，ADS1115 不执行时钟拉伸。
- `uii2c.v` 已改为用 `I_clk` 的 SCL 上升 / 下降沿使能驱动事务，`scl_clk` 只保留为观测电平，不再作为寄存器时钟。正常与故障恢复仿真均通过，实板时序待上板确认。

厂商源文件的编码与换行保持不变，见 `../.gitattributes`。

## ChipWatcher 目录

[`ui_cwc/`](ui_cwc/) 是早期 ChipWatcher 调试工程留下的 IP 目录，当前 [`fpga.al`](../fpga.al) 与 `build_td.tcl` 均不引用，生产顶层也不例化。保留该目录只用于查阅历史探针配置；需要新的上板抓取时，按 [`../06_doc/README.md`](../06_doc/README.md) §3 在调试副本中单独生成，不进入主线构建。
