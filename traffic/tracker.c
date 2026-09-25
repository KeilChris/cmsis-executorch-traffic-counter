/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The tracker and the counting line, see tracker.h.
 */

#include <string.h>

#include "tracker.h"

#define TRACKER_CONFIRM_HITS 3     /* detections before a track counts */
#define TRACKER_MAX_MISSES   10    /* frames without a detection before a track ends */
#define TRACKER_MATCH_IOU    0.30f /* least overlap of a detection with a track's predicted box */
#define TRACKER_VELOCITY_MIX 0.5f  /* new velocity = mix * measured + (1 - mix) * old */

static track_t tracks[TRACKER_MAX_TRACKS];
static traffic_counts_t counts;
static uint32_t next_id = 1;
static int line_pos, line_vertical, input_size;

static float iou(float ax1, float ay1, float ax2, float ay2, const detection_t *d)
{
    const float w = (ax2 < d->x2 ? ax2 : d->x2) - (ax1 > d->x1 ? ax1 : d->x1);
    const float h = (ay2 < d->y2 ? ay2 : d->y2) - (ay1 > d->y1 ? ay1 : d->y1);
    if (w <= 0.0f || h <= 0.0f) {
        return 0.0f;
    }
    const float inter = w * h;
    return inter / ((ax2 - ax1) * (ay2 - ay1) + (d->x2 - d->x1) * (d->y2 - d->y1) - inter);
}

static int8_t side_of(const track_t *t)
{
    const float c = line_vertical ? 0.5f * (t->x1 + t->x2) : 0.5f * (t->y1 + t->y2);
    return c < (float)line_pos ? -1 : 1;
}

static int32_t majority(const track_t *t)
{
    int32_t best = t->cls;
    for (int c = 0; c < DETECTOR_CLASSES; c++) {
        if (t->votes[c] > t->votes[best]) {
            best = c;
        }
    }
    return best;
}

void tracker_init(int size, int pos, int vertical)
{
    input_size    = size;
    line_pos      = pos;
    line_vertical = vertical;
    memset(tracks, 0, sizeof(tracks));
    memset(&counts, 0, sizeof(counts));
    next_id = 1;
}

void tracker_update(const detections_t *det)
{
    uint8_t taken[DETECTOR_MAX_DETECTIONS] = {0};

    /* Each live track: its box moved on by its velocity is what this frame's
       detection is compared with; the best overlapping unclaimed detection is its. */
    for (int i = 0; i < TRACKER_MAX_TRACKS; i++) {
        track_t *t = &tracks[i];
        if (t->id == 0U) {
            continue;
        }
        const float px1 = t->x1 + t->vx, py1 = t->y1 + t->vy, px2 = t->x2 + t->vx, py2 = t->y2 + t->vy;
        int best = -1;
        float best_iou = TRACKER_MATCH_IOU;
        for (uint32_t k = 0; k < det->count && k < DETECTOR_MAX_DETECTIONS; k++) {
            if (taken[k]) {
                continue;
            }
            const float o = iou(px1, py1, px2, py2, &det->det[k]);
            if (o > best_iou) {
                best_iou = o;
                best     = (int)k;
            }
        }
        if (best < 0) {
            /* Not seen: coast on the prediction. */
            t->x1 = px1;
            t->y1 = py1;
            t->x2 = px2;
            t->y2 = py2;
            t->misses++;
            continue;
        }
        taken[best] = 1U;
        const detection_t *d = &det->det[best];
        const float mvx = 0.5f * (d->x1 + d->x2) - 0.5f * (t->x1 + t->x2);
        const float mvy = 0.5f * (d->y1 + d->y2) - 0.5f * (t->y1 + t->y2);
        t->vx = TRACKER_VELOCITY_MIX * mvx + (1.0f - TRACKER_VELOCITY_MIX) * t->vx;
        t->vy = TRACKER_VELOCITY_MIX * mvy + (1.0f - TRACKER_VELOCITY_MIX) * t->vy;
        t->x1 = d->x1;
        t->y1 = d->y1;
        t->x2 = d->x2;
        t->y2 = d->y2;
        if (d->cls >= 0 && d->cls < DETECTOR_CLASSES && t->votes[d->cls] < 0xFFFFU) {
            t->votes[d->cls]++;
        }
        t->cls    = majority(t);
        t->misses = 0;
        if (t->hits < 0xFFFFU) {
            t->hits++;
        }
        if (!t->confirmed && t->hits >= TRACKER_CONFIRM_HITS) {
            t->confirmed = 1U;
            counts.tracks_confirmed++;
        }
    }

    /* Unclaimed detections start tracks. */
    for (uint32_t k = 0; k < det->count && k < DETECTOR_MAX_DETECTIONS; k++) {
        if (taken[k]) {
            continue;
        }
        track_t *t = NULL;
        for (int i = 0; i < TRACKER_MAX_TRACKS; i++) {
            if (tracks[i].id == 0U) {
                t = &tracks[i];
                break;
            }
        }
        if (t == NULL) {
            break;  /* full: the rest waits for the next frame */
        }
        const detection_t *d = &det->det[k];
        memset(t, 0, sizeof(*t));
        t->id  = next_id++;
        t->x1  = d->x1;
        t->y1  = d->y1;
        t->x2  = d->x2;
        t->y2  = d->y2;
        t->cls = d->cls >= 0 && d->cls < DETECTOR_CLASSES ? d->cls : 0;
        t->votes[t->cls] = 1U;
        t->hits = 1U;
        t->side = side_of(t);
        counts.tracks_started++;
    }

    /* The line: a confirmed track whose centre is on the other side of the
       line than before has crossed it. Once counted, a track stays counted. */
    for (int i = 0; i < TRACKER_MAX_TRACKS; i++) {
        track_t *t = &tracks[i];
        if (t->id == 0U) {
            continue;
        }
        if (t->misses > TRACKER_MAX_MISSES || t->x2 <= 0.0f || t->y2 <= 0.0f || t->x1 >= (float)input_size ||
            t->y1 >= (float)input_size) {
            t->id = 0U;  /* gone, or coasted off the picture */
            continue;
        }
        if (t->misses != 0U) {
            continue;  /* a coasting track does not cross */
        }
        const int8_t side = side_of(t);
        if (t->confirmed && !t->counted && t->side != 0 && side != t->side) {
            const int dir = side > 0 ? 0 : 1;
            t->counted = 1U;
            counts.total++;
            counts.by_class[t->cls]++;
            counts.by_direction[dir]++;
            counts.by_class_direction[t->cls][dir]++;
        }
        t->side = side;
    }
}

const track_t *tracker_tracks(void)
{
    return tracks;
}

uint32_t tracker_live(uint32_t *confirmed)
{
    uint32_t live = 0, sure = 0;
    for (int i = 0; i < TRACKER_MAX_TRACKS; i++) {
        if (tracks[i].id != 0U) {
            live++;
            sure += tracks[i].confirmed;
        }
    }
    if (confirmed != NULL) {
        *confirmed = sure;
    }
    return live;
}

const traffic_counts_t *tracker_counts(void)
{
    return &counts;
}

void tracker_reset_counts(void)
{
    memset(&counts, 0, sizeof(counts));
}

int tracker_line_pos(void)
{
    return line_pos;
}

int tracker_line_vertical(void)
{
    return line_vertical;
}
