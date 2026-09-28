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
is `.alif/M55_HP_mram_cfg_e7.json` with the E7's own device configuration
`.alif/app-device-config-e7.json`, silicon revision B4), or
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
| The kit's MT9M114 module, 640x480 RGB565 from the sensor's own ISP over MIPI CSI-2 (1 lane) into the CPI, which writes one frame buffer; at each VSYNC the application hands it the other of two (`camera.c`; the E7 has no ISP and its CPI no streaming mode) | CPI |
| The frame scaled to 416x416 interleaved RGB888 (an ARX3A0's raw Bayer frame demosaiced first, the colours of each 2x2 cell, no interpolation), turned if the module is mounted turned, gray-world white balance; the MT9M114 runs its own auto exposure, for an ARX3A0 the mean brightness drives the sensor gain (`camera_auto_exposure`) | camera thread (`image.c`) |
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

On the AppKit-E7 the joystick moves the line (`traffic/joystick.c`,
`APP_HAS_JOYSTICK`): left and right move a vertical line, up and down a
horizontal one, faster once held for a second, and the centre button turns it
by 90 degrees; the direction labels follow. The switches sit on the
low-power GPIO block (P15_0..4, pulled up by the pin table); a read of that
block costs milliseconds, so a thread at the lowest priority polls it every
40 ms while the vision thread waits for the NPU (from the vision thread the
five reads took 17 ms a frame and the panel flickered), and the vision thread
applies the requested line between two tracker updates; moving the line
counts nothing by itself.

`traffic/count_playback.py <dir> [--sweep]` reruns tracker.c's logic on the
PC over the board's detections of a playback (the count the board showed) and
over the float model's detections of the same inputs (the reference), by
class and direction, and reports where the tracks moved; `--sweep` tries
horizontal and vertical lines at several positions. On the YouTube clip
(vehicles along a road across the picture just below the middle) the
horizontal line at 208 counts 9 on the board and 12 on the host, a vertical
line at 208 counts 45 and 64: the line has to cut the traffic's path
(`TRAFFIC_LINE_POS`, `TRAFFIC_LINE_VERTICAL` in the layer).

The same four steps are VS Code tasks (Terminal > Run Task): "SDS: video to
CameraIn stream" (ffmpeg frames at a chosen rate, letterboxed or with
`--fill`, the sides cut so the picture fills the input), "SDS: play recording
to the board (USB)", "SDS: record from the camera (USB)" and "SDS: check
playback against the host model". One SDSIO server owns the USB device at a
time: a second playback started while one runs resets the link, and the
board's half-open streams then crash the next server; let a run finish or
stop it with Ctrl+C.

During a playback the camera thread pauses: the model inputs share the two
slots in SRAM1 (see below).

The link is the board's User USB (`SDS:IO:USB&MDK USB`, a custom-class
device "SDSIO-Client", VID 0xC251 PID 0x8007, high speed): the 40-frame
YouTube clip (20.7 MB) plays in about 8 s, where the J-Link RTT link
(`--transport rtt`, `tools/sdsio_rtt_bridge.py`) needed 5 s per frame.
Two things the USB device needs on the E7: the application powers the USB
PHY through the Secure Enclave (`usb_power_init` in `main.c`), and every
buffer the USB controller reads or writes in the DTCM must lie in the
linker's non-secure region `NS_REGION_0` (the TGU rejects the controller
elsewhere, and a control or bulk transfer then hangs silently): the EP0
buffer (`USBD0_BUF_MEM_LOCATE`), the SDS client's bulk buffer, the SDS
data block, and the RTX dynamic memory that holds the SDS thread's stack,
because the SDS client sends its command headers straight from the stack.

## Memory

| Memory | Holds |
|--------|-------|
| MRAM, from 0x80200000 | code and the test image in the HP core's region, then the 2.46 MB program (`ER_MODEL`, running on into the MRAM user region at 0x80400000). It stops 64 kB below the top of the MRAM, where the Secure Enclave's application package lives (`board/AppKit-E7/linker_ac6_traffic.sct.src`); 3.0 of the 3.4 MB are used |
| SRAM0, 4 MB | the ExecuTorch method pool, the two 640x480 RGB565 camera frames (614 kB each), the two 480x800 RGB888 panel buffers (1.15 MB each), the thread stacks; the first 4 kB stay free for the Cortex-A32 stub; 92 % used |
| SRAM1, 2.5 MB at 0x08000000 | two model input slots (519 kB each) and Vela's scratch (1.25 MB, `APP_TEMP_POOL_SECTION`; the NPU runs 81 ms from here as from SRAM0); `UNINIT`, powered through the Secure Enclave by the application; the CPI and the CDC200 cannot reach it, so the camera frames and the panel buffers are in SRAM0. SRAM8 (2 MB at 0x63200000) is no alternative for a panel buffer: with it the display DMA hangs the interconnect at boot, the debug port with it, and the image in MRAM then has to be replaced with the reset button pressed while the J-Link connects |
| DTCM, 1 MB | the console log, the SDS input buffer, the C library, the tracker; the non-secure region at its end holds the USB DMA buffers and RTX's dynamic memory |

Unlike the E8's, the E7's SRAM0 and SRAM1 are not contiguous, so the scatter
file places them separately; the input slots went from three to two to fit.

## Camera on the AppKit-E7

Four things stood between the pack's AppKit-E7 BSP and a live picture
(2026-09-25, all found on the board with the debugger):

- **The camera clock pin.** The BSP's pin table selects `LPCAM_XVCLK_B` on
  P0_3, the clock of the HE core's low-power CPI, which nothing enables. The
  sensor drivers run the HP CPI's pixel clock (`CAMERA_PIXCLK_CTRL`, 400 MHz /
  20), which reaches the pin only as `CAM_XVCLK_A`: without it the sensor never
  answers on I2C1. `board/AppKit-E7/RTE/BSP/.../pins.h` selects `CAM_XVCLK_A`,
  as the pack's own AppKit-E7 layer does.
- **The sensor.** The kit's module is an MT9M114 (I2C address 0x5D over MIPI),
  not an ARX3A0: the layer names the MT9M114 component,
  `RTE_MT9M114_CAMERA_SENSOR_MIPI_IMAGE_CONFIG 3` (640x480 RGB565). Its ISP
  tracks the exposure only once asked (`CPI_CAMERA_SENSOR_AE` in `camera.c`).
- **SRAM1 is the CPU's alone.** The CPI's writes to SRAM1 come back with AXI
  decode errors (`CAM_AXI_ERR_STAT` 0xff03), and the CDC200 cannot read it
  either (the panel goes dark). Neither the E8's nor the E7's device
  configuration in the boot table changes that. So the camera frames live in
  SRAM0 (`.bss.ai_pool`); to keep both panel buffers there too, Vela's scratch
  moved to SRAM1 next to the model input slots (see Memory).
- **One snapshot per frame.** In video mode the pack's driver refuses a new
  frame address while the CPI is busy (`ARM_DRIVER_ERROR_BUSY`), and the E7's
  CPI, which has no stream mode, then writes frame after frame past the
  buffer, over the thread stacks and the pools: a bus fault in the camera
  thread, and within seconds the Secure Enclave and the debug port are gone.
  `camera.c` takes one snapshot at a time (`CaptureFrame`, the STOP event) and
  starts the next one into the other buffer from the camera thread, about 25
  snapshots a second.

The panel's MADCTL value is a layer setting (`TRAFFIC_LCD_MADCTL`), written
before the video stream starts. On this ILI9806E in video mode only bit 0
does anything, a mirror along the long axis (the pack's panel driver sets
0x01, the layer 0x00); bits 7 and 6 are ignored. The panel hangs upside down, so
`IMAGE_PANEL_TURN_180` (the layer) makes `image.c` draw everything, the picture
and every rectangle behind boxes, line, score maps and text, at the opposite
x and y: no extra pass over the frame buffer (turning it in place afterwards
cost 60 ms a frame and starved the NPU).

With the camera the loop runs at 11.5 fps: the 15 ms RGB565 to input
conversion happens in the camera thread while the NPU works. Right after a
`load_and_debug` (a core-only reset) the first inference can fail with err 35,
the NPU still busy with the previous job; the loop recovers on its own.

## Performance on the board

AppKit-E7, HP core and Ethos-U55-256 at 400 MHz, the test image, no camera
(2026-09-25). The NPU time is the command stream from start to interrupt,
the rest of the frame is the CPU (the input copy, the decode, the tracker, the
picture); the display thread runs while the NPU works.

| Input | Vela | NPU | Frame | Detections on the test image |
|-------|------|-----|-------|------------------------------|
| 416 | `RTSS_HP_SRAM_MRAM`, `Shared_Sram` | 81.0 ms | 83.4 ms, 12.0 fps | 2 (float model on the host: 0.71, 0.56, 0.35, 0.27) |
| 416 | `RTSS_HP_DTCM_SRAM`, `Dtcm_Cache`, 640 kB cache in the DTCM | 84.0 ms | 86.4 ms, 11.6 fps | 2 |
| 320 | `RTSS_HP_SRAM_MRAM`, `Shared_Sram` | 40.6 ms | 42.2 ms, 23.7 fps | 0: the int8 model's best score is 0.28, below the 0.30 threshold (host: 0.47, 0.35, 0.28) |

The U55 is bound by its one AXI port, not by the MACs and not by the MRAM.
Its PMU over one 416 inference (32.4 M cycles, counters programmed from the
debugger after the driver's soft reset, `detector.cpp`'s inference hooks):

| Counter | Cycles or beats | Share |
|---------|-----------------|-------|
| `MAC_ACTIVE` | 7.6 M | 23 % |
| `MAC_STALLED_BY_IB` (waiting for input data) | 10.7 M | 33 % |
| `AXI0_RD_TRAN_REQ_STALLED` | 11.9 M | 37 % |
| `MAC_STALLED_BY_WD` (weights from the MRAM) | 1.1 M | 3 % |
| `AXI0_RD_DATA_BEAT_RECEIVED` | 12.8 M beats in 3.1 M transactions (4 beats each) | 102 MB read |
| `AXI0_WR_DATA_BEAT_WRITTEN` | 8.1 M beats | 65 MB written |

The same counters with the scratch cache in the DTCM are unchanged (the DTCM
is no faster for the NPU than SRAM0 on this part, whatever `ensemble_vela.ini`
assumes), so memory placement does not help; only less traffic does. A 320
input halves the NPU time but costs detections: the model would need the
score threshold and the int8 calibration revisited at that size
(`YOLO_IMGSZ=320` for `create_ai_layer.py` and `make_test_image.py`). The
1.5 ms of CPU work per frame and the 4 ms display thread are not worth
optimising: the frame is 97 % NPU.

## Status

Runs on the AppKit-E7 (AK-E7-AIML, Gen 2, MT9M114 module; 2026-09-25 and
after, AC6, `out/traffic/AppKit-E7/Release/traffic.axf`): the camera loop at
11.5 fps, the NPU at 81 ms per 416x416 frame, the joystick line, and SDS
recording and playback over the User USB. The tracker's counting is checked
on the host against the float model (`traffic/count_playback.py`).
