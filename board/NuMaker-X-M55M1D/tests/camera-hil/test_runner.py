#!/usr/bin/env python3
"""Host checks of HIL orchestration (not the driver); no target access."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
CAMERA = ROOT / 'pack-work/Nuvoton.NuMicro_M55M1_BSP/camera-hardening/release/3.1.5-rc.1/Board/NuMaker-X-M55M1D/Camera'

device = r'''
#pragma once
#include <stddef.h>
#include <stdint.h>
void cache_access(void *, size_t);
#define SCB_CleanInvalidateDCache_by_Addr(p,n) cache_access((void *)(p),(n))
#define SCB_InvalidateDCache_by_Addr(p,n) cache_access((void *)(p),(n))
'''
rtos = r'''
#pragma once
#include <stdint.h>
typedef struct { const char *name; void *stack_mem; uint32_t stack_size; int priority; } osThreadAttr_t;
enum { osPriorityNormal1=25 };
uint32_t osKernelGetTickCount(void);
uint32_t osKernelGetTickFreq(void);
int osDelay(uint32_t);
void *osThreadNew(void (*fn)(void *), void *, const osThreadAttr_t *);
'''
test = r'''
#include <assert.h>
#include <stdio.h>
#include "runner.c"

static bool configured, initialized, active, faulted, absent, forbid_a;
static bool bad_guard, no_dma, late_write, already_done, corrupt_b;
static uint32_t ticks;
static guarded_frame_t *pending;
static int32_t real_sensor(uint32_t p) { (void)p; return !absent; }
S_SENSOR_INFO g_sSensorHM1055_VGA_YUV422 = { .pfnInitSensor=real_sensor };
void cache_access(void *p, size_t n) {
    uintptr_t start=(uintptr_t)p, end=start+n;
    assert(!forbid_a || end <= (uintptr_t)&buffer_a || start >= (uintptr_t)(&buffer_a+1));
}
uint32_t osKernelGetTickCount(void) { return ticks; }
uint32_t osKernelGetTickFreq(void) { return 1000; }
int osDelay(uint32_t n) {
    ticks+=n;
    if (late_write && camera_hil_status.step==124) buffer_a.frame[0]^=1;
    if (corrupt_b && forbid_a) buffer_b.frame[0]=1;
    return 0;
}
void *osThreadNew(void (*fn)(void *), void *arg, const osThreadAttr_t *attr) {
    (void)fn; (void)arg; (void)attr; return (void *)1;
}
void rec_play_init(uint32_t n) { (void)n; }
void ImageSensor_SetEventCallback(ImageSensor_EventCallback_t cb) { (void)cb; }
int ImageSensor_Init(void) {
    if (faulted) return IMAGE_SENSOR_FAULT;
    if (active) return IMAGE_SENSOR_BUSY;
    initialized=configured=false;
    initialized=g_sSensorHM1055_VGA_YUV422.pfnInitSensor(0)!=0;
    return initialized ? IMAGE_SENSOR_OK : IMAGE_SENSOR_ERROR;
}
int ImageSensor_Config(E_IMAGE_FMT f,uint32_t w,uint32_t h,bool ratio) {
    (void)ratio;
    if (faulted) return IMAGE_SENSOR_FAULT;
    if (!initialized) return IMAGE_SENSOR_NOT_READY;
    if (active) return IMAGE_SENSOR_BUSY;
    if (f!=eIMAGE_FMT_RGB565 || w!=416 || h!=416) return IMAGE_SENSOR_INVALID;
    configured=true; return IMAGE_SENSOR_OK;
}
int ImageSensor_TriggerCapture(uint32_t a) {
    if (faulted) return IMAGE_SENSOR_FAULT;
    if (!configured) return IMAGE_SENSOR_NOT_READY;
    if (active) return IMAGE_SENSOR_BUSY;
    if (!a || a%32) return IMAGE_SENSOR_INVALID;
    pending=a==address(&buffer_a) ? &buffer_a : &buffer_b;
    active=true; return IMAGE_SENSOR_OK;
}
int ImageSensor_PollCaptureDone(void) {
    if (faulted) return IMAGE_SENSOR_FAULT;
    if (!active) return IMAGE_SENSOR_NOT_READY;
    if (!no_dma) memset(pending->frame,0x42,FRAME_BYTES);
    if (bad_guard) pending->after[0]=0;
    active=false; return IMAGE_SENSOR_OK;
}
int ImageSensor_AbortCapture(uint32_t budget) {
    if (faulted) return IMAGE_SENSOR_FAULT;
    if (!active) return IMAGE_SENSOR_OK;
    if (budget || already_done) { active=false; return IMAGE_SENSOR_OK; }
    faulted=forbid_a=true; return IMAGE_SENSOR_TIMEOUT;
}
static void reset_case(void) {
    configured=initialized=active=faulted=absent=forbid_a=false;
    bad_guard=no_dma=late_write=already_done=corrupt_b=false;
    camera_hil_status=(camera_hil_status_t){0};
    ticks=0; pending=NULL;
    g_sSensorHM1055_VGA_YUV422.pfnInitSensor=real_sensor;
}
int main(void) {
    reset_case(); normal_tests();
    assert(camera_hil_status.phase==CAMERA_HIL_NORMAL_PASS);
    assert(camera_hil_status.passed==29);
    assert(g_sSensorHM1055_VGA_YUV422.pfnInitSensor==real_sensor);
    quarantine_test();
    assert(camera_hil_status.phase==CAMERA_HIL_QUARANTINE_PASS);
    assert(camera_hil_status.reload_required && forbid_a);
    assert(camera_hil_status.passed==38);

    reset_case(); normal_tests(); already_done=true; quarantine_test();
    assert(camera_hil_status.phase==CAMERA_HIL_INCONCLUSIVE);
    reset_case(); bad_guard=true; normal_tests();
    assert(camera_hil_status.phase==CAMERA_HIL_FAILED && camera_hil_status.step==117);
    reset_case(); no_dma=true; normal_tests();
    assert(camera_hil_status.phase==CAMERA_HIL_FAILED && camera_hil_status.step==119);
    reset_case(); late_write=true; normal_tests();
    assert(camera_hil_status.phase==CAMERA_HIL_FAILED && camera_hil_status.step==124);
    reset_case(); normal_tests(); corrupt_b=true; quarantine_test();
    assert(camera_hil_status.phase==CAMERA_HIL_FAILED && camera_hil_status.step==209);
    reset_case(); absent=true; normal_tests();
    assert(camera_hil_status.phase==CAMERA_HIL_FAILED && camera_hil_status.step==105);
    assert(g_sSensorHM1055_VGA_YUV422.pfnInitSensor==real_sensor);
    reset_case(); absent=true; absent_sensor_test();
    assert(camera_hil_status.phase==CAMERA_HIL_ABSENT_PASS && camera_hil_status.reload_required);
    reset_case(); absent_sensor_test();
    assert(camera_hil_status.phase==CAMERA_HIL_FAILED && camera_hil_status.step==301);
    puts("PASS: HIL pass/fail/inconclusive checks, guard/late-write rejection, descriptor restore, quarantine cache exclusion");
}
'''

with tempfile.TemporaryDirectory(prefix='camera-hil-host-') as tmp:
    directory = Path(tmp)
    (directory / 'RTE_Components.h').write_text('#define CMSIS_device_header "NuMicro.h"\n')
    (directory / 'NuMicro.h').write_text(device)
    (directory / 'cmsis_os2.h').write_text(rtos)
    (directory / 'test.c').write_text(test)
    subprocess.run(['cc', '-std=c11', '-g', '-DCAMERA_HIL_TEST=1', '-DCAMERA_HIL_HOST_TEST=1',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I', str(directory), '-I', str(HERE), '-I', str(CAMERA),
                    '-I', str(ROOT / 'traffic'), str(directory / 'test.c'),
                    '-o', str(directory / 'test')], check=True)
    subprocess.run([str(directory / 'test')], check=True)
