"""Build an application/library update while keeping all trained assets unchanged."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--app', type=Path, required=True)
parser.add_argument('--runtime', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
sha = lambda value: hashlib.sha256(value).hexdigest()
here = Path(__file__).resolve().parent
manifest_bytes = (args.runtime / 'model.json').read_bytes()
manifest = json.loads(manifest_bytes)
assets = ('weights.bin', 'weights.tsv', 'qparams.tsv', 'thresholds.bin', 'preprocess.json')
assert set(manifest['files']) == set(assets) | {'libmlk_fault.so'}
for name, digest in manifest['files'].items():
    assert sha((args.runtime / name).read_bytes()) == digest, name
for path in (args.app, args.runtime / 'libmlk_fault.so'):
    assert path.read_bytes()[:6] == b'\x7fELF\x02\x01'
    assert int.from_bytes(path.read_bytes()[18:20], 'little') == 183
unchanged = ''.join(manifest['files'][name] + '  ' + name + '\n' for name in assets).encode()
full = unchanged + (manifest['files']['libmlk_fault.so'] + '  libmlk_fault.so\n'
    + sha(manifest_bytes) + '  model.json\n').encode()
version = 'f3p-fault-single-v1-' + sha(manifest_bytes)[:16]
files = {'mlk_ps_local': args.app.read_bytes(), 'install.sh': (here / 'install_update.sh').read_bytes(),
         'README.md': (here / 'README.md').read_bytes(),
         'runtime/model.json': manifest_bytes,
         'runtime/libmlk_fault.so': (args.runtime / 'libmlk_fault.so').read_bytes(),
         'runtime/UNCHANGED_ASSETS.sha256': unchanged,
         'runtime/FULL_RUNTIME.sha256': full, 'runtime/version': (version + '\n').encode()}
files['SHA256SUMS'] = ''.join(sha(data) + '  ' + name + '\n' for name, data in files.items()).encode()
args.output.parent.mkdir(parents=True, exist_ok=True)
with tarfile.open(args.output, 'w:gz') as archive:
    for name, data in files.items():
        info = tarfile.TarInfo('fault-live-update/' + name)
        info.size = len(data)
        info.mode = 0o755 if name in ('mlk_ps_local', 'install.sh') else 0o644
        archive.addfile(info, io.BytesIO(data))
with tarfile.open(args.output) as archive:
    for name, data in files.items():
        assert archive.extractfile('fault-live-update/' + name).read() == data
digest = sha(args.output.read_bytes())
args.output.with_name(args.output.name + '.sha256').write_bytes((digest + '  ' + args.output.name + '\n').encode())
print(json.dumps({'app': sha(files['mlk_ps_local']), 'library': manifest['files']['libmlk_fault.so'],
                  'version': version, 'archive': digest, 'unchanged_assets': list(assets)}))
