/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The tracker and the counting line of the traffic counter. The detections
 * of each frame are matched to the tracks of the frame before by overlap; a
 * track that has been seen a few times is confirmed, and counted, once, when
 * its centre crosses the line. The class of a track is the majority vote of
 * its detections; the count goes to the class the track had when it crossed.
 *
 * Nothing here is learned: the YOLO26 one-to-one head gives one box per
 * object, so a greedy match by IoU with a short constant-velocity prediction
 * follows a vehicle through a few frames of the 30 to 50 a second the NPU
 * delivers.
 */
#ifndef TRAFFIC_TRACKER_H_
#define TRAFFIC_TRACKER_H_

#include <stdint.h>

#include "detector.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TRACKER_MAX_TRACKS 24

typedef struct {
    uint32_t id;               /* 1, 2, 3, ...: 0 is a free entry */
    float    x1, y1, x2, y2;   /* the box, input pixels */
    float    vx, vy;           /* the centre's motion per frame, input pixels */
    int32_t  cls;              /* the majority class so far */
    uint16_t votes[DETECTOR_CLASSES];
    uint16_t hits;             /* frames with a detection */
    uint16_t misses;           /* frames without one, in a row */
    uint8_t  confirmed;        /* hits >= TRACKER_CONFIRM_HITS */
    uint8_t  counted;          /* has crossed the line and been counted */
    int8_t   side;             /* -1 before the line (above / left), +1 after it, 0 unknown */
} track_t;

/* Direction 0: from before the line to after it (downwards for a horizontal
   line, rightwards for a vertical one); direction 1: the other way. */
typedef struct {
    uint32_t total;
    uint32_t by_class[DETECTOR_CLASSES];
    uint32_t by_direction[2];
    uint32_t by_class_direction[DETECTOR_CLASSES][2];
    uint32_t tracks_started;   /* tracks ever created */
    uint32_t tracks_confirmed;
} traffic_counts_t;

/* `size`: the input size; the line is at `line_pos` input pixels, vertical or horizontal. */
void tracker_init(int size, int line_pos, int vertical);

/* One frame: match, update, count, prune. */
void tracker_update(const detections_t *det);

/* The live tracks (free entries have id 0). */
const track_t *tracker_tracks(void);

/* Tracks with an id, and how many of them are confirmed. */
uint32_t tracker_live(uint32_t *confirmed);

const traffic_counts_t *tracker_counts(void);
void tracker_reset_counts(void);

int tracker_line_pos(void);
int tracker_line_vertical(void);

#ifdef __cplusplus
}
#endif

#endif /* TRAFFIC_TRACKER_H_ */
