import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "06_doc"))
from monitor_encoder import Decoder, crc16


class MonitorTest(unittest.TestCase):
    def test_known_modbus_crc(self):
        self.assertEqual(crc16(bytes.fromhex("01 03 00 20 00 02")), 0xC1C5)
        self.assertEqual(crc16(bytes.fromhex("01 03 00 20 00 02 C5 C1")), 0)

    def test_chunking_crc_resync_and_signed_values(self):
        payload = struct.pack(">2sBBIiiBB", b"\xa5\x5a", 1, 1, 42, -4096, -600000, 0, 0)
        packet = payload + crc16(payload).to_bytes(2, "little")
        corrupt = bytearray(packet)
        corrupt[10] ^= 1
        decoder = Decoder()
        data = b"noise" + corrupt + b"\xa5" + packet + packet
        results = []
        for byte in data:
            results.extend(decoder.feed(bytes([byte])))
        self.assertEqual(len(results), 2)
        self.assertEqual(results[0]["sample_count"], 42)
        self.assertEqual(results[0]["raw_speed"], -4096)
        self.assertEqual(results[0]["rpm_milli"], -600000)

    def test_actual_rtl_capture(self):
        records = Decoder().feed((ROOT / "out/sim/telemetry.bin").read_bytes())
        self.assertEqual(len(records), 3)
        self.assertEqual(records[0]["rpm_milli"], 146484)
        self.assertEqual(records[1]["error_code"], 2)
        self.assertTrue(records[1]["error_event"])
        self.assertEqual(records[2]["raw_speed"], -4096)
        self.assertEqual(records[2]["rpm_milli"], -600000)


if __name__ == "__main__":
    unittest.main()
