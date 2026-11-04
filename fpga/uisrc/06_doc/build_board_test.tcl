# 板测顶层 board_test_top 的 TD 构建：read_hdl -> optimize_rtl/gate -> place -> route -> bitgen。
# 产出可下载码流 fpga/uisrc/out/board_test/board_test.bit：上板只接 JTAG 下载该码流，
# 不需要 PS 参与，看板上四只 LED 即可判定 PL 特征链路是否算得正确。
#
# 门禁（任一不满足即 exit 1）：
#   * check_timing 报告 no clock / invalid constraint / combinational loops /
#     no path constraints 四类问题均为 0；
#   * STA coverage = 100%（本设计只有 sys_clk 一个时钟域，端点全部可分析）；
#   * route 后 SWNS、HWNS 均 >= 0。
#
# 用法：
#   td-fhs -c 'cd /home/lfour/FHS/TD_Release_2026.2_NL && \
#              ./bin/td.sh /home/lfour/Projects/MLK-F3P-DR1M90G/fpga/uisrc/06_doc/build_board_test.tcl'
# 报告与中间文件写到 fpga/uisrc/out/board_test/。

# 本脚本在 fpga/uisrc/06_doc/ 下，故 root=fpga/uisrc、repo=仓库根。
set root [file normalize [file join [file dirname [info script]] ..]]
set repo [file normalize [file join $root ../..]]
set out  [file join $root out board_test]
file mkdir $out
cd $out
file delete -force route_timing_summary.rpt route.area check_timing.rpt board_test.bit

set sources [list \
    [file join $root 06_doc board_test_top.v] \
    [file join $root 01_rtl features sensor_features.v] \
    [file join $root 01_rtl features time_features.v] \
    [file join $root 01_rtl features unsigned_divider.v] \
    [file join $root 01_rtl features unsigned_sqrt.v] \
    [file join $root 01_rtl soc vibration_fft.sv] \
    [file join $root 01_rtl fft fft_radix2.v] \
    [file join $root 01_rtl fft fft_coeff_rom.v] \
]
# board_test_vectors.vh 由 board_test_top.v 用相对路径 include，随源文件目录解析，
# 不需要额外的 include 搜索路径。

if {[catch {
    foreach f $sources {
        if {![file exists $f]} { error "source missing: $f" }
    }
    import_device dr1_90.db -package DR1M90GEG400 -speed 2
    read_hdl -file [join $sources " "] -top board_test_top
    read_adc [file join $root 04_pin board_test.adc]
    read_sdc [file join $root 04_pin timing.sdc]
    optimize_rtl
    optimize_gate
    place
    route
    update_timing -mode final
    report_timing_summary -file route_timing_summary.rpt
    report_area -file route.area
    check_timing -file check_timing.rpt -verbose

    # ---- check_timing 门禁：四类问题必须为 0 ----
    set fh [open check_timing.rpt r]
    set checks [read $fh]
    close $fh
    foreach wanted {
        {checking no clock\(0\)}
        {checking invalid constraint\(0\)}
        {checking combinational loops\(0\)}
        {checking no path constraints\(0\)}
    } {
        if {![regexp $wanted $checks]} {
            error "check_timing reports: $wanted"
        }
        puts "RESULT build_board_test: check_timing ok -> $wanted"
    }

    # ---- 时序报告门禁：coverage 100%、SWNS/HWNS >= 0 ----
    set fh [open route_timing_summary.rpt r]
    set timing [read $fh]
    close $fh
    if {![regexp {sys_clk} $timing]} { error "sys_clk clock missing from timing summary" }
    if {![regexp {STA coverage *: *([0-9.]+)%} $timing unused coverage]} {
        error "STA coverage field missing"
    }
    if {$coverage < 100.0} { error "STA coverage = $coverage%, expected 100%" }
    puts "RESULT build_board_test: STA coverage = $coverage%"
    foreach label {SWNS HWNS} {
        if {![regexp [format {%s: *(-?[0-9]+\.[0-9]+)ns} $label] $timing unused slack]} {
            error "timing field $label missing"
        }
        if {$slack < 0} { error "$label=$slack ns violates timing" }
        puts "RESULT build_board_test: $label = $slack ns"
    }

    # ---- 码流：上板下载用，必须非空 ----
    set t0 [clock milliseconds]
    bitgen -bit board_test.bit
    set ms [expr {[clock milliseconds] - $t0}]
    if {![file exists [file join $out board_test.bit]]} { error "bitstream missing" }
    set bytes [file size [file join $out board_test.bit]]
    if {$bytes == 0} { error "bitstream empty" }
    puts "RESULT build_board_test: bitstream board_test.bit = $bytes bytes, bitgen $ms ms"
} message]} {
    puts stderr "RESULT build_board_test: FAIL $message"
    exit 1
}
puts "RESULT build_board_test: PASS"
exit
