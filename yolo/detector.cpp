/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The cat detector: the `detect` method of model/yolo.py, exported by
 * create_ai_layer.py into ai_layer_yolo/. The whole graph runs on the NPU and
 * returns, per anchor, the box distances and the cat score (int8). The CPU does
 * what is left of the YOLO26 head: threshold the score, turn the distances of
 * the survivors into boxes. The one-to-one head of YOLO26 is NMS-free; an IoU
 * check still drops the rare duplicate.
 *
 * The Ethos-U backend copies the input into the NPU scratch with
 * arm_ethos_io_memcpy(); the override below turns RGB888 into the int8 input
 * on the way (the input scale is 1/255 with zero point -128, so
 * q = pixel - 128 = pixel ^ 0x80): no staging buffer, no conversion pass.
 */

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include <arm_mve.h>

#include "RTE_Components.h"
#include CMSIS_device_header

#include <executorch/extension/data_loader/buffer_data_loader.h>
#include <executorch/runtime/core/evalue.h>
#include <executorch/runtime/core/exec_aten/exec_aten.h>
#include <executorch/runtime/core/memory_allocator.h>
#include <executorch/runtime/platform/runtime.h>

#include "arm_embedded_module.hpp"
#include "detector.h"
#include "model_io.h"
#include "model_pte.h"

using arm::embedded::EmbeddedModule;
using executorch::aten::DimOrderType;
using executorch::aten::ScalarType;
using executorch::aten::SizesType;
using executorch::aten::Tensor;
using executorch::aten::TensorImpl;
using executorch::extension::BufferDataLoader;
using executorch::runtime::EValue;
using executorch::runtime::MemoryAllocator;

namespace {

// Score threshold of a cat. The cat AP50 on COCO val2017 is 0.87 in float at
// this input size, 0.85 in int8 (yolo/eval_cats.py).
constexpr float kScoreThreshold = 0.30f;
constexpr float kIouDuplicate = 0.70f;

constexpr int32_t kInputShape[] = MODEL_DETECT_INPUT0_SHAPE;  // {1, S, S, 3}
constexpr int kSize = kInputShape[1];
constexpr int32_t kBoxShape[] = MODEL_DETECT_OUTPUT0_SHAPE;  // {1, 4, N}
constexpr int kAnchors = kBoxShape[2];
constexpr int kStrides[] = {8, 16, 32};
static_assert(kAnchors == (kSize / 8) * (kSize / 8) + (kSize / 16) * (kSize / 16) + (kSize / 32) * (kSize / 32),
              "anchor count");

// Method pool: the planned buffers (the two outputs, 18 kB) and the runtime's
// structures. Temp pool: Vela's scratch, 1224 KiB for YOLO26n at 416 x 416.
#ifdef YOLO_BENCHMARK_SRAM
constexpr size_t kMethodPoolSize = 0x10000;  // benchmark: room for the program copy in SRAM0
#else
constexpr size_t kMethodPoolSize = 0x40000;
#endif
constexpr size_t kTempPoolSize = 0x140000;
alignas(16) uint8_t g_method_pool[kMethodPoolSize] __attribute__((section(APP_POOL_SECTION)));
alignas(16) uint8_t g_temp_pool[kTempPoolSize] __attribute__((section(APP_POOL_SECTION)));

const uint8_t* g_input_rgb;  // the frame the input copy converts
const int8_t* g_scores;      // the cat score output of the last run
uint32_t g_candidates;       // its anchors above the threshold
int8_t g_score_max;          // its highest score
uint32_t g_npu_begin, g_npu_cycles;

inline uint32_t cycles() { return DWT->CYCCNT; }
inline uint32_t us(uint32_t c) { return static_cast<uint32_t>(static_cast<uint64_t>(c) * 1000000U / SystemCoreClock); }

// The module and the input tensor live as long as the application.
alignas(EmbeddedModule) uint8_t g_module_storage[sizeof(EmbeddedModule)];
EmbeddedModule* g_module;
std::array<SizesType, 4> g_sizes;
std::array<DimOrderType, 4> g_dim_order;
alignas(TensorImpl) uint8_t g_input_storage[sizeof(TensorImpl)];
TensorImpl* g_input;

float iou(const detection_t& a, const detection_t& b) {
  const float w = fminf(a.x2, b.x2) - fmaxf(a.x1, b.x1);
  const float h = fminf(a.y2, b.y2) - fmaxf(a.y1, b.y1);
  if (w <= 0.0f || h <= 0.0f) return 0.0f;
  const float inter = w * h;
  return inter / ((a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter);
}

// The rest of the YOLO26 head: the anchors above the threshold, their distances to boxes.
uint32_t decode(const int8_t* box, const int8_t* score, detection_t* out) {
  constexpr float kBoxScale = MODEL_DETECT_OUTPUT0_SCALE;
  constexpr int kBoxZp = MODEL_DETECT_OUTPUT0_ZERO_POINT;
  constexpr float kScoreScale = MODEL_DETECT_OUTPUT1_SCALE;
  constexpr int kScoreZp = MODEL_DETECT_OUTPUT1_ZERO_POINT;
  const int threshold_q = static_cast<int>(ceilf(kScoreThreshold / kScoreScale)) + kScoreZp;

  detection_t found[DETECTOR_MAX_DETECTIONS * 2];
  int n = 0;
  int base = 0;
  g_candidates = 0;
  g_score_max = INT8_MIN;
  for (int stride : kStrides) {
    const int cells = kSize / stride;
    for (int a = base; a < base + cells * cells; ++a) {
      if (score[a] > g_score_max) g_score_max = score[a];
      if (score[a] < threshold_q) continue;
      ++g_candidates;
      const int cell = a - base;
      const float cx = static_cast<float>(cell % cells) + 0.5f;
      const float cy = static_cast<float>(cell / cells) + 0.5f;
      const float s = static_cast<float>(stride);
      detection_t d;
      d.x1 = (cx - (box[0 * kAnchors + a] - kBoxZp) * kBoxScale) * s;
      d.y1 = (cy - (box[1 * kAnchors + a] - kBoxZp) * kBoxScale) * s;
      d.x2 = (cx + (box[2 * kAnchors + a] - kBoxZp) * kBoxScale) * s;
      d.y2 = (cy + (box[3 * kAnchors + a] - kBoxZp) * kBoxScale) * s;
      d.score = (score[a] - kScoreZp) * kScoreScale;
      if (n < DETECTOR_MAX_DETECTIONS * 2) {
        found[n++] = d;
      } else {  // full: replace the weakest
        int weakest = 0;
        for (int i = 1; i < n; ++i)
          if (found[i].score < found[weakest].score) weakest = i;
        if (d.score > found[weakest].score) found[weakest] = d;
      }
    }
    base += cells * cells;
  }

  // Strongest first, drop what overlaps a stronger box.
  for (int i = 1; i < n; ++i)
    for (int j = i; j > 0 && found[j].score > found[j - 1].score; --j) std::swap(found[j], found[j - 1]);
  uint32_t kept = 0;
  for (int i = 0; i < n && kept < DETECTOR_MAX_DETECTIONS; ++i) {
    bool duplicate = false;
    for (uint32_t k = 0; k < kept && !duplicate; ++k) duplicate = iou(found[i], out[k]) > kIouDuplicate;
    if (!duplicate) out[kept++] = found[i];
  }
  return kept;
}

}  // namespace

extern "C" const int DETECTOR_INPUT_SIZE = kSize;
extern "C" const int DETECTOR_ANCHORS = kAnchors;

// The Ethos-U backend's IO copy hook (strong override of the weak default).
// The input: RGB888 to int8 (x ^ 0x80), 16 bytes per Helium load / store,
// tail by predicate. Everything else (the two small outputs) is a memcpy.
extern "C" void arm_ethos_io_memcpy(void* dst, const void* src, size_t size) {
  if (src == g_input_rgb && size == MODEL_DETECT_INPUT0_NUMEL) {
    const uint8_t* s = static_cast<const uint8_t*>(src);
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8x16_t offset = vdupq_n_u8(0x80);
    for (int32_t left = static_cast<int32_t>(size); left > 0; left -= 16, s += 16, d += 16) {
      mve_pred16_t pred = vctp8q(static_cast<uint32_t>(left));
      vstrbq_p_u8(d, veorq_u8(vldrbq_z_u8(s, pred), offset), pred);
    }
    return;
  }
  memcpy(dst, src, size);
}

// The driver's hooks around the NPU job: command stream start to interrupt.
extern "C" void ethosu_inference_begin(struct ethosu_driver* drv, void* user_arg) {
  (void)drv;
  (void)user_arg;
  g_npu_begin = cycles();
}
extern "C" void ethosu_inference_end(struct ethosu_driver* drv, void* user_arg) {
  (void)drv;
  (void)user_arg;
  g_npu_cycles += cycles() - g_npu_begin;
}

extern "C" int32_t detector_init(void) { return detector_init_from(model_pte); }

extern "C" int32_t detector_init_from(const uint8_t* program) {
  executorch::runtime::runtime_init();
  DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  g_module = new (g_module_storage)
      EmbeddedModule(program, model_pte_size, std::make_unique<BufferDataLoader>(program, model_pte_size),
                     std::make_unique<MemoryAllocator>(kMethodPoolSize, g_method_pool),
                     std::make_unique<MemoryAllocator>(kTempPoolSize, g_temp_pool));
  for (int i = 0; i < 4; ++i) {
    g_sizes[i] = kInputShape[i];
    g_dim_order[i] = static_cast<DimOrderType>(i);
  }
  g_input = new (g_input_storage) TensorImpl(ScalarType::Char, 4, g_sizes.data(), nullptr, g_dim_order.data());
  return static_cast<int32_t>(g_module->load_method(MODEL_DETECT_METHOD));
}

extern "C" int32_t detector_run(const uint8_t* rgb, detections_t* out) {
  g_input_rgb = rgb;
  g_input->set_data(const_cast<uint8_t*>(rgb));
  std::vector<EValue> inputs{EValue(Tensor(g_input))};

  g_npu_cycles = 0;
  const uint32_t t0 = cycles();
  auto result = g_module->execute(MODEL_DETECT_METHOD, inputs);
  if (!result.ok()) return static_cast<int32_t>(result.error());
  if (result->size() != MODEL_DETECT_NUM_OUTPUTS) return -1;
  g_scores = (*result)[1].toTensor().const_data_ptr<int8_t>();
  out->count = decode((*result)[0].toTensor().const_data_ptr<int8_t>(), g_scores, out->det);
  out->total_us = us(cycles() - t0);
  out->npu_us = us(g_npu_cycles);
  for (uint32_t i = out->count; i < DETECTOR_MAX_DETECTIONS; ++i) out->det[i] = detection_t{};
  return 0;
}

extern "C" int32_t detector_last_scores(detector_scores_t* out) {
  if (g_scores == nullptr) return -1;
  out->score = g_scores;
  out->anchors = kAnchors;
  out->scale = MODEL_DETECT_OUTPUT1_SCALE;
  out->zero_point = MODEL_DETECT_OUTPUT1_ZERO_POINT;
  out->threshold = kScoreThreshold;
  out->candidates = g_candidates;
  out->max = (g_score_max - MODEL_DETECT_OUTPUT1_ZERO_POINT) * MODEL_DETECT_OUTPUT1_SCALE;
  return 0;
}
