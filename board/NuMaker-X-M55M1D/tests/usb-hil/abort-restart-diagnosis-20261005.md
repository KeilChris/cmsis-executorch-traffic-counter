# USB HIL: partial-OUT abort/restart failure — 2026-10-05

## Result

The full host output is retained in
[20261005-abort-restart.log](evidence/20261005-abort-restart.log).

- High-speed bulk MPS: 512.
- 59 completed cases, all with zero reported target error bits.
- Includes all 48 normal boundary-length OUT/IN/loopback cases, four
  short/ZLP cases, both callback-rearm cases, pending-abort/restart cases,
  and the finite partial-OUT abort check itself.
- Case 59: received 512 bytes of an 8192-byte OUT request, aborted, no
  completion callback, active=0, aborts=3, errors=0.
- Case 60: restarted OUT request of 513 bytes; target returned out_count=1,
  one completion callback, active=0, errors=8 (payload pattern mismatch).
- Suite therefore FAILED. The 59th case passing does not qualify the whole
  abort contract; its immediate restart failed. Unconfigure/reconfigure
  and optional reset qualification have not passed in this run.

## Live inspection

CMSIS Developer Assistant verified session `NuMaker USB HIL (launch)`,
program `out/usb-hil/NuMaker-X-M55M1D/Release/usb-hil.axf`,
runner `out/cmsis-executorch+NuMaker-X-M55M1D.usb-hil.cbuild-run.yml`,
CMSIS target `NuMaker-X-M55M1D@usb-test`, bundled pyOCD and GDB from CMSIS
Debugger 1.8.1-29-ga9128a9.

The target was running, then paused via the MCP tool. Stack: USB HIL main.c
line 11 (intentional WFI idle loop). No reset/retry/reflash or register writes.

Debugger RAM reads differ from the host's status response: harness case_id=57,
active=2, errors=0, aborts=3, while the host recorded case_id=60, active=0,
errors=8; raw OUT driver state still reports an earlier 8191-byte request.
These RAM reads are not accepted as a coherent failure snapshot. Cache/debug
read visibility remains a caveat; no cache maintenance was performed.

Peripheral snapshots after halt:
- HSUSBD_EPARSPCTL at 0x40205074: 0x00000008 (toggle=1).
- HSUSBD_EPAINTEN at 0x4020506c: 0.
- HSUSBD_EPADATCNT at 0x40205070: 0.

## Source defect and documented semantics

Selected source:
`Nuvoton/NuMicroM55_DFP/3.1.5/Library/CMSIS/Driver/Source/Driver_USBD/Driver_USBD_HSUSBD.c`,
`USBDn_EndpointTransferAbort`, line 1183:

```c
ep->EPRSPCTL |= HSUSBD_EPRSPCTL_FLUSH_Msk;
```

Documentation inspected through CMSIS documentation tools:
`user/nuvoton/numicrom55-dfp/en-us-trm-m55m1-series-en-rev1-02`,
M55M1 TRM **Rev 1.02**, August 20 2026, **pp.2807–2808**,
Endpoint A–R Response Control Register. SVD lookup confirms positions.

- Bit 0 requests FIFO flush.
- Bit 3 reads the current endpoint data toggle.
- Writing bit 3 as 1 clears that toggle; writing it as 0 does not clear it.

Consequently a read-modify-write with TOGGLE=1 writes that 1 back and clears
the sequence while requesting the flush. An ordinary local transfer abort
must not silently desynchronize the endpoint from the host.

Offline arithmetic of these documented register semantics:
| Toggle before | Existing flush write | Toggle after | Flush write excluding TOGGLE | Toggle after |
| --- | --- | --- | --- | --- |
| 0 | 0x01 | 0 | 0x01 | 0 |
| 1 | 0x09 | 0 | 0x01 | 1 |

This is a semantics check, not a hardware test of a corrected driver.
Unrelated response-control fields are omitted from this illustrative table;
a real patch must preserve appropriate configuration fields and must not
replay other command bits.

The observed loss of the initial 512 bytes of the next 513-byte request is
consistent with packet-sequence desynchronization: the first packet is
treated as a duplicate and the remaining one-byte packet completes as a
short packet. No USB bus trace was captured, so this packet-level explanation
is an inference from the failure signature plus the documented source defect,
not a directly observed bus trace.

The same abort function masks/clears IN activity but has no equivalent
OUT-interrupt retirement. That also needs review for a correct abort
contract, though it is not independently established as the cause of this
specific pattern failure.

## Next work

Prepare a separate writable DFP candidate, preserving installed pack caches
and the archived source-review bundle. Correct abort response-register writes
to retain the data toggle and retire OUT receive activity; add register-aware
regressions that model the write-one-to-clear toggle, then build/reload and
rerun the unchanged 62-case HIL suite. Do not weaken the test or reset the
endpoint's sequence in the verifier merely to mask the defect.

No driver or firmware was changed in this investigation. The installed pack,
archived candidate and writable-pack location were not patched. The board is
left paused for follow-up.

