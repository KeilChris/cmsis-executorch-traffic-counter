#!/usr/bin/env python3
# Copyright 2026 Arm Limited and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Load the pak image into the AppKit-E8's PSRAM through the J-Link.

    .venv/bin/python tools/load_pak.py [quake/data/id1/pak0.pak] [--address 0xA1000000]

Run it while the Quake image waits for the pak ("load id1/pak0.pak to
0xa1000000 now"). Plain memory writes through pylink, the core keeps running:
J-Link Commander's `loadbin` resets the core first, which loses the PSRAM
set-up, and its flash download takes the OSPI windows for flash banks.
"""

from __future__ import annotations

import argparse
import shutil
import sys
import time
from pathlib import Path

import pylink


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pak", nargs="?", type=Path, default=Path("quake/data/id1/pak0.pak"))
    parser.add_argument("--address", type=lambda v: int(v, 0), default=0xA1000000)
    parser.add_argument("--verify-only", action="store_true", help="do not write the image first, only verify and repair")
    parser.add_argument("--device", default="AE822FA0E5597BS0_M55_HP")
    args = parser.parse_args()

    data = args.pak.read_bytes()
    data += b"\0" * (-len(data) % 4)
    # pylink's default J-Link DLL may be an old one without the Alif devices: use JLinkExe's.
    exe = shutil.which("JLinkExe")
    lib = pylink.Library(dllpath=str(Path(exe).resolve().parent / "libjlinkarm.dylib")) if exe else None
    jlink = pylink.JLink(lib=lib) if lib else pylink.JLink()
    jlink.open()
    jlink.exec_command("SetBatchMode 1")
    jlink.exec_command("DisableFlashDL")
    jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
    jlink.connect(args.device, speed=4000)

    chunk = 0x10000

    def read(offset: int, size: int) -> bytes:
        return b"".join(w.to_bytes(4, "little") for w in jlink.memory_read32(args.address + offset, size // 4))

    if not args.verify_only:
        start = time.time()
        for offset in range(0, len(data), chunk):
            block = data[offset : offset + chunk]
            jlink.memory_write32(args.address + offset, [int.from_bytes(block[i : i + 4], "little") for i in range(0, len(block), 4)])
            if offset % (chunk * 32) == 0:
                print(f"\r[pak] write {offset >> 10} / {len(data) >> 10} kB", end="", flush=True)
        seconds = time.time() - start
        print(f"\r[pak] {len(data)} bytes to {args.address:#x} in {seconds:.0f} s ({len(data) / 1024 / seconds:.0f} kB/s)")

    # The debugger's burst writes into the PSRAM window occasionally drop a
    # 32-byte block without an error; single-word writes stick. Read everything
    # back and rewrite what differs, word by word, until a pass is clean.
    for attempt in range(1, 6):
        repaired = 0
        for offset in range(0, len(data), chunk):
            want = data[offset : offset + chunk]
            got = read(offset, len(want))
            if got == want:
                continue
            for i in range(0, len(want), 4):
                if got[i : i + 4] != want[i : i + 4]:
                    jlink.memory_write32(args.address + offset + i, [int.from_bytes(want[i : i + 4], "little")])
                    repaired += 1
        print(f"[pak] verify pass {attempt}: {repaired} words rewritten")
        if repaired == 0:
            break
    else:
        jlink.close()
        sys.exit("[pak] still differs after 5 passes")
    jlink.close()


if __name__ == "__main__":
    main()
