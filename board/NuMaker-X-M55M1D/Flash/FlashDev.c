/* Copyright 2026 Arm Limited and/or its affiliates. SPDX-License-Identifier: Apache-2.0 */

#include "FlashOS.h"

struct FlashDevice const FlashDevice = {
    FLASH_DRV_VERS,
    "NuMaker-X-M55M1D HyperRAM",
    EXTSPI,
    0x82000000,
    0x00800000,
    1024,
    0,
    0x00,
    1000,
    1000,
    0x1000, 0x000000,
    SECTOR_END
};
