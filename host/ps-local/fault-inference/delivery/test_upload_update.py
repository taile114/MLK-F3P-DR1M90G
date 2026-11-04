"""Linux pseudo-terminal tests; no physical serial device or board is accessed."""
import base64
import hashlib
import json
import os
from pathlib import Path
import pty
import re
import select
import tempfile
import termios
import threading
import unittest

from upload_update import upload


class UploadTest(unittest.TestCase):
    def test_local_checksum_rejected_before_opening_port(self):
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory) / 'package.tar.gz'
            package.write_bytes(b'data')
            Path(str(package) + '.sha256').write_text('0' * 64 + '  package.tar.gz\n')
            with self.assertRaisesRegex(ValueError, 'Local.*checksum'):
                upload(package, '/dev/nonexistent-test-port', Path(directory) / 'logs')

    def transfer(self, corrupt=False):
        master, slave = pty.openpty()
        previous = termios.tcgetattr(slave)
        stop = threading.Event()
        received = bytearray()
        state = {'renamed': False}

        def console():
            pending = bytearray()
            while not stop.is_set():
                if not select.select([master], [], [], 0.1)[0]:
                    continue
                pending.extend(os.read(master, 65536))
                while b'\r' in pending:
                    wire, _, rest = pending.partition(b'\r')
                    pending[:] = rest
                    text = wire.decode('ascii')
                    begin = re.search(r'__F3P_BEGIN_[a-f0-9]+', text)
                    end = re.search(r'__F3P_END_[a-f0-9]+', text)
                    if not begin or not end:
                        continue
                    output = ''
                    if 'base64 -d >>' in text:
                        received.extend(base64.b64decode(re.search(r"printf '%s' '([^']*)'", text)[1]))
                    elif 'sha256sum' in text:
                        digest = '0' * 64 if corrupt else hashlib.sha256(received).hexdigest()
                        output = digest + '  /tmp/fixture'
                    elif ' && mv ' in text:
                        state['renamed'] = True
                    response = (text + '\r\n' + begin[0] + '\r\n' + output + '\r\n' + end[0] + ':0\r\n').encode()
                    os.write(master, response)

        thread = threading.Thread(target=console, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory(prefix='serial test ') as directory:
                package = Path(directory) / 'package.tar.gz'
                payload = bytes(range(256)) * 17 + b'end'
                package.write_bytes(payload)
                Path(str(package) + '.sha256').write_text(hashlib.sha256(payload).hexdigest() + '  package.tar.gz\n')
                logs = Path(directory) / 'logs'
                if corrupt:
                    with self.assertRaisesRegex(ValueError, 'Remote.*mismatch'):
                        upload(package, os.ttyname(slave), logs)
                else:
                    report = upload(package, os.ttyname(slave), logs)
                    self.assertEqual(report['status'], 'passed')
                self.assertEqual(bytes(received), payload)
                self.assertEqual(state['renamed'], not corrupt)
                report = json.loads(next(logs.glob('transfer-*.json')).read_text())
                self.assertEqual(report['status'], 'failed' if corrupt else 'passed')
                self.assertTrue(report['port_released'])
                self.assertEqual(termios.tcgetattr(slave), previous)
        finally:
            stop.set()
            thread.join(2)
            os.close(slave)
            os.close(master)

    def test_verified_upload_and_terminal_restoration(self):
        self.transfer()

    def test_remote_checksum_failure_does_not_publish_archive(self):
        self.transfer(corrupt=True)


if __name__ == '__main__':
    unittest.main()
