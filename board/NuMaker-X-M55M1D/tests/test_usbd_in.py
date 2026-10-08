#!/usr/bin/env python3
"""Host-only tests of the selected driver's actual IN functions and IRQ dispatch.

C++ register mocks model W1C status and a host completing a packet during the
FIFO write. No probe is used. This tests software ordering, not PHY timing.
Use --driver PATH to test a DFP source instead of the working local override.
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
print(f"IN driver under test: {driver}", flush=True)
source = driver.read_text()


def function(signature, optional=False):
    if optional and signature not in source:
        return ""
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


functions = "\n".join([
    function("static int32_t USBDn_EndpointTransfer("),
    function("static uint32_t USBDn_EndpointTransferGetResult("),
    function("static int32_t USBDn_EndpointTransferAbort("),
    function("static void USBD_DataInStage("),
    function("static void USBD_DataInComplete(", optional=True),
])
start = source.index("                if (EP_DIR(ep_addr))", source.index("// Endpoint event"))
# Accept both the corrected OUT guard and the original plain else, so the
# pre-fix driver reaches the regression assertion rather than an extraction error.
end = source.index("\n                else", start)
dispatch = source[start:end]

mock = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <vector>
#define EP_NUM(a) ((a) & 15U)
#define EP_DIR(a) (((a) >> 7) & 1U)
#define ARM_DRIVER_OK 0
#define ARM_DRIVER_ERROR (-1)
#define ARM_DRIVER_ERROR_PARAMETER (-5)
#define ARM_USBD_EVENT_IN 1U
#define ARM_USBD_EVENT_OUT 2U
#define PERIPH_MAX_EP 1U
#define HSUSBD_EPINTSTS_BUFEMPTYIF_Msk 2U
#define HSUSBD_EPINTSTS_TXPKIF_Msk 8U
#define HSUSBD_EPINTSTS_RXPKIF_Msk 16U
#define HSUSBD_EPINTEN_BUFEMPTYIEN_Msk 2U
#define HSUSBD_EPINTEN_TXPKIEN_Msk 8U
#define HSUSBD_EPINTEN_RXPKIEN_Msk 16U
#define HSUSBD_EPRSPCTL_FLUSH_Msk 1U
#define HSUSBD_EPRSPCTL_MODE_Msk 6U
#define HSUSBD_EPRSPCTL_HALT_Msk 16U
#define HSUSBD_EPRSPCTL_DISBUF_Msk 128U
#define HSUSBD_EPRSPCTL_ZEROLEN_Msk 32U
#define HSUSBD_EPRSPCTL_SHORTTXEN_Msk 64U
#define HSUSBD_CEPCTL_FLUSH_Msk 1U
#define HSUSBD_CEPCTL_NAKCLR 0U
#define HSUSBD_CEPINTSTS_INTKIF_Msk 8U
#define HSUSBD_CEPINTSTS_STSDONEIF_Msk 1024U
#define HSUSBD_CEPINTEN_INTKIEN_Msk 8U
#define HSUSBD_CEPINTEN_STSDONEIEN_Msk 1024U
#define HSUSBD_CEPRXCNT_RXCNT_Msk 0xffffU
#define HSUSBD_DMACTL_DMARST_Msk 1U
static uint32_t irq_mask;
static uint32_t __get_PRIMASK() { return irq_mask; }
static void __disable_irq() { irq_mask = 1; }
static void __set_PRIMASK(uint32_t value) { irq_mask = value; }
struct Status {
    uint32_t bits = 0;
    operator uint32_t() const { return bits; }
    void operator=(uint32_t value) { bits &= ~(value & ~3U); } // low bits read-only
};
struct USBD_EP_t {
    uint32_t EPDAT = 0;
    Status EPINTSTS;
    uint32_t EPINTEN = 0, EPRSPCTL = 0;
};
struct USBD_t {
    USBD_EP_t EP[1];
    uint32_t CEPCTL = 0, CEPINTSTS = 0, CEPINTEN = 0, CEPRXCNT = 0;
    uint32_t CEPDAT = 0, DMACTL = 0, DMACNT = 0;
};
struct EP_Info_t {
    uint8_t *data = nullptr;
    uint32_t num = 0, num_transferred_total = 0, num_pending = 0;
    uint16_t max_packet_size = 512;
    uint8_t in_packet_pending = 0;
};
struct RW_Info_t {
    struct { unsigned powered = 1; } drv_status;
    EP_Info_t ep_info[1], cep_info[2];
    uint32_t cep_event = 0;
    void (*cb_endpoint_event)(uint8_t, uint32_t) = nullptr;
};
struct RO_Info_t { USBD_t *ptr_USBD; };
struct USBD_Info_t { RO_Info_t *ptr_ro_info; RW_Info_t *ptr_rw_info; };
static USBD_t regs;
static RW_Info_t rw;
static RO_Info_t ro = { &regs };
static USBD_Info_t info = { &ro, &rw };
static uint32_t SystemCoreClock = 220000000;
static std::vector<uint8_t> input, wire, fifo;
static unsigned callbacks, writes;
static bool complete_during_fill, rearm;
static uint8_t next_data[16];
static USBD_EP_t *USBD_EndpointEntry(const USBD_Info_t *i, uint8_t, bool) {
    return &i->ptr_ro_info->ptr_USBD->EP[0];
}
static void sent_by_host() {
    wire.insert(wire.end(), fifo.begin(), fifo.end());
    fifo.clear();
    regs.EP[0].EPRSPCTL = 0;
    regs.EP[0].EPINTSTS.bits = HSUSBD_EPINTSTS_BUFEMPTYIF_Msk | HSUSBD_EPINTSTS_TXPKIF_Msk;
}
static void USBD_WriteEpBuffer(uint32_t *, uint8_t *data, uint32_t n) {
    assert(fifo.empty());
    assert(n <= rw.ep_info[0].max_packet_size);
    fifo.assign(data, data + n);
    writes++;
    regs.EP[0].EPINTSTS.bits &= ~HSUSBD_EPINTSTS_BUFEMPTYIF_Msk;
    // A full packet may transmit before this function returns. Its event
    // must not subsequently be cleared while switching interrupt enables.
    if (complete_during_fill && n == rw.ep_info[0].max_packet_size)
        sent_by_host();
}
static void USBD_ReadEpBuffer(uint8_t *, uint32_t *, uint32_t) { assert(false); }
static void completed(uint8_t, uint32_t);
'''

irq = r'''
static void irq() {
    auto *ptr_usbd_info = &info;
    auto *ep = &regs.EP[0];
    uint8_t ep_addr = 0x81;
    uint32_t u32EpIntSts = ep->EPINTSTS & ep->EPINTEN;
    ep->EPINTSTS = u32EpIntSts;
''' + dispatch + "\n}\n"

tests = r'''
static void completed(uint8_t addr, uint32_t event) {
    assert(addr == 0x81 && event == ARM_USBD_EVENT_IN);
    assert(regs.EP[0].EPINTEN == 0);
    assert(rw.ep_info[0].num_transferred_total == rw.ep_info[0].num);
    callbacks++;
    if (rearm) {
        rearm = false;
        assert(USBDn_EndpointTransfer(&info, 0x81, next_data, sizeof next_data) == ARM_DRIVER_OK);
    }
}
static void setup(unsigned n, unsigned mps) {
    regs = USBD_t{};
    rw = RW_Info_t{};
    rw.cb_endpoint_event = completed;
    rw.ep_info[0].max_packet_size = mps;
    callbacks = writes = 0;
    complete_during_fill = rearm = false;
    input.resize(n);
    for (unsigned i = 0; i < n; i++) input[i] = (uint8_t)(i * 17U + i / 512U);
    wire.clear(); fifo.clear();
    // Include a stale packet-complete event; arming a new transfer must not
    // mistake it for completion of newly queued data.
    regs.EP[0].EPINTSTS.bits = HSUSBD_EPINTSTS_BUFEMPTYIF_Msk | HSUSBD_EPINTSTS_TXPKIF_Msk;
    assert(USBDn_EndpointTransfer(&info, 0x81, n ? input.data() : nullptr, n) == ARM_DRIVER_OK);
}
static void transfer(unsigned n, unsigned mps) {
    setup(n, mps);
    unsigned done = 0;
    do {
        irq(); // FIFO ready: queue exactly one packet, not complete it.
        const unsigned size = std::min(n - done, mps);
        assert(USBDn_EndpointTransferGetResult(&info, 0x81) == done);
        assert(fifo.size() == size);
        assert(callbacks == 0);
        assert(regs.EP[0].EPINTEN == HSUSBD_EPINTEN_TXPKIEN_Msk);
        if (!size) assert(regs.EP[0].EPRSPCTL & HSUSBD_EPRSPCTL_ZEROLEN_Msk);
        else if (size < mps) assert(regs.EP[0].EPRSPCTL & HSUSBD_EPRSPCTL_SHORTTXEN_Msk);
        // Empty indications/NAKs before TXPK must not queue another packet.
        const unsigned before = writes;
        regs.EP[0].EPINTSTS.bits |= HSUSBD_EPINTSTS_BUFEMPTYIF_Msk;
        for (unsigned i = 0; i < 10; i++) irq();
        assert(writes == before && callbacks == 0);
        sent_by_host();
        done += size;
        irq();
        assert(USBDn_EndpointTransferGetResult(&info, 0x81) == done);
        assert(callbacks == (done == n ? 1U : 0U));
    } while (done < n);
    assert(wire == input && fifo.empty());
    assert(callbacks == 1 && regs.EP[0].EPINTEN == 0);
    regs.EP[0].EPINTSTS.bits |= HSUSBD_EPINTSTS_TXPKIF_Msk;
    irq(); // no duplicate completion after completion
    assert(callbacks == 1);
}
int main() {
    for (unsigned mps : {64U, 512U})
        for (unsigned n : {16U, 64U, 65U, 128U, 400U, 408U, 512U, 513U, 2856U, 8192U, 0U})
            transfer(n, mps);

    setup(512, 512);
    complete_during_fill = true;
    irq(); // TXPK occurs inside FIFO fill, before EPINTEN is armed
    assert(callbacks == 0 && rw.ep_info[0].num_transferred_total == 0);
    irq(); // pending TXPK must still be delivered
    assert(callbacks == 1 && wire == input);

    setup(1024, 512);
    irq(); sent_by_host(); irq(); irq(); // one sent, one queued
    assert(rw.ep_info[0].num_transferred_total == 512);
    assert(USBDn_EndpointTransferAbort(&info, 0x81) == ARM_DRIVER_OK);
    fifo.clear(); // hardware FLUSH effect
    regs.EP[0].EPINTSTS.bits = HSUSBD_EPINTSTS_BUFEMPTYIF_Msk | HSUSBD_EPINTSTS_TXPKIF_Msk;
    irq();
    assert(callbacks == 0 && regs.EP[0].EPINTEN == 0);
    assert(rw.ep_info[0].num_transferred_total == 512); // do not count aborted packet
    assert(USBDn_EndpointTransfer(&info, 0x81, input.data(), 16) == ARM_DRIVER_OK);
    irq(); sent_by_host(); irq();
    assert(callbacks == 1 && rw.ep_info[0].num_transferred_total == 16);

    setup(16, 512);
    rearm = true;
    irq(); sent_by_host(); irq(); // callback immediately rearms
    assert(callbacks == 1 && regs.EP[0].EPINTEN == HSUSBD_EPINTEN_BUFEMPTYIEN_Msk);
    irq(); sent_by_host(); irq();
    assert(callbacks == 2);

    setup(16, 512);
    rw.cb_endpoint_event = nullptr;
    irq(); sent_by_host(); irq();
    assert(regs.EP[0].EPINTEN == 0 && rw.ep_info[0].num_transferred_total == 16);
    puts("PASS: IN FS/HS packets, short/ZLP, delayed/early TXPK, abort, stale events, callback rearm");
}
'''

with tempfile.TemporaryDirectory(prefix="numaker-usbd-in-") as tmp:
    exe = str(Path(tmp) / "test-usbd-in")
    subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++17", "-Wall", "-Wextra",
                    "-fsanitize=address,undefined", "-x", "c++", "-", "-o", exe],
                   input=mock + functions + irq + tests, text=True, check=True)
    subprocess.run([exe], check=True)
