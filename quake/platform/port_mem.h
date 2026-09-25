/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * External memories of the DevKit-E8: see port_mem.c.
 */
#ifndef QUAKE_PORT_MEM_H
#define QUAKE_PORT_MEM_H

#include <stdint.h>

#define PORT_HYPERRAM_BASE 0xA0000000U /* OSPI0 XIP window */
#define PORT_HYPERRAM_SIZE (64U * 1024U * 1024U)
#define PORT_FLASH_BASE    0xC0000000U /* OSPI1 XIP window, read-only */

/* Bring both memories up in XIP mode. 0, or -1 (HyperRAM) / -2 (flash). */
int port_mem_init(void);

/* Bring-up aid: prints 16 words written and read back, data cache off and on. */
void port_mem_probe(void);

/* Pattern test and bandwidth of the first `bytes` of the HyperRAM, printed;
 * returns the number of wrong words. Needs the DWT cycle counter running. */
int port_mem_test(uint32_t bytes);

#ifdef PORT_FLASH_PAK
/* The pak in the OSPI NOR flash, through the pack's CMSIS flash driver (no
 * memory mapping: see port_mem.c). port_flash_info(): 0 and the stored image's
 * size and adler32, 1 when the flash holds none, -1 on an error.
 * port_flash_restore(): copy the image into RAM. port_flash_store(): erase,
 * program, read back and compare, then write the header; minutes. All need the
 * RTOS running. The status is for a debugger: stage 1 driver set-up, 2 header,
 * 3 restoring, 4 erasing, 5 programming, 6 comparing (done_bytes counts in
 * 3..6), 7 stored; error = the stage's number that failed. */
struct port_flash_status {
  uint32_t stage, error, first_word, restore_ms, erase_ms, program_ms, done_bytes;
};
extern struct port_flash_status port_flash_status;
int port_flash_info(uint32_t *bytes, uint32_t *adler);
int port_flash_restore(uint8_t *to, uint32_t bytes);
int port_flash_store(const uint8_t *data, uint32_t bytes, uint32_t adler);
#endif

#endif /* QUAKE_PORT_MEM_H */
