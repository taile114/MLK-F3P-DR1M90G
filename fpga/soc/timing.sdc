create_clock -name sys_clk -period 10.000 [get_ports I_sysclk]
# PS template p2f_clk0 = 50 MHz (period 20 ns), p2f_clk1 = 100 MHz (period 10 ns);
# values from the generated Design.xml (Actual_IO_Frequency 49.995 / 99.99 MHz).
# Constrain at the top-level clock nets driven by the PS macro, not at a
# non-existent hierarchical pin.
create_clock -name axi_clk -period 20.000 [get_nets {w_port__ARM_Processor_System_0__p2f_clk0}]
# p2f_clk1 (100 MHz) is not constrained: prepare_soc.py removes the vendor
# reset block it fed, and that block's outputs were open in the template, so
# the PS macro's p2f_clk1 output has no load in the generated netlist.
# Board I/O budget: 1 ns placeholder (TBD on hardware). The sensor pads are
# asynchronous; this budget only gives every I/O endpoint a clocked reference
# so check_timing and STA coverage do not treat them as unconstrained.
set_input_delay -clock sys_clk -max 1.000 [get_ports {I_uart_rx I_int1 I_rs485_rx IO_spi_sdio IO_iic_scl IO_iic_sda}]
set_input_delay -clock sys_clk -min 0.000 [get_ports {I_uart_rx I_int1 I_rs485_rx IO_spi_sdio IO_iic_scl IO_iic_sda}]
set_output_delay -clock sys_clk -max 1.000 [get_ports {O_spi_sclk O_spi_cs_n O_rs485_tx O_rs485_de IO_spi_sdio IO_iic_scl IO_iic_sda O_led[*]}]
set_output_delay -clock sys_clk -min 0.000 [get_ports {O_spi_sclk O_spi_cs_n O_rs485_tx O_rs485_de IO_spi_sdio IO_iic_scl IO_iic_sda O_led[*]}]
# uii2c uses I_clk enables; no internal generated clock.
# Bound bundled-data and Gray-pointer routing. No broad clock-group exception.
set_max_delay 10.000 -datapath_only -from [get_regs {*S_hold*}] -to [get_regs {*O_data*}]
set_max_delay 10.000 -datapath_only -from [get_regs {*S_wgray*}] -to [get_regs {*S_wsync1*}]
set_max_delay 10.000 -datapath_only -from [get_regs {*S_rgray*}] -to [get_regs {*S_rsync1*}]
set_max_delay 10.000 -datapath_only -to [get_regs {*S_req_sync\[0\] *S_ack_sync\[0\] *S_enable_sync\[0\] *S_overflow_sync\[0\] *S_push_sync\[0\] *S_xack_sync\[0\]}]

# U7 alarm is generated in the 50 MHz AXI domain of this prototype.
set_output_delay -clock axi_clk -max 1.000 [get_ports {O_alarm}]
set_output_delay -clock axi_clk -min 0.000 [get_ports {O_alarm}]
