#!/usr/bin/env python3
"""Merge vendor HDMI and soft NPU with sensor registers and DMA."""
from pathlib import Path
import argparse
import re
import shutil
import xml.etree.ElementTree as ET
from attach_npu import attach_npu


def require(condition, message):
    if not condition:
        raise SystemExit('prepare_hdmi_soc: ' + message)

REPO = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--resource-root', type=Path, default=REPO / 'docs/MLK-F3P-DR1M90G-Resource')
args = parser.parse_args()
DEMO = args.resource_root.resolve() / '01_start/02_start_Linux/05_application_demo/05-1_QT环境搭建及demo演示/demo/soc_prj'
OUT = REPO / 'fpga/uisrc/out/soc_hdmi'
RTL = REPO / 'fpga/uisrc/01_rtl'
# The sensor AXI slave, the sensor DMA master and PS master GP0 share PLL0 clk0
# (100 MHz); HP1 must run in that same domain. The PS macro exposes no HP reset
# port, so HP1 and the DMA use the PS peripheral reset already feeding the slave.
SENSOR_AXI_CLK = 'w_net__PLL_0__clk0_out'
SENSOR_AXI_RSTN = 'w_net__Processor_System_Reset_0__peripheral_aresetn'
HP1 = 'w_port__ARM_Processor_System_0__slave_hp1_axi_'
# RS485 rpm channel: the slave owns the board transceiver and the three port
# names are fixed by the PL-PS interface contract, so they are passed up
# unchanged and land on the board pins of the fpga_top acquisition project.
# O_uart_tx (PL USB UART telemetry) is passed up only when the slave exposes it,
# so a regeneration never names a port the slave does not have.
SENSOR_RTL = RTL / 'soc/sensor_soc_axi.v'
_sensor_text = SENSOR_RTL.read_text(encoding="utf-8")
_sensor_head = _sensor_text[_sensor_text.index('module sensor_soc_axi'):
                            _sensor_text.index(');', _sensor_text.index('module sensor_soc_axi'))]
RS485_PORTS = ['I_rs485_rx', 'O_rs485_tx', 'O_rs485_de']
if re.search(r'\bO_uart_tx\b', _sensor_head):
    RS485_PORTS.append('O_uart_tx')

require((DEMO / 'soc_prj.al').is_file(), f'vendor HDMI template missing: {DEMO}')
require(OUT.resolve().is_relative_to((REPO / 'fpga/uisrc/out').resolve()), 'output outside fpga/uisrc/out')
shutil.rmtree(OUT, ignore_errors=True)
OUT.mkdir(parents=True)
shutil.copy2(DEMO / 'soc_prj.al', OUT / 'soc_hdmi.al')
for name in ('system', 'uisrc'):
    shutil.copytree(DEMO / name, OUT / name,
                    ignore=shutil.ignore_patterns('*_Runs', 'log', '*.bit', '*.hpf'))

sys_v = OUT / 'system/hdl/system.v'
s = sys_v.read_text(encoding="utf-8")
ports = '''module system (
    input     wire              I_uart_rx,
    input     wire              I_int1,
    output    wire              O_spi_sclk,
    output    wire              O_spi_cs_n,
    inout     wire              IO_spi_sdio,
    inout     wire              IO_iic_scl,
    inout     wire              IO_iic_sda,
    output    wire        [3:0] O_led,
    output    wire              O_alarm,
''' + ('    input     wire              I_rs485_rx,\n'
     '    output    wire              O_rs485_tx,\n'
     '    output    wire              O_rs485_de,\n'
     + ('    output    wire              O_uart_tx,\n' if 'O_uart_tx' in RS485_PORTS else '')) + '''    output    wire        [2:0] O_hdmi_tx_p ,
    output    wire              O_hdmi_clk_p
);'''
s, port_hits = re.subn(r'module system \(\s*output\s+wire\s+\[2:0\]\s+O_hdmi_tx_p\s*,\s*output\s+wire\s+O_hdmi_clk_p\s*\);', ports, s, count=1)
require(port_hits == 1, 'vendor system.v top port pattern not found; re-check the vendor HDMI template')
s = s.replace('); \n', ');\n    wire S_irq;\n    wire S_dma_irq;\n', 1)
require('    wire S_dma_irq;\n' in s, 'could not declare the sensor IRQ wires; re-check the vendor system.v')

ds0 = [('araddr', 32), ('arprot', 3), ('arready', 1), ('arvalid', 1),
       ('awaddr', 32), ('awprot', 3), ('awready', 1), ('awvalid', 1),
       ('bready', 1), ('bresp', 2), ('bvalid', 1), ('rdata', 32), ('rready', 1),
       ('rresp', 2), ('rvalid', 1), ('wdata', 32), ('wready', 1), ('wstrb', 4), ('wvalid', 1)]
decl = ''
for name, width in ds0:
    decl += f'    wire [{width-1}:0] w_net__AXI_Matrix_0__ds0_{name};\n'
# HP1 write channel carried by the sensor DMA; awlen is kept 8 bit here and
# sliced to the PS macro's 4 bit port because bursts are at most 16 beats.
# The vendor PS macro exposes no slave_hp1_axi_aresetn; older/newer revisions
# may, in which case the same PS peripheral reset drives it.
has_hp1_rst = '.slave_hp1_axi_aresetn(' in s
hp1 = [('awaddr', 33), ('awlen', 8), ('awburst', 2), ('awvalid', 1),
       ('awready', 1), ('wdata', 64), ('wstrb', 8), ('wlast', 1), ('wvalid', 1),
       ('wready', 1), ('bresp', 2), ('bvalid', 1), ('bready', 1)]
for name, width in hp1:
    decl += ('    wire [' + str(width-1) + ':0] ' if width > 1 else '    wire ') + HP1 + name + ';\n'
if has_hp1_rst:
    decl += '    wire ' + HP1 + 'aresetn;\n'
s = s.replace('    wire S_irq;\n', '    wire S_irq;\n' + decl, 1)

for name, width in ds0:
    for old in (f'.ds0_{name}()', f'.ds0_{name}(1\'b0)', f'.ds0_{name}(2\'b0)', f'.ds0_{name}(32\'b0)'):
        s = s.replace(old, f'.ds0_{name}(w_net__AXI_Matrix_0__ds0_{name})', 1)
    require(f'.ds0_{name}(w_net__AXI_Matrix_0__ds0_{name})' in s,
            f'DS0 {name} was not connected; re-check the vendor AXI matrix template')

# The vendor template ties the PS HP1 slave to constants. Route the sensor DMA
# write master onto it; the HP1 read channel stays tied off because sensor_dma
# has no read side. The aclk assign is forced onto the sensor AXI clock so the
# DMA, the AXI-Lite slave and PS master GP0 stay in one clock domain.
hp1_ties = [('awaddr', "33'b0"), ('awlen', "4'b0"),
            ('awburst', "2'b0"), ('awvalid', "1'b0"), ('wdata', "64'b0"),
            ('wstrb', "8'b0"), ('wlast', "1'b0"), ('wvalid', "1'b0"), ('bready', "1'b0")]
for name, const in hp1_ties:
    old = f'.slave_hp1_axi_{name}({const})'
    require(old in s, f'HP1 {name} constant tie not found; re-check the vendor PS macro instance')
    new = f'.slave_hp1_axi_{name}({HP1}{name}[3:0])' if name == 'awlen' else f'.slave_hp1_axi_{name}({HP1}{name})'
    s = s.replace(old, new, 1)
# awsize 为 2 bit 且恒为 8 字节拍宽：直接接 2'b11（sensor_dma 固定输出 3'b011）。
old = ".slave_hp1_axi_awsize(2'b0)"
require(old in s, 'HP1 awsize constant tie not found; re-check the vendor PS macro instance')
s = s.replace(old, ".slave_hp1_axi_awsize(2'b11)", 1)
for name in ('awready', 'wready', 'bresp', 'bvalid'):
    old = f'.slave_hp1_axi_{name}()'
    require(old in s, f'HP1 {name} open output not found; re-check the vendor PS macro instance')
    s = s.replace(old, f'.slave_hp1_axi_{name}({HP1}{name})', 1)
s, hp1_clk_hits = re.subn(r'assign ' + re.escape(HP1 + 'aclk') + r' = [^;]+;',
                          f'assign {HP1}aclk = {SENSOR_AXI_CLK} ;', s, count=1)
require(hp1_clk_hits == 1, 'slave_hp1_axi_aclk assign not found; re-check the vendor PS macro instance')
# HP1 reset: this PS macro has no slave_hp1_axi_aresetn port, so the DMA and the
# sensor AXI slave share the PS peripheral reset. If a macro revision exposes the
# port, drive it from that same reset instead of leaving it tied off.
if has_hp1_rst:
    s, hp1_rst_hits = re.subn(r'\.slave_hp1_axi_aresetn\([^)]*\)',
                              f'.slave_hp1_axi_aresetn({HP1}aresetn)', s, count=1)
    require(hp1_rst_hits == 1, 'slave_hp1_axi_aresetn port not rewritten')
    s = s.replace(f'assign {HP1}aclk = {SENSOR_AXI_CLK} ;',
                  f'assign {HP1}aclk = {SENSOR_AXI_CLK} ;\n    assign {HP1}aresetn = {SENSOR_AXI_RSTN} ;', 1)
require('soc_ctrl_port X_sensor' not in s, 'sensor slave already present before insertion')

# f2p_irq[0]/[1] are the vendor VDMA mm2s/s2mm interrupts. The sensor DMA block
# complete interrupt is wired to bit 3. Acquisition board tests observed no
# GIC activity from that line, while
# f2p_irq[0] wakes the VDMA driver and is disabled as spurious. The bit-3 wiring is
# kept for a later move to f2p_irq[1]; block completion is polled through
# DMA_BLK_SEQ (docs/PL-PS接口约定.md §4.4).
irq_new = r"assign w_port__ARM_Processor_System_0__f2p_irq = {12'b0,S_dma_irq,\1} ;"
irq_patterns = (
    r"assign w_port__ARM_Processor_System_0__f2p_irq = \{13'b0,([^}]+)\} ;",
    r"assign w_port__ARM_Processor_System_0__f2p_irq = ([A-Za-z_]\w*) ;",
)
irq_hits = 0
for pattern in irq_patterns:
    s, irq_hits = re.subn(pattern, irq_new, s, count=1)
    if irq_hits:
        break
require(irq_hits == 1, 'vendor f2p_irq assignment not found; re-check the vendor system.v')
require("{12'b0,S_dma_irq,w_net__Concat_0__Dout}" in s,
        'f2p_irq bit 3 rewrite did not land on the vendor Concat net')

rs485_conn = '        .I_rs485_rx(I_rs485_rx), .O_rs485_tx(O_rs485_tx), .O_rs485_de(O_rs485_de),\n'
if 'O_uart_tx' in RS485_PORTS:
    rs485_conn += '        .O_uart_tx(O_uart_tx),\n'
inst = f'''
    soc_ctrl_port X_sensor (
        .I_sysclk({SENSOR_AXI_CLK}), .I_uart_rx(I_uart_rx), .I_int1(I_int1),
        .O_spi_sclk(O_spi_sclk), .O_spi_cs_n(O_spi_cs_n),
        .IO_spi_sdio(IO_spi_sdio), .IO_iic_scl(IO_iic_scl), .IO_iic_sda(IO_iic_sda),
        .O_led(O_led), .O_alarm(O_alarm), .O_irq(S_irq), .O_dma_irq(S_dma_irq),
''' + rs485_conn + f'''        .aclk({SENSOR_AXI_CLK}), .rst_n({SENSOR_AXI_RSTN}),
'''
for name, width in ds0:
    inst += f'        .S_AXI_{name.upper()}(w_net__AXI_Matrix_0__ds0_{name}),\n'
for name, width in hp1:
    inst += f'        .M_axi_{name}({HP1}{name}),\n'
inst = inst.rstrip(',\n') + ');\n'
s = s.replace('\nendmodule', inst + '\nendmodule', 1)
require('soc_ctrl_port X_sensor' in s, 'sensor slave was not inserted into system.v')
require('.ds0_araddr()' not in s and '.ds0_awaddr()' not in s, 'DS0 still has unconnected ports')
require(f'.M_axi_awaddr({HP1}awaddr)' in s, 'sensor DMA master was not connected to HP1')
require('.rst_n(' + SENSOR_AXI_RSTN + ')' in s, 'sensor AXI reset net not found')
require('.O_dma_irq(S_dma_irq)' in s, 'DMA interrupt port not connected on soc_ctrl_port X_sensor')
s = attach_npu(s, OUT, args.resource_root.resolve(), SENSOR_AXI_CLK, SENSOR_AXI_RSTN, HP1)
sys_v.write_text(s, encoding="utf-8")

# Write the top wrapper explicitly. A vendor/DI regeneration can replace the
# wrapper with a stub that ties O_led low and leaves the sensor system
# disconnected; that image makes every active-low LED turn on and shows no data.
wrap = OUT / 'system/hdl/system_wrapper.v'
rs485_wrap_ports = ('  input  wire I_rs485_rx,\n'
                    '  output wire O_rs485_tx,\n'
                    '  output wire O_rs485_de,\n'
                    + ('  output wire O_uart_tx,\n' if 'O_uart_tx' in RS485_PORTS else ''))
rs485_wrap_conn = ('.I_rs485_rx(I_rs485_rx),\n.O_rs485_tx(O_rs485_tx),\n.O_rs485_de(O_rs485_de),\n'
                   + ('.O_uart_tx(O_uart_tx),\n' if 'O_uart_tx' in RS485_PORTS else ''))
wrap.write_text('''// Generated by prepare_hdmi_soc.py; do not edit in place.
module soc_hdmi_top
  (
  input  wire I_uart_rx,
  input  wire I_int1,
  output wire O_spi_sclk,
  output wire O_spi_cs_n,
  inout  wire IO_spi_sdio,
  inout  wire IO_iic_scl,
  inout  wire IO_iic_sda,
  output wire [3:0] O_led,
  output wire O_alarm,
''' + rs485_wrap_ports + '''  output [2:0] O_hdmi_tx_p,
  output  O_hdmi_clk_p
  );

system system(
.I_uart_rx(I_uart_rx),
.I_int1(I_int1),
.O_spi_sclk(O_spi_sclk),
.O_spi_cs_n(O_spi_cs_n),
.IO_spi_sdio(IO_spi_sdio),
.IO_iic_scl(IO_iic_scl),
.IO_iic_sda(IO_iic_sda),
.O_led(O_led), .O_alarm(O_alarm),
''' + rs485_wrap_conn + '''.O_hdmi_tx_p(O_hdmi_tx_p),
.O_hdmi_clk_p(O_hdmi_clk_p)
);
endmodule
''', encoding="utf-8")
require('.O_led(O_led)' in wrap.read_text(encoding="utf-8"), 'wrapper does not connect O_led')
require('.O_rs485_tx(O_rs485_tx)' in wrap.read_text(encoding="utf-8"),
        'wrapper does not connect O_rs485_tx')

# Sensor sources
src = REPO / 'fpga/uisrc/out/soc'
require((src / 'soc_ctrl_port.v').is_file(), 'missing fpga/uisrc/out/soc/soc_ctrl_port.v; run fpga/soc/prepare_soc.py first')
shutil.copy2(src / 'soc_ctrl_port.v', OUT / 'soc_ctrl_port.v')
# prepare_soc.py builds that wrapper from a fixed port list, so the copy used by
# this build gains the DMA interrupt port; the shared wrapper stays untouched.
port_v = OUT / 'soc_ctrl_port.v'
pv = port_v.read_text(encoding="utf-8")
pv = pv.replace(".AXI_CLK_FREQ_HZ(50_000_000)", ".AXI_CLK_FREQ_HZ(100_000_000)")
require('output wire O_alarm' in pv and '.O_alarm(O_alarm)' in pv,
        'soc_ctrl_port.v lacks U7 alarm output; re-run fpga/soc/prepare_soc.py')
pv, port_hits = re.subn(r'(\n    output wire O_irq,)', r'\1\n    output wire O_dma_irq,', pv, count=1)
require(port_hits == 1, 'soc_ctrl_port.v has no O_irq port; run fpga/soc/prepare_soc.py first')
pv, conn_hits = re.subn(r'(\n        \.O_irq\(O_irq\),)', r'\1\n        .O_dma_irq(O_dma_irq),', pv, count=1)
require(conn_hits == 1, 'soc_ctrl_port.v does not connect .O_irq(O_irq); run fpga/soc/prepare_soc.py first')
port_v.write_text(pv, encoding="utf-8")
# The RS485 ports come from the shared wrapper; if the slave gained or lost one
# after prepare_soc.py ran, stop instead of generating a half-connected top.
for _rs485_port in RS485_PORTS:
    require(re.search(r'\b' + _rs485_port + r'\b', pv) is not None,
            f'soc_ctrl_port.v lacks {_rs485_port}; re-run fpga/soc/prepare_soc.py')
# The feature units stay enabled in the merged build (WITH_FEATURES default 1).
# Their one hold exception is waived in timing_hdmi.sdc, see the note there.
sources = [OUT / 'soc_ctrl_port.v']
for p in sorted((RTL / 'soc').glob('*.v')) + sorted((RTL / 'soc').glob('*.sv')) + \
         [RTL / 'axi_lite_reg.v', RTL / 'p2f_irq.v', RTL / 'timebase.v',
          RTL / 'uart_rx_guard.v', RTL / 'temp_frame_parser.v', RTL / 'spi_adxl345.v',
          RTL / 'adxl345_ctrl.v', RTL / 'ads1115_ctrl.v',
          # RS485 Modbus master, rpm divider and PL telemetry frame builder used
          # by the sensor slave; listed so the flat read_hdl flow resolves them.
          RTL / 'encoder_uart.v', RTL / 'encoder_modbus.v', RTL / 'encoder_rpm.v',
          RTL / 'encoder_telemetry.v'] + \
         sorted((RTL / 'features').glob('*.v')) + sorted((RTL / 'fft').glob('*.v')) + \
         [REPO / 'fpga/uisrc/03_ip' / x for x in ('uiuart_rx.v', 'uii2c.v', 'ui_mspi_tx.v', 'ui_spi_rx.v')]:
    if p.name == 'axi_lite_error.v':
        continue
    shutil.copy2(p, OUT / p.name)
    sources.append(OUT / p.name)
# `include is resolved next to the including file (vendor IP does the same with
# ./include/define.v), so headers used by the flat flow are copied alongside.
for header in sorted((RTL / 'soc').glob('*.vh')) + sorted((RTL / 'soc').glob('*.svh')) + \
        sorted(RTL.glob('*.vh')) + sorted(RTL.glob('*.svh')):
    shutil.copy2(header, OUT / header.name)

# Merge constraints
adc = (OUT / 'uisrc/04_pin/fpga_pin.adc').read_text(encoding="utf-8")
local_adc = (REPO / 'fpga/uisrc/04_pin/fpga_pin.adc').read_text(encoding="utf-8")
for line in local_adc.splitlines():
    if line.startswith('set_pin_assignment') and 'I_sysclk' not in line:
        adc += line + '\n'
# RS485 board pins: encoder_pin.adc stays the single source for the transceiver
# pins shared with the fpga_top acquisition project. Only the ports this top
# carries are merged, so the fpga_top-only O_uart_tx pin never becomes an
# unmatched constraint here.
encoder_adc = (REPO / 'fpga/uisrc/04_pin/encoder_pin.adc').read_text(encoding="utf-8")
rs485_pins = {}
for line in encoder_adc.splitlines():
    match = re.match(r'set_pin_assignment\s*\{\s*(\S+)\s*\}', line)
    if match and match.group(1) in RS485_PORTS:
        rs485_pins[match.group(1)] = line
require(set(rs485_pins) == set(RS485_PORTS),
        'encoder_pin.adc lacks pin assignments for: ' +
        ', '.join(p for p in RS485_PORTS if p not in rs485_pins))
adc += ''.join(rs485_pins[p] + '\n' for p in RS485_PORTS)
(OUT / 'uisrc/04_pin/fpga_pin.adc').write_text(adc, encoding="utf-8")

# Append sources to soc_hdmi.al
al = OUT / 'soc_hdmi.al'
tree = ET.ElementTree(ET.fromstring(al.read_text(encoding="utf-8").replace('&', '&amp;')))
root = tree.getroot()
files = root.find('Source_Files')
verilog = files.find('Verilog')
sv = files.find('System_Verilog')
if sv is None:
    sv = ET.SubElement(files, 'System_Verilog')
for n, p in enumerate(sources, start=100):
    parent = sv if p.suffix in ('.sv', '.svh') else verilog
    f = ET.SubElement(parent, 'File', Path=str(p))
    info = ET.SubElement(f, 'FileInfo')
    for k, v in [('UsedInSyn', 'true'), ('UsedInP&R', 'true'), ('BelongTo', 'design_1'), ('CompileOrder', str(n))]:
        ET.SubElement(info, 'Attr', Name=k, Val=v)
root.set('Path', str(OUT))
name = root.find('Name')
if name is not None:
    name.text = 'soc_hdmi'
top = root.find('TOP_MODULE')
for el in top:
    if el.tag in ('LABEL', 'MODULE'):
        el.text = 'soc_hdmi_top'
ET.indent(tree)
tree.write(al, encoding='utf-8', xml_declaration=True)
al.write_text(al.read_text(encoding="utf-8").replace('&amp;', '&').replace("<?xml version='1.0' encoding='utf-8'?>", '<?xml version="1.0" encoding="UTF-8"?>'), encoding="utf-8")

# Flat read_hdl source list
flat = [OUT / 'system/hdl/system_wrapper.v', OUT / 'system/hdl/system.v']
for p in sorted((OUT / 'system/ip').rglob('*')):
    if p.suffix in ('.v', '.sv') and p.name != 'define.v':
        flat.append(p)
for p in sorted((OUT / 'uisrc/03_ip/alip/an_hdmi_tx').rglob('*')):
    if p.suffix in ('.v', '.sv'):
        flat.append(p)
for p in sorted((OUT / 'uisrc/03_ip/alip/video_timing_controller_v20/video_timing_controller_v20_files/RTL').rglob('*')):
    if p.suffix in ('.v', '.sv'):
        flat.append(p)
flat += sources
seen = []
for p in flat:
    if p not in seen and p.name != 'define.v':
        seen.append(p)
(OUT / 'sources_readhdl.list').write_text('\n'.join(p.as_posix() for p in seen) + '\n', encoding="utf-8")

# Clock constraints for the flat read_hdl flow.
# The vendor IP SDCs do not define clocks; their only timing file is
# uisrc/04_pin/timing.sdc, which calls derive_clocks without defining the PLL
# reference. The flat flow must therefore define p2f_clk0 first and let
# derive_clocks infer the PLL/VTG outputs from the IP parameters. Creating
# clocks directly on the PLL output nets is rejected as an invalid constraint.
# The vendor IP .sdc files contain only set_ip_property commands aimed at
# GUI-project cell paths. In the flat read_hdl netlist those paths do not
# exist, so reading them creates invalid constraints. They define no clocks;
# derive_clocks below supplies every PLL/VTG clock from p2f_clk0.
timing_lines = [
    '# PS macro p2f_clk0 = 50 MHz. This is the reference for both HDMI PLLs and',
    '# the VTG internal PLL; without it derive_clocks cannot infer any PLL clock.',
    'create_clock -name p2f_clk0 -period 20.000 [get_nets {system/w_port__ARM_Processor_System_0__p2f_clk0}]',
    'derive_clocks',
    '# The HDMI design crosses these domains through asynchronous FIFOs. Keep the',
    '# PS p2f/PS-AXI clocks together and give each PLL/VTG output its own group.',
    'set_clock_groups -asynchronous -name hdmi_async \\',
    '  -group [get_clocks {p2f_clk0 *DR1_PHY_PROCESSOR_SYSTEM_Inst.master_gp0_axi_aclk *DR1_PHY_PROCESSOR_SYSTEM_Inst.master_gp1_axi_aclk *DR1_PHY_PROCESSOR_SYSTEM_Inst.slave_hp0_axi_aclk *DR1_PHY_PROCESSOR_SYSTEM_Inst.slave_hp1_axi_aclk}] \\',
    '  -group [get_clocks {*PLL_0*clkc[0]}] \\',
    '  -group [get_clocks {*PLL_0*clkc[1]}] \\',
    '  -group [get_clocks {*PLL_0*clkc[2]}] \\',
    '  -group [get_clocks {*PLL_1*clkc[0]}] \\',
    '  -group [get_clocks {*dr1_pll_vtc*clkc[0]}]',
]
# RS485 rpm pads: the sensor UART has no external capture clock, so bound the
# register-to-pad routing with the same 10 ns datapath-only budget that
# fpga/uisrc/04_pin/io_boundary.sdc applies in the acquisition-only build.
timing_lines += [
    '# Alarm output: bounded routing to active-high GPIO pad.',
    'set_max_delay 10.000 -datapath_only -from [get_regs {*}] -to [get_ports {O_alarm}]',
    '# RS485 rpm pads: one 100 MHz period, datapath-only (no external phase).',
    'set_max_delay 10.000 -datapath_only -from [get_ports {I_rs485_rx}] -to [get_regs {*}]',
    'set_max_delay 10.000 -datapath_only -from [get_regs {*}] -to [get_ports {O_rs485_tx O_rs485_de}]',
]
if 'O_uart_tx' in RS485_PORTS:
    timing_lines.append('set_max_delay 10.000 -datapath_only -from [get_regs {*}] -to [get_ports {O_uart_tx}]')
(OUT / 'timing_hdmi.sdc').write_text('\n'.join(timing_lines) + '\n', encoding="utf-8")

# GUI builds use the same flattened HDL and clock constraints as batch builds.
# Removing the DI IP entry also prevents regenerating over the merged wrapper.
gui = ET.ElementTree(ET.fromstring(al.read_text(encoding='utf-8').replace('&', '&amp;')))
gui_files = gui.getroot().find('Source_Files')
gui_files.clear()
for section, paths in (
        ('Verilog', [p for p in seen if p.suffix == '.v']),
        ('System_Verilog', [p for p in seen if p.suffix == '.sv']),
        ('ADC_FILE', [OUT / 'uisrc/04_pin/fpga_pin.adc']),
        ('SDC_FILE', [OUT / 'timing_hdmi.sdc'])):
    group = ET.SubElement(gui_files, section)
    for order, path in enumerate(paths, 1):
        entry = ET.SubElement(group, 'File', Path=path.as_posix())
        info = ET.SubElement(entry, 'FileInfo')
        for key, value in [('UsedInSyn', 'true'), ('UsedInP&R', 'true'),
                           ('BelongTo', 'constraint_1' if section.endswith('_FILE') else 'design_1'),
                           ('CompileOrder', str(order))]:
            ET.SubElement(info, 'Attr', Name=key, Val=value)
ET.indent(gui)
gui.write(al, encoding='utf-8', xml_declaration=True)

# Build script
(OUT / 'build_hdmi_soc.tcl').write_text('''set out [file normalize [file join [file dirname [info script]]]]
cd $out
# Guard against a vendor/DI-regenerated stub wrapper: the sensor system must be
# instantiated and O_led must be driven by it, otherwise every active-low LED
# turns on and the HDMI/sensor image is dead.
set fh [open [file join $out system/hdl/system_wrapper.v] r]
set wrapper [read $fh]
close $fh
if {[string first ".O_led(O_led)" $wrapper] < 0} {
    error "system_wrapper.v does not connect O_led; re-run fpga/soc/prepare_hdmi_soc.py"
}
if {[string first ".O_rs485_tx(O_rs485_tx)" $wrapper] < 0} {
    error "system_wrapper.v does not route the RS485 transceiver; re-run fpga/soc/prepare_hdmi_soc.py"
}
set fh [open [file join $out uisrc/04_pin/fpga_pin.adc] r]
set pins [read $fh]
close $fh
if {[string first "LOCATION = N16" $pins] < 0} {
    error "fpga_pin.adc lacks the RS485 RX pin; re-run fpga/soc/prepare_hdmi_soc.py"
}
set fh [open [file join $out system/hdl/system.v] r]
set system [read $fh]
close $fh
if {[string first "soc_ctrl_port X_sensor" $system] < 0} {
    error "system.v lacks soc_ctrl_port X_sensor; re-run fpga/soc/prepare_hdmi_soc.py"
}
if {[string first "npu_hp1_write_mux X_hp1_write_mux" $system] < 0 ||
    [string first "design_0_Video_NPU_0 X_npu" $system] < 0} {
    error "system.v lacks NPU/HP1 arbitration; re-run fpga/soc/prepare_hdmi_soc.py"
}
if {[string first "N_irq,11'b0,S_dma_irq,w_net__Concat_0__Dout" $system] < 0} {
    error "system.v does not route the sensor DMA interrupt to f2p_irq bit 3; re-run fpga/soc/prepare_hdmi_soc.py"
}
if {[catch {
import_device dr1_90.db -package DR1M90GEG400 -speed 2
set f [open sources_readhdl.list r]
set sources [split [string trim [read $f]] "\n"]
close $f
read_hdl -file [join $sources " "] -top soc_hdmi_top
read_adc uisrc/04_pin/fpga_pin.adc
read_sdc timing_hdmi.sdc
optimize_rtl
optimize_gate
set_param place pr_strategy 2
set_param route post_route_opt high
set_param route postrt_opt_extra_round 4
# Sensor DMA and HDMI share one 100 MHz domain with ~0.02 ns of margin; the
# default hold fixer stops at the routing skew threshold and leaves ~12 ps.
set_param route fix_hold high
place
route
phys_opt -post_route -effort ultra
# phys_opt 重新优化后仍可能留下 10 ps 量级的保持违例，补一次布线后 fix_hold。
fix_hold
update_timing -mode final
report_timing_summary -file route_timing_summary.rpt
report_area -file route.area
check_timing -file check_timing.rpt -verbose
set f [open route_timing_summary.rpt r]
set timing [read $f]
close $f
foreach label {SWNS HWNS} {
    if {![regexp [format {%s: *(-?[0-9]+\\.[0-9]+)ns} $label] $timing unused slack]} {
        error "Timing field $label missing"
    }
    if {$slack < 0} { error "$label=$slack ns violates timing" }
}
bitgen -bit soc_hdmi.bit
# DTB generation checks this digest before enabling a probe-time PL MMIO node.
set f [open soc_hdmi.npu.sha256 w]
puts $f [lindex [exec sha256sum soc_hdmi.bit] 0]
close $f
} message]} {
    puts stderr "RESULT build_hdmi_soc: FAIL $message"
    exit 1
}
puts "RESULT build_hdmi_soc: PASS (RTL build; hardware HDMI/sensors/NPU verification TBD)"
exit
''', encoding="utf-8")

print('HP1 AXI: aclk=' + SENSOR_AXI_CLK + ', aresetn=' +
      ((HP1 + 'aresetn <- ' + SENSOR_AXI_RSTN) if has_hp1_rst else SENSOR_AXI_RSTN))
print('DMA IRQ: f2p_irq[3] <- S_dma_irq (DMA_IRQ[0] masked, one pulse per 4 KiB block)')
print('NPU: GP1 0x1F0000000; HP1 read + shared writes; f2p_irq[15]; YOLO/Resize disabled')
print('RS485 rpm: top ports ' + ' '.join(RS485_PORTS) +
      '; pins from fpga/uisrc/04_pin/encoder_pin.adc (N16/L17/N15, LVCMOS33)')
print(f'HDMI SoC project: {OUT}')
print('Sources:', len(sources))
print('Next: td-fhs -c "cd ... && ./bin/td.sh ' + str(OUT / 'build_hdmi_soc.tcl') + '"')
