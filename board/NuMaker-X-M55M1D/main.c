/* Copyright 2026 Arm Limited and/or its affiliates. SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>

#include "RTE_Components.h"
#include CMSIS_device_header

#include "cmsis_os2.h"
#include "hyperram_code.h"
#include "main.h"

#ifndef APP_THREAD_STACK_SIZE
#define APP_THREAD_STACK_SIZE 0x8000
#endif

static uint64_t app_thread_stack[APP_THREAD_STACK_SIZE / 8]
    __attribute__((section(APP_POOL_SECTION), aligned(16)));

static const osThreadAttr_t app_thread_attr = {
    .name = "app",
    .stack_mem = app_thread_stack,
    .stack_size = sizeof(app_thread_stack),
    .priority = osPriorityNormal,
};

static void app_thread(void *argument)
{
    (void)argument;
    app_main();
    for (;;) {
        osDelay(osWaitForever);
    }
}

/*
 * Startup calls this hook before C/C++ data initialization. HyperRAM must be
 * in direct-map mode before the scatter loader initializes sections placed at
 * 0x82000000. Keep this routine and every function it calls in internal flash.
 */
void Reset_Handler_PreInit(void)
{
    const uint32_t slew = GPIO_SLEWCTL_FAST1;

    SYS_UnlockReg();
    CLK_EnableAPLL(CLK_APLLCTL_APLLSRC_HIRC, FREQ_220MHZ, CLK_APLL0_SELECT);
    CLK_SetSCLK(CLK_SCLKSEL_SCLKSEL_APLL0);
    CLK_SET_HCLK2DIV(2);
    CLK_SET_PCLK0DIV(2);
    CLK_SET_PCLK1DIV(2);
    CLK_SET_PCLK2DIV(2);
    CLK_SET_PCLK3DIV(2);
    CLK_SET_PCLK4DIV(2);

    /* Camera: PD12 power-down, PF7..11 data, PG/H control and sync pins.
       The BSP's software-I2C and sensor code assume SYS_Init enabled these. */
    CLK_EnableModuleClock(GPIOD_MODULE);
    CLK_EnableModuleClock(GPIOF_MODULE);
    CLK_EnableModuleClock(GPIOG_MODULE);
    CLK_EnableModuleClock(GPIOH_MODULE);
    CLK_EnableModuleClock(GPIOJ_MODULE);
    CLK_EnableModuleClock(CCAP0_MODULE);
    CLK_EnableModuleClock(SPIM0_MODULE);

    SET_SPIM0_CLK_PH13();
    SET_SPIM0_D2_PJ5();
    SET_SPIM0_D3_PJ6();
    SET_SPIM0_D4_PH14();
    SET_SPIM0_D5_PH15();
    SET_SPIM0_D6_PG13();
    SET_SPIM0_D7_PG14();
    SET_SPIM0_MISO_PJ4();
    SET_SPIM0_MOSI_PJ3();
    SET_SPIM0_RESETN_PJ2();
    SET_SPIM0_RWDS_PG15();
    SET_SPIM0_SS_PJ7();

    PG->SMTEN |= GPIO_SMTEN_SMTEN13_Msk | GPIO_SMTEN_SMTEN14_Msk | GPIO_SMTEN_SMTEN15_Msk;
    PH->SMTEN |= GPIO_SMTEN_SMTEN13_Msk | GPIO_SMTEN_SMTEN14_Msk | GPIO_SMTEN_SMTEN15_Msk;
    PJ->SMTEN |= GPIO_SMTEN_SMTEN2_Msk | GPIO_SMTEN_SMTEN3_Msk | GPIO_SMTEN_SMTEN4_Msk |
                 GPIO_SMTEN_SMTEN5_Msk | GPIO_SMTEN_SMTEN6_Msk | GPIO_SMTEN_SMTEN7_Msk;
    GPIO_SetSlewCtl(PG, BIT13 | BIT14 | BIT15, slew);
    GPIO_SetSlewCtl(PH, BIT13 | BIT14 | BIT15, slew);
    GPIO_SetSlewCtl(PJ, BIT2 | BIT3 | BIT4 | BIT5 | BIT6 | BIT7, slew);

    /* HyperRAM_Init calibrates at offset zero. The linker leaves the first
       4 KiB unused so this cannot damage the debugger-loaded model. */
    HyperRAM_Init(SPIM0);
    SPIM_HYPER_EnterDirectMapMode(SPIM0);
    SYS_LockReg();
}

int main(void)
{
    InitDebugUart();
    ethos_setup();

    osKernelInitialize();
    osThreadNew(app_thread, NULL, &app_thread_attr);
    osKernelStart();
    for (;;) {
    }
}
