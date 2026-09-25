/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Target side of the Quake port for the Corstone-320 FVP: a headless run of
 * the engine, to bring it up and to check it without the board. The pak is
 * preloaded into DDR by the model (--data <cpu>=quake/data/id1/pak0.pak@0x90000000,
 * see the csolution's target-set), frames are counted and checksummed instead
 * of shown, and the run ends with the model when a timedemo has finished.
 */

#include <stdio.h>
#include <string.h>

#include "RTE_Components.h"
#include CMSIS_device_header

#include "port.h"

#define HUNK_SIZE (16U * 1024U * 1024U)
#define PAK_BASE  0x90000000U /* DDR4_3_S_BASE: nothing is linked there */

static uint8_t hunk[HUNK_SIZE] __attribute__((section(".bss.quake_hunk"), aligned(32)));

static uint32_t frames;
static uint32_t frame_sum; /* Adler-32 of the last frame: a run is comparable with the next */

void *port_hunk_base(void) { return hunk; }
size_t port_hunk_size(void) { return sizeof(hunk); }

const uint8_t *port_pak_image(void) { return (const uint8_t *)PAK_BASE; }

/* The DWT cycle counter wraps every 2^32 cycles; port_time() is called at
 * least once per frame, which is far more often. */
double port_time(void) {
  static uint32_t last;
  static uint64_t total;
  uint32_t now = DWT->CYCCNT;
  total += (uint32_t)(now - last);
  last = now;
  return (double)total / (double)SystemCoreClock;
}

static uint32_t adler32(const uint8_t *data, size_t size) {
  uint32_t a = 1U, b = 0U;
  for (size_t i = 0U; i < size; i++) {
    a = (a + data[i]) % 65521U;
    b = (b + a) % 65521U;
  }
  return (b << 16) | a;
}

void port_present(const uint8_t *frame, const uint8_t *palette_rgb) {
  (void)palette_rgb;
  frames++;
  if ((frames % 100U) == 0U) {
    frame_sum = adler32(frame, (size_t)QUAKE_VID_WIDTH * QUAKE_VID_HEIGHT);
    printf("[port] frame %u, adler32 %08x, t = %.2f s\n", (unsigned)frames, (unsigned)frame_sum, port_time());
  }
}

/* Semihosting SYS_EXIT ends the model. */
/* No NPU present path here: the span drawer fetches its texels. */
uint32_t *port_span_offsets(const uint8_t *surface_cache) {
  (void)surface_cache;
  return NULL;
}

void port_exit(int code) {
  register int r0 __asm__("r0") = 0x18;                       /* SYS_EXIT */
  register int r1 __asm__("r1") = code ? 0x20023 : 0x20026;   /* ADP_Stopped_RunTimeErrorUnknown : ApplicationExit */
  printf("[port] exit %d after %u frames\n", code, (unsigned)frames);
  __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
  for (;;) {
  }
}

/* "%i frames %5.1f seconds %5.1f fps" is CL_FinishTimeDemo's result line. */
void port_console_line(const char *text) {
  if (strstr(text, " frames ") != NULL && strstr(text, " fps") != NULL) {
    fputs(text, stdout);
    port_exit(0);
  }
}

int app_main(void) {
  static char *argv[] = {"quake", "+timedemo", "demo1"};

  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  setvbuf(stdout, NULL, _IONBF, 0);
  printf("Quake on the Corstone-320 FVP, headless: %ux%u, hunk %u MB, pak image at 0x%08x (%.4s)\n",
         QUAKE_VID_WIDTH, QUAKE_VID_HEIGHT, (unsigned)(HUNK_SIZE >> 20), (unsigned)PAK_BASE, (const char *)PAK_BASE);
  return quake_main((int)(sizeof(argv) / sizeof(argv[0])), argv);
}
