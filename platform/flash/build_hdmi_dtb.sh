#!/usr/bin/env bash
# Build the HDMI + pl_ctrl device tree from the vendor HDMI demo DTB.
# The vendor DTB is decompiled, the pl_ctrl node is inserted under /soc, the PL
# sensor DMA buffer is declared under /reserved-memory, and the result is
# compiled and checked with dtc. The pl_ctrl block-complete interrupt is
# optional and off by default (pl_irq): without it the driver stays in
# polling-only mode and reports the GIC number it would expect.
set -euo pipefail
flash="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$flash/../.." && pwd)"
vendor="$flash/vendor"
dtc="$vendor/sources/linux/scripts/dtc/dtc"
base="$repo/docs/MLK-F3P-DR1M90G-Resource/01_start/02_start_Linux/05_application_demo/05-1_QT环境搭建及demo演示/demo/boot/system.dtb"
out="$vendor/boards/DR1M/buildroot/images/boot/system.dtb"

# --- PL sensor DMA buffer (reserved-memory) ---------------------------------
# Physical DDR window the sensor HP1 write channel fills (docs/PL-PS接口约定.md
# §4.4). The address is a contract between PL and PS, so it is fixed here
# instead of being recomputed at build time: PS programs it through
# DMA_BASE_HI / DMA_BASE_LO and userspace gets it from `plctl dma --base`. The
# python step below re-reads the DDR size and the existing reserved regions from
# the vendor DTB and fails the build if this choice ever stops being free.
#
# Vendor DTB facts behind the choice:
#   /memory     0x0000_0000..0x3FFF_FFFF (1 GiB)
#   linux,cma   128 MiB reusable, alloc-ranges 0x0000_0000..0x1FFF_FFFF:
#               the kernel can only take CMA from the low 512 MiB
#   DDR top     the relocated U-Boot image (~904 KiB), its PSCI stacks and
#               CONFIG_SYS_NONCACHED_MEMORY (1 MiB) stay resident in the top
#               ~4 MiB (kernel_addr_r=0x1000_0000, fdt_addr_r=0x0800_0000)
# The buffer ends 16 MiB below the DDR top, 4 KiB aligned and 8-byte aligned.
# 1 MiB = 256 x 4096 B: one block per DMA_BYTES transfer, every block 4 KiB
# aligned and a single 4096 B write never crosses a 4 KiB boundary.
dma_base=0x3ef00000
dma_size=0x100000
# Keep the top 16 MiB clear for the U-Boot runtime image, its PSCI stacks and
# the non-cached area, so dma_base cannot creep into the resident U-Boot region.
dma_top_guard=0x1000000

[[ -x "$dtc" ]] || { echo "ERROR: dtc missing: $dtc" >&2; exit 1; }
[[ -f "$base" ]] || { echo "ERROR: vendor HDMI dtb missing: $base" >&2; exit 1; }
python3 "$repo/fpga/soc/verify_npu_bit.py"

printf 'PL sensor DMA buffer: phys=%s..%s size=%s (%d KiB = %d x 4 KiB), no-map\n' \
    "$dma_base" "$(printf '0x%x' $((dma_base + dma_size - 1)))" "$dma_size" \
    $((dma_size / 1024)) $((dma_size / 4096))
printf '  DMA_BASE_LO=%s DMA_BASE_HI=0x0 (plctl dma --base %s)\n' "$dma_base" "$dma_base"

# --- pl_ctrl block-complete interrupt (optional, default off) ---------------
# pl_irq=off       no `interrupts` property: the driver keeps polling DMA_BLK_SEQ
# pl_irq=auto      derive f2p_irq[3] from the rules below and write the cell
# pl_irq=<gic-id>  write this GIC ID verbatim (SPI = id - 32), for example the
#                  number /proc/interrupts reports on the board
#
# Derivation of the auto value (docs/PL-PS接口约定.md §4.4 and §5):
#   * a fabric interrupt line f2p_irq[n] reaches the GIC as SPI 82+n, i.e. GIC
#     ID 114+n. The vendor VDMA node in
#     platform/flash/vendor/sources/linux/arch/arm64/boot/dts/anlogic/
#     anlogic-dr1m90-softip.dts declares <GIC_SPI 82 ...> and <GIC_SPI 83 ...>,
#     and the SDK PL interrupt example
#     docs/MLK-F3P-DR1M90G-Resource/03_demo/3-2_ex_fpsoc/3_2_01_sdk_base/
#     13_sdk_pl_intr/soc_sdk/pl_intr/main.c:9 defines PL_IRQ_NUM0 as 114 for
#     PL_INTER0: SPI 82 + 32 = 114 confirms the +32 offset.
#   * §4.4 assigns the ring block-complete interrupt to f2p_irq[3], so the auto
#     value is SPI 85 / GIC ID 117.
#   * Design.xml proves the port is enabled, not which GIC line it uses:
#     fpga/uisrc/out/soc/ip/design_0_ARM_Processor_System_0/Design.xml:12 has
#     F2P_IRQ_Available=1 while every P2F_IRQ_*_Available is 0 (P2F is the
#     PS-to-PL direction). The PL must also drive f2p_irq[3]:
#     fpga/uisrc/out/soc/soc_top.v:1362 currently connects only bit 0
#     (.f2p_irq({15'd0,S_irq})), so board test is still required.
#   * the trigger type is IRQ_TYPE_LEVEL_HIGH (0x4): §4.4 makes DMA_IRQ[16]
#     PEND drive f2p_irq[3] as a level, so the line stays asserted until the PS
#     writes DMA_IRQ[8]=1 (W1C) and then drops. With EDGE_RISING (0x1) a block
#     completion that happens while PEND is already high produces no new edge,
#     and a pulse only one PL clock wide can be missed entirely, so the GIC
#     count stays 0 while DMA_IRQ[16] reads 1. The vendor DTB marks its VDMA
#     lines LEVEL_HIGH (0x4) as well, so the cell matches the vendor usage of
#     the same GIC.
# The variable controls the block-complete line only. §5 also lists the
# sensor/frame pulse on f2p_irq[0] = GIC 114, whose routing is equally
# unconfirmed; it stays undeclared, so `plctl irq` keeps polling. The driver
# requests every declared line (up to two) and services both registers, so a
# later two-cell `interrupts = <0x0 0x52 0x4>, <0x0 0x55 0x4>` works unchanged.
#
# The GIC ID stays REF until /proc/interrupts confirms it, so the default is
# off. The inherited interrupt-parent is the GICv3 node
# interrupt-controller@dd000000 (phandle 0x01) of the vendor DTB; /soc and the
# root node both set interrupt-parent, so the node needs no own parent.
pl_irq="${pl_irq:-off}"
# 4 = LEVEL_HIGH (default), 1 = edge rising. The SDK PL interrupt examples use
# POSTIVE_EDGE_TRIGGER for PL_INTER0/1, so edge is the vendor-proven setting.
pl_irq_type="${pl_irq_type:-4}"
pl_irq_spi=$((82 + 3))
pl_irq_gic=$((pl_irq_spi + 32))
pl_irq_cell=""
case "$pl_irq" in
off)
    ;;
auto)
    pl_irq_cell="0x0 $(printf '0x%x' "$pl_irq_spi") 0x${pl_irq_type}"
    ;;
*)
    [[ "$pl_irq" =~ ^[0-9]+$ ]] || {
        echo "ERROR: pl_irq must be off, auto or a GIC ID, got '$pl_irq'" >&2
        exit 1
    }
    pl_irq_gic="$pl_irq"
    if ((pl_irq_gic < 114 || pl_irq_gic > 129)); then
        echo "ERROR: pl_irq GIC ID $pl_irq_gic is outside the fabric range 114..129 (SPI 82..97)" >&2
        exit 1
    fi
    pl_irq_spi=$((pl_irq_gic - 32))
    pl_irq_cell="0x0 $(printf '0x%x' "$pl_irq_spi") 0x${pl_irq_type}"
    ;;
esac
if [[ -n "$pl_irq_cell" ]]; then
    printf 'pl_ctrl IRQ: pl_irq=%s -> f2p_irq[3] -> SPI %u -> GIC ID %u, interrupts = <%s> (trigger 0x%s; 4=level, 1=edge rising)\n' \
        "$pl_irq" "$pl_irq_spi" "$pl_irq_gic" "$pl_irq_cell"
    printf '  basis: VDMA dts <GIC_SPI 82/83> and SDK PL_IRQ_NUM0=114 give 114+n;\n'
    printf '         Design.xml F2P_IRQ_Available=1 proves the port only\n'
    printf '  verify on the board: /proc/interrupts lists GIC %u (usually virq %u), dmesg\n' \
        "$pl_irq_gic" "$pl_irq_gic"
    printf '         prints the requested line in "pl_ctrl v1.0 at ... N Linux IRQ line(s)"\n'
else
    printf 'pl_ctrl IRQ: pl_irq=off (no interrupts property; driver reports GIC %u and polls DMA_BLK_SEQ)\n' \
        "$pl_irq_gic"
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
"$dtc" -I dtb -O dts -o "$tmp/base.dts" "$base" 2>/dev/null
python3 - "$tmp/base.dts" "$tmp/merged.dts" "$dma_base" "$dma_size" "$dma_top_guard" \
        "$pl_irq_cell" <<'PY'
import re
import sys

src, dst, base_s, size_s, guard_s, irq_cell = sys.argv[1:7]
dma_base = int(base_s, 0)
dma_size = int(size_s, 0)
guard = int(guard_s, 0)

text = open(src).read()


def span_at(text, brace):
    """Return the (start, end) of the brace-delimited block at `brace`."""
    depth = 0
    in_str = False
    i = brace
    while i < len(text):
        c = text[i]
        if in_str:
            if c == '\\':
                i += 2
                continue
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return brace, i + 1
        i += 1
    raise SystemExit('unbalanced braces in the decompiled base DTB')


def find_node(text, pattern):
    m = re.compile(pattern, re.M).search(text)
    if not m:
        return None
    start, end = span_at(text, text.index('{', m.start()))
    return m, start, end


def child_nodes(text, body_start, body_stop, tabs):
    """Direct child nodes of a block as (name, header_start, body_start, body_stop)."""
    pat = re.compile(r'^\t{%d}([^\s{}]+)\s*\{' % tabs, re.M)
    out = []
    pos = body_start
    while True:
        m = pat.search(text, pos, body_stop)
        if not m:
            return out
        start, end = span_at(text, text.index('{', m.start()))
        out.append((m.group(1), m.start(), start + 1, end - 1))
        pos = end


def cells(text, start, stop, name):
    m = re.search(r'(?<![\w-])' + re.escape(name) + r'\s*=\s*<([^>]*)>', text[start:stop])
    if not m:
        return None
    return [int(cell, 0) for cell in m.group(1).split()]


def reserved_range(text, start, stop):
    """(lo, hi, how) covered by a reserved-memory child node."""
    reg = cells(text, start, stop, 'reg')
    if reg and len(reg) == 4:
        lo = (reg[0] << 32) | reg[1]
        return lo, lo + ((reg[2] << 32) | reg[3]), 'reg'
    alloc = cells(text, start, stop, 'alloc-ranges')
    pool = cells(text, start, stop, 'size')
    if alloc and len(alloc) == 4 and pool and len(pool) == 2:
        lo = (alloc[0] << 32) | alloc[1]
        window = lo + ((alloc[2] << 32) | alloc[3])
        return lo, max(window, lo + ((pool[0] << 32) | pool[1])), 'alloc-ranges'
    return None


root = find_node(text, r'^/ \{')
if not root:
    raise SystemExit('root node not found in the decompiled base DTB')
root_children = child_nodes(text, root[1] + 1, root[2] - 1, 1)

memory = None
reserved = None
for name, header, start, stop in root_children:
    if name == 'memory' or name.startswith('memory@'):
        if 'device_type = "memory"' in text[start:stop]:
            memory = (name, start, stop)
    elif name == 'reserved-memory':
        reserved = (name, start, stop)
if not memory:
    raise SystemExit('/memory node not found in the decompiled base DTB')
if not reserved:
    raise SystemExit('/reserved-memory node not found in the decompiled base DTB')

reg = cells(text, memory[1], memory[2], 'reg')
if not reg or len(reg) != 4:
    raise SystemExit('/memory node has no usable reg property')
ddr_lo = (reg[0] << 32) | reg[1]
ddr_hi = ddr_lo + ((reg[2] << 32) | reg[3])
print('base DTB /memory: 0x%08x..0x%08x (%d MiB)' % (ddr_lo, ddr_hi - 1, (ddr_hi - ddr_lo) >> 20))

taken = []
for name, header, start, stop in child_nodes(text, reserved[1] + 1, reserved[2] - 1, 2):
    parsed = reserved_range(text, start, stop)
    if not parsed:
        raise SystemExit('cannot parse the reserved-memory range of %s' % name)
    lo, hi, how = parsed
    taken.append((name, lo, hi))
    if how == 'reg':
        print('base DTB /reserved-memory/%s: reg 0x%08x..0x%08x (%d KiB)' %
              (name, lo, hi, (hi - lo) >> 10))
    else:
        pool = cells(text, start, stop, 'size')
        pool_kib = ((pool[0] << 32) | pool[1]) >> 10 if pool and len(pool) == 2 else 0
        print('base DTB /reserved-memory/%s: window 0x%08x..0x%08x, pool %d MiB' %
              (name, lo, hi, pool_kib >> 10))

dma_hi = dma_base + dma_size
if dma_base % 8:
    raise SystemExit('dma_base 0x%x is not 8-byte aligned' % dma_base)
if dma_base % 4096:
    raise SystemExit('dma_base 0x%x is not 4 KiB aligned' % dma_base)
if dma_hi > 0x100000000:
    raise SystemExit('sensor DMA buffer must stay below 4 GiB: DMA_BASE_HI is 0')
if dma_size <= 0 or dma_size % 4096:
    raise SystemExit('dma_size 0x%x is not a positive multiple of 4 KiB' % dma_size)
if dma_base < ddr_lo or dma_hi > ddr_hi:
    raise SystemExit('sensor DMA buffer 0x%x..0x%x is outside /memory' % (dma_base, dma_hi - 1))
if dma_hi > ddr_hi - guard:
    raise SystemExit('sensor DMA buffer 0x%x..0x%x leaves less than 0x%x below the DDR top' %
                     (dma_base, dma_hi - 1, guard))
for name, lo, hi in taken:
    if dma_base < hi and lo < dma_hi:
        raise SystemExit('sensor DMA buffer 0x%x..0x%x overlaps /reserved-memory/%s' %
                         (dma_base, dma_hi - 1, name))
print('sensor DMA buffer: 0x%08x..0x%08x (%d KiB, no-map, clears every reservation above)' %
      (dma_base, dma_hi - 1, dma_size >> 10))

# no-map: the kernel must never use this window as page cache, and an unmapped
# (not in the linear map) range makes /dev/mem mmap non-cached on arm64.
sensor = (
    '\t\tsensor_dma: sensor-dma-buffer@%x {\n'
    '\t\t\tno-map;\n'
    '\t\t\treg = <0x0 0x%x 0x0 0x%x>;\n'
    '\t\t};\n'
) % (dma_base, dma_base, dma_size)

children = child_nodes(text, reserved[1] + 1, reserved[2] - 1, 2)
if children:
    pos = children[0][1]
else:
    pos = reserved[2] - 1
text = text[:pos] + sensor + '\n' + text[pos:]

node = (
    '\n\t\tpl-ctrl@80000000 {\n'
    '\t\t\tcompatible = "mlk,pl-ctrl-1.0";\n'
    '\t\t\treg = <0x0 0x80000000 0x0 0x10000>;\n'
    '\t\t\tmemory-region = <&sensor_dma>;\n'
    + ('\t\t\tinterrupts = <%s>;\n' % irq_cell if irq_cell else '')
    + '\t\t\tstatus = "okay";\n'
    '\t\t};\n'
)
marker = '\n\t\tserial@F8400000 {'
if marker not in text:
    raise SystemExit('soc child node marker not found')
text = text.replace(marker, node + marker, 1)

# The matching TD bitstream contains vendor Video_NPU (soft operators). Its
# GP1 control window and f2p_irq[15] match the vendor NPU demo DTB exactly.
# Probe performs MMIO writes, so the bitstream digest is checked before this
# node is enabled; merely enabling the kernel driver is insufficient.
if 'soft_npu@' in text:
    raise SystemExit('ERROR: unexpected soft_npu node in HDMI base DTB')
soft_node = (
    '\n\t\tsoft_npu@1f0000000 {\n'
    '\t\t\tcompatible = "anlogic,soft_npu";\n'
    '\t\t\treg = <0x1 0xf0000000 0x0 0x10000>;\n'
    '\t\t\tinterrupts = <0x0 0x61 0x4>;\n'
    '\t\t\tstatus = "okay";\n'
    '\t\t};\n'
)
text = text.replace(marker, soft_node + marker, 1)
print('soft NPU: GP1 0x1f0000000/64 KiB, f2p_irq[15], SPI 97/GIC 129')
open(dst, 'w').write(text)
PY
"$dtc" -I dts -O dtb -o "$out" "$tmp/merged.dts" 2>"$tmp/dtc.err" || {
    cat "$tmp/dtc.err" >&2; exit 1;
}
"$dtc" -I dtb -O dts -o "$tmp/check.dts" "$out" 2>/dev/null
grep -q 'compatible = "mlk,pl-ctrl-1.0"' "$tmp/check.dts" || {
    echo "ERROR: pl_ctrl node missing after compile" >&2; exit 1;
}
grep -q 'sensor-dma-buffer@'"$(printf '%x' "$dma_base")" "$tmp/check.dts" || {
    echo "ERROR: sensor DMA reserved-memory node missing after compile" >&2; exit 1;
}
grep -q 'no-map;' "$tmp/check.dts" || {
    echo "ERROR: sensor DMA reserved-memory node is not no-map" >&2; exit 1;
}
grep -q 'memory-region = <0x' "$tmp/check.dts" || {
    echo "ERROR: pl_ctrl memory-region missing after compile" >&2; exit 1;
}
grep -q 'vdma@80200000' "$tmp/check.dts" || {
    echo "ERROR: VDMA node missing after compile" >&2; exit 1;
}
grep -q 'vdmafb@80100000' "$tmp/check.dts" || {
    echo "ERROR: vdmafb node missing after compile" >&2; exit 1;
}
python3 - "$tmp/check.dts" <<'PY'
import re
import sys

text = open(sys.argv[1]).read()
soft = re.search(r'soft_npu@1f0000000 \{(.*?)\n\t\t\};', text, re.S)
if not soft:
    raise SystemExit('ERROR: soft_npu node missing after compile')
for prop, expected in [('reg', [1, 0xf0000000, 0, 0x10000]), ('interrupts', [0, 97, 4])]:
    cells = re.search(prop + r' = <([^>]+)>;', soft.group(1))
    if not cells or [int(v, 0) for v in cells.group(1).split()] != expected:
        raise SystemExit('ERROR: soft_npu ' + prop + ' mismatch')
if 'compatible = "anlogic,soft_npu";' not in soft.group(1):
    raise SystemExit('ERROR: soft_npu compatible mismatch')
m = re.search(r'hard_npu@63F00000 \{(.*?)\n\t\t\};', text, re.S)
if not m:
    raise SystemExit('ERROR: hard_npu node missing after compile')
if not re.search(r'compatible = "anlogic,hard_npu";', m.group(1)):
    raise SystemExit('ERROR: hard_npu compatible missing after compile')
print('NPU check: hard_npu 0x63F00000 and soft_npu 0x1F0000000 declared')
PY
python3 - "$tmp/check.dts" "$dma_base" "$dma_size" "$pl_irq_cell" <<'PY'
import re
import sys

path, base_s, size_s, irq_cell = sys.argv[1:5]
base = int(base_s, 0)
size = int(size_s, 0)
text = open(path).read()

name = 'sensor-dma-buffer@%x' % base
m = re.search(r'\n\t\t' + re.escape(name) + r' \{(.*?)\n\t\t\};', text, re.S)
if not m:
    raise SystemExit('ERROR: %s missing after compile' % name)
block = m.group(1)
if not re.search(r'^\t\t\tno-map;$', block, re.M):
    raise SystemExit('ERROR: %s is not no-map' % name)
reg = re.search(r'reg = <0x([0-9a-f]+) 0x([0-9a-f]+) 0x([0-9a-f]+) 0x([0-9a-f]+)>;', block)
if not reg:
    raise SystemExit('ERROR: %s has no parsable reg property' % name)
cells = [int(value, 16) for value in reg.groups()]
if ((cells[0] << 32) | cells[1]) != base or ((cells[2] << 32) | cells[3]) != size:
    raise SystemExit('ERROR: %s reg does not match the configured buffer' % name)
phandle = re.search(r'phandle = <(0x[0-9a-f]+)>;', block)
if not phandle:
    raise SystemExit('ERROR: %s has no phandle' % name)
pl = re.search(r'\n\t\tpl-ctrl@80000000 \{(.*?)\n\t\t\};', text, re.S)
if not pl:
    raise SystemExit('ERROR: pl-ctrl node missing after compile')
region = re.search(r'memory-region = <(0x[0-9a-f]+)>;', pl.group(1))
if not region:
    raise SystemExit('ERROR: pl-ctrl has no memory-region property')
if int(region.group(1), 16) != int(phandle.group(1), 16):
    raise SystemExit('ERROR: pl-ctrl memory-region does not point at %s' % name)
if irq_cell:
    irq = re.search(r'interrupts = <([^>]*)>;', pl.group(1))
    if not irq:
        raise SystemExit('ERROR: pl-ctrl has no interrupts property although pl_irq is set')
    got = [int(cell, 0) for cell in irq.group(1).split()]
    want = [int(cell, 0) for cell in irq_cell.split()]
    if got != want:
        raise SystemExit('ERROR: pl-ctrl interrupts %s != requested %s' % (got, want))
    print('pl-ctrl IRQ check: interrupts = <%s>, SPI %d, GIC ID %d' %
          (' '.join('0x%x' % cell for cell in got), got[1], got[1] + 32))
elif re.search(r'interrupts = <', pl.group(1)):
    raise SystemExit('ERROR: pl-ctrl has an interrupts property although pl_irq=off')
else:
    print('pl-ctrl IRQ check: no interrupts property, polling-only as requested')
print('reserved-memory check: %s base=0x%x size=0x%x no-map, memory-region -> phandle %s' %
      (name, base, size, phandle.group(1)))
PY
echo "device tree -> $out"
ls -l "$out"
