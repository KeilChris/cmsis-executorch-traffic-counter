"""Exercise the real CLI with fake USB modules; never import/open real USB."""
from contextlib import redirect_stderr, redirect_stdout
import io
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

from test_host import FakeDevice
import verify_usb as h


class USBError(Exception):
    errno = 5
    backend_error_code = -99


class LoggingTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="usb-hil-log-test-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.path = self.root / "result.log"
        self.device = FakeDevice()
        self.device.iProduct = 1
        self.device.iSerialNumber = 2
        self.device.get_active_configuration = lambda: types.SimpleNamespace(
            bConfigurationValue=self.device.s["configured"])
        self.device.reset = self.reset
        # Import statements in main resolve only these explicitly fake modules.
        self.usb = types.ModuleType("usb")
        self.usb.core = types.ModuleType("usb.core")
        self.usb.core.USBError = USBError
        self.usb.core.find = Mock(return_value=[self.device])
        self.usb.util = types.ModuleType("usb.util")
        self.usb.util.get_string = lambda d, i: {1: h.PRODUCT, 2: h.SERIAL}[i]
        self.usb.util.claim_interface = Mock()
        self.usb.util.dispose_resources = Mock()
        self.usb.backend = types.ModuleType("usb.backend")
        self.usb.backend.libusb1 = types.ModuleType("usb.backend.libusb1")
        self.usb.backend.libusb1.get_backend = Mock(return_value=object())
        self.modules = {m.__name__: m for m in (self.usb, self.usb.core, self.usb.util,
                                               self.usb.backend, self.usb.backend.libusb1)}
        self.stdout, self.stderr = io.StringIO(), io.StringIO()

    def reset(self):
        self.device.s.update(configured=0, active=0, out_callbacks=0, in_callbacks=0,
                             out_count=0, in_count=0, resets=self.device.s["resets"] + 1)

    def run_cli(self, *options, default_path=False):
        argv = ["--expect-speed", "high", *options]
        if not default_path:
            argv += ["--log-file", str(self.path)]
        with patch.dict(sys.modules, self.modules), patch.object(h.time, "sleep"), \
                redirect_stdout(self.stdout), redirect_stderr(self.stderr), \
                patch.object(h, "LOG_DIR", self.root / "auto"):
            return h.main(argv)

    def test_success_writes_complete_log_not_terminal_dump(self):
        self.assertEqual(self.run_cli(), 0)
        log = self.path.read_text()
        self.assertIn('"result": "PASS"', log)
        self.assertEqual(log.count("CASE_RESULT "), 62)
        self.assertIn("verifier_sha256", log)
        self.assertIn("Finished UTC:", log)
        self.assertIn("62 cases", self.stdout.getvalue())
        self.assertIn(str(self.path), self.stdout.getvalue())
        self.assertNotIn("case_id", self.stdout.getvalue())
        self.assertNotIn("CASE_RESULT", self.stdout.getvalue())

    def test_reset_exception_retains_phase_status_codes_and_traceback(self):
        def fail_reset():
            # Previous results are flushed BEFORE the reset API is attempted.
            live_log = self.path.read_text()
            self.assertEqual(live_log.count("CASE_RESULT "), 62)
            self.assertIn("RESET_BEFORE", live_log)
            print("backend diagnostic", file=sys.stderr)
            raise USBError("reset not supported")
        self.device.reset = fail_reset
        self.assertEqual(self.run_cli("--reset-test"), 1)
        log = self.path.read_text()
        for fragment in ('"result": "FAIL"', '"phase": "reset: device.reset()"',
                         '"backend_error_code": -99', '"errno": 5',
                         '"last_successful_status"', '"active": 1',
                         '"out_count": 512', '"completed_cases"', "Traceback",
                         "backend diagnostic", "reset not supported"):
            self.assertIn(fragment, log)
        self.assertNotIn("USB HIL verified:", log)

    def test_optional_reset_success_still_has_64_cases(self):
        self.assertEqual(self.run_cli("--reset-test"), 0)
        log = self.path.read_text()
        self.assertEqual(log.count("CASE_RESULT "), 64)
        self.assertIn("RESET_AFTER", log)
        self.assertIn("64 cases", self.stdout.getvalue())

    def test_reset_configuration_no_device_retries_without_repeating_reset_or_cases(self):
        configure = self.device.set_configuration
        disconnected = USBError("new handle disappeared")
        disconnected.errno, disconnected.backend_error_code = 19, -4
        failed = False

        def set_configuration(value):
            nonlocal failed
            if self.device.s["resets"] == 2 and not failed:
                failed = True
                raise disconnected
            configure(value)

        self.device.set_configuration = set_configuration
        self.device.reset = Mock(wraps=self.reset)
        self.assertEqual(self.run_cli("--reset-test"), 0)
        log = self.path.read_text()
        self.assertEqual(log.count("CASE_RESULT "), 64)
        self.assertEqual(log.count("RECONNECT_ATTEMPT "), 2)
        self.assertIn('"stage": "configure new USB handle"', log)
        self.assertIn('"backend_error_code": -4', log)
        self.assertIn("RECONNECT_READY", log)
        self.device.reset.assert_called_once_with()
        self.assertEqual(self.usb.core.find.call_count, 3)

    def test_discovery_failure_is_logged(self):
        self.usb.core.find.return_value = []
        self.assertEqual(self.run_cli(), 1)
        log = self.path.read_text()
        self.assertIn('"phase": "find and identify USB HIL device"', log)
        self.assertIn("found 0", log)
        self.usb.util.dispose_resources.assert_not_called()

    def test_unexpected_exception_is_logged(self):
        self.device.write = Mock(side_effect=OSError("unexpected write failure"))
        self.assertEqual(self.run_cli(), 1)
        self.assertIn("unexpected write failure", self.path.read_text())
        self.assertIn("Traceback", self.path.read_text())

    def test_keyboard_interrupt_is_logged(self):
        self.usb.core.find.side_effect = KeyboardInterrupt
        self.assertEqual(self.run_cli(), 130)
        self.assertIn('"result": "INTERRUPTED"', self.path.read_text())
        self.assertIn("KeyboardInterrupt", self.path.read_text())

    def test_primary_error_not_masked_by_cleanup(self):
        self.device.reset = Mock(side_effect=USBError("primary reset failure"))
        self.usb.util.dispose_resources.side_effect = USBError("secondary cleanup failure")
        self.assertEqual(self.run_cli("--reset-test"), 1)
        log = self.path.read_text()
        self.assertIn('"error": "primary reset failure"', log)
        self.assertIn('"cleanup_error"', log)
        self.assertIn("secondary cleanup failure", log)

    def test_cleanup_failure_cannot_report_pass(self):
        self.usb.util.dispose_resources.side_effect = USBError("cleanup failure")
        self.assertEqual(self.run_cli(), 1)
        self.assertNotIn('"result": "PASS"', self.path.read_text())
        self.assertNotIn("USB HIL verified:", self.stdout.getvalue())

    def test_rediscovery_failure_logs_retry_reason_without_stale_cleanup(self):
        clock = [0.0]

        def find(**kwargs):
            if self.usb.core.find.call_count == 1:
                return [self.device]
            clock[0] = 11.0
            return []

        self.usb.core.find.side_effect = find
        with patch.object(h.time, "monotonic", side_effect=lambda: clock[0]):
            self.assertEqual(self.run_cli("--reset-test"), 1)
        log = self.path.read_text()
        self.assertIn("RECONNECT_ERROR", log)
        self.assertIn("HIL did not return after USB reset", log)
        self.usb.util.dispose_resources.assert_called_once_with(self.device)

    def test_status_only_is_logged(self):
        self.assertEqual(self.run_cli("--status-only"), 0)
        self.assertIn("CONNECTED (bulk tests not run)", self.path.read_text())
        self.assertNotIn("CASE_RESULT", self.path.read_text())

    def test_automatic_names_are_unique_and_created_before_usb_access(self):
        def find(**kwargs):
            logs = list((self.root / "auto").glob("*.log"))
            self.assertTrue(logs)
            self.assertTrue(all("started_utc" in p.read_text() for p in logs))
            return [self.device]
        self.usb.core.find.side_effect = find
        self.assertEqual(self.run_cli("--status-only", default_path=True), 0)
        self.assertEqual(self.run_cli("--status-only", default_path=True), 0)
        self.assertEqual(len(list((self.root / "auto").glob("*.log"))), 2)

    def test_existing_log_refused_before_usb_access(self):
        self.path.write_text("existing evidence")
        with self.assertRaises(SystemExit) as error:
            self.run_cli()
        self.assertEqual(error.exception.code, 2)
        self.assertEqual(self.path.read_text(), "existing evidence")
        self.usb.backend.libusb1.get_backend.assert_not_called()
        self.usb.core.find.assert_not_called()

    def test_unwritable_log_refused_before_usb_access(self):
        self.path.write_text("not a directory")
        self.path = self.path / "child.log"
        with self.assertRaises(SystemExit) as error:
            self.run_cli()
        self.assertEqual(error.exception.code, 2)
        self.usb.core.find.assert_not_called()

    def test_argument_validation_failure_is_logged_without_usb_access(self):
        self.assertEqual(self.run_cli("--timeout-ms", "0"), 1)
        self.assertIn("timeout must be positive", self.path.read_text())
        self.usb.core.find.assert_not_called()


if __name__ == "__main__":
    unittest.main()
