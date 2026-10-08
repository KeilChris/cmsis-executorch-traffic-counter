#!/usr/bin/env python3
"""Validate the isolated camera image and prepare its runner; never access a probe."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "Flash"))
from prepare_cbuild_run import prepare


RUNNER_NAME = "cmsis-executorch+NuMaker-X-M55M1D.camera-hil.hyperram.cbuild-run.yml"
IMAGE_BASE = "camera-hil/NuMaker-X-M55M1D/Release/camera-hil"
OUTPUT = (
    "  output:\n"
    f"    - file: {IMAGE_BASE}.axf\n"
    "      type: elf\n"
    "      project: camera-hil\n"
    "      load: symbols\n"
    f"    - file: {IMAGE_BASE}.hex\n"
    "      type: hex\n"
    "      project: camera-hil\n"
    "      load: image\n"
)


def validate(text: str, output_dir: Path) -> None:
    # Deliberately accept only the known generated layout, not arbitrary YAML.
    # A format change must fail closed rather than load a different image.
    for key, expected in (
        ("target-type", "NuMaker-X-M55M1D"),
        ("target-set", "camera-test"),
        ("device", "Nuvoton::M55M1H2LJAE"),
    ):
        if re.findall(rf"^  {key}: (.*)$", text, re.MULTILINE) != [expected]:
            raise ValueError(f"expected {key}: {expected}; select/build the camera-test target-set")
    outputs = re.findall(
        r"^  output:\n.*?(?=^  [^ \n]|\Z)", text, re.MULTILINE | re.DOTALL
    )
    if len(outputs) != 1 or re.sub(r"^      info: .*\n", "", outputs[0], flags=re.MULTILINE) != OUTPUT:
        raise ValueError("runner must contain only camera-hil Release ELF symbols and HEX image")
    for suffix in (".axf", ".hex"):
        image = output_dir / (IMAGE_BASE + suffix)
        if not image.is_file() or image.stat().st_size == 0:
            raise ValueError(f"missing/empty {image}; build camera-test first")


def prepare_launch(source: Path, destination: Path) -> Path:
    source, destination = source.resolve(), destination.resolve()
    if destination.name != RUNNER_NAME or destination == source:
        raise ValueError(f"destination must be the separate {RUNNER_NAME}")
    if source.parent != destination.parent:
        raise ValueError("source and prepared runner must share the out directory (relative paths)")
    validate(source.read_text(encoding="utf-8"), source.parent)
    # Preserve both the traffic runner and any previously valid HIL runner on
    # failure. Keep the temporary runner alongside the source for relative paths.
    with tempfile.NamedTemporaryFile(dir=source.parent, suffix=".cbuild-run.yml", delete=False) as tmp:
        temporary = Path(tmp.name)
    try:
        prepare(source, temporary)
        validate(temporary.read_text(encoding="utf-8"), source.parent)
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    return destination


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    try:
        print(prepare_launch(args.source, args.destination))
    except (OSError, ValueError) as error:
        parser.exit(1, f"Camera HIL launch refused: {error}\n")


if __name__ == "__main__":
    main()
