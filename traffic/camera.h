/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The camera of the traffic counter on the Ensemble E7 (AppKit-E7). The E7
 * has no ISP and its CPI no streaming mode: the sensor streams over MIPI
 * CSI-2 into the CPI, which writes one frame buffer; at each VSYNC the
 * application hands it the next of CAMERA_BUFFERS buffers, as the pack's
 * vStream driver does, and the frame just completed is the newest one. The
 * pixels stay as the sensor delivers them; image.c turns them into the model
 * input on the CPU:
 *
 *   ARX3A0 (the AppKit-E7 Gen 2 module, RTE_Drivers_CAMERA_SENSOR_ARX3A0):
 *       560x560 RAW8 Bayer, one byte per pixel (RTE_Device.h: RAW10 over
 *       CSI-2, the CPI keeps the top 8 bits). No auto exposure in the sensor:
 *       camera_auto_exposure() adjusts its gain from the picture's brightness.
 *   MT9M114 (RTE_Drivers_CAMERA_SENSOR_MT9M114): 640x480 RGB565 from the
 *       sensor's own ISP (RTE_MT9M114_CAMERA_SENSOR_MIPI_IMAGE_CONFIG 3).
 */
#ifndef TRAFFIC_CAMERA_H_
#define TRAFFIC_CAMERA_H_

#include <stdint.h>

#include "RTE_Components.h"
#include "RTE_Device.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(APP_CAMERA_NUVOTON)
#define CAMERA_WIDTH   416
#define CAMERA_HEIGHT  416
#define CAMERA_RAW8    0  /* CCAP converts HM1055 YUV422 to RGB565 */
#define CAMERA_BPP     2
#elif defined(RTE_Drivers_CAMERA_SENSOR_ARX3A0)
#define CAMERA_WIDTH   RTE_ARX3A0_CAMERA_SENSOR_FRAME_WIDTH
#define CAMERA_HEIGHT  RTE_ARX3A0_CAMERA_SENSOR_FRAME_HEIGHT
#define CAMERA_RAW8    1  /* one byte per pixel, Bayer mosaic */
#define CAMERA_BPP     1
#else
#define CAMERA_WIDTH   640
#define CAMERA_HEIGHT  480
#define CAMERA_RAW8    0  /* RGB565 */
#define CAMERA_BPP     2
#endif

/* The Bayer order of the RAW8 frame, as image.h numbers it (IMAGE_BAYER_*):
   the colour of the top-left pixel of each 2x2 cell and its right neighbour.
   To be confirmed on the board: a picture with red and blue swapped, or
   green cast, means another order. */
#ifndef CAMERA_BAYER
#define CAMERA_BAYER 1  /* GRBG */
#endif

/* How the picture is turned on the way to the model input (and the panel),
   in quarter turns counter-clockwise, for a camera module mounted turned. */
#ifndef CAMERA_QUARTER_TURNS
#define CAMERA_QUARTER_TURNS 0
#endif

/* Frame buffers, in SRAM0 (the E7's CPI cannot write SRAM1: AXI decode
   errors): the CPI takes one snapshot into one buffer while the application
   reads the other. */
#define CAMERA_BUFFERS 2

/* Power and configure the sensor and capture interface. The exact nonzero
   initialization step is board-specific. */
int32_t camera_init(void);

/* The newest completed frame (CAMERA_WIDTH x CAMERA_HEIGHT x CAMERA_BPP
   bytes), the D-cache invalidated; waits up to `timeout_ms` for a new one.
   NULL on a timeout. Read it within one frame time. */
const void *camera_frame(uint32_t timeout_ms);

/* Frames completed since the start, and camera errors (FIFO overruns, bus or CSI-2 errors). */
uint32_t camera_frame_count(void);
uint32_t camera_error_count(void);

/* Sensor gain control from the mean brightness (0..255) of the last frame:
   for a sensor without auto exposure (the ARX3A0). Call once per frame. */
void camera_auto_exposure(uint32_t mean);

/* The gain the auto exposure last set, in 16.16 fixed point (0x10000 = 1.0). */
uint32_t camera_gain(void);

#ifdef __cplusplus
}
#endif

#endif /* TRAFFIC_CAMERA_H_ */
