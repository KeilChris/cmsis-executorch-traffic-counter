/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Ethos-U85 side of the Quake port: see port_npu.cpp.
 */
#ifndef QUAKE_PORT_NPU_H
#define QUAKE_PORT_NPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Run qpresent and qshade on synthetic planes and print their timings.
 * 0, or -1 when a method failed. Needs the DWT cycle counter running. */
int port_npu_measure(void);

/* The NPU present path: load qpresent once, then show frames of
 * QUAKE_VID_WIDTH x QUAKE_VID_HEIGHT palette indices. 0, or -1 on an error. */
int port_npu_present_init(void);
int port_npu_present(const uint8_t *frame, const uint8_t *palette_rgb);

/* Pipelined: port_npu_rtos_init() once, from a thread, before the first NPU
 * job (RTX semaphores for the driver, the present thread). The present call
 * returns when the planes are handed over; the NPU and the display are driven
 * by the present thread while the caller renders the next frame. */
int port_npu_rtos_init(void);
void port_npu_present_pipelined(const uint8_t *frame, const uint8_t *palette_rgb);
void port_npu_present_drain(void);

/* Deferred texturing (qfetch, Ethos-U85 only): the span drawer stores texel
 * offsets, the NPU fetches the texels from the surface cache, resolves the
 * palette, rotates and scales. port_npu_fetch_offsets() is the plane the span
 * drawer writes (NULL where the AI layer has no qfetch); port_npu_fetch_select()
 * switches the present thread between qpresent (0) and qfetch (1), -1 on an
 * error; port_npu_present_fetch() shows a frame drawn that way and returns
 * when the NPU job has its copy of the cache, the offsets and the frame. */
uint32_t *port_npu_fetch_offsets(void);
int port_npu_fetch_select(int fetch);
void port_npu_present_fetch(const uint8_t *frame, const uint8_t *palette_rgb, const uint8_t *surface_cache);

/* DWT cycles of the hand-off pass and of the execute call, accumulated; the caller resets them. */
extern uint32_t npu_handoff_cycles, npu_execute_cycles;
/* Of the execute time: the NPU job (command stream start to interrupt) and the backend's IO copies. */
extern uint32_t npu_busy_cycles, npu_copy_cycles;

#ifdef __cplusplus
}
#endif

#endif /* QUAKE_PORT_NPU_H */
