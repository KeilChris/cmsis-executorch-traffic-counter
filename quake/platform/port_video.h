/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Quake's frame on the board's panel: see port_video.c.
 */
#ifndef QUAKE_PORT_VIDEO_H
#define QUAKE_PORT_VIDEO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring the panel up; 0, or the display driver's status. */
int video_init(void);

/* The back buffer (480 x 800 RGB888), once the controller no longer scans it
 * out; video_end() shows it at the next vertical blank. */
uint8_t *video_begin(void);
void video_end(void);

/* For a frame the NPU wrote: as video_end(), without the data-cache clean.
 * video_back() is the index (0, 1) of the back buffer. With PORT_ZERO_COPY the
 * two buffers are not ours: video_set_buffers(), before video_init(). */
void video_flip(void);
int video_back(void);
void video_show(int index); /* as video_flip(), for buffer `index` */
#ifdef PORT_ZERO_COPY
void video_set_buffers(uint8_t *first, uint8_t *second);
#endif

/* PORT_PANEL_TOP_LEFT, the landscape orientation, is shared with the NPU path. */
#ifndef PORT_PANEL_TOP_LEFT
#define PORT_PANEL_TOP_LEFT 1
#endif

/* The byte order of a pixel in the frame buffer. The CDC200's 24-bit format
 * is a packed little-endian 0xRRGGBB: blue is the first byte in memory. (Seen
 * on the panel: with red first, the brown palette of Quake comes out blue.) */
#ifndef PORT_PANEL_BGR
#define PORT_PANEL_BGR 1
#endif

/* Show one frame of QUAKE_VID_WIDTH x QUAKE_VID_HEIGHT palette indices. */
void video_present(const uint8_t *frame, const uint8_t *palette_rgb);

/* DWT cycles spent presenting and waiting for the vertical blank, accumulated; the caller resets them. */
extern uint32_t video_present_cycles, video_wait_cycles;

#ifdef __cplusplus
}
#endif

#endif /* QUAKE_PORT_VIDEO_H */
