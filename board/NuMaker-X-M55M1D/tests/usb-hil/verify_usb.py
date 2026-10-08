#!/usr/bin/env python3
"""User-run USB HIL verifier. This is NOT an SDS client. No USB access on import."""
from __future__ import annotations

import argparse
from contextlib import redirect_stderr, redirect_stdout
from datetime import datetime, timezone
import errno
import hashlib
import json
import os
from pathlib import Path
import platform
import struct
import sys
import time
import traceback

MAGIC, VERSION, CAPACITY = 0x5548494C, 1, 8192
STATUS, ARM_OUT, ARM_IN, ABORT, LOOPBACK = range(0x30, 0x35)
REARM = 0x8000
OUT, IN = 0x01, 0x81
VID, PID = 0xC251, 0x8007  # Existing local demo identity; not a new PID allocation.
PRODUCT, SERIAL = "NuMaker USB HIL", "NU-USB-HIL-01"
FIELDS = ("magic version configured mps resets case_id mode active requested "
          "out_count in_count out_callbacks in_callbacks aborts errors last_driver_error").split()
LOG_DIR = Path(__file__).resolve().parents[4] / "logs" / "usb-hil"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def pattern(length, seed):
    return bytes(((i * 73 + (i >> 8) * 19 + seed * 29) ^ ((i >> 3) + seed)) & 255
                 for i in range(length))


def decode_status(data):
    require(len(data) == 64, f"short status: {len(data)}/64 bytes")
    result = dict(zip(FIELDS, struct.unpack("<15Ii", bytes(data))))
    require(result["magic"] == MAGIC and result["version"] == VERSION,
            "wrong firmware/protocol; refusing to test this device")
    require(result["errors"] == 0,
            f"target failure bits 0x{result['errors']:x}: {result}")
    return result


def boundary_lengths():
    return [0, 1, 63, 64, 65, 127, 128, 129, 511, 512, 513, 1023, 1024, 1025, 8191, 8192]


class Verifier:
    def __init__(self, device, timeout_ms=3000):
        self.device, self.timeout_ms = device, timeout_ms
        self.cases = []
        self.active_case = "initial status"
        self.last_successful_status = None
        self.initial_resets = self.status()["resets"]

    def status(self, timeout_ms=None):
        timeout = self.timeout_ms if timeout_ms is None else timeout_ms
        status = decode_status(self.device.ctrl_transfer(0xC0, STATUS, 0, 0, 64,
                                                        timeout=timeout))
        self.last_successful_status = status
        return status

    def command(self, command, size=0, seed=0):
        require(self.device.ctrl_transfer(0x40, command, size, seed, b"",
                                         timeout=self.timeout_ms) == 0, "unexpected control payload")

    def arm(self, command, size, seed):
        before = self.status()
        require(before["configured"] == 1 and before["active"] == 0, "device not idle/configured")
        self.command(command, size, seed)
        armed = self.status()
        require(armed["case_id"] == before["case_id"] + 1, "case was not armed")
        require(armed["mode"] == command and armed["requested"] == size, "wrong armed case")
        require(armed["resets"] == self.initial_resets, "unexpected bus reset")
        return armed

    def write(self, data):
        actual = self.device.write(OUT, data, timeout=self.timeout_ms)
        require(actual == len(data), f"short host write {actual}/{len(data)}")

    def read(self, length):
        # A nonzero host read is needed to exercise a target-generated ZLP.
        return bytes(self.device.read(IN, max(1, length), timeout=self.timeout_ms))

    def wait(self, out_callbacks, in_callbacks, out_count, in_count):
        deadline = time.monotonic() + self.timeout_ms / 1000
        while True:
            s = self.status()
            require(s["resets"] == self.initial_resets, "unexpected bus reset during transfer")
            require(s["out_callbacks"] <= out_callbacks and s["in_callbacks"] <= in_callbacks,
                    f"extra completion callback: {s}")
            if s["active"] == 0:
                require((s["out_callbacks"], s["in_callbacks"], s["out_count"], s["in_count"]) ==
                        (out_callbacks, in_callbacks, out_count, in_count), f"bad completion: {s}")
                return s
            require(time.monotonic() < deadline, f"completion timeout: {s}")
            time.sleep(0.01)

    def stable(self, previous):
        # A finite ownership check, not a proof against arbitrarily late writes.
        time.sleep(0.05)
        now = self.status()
        require(now == previous, f"state changed after completion/abort: {previous} -> {now}")

    def passed(self, name, status):
        self.cases.append({"case": name, "status": status})
        print(f"PASS {name}", flush=True)
        # Preserve each completed result even if a later reset/exception interrupts
        # the run before its final JSON report can be written.
        print("CASE_RESULT " + json.dumps(self.cases[-1]), flush=True)

    def one(self, mode, length, seed=17):
        self.active_case = f"{ {ARM_OUT: 'OUT', ARM_IN: 'IN', LOOPBACK: 'loopback'}[mode]} {length}"
        self.arm(mode, length, seed)
        expected = pattern(length, seed)
        if mode != ARM_IN:
            self.write(expected)
        if mode != ARM_OUT:
            require(self.read(length) == expected, f"IN/loopback data mismatch at {length} bytes")
        s = self.wait(int(mode != ARM_IN), int(mode != ARM_OUT),
                      length if mode != ARM_IN else 0, length if mode != ARM_OUT else 0)
        self.stable(s)
        self.passed(f"{ {ARM_OUT: 'OUT', ARM_IN: 'IN', LOOPBACK: 'loopback'}[mode]} {length}", s)

    def short_out(self, mps, length):
        self.active_case = f"short/ZLP OUT {length} into {CAPACITY}"
        self.arm(ARM_OUT, CAPACITY, 23)
        self.write(pattern(length, 23))
        if length and length % mps == 0:
            # Exact packet does not terminate the larger requested transfer.
            s = self.status()
            require(s["active"] == 1 and s["out_callbacks"] == 0,
                    "OUT completed before short/ZLP terminator")
            self.write(b"")
        s = self.wait(1, 0, length, 0)
        self.stable(s)
        self.passed(f"short/ZLP OUT {length} into {CAPACITY}", s)

    def rearm(self, mode, mps):
        self.active_case = f"callback rearm {mode:#x}"
        length = mps + 1
        self.arm(mode, length, REARM | 41)
        for seed in (41, 42):
            if mode == ARM_OUT:
                self.write(pattern(length, seed))
            else:
                require(self.read(length) == pattern(length, seed), "callback-rearmed IN data mismatch")
        s = self.wait(2 if mode == ARM_OUT else 0, 2 if mode == ARM_IN else 0,
                      length if mode == ARM_OUT else 0, length if mode == ARM_IN else 0)
        self.stable(s)
        self.passed(f"callback rearm {mode:#x}", s)

    def abort(self, mode, mps, partial=False):
        self.active_case = f"abort {'partial ' if partial else ''}{mode:#x}"
        armed = self.arm(mode, CAPACITY, 67)
        if partial:
            # A full packet is shorter than the target's request, not a short packet.
            self.write(pattern(mps, 67))
            deadline = time.monotonic() + self.timeout_ms / 1000
            while self.status()["out_count"] != mps:
                require(time.monotonic() < deadline, "partial OUT did not reach driver")
                time.sleep(0.01)
        self.command(ABORT)
        s = self.status()
        require(s["active"] == 0 and s["aborts"] == armed["aborts"] + 1,
                f"abort did not retire the transfer: {s}")
        require(s["out_callbacks"] == 0 and s["in_callbacks"] == 0,
                "aborted incomplete transfer produced completion")
        self.stable(s)
        self.passed(f"abort {'partial ' if partial else ''}{mode:#x}", s)
        self.one(mode, mps + 1, 71)  # restart without endpoint reset/reconfigure

    def run(self):
        initial = self.status()
        require(initial["configured"] == 1 and initial["mps"] in (64, 512), "bad configuration")
        mps = initial["mps"]
        print(f"Negotiated bulk MPS {mps}: {'high' if mps == 512 else 'full'} speed", flush=True)
        for length in boundary_lengths():
            for mode in (ARM_OUT, ARM_IN, LOOPBACK):
                self.one(mode, length)
        for length in (0, mps - 1, mps, mps + 1):
            self.short_out(mps, length)
        for mode in (ARM_OUT, ARM_IN):
            self.rearm(mode, mps)
            self.abort(mode, mps)
        self.abort(ARM_OUT, mps, partial=True)
        # Exercise unconfiguration through the standard control request.
        self.active_case = "unconfigure/reconfigure"
        self.device.ctrl_transfer(0, 9, 0, 0, b"", timeout=self.timeout_ms)
        s = self.status()
        require(s["configured"] == 0 and s["active"] == 0, "unconfigure did not retire endpoints")
        self.device.set_configuration(1)
        self.one(LOOPBACK, 8192, 97)
        self.passed("unconfigure/reconfigure", self.status())
        return mps


class DeviceNotFound(RuntimeError):
    """No matching identity yet; unlike ambiguous identity or invalid status."""


def find_device(usb_core, usb_util, serial=SERIAL, backend=None, timeout_ms=3000):
    candidates = []
    opened = []
    try:
        for device in usb_core.find(find_all=True, idVendor=VID, idProduct=PID, backend=backend):
            opened.append(device)
            if (usb_util.get_string(device, device.iProduct) == PRODUCT and
                    usb_util.get_string(device, device.iSerialNumber) == serial):
                candidates.append(device)
            else:
                usb_util.dispose_resources(device)
                opened.pop()
        message = (f"expected exactly one '{PRODUCT}' device, found {len(candidates)}; "
                   "load USB HIL and continue past main")
        if not candidates:
            raise DeviceNotFound(message)
        require(len(candidates) == 1, message)
        # Read-only fingerprint check BEFORE claiming/configuring/resetting anything.
        decode_status(candidates[0].ctrl_transfer(0xC0, STATUS, 0, 0, 64, timeout=timeout_ms))
        return candidates[0]
    except (Exception, KeyboardInterrupt) as original:
        # Discovery can open a handle before returning it to the caller. Do not
        # leak that handle when a descriptor/status read fails during re-enumeration.
        cleanup_errors = []
        for device in opened:
            try:
                usb_util.dispose_resources(device)
            except (Exception, KeyboardInterrupt) as exc:
                cleanup_errors.append(exception_details(exc))
                print("DISCOVERY_CLEANUP_ERROR " + json.dumps(exception_details(exc)), flush=True)
        if cleanup_errors and not isinstance(original, KeyboardInterrupt):
            # A failed cleanup must not turn into a reconnect retry merely
            # because the original descriptor/status read was NO_DEVICE.
            raise RuntimeError(f"discovery cleanup failed after {original}: {cleanup_errors}") from original
        raise


def exception_details(exc):
    return {"type": type(exc).__name__, "message": str(exc), "repr": repr(exc),
            "errno": getattr(exc, "errno", None),
            "backend_error_code": getattr(exc, "backend_error_code", None)}


def is_no_device(exc):
    # Prefer libusb's code when present. Do not reinterpret timeout, stall,
    # access-denied, busy or generic I/O errors as a transient disconnect.
    backend_code = getattr(exc, "backend_error_code", None)
    return backend_code == -4 if backend_code is not None else getattr(exc, "errno", None) == errno.ENODEV


def reconnect_after_reset(runner, before, mps, usb_core, usb_util, backend, step,
                          timeout_s=10.0):
    """Retry only post-reset absence/NO_DEVICE, always using newly discovered handles.

    One deadline covers discovery, configuration, claiming and status. libusb's
    synchronous configuration/claim calls cannot be interrupted by this deadline;
    check it between calls and reject even a successful attempt if it ran late.
    Never repeat device.reset(), a test case, or a failed protocol assertion.
    """
    started = time.monotonic()
    deadline = started + timeout_s
    attempt = 0
    last_error = None

    def remaining_ms():
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError(f"HIL did not return after USB reset within {timeout_s:g}s; "
                               f"last reconnect error: {last_error}") from last_error
        return min(runner.timeout_ms, max(1, int(remaining * 1000)))

    while True:
        timeout_ms = remaining_ms()
        attempt += 1
        stage = "rediscover and identify device"
        print("RECONNECT_ATTEMPT " + json.dumps({"attempt": attempt,
              "elapsed_s": time.monotonic() - started}), flush=True)
        try:
            step(f"reset: reconnect attempt {attempt}: {stage}")
            runner.device = find_device(usb_core, usb_util, backend=backend, timeout_ms=timeout_ms)
            stage = "configure new USB handle"
            step(f"reset: reconnect attempt {attempt}: {stage}")
            remaining_ms()
            runner.device.set_configuration(1)
            stage = "claim new USB handle"
            step(f"reset: reconnect attempt {attempt}: {stage}")
            remaining_ms()
            usb_util.claim_interface(runner.device, 0)
            stage = "verify reset event and retired transfer"
            step(f"reset: reconnect attempt {attempt}: {stage}")
            after = runner.status(timeout_ms=remaining_ms())
            print("RESET_AFTER " + json.dumps(after), flush=True)
            require(after["configured"] == 1 and after["mps"] == mps,
                    "bad configuration or changed speed after USB reset")
            require(after["resets"] > before["resets"] and after["active"] == 0,
                    "no reset event or transfer still active")
            require(after["out_callbacks"] == 0 and after["in_callbacks"] == 0,
                    "reset-abandoned transfer completed unexpectedly")
            remaining_ms()
            print("RECONNECT_READY " + json.dumps({"attempt": attempt,
                  "elapsed_s": time.monotonic() - started}), flush=True)
            return after
        except (DeviceNotFound, usb_core.USBError) as exc:
            retryable = isinstance(exc, DeviceNotFound) or is_no_device(exc)
            print("RECONNECT_ERROR " + json.dumps({"attempt": attempt, "stage": stage,
                  "elapsed_s": time.monotonic() - started, "retryable": retryable,
                  "exception": exception_details(exc)}), flush=True)
            if not retryable:
                raise
            last_error = exc
            if runner.device is not None:
                # Release any partial claim before rediscovery; never reuse a
                # failed handle. Cleanup failure is fatal, not another retry.
                usb_util.dispose_resources(runner.device)
                runner.device = None
            remaining_ms()
            time.sleep(min(0.1, max(0, deadline - time.monotonic())))


def run_logged(args):
    """Log one suite; only its explicit reset reconnection has bounded retries."""
    device = None
    runner = None
    code = 0
    phase = "validate arguments and import USB backend"

    def step(name):
        nonlocal phase
        phase = name
        print(f"PHASE {name}", flush=True)

    try:
        require(args.timeout_ms > 0, "timeout must be positive")
        require(not (args.status_only and args.reset_test), "--status-only and --reset-test are mutually exclusive")
        import usb.core
        import usb.util
        import usb.backend.libusb1
        step("load libusb backend")
        backend = usb.backend.libusb1.get_backend(find_library=(lambda _: args.libusb) if args.libusb else None)
        require(backend is not None, "libusb backend unavailable; pass --libusb /absolute/path/to/libusb-1.0 library")
        step("find and identify USB HIL device")
        device = find_device(usb.core, usb.util, backend=backend)
        step("configure and claim USB interface")
        try:
            configuration = device.get_active_configuration().bConfigurationValue
        except usb.core.USBError as exc:
            print("CONFIGURATION_QUERY_ERROR " + json.dumps(exception_details(exc)), flush=True)
            configuration = 0
        if configuration != 1:
            device.set_configuration(1)
        usb.util.claim_interface(device, 0)
        runner = Verifier(device, args.timeout_ms)
        if args.expect_speed:
            require(runner.status()["mps"] == {"high": 512, "full": 64}[args.expect_speed],
                    "negotiated speed does not match --expect-speed")
        if args.status_only:
            result = {"result": "CONNECTED (bulk tests not run)", "status": runner.status()}
        else:
            result = run_suite(args, device, runner, backend, step)
            # Reset may replace the device handle. Cleanup must use the new one.
            device = runner.device
    except (Exception, KeyboardInterrupt) as exc:
        code = 130 if isinstance(exc, KeyboardInterrupt) else 1
        result = {"result": "INTERRUPTED" if code == 130 else "FAIL",
                  "error": str(exc), "exception": exception_details(exc), "phase": phase,
                  "active_case": runner.active_case if runner else None,
                  "last_successful_status": runner.last_successful_status if runner else None,
                  "completed_cases": runner.cases if runner else []}
        # No additional USB status request: report only data already observed.
        print("ERROR " + json.dumps({k: v for k, v in result.items() if k != "completed_cases"}), flush=True)
        traceback.print_exc()
    finally:
        cleanup_device = runner.device if runner else device
        if cleanup_device is not None:
            try:
                print("PHASE release host USB resources", flush=True)
                # runner.device tracks reset/re-discovery even when run_suite fails.
                usb.util.dispose_resources(cleanup_device)
            except (Exception, KeyboardInterrupt) as exc:
                print("CLEANUP_ERROR " + json.dumps(exception_details(exc)), flush=True)
                traceback.print_exc()
                if code == 0:
                    code = 130 if isinstance(exc, KeyboardInterrupt) else 1
                    result = {"result": "INTERRUPTED" if code == 130 else "FAIL",
                              "error": str(exc), "exception": exception_details(exc),
                              "phase": "release host USB resources",
                              "active_case": runner.active_case if runner else None,
                              "last_successful_status": runner.last_successful_status if runner else None,
                              "completed_cases": runner.cases if runner else []}
                else:
                    result["cleanup_error"] = exception_details(exc)
    print(json.dumps(result, indent=2), flush=True)
    if code:
        summary = (f"USB HIL {result['result'].lower()} during {result['phase']}: "
                   f"{result['error'] or result['exception']['type']}. "
                   "Retain the log; inspect before retry/reset.")
    elif args.status_only:
        summary = "CONNECTED (bulk tests not run)"
    else:
        summary = (f"USB HIL verified: {len(runner.cases)} cases; "
                   f"hardware speed scope: {result['mps']}-byte MPS")
    print(summary, flush=True)
    return code, summary


def run_suite(args, device, runner, backend, step):
    import usb.core
    import usb.util
    step("62-case transfer suite")
    mps = runner.run()
    if args.reset_test:
        runner.active_case = "bus-reset with partial OUT"
        step("reset: arm incomplete OUT")
        runner.arm(ARM_OUT, CAPACITY, 101)
        step("reset: send and observe first OUT packet")
        runner.write(pattern(mps, 101))
        deadline = time.monotonic() + args.timeout_ms / 1000
        before = runner.status()
        while before["out_count"] != mps:
            require(time.monotonic() < deadline, "reset case did not receive its first packet")
            time.sleep(0.01)
            before = runner.status()
        require(before["active"] == 1 and before["out_callbacks"] == 0,
                "reset case was not pending")
        print("RESET_BEFORE " + json.dumps(before), flush=True)
        step("reset: device.reset()")
        device.reset()
        step("reset: release old USB handle")
        usb.util.dispose_resources(device)
        device = None
        runner.device = None
        after = reconnect_after_reset(runner, before, mps, usb.core, usb.util, backend, step)
        runner.initial_resets = after["resets"]
        step("reset: 8192-byte restart loopback")
        runner.one(LOOPBACK, CAPACITY, 107)
        runner.passed("bus-reset/reconfigure/restart", runner.status())
    return {"result": "PASS", "mps": mps, "cases": runner.cases, "reset_test": args.reset_test}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reset-test", action="store_true",
                        help="also issue a USB bus reset with an incomplete OUT transfer")
    parser.add_argument("--status-only", action="store_true", help="check identity/configuration only; no bulk tests")
    parser.add_argument("--libusb", help="explicit libusb-1.0 library path if automatic discovery fails")
    parser.add_argument("--expect-speed", choices=("high", "full"))
    parser.add_argument("--timeout-ms", type=int, default=3000)
    parser.add_argument("--log-file", type=Path,
                        help="new log path (never overwritten); default: logs/usb-hil/<UTC timestamp>-<pid>.log")
    argv = list(sys.argv[1:] if argv is None else argv)
    args = parser.parse_args(argv)
    started = datetime.now(timezone.utc)
    path = (args.log_file or LOG_DIR / f"{started:%Y%m%dT%H%M%S.%fZ}-{os.getpid()}.log").resolve()
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        output = path.open("x", encoding="utf-8", buffering=1)
    except OSError as exc:
        parser.exit(2, f"Cannot create USB HIL log {path}: {exc}. No USB access attempted.\n")
    console = sys.stdout
    print(f"USB HIL log: {path}", file=console, flush=True)
    with output, redirect_stdout(output), redirect_stderr(output):
        print(json.dumps({"started_utc": started.isoformat(), "argv": argv,
                          "python": sys.version, "platform": platform.platform(),
                          "verifier_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}), flush=True)
        code, summary = run_logged(args)
        print(f"Finished UTC: {datetime.now(timezone.utc).isoformat()}; exit code: {code}", flush=True)
    print(summary, file=console, flush=True)
    print(f"Full results: {path}", file=console, flush=True)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
