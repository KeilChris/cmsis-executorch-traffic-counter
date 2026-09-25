/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cat detection with YOLO26n on the Ethos-U85 of the Alif Ensemble E8 (M55-HP),
 * live from the camera to the panel, with SDS recording and playback.
 *
 * Each frame:
 *   1. the input: the newest camera frame (the AppKit's OV5675 through the
 *      E8's ISP, already the 416x416 centre square; or an MT9M114's RGB565), its centre
 *      square scaled to the 416x416 RGB888 model input; or, in playback, the
 *      next CameraIn record from SDSIO-Server; without a camera, the test image
 *      (yolo/test_image.c)
 *   2. recording: the input goes into the CameraIn stream
 *   3. the detector: YOLO26n on the NPU, the head's decode on the CPU (detector.cpp)
 *   4. the result goes into the Detections stream while recording or playing back
 *   5. the panel: the picture in a 480x480 view (an RGB565 camera square 1:1, else
 *      the input 1:1); the boxes, the detections and the raw scores around it
 *      in the display thread, while the NPU works on the next frame;
 *      double-buffered
 *
 * The console is a log buffer that the debugger reads (board layer); the latest
 * result and the frame timing are in the global `yolo_status`.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "RTE_Components.h"
#include CMSIS_device_header

#include "cmsis_os2.h"
#include "se_services_port.h"
#include "services_lib_api.h"

#include "detector.h"
#include "model_pte.h"
#include "image.h"
#include "test_image.h"
#ifdef APP_HAS_DISPLAY
#include "board_display.h"
#endif
#ifdef APP_HAS_CAMERA
#include "camera.h"
#endif
#ifdef APP_HAS_SDS
#include "rec_play.h"
#endif

// ---------------------------------------------------------------------------
// What the debugger reads.
// ---------------------------------------------------------------------------
struct YoloStatus {
  uint32_t frames;          // frames processed
  int32_t source;           // 0 test image, 1 camera, 2 SDS playback
  int32_t mode;             // rec_play_mode_t
  int32_t camera_status;    // camera_init(): 0 ok, else the failing step; -1 no camera support
  uint32_t camera_frames;   // frames the camera delivered
  uint32_t camera_errors;
  int32_t detector_status;  // last ExecuTorch error, 0 ok
  uint32_t frame_us;        // last frame, input to presented
  uint32_t input_us;        // waiting for the camera input or the playback read
  uint32_t convert_us;      // camera thread: ISP output to model input
  uint32_t detect_us;       // detector_run: execute and decode
  uint32_t npu_us;          // the NPU job alone
  uint32_t picture_us;      // the input into the frame buffer (vision thread)
  uint32_t display_us;      // display thread: boxes, text, heat maps and cache clean
  uint32_t sds_us;          // SDS writes
  float fps;                // frames per second, averaged over one second
  float wb_gain[3];          // camera white balance gains R, G, B (ISP path)
  uint32_t camera_video;     // 1: the ISP captures continuously, 0: a snapshot per frame
  detections_t result;      // the detections of the last frame
};

extern "C" {
volatile YoloStatus yolo_status;
// The benchmark build's result (YOLO_BENCHMARK).
struct {
  int32_t status;
  uint32_t runs, count;
  uint32_t total_us_min, total_us_sum, npu_us_min, npu_us_sum;
} volatile yolo_benchmark;
// Every 8th pixel of the last model input (52 x 52 RGB888): small enough for
// a debugger to read in two requests and look at the picture on the host.
uint8_t yolo_thumbnail[52 * 52 * 3];
}

namespace {

constexpr int kSize = TEST_IMAGE_SIZE;
constexpr uint32_t kInputBytes = kSize * kSize * 3;

alignas(32) uint8_t g_input[kInputBytes] __attribute__((section(APP_POOL_SECTION)));

#ifdef APP_HAS_DISPLAY
// One frame buffer in SRAM0, the other in SRAM1 (powered by sram1_power_on, not initialised).
constexpr uint32_t kFrameBytes = IMAGE_PANEL_W * IMAGE_PANEL_H * 3;
alignas(32) uint8_t g_framebuffer0[kFrameBytes] __attribute__((section(APP_FRAMEBUFFER_SECTION)));
alignas(32) uint8_t g_framebuffer1[kFrameBytes] __attribute__((section(".bss.sram1")));
uint8_t* const g_framebuffer[2] = {g_framebuffer0, g_framebuffer1};
#endif

// SRAM1 (4 MB at 0x02400000) is off after boot on the AppKit-E8; a store to it
// hangs the bus. Power and clock it through the Secure Enclave before any use.
bool sram1_power_on() {
  uint32_t error = 0;
  if (SERVICES_power_memory_req(se_services_s_handle, POWER_MEM_SRAM_0_ENABLE | POWER_MEM_SRAM_1_ENABLE, &error) !=
          SERVICES_REQ_SUCCESS ||
      error != 0) {
    printf("SRAM1: power request failed (error %lu)\n", static_cast<unsigned long>(error));
    return false;
  }
  if (SERVICES_clocks_enable_clock(se_services_s_handle, CLKEN_SRAM1, true, &error) != SERVICES_REQ_SUCCESS || error != 0) {
    printf("SRAM1: clock request failed (error %lu)\n", static_cast<unsigned long>(error));
    return false;
  }
  return true;
}

inline uint32_t cycles() { return DWT->CYCCNT; }
inline uint32_t us(uint32_t c) { return static_cast<uint32_t>(static_cast<uint64_t>(c) * 1000000U / SystemCoreClock); }

enum Source { kTestImage = 0, kCamera = 1, kPlayback = 2 };

#if defined(APP_HAS_CAMERA) && CAMERA_PLANAR_RGB
// The camera thread: every ISP frame, right at its end, turned into a model
// input in one of three slots (the ISP rewrites its one buffer with the next
// frame, so it has to be copied out at once). The vision thread takes the
// newest slot; the camera thread never writes the newest one or the one
// being read. Frame rate: the camera's, not capture + processing.
alignas(32) uint8_t g_slot[3][kInputBytes] __attribute__((section(".bss.sram1")));
volatile int g_slot_newest = -1;   // the newest complete input, -1 when taken
volatile int g_slot_reading = -1;  // the vision thread's input
osEventFlagsId_t g_slot_event;
uint64_t g_camera_stack[512] __attribute__((section(APP_POOL_SECTION)));
volatile uint32_t g_convert_cycles;

__NO_RETURN void camera_thread(void*) {
  for (;;) {
    const void* frame = camera_frame(200);
    if (frame == nullptr) continue;
    osKernelLock();
    int slot = 0;
    while (slot == g_slot_newest || slot == g_slot_reading) ++slot;
    osKernelUnlock();
    const uint32_t t0 = DWT->CYCCNT;
    // The AppKit's OV5675 module is mounted turned: CAMERA_QUARTER_TURNS (camera.h) gives the upright picture.
    image_planar_to_input(static_cast<const uint8_t*>(frame), CAMERA_WIDTH, CAMERA_HEIGHT, g_slot[slot], kSize, CAMERA_QUARTER_TURNS);
    g_convert_cycles = DWT->CYCCNT - t0;
    camera_release();
    osKernelLock();
    g_slot_newest = slot;
    osKernelUnlock();
    osEventFlagsSet(g_slot_event, 1U);
  }
}

// The newest camera input, waiting up to `timeout_ms`; nullptr on a timeout.
const uint8_t* camera_input(uint32_t timeout_ms) {
  for (;;) {
    osKernelLock();
    const int slot = g_slot_newest;
    if (slot >= 0) {
      g_slot_reading = slot;
      g_slot_newest = -1;
    }
    osKernelUnlock();
    if (slot >= 0) return g_slot[slot];
    if (osEventFlagsWait(g_slot_event, 1U, osFlagsWaitAny, timeout_ms) & osFlagsError) return nullptr;
  }
}

void camera_input_done() { g_slot_reading = -1; }
#endif


#ifdef APP_HAS_DISPLAY
// Around the picture (480 x 160 above, 480 x 160 below):
//   above: the verdict, and each detection with its score, position and size
//   below: the model's raw cat scores of every anchor as three heat maps
//          (stride 8, 16, 32), the anchors above the threshold, the highest
//          score, the timing and the input source
void draw_status(uint8_t* fb, const detections_t& det, const detector_scores_t* scores, Source source, int mode,
                 float fps) {
  constexpr uint32_t kGrey = 0xC0C0C0U, kDim = 0x707070U, kGreen = 0x00FF00U, kRed = 0xFF3030U, kBlue = 0x40A0FFU,
                     kYellow = 0xFFD000U;
  image_fill(fb, 0, 0, IMAGE_PANEL_W, IMAGE_VIEW_TOP, 0x000000U);
  image_fill(fb, 0, IMAGE_VIEW_TOP + IMAGE_VIEW, IMAGE_PANEL_W, IMAGE_PANEL_H, 0x000000U);

  char line[48];
  image_text(fb, 12, 8, 2, "YOLO26N CAT DETECTOR  ETHOS-U85", kDim);
  if (det.count > 0) {
    float best = 0.0f;
    for (uint32_t i = 0; i < det.count; ++i) best = det.det[i].score > best ? det.det[i].score : best;
    snprintf(line, sizeof(line), "%lu CAT%s  %.2f", static_cast<unsigned long>(det.count), det.count == 1 ? "" : "S",
             static_cast<double>(best));
    image_text(fb, 12, 30, 4, line, kYellow);
  } else {
    image_text(fb, 12, 30, 4, "NO CAT", 0x606060U);
  }
  // One line per detection: score, top left and size in input pixels.
  constexpr uint32_t kListed = 4;
  for (uint32_t i = 0; i < det.count && i < kListed; ++i) {
    const detection_t& d = det.det[i];
    snprintf(line, sizeof(line), "%lu %.2f  AT %3d,%3d  %3dX%3d", static_cast<unsigned long>(i + 1),
             static_cast<double>(d.score), static_cast<int>(d.x1), static_cast<int>(d.y1),
             static_cast<int>(d.x2 - d.x1), static_cast<int>(d.y2 - d.y1));
    image_text(fb, 12, 70 + static_cast<int>(i) * 22, 2, line, kGreen);
  }
  if (det.count > kListed) {
    snprintf(line, sizeof(line), "+%lu MORE", static_cast<unsigned long>(det.count - kListed));
    image_text(fb, 360, 70 + 3 * 22, 2, line, kGreen);
  }

  const int y = IMAGE_VIEW_TOP + IMAGE_VIEW + 8;
  if (scores != nullptr) {
    // Three maps of 104 x 104 (the 52 x 52 cells of stride 8 at 2 pixels a cell).
    image_score_maps(fb, 12, y, scores, DETECTOR_INPUT_SIZE, 2);
    image_text(fb, 12, y + 110, 1, "CAT SCORE PER ANCHOR  S8  S16  S32", kDim);
    snprintf(line, sizeof(line), "MAX %.2f", static_cast<double>(scores->max));
    image_text(fb, 360, y, 2, line, scores->max >= scores->threshold ? kYellow : kGrey);
    snprintf(line, sizeof(line), "HITS %lu", static_cast<unsigned long>(scores->candidates));
    image_text(fb, 360, y + 22, 2, line, kGrey);
    image_fill(fb, 12, y + 128, 26, y + 142, 0xFFFFFFU);
    snprintf(line, sizeof(line), "AT OR ABOVE THRESHOLD %.2f", static_cast<double>(scores->threshold));
    image_text(fb, 34, y + 128, 2, line, kDim);
  }
  snprintf(line, sizeof(line), "%.1f FPS", static_cast<double>(fps));
  image_text(fb, 360, y + 44, 2, line, kGrey);
  snprintf(line, sizeof(line), "NPU %.1f", det.npu_us / 1000.0);
  image_text(fb, 360, y + 66, 2, line, kGrey);

  const char* what = source == kCamera ? "CAMERA" : source == kPlayback ? "SDS PLAY" : "TEST IMG";
  uint32_t colour = source == kPlayback ? kBlue : kGreen;
#ifdef APP_HAS_SDS
  if (mode == REC_PLAY_RECORD) {
    what = "REC";
    colour = kRed;
  }
#else
  (void)mode;
#endif
  image_fill(fb, 360, y + 92, 376, y + 108, colour);
  image_text(fb, 382, y + 94, 2, what, colour);
}

// The display thread draws everything but the picture while the vision
// thread runs the NPU on the next frame: the picture goes into the back
// buffer in the vision thread (the input slot is free again right after),
// the rest here, from a copy of the frame's results.
constexpr int kAnchors = (kSize / 8) * (kSize / 8) + (kSize / 16) * (kSize / 16) + (kSize / 32) * (kSize / 32);
struct DisplayJob {
  uint8_t* fb;
  detections_t det;
  detector_scores_t scores;
  bool has_scores;
  int view;  // picture width on the panel, for the boxes
  Source source;
  int mode;
  float fps;
};
DisplayJob g_job;
int8_t g_job_scores[kAnchors];
osSemaphoreId_t g_display_go;    // a job is ready
osSemaphoreId_t g_display_idle;  // the last job is on the panel: the other buffer is free
uint64_t g_display_stack[1024] __attribute__((section(APP_POOL_SECTION)));

__NO_RETURN void display_thread(void*) {
  for (;;) {
    osSemaphoreAcquire(g_display_go, osWaitForever);
    const uint32_t t0 = cycles();
    uint8_t* fb = g_job.fb;
    image_draw_detections(fb, &g_job.det, kSize, g_job.view);
    draw_status(fb, g_job.det, g_job.has_scores ? &g_job.scores : nullptr, g_job.source, g_job.mode, g_job.fps);
    SCB_CleanDCache_by_Addr(fb, static_cast<int32_t>(kFrameBytes));
    yolo_status.display_us = us(cycles() - t0);
    display_present(fb);
    // Presented takes effect at the panel's next refresh: until then the
    // other buffer is still on the panel and must not be drawn over.
    display_wait_shown(fb);
    osSemaphoreRelease(g_display_idle);
  }
}
#endif

}  // namespace

extern "C" int app_main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  memset(const_cast<YoloStatus*>(&yolo_status), 0, sizeof(yolo_status));
  printf("YOLO26n cat detector: Ethos-U85, input %dx%d RGB888, core %lu MHz\n", kSize, kSize,
         static_cast<unsigned long>(SystemCoreClock / 1000000U));

  if (!sram1_power_on()) return 1;

#ifdef YOLO_BENCHMARK
  // Benchmark build (define YOLO_BENCHMARK, and YOLO_BENCHMARK_SRAM for the
  // program in SRAM): no camera, display or SDS; the test image through the
  // detector YOLO_BENCHMARK times, the timings in yolo_benchmark.
  {
    const uint8_t* program = model_pte;
#ifdef YOLO_BENCHMARK_SRAM
    // A copy of the program in SRAM0. The NPU driver must fetch the command
    // stream and region 0 (the weights) through the SRAM port, NPU_QCONFIG=0
    // and NPU_REGIONCFG_0=0: by default they go through the EXT port, which
    // reaches the MRAM but not the SRAM (NPU status 0x804, a bus abort on the
    // EXT interface, err 35).
    static uint8_t copy[YOLO_BENCHMARK_SRAM] __attribute__((section(APP_POOL_SECTION), aligned(16)));
    if (model_pte_size > sizeof(copy)) {
      printf("benchmark: program %lu bytes, copy buffer %u\n", model_pte_size, static_cast<unsigned>(sizeof(copy)));
      for (;;) osDelay(1000);
    }
    memcpy(copy, model_pte, model_pte_size);
    SCB_CleanDCache_by_Addr(copy, static_cast<int32_t>(model_pte_size));
    program = copy;
#endif
    int32_t err = detector_init_from(program);
    yolo_benchmark.status = err;
    detections_t d{};
    for (int i = 0; err == 0 && i < YOLO_BENCHMARK; ++i) {
      const uint32_t t0 = cycles();
      err = detector_run(test_image, &d);
      const uint32_t t = us(cycles() - t0);
      // The first run loads the method; after a debugger reset of the core
      // alone it can also meet the NPU still busy with the last job (err 35).
      if (i == 0) {
        err = 0;
        continue;
      }
      yolo_benchmark.runs++;
      yolo_benchmark.total_us_sum += t;
      yolo_benchmark.npu_us_sum += d.npu_us;
      if (yolo_benchmark.total_us_min == 0 || t < yolo_benchmark.total_us_min) yolo_benchmark.total_us_min = t;
      if (yolo_benchmark.npu_us_min == 0 || d.npu_us < yolo_benchmark.npu_us_min) yolo_benchmark.npu_us_min = d.npu_us;
      yolo_benchmark.count = d.count;
    }
    yolo_benchmark.status = err;
    printf("benchmark: %lu runs, total min %lu us avg %lu us, NPU min %lu us avg %lu us, %lu cats\n",
           static_cast<unsigned long>(yolo_benchmark.runs), static_cast<unsigned long>(yolo_benchmark.total_us_min),
           static_cast<unsigned long>(yolo_benchmark.total_us_sum / (yolo_benchmark.runs ? yolo_benchmark.runs : 1)),
           static_cast<unsigned long>(yolo_benchmark.npu_us_min),
           static_cast<unsigned long>(yolo_benchmark.npu_us_sum / (yolo_benchmark.runs ? yolo_benchmark.runs : 1)),
           static_cast<unsigned long>(yolo_benchmark.count));
    for (;;) osDelay(1000);
  }
#endif

  int32_t status = detector_init();
  if (status != 0) {
    yolo_status.detector_status = status;
    printf("detector: method load failed (err=%ld)\n", static_cast<long>(status));
    return 1;
  }

#ifdef APP_HAS_CAMERA
  const int32_t camera_status = camera_init();
#if CAMERA_PLANAR_RGB
  const char* camera_kind = "OV5675 through the ISP, 416x416 RGB";
#else
  const char* camera_kind = "640x480 RGB565 streaming";
#endif
  printf("camera: %s (%ld)\n", camera_status == 0 ? camera_kind : "not available", static_cast<long>(camera_status));
#else
  const int32_t camera_status = -1;
#endif
  yolo_status.camera_status = camera_status;
  const bool camera_on = camera_status == 0;
#if defined(APP_HAS_CAMERA) && CAMERA_PLANAR_RGB
  if (camera_on) {
    g_slot_event = osEventFlagsNew(nullptr);
    static const osThreadAttr_t attr = {.name = "camera", .stack_mem = g_camera_stack, .stack_size = sizeof(g_camera_stack),
                                        .priority = osPriorityAboveNormal};
    osThreadNew(camera_thread, nullptr, &attr);
  }
#endif

#ifdef APP_HAS_SDS
  rec_play_init(kInputBytes);
#endif

#ifdef APP_HAS_DISPLAY
  for (uint8_t* fb : g_framebuffer) {
    memset(fb, 0, kFrameBytes);
    SCB_CleanDCache_by_Addr(fb, static_cast<int32_t>(kFrameBytes));
  }
  bool display_on = display_init() == 0 && display_start(g_framebuffer[1]) == 0;
  if (!display_on) printf("display: not available\n");
  int back = 0;
  if (display_on) {
    g_display_go = osSemaphoreNew(1, 0, nullptr);
    g_display_idle = osSemaphoreNew(1, 1, nullptr);
    // Below the vision thread: when the NPU is done, the vision thread goes on at once.
    static const osThreadAttr_t attr = {.name = "display", .stack_mem = g_display_stack,
                                        .stack_size = sizeof(g_display_stack), .priority = osPriorityBelowNormal};
    osThreadNew(display_thread, nullptr, &attr);
  }
#endif

  if (!camera_on) memcpy(g_input, test_image, kInputBytes);

  uint32_t fps_frames = 0, fps_start = osKernelGetTickCount();
  float fps = 0.0f;
  detections_t det{};
  for (uint32_t frame = 0;; ++frame) {
    const uint32_t t_frame = cycles();
    int mode = 0;
#ifdef APP_HAS_SDS
    mode = rec_play_poll();
#endif
    uint32_t timeslot = osKernelGetTickCount();

    // 1. The input.
    Source source = camera_on ? kCamera : kTestImage;
    const void* camera = nullptr;
    const uint8_t* input = g_input;
    uint32_t t0 = cycles();
#ifdef APP_HAS_SDS
    if (mode == REC_PLAY_PLAYBACK) {
      if (rec_play_read_input(g_input, kInputBytes, &timeslot) != 1) continue;  // ended: the streams close at the next poll
      source = kPlayback;
    }
#endif
#ifdef APP_HAS_CAMERA
    if (source == kCamera) {
#if CAMERA_PLANAR_RGB
      input = camera_input(200);  // the camera thread converted it already
      if (input == nullptr) {
        printf("camera: no frame for 200 ms (%lu frames, %lu errors)\n", static_cast<unsigned long>(camera_frame_count()),
               static_cast<unsigned long>(camera_error_count()));
        continue;
      }
#else
      camera = camera_frame(200);
      if (camera == nullptr) {
        printf("camera: no frame for 200 ms (%lu frames, %lu errors)\n", static_cast<unsigned long>(camera_frame_count()),
               static_cast<unsigned long>(camera_error_count()));
        continue;
      }
#endif
#if !CAMERA_PLANAR_RGB
      image_camera_to_input(static_cast<const uint16_t*>(camera), CAMERA_WIDTH, CAMERA_HEIGHT, g_input, kSize);
#endif
    }
#endif
    const uint32_t t_input = cycles() - t0;

    // 2. Recording: the input.
    uint32_t t_sds = 0;
#ifdef APP_HAS_SDS
    if (mode == REC_PLAY_RECORD) {
      t0 = cycles();
      rec_play_write_input(input, kInputBytes, timeslot);
      t_sds += cycles() - t0;
    }
#endif

    // 3. The detector.
    t0 = cycles();
    status = detector_run(input, &det);
    const uint32_t t_detect = cycles() - t0;
    det.frame = frame;
    if (status != 0) {
      yolo_status.detector_status = status;
      printf("detector: run failed (err=%ld)\n", static_cast<long>(status));
      osDelay(1000);
      continue;
    }

    // 4. The result, while recording or playing back.
#ifdef APP_HAS_SDS
    if (mode != REC_PLAY_IDLE) {
      t0 = cycles();
      rec_play_write_output(&det, sizeof(det), timeslot);
      t_sds += cycles() - t0;
    }
#endif

    // 5. The panel: the picture here, the rest in the display thread while
    //    the NPU works on the next frame.
    t0 = cycles();
#ifdef APP_HAS_DISPLAY
    if (display_on) {
      osSemaphoreAcquire(g_display_idle, osWaitForever);  // the back buffer is off the panel
      uint8_t* fb = g_framebuffer[back];
#ifdef APP_HAS_CAMERA
#if !CAMERA_PLANAR_RGB
      if (camera != nullptr)
        image_camera_to_view(static_cast<const uint16_t*>(camera), CAMERA_WIDTH, CAMERA_HEIGHT, fb);
      else
#endif
#endif
        image_input_to_view(input, kSize, fb);
      g_job.fb = fb;
      g_job.det = det;
      g_job.has_scores = detector_last_scores(&g_job.scores) == 0 && g_job.scores.anchors <= kAnchors;
      if (g_job.has_scores) {
        memcpy(g_job_scores, g_job.scores.score, static_cast<size_t>(g_job.scores.anchors));
        g_job.scores.score = g_job_scores;
      }
      g_job.view = camera != nullptr ? IMAGE_VIEW : kSize;
      g_job.source = source;
      g_job.mode = mode;
      g_job.fps = fps;
      osSemaphoreRelease(g_display_go);
      back ^= 1;
    }
#endif
    const uint32_t t_picture = cycles() - t0;

    // The status for the debugger, the console every 100 frames.
    ++fps_frames;
    const uint32_t now = osKernelGetTickCount();
    if (now - fps_start >= 1000U) {
      fps = fps_frames * 1000.0f / static_cast<float>(now - fps_start);
      fps_frames = 0;
      fps_start = now;
    }
    yolo_status.frames = frame + 1;
    yolo_status.source = source;
    yolo_status.mode = mode;
#ifdef APP_HAS_CAMERA
    yolo_status.camera_frames = camera_frame_count();
    yolo_status.camera_errors = camera_error_count();
#if CAMERA_PLANAR_RGB
    image_planar_gains(const_cast<float*>(yolo_status.wb_gain));
    yolo_status.camera_video = camera_video_mode();
    yolo_status.convert_us = us(g_convert_cycles);
#endif
#endif
    yolo_status.detector_status = 0;
    yolo_status.frame_us = us(cycles() - t_frame);
    yolo_status.input_us = us(t_input);
    yolo_status.detect_us = us(t_detect);
    yolo_status.npu_us = det.npu_us;
    yolo_status.picture_us = us(t_picture);
    yolo_status.sds_us = us(t_sds);
    yolo_status.fps = fps;
    memcpy(const_cast<detections_t*>(&yolo_status.result), &det, sizeof(det));
    for (int y = 0; y < 52; ++y)
      for (int x = 0; x < 52; ++x) memcpy(&yolo_thumbnail[(y * 52 + x) * 3], &input[((y * 8) * kSize + x * 8) * 3], 3);
#if defined(APP_HAS_CAMERA) && CAMERA_PLANAR_RGB
    if (source == kCamera) camera_input_done();
#endif
    if (frame % 100 == 0) {
      printf("frame %lu: %lu cat%s, %.1f fps; frame %lu us = input %lu + detect %lu (NPU %lu) + picture %lu + SDS %lu; display thread %lu\n",
             static_cast<unsigned long>(frame), static_cast<unsigned long>(det.count), det.count == 1 ? "" : "s", static_cast<double>(fps),
             static_cast<unsigned long>(yolo_status.frame_us), static_cast<unsigned long>(yolo_status.input_us),
             static_cast<unsigned long>(yolo_status.detect_us), static_cast<unsigned long>(det.npu_us),
             static_cast<unsigned long>(yolo_status.picture_us), static_cast<unsigned long>(yolo_status.sds_us),
             static_cast<unsigned long>(yolo_status.display_us));
    }
  }
}
