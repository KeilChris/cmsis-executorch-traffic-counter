/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The camera of the traffic counter, on the Ensemble pack's Driver_CPI, see
 * camera.h. The buffer swap at VSYNC follows the pack's vstream_video_in.c
 * of the AppKit-E7 layer.
 */

#include <stddef.h>
#include <stdint.h>

#include "RTE_Components.h"
#include CMSIS_device_header

#if defined(APP_CAMERA_NUVOTON)

#include "ImageSensor.h"
#include "cmsis_os2.h"
#include "camera.h"

#define CAMERA_FRAME_BYTES (CAMERA_WIDTH * CAMERA_HEIGHT * CAMERA_BPP)

static uint8_t camera_buffer[CAMERA_BUFFERS][CAMERA_FRAME_BYTES]
    __attribute__((aligned(32), section(".bss.ai_pool")));
static uint32_t camera_frames;
static uint32_t camera_errors;
static uint32_t camera_next;
static osEventFlagsId_t camera_flags;
#if defined(IMAGE_SENSOR_CAPTURE_CONTRACT_VERSION) && IMAGE_SENSOR_CAPTURE_CONTRACT_VERSION >= 2
static bool camera_fault;
#endif

static void camera_event(uint32_t events)
{
    osEventFlagsSet(camera_flags, events);
}

int32_t camera_init(void)
{
#if defined(IMAGE_SENSOR_CAPTURE_CONTRACT_VERSION) && IMAGE_SENSOR_CAPTURE_CONTRACT_VERSION >= 2
    if (camera_fault) return 4; /* A quarantined DMA buffer requires reset. */
#endif
    if (camera_flags != NULL) return 0;
    camera_flags = osEventFlagsNew(NULL);
    if (camera_flags == NULL) {
        return 1;
    }
    ImageSensor_SetEventCallback(camera_event);
    if (ImageSensor_Init() != 0) {
        ImageSensor_SetEventCallback(NULL);
        osEventFlagsDelete(camera_flags);
        camera_flags = NULL;
        return 2;
    }
    if (ImageSensor_Config(eIMAGE_FMT_RGB565, CAMERA_WIDTH, CAMERA_HEIGHT, true) != 0) {
        ImageSensor_SetEventCallback(NULL);
        osEventFlagsDelete(camera_flags);
        camera_flags = NULL;
        return 3;
    }
    return 0;
}

const void *camera_frame(uint32_t timeout_ms)
{
    uint8_t *frame = camera_buffer[camera_next];
    if (camera_flags == NULL) return NULL;
#if defined(IMAGE_SENSOR_CAPTURE_CONTRACT_VERSION) && IMAGE_SENSOR_CAPTURE_CONTRACT_VERSION >= 2
    if (camera_fault) {
        osDelay(1U); /* A quarantined camera must not busy-spin/starve USB. */
        return NULL;
    }
    /* Exclusive, cache-line-aligned buffer: remove dirty CPU lines BEFORE DMA,
     * invalidate again only once a complete frame has been released by CCAP. */
    SCB_CleanInvalidateDCache_by_Addr(frame, CAMERA_FRAME_BYTES);
#endif
    osEventFlagsClear(camera_flags,
                      IMAGE_SENSOR_EVENT_FRAME_COMPLETE | IMAGE_SENSOR_EVENT_ERROR);
    int capture_result = ImageSensor_TriggerCapture((uint32_t)(uintptr_t)frame);
    if (capture_result != 0) {
#if defined(IMAGE_SENSOR_CAPTURE_CONTRACT_VERSION) && IMAGE_SENSOR_CAPTURE_CONTRACT_VERSION >= 2
        if (capture_result == IMAGE_SENSOR_FAULT) camera_fault = true;
#endif
        ++camera_errors;
        return NULL;
    }
    uint32_t events = osEventFlagsWait(camera_flags,
                                       IMAGE_SENSOR_EVENT_FRAME_COMPLETE |
                                           IMAGE_SENSOR_EVENT_ERROR,
                                       osFlagsWaitAny, timeout_ms);
    if ((events & osFlagsError) != 0U ||
        (events & IMAGE_SENSOR_EVENT_ERROR) != 0U ||
        ImageSensor_WaitCaptureDone() != 0) {
#if defined(IMAGE_SENSOR_CAPTURE_CONTRACT_VERSION) && IMAGE_SENSOR_CAPTURE_CONTRACT_VERSION >= 2
        if (ImageSensor_AbortCapture(IMAGE_SENSOR_CAPTURE_POLL_LIMIT) != IMAGE_SENSOR_OK)
            camera_fault = true; /* Never submit/reuse the still-owned buffer. */
#endif
        ++camera_errors;
        return NULL;
    }
    SCB_InvalidateDCache_by_Addr(frame, CAMERA_FRAME_BYTES);
    camera_next = (camera_next + 1U) % CAMERA_BUFFERS;
    ++camera_frames;
    return frame;
}

uint32_t camera_frame_count(void) { return camera_frames; }
uint32_t camera_error_count(void) { return camera_errors; }
void camera_auto_exposure(uint32_t mean) { (void)mean; }
uint32_t camera_gain(void) { return 0x10000U; }

#else

#include "Driver_CPI.h"
#include "camera.h"
#include "cmsis_os2.h"

extern ARM_DRIVER_CPI Driver_CPI;

#define FLAG_FRAME 1U

#define CAMERA_FRAME_BYTES (CAMERA_WIDTH * CAMERA_HEIGHT * CAMERA_BPP)
#define CAMERA_ERRORS (ARM_CPI_EVENT_ERR_CAMERA_INPUT_FIFO_OVERRUN | ARM_CPI_EVENT_ERR_CAMERA_OUTPUT_FIFO_OVERRUN | \
                       ARM_CPI_EVENT_ERR_HARDWARE | ARM_CPI_EVENT_MIPI_CSI2_ERROR)

/* In SRAM1, not initialised by the C library (linker_ac6_traffic.sct.src):
   app_main powers SRAM1 before camera_init. */
static uint8_t camera_buffer[CAMERA_BUFFERS][CAMERA_FRAME_BYTES] __attribute__((aligned(32), section(".bss.ai_pool")));  /* TEST: SRAM0 */

static osEventFlagsId_t camera_flags;
static volatile uint32_t camera_frames;  /* complete frames since the start */
static volatile uint32_t camera_errors;
static volatile uint32_t camera_writing; /* the buffer the CPI fills now */
static volatile uint32_t camera_latest;  /* the newest complete buffer */
static uint32_t camera_returned;         /* camera_frames at the last camera_frame() */
static uint32_t gain_q16 = 0x10000U;

/* One snapshot at a time: the CPI stops after a frame (STOP event), and
   camera_frame() starts the next one into the other buffer from the camera
   thread. Video mode is out on the E7: the driver refuses a new frame address
   while the CPI is busy, and the CPI then writes frame after frame past the
   buffer, over everything behind it. */
static void camera_event(uint32_t event)
{
    if (event & ARM_CPI_EVENT_CAMERA_CAPTURE_STOPPED) {
        camera_latest = camera_writing;
        camera_frames = camera_frames + 1U;
        osEventFlagsSet(camera_flags, FLAG_FRAME);
    }
    if (event & CAMERA_ERRORS) {
        camera_errors = camera_errors + 1U;
    }
}

int32_t camera_init(void)
{
    camera_flags = osEventFlagsNew(NULL);
    if (Driver_CPI.Initialize(camera_event) != ARM_DRIVER_OK) {
        return 1;
    }
    if (Driver_CPI.PowerControl(ARM_POWER_FULL) != ARM_DRIVER_OK) {
        return 2;
    }
    if (Driver_CPI.Control(CPI_CONFIGURE, 0U) != ARM_DRIVER_OK) {
        return 3;
    }
    if (Driver_CPI.Control(CPI_CAMERA_SENSOR_CONFIGURE, 0U) != ARM_DRIVER_OK) {
        return 4;
    }
    if (Driver_CPI.Control(CPI_EVENTS_CONFIGURE, ARM_CPI_EVENT_CAMERA_CAPTURE_STOPPED | CAMERA_ERRORS) != ARM_DRIVER_OK) {
        return 5;
    }
#if !CAMERA_RAW8
    /* The MT9M114's ISP tracks the exposure only once asked (the pack's driver
       writes its AE track register on this request, not at configuration). */
    if (Driver_CPI.Control(CPI_CAMERA_SENSOR_AE, 1U) != ARM_DRIVER_OK) {
        return 6;
    }
#endif
    camera_writing = 0U;
    if (Driver_CPI.CaptureFrame(camera_buffer[0]) != ARM_DRIVER_OK) {
        return 7;
    }
    return 0;
}

const void *camera_frame(uint32_t timeout_ms)
{
    if (camera_frames == camera_returned) {
        osEventFlagsClear(camera_flags, FLAG_FRAME);
        if (camera_frames == camera_returned &&
            (osEventFlagsWait(camera_flags, FLAG_FRAME, osFlagsWaitAny, timeout_ms) & osFlagsError) != 0U) {
            return NULL;
        }
    }
    camera_returned = camera_frames;
    const uint8_t *frame = camera_buffer[camera_latest];
    /* The next snapshot goes into the other buffer while this one is read. */
    camera_writing = (camera_latest + 1U) % CAMERA_BUFFERS;
    if (Driver_CPI.CaptureFrame(camera_buffer[camera_writing]) != ARM_DRIVER_OK) {
        camera_errors = camera_errors + 1U;
    }
    SCB_InvalidateDCache_by_Addr((void *)frame, (int32_t)CAMERA_FRAME_BYTES);
    return frame;
}

uint32_t camera_frame_count(void)
{
    return camera_frames;
}

uint32_t camera_error_count(void)
{
    return camera_errors;
}

void camera_auto_exposure(uint32_t mean)
{
#if CAMERA_RAW8
    /* Towards a mean of 96 (raw, before the white balance), a quarter of the
       way per frame; the pack's gain control takes 16.16 (1.0 = 0x10000) and
       shortens the integration time below 1.0. Small steps are skipped so the
       I2C writes do not run every frame. */
    static const uint32_t target = 96U;
    if (mean == 0U) {
        mean = 1U;
    }
    const float wanted = (float)gain_q16 * ((float)target / (float)mean);
    float next = (float)gain_q16 + 0.25f * (wanted - (float)gain_q16);
    next = next < 0x2000 ? 0x2000 : (next > 0x100000 ? 0x100000 : next); /* 1/8 .. 16 */
    const uint32_t q = (uint32_t)next;
    const uint32_t diff = q > gain_q16 ? q - gain_q16 : gain_q16 - q;
    if (diff * 32U > gain_q16) {  /* more than about 3 % */
        if (Driver_CPI.Control(CPI_CAMERA_SENSOR_GAIN, q) == ARM_DRIVER_OK) {
            gain_q16 = q;
        }
    }
#else
    (void)mean;  /* the MT9M114 has its own auto exposure */
#endif
}

uint32_t camera_gain(void)
{
    return gain_q16;
}

#endif /* APP_CAMERA_NUVOTON */
