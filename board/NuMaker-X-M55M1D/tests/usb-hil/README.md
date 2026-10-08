# Isolated USB Device driver qualification

This is a **separate firmware image**, not traffic or camera HIL. It calls the
packaged `Driver_USBD1` from local DFP **3.1.6-rc.1** (HS component 1.2.0)
directly. There is no
MDK USB middleware, SDS, RTX, camera, LCD, model or HyperRAM initialization.
The LCD will not update. The BSP in solution metadata identifies the board;
no BSP camera/display/memory component is linked into this project.

## Current status — 2026-10-06

- The DFP 3.1.5 run failed after 59 passing cases: the partial-OUT-abort
  restart returned 1 byte instead of 513. The full evidence and register
  explanation are in [the diagnosis](abort-restart-diagnosis-20261005.md).
- USB HIL alone now selects the separate abort-hardening DFP 3.1.6-rc.1.
  Installed packs, traffic/camera DFP selection and the 62-case test matrix and
  assertions are unchanged. The launch guard rejects the old DFP 3.1.5 runner.
- Candidate target build **PASS**, CMSIS job b-1; load/debug **PASS**, job d-2.
  Verified `usb-hil.axf` at this project's `main.c:9`, then continued past
  `main()` with no user breakpoints. Guarded load payload: **30,880 bytes**,
  all in internal flash. The user subsequently reported **62 cases verified**
  at 512-byte MPS. A subsequently logged optional reset run passed **64 cases**
  (`logs/usb-hil/20261006T062411.444036Z-39400.log`). Its repeat failed after
  62 cases, while configuring the newly rediscovered handle: **NO_DEVICE**,
  errno 19 / libusb −4 (`logs/usb-hil/reset-repeat-20261006-02.log`). This was
  the failure investigated before the host-only correction below.
- The host verifier saves complete logs and now retries the entire post-reset
  connection handshake, not just discovery, within one 10-second window.
  Only absent devices / NO_DEVICE are retryable; firmware/protocol errors and
  other USB errors remain fatal. **56 offline host/launch/logging/reconnection
  tests PASS**, including simulated 62/64-case runs and negative tests. This
  host-only fix needed no firmware rebuild.
- Hardware retest **PASS twice, 64/64 cases each, at 512-byte MPS**:
  `reset-reconnect-20261006-03.log` recovered the same configuration NO_DEVICE
  on attempt 2 in 106.5 ms; `reset-reconnect-20261006-04.log` connected on
  attempt 1 in 3.24 ms. Reset counts advanced 1 → 2 → 3, abandoned work had no
  completion callbacks, and both fresh 8192-byte loopbacks passed. The specific
  host-reconnection failure is closed for these runs, not a general USB or
  full-speed qualification claim. Original logs are retained under `logs/usb-hil/`.
- [Candidate integration and hashed hardware evidence](../../../../pack-work/Nuvoton.NuMicroM55_DFP/abort-hardening/integration-20261006.md#hardware-retest--pass-at-high-speed).

## Optional full-speed procedure — skipped

**Intentionally skipped at the user's request on 2026-10-06.** Qualification
for this effort is high-speed-only; no full-speed hardware run is claimed and
no firmware speed override has been added. The procedure below is retained
only as an optional reference, not a scheduled next step. An ordinary direct
high-speed connection is not a substitute for full-speed evidence.

Keep the same candidate firmware and `Driver_USBD1`. A full-speed-only USB path
to J13 is needed (for example, a known 12 Mbit/s-only hub or a test fixture with
a documented full-speed setting). Do not assume an ordinary USB 2.0 hub limits
speed. Do not change peripheral registers or descriptor packet sizes to manufacture
coverage. `--expect-speed full` is an assertion, **not** a speed-selection command.

1. Finish/close any host verifier or SDS server before changing the connection.
   Leave the debugger cable connected. Route J13 through the full-speed-only
   path once its capabilities are confirmed. Keep the USB HIL application
   running past `main()` with breakpoints disabled.
2. First run identification/configuration only, from the repository root:

   ```sh
   .venv/bin/python board/NuMaker-X-M55M1D/tests/usb-hil/verify_usb.py --status-only --expect-speed full --libusb /opt/homebrew/lib/libusb-1.0.dylib --log-file logs/usb-hil/full-speed-status-20261006-01.log
   ```

   Require `CONNECTED (bulk tests not run)` and, in the log, `configured: 1`,
   `mps: 64`, `errors: 0`. If speed validation fails, stop and inspect that log;
   do not drop the speed assertion. No bulk/reset test has run in this step.
3. With the setup unchanged, run the 62-case suite:

   ```sh
   .venv/bin/python board/NuMaker-X-M55M1D/tests/usb-hil/verify_usb.py --expect-speed full --libusb /opt/homebrew/lib/libusb-1.0.dylib --log-file logs/usb-hil/full-speed-transfer-20261006-01.log
   ```

   Require `USB HIL verified: 62 cases; hardware speed scope: 64-byte MPS`.
4. Only after that passes, add `--reset-test`, using a new log file such as
   `logs/usb-hil/full-speed-reset-20261006-01.log`. Require **64 cases at
   64-byte MPS**, then repeat without reset/reload/replug using a distinct log.
   On any failure, retain the log and leave the board unchanged for inspection.

If no controlled full-speed path is available, leave this gate **not tested**;
the two high-speed passes remain valid within their stated scope. This tests
the HS controller/driver operating at full speed, not the separate FS driver
component. Traffic/camera still use DFP 3.1.5; application migration to this
candidate and its SDS regression are separate, not implied by these HIL passes.
A controlled candidate traffic/SDS regression at high speed is the next
available application check; it requires an explicit pack-selection change
and application load, neither of which has been performed at this checkpoint.

## Initial offline baseline — 2026-10-05 (DFP 3.1.5)

- CMSIS target-set `NuMaker-X-M55M1D@usb-test`, project `usb-hil.Release`:
  **build PASS** (jobs b-7 and b-8; b-8 includes the 8 KiB stack).
- Build-artifact tools confirm `Driver_USBD1` comes from
  `Driver_USBD_HSUSBD.o`. Image load data: **30,816 bytes in internal flash**.
  Runtime placement: 8,192-byte DTCM stack, 22,496 bytes in internal SRAM,
  3,376 bytes of ITCM code; no external-memory payload. These are this build's
  measurements, not fixed size requirements.
- Complete harness compiled against the real CMSIS API headers with mocked
  driver callbacks, ASan and UBSan: **PASS**. Includes negative tests for
  corrupt data, incorrect counts, duplicate callbacks, guard/late-write
  corruption, abort failure/quarantine and API failures.
- **19 Python host/launch tests PASS**, including a simulated 62-case sequence
  at each packet size and launch rejection of foreign/external-memory images.
- Existing **33 traffic host tests** and **6 camera launch-guard tests PASS**.
- Project virtual environment now has **PyUSB 1.3.1**. Loading the existing
  `/opt/homebrew/lib/libusb-1.0.dylib` backend passed without opening/enumerating
  devices. No system packages or installed CMSIS packs were changed.
- **Not loaded or hardware-tested yet.** No board pause, reset, programming or
  USB traffic was performed during implementation. Hardware results must be
  recorded separately; mock tests do not qualify the packaged driver.

The CMSIS environment skill located the manifest's Toolbox 2.14.1 artifact
(its executable reports cbuild 2.14.0), CMake 4.2.1, Ninja 1.13.2, AC6 6.24.0,
GCC 14.3.1 and Clang 22.1.0 with process-local registrations. Actual target
builds used the VS Code CMSIS action; their runner reports csolution
2.15.1+p3-gf46d68bf and their ELF reports AC6 6.24. The diagnostic-log tool
returned an older camera log, so it was not treated as USB build evidence.

The initial build reported missing RTE files/dependencies. The test's five RTE
configuration sources were copied from DFP 3.1.5, not from the traffic layer;
only the local scatter file's default stack size changed to 8 KiB. The packaged
HS component defines `RTE_USBD1=1`; the app retains `NVT_VECTOR_ON_FLASH`.
The DFP's declared Common dependencies include both PDMA and LPPDMA even
though this USB driver uses CPU FIFO copies. No local USB driver copy is compiled.

## First hardware run

1. Close SDSIO servers. Leave J13 connected through the known-good data cable
   and leave the debugger cable connected. Stop the existing traffic debug
   session before launching this different image.
2. In CMSIS Manage Solution select **NuMaker-X-M55M1D**, target-set
   **usb-test**, and build if anything changed. The normal/default mapping
   remains `traffic.Release`; `camera-test` remains `camera-hil.Release`.
3. In Run and Debug choose **NuMaker USB HIL (launch)** and press F5.
   Do **not** use a traffic or camera launch. The prelaunch guard checks the
   device, DFP, target-set, exact ELF/HEX output pair, HEX checksums and every
   load-data address before preparing a dedicated runner. It refuses data
   outside internal flash. No HyperRAM loader is needed.
4. At `main()`, continue. Disable other breakpoints/logpoints. This deliberately
   replaces the traffic program in internal flash; source/configuration and
   traffic build outputs remain intact. No camera image is expected.
5. From the repository root, first check identification/configuration:

   ```sh
   .venv/bin/python board/NuMaker-X-M55M1D/tests/usb-hil/verify_usb.py --status-only --expect-speed high --libusb /opt/homebrew/lib/libusb-1.0.dylib
   ```

   Expect `CONNECTED (bulk tests not run)`, `configured: 1`, `mps: 512`, and
   `errors: 0`. If this fails, preserve the output; do not switch drivers or
   poke peripheral registers. A control/enumeration failure can also be a
   defect in this new test harness, not necessarily in the DFP.
6. Run the bounded transfer suite. Its complete output/JSON is saved
   automatically; Terminal shows the log path and final summary:

   ```sh
   .venv/bin/python board/NuMaker-X-M55M1D/tests/usb-hil/verify_usb.py --expect-speed high --libusb /opt/homebrew/lib/libusb-1.0.dylib
   ```

   Expected success: `USB HIL verified: 62 cases; hardware speed scope: 512-byte MPS`.
   A timeout, pattern mismatch or target error invalidates the run. Error bits
   stay latched until firmware reload; do not manually clear them to claim a pass.
7. Only after that passes, repeat with **`--reset-test`**. This additionally
   issues a real USB bus reset while an OUT transfer is incomplete, checks a
   new reset event, reconfigures and repeats an 8192-byte loopback. A pass
   reports 64 cases. This is USB bus reset, **not** MCU/debugger reset.

For other hosts, omit `--libusb` if library discovery works, or supply the
actual library path. The Python dependency is listed in
[requirements-host.txt](requirements-host.txt); a libusb-1.0 backend is also
required. The verifier never automatically detaches a kernel driver or chooses
between multiple matching devices. Close other applications claiming the test
interface. Do not run this verifier against SDS firmware.

## Automatic result logs

Every test invocation creates a new UTF-8 log under the repository's
`logs/usb-hil/` directory, named `<UTC timestamp>-<process id>.log`. The path is
printed before USB discovery and again beside the final summary. Complete
case results and JSON are written to the file, not dumped into Terminal.
This works with the existing commands; no shell redirection is required.

The log records arguments, Python/host information, verifier SHA-256, each
completed case and status, reset-test stages, and the final result. Failures
include exception type/message, libusb error codes when available, traceback,
active case and the **last successful status response**. That response is
historical, not an extra post-failure read from the device. Rediscovery failures
and cleanup errors are retained without masking the original failure.

After the optional reset, `RECONNECT_ATTEMPT`, `RECONNECT_ERROR` and
`RECONNECT_READY` entries retain each attempt, elapsed time, failing stage and
USB error codes. A single 10-second retry window covers rediscovery, identity
checking, configuration, interface claiming and status reading. Each failed
handle is disposed before rediscovery; the verifier never repeats `reset()`
or a bulk test to obtain a pass. Only absence / NO_DEVICE is retried. An
ambiguous identity, wrong protocol/invalid status, target failure bits, missing reset events,
unexpected callbacks, changed speed, timeout/stall/access/busy errors and
cleanup failures are not retryable. Initial connection failures are not retried.
The new status must show a reset counter advance, configured endpoints at the
same speed, no active transfer and no completion for abandoned work; the fresh
8192-byte loopback still checks all data/counts/callbacks. The deadline is
checked between synchronous libusb calls (configuration/claim cannot be
interrupted by this Python deadline); a late successful handshake is rejected.

Results are flushed during the run, including immediately before the reset
API call. Ctrl+C produces an interrupted result; abrupt process termination
may leave a partial log, but already-flushed results remain available. Logs
cannot recover output from runs made before this change.

To choose a filename explicitly, add for example:

```sh
--log-file logs/usb-hil/reset-check-01.log
```

The file must not already exist. If log creation fails, the verifier stops
**before any USB access**. Existing evidence is never overwritten. Logs are
Git-ignored but kept outside the generated `out/` directory so firmware builds
do not replace them. Share the complete log file rather than terminal excerpts.

## What the suite exercises

The single vendor-class interface has bulk OUT `0x01` and IN `0x81`. Endpoint
zero handles a minimal set of standard requests and the test commands below.
All descriptor responses fit one EP0 packet; address changes occur after the
status-IN callback, since the selected driver writes the address immediately.
The existing demo VID/PID `C251:8007` is reused **only for this local lab test**,
not a new product allocation. Product `NuMaker USB HIL`, serial `NU-USB-HIL-01`,
protocol magic and version are checked before configuring/claiming the device.
An independently allocated identity is needed before distribution as a product.

| Check | Scope |
|---|---|
| OUT, IN and loopback | 0, 1, 63/64/65, 127/128/129, 511/512/513, 1023/1024/1025, 8191/8192 bytes |
| OUT short termination | Capacity 8192; ZLP, MPS−1, exact MPS plus explicit ZLP, MPS+1 |
| Callback rearm | Two transfers in each direction; second armed by the completion callback with a new pattern seed |
| Abort/restart | Pending IN, pending OUT and partially received OUT; no completion allowed for aborted work |
| Unconfigure/reconfigure | Standard SET_CONFIGURATION 0 then 1, followed by full loopback |
| Optional bus reset | Partial OUT abandoned by reset, event observed, reconfiguration and fresh loopback |

The host checks every returned payload byte. The firmware validates received
patterns and untouched tails, 32-byte guards, returned lengths and callback
ownership. Completed/aborted buffers are fingerprinted and rechecked on later
control requests until reused; the host includes a 50 ms stability interval.
This is a finite check, not proof that arbitrarily late writes are impossible.
Reset discards the active transaction without immediate buffer reuse; reset
checks cover guards and unexpected callbacks, not retained whole-buffer
fingerprints across reset. Failed abort/transfer submission leaves the active
buffer quarantined and latches an error; it must not be resubmitted.

At 512-byte MPS, 64-byte transfers are short high-speed packets, **not full-speed
hardware validation**. A separately controlled full-speed connection and
`--expect-speed full` are needed for the 64-byte-MPS run. Do not force controller
registers to claim speed coverage. Cable-loss/reconnect, power lifecycle,
suspend/resume stress, partial-IN abort, long endurance, real USB bus timing and
full CMSIS-Driver/USB compliance remain additional qualification work. No
automatic SDS recovery claim follows from a standalone USB test.

## Protocol and inspection

`usb_hil_status` is also available to the debugger, but avoid stopping the CPU
while the host has an active transfer. There is no UART diagnostic output.
Vendor requests use device recipient, no OUT data stage:

| Request | Direction | Parameters |
|---|---|---|
| `0x30` status | IN | wValue/wIndex 0; returns 64 bytes |
| `0x31` arm OUT | OUT | wValue = capacity ≤8192; wIndex = seed, bit15 = rearm once |
| `0x32` arm IN | OUT | wValue = length ≤8192; same seed/rearm fields |
| `0x33` abort | OUT | wValue/wIndex 0; retires active transfers only on success |
| `0x34` loopback | OUT | wValue = capacity; wIndex = seed, no rearm flag |

Status layout is sixteen little-endian 32-bit words as named in `usb_hil.h`;
the final driver-error word is signed. Error bits: driver/API `0x01`, guards/tail
`0x02`, count `0x04`, pattern `0x08`, unexpected callback `0x10`, changed returned
buffer `0x20`. New test cases are refused while active or after any error.

Implementation basis: CMSIS 6.2.0 `Driver_USBD.h` API 2.3 and the original DFP
3.1.5 source; the current candidate changes the driver, not the HIL protocol.
Peripheral facts were checked with the documentation tools:
`user/nuvoton/numicrom55-dfp/en-us-trm-m55m1-series-en-rev1-02`, **Rev 1.02**, p.2755
§6.47.5.2 (packet validation), p.2777 (function address), p.2783 (status-stage
completion). No peripheral-register workaround is introduced by this harness.

## Offline checks and restoration

These commands do not access hardware:

```sh
.venv/bin/python board/NuMaker-X-M55M1D/tests/usb-hil/test_firmware.py
.venv/bin/python -m unittest discover -s board/NuMaker-X-M55M1D/tests/usb-hil -p 'test_*.py' -v
```

To restore traffic: stop HIL debugging, select the normal/default NuMaker
target-set, build, select **CMSIS_DAP@pyOCD (launch)**, perform the full HyperRAM
load and continue past `main()`. Do not attach traffic symbols to the HIL image
or use the old USB-HIL runner to load traffic. Repeat a short SDS recording/playback
before making any claim that traffic still works after the hardware test.
