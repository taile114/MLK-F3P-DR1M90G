#!/usr/bin/env python3
"""Build a local SoC source tree from the installed vendor template, without demo CSRs."""
from pathlib import Path
import argparse
import re
import shutil
import xml.etree.ElementTree as ET

REPO = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--resource-root', type=Path, default=REPO / 'docs/MLK-F3P-DR1M90G-Resource')
args = parser.parse_args()
SRC = args.resource_root.resolve() / '03_demo/3-2_ex_fpsoc/3_2_01_sdk_base/20_axi_lite_register/soc_prj'
OUT = REPO / 'fpga/uisrc/out/soc'
SYSTEM = SRC / 'uisrc/01_rtl/system'
if not SYSTEM.is_dir():
    raise SystemExit(f'Vendor AXI template missing: {SYSTEM}')
if not OUT.resolve().is_relative_to((REPO / 'fpga/uisrc/out').resolve()):
    raise SystemExit('Generated SoC output must stay inside fpga/uisrc/out')
OUT.mkdir(parents=True, exist_ok=True)
selected = []
for stale in ('system_Processor_System_Reset_0', 'system_Constant_0'):
    shutil.rmtree(OUT / 'ip' / stale, ignore_errors=True)
for source in sorted((SYSTEM / 'ip').iterdir()):
    if (source.name.startswith('.') or 'axi_lite_reg' in source.name
            or source.name in ('system_Processor_System_Reset_0', 'system_Constant_0')):
        continue
    target = OUT / 'ip' / source.name
    shutil.copytree(source, target, dirs_exist_ok=True)
    selected.append(target / (source.name + '.xml'))

s = (SYSTEM / 'hdl/system.v').read_text(encoding="utf-8")
# RS485 rpm channel: the slave owns the board transceiver, and the three port
# names are fixed by the PL-PS interface contract, so they are passed up
# unchanged and land on the board pins of the fpga_top acquisition project.
# O_uart_tx (PL USB UART telemetry) is passed up only when the slave exposes it,
# so a regeneration never names a port the slave does not have.
SENSOR = REPO / 'fpga/uisrc/01_rtl/soc/sensor_soc_axi.v'
sensor_text = SENSOR.read_text(encoding="utf-8")
sensor_head = sensor_text[sensor_text.index('module sensor_soc_axi'):
                          sensor_text.index(');', sensor_text.index('module sensor_soc_axi'))]
sensor_extra = ['I_rs485_rx', 'O_rs485_tx', 'O_rs485_de']
physical = '''input wire I_sysclk, I_uart_rx, I_int1,
    output wire O_spi_sclk, O_spi_cs_n,
    inout wire IO_spi_sdio, IO_iic_scl, IO_iic_sda,
    output wire [3:0] O_led,
    output wire O_alarm,
    input wire I_rs485_rx,
    output wire O_rs485_tx, O_rs485_de,
    '''
if re.search(r'\bO_uart_tx\b', sensor_head):
    physical += 'output wire O_uart_tx,\n    '
    sensor_extra.append('O_uart_tx')
slave_ports = set(re.findall(r'\b(?:IO|I|O)_\w+', sensor_head))
if not set(sensor_extra) <= slave_ports:
    print('WARNING: sensor_soc_axi.v does not expose ' +
          ' '.join(p for p in sensor_extra if p not in slave_ports) +
          '; the generated top is wired to the interface contract and needs that slave change before synthesis')
s = s.replace('module system(', 'module soc_top(\n    ' + physical)
# p2f_rst0_n is the PS reset output already routed inside the design; it must not
# become a top-level port (no board pin, random placement risks the device).
s = re.sub(r'O_led,\s+output\s+wire\s+p2f_rst0_n', 'O_led', s)
s = re.sub(r'\n\s*assign p2f_rst0_n = [^;]+;', '', s)
s = s.replace('.f2p_irq(16\'b0)', ".f2p_irq({15'd0,S_irq})")
s = s.replace(') ;', ') ;\n    wire S_irq;', 1)
# The vendor template leaves Processor_System_Reset_0 and Constant_0 dangling
# (all reset outputs open; pll_locked tied to the constant). They therefore have
# no function and, after optimization, remove the only load of p2f_clk1. Drop
# both instances so the generated netlist has no dead reset tree.
for instance in ('system_Processor_System_Reset_0 Processor_System_Reset_0',
                 'system_Constant_0 Constant_0'):
    s, count = re.subn(re.escape(instance) + r'\s*\(.*?\)\s*;', '', s, count=1, flags=re.S)
    if count != 1:
        raise SystemExit(f'instance not found: {instance}')
    module = instance.rsplit(' ', 1)[1]
    s = re.sub(r'\n\s*wire\s+(?:\[[^\]]*\]\s+)?w_(?:port|net)__' + module + r'__[^;]*;', '', s)
    s = re.sub(r'\n\s*assign\s+w_(?:port|net)__' + module + r'__[^;]*;', '', s)
s = re.sub(r'\n\s*wire\s+w_net__ARM_Processor_System_0__p2f_clk1\s*;', '', s)
s = re.sub(r'\n\s*assign\s+w_net__ARM_Processor_System_0__p2f_clk1[^;]*;', '', s)
for n in range(4):
    # Preserve the entire 64 KiB offset, rather than the template's 6-bit aperture.
    for ch in ('AWADDR', 'ARADDR'):
        s = re.sub(r'wire \[5:0\](\s+w_port__axi_lite_reg_'+str(n)+'__S_AXI_'+ch+r'\s*;)',r'wire [15:0]\1',s)
    s = s.replace('design_0_axi_lite_reg_'+str(n)+' axi_lite_reg_'+str(n),
                  ('soc_ctrl_port' if n == 0 else 'axi_lite_error')+' axi_lite_reg_'+str(n))
    if n == 0:
        s = s.replace('axi_lite_reg_0 (', '''axi_lite_reg_0 (
        .I_sysclk(I_sysclk),.I_uart_rx(I_uart_rx),.I_int1(I_int1),
        .O_spi_sclk(O_spi_sclk),.O_spi_cs_n(O_spi_cs_n),
        .IO_spi_sdio(IO_spi_sdio),.IO_iic_scl(IO_iic_scl),.IO_iic_sda(IO_iic_sda),
        .O_led(O_led),.O_alarm(O_alarm),.O_irq(S_irq),''' +
                      ''.join(f'.{p}({p}),' for p in sensor_extra))
# The standalone top has no HP master port: tie the unused DMA write-response
# inputs low and leave the write address/data outputs open.
s, tie_hits = re.subn(r'(soc_ctrl_port axi_lite_reg_0 \(.*?)\);',
    r"\1, .M_axi_awready(1'b0), .M_axi_wready(1'b0),"
    r" .M_axi_bresp(2'b0), .M_axi_bvalid(1'b0));", s, count=1, flags=re.S)
if tie_hits != 1:
    raise SystemExit('soc_ctrl_port instance not found in soc_top.v')
(OUT / 'soc_top.v').write_text(s, encoding="utf-8")

# Adapter uses the template's AXI port names, never the vendor axi_lite_reg module.
ports = (REPO / 'fpga/uisrc/01_rtl/soc/axi_lite_error.v').read_text(encoding="utf-8")
ports = ports[ports.index('module axi_lite_error('):ports.index('    reg S_aw')]
ports = ports.replace('module axi_lite_error(', 'module soc_ctrl_port(\n    '+physical+'output wire O_irq,\n    ')
ports = ports.replace('output reg ', 'output wire ')
# The DMA write master is not part of the template aperture; append its ports to
# the generated wrapper so the SoC top (or the HDMI top) can route them to HP.
dma_phys = '''    output wire [32:0] M_axi_awaddr,
    output wire [7:0] M_axi_awlen,
    output wire [2:0] M_axi_awsize,
    output wire [1:0] M_axi_awburst,
    output wire M_axi_awvalid,
    input wire M_axi_awready,
    output wire [63:0] M_axi_wdata,
    output wire [7:0] M_axi_wstrb,
    output wire M_axi_wlast,
    output wire M_axi_wvalid,
    input wire M_axi_wready,
    input wire [1:0] M_axi_bresp,
    input wire M_axi_bvalid,
    output wire M_axi_bready'''
anchor = '    output wire [31:0] S_AXI_RDATA\n);'
if anchor not in ports:
    raise SystemExit('soc_ctrl_port: S_AXI_RDATA port anchor not found')
ports = ports.replace(anchor, '    output wire [31:0] S_AXI_RDATA,\n' + dma_phys + '\n);', 1)
connections = ['.I_clk(aclk)', '.I_rstn(rst_n)', '.I_sysclk(I_sysclk)']
for name in ['I_uart_rx','I_int1','O_spi_sclk','O_spi_cs_n','IO_spi_sdio','IO_iic_scl','IO_iic_sda','O_led','O_alarm','O_irq'] + sensor_extra:
    connections.append(f'.{name}({name})')
for ch in ['awaddr','awvalid','awready','wdata','wstrb','wvalid','wready','bresp','bvalid','bready','araddr','arvalid','arready','rdata','rresp','rvalid','rready']:
    direction = 'O' if ch in ['awready','wready','bresp','bvalid','arready','rdata','rresp','rvalid'] else 'I'
    connections.append(f'.{direction}_{ch}(S_AXI_{ch.upper()})')
# DMA write master: 64 bit data / 33 bit address, no read channel.
for name in ['awaddr','awlen','awsize','awburst','awvalid','wdata','wstrb','wlast','wvalid','bready']:
    connections.append(f'.M_axi_{name}(M_axi_{name})')
for name in ['awready','wready','bresp','bvalid']:
    connections.append(f'.M_axi_{name}(M_axi_{name})')
(OUT / 'soc_ctrl_port.v').write_text(ports+'    sensor_soc_axi #(.AXI_CLK_FREQ_HZ(50_000_000)) X_sensor(\n        '+',\n        '.join(connections)+');\nendmodule\n', encoding="utf-8")

# RS485 board pins for the SoC top. encoder_pin.adc stays the single source for
# the transceiver pins shared with the fpga_top acquisition project; only the
# ports this SoC top carries are copied, so an fpga_top-only pin never becomes an
# unmatched constraint in this project.
encoder_adc = (REPO / 'fpga/uisrc/04_pin/encoder_pin.adc').read_text(encoding="utf-8")
rs485_lines = []
for line in encoder_adc.splitlines():
    match = re.match(r'set_pin_assignment\s*\{\s*(\S+)\s*\}', line)
    if match and match.group(1) in sensor_extra:
        rs485_lines.append(line)
pinned = {re.match(r'set_pin_assignment\s*\{\s*(\S+)\s*\}', line).group(1) for line in rs485_lines}
missing = [p for p in sensor_extra if p not in pinned]
if missing:
    raise SystemExit('encoder_pin.adc has no pin assignment for: ' + ', '.join(missing))
(OUT / 'rs485_pin.adc').write_text(
    '# Generated by prepare_soc.py from fpga/uisrc/04_pin/encoder_pin.adc; do not edit in place.\n'
    '# RS485 rpm channel: same board pins as the fpga_top acquisition project.\n'
    + '\n'.join(rs485_lines) + '\n', encoding="utf-8")

# Keep the IP-XACT topology alongside the RTL for address extraction and review.
# HPF export remains gated on integrator validation; this file is not a frozen platform.
ns = {'i': 'http://www.accellera.org/XMLSchema/IPXACT/1685-2014'}
tree = ET.parse(SYSTEM / 'Design.xml')
root = tree.getroot()
namespace = root.tag.split('}')[0]+'}'
for instance in root.iter(namespace+'componentInstance'):
    name = instance.find(namespace+'instanceName')
    if name is not None and name.text and name.text.startswith('axi_lite_reg_'):
        ref = instance.find(namespace+'componentRef')
        ref.set('vendor','mlk'); ref.set('library','module')
        ref.set('name','soc_ctrl_port' if name.text.endswith('_0') else 'axi_lite_error')
        ref.set('version','1.0')
        for el in instance.iter(namespace+'configurableElementValue'):
            if 'ADDR_WIDTH' in el.get('referenceId',''): el.text='16'
removed = {'Processor_System_Reset_0', 'Constant_0'}
for parent in root.iter():
    for child in list(parent):
        tag = child.tag.split('}')[-1]
        if tag == 'componentInstance':
            name_el = child.find(namespace+'instanceName')
            if name_el is not None and name_el.text in removed:
                parent.remove(child)
        elif tag == 'designPort':
            name_el = child.find(namespace+'name')
            if name_el is not None and name_el.text == 'p2f_rst0_n':
                parent.remove(child)
for parent in root.iter():
    for child in list(parent):
        if child.tag.split('}')[-1] != 'adHocConnection':
            continue
        for ref in list(child.iter(namespace+'internalPortReference')):
            if ref.get('componentRef') in removed:
                for holder in child.iter(namespace+'portReferences'):
                    if ref in list(holder): holder.remove(ref)
        for ref in list(child.iter(namespace+'externalPortReference')):
            if ref.get('portRef') == 'p2f_rst0_n':
                for holder in child.iter(namespace+'portReferences'):
                    if ref in list(holder): holder.remove(ref)
        refs = list(child.iter(namespace+'internalPortReference')) + list(child.iter(namespace+'externalPortReference'))
        if len(refs) < 2:
            parent.remove(child)
ET.indent(tree)
tree.write(OUT / 'Design.xml',encoding='utf-8',xml_declaration=True)

project = ET.ElementTree(ET.fromstring((SRC / 'fpga_prj.al').read_text(encoding="utf-8").replace('&','&amp;')))
p = project.getroot()
p.set('Path',str(OUT)); p.find('Name').text='soc'
for el in p.find('TOP_MODULE'):
    if el.tag in ('LABEL','MODULE'): el.text='soc_top'
files = p.find('Source_Files')
for child in list(files): files.remove(child)
verilog = ET.SubElement(files,'Verilog')
system_verilog = ET.SubElement(files,'System_Verilog')
sources = list((REPO / 'fpga/uisrc/01_rtl').glob('*.v'))
sources = [x for x in sources if x.name != 'fpga_top.v']
sources += list((REPO / 'fpga/uisrc/01_rtl/features').glob('*.v'))
sources += list((REPO / 'fpga/uisrc/01_rtl/fft').glob('*.v'))
sources += list((REPO / 'fpga/uisrc/01_rtl/soc').glob('*.v'))
sources += list((REPO / 'fpga/uisrc/01_rtl/soc').glob('*.sv'))
sources += [REPO / 'fpga/uisrc/03_ip' / x for x in ['uiuart_rx.v','uii2c.v','ui_mspi_tx.v','ui_spi_rx.v']]
sources += [OUT / 'soc_top.v', OUT / 'soc_ctrl_port.v']
for source in selected:
    for vf in ET.parse(source).getroot().iter():
        if vf.tag.endswith('}file'):
            n = next((x.text for x in vf if x.tag.endswith('}name')), None)
            if n and n.endswith(('.v','.sv')): sources.append(source.parent / n)
for n, source in enumerate(sorted(sources)):
    parent = system_verilog if source.suffix.lower() in ('.sv', '.svh') else verilog
    f = ET.SubElement(parent,'File',Path=str(source))
    info = ET.SubElement(f,'FileInfo')
    for key,val in [('UsedInSyn','true'),('UsedInP&R','true'),('BelongTo','design_1'),('CompileOrder',str(n+1))]:
        ET.SubElement(info,'Attr',Name=key,Val=val)
adc = ET.SubElement(files, 'ADC_FILE')
for order, rel in enumerate(('../../04_pin/fpga_pin.adc', 'rs485_pin.adc'), start=1):
    f = ET.SubElement(adc, 'File', Path=rel)
    info = ET.SubElement(f, 'FileInfo')
    for key, val in [('UsedInSyn', 'true'), ('UsedInP&R', 'true'), ('BelongTo', 'constraint_1'), ('CompileOrder', str(order))]:
        ET.SubElement(info, 'Attr', Name=key, Val=val)
sdc = ET.SubElement(files, 'SDC_FILE')
for order, rel in enumerate(('../../../soc/timing.sdc', '../../04_pin/io_boundary.sdc'), start=2):
    f = ET.SubElement(sdc, 'File', Path=rel)
    info = ET.SubElement(f, 'FileInfo')
    for key, val in [('UsedInSyn', 'true'), ('UsedInP&R', 'true'), ('BelongTo', 'constraint_1'), ('CompileOrder', str(order))]:
        ET.SubElement(info, 'Attr', Name=key, Val=val)
ip = ET.Element('IP_FILE')
for source in selected:
    f = ET.SubElement(ip,'File',Path=str(source)); info=ET.SubElement(f,'FileInfo')
    for key,val in [('UsedInSyn','true'),('UsedInP&R','true'),('BelongTo','design_1')]:
        ET.SubElement(info,'Attr',Name=key,Val=val)
ET.indent(project)
(OUT / 'sources.list').write_text('\n'.join(str(x) for x in sources)+'\n', encoding="utf-8")
text = (REPO / 'fpga/uisrc/fpga.al').read_text(encoding="utf-8")
block = ET.tostring(files,encoding='unicode').replace('&amp;','&').replace(' />','/>')
text = re.sub(r'<Source_Files>.*?</Source_Files>', lambda _: block, text, flags=re.S)
text = text.replace('<Name>fpga</Name>','<Name>soc</Name>').replace('<LABEL>fpga_top</LABEL>','<LABEL>soc_top</LABEL>').replace('<MODULE>fpga_top</MODULE>','<MODULE>soc_top</MODULE>')
text = re.sub(r'Path="[^"]*"(?=>)', lambda _: 'Path="'+OUT.as_posix()+'"',text,count=1)
(OUT / 'soc.al').write_text(text, encoding="utf-8")
al = OUT / 'soc.al'
al.write_text(al.read_text(encoding="utf-8").replace('&amp;','&').replace(' />','/>').replace("<?xml version='1.0' encoding='utf-8'?>", '<?xml version="1.0" encoding="UTF-8"?>'), encoding="utf-8")
print(f'SoC project: {OUT / "soc.al"}')
print('RS485 rpm pins: rs485_pin.adc <- fpga/uisrc/04_pin/encoder_pin.adc; top ports ' + ' '.join(sensor_extra))
print('Design.xml: candidate topology; integrator validation and HPF export TBD')
