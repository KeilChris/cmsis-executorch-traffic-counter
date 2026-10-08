/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * SDS recording and playback for the traffic counter, see rec_play.h. The
 * control thread follows sds_control.c of the SDS template application
 * (ARM::SDS 3.1.0, template/algorithm), without its LED and button.
 */

#include <stdio.h>
#include <string.h>

#include "RTE_Components.h"
#include CMSIS_device_header

#include "cmsis_os2.h"
#include "sds.h"

#include "rec_play.h"

#ifndef APP_POOL_SECTION
#define APP_POOL_SECTION ".bss.ai_pool"
#endif

#ifdef APP_SDS_POOL_SECTION
#define SDS_POOL_ATTR __attribute__((section(APP_SDS_POOL_SECTION)))
#else
#define SDS_POOL_ATTR
#endif

/* The CameraIn stream buffer holds one record and its header plus the
   transfer chunks around it; the Detections records are small. Boards may
   move both buffers with APP_SDS_POOL_SECTION when their DTCM cannot hold a
   complete 416x416 RGB888 record. */
#define REC_PLAY_IN_BUF_MAX  (416U * 416U * 3U + 16384U)
#define REC_PLAY_OUT_BUF     8192U
#define REC_PLAY_WAIT_MS     30000U /* playback: longest wait for the next record (the RTT down link moves about 100 kB/s: 5 s per frame) */

static uint8_t sds_in_buf[REC_PLAY_IN_BUF_MAX]
    __attribute__((aligned(32))) SDS_POOL_ATTR;
static uint8_t sds_out_buf[REC_PLAY_OUT_BUF]
    __attribute__((aligned(32))) SDS_POOL_ATTR;
static uint32_t in_buf_size;

static sdsId_t in_id, out_id;
static uint8_t playing;  /* the open streams are a playback */

static uint64_t control_stack[1024] __attribute__((section(APP_POOL_SECTION)));
static const osThreadAttr_t control_attr = {
    .name       = "sdsControl",
    .stack_mem  = control_stack,
    .stack_size = sizeof(control_stack),
    .priority   = osPriorityNormal1,
};

/* The SDS 3.1.0 MDK USB client leaves the USB stack and its event flags
   allocated when its initial wait for USB configuration times out.  Clean up
   that partial initialization before retrying.  This lower-layer entry point
   is provided by SDS:IO:USB&MDK USB or the board-local USB transport. */
#if defined(RTE_SDS_IO_CLIENT_USB_MDK) || defined(APP_SDS_USB_MDK_LOCAL)
extern int32_t sdsioClientUninit(void);
#endif

static void sds_event(sdsId_t id, uint32_t event)
{
    (void)id;
    if (event & SDS_EVENT_ERROR_IO) {
        printf("SDS: I/O error\n");
    }
}

/* Exchange the flags with SDSIO-Server every 100 ms and run the state
   machine; START, ACTIVE and STOP_REQ belong to the vision thread. */
static __NO_RETURN void control_thread(void *argument)
{
    (void)argument;
    uint32_t next;

    /* USB configuration can happen well after application startup (for
       example when SDSIO-Server is launched later).  Do not permanently
       disable record/playback after the SDS client's one-shot timeout. */
    while (sdsInit(sds_event) != SDS_OK) {
        printf("SDS: init pending, waiting for host USB connection\n");
#if defined(RTE_SDS_IO_CLIENT_USB_MDK) || defined(APP_SDS_USB_MDK_LOCAL)
        (void)sdsioClientUninit();
#endif
        osDelay(1000U);
    }
    printf("SDS: initialized\n");
    next = osKernelGetTickCount();

    for (;;) {
        sdsExchange();

        if (sdsFlags & SDS_FLAG_TERMINATE) {
            sdsState = SDS_STATE_TERMINATE;
        } else if (sdsFlags & SDS_FLAG_RESET) {
            sdsState = SDS_STATE_RESET;
        } else if (((sdsFlags & SDS_FLAG_ALIVE) == 0U) && (sdsState != SDS_STATE_ACTIVE)) {
            sdsState = SDS_STATE_INACTIVE;
        }

        switch (sdsState) {
        case SDS_STATE_INACTIVE:
            if (sdsFlags & SDS_FLAG_ALIVE) {
                sdsState = SDS_STATE_CONNECTED;
            }
            break;
        case SDS_STATE_CONNECTED:
            if (sdsFlags & SDS_FLAG_START) {
                sdsState = SDS_STATE_START;
            }
            break;
        case SDS_STATE_START:     /* the vision thread opens the streams */
        case SDS_STATE_ACTIVE:    /* it closes them when START is cleared or the playback ends */
        case SDS_STATE_STOP_REQ:
            break;
        case SDS_STATE_STOP_DONE:
            sdsFlagsModify(0U, SDS_FLAG_START);
            sdsState = SDS_STATE_INACTIVE;
            break;
        case SDS_STATE_END:
            sdsState = SDS_STATE_STOP_REQ;
            break;
        case SDS_STATE_RESET:
            __NVIC_SystemReset();
            break;
        case SDS_STATE_TERMINATE:
        default:
            sdsFlagsModify(0U, SDS_FLAG_TERMINATE);
            sdsState = SDS_STATE_INACTIVE;
            break;
        }

        next += 100U;
        osDelayUntil(next);
    }
}

void rec_play_init(uint32_t input_size)
{
    in_buf_size = input_size + 16384U;
    if (in_buf_size > sizeof(sds_in_buf)) {
        in_buf_size = sizeof(sds_in_buf);
    }
    if (osThreadNew(control_thread, NULL, &control_attr) == NULL) {
        printf("SDS: failed to create control thread\n");
    }
}

static void close_streams(void)
{
    /* sdsClose waits for SDSIO-Server's answer: only with a live link. */
    if (sdsFlags & SDS_FLAG_ALIVE) {
        if (in_id != NULL) {
            sdsClose(in_id);
        }
        if (out_id != NULL) {
            sdsClose(out_id);
        }
        printf("SDS: %s stopped\n", playing ? "playback" : "recording");
    } else {
        printf("SDS: link lost, %s streams dropped\n", playing ? "playback" : "recording");
    }
    in_id  = NULL;
    out_id = NULL;
}

rec_play_mode_t rec_play_poll(void)
{
    const uint32_t state = sdsState;
    const uint32_t flags = sdsFlags;

    if (state == SDS_STATE_START) {
        playing = (flags & SDS_FLAG_PLAYBACK) ? 1U : 0U;
        in_id   = sdsOpen("CameraIn", playing ? sdsModeRead : sdsModeWrite, sds_in_buf, in_buf_size);
        out_id  = (in_id != NULL) ? sdsOpen("Detections", sdsModeWrite, sds_out_buf, sizeof(sds_out_buf)) : NULL;
        if (in_id == NULL || out_id == NULL) {
            printf("SDS: %s start failed\n", playing ? "playback" : "recording");
            close_streams();
            sdsState = SDS_STATE_STOP_DONE;
            return REC_PLAY_IDLE;
        }
        printf("SDS: %s started\n", playing ? "playback" : "recording");
        sdsState = SDS_STATE_ACTIVE;
        return playing ? REC_PLAY_PLAYBACK : REC_PLAY_RECORD;
    }
    if (state == SDS_STATE_ACTIVE) {
        if ((flags & SDS_FLAG_START) != 0U && (flags & SDS_FLAG_ALIVE) != 0U) {
            return playing ? REC_PLAY_PLAYBACK : REC_PLAY_RECORD;
        }
        close_streams();
        sdsState = SDS_STATE_STOP_DONE;
        return REC_PLAY_IDLE;
    }
    if (state == SDS_STATE_STOP_REQ) {
        close_streams();
        sdsState = SDS_STATE_STOP_DONE;
    }
    return REC_PLAY_IDLE;
}

int32_t rec_play_read_input(void *buf, uint32_t size, uint32_t *timeslot)
{
    for (uint32_t waited = 0U; waited < REC_PLAY_WAIT_MS; waited++) {
        const int32_t ret = sdsRead(in_id, timeslot, buf, size);
        if (ret == (int32_t)size) {
            return 1;
        }
        if (ret == SDS_NO_DATA) {
            osDelay(1U);
            continue;
        }
        if (ret != SDS_EOS) {
            printf("SDS: CameraIn read returned %ld, expected %lu bytes\n", (long)ret, (unsigned long)size);
        }
        sdsState = SDS_STATE_STOP_REQ;
        return (ret == SDS_EOS) ? 0 : -1;
    }
    printf("SDS: no CameraIn record for %u ms\n", (unsigned)REC_PLAY_WAIT_MS);
    sdsState = SDS_STATE_STOP_REQ;
    return -1;
}

static int32_t write_record(sdsId_t id, const void *buf, uint32_t size,
                            uint32_t timeslot, uint32_t required_flags)
{
    const uint32_t started = osKernelGetTickCount();
    int32_t ret;
    for (;;) {
        if ((sdsFlags & required_flags) != required_flags) {
            break;
        }
        ret = sdsWrite(id, timeslot, buf, size);
        if (ret == (int32_t)size) {
            return 0;
        }
        if ((ret != SDS_NO_SPACE) ||
            ((uint32_t)(osKernelGetTickCount() - started) >= REC_PLAY_WAIT_MS)) {
            break;
        }
        osDelay(1U);
    }
    sdsState = SDS_STATE_STOP_REQ;
    return -1;
}

int32_t rec_play_write_input(const void *buf, uint32_t size, uint32_t timeslot)
{
    /* Stop admitting new pairs when the host clears START. */
    return write_record(in_id, buf, size, timeslot, SDS_FLAG_ALIVE | SDS_FLAG_START);
}

int32_t rec_play_write_output(const void *buf, uint32_t size, uint32_t timeslot)
{
    /* The caller has already accepted this frame's input. Finish its pair
       even after START clears; rec_play_poll closes streams between frames.
       A lost link or a persistently full buffer still aborts the write. */
    return write_record(out_id, buf, size, timeslot, SDS_FLAG_ALIVE);
}

uint32_t rec_play_state(void)
{
    return sdsState;
}

uint32_t rec_play_flags(void)
{
    return sdsFlags;
}
