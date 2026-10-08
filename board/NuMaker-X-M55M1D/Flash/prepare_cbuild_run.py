#!/usr/bin/env python3
"""Replace the M55M1 DFP's SPI-flash region with the board HyperRAM loader."""

from __future__ import annotations

import argparse
import os
import re
from pathlib import Path


HYPERRAM_BASE = 0x82000000
HYPERRAM_SIZE = 0x00800000


def prepare(source: Path, destination: Path, image: Path | None = None) -> Path:
    text = source.read_text(encoding="utf-8")
    if "  target-type: NuMaker-X-M55M1D\n" not in text:
        return source.resolve()

    flash_dir = Path(__file__).resolve().parent
    algorithm = flash_dir / "M55M1_HyperRAM.FLM"
    if not algorithm.is_file():
        raise FileNotFoundError(
            f"{algorithm} is missing; run {flash_dir / 'build.sh'} first"
        )
    relative_algorithm = os.path.relpath(algorithm, destination.parent)

    spi_algorithm = re.compile(
        r"^    - algorithm: .*?/M55M1_SPIM\.FLM\n"
        r"      start: 0x82000000\n"
        r"      size: 0x02000000\n"
        r"      ram-start: 0x20000000\n"
        r"      ram-size: 0x00008000\n",
        re.MULTILINE,
    )
    replacement = (
        f"    - algorithm: {relative_algorithm}\n"
        "      start: 0x82000000\n"
        "      size: 0x00800000\n"
        "      ram-start: 0x20000000\n"
        "      ram-size: 0x00020000\n"
    )
    text, replacements = spi_algorithm.subn(replacement, text)
    if replacements != 1:
        raise ValueError("the generated runner has no SPIM0 programming region")

    if image is not None:
        relative_image = os.path.relpath(image.resolve(), destination.parent)
        output = re.compile(
            r"^  output:\n.*?(?=^  system-resources:\n)",
            re.MULTILINE | re.DOTALL,
        )
        replacement = (
            "  output:\n"
            f"    - file: {relative_image}\n"
            "      type: hex\n"
            "      load: image\n"
        )
        text, replacements = output.subn(replacement, text)
        if replacements != 1:
            raise ValueError("the generated runner has no output section")

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(text, encoding="utf-8")
    return destination.resolve()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--image", type=Path)
    args = parser.parse_args()
    print(prepare(args.source.resolve(), args.destination.resolve(), args.image))


if __name__ == "__main__":
    main()
