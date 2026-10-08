# Camera capture: retained serialized baseline

`TRAFFIC_CAMERA_SERIAL: 1` in `Board-Traffic.clayer.yml` is the retained NuMaker
baseline after the user confirmed the streaks disappeared on 2026-10-02.
Use `0` only for a controlled comparison with the background camera thread;
concurrent capture is not the validated quality baseline. Other board targets
default to their original path. See [Validation.md](Validation.md) for the
subsequent successful USB recording/playback regression and open tests.

The retained sequence is:

1. Wait for the preceding LCD job to finish transferring its pixels.
2. Capture one complete camera frame, then convert it to the model input.
3. Run inference and tracking.
4. Submit the next LCD job; finish it before the next capture.

No camera worker is created, so neither inference nor an LCD transfer overlaps
capture. Playback reads its SDS input without capturing real camera frames.
The sensor configuration, 55 MHz clock, capture buffers in HyperRAM, stride,
scaler, USB configuration and installed packs are unchanged. USB/SDS tasks are
still enabled. The initial camera-only comparison was made with SDS stopped;
the later USB regression below exercises recording and playback as well.
The LCD continues refreshing from its own memory; only host-to-LCD transfers
are serialized. This favors image integrity over capture concurrency.

## Evidence and comparison

- Reducing the sensor clock from 55 MHz to approximately 36.7 MHz made no
  visible improvement; the original divider was restored.
- Streaks were present in recorded model-input images as well as on the LCD.
- Live capture settings matched the intended crop (480x480 at x=80), scaling
  (416/480 on both axes) and packet stride (416 pixels).
- `CCAP_FIFOTH` read `0x870D0507` with `OVF=1`, despite `camera_errors=0`.
  The existing error counter does not test that overflow flag. Writing zero
  to OVF did not clear it, so this observation does not establish when it set.

The comparison used the same stationary scene and lighting, with execution
continued past `main` and the diagnostic breakpoint removed for the visual
check. Improvement supports a contention/timing hypothesis, but does not prove
which bus master or overlapping transfer was responsible.

## Serialized-build checks (2026-10-02)

- NuMaker Release build succeeded; 33 traffic host tests and the then-available
  three standalone USB regression scripts passed. The host tests exercise
  capture ordering, timeout recovery, display-off and playback/fallback paths.
- After loading, the RTOS task list had no background camera worker.
- Before the first capture: `camera_frames=0`, `CCAP_FIFOTH=0x070D0507`
  (`OVF=0`) and `CLK_VSENSEDIV=1` (the original 55 MHz setting).
- At the next capture entry after 20 completed frames: `camera_frames=20`,
  `camera_errors=0`, `traffic_status.detector_status=0` and still
  `CCAP_FIFOTH=0x070D0507` (`OVF=0`). The conditional breakpoint evaluated
  at capture entry, with CCAP idle, not during a transfer.
- The diagnostic breakpoint was removed, GDB reported no breakpoints, and
  execution was resumed for an uninterrupted visual comparison.

Image-quality result: **user confirmed the horizontal streaks are completely
gone** on 2026-10-02 after execution resumed. Leave `TRAFFIC_CAMERA_SERIAL: 1`
enabled as the working baseline.

Together with the clear FIFO flag after 20 frames, this strongly supports
capture contention/timing as the cause of the observed corruption. It does
not isolate which overlapping transfer was responsible. The exact bus-level
mechanism and long-duration behavior remain unverified.

## Subsequent USB regression (2026-10-02)

With serialization retained and the project-local USB IN completion correction:

- 5-second recording: 15/15 paired frames.
- Playback and repeat playback without reset: 15/15 frames in 4.4 s each
  (reported 3.42 frames/s).
- Subsequent 30-second recording: 86/86 paired frames.
- Playback: 86/86 frames in 20.4 s (reported 4.23 frames/s).

All are user-reported hardware runs without fatal protocol errors. This replaces
the earlier pending USB validation, but does not qualify reconnect recovery or
long-duration endurance. [Validation.md](Validation.md) distinguishes structural
SDS pairing from visual quality and inference-value validation.
