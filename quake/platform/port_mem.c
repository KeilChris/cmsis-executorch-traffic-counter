/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * External memories of the DevKit-E8 for the Quake port, both memory mapped:
 *
 *   HyperRAM  64 MB ISSI IS66 on OSPI0, XIP window 0xA0000000: Quake's hunk.
 *   NOR flash ISSI IS25WX256 on OSPI1, XIP window 0xC0000000, read-only: the
 *             pak image (program it once with the pack's flash algorithm).
 *
 * The sequence is the Ensemble pack's (Boards/Templates/Baremetal/
 * demo_psram_e8.c, ospi_xip/): pulse the device's reset line, then let the
 * XIP set-up code configure the controller from RTE_Device.h. Pins and the
 * MPU regions of both windows are the board layer's and the pack's defaults.
 * Nothing is linked into either window: the scatter-loader runs before this.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "RTE_Components.h"
#include CMSIS_device_header

#include "Driver_IO.h"
#include "board_config.h"
#include "ospi_psram_xip.h"
#include "sys_ctrl_aes.h"
#include "sys_ctrl_ospi.h"
#if BOARD_APS512XXN_PSRAM_PRESENT
#include "APS512XXN_PSRAM.h" /* AppKit-E8: AP Memory PSRAM, no reset line */
#endif
#ifdef RTE_Drivers_ISSI_FLASH_XIP_CORE
#include "setup_flash_xip.h" /* ISSI NOR flash on OSPI1 */
#endif
#ifdef PORT_FLASH_PAK
#include "Driver_Flash.h"
#include "cmsis_os2.h"
#endif

#include "port_mem.h"
#include "port_splash.h"

static uint32_t cycles(void) { return DWT->CYCCNT; }

static void delay_ms(uint32_t ms) {
  uint32_t t0 = cycles();
  while ((uint32_t)(cycles() - t0) < ms * (SystemCoreClock / 1000U)) {
  }
}

/* Reset pulse with the devices' timing: the pack's demo has none and gets away
 * with it because a UART printf follows. The HyperRAM needs 150 us after the
 * release of its reset (tVCS) before the first access; a read that comes
 * earlier never gets its data strobe and stalls the bus, debug port included. */
static int32_t pulse_reset(ARM_DRIVER_GPIO *gpio, uint8_t pin) {
  if (gpio->Initialize(pin, NULL) != ARM_DRIVER_OK || gpio->PowerControl(pin, ARM_POWER_FULL) != ARM_DRIVER_OK ||
      gpio->SetDirection(pin, GPIO_PIN_DIRECTION_OUTPUT) != ARM_DRIVER_OK ||
      gpio->SetValue(pin, GPIO_PIN_OUTPUT_STATE_LOW) != ARM_DRIVER_OK) {
    return -1;
  }
  delay_ms(2U);
  if (gpio->SetValue(pin, GPIO_PIN_OUTPUT_STATE_HIGH) != ARM_DRIVER_OK) {
    return -1;
  }
  delay_ms(5U);
  return 0;
}

/* xorshift32: every word differs from its neighbours and from its address. */
static uint32_t next(uint32_t x) {
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return x;
}

int port_mem_init(void) {
  static ospi_psram_xip_config ram = {
      .instance = BOARD_PSRAM_OSPI_INSTANCE,
#if BOARD_ISSI_HYPERRAM_PRESENT
      .ram_init = NULL,
      .ram_type = RAM_TYPE_HYPERRAM,
#else
      .ram_init = aps512xxn_psram_init,
      .ram_type = RAM_TYPE_PSRAM,
#endif
  };

#if BOARD_ISSI_HYPERRAM_PRESENT
  extern ARM_DRIVER_GPIO ARM_Driver_GPIO_(BOARD_IS66_HYPERRAM_RESET_GPIO_PORT);
  if (pulse_reset(&ARM_Driver_GPIO_(BOARD_IS66_HYPERRAM_RESET_GPIO_PORT), BOARD_IS66_HYPERRAM_RESET_GPIO_PIN) != 0) {
    printf("[mem] HyperRAM reset failed\n");
    return -1;
  }
#endif
  /* A debugger load or reset restarts the core only: the controller is then
   * still in XIP mode from the run before, the RAM keeps working as it is, and
   * a second set-up would never finish (its register transfers get no answer
   * from a device that is in XIP mode). Keep what is there. */
  enable_ospi_clk(BOARD_PSRAM_OSPI_INSTANCE == 0 ? OSPI_INSTANCE_0 : OSPI_INSTANCE_1); /* the AES block is behind it */
  if ((((AES_Type *)(BOARD_PSRAM_OSPI_INSTANCE == 0 ? AES0_BASE : AES1_BASE))->AES_CONTROL & AES_CONTROL_XIP_EN) != 0U) {
    printf("[mem] external RAM is in XIP mode already (warm start), set-up skipped\n");
    return 0;
  }
  if (ospi_psram_xip_init(&ram) < 0) {
    printf("[mem] RAM XIP set-up failed\n");
    return -1;
  }
#ifdef RTE_Drivers_ISSI_FLASH_XIP_CORE
  extern ARM_DRIVER_GPIO ARM_Driver_GPIO_(BOARD_OSPI_FLASH_RESET_GPIO_PORT);
  if (pulse_reset(&ARM_Driver_GPIO_(BOARD_OSPI_FLASH_RESET_GPIO_PORT), BOARD_OSPI_FLASH_RESET_GPIO_PIN) != 0 ||
      setup_flash_xip() != 0 || !flash_xip_enabled()) {
    printf("[mem] OSPI flash XIP set-up failed\n");
    return -2;
  }
#endif
  return 0;
}

/* Bring-up aid: 16 words written and read one at a time with the data cache
 * off (single bus accesses), then the same through the cache (32-byte line
 * bursts); what comes back is printed next to what was written. */
void port_mem_probe(void) {
  volatile uint32_t *const ram = (volatile uint32_t *)PORT_HYPERRAM_BASE;
  for (int cached = 0; cached < 2; cached++) {
    uint32_t x = 0x2545F491U, wrong = 0U;
    if (cached) {
      SCB_EnableDCache();
    } else {
      SCB_DisableDCache();
    }
    for (uint32_t i = 0U; i < 16U; i++) {
      x = next(x);
      ram[0x1000U * (uint32_t)cached + i] = x;
    }
    if (cached) {
      SCB_CleanDCache_by_Addr((void *)(PORT_HYPERRAM_BASE + 0x4000U), 64);
      SCB_InvalidateDCache_by_Addr((void *)(PORT_HYPERRAM_BASE + 0x4000U), 64);
    }
    printf("[mem] probe, data cache %s:\n", cached ? "on" : "off");
    x = 0x2545F491U;
    for (uint32_t i = 0U; i < 16U; i++) {
      uint32_t got = ram[0x1000U * (uint32_t)cached + i];
      x = next(x);
      wrong += (got != x);
      printf("  [%2u] wrote %08x read %08x%s\n", (unsigned)i, (unsigned)x, (unsigned)got, got == x ? "" : "  <--");
    }
    printf("[mem] probe, data cache %s: %u of 16 wrong\n", cached ? "on" : "off", (unsigned)wrong);
  }
}

int port_mem_test(uint32_t bytes) {
  uint32_t *const ram = (uint32_t *)PORT_HYPERRAM_BASE;
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; /* a debugger that detached has cleared it */
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  const uint32_t words = bytes / sizeof(uint32_t);
  uint32_t errors = 0U, x = 0x2545F491U;
  uint32_t mhz = SystemCoreClock / 1000000U;

  uint32_t t0 = cycles();
  for (uint32_t i = 0U; i < words; i++) {
    x = next(x);
    ram[i] = x;
  }
  SCB_CleanDCache_by_Addr(ram, (int32_t)bytes);
  uint32_t t_write = cycles() - t0;

  SCB_InvalidateDCache_by_Addr(ram, (int32_t)bytes);
  x = 0x2545F491U;
  t0 = cycles();
  uint32_t bucket[16] = {0U}, shown = 0U;
  for (uint32_t i = 0U; i < words; i++) {
    x = next(x);
    if (ram[i] != x) {
      errors++;
      bucket[(uint64_t)i * 16U / words]++;
      if (shown < 4U) {
        shown++;
        printf("[mem]   word %u (0x%08x): wrote %08x read %08x\n", (unsigned)i, (unsigned)(PORT_HYPERRAM_BASE + 4U * i),
               (unsigned)x, (unsigned)ram[i]);
      }
    }
  }
  uint32_t t_read = cycles() - t0;
  if (errors != 0U) {
    printf("[mem]   errors per sixteenth of the range:");
    for (uint32_t b = 0U; b < 16U; b++) {
      printf(" %u", (unsigned)bucket[b]);
    }
    printf("\n");
  }

  /* Random 32-byte reads, a cache line each: what BSP traversal will see. */
  const uint32_t probes = 20000U;
  uint32_t sum = 0U;
  SCB_InvalidateDCache_by_Addr(ram, (int32_t)bytes);
  t0 = cycles();
  for (uint32_t i = 0U; i < probes; i++) {
    x = next(x);
    sum += ram[(x % words) & ~7U];
  }
  uint32_t t_random = cycles() - t0;

  printf("[mem] external RAM %u MB: %u errors; write %u kB/s, read %u kB/s, random line read %u ns (sum %08x)\n",
         (unsigned)(bytes >> 20), (unsigned)errors, (unsigned)((uint64_t)bytes * SystemCoreClock / t_write / 1024U),
         (unsigned)((uint64_t)bytes * SystemCoreClock / t_read / 1024U),
         (unsigned)((uint64_t)t_random * 1000U / mhz / probes), (unsigned)sum);
#ifdef RTE_Drivers_ISSI_FLASH_XIP_CORE
  printf("[mem] OSPI flash at 0x%08x starts with \"%.4s\"\n", (unsigned)PORT_FLASH_BASE, (const char *)PORT_FLASH_BASE);
#endif
  return (int)errors;
}

#ifdef PORT_FLASH_PAK
/* ---------------------------------------------------------------------------
 * The pak in the OSPI NOR flash: it survives a power cycle, the PSRAM does
 * not. The AppKit-E8 carries a Macronix MX66UW1G (1 Gbit, octal, OSPI1), not
 * the DevKit's ISSI part, and the pack's XIP set-up only knows the ISSI one:
 * so the flash is not memory mapped here. It is read and written through the
 * pack's CMSIS flash driver, and a cold boot copies the pak from the flash
 * into the PSRAM, where Quake reads it as ever. Layout: a header in the first
 * 4 kB sector, the image from the second one on.
 * --------------------------------------------------------------------------- */
extern ARM_DRIVER_FLASH ARM_Driver_Flash_(BOARD_OSPI_FLASH_INSTANCE); /* MX66UW1G.c */
extern ARM_DRIVER_GPIO  ARM_Driver_GPIO_(BOARD_OSPI_FLASH_RESET_GPIO_PORT);

#define FLASH_SECTOR      4096U
#define FLASH_IMAGE_AT    FLASH_SECTOR
#define FLASH_MAGIC       0x4B415051U /* "QPAK" */

struct flash_header {
  uint32_t magic, bytes, adler, reserved;
};

struct port_flash_status port_flash_status __attribute__((used));
static ARM_DRIVER_FLASH *const flash = &ARM_Driver_Flash_(BOARD_OSPI_FLASH_INSTANCE);
static int flash_open_done;

static int flash_wait(void) {
  for (uint32_t ms = 0U; ms < 60000U; ms++) {
    ARM_FLASH_STATUS status = flash->GetStatus();
    if (status.error) {
      return -1;
    }
    if (!status.busy) {
      return 0;
    }
    osDelay(1U);
  }
  return -1;
}

static int flash_open(void) {
  if (flash_open_done) {
    return 0;
  }
  port_flash_status.stage = 1U;
  if (pulse_reset(&ARM_Driver_GPIO_(BOARD_OSPI_FLASH_RESET_GPIO_PORT), BOARD_OSPI_FLASH_RESET_GPIO_PIN) != 0 ||
      flash->Initialize(NULL) != ARM_DRIVER_OK || flash->PowerControl(ARM_POWER_FULL) != ARM_DRIVER_OK) {
    port_flash_status.error = 1U;
    return -1;
  }
  flash_open_done = 1;
  return 0;
}

/* The driver counts in 16-bit units; addresses are bytes. */
static int flash_read(uint32_t addr, void *data, uint32_t bytes) {
  return (flash->ReadData(addr, data, (bytes + 1U) / 2U) < 0 || flash_wait() != 0) ? -1 : 0;
}

int port_flash_info(uint32_t *bytes, uint32_t *adler) {
  struct flash_header header;
  if (flash_open() != 0) {
    return -1;
  }
  port_flash_status.stage = 2U;
  if (flash_read(0U, &header, sizeof(header)) != 0) {
    port_flash_status.error = 2U;
    return -1;
  }
  port_flash_status.first_word = header.magic;
  if (header.magic != FLASH_MAGIC || header.bytes == 0U || header.bytes > 0x02000000U) {
    return 1; /* no image */
  }
  *bytes = header.bytes;
  *adler = header.adler;
  return 0;
}

int port_flash_restore(uint8_t *to, uint32_t bytes) {
  uint32_t t0 = osKernelGetTickCount();
  port_flash_status.stage = 3U;
  for (uint32_t done = 0U; done < bytes; done += 0x10000U) { /* 64 kB at a time: progress for a debugger */
    uint32_t n = bytes - done < 0x10000U ? bytes - done : 0x10000U;
    if (flash_read(FLASH_IMAGE_AT + done, to + done, n) != 0) {
      port_flash_status.error = 3U;
      return -1;
    }
    port_flash_status.done_bytes = done + n;
    port_splash_progress(30 + (int)((uint64_t)(done + n) * 60U / bytes)); /* 30 .. 90 % */
  }
  SCB_CleanDCache_by_Addr(to, (int32_t)bytes);
  port_flash_status.restore_ms = osKernelGetTickCount() - t0;
  return 0;
}

int port_flash_store(const uint8_t *data, uint32_t bytes, uint32_t adler) {
  static uint16_t chunk[FLASH_SECTOR / 2U];
  struct flash_header header = {FLASH_MAGIC, bytes, adler, 0U};
  uint32_t t0 = osKernelGetTickCount();
  uint32_t end = FLASH_IMAGE_AT + ((bytes + FLASH_SECTOR - 1U) & ~(FLASH_SECTOR - 1U));

  if (flash_open() != 0) {
    return -1;
  }
  port_flash_status.stage = 4U; /* erase: the header's sector first, so a half-written image is never taken for one */
  for (uint32_t addr = 0U; addr < end; addr += FLASH_SECTOR) {
    if (flash->EraseSector(addr) != ARM_DRIVER_OK || flash_wait() != 0) {
      port_flash_status.error = 4U;
      return -1;
    }
    port_flash_status.done_bytes = addr;
    if ((addr & 0x3FFFFU) == 0U) port_splash_progress(30 + (int)((uint64_t)addr * 40U / end)); /* erase: 30 .. 70 % */
  }
  port_flash_status.erase_ms = osKernelGetTickCount() - t0;
  port_flash_status.stage = 5U;
  t0 = osKernelGetTickCount();
  for (uint32_t done = 0U; done < bytes; done += FLASH_SECTOR) {
    uint32_t n = bytes - done < FLASH_SECTOR ? bytes - done : FLASH_SECTOR;
    memset(chunk, 0xFF, sizeof(chunk));
    memcpy(chunk, data + done, n);
    if (flash->ProgramData(FLASH_IMAGE_AT + done, chunk, FLASH_SECTOR / 2U) < 0 || flash_wait() != 0) {
      port_flash_status.error = 5U;
      return -1;
    }
    port_flash_status.done_bytes = done + n;
    if ((done & 0x3FFFFU) == 0U) port_splash_progress(70 + (int)((uint64_t)done * 20U / bytes)); /* program: 70 .. 90 % */
  }
  port_flash_status.program_ms = osKernelGetTickCount() - t0;
  port_flash_status.stage = 6U; /* read back and compare, then the header makes it valid */
  for (uint32_t done = 0U; done < bytes; done += FLASH_SECTOR) {
    uint32_t n = bytes - done < FLASH_SECTOR ? bytes - done : FLASH_SECTOR;
    if (flash_read(FLASH_IMAGE_AT + done, chunk, FLASH_SECTOR) != 0 || memcmp(chunk, data + done, n) != 0) {
      port_flash_status.error = 6U;
      port_flash_status.done_bytes = done;
      return -1;
    }
  }
  memset(chunk, 0xFF, sizeof(chunk));
  memcpy(chunk, &header, sizeof(header));
  if (flash->ProgramData(0U, chunk, sizeof(header) / 2U) < 0 || flash_wait() != 0) {
    port_flash_status.error = 7U;
    return -1;
  }
  port_flash_status.stage = 7U;
  return 0;
}
#endif /* PORT_FLASH_PAK */

