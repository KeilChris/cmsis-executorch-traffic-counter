/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The camera of the Alif E8 kits for the cat detector. Two paths, chosen by
 * the board layer's RTE_Device.h:
 *
 *   RTE_ISP = 1  (AppKit-E8: OV5675 raw Bayer on MIPI CSI-2)
 *       CPI -> ISP (demosaic, auto exposure, auto white balance, colour
 *       correction, gamma, the centre square scaled to the model input) ->
 *       planar RGB888 in SRAM. Continuous capture into one buffer that the
 *       application copies out at each frame end (camera_frame /
 *       camera_release); a snapshot per frame if that fails.
 *
 *   RTE_ISP = 0  (an MT9M114, whose own ISP delivers RGB565)
 *       CPI in streaming mode into four frame buffers it cycles through.
 */
#ifndef YOLO_CAMERA_H_
#define YOLO_CAMERA_H_

#include <stdint.h>

#include "RTE_Device.h"

#ifdef __cplusplus
extern "C" {
#endif

#if RTE_ISP
/* The ISP output: planar RGB888, one plane per colour (R, G, B). */
#define CAMERA_WIDTH  RTE_ISP_OUTPUT_WIDTH
#define CAMERA_HEIGHT RTE_ISP_OUTPUT_HEIGHT
#define CAMERA_PLANAR_RGB 1
/* How the picture is turned on the way to the model input (and the panel), in
   quarter turns counter-clockwise: 2 is upside down for the AppKit-E8's camera. */
#ifndef CAMERA_QUARTER_TURNS
#define CAMERA_QUARTER_TURNS 2
#endif
/* The OV5675's frame length in lines (0 keeps the pack's 2000, 31.7 fps):
   1016 is 972 lines of picture plus a short blanking, 62 fps. The ISP's auto
   exposure is capped to fit it (camera.c). */
#ifndef CAMERA_OV5675_VTS
#define CAMERA_OV5675_VTS 1016
#endif
#else
/* The CPI output: RGB565, 640 x 480. */
#define CAMERA_WIDTH  640
#define CAMERA_HEIGHT 480
#define CAMERA_PLANAR_RGB 0
#endif

/* Power the sensor, configure CSI-2, the CPI (and the ISP), start capturing.
   0 on success, else the step that failed (1 initialize, 2 power: no sensor
   answering on I2C, 3 CPI configuration, 4 sensor configuration, 5 events,
   6 buffers, 7 capture start). */
int32_t camera_init(void);

/* The newest completed frame, the D-cache invalidated; waits up to
   `timeout_ms` for one. NULL on a timeout or a capture error. With the ISP
   the buffer stays the application's until camera_release(). */
const void *camera_frame(uint32_t timeout_ms);

/* Done with the frame of camera_frame(): the ISP captures the next one into it. */
void camera_release(void);

/* Frames completed since the start, and camera errors (FIFO overruns, bus or CSI-2 errors). */
uint32_t camera_frame_count(void);
uint32_t camera_error_count(void);

#if RTE_ISP
/* 1 when the ISP captures continuously, 0 when it fell back to a snapshot per frame. */
uint32_t camera_video_mode(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* YOLO_CAMERA_H_ */
