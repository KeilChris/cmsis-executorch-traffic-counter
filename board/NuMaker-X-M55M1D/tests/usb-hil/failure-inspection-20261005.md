# USB HIL failure inspection — 2026-10-05

## Evidence and preservation

User reported:
```text
USB HIL failed; retain this output and inspect before retry/reset.
```

When asked for the full terminal output, the user supplied only:
```c
        SCB->DCISW = (((sets << SCB_DCISW_SET_Pos) & SCB_DCISW_SET_Msk) |
```

The verifier JSON error and completed-case list were not available in the
conversation. The banner alone does not identify the failing test.

Inspected through CMSIS Developer Assistant MCP, window pid 18239. The target
was running and was paused for inspection. No test retry, reset, reflash,
register write, cache-maintenance operation, or firmware/configuration edit
was performed. The target is left paused with the debug session open.

## Debugger-symbol mismatch

Active session: `CMSIS_DAP@pyOCD (launch)`. Its launch configuration names
`out/traffic/NuMaker-X-M55M1D/Release/traffic.axf`, not USB HIL.
The displayed stack was `SCB_InvalidateDCache` / traffic `Reset_Handler`;
that source interpretation is not reliable for the running USB HIL image.

Current build index selects `NuMaker-X-M55M1D@usb-test`,
`usb-hil.Release+NuMaker-X-M55M1D`, Nuvoton M55M1H2LJAE, Cortex-M55,
DFP 3.1.5, AC6, CMSIS-DAP SWD 1 MHz.

Core snapshot:
- PC `0x0010667e`, LR `0x001067ed`, SP/MSP/PSP `0x20020000`.
- xPSR: Z C, GE=0, EXC=0, T; PRIMASK/BASEPRI/FAULTMASK all zero.
- Artifact lookup for USB HIL places PC in `main + 10`,
  main at `0x00106674`, size 14 bytes.
- Target flash bytes at main:
  `00 f0 7e f8 30 bf 30 bf 30 bf 30 bf fa e7 00 00`.
  PC points at a WFI instruction in the intentional idle loop.

This establishes that the displayed cache-invalidation source line is
misleading, not that the USB test succeeded or that the failure cause is known.

## Raw memory evidence

Addresses were resolved against `usb-hil.axf` using the artifact tools.
These are debugger memory snapshots, NOT the host's failed-run status response.
No cache-coherency assumption is made. In particular, the raw harness status
reports IN count 8192 while raw driver endpoint state reports 8191; without
the host log and matching live symbols, do not assign the failure to a case
or infer memory corruption from this mismatch.

Harness status at `0x2010018c`, interpreted as the current 64-byte wire layout:
```json
{
  "magic": 1430800716,
  "version": 1,
  "configured": 1,
  "mps": 512,
  "resets": 1,
  "case_id": 47,
  "mode": 50,
  "active": 0,
  "requested": 8192,
  "out_count": 0,
  "in_count": 8192,
  "out_callbacks": 0,
  "in_callbacks": 1,
  "aborts": 3,
  "errors": 0,
  "last_driver_error": 0
}
```

### Raw harness-status tool response

```text
Memory at 0x2010018c (64 bytes):

Hex:
  0x2010018c: 4c 49 48 55 01 00 00 00 01 00 00 00 00 02 00 00
  0x2010019c: 01 00 00 00 2f 00 00 00 32 00 00 00 00 00 00 00
  0x201001ac: 00 20 00 00 00 00 00 00 00 20 00 00 00 00 00 00
  0x201001bc: 01 00 00 00 03 00 00 00 00 00 00 00 00 00 00 00

```

### Raw driver-state tool response

`usbd1_rw_info` is a 512-byte object at `0x20104de0` from
`Driver_USBD_HSUSBD.o`.

```text
Memory at 0x20104de0 (512 bytes):

Hex:
  0x20104de0: 59 55 10 00 91 55 10 00 03 00 02 01 40 04 00 00
  0x20104df0: 00 00 00 00 00 00 00 00 40 33 00 00 00 00 00 00
  0x20104e00: 80 0d 10 20 01 02 00 00 01 00 00 02 01 02 00 00
  0x20104e10: 00 00 00 00 01 00 00 00 c0 2d 10 20 00 20 00 00
  0x20104e20: 01 00 00 02 ff 1f 00 00 00 00 00 00 81 00 00 00
  0x20104e30: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104e40: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104e50: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104e60: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104e70: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104e80: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104e90: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104ea0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104eb0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104ec0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104ed0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104ee0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104ef0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104f00: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104f10: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104f20: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104f30: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104f40: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104f50: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104f60: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104f70: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104f80: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104f90: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104fa0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  0x20104fb0: 00 00 00 00 00 00 00 00 01 00 40 00 00 00 00 00
  0x20104fc0: 00 00 00 00 00 00 00 00 14 0d 10 20 00 00 00 00
  0x20104fd0: 01 00 40 00 00 00 00 00 00 00 00 00 80 00 00 00

```

## Pending

Recover the Python verifier's JSON `error` field and last PASS lines from the
terminal that ran `verify_usb.py`. No terminal-output MCP tool is available
in this session. Do not rerun just to recover the old output.

Correct debugger symbols without resetting if further live inspection is
needed. The dedicated `NuMaker USB HIL (launch)` configuration is available
for a later explicitly approved fresh run; starting it now would reload/reset
and discard this preserved state. USB bulk qualification remains unpassed.

