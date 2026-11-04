"""Linux offline tests using isolated file trees and a simulated Qt service."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest


class InstallerTest(unittest.TestCase):
    def setUp(self):
        if os.name != 'posix':
            self.skipTest('Linux shell required')
        old = os.environ.get('MLK_TEST_OLD_APPLICATION')
        base = os.environ.get('MLK_TEST_BASE_RUNTIME')
        if not old or not base:
            self.skipTest('Set MLK_TEST_OLD_APPLICATION and MLK_TEST_BASE_RUNTIME')
        self.temp = tempfile.TemporaryDirectory(prefix='mlk-installer-')
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.root = self.work / 'root'
        self.target = self.root / 'opt/qt_play/mlk_ps_local'
        self.target.parent.mkdir(parents=True)
        shutil.copyfile(old, str(self.target))
        self.old_hash = hashlib.sha256(self.target.read_bytes()).hexdigest()
        self.runtime = self.root / 'opt/mlk_fault-packages/baseline'
        shutil.copytree(base, str(self.runtime))
        self.link = self.root / 'opt/mlk_fault'
        self.link.symlink_to('mlk_fault-packages/baseline')
        self.service = self.root / 'etc/init.d/S99qtapp'
        self.service.parent.mkdir(parents=True)
        self.service.write_text('''#!/bin/sh
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
case "$1" in
stop) rm -f "$root/running" ;;
start) if [ -e "$root/fail-next-start" ]; then rm "$root/fail-next-start"; exit 1; fi; touch "$root/running" ;;
status) test -e "$root/running" ;;
esac
''')
        self.service.chmod(0o755)
        (self.root / 'running').touch()
        self.payload = self.work / 'fault-live-update'
        package = Path(__file__).with_name('fault-live-update.tar.gz')
        with tarfile.open(str(package)) as archive:
            for member in archive.getmembers():
                assert member.isfile() and member.name.startswith('fault-live-update/')
                assert '..' not in Path(member.name).parts and not member.name.startswith('/')
                dest = self.work / member.name
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(archive.extractfile(member).read())

    def run_install(self):
        env = os.environ.copy()
        env['MLK_UPDATE_ROOT'] = str(self.root)
        return subprocess.run(['/bin/sh', str(self.payload / 'install.sh')], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)

    def test_install_and_idempotent(self):
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.target.read_bytes(), (self.payload / 'mlk_ps_local').read_bytes())
        self.assertEqual((self.link / 'libmlk_fault.so').read_bytes(), (self.payload / 'runtime/libmlk_fault.so').read_bytes())
        for name in ('weights.bin', 'weights.tsv', 'thresholds.bin', 'qparams.tsv', 'preprocess.json'):
            self.assertEqual((self.link / name).read_bytes(), (self.runtime / name).read_bytes())
        before = self.root / 'opt/mlk_fault.previous'
        self.assertEqual(os.readlink(str(before)), 'mlk_fault-packages/baseline')
        backup = self.target.with_name(self.target.name + '.before-thermal-' + self.old_hash[:16])
        self.assertEqual(hashlib.sha256(backup.read_bytes()).hexdigest(), self.old_hash)
        again = self.run_install()
        self.assertEqual(again.returncode, 0, again.stdout)
        self.assertIn('already installed', again.stdout)
        self.assertEqual(os.readlink(str(before)), 'mlk_fault-packages/baseline')

    def test_asset_mismatch_rejected_before_switch(self):
        (self.runtime / 'weights.bin').write_bytes(b'incompatible weights')
        result = self.run_install()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(os.readlink(str(self.link)), 'mlk_fault-packages/baseline')
        self.assertEqual(hashlib.sha256(self.target.read_bytes()).hexdigest(), self.old_hash)
        self.assertTrue((self.root / 'running').exists())

    def test_service_failure_restores_application_and_runtime(self):
        (self.root / 'fail-next-start').touch()
        result = self.run_install()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('restoring application and runtime link', result.stdout)
        self.assertEqual(hashlib.sha256(self.target.read_bytes()).hexdigest(), self.old_hash)
        self.assertEqual(os.readlink(str(self.link)), 'mlk_fault-packages/baseline')
        self.assertTrue((self.root / 'running').exists())


if __name__ == '__main__':
    unittest.main()
