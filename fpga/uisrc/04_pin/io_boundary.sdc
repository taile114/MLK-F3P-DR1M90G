# On-chip pad/register routing budget: one 100 MHz period.
# Datapath-only bounds assert no external sensor/clock phase relationship.
# Board delay and external device setup/hold require separate physical validation.
# SoC timing.sdc adds a 1 ns placeholder input/output delay so the sensor
# pads have a clocked reference. That value is a board budget placeholder,
# not a measured trace/device delay; see docs/PL-PS接入与上板交付.md.
set_max_delay 10.000 -datapath_only -from [get_ports {I_uart_rx I_int1 I_rs485_rx IO_spi_sdio IO_iic_scl IO_iic_sda}] -to [get_regs {*}]
set_max_delay 10.000 -datapath_only -from [get_regs {*}] -to [get_ports {O_spi_sclk O_spi_cs_n O_rs485_tx O_rs485_de IO_spi_sdio IO_iic_scl IO_iic_sda O_led[*] O_alarm}]
