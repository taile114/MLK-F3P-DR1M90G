#!/usr/bin/env python3
"""Reproduce the STA-coverage control build from the untouched vendor example.

The coverage residual recorded in docs/PL-PS接入与上板交付.md §2.2 is measured by
building the vendor's own 20_axi_lite_register design without editing a single
vendor file. This script only collects that design's RTL, writes the clock
constraint, and emits a headless TD build script; it never writes into the
resource pack.
"""
from pathlib import Path
import xml.etree.ElementTree as ET

REPO = Path(__file__).resolve().parents[2]
SRC = (REPO / 'docs/MLK-F3P-DR1M90G-Resource/03_demo/3-2_ex_fpsoc'
       / '3_2_01_sdk_base/20_axi_lite_register/soc_prj')
SYS = SRC / 'uisrc/01_rtl/system'
OUT = REPO / 'fpga/uisrc/out/vendor_ctrl'
TOP = 'system_top'
# The top is the system_top wrapper, so the PS output clock net sits one level
# down. Clocking the un-prefixed net (or the w_net__ alias) leaves the clock
# undefined and drops the reported coverage to 83.97%.
CLOCK_NET = 'system/w_port__ARM_Processor_System_0__p2f_clk0'


def require(condition, message):
    if not condition:
        raise SystemExit('vendor_coverage_control: ' + message)


require(SYS.is_dir(), 'vendor example not found; install the resource pack first')

sources = []
for ip in sorted((SYS / 'ip').iterdir()):
    if ip.name.startswith('.'):
        continue
    xml = ip / (ip.name + '.xml')
    if not xml.is_file():
        continue
    for el in ET.parse(xml).getroot().iter():
        if not el.tag.endswith('}file'):
            continue
        name = next((x.text for x in el if x.tag.endswith('}name')), None)
        if not name or not name.endswith(('.v', '.sv')) or Path(name).name == 'define.v':
            continue
        path = ip / name
        if path.is_file() and path not in sources:
            sources.append(path)
for extra in ('hdl/system.v', 'hdl/system_wrapper.v'):
    path = SYS / extra
    require(path.is_file(), 'missing ' + str(path))
    if path not in sources:
        sources.append(path)
require(len(sources) > 10, 'source collection looks empty; re-check the resource pack')

OUT.mkdir(parents=True, exist_ok=True)
(OUT / 'sources.list').write_text('\n'.join(str(p) for p in sources) + '\n')

(OUT / 'timing.sdc').write_text(
    '# Reference clock for the PS macro, same form as fpga/soc/timing.sdc.\n'
    'create_clock -name axi_clk -period 20.000 [get_nets {' + CLOCK_NET + '}]\n'
    'derive_clocks\n')

adc = SRC / 'uisrc/04_pin/pin.adc'
require(adc.is_file(), 'missing ' + str(adc))
(OUT / 'build_vendor_ctrl.tcl').write_text('''set out [file normalize [file join [file dirname [info script]]]]
cd $out
if {[catch {
import_device dr1_90.db -package DR1M90GEG400 -speed 2
set f [open sources.list r]
set sources [split [string trim [read $f]] "\\n"]
close $f
read_hdl -file [join $sources " "] -top ''' + TOP + '''
read_adc ''' + str(adc) + '''
read_sdc timing.sdc
optimize_rtl
optimize_gate
place
route
update_timing -mode final
report_timing_summary -file route_timing_summary.rpt
check_timing -file check_timing.rpt -verbose
set f [open route_timing_summary.rpt r]
set timing [read $f]
close $f
puts "VENDOR_CTRL: [regexp -inline {STA coverage *: *[0-9.]+%} $timing]"
puts "VENDOR_CTRL: [regexp -inline {SWNS: *-?[0-9.]+ns} $timing]"
puts "VENDOR_CTRL: [regexp -inline {HWNS: *-?[0-9.]+ns} $timing]"
} message]} {
    puts stderr "RESULT vendor_coverage_control: FAIL $message"
    exit 1
}
puts "RESULT vendor_coverage_control: PASS"
exit
''')

print('sources:', len(sources))
print('workspace:', OUT)
print('run: td-fhs -c "cd /home/lfour/FHS/TD_Release_2026.2_NL && ./bin/td.sh ' + str(OUT) + '/build_vendor_ctrl.tcl"')
