/* Copyright 2026 Arm Limited and/or its affiliates. SPDX-License-Identifier: Apache-2.0 */

#include <stdarg.h>
#include <stdint.h>

#include "NuMicro.h"
#include "hyperram_code.h"

#define HYPERRAM_BASE 0x82000000UL
#define HYPERRAM_SIZE 0x00800000UL
#define LOADER_SCLK_HZ 96000000UL
#define CLK_APLLCTL_APLLSRC_HIRC 0x00000002UL

uint32_t SystemCoreClock = LOADER_SCLK_HZ;

/* The BSP helper only prints the selected DLL delay. A flash algorithm has no
 * console, so satisfy that reference without pulling in semihosting. */
int loader_printf(const char *format, ...)
{
    (void)format;
    return 0;
}

/* Keep the loader independent of system_M55M1.c and the complete clock
 * driver. HyperRAM initialization only needs these three small helpers. */
uint32_t CLK_GetSCLKFreq(void)
{
    return LOADER_SCLK_HZ;
}

void CLK_EnableModuleClock(uint64_t module)
{
    uint32_t address = (uint32_t)MODULE_CLKCTL_BASE +
                       (uint32_t)(MODULE_CLKCTL(module) << 2);
    *(volatile uint32_t *)address |= 1UL << (uint32_t)MODULE_CLKEN_Pos(module);
}

void GPIO_SetSlewCtl(GPIO_T *port, uint32_t pins, uint32_t mode)
{
    uint32_t pin;

    for (pin = 0; pin < GPIO_PIN_MAX; pin++) {
        if ((pins & (1UL << pin)) != 0U) {
            port->SLEWCTL = (port->SLEWCTL & ~(3UL << (pin << 1))) |
                            (mode << (pin << 1));
        }
    }
}

static int address_is_valid(unsigned long address, unsigned long size)
{
    return (address >= HYPERRAM_BASE) &&
           (size <= HYPERRAM_SIZE) &&
           (address <= (HYPERRAM_BASE + HYPERRAM_SIZE - size));
}

static int configure_hyperram(void)
{
    uint32_t timeout = 1000000U;

    SYS_UnlockReg();
    if (SYS_IsRegLocked() != 0U) {
        return 1;
    }

    /* Match the clock setup used by Nuvoton's supplied M55M1_SPIM algorithm.
     * HyperRAM timing calculation requires a useful SPIM clock; running it
     * from the 12 MHz reset clock underflows the CSMAXLT calculation. */
    CLK->SRCCTL |= CLK_SRCCTL_HIRCEN_Msk;
    while (((CLK->STATUS & CLK_STATUS_HIRCSTB_Msk) == 0U) && (--timeout != 0U)) {
    }
    if (timeout == 0U) {
        return 1;
    }
    if ((PMC->PLSTS & PMC_PLSTS_PLSTATUS_Msk) > PMC_PLSTS_PLSTATUS_PL1) {
        timeout = 1000000U;
        while (((PMC->PLCTL & PMC_PLCTL_WRBUSY_Msk) != 0U) &&
               (--timeout != 0U)) {
        }
        if (timeout == 0U) {
            return 1;
        }
        PMC->PLCTL = PMC_PLCTL_PLSEL_PL1;
        timeout = 1000000U;
        while (((PMC->PLSTS & PMC_PLSTS_PLCBUSY_Msk) != 0U) &&
               (--timeout != 0U)) {
        }
        if (timeout == 0U) {
            return 1;
        }
    }

    if ((CLK->SCLKSEL & CLK_SCLKSEL_SCLKSEL_Msk) ==
        CLK_SCLKSEL_SCLKSEL_APLL0) {
        CLK->SCLKSEL = CLK_SCLKSEL_SCLKSEL_HIRC;
    }
    CLK->SRCCTL &= ~CLK_SRCCTL_APLL0EN_Msk;
    CLK->APLL0CTL = 0x10008416U;
    CLK->APLL0SEL = CLK_APLLCTL_APLLSRC_HIRC;
    CLK->SRCCTL |= CLK_SRCCTL_APLL0EN_Msk;
    timeout = 1000000U;
    while (((CLK->STATUS & CLK_STATUS_APLL0STB_Msk) == 0U) &&
           (--timeout != 0U)) {
    }
    if (timeout == 0U) {
        return 1;
    }
    CLK->SCLKSEL = CLK_SCLKSEL_SCLKSEL_APLL0;
    CLK->SCLKDIV = 0U;

    CLK_EnableModuleClock(GPIOG_MODULE);
    CLK_EnableModuleClock(GPIOH_MODULE);
    CLK_EnableModuleClock(GPIOJ_MODULE);
    CLK_EnableModuleClock(SPIM0_MODULE);

    SYS->SPIMRST = SYS_SPIMRST_SPIM0RST_Msk;
    SYS->SPIMRST = 0U;

    SET_SPIM0_CLKN_PH12();
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

    PG->SMTEN |= GPIO_SMTEN_SMTEN13_Msk | GPIO_SMTEN_SMTEN14_Msk |
                 GPIO_SMTEN_SMTEN15_Msk;
    PH->SMTEN |= GPIO_SMTEN_SMTEN12_Msk | GPIO_SMTEN_SMTEN13_Msk |
                 GPIO_SMTEN_SMTEN14_Msk | GPIO_SMTEN_SMTEN15_Msk;
    PJ->SMTEN |= GPIO_SMTEN_SMTEN2_Msk | GPIO_SMTEN_SMTEN3_Msk |
                 GPIO_SMTEN_SMTEN4_Msk | GPIO_SMTEN_SMTEN5_Msk |
                 GPIO_SMTEN_SMTEN6_Msk | GPIO_SMTEN_SMTEN7_Msk;
    GPIO_SetSlewCtl(PG, BIT13 | BIT14 | BIT15, GPIO_SLEWCTL_FAST1);
    GPIO_SetSlewCtl(PH, BIT12 | BIT13 | BIT14 | BIT15, GPIO_SLEWCTL_FAST1);
    GPIO_SetSlewCtl(PJ, BIT2 | BIT3 | BIT4 | BIT5 | BIT6 | BIT7,
                    GPIO_SLEWCTL_FAST1);

    HyperRAM_Init(SPIM0);
    SPIM_HYPER_EnterDirectMapMode(SPIM0);
    SYS_LockReg();
    return 0;
}

int Init(unsigned long address, unsigned long clock, unsigned long function)
{
    (void)address;
    (void)clock;
    (void)function;

    /* Flash algorithms run without the application's Reset_Handler. Enable
     * the FPU before the BSP's timing helper performs floating-point math. */
    SCB->CPACR |= (3UL << 20) | (3UL << 22);
    __DSB();
    __ISB();
    SCB_DisableDCache();
    return configure_hyperram();
}

int UnInit(unsigned long function)
{
    (void)function;
    return 0;
}

int EraseChip(void)
{
    return 0;
}

int EraseSector(unsigned long address)
{
    return address_is_valid(address, 1U) ? 0 : 1;
}

int ProgramPage(unsigned long address, unsigned long size, unsigned char *data)
{
    volatile uint8_t *destination;
    unsigned long index;

    if (!address_is_valid(address, size)) {
        return 1;
    }

    destination = (volatile uint8_t *)address;
    for (index = 0; index < size; index++) {
        destination[index] = data[index];
    }
    __DSB();
    return 0;
}

unsigned long Verify(unsigned long address, unsigned long size, unsigned char *data)
{
    volatile const uint8_t *source;
    unsigned long index;

    if (!address_is_valid(address, size)) {
        return address;
    }

    source = (volatile const uint8_t *)address;
    for (index = 0; index < size; index++) {
        if (source[index] != data[index]) {
            return address + index;
        }
    }
    return address + size;
}
