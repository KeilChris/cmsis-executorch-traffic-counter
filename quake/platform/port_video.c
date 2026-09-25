/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Quake's frame on the board's panel, on the CPU: the reference for the NPU
 * present path and the fallback. Quake renders QUAKE_VID_WIDTH x
 * QUAKE_VID_HEIGHT palette indices, landscape; the panel is 480 x 800 RGB888,
 * portrait. One pass rotates by 90 degrees, resolves the palette and doubles
 * every pixel: Quake's column x becomes the panel rows 2x and 2x + 1.
 *
 * Double buffered like the NPU render demo (board/DevKit-E8/board_display.c):
 * the back buffer is written once the controller has started a frame from the
 * other one, and shown at the next vertical blank.
 */

#include <string.h>

#include "RTE_Components.h"
#include CMSIS_device_header

#include "board_display.h"
#include "port.h"
#include "port_video.h"

#define SCALE        QUAKE_PANEL_SCALE
#define PANEL_WIDTH  (SCALE * QUAKE_VID_HEIGHT) /* 480 */
#define PANEL_HEIGHT (SCALE * QUAKE_VID_WIDTH)  /* 800 */
#define FRAME_BYTES  (PANEL_WIDTH * PANEL_HEIGHT * 3)

/* How the board is held in landscape: 1 = the panel's top edge is on the
 * left (rotate the picture clockwise), 0 = on the right. */
#ifndef PORT_PANEL_TOP_LEFT
#define PORT_PANEL_TOP_LEFT 1
#endif

#ifdef PORT_ZERO_COPY
/* No frame buffers of our own: the panel shows the NPU's output where it is,
 * in the scratch of the two present modules (port_npu.cpp sets them). */
static uint8_t *framebuffer[2];
void video_set_buffers(uint8_t *first, uint8_t *second) {
  framebuffer[0] = first;
  framebuffer[1] = second;
}
#else
static uint8_t framebuffer[2][FRAME_BYTES] __attribute__((section(".bss.lcd_frame_buf"), aligned(32)));
#endif
static int back;
static uint32_t free_after;      /* display frame count at the last flip request */
static const uint8_t *presented; /* the buffer of that request: the back buffer is free once it is on screen */
static int display_on;

uint32_t video_present_cycles, video_wait_cycles;

int video_init(void) {
  for (int i = 0; i < 2; i++) {
    memset(framebuffer[i], 0, FRAME_BYTES);
    SCB_CleanDCache_by_Addr(framebuffer[i], FRAME_BYTES);
  }
  int32_t status = display_init();
  if (status == 0) {
    status = display_start(framebuffer[1]);
  }
  display_on = (status == 0);
  back = 0;
  return (int)status;
}

uint8_t *video_begin(void) {
  uint32_t t0 = DWT->CYCCNT;
  if (display_on && presented != NULL) {
    display_wait_shown(presented); /* not a frame count: see board_display.c */
  }
  video_wait_cycles += DWT->CYCCNT - t0;
  return framebuffer[back];
}

int video_back(void) { return back; }

/* Show buffer `index`, a frame the NPU wrote: the other one becomes the back buffer. */
void video_show(int index) {
  if (display_on) {
    display_present(framebuffer[index]);
    free_after = display_frame_count();
    presented = framebuffer[index];
    back = index ^ 1;
  }
}

/* Show the back buffer. video_end() is for a frame the CPU wrote (it is still
 * in the data cache); video_flip() for one the NPU wrote into memory. */
void video_flip(void) {
  if (display_on) {
    display_present(framebuffer[back]);
    free_after = display_frame_count();
    presented = framebuffer[back];
    back ^= 1;
  }
}

void video_end(void) {
  if (display_on) {
    SCB_CleanDCache_by_Addr(framebuffer[back], FRAME_BYTES);
  }
  video_flip();
}

void video_present(const uint8_t *frame, const uint8_t *palette_rgb) {
  uint8_t *target = video_begin();
  uint32_t t1 = DWT->CYCCNT;

  for (int x = 0; x < QUAKE_VID_WIDTH; x++) {
#if PORT_PANEL_TOP_LEFT
    /* clockwise: panel row 2x from the top, panel column from Quake's bottom row up */
    uint8_t *row = target + (size_t)(SCALE * x) * PANEL_WIDTH * 3;
    const uint8_t *source = frame + (size_t)(QUAKE_VID_HEIGHT - 1) * QUAKE_VID_WIDTH + x;
    const int step = -QUAKE_VID_WIDTH;
#else
    uint8_t *row = target + (size_t)(SCALE * (QUAKE_VID_WIDTH - 1 - x)) * PANEL_WIDTH * 3;
    const uint8_t *source = frame + x;
    const int step = QUAKE_VID_WIDTH;
#endif
    uint8_t *out = row;
    for (int y = 0; y < QUAKE_VID_HEIGHT; y++, source += step) {
      const uint8_t *rgb = palette_rgb + 3U * *source;
      for (int k = 0; k < SCALE; k++, out += 3) {
#if PORT_PANEL_BGR
        out[0] = rgb[2], out[1] = rgb[1], out[2] = rgb[0];
#else
        out[0] = rgb[0], out[1] = rgb[1], out[2] = rgb[2];
#endif
      }
    }
    for (int k = 1; k < SCALE; k++) {
      memcpy(row + (size_t)k * PANEL_WIDTH * 3, row, PANEL_WIDTH * 3);
    }
  }

  video_end();
  video_present_cycles += DWT->CYCCNT - t1;
}
