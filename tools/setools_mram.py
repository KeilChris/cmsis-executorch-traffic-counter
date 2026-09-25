#!/usr/bin/env python3
# Copyright 2026 Arm Limited and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Program an HP-core image into the MRAM of an Alif E8 board through the Secure Enclave (SETOOLS).

    python tools/setools_mram.py out/yolo/AppKit-E8/Release/yolo.hex [--part BS0] [--setools /Applications/Alif]

The image becomes the HP_APP of the boot table (.alif/M55_HP_mram_cfg.json with
the stub replaced by the image): the Secure Enclave starts the HP core on it
at every boot, without a debugger. Writing goes over the SEUART (SW4 on
SEUART), not through the J-Link, whose MRAM loader leaves the board wedged
after large downloads (a J-Link reset then never reaches main). 3 MB take
about 75 s. Afterwards, CMSIS Load finds the MRAM up to date and only resets.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
PARTS = {
    "BS0": "E8 (AE822FA0E5597BS0) - 5.5 MRAM / 9.75 SRAM",  # AppKit-E8
    "LS0": "E8 (AE822FA0E5597LS0) - 5.5 MRAM / 9.75 SRAM",  # DevKit-E8
}


def hex_to_bin(hex_file: Path) -> tuple[int, bytes]:
    base, chunks = 0, {}
    for line in hex_file.read_text().splitlines():
        count, addr, kind = int(line[1:3], 16), int(line[3:7], 16), int(line[7:9], 16)
        data = bytes.fromhex(line[9 : 9 + 2 * count])
        if kind == 4:
            base = int.from_bytes(data, "big") << 16
        elif kind == 0:
            chunks[base + addr] = data
    start = min(chunks)
    end = max(a + len(d) for a, d in chunks.items())
    image = bytearray(b"\xff" * (end - start))
    for a, d in chunks.items():
        image[a - start : a - start + len(d)] = d
    image += b"\xff" * (-len(image) % 16)  # the MRAM takes 16-byte lines
    return start, bytes(image)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("hex", type=Path)
    ap.add_argument("--part", choices=sorted(PARTS), default="BS0", help="BS0: AppKit-E8 (default), LS0: DevKit-E8")
    ap.add_argument("--setools", type=Path, default=Path("/Applications/Alif"), help="SETOOLS root (alif.setools.root)")
    args = ap.parse_args()

    start, image = hex_to_bin(args.hex)
    if start != 0x80200000:
        sys.exit(f"{args.hex}: starts at {start:#x}, not at the HP MRAM region 0x80200000")
    name = "hp_app_mram.bin"
    (args.setools / "build/images" / name).write_bytes(image)
    config = json.loads((HERE / ".alif/M55_HP_mram_cfg.json").read_text())
    config["HP_APP"]["binary"] = name
    config_file = args.setools / "build/config/hp_app_mram_cfg.json"
    config_file.write_text(json.dumps(config, indent=4))
    print(f"[setools] {args.hex.name}: {len(image)} bytes at {start:#x}")

    def run(*cmd: str, answer: str | None = None) -> None:
        result = subprocess.run(cmd, cwd=args.setools, input=answer, text=True, capture_output=True)
        lines = [l for l in result.stdout.replace("\r", "\n").splitlines() if "%" not in l and l.strip()]
        print("\n".join(lines[-4:]))
        if result.returncode != 0:
            sys.exit(f"[setools] {' '.join(cmd)} failed ({result.returncode}): {result.stderr.strip()}")

    run("./tools-config", "-p", PARTS[args.part])
    run("./app-gen-toc", "-f", str(config_file.relative_to(args.setools)))
    # The board's A1 silicon differs from the A0 the tools expect: confirm with y.
    run("./app-write-mram", "-p", answer="y\n")
    shutil.copy(args.setools / "build/app-package-map.txt", HERE / "tmp/app-package-map.txt")
    print("[setools] done; map in tmp/app-package-map.txt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
