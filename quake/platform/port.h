/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The seam between Quake's platform drivers (sys_alif.c, vid_alif.c, ...: C,
 * the quakedef.h world) and the target side (memories, time, the present
 * path). Quake's headers and the device / ExecuTorch headers never meet in
 * one translation unit: quakedef.h defines its own `qboolean {false, true}`.
 */
#ifndef QUAKE_PORT_H
#define QUAKE_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Quake renders this view; the hand-off pass rotates it to the 480 x 800 panel. */
#ifndef QUAKE_VID_WIDTH
#define QUAKE_VID_WIDTH  400
#define QUAKE_VID_HEIGHT 240
#endif
/* The panel is 480 x 800: the view is scaled by this on the way (2, or 4 for a 200 x 120 view). */
#define QUAKE_PANEL_SCALE (480 / QUAKE_VID_HEIGHT)

/* Quake's one memory block (zone, hunk, cache). */
void  *port_hunk_base(void);
size_t port_hunk_size(void);

/* The pak image in memory (OSPI flash XIP on the board, preloaded DDR on the
 * FVP), or NULL when there is none. fs_pak_mem.c serves it as id1/pak0.pak. */
const uint8_t *port_pak_image(void);

/* Seconds since start, monotonic. */
double port_time(void);

/* One finished frame: QUAKE_VID_WIDTH x QUAKE_VID_HEIGHT palette indices and
 * the current palette (256 x RGB, with Quake's gamma and screen blend). */
void port_present(const uint8_t *frame, const uint8_t *palette_rgb);

/* Deferred texturing (the "NPU fetch" present path): the plane the span drawer
 * writes texel offsets into while it draws the next frame, or NULL when that
 * frame is to be drawn with texels as usual. Asked once per frame, after
 * port_present(). surface_cache is what the offsets count from. */
uint32_t *port_span_offsets(const uint8_t *surface_cache);

/* Every console line goes past here before it is printed. */
void port_console_line(const char *text);

/* Sys_Error and Sys_Quit end here. Does not return. */
void port_exit(int code);

/* Queue a Quake console command (implemented on Quake's side, sys_alif.c). */
void port_queue_command(const char *text);

/* The engine: Host_Init, then Host_Frame forever. */
int quake_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* QUAKE_PORT_H */
