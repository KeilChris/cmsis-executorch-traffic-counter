/* Host-only orchestration tests; no controller registers, probe or USB bus. */
#include <assert.h>
#include <stdio.h>
#include "usb_hil.c"

static ARM_USBD_SignalDeviceEvent_t dev_cb;
static ARM_USBD_SignalEndpointEvent_t ep_cb;
static uint8_t setup_bytes[8], control[64];
static uint8_t *buffers[2];
static uint32_t lengths[2], results[2], control_size, stalls, address;
static int abort_failure, init_failure, power_failure, transfer_failure;
static ARM_USBD_STATE bus = {.speed = ARM_USB_SPEED_HIGH, .vbus = 1, .active = 1};
static int32_t initialize(ARM_USBD_SignalDeviceEvent_t d, ARM_USBD_SignalEndpointEvent_t e) {
    dev_cb = d; ep_cb = e; return init_failure ? ARM_DRIVER_ERROR : ARM_DRIVER_OK;
}
static int32_t power(ARM_POWER_STATE state) { (void)state; return power_failure ? ARM_DRIVER_ERROR : 0; }
static int32_t connect_device(void) { return 0; }
static ARM_USBD_STATE get_state(void) { return bus; }
static int32_t set_address(uint8_t value) { address = value; return 0; }
static int32_t read_setup(uint8_t *s) { memcpy(s, setup_bytes, 8); return 0; }
static int32_t configure(uint8_t ep, uint8_t type, uint16_t mps) {
    assert(ep == HIL_OUT || ep == HIL_IN); assert(type == ARM_USB_ENDPOINT_BULK);
    assert(mps == 512 || mps == 64); return 0;
}
static int32_t unconfigure(uint8_t ep) { assert(ep == HIL_OUT || ep == HIL_IN); return 0; }
static int32_t set_stall(uint8_t ep, bool value) { (void)ep; stalls += value; return 0; }
static int32_t transfer(uint8_t ep, uint8_t *data, uint32_t size) {
    if (ep == 0x80) {
        assert(size <= sizeof(control)); memcpy(control, data, size); control_size = size;
        return 0;
    }
    assert(ep == HIL_OUT || ep == HIL_IN);
    unsigned d = ep == HIL_IN;
    buffers[d] = data; lengths[d] = size; results[d] = 0;
    return transfer_failure ? ARM_DRIVER_ERROR : 0;
}
static uint32_t result(uint8_t ep) { return results[ep == HIL_IN]; }
static int32_t abort_transfer(uint8_t ep) { (void)ep; return abort_failure ? ARM_DRIVER_ERROR : 0; }
ARM_DRIVER_USBD Driver_USBD1 = {
    .Initialize = initialize, .PowerControl = power, .DeviceConnect = connect_device,
    .DeviceGetState = get_state, .DeviceSetAddress = set_address, .ReadSetupPacket = read_setup,
    .EndpointConfigure = configure, .EndpointUnconfigure = unconfigure, .EndpointStall = set_stall,
    .EndpointTransfer = transfer, .EndpointTransferGetResult = result, .EndpointTransferAbort = abort_transfer
};
static void request(uint8_t type, uint8_t command, uint16_t value, uint16_t index, uint16_t size) {
    uint8_t s[] = {type, command, value, value >> 8, index, index >> 8, size, size >> 8};
    memcpy(setup_bytes, s, 8); ep_cb(0, ARM_USBD_EVENT_SETUP);
}
static void fresh(void) {
    abort_failure = init_failure = power_failure = transfer_failure = 0;
    stalls = address = 0;
    assert(usb_hil_start() == 0);
    dev_cb(ARM_USBD_EVENT_RESET);
    request(0, 9, 1, 0, 0);
    assert(configured && !usb_hil_status.errors);
}
static void complete_out(uint32_t n, uint16_t s) {
    assert(n <= HIL_CAPACITY);
    for (uint32_t i = 0; i < n; ++i) buffers[0][i] = pattern(i, s);
    results[0] = n; ep_cb(HIL_OUT, ARM_USBD_EVENT_OUT);
}
static void complete_in(void) {
    results[1] = lengths[1]; ep_cb(HIL_IN, ARM_USBD_EVENT_IN);
}
int main(void) {
    fresh();
    request(0, 5, 37, 0, 0);
    assert(address == 0); ep_cb(0x80, ARM_USBD_EVENT_IN); assert(address == 37);
    request(0x80, 6, 0x200, 0, 255);
    assert(control_size == 32 && control[22] == 0 && control[23] == 2);
    request(0x80, 6, 0x700, 0, 255);
    assert(control_size == 32 && control[22] == 64 && control[23] == 0);
    bus.speed = ARM_USB_SPEED_FULL;
    request(0x80, 6, 0x200, 0, 255); assert(control[22] == 64 && control[23] == 0);
    bus.speed = ARM_USB_SPEED_HIGH;
    request(0x80, 6, 0x3ee, 0, 255); assert(stalls == 1);
    request(0x40, HIL_ARM_IN, HIL_CAPACITY + 1, 0, 0); assert(stalls == 2);

    const unsigned sizes[] = {0,1,63,64,65,127,128,129,511,512,513,1023,1024,1025,8191,8192};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
        fresh(); request(0x40, HIL_LOOPBACK, sizes[i], 17, 0);
        complete_out(sizes[i], 17);
        assert(usb_hil_status.active == TX_ACTIVE);
        assert(!memcmp(rx.data, tx.data, sizes[i]));
        complete_in(); audit();
        assert(!usb_hil_status.errors && !usb_hil_status.active);
        assert(usb_hil_status.out_callbacks == 1 && usb_hil_status.in_callbacks == 1);
    }
    fresh(); request(0x40, HIL_ARM_OUT, 8192, 23, 0); complete_out(511, 23);
    assert(usb_hil_status.out_count == 511 && !usb_hil_status.errors);
    fresh(); request(0x40, HIL_ARM_OUT, 513, HIL_REARM | 41, 0);
    complete_out(513, 41); assert(usb_hil_status.active == RX_ACTIVE);
    complete_out(513, 42); assert(!usb_hil_status.active && usb_hil_status.out_callbacks == 2);
    assert(!usb_hil_status.errors);
    fresh(); request(0x40, HIL_ARM_IN, 513, HIL_REARM | 41, 0);
    complete_in(); assert(tx.data[0] == pattern(0, 42)); complete_in();
    assert(!usb_hil_status.active && usb_hil_status.in_callbacks == 2);
    assert(!usb_hil_status.errors);

    fresh(); request(0x40, HIL_ARM_OUT, 8192, 67, 0); request(0x40, HIL_ABORT, 0, 0, 0);
    assert(!usb_hil_status.active && usb_hil_status.aborts == 1);
    assert(!usb_hil_status.out_callbacks); rx.data[0] ^= 1; audit();
    assert(usb_hil_status.errors & HIL_ERR_LATE_WRITE);
    fresh(); request(0x40, HIL_ARM_OUT, 8192, 67, 0); abort_failure = 1;
    request(0x40, HIL_ABORT, 0, 0, 0);
    assert(usb_hil_status.active == RX_ACTIVE && !rx_frozen && stalls == 1);
    request(0x40, HIL_ARM_OUT, 1, 17, 0); assert(stalls == 2);
    fresh(); request(0x40, HIL_ARM_OUT, 10, 17, 0); complete_out(11, 17);
    assert(usb_hil_status.errors & HIL_ERR_COUNT);
    fresh(); request(0x40, HIL_ARM_OUT, 10, 17, 0); complete_out(10, 18);
    assert(usb_hil_status.errors & HIL_ERR_PATTERN);
    fresh(); request(0x40, HIL_ARM_IN, 10, 17, 0); results[1] = 9;
    ep_cb(HIL_IN, ARM_USBD_EVENT_IN); assert(usb_hil_status.errors & HIL_ERR_COUNT);
    fresh(); request(0x40, HIL_ARM_IN, 10, 17, 0); complete_in(); complete_in();
    assert(usb_hil_status.errors & HIL_ERR_CALLBACK);
    fresh(); rx.after[0] ^= 1; audit(); assert(usb_hil_status.errors & HIL_ERR_GUARD);
    fresh(); request(0x40, HIL_ARM_OUT, 8192, 17, 0);
    dev_cb(ARM_USBD_EVENT_RESET); assert(!configured && !usb_hil_status.active);
    assert(usb_hil_status.resets == 2);
    request(0, 9, 1, 0, 0); request(0x40, HIL_LOOPBACK, 64, 17, 0);
    complete_out(64, 17); complete_in(); assert(!usb_hil_status.errors);
    request(0, 9, 0, 0, 0); assert(!configured);
    fresh(); transfer_failure = 1; request(0x40, HIL_ARM_OUT, 10, 17, 0);
    assert(usb_hil_status.errors & HIL_ERR_DRIVER);
    assert(usb_hil_status.active == RX_ACTIVE); /* fail closed */
    fresh(); init_failure = 1; assert(usb_hil_start() == ARM_DRIVER_ERROR);
    assert(usb_hil_status.errors & HIL_ERR_DRIVER);
    init_failure = 0; power_failure = 1; assert(usb_hil_start() == ARM_DRIVER_ERROR);
    puts("PASS: descriptors/address staging, 16 sizes, short/rearm/abort/reset; negative counts/patterns/guards/late writes/callbacks/API errors");
    return 0;
}
