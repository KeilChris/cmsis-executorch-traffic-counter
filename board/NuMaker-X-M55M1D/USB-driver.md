# NuMaker packaged HSUSBD driver and SDS transport

The board now selects `CMSIS Driver:USB Device:M55M1_M5531_HS` from locally
prepared `Nuvoton::NuMicroM55_DFP@3.1.5`. It provides `Driver_USBD1` for MDK USB
device 0 on J13. The former `Driver_USBD_HSUSBD_fixed.c` override was removed.
The user confirmed the packaged driver works (2026-10-02). The source review
bundle is preserved in [pack-work](../../pack-work/Nuvoton.NuMicroM55_DFP/README.md).

Do not add the former custom USB component/source alongside the packaged driver:
that would compile two definitions of `Driver_USBD1` and the interrupt handler.
`Device:StdDriver:HSUSBD` remains selected for the peripheral support code.

The sections below retain the original correction/validation history. References
to the local driver describe that earlier implementation, now packaged in DFP
3.1.5. For IN/OUT test commands, supply `--driver /path/to/DFP/3.1.5/Library/CMSIS/Driver/Source/Driver_USBD/Driver_USBD_HSUSBD.c`;
the default removed override is no longer a valid test input. The project-local
SDS transport is still required and is a separate upstream change.

The latest packaged-driver regression (2026-10-05, replacement J13 cable) passed
86/86 recording and repeat playback, plus a 120-second recording with 338/338
pairs and playback in 78.7 s (4.29 fps). Offline checks confirm sizes and ordered
timeslot pairing. A separate fresh-folder startup recheck passed 15/15 after a
fresh restart with a complete server log. The earlier empty attempt is still
unexplained; this recheck qualifies one run, not every startup. See the
[dated validation record](Validation.md#replacement-cable-regression--2026-10-05).

The separate [USB-only HIL project](tests/usb-hil/README.md) now exercises the
packaged API without SDS, camera or middleware. Host checks and target builds
pass; on-board execution remains pending. Do not confuse its test coverage with
successful hardware qualification.

## Historical application baseline (2026-10-02, before pack migration)

The local IN/OUT corrections and SDS transport changes below are retained.
With serialized camera capture, the user reported 15/15 paired recording
frames, two 15/15 playbacks without reset (4.4 s each), then a 30-second
recording with 86/86 pairs and playback of all 86 in 20.4 s (4.23 frames/s).
All those sessions closed cleanly without a fatal protocol error.
See [Validation.md](Validation.md) for the configuration, exact evidence scope
and repeat procedure. This is application-level hardware validation, not USB
compliance or proof of reconnect/long-duration reliability.

## Packet-completion-driven IN transfers

Before the IN correction, with serialized camera capture enabled, enumeration
was recovered by a
software detach/reattach, but the subsequent recording failed during transfer
with a server protocol mismatch (before Stop). The failed files remain in
`recordings/traffic/camera-serial-check/`; do not use that pair for validation.
The input file contains 14 structurally complete frames. Comparing the saved
detection bytes with the still-intact target `sds_out_buf` established an exact
512-byte gap: the file equals target bytes `[0,4392)` followed by `[4904,5712)`.
This establishes corruption in the transfer path, not its precise root cause.
The later incomplete IN transfer (3584/8192 bytes) may be a consequence of the
server stopping; it does not by itself identify the initial failure.

The retained local driver allows one outstanding non-control IN packet:

- `BUFEMPTY` permits filling one packet but does not advance the transferred
  count. `num_pending` and `in_packet_pending` track that packet separately.
- Only `TXPK` advances `num_transferred_total`. The next FIFO fill is enabled
  after that event, and the middleware receives one completion for the entire
  transfer, after its final packet.
- Old `TXPK` status is cleared before filling the FIFO, so a completion during
  the fill is preserved. Completion disables the endpoint interrupt before
  notifying the middleware, allowing a callback to rearm it safely.
- Aborting an IN transfer disables its interrupt and retires its pending state
  before flushing. Explicit zero-length transfers use `ZEROLEN`; nonzero short
  packets retain `SHORTTXEN`.

The register basis is `user/nuvoton/numicrom55-dfp/en-us-trm-m55m1-series-en-rev1-02`,
M55M1 TRM Rev 1.02: p.2755 §6.47.5.2 auto-validation, p.2800 `TXPKIF`, p.2801
`BUFEMPTYIF`, and p.2808 `SHORTTXEN`/`ZEROLEN`. The distinction between FIFO
availability and packet transmission motivated this correction. Subsequent
hardware sessions passed, but the exact wire-level cause of the missing packet
has not been isolated.
That original IN correction left enumeration, control-endpoint handling, the
OUT correction, SDS transport, camera serialization and installed packs unchanged.

Run `python3 board/NuMaker-X-M55M1D/tests/test_usbd_in.py --driver /path/to/DFP/3.1.5/Library/CMSIS/Driver/Source/Driver_USBD/Driver_USBD_HSUSBD.c` for host-only tests of
the actual transmit, result, abort and IRQ-dispatch source. The mocked W1C
register/FIFO checks 64/512-byte packets, multi-packet and short transfers, ZLP,
FIFO-empty indications before completion, completion during FIFO fill, stale
events, abort/restart, callback rearm and a missing callback. It runs with
address/undefined-behavior sanitizers. The original driver fails the assertion
that queued-but-uncompleted bytes must not yet appear in the transfer result;
the retained path passes. This is not a PHY or USB-bus timing test.

To test a changed firmware image, perform a full debugger reload, continue past
`main`, and run without per-frame/per-packet breakpoints. After a fatal protocol
error the old SDS state is invalid and also requires a reload before retrying:

```sh
.venv/bin/python traffic/sds_session.py record 5 --transport usb --workdir recordings/traffic/camera-usb-in-check
.venv/bin/python traffic/sds_session.py play --transport usb --workdir recordings/traffic/camera-usb-in-check
```

Run playback only if recording reports all frames paired and no fatal error.
Use a fresh work directory if this test's directory already contains a
failed recording. Check that the camera remains streak-free, both streams close,
and repeated recording/playback passes. The extra interrupt per packet may
reduce throughput; compare recording frame counts and playback elapsed time.
Do not claim a successful hardware result from the host tests or build alone.

## Receive correction

The original `USBD_DataOutStage` copies each packet to the start of the
destination and signals OUT completion after every packet. It leaves receive
interrupts enabled after completion. Live playback failed in `sdsReadHandler`
with `SDS_ERROR_IO`; the reported received count exceeded the requested buffer
size and the response header had been overwritten by image data. Reducing SDS
transfer sizes did not resolve the receive-buffer ownership problem.

The local handler appends packets at `num_transferred_total`, bounds copies by
the requested capacity, and completes on the requested length or a short packet
(including a zero-length packet). It disables RX packet interrupts before
notifying the middleware. The interrupt dispatcher checks the enabled RX event
before calling the handler. SDS buffers are restored to 8192 bytes.

These transfers use CPU FIFO copies, not USB DMA; adding cache invalidation
would not address this receive-completion defect.

## SDS USB completion scheduling

The board also selects `SDS:IO:Custom` and supplies `sdsio_client_usb_mdk.c`,
derived from ARM::SDS 3.1.0's MDK USB transport. The generic SDS client and
stream implementation still come from the unmodified pack. The local transport
uses the existing `RTE/SDS/sdsio_client_usb_mdk_config.h`.

In the original transport, the high-priority USB OUT callback calls
`USBD_EndpointReadGetResult`. An immediate completion can preempt the caller
inside `USBD_EndpointRead`, before MDK USB releases its endpoint semaphore.
The callback then enters `USBD_DriverEndpointTransferGetResult`'s 100 ms retry
delay. This stack was observed on hardware during slow playback.

The local callback only sets an event. The serialized SDS receiving thread
waits for that event and collects the result after `USBD_EndpointRead` returns,
so it cannot preempt itself while holding the semaphore. That thread alone
updates the receive-buffer counters. Pending completions are preserved across
rearm, and zero-length packets rearm reception. No scheduler or interrupt lock,
middleware retry constant change, or increased timeout is needed.

This scheduling correction is an SDS transport integration fix, separate from
the DFP's multi-packet receive defect above. It should be proposed upstream to
SDS, not presented as another Nuvoton peripheral-register defect.

## Waiting for USB configuration

The local transport keeps USB initialized and attached while waiting for the
host to configure it. Previously the three-second data-transfer timeout was
also used for enumeration, after which `rec_play.c` uninitialized USB and
retried. That cycle can interrupt host discovery/configuration; removing it
does not yet prove the cause of this hardware's enumeration failure. Only the
SDS control thread waits; camera, inference and display continue.
Actual initialization/allocation errors still return to the existing cleanup
and retry path. Send/receive timeouts remain unchanged.

`python3 board/NuMaker-X-M55M1D/tests/test_sds_usb_init.py` tests the actual
initializer with a virtual delayed host (ten seconds), immediate configuration,
tick wrap and allocation/initialize/connect failures. The original pack
initializer fails the delayed-host case; the local version passes. The final
hardware sessions connected successfully with this change retained, but they
do not establish the earlier enumeration failure's root cause or validate
automatic disconnect/reconnect recovery.

## Validation and upstream work

Run `python3 board/NuMaker-X-M55M1D/tests/test_usbd_out.py --driver /path/to/DFP/3.1.5/Library/CMSIS/Driver/Source/Driver_USBD/Driver_USBD_HSUSBD.c` for host tests of the
actual receive-handler source with mocked FIFO/registers. They cover packet
assembly, short packets, zero-length packets, buffer bounds and callback rearm.
They do not validate hardware FIFO/interrupt timing. Earlier hardware playback
passed with all 23/23 detection records. Recording after playback then exposed
a stop-boundary pairing issue (19 inputs, 20 results), addressed separately
below. The later paired hardware results supersede that earlier validation
stage. Automatic disconnect/reconnect recovery remains unqualified.

`python3 board/NuMaker-X-M55M1D/tests/test_sds_usb_receive.py` exercises the
actual callback/receiver with a mock that delivers completion before the
middleware releases its semaphore. The original pack source fails the lock
assertion; the local source passes. The tests also cover buffered tails,
nonblocking polls, rearm, zero-length packets, partial reads and errors.

`python3 -m unittest discover -s traffic/tests -v` tests host-side playback
validation. `traffic/sds_session.py` now checks record counts, payload lengths,
fresh output and timeslot ordering, then prints elapsed playback time. A clean
stream close alone is not success: the previous hardware run closed both
streams but returned only 3 of 23 expected detection records in roughly a
minute. The subsequent scheduling-fix tests returned all 23 records.

The recording-stop correction gates detection writes on acceptance of the
corresponding input. After an accepted input, a result write may finish after
START clears; it still aborts on link loss or a bounded full-buffer timeout.
`traffic/tests/test_record_pairing.py` compiles the actual application recording
blocks and writer with simulated Stop, buffer backpressure and link loss. The
host recording task also validates counts and ordered timeslots. The final
15/15 and 86/86 recordings passed these checks with the correction retained;
the 86/86 run also exercised recording after playback without a reset.

### Earlier failures and diagnostic evidence

The following failed runs predate the successful baseline. Their files and
observations are retained as debugging evidence, not as validation inputs.

The next recording attempt never connected a USB client: the old host helper
sent `R`, waited five seconds, and sent `S` while the server was still waiting
for the device. That run did not exercise the firmware pairing correction.
The helper now requires a live flags exchange before `R`/`P`, starts the record
timer after both streams open, and waits for both closes before validation.
Offline virtual-clock tests cover delayed/missing connection, stream startup
and drain, server exit, and playback startup/completion. A connection timeout
now reports a connection error without sending `R`/`P`.

The subsequent `CameraIn.2.sds` run connected, but failed near Stop with a
server protocol mismatch: 20 saved inputs and 14 saved results. Live inspection
afterwards found 21 input records admitted (`cnt_in=10902696`) and 21 results
admitted (`cnt_in=8568`), so application admission was paired in that run.
The IN endpoint still referenced `sdsDataBlockBuf` for an 8192-byte transfer
with a reported count of 3584, while that shared buffer already contained
result records. Both SDS streams remained allocated with HALT set after close
failed. This establishes an incomplete-send/buffer-lifetime problem, but not
whether the initial timeout preceded the server's framing error. The later
serialized-camera investigation isolated a 512-byte gap, described above.
The host helper now rejects fatal server errors even when cleanup logs both
stream closes. Those failed recordings are not successful validation of
recording-stop behavior; the later passing sessions are listed at the top.

Run throughput tests without breakpoints or logpoints in per-packet/per-frame
paths. A conditional breakpoint still halts through the probe on every hit.

For the DFP, the IN/OUT and application-RTE corrections have been migrated into
the locally prepared DFP 3.1.5 HS USB Device component. The application already
selects it and no longer compiles a local USB driver. Upstream review and
driver-only hardware qualification remain separate work: test full-speed
(64-byte) and high-speed (512-byte) boundaries, abort/reset, disconnect and
rearming. Successful SDS sessions alone do not establish those API contracts.

For the BSP, keep J13 instance selection, PHY reference clock, memory placement,
camera and display wiring in the board layer. Expose standard peripheral access
through CMSIS-Drivers where an API exists; camera/display operations can remain
documented board interfaces backed by those drivers. Provide a USB SDS
record/playback example as an integration regression test.

The concrete ownership, implementation steps and acceptance criteria are in
[Pack-improvements.md](Pack-improvements.md). Installed packs, firmware and
configuration remain unchanged by this documentation consolidation. The
project-local SDS transport is still required; packaging the USB controller
driver did not replace that separate integration correction.

A [DFP source review bundle](../../pack-work/Nuvoton.NuMicroM55_DFP/README.md)
preserves the original IN/OUT changes and portable RTE configuration selection.
Its source snapshot is not a complete DFP. The transfer tests accept
`--driver PATH` for that snapshot or the selected packaged driver. Its archived
patch verifier still references the removed override; see the bundle README
before trying to reproduce that historical check.
