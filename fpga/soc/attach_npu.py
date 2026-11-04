"""Attach the vendor Video_NPU to GP1 and the shared HP1 DDR port."""
from pathlib import Path
import re
import shutil


def ports(path):
    header = path.read_text().split('module ', 1)[1].split(');', 1)[0]
    return [(direction, width or '', name) for direction, width, name in re.findall(
        r'\b(input|output)\s*(\[\d+:\d+\])?\s*(\w+)\s*(?:,|$)', header, re.M)]


def attach_npu(text, out, resource_root, clk, rst_n, sensor_hp):
    ip_root = (Path(resource_root) / '01_start/02_start_Linux/05_application_demo/'
               '05-5_NPU演示/demo/soc_prj/design_0/ip')
    names = ('design_0_Video_NPU_0', 'design_0_AXI_Protocol_Converter_1')
    for name in names:
        src = ip_root / name
        if not (src / (name + '.v')).is_file():
            raise SystemExit(f'Vendor NPU IP missing: {src}')
        shutil.copytree(src, out / 'system/ip' / name)

    # Vendor Component.xml declares these as optional macro features. FF6
    # uses hard-NPU FCs and no image preprocessing/postprocessing. Retain the
    # real soft-NN register/DMA/IRQ logic and all generic NN operators, while
    # excluding the unrelated YOLO and image-resize engines from this image.
    defines = out / 'system/ip' / names[0] / 'RTL/include/define.v'
    config = defines.read_text()
    for feature in ('SOFT_YOLO', 'SOFT_RESIZE'):
        config, hits = re.subn(r'^`define ' + feature + r'_\w+\s*$', '', config, flags=re.M)
        if hits != 1:
            raise SystemExit(f'Vendor NPU configuration changed: missing {feature}')
    defines.write_text(config)

    def replace_port(prefix, name, expression):
        nonlocal text
        text, hits = re.subn(r'\.' + prefix + name + r'\([^)]*\)',
                             f'.{prefix}{name}({expression})', text, count=1)
        if hits != 1:
            raise SystemExit(f'NPU attachment: missing PS port {prefix}{name}')

    decl = ['    wire N_irq;']
    gp = [('awid', 12), ('awaddr', 33), ('awlen', 4), ('awsize', 2),
          ('awburst', 2), ('awlock', 1), ('awcache', 4), ('awprot', 3),
          ('awvalid', 1), ('awready', 1), ('wdata', 32), ('wstrb', 4),
          ('wlast', 1), ('wvalid', 1), ('wready', 1), ('bid', 13),
          ('bresp', 2), ('bvalid', 1), ('bready', 1), ('arid', 12),
          ('araddr', 33), ('arlen', 4), ('arsize', 2), ('arburst', 2),
          ('arlock', 1), ('arcache', 4), ('arprot', 3), ('arvalid', 1),
          ('arready', 1), ('rid', 13), ('rdata', 32), ('rresp', 2),
          ('rlast', 1), ('rvalid', 1), ('rready', 1)]
    for name, width in gp:
        decl.append(f'    wire [{width-1}:0] N_gp_{name};')
        expr = f'N_gp_{name}' + ('[11:0]' if name in ('bid', 'rid') else '')
        replace_port('master_gp1_axi_', name, expr)
    text, hits = re.subn(r'assign w_port__ARM_Processor_System_0__master_gp1_axi_aclk = [^;]+;',
                         f'assign w_port__ARM_Processor_System_0__master_gp1_axi_aclk = {clk};', text, count=1)
    if hits != 1:
        raise SystemExit('NPU attachment: GP1 clock assignment missing')

    converter_ports = ports(ip_root / names[1] / (names[1] + '.v'))
    npu_ports = ports(ip_root / names[0] / (names[0] + '.v'))
    if len(converter_ports) != 63 or len(npu_ports) != 59:
        raise SystemExit(f'Vendor NPU interface changed: converter={len(converter_ports)}, npu={len(npu_ports)}')
    lite_names = set()
    converter = []
    for _, width, name in converter_ports:
        if name.startswith('ds_axi_'):
            sig = name.removeprefix('ds_axi_')
            decl.append(f'    wire {width} N_lite_{sig};')
            lite_names.add(sig)
            expr = f'N_lite_{sig}'
        elif name.startswith('us_axi_'):
            sig = name.removeprefix('us_axi_')
            if sig in ('buser', 'ruser'):
                expr = ''
            elif sig in ('awuser', 'aruser', 'wuser'):
                expr = "1'b0"
            elif sig in ('awqos', 'arqos'):
                expr = "4'b0"
            elif sig in ('awid', 'arid', 'awsize', 'arsize'):
                expr = "{1'b0,N_gp_" + sig + '}'
            elif sig in ('awlen', 'arlen'):
                expr = "{4'b0,N_gp_" + sig + '}'
            else:
                expr = 'N_gp_' + sig
        else:
            expr = clk if name == 'aclk' else rst_n
        converter.append(f'        .{name}({expr})')

    npu = []
    ddr = {}
    for direction, width, name in npu_ports:
        if name in ('S_AXILite_ACLK', 'M_AXI_ACLK'):
            expr = clk
        elif name in ('S_AXILite_AResetn', 'M_AXI_AResetn'):
            expr = rst_n
        elif name == 'npu_soft_intr':
            expr = 'N_irq'
        elif name.startswith('s_axilite_'):
            sig = name.removeprefix('s_axilite_')[:-2]
            if sig not in lite_names:
                raise SystemExit(f'Unknown NPU AXI-Lite signal: {sig}')
            expr = 'N_lite_' + sig
        else:
            sig = name.removeprefix('m_axi_').split('_soft_')[0]
            ddr[sig] = width
            decl.append(f'    wire {width} N_ddr_{sig};')
            expr = 'N_ddr_' + sig
        npu.append(f'        .{name}({expr})')

    # NPU alone uses the HP1 read channel. Writes preserve all six ID bits and
    # are arbitrated until B acceptance; sensor DMA cannot interleave a burst.
    write_fields = [sig for sig in ddr if sig.startswith(('aw', 'w', 'b'))]
    mux = [f'        .clk({clk})', f'        .rst_n({rst_n})']
    sensor_constants = {'awid': "6'b0", 'awsize': "2'b11", 'awlock': "1'b0",
                        'awcache': "4'b0", 'awprot': "3'b0"}
    for sig in ddr:
        if sig not in write_fields:
            replace_port('slave_hp1_axi_', sig, 'N_ddr_' + sig)
            continue
        decl.append(f'    wire {ddr[sig]} N_hp_{sig};')
        replace_port('slave_hp1_axi_', sig, 'N_hp_' + sig)
        mux.append(f'        .m_{sig}(N_hp_{sig})')
        sensor = sensor_constants.get(sig, sensor_hp + sig)
        if sig == 'awlen':
            sensor += '[3:0]'
        if sig == 'bid':
            decl.append('    wire [5:0] N_unused_sensor_bid;')
            sensor = 'N_unused_sensor_bid'
        mux.append(f'        .s_{sig}({{N_ddr_{sig},{sensor}}})')

    old_irq = "{12'b0,S_dma_irq,w_net__Concat_0__Dout}"
    if text.count(old_irq) != 1:
        raise SystemExit('NPU attachment: sensor/HDMI IRQ assignment missing')
    text = text.replace(old_irq, "{N_irq,11'b0,S_dma_irq,w_net__Concat_0__Dout}")
    text = text.replace('    wire S_irq;', '\n'.join(decl) + '\n    wire S_irq;', 1)
    instances = '\n    design_0_AXI_Protocol_Converter_1 X_npu_gp1 (\n' + ',\n'.join(converter) + '\n    );\n'
    instances += '\n    design_0_Video_NPU_0 X_npu (\n' + ',\n'.join(npu) + '\n    );\n'
    instances += '\n    npu_hp1_write_mux X_hp1_write_mux (\n' + ',\n'.join(mux) + '\n    );\n'
    return text.replace('\nendmodule', instances + '\nendmodule', 1)
