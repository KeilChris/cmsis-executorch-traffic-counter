/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The camera of the cat detector, on the Ensemble pack's Driver_CPI, see
 * camera.h. The ISP path follows Alif's viewfinder demo
 * (github.com/alifsemi/alif_M55-viewfinder, camera/camera.c): one buffer
 * queued to the ISP and, after each frame, the ISP's frame-end processing
 * (its AE algorithm) with the exposure it computed written back to the
 * sensor. Unlike the demo it captures continuously, not a snapshot per frame.
 */

#include <stddef.h>
#include <stdint.h>

#include "RTE_Components.h"
#include CMSIS_device_header

#include "Driver_CPI.h"
#include "camera.h"
#include "cmsis_os2.h"
#if RTE_ISP
#include <string.h>

#include "Driver_ISP.h"
#include "isp_param.h"
#include "vsi_comm_video.h"
#if defined(RTE_Drivers_CAMERA_SENSOR_OV5675) && CAMERA_OV5675_VTS
#include "Camera_Sensor_i2c.h"
#include "Driver_I2C.h"
#endif
#endif

extern ARM_DRIVER_CPI Driver_CPI;

#define FLAG_FRAME 1U
#define FLAG_ERROR 2U

static osEventFlagsId_t camera_flags;
static volatile uint32_t camera_frames;  /* complete frames since the start */
static volatile uint32_t camera_errors;

#define CAMERA_ERRORS (ARM_CPI_EVENT_ERR_CAMERA_INPUT_FIFO_OVERRUN | ARM_CPI_EVENT_ERR_CAMERA_OUTPUT_FIFO_OVERRUN | \
                       ARM_CPI_EVENT_ERR_HARDWARE | ARM_CPI_EVENT_MIPI_CSI2_ERROR)

#if RTE_ISP
/* ------------------------------------------------------------------------ */
/* CPI -> ISP -> planar RGB888                                              */
/* ------------------------------------------------------------------------ */

#define PLANE_BYTES (CAMERA_WIDTH * CAMERA_HEIGHT)
#ifndef CAMERA_ISP_CONTINUOUS
#define CAMERA_ISP_CONTINUOUS 1  /* see camera_init */
#endif
#define ISP_BUFFERS 1U

/* The ISP writes here (its memory interface, not the CPI: RTE_CPI_AXI_PORT
   is off). In SRAM1, not initialised by the C library: app_main powers SRAM1
   before camera_init (linker_ac6_yolo.sct.src). */
static uint8_t isp_buffer[ISP_BUFFERS][3 * PLANE_BYTES] __attribute__((aligned(32), section(".bss.sram1")));
static VIDEO_BUF_S isp_video_buffer[ISP_BUFFERS];
static volatile uint8_t video_mode;   /* 1: continuous capture, 0: a snapshot per frame */
static volatile uint8_t capturing;    /* snapshot mode: a capture is in flight */

static void camera_event(uint32_t event)
{
    if (event & ARM_ISP_MI_EVENT_MP_FRAME_END_DETECTED) {
        camera_frames = camera_frames + 1U;
        osEventFlagsSet(camera_flags, FLAG_FRAME);
    }
    if (event & CAMERA_ERRORS) {
        camera_errors = camera_errors + 1U;
        osEventFlagsSet(camera_flags, FLAG_ERROR);
    }
}

/* The pack's OV5675 calibration (ov5675_isp_param.c) is marked "not yet
   sensor-calibrated": auto white balance on, an identity colour matrix, and
   the picture comes out nearly grey. These are the values Alif's viewfinder
   demo uses for this sensor (camera/isp_calibration_ov5675.c): RAW10 input,
   no AWB, a tuned colour matrix (7 fractional bits), a capped exposure. Applied
   before the ISP is initialised. */
static void isp_tuning(void)
{
    static const vsi_s16_t ccm[9] = {289, -70, -16, -21, 183, -67, -81, -8, 304};
    /* The sensor's RAW10 as it is (RTE_OV5675_CAMERA_SENSOR_OVERRIDE_CPI_COLOR_MODE
       0), GRBG order: with the pack's RAW8 truncation the colours came out wrong. */
    port_attr.pixelFormat = PIXEL_FORMAT_GRBG10;
#if RTE_ISP_AE_MODULE
    /* 8 ms of the ISP's reckoning, 480 lines. Longer exposures stretch the
       sensor's frame beyond CAMERA_OV5675_VTS (measured: 900 lines gave
       45 fps, 480 keeps 62). */
    calibration_data.modules.ae.autoAttr.expTimeRange.max = CAMERA_OV5675_VTS ? 8000 : 16000;
#endif
#if RTE_ISP_WBM_MODULE
    calibration_data.modules.wbm.enable = 0;
#endif
#if RTE_ISP_WB_MODULE
    calibration_data.modules.wb.enable = 0;
#endif
#if RTE_ISP_CCM_MODULE
    calibration_data.modules.ccm.opType = OP_TYPE_MANUAL;
    memcpy(calibration_data.modules.ccm.manualAttr.colorMatrix, ccm, sizeof(ccm));
#endif
}

#if defined(RTE_Drivers_CAMERA_SENSOR_OV5675) && CAMERA_OV5675_VTS
/* The OV5675's frame length (VTS, 0x380E/0x380F, in lines). The pack's
   1296x972 mode has 2000: 972 lines of picture and a long vertical blanking,
   31.7 fps. Written after the pack's register table, before streaming starts. */
#define I2C_DRIVER_(n) Driver_I2C##n
#define I2C_DRIVER(n)  I2C_DRIVER_(n)
extern ARM_DRIVER_I2C I2C_DRIVER(RTE_OV5675_CAMERA_SENSOR_I2C_INSTANCE);

static int32_t sensor_frame_length(uint32_t lines)
{
    static CAMERA_SENSOR_SLAVE_I2C_CONFIG i2c = {
        .drv_i2c                        = &I2C_DRIVER(RTE_OV5675_CAMERA_SENSOR_I2C_INSTANCE),
        .bus_speed                      = ARM_I2C_BUS_SPEED_STANDARD,
        .cam_sensor_slave_addr          = 0x10,
        .cam_sensor_slave_reg_addr_type = CAMERA_SENSOR_I2C_REG_ADDR_TYPE_16BIT,
    };
    int32_t ret = camera_sensor_i2c_write(&i2c, 0x380EU, (lines >> 8) & 0xFFU, CAMERA_SENSOR_I2C_REG_SIZE_8BIT);
    if (ret == ARM_DRIVER_OK) {
        ret = camera_sensor_i2c_write(&i2c, 0x380FU, lines & 0xFFU, CAMERA_SENSOR_I2C_REG_SIZE_8BIT);
    }
    return ret;
}
#endif

static int32_t snapshot_start(void)
{
    osEventFlagsClear(camera_flags, FLAG_FRAME | FLAG_ERROR);
    capturing = 1U;
    /* The address is not used: the CPI's AXI output is off, the ISP writes the queued buffer. */
    return Driver_CPI.CaptureFrame((void *)0xABCDABCDU);
}

/* The ISP's per-frame processing (statistics, AE), and the exposure its AE
   computed written back to the sensor, or it never converges. */
static void isp_frame_end(void)
{
    Driver_CPI.Control(ISP_PROCESS_FRAME_END, 0U);
#if RTE_ISP_AE_MODULE
    static uint32_t last_line, last_gain;
    struct isp_ae_cached_values ae = {0};
    if (Driver_CPI.Control(ISP_CONTROL_AE_GET_CACHED, (uint32_t)(uintptr_t)&ae) == ARM_DRIVER_OK && ae.int_line != 0U) {
        const uint32_t gain_q16_16 = (ae.again * ae.dgain) / 16U;
        if (ae.int_line != last_line || gain_q16_16 != last_gain) {
            Driver_CPI.Control(CPI_ISP_CAMERA_SENSOR_EXPOSURE, ae.int_line);
            Driver_CPI.Control(CPI_ISP_CAMERA_SENSOR_GAIN, gain_q16_16);
            last_line = ae.int_line;
            last_gain = gain_q16_16;
        }
    }
#endif
}

int32_t camera_init(void)
{
    camera_flags = osEventFlagsNew(NULL);
    isp_tuning();
    for (uint32_t b = 0; b < ISP_BUFFERS; b++) {
        VIDEO_BUF_S *v = &isp_video_buffer[b];
        v->index = b;
        v->numPlanes = 3U;
        v->imageSize = 3U * PLANE_BYTES;
        for (uint32_t p = 0; p < 3U; p++) {
            v->planes[p].dmaPhyAddr = (vsi_dma_t)(uintptr_t)(isp_buffer[b] + p * PLANE_BYTES);
            v->planes[p].size = PLANE_BYTES;
            v->planes[p].pUserAddr = isp_buffer[b] + p * PLANE_BYTES;
        }
    }

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
#if defined(RTE_Drivers_CAMERA_SENSOR_OV5675) && CAMERA_OV5675_VTS
    if (sensor_frame_length(CAMERA_OV5675_VTS) != ARM_DRIVER_OK) {
        return 4;
    }
#endif
    if (Driver_CPI.Control(CPI_EVENTS_CONFIGURE, ARM_CPI_EVENT_CAMERA_CAPTURE_STOPPED | CAMERA_ERRORS) != ARM_DRIVER_OK) {
        return 5;
    }
    for (uint32_t b = 0; b < ISP_BUFFERS; b++) {
        if (Driver_CPI.Control(ISP_CONTROL_QBUF, (uint32_t)(uintptr_t)&isp_video_buffer[b]) != ARM_DRIVER_OK) {
            return 6;
        }
    }
    /* CPI_CONFIGURE soft-resets the CPI and leaves it started (CAM_CTRL START);
       before the sensor streams there is no pixel clock, the CPI stays BUSY
       and the capture start is refused (ARM_DRIVER_ERROR_BUSY). Reset it once
       more without START (CAM_CTRL bit 8, set then cleared). */
    CPI->CAM_CTRL = 0U;
    CPI->CAM_CTRL = 1U << 8;
    CPI->CAM_CTRL = 0U;

#if CAMERA_ISP_CONTINUOUS
    /* Continuous capture: the sensor streams, the ISP fills the queued
       buffers in turn (Alif's viewfinder takes one snapshot per frame instead,
       which restarts the sensor every time and halves the frame rate).
       One buffer, as in snapshot mode: see camera_frame. */
    osEventFlagsClear(camera_flags, FLAG_FRAME | FLAG_ERROR);
    if (Driver_CPI.CaptureVideo((void *)0xABCDABCDU) == ARM_DRIVER_OK &&
        (osEventFlagsWait(camera_flags, FLAG_FRAME, osFlagsWaitAny, 1000U) & osFlagsError) == 0U) {
        video_mode = 1U;
        osEventFlagsSet(camera_flags, FLAG_FRAME);  /* the frame just waited for */
        return 0;
    }
    /* Fallback: a snapshot per frame. */
    Driver_CPI.Stop();
    CPI->CAM_CTRL = 0U;
    CPI->CAM_CTRL = 1U << 8;
    CPI->CAM_CTRL = 0U;
#endif
    if (snapshot_start() != ARM_DRIVER_OK) {
        return 7;
    }
    return 0;
}

const void *camera_frame(uint32_t timeout_ms)
{
    if (!video_mode) {
        if (!capturing) {
            return NULL;
        }
        const uint32_t flags = osEventFlagsWait(camera_flags, FLAG_FRAME | FLAG_ERROR, osFlagsWaitAny, timeout_ms);
        if ((flags & osFlagsError) != 0U || (flags & FLAG_FRAME) == 0U) {
            return NULL;
        }
        capturing = 0U;
        isp_frame_end();
        SCB_InvalidateDCache_by_Addr(isp_buffer[0], (int32_t)sizeof(isp_buffer[0]));
        return isp_buffer[0];
    }

    /* Continuous: one buffer, rewritten by the ISP every frame (as the
       viewfinder does it; ISP_CONTROL_DQBUF returned no buffer here). The
       application takes the frame right at its end and reads it top to
       bottom faster than the ISP writes the next one (image_planar_to_input,
       3 ms against 16 ms a frame), so it stays ahead of the writes. The
       ISP's frame-end work, with its I2C writes, waits for camera_release. */
    osEventFlagsClear(camera_flags, FLAG_FRAME);
    const uint32_t flags = osEventFlagsWait(camera_flags, FLAG_FRAME, osFlagsWaitAny, timeout_ms);
    if ((flags & osFlagsError) != 0U) {
        return NULL;
    }
    SCB_InvalidateDCache_by_Addr(isp_buffer[0], (int32_t)sizeof(isp_buffer[0]));
    return isp_buffer[0];
}

void camera_release(void)
{
    if (video_mode) {
        static uint32_t processed;
        while (processed != camera_frames) {
            processed++;
            isp_frame_end();
        }
    } else if (!capturing && snapshot_start() != ARM_DRIVER_OK) {
        camera_errors = camera_errors + 1U;
    }
}

uint32_t camera_video_mode(void)
{
    return video_mode;
}

#else
/* ------------------------------------------------------------------------ */
/* CPI streaming into four RGB565 frame buffers                             */
/* ------------------------------------------------------------------------ */

#define CAMERA_BUFFERS     4U  /* RTE_CPI_NUM_ACTIVE_FRAMEBUFFERS */
#define CAMERA_FRAME_BYTES (CAMERA_WIDTH * CAMERA_HEIGHT * 2U)

/* In SRAM1, not initialised by the C library (linker_ac6_yolo.sct.src):
   app_main powers SRAM1 before camera_init. */
static uint8_t camera_buffer[CAMERA_BUFFERS][CAMERA_FRAME_BYTES] __attribute__((aligned(32), section(".bss.sram1")));
static void *camera_buffers[CAMERA_BUFFERS];
static volatile uint32_t camera_latest;  /* index of the newest complete frame */
static uint32_t camera_returned;         /* camera_frames at the last camera_frame() */

/* At each VSYNC the CPI reports the buffer it has just finished
   (ARM_CPI_VSYNC_BUF_IDX_Pos): the newest complete frame. */
static void camera_event(uint32_t event)
{
    if (event & ARM_CPI_EVENT_CAMERA_FRAME_VSYNC_DETECTED) {
        static uint8_t started;  /* the first VSYNC starts the first frame: nothing complete yet */
        if (started) {
            camera_latest = (event >> ARM_CPI_VSYNC_BUF_IDX_Pos) & (CAMERA_BUFFERS - 1U);
            camera_frames = camera_frames + 1U;
            osEventFlagsSet(camera_flags, FLAG_FRAME);
        }
        started = 1U;
    }
    if (event & CAMERA_ERRORS) {
        camera_errors = camera_errors + 1U;
    }
}

int32_t camera_init(void)
{
    camera_flags = osEventFlagsNew(NULL);
    for (uint32_t i = 0; i < CAMERA_BUFFERS; i++) {
        camera_buffers[i] = camera_buffer[i];
    }
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
    if (Driver_CPI.Control(CPI_EVENTS_CONFIGURE, ARM_CPI_EVENT_CAMERA_FRAME_VSYNC_DETECTED | CAMERA_ERRORS) != ARM_DRIVER_OK) {
        return 5;
    }
    /* See the ISP path: clear the BUSY that CPI_CONFIGURE leaves behind. */
    CPI->CAM_CTRL = 0U;
    CPI->CAM_CTRL = 1U << 8;
    CPI->CAM_CTRL = 0U;
    /* Streaming mode: the argument is the array of the frame buffers. */
    if (Driver_CPI.CaptureVideo(camera_buffers) != ARM_DRIVER_OK) {
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
    SCB_InvalidateDCache_by_Addr((void *)frame, (int32_t)CAMERA_FRAME_BYTES);
    return frame;
}

void camera_release(void)
{
    /* The CPI cycles through its buffers on its own. */
}
#endif

uint32_t camera_frame_count(void)
{
    return camera_frames;
}

uint32_t camera_error_count(void)
{
    return camera_errors;
}
