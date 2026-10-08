#!/usr/bin/env python3
"""Compile the actual USB initializer with a delayed-host mock; no board I/O."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

source_path = (Path(sys.argv[1]) if len(sys.argv) > 1 else
               Path(__file__).resolve().parents[1] / "sdsio_client_usb_mdk.c")
source = source_path.read_text()
start = source.index("\nint32_t sdsioClientInit (void)") + 1
initializer = source[start:source.index("\n}\n", start) + 3]

mock = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define SDS_ERROR_IO (-6)
#define SDS_OK 0
#define SDSIO_USB_DEVICE_INDEX 0U
#define SDSIO_USB_TIMEOUT 3000U
#define SDS_PRINTF(...) ((void)0)
typedef enum { usbOK, usbError } usbStatus;
typedef void *osEventFlagsId_t;
static osEventFlagsId_t sdsioOutEventFlagId, sdsioInEventFlagId;
static unsigned allocations, fail_allocation, init_calls, connect_calls, delays, ready_after;
static uint32_t tick;
static usbStatus init_status, connect_status;
static osEventFlagsId_t osEventFlagsNew(const void *attr) {
    assert(attr == NULL);
    ++allocations;
    return allocations == fail_allocation ? NULL : (void *)(uintptr_t)allocations;
}
static usbStatus USBD_Initialize(unsigned device) {
    assert(device == 0); ++init_calls; return init_status;
}
static usbStatus USBD_Connect(unsigned device) {
    assert(device == 0); ++connect_calls; return connect_status;
}
static bool USBD_Configured(unsigned device) {
    assert(device == 0); return delays >= ready_after;
}
static void osDelay(uint32_t ticks) {
    assert(ticks == 1);
    ++tick; ++delays;
    assert(delays <= ready_after); // Poll yields; no teardown/reallocation.
    assert(allocations == 2 && init_calls == 1 && connect_calls == 1);
}
uint32_t osKernelGetTickCount(void) { return tick; } // Also compile the old implementation.
static void setup(unsigned delay) {
    allocations = fail_allocation = init_calls = connect_calls = delays = tick = 0;
    ready_after = delay;
    init_status = connect_status = usbOK;
    sdsioOutEventFlagId = sdsioInEventFlagId = 0;
}
'''
tests = r'''
int main(void) {
    setup(0);
    assert(sdsioClientInit() == SDS_OK && delays == 0);
    setup(10000); // Host/cable appears well after the old three-second deadline.
    assert(sdsioClientInit() == SDS_OK && delays == 10000);
    assert(allocations == 2 && init_calls == 1 && connect_calls == 1);
    setup(10000); tick = UINT32_MAX - 100;
    assert(sdsioClientInit() == SDS_OK && delays == 10000); // Tick wrap irrelevant.
    setup(0); fail_allocation = 1;
    assert(sdsioClientInit() == SDS_ERROR_IO && init_calls == 0);
    setup(0); fail_allocation = 2;
    assert(sdsioClientInit() == SDS_ERROR_IO && init_calls == 0);
    setup(0); init_status = usbError;
    assert(sdsioClientInit() == SDS_ERROR_IO && connect_calls == 0);
    setup(0); connect_status = usbError;
    assert(sdsioClientInit() == SDS_ERROR_IO && delays == 0);
    puts("PASS: immediate/delayed host, tick wrap, allocation/init/connect errors");
}
'''
with tempfile.TemporaryDirectory(prefix="sds-usb-init-") as tmp:
    executable = str(Path(tmp) / "test-init")
    subprocess.run([os.environ.get("CC", "clang"), "-x", "c", "-std=c11",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-o", executable, "-"], input=mock + initializer + tests,
                   text=True, check=True)
    subprocess.run([executable], check=True)
