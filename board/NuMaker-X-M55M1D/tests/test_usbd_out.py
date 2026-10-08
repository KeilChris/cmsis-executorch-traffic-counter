#!/usr/bin/env python3
"""Exercise the actual OUT handler with a host-side FIFO/register mock.

Run with python3 board/NuMaker-X-M55M1D/tests/test_usbd_out.py.
Use --driver PATH to test a DFP source instead of the working local override.
This does not connect to hardware. CC defaults to clang.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--driver", type=Path,
                    default=Path(__file__).resolve().parents[1] / "Driver_USBD_HSUSBD_fixed.c",
                    help="HSUSBD C source to test (default: working project override)")
driver = parser.parse_args().driver.resolve()
if not driver.is_file():
    parser.error(f"driver source does not exist: {driver}")
print(f"OUT driver under test: {driver}", flush=True)
source = driver.read_text()
start = source.index("static void USBD_DataOutStage(")
end = source.index("\n}\n", start) + 3
handler = source[start:end]

mock = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define HSUSBD_EPDATCNT_DATCNT_Msk 0xffffU
#define HSUSBD_EPINTEN_RXPKIEN_Msk 1U
#define ARM_USBD_EVENT_OUT 1U
typedef struct { uint32_t EPDATCNT, EPDAT, EPINTEN; } USBD_EP_t;
typedef struct { USBD_EP_t EP[1]; } USBD_t;
typedef struct {
    uint8_t *data;
    uint32_t num, num_transferred_total, max_packet_size;
} EP_Info_t;
typedef struct {
    EP_Info_t ep_info[1];
    void (*cb_endpoint_event)(uint8_t, uint32_t);
} RW_Info_t;
typedef struct { USBD_t *ptr_USBD; } RO_Info_t;
typedef struct { RO_Info_t *ptr_ro_info; RW_Info_t *ptr_rw_info; } USBD_Info_t;
static USBD_t regs;
static RW_Info_t rw;
static RO_Info_t ro = { &regs };
static USBD_Info_t info = { &ro, &rw };
static uint8_t fifo[512], data[8194];
static unsigned callbacks;
static bool rearm;
static USBD_EP_t *USBD_EndpointEntry(const USBD_Info_t *i, uint8_t addr, bool add) {
    (void)addr; (void)add;
    return i->ptr_ro_info->ptr_USBD->EP;
}
static void USBD_ReadEpBuffer(uint8_t *dst, uint32_t *port, uint32_t n) {
    (void)port;
    assert(n <= sizeof fifo);
    memcpy(dst, fifo, n);
}
static void completed(uint8_t addr, uint32_t event) {
    assert(addr == 1 && event == ARM_USBD_EVENT_OUT);
    assert(regs.EP[0].EPINTEN == 0); /* Quiesce before handing off the buffer. */
    callbacks++;
    if (rearm) { /* Middleware may rearm within the completion callback. */
        rw.ep_info[0].num_transferred_total = 0;
        regs.EP[0].EPINTEN = HSUSBD_EPINTEN_RXPKIEN_Msk;
    }
}
static void setup_mps(unsigned n, unsigned mps) {
    memset(data, 0xa5, sizeof data);
    rw.ep_info[0] = (EP_Info_t){ data + 1, n, 0, mps };
    rw.cb_endpoint_event = completed;
    regs.EP[0].EPINTEN = HSUSBD_EPINTEN_RXPKIEN_Msk;
    callbacks = 0;
    rearm = false;
}
static void setup(unsigned n) { setup_mps(n, 512); }
'''
tests = r'''
static void packet(unsigned n, uint8_t value) {
    memset(fifo, value, sizeof fifo);
    regs.EP[0].EPDATCNT = n;
    /* Model interrupt delivery only while RXPK is enabled. */
    if (regs.EP[0].EPINTEN & HSUSBD_EPINTEN_RXPKIEN_Msk)
        USBD_DataOutStage(&info, 1);
}
int main(void) {
    const unsigned packet_sizes[] = {64, 512};
    const unsigned lengths[] = {0, 16, 64, 65, 128, 400, 408, 512, 513, 8192};
    for (unsigned p = 0; p < sizeof packet_sizes / sizeof packet_sizes[0]; p++) {
        for (unsigned l = 0; l < sizeof lengths / sizeof lengths[0]; l++) {
            const unsigned mps = packet_sizes[p], n = lengths[l];
            setup_mps(n, mps);
            unsigned done = 0, seq = 0;
            do {
                const unsigned size = n - done < mps ? n - done : mps;
                packet(size, (uint8_t)seq++);
                done += size;
                assert(rw.ep_info[0].num_transferred_total == done);
                assert(callbacks == (done == n ? 1U : 0U));
            } while (done < n);
            for (unsigned i = 0; i < n; i++) assert(data[i + 1] == (uint8_t)(i / mps));
            assert(data[0] == 0xa5 && data[n + 1] == 0xa5);
        }
        setup_mps(8192, packet_sizes[p]);
        packet(packet_sizes[p], 1); packet(0, 0);
        assert(callbacks == 1 && rw.ep_info[0].num_transferred_total == packet_sizes[p]);
    }

    setup(8192);
    for (unsigned p = 0; p < 16; p++) {
        packet(512, (uint8_t)p);
        assert(callbacks == (p == 15 ? 1U : 0U));
        assert(rw.ep_info[0].num_transferred_total == (p + 1) * 512);
    }
    for (unsigned i = 0; i < 8192; i++) assert(data[i + 1] == i / 512);
    assert(data[0] == 0xa5 && data[8193] == 0xa5);
    packet(512, 0xee); /* No writes or callbacks after completion. */
    assert(callbacks == 1 && data[1] == 0 && data[8192] == 15);

    setup(8192);
    packet(512, 1); packet(16, 2);
    assert(callbacks == 1 && rw.ep_info[0].num_transferred_total == 528);
    assert(data[512] == 1 && data[513] == 2 && data[529] == 0xa5);

    setup(8192);
    packet(512, 1); packet(0, 0);
    assert(callbacks == 1 && rw.ep_info[0].num_transferred_total == 512);

    setup(512);
    packet(512, 7);
    assert(callbacks == 1 && data[512] == 7 && data[513] == 0xa5);

    setup(16); /* Destination bounds even when a packet exceeds capacity. */
    packet(512, 8);
    assert(callbacks == 1 && rw.ep_info[0].num_transferred_total == 16);
    assert(data[16] == 8 && data[17] == 0xa5);

    setup(512);
    rearm = true;
    packet(512, 1); packet(512, 2);
    assert(callbacks == 2 && regs.EP[0].EPINTEN != 0);
    assert(data[1] == 2 && data[512] == 2);
    puts("PASS: OUT FS/HS multipacket, short/ZLP, exact length, bounds, callback rearm");
}
'''
with tempfile.TemporaryDirectory(prefix="numaker-usbd-test-") as tmp:
    executable = str(Path(tmp) / "test-usbd-out")
    subprocess.run([os.environ.get("CC", "clang"), "-x", "c", "-std=c11",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-o", executable, "-"], input=mock + handler + tests,
                   text=True, check=True)
    subprocess.run([executable], check=True)
