/* Copyright 2026 Arm Limited and/or its affiliates. SPDX-License-Identifier: Apache-2.0 */
#ifndef CAMERA_HIL_RUNNER_H
#define CAMERA_HIL_RUNNER_H
#include <stdint.h>

enum {
    CAMERA_HIL_IDLE = 1,
    CAMERA_HIL_RUNNING = 2,
    CAMERA_HIL_NORMAL_PASS = 3,
    CAMERA_HIL_QUARANTINE_PASS = 4,
    CAMERA_HIL_ABSENT_PASS = 5,
    CAMERA_HIL_INCONCLUSIVE = 254,
    CAMERA_HIL_FAILED = 255
};
typedef struct {
    uint32_t magic;            /* 0x43415032: this diagnostic image, not traffic */
    uint32_t command;          /* debugger: 1=normal/abort, 2=zero-budget abort,
                                 3=absent sensor (cold boot without module only) */
    uint32_t phase;            /* enum above; no automatic tests at startup */
    uint32_t step;             /* current assertion; see README */
    int32_t actual;
    int32_t expected;
    uint32_t passed;           /* successful assertions, cumulative this boot */
    uint32_t heartbeat;        /* separate RTOS thread, NOT proof of USB I/O */
    uint32_t irq_count;
    uint32_t events;
    uint32_t operation_ticks;  /* most recent init/abort elapsed kernel ticks */
    uint32_t tick_hz;
    uint32_t reload_required;  /* never reuse quarantined buffer, even if DMA later stops */
} camera_hil_status_t;
extern volatile camera_hil_status_t camera_hil_status;
#endif
