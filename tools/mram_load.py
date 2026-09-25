#!/usr/bin/env python3
# Copyright 2026 Arm Limited and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Program an image into the MRAM of an Alif Ensemble E8 board through the J-Link, and verify it.

    python tools/mram_load.py out/quake/AppKit-E8/Debug/quake.hex [--device AE822FA0E5597BS0_M55_HP] [--no-run]

Why not J-Link's `loadfile`: its MRAM loader runs on the target and fails with
"block verification error" when the core comes out of reset in a bad state,
which a damaged first MRAM line (the vector table) guarantees; and its
in-session read-back is answered from a cache, so a wrong line looks right.
32-bit debugger writes do not work either: the MRAM commits 16-byte lines and
wants them as 64-bit stores (drivers/source/mram.c in the Ensemble pack).

So this stages the image in the bulk SRAM, runs a ten-instruction copy loop on
the halted core (two 64-bit stores per line, from the DTCM) and then reads the
whole image back in a fresh J-Link session and compares it.
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

MRAM_APP_BASE = 0x80200000
STAGE = 0x02100000  # bulk SRAM, clear of the Cortex-A32 stub at 0x02000000
CODE = 0x20080000  # DTCM
STACK = 0x200A0000

# r0 = MRAM line, r1 = source, r2 = source end
COPY_LOOP = [
    0x4500E9D1,  # ldrd r4, r5, [r1]
    0x6702E9D1,  # ldrd r6, r7, [r1, #8]
    0x4500E9C0,  # strd r4, r5, [r0]
    0x6702E9C0,  # strd r6, r7, [r0, #8]
    0x31103010,  # adds r0, #16 ; adds r1, #16
    0xD3F34291,  # cmp r1, r2 ; bcc loop
    0x0000BE00,  # bkpt
]


def hex_to_bin(hex_file: Path) -> bytes:
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
    image += b"\xff" * (-len(image) % 16)  # whole MRAM lines
    return bytes(image)


def jlink(device: str, commands: list[str]) -> str:
    with tempfile.NamedTemporaryFile("w", suffix=".jlink", delete=False) as script:
        script.write("\n".join(commands + ["exit"]) + "\n")
    result = subprocess.run(
        ["JLinkExe", "-device", device, "-if", "swd", "-speed", "4000", "-autoconnect", "1", "-NoGui", "1",
         "-ExitOnError", "1", "-CommandFile", script.name],
        text=True, capture_output=True)
    Path(script.name).unlink()
    if "Could not connect" in result.stdout or "Failed to power up DAP" in result.stdout:
        sys.exit("[mram] J-Link cannot connect to the core (debug port down?)")
    return result.stdout


def bad_lines(device: str, image: bytes, tmp: Path) -> list[int]:
    back = tmp / "readback.bin"
    jlink(device, ["h", f"savebin {back} {MRAM_APP_BASE:#x} {len(image):#x}"])
    got = back.read_bytes()[: len(image)]
    return [i for i in range(0, len(image), 16) if image[i : i + 16] != got[i : i + 16]]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("hex", type=Path)
    parser.add_argument("--device", default="AE822FA0E5597BS0_M55_HP", help="J-Link device name")
    parser.add_argument("--no-run", action="store_true", help="leave the core halted after programming")
    args = parser.parse_args()

    image = hex_to_bin(args.hex)
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        (tmp / "image.bin").write_bytes(image)
        words = " ".join(f"{w:#010x}" for w in COPY_LOOP)
        print(f"[mram] {args.hex.name}: {len(image)} bytes to {MRAM_APP_BASE:#x}")
        out = jlink(args.device, [
            "exec DisableFlashDL",  # plain memory accesses only
            "r",                    # caches and MPU off, whatever ran before
            "h",
            f"loadbin {tmp / 'image.bin'} {STAGE:#x}",
            f"w4 {CODE:#x} {words}",
            f"wreg R0 {MRAM_APP_BASE:#x}",
            f"wreg R1 {STAGE:#x}",
            f"wreg R2 {STAGE + len(image):#x}",
            f"wreg MSP {STACK:#x}",
            f"setpc {CODE:#x}",
            "g",
            "Sleep 3000",
            "h",
            "regs",
        ])
        stopped = [l for l in out.splitlines() if l.startswith("PC =")]
        if not stopped or f"{CODE + 0x18:08X}" not in stopped[-1].upper():
            print("[mram] the copy loop did not end at its breakpoint:", stopped[-1] if stopped else "no PC")
        bad = bad_lines(args.device, image, tmp)
        if bad:
            sys.exit(f"[mram] VERIFY FAILED: {len(bad)} of {len(image) // 16} lines differ, first at {MRAM_APP_BASE + bad[0]:#x}")
        print(f"[mram] verified, {len(image) // 16} lines, vectors "
              + " ".join(f"{w:08X}" for w in struct.unpack("<4I", image[:16])))
        if not args.no_run:
            jlink(args.device, ["r", "g"])
            print("[mram] reset, running")


if __name__ == "__main__":
    main()
