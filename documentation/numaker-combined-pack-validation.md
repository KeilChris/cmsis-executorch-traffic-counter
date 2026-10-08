# NuMaker combined-pack validation — 2026-10-08

## Outcome

This validates the traffic application with both review candidates together,
not the standalone USB HIL image. **PASS for the planned application regression
on 2026-10-08**: all seven logs and their retained SDS record headers were
inspected. This is scoped integration evidence, not general USB conformance.

## Completed results

Evidence directory: [logs/combined-packs/20261008-B31DJy](../logs/combined-packs/20261008-B31DJy).

| Check | Result | Log |
| --- | --- | --- |
| 30-second recording | 86/86 paired frames | `01-record-30s.log` |
| Playback | 86/86, 20.8 s, 4.14 frames/s | `02-playback.log` |
| Repeat playback | 86/86, 20.5 s, 4.20 frames/s | `03-playback-repeat.log` |
| Recording after playback | 15/15 paired frames | `04-record-after-playback.log` |
| 120-second recording | 339/339 paired frames | `05-record-120s.log` |
| Soak playback | 339/339, 79.0 s, 4.29 frames/s | `06-playback-soak.log` |
| Fresh-start recording | 16/16 paired frames | `07-record-fresh-startup.log` |

Every log shows both streams opening and closing, a verified result, and normal
server shutdown, without a fatal error or unexpected USB disconnect. Offline
reinspection confirmed nonempty streams, complete record boundaries, expected
payload sizes (CameraIn: 519168 bytes; Detections: 400 bytes), and matching ordered
timeslots for every pair. The first playback was checked against its preserved
`Detections.0.first-playback.sds`, not the output overwritten by repeat playback.
The ELF/HEX hashes below still match the prepared build.

The user confirmed a streak-free LCD after recording following playback. The
fresh-start result followed the instructed user-operated HyperRAM reload/debug
sequence; the SDS log itself does not independently prove the physical reset
method. This is not a cold-power-cycle or USB hot-plug qualification.

The [shareable review addendum](../pack-work/nuvoton-review/deliverables/COMBINED-VALIDATION-20261008.md)
records scope and hashes. Existing review ZIPs remain unchanged. The following
procedure is retained for reproduction; no further rerun is required to close
this planned regression.

## Tested configuration

- DFP: `Nuvoton::NuMicroM55_DFP@3.1.6-rc.1`, from
  `pack-work/Nuvoton.NuMicroM55_DFP/abort-hardening/release/3.1.6-rc.1`.
- BSP: `Nuvoton::NuMicro_M55M1_BSP@3.1.5-rc.1`, from
  `pack-work/Nuvoton.NuMicro_M55M1_BSP/camera-hardening/release/3.1.5-rc.1`.
- Context: `traffic.Release+NuMaker-X-M55M1D`; target set: default,
  **not** `usb-test` or `camera-test`.
- Resolved HS USB driver component: `M55M1_M5531_HS@1.2.0`.
- Existing model, serialized camera capture, polling LCD, RTX settings and
  project-local `sdsio_client_usb_mdk.c` are retained.
- Selected candidate directories match all 439 DFP / 43 BSP archive files.
  The review archives and sibling vendor repositories were not edited.
- CMSIS build action completed successfully. Its diagnostic report shows
  AC6 6.24.0 and no compiler/linker errors. The generated context reports an
  unrelated requirement warning for unused `ARM::Cortex_DFP@1.2.0` requesting
  CMSIS 6.3.0-0; this application resolves CMSIS 6.2.0.

Snapshot identifiers (recompute after any rebuild/change):

| Artifact | SHA-256 |
| --- | --- |
| DFP review `.pack` | `0de7db9474e52d84690f75e694f909cbecebc890ce45d0a21f9b320bc9128efa` |
| BSP review `.pack` | `973b08877a6e0329097e313d51a2691c4ff0857fa7fc98b5f79b4ed44e61129e` |
| `out/traffic/NuMaker-X-M55M1D/Release/traffic.axf` | `7a090c23812aef1fd61f8a1172e20a4319dc4603a9ddf88df47dbb2341fac816` |
| `out/traffic/NuMaker-X-M55M1D/Release/traffic.hex` | `3cc925800c27947f525851ba9ce437d4e7a291cf01b2388b7f7d1c5c25ff803f` |

Build environment preflight: the workspace manifest's installed CMSIS-Toolbox
2.14.1 artifact (whose `cbuild --version` reports 2.14.0), CMake 4.2.1,
Ninja 1.13.2 and requested AC6 6.24.0 / GCC 14.3.1 / CLANG 22.1.0 were found.
Compiler registration/PATH changes were process-local only. The VS Code build
uses its own activated environment; generated metadata reports csolution
2.15.1+p3-gf46d68bf. This is application integration validation, not a claim
of identical build-tool versions to the older qualification.

## 1. Load and run the correct image

Use the CMSIS Developer Assistant / CMSIS debugger workflow. The prepared
runner is `out/cmsis-executorch+NuMaker-X-M55M1D.hyperram.cbuild-run.yml`.
Before loading, verify that it names `traffic.axf`/`traffic.hex`, the two
candidate pack versions above and `board/NuMaker-X-M55M1D/Flash/M55M1_HyperRAM.FLM`.
Do not use a stale runner naming USB HIL or the generic SPI-flash loader for
the HyperRAM model region.

For a manual load, select **Tasks: Run Task → NuMaker HyperRAM Load**, wait for
completion, then select **CMSIS_DAP@pyOCD (debug loaded image)** from the Run and
Debug configuration dropdown. Continue past `main()` with F5. Do not select
the generic `CMSIS Load` task or either HIL launch.

Setup history: an initial tool programming request had no confirmed completion.
The user's later generic `CMSIS Load` attempts failed with `flash init failure`
using `M55M1_SPIM.FLM` for the HyperRAM region. The user subsequently confirmed
successful **NuMaker HyperRAM Load** and started the traffic debugger before
the seven successful sessions. Those setup failures are not USB runtime failures.

After programming succeeds, use the traffic debugger configuration with
matching symbols, continue past `main()` and remove/disable interfering test
breakpoints. Keep the camera and LCD fitted, J13 on a known-good USB data
cable, and the debugger/power connected. Close other SDSIO-Servers.

Confirm live camera motion, a streak-free LCD image, and inference responding
to a recognizable object. Keep the board running throughout steps 2–5. Do not
pause/reset it or reconnect USB between these sessions.

## 2. Create a fresh evidence directory

Run the following user commands in one project-root terminal (macOS zsh/bash).
They use the existing project Python environment and SDS host utility.

```sh
set -o pipefail
mkdir -p recordings/traffic logs/combined-packs
validation_run=$(mktemp -d "$PWD/recordings/traffic/combined-packs-20261008-XXXXXX")
validation_logs=$(mktemp -d "$PWD/logs/combined-packs/20261008-XXXXXX")
printf 'Recording directory: %s\nLog directory: %s\n' "$validation_run" "$validation_logs"
```

Keep this terminal open so the two variables remain available. Fresh directories
avoid mixing this firmware's results with earlier recordings. Run each step
once; for a repeat of the entire sequence, create new directories. If any
command fails, stop and preserve the output rather than proceeding or retrying.

## 3. Record 30 seconds

```sh
.venv/bin/python -u traffic/sds_session.py record 30 --transport usb \
  --workdir "$validation_run" 2>&1 | tee "$validation_logs/01-record-30s.log"
```

Require a nonzero `Recording verified: N/N paired frames`, both streams closed,
and no protocol error or unexpected USB disconnect. The exact frame count is
not fixed; compare throughput with earlier runs only after correctness passes.

## 4. Play twice without resetting

```sh
.venv/bin/python -u traffic/sds_session.py play --transport usb \
  --workdir "$validation_run" 2>&1 | tee "$validation_logs/02-playback.log"
```

Require `Playback verified: N/N frames`, matching the first recording's count.
Retain its output before the second playback replaces the same `.p.sds` file:

```sh
cp -p "$validation_run/Detections.0.p.sds" "$validation_logs/Detections.0.first-playback.sds"
.venv/bin/python -u traffic/sds_session.py play --transport usb \
  --workdir "$validation_run" 2>&1 | tee "$validation_logs/03-playback-repeat.log"
```

Require the same N/N result, no disconnect/error, and a return to live camera
display after playback. Frame pairing is not proof of model accuracy or
bit-identical detections; those are separate checks.

## 5. Record after playback

```sh
.venv/bin/python -u traffic/sds_session.py record 5 --transport usb \
  --workdir "$validation_run" 2>&1 | tee "$validation_logs/04-record-after-playback.log"
```

Require a nonzero paired result for the new index-1 recording. Confirm that
the live LCD remains streak-free and continues updating afterwards.

## 6. Extended and startup checks

Only after steps 3–5 pass:

```sh
validation_soak=$(mktemp -d "$PWD/recordings/traffic/combined-soak-20261008-XXXXXX")
.venv/bin/python -u traffic/sds_session.py record 120 --transport usb \
  --workdir "$validation_soak" 2>&1 | tee "$validation_logs/05-record-120s.log"
```

Then, if successful:

```sh
.venv/bin/python -u traffic/sds_session.py play --transport usb \
  --workdir "$validation_soak" 2>&1 | tee "$validation_logs/06-playback-soak.log"
```

Finally perform a deliberate fresh debugger load/restart, continue execution,
and confirm live camera operation before this separate startup recording:

```sh
validation_startup=$(mktemp -d "$PWD/recordings/traffic/combined-startup-20261008-XXXXXX")
.venv/bin/python -u traffic/sds_session.py record 5 --transport usb \
  --workdir "$validation_startup" 2>&1 | tee "$validation_logs/07-record-fresh-startup.log"
```

Do not power-cycle without reloading the debugger-loaded HyperRAM image.
Record the restart/load method and any cable intervention. An automatic retry
or USB reconnection would make this an ambiguous first-start test.

## Acceptance and failure handling

Return the log directory and visual observations. A pass requires the intended
firmware identity, clean live camera/LCD, paired recording/playback through all
tested transitions, and no unexpected disconnect, protocol error or fault.
Preserve any failed files/logs and current target state; inspect before reset.
Full-speed USB and broader USB compliance remain outside this validation.

The completed, reviewed outcome is recorded above and in dated supplemental
evidence; do not replace the frozen reviewer ZIPs. Rollback, if needed, means
restoring the board layer's
DFP selection to installed 3.1.5, rebuilding and reloading, not changing a pack
cache or either frozen candidate.
