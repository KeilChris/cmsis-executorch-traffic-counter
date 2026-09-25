/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Ethos-U driver's semaphores on RTX (strong overrides of the driver's
 * weak ones). The driver's own wait for the end of an NPU job is a __WFE()
 * loop: the thread that runs the model keeps the CPU for the whole job, and
 * threads of lower priority, the display thread here, never run meanwhile.
 * With an RTX semaphore the thread blocks and the CPU is free during the job.
 *
 * The driver creates its semaphores in ethosu_init(), before the kernel runs,
 * and gives one before any thread exists. So each semaphore counts on its own
 * (with __WFE / __SEV, like the driver) until its first take from a thread
 * with the kernel running creates the RTX semaphore and hands the count over.
 */

#include <stdint.h>

#include "RTE_Components.h"
#include CMSIS_device_header

#include "cmsis_os2.h"
#include "ethosu_driver.h"

#define SEMAPHORES 4U

typedef struct {
    volatile uint32_t count;     /* gives before the RTX semaphore exists */
    osSemaphoreId_t volatile id; /* the RTX semaphore, NULL until the first take in a thread */
} semaphore_t;

static semaphore_t semaphores[SEMAPHORES];
static uint32_t semaphores_used;

static int in_thread_with_kernel(void)
{
    return __get_IPSR() == 0U && osKernelGetState() == osKernelRunning;
}

void *ethosu_semaphore_create(void)
{
    if (semaphores_used == SEMAPHORES) {
        return NULL;
    }
    semaphore_t *s = &semaphores[semaphores_used++];
    s->count = 0U;
    s->id = NULL;
    return s;
}

void ethosu_semaphore_destroy(void *sem)
{
    semaphore_t *s = sem;
    if (s->id != NULL) {
        osSemaphoreDelete(s->id);
        s->id = NULL;
    }
}

int ethosu_semaphore_take(void *sem, uint64_t timeout)
{
    semaphore_t *s = sem;
    if (!in_thread_with_kernel()) {
        while (s->count == 0U) {
            __WFE();
        }
        __disable_irq();
        s->count--;
        __enable_irq();
        return 0;
    }
    if (s->id == NULL) {
        /* Created outside the critical section (no SVC with interrupts off);
           the gives counted so far move over once it is published. */
        osSemaphoreId_t id = osSemaphoreNew(16U, 0U, NULL);
        if (id == NULL) {
            return -1;
        }
        __disable_irq();
        const uint32_t pending = s->count;
        s->count = 0U;
        s->id = id;
        __enable_irq();
        for (uint32_t i = 0; i < pending; i++) {
            osSemaphoreRelease(id);
        }
    }
    const uint32_t ticks = timeout == ETHOSU_SEMAPHORE_WAIT_FOREVER ? osWaitForever : (uint32_t)timeout;
    return osSemaphoreAcquire(s->id, ticks) == osOK ? 0 : -1;
}

int ethosu_semaphore_give(void *sem)
{
    semaphore_t *s = sem;
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    osSemaphoreId_t id = s->id;
    if (id == NULL) {
        s->count++;
    }
    __set_PRIMASK(primask);
    if (id != NULL) {
        return osSemaphoreRelease(id) == osOK ? 0 : -1;
    }
    __SEV();
    return 0;
}
