# NuMaker application validation — updated 2026-10-05

The camera-quality and USB SDS fixes are retained as the working application
baseline. The hardware results below were reported by the user on the physical
board; host tests are separate evidence. This is not general driver conformance
or production qualification.

Current test configuration: local BSP **3.1.5-rc.1** / Camera **1.2.0** with
packaged DFP **3.1.5**, retaining BSP **3.1.4** for rollback. The latest
[replacement-cable regression](#replacement-cable-regression--2026-10-05)
passes recording and repeat playback. The fresh-folder startup recheck below
also passed for that run. This does not promote the BSP candidate to a fully
qualified release.

## Configuration to retain

- NuMaker-X-M55M1D / M55M1H2LJAE, NuMaker-TFT-LCD5 V1.2 (LT7381),
  CMOS-720P V1.0 (HM1055), macOS host and J13 high-speed target USB.
- `traffic.Release+NuMaker-X-M55M1D`, Arm Compiler 6, RTX5 and Ethos-U55-256.
- `TRAFFIC_CAMERA_SERIAL: 1` in [Board-Traffic.clayer.yml](Board-Traffic.clayer.yml).
  Capture waits for the previous LCD transfer, then finishes before inference
  and the next LCD transfer. No background camera worker is created. During
  SDS playback, frames come from USB without real camera captures.
- Original 55 MHz sensor clock; the approximately 36.7 MHz experiment did not
  improve the streaks. Keep the existing crop/scaler/stride and memory placement.
- Packaged DFP 3.1.5 `CMSIS Driver:USB Device:M55M1_M5531_HS` supplies `Driver_USBD1`.
  Retain bounded multi-packet OUT assembly and the one-packet-at-a-time IN
  completion path. Do not also compile a local USB driver implementation.
- Project-local [sdsio_client_usb_mdk.c](sdsio_client_usb_mdk.c), selected through
  `SDS:IO:Custom`, defers OUT result collection to the receiving thread and keeps
  USB attached while waiting for configuration. SDS transfer buffers remain
  8192 bytes. Generic SDS stream/client sources remain in the unmodified ARM pack.
- Keep application input/result pairing and the host session helper's live-client,
  fatal-error, payload-length, record-count and ordered-timeslot checks.

The preserved baseline uses locally prepared BSP **3.1.4** and DFP **3.1.5**. The user
reported that the packaged USB driver works, then that the BSP migration works
(2026-10-02). Keep the exact development-pack artifacts and source patches;
this does not establish that a public download with those versions is identical.
The historical results below predate the migration and must not be relabelled
as measurements of the new packs. The initial post-migration confirmation
supplied no new frame counts or timings; the subsequent camera candidate's
results are recorded separately below. The original sources/provenance remain in the
[DFP](../../pack-work/Nuvoton.NuMicroM55_DFP/README.md) and
[BSP](../../pack-work/Nuvoton.NuMicro_M55M1_BSP/README.md) review bundles.

The [camera-hardening candidate](../../pack-work/Nuvoton.NuMicro_M55M1_BSP/camera-hardening/README.md)
is separate from the preserved BSP 3.1.4 baseline. The project now
selects its local BSP **3.1.5-rc.1** / Camera **1.2.0** for qualification:
pack validation, the target build and debugger loading are complete. Live
camera counters advanced from **39 to 125**, with **zero camera errors**;
initialization and the last detector status were OK, and camera fault was false.
The user confirms the new image remains streak-free (2026-10-02). See the
[candidate checks](../../pack-work/Nuvoton.NuMicro_M55M1_BSP/camera-hardening/CHECKS.md).
Candidate SDS normal-operation checks have also passed (below). Retain 3.1.4
for rollback while candidate failure/recovery tests remain open; the historical
quantitative SDS results do not qualify the candidate.

## Historical baseline hardware results

| Test | Observed result | Evidence |
|---|---|---|
| Camera quality with serialized capture | Horizontal streaks completely gone | User's visual confirmation, 2026-10-02 |
| Capture status after 20 completed frames | No camera/detector error; FIFO overflow flag clear at inspection | Earlier live-debug observations in [camera notes](Camera-experiment.md) |
| 5 s USB recording | 15/15 paired input/result records | `camera-usb-in-check`, user server log |
| Playback of that recording | 15/15 in 4.4 s, reported 3.42 frames/s | Same directory, user server log |
| Repeat playback without reset | 15/15 in 4.4 s, reported 3.42 frames/s | Same directory, second user server log |
| 30 s recording after playback | 86/86 paired input/result records | `camera-usb-soak-30s`, user server log |
| Playback of the 30 s recording | 86/86 in 20.4 s, reported 4.23 frames/s | Same directory, user server log |

Directories are under `recordings/traffic/`. Every listed SDS run closed both
streams without a fatal protocol error. The helper also verified record sizes
and matching ordered timeslots. Camera records contain 519168 payload bytes
(416 × 416 × 3); detection records contain 400 payload bytes.

Rates are the helper's end-to-end playback measurements, not isolated NPU time
or camera frame rate. Printed elapsed time is rounded, so dividing by it may
differ slightly from the printed rate. The historical 4.48 fps live-loop figure
predates serialization. Pair verification does not compare detection values
against a host model, establish identical live/playback inference, or check
every image pixel. The image-quality conclusion comes from the separate visual
comparison, not the SDS record counts.

## Candidate BSP 3.1.5-rc.1 normal-operation regression

User-reported on 2026-10-02, in response to the requested sequence without a
reset between operations:

| Test | Result | Directory under `recordings/traffic/` |
|---|---|---|
| 30 s recording | 85/85 paired frames | `camera-rc1-30s` |
| Playback | 85/85 in 20.2 s (4.22 frames/s) | `camera-rc1-30s` |
| Repeat playback | 85/85 in 20.2 s (4.22 frames/s) | `camera-rc1-30s` |
| 30 s recording after playback | 85/85 paired frames | `camera-rc1-after-playback` |
| Live image after final recording | Streak-free | User visual confirmation |

Offline parsing independently confirmed payload lengths and ordered timeslot
pairing for both recordings, the current playback output and its retained
`.bak` predecessor. Timing comes from the user's helper output, not this offline
check; the files do not establish whether a reset occurred. This completes the
bounded normal-operation regression, not failure-path/abort ownership testing,
reconnect/endurance qualification or general CMSIS-Driver conformance. Later
camera HIL results in the candidate checks separately cover normal ownership,
graceful abort, zero-budget timeout quarantine, physical sensor absence and
full debugger-reload recovery; remaining physical-fault cases are still open.

## Replacement-cable regression — 2026-10-05

The user suspected a faulty J13 cable after the recent enumeration/disconnect
failures and reran the unchanged traffic firmware with a replacement cable.
The requested sequence was recording → playback → repeat playback → a longer
recording → playback, without intervening reset/reconnection, then a separate
full debugger reload and startup recording. The user supplied these verification
lines; durations and playback rates were not independently remeasured:

| Test | User-reported result | Saved files under `recordings/traffic/` |
|---|---|---|
| 30 s recording | 86/86 paired frames | `cable-check-20261005`, index 0 |
| Playback | 86/86 in 20.6 s (4.18 frames/s) | Same directory, `Detections.0.p.sds.bak` retained after repeat |
| Repeat playback | 86/86 in 20.6 s (4.18 frames/s) | Same directory, `Detections.0.p.sds` |
| 120 s recording after playback | 338/338 paired frames | `cable-soak-120s-20261005`, index 0 |
| Playback of the 120 s recording | 338/338 in 78.7 s (4.29 frames/s) | Same directory, `Detections.0.p.sds` |
| Requested 5 s startup recording | 15/15 paired frames | `cable-startup-20261005`, **index 1**, qualification below |

**Startup qualification:** offline inspection found empty `CameraIn.0.sds`
and `Detections.0.sds` preceding the valid index-1 pair. The successful 15/15
recording is confirmed, but these files and the supplied verification line do
not establish first-attempt enumeration after reload, or whether reset/replug
was needed between attempts. The user cannot recall the first attempt, so its
cause and recovery sequence remain unknown. The separate fresh-folder recheck
below supplies new startup evidence, not an explanation of that failure. Preserve the empty files
as evidence. This folder is not a ready playback reference: the helper selects
`CameraIn.0.sds`, which is empty.

On 2026-10-05, independent **offline** parsing with the unchanged
`traffic/sds_session.py::sds_timeslots` confirmed all six nonempty input/output
comparisons in the table, including both saved 86-frame playback outputs.
Every comparison has matching record counts, correct 519168-byte camera and
400-byte detection payload lengths, no truncation and matching ordered
timeslots. The empty startup index-0 pair contains 0/0 records and is **not a
pass**. No files were deleted or renamed, and no USB server/debugger was used
for these checks. The 30 host-only `test_sds_session.py` tests also passed.

The file checks do not compare detection values/image pixels, time playback,
prove uninterrupted execution or identify the electrical cause of a disconnect.
Replacing the suspected faulty cable is the **likely explanation for the
improved connection behavior**, not a proven explanation for every historical
defect.
Keep the existing IN/OUT and SDS scheduling fixes. The 120-second run is bounded
sustained-transfer evidence, not broad endurance, hot-plug recovery or USB
conformance. No firmware, pack payload, project selection or debugger setting
was changed for this documentation consolidation.

### Fresh-folder startup recheck — 2026-10-05

The user confirmed this was the first attempt after a fresh restart in response
to the requested full reload/no-reset/no-replug procedure, and supplied the full
server log for `recordings/traffic/cable-startup-recheck-20261005`:

- USB client connected and exchanged alive flags before `R`.
- `CameraIn.0.sds` and `Detections.0.sds` opened, then both closed after `S`.
- Server terminated cleanly; **15/15 paired frames**, no disconnect/fatal error.

Offline parsing also confirmed 15 records each, correct payload sizes and
identical ordered timeslots. **Startup recheck PASS for this run.** Together
with the 86-frame repeat playback and 338-frame sustained test, this completes
the requested bounded application regression. It is not a guarantee for every
startup or host/cable, and the earlier empty pair remains unexplained.

At that checkpoint the isolated [USB driver HIL project](tests/usb-hil/README.md)
was implemented, host-tested and target-built, but not yet hardware-qualified.
Its subsequent high-speed results are recorded below; they do not change the
scope of the DFP 3.1.5 traffic regression above.

## Isolated USB HIL follow-up — 2026-10-06

USB HIL alone selects local DFP **3.1.6-rc.1**, HS component **1.2.0**.
After the abort-hardening driver change and a host-only post-reset reconnection
fix, two user-run suites passed **64/64 cases at 512-byte MPS**. Run 03
reproduced configuration NO_DEVICE and recovered in 106.5 ms on attempt 2;
run 04 reconnected on attempt 1 in 3.24 ms. Reset counts advanced 1 → 2 → 3;
abandoned work produced no completions and both fresh 8192-byte loopbacks passed.

The [integration evidence](../../pack-work/Nuvoton.NuMicroM55_DFP/abort-hardening/integration-20261006.md#hardware-retest--pass-at-high-speed)
records log filenames/hashes, firmware and host provenance, and limitations.
This closes the observed host-reconnection failure for that tested setup, not
general USB compliance. Full-speed testing was intentionally skipped at the
user's request on 2026-10-06; broader lifecycle testing remains open.
Traffic/camera still select DFP 3.1.5; these isolated results neither migrate
the application nor establish candidate-DFP SDS behavior. No board or firmware
changes were made while consolidating this evidence.

## Repeat the regression

1. Build in the CMSIS view only if the firmware changed. For a new image or after
   power removal, use `CMSIS_DAP@pyOCD (launch)` and allow the slow full load to
   finish. HyperRAM is volatile; a power cycle alone cannot restart this image.
2. Continue past `main()` and remove/disable per-frame and per-packet breakpoints
   or logpoints. Connect J13 using a known-good data cable and close other SDSIO
   servers. Keep the cable/port and firmware unchanged throughout the sequence.
3. From the project directory, choose a fresh work directory and record:

   ```sh
   .venv/bin/python traffic/sds_session.py record 30 --transport usb --workdir recordings/traffic/numaker-baseline-check
   ```

4. Require `Recording verified: N/N paired frames`, nonzero N, and no fatal error.
   Then play the same directory:

   ```sh
   .venv/bin/python traffic/sds_session.py play --transport usb --workdir recordings/traffic/numaker-baseline-check
   ```

5. Require `Playback verified: N/N frames`. Repeat playback without reset, then
   record again to a new work directory. Check the live image remains streak-free
   when capture resumes. Compare counts and rates, but do not require exactly
   86 frames from every 30-second scene/run.

A fatal protocol error invalidates that run even if cleanup closes both streams.
Stop the server and reload the debugger-loaded image before retrying. Preserve
failed files separately; do not use them as a known-good playback reference.
The helper waits for a live client exchange before sending Record/Play. A USB
connection timeout is not a recording or playback result and does not prove the
processor is halted.

## Host-only regression checks

Run from the repository root; none of these tests accesses the board:

```sh
python3 -m unittest discover -s traffic/tests -v
python3 board/NuMaker-X-M55M1D/tests/test_usbd_in.py --driver /path/to/DFP/3.1.5/Library/CMSIS/Driver/Source/Driver_USBD/Driver_USBD_HSUSBD.c
python3 board/NuMaker-X-M55M1D/tests/test_usbd_out.py --driver /path/to/DFP/3.1.5/Library/CMSIS/Driver/Source/Driver_USBD/Driver_USBD_HSUSBD.c
python3 board/NuMaker-X-M55M1D/tests/test_sds_usb_init.py
python3 board/NuMaker-X-M55M1D/tests/test_sds_usb_receive.py
```

Substitute your DFP source path above; the old default local USB override has
been removed. The USB tests compile actual source excerpts against host mocks. The IN suite
uses address/undefined-behavior sanitizers. These tests cover software ordering,
buffer ownership and boundary cases, not the PHY, real interrupt timing or
USB compliance. See [USB-driver.md](USB-driver.md) for their individual scope.

Historical recheck on 2026-10-02: all 33 traffic tests and all four standalone
USB scripts passed. Offline parsing of the saved
`camera-usb-in-check` and `camera-usb-soak-30s` files also confirmed 15/15 and
86/86 respectively, for both recording and playback outputs, with correct
payload lengths and matching ordered timeslots. That file check does not
remeasure playback duration or rerun hardware. Only documentation and comments
changed in that earlier consolidation; no firmware rebuild or board reload was needed.

## Still open

- Repeated-startup reliability beyond the successful fresh-folder recheck.
  Cable disconnect/reconnect, bus reset during traffic and automatic recovery
  remain unqualified; the earlier empty startup pair is still unexplained.
- Long-duration endurance, other hosts/hubs, and full-speed hardware transfers.
  The successful 120-second run is still a bounded regression despite its
  `soak` directory name.
- The exact competing bus master/timing mechanism behind the camera streaks,
  and the precise wire-level cause of the earlier missing 512-byte USB packet.
- Concurrent camera/inference/LCD operation without corruption. Keep serialized
  capture until controlled testing establishes a safe alternative.
- Persistent standalone boot and power-cycle recovery without a debugger load.
- Clean-pack reproducibility and complete CMSIS-Driver contract validation.

Future throughput or pack changes should repeat this regression before replacing
the baseline. Upstream recommendations are in [Pack-improvements.md](Pack-improvements.md);
that plan distinguishes completed local migrations from remaining upstream work.
