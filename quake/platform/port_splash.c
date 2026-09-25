/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The boot screen: what the firmware is doing before Quake can draw (the NPU
 * check, the external RAM, the pak in the PSRAM or the flash), as text lines
 * and a progress bar on the panel. Quake's own font is in the pak, which is
 * what is being fetched: this has a 5 x 7 font of its own, digits, capitals
 * and a few marks. Drawn straight into the display's back buffer, in the
 * panel's orientation (portrait, 480 x 800 RGB888, the byte order of
 * port_video.h), scaled 3x.
 */

#include <stdint.h>
#include <string.h>

#include "RTE_Components.h"
#include CMSIS_device_header

#include "port_splash.h"
#include "port_video.h"

#define PANEL_W 480
#define PANEL_H 800
#define SCALE   3
#define LINE_H  (10 * SCALE)
#define MARGIN  24
#define LINES   14

/* 5 x 7 glyphs, one byte per row, bit 4 = left pixel. ' ' .. 'Z', a subset. */
static const uint8_t font[][7] = {
    [0] = {0, 0, 0, 0, 0, 0, 0},                                                    /* space */
    ['%' - ' '] = {0x19, 0x1A, 0x02, 0x04, 0x08, 0x0B, 0x13},
    ['.' - ' '] = {0, 0, 0, 0, 0, 0x0C, 0x0C},
    ['/' - ' '] = {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10},
    ['0' - ' '] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    ['1' - ' '] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    ['2' - ' '] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    ['3' - ' '] = {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    ['4' - ' '] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    ['5' - ' '] = {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    ['6' - ' '] = {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    ['7' - ' '] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    ['8' - ' '] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    ['9' - ' '] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    [':' - ' '] = {0, 0x0C, 0x0C, 0, 0x0C, 0x0C, 0},
    ['A' - ' '] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    ['B' - ' '] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    ['C' - ' '] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
    ['D' - ' '] = {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C},
    ['E' - ' '] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
    ['F' - ' '] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    ['G' - ' '] = {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},
    ['H' - ' '] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    ['I' - ' '] = {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    ['K' - ' '] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},
    ['L' - ' '] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    ['M' - ' '] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
    ['N' - ' '] = {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},
    ['O' - ' '] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    ['P' - ' '] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    ['Q' - ' '] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},
    ['R' - ' '] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    ['S' - ' '] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},
    ['T' - ' '] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    ['U' - ' '] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    ['V' - ' '] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
    ['W' - ' '] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11},
    ['X' - ' '] = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
    ['Y' - ' '] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04},
    ['Z' - ' '] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},
};

static uint8_t *fb;    /* the panel buffer being drawn */
static int line;       /* next text line */
static int splash_on;
static int last_percent = -1;
static char lines[LINES][40];  /* the picture, to draw it into either buffer */

static void pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  if ((unsigned)x >= PANEL_W || (unsigned)y >= PANEL_H) return;
  uint8_t *p = fb + ((size_t)y * PANEL_W + (size_t)x) * 3U;
#if PORT_PANEL_BGR
  p[0] = b, p[1] = g, p[2] = r;
#else
  p[0] = r, p[1] = g, p[2] = b;
#endif
}

/* The panel is portrait, the picture landscape (800 wide, 480 high): the same
 * mapping as the game's CPU present path in port_video.c (landscape x runs
 * down the panel's rows, landscape y from the panel's last column back). */
static void landscape(int lx, int ly, uint8_t r, uint8_t g, uint8_t b) {
#if PORT_PANEL_TOP_LEFT
  pixel(PANEL_W - 1 - ly, lx, r, g, b);
#else
  pixel(ly, PANEL_H - 1 - lx, r, g, b);
#endif
}

static void glyph(int tx, int ty, char c, uint8_t r, uint8_t g, uint8_t b) {
  if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
  if (c < ' ' || c > 'Z') c = ' ';
  const uint8_t *rows = font[c - ' '];
  for (int row = 0; row < 7; row++)
    for (int col = 0; col < 5; col++)
      if (rows[row] & (0x10 >> col))
        for (int j = 0; j < SCALE; j++)
          for (int i = 0; i < SCALE; i++) {
            landscape(tx + col * SCALE + i, ty + row * SCALE + j, r, g, b);
          }
}

static void text(int tx, int ty, const char *s, uint8_t r, uint8_t g, uint8_t b) {
  for (; *s; s++, tx += 6 * SCALE) glyph(tx, ty, *s, r, g, b);
}

/* The whole picture into fb: title, the lines so far, the bar. */
static void redraw(void) {
  memset(fb, 0, (size_t)PANEL_W * PANEL_H * 3U);
  text(MARGIN, MARGIN, "QUAKE ON THE ALIF E8", 255, 200, 80);
  text(MARGIN, MARGIN + LINE_H, "CORTEX-M55 + ETHOS-U", 255, 200, 80);
  for (int i = 0; i < line; i++) text(MARGIN, MARGIN + (3 + i) * LINE_H, lines[i], 220, 220, 220);
  /* the bar: the last text line's worth of height at the bottom of the view */
  int ty = 480 - MARGIN - LINE_H;
  int w = 800 - 2 * MARGIN;
  for (int i = 0; i < w; i++) {
    int on = last_percent >= 0 && i * 100 < last_percent * w;
    uint8_t v = on ? 255 : 40;
    for (int j = 0; j < 8 * SCALE / 2; j++) landscape(MARGIN + i, ty + j, on ? 255 : v, on ? 200 : v, on ? 80 : v);
  }
}

void port_splash_begin(void) {
  fb = video_begin();
  if (fb == NULL) return;
  splash_on = 1;
  line = 0;
  last_percent = 0;
  redraw();
  port_splash_progress(0);
}

void port_splash_line(const char *s) {
  if (!splash_on || fb == NULL) return;
  if (line < LINES) {
    strncpy(lines[line], s, sizeof(lines[0]) - 1U);
    lines[line][sizeof(lines[0]) - 1U] = '\0';
    line++;
  }
  redraw();
  port_splash_progress(-1);
}

void port_splash_progress(int percent) {
  if (!splash_on || fb == NULL) return;
  if (percent >= 0) last_percent = percent;
  redraw();
  /* Show it: flip, then draw the same picture again into the buffer that is
   * the back one now (the two panel buffers are the NPU modules' scratch with
   * PORT_ZERO_COPY: they are written, never copied from). */
  video_end();
  fb = video_begin();
  if (fb != NULL) redraw();
}

void port_splash_end(void) {
  /* The back buffer is the splash's again after the last flip: leave both
   * buffers to Quake. Its first frame overwrites everything. */
  splash_on = 0;
  fb = NULL;
}

int port_splash_active(void) { return splash_on; }
