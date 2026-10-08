/* Copyright 2026 Arm Limited and/or its affiliates. SPDX-License-Identifier: Apache-2.0 */

#include <stdbool.h>
#include <stdint.h>

#include "Display.h"
#include "board_display.h"

static volatile uint32_t frames_presented;

static int32_t present(const void *fb)
{
    const S_DISP_RECT full = {
        .u32TopLeftX = 0,
        .u32TopLeftY = 0,
        .u32BottonRightX = APP_DISPLAY_WIDTH - 1,
        .u32BottonRightY = APP_DISPLAY_HEIGHT - 1,
    };
    Display_FillRect((uint16_t *)(uintptr_t)fb, &full, 1);
    ++frames_presented;
    return 0;
}

int32_t display_init(void)
{
    return Display_Init();
}

int32_t display_start(const void *fb)
{
    return present(fb);
}

int32_t display_present(const void *fb)
{
    return present(fb);
}

uint32_t display_frame_count(void)
{
    return frames_presented;
}

int32_t display_wait_frame(uint32_t count)
{
    return frames_presented > count ? 0 : -1;
}

int32_t display_wait_shown(const void *fb)
{
    (void)fb;
    return 0;
}
