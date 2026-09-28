/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The joystick, see joystick.h. The pins are not configured here beyond
 * their direction: the board's pin table (pins.h) has muxed P15_0..4 to the
 * low-power GPIO with their pull-ups, and the pin driver's SetPullResistor
 * rewrites a whole LPGPIO_CTRL register with the pad bits alone (the
 * register has a layout of its own, drivers/source/pinconf.c), which upset
 * the inputs: the line jumped about every frame and the panel seemed to
 * flicker. A switch has to read the same for three frames in a row before
 * it counts, against bounce and noise.
 */

#include "joystick.h"

#ifdef APP_HAS_JOYSTICK  /* boards without the switches build an empty file */

#include "Driver_GPIO.h"
#include "board_defs.h"

extern ARM_DRIVER_GPIO Driver_GPIO;

#ifndef TRAFFIC_JOY_TURN_180
#ifdef IMAGE_PANEL_TURN_180
#define TRAFFIC_JOY_TURN_180 IMAGE_PANEL_TURN_180
#else
#define TRAFFIC_JOY_TURN_180 0
#endif
#endif

#define GPIO_PIN_NUMBER(port, pin) ((8U * (port)) + (pin))

static const struct {
    uint32_t pin;
    uint32_t bit;
} joystick_pins[] = {
#if TRAFFIC_JOY_TURN_180
    {GPIO_PIN_NUMBER(BOARD_JOY_SW_A_GPIO_PORT, BOARD_JOY_SW_A_GPIO_PIN), JOY_DOWN},
    {GPIO_PIN_NUMBER(BOARD_JOY_SW_B_GPIO_PORT, BOARD_JOY_SW_B_GPIO_PIN), JOY_UP},
    {GPIO_PIN_NUMBER(BOARD_JOY_SW_C_GPIO_PORT, BOARD_JOY_SW_C_GPIO_PIN), JOY_RIGHT},
    {GPIO_PIN_NUMBER(BOARD_JOY_SW_D_GPIO_PORT, BOARD_JOY_SW_D_GPIO_PIN), JOY_LEFT},
#else
    {GPIO_PIN_NUMBER(BOARD_JOY_SW_A_GPIO_PORT, BOARD_JOY_SW_A_GPIO_PIN), JOY_UP},
    {GPIO_PIN_NUMBER(BOARD_JOY_SW_B_GPIO_PORT, BOARD_JOY_SW_B_GPIO_PIN), JOY_DOWN},
    {GPIO_PIN_NUMBER(BOARD_JOY_SW_C_GPIO_PORT, BOARD_JOY_SW_C_GPIO_PIN), JOY_LEFT},
    {GPIO_PIN_NUMBER(BOARD_JOY_SW_D_GPIO_PORT, BOARD_JOY_SW_D_GPIO_PIN), JOY_RIGHT},
#endif
    {GPIO_PIN_NUMBER(BOARD_JOY_SW_CENTER_GPIO_PORT, BOARD_JOY_SW_CENTER_GPIO_PIN), JOY_SELECT},
};

#define JOYSTICK_SETTLE 3U  /* equal reads in a row before a switch counts */

static uint32_t joystick_last;     /* the last debounced state */
static uint32_t joystick_raw[JOYSTICK_SETTLE];
static uint32_t joystick_raw_idx;

int32_t joystick_init(void)
{
    int32_t status = 0;
    for (unsigned i = 0; i < sizeof(joystick_pins) / sizeof(joystick_pins[0]); i++) {
        if (Driver_GPIO.SetDirection(joystick_pins[i].pin, ARM_GPIO_INPUT) != ARM_DRIVER_OK) {
            status = 1;
        }
    }
    return status;
}

uint32_t joystick_read(void)
{
    uint32_t held = 0U;
    for (unsigned i = 0; i < sizeof(joystick_pins) / sizeof(joystick_pins[0]); i++) {
        if (Driver_GPIO.GetInput(joystick_pins[i].pin) == 0U) {  /* to ground when pressed */
            held |= joystick_pins[i].bit;
        }
    }
    joystick_raw[joystick_raw_idx] = held;
    joystick_raw_idx = (joystick_raw_idx + 1U) % JOYSTICK_SETTLE;
    /* A bit is set (or clear) only when every recent read agrees. */
    uint32_t all = ~0U, any = 0U;
    for (unsigned i = 0; i < JOYSTICK_SETTLE; i++) {
        all &= joystick_raw[i];
        any |= joystick_raw[i];
    }
    return (joystick_last & any) | all;  /* keep a bit while reads disagree, set it when all agree */
}

uint32_t joystick_pressed(void)
{
    const uint32_t held = joystick_read();
    const uint32_t pressed = held & ~joystick_last;
    joystick_last = held;
    return pressed;
}

uint32_t joystick_held(void)
{
    return joystick_last;
}

#endif /* APP_HAS_JOYSTICK */
