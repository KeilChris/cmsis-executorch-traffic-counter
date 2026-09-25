/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Ethos-U85 side of the Quake port: the ExecuTorch methods of
 * model/quake.py (qpresent, qshade). For now this measures them on the board
 * with synthetic planes, one method at a time out of the same pools; the
 * present thread grows out of it.
 *
 * Quake's headers are not included here (see port.h).
 */

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <arm_mve.h>

#include "RTE_Components.h"
#include CMSIS_device_header

#include <executorch/extension/data_loader/buffer_data_loader.h>
#include <executorch/runtime/core/evalue.h>
#include <executorch/runtime/core/exec_aten/exec_aten.h>
#include <executorch/runtime/core/memory_allocator.h>
#include <executorch/runtime/platform/runtime.h>

#include "cmsis_os2.h"

#include "arm_embedded_module.hpp"
#include "model_io.h"
#include "model_pte.h"
#include "port.h"
#include "port_npu.h"
#include "port_video.h"

using arm::embedded::EmbeddedModule;
using executorch::aten::DimOrderType;
using executorch::aten::ScalarType;
using executorch::aten::SizesType;
using executorch::aten::Tensor;
using executorch::aten::TensorImpl;
using executorch::extension::BufferDataLoader;
using executorch::runtime::EValue;
using executorch::runtime::MemoryAllocator;

uint32_t npu_handoff_cycles, npu_execute_cycles, npu_busy_cycles, npu_copy_cycles;

namespace {

// Method pool: the planned buffers of one method (the 1.15 MB frame) and the
// runtime's structures. Temp pool: Vela's scratch, 2625 KiB for every Quake graph.
constexpr size_t kMethodPoolSize = 0x130000;
#ifndef QUAKE_TEMP_POOL_SIZE
#ifdef MODEL_QFETCH_METHOD
#define QUAKE_TEMP_POOL_SIZE 0x2F0000  // qfetch: 3000 KiB of scratch (the surface cache and the offsets are in it)
#else
#define QUAKE_TEMP_POOL_SIZE 0x2C0000  // Ethos-U85 graphs: 2625 KiB of scratch (the board layer of the U55 target says 0x170000)
#endif
#endif
constexpr size_t kTempPoolSize = QUAKE_TEMP_POOL_SIZE;

alignas(16) uint8_t g_method_pool[kMethodPoolSize] __attribute__((section(".bss.ai_pool")));
alignas(16) uint8_t g_temp_pool[kTempPoolSize] __attribute__((section(".bss.ai_pool")));
#ifdef PORT_ZERO_COPY
// Zero-copy present: the panel shows the frame where the NPU wrote it, in the
// scratch. Two present modules with pools of their own take turns, so the
// panel never scans out the scratch the NPU is writing: double buffering
// without frame buffers and without the 1.15 MB output copy per frame.
alignas(16) uint8_t g_method_pool2[kMethodPoolSize] __attribute__((section(".bss.ai_pool")));
alignas(16) uint8_t g_temp_pool2[kTempPoolSize] __attribute__((section(".bss.ai_pool")));
#endif

constexpr int32_t kAlbedoShape[] = MODEL_QSHADE_INPUT0_SHAPE;  // {1, 3, H, W}
constexpr int32_t kLightShape[] = MODEL_QSHADE_INPUT1_SHAPE;   // {1, 1, H, W}
constexpr int32_t kBlendShape[] = MODEL_QSHADE_INPUT2_SHAPE;   // {1, 3, 1, 1}

// One staging buffer for all present paths: the albedo planes and the light
// plane of qpresent / qshade, or (the same 384 kB) the texel offsets of qfetch.
alignas(32) int8_t g_staging[MODEL_QSHADE_INPUT0_NUMEL + MODEL_QSHADE_INPUT1_NUMEL] __attribute__((section(".bss.ai_pool")));
constexpr int8_t* g_albedo = g_staging;
constexpr int8_t* g_light = g_staging + MODEL_QSHADE_INPUT0_NUMEL;
int8_t g_blend_k[MODEL_QSHADE_INPUT2_NUMEL];
int8_t g_blend_c[MODEL_QSHADE_INPUT3_NUMEL];

uint32_t g_io_copy_cycles;
uint8_t* g_frame_target;  // where the frame goes instead of the output tensor
#ifdef PORT_ZERO_COPY
bool g_zero_copy;                   // a present job: leave the frame in the scratch
const uint8_t* g_frame_in_scratch;  // where the last job left it
int8_t* g_input_in_scratch;         // where the backend wants the input planes
#endif
uint32_t g_npu_begin, g_npu_cycles;

inline uint32_t cycles() { return DWT->CYCCNT; }
inline unsigned us(uint32_t c) { return static_cast<unsigned>(static_cast<uint64_t>(c) * 1000000U / SystemCoreClock); }

struct TensorBox {  // tensor wrapping without the tensor extension, as in src/app_main.cpp
  std::array<SizesType, 4> sizes;
  std::array<DimOrderType, 4> dim_order;
  std::unique_ptr<TensorImpl> impl;

  TensorBox(ScalarType type, const int32_t* shape, int ndim, void* data) {
    for (int i = 0; i < ndim; ++i) {
      sizes[i] = shape[i];
      dim_order[i] = static_cast<DimOrderType>(i);
    }
    impl = std::make_unique<TensorImpl>(type, ndim, sizes.data(), data, dim_order.data());
  }
  EValue evalue() { return EValue(Tensor(impl.get())); }
};

// One method, a fresh module over the shared pools: load, one warm-up, `runs` timed executes.
int measure(const char* method, std::vector<EValue> inputs, int runs) {
  EmbeddedModule module(model_pte, model_pte_size, std::make_unique<BufferDataLoader>(model_pte, model_pte_size),
                        std::make_unique<MemoryAllocator>(kMethodPoolSize, g_method_pool),
                        std::make_unique<MemoryAllocator>(kTempPoolSize, g_temp_pool));
  uint32_t total = 0, best = UINT32_MAX, copy = 0, npu = 0;
  int8_t first = 0, last = 0;
  for (int run = -1; run < runs; ++run) {
    g_io_copy_cycles = 0;
    g_npu_cycles = 0;
    uint32_t t0 = cycles();
    auto out = module.execute(method, inputs);
    uint32_t t = cycles() - t0;
    if (!out.ok() || out->empty()) {
      printf("[npu] %s failed (err=%u)\n", method, static_cast<unsigned>(out.error()));
      return -1;
    }
    if (run < 0) continue;  // warm-up: method load
    total += t;
    copy += g_io_copy_cycles;
    npu += g_npu_cycles;
    if (t < best) best = t;
    const auto& frame = (*out)[0].toTensor();
    first = frame.const_data_ptr<int8_t>()[0];
    last = frame.const_data_ptr<int8_t>()[frame.numel() - 1];
  }
  printf("[npu] %-13s execute avg %u us, min %u us; IO copies %u us; NPU busy %u us; frame[0] %d, frame[last] %d\n", method,
         us(total / runs), us(best), us(copy / runs), us(npu / runs), first, last);
  return 0;
}

}  // namespace

// The Ethos-U backend's IO copy hook (strong override of the weak default),
// timed. The frame, the output of the present methods, goes straight into the
// display's back buffer, int8 -> uint8 (q + 128 is q ^ 0x80) on the way; the
// method's own output tensor stays untouched.
extern "C" void arm_ethos_io_memcpy(void* dst, const void* src, size_t size) {
  uint32_t t0 = cycles();
#ifdef PORT_ZERO_COPY
  if (size == MODEL_QPRESENT_OUTPUT0_NUMEL && g_zero_copy) {
    g_frame_in_scratch = static_cast<const uint8_t*>(src);  // the frame stays where it is: the panel shows it there
    return;
  }
  if (g_zero_copy && size == MODEL_QPRESENT_INPUT0_NUMEL) {
    if (dst == src) return;  // the hand-off pass wrote the planes where the NPU reads them
    g_input_in_scratch = static_cast<int8_t*>(dst);
  }
#endif
  if (size == MODEL_QPRESENT_OUTPUT0_NUMEL && g_frame_target != nullptr) {
    const uint32_t* s = static_cast<const uint32_t*>(src);  // the scratch is 16-byte aligned, the frame 32
    uint32_t* d = reinterpret_cast<uint32_t*>(g_frame_target);
    for (size_t i = 0; i < size / 4; ++i) d[i] = s[i] ^ 0x80808080U;
  } else if (size >= 4096U) {
    // The input planes (288 kB per frame): 16 bytes per Helium load / store pair,
    // tail by predicate. The C library's memcpy here is a generic Armv6 routine.
    const uint8_t* s = static_cast<const uint8_t*>(src);
    uint8_t* d = static_cast<uint8_t*>(dst);
    for (int32_t left = static_cast<int32_t>(size); left > 0; left -= 16, s += 16, d += 16) {
      mve_pred16_t pred = vctp8q(static_cast<uint32_t>(left));
      vstrbq_p_u8(d, vldrbq_z_u8(s, pred), pred);
    }
  } else {
    memcpy(dst, src, size);
  }
  g_io_copy_cycles += cycles() - t0;
  npu_copy_cycles += cycles() - t0;
}

// The driver's hooks around the NPU job: command stream start to interrupt.
namespace {
osSemaphoreId_t g_staging_free;  // the input planes may be written: the NPU job has its copy
bool g_pipelined_job;
}  // namespace

extern "C" void ethosu_inference_begin(struct ethosu_driver* drv, void* user_arg) {
  (void)drv;
  (void)user_arg;
  g_npu_begin = cycles();
  // The backend has copied the inputs into the scratch: the renderer may fill the planes again.
  if (g_pipelined_job) osSemaphoreRelease(g_staging_free);
}
extern "C" void ethosu_inference_end(struct ethosu_driver* drv, void* user_arg) {
  (void)drv;
  (void)user_arg;
  g_npu_cycles += cycles() - g_npu_begin;
  npu_busy_cycles += cycles() - g_npu_begin;
}

namespace {
void runtime_once() {
  static bool done;
  if (!done) {
    executorch::runtime::runtime_init();
    done = true;
  }
}
}  // namespace

extern "C" int port_npu_measure(void) {
  runtime_once();
  printf("[npu] program %lu bytes; method pool %u B, temp pool %u B\n", static_cast<unsigned long>(model_pte_size),
         static_cast<unsigned>(kMethodPoolSize), static_cast<unsigned>(kTempPoolSize));

  // A gradient in the albedo, light from black to full overbright, no blend.
  for (size_t i = 0; i < MODEL_QSHADE_INPUT0_NUMEL; ++i) g_albedo[i] = static_cast<int8_t>((i * 7U) & 0xFFU);
  for (size_t i = 0; i < MODEL_QSHADE_INPUT1_NUMEL; ++i) g_light[i] = static_cast<int8_t>(i & 0xFFU);
  memset(g_blend_k, MODEL_QSHADE_INPUT2_QMAX, sizeof(g_blend_k));  // k = 1
  memset(g_blend_c, MODEL_QSHADE_INPUT3_QMIN, sizeof(g_blend_c));  // c = 0

  TensorBox albedo(ScalarType::Char, kAlbedoShape, 4, g_albedo);
  TensorBox light(ScalarType::Char, kLightShape, 4, g_light);
  TensorBox blend_k(ScalarType::Char, kBlendShape, 4, g_blend_k);
  TensorBox blend_c(ScalarType::Char, kBlendShape, 4, g_blend_c);

  constexpr int kRuns = 20;
  if (measure(MODEL_QPRESENT_METHOD, {albedo.evalue()}, kRuns) != 0) return -1;
  if (measure(MODEL_QSHADE_METHOD, {albedo.evalue(), light.evalue(), blend_k.evalue(), blend_c.evalue()}, kRuns) != 0) return -1;
#ifdef MODEL_QPRESENT_NHWC_METHOD
  {
    // The Ethos-U55 variants on rotated, interleaved planes: the same buffers, another shape.
    constexpr int32_t kFrameShape[] = MODEL_QSHADE_NHWC_INPUT0_SHAPE, kLightPlaneShape[] = MODEL_QSHADE_NHWC_INPUT1_SHAPE;
    TensorBox frame(ScalarType::Char, kFrameShape, 4, g_albedo), plane(ScalarType::Char, kLightPlaneShape, 4, g_light);
    if (measure(MODEL_QPRESENT_NHWC_METHOD, {frame.evalue()}, kRuns) != 0) return -1;
    if (measure(MODEL_QSHADE_NHWC_METHOD, {frame.evalue(), plane.evalue(), blend_k.evalue(), blend_c.evalue()}, kRuns) != 0) return -1;
  }
#endif
#ifdef MODEL_QPRESENT4_METHOD
  {
    constexpr int32_t kQuarterShape[] = MODEL_QPRESENT4_INPUT0_SHAPE;
    TensorBox quarter(ScalarType::Char, kQuarterShape, 4, g_albedo);
    if (measure(MODEL_QPRESENT4_METHOD, {quarter.evalue()}, kRuns) != 0) return -1;
  }
#endif
#ifdef MODEL_QVERT_METHOD
  {
    // The alias vertex stage on made-up vertices: the transform keeps z in [0.25, 1].
    constexpr int32_t kVertsShape[] = MODEL_QVERT_INPUT0_SHAPE, kXformShape[] = MODEL_QVERT_INPUT1_SHAPE;
    constexpr int32_t kNdotlShape[] = MODEL_QVERT_INPUT2_SHAPE, kVertLightShape[] = MODEL_QVERT_INPUT3_SHAPE;
    static int16_t verts[MODEL_QVERT_INPUT0_NUMEL], ndotl[MODEL_QVERT_INPUT2_NUMEL];
    static int16_t xform[MODEL_QVERT_INPUT1_NUMEL], vlight[MODEL_QVERT_INPUT3_NUMEL] = {1024, 2048};
    for (size_t i = 0; i < MODEL_QVERT_INPUT0_NUMEL; ++i) verts[i] = (i % 4U) == 3U ? 4096 : static_cast<int16_t>((i * 37U) & 0xFFFU);
    for (size_t i = 0; i < MODEL_QVERT_INPUT2_NUMEL; ++i) ndotl[i] = static_cast<int16_t>(((i * 53U) & 0x1FFFU) - 4096);
    xform[0] = 400, xform[4] = 400, xform[8] = 600, xform[11] = 1200;  // x, y, z scales; z offset 0.29
    TensorBox v(ScalarType::Short, kVertsShape, 3, verts), x(ScalarType::Short, kXformShape, 3, xform);
    TensorBox n(ScalarType::Short, kNdotlShape, 3, ndotl), l(ScalarType::Short, kVertLightShape, 3, vlight);
    if (measure(MODEL_QVERT_METHOD, {v.evalue(), x.evalue(), n.evalue(), l.evalue()}, kRuns) != 0) return -1;
  }
#endif
  return 0;
}

// ---------------------------------------------------------------------------
// The NPU present path: the hand-off pass on the CPU (rotate to the portrait
// panel, resolve the palette, planar int8), qpresent on the NPU (2x bilinear
// upscale, interleave), the output copy into the display's back buffer.
// ---------------------------------------------------------------------------
extern "C" volatile bool ethosu_dcache_keep;  // board/DevKit-E8/ethosu_cb_dcache.c

namespace {
EmbeddedModule* g_present;
#ifdef PORT_ZERO_COPY
EmbeddedModule* g_presents[2];  // one per panel buffer
TensorBox* g_present_ins[2];    // their input planes, in their scratch
#endif
int8_t* g_handoff_target = g_staging;  // where the hand-off pass writes
TensorBox* g_present_in;
int8_t g_lut[3][256];
// The Ethos-U55 layer has a present graph for rotated, interleaved planes
// (qpresent_nhwc: no transposes left in the graph). Measured on the M55-HE:
// NPU busy 20.7 -> 9.6 ms, but the rotating hand-off pass costs the CPU 5.4 ms
// instead of 1.9 ms, and the CPU is the bottleneck there: 19.8 -> 18.6 fps.
// So it is an option, not the default.
#if QUAKE_PANEL_SCALE == 4  // a 200 x 120 view, scaled 4x by the NPU
#define PRESENT_METHOD MODEL_QPRESENT4_METHOD
constexpr int32_t kPresentShape[] = MODEL_QPRESENT4_INPUT0_SHAPE;  // {1, 3, H, W}
#elif defined(PORT_PRESENT_NHWC) && defined(MODEL_QPRESENT_NHWC_METHOD) && PORT_PANEL_TOP_LEFT
#define PRESENT_METHOD MODEL_QPRESENT_NHWC_METHOD
#define PRESENT_NHWC 1
constexpr int32_t kPresentShape[] = MODEL_QPRESENT_NHWC_INPUT0_SHAPE;  // {1, W, H, 3}: the portrait panel
#else
#define PRESENT_METHOD MODEL_QPRESENT_METHOD
constexpr int32_t kPresentShape[] = MODEL_QPRESENT_INPUT0_SHAPE;  // {1, 3, H, W}
#endif
}  // namespace


extern "C" int port_npu_present_init(void) {
  runtime_once();
  g_present = new EmbeddedModule(model_pte, model_pte_size, std::make_unique<BufferDataLoader>(model_pte, model_pte_size),
                                 std::make_unique<MemoryAllocator>(kMethodPoolSize, g_method_pool),
                                 std::make_unique<MemoryAllocator>(kTempPoolSize, g_temp_pool));
  g_present_in = new TensorBox(ScalarType::Char, kPresentShape, 4, g_albedo);
  if (g_present->load_method(PRESENT_METHOD) != executorch::runtime::Error::Ok) return -1;
#ifdef PORT_ZERO_COPY
  g_presents[0] = g_present;
  g_presents[1] = new EmbeddedModule(model_pte, model_pte_size, std::make_unique<BufferDataLoader>(model_pte, model_pte_size),
                                     std::make_unique<MemoryAllocator>(kMethodPoolSize, g_method_pool2),
                                     std::make_unique<MemoryAllocator>(kTempPoolSize, g_temp_pool2));
  if (g_presents[1]->load_method(PRESENT_METHOD) != executorch::runtime::Error::Ok) return -1;
  // One job each tells where Vela put the output in the scratch: the panel's two buffers.
  uint8_t* buffers[2];
  memset(g_albedo, 0, MODEL_QSHADE_INPUT0_NUMEL);
  for (int i = 0; i < 2; ++i) {
    g_zero_copy = true;
    auto out = g_presents[i]->execute(PRESENT_METHOD, {g_present_in->evalue()});
    g_zero_copy = false;
    if (!out.ok() || g_frame_in_scratch == nullptr || g_input_in_scratch == nullptr) return -1;
    buffers[i] = const_cast<uint8_t*>(g_frame_in_scratch);
    // The input planes stay in the staging buffer and are copied in. Tried: the hand-off
    // pass writing them where the NPU reads them (the address the hook sees). Vela places
    // the output over the input's area (their lifetimes do not overlap), and that output
    // is what the panel is showing: the next frame's planes distorted the picture.
    g_present_ins[i] = new TensorBox(ScalarType::Char, kPresentShape, 4, g_staging);
    printf("[npu] module %d: input planes at %p, output frame at %p\n", i, static_cast<void*>(g_input_in_scratch),
           static_cast<void*>(buffers[i]));
    g_frame_in_scratch = nullptr;
    g_input_in_scratch = nullptr;
  }
  printf("[npu] zero-copy present: the panel's buffers are the NPU outputs at %p and %p\n", static_cast<void*>(buffers[0]),
         static_cast<void*>(buffers[1]));
  video_set_buffers(buffers[0], buffers[1]);
#ifdef PORT_DCACHE_KEEP
  // The CPU reads nothing the NPU writes: the invalidate after each job can go
  // (ethosu_cb_dcache.c). Measured on the M55-HE: no difference (33.57 against
  // 33.46 ms of CPU per frame), the renderer's working set does not fit the
  // cache anyway. Off by default.
  ethosu_dcache_keep = true;
#endif
#endif
  return 0;
}

namespace {
// The hand-off pass: Quake's palette indices to three int8 planes (zero point
// -128), row by row, 16 pixels per Helium gather. The planes keep Quake's
// landscape layout; the NPU transposes them onto the portrait panel, and the
// row order here (bottom row first) makes that transpose the rotation that
// puts the picture upright with the panel's top edge on the left.
void handoff(const uint8_t* frame, const uint8_t* palette_rgb) {
  constexpr int kWidth = QUAKE_VID_WIDTH, kHeight = QUAKE_VID_HEIGHT;
  static_assert(kWidth >= 16, "the row loop works on 16 pixels");
  for (int i = 0; i < 256; ++i) {
#ifdef PORT_ZERO_COPY
    // The graph only moves bytes (transpose, nearest-neighbour resize): what goes in as
    // a colour byte comes out as that byte, ready for the panel. No int8 offset either way.
    for (int c = 0; c < 3; ++c) g_lut[c][i] = static_cast<int8_t>(palette_rgb[3 * i + c]);
#else
    for (int c = 0; c < 3; ++c) g_lut[c][i] = static_cast<int8_t>(palette_rgb[3 * i + c] ^ 0x80);
#endif
  }
#ifdef PRESENT_NHWC
  // Rotated and interleaved for qpresent_nhwc: panel[x][H - 1 - y] = palette[frame[y][x]].
  // The frame is read row by row; each source row lands in one column of the
  // panel, 720 bytes apart. Those 400 cache lines stay in the data cache from
  // one source row to the next (three more bytes each), so the scatter is cheap.
  {
    constexpr uint32_t kPitch = kHeight * 3;  // one panel row
#if PORT_PANEL_BGR
    const int8_t *first = g_lut[2], *third = g_lut[0];
#else
    const int8_t *first = g_lut[0], *third = g_lut[2];
#endif
    static_assert(kWidth % 4 == 0, "the row loop has no tail");
    const uint32x4_t step = vdupq_n_u32(4U * kPitch);
    for (int y = 0; y < kHeight; ++y) {
      const uint8_t* source = frame + y * kWidth;
      uint32x4_t offset = vaddq_n_u32(vmulq_n_u32(vidupq_n_u32(0U, 1), kPitch), static_cast<uint32_t>(kHeight - 1 - y) * 3U);
      for (int x = 0; x < kWidth; x += 4) {
        uint32x4_t index = vldrbq_u32(source + x);
        vstrbq_scatter_offset_s32(g_handoff_target, offset, vldrbq_gather_offset_s32(first, index));
        vstrbq_scatter_offset_s32(g_handoff_target + 1, offset, vldrbq_gather_offset_s32(g_lut[1], index));
        vstrbq_scatter_offset_s32(g_handoff_target + 2, offset, vldrbq_gather_offset_s32(third, index));
        offset = vaddq_u32(offset, step);
      }
    }
    return;
  }
#endif
  // The graph interleaves its three planes in order; the panel wants blue first (PORT_PANEL_BGR).
#if PORT_PANEL_BGR
  int8_t* blue = g_handoff_target;
  int8_t* green = g_handoff_target + kWidth * kHeight;
  int8_t* red = g_handoff_target + 2 * kWidth * kHeight;
#else
  int8_t* red = g_handoff_target;
  int8_t* green = g_handoff_target + kWidth * kHeight;
  int8_t* blue = g_handoff_target + 2 * kWidth * kHeight;
#endif
  for (int y = 0; y < kHeight; ++y) {
#if PORT_PANEL_TOP_LEFT
    const uint8_t* source = frame + (kHeight - 1 - y) * kWidth;
    for (int i = 0; i < kWidth; i += 16) {
      const int x = i + 16 <= kWidth ? i : kWidth - 16;  // the last group overlaps the one before where the width is no multiple of 16
      uint8x16_t index = vldrbq_u8(source + x);
      vstrbq_s8(red + x, vldrbq_gather_offset_s8(g_lut[0], index));
      vstrbq_s8(green + x, vldrbq_gather_offset_s8(g_lut[1], index));
      vstrbq_s8(blue + x, vldrbq_gather_offset_s8(g_lut[2], index));
    }
#else
    const uint8_t* source = frame + y * kWidth;  // top row first, every row mirrored
    for (int x = 0; x < kWidth; ++x) {
      const uint8_t index = source[kWidth - 1 - x];
      red[x] = g_lut[0][index];
      green[x] = g_lut[1][index];
      blue[x] = g_lut[2][index];
    }
#endif
    red += kWidth;
    green += kWidth;
    blue += kWidth;
  }
}
}  // namespace

extern "C" int port_npu_present(const uint8_t* frame, const uint8_t* palette_rgb) {
  uint32_t t0 = cycles();
  handoff(frame, palette_rgb);
  uint32_t t1 = cycles();
  npu_handoff_cycles += t1 - t0;

  g_frame_target = video_begin();
  uint32_t t2 = cycles();
#ifdef PORT_ZERO_COPY
  const int module = video_back();  // after video_begin(): that buffer is off the screen
  g_zero_copy = true;
  auto out = g_presents[module]->execute(PRESENT_METHOD, {g_present_ins[module]->evalue()});
  g_zero_copy = false;
#else
  auto out = g_present->execute(PRESENT_METHOD, {g_present_in->evalue()});
#endif
  g_frame_target = nullptr;
  npu_execute_cycles += cycles() - t2;
  if (!out.ok()) return -1;
#ifdef PORT_ZERO_COPY
  video_show(module);  // the NPU wrote the frame: nothing of it is in the data cache
#else
  video_end();
#endif
  return 0;
}

// ---------------------------------------------------------------------------
// The driver's semaphores on RTX. The driver's default spins on __WFE while
// the NPU runs; with these the waiting thread sleeps and the renderer gets the
// core. ethosu_init() runs before the kernel does, so the RTX objects are
// created later, by port_npu_rtos_init(); until then the default's counter is
// all there is. One thread uses the NPU, so the mutexes stay the no-ops they are.
// ---------------------------------------------------------------------------
namespace {
struct DriverSemaphore {
  volatile uint32_t count;
  osSemaphoreId_t id;
};
DriverSemaphore g_driver_semaphores[4];
unsigned g_driver_semaphore_count;
}  // namespace

extern "C" void* ethosu_semaphore_create(void) {
  return g_driver_semaphore_count < 4 ? &g_driver_semaphores[g_driver_semaphore_count++] : nullptr;
}
extern "C" void ethosu_semaphore_destroy(void* sem) { (void)sem; }

extern "C" int ethosu_semaphore_take(void* sem, uint64_t timeout) {
  (void)timeout;
  auto* s = static_cast<DriverSemaphore*>(sem);
  while (s->count == 0) {
    if (s->id != nullptr) {
      osSemaphoreAcquire(s->id, osWaitForever);
    } else {
      __WFE();
    }
  }
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  s->count--;
  __set_PRIMASK(primask);
  return 0;
}

extern "C" int ethosu_semaphore_give(void* sem) {  // from the NPU's interrupt, or a thread
  auto* s = static_cast<DriverSemaphore*>(sem);
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  s->count++;
  __set_PRIMASK(primask);
  if (s->id != nullptr) osSemaphoreRelease(s->id);
  __SEV();
  return 0;
}

// ---------------------------------------------------------------------------
// The present thread: the NPU works on frame N while the renderer's thread
// draws frame N + 1. The renderer fills the planes (hand-off pass) and hands
// over; it gets the planes back when the NPU job has started.
// ---------------------------------------------------------------------------
namespace {
osSemaphoreId_t g_frame_ready;
volatile bool g_present_busy;
uint64_t g_present_stack[0x8000 / 8] __attribute__((section(".bss.ai_pool")));

#ifdef MODEL_QFETCH_METHOD
// Deferred texturing: what the span drawer writes instead of texels, and the
// inputs of qfetch. The cache and the frame are Quake's own buffers.
constexpr int32_t kFetchCacheShape[] = MODEL_QFETCH_INPUT0_SHAPE;    // {bytes of the surface cache, 1}
constexpr int32_t kFetchOffsetShape[] = MODEL_QFETCH_INPUT1_SHAPE;   // {pixels}
constexpr int32_t kFetchFrameShape[] = MODEL_QFETCH_INPUT2_SHAPE;    // {pixels}
constexpr int32_t kFetchPaletteShape[] = MODEL_QFETCH_INPUT3_SHAPE;  // {256, 3}
static_assert(sizeof(g_staging) == MODEL_QFETCH_INPUT1_NUMEL * sizeof(uint32_t), "the offsets share the staging buffer");
uint32_t* const g_offsets = reinterpret_cast<uint32_t*>(g_staging);
int8_t g_fetch_palette[MODEL_QFETCH_INPUT3_NUMEL];
TensorBox *g_fetch_cache, *g_fetch_offsets, *g_fetch_frame, *g_fetch_pal;
const uint8_t *g_fetch_cache_data, *g_fetch_frame_data;
#endif
bool g_fetch;  // the present thread runs qfetch, not qpresent

void present_thread(void*) {
  for (;;) {
    osSemaphoreAcquire(g_frame_ready, osWaitForever);
    uint32_t t0 = cycles();
    g_frame_target = video_begin();
    g_pipelined_job = true;
#ifdef MODEL_QFETCH_METHOD
    auto out = g_fetch ? g_present->execute(MODEL_QFETCH_METHOD, {g_fetch_cache->evalue(), g_fetch_offsets->evalue(),
                                                                  g_fetch_frame->evalue(), g_fetch_pal->evalue()})
                       : g_present->execute(PRESENT_METHOD, {g_present_in->evalue()});
#elif defined(PORT_ZERO_COPY)
    // The module whose scratch is the display's back buffer, decided here, when
    // the job starts and video_begin() has seen the last flip take effect. (Decided
    // by the renderer at hand-off time it went wrong: a frame queued behind a
    // running job got the module that was about to go on screen.)
    const int module = video_back();
    g_zero_copy = true;
    auto out = g_presents[module]->execute(PRESENT_METHOD, {g_present_ins[module]->evalue()});
    g_zero_copy = false;
#else
    auto out = g_present->execute(PRESENT_METHOD, {g_present_in->evalue()});
#endif
    g_pipelined_job = false;
    g_frame_target = nullptr;
#ifdef PORT_ZERO_COPY
    if (out.ok()) video_show(module);  // the NPU wrote the frame: nothing of it is in the data cache
#else
    if (out.ok()) video_end();
#endif
    npu_execute_cycles += cycles() - t0;
    g_present_busy = false;
  }
}
}  // namespace

extern "C" int port_npu_rtos_init(void) {
  for (unsigned i = 0; i < g_driver_semaphore_count; ++i) {
    DriverSemaphore& s = g_driver_semaphores[i];
    s.id = osSemaphoreNew(0xFFFFU, s.count, nullptr);
    if (s.id == nullptr) return -1;
  }
  g_staging_free = osSemaphoreNew(1U, 1U, nullptr);
  g_frame_ready = osSemaphoreNew(1U, 0U, nullptr);
  static const osThreadAttr_t attr = {
      .name = "present",
      .stack_mem = g_present_stack,
      .stack_size = sizeof(g_present_stack),
      .priority = osPriorityAboveNormal,  // above the renderer: the NPU and the panel are never kept waiting
  };
  return (g_staging_free && g_frame_ready && osThreadNew(present_thread, nullptr, &attr)) ? 0 : -1;
}

// Pipelined present, from the renderer's thread: waits for the planes, fills them, hands over.
extern "C" void port_npu_present_pipelined(const uint8_t* frame, const uint8_t* palette_rgb) {
  osSemaphoreAcquire(g_staging_free, osWaitForever);
  uint32_t t0 = cycles();
  handoff(frame, palette_rgb);
  npu_handoff_cycles += cycles() - t0;
  g_present_busy = true;
  osSemaphoreRelease(g_frame_ready);
}

// Let the pipeline run dry before another path touches the display.
extern "C" void port_npu_present_drain(void) {
  while (g_present_busy) osDelay(1U);
}

// ---------------------------------------------------------------------------
// Deferred texturing: qfetch in place of qpresent. One method is loaded at a
// time (each plans the 1.15 MB frame in the method pool): a switch reloads.
// ---------------------------------------------------------------------------
#ifdef MODEL_QFETCH_METHOD
extern "C" uint32_t* port_npu_fetch_offsets(void) { return g_offsets; }

extern "C" int port_npu_fetch_select(int fetch) {
  if (g_fetch == (fetch != 0)) return 0;
  port_npu_present_drain();
  delete g_present;
  g_present = new EmbeddedModule(model_pte, model_pte_size, std::make_unique<BufferDataLoader>(model_pte, model_pte_size),
                                 std::make_unique<MemoryAllocator>(kMethodPoolSize, g_method_pool),
                                 std::make_unique<MemoryAllocator>(kTempPoolSize, g_temp_pool));
  if (g_present->load_method(fetch ? MODEL_QFETCH_METHOD : PRESENT_METHOD) != executorch::runtime::Error::Ok) return -1;
  g_fetch = (fetch != 0);
  return 0;
}

// From the renderer's thread. The inputs are Quake's live buffers: the
// renderer goes on when the NPU job has started, which is when the backend has
// copied them into its scratch.
extern "C" void port_npu_present_fetch(const uint8_t* frame, const uint8_t* palette_rgb, const uint8_t* surface_cache) {
  uint32_t t0 = cycles();
  osSemaphoreAcquire(g_staging_free, osWaitForever);
  if (frame != g_fetch_frame_data || surface_cache != g_fetch_cache_data) {
    delete g_fetch_cache, delete g_fetch_offsets, delete g_fetch_frame, delete g_fetch_pal;
    g_fetch_cache = new TensorBox(ScalarType::Char, kFetchCacheShape, 2, const_cast<uint8_t*>(surface_cache));
    g_fetch_offsets = new TensorBox(ScalarType::Int, kFetchOffsetShape, 1, g_offsets);
    g_fetch_frame = new TensorBox(ScalarType::Char, kFetchFrameShape, 1, const_cast<uint8_t*>(frame));
    g_fetch_pal = new TensorBox(ScalarType::Char, kFetchPaletteShape, 2, g_fetch_palette);
    g_fetch_frame_data = frame;
    g_fetch_cache_data = surface_cache;
  }
  // The graph looks the palette up with pixel + 128, the pixel being the byte
  // seen as int8: row j is colour j ^ 0x80. Blue first for the panel.
  for (int j = 0; j < 256; ++j) {
    const uint8_t* rgb = palette_rgb + 3 * (j ^ 0x80);
#if PORT_PANEL_BGR
    g_fetch_palette[3 * j + 0] = static_cast<int8_t>(rgb[2] ^ 0x80);
    g_fetch_palette[3 * j + 2] = static_cast<int8_t>(rgb[0] ^ 0x80);
#else
    g_fetch_palette[3 * j + 0] = static_cast<int8_t>(rgb[0] ^ 0x80);
    g_fetch_palette[3 * j + 2] = static_cast<int8_t>(rgb[2] ^ 0x80);
#endif
    g_fetch_palette[3 * j + 1] = static_cast<int8_t>(rgb[1] ^ 0x80);
  }
  g_present_busy = true;
  osSemaphoreRelease(g_frame_ready);
  osSemaphoreAcquire(g_staging_free, osWaitForever);  // the NPU job has its copies
  osSemaphoreRelease(g_staging_free);
  npu_handoff_cycles += cycles() - t0;
}
#else
extern "C" uint32_t* port_npu_fetch_offsets(void) { return nullptr; }
extern "C" int port_npu_fetch_select(int fetch) { return fetch ? -1 : 0; }
extern "C" void port_npu_present_fetch(const uint8_t*, const uint8_t*, const uint8_t*) {}
#endif
