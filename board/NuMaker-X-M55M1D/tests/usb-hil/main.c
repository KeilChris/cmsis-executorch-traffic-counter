/* Copyright 2026 Arm Limited and/or its affiliates. SPDX-License-Identifier: Apache-2.0 */
#include "RTE_Components.h"
#include CMSIS_device_header
#include "usb_hil.h"

int main(void) {
    /* DFP startup supplies the clocks and internal-memory initialization.
       The packaged driver owns USB power/PHY/IRQ setup. No HyperRAM or RTOS. */
    (void)usb_hil_start();
    for (;;) {
        __WFI();
    }
}
