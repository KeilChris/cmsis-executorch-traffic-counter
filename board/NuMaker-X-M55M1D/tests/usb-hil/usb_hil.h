/* Copyright 2026 Arm Limited and/or its affiliates. SPDX-License-Identifier: Apache-2.0 */
#ifndef USB_HIL_H
#define USB_HIL_H
#include <stdint.h>

#define HIL_MAGIC 0x5548494cU
#define HIL_VERSION 1U
#define HIL_CAPACITY 8192U
#define HIL_OUT 0x01U
#define HIL_IN  0x81U
#define HIL_STATUS 0x30U
#define HIL_ARM_OUT 0x31U
#define HIL_ARM_IN 0x32U
#define HIL_ABORT 0x33U
#define HIL_LOOPBACK 0x34U
#define HIL_REARM 0x8000U

#define HIL_ERR_DRIVER     (1U << 0)
#define HIL_ERR_GUARD      (1U << 1)
#define HIL_ERR_COUNT      (1U << 2)
#define HIL_ERR_PATTERN    (1U << 3)
#define HIL_ERR_CALLBACK   (1U << 4)
#define HIL_ERR_LATE_WRITE (1U << 5)

/* Exactly 64 little-endian bytes on the wire; errors are sticky until reload. */
typedef struct {
    uint32_t magic, version, configured, mps, resets, case_id, mode, active;
    uint32_t requested, out_count, in_count, out_callbacks, in_callbacks;
    uint32_t aborts, errors;
    int32_t last_driver_error;
} usb_hil_status_t;
extern volatile usb_hil_status_t usb_hil_status;
int32_t usb_hil_start(void);
#endif
