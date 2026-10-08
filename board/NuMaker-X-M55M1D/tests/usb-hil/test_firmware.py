#!/usr/bin/env python3
"""Compile the complete USB HIL orchestration with mocked CMSIS API; never use USB."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmsis", type=Path, default=Path(os.environ.get(
        "CMSIS_PACK_ROOT", str(Path.home() / ".cache/arm/packs"))) / "ARM/CMSIS/6.2.0")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    includes = args.cmsis / "CMSIS/Driver/Include"
    if not (includes / "Driver_USBD.h").is_file():
        parser.error("CMSIS 6.2.0 headers not found; pass --cmsis /path/to/ARM/CMSIS/6.2.0")
    with tempfile.TemporaryDirectory(prefix="numaker-usb-hil-") as temporary:
        binary = Path(temporary) / "test-firmware"
        subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-I", str(includes), str(root / "test_firmware.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
