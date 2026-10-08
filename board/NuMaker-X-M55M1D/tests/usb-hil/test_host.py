"""Offline host-verifier tests. No PyUSB import or device discovery."""
import struct
import unittest
from unittest.mock import patch
import verify_usb as h


class FakeDevice:
    def __init__(self, mps=512):
        self.s = dict.fromkeys(h.FIELDS, 0)
        self.s.update(magic=h.MAGIC, version=h.VERSION, configured=1, mps=mps, resets=1)
        self.seed = 0
        self.repeat = False
        self.received = b""
        self.echo = b""

    def ctrl_transfer(self, kind, command, value, index, data, timeout=0):
        if kind == 0xC0 and command == h.STATUS:
            return struct.pack("<15Ii", *(self.s[key] for key in h.FIELDS))
        if kind == 0 and command == 9:
            self.s["configured"] = value
            return 0
        if command == h.ABORT:
            self.s["active"] = 0
            self.s["aborts"] += 1
            return 0
        self.s.update(case_id=self.s["case_id"] + 1, mode=command, requested=value,
                      out_count=0, in_count=0, out_callbacks=0, in_callbacks=0,
                      active=2 if command == h.ARM_IN else 1)
        self.seed = index & ~h.REARM
        self.repeat = bool(index & h.REARM)
        self.received = b""
        return 0

    def write(self, ep, data, timeout=0):
        assert ep == h.OUT and self.s["active"] == 1
        self.received += bytes(data)
        self.s["out_count"] = len(self.received)
        if len(self.received) == self.s["requested"] or len(data) % self.s["mps"] != 0 or not data:
            assert self.received == h.pattern(len(self.received), self.seed)
            self.s["out_callbacks"] += 1
            self.s["active"] = 0
            if self.s["mode"] == h.LOOPBACK:
                self.echo = self.received
                self.s["active"] = 2
            elif self.repeat:
                self.repeat = False
                self.seed += 1
                self.received = b""
                self.s["active"] = 1
        return len(data)

    def read(self, ep, length, timeout=0):
        assert ep == h.IN and self.s["active"] == 2
        data = self.echo if self.s["mode"] == h.LOOPBACK else h.pattern(self.s["requested"], self.seed)
        self.s["in_count"] = len(data)
        self.s["in_callbacks"] += 1
        self.s["active"] = 0
        if self.repeat:
            self.repeat = False
            self.seed += 1
            self.s["active"] = 2
        return data

    def set_configuration(self, value):
        self.s["configured"] = value


class HostTests(unittest.TestCase):
    def test_boundary_matrix_both_speeds(self):
        for mps in (64, 512):
            with self.subTest(mps=mps), patch.object(h.time, "sleep"), patch("builtins.print"):
                runner = h.Verifier(FakeDevice(mps))
                self.assertEqual(runner.run(), mps)
                self.assertEqual(len(runner.cases), 62)

    def test_wrong_firmware(self):
        device = FakeDevice()
        device.s["magic"] = 0
        with self.assertRaisesRegex(RuntimeError, "wrong firmware"):
            h.Verifier(device)

    def test_short_status(self):
        with self.assertRaisesRegex(RuntimeError, "short status"):
            h.decode_status(bytes(63))

    def test_sticky_target_failure(self):
        device = FakeDevice()
        device.s["errors"] = 32
        with self.assertRaisesRegex(RuntimeError, "target failure"):
            h.Verifier(device)

    def test_pattern_error(self):
        device = FakeDevice()
        runner = h.Verifier(device)
        with patch.object(device, "read", return_value=b"bad"):
            with self.assertRaisesRegex(RuntimeError, "data mismatch"):
                runner.one(h.ARM_IN, 3)

    def test_short_host_write(self):
        runner = h.Verifier(FakeDevice())
        with patch.object(runner.device, "write", return_value=0):
            with self.assertRaisesRegex(RuntimeError, "short host write"):
                runner.one(h.ARM_OUT, 5)

    def test_unexpected_reset(self):
        runner = h.Verifier(FakeDevice())
        runner.device.s["resets"] += 1
        with self.assertRaisesRegex(RuntimeError, "unexpected bus reset"):
            runner.arm(h.ARM_OUT, 5, 17)

    def test_duplicate_completion(self):
        runner = h.Verifier(FakeDevice())
        runner.device.s["out_callbacks"] = 2
        with self.assertRaisesRegex(RuntimeError, "extra completion"):
            runner.wait(1, 0, 5, 0)

    def test_missing_completion(self):
        runner = h.Verifier(FakeDevice())
        with self.assertRaisesRegex(RuntimeError, "bad completion"):
            runner.wait(1, 0, 5, 0)

    def test_timeout(self):
        runner = h.Verifier(FakeDevice(), timeout_ms=1)
        runner.device.s["active"] = 1
        with patch.object(h.time, "monotonic", side_effect=[0, 1]):
            with self.assertRaisesRegex(RuntimeError, "completion timeout"):
                runner.wait(1, 0, 5, 0)

    def test_late_change(self):
        runner = h.Verifier(FakeDevice())
        previous = runner.status()
        runner.device.s["in_callbacks"] = 1
        with patch.object(h.time, "sleep"):
            with self.assertRaisesRegex(RuntimeError, "state changed"):
                runner.stable(previous)


if __name__ == "__main__":
    unittest.main()
