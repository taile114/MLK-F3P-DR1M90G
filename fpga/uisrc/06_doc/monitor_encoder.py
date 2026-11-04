"""Decode read-only PL UART telemetry to text/CSV; sends no serial commands."""
import argparse
import csv
import struct
import sys
from pathlib import Path


def crc16(data):
    crc = 0xFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


class Decoder:
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data):
        self.buffer.extend(data)
        records = []
        while len(self.buffer) >= 20:
            if self.buffer[:3] != b"\xa5\x5a\x01" or crc16(self.buffer[:20]):
                del self.buffer[0]
                continue
            _, _, flags, count, raw, rpm, error, exception = struct.unpack(
                ">2sBBIiiBB", self.buffer[:18]
            )
            records.append(dict(
                sample_count=count, online=bool(flags & 1),
                overflow=bool(flags & 2), error_event=bool(flags & 4),
                raw_speed=raw, rpm_milli=rpm, error_code=error,
                exception_code=exception,
            ))
            del self.buffer[:20]
        return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port", help="PL USB UART COM port, e.g. COM10")
    source.add_argument("--input", type=Path, help="captured binary UART bytes")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--csv", type=Path)
    args = parser.parse_args()
    if args.input:
        stream = args.input.open("rb")
    else:
        try:
            import serial
        except ImportError:
            parser.error("Live serial reading requires pyserial: python -m pip install pyserial")
        stream = serial.Serial(args.port, args.baud, timeout=1)
    decoder = Decoder()
    csv_file = args.csv.open("w", newline="", encoding="utf-8") if args.csv else None
    writer = None
    try:
        with stream:
            while True:
                data = stream.read(256 if args.input else max(1, stream.in_waiting))
                if not data and args.input:
                    break
                for record in decoder.feed(data):
                    if csv_file:
                        if writer is None:
                            writer = csv.DictWriter(csv_file, fieldnames=list(record))
                            writer.writeheader()
                        writer.writerow(record)
                        csv_file.flush()
                    if record["error_event"]:
                        print(f'ERROR code={record["error_code"]} exception={record["exception_code"]} '
                              f'last_sample={record["sample_count"]}', flush=True)
                    else:
                        print(f'sample={record["sample_count"]} raw={record["raw_speed"]} '
                              f'rpm={record["rpm_milli"] / 1000:.3f} '
                              f'overflow={int(record["overflow"])}', flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        if csv_file:
            csv_file.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
