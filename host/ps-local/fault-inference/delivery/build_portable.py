"""Build a standalone Linux/Windows serial delivery ZIP using the Python standard library."""
import argparse
import hashlib
from pathlib import Path
import zipfile

here = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
package = here / 'fault-live-update.tar.gz'
expected = (here / (package.name + '.sha256')).read_text(encoding='utf-8').split()[0]
assert hashlib.sha256(package.read_bytes()).hexdigest() == expected
files = {name: here / name for name in ('README.md', 'upload_update.py', 'Upload-Update.ps1', package.name, package.name + '.sha256')}
files['f3p_serial.ps1'] = here.parents[3] / 'npu/tools/f3p_serial.ps1'
args.output.parent.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(args.output, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
    for name, path in files.items():
        archive.write(path, 'fault-live-update-portable/' + name)
with zipfile.ZipFile(args.output) as archive:
    assert archive.testzip() is None
    for name, path in files.items():
        assert archive.read('fault-live-update-portable/' + name) == path.read_bytes()
digest = hashlib.sha256(args.output.read_bytes()).hexdigest()
args.output.with_name(args.output.name + '.sha256').write_bytes((digest + '  ' + args.output.name + '\n').encode())
print(digest, args.output)
