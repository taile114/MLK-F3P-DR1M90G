#!/usr/bin/env python3
"""Check the staged FF6 payload and AArch64 runtime dependencies before packing."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

flash = Path(__file__).resolve().parent
root = Path(sys.argv[1]).resolve()
readelf = flash / 'vendor/tools/aarch64-linux/bin/aarch64-linux-gnu-readelf'


def image_file(name):
    # Absolute links inside an image refer to the image root, not the host.
    pending = list(PurePosixPath('/' + str(name).lstrip('/')).parts[1:])
    resolved = []
    links = 0
    while pending:
        part = pending.pop(0)
        if part in ('', '.'):
            continue
        if part == '..':
            if not resolved:
                raise SystemExit('Rootfs symlink escapes image: ' + str(name))
            resolved.pop()
            continue
        candidate = root.joinpath(*resolved, part)
        if candidate.is_symlink():
            links += 1
            if links > 40:
                raise SystemExit('Rootfs symlink loop: ' + str(name))
            target = os.readlink(candidate)
            if target.startswith('/'):
                resolved = []
            pending = list(PurePosixPath(target).parts) + pending
            pending = [p for p in pending if p != '/']
        else:
            resolved.append(part)
    return root.joinpath(*resolved)


model_dir = root / 'opt/mlk_rul'
firmware = root / 'lib/firmware/soc_hdmi.bit'
stamp = root / 'lib/firmware/soc_hdmi.npu.sha256'
if hashlib.sha256(firmware.read_bytes()).hexdigest() != stamp.read_text().strip():
    raise SystemExit('Rootfs NPU firmware digest mismatch')
model = json.loads((model_dir / 'model.json').read_text())
required = {'weights.bin', 'weights.tsv', 'qparams_ff6.tsv', 'input_f32.bin', 'libmlk_rul.so'}
if (model.get('model_id') != 'motor_stable23_v1' or model.get('execution_plan') != 'ff6'
        or model.get('npu_branches') != 6 or model.get('shape') != [1, 23, 110]
        or model.get('quantization_plan') != 'qparams_ff6.tsv'
        or set(model.get('sha256', {})) != required):
    raise SystemExit('Rootfs has no sealed FF6 six-branch model')
for name in required:
    if hashlib.sha256((model_dir / name).read_bytes()).hexdigest() != model['sha256'][name]:
        raise SystemExit('Rootfs model checksum mismatch: ' + name)
if (model_dir / 'input_f32.bin').stat().st_size != model['windows'] * 2530 * 4:
    raise SystemExit('Rootfs replay input length mismatch')

search_dirs = ['opt/qt_play/qt_lib/lib', 'opt/armnn_lib/lib', 'lib', 'usr/lib']
queue = ['opt/mlk_rul/libmlk_rul.so', 'opt/armnn_lib/lib/libarmnn.so.32',
         'opt/qt_play/mlk_ps_local', 'usr/bin/mlk-fault', 'usr/sbin/wpa_supplicant', 'usr/sbin/wpa_cli',
         'usr/sbin/wpa_passphrase', 'usr/sbin/quectel-CM', 'usr/sbin/chronyd', 'usr/bin/chronyc']
checker = image_file('opt/mlk_rul-validation/check_engine')
if checker.is_file():
    queue.append('opt/mlk_rul-validation/check_engine')
    for name in ('weights.bin', 'weights.tsv', 'qparams_ff6.tsv', 'input_f32.bin', 'libmlk_rul.so'):
        if image_file('opt/mlk_rul-validation/' + name) != model_dir / name:
            raise SystemExit('Rootfs validation link mismatch: ' + name)
    if image_file('opt/mlk_rul-validation/reference_f32.bin').stat().st_size != model['windows'] * 4:
        raise SystemExit('Rootfs validation reference length mismatch')
seen = set()
while queue:
    path = image_file(queue.pop(0))
    if path in seen:
        continue
    if not path.is_file():
        raise SystemExit('Missing rootfs executable/library: ' + str(path.relative_to(root)))
    if not re.search(r'Machine:\s+AArch64', subprocess.check_output(
            [str(readelf), '-h', str(path)], text=True)):
        raise SystemExit('Non-AArch64 rootfs executable/library: ' + str(path))
    seen.add(path)
    dynamic = subprocess.check_output([str(readelf), '-d', str(path)], text=True)
    for needed in re.findall(r'\(NEEDED\).*?\[([^]]+)\]', dynamic):
        candidates = [str(Path(d) / needed) for d in search_dirs[:2]]
        candidates += [str(path.parent.relative_to(root) / needed)]
        candidates += [str(Path(d) / needed) for d in search_dirs[2:]]
        found = next((p for p in candidates if image_file(p).is_file()), None)
        if found is None:
            raise SystemExit(f'Missing rootfs dependency {needed} required by {path.relative_to(root)}')
        queue.append(found)
    program = subprocess.check_output([str(readelf), '-l', str(path)], text=True)
    interpreter = re.search(r'Requesting program interpreter: ([^]]+)\]', program)
    if interpreter:
        queue.append(interpreter.group(1))
launcher = (root / 'opt/qt_play/run_arm_app.sh').read_text()
if '/opt/armnn_lib/lib' not in launcher:
    raise SystemExit('Qt launcher does not include ArmNN in LD_LIBRARY_PATH')
for name in ('etc/init.d/S40mlknetwork', 'usr/bin/mlk_wifi_dhcp', 'etc/init.d/S49mlktime',
             'usr/bin/mlk_time_status', 'usr/bin/mlk_set_time',
             'etc/init.d/S98plctrl', 'etc/init.d/S99qtapp'):
    if not os.access(root / name, os.X_OK):
        raise SystemExit('Rootfs startup script missing/not executable: ' + name)
print(f'Rootfs NPU check: sealed FF6, {model["windows"]} windows, {len(seen)} AArch64 ELF files and dependencies OK')

for name in ('regulatory.db', 'regulatory.db.p7s', 'aic8800D80/fmacfw_8800d80_u02.bin'):
    if not (root / 'lib/firmware' / name).is_file():
        raise SystemExit('Missing network firmware: ' + name)
print('Rootfs network check: WiFi/cellular userspace, dependencies, firmware and startup scripts OK')
if not (root / 'etc/chrony.conf').is_file():
    raise SystemExit('Missing client NTP configuration')
if 'denyinterfaces wlan*' not in (root / 'etc/dhcpcd.conf').read_text().splitlines():
    raise SystemExit('WiFi must not be managed by both dhcpcd and udhcpc')
