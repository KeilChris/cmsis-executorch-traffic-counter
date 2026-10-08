/* Copyright 2026 Arm Limited and/or its affiliates. SPDX-License-Identifier: Apache-2.0 */

#include <stddef.h>
#include <stdint.h>

#include "RTE_Components.h"
#include CMSIS_device_header

void ethosu_flush_dcache(const uint64_t *base_addr, const size_t *base_addr_size, int num_base_addr)
{
    (void)base_addr;
    (void)base_addr_size;
    if (num_base_addr > 0) {
        SCB_CleanDCache();
    } else {
        __DSB();
    }
}

void ethosu_invalidate_dcache(const uint64_t *base_addr, const size_t *base_addr_size, int num_base_addr)
{
    (void)base_addr;
    (void)base_addr_size;
    if (num_base_addr > 0) {
        SCB_CleanInvalidateDCache();
    } else {
        __DSB();
    }
}
