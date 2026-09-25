#!/usr/bin/env python3
# Copyright 2026 Arm Limited and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Program and watch the DevKit-E8 without touching it (SW4 stays on SEUART).

    python tools/devkit.py flash out/quake/DevKit-E8/Debug/quake.hex
    python tools/devkit.py log   out/quake/DevKit-E8/Debug/quake.axf.map [--follow SECONDS]

flash  writes the image through the Secure Enclave (Alif SETOOLS: app-gen-toc,
       app-write-mram over the SEUART), as the M55_HP application the SE boots,
       and the SE resets the board. Both debugger routes into the MRAM have
       corrupted images on this board (J-Link's loader the first 16 bytes,
       pyOCD timed out on a page); the SE route is the vendor's and needs no
       debug port, so it also recovers a board whose debug port is dead.
log    prints the console of an image built with board/DevKit-E8/
       retarget_stdio_log.c: the `console_log` ring buffer in the DTCM, read
       through the J-Link while the core runs.

SETOOLS is found through $ALIF_SETOOLS_ROOT, else the `alif.setools.root`
setting of VS Code, else /Applications/Alif.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
MRAM_APP_BASE = 0x80200000  # APP_MRAM_HP_BASE: where the SE boots the M55_HP from
JLINK_DEVICE = "AE822FA0E5597LS0_M55_HP"


def setools_root() -> Path:
    if os.environ.get("ALIF_SETOOLS_ROOT"):
        return Path(os.environ["ALIF_SETOOLS_ROOT"])
    settings = Path.home() / "Library/Application Support/Code/User/settings.json"
    if settings.is_file():
        found = re.search(r'"alif\.setools\.root"\s*:\s*"([^"]+)"', settings.read_text())
        if found:
            return Path(found.group(1))
    return Path("/Applications/Alif")


def hex_to_bin(hex_file: Path) -> bytes:
    """The image of an Intel HEX file from MRAM_APP_BASE on, gaps filled with 0xFF."""
    base, chunks = 0, {}
    for line in hex_file.read_text().splitlines():
        count, addr, kind = int(line[1:3], 16), int(line[3:7], 16), int(line[7:9], 16)
        data = bytes.fromhex(line[9 : 9 + 2 * count])
        if kind == 4:
            base = int.from_bytes(data, "big") << 16
        elif kind == 0:
            chunks[base + addr] = data
    start = min(chunks)
    if start != MRAM_APP_BASE:
        sys.exit(f"{hex_file}: image starts at {start:#x}, not at {MRAM_APP_BASE:#x}")
    end = max(a + len(d) for a, d in chunks.items())
    image = bytearray(b"\xff" * (end - start))
    for a, d in chunks.items():
        image[a - start : a - start + len(d)] = d
    return bytes(image)


def flash(hex_file: Path) -> None:
    root = setools_root()
    image = hex_to_bin(hex_file)
    name = hex_file.stem + ".bin"
    (root / "build/images" / name).write_bytes(image)

    # The project's boot configuration, with this image as the M55_HP application.
    config = json.loads((HERE / ".alif/M55_HP_mram_cfg.json").read_text())
    config["HP_APP"]["binary"] = name
    config_file = root / "build/config" / (hex_file.stem + "_mram_cfg.json")
    config_file.write_text(json.dumps(config, indent=4))

    print(f"[devkit] {name}: {len(image)} bytes to {MRAM_APP_BASE:#x} through the Secure Enclave")
    for command in (["./app-gen-toc", "-f", str(config_file.relative_to(root))], ["./app-write-mram", "-p"]):
        result = subprocess.run(command, cwd=root, text=True, capture_output=True)
        # The tools draw progress bars with carriage returns: keep the statements.
        lines = [l for l in re.split(r"[\r\n]+", result.stdout + result.stderr) if l.strip() and "%:" not in l]
        for line in lines[-8:]:
            print("   ", line[:160])
        if result.returncode != 0:
            sys.exit(f"[devkit] {command[0]} failed ({result.returncode})")


def console_log_address(map_file: Path) -> int:
    # The memory map's line of the section: "0x20000000  0x0000800c  Zero  RW  3036  .bss.console_log  <object>"
    found = re.search(r"^\s*(0x[0-9a-fA-F]+)\s+0x[0-9a-fA-F]+\s+Zero\s.*\.bss\.console_log\b", map_file.read_text(errors="replace"), re.M)
    if not found:
        sys.exit(f"{map_file}: no .bss.console_log section (is the image built with retarget_stdio_log.c?)")
    return int(found.group(1), 16)


def read_memory(address: int, size: int) -> bytes:
    """Read target memory through J-Link Commander without halting the core."""
    with tempfile.TemporaryDirectory() as tmp:
        out, script = Path(tmp) / "mem.bin", Path(tmp) / "read.jlink"
        script.write_text(f"savebin {out} {address:#x} {size:#x}\nexit\n")
        result = subprocess.run(
            ["JLinkExe", "-device", JLINK_DEVICE, "-if", "swd", "-speed", "4000", "-autoconnect", "1", "-NoGui", "1",
             "-ExitOnError", "1", "-CommandFile", str(script)],
            text=True, capture_output=True)
        if not out.is_file():
            tail = [l for l in result.stdout.splitlines() if l.strip()][-3:]
            sys.exit("[devkit] J-Link could not read the target: " + " | ".join(tail))
        return out.read_bytes()


LOG_SIZE = 0x8000  # CONSOLE_LOG_SIZE of retarget_stdio_log.c


def read_log(address: int) -> tuple[int, str]:
    raw = read_memory(address, 12 + LOG_SIZE)  # one debugger session per poll
    magic, size, head = struct.unpack("<III", raw[:12])
    if magic != 0x474F4C43 or size != LOG_SIZE:
        return 0, ""
    text = raw[12:]
    if head <= size:
        text = text[:head]
    else:  # wrapped: the oldest character is at head % size
        text = text[head % size :] + text[: head % size]
    return head, text.decode("latin-1")


def log(map_file: Path, follow: float) -> None:
    address = console_log_address(map_file)
    shown, deadline = 0, time.time() + follow
    while True:
        head, text = read_log(address)
        if head > shown:
            sys.stdout.write(text[-(head - shown):] if head - shown < len(text) else text)
            sys.stdout.flush()
            shown = head
        if time.time() >= deadline:
            break
        time.sleep(2.0)
    if shown == 0:
        print(f"[devkit] console_log at {address:#x} is empty")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    flash_parser = commands.add_parser("flash", help="program an image through the Secure Enclave")
    flash_parser.add_argument("hex", type=Path)
    log_parser = commands.add_parser("log", help="print the console log of the running image")
    log_parser.add_argument("map", type=Path)
    log_parser.add_argument("--follow", type=float, default=0.0, metavar="SECONDS", help="keep reading for this long")
    args = parser.parse_args()
    if not shutil.which("JLinkExe") and args.command == "log":
        sys.exit("JLinkExe is not on the PATH")
    if args.command == "flash":
        flash(args.hex)
    else:
        log(args.map, args.follow)


if __name__ == "__main__":
    main()
