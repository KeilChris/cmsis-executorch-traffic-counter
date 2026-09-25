/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pixel work of the cat detector on the CPU: the camera frame to the model
 * input, the pictures to the panel, boxes and text on the panel.
 *
 * The panel frame is APP_DISPLAY_WIDTH x APP_DISPLAY_HEIGHT RGB888 (480 x 800,
 * portrait). The picture goes in a square view of the panel width, 480 x 480,
 * centred vertically; above and below it are the status lines.
 */
#ifndef YOLO_IMAGE_H_
#define YOLO_IMAGE_H_

#include <stdint.h>

#include "detector.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMAGE_PANEL_W   APP_DISPLAY_WIDTH
#define IMAGE_PANEL_H   APP_DISPLAY_HEIGHT
#define IMAGE_VIEW      APP_DISPLAY_WIDTH                     /* the square picture on the panel */
#define IMAGE_VIEW_TOP  ((APP_DISPLAY_HEIGHT - APP_DISPLAY_WIDTH) / 2)

/* The centre square of a camera frame (RGB565, w x h, h <= w), scaled to the
   model input (RGB888, size x size). */
void image_camera_to_input(const uint16_t *camera, int w, int h, uint8_t *rgb, int size);

/* Planar RGB888 (three w x h planes: R, G, B), as the ISP writes it, to the
   interleaved model input (RGB888, size x size); scaled when w, h != size.
   quarter_turns: turn it that many times 90 degrees counter-clockwise (square, unscaled input only).
   A gray-world white balance is applied on the way (see image.c). */
void image_planar_to_input(const uint8_t *planes, int w, int h, uint8_t *rgb, int size, int quarter_turns);

/* The white-balance gains (R, G, B) image_planar_to_input applies, adapted every frame. */
void image_planar_gains(float gains[3]);

/* The centre square of a camera frame, 1:1 into the view of the panel (h == IMAGE_VIEW). */
void image_camera_to_view(const uint16_t *camera, int w, int h, uint8_t *panel);

/* A model input (RGB888, size x size), 1:1 in the middle of the view of the panel. */
void image_input_to_view(const uint8_t *rgb, int size, uint8_t *panel);

/* The boxes of `det` (input pixels of a size x size input) on the view, where
   the picture is `view` pixels wide, centred (size: the input 1:1; IMAGE_VIEW:
   a camera square filling the view); a score bar at the top of each box. */
void image_draw_detections(uint8_t *panel, const detections_t *det, int size, int view);

/* The cat score of every anchor of the last run as three heat maps (stride 8,
   16 and 32) side by side from (x, y), each (size / 8) * px pixels square with
   4 * px between them: dark (low) through blue and red to yellow, white at or
   above the detection threshold. */
void image_score_maps(uint8_t *panel, int x, int y, const detector_scores_t *scores, int size, int px);

/* Filled rectangle on the panel, clipped: [x0, x1) x [y0, y1). */
void image_fill(uint8_t *panel, int x0, int y0, int x1, int y1, uint32_t rgb);

/* Text in a 5x7 font scaled by `scale` (upper case, digits, " .:%/-+"). Returns the x after it. */
int image_text(uint8_t *panel, int x, int y, int scale, const char *text, uint32_t rgb);

#ifdef __cplusplus
}
#endif

#endif /* YOLO_IMAGE_H_ */
