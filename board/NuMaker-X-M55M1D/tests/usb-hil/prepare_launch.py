#!/usr/bin/env python3
"""Prepare a USB-HIL-only runner after checking identity and internal-flash HEX.

No debugger, programmer, USB access or installed-pack writes.
"""
from __future__ import annotations
import argparse
from pathlib import Path
import re
import tempfile

RUNNER_NAME = "cmsis-executorch+NuMaker-X-M55M1D.usb-hil.cbuild-run.yml"
IMAGE_BASE = "usb-hil/NuMaker-X-M55M1D/Release/usb-hil"
DEVICE_PACK = "Nuvoton::NuMicroM55_DFP@3.1.6-rc.1"
OUTPUT = ("  output:\n"
          f"    - file: {IMAGE_BASE}.axf\n"
          "      type: elf\n      project: usb-hil\n      load: symbols\n"
          f"    - file: {IMAGE_BASE}.hex\n"
          "      type: hex\n      project: usb-hil\n      load: image\n")


def validate_hex(text):
    base, total, eof = 0, 0, False
    for line in text.splitlines():
        if not line.strip():
            continue
        if eof or not line.startswith(":"):
            raise ValueError("invalid HEX or records after EOF")
        record = bytes.fromhex(line[1:])
        if len(record) < 5 or len(record) != record[0] + 5 or sum(record) & 255:
            raise ValueError("invalid HEX record length/checksum")
        size, kind = record[0], record[3]
        offset = int.from_bytes(record[1:3], "big")
        payload = record[4:-1]
        if kind == 0:
            address = base + offset
            if size and not (0x00100000 <= address and address + size <= 0x00300000):
                raise ValueError(f"HEX data outside internal flash: 0x{address:08x}")
            total += size
        elif kind in (2, 4) and size == 2 and offset == 0:
            base = int.from_bytes(payload, "big") << (4 if kind == 2 else 16)
        elif kind in (3, 5) and size == 4 and offset == 0:
            pass  # entry record, not load data
        elif kind == 1 and size == 0 and offset == 0:
            eof = True
        else:
            raise ValueError(f"unsupported HEX record type/length: {kind}/{size}")
    if not eof or total == 0:
        raise ValueError("missing HEX EOF or no image data")
    return total


def validate(text, output_dir):
    for key, expected in (("target-type", "NuMaker-X-M55M1D"), ("target-set", "usb-test"),
                          ("device", "Nuvoton::M55M1H2LJAE"),
                          ("device-pack", DEVICE_PACK)):
        if re.findall(rf"^  {key}: (.*)$", text, re.MULTILINE) != [expected]:
            raise ValueError(f"expected {key}: {expected}; select/build usb-test")
    outputs = re.findall(r"^  output:\n.*?(?=^  [^ \n]|\Z)", text, re.MULTILINE | re.DOTALL)
    if len(outputs) != 1 or re.sub(r"^      info: .*\n", "", outputs[0], flags=re.MULTILINE) != OUTPUT:
        raise ValueError("runner must contain only USB HIL Release ELF symbols and HEX image")
    for suffix in (".axf", ".hex"):
        image = output_dir / (IMAGE_BASE + suffix)
        if not image.is_file() or image.stat().st_size == 0:
            raise ValueError(f"missing/empty image: {image}")
    return validate_hex((output_dir / (IMAGE_BASE + ".hex")).read_text())


def prepare_launch(source, destination):
    source, destination = source.resolve(), destination.resolve()
    if source == destination or source.parent != destination.parent or destination.name != RUNNER_NAME:
        raise ValueError(f"destination must be a separate {RUNNER_NAME} beside the source")
    text = source.read_text()
    size = validate(text, source.parent)
    with tempfile.NamedTemporaryFile(mode="w", dir=source.parent, suffix=".yml", delete=False) as tmp:
        temporary = Path(tmp.name)
        tmp.write(text)
    try:
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    return size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    try:
        size = prepare_launch(args.source, args.destination)
        print(f"USB HIL runner ready: {size} internal-flash bytes; {args.destination}")
    except (OSError, ValueError) as exc:
        parser.exit(1, f"USB HIL launch refused: {exc}\n")


if __name__ == "__main__":
    main()
