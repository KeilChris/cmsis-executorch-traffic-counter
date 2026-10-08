/*
 * Copyright (c) 2025-2026 Arm Limited. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Project-local SDS 3.1.0 USB transport (Keil::USB:Device:Custom Class).
// OUT callbacks signal only; the serialized receiving thread owns the buffer
// counters and collects results after USBD_EndpointRead has released its lock.
// See USB-driver.md. Keep the installed ARM::SDS pack unchanged.

#include <string.h>

#include "cmsis_os2.h"
#include "cmsis_compiler.h"
#include "rl_usb.h"
#include "Driver_USBD.h"

#include "sds.h"
#include "sdsio_client.h"
#include "sdsio_client_usb_mdk_config.h"

// SDS IO event flag identifiers
static osEventFlagsId_t sdsioOutEventFlagId;
static osEventFlagsId_t sdsioInEventFlagId;

// USBD bulk max packet size
static uint32_t bulkMaxPacketSize;

// USBD bulk OUT Endpoint address
static uint32_t bulkOutEpAddr;

// USBD bulk IN Endpoint address
static uint32_t bulkInEpAddr;

// USBD bulk OUT buffer
static uint8_t  bulkOutBuffer[SDSIO_USB_BULK_OUT_BUF_SIZE] __ALIGNED(32);
static uint32_t bulkOutCnt;
static uint32_t bulkOutIdx;

// SDS IO event flag values
#define SDSIO_CLIENT_EVENT_DATA_SENT        (1UL << 0)
#define SDSIO_CLIENT_EVENT_DATA_RECEIVED    (1UL << 1)

// Static function prototypes
static void USBD_Endpoint_Event (uint8_t ep_num, uint32_t event);

// Expansion macro used to create USBD_CDCn_ACM_ Callback functions
#define EXPAND_SYMBOL(prefix, value, suffix) prefix##value##suffix
#define CREATE_SYMBOL(prefix, value, suffix) EXPAND_SYMBOL(prefix, value, suffix)

// Callback function called during USBD_Initialize to initialize the USB Custom class instance.
// void USBD_CustomClassN_Initialize (void)
void CREATE_SYMBOL(USBD_CustomClass, SDSIO_USB_INSTANCE, _Initialize) (void) {
}

// Callback function called during USBD_Uninitialize to de-initialize the USB Custom class instance.
// void USBD_CustomClassN_Uninitialize (void)
void CREATE_SYMBOL(USBD_CustomClass, SDSIO_USB_INSTANCE, _Uninitialize) (void) {
}

// Callback function called when Endpoint Start was requested (by activating interface or configuration).
// void USBD_CustomClassN_EndpointStart (uint8_t ep_addr)
void CREATE_SYMBOL(USBD_CustomClass, SDSIO_USB_INSTANCE, _EndpointStart) (uint8_t ep_addr) {

  if (USBD_GetState(SDSIO_USB_INSTANCE).speed == USB_SPEED_HIGH) {
    bulkMaxPacketSize = 512U;
  } else {
    bulkMaxPacketSize = 64U;
  }

  if (ep_addr & 0x80) {
    // IN Endpoint
    bulkInEpAddr = ep_addr;
  } else {
    // OUT Endpoint
    bulkOutEpAddr = ep_addr;
    bulkOutCnt = 0U;
    bulkOutIdx = 0U;

    // Start reception of up to maximum packet size on bulk OUT endpoint
    USBD_EndpointRead(SDSIO_USB_DEVICE_INDEX,
                      bulkOutEpAddr,
                      bulkOutBuffer,
                      bulkMaxPacketSize);
  }
}

// Callback function called when DATA was sent or received on Endpoint m.
// void USBD_CustomClassN_EndpointM_Event (uint32_t event)
void CREATE_SYMBOL(USBD_CustomClass, SDSIO_USB_INSTANCE, _Endpoint1_Event)  (uint32_t event) { USBD_Endpoint_Event(1, event); }
void CREATE_SYMBOL(USBD_CustomClass, SDSIO_USB_INSTANCE, _Endpoint2_Event)  (uint32_t event) { USBD_Endpoint_Event(2, event); }
void CREATE_SYMBOL(USBD_CustomClass, SDSIO_USB_INSTANCE, _Endpoint3_Event)  (uint32_t event) { USBD_Endpoint_Event(3, event); }
void CREATE_SYMBOL(USBD_CustomClass, SDSIO_USB_INSTANCE, _Endpoint4_Event)  (uint32_t event) { USBD_Endpoint_Event(4, event); }
void CREATE_SYMBOL(USBD_CustomClass, SDSIO_USB_INSTANCE, _Endpoint5_Event)  (uint32_t event) { USBD_Endpoint_Event(5, event); }
void CREATE_SYMBOL(USBD_CustomClass, SDSIO_USB_INSTANCE, _Endpoint6_Event)  (uint32_t event) { USBD_Endpoint_Event(6, event); }
void CREATE_SYMBOL(USBD_CustomClass, SDSIO_USB_INSTANCE, _Endpoint7_Event)  (uint32_t event) { USBD_Endpoint_Event(7, event); }
void CREATE_SYMBOL(USBD_CustomClass, SDSIO_USB_INSTANCE, _Endpoint8_Event)  (uint32_t event) { USBD_Endpoint_Event(8, event); }
void CREATE_SYMBOL(USBD_CustomClass, SDSIO_USB_INSTANCE, _Endpoint9_Event)  (uint32_t event) { USBD_Endpoint_Event(9, event); }
void CREATE_SYMBOL(USBD_CustomClass, SDSIO_USB_INSTANCE, _Endpoint10_Event)  (uint32_t event) { USBD_Endpoint_Event(10, event); }
void CREATE_SYMBOL(USBD_CustomClass, SDSIO_USB_INSTANCE, _Endpoint11_Event)  (uint32_t event) { USBD_Endpoint_Event(11, event); }
void CREATE_SYMBOL(USBD_CustomClass, SDSIO_USB_INSTANCE, _Endpoint12_Event)  (uint32_t event) { USBD_Endpoint_Event(12, event); }
void CREATE_SYMBOL(USBD_CustomClass, SDSIO_USB_INSTANCE, _Endpoint13_Event)  (uint32_t event) { USBD_Endpoint_Event(13, event); }
void CREATE_SYMBOL(USBD_CustomClass, SDSIO_USB_INSTANCE, _Endpoint14_Event)  (uint32_t event) { USBD_Endpoint_Event(14, event); }
void CREATE_SYMBOL(USBD_CustomClass, SDSIO_USB_INSTANCE, _Endpoint15_Event)  (uint32_t event) { USBD_Endpoint_Event(15, event); }

/**
  \brief       Callback function called when DATA was sent or received on Endpoint 1.
  \param[in]   event       event on Endpoint:
                           - ARM_USBD_EVENT_OUT = data OUT received
                           - ARM_USBD_EVENT_IN  = data IN  sent
*/
static void USBD_Endpoint_Event (uint8_t ep_num, uint32_t event) {
  uint8_t ep_addr;

  if (event & ARM_USBD_EVENT_OUT) {
    // Data received on OUT Endpoint
    ep_addr = ep_num;
    if (ep_addr == bulkOutEpAddr) {
      // Do not query the result here: this higher-priority thread can preempt
      // USBD_EndpointRead while its endpoint semaphore is still held.
      osEventFlagsSet(sdsioOutEventFlagId, SDSIO_CLIENT_EVENT_DATA_RECEIVED);
    }
  }
  if (event & ARM_USBD_EVENT_IN) {
    // Data sent on IN Endpoint
    ep_addr = ep_num | 0x80;
    if (ep_addr == bulkInEpAddr) {
      osEventFlagsSet(sdsioInEventFlagId, SDSIO_CLIENT_EVENT_DATA_SENT);
    }
  }
}


/**
  \fn          int32_t sdsioClientInit (void)
  \brief       Initialize SDSIO-Client.
  \return      SDS_OK on success or
               a negative value on error (see \ref SDS_Return_Codes)
*/
int32_t sdsioClientInit (void) {
  int32_t  ret = SDS_ERROR_IO;

  sdsioOutEventFlagId = osEventFlagsNew(NULL);
  sdsioInEventFlagId  = osEventFlagsNew(NULL);
  if ((sdsioOutEventFlagId != NULL) && (sdsioInEventFlagId != NULL)) {
    // Initialize USB Device stack.
    ret = SDS_ERROR_IO;
    if (USBD_Initialize(SDSIO_USB_DEVICE_INDEX) == usbOK) {
      if (USBD_Connect(SDSIO_USB_DEVICE_INDEX) == usbOK) {
        // Enumeration is host-driven, not a data-transfer timeout. Keep the
        // device attached while waiting for a cable/host instead of tearing
        // down and reinitializing USB every SDSIO_USB_TIMEOUT ticks. Only the
        // SDS control thread waits here; camera, inference and display run.
        // Data Send/Receive retain their bounded SDSIO_USB_TIMEOUT waits.
        while (USBD_Configured(SDSIO_USB_DEVICE_INDEX) == false) {
          osDelay(1U);
        }
        ret = SDS_OK;
      }
    }
  } else {
    ret = SDS_ERROR_IO;
  }

  if (ret == SDS_OK) {
    SDS_PRINTF("SDSIO-Client USB interface initialized successfully\n");
  } else {
    SDS_PRINTF("SDSIO-Client USB interface initialization failed!\n");
    SDS_PRINTF("Ensure that device is connected via USB to the host PC running SDSIO-Server, then restart the application!\n");
  }

  return ret;
}

/**
  \fn          int32_t sdsioClientUninit (void)
  \brief       Un-Initialize SDSIO-Client.
  \return      SDS_OK on success or
               a negative value on error (see \ref SDS_Return_Codes)
*/
int32_t sdsioClientUninit (void) {
  USBD_Disconnect(SDSIO_USB_DEVICE_INDEX);
  USBD_Uninitialize(SDSIO_USB_DEVICE_INDEX);
  if (sdsioOutEventFlagId != NULL) {
    if (osEventFlagsDelete(sdsioOutEventFlagId) == osOK) {
      sdsioOutEventFlagId = NULL;
    }
  }
  if (sdsioInEventFlagId != NULL) {
    if (osEventFlagsDelete(sdsioInEventFlagId) == osOK) {
      sdsioInEventFlagId = NULL;
    }
  }
  return SDS_OK;
}

/**
  \fn          int32_t sdsioClientSend (const uint8_t *buf, uint32_t buf_size)
  \brief       Send data to SDSIO-Server (blocking).
  \param[in]   buf         pointer to buffer with data to send
  \param[in]   buf_size    buffer size in bytes
  \return      number of bytes successfully sent or
               a negative value on error (see \ref SDS_Return_Codes)
*/
int32_t sdsioClientSend (const uint8_t *buf, uint32_t buf_size) {
  uint32_t  num = 0U;
  int32_t   ret = SDS_ERROR_IO;
  int32_t   event_status;
  usbStatus usb_status;

  while (num < buf_size) {
    usb_status = USBD_EndpointWrite(SDSIO_USB_DEVICE_INDEX,
                                    bulkInEpAddr,
                                    buf + num,
                                    (buf_size - num));
    if (usb_status != usbOK) {
      // Error happened.
      if (usb_status == usbTimeout) {
        // Timeout happened.
        ret = SDS_ERROR_TIMEOUT;
      } else {
        // Error happened.
        ret = SDS_ERROR_IO;
      }
      break;
    }
    // Wait for data sent event.
    event_status = osEventFlagsWait(sdsioInEventFlagId,
                                    SDSIO_CLIENT_EVENT_DATA_SENT,
                                    osFlagsWaitAll,
                                    SDSIO_USB_TIMEOUT);
    if ((event_status & osFlagsError) != 0U) {
      if (event_status == (int32_t)osFlagsErrorTimeout) {
        // Timeout happened.
        ret = SDS_ERROR_TIMEOUT;
      } else {
        // Error happened.
        ret = SDS_ERROR_IO;
      }
      break;
    }
    num += USBD_EndpointWriteGetResult(SDSIO_USB_DEVICE_INDEX, bulkInEpAddr);
  }
  if (num != 0U) {
    ret = (int32_t)num;
  }
  return ret;
}

/* Receive bytes in the serialized SDS caller, not in the USB callback. */
int32_t sdsioClientReceive (uint8_t *buf, uint32_t buf_size, sdsioReceiveMode_t mode) {
  uint32_t num = 0U;
  int32_t ret = 0;
  uint32_t event_status, cnt, len;
  usbStatus usb_status;

  while (num < buf_size) {
    if (bulkOutCnt == 0U) {
      // Consume each completion exactly once, including a completion delivered
      // before the previous USBD_EndpointRead call returned. sdsio_client.c
      // serializes receivers with its mutex; only this thread owns the counts.
      event_status = osEventFlagsWait(sdsioOutEventFlagId,
                                     SDSIO_CLIENT_EVENT_DATA_RECEIVED,
                                     osFlagsWaitAll,
                                     mode == sdsioReceiveNonBlocking ? 0U : SDSIO_USB_TIMEOUT);
      if ((event_status & osFlagsError) != 0U) {
        if ((mode == sdsioReceiveNonBlocking) &&
            ((event_status == osFlagsErrorResource) || (event_status == osFlagsErrorTimeout))) {
          break;
        }
        ret = (event_status == osFlagsErrorTimeout) ? SDS_ERROR_TIMEOUT : SDS_ERROR_IO;
        break;
      }
      bulkOutIdx = 0U;
      bulkOutCnt = USBD_EndpointReadGetResult(SDSIO_USB_DEVICE_INDEX, bulkOutEpAddr);
      if (bulkOutCnt > sizeof(bulkOutBuffer)) {
        ret = SDS_ERROR_IO;
        break;
      }
    }

    cnt = bulkOutCnt;
    if (cnt > (buf_size - num)) {
      cnt = buf_size - num;
    }
    memcpy(buf + num, bulkOutBuffer + bulkOutIdx, cnt);
    num += cnt;
    bulkOutIdx += cnt;
    bulkOutCnt -= cnt;

    if (bulkOutCnt == 0U) {
      // Rearm even after a zero-length USB packet. Never clear the event here:
      // the rearm itself may synchronously deliver the next completion.
      len = buf_size - num;
      if (len == 0U) {
        len = bulkMaxPacketSize;
      } else if (len >= sizeof(bulkOutBuffer)) {
        len = sizeof(bulkOutBuffer);
      } else {
        len = ((len + bulkMaxPacketSize - 1U) / bulkMaxPacketSize) * bulkMaxPacketSize;
      }
      usb_status = USBD_EndpointRead(SDSIO_USB_DEVICE_INDEX, bulkOutEpAddr, bulkOutBuffer, len);
      if (usb_status != usbOK) {
        ret = (usb_status == usbTimeout) ? SDS_ERROR_TIMEOUT : SDS_ERROR_IO;
        break;
      }
      if (mode == sdsioReceiveNonBlocking) {
        break;
      }
    }
  }

  return (num != 0U) ? (int32_t)num : ret;
}
