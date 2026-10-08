# Isolated camera hardware qualification

This is a **different firmware image**, not the traffic counter. It uses the
unmodified BSP 3.1.5-rc.1 Camera 1.2.0 component, the existing custom NuMaker
board/startup/HyperRAM layer, RTX and the existing USB SDS control transport.
It omits the model, inference loop and LCD updates. A dark or unchanged LCD is
expected. It never starts a capture/test automatically.

## Status — updated 2026-10-05

The isolated HIL observations below are from 2026-10-02; the subsequent traffic
SDS recheck is dated separately.

- Host orchestration checks with ASan/UBSan: PASS (`test_runner.py`). These test
  the test runner against API/cache mocks, not the hardware driver.
- CMSIS `NuMaker-X-M55M1D@camera-test` target build: PASS; linked image is
  `out/camera-hil/NuMaker-X-M55M1D/Release/camera-hil.axf`.
- Candidate driver/package and working traffic sources/image: unchanged.
- Dedicated **NuMaker camera HIL (launch)** configuration added; the working
  traffic configurations are preserved. Host-only launch-guard tests pass.
- **Loaded and tested on hardware:** the user loaded the dedicated HIL image;
  the CMSIS debugger verified its program/runner and stopped at `main()`.
  After continuing, IDLE was confirmed before issuing commands from the debugger.
- Command 1: **PASS**, `phase=3`, `step=129`, `passed=30`,
  `reload_required=0`. The successful abort took 42 ticks at 1000 Hz.
- Command 2: **PASS**, `phase=4`, `step=209`, `passed=39` cumulative,
  `reload_required=1`. The zero-budget abort returned TIMEOUT and subsequent
  API calls rejected reuse. Heartbeat advanced from 1980 after command 1 to
  2987 after command 2; this is scheduler evidence, not a USB wire test.
- **Full debugger-reload recovery PASS:** following command 2, the user stopped
  debugging and relaunched the same HIL image. Status was zeroed at `main()`;
  startup reached IDLE (`phase=1`, `passed=1`, `reload_required=0`). Command 1
  then passed again: `phase=3`, `step=129`, `passed=30`, `reload_required=0`,
  abort time 42 ticks at 1000 Hz. Board left paused in NORMAL_PASS.
  This verifies the full reload sequence, not reset-only or automatic recovery.
- **Command 3 (physically absent camera) PASS:** after the user removed the
  camera with board power disconnected and cold-started/reloaded the HIL image,
  initialization returned ERROR in 596 ticks at 1000 Hz (596 ms, below the
  one-second limit). Config/Trigger returned NOT_READY. Final status:
  `phase=5`, `step=304`, `passed=5` (startup assertion plus four absence checks),
  `reload_required=1`, `irq_count=0`, `events=0`. Debugging was stopped afterward
  for powered-off camera refitting.
- **Post-refit recovery PASS:** after the user refitted the camera while
  unpowered and reloaded the HIL image, startup reached IDLE and command 1
  passed again: `phase=3`, `step=129`, `passed=30`, `reload_required=0`,
  `irq_count=2`, `events=1`. Graceful abort took 40 ticks at 1000 Hz (40 ms).
  This verifies the powered-off refit plus full reload sequence, not hot-plug
  or reset-only recovery. The HIL debug session was ended afterward.
- **Traffic restored, runtime check PASS:** the default target-set's Release
  image and debugger symbols match again. After continuing from `main()`,
  camera/processed frames advanced 41 → 88, with camera/detector status 0,
  camera errors 0 and `camera_fault=false` at both paused observations.
  Execution was resumed and left running. The user subsequently confirmed a
  clean, streak-free live LCD camera image. The post-restoration replacement-cable
  SDS recheck passed on 2026-10-05: 86/86 recording/repeat playback and 338/338
  recording/playback. A separate fresh-folder startup recheck then passed 15/15
  after a fresh restart; the earlier empty pair remains unexplained. See the
  [validation record](../../Validation.md#replacement-cable-regression--2026-10-05).
  These normal-operation results do not qualify USB I/O during camera quarantine.
- **Configuration cleanup complete:** with user approval, the inactive
  `camera-test` image selection was restored from `traffic.Debug` to
  `camera-hil.Release`. The default `traffic.Release` mapping is unchanged.
  YAML/mapping validation and the six host launch-guard tests pass; no rebuild,
  reflash or debug-session change was performed for this configuration edit.
- Remaining physical-fault/USB tests are listed below. **Reset/reload before
  further camera use after any new quarantine or the absent-camera test.**

The test deliberately exercises only documented capture APIs; it never forces
CCAPEN low, resets an active DMA, gates a live camera clock, supplies an invalid
DMA destination or edits packaged driver files. Rationale: M55M1 TRM Rev 1.02,
`user/nuvoton/numicrom55-dfp/en-us-trm-m55m1-series-en-rev1-02`,
§6.25.5.4–6.25.5.6, pp.1670–1671: DMA writes to the programmed buffer and
SHUTTER disables capture after a frame. No force-stop DMA-drain guarantee is
assumed. Documentation tooling falls back to installed BSP 3.1.4 metadata for
the local candidate; the cited device TRM is the explicitly selected Rev 1.02.

## Build and debug selection

1. Stop any existing debug session and wait until VS Code shows it ended.
2. In CMSIS Manage Solution select **NuMaker-X-M55M1D**, target-set
   **camera-test**, then build. Normal traffic remains the default target-set.
   The test has its own project/RTE/output directories, but the generated
   `out/cmsis-executorch+NuMaker-X-M55M1D.cbuild-run.yml` is shared across sets
   and now lists **camera-hil**, not traffic.
3. In **Run and Debug**, select **NuMaker camera HIL (launch)**. Its `program`
   names **camera-hil.axf**; its load task and debugger use the dedicated
   `out/cmsis-executorch+NuMaker-X-M55M1D.camera-hil.hyperram.cbuild-run.yml`.
   Do not select a traffic launch while the test target-set is active.
4. Start this configuration (F5) and wait for `main()`. The prelaunch tasks
   validate the generated runner's target/device, `camera-test` target-set,
   exact HIL ELF/HEX output pair and file presence before loading. They prepare
   the separate runner with the board's HyperRAM algorithm; the traffic runner
   is not overwritten. If preparation fails, **do not choose Debug Anyway**:
   correct the selection/build first. Task definitions are kept in
   `.vscode.d/tasks.json` and mirrored in `.vscode/tasks.json` for immediate use.
   This launch does not rebuild automatically. The test's
   model-free image has no initialized HyperRAM payload: its buffers are UNINIT
   and the reused `Reset_Handler_PreInit` initializes HyperRAM before C startup.
   The normal traffic image still needs its full custom HyperRAM load to restore.
5. Continue past `main()`. Use the debugger's variables/expression tools to read
   **`camera_hil_status`**. Expect `magic=0x43415032`, `phase=1`, `command=0`,
   `reload_required=0`; the independent `heartbeat` should advance between runs.

No test results should be claimed until the image/program match is verified.
Avoid per-frame breakpoints and function calls from Watch expressions. Pause
before inspecting, wait for a valid stopped stack frame, then resume afterward.

## Commands and acceptance

With the CPU paused, change only `camera_hil_status.command` using the debugger,
then continue. Tests execute from the application thread, never inside debugger
function evaluation or an ISR. Read results after allowing execution; a status
stuck at RUNNING is not a pass. The normal test is allowed only from IDLE;
the quarantine test only after NORMAL_PASS. Invalid sequences fail closed.

| Command | Prerequisite | Expected result |
|---|---|---|
| `1` | Fresh load, camera fitted | `phase=3`, `step=129`, `passed=30`, no reload required |
| `2` | Command 1 passed, camera still fitted | `phase=4`, `step=209`, `passed=39`, reload required |
| `3` | Fresh cold start/load with camera absent, arranged **only while power is removed** | `phase=5`, `step=304`, `passed=5`, reload required |

`passed` includes the initial heartbeat-thread allocation assertion. `phase=255`
is failure: retain `step`, `actual`, `expected` and the frame/DMA state for
inspection. `phase=254` is **inconclusive**: a zero-budget abort encountered an
already-stopped frame and did not exercise timeout quarantine. Reload before a
fresh attempt. Do not clear the driver's fault latch or `reload_required` to
force reuse; buffers are reserved until reset/reload.

### Command 1: guards, ownership and successful abort

- Steps 101–104: capture/configure before initialization; a synthetic failure
  returned through the sensor descriptor; propagate ERROR / NOT_READY. The real
  callback is restored before assertions. **This is not an electrical I2C NACK.**
- 105–111: real sensor initialization, unsupported format/dimensions, null and
  misaligned buffers. The test retains RGB565 416×416 and the existing clock/pins.
- 112–121: real capture into A; reject overlapping capture/config/init; release
  only after PollCaptureDone. Check full frame/4 KiB guard regions and reject a
  frame that never changed from its initial fill. B must remain untouched.
- 122–129: request a bounded graceful abort (32,000,000 stop polls, **not ms**).
  On success verify A/guards stable for 250 ms, capture into B, verify A still
  unchanged and B actually received data. On failure stop testing; never reuse A.

The CPU cleans/invalidates aligned buffers before DMA and invalidates again only
after release. Byte-for-byte stability over a finite observation window is
evidence, not proof that no late DMA write is possible in every timing condition.

### Command 2: timeout quarantine

Trigger A then call `ImageSensor_AbortCapture(0)` immediately. TIMEOUT is expected
if the one-shot is still active. After timeout, the test **does not read, clean,
invalidate, overwrite or resubmit A**, even if the hardware stops later. It
requires Trigger/Config/Init/Poll/Abort to return FAULT and verifies B stays
unchanged after another 500 ms. This tests exhaustion of an abort budget with a
healthy sensor, **not** a physically missing PCLK/VSYNC or the blocking Wait
timeout. Those cases remain separate pending tests.

### Command 3: physically absent sensor (optional later stage)

Only after powering off and removing the camera module, cold start/load the
test image and select command 3. Require real initialization to return ERROR
within 1 second (`operation_ticks`, `tick_hz`), with Config/Trigger NOT_READY.
If it remains RUNNING, pause and inspect; do not treat the heartbeat as success.
This exercises absence rejection; it does not identify every individual NACK
phase. Never disconnect/reconnect the camera on a powered board. Refit it with
power removed and perform a full debugger load afterward.

## USB and remaining coverage

The separate RTOS heartbeat checks scheduler progress, **not USB wire traffic**.
The existing SDS control thread is retained so a host can check the live-client
flag exchange before/after failure. This diagnostic image does not call the
vision stream state machine: **do not run the traffic record/play helper** on
it; CameraIn/Detections will not open. A USB live-client/flags test still needs
to be executed and logged independently.

Still pending: real missing-sync capture timeout, hardware bus-error injection
using a reviewed safe method, electrical I2C NACK coverage beyond sensor absence,
USB responsiveness after quarantine and reset-only recovery. Full debugger-reload
recovery after zero-budget quarantine and powered-off camera refitting passed
as noted above. Host driver tests cover software cases but
cannot replace those hardware results. Do not call this a complete
camera/CMSIS-Driver qualification.

## Restore traffic

Stop the diagnostic debug session; select NuMaker's **default** target-set;
build traffic, then use the preserved **CMSIS_DAP@pyOCD (launch)** full HyperRAM
load. Check both `program` and runner outputs again name `traffic`. Do not use
"debug loaded image" to restore after diagnostic programming or a power cycle.

## Host check

```sh
python3 board/NuMaker-X-M55M1D/tests/camera-hil/test_runner.py
python3 board/NuMaker-X-M55M1D/tests/camera-hil/test_prepare_launch.py
```

The first compiles this runner against mocks with address/undefined-behavior sanitizers,
checks pass/fail/inconclusive paths, descriptor restoration, guard corruption,
late-write/no-DMA rejection, and forbids cache accesses to quarantined A.
It never connects to the board.

The second tests the launch guard against wrong target/device/image entries,
missing/empty images, unsafe destination paths and preparation failures. It
checks that failures preserve the previous HIL runner. Neither test flashes
firmware or qualifies the camera hardware.
