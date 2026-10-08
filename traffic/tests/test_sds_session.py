"""Offline tests: no SDS server, USB, serial port or debugger is opened."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("sds_session", Path(__file__).resolve().parents[1] / "sds_session.py")
session = importlib.util.module_from_spec(spec)
spec.loader.exec_module(session)


class SessionLifecycle(unittest.TestCase):
    """Virtual clock/server: these tests never communicate with a board."""
    READY = "SDSIO-Client USB device connected.\nsdsFlags = 0x10000000.\n"
    OPEN = "Record:   CameraIn (CameraIn.2.sds)\nRecord:   Detections (Detections.2.sds)\n"
    CLOSED = "Closed:   CameraIn (CameraIn.2.sds)\nClosed:   Detections (Detections.2.sds)\n"

    def setUp(self):
        self.now = 0.0
        self.returncode = None
        self.output = []
        self.commands = []
        self.events = []
        self.responses = {}
        clock = mock.patch.object(session.time, "monotonic", side_effect=lambda: self.now)
        clock.start()
        self.addCleanup(clock.stop)

    def poll(self):
        return self.returncode

    def pump(self, seconds):
        self.now += seconds
        pending, self.events = self.events, []
        for when, value in sorted(pending, key=lambda event: event[0]):
            if when > self.now:
                self.events.append((when, value))
            elif isinstance(value, int):
                self.returncode = value
            else:
                self.output.append(value)

    def command(self, key):
        self.commands.append((key, self.now))
        for delay, text in self.responses.get(key, []):
            self.events.append((self.now + delay, text))

    def run_session(self, mode="record"):
        return session.run_session(self, self.pump, self.command, self.output,
                                   mode=mode, seconds=5, timeout=30,
                                   connect_timeout=20, stream_timeout=15)

    def test_delayed_connection_open_and_close(self):
        self.events = [(12, self.READY)]  # Longer than the former four-second wait.
        self.responses = {b"R": [(3, self.OPEN)], b"S": [(8, self.CLOSED)]}
        _, log, elapsed = self.run_session()
        self.assertEqual([key for key, _ in self.commands], [b"R", b"S"])
        start, stop = [when for _, when in self.commands]
        self.assertGreaterEqual(start, 12)
        self.assertGreaterEqual(stop - start, 8)  # Open latency + full requested clip.
        self.assertGreaterEqual(self.now - stop, 8)  # Drain beyond former five seconds.
        self.assertGreaterEqual(elapsed, 16)
        self.assertIn(self.CLOSED, log)

    def test_no_client_sends_no_record_or_stop(self):
        self.events = [(1, "SDSIO-Server waiting for USB SDSIO-Client...\n")]
        with self.assertRaisesRegex(RuntimeError, "no live SDSIO-Client.*no recording/playback command"):
            self.run_session()
        self.assertEqual(self.commands, [])

    def test_usb_connected_but_no_flags_is_not_ready(self):
        self.events = [(1, "SDSIO-Client USB device connected.\n")]
        with self.assertRaisesRegex(RuntimeError, "no live SDSIO-Client"):
            self.run_session("play")
        self.assertEqual(self.commands, [])

    def test_disconnect_invalidates_old_alive_flag(self):
        self.events = [(1, self.READY + "USB SDSIO-Client disconnected.\n")]
        with self.assertRaisesRegex(RuntimeError, "no live SDSIO-Client"):
            self.run_session()
        self.assertEqual(self.commands, [])

    def test_waiting_invalidates_old_alive_flag(self):
        self.assertIsNone(session.client_flags(self.READY + "SDSIO-Server waiting for USB SDSIO-Client...\n"))
        self.assertEqual(session.client_flags(self.READY + "sdsFlags = 0x30000000.\n"), 0x30000000)

    def test_fragmented_flags_wait_for_complete_report(self):
        self.events = [(1, "sdsFlags = 0x10000000"), (2, ".\n")]
        self.responses = {b"R": [(0, self.OPEN)], b"S": [(0, self.CLOSED)]}
        self.run_session()
        self.assertGreaterEqual(self.commands[0][1], 2)

    def test_active_session_is_not_overwritten(self):
        self.events = [(1, "sdsFlags = 0x90000000.\n")]
        with self.assertRaisesRegex(RuntimeError, "already has START set"):
            self.run_session()
        self.assertEqual(self.commands, [])

    def test_server_exit_during_connection(self):
        self.events = [(1, 1)]
        with self.assertRaisesRegex(RuntimeError, "exited with status 1"):
            self.run_session()
        self.assertEqual(self.commands, [])
        self.assertLess(self.now, 2)

    def test_missing_stream_has_start_timeout(self):
        self.events = [(1, self.READY)]
        self.responses = {b"R": [(1, "Record:   CameraIn (CameraIn.2.sds)\n")]}
        with self.assertRaisesRegex(RuntimeError, "did not open both.*15 seconds"):
            self.run_session()
        self.assertEqual([key for key, _ in self.commands], [b"R"])

    def test_missing_close_has_drain_timeout(self):
        self.events = [(1, self.READY)]
        self.responses = {b"R": [(1, self.OPEN)],
                          b"S": [(1, "Closed:   CameraIn (CameraIn.2.sds)\n")]}
        with self.assertRaisesRegex(RuntimeError, "did not close both.*after Stop"):
            self.run_session()
        self.assertEqual([key for key, _ in self.commands], [b"R", b"S"])

    def test_playback_waits_for_flags_and_both_closes(self):
        self.events = [(8, self.READY)]
        self.responses = {b"P": [(0.1, "sdsFlags = 0x10000000.\n"),
                                  (1, "sdsFlags = 0xB0000000.\n"),
                                  (3, self.CLOSED + "sdsFlags = 0x30000000.\n")]}
        _, log, elapsed = self.run_session("play")
        self.assertEqual([key for key, _ in self.commands], [b"P"])
        self.assertGreaterEqual(self.commands[0][1], 8)
        self.assertGreaterEqual(elapsed, 3)
        self.assertIn(self.CLOSED, log)

    def test_playback_stop_without_close_is_failure(self):
        self.events = [(1, self.READY)]
        self.responses = {b"P": [(1, "sdsFlags = 0xB0000000.\n"),
                                  (2, "sdsFlags = 0x30000000.\n")]}
        with self.assertRaisesRegex(RuntimeError, "stopped playback before both streams closed"):
            self.run_session("play")

    def test_playback_timeout(self):
        self.events = [(1, self.READY)]
        with self.assertRaisesRegex(RuntimeError, "playback did not finish within 30 seconds"):
            self.run_session("play")

    def test_fatal_error_with_cleanup_closes_is_not_success(self):
        fatal = "=== FATAL ERROR === : Data integrity error - protocol mismatch. Restart the SDSIO-Client.\n"
        for mode in ("record", "play"):
            with self.subTest(mode=mode):
                self.now = 0
                self.output.clear()
                self.events = [(1, self.READY)]
                self.responses = {b"R": [(0, self.OPEN)],
                                  b"S": [(1, fatal + self.CLOSED)],
                                  b"P": [(1, fatal + self.CLOSED)]}
                with self.assertRaisesRegex(RuntimeError, "fatal protocol/transport error"):
                    self.run_session(mode)

    def test_fatal_error_during_recording_does_not_send_stop(self):
        self.events = [(1, self.READY)]
        self.responses = {b"R": [(0, self.OPEN), (2, "=== FATAL ERROR ===\n")]}
        with self.assertRaisesRegex(RuntimeError, "Restart the SDSIO-Client"):
            self.run_session()
        self.assertEqual([key for key, _ in self.commands], [b"R"])

    def test_nonpositive_or_nonfinite_duration_rejected(self):
        for value in ("0", "-1", "nan", "inf"):
            with self.subTest(value=value), self.assertRaises(session.argparse.ArgumentTypeError):
                session.positive_seconds(value)


class PlaybackValidation(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="sds-session-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.input = self.root / "CameraIn.0.sds"
        self.output = self.root / "Detections.0.p.sds"
        self.log = ("Playback: CameraIn (CameraIn.0.sds)\n"
                    "Record:   Detections (Detections.0.p.sds)\n"
                    "Closed:   CameraIn (CameraIn.0.sds)\n"
                    "Closed:   Detections (Detections.0.p.sds)\n")
        self.records(self.input, [100, 200, 300], 416 * 416 * 3)
        self.records(self.output, [100, 200, 300], 400)

    @staticmethod
    def records(path, slots, size):
        with path.open("wb") as f:
            for slot in slots:
                f.write(struct.pack("<II", slot, size))
                f.write(bytes(size))

    def validate(self):
        return session.validate_playback(self.root, self.log, 0)

    def test_complete(self):
        self.assertEqual(self.validate(), 3)

    def test_closed_but_incomplete(self):
        self.records(self.output, [100], 400)
        with self.assertRaisesRegex(RuntimeError, "incomplete playback: 1/3"):
            self.validate()

    def test_timeslots_out_of_order(self):
        self.records(self.output, [200, 100, 300], 400)
        with self.assertRaisesRegex(RuntimeError, "timeslots"):
            self.validate()

    def test_stale_output(self):
        with self.assertRaisesRegex(RuntimeError, "not updated"):
            session.validate_playback(self.root, self.log, self.output.stat().st_mtime_ns + 1)

    def test_truncated_header(self):
        with self.output.open("ab") as f:
            f.write(b"abc")
        with self.assertRaisesRegex(RuntimeError, "truncated record header"):
            self.validate()

    def test_truncated_payload(self):
        with self.output.open("r+b") as f:
            f.truncate(self.output.stat().st_size - 1)
        with self.assertRaisesRegex(RuntimeError, "truncated record payload"):
            self.validate()

    def test_wrong_payload_size(self):
        self.records(self.output, [100, 200, 300], 396)
        with self.assertRaisesRegex(RuntimeError, "expected 400"):
            self.validate()

    def test_missing_stream(self):
        with self.assertRaisesRegex(RuntimeError, "did not open both"):
            session.validate_playback(self.root, "Closed:   Detections", 0)

    def test_multiple_files(self):
        self.records(self.root / "CameraIn.1.sds", [400, 500], 416 * 416 * 3)
        self.records(self.root / "Detections.1.p.sds", [400, 500], 400)
        self.log += ("Playback: CameraIn (CameraIn.1.sds)\n"
                     "Record:   Detections (Detections.1.p.sds)\n")
        self.assertEqual(self.validate(), 5)

    def recording_log(self):
        self.output = self.output.rename(self.root / "Detections.0.sds")
        return self.log.replace("Playback: CameraIn", "Record:   CameraIn").replace("Detections.0.p.sds", "Detections.0.sds")

    def test_complete_recording(self):
        self.assertEqual(session.validate_recording(self.root, self.recording_log(), 0), 3)

    def test_recording_orphan_detection(self):
        log = self.recording_log()
        self.records(self.output, [100, 200, 300, 400], 400)
        with self.assertRaisesRegex(RuntimeError, "incomplete recording: 4/3"):
            session.validate_recording(self.root, log, 0)

    def test_recording_missing_detection(self):
        log = self.recording_log()
        self.records(self.output, [100, 200], 400)
        with self.assertRaisesRegex(RuntimeError, "incomplete recording: 2/3"):
            session.validate_recording(self.root, log, 0)

    def test_recording_wrong_timeslots(self):
        log = self.recording_log()
        self.records(self.output, [100, 200, 400], 400)
        with self.assertRaisesRegex(RuntimeError, "timeslots"):
            session.validate_recording(self.root, log, 0)

    def test_recording_stale_input(self):
        log = self.recording_log()
        with self.assertRaisesRegex(RuntimeError, "input was not updated"):
            session.validate_recording(self.root, log, self.input.stat().st_mtime_ns + 1)


if __name__ == "__main__":
    unittest.main()
