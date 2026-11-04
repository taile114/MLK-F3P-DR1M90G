#!/usr/bin/env python3
"""Linux serial upload, Python standard library only; never installs automatically."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import select
import time
import uuid


class Serial:
    def __init__(self, device):
        import fcntl
        import termios
        import tty
        self.fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.previous = None
        self.exclusive = False
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.ioctl(self.fd, termios.TIOCEXCL)
            self.exclusive = True
            self.previous = termios.tcgetattr(self.fd)
            tty.setraw(self.fd, termios.TCSANOW)
            attrs = termios.tcgetattr(self.fd)
            attrs[4] = attrs[5] = termios.B115200
            attrs[2] &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.HUPCL | termios.CRTSCTS)
            attrs[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        except BaseException:
            self.close()
            raise

    def close(self):
        import fcntl
        import termios
        if self.fd is None:
            return
        try:
            if self.previous is not None:
                termios.tcsetattr(self.fd, termios.TCSANOW, self.previous)
        finally:
            try:
                if self.exclusive:
                    fcntl.ioctl(self.fd, termios.TIOCNXCL)
            finally:
                os.close(self.fd)
                self.fd = None

    def write(self, data):
        deadline = time.monotonic() + 15
        while data:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([], [self.fd], [], remaining)[1]:
                raise TimeoutError('Serial write timeout')
            try:
                data = data[os.write(self.fd, data):]
            except BlockingIOError:
                continue

    def command(self, code):
        tag = uuid.uuid4().hex[:12]
        begin, end = '__F3P_BEGIN_' + tag, '__F3P_END_' + tag
        wire = f'''printf '\\n{begin}\\n'; {{ {code}; }}; printf '\\n{end}:%s\\n' "$?"\r'''.encode()
        if len(wire) >= 500:
            raise ValueError('Console command too long')
        self.write(wire)
        pattern = re.compile(rb'(?ms)^' + begin.encode() + rb'\r?\n(.*?)\r?\n' + end.encode() + rb':(\d+)\r?\n')
        response = bytearray()
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if not select.select([self.fd], [], [], max(0, deadline - time.monotonic()))[0]:
                break
            try:
                chunk = os.read(self.fd, 65536)
            except BlockingIOError:
                continue
            if not chunk:
                raise ConnectionError('Serial device disconnected')
            response.extend(chunk)
            match = pattern.search(response)
            if match:
                output = match[1].decode('utf-8', errors='replace').strip()
                if int(match[2]):
                    raise RuntimeError('Remote command failed: ' + output)
                return output
            if len(response) > 4 * 1024 * 1024:
                raise RuntimeError('Unexpected console output exceeds limit')
        raise TimeoutError('No Linux command response; stop and inspect the console: ' +
                           response[-4096:].decode('utf-8', errors='replace'))


def upload(package, device, logdir):
    package = Path(package).resolve()
    payload = package.read_bytes()
    digest = hashlib.sha256(payload).hexdigest()
    expected = Path(str(package) + '.sha256').read_text(encoding='utf-8').split()[0]
    if not re.fullmatch(r'[0-9a-fA-F]{64}', expected) or digest != expected.lower():
        raise ValueError('Local update archive checksum mismatch')
    tag = uuid.uuid4().hex[:12]
    remote, part = f'/tmp/fault-live-update-{tag}.tar.gz', f'/tmp/cdx-upl-{tag}'
    logdir = Path(logdir)
    logdir.mkdir(parents=True, exist_ok=True)
    report_path = logdir / f'transfer-{tag}.json'
    report = dict(port=device, format='115200 8N1 None', remote_path=remote,
                  partial_path=part, bytes=len(payload), sha256=digest, status='running')
    connection = None
    try:
        connection = Serial(device)
        connection.write(b'\x15\r')
        connection.command('test "$(id -u)" = 0')
        connection.command(f"test ! -e '{remote}' && (set -C; : > '{part}')")
        for offset in range(0, len(payload), 192):
            encoded = base64.b64encode(payload[offset:offset + 192]).decode('ascii')
            connection.command(f"printf '%s' '{encoded}' | base64 -d >> '{part}'")
            if offset % 24576 == 0:
                print(f'upload_bytes={min(offset + 192, len(payload))}/{len(payload)}', flush=True)
        actual = connection.command(f"sha256sum '{part}'").split()[0]
        if actual != digest:
            raise ValueError('Remote upload SHA-256 mismatch; partial file retained')
        connection.command(f"test ! -e '{remote}' && mv '{part}' '{remote}'")
        report['status'] = 'passed'
    except BaseException as error:
        report['status'], report['error'] = 'failed', str(error) or type(error).__name__
        raise
    finally:
        try:
            if connection is not None:
                connection.close()
        finally:
            report['port_released'] = True
            report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
            print('Report:', report_path)
    destination = '/tmp/fault-live-' + tag
    commands = [f'mkdir -p {destination}', f'gzip -dc {remote} | tar -xf - -C {destination}',
                f'sh {destination}/fault-live-update/install.sh']
    print('Transfer verified. Reopen the Linux serial console and execute:')
    print('\n'.join(commands))
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True, help='/dev/ttyACM0 or /dev/serial/by-id/...')
    parser.add_argument('--package', type=Path, default=Path(__file__).with_name('fault-live-update.tar.gz'))
    parser.add_argument('--log-dir', type=Path, default=Path(__file__).with_name('logs'))
    args = parser.parse_args()
    if os.name != 'posix':
        parser.error('This uploader requires Linux; use Upload-Update.ps1 on Windows')
    upload(args.package, args.port, args.log_dir)
