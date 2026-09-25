/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Target side of the Quake port for the Alif DevKit-E8 (Cortex-M55 HP).
 *
 * app_main runs in the board layer's 32 kB RTX thread (board/DevKit-E8/
 * main.c). It brings the external memories up, tests the HyperRAM and starts
 * Quake in a thread of its own: Quake's render path keeps about 240 kB of
 * edge, surface and span lists on the stack, so that stack is a static block
 * in the DTCM (.bss.quake_stack in the scatter file). Quake's hunk is the
 * HyperRAM, the pak image is the OSPI flash, both memory mapped.
 *
 * This first version is headless, like the FVP's: frames are counted and
 * checksummed, a timedemo's result is the measurement. The present paths
 * (CPU, then NPU) replace port_present().
 */

#include <stdio.h>
#include <string.h>

#include "RTE_Components.h"
#include CMSIS_device_header

#include "cmsis_os2.h"

#include "port.h"
#include "port_mem.h"
#include "port_npu.h"
#include "port_splash.h"
#include "port_video.h"

/* Bring-up bisection: how far app_main goes before it stops (the console log
 * says where). 1: NPU timings, 2: + XIP set-up of both memories, 3: + HyperRAM
 * test, 4: + Quake. */
#ifndef PORT_BRINGUP_STAGE
#define PORT_BRINGUP_STAGE 4
#endif

#define HUNK_SIZE         (16U * 1024U * 1024U)
#define QUAKE_STACK_SIZE  (320U * 1024U)
#define STACK_FILL        0xA5A5A5A5A5A5A5A5ULL

static uint64_t quake_stack[QUAKE_STACK_SIZE / 8U] __attribute__((section(".bss.quake_stack"), used));

static uint32_t frames;

void *port_hunk_base(void) { return (void *)PORT_HYPERRAM_BASE; }
size_t port_hunk_size(void) { return HUNK_SIZE; }

/* The pak image: the OSPI flash where it is set up (DevKit-E8), else the
 * external RAM above the hunk, loaded there through the debugger (AppKit-E8:
 * tools/load_pak.sh). */
#if defined(RTE_Drivers_ISSI_FLASH_XIP_CORE) && !defined(PORT_FLASH_PAK)
#define PAK_BASE PORT_FLASH_BASE
#else
#define PAK_BASE (PORT_HYPERRAM_BASE + 0x01000000U)
#define PAK_IN_RAM 1
#endif

/* Where the pak is read from: PAK_BASE, or (PORT_FLASH_PAK) the OSPI flash once it holds a valid copy. */
static const uint8_t *pak_image = (const uint8_t *)PAK_BASE;
const uint8_t *port_pak_image(void) { return pak_image; }

/* The shareware pak0.pak, version 1.06: what Quake accepts without the registered data. */
#define PAK_BYTES   18689235U
#define PAK_ADLER32 0x785c230aU

/* For a debugger: the checksum of the pak image as the CPU last read it, how
 * often it was looked at, and whether the firmware is waiting for it. */
struct port_pak_status {
  uint32_t adler, checks, valid, waiting;
} port_pak_status __attribute__((used));

#ifdef PORT_FLASH_PAK
volatile uint32_t port_flash_gate __attribute__((used)) = 1U; /* a debugger can clear it at main to keep the firmware off the flash */
uint32_t port_flash_result __attribute__((used));
#endif
static int pak_valid_at(const uint8_t *data);
static int pak_valid(void) { return pak_valid_at((const uint8_t *)PAK_BASE); }

static int pak_valid_at(const uint8_t *data) {
  uint32_t a = 1U, b = 0U, left = PAK_BYTES;
  port_pak_status.checks++;
  if (memcmp(data, "PACK", 4) != 0) {
    port_pak_status.adler = 0U;
    return (int)(port_pak_status.valid = 0U);
  }
  while (left != 0U) { /* adler32, the modulo once per 5552 bytes (the sums fit 32 bits until then) */
    uint32_t n = left < 5552U ? left : 5552U;
    left -= n;
    while (n-- != 0U) {
      a += *data++;
      b += a;
    }
    a %= 65521U;
    b %= 65521U;
  }
  port_pak_status.adler = (b << 16) | a;
  return (int)(port_pak_status.valid = (port_pak_status.adler == PAK_ADLER32));
}

/* The RTOS system timer keeps running when a debugger detaches (the DWT
 * cycle counter does not). It wraps every 2^32 counts; this is called at
 * least once per frame. */
double port_time(void) {
  static uint32_t last;
  static uint64_t total;
  uint32_t now = osKernelGetSysTimerCount();
  total += (uint32_t)(now - last);
  last = now;
  return (double)total / (double)osKernelGetSysTimerFreq();
}

/* Stack bytes never written since start: the fill pattern from the bottom up
 * (RTX keeps its stack-check magic word in the first one). */
static uint32_t stack_headroom(void) {
  uint32_t words = 1U;
  while (words < (QUAKE_STACK_SIZE / 8U) && quake_stack[words] == STACK_FILL) {
    words++;
  }
  return words * 8U;
}

static uint32_t adler32(const uint8_t *data, size_t size) {
  uint32_t a = 1U, b = 0U;
  for (size_t i = 0U; i < size; i++) {
    a = (a + data[i]) % 65521U;
    b = (b + a) % 65521U;
  }
  return (b << 16) | a;
}

static int benchmark_mode;

/* The benchmark's results, also for a debugger (the console may be in other
 * hands): per present path the CPU-busy microseconds per frame, the load in
 * percent, the frames and the run's milliseconds; the checksums of the CPU
 * pass at frames 300, 400 and 900; `done` when the last run is over. */
struct port_results {
  uint32_t busy_us[4], load[4], frames[4], run_ms[4];
  uint32_t adler[3];
  uint32_t done;
} port_results __attribute__((used));
static int npu_ok = 1;
static const char *const names[] = {"CPU", "NPU sequential", "NPU pipelined", "NPU fetch"};

#define PRESENT_MODES 4 /* CPU, NPU sequential, NPU pipelined, NPU fetch (deferred texturing) */
static int frame_mode = -1;       /* how the frame being drawn is to be presented; -1: not decided yet */
static const uint8_t *fetch_cache; /* the surface cache, base of the span drawer's offsets */

/* The present path of the next frame. It is decided before the frame is drawn:
 * a frame of texel offsets can only go through qfetch. */
static int next_mode(int npu_ok, int fetch_ok) {
#ifdef PORT_TIMEDEMO
  int mode = benchmark_mode; /* one timedemo per present path */
#elif defined(PORT_PRESENT_CYCLE)
  int mode = (int)(((frames + 1U) / 300U) % PRESENT_MODES); /* A/B/C/D on the panel, 300 frames each */
#elif defined(PORT_PRESENT_FETCH)
  int mode = 3; /* deferred texturing (Ethos-U85 only; not validated on hardware yet) */
#else
  int mode = 2; /* the NPU, pipelined */
#endif
  if (mode == 3 && !fetch_ok) {
    mode = 2;
  }
  return npu_ok ? mode : 0;
}

/* CPU load. RTX's idle thread (a weak busy loop in RTX_Config.c) is replaced by
 * one that counts the cycles it gets: two passes of its loop are a few cycles
 * apart unless something ran in between, and that time is not idle time. What
 * is not idle is CPU work: renderer, hand-off pass, IO copies, interrupts. */
static volatile uint32_t idle_cycles;
static uint64_t run_cycles, run_idle; /* since the first frame of the run (a timedemo, or everything) */
static uint32_t run_frames;

__NO_RETURN void osRtxIdleThread(void *argument) {
  uint32_t last = DWT->CYCCNT;
  (void)argument;
  for (;;) {
    uint32_t now = DWT->CYCCNT;
    if ((now - last) < 256U) {
      idle_cycles += now - last;
    }
    last = now;
  }
}

#ifdef PORT_PROFILE
/* A sampling profiler without a probe: every millisecond a high-priority thread
 * looks at where the Quake thread was preempted (the PC in the exception frame
 * RTX keeps on that thread's stack) and counts it, in 64-byte buckets of the
 * code. Printed after the pipelined timedemo; tools' side: the linker map. */
#include "rtx_os.h"

#define PROFILE_BASE    0x80000000U
#define PROFILE_BUCKETS 8192U /* x 64 bytes: 512 kB of code */
static uint16_t profile_hits[PROFILE_BUCKETS];
static uint32_t profile_samples, profile_blocked, profile_elsewhere;
static osThreadId_t profile_target;
static volatile int profile_on;

static void profile_thread(void *argument) {
  (void)argument;
  for (;;) {
    osDelay(1U);
    const osRtxThread_t *thread = (const osRtxThread_t *)profile_target;
    if (!profile_on || thread == NULL) {
      continue;
    }
    profile_samples++;
    if (thread->state != osRtxThreadReady) { /* waiting (for the planes, the NPU): not CPU time */
      profile_blocked++;
      continue;
    }
    /* R4-R11, then S16-S31 when the frame is an extended one, then R0-R3, R12, LR, PC */
    const uint32_t *frame = (const uint32_t *)thread->sp + 8U + (((thread->stack_frame & 0x10U) == 0U) ? 16U : 0U);
    uint32_t bucket = (frame[6] - PROFILE_BASE) >> 6;
    if (bucket < PROFILE_BUCKETS) {
      if (profile_hits[bucket] != 0xFFFFU) {
        profile_hits[bucket]++;
      }
    } else {
      profile_elsewhere++;
    }
  }
}

static void profile_start(osThreadId_t target) {
  static const osThreadAttr_t attr = {.name = "profile", .stack_size = 1024U, .priority = osPriorityHigh};
  profile_target = target;
  (void)osThreadNew(profile_thread, NULL, &attr);
}

static void profile_print(void) {
  printf("[profile] %u samples, %u with the Quake thread waiting, %u outside the code\n", (unsigned)profile_samples,
         (unsigned)profile_blocked, (unsigned)profile_elsewhere);
  for (uint32_t i = 0U; i < PROFILE_BUCKETS; i++) {
    if (profile_hits[i] >= 150U) {
      printf("[profile] %08x %u\n", (unsigned)(PROFILE_BASE + (i << 6)), (unsigned)profile_hits[i]);
    }
  }
}
#endif

/* adler32 of the frame as the NPU resolves it: a pixel that holds the marker
 * 255 is the texel at its offset in the surface cache. Equal to the checksum
 * of the same frame drawn with texels: the fetch is bit-exact. */
static uint32_t adler32_resolved(const uint8_t *frame, const uint32_t *offsets, const uint8_t *cache, size_t size) {
  uint32_t a = 1U, b = 0U;
  for (size_t i = 0U; i < size; i++) {
    a = (a + (frame[i] == 255U ? cache[offsets[i]] : frame[i])) % 65521U;
    b = (b + a) % 65521U;
  }
  return (b << 16) | a;
}

/* A/B/C/D: the present path changes every 300 frames, the report says which one ran. */
void port_present(const uint8_t *frame, const uint8_t *palette_rgb) {
  static double last;
  static int previous;
  static uint32_t load_cycles, load_idle, seen_cycles, seen_idle;
  int mode = frame_mode < 0 ? next_mode(npu_ok, 0) : frame_mode; /* the first frame was drawn with texels */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; /* a debugger that detached has cleared it */
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  { /* the run's totals, in 64 bits: a timedemo outlasts the 32-bit cycle counter */
    uint32_t now = DWT->CYCCNT, idle_now = idle_cycles;
    if (run_frames != 0U) {
      run_cycles += (uint32_t)(now - seen_cycles);
      run_idle += (uint32_t)(idle_now - seen_idle);
    }
    seen_cycles = now;
    seen_idle = idle_now;
    run_frames++;
  }
  if (previous >= 2 && mode != previous) {
    port_npu_present_drain();
  }
  if ((mode == 3) != (previous == 3) && port_npu_fetch_select(mode == 3) != 0) {
    printf("[port] cannot load the %s method, staying on the CPU path\n", mode == 3 ? "qfetch" : "qpresent");
    npu_ok = 0;
    mode = 0; /* a frame of offsets shown by the CPU path: one wrong frame */
  }
  previous = mode;
  if (mode == 3) {
    port_npu_present_fetch(frame, palette_rgb, fetch_cache);
  } else if (mode == 2) {
    port_npu_present_pipelined(frame, palette_rgb);
  } else if (mode == 1) {
    if (port_npu_present(frame, palette_rgb) != 0) {
      printf("[port] NPU present failed, staying on the CPU path\n");
      npu_ok = 0;
    }
  } else {
    video_present(frame, palette_rgb);
  }
  frames++;
#ifdef PORT_TIMEDEMO
  if ((frames % 100U) == 0U && ((frames % 1000U) == 300U || (frames % 1000U) == 400U || (frames % 1000U) == 900U)) { /* the checksum frames */
#else
  if ((frames % 100U) == 0U) {
#endif
#ifdef PORT_TIMEDEMO
    /* A benchmark log: the frames whose checksums say the renderer still draws the same. */
    uint32_t sum = adler32(frame, (size_t)QUAKE_VID_WIDTH * QUAKE_VID_HEIGHT);
    if (frames < 1000U) {
      port_results.adler[frames == 300U ? 0 : frames == 400U ? 1 : 2] = sum;
    }
    printf("[port] frame %u, %s: adler32 %08x\n", (unsigned)frames, names[mode], (unsigned)sum);
  }
  if (0) {
#endif
    double now = port_time();
    uint32_t khz = SystemCoreClock / 1000U;
    /* The checksum of Quake's own frame says whether the renderer still draws
     * what it drew before (timedemo is deterministic; the FVP run is the reference). */
    /* The acceptance number of an optimization: CPU-busy time per frame (the
     * demo is deterministic, so the same frame numbers compare across runs). */
    uint32_t total = DWT->CYCCNT - load_cycles, idle = idle_cycles - load_idle;
    printf("[port] frame %u: CPU load %u %%, CPU busy %u us per frame\n", (unsigned)frames,
           (unsigned)(100U - (uint32_t)((uint64_t)idle * 100U / total)), (unsigned)((uint64_t)(total - idle) * 10U / khz));
    printf("[port] frame %u, %s: %.1f fps; CPU present %u us, hand-off %u us, NPU present %u us (NPU busy %u us, IO copies %u us), vsync wait %u us per frame; adler32 %08x\n",
           (unsigned)frames, names[mode], 100.0 / (now - last), (unsigned)(video_present_cycles / 100U * 1000U / khz),
           (unsigned)(npu_handoff_cycles / 100U * 1000U / khz), (unsigned)(npu_execute_cycles / 100U * 1000U / khz),
           (unsigned)(npu_busy_cycles / 100U * 1000U / khz), (unsigned)(npu_copy_cycles / 100U * 1000U / khz),
           (unsigned)(video_wait_cycles / 100U * 1000U / khz),
           (unsigned)(mode == 3 ? adler32_resolved(frame, port_npu_fetch_offsets(), fetch_cache, (size_t)QUAKE_VID_WIDTH * QUAKE_VID_HEIGHT)
                                : adler32(frame, (size_t)QUAKE_VID_WIDTH * QUAKE_VID_HEIGHT)));
    last = now;
    video_present_cycles = video_wait_cycles = npu_handoff_cycles = npu_execute_cycles = 0U;
    npu_busy_cycles = npu_copy_cycles = 0U;
    load_cycles = DWT->CYCCNT;
    load_idle = idle_cycles;
  }
}

uint32_t *port_span_offsets(const uint8_t *surface_cache) {
  uint32_t *offsets = port_npu_fetch_offsets();
  fetch_cache = surface_cache;
  frame_mode = next_mode(npu_ok, offsets != NULL);
  return frame_mode == 3 ? offsets : NULL;
}

void port_exit(int code) {
  port_results.done = 1U;
  printf("[port] exit %d after %u frames, stack headroom %u B\n", code, (unsigned)frames, (unsigned)stack_headroom());
  for (;;) {
    osDelay(osWaitForever);
  }
}

/* "%i frames %5.1f seconds %5.1f fps" is CL_FinishTimeDemo's result line. With
 * PORT_TIMEDEMO the demo runs once per present path, then the run ends. */
void port_console_line(const char *text) {
#ifdef PORT_TIMEDEMO
  if (strstr(text, " frames ") != NULL && strstr(text, " fps") != NULL) {
    /* The number an optimization is judged by: CPU time per frame of the same 969 frames. */
    uint64_t busy = run_cycles - run_idle;
    printf("[port] timedemo, %s: %s", names[benchmark_mode], text);
    port_results.busy_us[benchmark_mode] = (uint32_t)(busy * 1000000U / SystemCoreClock / (run_frames - 1U));
    port_results.load[benchmark_mode] = (uint32_t)(busy * 100U / run_cycles);
    port_results.frames[benchmark_mode] = run_frames - 1U;
    port_results.run_ms[benchmark_mode] = (uint32_t)(run_cycles * 1000U / SystemCoreClock);
    printf("[port] timedemo, %s: CPU busy %u us per frame, CPU load %u %% (%u frames)\n", names[benchmark_mode],
           (unsigned)(busy * 1000000U / SystemCoreClock / (run_frames - 1U)), (unsigned)(busy * 100U / run_cycles),
           (unsigned)(run_frames - 1U));
    run_cycles = run_idle = 0U;
    run_frames = 0U;
#ifdef PORT_PROFILE
    if (benchmark_mode == 2) { /* the pipelined run is over: that is the one profiled */
      profile_on = 0;
      profile_print();
    }
    profile_on = (benchmark_mode + 1 == 2);
#endif
    if (++benchmark_mode < (port_npu_fetch_offsets() != NULL ? PRESENT_MODES : PRESENT_MODES - 1)) {
      port_queue_command("timedemo demo1\n");
    } else {
      port_exit(0);
    }
  }
#else
  (void)text;
#endif
}

static void quake_thread(void *argument) {
#ifdef PORT_TIMEDEMO
#if QUAKE_VID_WIDTH < 320
  static char *argv[] = {"quake", "+viewsize", "120", "+timedemo", "demo1"}; /* no status bar: it is 320 columns wide */
#else
  static char *argv[] = {"quake", "+timedemo", "demo1"}; /* the benchmark: as fast as it goes, then stop */
#endif
#else
#if QUAKE_VID_WIDTH < 320
  static char *argv[] = {"quake", "+viewsize", "120"}; /* attract mode, no status bar: it is 320 columns wide */
#else
  static char *argv[] = {"quake"}; /* Quake's attract mode: the three demos in a loop */
#endif
#endif
  (void)argument;
  quake_main((int)(sizeof(argv) / sizeof(argv[0])), argv);
}

int app_main(void) {
  static const osThreadAttr_t attr = {
      .name = "quake",
      .stack_mem = quake_stack,
      .stack_size = sizeof(quake_stack),
      .priority = osPriorityBelowNormal, /* below the present thread (this one) */
  };

  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; /* the cycle counter times the memory test */
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  setvbuf(stdout, NULL, _IONBF, 0);
  printf("Quake on the Alif E8: %ux%u, hunk %u MB of external RAM at 0x%08x, pak image at 0x%08x\n",
         QUAKE_VID_WIDTH, QUAKE_VID_HEIGHT, (unsigned)(HUNK_SIZE >> 20), (unsigned)PORT_HYPERRAM_BASE,
         (unsigned)PAK_BASE);

  if (port_npu_rtos_init() != 0) {
    printf("[port] cannot create the present thread\n");
    return 1;
  }
#ifdef PORT_NPU_MEASURE
  /* Timing of every graph at boot (about three seconds). It runs its jobs out
   * of the shared pools; with PORT_ZERO_COPY those pools become the panel's
   * memory afterwards, so it goes first, before anything is on the panel. */
  if (port_npu_measure() != 0) {
    return 1;
  }
#endif
  printf("[port] stage 1 done\n");
  {
    int npu_status = port_npu_present_init(); /* with PORT_ZERO_COPY: says where the panel's buffers are */
    printf("[port] display: status %d, NPU present: status %d\n", video_init(), npu_status);
    port_splash_begin();
    port_splash_line("NPU CHECK OK");
  }
  port_splash_progress(10);
  port_splash_line("EXTERNAL RAM");
  if (PORT_BRINGUP_STAGE < 2) {
    return 0;
  }
#ifdef PORT_KEY_BEFORE_XRAM
  /* A first access that hangs the OSPI bus takes the debug port with it, at
   * every boot. Waiting for a key keeps a freshly reset board debuggable. */
  printf("[port] press a key to set the external RAM up\n");
  (void)getchar();
#endif
  if (port_mem_init() != 0) {
    return 1;
  }
  printf("[port] stage 2 done\n");
  port_splash_progress(20);
  if (PORT_BRINGUP_STAGE < 3) {
    return 0;
  }
#ifdef PAK_IN_RAM
  /* The PSRAM keeps its content through a reset of the SoC: a pak that is
   * already there is kept, and the test, which would overwrite it, skipped.
   * "There" means the whole image, as this CPU reads it: four magic bytes also
   * survive a short power-off that lets other cells decay, or a half-done load. */
  port_splash_line("CHECKING THE PAK IN RAM");
  int pak_present = pak_valid();
  port_splash_line(pak_present ? "PAK IN RAM: VALID" : "PAK IN RAM: NONE");
  port_splash_progress(30);
#ifdef PORT_FLASH_PAK
  /* The OSPI flash keeps the pak through a power cycle: a cold boot copies it
   * into the PSRAM; a valid PSRAM copy that the flash does not have yet is
   * stored there, once (port_flash_gate: a debugger can switch this off). */
  if (port_flash_gate != 0U) {
    uint32_t bytes = 0U, sum = 0U;
    int stored = port_flash_info(&bytes, &sum); /* 0: an image, 1: none, -1: error */
    port_flash_result = (uint32_t)(stored + 10);
    port_splash_line(stored == 0 ? "PAK IN FLASH: FOUND" : stored == 1 ? "PAK IN FLASH: NONE" : "FLASH: ERROR");
    if (!pak_present && stored == 0 && bytes == PAK_BYTES && sum == PAK_ADLER32) {
      printf("[port] copying the pak from the OSPI flash into the PSRAM\n");
      port_splash_line("COPYING FLASH TO RAM");
      pak_present = (port_flash_restore((uint8_t *)PAK_BASE, bytes) == 0) && pak_valid();
      port_flash_result = pak_present ? 1U : 2U; /* 1: restored and valid, 2: restore failed */
      port_splash_line(pak_present ? "COPY OK" : "COPY FAILED");
    } else if (pak_present && (stored == 1 || (stored == 0 && sum != PAK_ADLER32))) {
      printf("[port] storing the pak in the OSPI flash: this takes minutes\n");
      port_splash_line("STORING PAK IN FLASH: MINUTES");
      port_flash_result = (port_flash_store((const uint8_t *)PAK_BASE, PAK_BYTES, PAK_ADLER32) == 0) ? 3U : 4U; /* 3: stored and compared */
      port_splash_line(port_flash_result == 3U ? "STORED" : "STORE FAILED");
    }
  }
#endif
#else
  int pak_present = 0;
#endif
  printf("[port] first access to 0x%08x...%s\n", (unsigned)PORT_HYPERRAM_BASE, pak_present ? " pak found, RAM test skipped" : "");
  if (!pak_present && (port_mem_test(1U << 20) != 0 || port_mem_test(PORT_HYPERRAM_SIZE) != 0)) {
    return 1;
  }
  printf("[port] stage 3 done\n");
  if (PORT_BRINGUP_STAGE < 4) {
    return 0;
  }
#ifdef PAK_IN_RAM
  if (!pak_present) {
    SCB_CleanInvalidateDCache(); /* nothing of the test left in the cache while the debugger writes */
    printf("[port] load id1/pak0.pak to 0x%08x now (a debugger: restore ... binary 0x%08x); it is picked up when it is complete\n",
           (unsigned)PAK_BASE, (unsigned)PAK_BASE);
    port_pak_status.waiting = 1U;
    port_splash_line("NO PAK: LOAD PAK0.PAK TO 0XA1000000");
    do { /* no key: the console may be in other hands. Look again every two seconds. */
      osDelay(2000U);
      SCB_InvalidateDCache_by_Addr((void *)PAK_BASE, 0x02000000); /* the debugger writes behind the cache */
    } while (!pak_valid());
    port_pak_status.waiting = 0U;
  }
#endif
  if (memcmp(port_pak_image(), "PACK", 4) != 0) { /* flash or PSRAM, whichever pak_image says */
    printf("[port] no pak image at 0x%08x\n", (unsigned)PAK_BASE);
    return 1;
  }

  port_splash_line("STARTING QUAKE");
  port_splash_progress(100);
  port_splash_end();
  for (uint32_t i = 0U; i < (QUAKE_STACK_SIZE / 8U); i++) {
    quake_stack[i] = STACK_FILL;
  }
  osThreadId_t quake = osThreadNew(quake_thread, NULL, &attr);
  if (quake == NULL) {
    printf("[port] cannot start the Quake thread\n");
    return 1;
  }
#ifdef PORT_PROFILE
  profile_start(quake);
#endif
  return 0;
}
