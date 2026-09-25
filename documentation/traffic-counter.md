# Traffic counter with YOLO26n on the Ethos-U55 (AppKit-E7)

The vehicles an Alif Ensemble E7 AppKit (AK-E7-AIML, Gen 2) sees through its
camera are detected by Ultralytics YOLO26n on the Ethos-U55-256 next to the
Cortex-M55 HP core, followed from frame to frame by a small tracker and
counted, per class and direction, as they cross a line on the picture. The
panel shows the picture, the tracks, the line and the tallies. The project is
`traffic/traffic.cproject.yml`, target-type `AppKit-E7`, a fork of the cat
detector of the AppKit-E8 ([yolo-cats.md](yolo-cats.md)).

## The model

[`model/traffic.py`](../model/traffic.py) is the cut of YOLO26n that
`model/yolo.py` makes for the cat, with the classification branch ending in
the five COCO vehicle classes instead of the cat alone:

| Method | Input | Outputs |
|--------|-------|---------|
| `detect` | `int8 (1, 416, 416, 3)`: RGB888 as the camera path delivers it, `q = pixel ^ 0x80` | `box int8 (1, 4, 3549)`: left, top, right, bottom distance from each anchor, in anchor strides; `cls int8 (1, 5, 3549)`: sigmoid score of bicycle, car, motorcycle, bus, truck |

Vela 5.1 for the Ethos-U55-256 (`RTSS_HP_SRAM_MRAM`, `Shared_Sram`): every
operator on the NPU (the Ethos-U55 has no TRANSPOSE, but the input permute
folds into the backend's layout passes), 1.13 GMAC, 1219 KiB SRAM scratch,
2184 KiB of weights in the MRAM, a 2.46 MB program.

`traffic/eval_vehicles.py` measures the vehicle AP50 of the float and the
int8 model on a dataset in the YOLO layout; the int8 score output is
calibrated on the COCO128 images with the most vehicles.

## Build and run

```bash
./setup_venv.sh && .venv/bin/python -m pip install ultralytics   # once
MODEL_FLAVOR=traffic .venv/bin/python create_ai_layer.py ai_layer_traffic/cmsis-executorch.cbuild-mlops.yml
.venv/bin/python traffic/make_test_image.py      # the test image, traffic/test_image.c (not committed)
cbuild cmsis-executorch.csolution.yml --active AppKit-E7 --packs
```

The Secure Enclave has to boot the HP core from its MRAM region: the "Alif:
Install M55_HP debug stubs (AppKit-E7)" task (SW4 on SEUART; the boot table
is `.alif/M55_HP_mram_cfg.json`, the same as the E8's), or
`tools/setools_mram.py --part E7` to program the image itself through the
SE. Then load and debug `AppKit-E7` from the CMSIS view, or the launch
configuration "M55_HP JLink AppKit-E7 traffic".

The console is a log buffer in the DTCM, `console_log`, that the debugger
reads (`JLINK_DEVICE=AE722F80F55D5LS_M55_HP python tools/devkit.py log
out/traffic/AppKit-E7/Release/traffic.axf.map`). The global `traffic_status`
holds the tallies (`counts`), the frame counters, the times of each step in
microseconds, the camera gain and white balance, the live tracks and the
latest detections; writing 1 to `traffic_reset_counts` zeroes the tallies.

## The pipeline

| Step | Where |
|------|-------|
| ARX3A0 raw Bayer, 560x560 at 60 fps, MIPI CSI-2 into the CPI, which writes one frame buffer; at each VSYNC the application hands it the next of three (`camera.c`; the E7 has no ISP and its CPI no streaming mode) | CPI |
| The frame demosaiced (the colours of each 2x2 Bayer cell, no interpolation), scaled to 416x416 interleaved RGB888, turned if the module is mounted turned, gray-world white balance; the mean brightness drives the sensor gain (`camera_auto_exposure`) | camera thread (`image.c`) |
| RGB888 to int8 in the backend's input copy, `detect` on the NPU, the best class per anchor and the decode | vision thread (`detector.cpp`) |
| Detections matched to the tracks by overlap with a constant-velocity prediction; a track is confirmed after 3 detections and counted once when its centre crosses the line (`tracker.c`) | vision thread |
| The picture into the back frame buffer | vision thread |
| The line, the boxes in the class colours, the tallies, the score maps, cache clean, present | display thread, while the NPU runs the next frame |

The counting line is horizontal in the middle of the picture by default:
`TRAFFIC_LINE_POS` (input pixels) and `TRAFFIC_LINE_VERTICAL` in the
`define:` block of `traffic/traffic.cproject.yml` move and turn it.
Direction 0 ("DOWN", or "RIGHT" for a vertical line) is a crossing towards
larger coordinates.

Camera settings to confirm on the board (`traffic/camera.h`): `CAMERA_BAYER`
(the Bayer order of the ARX3A0's frame, GRBG assumed: swapped red and blue or
a green cast mean another order) and `CAMERA_QUARTER_TURNS` (the module's
mounting). The pack's own AppKit-E7 layer names the MT9M114 sensor; with that
module the layer's sensor component and `RTE_MT9M114_CAMERA_SENSOR_MIPI_IMAGE_CONFIG 3`
(640x480 RGB565) select the RGB565 path of `camera.c`.

## Recording and playback (SDS)

As for the cat detector: the streams `CameraIn` (each 416x416 RGB888 model
input) and `Detections` (a `detections_t` per frame, now with a class per
box), metadata in `recordings/traffic/*.sds.yml`.

```bash
.venv/bin/python traffic/sds_session.py record 20     # 20 s from the camera
.venv/bin/python traffic/sds_session.py play          # CameraIn.<n>.sds into the detector instead of the camera
.venv/bin/python traffic/images_to_sds.py make recordings/traffic/playback img1.jpg img2.jpg ...
.venv/bin/python traffic/sds_session.py play --workdir recordings/traffic/playback
.venv/bin/python traffic/images_to_sds.py check recordings/traffic/playback   # board vs host float model
```

During a playback the camera thread pauses: the model inputs share the two
slots in SRAM1 (see below).

## Memory

| Memory | Holds |
|--------|-------|
| MRAM, from 0x80200000 | code and the test image in the HP core's region, then the 2.46 MB program (`ER_MODEL`, running on into the MRAM user region at 0x80400000). It stops 64 kB below the top of the MRAM, where the Secure Enclave's application package lives (`board/AppKit-E7/linker_ac6_traffic.sct.src`); 3.0 of the 3.4 MB are used |
| SRAM0, 4 MB | the ExecuTorch method pool, Vela's scratch (1.25 MB), both 480x800 RGB888 frame buffers, the thread stacks; the first 4 kB stay free for the Cortex-A32 stub; 3.75 MB used |
| SRAM1, 2.5 MB at 0x08000000 | three raw camera frames (314 kB each), two model input slots (519 kB each), the SDS input buffer (535 kB); `UNINIT`, powered through the Secure Enclave by the application; 2.4 MB used |
| DTCM, 1 MB | the console log, the C library, RTX, the tracker |

Unlike the E8's, the E7's SRAM0 and SRAM1 are not contiguous, so the scatter
file places them separately; the input slots went from three to two to fit.

## Status

Built for the AppKit-E7 (2026-09-25, AC6, `out/traffic/AppKit-E7/Release/traffic.axf`)
and the tracker's counting checked on the host. Not yet run on a board: no
AppKit-E7 was attached when the fork was made. What the first board run has
to confirm: the ARX3A0's Bayer order and orientation, the CPI buffer swap at
VSYNC (the pack's vStream driver does the same), the gain loop, and the NPU
time (the Ethos-U55-256 has half the MACs of the E8's U85-256; Vela's model
is 1.13 GMAC per frame).
