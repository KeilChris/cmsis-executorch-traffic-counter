#!/usr/bin/env python3
"""Compile the actual USB callback/receiver with a preempting middleware mock.

No hardware access. Reproduces completion while EndpointRead holds its lock.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

source_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "sdsio_client_usb_mdk.c"
source = source_path.read_text()


def function(signature):
    start = source.index("\n" + signature) + 1
    end = source.index("\n}\n", start) + 3
    return source[start:end]


callback = function("static void USBD_Endpoint_Event (uint8_t ep_num, uint32_t event) {")
receiver = function("int32_t sdsioClientReceive (")
mock = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ARM_USBD_EVENT_OUT 1U
#define ARM_USBD_EVENT_IN 2U
#define SDSIO_CLIENT_EVENT_DATA_SENT 1U
#define SDSIO_CLIENT_EVENT_DATA_RECEIVED 2U
#define SDSIO_USB_DEVICE_INDEX 0U
#define SDSIO_USB_TIMEOUT 3000U
#define osFlagsWaitAll 1U
#define osFlagsError 0x80000000U
#define osFlagsErrorResource 0xfffffffdU
#define osFlagsErrorTimeout 0xfffffffeU
#define SDS_ERROR_TIMEOUT (-5)
#define SDS_ERROR_IO (-6)
typedef enum { usbOK, usbTimeout, usbError } usbStatus;
typedef enum { sdsioReceiveBlocking, sdsioReceiveNonBlocking } sdsioReceiveMode_t;
static unsigned sdsioOutEventFlagId = 1, sdsioInEventFlagId = 2;
static uint32_t bulkMaxPacketSize = 512, bulkOutEpAddr = 1, bulkInEpAddr = 129;
static uint8_t bulkOutBuffer[8192];
static uint32_t bulkOutCnt, bulkOutIdx;
static uint32_t pending, result, result_reads, rearm_count, wire_offset, wire_size;
static uint32_t packet_limit, first_empty;
static bool endpoint_locked;
static usbStatus arm_status;
static uint8_t wire[20000];
static void USBD_Endpoint_Event(uint8_t ep_num, uint32_t event);
static uint32_t osEventFlagsSet(unsigned id, uint32_t flags) {
    if (id == sdsioOutEventFlagId) pending |= flags;
    return flags;
}
static uint32_t osEventFlagsWait(unsigned id, uint32_t flags, unsigned opts, unsigned timeout) {
    assert(id == sdsioOutEventFlagId && opts == osFlagsWaitAll);
    if (pending & flags) { pending &= ~flags; return flags; }
    return timeout ? osFlagsErrorTimeout : osFlagsErrorResource;
}
static uint32_t USBD_EndpointReadGetResult(unsigned dev, unsigned ep) {
    assert(dev == 0 && ep == 1);
    // The old callback queries this with the lock held, causing 100 ms retries.
    assert(!endpoint_locked);
    ++result_reads;
    return result;
}
static usbStatus USBD_EndpointRead(unsigned dev, unsigned ep, uint8_t *buf, unsigned len) {
    assert(dev == 0 && ep == 1 && buf == bulkOutBuffer);
    assert(len && len <= sizeof bulkOutBuffer && len % bulkMaxPacketSize == 0);
    ++rearm_count;
    if (arm_status != usbOK) return arm_status;
    endpoint_locked = true;
    if (first_empty) {
        first_empty = 0;
        result = 0;
        USBD_Endpoint_Event(1, ARM_USBD_EVENT_OUT);
    } else if (wire_offset < wire_size) {
        unsigned n = wire_size - wire_offset;
        if (n > len) n = len;
        if (packet_limit && n > packet_limit) n = packet_limit;
        memcpy(buf, wire + wire_offset, n);
        wire_offset += n;
        result = n;
        USBD_Endpoint_Event(1, ARM_USBD_EVENT_OUT);
    }
    endpoint_locked = false;
    return usbOK;
}
static void setup(unsigned size) {
    bulkOutCnt = bulkOutIdx = pending = result_reads = rearm_count = wire_offset = 0;
    packet_limit = first_empty = 0;
    wire_size = size;
    endpoint_locked = false;
    arm_status = usbOK;
    for (unsigned i = 0; i < sizeof wire; ++i) wire[i] = (uint8_t)(i * 13 + i / 512);
}
'''
tests = r'''
int main(void) {
    uint8_t data[16000];
    setup(16000);
    assert(USBD_EndpointRead(0, 1, bulkOutBuffer, 512) == usbOK);
    assert(result_reads == 0 && bulkOutCnt == 0 && pending == 2);
    // Header consumption must preserve the rest of the completed USB buffer.
    assert(sdsioClientReceive(data, 16, sdsioReceiveBlocking) == 16);
    assert(bulkOutCnt == 496 && result_reads == 1);
    assert(sdsioClientReceive(data + 16, sizeof(data) - 16, sdsioReceiveBlocking) == sizeof(data) - 16);
    assert(memcmp(data, wire, sizeof(data)) == 0);
    assert(result_reads == 3 && pending == 0);

    setup(0);
    assert(sdsioClientReceive(data, 16, sdsioReceiveNonBlocking) == 0);
    assert(result_reads == 0);
    assert(sdsioClientReceive(data, 16, sdsioReceiveBlocking) == SDS_ERROR_TIMEOUT);

    setup(32);
    packet_limit = 16;
    USBD_EndpointRead(0, 1, bulkOutBuffer, 512);
    assert(sdsioClientReceive(data, 32, sdsioReceiveNonBlocking) == 16);
    assert(pending == 2); // Completion during rearm must not be cleared/lost.
    assert(sdsioClientReceive(data + 16, 16, sdsioReceiveNonBlocking) == 16);
    assert(memcmp(data, wire, 32) == 0 && result_reads == 2);

    setup(16);
    first_empty = 1;
    USBD_EndpointRead(0, 1, bulkOutBuffer, 512);
    assert(sdsioClientReceive(data, 16, sdsioReceiveBlocking) == 16);
    assert(result_reads == 2 && memcmp(data, wire, 16) == 0);

    setup(16);
    USBD_EndpointRead(0, 1, bulkOutBuffer, 512);
    assert(sdsioClientReceive(data, 32, sdsioReceiveBlocking) == 16);
    assert(sdsioClientReceive(data, 16, sdsioReceiveBlocking) == SDS_ERROR_TIMEOUT);

    setup(0);
    pending = 2;
    result = sizeof(bulkOutBuffer) + 1;
    assert(sdsioClientReceive(data, 16, sdsioReceiveBlocking) == SDS_ERROR_IO);

    setup(0);
    pending = 2;
    result = 0;
    arm_status = usbError;
    assert(sdsioClientReceive(data, 16, sdsioReceiveBlocking) == SDS_ERROR_IO);
    puts("PASS: early completion, buffered tail, blocking/nonblocking, rearm, ZLP, partial/error paths");
}
'''
with tempfile.TemporaryDirectory(prefix="sds-usb-receive-") as tmp:
    executable = str(Path(tmp) / "test-receive")
    subprocess.run([os.environ.get("CC", "clang"), "-x", "c", "-std=c11",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-o", executable, "-"], input=mock + callback + receiver + tests,
                   text=True, check=True)
    subprocess.run([executable], check=True)
