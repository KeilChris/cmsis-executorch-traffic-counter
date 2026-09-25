/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The boot screen on the panel: text lines and a progress bar while the
 * firmware fetches the pak (port_splash.c). Needs video_init() done.
 */
#ifndef QUAKE_PORT_SPLASH_H
#define QUAKE_PORT_SPLASH_H

#ifdef __cplusplus
extern "C" {
#endif

void port_splash_begin(void);          /* clear the panel, title */
void port_splash_line(const char *s);  /* one more text line (capitals, digits, .:/%) */
void port_splash_progress(int percent); /* the bar; -1 keeps the last value; shows the picture */
void port_splash_end(void);            /* Quake draws from now on */
int  port_splash_active(void);

#ifdef __cplusplus
}
#endif

#endif /* QUAKE_PORT_SPLASH_H */
