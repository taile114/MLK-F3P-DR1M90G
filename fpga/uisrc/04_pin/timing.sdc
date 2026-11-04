create_clock -name {sys_clk} -period 10.000 -waveform {0.000 5.000} [get_ports {I_sysclk}]
