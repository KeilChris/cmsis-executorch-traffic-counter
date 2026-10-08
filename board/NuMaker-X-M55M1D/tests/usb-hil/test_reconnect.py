"""Offline reset reconnect tests: fake clock/handles only, no real USB import."""
from contextlib import redirect_stdout
import errno
import io
import types
import unittest
from unittest.mock import Mock, call, patch

from test_host import FakeDevice
import verify_usb as h


class USBError(Exception):
    def __init__(self, message="device disappeared", backend_error_code=-4, error_number=errno.ENODEV):
        super().__init__(message)
        self.backend_error_code = backend_error_code
        self.errno = error_number


class Clock:
    now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


def device():
    result = FakeDevice()
    result.s["resets"] = 2
    result.iProduct, result.iSerialNumber = 1, 2
    result.ctrl_transfer = Mock(wraps=result.ctrl_transfer)
    result.set_configuration = Mock(wraps=result.set_configuration)
    result.reset = Mock()
    result.write = Mock(wraps=result.write)
    result.read = Mock(wraps=result.read)
    return result


class ReconnectTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.log = io.StringIO()
        self.runner = h.Verifier(FakeDevice())
        self.before = self.runner.status()
        self.runner.device = None  # The one explicit reset already released the old handle.
        self.first, self.second = device(), device()
        self.core = types.SimpleNamespace(USBError=USBError, find=Mock(return_value=[self.first]))
        self.util = types.SimpleNamespace(
            get_string=Mock(side_effect=lambda d, i: {1: h.PRODUCT, 2: h.SERIAL}[i]),
            claim_interface=Mock(), dispose_resources=Mock())
        self.step = Mock()

    def reconnect(self, timeout_s=0.25):
        with patch.object(h.time, "monotonic", self.clock.monotonic), \
                patch.object(h.time, "sleep", self.clock.sleep), redirect_stdout(self.log):
            return h.reconnect_after_reset(self.runner, self.before, 512, self.core,
                                           self.util, None, self.step, timeout_s=timeout_s)

    def assert_fresh_retry(self):
        self.assertEqual(self.reconnect()["resets"], 2)
        self.assertIs(self.runner.device, self.second)
        self.util.dispose_resources.assert_called_once_with(self.first)
        self.assertEqual(self.core.find.call_count, 2)
        self.assertEqual(self.log.getvalue().count("RECONNECT_ERROR "), 1)
        for handle in (self.first, self.second):
            handle.reset.assert_not_called()
            handle.write.assert_not_called()
            handle.read.assert_not_called()

    def test_configure_disappearance_discards_handle_and_rediscovers(self):
        self.first.set_configuration.side_effect = USBError()
        self.core.find.side_effect = [[self.first], [self.second]]
        self.assert_fresh_retry()
        self.util.claim_interface.assert_called_once_with(self.second, 0)

    def test_claim_disappearance_discards_handle_and_rediscovers(self):
        self.util.claim_interface.side_effect = [USBError(), None]
        self.core.find.side_effect = [[self.first], [self.second]]
        self.assert_fresh_retry()
        self.assertEqual(self.util.claim_interface.call_args_list,
                         [call(self.first, 0), call(self.second, 0)])

    def test_status_disappearance_repeats_whole_handshake(self):
        fingerprint = self.first.ctrl_transfer(0xC0, h.STATUS, 0, 0, 64)
        self.first.ctrl_transfer.side_effect = [fingerprint, USBError()]
        self.core.find.side_effect = [[self.first], [self.second]]
        self.assert_fresh_retry()
        self.second.set_configuration.assert_called_once_with(1)

    def test_descriptor_disappearance_releases_discovery_handle(self):
        self.util.get_string.side_effect = [USBError(), h.PRODUCT, h.SERIAL]
        self.core.find.side_effect = [[self.first], [self.second]]
        self.assert_fresh_retry()
        self.first.set_configuration.assert_not_called()

    def test_fingerprint_disappearance_releases_discovery_handle(self):
        self.first.ctrl_transfer.side_effect = USBError()
        self.core.find.side_effect = [[self.first], [self.second]]
        self.assert_fresh_retry()
        self.first.set_configuration.assert_not_called()

    def test_enumeration_disappearance_is_retried(self):
        self.core.find.side_effect = [USBError(), [self.first]]
        self.assertEqual(self.reconnect()["resets"], 2)
        self.assertEqual(self.core.find.call_count, 2)
        self.util.dispose_resources.assert_not_called()

    def test_empty_enumeration_then_success(self):
        self.core.find.side_effect = [[], [self.first]]
        self.assertEqual(self.reconnect()["resets"], 2)
        self.assertEqual(self.core.find.call_count, 2)

    def test_persistent_absence_expires_without_reset_or_bulk_tests(self):
        self.core.find.return_value = []
        with self.assertRaisesRegex(RuntimeError, "within 0.25s; last reconnect error:.*found 0"):
            self.reconnect()
        self.assertEqual(self.core.find.call_count, 3)
        self.assertIsNone(self.runner.device)
        self.util.dispose_resources.assert_not_called()
        self.assertEqual(self.clock.now, 0.25)

    def test_persistent_no_device_expires_and_releases_each_failed_handle(self):
        handles = []

        def find(**kwargs):
            new = device()
            new.set_configuration.side_effect = USBError()
            handles.append(new)
            return [new]

        self.core.find.side_effect = find
        with self.assertRaisesRegex(RuntimeError, "within 0.25s; last reconnect error: device disappeared"):
            self.reconnect()
        self.assertEqual(len(handles), 3)
        self.assertEqual(self.util.dispose_resources.call_args_list, [call(d) for d in handles])
        self.assertIsNone(self.runner.device)

    def test_retries_share_one_deadline(self):
        def configure(value):
            self.clock.now += 0.08
            raise USBError()

        self.first.set_configuration.side_effect = configure
        with self.assertRaisesRegex(RuntimeError, "within 0.25s"):
            self.reconnect()
        self.assertEqual(self.core.find.call_count, 2)
        self.assertLess(self.clock.now, 0.3)

    def test_slow_success_is_not_accepted_after_deadline(self):
        def status(*args, **kwargs):
            data = FakeDevice.ctrl_transfer(self.first, *args, **kwargs)
            if self.util.claim_interface.called:
                self.clock.now = 1.0
            return data

        self.first.ctrl_transfer.side_effect = status
        with self.assertRaisesRegex(RuntimeError, "within 0.25s"):
            self.reconnect()
        self.assertNotIn("RECONNECT_READY", self.log.getvalue())
        self.assertEqual(self.core.find.call_count, 1)

    def test_status_timeout_capped_to_remaining_budget(self):
        def configure(value):
            self.clock.now += 0.2

        self.first.set_configuration.side_effect = configure
        self.reconnect()
        self.assertLessEqual(self.first.ctrl_transfer.call_args.kwargs["timeout"], 50)
        self.assertGreater(self.first.ctrl_transfer.call_args.kwargs["timeout"], 0)

    def test_other_usb_errors_are_fatal_at_every_stage(self):
        for code, error_number in [(-7, errno.ETIMEDOUT), (-9, errno.EPIPE),
                                   (-3, errno.EACCES), (-6, errno.EBUSY), (-1, errno.EIO)]:
            for stage in ("discovery", "configure", "claim", "status"):
                with self.subTest(code=code, stage=stage):
                    self.setUp()
                    error = USBError("not transient", code, error_number)
                    if stage == "discovery":
                        self.core.find.side_effect = error
                    elif stage == "configure":
                        self.first.set_configuration.side_effect = error
                    elif stage == "claim":
                        self.util.claim_interface.side_effect = error
                    else:
                        initial = self.first.ctrl_transfer(0xC0, h.STATUS, 0, 0, 64)
                        self.first.ctrl_transfer.side_effect = [initial, error]
                    with self.assertRaisesRegex(USBError, "not transient"):
                        self.reconnect()
                    self.assertEqual(self.core.find.call_count, 1)
                    self.assertIn('"retryable": false', self.log.getvalue())

    def test_wrong_firmware_and_target_errors_fail_without_configuring(self):
        for field, value, message in [("magic", 0, "wrong firmware"),
                                      ("version", 99, "wrong firmware"),
                                      ("errors", 32, "target failure")]:
            with self.subTest(field=field):
                self.setUp()
                self.first.s[field] = value
                with self.assertRaisesRegex(RuntimeError, message):
                    self.reconnect()
                self.assertEqual(self.core.find.call_count, 1)
                self.first.set_configuration.assert_not_called()
                self.util.dispose_resources.assert_called_once_with(self.first)
                self.assertIsNone(self.runner.device)

    def test_short_fingerprint_is_not_retried(self):
        self.first.ctrl_transfer.return_value = bytes(63)
        with self.assertRaisesRegex(RuntimeError, "short status"):
            self.reconnect()
        self.assertEqual(self.core.find.call_count, 1)
        self.util.dispose_resources.assert_called_once_with(self.first)

    def test_ambiguous_identity_is_fatal_and_releases_both_handles(self):
        self.core.find.return_value = [self.first, self.second]
        with self.assertRaisesRegex(RuntimeError, "found 2"):
            self.reconnect()
        self.assertEqual(self.core.find.call_count, 1)
        self.assertEqual(self.util.dispose_resources.call_args_list,
                         [call(self.first), call(self.second)])
        self.first.set_configuration.assert_not_called()
        self.second.set_configuration.assert_not_called()

    def test_reset_and_status_assertions_are_not_retried(self):
        for field, value, message in [("resets", 1, "no reset event"),
                                      ("resets", 0, "no reset event"),
                                      ("active", 1, "transfer still active"),
                                      ("out_callbacks", 1, "completed unexpectedly"),
                                      ("in_callbacks", 1, "completed unexpectedly"),
                                      ("mps", 64, "changed speed"),
                                      ("configured", 0, "bad configuration"),
                                      ("errors", 4, "target failure")]:
            with self.subTest(field=field, value=value):
                self.setUp()
                self.first.set_configuration.side_effect = lambda v: self.first.s.update({field: value})
                with self.assertRaisesRegex(RuntimeError, message):
                    self.reconnect()
                self.assertEqual(self.core.find.call_count, 1)
                # The CLI retains this handle for its finally cleanup on fatal failure.
                self.assertIs(self.runner.device, self.first)

    def test_failed_discard_is_fatal_not_another_reconnect_attempt(self):
        self.first.set_configuration.side_effect = USBError()
        self.util.dispose_resources.side_effect = RuntimeError("cannot release handle")
        with self.assertRaisesRegex(RuntimeError, "cannot release handle"):
            self.reconnect()
        self.assertEqual(self.core.find.call_count, 1)
        self.assertIs(self.runner.device, self.first)
        self.assertIn("device disappeared", self.log.getvalue())

    def test_discovery_cleanup_failure_cannot_become_a_retry(self):
        self.first.ctrl_transfer.side_effect = USBError()
        self.util.dispose_resources.side_effect = RuntimeError("cannot release discovery handle")
        with self.assertRaisesRegex(RuntimeError, "discovery cleanup failed after device disappeared"):
            self.reconnect()
        self.assertEqual(self.core.find.call_count, 1)
        self.assertIn("DISCOVERY_CLEANUP_ERROR", self.log.getvalue())
        self.first.set_configuration.assert_not_called()

    def test_keyboard_interrupt_is_not_retried(self):
        self.first.set_configuration.side_effect = KeyboardInterrupt
        with self.assertRaises(KeyboardInterrupt):
            self.reconnect()
        self.assertEqual(self.core.find.call_count, 1)

    def test_no_device_code_classification_is_narrow(self):
        self.assertTrue(h.is_no_device(USBError()))
        self.assertTrue(h.is_no_device(USBError(backend_error_code=None)))
        self.assertFalse(h.is_no_device(USBError(backend_error_code=-7)))
        self.assertFalse(h.is_no_device(USBError(backend_error_code=None, error_number=errno.EIO)))


if __name__ == "__main__":
    unittest.main()
