#!/usr/bin/env python3
"""Preprocess the DFP candidate's actual configuration include prologue.

No firmware is built and no board is accessed. Tests the project RTE include
path, the legacy PRJ_RTE_DEVICE_HEADER override, and a missing configuration.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


root = Path(__file__).resolve().parents[3]
default_driver = (root / "pack-work/Nuvoton.NuMicroM55_DFP/Library/CMSIS/Driver/Source/"
                  "Driver_USBD/Driver_USBD_HSUSBD.c")
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--driver", type=Path, default=default_driver)
driver = parser.parse_args().driver.resolve()
if not driver.is_file():
    parser.error(f"driver source does not exist: {driver}")
print(f"Configuration source under test: {driver}", flush=True)
source = driver.read_text()
prologue = source[source.index("#ifdef _RTE_"):source.index('#include "Driver_USBD.h"')]

with tempfile.TemporaryDirectory(prefix="numaker-usbd-config-") as tmp:
    directory = Path(tmp)
    project = directory / "project-rte"
    project.mkdir()
    bundled = directory / "RTE_Device"
    bundled.mkdir()
    # Deliberately poison the old relative fallback. A successful build must
    # select the project config or the explicit override, never this header.
    (bundled / "RTE_Device.h").write_text('#error "bundled RTE defaults selected"\n')
    (project / "RTE_Components.h").write_text("#define RTE_USBD1 1\n")
    config = project / "RTE_Device_USBD.h"
    config.write_text("#define CONFIG_SOURCE 73\n#define RTE_USBD1_MANAGE_POWER 0\n")
    (project / "custom_config.h").write_text(
        "#define CONFIG_SOURCE 29\n#define RTE_USBD1_MANAGE_POWER 0\n")
    unit = directory / "config.c"
    unit.write_text(prologue + """
#if RTE_USBD1 != 1
#error "HS instance selection was not preserved"
#endif
#if CONFIG_SOURCE != EXPECT_CONFIG_SOURCE
#error "wrong RTE configuration selected"
#endif
#if RTE_USBD1_MANAGE_POWER != 0
#error "project power setting was not preserved"
#endif
""")
    command = [os.environ.get("CC", "clang"), "-E", "-x", "c", "-Werror",
               "-D_RTE_", "-I", str(project), str(unit)]
    result = subprocess.run(command + ["-DEXPECT_CONFIG_SOURCE=73"],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run(command + ["-DEXPECT_CONFIG_SOURCE=29",
                                      '-DPRJ_RTE_DEVICE_HEADER="custom_config.h"'],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    # A missing generated header must fail loudly rather than compile against
    # the pack's own defaults. Only remove the temporary test fixture.
    config.unlink()
    result = subprocess.run(command + ["-DEXPECT_CONFIG_SOURCE=73"],
                            capture_output=True, text=True)
    assert result.returncode != 0 and "RTE_Device_USBD.h" in result.stderr, result.stderr
    assert "bundled RTE defaults selected" not in result.stderr, result.stderr
    # The explicit override must still work when the default header is absent.
    result = subprocess.run(command + ["-DEXPECT_CONFIG_SOURCE=29",
                                      '-DPRJ_RTE_DEVICE_HEADER="custom_config.h"'],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr

print("PASS: generated RTE config, explicit override, missing config, HS selection/power setting")
