# NuMaker DFP/BSP improvement plan

Completed migrations and proposed upstream work following the
[working validation record, updated 2026-10-06](Validation.md).
This document does not apply pack changes. The USB controller driver is now
packaged; retain the separate project-local SDS transport and application
capture/pairing policy until their replacements pass the same regression.

The first USB migration deliverable is now available as a
[source-only review bundle](../../pack-work/Nuvoton.NuMicroM55_DFP/README.md):
staged driver, provenance, unified patch and passing host checks. The user has
since packaged the USB changes in DFP 3.1.5, selected its HS component and
reported that USB still works. Reconnect/endurance qualification remains open.

The [BSP audit and source bundle](../../pack-work/Nuvoton.NuMicro_M55M1_BSP/README.md)
preserves the original additions. Release preparation was then applied in the
user's BSP workspace; the baseline used BSP 3.1.4 / DFP 3.1.5 and the
user reports that the migration works. The older pack identifiers below refer
to the original audit, not the current selections.

The selected [camera-hardening candidate](../../pack-work/Nuvoton.NuMicro_M55M1_BSP/camera-hardening/README.md)
adds checked sensor initialization and a bounded frame-boundary abort contract.
It is packaged as BSP **3.1.5-rc.1** / Camera **1.2.0**, selected through a local
pack path and target-built with DFP 3.1.5. PackChk passes with no errors/warnings.
It leaves the installed working pack intact. Debugger loading and a live camera
smoke check now pass, as does the bounded candidate SDS regression (85/85 frames
through recording, repeat playback and recording afterward). Subsequent
replacement-cable tests passed 86/86 recording/repeat playback and 338/338
recording/playback. Camera HIL capture/ownership, graceful abort, zero-budget
quarantine, physical sensor absence and full-reload recovery checks also passed;
remaining physical-fault qualification is open. The separate fresh-folder
startup recheck passed 15/15 after a fresh restart; the earlier empty attempt
remains unexplained. See the dated validation record for evidence limits.

## USB-only HIL — bounded high-speed checks passed; full speed skipped

The separate [USB HIL project](tests/usb-hil/README.md) and host verifier are
implemented and target-built, without camera, model, SDS framing or middleware.
The normal traffic mapping is preserved. A separate abort-hardening candidate
DFP **3.1.6-rc.1** / HS component **1.2.0**, selected for USB HIL only, passed
**64/64 cases twice at 512-byte MPS**, including the reset extension. The
host-only reconnection fix recovered the observed NO_DEVICE failure on the
first run; the repeat also passed. The
[dated evidence](../../pack-work/Nuvoton.NuMicroM55_DFP/abort-hardening/integration-20261006.md#hardware-retest--pass-at-high-speed)
retains hashes and scope. Full-speed hardware testing was intentionally skipped
at the user's request on 2026-10-06; qualification remains high-speed-only.
Traffic and camera still use DFP 3.1.5; a controlled candidate application
regression at high speed is the next proposed step, not yet applied.
The qualification requirements below remain the broader acceptance gate:
use deterministic data
and guarded buffers; check returned lengths, exactly one completion per
transfer, rearming and ownership after abort/reset/disconnect. Exercise zero,
short, exact and multi-packet transfers around both 64-byte and 512-byte packet
sizes, including 8192-byte transfers. Full-speed and high-speed need separate
hardware evidence. A driver reset/disconnect test does not establish automatic
SDS session recovery.

The fresh-directory startup recheck now passed for one run with a complete
server log. Wider startup/reconnect reliability still needs repeated hardware
evidence; it does not explain the old empty recording retrospectively.

## What already exists, and what needs a different interface

The inspected **locally improved** `NuMicroM55_DFP/3.1.4-rc.5` PDSC already
declares CMSIS-Drivers for CAN, Ethernet MAC, Flash, GPIO, I2C, MCI, SAI, SPI,
USART, USB Device and USB Host. Its USB Device selection is split into
`M55M1_M5531_FS` and `M55M1_M5531_HS`. These are not all missing drivers;
presence in the PDSC also does not establish correct behavior or conformance.

The inspected `NuMicro_M55M1_BSP/3.1.3` PDSC provides CMSIS VIO and Board Support
components for HyperRAM, `NuMaker-TFT-LCD5&LT7381` and `CMOS-720P&HM1055`, plus a
reusable board layer. Those workspaces include local changes and must not be
confused with pristine published releases bearing the same version identifiers.

The local `ARM::CMSIS@6.2.0` `CMSIS/Driver/Include` inventory defines standard
USB Device, I2C, GPIO and SPI interfaces, but no camera, display, CCAP, EBI or
general DMA interface. A vendor capture/display API can be reusable and well
specified without being mislabelled a standard `ARM_DRIVER_CAMERA` or
`ARM_DRIVER_DISPLAY`.

| Function | Recommended owner and interface |
|---|---|
| J13 USB controller | DFP: existing standard `ARM_DRIVER_USBD`, instance `Driver_USBD1` |
| Sensor register/control pins | BSP sensor layer over standard I2C/GPIO where routing permits |
| CCAP capture engine | DFP reusable, explicitly vendor-specific capture interface |
| HM1055 modes, reset/power pins, module wiring | BSP `Board Support:Camera` |
| LT7381 commands and panel rendering | BSP `Board Support:Display` over the actual EBI/PDMA transport |
| HyperRAM and board startup wiring | BSP memory/board layer, using DFP peripheral support |
| USB SDS thread scheduling | ARM SDS transport, not a Nuvoton register-driver feature |
| Capture/inference/display serialization | Application policy; retain until safe concurrency is demonstrated |

## 1. DFP: ship a validated standard USB Device driver — highest priority

Starting evidence: the [preserved driver candidate](../../pack-work/Nuvoton.NuMicroM55_DFP/Library/CMSIS/Driver/Source/Driver_USBD/Driver_USBD_HSUSBD.c),
[USB-driver.md](USB-driver.md), and the IN/OUT host tests in [tests](tests).

Items 1–3 describe the corrections already migrated into the locally prepared
DFP 3.1.5. They remain upstream review requirements, not pending changes to the
working traffic image. The lifecycle audit and independent HIL work in 4–5
remain to be completed.

1. Merge the OUT packet-assembly correction into the packaged HSUSBD driver:
   append at the current offset, enforce requested bounds, complete at the
   requested length or short packet, and mask the completed receive event
   before invoking the callback. Preserve safe callback rearming.
2. Review and merge the IN completion state machine: distinguish queued bytes
   from transmitted bytes, keep one packet pending, account on transmission
   completion, and notify once per completed transfer. Retain short/ZLP handling,
   stale-event handling, and abort cleanup before buffer reuse. Do not increase
   timeouts to disguise incomplete transfers.
3. Keep independent FS/HS component selection, a single owner of each IRQ and
   exported driver instance, and application-selected RTE configuration rather
   than silently including driver-directory defaults. Expose instance/clock
   choices in configuration; keep board J13 wiring in the BSP layer.
4. Audit `Initialize`/`PowerControl`/`Uninitialize`, transfer result, abort,
   endpoint unconfiguration, bus-reset and disconnect behavior against the
   standard API contract. Make peripheral clock/PHY lifecycle self-contained
   given documented board clock prerequisites. A register write from the
   application/debugger must not be the normal USB startup requirement.
5. Add a driver-only bulk loopback/boundary example and an MDK USB integration
   example, so failures can be reproduced without the model, camera or SDS.

Acceptance: retain the host regression tests, then test on hardware at both
64-byte and 512-byte packet boundaries: zero, short, exact-packet, multi-packet
and 8192-byte transfers, callback rearm, abort/restart, unconfigure, reset and
cable loss during active traffic. Check data patterns, returned counts, one
completion per transfer and no writes after ownership returns to the caller.
Repeat the SDS regression and long-duration/reconnect tests. Full-speed mock
coverage is not full-speed hardware validation. Run applicable CMSIS-Driver
validation tests as well; SDS success alone is not conformance certification.

Register evidence for the IN review: M55M1 TRM Rev 1.02 (2026-08-20), document
`user/nuvoton/numicrom55-dfp/en-us-trm-m55m1-series-en-rev1-02`, p.2800
(`TXPKIF` packet transmitted), p.2801 (`BUFEMPTYIF` local FIFO availability),
p.2808 (short packet, ZLP and flush controls). These distinctions support the
state-machine review; they do not prove the historical packet-loss mechanism.

## 2. BSP camera: separate sensor control from capture mechanics

Starting sources, relative to the BSP: `Board/NuMaker-X-M55M1D/Camera/ImageSensor.c`
and `Camera/Sensor/{Sensor_HM1055.c,SWI2C.c}`. The current sensor code bit-bangs
PH2/PH3 through SWI2C; it does not currently use `Driver_I2C`.

1. Keep HM1055 register tables, supported pixel formats/modes and module
   reset/power sequence in the BSP. Put bus access behind a small sensor-control
   adapter that returns read/write failures instead of ignoring them.
2. Verify connector routing and available pin multiplexing against the board
   manual before selecting a hardware I2C instance. If usable, select the
   existing standard DFP I2C driver and declare that dependency. Do not assume
   that PH2/PH3 can be switched to a suitable hardware controller without checking.
   If software I2C remains necessary, use a documented GPIO-backed adapter;
   only advertise an I2C CMSIS-Driver implementation if its contract is implemented
   and tested. Handle reset/power GPIO through the existing GPIO driver where
   suitable; pin multiplexing and module clock setup remain device/board-specific.
3. Separate CCAP setup/IRQ/buffer state from HM1055 configuration into a reusable
   device capture layer. Specify capabilities, dimensions/stride, supported
   memory regions/alignment, cache ownership, start, completion/error callbacks,
   status and abort. Exactly one component owns the CCAP IRQ.
4. Preserve the added frame/error callbacks and make timeout/abort quiesce the
   hardware before a buffer can be reused. Define overflow detection/recovery
   explicitly: `camera_errors=0` alone did not exclude FIFO overflow in the
   investigation. Document callback context; do not put RTOS waits or diagnostic
   printing inside the capture IRQ.
5. Supply a minimal capture example without inference/display, then a documented
   combined camera/display example. Keep the traffic application's serialized
   policy rather than promising concurrent HyperRAM traffic is safe.

Acceptance: missing sensor/NACK and capture timeout return bounded failures;
capture can be aborted and restarted; no late write corrupts a returned buffer;
pattern/scene recordings are streak-free with correct format and stride. Test
the declared cache policy and memory placement, and inspect overflow/error
status under load. Optimize concurrency only as a separate measured experiment.

## 3. BSP LCD: complete component with an explicit transfer contract

Starting sources: `Board/NuMaker-X-M55M1D/Display/Display.c`,
`Display/LCD/LCD_LT7381.c`, `Display/drv_pdma.c`; project adapter
[board_display.c](board_display.c).

1. Package the full display implementation, including controller initialization,
   board EBI pin setup and required PDMA support; retain dependency declarations
   and configuration for the 800×480 RGB565 module. The inspected local BSP
   already contains these sources, so upstream the changes rather than adding
   a duplicate application implementation.
2. Define whether each draw call is synchronous or asynchronous and when the
   source buffer is released. The current application adapter relies on the
   polling path; an asynchronous replacement needs actual completion/error
   reporting, not a `display_wait_shown` stub that always succeeds.
3. Add bounded controller/FIFO/PDMA waits, clipping/length checks, meaningful
   errors and documented cache/DMA buffer rules. Ensure missing/unresponsive
   display hardware cannot leave the application in an endless busy loop.
4. Keep this as a board display API. The LT7381 path here is EBI, not SPI;
   selecting `Driver_SPI` solely to obtain a CMSIS label is not an equivalent
   implementation. Use standard GPIO for suitable control pins and reusable
   vendor EBI/PDMA support for pixel transport.

Acceptance: color bars, edge/stride tests, full-screen and clipped rectangles,
buffer lifetime, missing-panel timeout and concurrent USB operation. Repeat
the serialized camera quality and SDS tests after changing bus scheduling.

## 4. BSP/DFP packaging and board initialization

1. Preserve the local pack changes as reviewable source changes, bump component
   and pack versions appropriately, and publish release notes with source
   revisions. Install the resulting packs into a clean pack root and reproduce
   the board build: the current modified cache is not a release artifact.
2. Supply a self-contained Board layer declaring required clocks, pin routes,
   components, startup hooks, RTE settings and memory regions. Include camera,
   LCD and USB examples with only their declared dependencies; test combinations
   for duplicate IRQs/driver symbols and pin/DMA-channel conflicts.
3. Keep HyperRAM initialization before C runtime initialization, plus the
   board-specific debugger loader and its source/build recipe. Describe the
   volatile-memory constraint prominently. Do not present memory-mapped HyperRAM
   as ordinary SPI transfers or claim standalone boot from this RAM-loader flow.
4. Review the DFP vector-table placement/alignment issue behind this project's
   `NVT_VECTOR_ON_FLASH` workaround and provide a tested startup/linker correction.
   Keep that review separate from USB/camera changes; do not remove the working
   workaround without reset/exception/RTX validation.

Acceptance: build and debugger-load from a clean checkout and clean versioned
packs, with no pack-cache edits or manual register pokes. Verify startup, RTOS,
HyperRAM, camera, display and J13 using the documented board layer. Persistent
standalone boot remains a separate future deliverable.

## 5. ARM SDS integration — separate upstream owner

Propose the [local USB transport](sdsio_client_usb_mdk.c) changes to ARM SDS:
the USB OUT callback signals an event, while the receiving thread collects the
result after the read API returns. This avoids the observed middleware semaphore
preemption/retry path. Preserve pending completions, buffered tails and ZLP rearm.
Keep USB attached while waiting for host configuration; do not reuse the short
data-transfer deadline as an enumeration deadline.

Use `tests/test_sds_usb_receive.py` and `tests/test_sds_usb_init.py` as regressions.
Review transfer-timeout cleanup and buffer ownership independently from the
Nuvoton driver. Keep generic ARM SDS pack sources untouched in this project;
the local transport is the reviewable override. Host session validation and
application frame pairing stay in the traffic example, not the peripheral DFP.

## Migration order

USB controller packaging and the initial BSP migration are complete locally;
the SDS transport remains project-local and camera hardening remains a candidate.
Next qualify the USB driver independently and close the remaining camera/pack
release gates, then prepare upstream submissions with explicit contracts and examples.
For each new pack version, remove only the corresponding local override, select
the replacement component, and repeat host tests plus the hardware matrix in
[Validation.md](Validation.md). Never compile both USB driver implementations.
Keep the baseline unchanged until its replacement passes; improve throughput,
reconnect recovery and persistent boot in separately scoped follow-up work.
