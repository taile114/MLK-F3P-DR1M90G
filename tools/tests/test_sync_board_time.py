"""Exercise the embedded console client against a timed, simulated UART."""
import ast
from pathlib import Path
from types import SimpleNamespace
import re
import unittest


SOURCE = Path(__file__).resolve().parents[1] / 'sync_board_time.sh'
embedded = SOURCE.read_text().split("<<'PY'\n", 1)[1].rsplit('\nPY', 1)[0]
tree = ast.parse(embedded)
selected = [node for node in tree.body if isinstance(node, (ast.FunctionDef, ast.ClassDef))
            and node.name in ('SyncError', 'wait_for_session', 'run_periodic')]


class Console:
    def __init__(self, events=()):
        self.now = 0.0
        self.events = list(events)
        self.writes = []
        self.closed = []
        self.logins = 0
        self.ns = dict(re=re, args=SimpleNamespace(device='simulated UART'), synced=0)
        self.ns['time'] = SimpleNamespace(monotonic=lambda: self.now, sleep=self.sleep)
        self.ns['os'] = SimpleNamespace(write=self.write, close=self.closed.append)
        self.ns['read_for'] = self.read
        exec(compile(ast.Module(body=selected, type_ignores=[]), str(SOURCE), 'exec'), self.ns)

    def sleep(self, seconds):
        self.now += seconds

    def read(self, fd, seconds):
        self.now += seconds
        data = b''
        while self.events and self.events[0][0] <= self.now:
            data += self.events.pop(0)[1]
        return data

    def write(self, fd, data):
        self.writes.append((self.now, data))
        if data == b'operator\r':
            self.events.append((self.now + 0.1, b'Password: '))
        elif data == b'example-password\r' or data == b'\r':
            self.events.append((self.now + 0.1, b'\r\n# '))

    def login(self, timeout=12):
        self.ns['wait_for_session'](3, 'operator', 'example-password', timeout)


class BootProtection(unittest.TestCase):
    def test_boot_countdown_is_receive_only(self):
        uart = Console([(0.1, b'U-Boot 2021.01\r\nHit any key to stop autoboot:  3'),
                        (1.1, b'\b2'), (2.1, b'\b1'), (3.1, b'\b0\r\nStarting kernel ...'),
                        (4.1, b'\r\nLinux version 6.1\r\nWelcome to Buildroot\r\n'),
                        (5.1, b'buildroot login: ')])
        uart.login()
        self.assertEqual([data for _, data in uart.writes],
                         [b'operator\r', b'example-password\r'])
        self.assertTrue(all(stamp >= 5.1 for stamp, _ in uart.writes))

    def test_stopped_uboot_is_never_given_login_or_clock_input(self):
        uart = Console([(0.1, b'U-Boot 2021.01\r\n=> ')])
        with self.assertRaises(uart.ns['SyncError']):
            uart.login()
        self.assertEqual(uart.writes, [])

    def test_fragmented_login_and_password(self):
        uart = Console([(0.1, b'buildroot lo'), (0.6, b'gin: ')])
        uart.login()
        self.assertEqual([data for _, data in uart.writes],
                         [b'operator\r', b'example-password\r'])

    def test_quiet_existing_linux_session_gets_one_delayed_probe(self):
        uart = Console()
        uart.login()
        self.assertEqual(len(uart.writes), 1)
        self.assertEqual(uart.writes[0][1], b'\r')
        self.assertGreaterEqual(uart.writes[0][0], 5.0)

    def test_buffered_old_shell_prompt_does_not_override_new_boot(self):
        uart = Console([(0.1, b'\r\n# \r\nU-Boot 2021.01\r\n=> ')])
        with self.assertRaises(uart.ns['SyncError']):
            uart.login()
        self.assertEqual(uart.writes, [])

    def test_periodic_rounds_revalidate_and_release_the_console(self):
        uart = Console()
        connections = []

        def connect():
            fd = len(connections) + 10
            connections.append(fd)
            return fd

        uart.ns['connect'] = connect
        uart.ns['sync_once'] = lambda fd, periodic: ('example', 0, 0, 0)
        uart.ns['run_periodic'](30, 2)
        self.assertEqual(connections, [10, 11])
        self.assertEqual(uart.closed, [10, 11])


if __name__ == '__main__':
    unittest.main()
