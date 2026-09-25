/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The cat detector: YOLO26n on the Ethos-U85 (the `detect` method of
 * model/yolo.py in ai_layer_yolo/) and the rest of its head on the CPU.
 */
#ifndef YOLO_DETECTOR_H_
#define YOLO_DETECTOR_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DETECTOR_MAX_DETECTIONS 16

typedef struct {
    float x1, y1, x2, y2;  /* input pixels (DETECTOR_INPUT_SIZE square) */
    float score;
} detection_t;

/* The result of one frame; also the record of the SDS stream "Detections"
   (yolo/Detections.sds.yml), so the layout is fixed: little endian, no padding. */
typedef struct {
    uint32_t    frame;     /* frame counter of the application */
    uint32_t    count;     /* valid entries in det[] */
    uint32_t    npu_us;    /* NPU job, command stream start to interrupt */
    uint32_t    total_us;  /* module.execute() and the decode */
    detection_t det[DETECTOR_MAX_DETECTIONS];
} detections_t;

/* Input size in pixels: the model takes DETECTOR_INPUT_SIZE x DETECTOR_INPUT_SIZE RGB888. */
extern const int DETECTOR_INPUT_SIZE;

/* Anchors of the model: the cells of the stride 8, 16 and 32 maps, row by row, stride 8 first. */
extern const int DETECTOR_ANCHORS;

/* The raw cat scores of the last detector_run, valid until the next run. */
typedef struct {
    const int8_t *score;    /* one per anchor */
    int           anchors;
    float         scale;    /* score = (q - zero_point) * scale */
    int           zero_point;
    float         threshold;  /* the detection threshold */
    uint32_t      candidates; /* anchors at or above the threshold, before the duplicate check */
    float         max;        /* the highest score */
} detector_scores_t;

/* Load the program and the method. 0 on success. */
int32_t detector_init(void);

/* The same with a copy of the program somewhere else (the benchmark's copy in SRAM). */
int32_t detector_init_from(const uint8_t *program);

/* Run the detector on an interleaved RGB888 image of the input size (in the
   bulk SRAM or the MRAM); fills `out` (frame is left alone). 0 on success,
   else the ExecuTorch error. */
int32_t detector_run(const uint8_t *rgb, detections_t *out);

/* The scores of the last successful run. 0 on success, -1 before the first. */
int32_t detector_last_scores(detector_scores_t *out);

#ifdef __cplusplus
}
#endif

#endif /* YOLO_DETECTOR_H_ */
