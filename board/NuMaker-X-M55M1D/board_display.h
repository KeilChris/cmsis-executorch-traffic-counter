/* Copyright 2026 Arm Limited and/or its affiliates. SPDX-License-Identifier: Apache-2.0 */
#ifndef NUMAKER_BOARD_DISPLAY_H_
#define NUMAKER_BOARD_DISPLAY_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t display_init(void);
int32_t display_start(const void *fb);
int32_t display_present(const void *fb);
uint32_t display_frame_count(void);
int32_t display_wait_frame(uint32_t count);
int32_t display_wait_shown(const void *fb);

#ifdef __cplusplus
}
#endif

#endif
