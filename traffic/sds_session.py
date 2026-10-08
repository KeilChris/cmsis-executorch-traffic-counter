#!/usr/bin/env python3
# Copyright 2026 Arm Limited and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Record from, or play back to, the traffic counter over SDS in one command.

    python traffic/sds_session.py record 20        # 20 s of CameraIn + Detections
    python traffic/sds_session.py play             # CameraIn.<n>.sds back into the detector

It starts SDSIO-Server (ARM::SDS utilities) on the board's User USB
(--transport usb, the default: the firmware enumerates as "SDSIO-Client")
and drives its keyboard interface through a pseudo terminal: R starts a
recording, P a playback, S stops, X ends the server. The firmware must be
running (it opens the streams when the server says so, rec_play.c). Files go
to recordings/traffic/ (CameraIn.<n>.sds, Detections.<n>.sds, and
Detections.<n>.p.sds for a playback), next to their *.sds.yml metadata.

With --transport rtt it starts tools/sdsio_rtt_bridge.py (the J-Link's RTT
channel 1 on a TCP socket) and SDSIO-Server in connect mode on it, for a
firmware built with the SDS:IO:RTT layer. The J-Link connect halts the core;
the bridge resumes it once SDSIO-Server is connected. Stop any debug session
first if the J-Link refuses a second connection. RTT moves about one frame
per 5 s, USB the whole 40-frame clip in 8 s.
"""

from __future__ import annotations

import argparse
import errno
import math
import os
import pty
import re
import select
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SDS_UTILITIES = Path.home() / ".cache/arm/packs/ARM/SDS/3.1.0/utilities"


def client_flags(log: str) -> int | None:
    """Latest target flags, invalidated by a connection change (USB or socket)."""
    flags = None
    for line in log.splitlines():
        match = re.search(r"sdsFlags = 0x([0-9A-Fa-f]+)\.", line)
        if match:
            flags = int(match[1], 16)
        elif "SDSIO-Client" in line and ("connected." in line or "waiting" in line):
            flags = None
    return flags


def streams_reported(log: str, camera_action: str, detection_action: str) -> bool:
    return all(re.search(rf"{action}:\s+{stream}\s+\([^\r\n]+\)", log)
               for action, stream in ((camera_action, "CameraIn"),
                                      (detection_action, "Detections")))


def check_server_errors(log: str) -> None:
    # The server closes files during fatal-error cleanup as well as after a
    # normal target close. Never mistake that cleanup for session completion.
    if "=== FATAL ERROR ===" in log:
        raise RuntimeError("SDSIO-Server reported a fatal protocol/transport error; "
                           "the recording/playback is invalid. Restart the SDSIO-Client "
                           "before retrying (debugger-load-only boards need a debugger reload)")


def run_session(server, pump, command, output: list[str], *, mode: str,
                seconds: float, timeout: float, connect_timeout: float,
                stream_timeout: float) -> tuple[int, str, float]:
    """Drive a session using server acknowledgements, not fixed startup delays.

    Callbacks keep this orchestration testable without opening USB or a PTY.
    The caller owns server shutdown and file validation.
    """
    def log() -> str:
        return "".join(output)

    def wait_until(predicate, limit: float, failure: str) -> None:
        deadline = time.monotonic() + limit
        while True:
            check_server_errors(log())
            if predicate():
                return
            if server.poll() is not None:
                raise RuntimeError(f"SDSIO-Server exited with status {server.returncode}: {failure}")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError(failure)
            pump(min(0.2, remaining))

    wait_until(lambda: bool((client_flags(log()) or 0) & 0x10000000), connect_timeout,
               f"no live SDSIO-Client within {connect_timeout:g} seconds; "
               "no recording/playback command was sent. Check the target USB cable "
               "(J13 on NuMaker), continue execution past main/breakpoints, "
               "and close any other SDSIO-Server")
    if (client_flags(log()) or 0) & 0x80000000:
        raise RuntimeError("target already has START set; stop the existing SDS session before retrying")

    output_start = len(log())
    started_ns = time.time_ns()
    started = time.monotonic()
    command(b"R" if mode == "record" else b"P")

    def session_log() -> str:
        return log()[output_start:]

    if mode == "record":
        wait_until(lambda: streams_reported(session_log(), "Record", "Record"), stream_timeout,
                   f"recording did not open both CameraIn and Detections within {stream_timeout:g} seconds")
        # USB discovery and stream startup must not consume the requested clip.
        pump(seconds)
        check_server_errors(log())
        command(b"S")
        wait_until(lambda: streams_reported(session_log(), "Closed", "Closed"), stream_timeout,
                   f"recording did not close both streams within {stream_timeout:g} seconds after Stop")
    else:
        def playback_finished() -> bool:
            current = session_log()
            if streams_reported(current, "Closed", "Closed"):
                return True
            # An idle flags report may already be in flight when P is sent.
            # Only treat clearing START as a stop after observing its assertion.
            flags = [int(value, 16) for value in re.findall(r"sdsFlags = 0x([0-9A-Fa-f]+)\.", current)]
            if any(value & 0x80000000 for value in flags) and not (flags[-1] & 0x80000000):
                raise RuntimeError("target stopped playback before both streams closed")
            return False

        wait_until(playback_finished, timeout, f"playback did not finish within {timeout:g} seconds")
    return started_ns, session_log(), time.monotonic() - started


def positive_seconds(value: str) -> float:
    seconds = float(value)
    if not math.isfinite(seconds) or seconds <= 0:
        raise argparse.ArgumentTypeError("must be a finite number greater than zero")
    return seconds


def sds_timeslots(path: Path, payload_size: int) -> list[int]:
    """Read record headers without loading image payloads; reject truncated files."""
    slots = []
    file_size = path.stat().st_size
    with path.open("rb") as stream:
        while stream.tell() < file_size:
            offset = stream.tell()
            header = stream.read(8)
            if len(header) != 8:
                raise RuntimeError(f"{path.name}: truncated record header at byte {offset}")
            timeslot, size = struct.unpack("<II", header)
            if size != payload_size:
                raise RuntimeError(f"{path.name}: record at byte {offset} has {size} bytes, expected {payload_size}")
            if stream.tell() + size > file_size:
                raise RuntimeError(f"{path.name}: truncated record payload at byte {offset}")
            stream.seek(size, os.SEEK_CUR)
            slots.append(timeslot)
    return slots


def validate_playback(workdir: Path, log: str, started_ns: int) -> int:
    """Require a fresh output record for every played model-input record, in order.

    Stream close messages also occur after a target timeout, so they alone are
    not evidence of completed playback. SDS timeslots are source timestamps,
    not a measurement of elapsed playback time.
    """
    inputs = re.findall(r"Playback:\s+CameraIn\s+\(([^\r\n]+)\)", log)
    outputs = re.findall(r"Record:\s+Detections\s+\(([^\r\n]+)\)", log)
    return validate_stream_pairs(workdir, inputs, outputs, started_ns, "playback")


def validate_recording(workdir: Path, log: str, started_ns: int) -> int:
    """Reject an orphan input/result at Stop, even when both streams closed."""
    inputs = re.findall(r"Record:\s+CameraIn\s+\(([^\r\n]+)\)", log)
    outputs = re.findall(r"Record:\s+Detections\s+\(([^\r\n]+)\)", log)
    return validate_stream_pairs(workdir, inputs, outputs, started_ns, "recording")


def validate_stream_pairs(workdir: Path, inputs: list[str], outputs: list[str],
                          started_ns: int, operation: str) -> int:
    if not inputs or not outputs:
        raise RuntimeError(f"{operation} did not open both CameraIn and Detections")
    expected, actual = [], []
    for filename in inputs:
        path = workdir / filename
        if operation == "recording" and path.stat().st_mtime_ns < started_ns:
            raise RuntimeError(f"{path.name}: input was not updated by this recording")
        expected.extend(sds_timeslots(path, 416 * 416 * 3))
    for filename in outputs:
        path = workdir / filename
        if path.stat().st_mtime_ns < started_ns:
            raise RuntimeError(f"{path.name}: output was not updated by this {operation}")
        actual.extend(sds_timeslots(path, 400))  # detections_t, see Detections.sds.yml
    if not expected:
        raise RuntimeError("CameraIn contains no frames")
    if len(actual) != len(expected):
        raise RuntimeError(f"incomplete {operation}: {len(actual)}/{len(expected)} detection records; "
                           "closing streams does not prove all frames were processed")
    if actual != expected:
        raise RuntimeError(f"{operation} detection timeslots do not match the input frames in order")
    return len(actual)


def rtt_address(map_file: Path) -> int:
    """The _SEGGER_RTT control block from the linker map (the J-Link's search does not cover the DTCM)."""
    for line in map_file.read_text(errors="replace").splitlines():
        parts = line.split()
        # armlink memory map: "0x20001c34   0x000000a8   Zero   RW   5138    .bss._SEGGER_RTT    SEGGER_RTT.o"
        if ".bss._SEGGER_RTT" in parts and parts[0].startswith("0x"):
            return int(parts[0], 16)
    sys.exit(f"{map_file}: no _SEGGER_RTT")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["record", "play"])
    ap.add_argument("seconds", nargs="?", type=positive_seconds, default=20.0, help="recording length after both streams open (record only)")
    ap.add_argument("--device", default="AE722F80F55D5LS_M55_HP", help="J-Link device (the AppKit-E7)")
    ap.add_argument("--map", type=Path, default=ROOT / "out/traffic/AppKit-E7/Release/traffic.axf.map")
    ap.add_argument("--workdir", type=Path, default=ROOT / "recordings/traffic")
    ap.add_argument("--port", type=int, default=5050)
    ap.add_argument("--transport", choices=["usb", "rtt"], default="usb",
                    help="usb: the board's User USB (SDS:IO:USB, no J-Link); rtt: the J-Link RTT bridge")
    ap.add_argument("--timeout", type=positive_seconds, default=1800.0, help="longest playback in seconds")
    ap.add_argument("--connect-timeout", type=positive_seconds, default=60.0,
                    help="seconds to wait for a live SDS client before sending R/P (default: 60)")
    ap.add_argument("--stream-timeout", type=positive_seconds, default=60.0,
                    help="seconds to wait for recording streams to open/close (default: 60)")
    args = ap.parse_args()

    args.workdir.mkdir(parents=True, exist_ok=True)
    if args.mode == "play":
        camera_input = args.workdir / "CameraIn.0.sds"
        if not camera_input.is_file():
            sys.exit(f"Playback input not found: {camera_input}")
        if camera_input.stat().st_size == 0:
            sys.exit(
                f"Playback input is empty: {camera_input}. "
                "Move or replace the failed index-0 recording before playback."
            )
    python = sys.executable
    bridge = None if args.transport == "usb" else subprocess.Popen(
        [python, str(ROOT / "tools/sdsio_rtt_bridge.py"), "--device", args.device,
         "--rtt-addr", hex(rtt_address(args.map)), "--port", str(args.port)],
        stdout=sys.stderr, stderr=sys.stderr)
    if bridge is not None:
        time.sleep(3.0)  # the bridge listens before it attaches
    interface = ["usb"] if args.transport == "usb" else ["socket", "--port", str(args.port), "--connect"]

    master, slave = pty.openpty()
    server = subprocess.Popen(
        [python, str(SDS_UTILITIES / "sdsio-server.py"), *interface,
         "--workdir", str(args.workdir), "--no-progress-info"],
        stdin=slave, stdout=slave, stderr=slave, close_fds=True)
    os.close(slave)

    output = []  # everything SDSIO-Server printed

    def pump(seconds: float) -> None:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            ready, _, _ = select.select([master], [], [], 0.2)
            if ready:
                try:
                    data = os.read(master, 4096)
                except OSError as exc:
                    # A PTY master reports EIO after its child closes the slave.
                    # That is EOF, not the useful cause of a server startup
                    # failure; the server output immediately before it is.
                    if exc.errno == errno.EIO:
                        break
                    raise
                if not data:
                    break
                text = data.decode(errors="replace")
                output.append(text)
                sys.stdout.write(text)
                sys.stdout.flush()
            elif server.poll() is not None:
                break

    def command(key: bytes) -> None:
        if server.poll() is not None:
            pump(0.5)  # drain a traceback or diagnostic still buffered by the PTY
            raise RuntimeError(f"SDSIO-Server exited with status {server.returncode}")
        try:
            os.write(master, key)
        except OSError as exc:
            pump(0.5)
            status = server.poll()
            raise RuntimeError(
                f"SDSIO-Server closed its terminal"
                f"{f' with status {status}' if status is not None else ''}"
            ) from exc

    failed = None
    try:
        started_ns, session_output, elapsed = run_session(
            server, pump, command, output, mode=args.mode, seconds=args.seconds,
            timeout=args.timeout, connect_timeout=args.connect_timeout,
            stream_timeout=args.stream_timeout)
        command(b"X")
        pump(3.0)
        check_server_errors("".join(output))
        if args.mode == "play":
            frames = validate_playback(args.workdir, session_output, started_ns)
            print(f"\nPlayback verified: {frames}/{frames} frames in {elapsed:.1f} s "
                  f"({frames / max(elapsed, 0.001):.2f} frames/s)")
        else:
            frames = validate_recording(args.workdir, session_output, started_ns)
            print(f"\nRecording verified: {frames}/{frames} paired frames")
    except (RuntimeError, OSError) as exc:
        failed = exc
    finally:
        if server.poll() is None:
            try:
                command(b"X")
                pump(1.0)
            except (RuntimeError, OSError):
                pass  # Preserve the original diagnostic if shutdown also fails.
        for proc in (server, bridge):
            if proc is None:
                continue
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(5)
                except subprocess.TimeoutExpired:
                    proc.kill()
        os.close(master)
    if failed is not None:
        print(f"\nSDS session failed: {failed}", file=sys.stderr)
        return 1
    print("\nfiles:", *sorted(p.name for p in args.workdir.glob("*.sds")), sep="\n  ")
    return 0


if __name__ == "__main__":
    sys.exit(main())
