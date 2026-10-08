/* Copyright 2026 Arm Limited and/or its affiliates. SPDX-License-Identifier: Apache-2.0 */
#ifndef CAMERA_HIL_TEST
#error "This file belongs only in the isolated camera-hil project"
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "RTE_Components.h"
#include CMSIS_device_header
#include "cmsis_os2.h"
#include "ImageSensor.h"
#include "Sensor.h"
#include "rec_play.h"
#include "runner.h"

#if !defined(IMAGE_SENSOR_CAPTURE_CONTRACT_VERSION) || IMAGE_SENSOR_CAPTURE_CONTRACT_VERSION < 2
#error "Camera HIL requires the version-2 camera contract"
#endif

#define FRAME_BYTES (416U * 416U * 2U)
#define GUARD_BYTES 4096U
#define ABORT_POLLS 32000000U /* iterations, not milliseconds; bounded */
#if defined(CAMERA_HIL_HOST_TEST)
#define MEMORY __attribute__((aligned(32)))
#else
#define MEMORY __attribute__((aligned(32), section(".bss.hyperram")))
#endif
typedef struct {
    uint8_t before[GUARD_BYTES];
    uint8_t frame[FRAME_BYTES];
    uint8_t after[GUARD_BYTES];
} guarded_frame_t;
_Static_assert(FRAME_BYTES % 32U == 0U, "DMA/cache line isolation");
_Static_assert(sizeof(guarded_frame_t) % 32U == 0U, "whole cache lines");
static guarded_frame_t buffer_a MEMORY;
static guarded_frame_t buffer_b MEMORY;
static uint8_t snapshot[FRAME_BYTES] MEMORY;
static uint64_t heartbeat_stack[256] MEMORY;
volatile camera_hil_status_t camera_hil_status;

static uint32_t milliseconds(uint32_t ms)
{
    uint32_t ticks = (ms * osKernelGetTickFreq() + 999U) / 1000U;
    return ticks ? ticks : 1U;
}

static bool expect(uint32_t step, int32_t actual, int32_t expected)
{
    camera_hil_status.step = step;
    camera_hil_status.actual = actual;
    camera_hil_status.expected = expected;
    if (actual != expected) {
        camera_hil_status.phase = CAMERA_HIL_FAILED;
        camera_hil_status.reload_required = 1;
        return false; /* retain buffers/state; no speculative abort/recovery */
    }
    ++camera_hil_status.passed;
    return true;
}

#define EXPECT(check_id, expression, wanted) do { \
    camera_hil_status.step = (check_id); \
    if (!expect((check_id), (expression), (wanted))) return; \
} while (0)

static bool bytes_equal(const uint8_t *p, size_t n, uint8_t value)
{
    for (size_t i = 0; i < n; ++i) if (p[i] != value) return false;
    return true;
}

static void prepare(guarded_frame_t *b)
{
    memset(b->before, 0xA5, sizeof(b->before));
    memset(b->frame, 0xCC, sizeof(b->frame));
    memset(b->after, 0x5A, sizeof(b->after));
    SCB_CleanInvalidateDCache_by_Addr(b, sizeof(*b));
}

/* Only call after ownership release, or on a buffer never submitted to DMA. */
static bool guards_ok(guarded_frame_t *b)
{
    SCB_InvalidateDCache_by_Addr(b, sizeof(*b));
    return bytes_equal(b->before, sizeof(b->before), 0xA5) &&
           bytes_equal(b->after, sizeof(b->after), 0x5A);
}

static uint32_t address(guarded_frame_t *b)
{
    return (uint32_t)(uintptr_t)b->frame;
}

static void event(uint32_t events)
{
    ++camera_hil_status.irq_count;
    camera_hil_status.events |= events;
}

static int completion(void)
{
    uint32_t start = osKernelGetTickCount();
    int result;
    do {
        result = ImageSensor_PollCaptureDone();
        if (result != IMAGE_SENSOR_BUSY) return result;
        osDelay(1U);
    } while ((uint32_t)(osKernelGetTickCount() - start) < milliseconds(1000U));
    return IMAGE_SENSOR_TIMEOUT; /* buffer remains owned; caller fails closed */
}

static bool stable(guarded_frame_t *b)
{
    if (!guards_ok(b)) return false;
    memcpy(snapshot, b->frame, FRAME_BYTES);
    osDelay(milliseconds(250U));
    if (!guards_ok(b)) return false;
    return memcmp(snapshot, b->frame, FRAME_BYTES) == 0;
}

static int32_t synthetic_init_failure(uint32_t parameter)
{
    (void)parameter;
    return 0; /* sensor-descriptor FALSE: NOT an electrical I2C NACK test */
}

static void normal_tests(void)
{
    int result;
    PFN_INIT_SENSOR_FUNC real_init = g_sSensorHM1055_VGA_YUV422.pfnInitSensor;
    camera_hil_status.phase = CAMERA_HIL_RUNNING;
    EXPECT(101, ImageSensor_Config(eIMAGE_FMT_RGB565,416,416,true), IMAGE_SENSOR_NOT_READY);
    EXPECT(102, ImageSensor_TriggerCapture(0), IMAGE_SENSOR_NOT_READY);
    g_sSensorHM1055_VGA_YUV422.pfnInitSensor = synthetic_init_failure;
    camera_hil_status.step = 103;
    result = ImageSensor_Init();
    g_sSensorHM1055_VGA_YUV422.pfnInitSensor = real_init; /* always restore */
    EXPECT(103, result, IMAGE_SENSOR_ERROR);
    EXPECT(104, ImageSensor_Config(eIMAGE_FMT_RGB565,416,416,true), IMAGE_SENSOR_NOT_READY);
    uint32_t start = osKernelGetTickCount();
    camera_hil_status.step = 105;
    result = ImageSensor_Init();
    camera_hil_status.operation_ticks = osKernelGetTickCount() - start;
    EXPECT(105, result, IMAGE_SENSOR_OK);
    EXPECT(106, ImageSensor_Config(eIMAGE_FMT_RGB565,0,416,true), IMAGE_SENSOR_INVALID);
    EXPECT(107, ImageSensor_Config(eIMAGE_FMT_RGB565,415,416,true), IMAGE_SENSOR_INVALID);
    EXPECT(108, ImageSensor_Config(eIMAGE_FMT_ONLY_Y_1BIT,416,416,true), IMAGE_SENSOR_INVALID);
    EXPECT(109, ImageSensor_Config(eIMAGE_FMT_RGB565,416,416,true), IMAGE_SENSOR_OK);
    EXPECT(110, ImageSensor_TriggerCapture(0), IMAGE_SENSOR_INVALID);
    prepare(&buffer_a);
    prepare(&buffer_b);
    EXPECT(111, ImageSensor_TriggerCapture(address(&buffer_a) + 1U), IMAGE_SENSOR_INVALID);
    EXPECT(112, ImageSensor_TriggerCapture(address(&buffer_a)), IMAGE_SENSOR_OK);
    EXPECT(113, ImageSensor_TriggerCapture(address(&buffer_b)), IMAGE_SENSOR_BUSY);
    EXPECT(114, ImageSensor_Init(), IMAGE_SENSOR_BUSY);
    EXPECT(115, ImageSensor_Config(eIMAGE_FMT_RGB565,416,416,true), IMAGE_SENSOR_BUSY);
    EXPECT(116, completion(), IMAGE_SENSOR_OK);
    EXPECT(117, stable(&buffer_a), true);
    EXPECT(118, guards_ok(&buffer_b) && bytes_equal(buffer_b.frame, FRAME_BYTES, 0xCC), true);
    /* Avoid passing on a no-DMA frame that merely retained its initial fill. */
    EXPECT(119, !bytes_equal(buffer_a.frame, FRAME_BYTES, 0xCC), true);
    EXPECT(120, ImageSensor_PollCaptureDone(), IMAGE_SENSOR_NOT_READY);
    EXPECT(121, ImageSensor_AbortCapture(0), IMAGE_SENSOR_OK);

    prepare(&buffer_a);
    EXPECT(122, ImageSensor_TriggerCapture(address(&buffer_a)), IMAGE_SENSOR_OK);
    start = osKernelGetTickCount();
    camera_hil_status.step = 123;
    result = ImageSensor_AbortCapture(ABORT_POLLS);
    camera_hil_status.operation_ticks = osKernelGetTickCount() - start;
    EXPECT(123, result, IMAGE_SENSOR_OK);
    EXPECT(124, stable(&buffer_a), true);
    /* Snapshot A remains stable while a subsequent real capture uses B. */
    EXPECT(125, ImageSensor_TriggerCapture(address(&buffer_b)), IMAGE_SENSOR_OK);
    EXPECT(126, completion(), IMAGE_SENSOR_OK);
    EXPECT(127, guards_ok(&buffer_b), true);
    EXPECT(128, guards_ok(&buffer_a) && memcmp(snapshot, buffer_a.frame, FRAME_BYTES) == 0, true);
    EXPECT(129, !bytes_equal(buffer_b.frame, FRAME_BYTES, 0xCC), true);
    camera_hil_status.phase = CAMERA_HIL_NORMAL_PASS;
}

static void quarantine_test(void)
{
    camera_hil_status.phase = CAMERA_HIL_RUNNING;
    prepare(&buffer_a);
    prepare(&buffer_b);
    EXPECT(201, ImageSensor_TriggerCapture(address(&buffer_a)), IMAGE_SENSOR_OK);
    camera_hil_status.step = 202;
    uint32_t start = osKernelGetTickCount();
    int result = ImageSensor_AbortCapture(0);
    camera_hil_status.operation_ticks = osKernelGetTickCount() - start;
    if (result == IMAGE_SENSOR_OK) {
        /* Frame already stopped: valid behavior, but not a timeout test. */
        camera_hil_status.actual = result;
        camera_hil_status.expected = IMAGE_SENSOR_TIMEOUT;
        camera_hil_status.phase = CAMERA_HIL_INCONCLUSIVE;
        camera_hil_status.reload_required = 1;
        return;
    }
    EXPECT(202, result, IMAGE_SENSOR_TIMEOUT);
    camera_hil_status.reload_required = 1;
    /* NEVER read/clean/invalidate/overwrite A after the failed abort. It stays
     * reserved until reload, even if the one-shot DMA completes later. */
    EXPECT(203, ImageSensor_TriggerCapture(address(&buffer_b)), IMAGE_SENSOR_FAULT);
    EXPECT(204, ImageSensor_Config(eIMAGE_FMT_RGB565,416,416,true), IMAGE_SENSOR_FAULT);
    EXPECT(205, ImageSensor_Init(), IMAGE_SENSOR_FAULT);
    EXPECT(206, ImageSensor_PollCaptureDone(), IMAGE_SENSOR_FAULT);
    EXPECT(207, ImageSensor_AbortCapture(ABORT_POLLS), IMAGE_SENSOR_FAULT);
    osDelay(milliseconds(500U));
    EXPECT(208, ImageSensor_TriggerCapture(address(&buffer_b)), IMAGE_SENSOR_FAULT);
    EXPECT(209, guards_ok(&buffer_b) && bytes_equal(buffer_b.frame, FRAME_BYTES, 0xCC), true);
    camera_hil_status.phase = CAMERA_HIL_QUARANTINE_PASS;
}

static void absent_sensor_test(void)
{
    camera_hil_status.phase = CAMERA_HIL_RUNNING;
    uint32_t start = osKernelGetTickCount();
    camera_hil_status.step = 301;
    int result = ImageSensor_Init(); /* actual sensor I2C, no injected callback */
    camera_hil_status.operation_ticks = osKernelGetTickCount() - start;
    EXPECT(301, result, IMAGE_SENSOR_ERROR);
    EXPECT(302, ImageSensor_Config(eIMAGE_FMT_RGB565,416,416,true), IMAGE_SENSOR_NOT_READY);
    EXPECT(303, ImageSensor_TriggerCapture(address(&buffer_a)), IMAGE_SENSOR_NOT_READY);
    EXPECT(304, camera_hil_status.operation_ticks < milliseconds(1000U), true);
    camera_hil_status.reload_required = 1;
    camera_hil_status.phase = CAMERA_HIL_ABSENT_PASS;
}

static void heartbeat(void *argument)
{
    (void)argument;
    for (;;) {
        ++camera_hil_status.heartbeat;
        osDelay(milliseconds(10U));
    }
}

int app_main(void)
{
    static const osThreadAttr_t attr = {
        .name = "cameraTestHeartbeat",
        .stack_mem = heartbeat_stack,
        .stack_size = sizeof(heartbeat_stack),
        .priority = osPriorityNormal1,
    };
    camera_hil_status.magic = 0x43415032;
    camera_hil_status.tick_hz = osKernelGetTickFreq();
    if (!expect(1, osThreadNew(heartbeat, NULL, &attr) != NULL, true)) return -1;
    ImageSensor_SetEventCallback(event);
    rec_play_init(416U * 416U * 3U); /* control/flags only; no diagnostic image streams */
    camera_hil_status.phase = CAMERA_HIL_IDLE;
    for (;;) {
        uint32_t command = camera_hil_status.command;
        camera_hil_status.command = 0;
        if (command && !camera_hil_status.reload_required) {
            if (command == 1 && camera_hil_status.phase == CAMERA_HIL_IDLE) normal_tests();
            else if (command == 2 && camera_hil_status.phase == CAMERA_HIL_NORMAL_PASS) quarantine_test();
            else if (command == 3 && camera_hil_status.phase == CAMERA_HIL_IDLE) absent_sensor_test();
            else (void)expect(2, command, 0); /* wrong sequence: fail closed */
        }
        osDelay(milliseconds(10U));
    }
}
