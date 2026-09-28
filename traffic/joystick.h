/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The board's joystick (five switches to ground on GPIO pins, pulled up),
 * read through the CMSIS GPIO driver. The pins come from the board's
 * board_defs.h (BOARD_JOY_SW_A .. D and CENTER); the pack's VIO driver maps
 * A = up, B = down, C = left, D = right. TRAFFIC_JOY_TURN_180 swaps both
 * axes for a panel that is shown turned by 180 degrees (image.c's
 * IMAGE_PANEL_TURN_180), so "left" on the stick is left on the picture.
 */

#ifndef TRAFFIC_JOYSTICK_H_
#define TRAFFIC_JOYSTICK_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JOY_UP     (1U << 0)
#define JOY_DOWN   (1U << 1)
#define JOY_LEFT   (1U << 2)
#define JOY_RIGHT  (1U << 3)
#define JOY_SELECT (1U << 4)

/* Configure the pins as inputs (the pull-ups come from the pin table). 0 on success. */
int32_t joystick_init(void);

/* Read the switches (once per frame): the debounced JOY_* bits held down now. */
uint32_t joystick_read(void);

/* Read the switches: the ones pressed since the last call (down now, up then). */
uint32_t joystick_pressed(void);

/* The debounced state of the last read, without reading again. */
uint32_t joystick_held(void);

#ifdef __cplusplus
}
#endif

#endif /* TRAFFIC_JOYSTICK_H_ */
