/* Copyright 2026 Arm Limited and/or its affiliates. SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>

#include "RTE_Components.h"
#include CMSIS_device_header
#include "ethosu_driver.h"

static struct ethosu_driver ethos_driver;

void NPU_IRQHandler(void)
{
    ethosu_irq_handler(&ethos_driver);
}

void ethos_setup(void)
{
    struct ethosu_hw_info info;

    SYS_UnlockReg();
    CLK_EnableModuleClock(NPU0_MODULE);
    SYS_LockReg();

    if (ethosu_init(&ethos_driver, (void *)NPU_BASE, NULL, 0, 1, 1) != 0) {
        printf("Failed to initialize Arm Ethos-U driver\n");
        return;
    }
    NVIC_EnableIRQ(NPU_IRQn);
    ethosu_get_hw_info(&ethos_driver, &info);
    printf("Ethos-U: v%u.%u.%u, %lu MACs/cycle, command stream v%u\n",
           info.version.arch_major_rev, info.version.arch_minor_rev,
           info.version.arch_patch_rev, (unsigned long)(1UL << info.cfg.macs_per_cc),
           info.cfg.cmd_stream_version);
}
