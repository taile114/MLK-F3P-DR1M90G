#!/usr/bin/env bash
# 统一工程的 Icarus Verilog 仿真入口。
# 依赖 Nix devShell 中的 iverilog / vvp（见 flake.nix），或系统 PATH 中的同名工具。
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$root/out/sim"
mkdir -p "$out"
cd "$root"

rtl=(
    "$root/01_rtl/uart_rx_guard.v"
    "$root/01_rtl/temp_frame_parser.v"
    "$root/01_rtl/spi_adxl345.v"
    "$root/01_rtl/adxl345_ctrl.v"
    "$root/01_rtl/ads1115_ctrl.v"
)
ip=(
    "$root/03_ip/uiuart_rx.v"
    "$root/03_ip/ui_mspi_tx.v"
    "$root/03_ip/ui_spi_rx.v"
    "$root/03_ip/uii2c.v"
)
encoder=("$root"/01_rtl/encoder_*.v)

run_tb() {
    local tb="$1"; shift
    local name="$tb" arg
    for arg in "$@"; do
        if [[ "$arg" == *=* ]]; then name+="_${arg##*.}"; fi
    done
    echo "== $name =="
    local source="$root/02_sim/$tb.v"
    [[ -f "$source" ]] || source="$root/02_sim/$tb.sv"
    iverilog -g2012 -Wall -Wno-timescale -s "$tb" \
        -o "$out/$name.vvp" "$source" "$@" \
        2>&1 | tee "$out/$name.compile.log"
    vvp "$out/$name.vvp" 2>&1 | tee "$out/$name.log"
    if grep -Eq 'FAIL[=: ]+[1-9]|\[FAIL\]|^FAIL|FATAL|ERROR:' "$out/$name.log"; then
        echo "仿真失败：$tb" >&2
        return 1
    fi
}

# 温度：GT-M601 串口板
run_tb tb_uart_rx    "${rtl[0]}" "$root/02_sim/temp_uart_source.v" "${ip[0]}" "$root/02_sim/ui_cwc_stub_temp.v"
run_tb tb_temp_parser "${rtl[0]}" "${rtl[1]}" "${ip[0]}" "$root/02_sim/ui_cwc_stub_temp.v"
run_tb tb_temp_test  "${rtl[0]}" "${rtl[1]}" "$root/02_sim/temp_test.v" "$root/02_sim/temp_uart_source.v" "${ip[0]}" "$root/02_sim/ui_cwc_stub_temp.v"

# 振动：ADXL345 三线 SPI
run_tb tb_fast       "$root/02_sim/adxl345_test.v" "${rtl[2]}" "${rtl[3]}" "${ip[1]}" "${ip[2]}" "$root/02_sim/adxl345_slave_model.v" "$root/02_sim/ui_cwc_stub_adxl.v"
run_tb tb_spi_l3     "$root/02_sim/adxl345_test.v" "${rtl[2]}" "${rtl[3]}" "${ip[1]}" "${ip[2]}" "$root/02_sim/adxl345_slave_model.v" "$root/02_sim/ui_cwc_stub_adxl.v"
run_tb tb_spi_spec   "$root/02_sim/adxl345_test.v" "${rtl[2]}" "${rtl[3]}" "${ip[1]}" "${ip[2]}" "$root/02_sim/adxl345_slave_model.v" "$root/02_sim/ui_cwc_stub_adxl.v"

# 电流：ADS1115 I2C
run_tb tb_ads1115        "$root/02_sim/ads1115_test.v" "${rtl[4]}" "${ip[3]}" "$root/02_sim/ads1115_i2c_slave_model.v" "$root/02_sim/ui_cwc_stub_ads.v"
run_tb tb_ads1115_faults "$root/02_sim/ads1115_test.v" "${rtl[4]}" "${ip[3]}" "$root/02_sim/ads1115_i2c_slave_model.v" "$root/02_sim/ui_cwc_stub_ads.v"

# 转速：UART、Modbus RTU 故障恢复、有符号换算；统一顶层遥测在下方验证。
run_tb tb_encoder_uart -I"$root/02_sim" "${encoder[@]}"
run_tb tb_encoder_rpm -I"$root/02_sim" "${encoder[@]}"
run_tb tb_encoder_modbus -I"$root/02_sim" "${encoder[@]}"

run_tb tb_alarm_sequencer "$root/01_rtl/soc/alarm_sequencer.sv"

# 共同时基及特征、FFT、AXI 本体与实际集成顶层。
timebase="$root/01_rtl/timebase.v"
run_tb tb_timebase "$timebase"
features=("$root"/01_rtl/features/*.v)
fft=("$root"/01_rtl/fft/*.v)
run_tb tb_time_features "${features[@]}"
run_tb tb_time_features "${features[@]}" -P tb_time_features.LOGN=9
run_tb tb_fft "${fft[@]}" -P tb_fft.WINDOW=0
run_tb tb_fft "${fft[@]}"
run_tb tb_fft "${fft[@]}" -P tb_fft.LOGN=9 -P tb_fft.WINDOW=0
run_tb tb_fft "${fft[@]}" -P tb_fft.LOGN=9
run_tb tb_axi_lite_reg "$timebase" "$root/01_rtl/axi_lite_reg.v" "$root/01_rtl/soc/alarm_sequencer.sv" "$root/01_rtl/p2f_irq.v" "${features[@]}"
run_tb tb_fpga_top "$timebase" "$root/02_sim/ui_cwc_stub_top.v" "$root/01_rtl/fpga_top.v" "${rtl[@]}" "${ip[@]}" "${features[@]}" "${encoder[@]}"
run_tb tb_encoder_top -I"$root/02_sim" "$timebase" "$root/01_rtl/fpga_top.v" "${rtl[@]}" "${ip[@]}" "${features[@]}" "${encoder[@]}"
python3 "$root/02_sim/test_monitor.py"

soc=("$root"/01_rtl/soc/*.v "$root"/01_rtl/soc/*.sv)
run_tb tb_cdc_soc "${soc[@]}" "${encoder[@]}"
run_tb tb_sensor_soc_axi "${soc[@]}" "$timebase" "$root/01_rtl/axi_lite_reg.v" "$root/01_rtl/p2f_irq.v" "${features[@]}" "${fft[@]}" "${rtl[@]}" "${ip[@]}" "${encoder[@]}"
# 转速通道接进 SoC（接口 v1.2 §14）：寄存器窗口、跨域快照、DMA 三字记录与打包仲裁。
run_tb tb_speed_channel -I"$root/02_sim" "${soc[@]}" "$timebase" "$root/01_rtl/axi_lite_reg.v" "$root/01_rtl/p2f_irq.v" "${features[@]}" "${fft[@]}" "${rtl[@]}" "${ip[@]}" "${encoder[@]}" "$root/02_sim/axi_slave_mem_model.v"
# 同一通道的闭环集成：真实 RS485 引脚 + Modbus 从站模型，经 CDC 落到寄存器与 DMA 记录。
run_tb tb_speed_soc -I"$root/02_sim" "${soc[@]}" "$timebase" "$root/01_rtl/axi_lite_reg.v" "$root/01_rtl/p2f_irq.v" "${features[@]}" "${fft[@]}" "${rtl[@]}" "${ip[@]}" "${encoder[@]}" "$root/02_sim/axi_slave_mem_model.v"

run_tb tb_vibration_fft "${soc[@]}" "${fft[@]}"
run_tb tb_vibration_fft "${soc[@]}" "${fft[@]}" -P tb_vibration_fft.WINDOW=0
run_tb tb_vibration_fft "${soc[@]}" "${fft[@]}" -P tb_vibration_fft.LOGN=9
run_tb tb_vibration_fft "${soc[@]}" "${fft[@]}" -P tb_vibration_fft.LOGN=9 -P tb_vibration_fft.WINDOW=0

# DMA 写通道：sensor_dma 单元级自检测（AXI4 写通道从机模型）。
run_tb tb_sensor_dma "${soc[@]}" "$root/02_sim/axi_slave_mem_model.v"
run_tb tb_npu_hp1_write_mux "$root/01_rtl/soc/npu_hp1_write_mux.sv"

# 环形 DMA + 块完成中断：0xA8/0xAC/0xB0/0xB4、DMA_CTRL[3] 与 sensor_dma 集成自检。
run_tb tb_dma_ring_irq "${soc[@]}" "$timebase" "$root/01_rtl/axi_lite_reg.v" "$root/01_rtl/p2f_irq.v" "${encoder[@]}" "$root/02_sim/axi_slave_mem_model.v"

# 完整频谱窗口（契约 §4.6）：bin RAM 逐字快照、拷贝期读等待、序号与快照同拍发布。
spectrum_deps=("${soc[@]}" "$timebase" "$root/01_rtl/axi_lite_reg.v" "$root/01_rtl/p2f_irq.v" "${features[@]}" "${fft[@]}" "${rtl[@]}" "${ip[@]}" "${encoder[@]}")
run_tb tb_spectrum_window "${spectrum_deps[@]}"
run_tb tb_spectrum_window "${spectrum_deps[@]}" -P tb_spectrum_window.LOGN=9

# 板测顶层（headless 自检）：固定向量 -> 特征/FFT -> 片上比较与 LED 三态，不接外设。
run_tb tb_board_test -I"$root/06_doc" "$root/06_doc/board_test_top.v" "${features[@]}" "${fft[@]}" "$root/01_rtl/soc/vibration_fft.sv" "$root/01_rtl/soc/reset_sync.sv"

echo "全部仿真完成，日志：out/sim/"
