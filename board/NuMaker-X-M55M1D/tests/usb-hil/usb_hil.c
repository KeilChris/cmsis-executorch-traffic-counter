/* Copyright 2026 Arm Limited and/or its affiliates. SPDX-License-Identifier: Apache-2.0 */
#include "usb_hil.h"
#include "Driver_USBD.h"
#include <stddef.h>
#include <string.h>

extern ARM_DRIVER_USBD Driver_USBD1;
#define usb Driver_USBD1
#define GUARD 32U
#define FILL 0xa5U
#define RX_ACTIVE 1U
#define TX_ACTIVE 2U
typedef struct {
    uint8_t before[GUARD], data[HIL_CAPACITY], after[GUARD];
} guarded_t;
static guarded_t rx __attribute__((aligned(32)));
static guarded_t tx __attribute__((aligned(32)));
static uint32_t rx_hash, tx_hash;
static uint16_t seed;
static uint8_t rx_frozen, tx_frozen, repeats, configured, pending_address;
static uint8_t reply[64];
volatile usb_hil_status_t usb_hil_status;
_Static_assert(sizeof(usb_hil_status_t) == 64, "wire status must be 64 bytes");

/* Local lab identity borrowed from the existing demo, NOT an allocated PID.
   Product/serial plus protocol magic prevent the verifier selecting SDS. */
static const uint8_t device_desc[] = {
    18,1,0,2,0,0,0,64,0x51,0xc2,0x07,0x80,0,1,1,2,3,1
};
static const uint8_t qualifier[] = {10,6,0,2,0,0,0,64,1,0};
static const uint8_t config_template[] = {
    9,2,32,0,1,1,0,0xc0,1,
    9,4,0,0,2,0xff,0,0,0,
    7,5,HIL_OUT,2,0,2,0, 7,5,HIL_IN,2,0,2,0
};

static uint8_t pattern(uint32_t i, uint16_t s) {
    return (uint8_t)((i * 73U + (i >> 8) * 19U + s * 29U) ^ ((i >> 3) + s));
}
static uint32_t digest(const guarded_t *b) {
    const uint8_t *p = (const uint8_t *)b;
    uint32_t h = 2166136261U;
    for (size_t i = 0; i < sizeof(*b); ++i) h = (h ^ p[i]) * 16777619U;
    return h;
}
static int32_t checked(int32_t rc) {
    if (rc != ARM_DRIVER_OK) {
        usb_hil_status.errors |= HIL_ERR_DRIVER;
        usb_hil_status.last_driver_error = rc;
    }
    return rc;
}
static void audit(void) {
    for (uint32_t i = 0; i < GUARD; ++i) {
        if (rx.before[i] != FILL || rx.after[i] != FILL ||
            tx.before[i] != FILL || tx.after[i] != FILL)
            usb_hil_status.errors |= HIL_ERR_GUARD;
    }
    if ((rx_frozen && digest(&rx) != rx_hash) ||
        (tx_frozen && digest(&tx) != tx_hash))
        usb_hil_status.errors |= HIL_ERR_LATE_WRITE;
}
static void freeze_rx(void) { rx_hash = digest(&rx); rx_frozen = 1; }
static void freeze_tx(void) { tx_hash = digest(&tx); tx_frozen = 1; }

static int32_t abort_active(void) {
    int32_t rc = ARM_DRIVER_OK;
    /* Do not expose/reuse a buffer if abort fails. Keep its active bit set. */
    if (usb_hil_status.active & RX_ACTIVE) {
        rc = checked(usb.EndpointTransferAbort(HIL_OUT));
        if (rc == ARM_DRIVER_OK) {
            usb_hil_status.active &= ~RX_ACTIVE;
            ++usb_hil_status.aborts;
            freeze_rx();
        }
    }
    if (usb_hil_status.active & TX_ACTIVE) {
        int32_t r = checked(usb.EndpointTransferAbort(HIL_IN));
        if (r == ARM_DRIVER_OK) {
            usb_hil_status.active &= ~TX_ACTIVE;
            ++usb_hil_status.aborts;
            freeze_tx();
        } else rc = r;
    }
    audit();
    return rc;
}
static int32_t arm_rx(void) {
    rx_frozen = 0;
    memset(&rx, FILL, sizeof(rx));
    usb_hil_status.active |= RX_ACTIVE;
    return checked(usb.EndpointTransfer(HIL_OUT, rx.data, usb_hil_status.requested));
}
static int32_t arm_tx(uint32_t length, int echo) {
    tx_frozen = 0;
    memset(&tx, FILL, sizeof(tx));
    for (uint32_t i = 0; i < length; ++i)
        tx.data[i] = echo ? rx.data[i] : pattern(i, seed);
    usb_hil_status.active |= TX_ACTIVE;
    return checked(usb.EndpointTransfer(HIL_IN, tx.data, length));
}

static void stall(void) { (void)checked(usb.EndpointStall(0x80, true)); }
static void send_reply(uint32_t size, uint32_t requested) {
    if (size > requested) size = requested;
    (void)checked(usb.EndpointTransfer(0x80, reply, size));
}
static uint16_t le16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static int valid_ep(uint16_t ep) { return ep == HIL_OUT || ep == HIL_IN; }

static void setup(void) {
    uint8_t s[8];
    if (checked(usb.ReadSetupPacket(s)) != ARM_DRIVER_OK) return;
    uint16_t value = le16(s + 2), index = le16(s + 4), length = le16(s + 6);
    pending_address = 0xff;
    audit();
    memset(reply, 0, sizeof(reply));
    if (s[0] == 0xc0 && s[1] == HIL_STATUS && value == 0 && index == 0) {
        usb_hil_status.configured = configured;
        if (usb_hil_status.active & RX_ACTIVE)
            usb_hil_status.out_count = usb.EndpointTransferGetResult(HIL_OUT);
        if (usb_hil_status.active & TX_ACTIVE)
            usb_hil_status.in_count = usb.EndpointTransferGetResult(HIL_IN);
        if (usb_hil_status.out_count > usb_hil_status.requested ||
            usb_hil_status.in_count > usb_hil_status.requested)
            usb_hil_status.errors |= HIL_ERR_COUNT;
        /* Callback and setup handling share one IRQ, so snapshot is coherent. */
        usb_hil_status_t snapshot = usb_hil_status;
        memcpy(reply, &snapshot, sizeof(snapshot));
        send_reply(sizeof(snapshot), length);
        return;
    }
    if (s[0] == 0x40 && length == 0 && configured) {
        if (s[1] == HIL_ABORT && value == 0 && index == 0) {
            if (abort_active() == ARM_DRIVER_OK) send_reply(0, 0); else stall();
            return;
        }
        if ((s[1] == HIL_ARM_OUT || s[1] == HIL_ARM_IN || s[1] == HIL_LOOPBACK) &&
            value <= HIL_CAPACITY && !usb_hil_status.active && !usb_hil_status.errors &&
            !(s[1] == HIL_LOOPBACK && (index & HIL_REARM))) {
            usb_hil_status.case_id++;
            usb_hil_status.mode = s[1];
            usb_hil_status.requested = value;
            usb_hil_status.out_count = usb_hil_status.in_count = 0;
            usb_hil_status.out_callbacks = usb_hil_status.in_callbacks = 0;
            seed = index & ~HIL_REARM;
            repeats = (index & HIL_REARM) ? 1 : 0;
            int32_t rc = s[1] == HIL_ARM_IN ? arm_tx(value, 0) : arm_rx();
            if (rc == ARM_DRIVER_OK) send_reply(0, 0); else stall();
            return;
        }
        stall(); return;
    }
    if (s[0] == 0x80 && s[1] == 6) { /* GET_DESCRIPTOR */
        uint8_t type = value >> 8, number = value & 255;
        if (type == 1 && number == 0 && index == 0) {
            memcpy(reply, device_desc, sizeof(device_desc));
            send_reply(sizeof(device_desc), length); return;
        }
        if ((type == 2 || type == 7) && number == 0 && index == 0) {
            uint16_t mps = usb.DeviceGetState().speed == ARM_USB_SPEED_HIGH ? 512 : 64;
            if (type == 7) mps = mps == 512 ? 64 : 512;
            memcpy(reply, config_template, sizeof(config_template));
            reply[1] = type;
            reply[22] = reply[29] = mps & 255;
            reply[23] = reply[30] = mps >> 8;
            send_reply(sizeof(config_template), length); return;
        }
        if (type == 6 && number == 0 && index == 0) {
            memcpy(reply, qualifier, sizeof(qualifier));
            send_reply(sizeof(qualifier), length); return;
        }
        if (type == 3 && number <= 3) {
            if (number == 0) {
                reply[0] = 4; reply[1] = 3; reply[2] = 9; reply[3] = 4;
            } else {
                const char *strings[] = {"", "Local test", "NuMaker USB HIL", "NU-USB-HIL-01"};
                size_t n = strlen(strings[number]);
                reply[0] = (uint8_t)(2 + n * 2); reply[1] = 3;
                for (size_t i = 0; i < n; ++i) reply[2 + 2 * i] = strings[number][i];
            }
            send_reply(reply[0], length); return;
        }
    } else if (s[0] == 0 && s[1] == 5 && value <= 127 && index == 0 && length == 0) {
        /* FADDR changes immediately in the DFP: defer until status-IN completes. */
        pending_address = (uint8_t)value;
        send_reply(0, 0); return;
    } else if (s[0] == 0 && s[1] == 9 && value <= 1 && index == 0 && length == 0) {
        if (abort_active() != ARM_DRIVER_OK) { stall(); return; }
        if (configured) {
            if (checked(usb.EndpointUnconfigure(HIL_OUT)) != ARM_DRIVER_OK ||
                checked(usb.EndpointUnconfigure(HIL_IN)) != ARM_DRIVER_OK) { stall(); return; }
        }
        configured = 0;
        if (value == 1) {
            usb_hil_status.mps = usb.DeviceGetState().speed == ARM_USB_SPEED_HIGH ? 512 : 64;
            if (checked(usb.EndpointConfigure(HIL_OUT, ARM_USB_ENDPOINT_BULK, usb_hil_status.mps)) != ARM_DRIVER_OK ||
                checked(usb.EndpointConfigure(HIL_IN, ARM_USB_ENDPOINT_BULK, usb_hil_status.mps)) != ARM_DRIVER_OK) { stall(); return; }
            configured = 1;
        }
        usb_hil_status.configured = configured;
        send_reply(0, 0); return;
    } else if (s[0] == 0x80 && s[1] == 8 && value == 0 && index == 0 && length == 1) {
        reply[0] = configured; send_reply(1, length); return;
    } else if (s[0] == 0x81 && s[1] == 10 && configured && value == 0 && index == 0 && length == 1) {
        send_reply(1, length); return; /* GET_INTERFACE, alternate zero */
    } else if (s[0] == 1 && s[1] == 11 && configured && value == 0 && index == 0 && length == 0) {
        send_reply(0, 0); return; /* SET_INTERFACE, only alternate zero */
    } else if (s[1] == 0 && value == 0 && length == 2 &&
               ((s[0] == 0x80 && index == 0) ||
                (s[0] == 0x81 && configured && index == 0) ||
                (s[0] == 0x82 && configured && valid_ep(index)))) {
        reply[0] = s[0] == 0x80 ? 1 : 0; /* self-powered, no remote wakeup/halt */
        send_reply(2, length); return;
    } else if (s[0] == 2 && s[1] == 1 && value == 0 && length == 0 && configured && valid_ep(index)) {
        if (checked(usb.EndpointStall((uint8_t)index, false)) == ARM_DRIVER_OK) send_reply(0, 0);
        else stall();
        return;
    }
    stall();
}

static void endpoint_event(uint8_t ep, uint32_t event) {
    if (ep == 0 && (event & ARM_USBD_EVENT_SETUP)) { setup(); return; }
    if (ep == 0x80 && (event & ARM_USBD_EVENT_IN)) {
        if (pending_address != 0xff) {
            (void)checked(usb.DeviceSetAddress(pending_address));
            pending_address = 0xff;
        }
        return;
    }
    if (ep == 0) return; /* driver completes the control status handshake */
    if (ep == HIL_OUT && (event & ARM_USBD_EVENT_OUT)) {
        if (!(usb_hil_status.active & RX_ACTIVE)) { usb_hil_status.errors |= HIL_ERR_CALLBACK; return; }
        usb_hil_status.active &= ~RX_ACTIVE;
        usb_hil_status.out_count = usb.EndpointTransferGetResult(ep);
        ++usb_hil_status.out_callbacks;
        uint32_t n = usb_hil_status.out_count;
        if (n > usb_hil_status.requested || n > HIL_CAPACITY) usb_hil_status.errors |= HIL_ERR_COUNT;
        else {
            for (uint32_t i = 0; i < HIL_CAPACITY; ++i) {
                if (rx.data[i] != (i < n ? pattern(i, seed) : FILL)) {
                    usb_hil_status.errors |= i < n ? HIL_ERR_PATTERN : HIL_ERR_GUARD;
                    break;
                }
            }
        }
        freeze_rx(); audit();
        if (usb_hil_status.errors) return;
        if (usb_hil_status.mode == HIL_LOOPBACK) (void)arm_tx(n, 1);
        else if (repeats) { --repeats; ++seed; (void)arm_rx(); }
    } else if (ep == HIL_IN && (event & ARM_USBD_EVENT_IN)) {
        if (!(usb_hil_status.active & TX_ACTIVE)) { usb_hil_status.errors |= HIL_ERR_CALLBACK; return; }
        usb_hil_status.active &= ~TX_ACTIVE;
        usb_hil_status.in_count = usb.EndpointTransferGetResult(ep);
        ++usb_hil_status.in_callbacks;
        uint32_t expected = usb_hil_status.mode == HIL_LOOPBACK ? usb_hil_status.out_count : usb_hil_status.requested;
        if (usb_hil_status.in_count != expected) usb_hil_status.errors |= HIL_ERR_COUNT;
        freeze_tx(); audit();
        if (!usb_hil_status.errors && repeats) { --repeats; ++seed; (void)arm_tx(expected, 0); }
    } else usb_hil_status.errors |= HIL_ERR_CALLBACK;
}

static void device_event(uint32_t event) {
    if (event & ARM_USBD_EVENT_RESET) {
        ++usb_hil_status.resets;
        configured = 0;
        usb_hil_status.configured = 0;
        usb_hil_status.active = 0;
        pending_address = 0xff;
        repeats = 0;
        /* The driver resets endpoint state before callback and hardware after
           return. Retain buffers, do not rearm here. Guard checks stay enabled. */
        rx_frozen = tx_frozen = 0;
    }
    if (event & ARM_USBD_EVENT_VBUS_OFF) {
        /* Link loss is not proof of released ownership. Explicit abort is
           required; failure leaves active buffers quarantined until reload. */
        (void)abort_active();
        configured = 0;
        usb_hil_status.configured = 0;
    }
}

int32_t usb_hil_start(void) {
    memset((void *)&usb_hil_status, 0, sizeof(usb_hil_status));
    memset(&rx, FILL, sizeof(rx)); memset(&tx, FILL, sizeof(tx));
    rx_frozen = tx_frozen = repeats = configured = 0;
    pending_address = 0xff;
    usb_hil_status.magic = HIL_MAGIC; usb_hil_status.version = HIL_VERSION;
    int32_t rc = checked(usb.Initialize(device_event, endpoint_event));
    if (rc == ARM_DRIVER_OK) rc = checked(usb.PowerControl(ARM_POWER_FULL));
    if (rc == ARM_DRIVER_OK) rc = checked(usb.DeviceConnect());
    return rc;
}
