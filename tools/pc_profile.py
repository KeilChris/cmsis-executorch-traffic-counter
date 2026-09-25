#!/usr/bin/env python3
# Copyright 2026 Arm Limited and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Statistical profile of the running target: sample the DWT program counter sample register
(DWT_PCSR) through the J-Link while the core runs, and map the samples to functions of the ELF.

    .venv/bin/python tools/pc_profile.py out/quake/AppKit-E8/Debug/quake.axf --seconds 20 [--tag-symbol benchmark_mode]

Nothing is added to the firmware and the core is never halted. With --tag-symbol the samples are
grouped by the value of a 32-bit variable (read every 64 samples), e.g. the present path in use.
"""
from __future__ import annotations

import argparse, bisect, collections, re, shutil, subprocess, time
from pathlib import Path

import pylink

DWT_PCSR = 0xE000101C
FROMELF = Path.home() / ".vcpkg/artifacts/2139c4c6/compilers.arm.armclang/6.24.0/bin/fromelf"


def symbols(elf: Path):
    text = subprocess.run([str(FROMELF), "--text", "-s", str(elf)], capture_output=True, text=True).stdout
    code, data = [], {}
    for line in text.splitlines():
        m = re.match(r"\s*\d+\s+(\S+)\s+0x([0-9a-f]+)\s+\S+\s+\S+\s+(Code|Data)\s", line)
        if not m or m.group(1).startswith("$"):
            continue
        if m.group(3) == "Code":
            code.append((int(m.group(2), 16) & ~1, m.group(1)))
        else:
            data[m.group(1)] = int(m.group(2), 16)
    code.sort()
    return code, data


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--seconds", type=float, default=20.0)
    parser.add_argument("--device", default="AE822FA0E5597BS0_M55_HP")
    parser.add_argument("--tag-symbol")
    parser.add_argument("--top", type=int, default=22)
    args = parser.parse_args()

    code, data = symbols(args.elf)
    starts = [a for a, _ in code]
    lib = pylink.Library(dllpath=str(Path(shutil.which("JLinkExe")).resolve().parent / "libjlinkarm.dylib"))
    j = pylink.JLink(lib=lib); j.open(); j.exec_command("SetBatchMode 1"); j.exec_command("DisableFlashDL")
    j.set_tif(pylink.enums.JLinkInterfaces.SWD); j.connect(args.device, speed=4000)
    tag_address = data.get(args.tag_symbol) if args.tag_symbol else None

    histograms: dict[int, collections.Counter] = collections.defaultdict(collections.Counter)
    tag, count, end = 0, 0, time.time() + args.seconds
    while time.time() < end:
        if tag_address is not None and count % 64 == 0:
            tag = j.memory_read32(tag_address, 1)[0]
        pc = j.memory_read32(DWT_PCSR, 1)[0]
        count += 1
        if pc == 0xFFFFFFFF:
            histograms[tag]["<sleeping / no sample>"] += 1
            continue
        i = bisect.bisect_right(starts, pc & ~1) - 1
        histograms[tag][code[i][1] if i >= 0 else hex(pc)] += 1
    j.close()

    print(f"{count} samples in {args.seconds:.0f} s ({count / args.seconds:.0f}/s)")
    for tag, histogram in sorted(histograms.items()):
        total = sum(histogram.values())
        print(f"\n--- {args.tag_symbol or 'all'} = {tag}: {total} samples")
        for name, n in histogram.most_common(args.top):
            print(f"  {100.0 * n / total:5.1f} %  {name}")


if __name__ == "__main__":
    main()
