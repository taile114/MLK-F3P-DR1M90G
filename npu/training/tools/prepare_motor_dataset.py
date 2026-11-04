"""Build auditable, motor-separated FKP-DVGT inputs from UT motor CSV archives.

The stable23_v1 features deliberately differ from the recovered full-FFT
implementation. They require training a new checkpoint. Existing model sources
are never edited. Only the destination data directory is written.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import shutil
import sys
import zipfile
from collections import Counter
from datetime import datetime
from pathlib import Path
from xml.etree import ElementTree as ET

import numpy as np
import pandas as pd

VERSION = "motor_stable23_v1"
FS = 10240.0
SEGMENT = 10000
WINDOW = 10
SPLITS = {2: "train", 3: "train", 5: "train", 6: "val", 7: "test"}
EXPECTED = {2: (108, 28), 3: (103, 26), 5: (110, 28), 6: (109, 27), 7: (107, 27)}
CHANNELS = ["Current 1", "Current 2", "Current 3", "Voltage 1", "Voltage 2",
            "Voltage 3", "Accelerometer 1", "Accelerometer 2", "Microphone",
            "Tachometer", "Temperature"]
FEATURES = ["mean", "sample_std", "root_amplitude", "rms", "abs_peak",
            "skewness", "kurtosis", "crest_factor", "clearance_factor",
            "shape_factor", "impulse_factor", "spectral_mean_power",
            "spectral_power_variance", "spectral_power_skewness",
            "spectral_power_kurtosis", "frequency_centroid_hz",
            "frequency_std_hz", "frequency_rms_hz", "frequency_shape_hz",
            "frequency_ratio", "frequency_cv", "frequency_skewness",
            "frequency_kurtosis"]


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2,
                                    allow_nan=False) + "\n", encoding="utf-8")


def write_csv(path, rows):
    rows = list(rows)
    if not rows:
        raise ValueError(f"No rows for {path}")
    with Path(path).open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def ratio(a, b, tolerance=0.0):
    a, b = np.broadcast_arrays(np.asarray(a, dtype=np.float64),
                               np.asarray(b, dtype=np.float64))
    return np.divide(a, b, out=np.zeros_like(a), where=np.abs(b) > tolerance)


def stable_features(x):
    """[10000,11] -> [11,23], finite float64; mean removed for spectral features.

    Time moments f1-f11 retain the recovered definitions when non-degenerate.
    Spectral moments use a one-sided, Parseval-normalized AC power spectrum.
    Frequency moments use power-normalized weights, in Hz. Degenerate AC
    spectra yield zero frequency features, never sentinel values.
    """
    x = np.asarray(x, dtype=np.float64)
    if x.shape != (SEGMENT, 11) or not np.isfinite(x).all():
        raise ValueError("Expected finite [10000,11]")
    n = len(x)
    mean = x.mean(axis=0)
    centered = x - mean
    # Suppress numerical residuals of constant/quantized slow sensor traces.
    constant = np.ptp(x, axis=0) <= 1e-10 * np.maximum(1.0, np.max(np.abs(x), axis=0))
    centered[:, constant] = 0.0
    std = np.sqrt(np.sum(centered ** 2, axis=0) / (n - 1))
    root = np.mean(np.sqrt(np.abs(x)), axis=0) ** 2
    rms = np.sqrt(np.mean(x ** 2, axis=0))
    peak = np.max(np.abs(x), axis=0)
    absmean = np.mean(np.abs(x), axis=0)
    time = [mean, std, root, rms, peak,
            ratio(np.sum(centered ** 3, axis=0), (n - 1) * std ** 3),
            ratio(np.sum(centered ** 4, axis=0), (n - 1) * std ** 4),
            ratio(peak, rms), ratio(peak, root), ratio(rms, absmean),
            ratio(peak, absmean)]
    power = np.abs(np.fft.rfft(centered, axis=0)) ** 2 / n ** 2
    power[1:-1] *= 2
    power[0] = 0.0
    freq = np.fft.rfftfreq(n, d=1 / FS)[:, None]
    pm = power.mean(axis=0)
    pc = power - pm
    pv = np.mean(pc ** 2, axis=0)
    spectral = [pm, pv, ratio(np.mean(pc ** 3, axis=0), pv ** 1.5),
                ratio(np.mean(pc ** 4, axis=0), pv ** 2)]
    total = power.sum(axis=0)
    weights = ratio(power, total)
    centroid = np.sum(weights * freq, axis=0)
    delta = freq - centroid
    spread = np.sqrt(np.maximum(0, np.sum(weights * delta ** 2, axis=0)))
    m2 = np.sum(weights * freq ** 2, axis=0)
    m4 = np.sum(weights * freq ** 4, axis=0)
    spectral.extend([centroid, spread, np.sqrt(m2), np.sqrt(ratio(m4, m2)),
                     ratio(m2, np.sqrt(m4)), ratio(spread, centroid, 1e-9),
                     ratio(np.sum(weights * delta ** 3, axis=0), spread ** 3, 1e-18),
                     ratio(np.sum(weights * delta ** 4, axis=0), spread ** 4, 1e-24)])
    out = np.stack(time + spectral, axis=1)
    if out.shape != (11, 23) or not np.isfinite(out).all():
        raise ValueError("Non-finite stable features")
    return out, constant


def clean_segment(x):
    """Causal forward fill <= 10 missing samples/channel, max run 3; no leading fill."""
    x = x.copy()
    bad = ~np.isfinite(x)
    filled = int(bad.sum())
    if not filled:
        return x, 0, ""
    for col in range(11):
        ids = np.flatnonzero(bad[:, col])
        if not len(ids):
            continue
        if len(ids) > 10 or bad[0, col]:
            return None, 0, f"missing_limit_or_leading_missing:{CHANNELS[col]}"
        longest = max(len(g) for g in np.split(ids, np.where(np.diff(ids) != 1)[0] + 1))
        if longest > 3:
            return None, 0, f"missing_run_over_3:{CHANNELS[col]}"
        for idx in ids:
            x[idx, col] = x[idx - 1, col]
    return x, filled, ""


def document_masks(path):
    ns = {"w": "http://schemas.openxmlformats.org/wordprocessingml/2006/main"}
    with zipfile.ZipFile(path) as archive:
        root = ET.fromstring(archive.read("word/document.xml"))
    masks, current = [], None
    for para in root.findall(".//w:p", ns):
        text = "".join(node.text or "" for node in para.findall(".//w:t", ns)).strip()
        match = re.match(r"^Motor\s+(\d+)\s*[：:]", text)
        if match:
            current = int(match.group(1))
        match = re.match(r"d00\[c\(\((\d+) \* 10000 \+ 1\):\((\d+) \* 10000\)\), (\d+)\] <-", text)
        if current in SPLITS and match:
            start, end, channel = map(int, match.groups())
            masks.append({"motor_id": current, "start_block": start, "end_block": end,
                          "channel_1based": channel, "channel": CHANNELS[channel - 1],
                          "evidence": text})
    if not masks:
        raise ValueError("Expected annotated corruption intervals in the processing record")
    return masks


def self_check():
    zero, _ = stable_features(np.zeros((SEGMENT, 11)))
    if np.any(zero):
        raise AssertionError("Zero signal features must all be zero")
    dc, _ = stable_features(np.full((SEGMENT, 11), 37.5))
    if np.any(dc[:, 11:]) or np.any(dc[:, 5:7]):
        raise AssertionError("Constant signals must have stable zero spectral/central moments")
    time = np.arange(SEGMENT) / FS
    frequency = 100 * FS / SEGMENT  # exact FFT bin, 102.4 Hz
    x = np.repeat(np.sin(2 * np.pi * frequency * time)[:, None], 11, axis=1)
    sine, _ = stable_features(x)
    scaled, _ = stable_features(3 * x)
    np.testing.assert_allclose(sine[:, 15], frequency, atol=1e-7)
    np.testing.assert_allclose(sine[:, 15:], scaled[:, 15:], atol=1e-5)
    np.testing.assert_array_equal(sine, stable_features(x)[0])
    damaged = x.copy()
    damaged[10:12, 0] = np.nan
    fixed, count, reason = clean_segment(damaged)
    assert not reason and count == 2 and np.isfinite(fixed).all()
    damaged[20:24, 0] = np.nan
    assert clean_segment(damaged)[0] is None
    damaged = x.copy()
    damaged[0, 0] = np.nan
    assert clean_segment(damaged)[0] is None
    return {"zero_signal": "pass", "constant_signal": "pass", "known_frequency": "pass",
            "frequency_scale_invariance": "pass", "determinism": "pass", "causal_missing_rules": "pass"}


def run(args):
    base, destination = args.data_root.resolve(), args.output.resolve()
    raw_root = args.raw_root.resolve() if args.raw_root else base.parent / "raw" / "run_to_failure_5motors"
    if destination.exists():
        if not (args.resume and (destination / "BUILD_IN_PROGRESS.json").exists()
                and not (destination / "quality_report.json").exists()):
            raise FileExistsError(f"Refusing to overwrite completed or unrelated output {destination}")
    checks = self_check()
    masks = document_masks(base / "数据处理过程记录-Motor.docx")
    destination.mkdir(parents=True, exist_ok=args.resume)
    write_json(destination / "BUILD_IN_PROGRESS.json", {"status": "in_progress", "version": VERSION})
    write_json(destination / "document_quality_masks.json", masks)
    raw_root.mkdir(parents=True, exist_ok=True)
    arrays, inventories, segments, cycles, summaries = {}, [], [], [], {}
    for motor, split in SPLITS.items():
        source = "Motor data CSV  2-5.zip" if motor in (2, 3, 5) else f"Motor{motor}.zip"
        with zipfile.ZipFile(base / source) as archive:
            names = sorted(n for n in archive.namelist()
                           if n.startswith(f"Motor{motor}/SteadyState/") and n.endswith(".csv"))
            dates = sorted({re.search(r"(\d{14})\.csv$", n).group(1)[:8] for n in names})
            if (len(names), len(dates)) != EXPECTED[motor]:
                raise ValueError(f"Unexpected inventory for Motor{motor}")
            date_map = {day: i + 1 for i, day in enumerate(dates)}
            total_cycles = len(dates)
            for day in dates:
                cycles.append({"motor_id": motor, "split": split, "date": day,
                               "cycle_id": date_map[day], "terminal_cycle_id": total_cycles,
                               "rul_cycles": total_cycles - date_map[day],
                               "files_observed": sum(day in n for n in names),
                               "basis": "chronological_observed_test_session",
                               "endpoint_convention": "last_recorded_session_zero"})
            kept, info = [], []
            motor_masks = [m for m in masks if m["motor_id"] == motor]
            for file_idx, name in enumerate(names):
                target = raw_root / name
                if not target.resolve().is_relative_to(raw_root.resolve()):
                    raise ValueError("Unsafe archive path")
                member = archive.getinfo(name)
                blob = archive.read(name)  # zipfile validates the member CRC
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.exists():
                    if hashlib.sha256(target.read_bytes()).digest() != hashlib.sha256(blob).digest():
                        raise ValueError(f"Existing raw CSV differs: {target}")
                else:
                    target.write_bytes(blob)
                frame = pd.read_csv(target)
                canonical = {str(c).replace(".", " ").strip(): c for c in frame.columns}
                if any(c not in canonical for c in CHANNELS):
                    raise ValueError(f"Missing sensor columns: {name}")
                x = frame[[canonical[c] for c in CHANNELS]].apply(pd.to_numeric, errors="coerce").to_numpy(dtype=np.float64)
                t = pd.to_numeric(frame[canonical["X_Value"]], errors="coerce").to_numpy(dtype=np.float64)
                if x.shape[1] != 11 or not 2 <= len(x) <= 2 * SEGMENT:
                    raise ValueError(f"Unexpected CSV shape: {name}, {x.shape}")
                if not np.isfinite(t).all() or not np.all(np.diff(t) > 0):
                    raise ValueError(f"Invalid local time axis: {name}")
                inferred_fs = (len(t) - 1) / (t[-1] - t[0])
                if abs(inferred_fs / FS - 1) > 1e-4:
                    raise ValueError(f"Unexpected sampling rate: {name}, {inferred_fs}")
                timestamp = re.search(r"(\d{14})\.csv$", name).group(1)
                cycle = date_map[timestamp[:8]]
                inventories.append({"motor_id": motor, "split": split, "source_archive": source,
                                    "member": name, "raw_path": str(target), "rows": len(x),
                                    "crc32": f"{member.CRC:08x}", "sha256": hashlib.sha256(blob).hexdigest(),
                                    "timestamp": timestamp, "cycle_id": cycle,
                                    "trailing_incomplete_samples": len(x) % SEGMENT,
                                    "rul_cycles": total_cycles - cycle, "inferred_fs_hz": inferred_fs,
                                    "nonfinite_sensor_values": int((~np.isfinite(x)).sum())})
                for part in range(2):
                    block = 2 * file_idx + part
                    affected = [m for m in motor_masks if m["start_block"] <= block < m["end_block"]]
                    record = {"motor_id": motor, "split": split, "block_id": block,
                              "source_member": name, "sample_start": part * SEGMENT,
                              "sample_end_exclusive": (part + 1) * SEGMENT,
                              "available_samples": max(0, min(SEGMENT, len(x) - part * SEGMENT)),
                              "timestamp": timestamp, "cycle_id": cycle, "rul_cycles": total_cycles - cycle,
                              "status": "kept", "reason": "", "filled_values": 0,
                              "constant_channels": "", "known_bad_channels": ""}
                    if record["available_samples"] != SEGMENT:
                        record.update(status="quarantined", reason="incomplete_10000_sample_segment")
                    elif affected:
                        record.update(status="quarantined", reason="document_annotated_sensor_corruption",
                                      known_bad_channels=";".join(sorted({m["channel"] for m in affected})))
                    else:
                        chunk, filled, reason = clean_segment(x[part * SEGMENT:(part + 1) * SEGMENT])
                        if reason:
                            record.update(status="quarantined", reason=reason)
                        else:
                            feature, constant = stable_features(chunk)
                            record.update(filled_values=filled, constant_channels=";".join(np.asarray(CHANNELS)[constant]))
                            kept.append(feature)
                            info.append(record)
                    segments.append(record)
                if (file_idx + 1) % 25 == 0 or file_idx + 1 == len(names):
                    print(json.dumps({"motor": motor, "csv_done": file_idx + 1, "csv_total": len(names),
                                      "valid_segments": len(kept)}, ensure_ascii=False), flush=True)
            if len(kept) < WINDOW:
                raise ValueError(f"Too few valid segments: Motor{motor}")
            arrays[motor] = (np.stack(kept), info)
            summaries[str(motor)] = {"split": split, "csv_files": len(names), "observed_cycles": len(dates),
                                     "valid_segments": len(kept), "quarantined_segments": 2 * len(names) - len(kept),
                                     "samples": len(kept) - WINDOW + 1,
                                     "terminal_date": dates[-1]}
    write_csv(destination / "file_inventory.csv", inventories)
    write_csv(destination / "cycle_map.csv", cycles)
    write_csv(destination / "segment_quality.csv", segments)
    train_features = np.concatenate([v[0] for k, v in arrays.items() if SPLITS[k] == "train"])
    minimum, maximum = train_features.min(axis=0), train_features.max(axis=0)
    constant_features = maximum - minimum <= 1e-10 * np.maximum(1.0, np.maximum(np.abs(minimum), np.abs(maximum)))
    scale = np.where(constant_features, 1.0, maximum - minimum)
    np.savez(destination / "normalization.npz", minimum=minimum, maximum=maximum, scale=scale,
             constant_features=constant_features, channels=np.asarray(CHANNELS), features=np.asarray(FEATURES))
    feature_dir = destination / "features"
    feature_dir.mkdir()
    sample_index, split_reports = [], {}
    for split in ("train", "val", "test"):
        xs, ys, motor_ids, cycle_ids, end_blocks = [], [], [], [], []
        split_index = 0
        for motor, (feature, info) in arrays.items():
            if SPLITS[motor] != split:
                continue
            np.savez_compressed(feature_dir / f"Motor{motor}.npz", features=feature,
                                block_ids=np.asarray([r["block_id"] for r in info]),
                                cycle_ids=np.asarray([r["cycle_id"] for r in info]),
                                rul_cycles=np.asarray([r["rul_cycles"] for r in info], dtype=np.float32))
            normalized = (feature - minimum) / scale
            normalized[:, constant_features] = 0.0
            for start in range(len(info) - WINDOW + 1):
                current = info[start:start + WINDOW]
                end = current[-1]
                blocks = [r["block_id"] for r in current]
                xs.append(normalized[start:start + WINDOW])
                ys.append(end["rul_cycles"])
                motor_ids.append(motor)
                cycle_ids.append(end["cycle_id"])
                end_blocks.append(end["block_id"])
                skipped = blocks[-1] - blocks[0] + 1 - WINDOW
                sample_index.append({"split": split, "sample_index": split_index, "motor_id": motor,
                                     "start_cycle": current[0]["cycle_id"], "end_cycle": end["cycle_id"],
                                     "rul_cycles": end["rul_cycles"], "end_timestamp": end["timestamp"],
                                     "end_member": end["source_member"], "input_block_ids": json.dumps(blocks),
                                     "skipped_quarantined_blocks_in_history": skipped,
                                     "history_policy": "10_valid_observations_in_chronological_order"})
                split_index += 1
        x64 = np.stack(xs)
        x = x64.astype(np.float32)
        y = np.asarray(ys, dtype=np.float32)
        if not np.isfinite(x).all() or not np.isfinite(y).all() or (y < 0).any():
            raise ValueError(f"Nonfinite or negative training data: {split}")
        np.savez_compressed(destination / f"{split}.npz", samples=x, labels=y,
                            motor_ids=np.asarray(motor_ids, dtype=np.int16),
                            cycle_ids=np.asarray(cycle_ids, dtype=np.int16),
                            end_block_ids=np.asarray(end_blocks, dtype=np.int32))
        split_reports[split] = {"samples": len(x), "shape": list(x.shape), "dtype": str(x.dtype),
                                "label_min": float(y.min()), "label_max": float(y.max()),
                                "input_min": float(x.min()), "input_max": float(x.max()),
                                "fraction_outside_train_minmax": float(np.mean((x < -1e-6) | (x > 1 + 1e-6))),
                                "motor_ids": sorted(set(motor_ids))}
        if split == "test":
            board = destination / "board_replay"
            board.mkdir()
            canonical = x.reshape(len(x), 110, 23).transpose(0, 2, 1).copy().astype("<f4")
            canonical.tofile(board / "test_input_f32.bin")
            write_csv(board / "test_labels.csv", [{"sample_index": i, "rul_cycles": float(v)} for i, v in enumerate(y)])
            write_json(board / "input_contract.json", {"version": VERSION, "dtype": "little_endian_float32",
                       "shape": list(canonical.shape), "one_sample_shape": [1, 23, 110],
                       "bytes_per_sample": 23 * 110 * 4, "normalization_already_applied": True,
                       "rul_unit": "observed_aging_test_session", "labels_are_not_inputs": True})
    write_csv(destination / "sample_index.csv", sample_index)
    rules = {"feature_version": VERSION, "sensor_order": CHANNELS, "feature_order": FEATURES,
             "sample_rate_hz": FS, "segment_length": SEGMENT, "segment_stride": SEGMENT,
             "window": WINDOW, "window_stride": 1, "split_motors": SPLITS,
             "raw_boundaries": "never cross a CSV boundary during signal feature extraction",
             "history": "10 valid observations; known quarantined gaps allowed and explicitly indexed; never crosses motors",
             "cycle_basis": "one observed dated test session per cycle for the five selected motors",
             "rul_definition": "terminal_observed_cycle_id - window_end_cycle_id",
             "endpoint_limit": "terminal recorded session is the run-to-failure endpoint convention, not an exact failure timestamp",
             "normalization": "train-only per-sensor/per-feature min-max in float64, then float32; no val/test clipping",
             "constant_train_features": "zeroed in every split; threshold 1e-10 * max(1,abs(train extrema))",
             "missing": "causal forward fill at most 10 samples/channel and runs <=3; no leading missing; otherwise quarantine",
             "document_masks": "LHS of annotated intervals mapped to nominal two 10000-sample slots per chronological CSV; quarantine, no future copy",
             "short_files": "only complete 10000-sample segments retained; partial tails quarantined, no zero padding",
             "spectral": "demean; rfft; power abs(fft)^2/N^2; double non-DC/non-Nyquist bins; DC=0; frequencies Hz",
             "formula_changes": ["single-sided AC spectrum for all 12 spectral features",
                                 "spectral power skewness/kurtosis normalized by power variance",
                                 "frequency moments normalized by total spectral power",
                                 "constant/zero-energy channels have zero undefined central and frequency moments"],
             "notes": ["Motor2 has 28 dated sessions, versus 27 in the local summary; no cycle inferred by dividing file count by four",
                       "Motor6 has five records on 2011-08-22, all assigned to the same session",
                       "Motor7 final filename date is 2012-06-08 after 2011-12-19; preserved verbatim and not interpreted as extra aging cycles",
                       "Requires new training; not feature-identical to the recovered full_fft_23_features pipeline"],
             "source": {"processing_record": str(base / "数据处理过程记录-Motor.docx"),
                        "run_to_failure_reference": "https://papers.phmsociety.org/index.php/ijphm/article/download/2285/1267"}}
    write_json(destination / "preprocessing.json", rules)
    quality = {"status": "data_checks_passed", "feature_version": VERSION,
               "source_csv_count": len(inventories), "raw_rows": sum(r["rows"] for r in inventories),
               "segment_count": len(segments), "kept_segments": sum(r["status"] == "kept" for r in segments),
               "quarantined_segments": sum(r["status"] != "kept" for r in segments),
               "quarantine_reasons": dict(Counter(r["reason"] for r in segments if r["status"] != "kept")),
               "forward_filled_values": sum(r["filled_values"] for r in segments),
               "constant_train_feature_count": int(constant_features.sum()),
               "self_checks": checks, "motors": summaries, "splits": split_reports,
               "source_code_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    write_json(destination / "quality_report.json", quality)
    (destination / "BUILD_IN_PROGRESS.json").unlink()
    print(json.dumps(quality, ensure_ascii=False, indent=2), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--raw-root", type=Path)
    parser.add_argument("--resume", action="store_true", help="Resume only a marked, incomplete build")
    args = parser.parse_args()
    run(args)
