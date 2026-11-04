# TD 全流程构建（工程模式）。
# 运行（NixOS, 见 docs/NixOS下TD与Questa使用.md）：
#   td-fhs -c 'cd /home/lfour/FHS/TD_Release_2026.2_NL && ./bin/td.sh <绝对路径>/build_td.tcl'
# 验收要求：STA 100%、SWNS/HWNS 非负，生成独立采集验证码流。
set root [file normalize [file join [file dirname [info script]] ..]]
set out  [file join $root out td]
file mkdir $out
cd $root

file delete -force [file join $root fpga_Runs]
# 清理本脚本生成的报告/码流，避免错误后读取残留产物。
foreach name {route_timing_summary.rpt route.area fpga_top.bit} {
    file delete -force [file join $out $name]
}
if {[catch {
import_device dr1_90.db -package DR1M90GEG400 -speed 2
open_project fpga.al
elaborate -top fpga_top
read_adc 04_pin/fpga_pin.adc
read_adc 04_pin/encoder_pin.adc
read_sdc 04_pin/timing.sdc
read_sdc 04_pin/io_boundary.sdc
read_sdc 04_pin/encoder.sdc
optimize_rtl
optimize_gate
place
route
update_timing -mode final
report_timing_summary -file "$out/route_timing_summary.rpt"
report_area -file "$out/route.area"
check_timing -file "$out/check_timing.rpt" -verbose
set f [open [file join $out route_timing_summary.rpt] r]
set timing [read $f]
close $f
if {![regexp {clk[0-9]+: +100\.000 +[0-9]+ +sys_clk} $timing]} {
    error "sys_clk at 100 MHz missing from timing report"
}
if {![file exists [file join $out route.area]]} { error "Area report missing" }
if {![regexp {STA coverage *: *100\.00%} $timing]} { error "STA coverage is not 100%" }
foreach label {SWNS HWNS} {
    if {![regexp [format {%s: *(-?[0-9]+\.[0-9]+)ns} $label] $timing unused slack]} {
        error "Timing field $label missing"
    }
    if {$slack < 0} { error "$label=$slack ns violates timing" }
}
bitgen -bit "$out/fpga_top.bit"
if {![file exists [file join $out fpga_top.bit]]} { error "Bitstream missing" }
if {[file size [file join $out fpga_top.bit]] == 0} { error "Bitstream empty" }
} message]} {
    puts stderr "RESULT build_td: FAIL $message"
    exit 1
}
puts "RESULT build_td: PASS"
exit
