# fpga_top only: RX is asynchronous; retain timing between synchronizer flops.
set_false_path -from [get_ports {I_rs485_rx}] -to [get_regs {X_encoder_modbus.uart.rx_ff[0]}]
# Serial outputs have no synchronous external capture clock. This bounds only
# internal register-to-pad routing, not external setup/hold timing.
set_max_delay 10.000 -datapath_only -from [get_regs {*}] -to [get_ports {O_rs485_tx O_rs485_de O_uart_tx}]
