"""Freeze a protocol-quantized native window against the training implementation.

Run in the training Python environment; Qt tests consume the frozen artifacts and
do not need NumPy/PyTorch or original recordings. No weights are fitted or edited.
"""
from pathlib import Path
import hashlib
import argparse
import json
import sys

import numpy as np
import torch

repo = Path(__file__).resolve().parents[2]
model_root = repo / 'npu/fault-diagnosis/model'
sys.path.insert(0, str(model_root))
from fault_mtl.exploratory import NativeBlockFeatures
from fault_mtl.preprocessing import FeatureNormalizer, NativeRecording, read_vibration_block_metadata
from fault_mtl.schema import INPUT_SHAPES
from fault_mtl.evaluate import load_checkpoint

torch.set_num_threads(2)
cache = model_root / 'cache/active_selection_single_v1'
manifest = json.loads((cache / 'dataset_manifest.json').read_text(encoding='utf-8'))
preprocess = json.loads((cache / 'preprocess.json').read_text(encoding='utf-8'))
rows = [json.loads(line) for line in (cache / 'windows.jsonl').read_text().splitlines()]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--class-index', type=int, choices=(0, 3), default=0)
args = parser.parse_args()
row = next(r for r in rows if r['accepted'] and r['class_target'] == args.class_index and r['split'] == 'test')
metadata = {}
for path in manifest['source_block_metadata']:
    metadata.update(read_vibration_block_metadata(path))
recording = NativeRecording(row['source_data'], vibration_metadata=metadata)
start, end = row['window_start_us'], row['window_end_us']
ids = row['quality_observations']['native_block_ids']
blocks = []
vibration_t, vibration_x, vibration_ids = [], [], []
quantized_metadata = {}
for block_id in ids:
    ix = np.flatnonzero(recording.arrays['vibration_block'] == block_id)
    t = recording.arrays['vibration_t_us'][ix]
    values = recording.arrays['vibration_value'][ix].astype(np.float32)
    rate = int(round(metadata[block_id]['rate_hz']))
    times = int(t[0]) + np.arange(len(ix), dtype=np.int64) * 1000000 // rate
    if times[0] < start or times[-1] >= end:
        continue
    blocks.append(dict(channel=2, t0Us=str(times[0]), rateHz=rate, count=len(ix), axes=3,
                       values=values.reshape(-1).tolist()))
    vibration_t.extend(times.tolist())
    vibration_x.extend(values.tolist())
    vibration_ids.extend([block_id] * len(ix))
    quantized_metadata[block_id] = dict(channel=2, axes=3, rate_hz=rate)

class Recording:
    def channel(self, name):
        return self.arrays[name + '_t_us'], self.arrays[name + '_value']

reference = Recording()
reference.arrays = dict(vibration_t_us=np.asarray(vibration_t, dtype=np.int64),
                        vibration_value=np.asarray(vibration_x, dtype=np.float32),
                        vibration_block=np.asarray(vibration_ids, dtype=np.int64))
reference.vibration_block_metadata = quantized_metadata
for channel, name, rate in [(0, 'temperature', 1), (1, 'current', 50), (3, 'speed', 10)]:
    t, values = recording.slice(name, start, end)
    values = values.astype(np.float32)
    if name == 'temperature':
        values[:] = np.float32(23.6)
    reference.arrays[name + '_t_us'] = t
    reference.arrays[name + '_value'] = values
    for instant, value in zip(t, values):
        blocks.append(dict(channel=channel, t0Us=str(int(instant)), rateHz=rate, count=1,
                           axes=1, values=[float(value)]))

native = NativeBlockFeatures(reference, preprocess['dsp'])
features, quality, reasons = native.extract(end)
assert not reasons, reasons
normalizer = FeatureNormalizer(preprocess['parameters'])
transformed = normalizer.transform(features)
flat = np.concatenate([transformed[name].reshape(-1) for name in INPUT_SHAPES]).astype('<f4')
assert flat.size == 27683
checkpoint = model_root / 'runs/active_selection_single_v1_seed42/best.pt'
model, _ = load_checkpoint(checkpoint, torch.device('cpu'))
expectations = []
with torch.no_grad():
    for temperature in (23.6, 35.0, 35.1, 41.0, 46.0):
        reference.arrays['temperature_value'][:] = np.float32(temperature)
        f, _, why = native.extract(end)
        assert not why
        batch = {name: torch.from_numpy(value[None]) for name, value in normalizer.transform(f).items()}
        output = model.forward_batch(batch)
        logits = output['class_logits'][0].numpy().astype(np.float64)
        selected = int(logits.argmax())
        masked = logits.copy()
        if temperature <= 35:
            masked[3] = -np.inf
        chosen = int(masked.argmax())
        expectations.append(dict(temperature=temperature, rawClass=selected, guardedClass=chosen))

out = repo / 'host/tests/fixtures/fault-preprocess'
out.mkdir(parents=True, exist_ok=True)
prefix = 'thermal-' if args.class_index == 3 else ''
(out / (prefix + 'expected.f32')).write_bytes(flat.tobytes())
document = dict(schema='mlk.fault.preprocess_fixture.v1', startUs=str(start), endUs=str(end), blocks=blocks,
    expectedFile=prefix + 'expected.f32', expectedSha256=hashlib.sha256(flat.tobytes()).hexdigest(),
    branches=[dict(name=name, count=int(np.prod(shape))) for name, shape in INPUT_SHAPES.items()],
    expectations=expectations, quality=quality,
    source=dict(windowId=row['window_id'], sourceSha256=row['source_sha256'],
                adaptation='native rate rounded to wire uint16 Hz; samples float32; T fixed to 23.6 C',
                referenceImplementation='NativeBlockFeatures.extract + FeatureNormalizer.transform',
                checkpointSha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
                preprocessSha256=hashlib.sha256((cache / 'preprocess.json').read_bytes()).hexdigest()))
(out / (prefix + 'window.json')).write_text(json.dumps(document, separators=(',', ':')) + '\n', encoding='utf-8')
print(json.dumps(dict(blocks=len(blocks), expectedSha256=document['expectedSha256'], expectations=expectations)))
