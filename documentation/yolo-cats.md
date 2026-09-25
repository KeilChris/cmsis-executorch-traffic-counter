# Cat detection with YOLO26n on the Ethos-U85

Ultralytics YOLO26n (COCO-pretrained, class 15 is "cat") runs on the Ethos-U85
of the Alif Ensemble E8, next to the Cortex-M55 HP core. The whole network is
on the NPU; the CPU thresholds the scores and decodes the boxes; the panel
shows the picture with the boxes. The project is `yolo/yolo.cproject.yml`,
target-set `yolo` of the target-types `AppKit-E8` and `DevKit-E8`.

## The model

[`model/yolo.py`](../model/yolo.py) keeps the NMS-free one-to-one head of
YOLO26 and cuts the network before its top-k and gather, which are not NPU
operators. The classification branch ends in the cat channel alone.

| Method | Input | Outputs |
|--------|-------|---------|
| `detect` | `int8 (1, 416, 416, 3)`: RGB888 as a camera delivers it, `q = pixel ^ 0x80` | `box int8 (1, 4, 3549)`: left, top, right, bottom distance from each anchor, in anchor strides; `cat int8 (1, 1, 3549)`: sigmoid score |

The 3549 anchors are the cells of the 52x52, 26x26 and 13x13 feature maps, row
by row, stride 8 first. Vela 5.1 for the Ethos-U85-256 (`Ethos_U85_SRAM_MRAM`,
`Shared_Sram`): 174 operators, all on the NPU, 1.12 GMAC, 1224 KiB SRAM scratch,
2286 KiB of weights in the MRAM, a 2.38 MB program.

Accuracy of the cat class on COCO val2017, 184 images with 202 cats plus 190
without (`yolo/eval_cats.py`; the int8 row is the PT2E-quantized model with its
fake quantization, on the host):

| Input | float AP50 | int8 AP50 |
|-------|-----------|-----------|
| 256 | 0.757 | 0.731 |
| 320 | 0.822 | 0.810 |
| 416 | 0.871 | 0.852 |

The input size is `YOLO_IMGSZ` (default 416). The int8 score output is
calibrated on COCO128 and saturates at 0.82.

## Build and run

```bash
./setup_venv.sh && .venv/bin/python -m pip install ultralytics   # once
MODEL_FLAVOR=yolo .venv/bin/python create_ai_layer.py ai_layer_yolo/cmsis-executorch.cbuild-mlops.yml
.venv/bin/python yolo/make_test_image.py      # the test image, yolo/test_image.c (not committed)
cbuild cmsis-executorch.csolution.yml --active AppKit-E8@yolo --packs
```

The Secure Enclave has to boot the HP core from its MRAM region: the
"Alif: Install M55_HP debug stubs" task for your board (SW4 on SEUART). Then
load and debug `AppKit-E8@yolo` (or `DevKit-E8@yolo`) from the CMSIS view.
ExecuTorch is 1.5.1 (the pack `PyTorch::ExecuTorch@1.5.1`, pip `executorch==1.5.1`).

The console is a log buffer in the DTCM, `console_log`, so SW4 can stay on
SEUART; the debugger reads it. The global `yolo_status` holds the frame
counters, the times of each step in microseconds, the white-balance gains and
the latest detections (`result`, in input pixels).

## The pipeline

| Step | Where | Time (AppKit-E8, 416) |
|------|-------|------|
| OV5675 raw Bayer, 1296x972, MIPI CSI-2 into the CPI, the ISP: demosaic, auto exposure, colour correction, the centre square scaled to 416x416 planar RGB888 | ISP | 16 ms per frame: 62 fps with the frame length cut to 1016 lines (`CAMERA_OV5675_VTS` in `camera.h`; the pack's 2000 gives 31.7 fps) |
| Planes to interleaved RGB888, turned upright (`CAMERA_QUARTER_TURNS`, 180° for the AppKit-E8), gray-world white balance; Helium, read top to bottom ahead of the ISP's next frame | camera thread (`image.c`) | 3.2 ms |
| RGB888 to int8 in the backend's input copy, `detect` on the NPU, the decode | vision thread (`detector.cpp`) | 17.5 ms, of which the NPU 16.4 ms |
| The picture into the back frame buffer | vision thread | 0.9 ms |
| Boxes, detections, score maps, text, cache clean, present | display thread, while the NPU runs the next frame | 3.5 ms |

The application runs at 50 frames per second at 416 (the NPU is the limit)
and at 61.6, the sensor's rate, with a 320 input. What got it there from 31.7:

- The sensor's frame length: 2000 lines of which 972 carry the picture. At
  1016 lines it runs at 62 fps. The auto exposure is capped at 8 ms (the
  ISP's reckoning) because longer exposures stretch the frame again; in a dim
  room the picture gets darker. `CAMERA_OV5675_VTS 0` restores 31.7 fps and a
  16 ms exposure.
- The camera conversion with Helium for the 180° turn: 3.2 ms instead of 8.1.
  It reads the ISP's one buffer top to bottom much faster than the ISP writes
  the next frame into it, so the short blanking is enough; the ISP's
  frame-end work and its I2C writes come after it.
- The Ethos-U driver's wait for the NPU was a `__WFE` loop that kept the CPU:
  `yolo/ethosu_rtos.c` puts it on RTX semaphores, so the vision thread sleeps
  during the job.
- The display thread draws everything but the picture during that sleep.

The ISP settings (sensor format, crop, output size, the colour matrix) are in
the board layer's `RTE_Device.h`; the tuning that the ISP library does not take
from there is `isp_tuning()` in `camera.c`. The ISP runs in continuous mode
with one buffer, handed back at each frame end (`CAMERA_ISP_CONTINUOUS`); two
buffers in continuous mode deliver no frames yet. The DevKit-E8 variant keeps
the MT9M114 path (RGB565 from the CPI in streaming mode) and has not been run
with a camera.

## Recording and playback (SDS)

The board layer adds SDS 3.1 over SEGGER RTT (channel 1). The streams are
`CameraIn` (each 416x416 RGB888 model input) and `Detections` (a
`detections_t` per frame); their metadata is in `recordings/yolo/*.sds.yml`.

```bash
.venv/bin/python yolo/sds_session.py record 20     # 20 s from the camera
.venv/bin/python yolo/sds_session.py play          # CameraIn.<n>.sds into the detector instead of the camera
.venv/bin/python yolo/images_to_sds.py make recordings/yolo/playback img1.jpg img2.jpg ...
.venv/bin/python yolo/sds_session.py play --workdir recordings/yolo/playback
.venv/bin/python yolo/images_to_sds.py check recordings/yolo/playback   # board vs host float model
```

`sds_session.py` starts `tools/sdsio_rtt_bridge.py` (the J-Link's RTT on a
TCP socket) and SDSIO-Server on it; stop the debug session first, the J-Link
takes one connection.

## Benchmarks

`YOLO_BENCHMARK` (see the `define:` block in `yolo/yolo.cproject.yml`) runs
the test image through the detector 50 times, without camera, display or SDS,
and leaves the times in `yolo_benchmark`. AppKit-E8, HP core at 400 MHz,
Ethos-U85-256, averages of 49 runs after a warm-up run:

| Input | Vela memory mode | Program in | NPU | `execute` + decode |
|-------|------------------|------------|-----|--------------------|
| 416 | `Shared_Sram` | MRAM | 16.34 ms | 17.74 ms |
| 416 | `Sram_Only` | MRAM | 34.3 ms | |
| 320 | `Shared_Sram` | MRAM | 9.22 ms | 10.08 ms |
| 320 | `Shared_Sram` | SRAM0 | 9.15 ms | 9.99 ms |

- The weights stream from the MRAM as fast as from the SRAM: the copy of the
  program into SRAM0 gains under 1 %, so the program stays in the MRAM.
- Vela's `Sram_Only` schedule is half as fast. It assumes all tensors share
  one SRAM and tiles for it.
- The program in SRAM needs `NPU_QCONFIG=0` and `NPU_REGIONCFG_0=0`: the
  driver sends the command stream and the weights through the NPU's EXT port
  by default, and on the E8 that port reaches the MRAM but not the SRAM. Without
  it the job ends in a bus abort, NPU status 0x804, ExecuTorch error 35.
- 320 instead of 416 cuts the NPU time by 44 % for a cat AP50 of 0.810 instead
  of 0.852: 61.6 fps live instead of 50. 416 stays the default. For 320:
  `YOLO_IMGSZ=320` for the export and `make_test_image.py`, and
  `RTE_ISP_OUTPUT_WIDTH/HEIGHT 320` in the board's `RTE_Device.h`.
- The input conversion costs no extra pass: the backend's input copy
  (`arm_ethos_io_memcpy`) does `pixel ^ 0x80` with Helium.

## Memory

| Memory | Holds |
|--------|-------|
| MRAM, from 0x80200000 | code and the test image in the HP core's region, then the 2.38 MB program (`ER_MODEL`, in the MRAM user region, read by the NPU and the ExecuTorch loader). It stops 64 kB below the top of the MRAM, where the Secure Enclave's application package lives (`board/DevKit-E8/linker_ac6_yolo.sct.src`) |
| SRAM0 | Vela scratch (1.25 MB), the method pool, a frame buffer, the input copy, the thread stacks; the first 4 kB stay free for the Cortex-A32 stub |
| SRAM1 | the ISP buffer, the three input slots, the second frame buffer. It is off at reset: `sram1_power_on()` asks the Secure Enclave to power it, and its region is `UNINIT` |
| DTCM | the console log, the C library and RTX data |

The pack's MPU table maps the MRAM user region as Device memory: the CPU must
not read constants there unaligned, so only the program lives there.

## Status

On the AppKit-E8 (2026-09-24): camera, panel, detection and SDS recording and
playback work; 50 frames per second live. The test image gives two cats that
match the float model on the host to a pixel or two; the 12 COCO images of
`recordings/yolo/playback` play back through SDS (`images_to_sds.py check`
compares the board with the host).
Open: two-buffer continuous capture of the ISP, and the DevKit-E8 camera path.
