/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pixel work of the cat detector on the CPU, see image.h.
 *
 * RGB565 as the CPI stores it (Alif Ensemble HWRM, CPI, "Data Mode = 16, MIPI
 * CSI Data Format = IPI-48, RGB565"): one halfword per pixel, B in bits 4:0,
 * G in 10:5, R in 15:11. RGB888 is three bytes per pixel, R first: the model
 * input and the panel frame alike.
 */

#include <stdint.h>
#include <string.h>

#include <arm_mve.h>

#include "image.h"

static inline void rgb565_to_888(uint16_t p, uint8_t *d)
{
    const uint32_t r = (p >> 11) & 0x1FU, g = (p >> 5) & 0x3FU, b = p & 0x1FU;
    d[0] = (uint8_t)((r << 3) | (r >> 2));
    d[1] = (uint8_t)((g << 2) | (g >> 4));
    d[2] = (uint8_t)((b << 3) | (b >> 2));
}

void image_camera_to_input(const uint16_t *camera, int w, int h, uint8_t *rgb, int size)
{
    const int x0 = (w - h) / 2;  /* the centre square, h x h */
    for (int y = 0; y < size; y++) {
        const uint16_t *row = camera + (y * h / size) * w + x0;
        for (int x = 0; x < size; x++, rgb += 3) {
            rgb565_to_888(row[x * h / size], rgb);
        }
    }
}

/* White balance of the ISP output: the pack's OV5675 calibration has no
   working AWB (with it on the picture is grey, with it off and Alif's colour
   matrix it is purple), so the CPU does a gray-world balance on the way to
   the model input: per colour a lookup table with the gain that makes the
   channel means equal, a tenth of the way per frame. */
static uint8_t wb_lut[3][256];
static uint8_t wb_q7[3];  /* the gains in 1.7 fixed point, for the Helium path */
static float   wb_gain[3];

static void wb_build(void)
{
    for (int c = 0; c < 3; c++) {
        for (int v = 0; v < 256; v++) {
            const int o = (int)((float)v * wb_gain[c] + 0.5f);
            wb_lut[c][v] = (uint8_t)(o > 255 ? 255 : o);
        }
        const int q = (int)(wb_gain[c] * 128.0f + 0.5f);
        wb_q7[c] = (uint8_t)(q > 255 ? 255 : q);
    }
}

/* v * gain / 128, saturated: the even and odd lanes widened to 16 bits, then narrowed back in place. */
static inline uint8x16_t gain_q7(uint8x16_t v, uint8_t gain)
{
    const uint8x16_t g = vdupq_n_u8(gain);
    const uint8x16_t even = vqshrnbq_n_u16(vdupq_n_u8(0), vmullbq_int_u8(v, g), 7);
    return vqshrntq_n_u16(even, vmulltq_int_u8(v, g), 7);
}

/* Turned 180 degrees with Helium: input pixel i is output pixel n - 1 - i.
   16 input pixels a step, read top to bottom, so the read stays ahead of the
   ISP writing the next frame into the same buffer; stored backwards and
   interleaved by three scatter stores. 3.2 ms for 416 x 416 (the LUT and
   tile walk of the other turns: 8.1 ms). */
static void planar_to_input_180(const uint8_t *r, const uint8_t *g, const uint8_t *b, uint8_t *rgb, int n,
                                uint32_t sums[3])
{
    static const uint8_t backwards[16] = {45, 42, 39, 36, 33, 30, 27, 24, 21, 18, 15, 12, 9, 6, 3, 0};
    const uint8x16_t offset = vld1q_u8(backwards);
    const uint8_t qr = wb_q7[0], qg = wb_q7[1], qb = wb_q7[2];
    uint32_t sr = 0U, sg = 0U, sb = 0U;
    uint8_t *d = rgb + 3 * (n - 16);
    for (int i = 0; i < n; i += 16, d -= 48) {
        const uint8x16_t vr = vld1q_u8(r + i), vg = vld1q_u8(g + i), vb = vld1q_u8(b + i);
        sr = vaddvaq_u8(sr, vr);
        sg = vaddvaq_u8(sg, vg);
        sb = vaddvaq_u8(sb, vb);
        vstrbq_scatter_offset_u8(d, offset, gain_q7(vr, qr));
        vstrbq_scatter_offset_u8(d + 1, offset, gain_q7(vg, qg));
        vstrbq_scatter_offset_u8(d + 2, offset, gain_q7(vb, qb));
    }
    sums[0] = sr;
    sums[1] = sg;
    sums[2] = sb;
}

void image_planar_gains(float gains[3])
{
    for (int c = 0; c < 3; c++) {
        gains[c] = wb_gain[c];
    }
}

void image_planar_to_input(const uint8_t *planes, int w, int h, uint8_t *rgb, int size, int quarter_turns)
{
    if (wb_gain[1] == 0.0f) {
        wb_gain[0] = wb_gain[1] = wb_gain[2] = 1.0f;
        wb_build();
    }
    const uint8_t *r = planes, *g = planes + w * h, *b = planes + 2 * w * h;
    const uint8_t *lr = wb_lut[0], *lg = wb_lut[1], *lb = wb_lut[2];
    uint32_t sr = 0U, sg = 0U, sb = 0U;
    quarter_turns &= 3;
    if (w == size && h == size && quarter_turns == 2 && (size * size) % 16 == 0) {
        uint32_t sums[3];
        planar_to_input_180(r, g, b, rgb, size * size, sums);
        sr = sums[0];
        sg = sums[1];
        sb = sums[2];
    } else if (w == size && h == size && quarter_turns != 0) {
        /* Turned counter-clockwise: output (x, y) is input pixel
           base + x * dx + y * dy. In 32 x 32 tiles: a column walk over whole
           rows misses the D-cache on every pixel (34 ms a frame; tiled 8 ms). */
        int base, dx, dy;
        switch (quarter_turns) {
        case 1:  base = size - 1;         dx = size;  dy = -1;    break;  /* 90 degrees  */
        case 2:  base = size * size - 1;  dx = -1;    dy = -size; break;  /* 180 degrees */
        default: base = (size - 1) * size; dx = -size; dy = 1;    break;  /* 270 degrees */
        }
        enum { T = 32 };
        for (int ty = 0; ty < size; ty += T) {
            for (int tx = 0; tx < size; tx += T) {
                for (int y = ty; y < ty + T && y < size; y++) {
                    uint8_t *d = rgb + (y * size + tx) * 3;
                    int i = base + tx * dx + y * dy;
                    for (int x = tx; x < tx + T && x < size; x++, d += 3, i += dx) {
                        const uint8_t vr = r[i], vg = g[i], vb = b[i];
                        sr += vr;
                        sg += vg;
                        sb += vb;
                        d[0] = lr[vr];
                        d[1] = lg[vg];
                        d[2] = lb[vb];
                    }
                }
            }
        }
    } else {
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++, rgb += 3) {
                const int i = (y * h / size) * w + x * w / size;
                const uint8_t vr = r[i], vg = g[i], vb = b[i];
                sr += vr;
                sg += vg;
                sb += vb;
                rgb[0] = lr[vr];
                rgb[1] = lg[vg];
                rgb[2] = lb[vb];
            }
        }
    }

    /* Next frame's gains: green stays, red and blue move a tenth of the way
       towards the green mean, within 0.5 .. 2. */
    const float mg = (float)sg + 1.0f;
    const float target[3] = {mg / ((float)sr + 1.0f), 1.0f, mg / ((float)sb + 1.0f)};
    for (int c = 0; c < 3; c += 2) {
        float t = target[c] < 0.5f ? 0.5f : (target[c] > 2.0f ? 2.0f : target[c]);
        wb_gain[c] += 0.1f * (t - wb_gain[c]);
    }
    wb_build();
}

void image_camera_to_view(const uint16_t *camera, int w, int h, uint8_t *panel)
{
    const int x0 = (w - h) / 2;
    for (int y = 0; y < IMAGE_VIEW && y < h; y++) {
        const uint16_t *row = camera + y * w + x0;
        uint8_t *d = panel + ((IMAGE_VIEW_TOP + y) * IMAGE_PANEL_W) * 3;
        for (int x = 0; x < IMAGE_VIEW; x++, d += 3) {
            rgb565_to_888(row[x], d);
        }
    }
}

void image_input_to_view(const uint8_t *rgb, int size, uint8_t *panel)
{
    /* 1:1, centred in the view: a row copy each, no scaling (the upscale to
       the full 480 width cost 10 ms a frame). The margins stay as they are. */
    const int x0 = (IMAGE_VIEW - size) / 2, y0 = IMAGE_VIEW_TOP + (IMAGE_VIEW - size) / 2;
    for (int y = 0; y < size; y++) {
        memcpy(panel + ((y0 + y) * IMAGE_PANEL_W + x0) * 3, rgb + y * size * 3, (size_t)size * 3U);
    }
}

void image_fill(uint8_t *panel, int x0, int y0, int x1, int y1, uint32_t rgb)
{
    const uint8_t r = (uint8_t)(rgb >> 16), g = (uint8_t)(rgb >> 8), b = (uint8_t)rgb;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > IMAGE_PANEL_W ? IMAGE_PANEL_W : x1;
    y1 = y1 > IMAGE_PANEL_H ? IMAGE_PANEL_H : y1;
    for (int y = y0; y < y1; y++) {
        uint8_t *p = panel + (y * IMAGE_PANEL_W + x0) * 3;
        for (int x = x0; x < x1; x++, p += 3) {
            p[0] = r;
            p[1] = g;
            p[2] = b;
        }
    }
}

void image_draw_detections(uint8_t *panel, const detections_t *det, int size, int view)
{
    const int t = 3;  /* line width */
    const uint32_t green = 0x00FF00U;
    const int x0 = (IMAGE_VIEW - view) / 2, y0 = IMAGE_VIEW_TOP + (IMAGE_VIEW - view) / 2;
    for (uint32_t i = 0; i < det->count; i++) {
        const detection_t *d = &det->det[i];
        const int x1 = x0 + (int)(d->x1 * view / size), x2 = x0 + (int)(d->x2 * view / size);
        int y1 = y0 + (int)(d->y1 * view / size);
        int y2 = y0 + (int)(d->y2 * view / size);
        y1 = y1 < y0 ? y0 : y1;  /* the boxes stay on the picture */
        y2 = y2 > y0 + view ? y0 + view : y2;
        image_fill(panel, x1, y1, x2, y1 + t, green);
        image_fill(panel, x1, y2 - t, x2, y2, green);
        image_fill(panel, x1, y1, x1 + t, y2, green);
        image_fill(panel, x2 - t, y1, x2, y2, green);
        image_fill(panel, x1, y1 + t, x1 + (int)(d->score * (float)(x2 - x1)), y1 + t + 8, green);
    }
}

void image_score_maps(uint8_t *panel, int x, int y, const detector_scores_t *scores, int size, int px)
{
    /* Colour per int8 score: black, blue, red, yellow; white at the threshold and above. */
    uint32_t lut[256];
    for (int q = -128; q < 128; q++) {
        float v = ((float)(q - scores->zero_point) * scores->scale) / scores->threshold;
        v = v < 0.0f ? 0.0f : v;
        uint32_t c;
        if (v >= 1.0f) {
            c = 0xFFFFFFU;
        } else {
            const int r = (int)(255.0f * (v < 0.25f ? 0.0f : (v < 0.6f ? (v - 0.25f) / 0.35f : 1.0f)));
            const int gr = (int)(255.0f * (v < 0.6f ? 0.0f : (v - 0.6f) / 0.4f));
            const int bl = (int)(255.0f * (v < 0.25f ? v / 0.25f : (v < 0.6f ? 1.0f - (v - 0.25f) / 0.35f : 0.0f)));
            c = ((uint32_t)r << 16) | ((uint32_t)gr << 8) | (uint32_t)bl;
        }
        lut[(uint8_t)q] = c;
    }
    /* The stride 8, 16 and 32 maps side by side, each drawn `px`-per-cell times
       the stride / 8, so all three are the same size. */
    int base = 0;
    for (int stride = 8; stride <= 32; stride *= 2) {
        const int cells = size / stride, cell = px * stride / 8;
        for (int cy = 0; cy < cells; cy++) {
            for (int cx = 0; cx < cells; cx++) {
                const int x0 = x + cx * cell, y0 = y + cy * cell;
                image_fill(panel, x0, y0, x0 + cell, y0 + cell, lut[(uint8_t)scores->score[base + cy * cells + cx]]);
            }
        }
        base += cells * cells;
        x += cells * cell + px * 4;
    }
}

/* The classic 5x7 font, ASCII 0x20 to 0x5A: five columns per glyph, bit 0 at the top. */
static const uint8_t font5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x08, 0x2A, 0x1C, 0x2A, 0x08}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31}, {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00},
    {0x00, 0x56, 0x36, 0x00, 0x00}, {0x00, 0x08, 0x14, 0x22, 0x41}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x41, 0x22, 0x14, 0x08, 0x00}, {0x02, 0x01, 0x51, 0x09, 0x06}, {0x32, 0x49, 0x79, 0x41, 0x3E},
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x01, 0x01},
    {0x3E, 0x41, 0x41, 0x51, 0x32}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x04, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x7F, 0x20, 0x18, 0x20, 0x7F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x51, 0x49, 0x45, 0x43},
};

int image_text(uint8_t *panel, int x, int y, int scale, const char *text, uint32_t rgb)
{
    for (; *text != '\0'; text++) {
        int c = (unsigned char)*text;
        if (c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        }
        if (c < 0x20 || c > 0x5A) {
            c = '?';
        }
        const uint8_t *glyph = font5x7[c - 0x20];
        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if (glyph[col] & (1U << row)) {
                    image_fill(panel, x + col * scale, y + row * scale, x + (col + 1) * scale, y + (row + 1) * scale, rgb);
                }
            }
        }
        x += 6 * scale;
    }
    return x;
}
