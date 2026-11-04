# 独立工程验证全部新增数据通路；不创建 SoC，不下载码流。
set root [file normalize [file join [file dirname [info script]] ..]]
set out [file join $root out validation]
file mkdir $out
set f [open [file join $root fpga.al] r]
set project [read $f]
close $f
set project [string map [list {Path="} "Path=\"$root/" \
    {<Name>fpga</Name>} {<Name>validation</Name>} \
    {<LABEL>fpga_top</LABEL>} {<LABEL>validation_top</LABEL>} \
    {<MODULE>fpga_top</MODULE>} {<MODULE>validation_top</MODULE>} \
    {        </Verilog>} "            <File Path=\"$root/06_doc/validation_top.v\"><FileInfo><Attr Name=\"UsedInSyn\" Val=\"true\"/><Attr Name=\"UsedInP&R\" Val=\"true\"/><Attr Name=\"BelongTo\" Val=\"design_1\"/><Attr Name=\"CompileOrder\" Val=\"25\"/></FileInfo></File>\n        </Verilog>"] $project]
set f [open [file join $out validation.al] w]
puts $f $project
close $f
set f [open [file join $out validation.adc] w]
puts $f {set_pin_assignment { I_sysclk } { LOCATION = H16; IOSTANDARD = LVCMOS33; PULLTYPE = PULLUP; }}
puts $f {set_pin_assignment { O_led[0] } { LOCATION = T19; IOSTANDARD = LVCMOS18; PULLTYPE = NONE; }}
puts $f {set_pin_assignment { O_led[1] } { LOCATION = R19; IOSTANDARD = LVCMOS18; PULLTYPE = NONE; }}
puts $f {set_pin_assignment { O_led[2] } { LOCATION = J15; IOSTANDARD = LVCMOS33; PULLTYPE = NONE; }}
puts $f {set_pin_assignment { O_led[3] } { LOCATION = M20; IOSTANDARD = LVCMOS33; PULLTYPE = NONE; }}
close $f
cd $out
file delete -force [file join $out validation_Runs]
# 清理本脚本生成的报告/码流，避免错误后读取残留产物。
foreach name {route_timing_summary.rpt route.area validation.bit} {
    file delete -force [file join $out $name]
}
if {[catch {
import_device dr1_90.db -package DR1M90GEG400 -speed 2
open_project validation.al
elaborate -top validation_top
read_adc validation.adc
read_sdc ../../04_pin/timing.sdc
optimize_rtl
optimize_gate
place
route
update_timing -mode final
report_timing_summary -file route_timing_summary.rpt
report_area -file route.area
check_timing -file check_timing.rpt -verbose
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
bitgen -bit validation.bit
if {![file exists [file join $out validation.bit]]} { error "Bitstream missing" }
if {[file size [file join $out validation.bit]] == 0} { error "Bitstream empty" }
} message]} {
    puts stderr "RESULT build_validation: FAIL $message"
    exit 1
}
puts "RESULT build_validation: PASS"
exit
