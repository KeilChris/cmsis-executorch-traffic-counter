#!/usr/bin/env python3
# Copyright 2026 Arm Limited and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""The traffic count of a playback, on the PC: the board's tracker run over its own detections and over the float model's.

    python traffic/count_playback.py <recording dir> [--line 208] [--vertical] [--sweep] [--host-only|--board-only]

<recording dir> holds CameraIn.<n>.sds (the model inputs) and, after a playback,
Detections.<n>.p.sds (what the board found). This runs tracker.c's logic (a
line-by-line port) over

  board  the detections the board recorded, so the count is the one the board
         showed for that playback (the app prints it every 100 frames), and
  host   the float model's detections of the same inputs, decoded as the board
         does (threshold 0.30, strongest first, duplicates over IoU 0.7 dropped,
         16 per frame), the reference,

and prints both counts by class and direction, plus where the tracks moved, so
a line that the traffic does not cross shows up. --sweep repeats the count for
a horizontal and a vertical line at several positions. The host detections are
cached in <recording dir>/host_detections.npz.
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "model"))
sys.path.insert(0, str(HERE))

from images_to_sds import MAX_DETECTIONS, RECORD, records  # noqa: E402

# tracker.c / detector.cpp
CLASSES = 5
CLASS_NAMES = ("bicycle", "car", "motorcycle", "bus", "truck")
CONFIRM_HITS = 3
MAX_MISSES = 10
MATCH_IOU = 0.30
VELOCITY_MIX = 0.5
MAX_TRACKS = 24
SCORE_THRESHOLD = 0.30
IOU_DUPLICATE = 0.70


def iou(a, b) -> float:
    w = min(a[2], b[2]) - max(a[0], b[0])
    h = min(a[3], b[3]) - max(a[1], b[1])
    if w <= 0 or h <= 0:
        return 0.0
    inter = w * h
    return inter / ((a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter)


@dataclass
class Track:
    id: int
    box: list[float]
    vx: float = 0.0
    vy: float = 0.0
    cls: int = 0
    votes: list[int] = field(default_factory=lambda: [0] * CLASSES)
    hits: int = 1
    misses: int = 0
    confirmed: bool = False
    counted: bool = False
    side: int = 0
    path: list[tuple[float, float]] = field(default_factory=list)


class Tracker:
    """tracker.c: the same matching, coasting, confirmation and line crossing."""

    def __init__(self, size: int, line_pos: int, vertical: bool) -> None:
        self.size, self.line_pos, self.vertical = size, line_pos, vertical
        self.tracks: list[Track] = []
        self.next_id = 1
        self.total = 0
        self.by_class = [0] * CLASSES
        self.by_direction = [0, 0]
        self.by_class_direction = [[0, 0] for _ in range(CLASSES)]
        self.started = self.confirmed = 0
        self.ended: list[Track] = []  # for the motion summary

    def side_of(self, t: Track) -> int:
        c = 0.5 * (t.box[0] + t.box[2]) if self.vertical else 0.5 * (t.box[1] + t.box[3])
        return -1 if c < self.line_pos else 1

    def update(self, dets: np.ndarray) -> None:
        """dets: (K, 6) x1, y1, x2, y2, score, cls, in the board's order."""
        taken = [False] * len(dets)
        for t in self.tracks:
            pred = [t.box[0] + t.vx, t.box[1] + t.vy, t.box[2] + t.vx, t.box[3] + t.vy]
            best, best_iou = -1, MATCH_IOU
            for k, d in enumerate(dets):
                if taken[k]:
                    continue
                o = iou(pred, d)
                if o > best_iou:
                    best, best_iou = k, o
            if best < 0:
                t.box = pred
                t.misses += 1
                continue
            taken[best] = True
            d = dets[best]
            mvx = 0.5 * (d[0] + d[2]) - 0.5 * (t.box[0] + t.box[2])
            mvy = 0.5 * (d[1] + d[3]) - 0.5 * (t.box[1] + t.box[3])
            t.vx = VELOCITY_MIX * mvx + (1 - VELOCITY_MIX) * t.vx
            t.vy = VELOCITY_MIX * mvy + (1 - VELOCITY_MIX) * t.vy
            t.box = [float(d[0]), float(d[1]), float(d[2]), float(d[3])]
            c = int(d[5])
            if 0 <= c < CLASSES:
                t.votes[c] += 1
            t.cls = max(range(CLASSES), key=lambda i: (t.votes[i], i == t.cls))
            t.misses = 0
            t.hits += 1
            t.path.append((0.5 * (t.box[0] + t.box[2]), 0.5 * (t.box[1] + t.box[3])))
            if not t.confirmed and t.hits >= CONFIRM_HITS:
                t.confirmed = True
                self.confirmed += 1
        for k, d in enumerate(dets):
            if taken[k]:
                continue
            if len(self.tracks) >= MAX_TRACKS:
                break
            c = int(d[5]) if 0 <= int(d[5]) < CLASSES else 0
            t = Track(self.next_id, [float(d[0]), float(d[1]), float(d[2]), float(d[3])], cls=c)
            t.votes[c] = 1
            t.side = self.side_of(t)
            t.path.append((0.5 * (t.box[0] + t.box[2]), 0.5 * (t.box[1] + t.box[3])))
            self.next_id += 1
            self.tracks.append(t)
            self.started += 1
        live = []
        for t in self.tracks:
            if t.misses > MAX_MISSES or t.box[2] <= 0 or t.box[3] <= 0 or t.box[0] >= self.size or t.box[1] >= self.size:
                self.ended.append(t)
                continue
            live.append(t)
            if t.misses != 0:
                continue
            side = self.side_of(t)
            if t.confirmed and not t.counted and t.side != 0 and side != t.side:
                direction = 0 if side > 0 else 1
                t.counted = True
                self.total += 1
                self.by_class[t.cls] += 1
                self.by_direction[direction] += 1
                self.by_class_direction[t.cls][direction] += 1
            t.side = side
        self.tracks = live

    def report(self, name: str) -> None:
        d0, d1 = ("RIGHT", "LEFT") if self.vertical else ("DOWN", "UP")
        by_class = ", ".join(f"{CLASS_NAMES[c]} {n}" for c, n in enumerate(self.by_class) if n)
        print(f"{name}: {self.total} counted ({d0} {self.by_direction[0]}, {d1} {self.by_direction[1]}); "
              f"{by_class or 'none'}; tracks started {self.started}, confirmed {self.confirmed}")


def board_detections(path: Path) -> list[np.ndarray]:
    out = []
    for _, data in records(path):
        rec = RECORD.unpack(data[: RECORD.size])
        out.append(np.array([rec[4 + 6 * k : 10 + 6 * k] for k in range(rec[1])], dtype=np.float32).reshape(-1, 6))
    return out


def board_decode(dets: np.ndarray) -> np.ndarray:
    """detector.cpp after the threshold: strongest first, duplicates dropped, at most MAX_DETECTIONS."""
    order = np.argsort(-dets[:, 4], kind="stable")
    kept: list[np.ndarray] = []
    for i in order:
        if len(kept) >= MAX_DETECTIONS:
            break
        if all(iou(dets[i], k) <= IOU_DUPLICATE for k in kept):
            kept.append(dets[i])
    return np.array(kept, dtype=np.float32).reshape(-1, 6)


def host_detections(workdir: Path) -> list[np.ndarray]:
    cache = workdir / "host_detections.npz"
    frames = workdir / "CameraIn.0.sds"
    if cache.exists() and cache.stat().st_mtime >= frames.stat().st_mtime:
        z = np.load(cache)
        return [z[f"f{i}"] for i in range(len(z.files))]
    import torch

    from traffic import IMAGE_SIZE, YoloVehicles, decode, load_detection_model

    model = YoloVehicles(load_detection_model()).eval()
    out = []
    for i, (_, data) in enumerate(records(frames)):
        rgb = np.frombuffer(data, np.uint8).reshape(IMAGE_SIZE, IMAGE_SIZE, 3)
        with torch.no_grad():
            box, score = model(torch.from_numpy(rgb.copy()).float().div(255).unsqueeze(0))
        out.append(board_decode(decode(box[0].numpy(), score[0].numpy(), SCORE_THRESHOLD)))
        if i % 100 == 0:
            print(f"host model: frame {i}", file=sys.stderr)
    np.savez_compressed(cache, **{f"f{i}": d for i, d in enumerate(out)})
    return out


def run(dets: list[np.ndarray], size: int, line: int, vertical: bool) -> Tracker:
    tr = Tracker(size, line, vertical)
    for d in dets:
        tr.update(d)
    tr.ended.extend(tr.tracks)
    return tr


def motion(tr: Tracker, size: int) -> str:
    """Where the confirmed tracks went: the mean start and end of their centres."""
    paths = [t.path for t in tr.ended if t.confirmed and len(t.path) >= 2]
    if not paths:
        return "no confirmed tracks"
    start = np.array([p[0] for p in paths])
    end = np.array([p[-1] for p in paths])
    move = end - start
    dx, dy = np.abs(move[:, 0]).mean(), np.abs(move[:, 1]).mean()
    rows = np.concatenate([[p[1] for p in path] for path in paths])
    cols = np.concatenate([[p[0] for p in path] for path in paths])
    return (f"{len(paths)} confirmed tracks, {np.mean([len(p) for p in paths]):.1f} detections each; "
            f"centres at x {np.percentile(cols, 10):.0f}..{np.percentile(cols, 90):.0f}, "
            f"y {np.percentile(rows, 10):.0f}..{np.percentile(rows, 90):.0f} of {size}; "
            f"a track moves {dx:.0f} px in x and {dy:.0f} px in y on average")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("workdir", type=Path)
    ap.add_argument("--line", type=int, default=None, help="line position in input pixels (default: the middle)")
    ap.add_argument("--vertical", action="store_true", help="a vertical line (default: horizontal, as the board)")
    ap.add_argument("--sweep", action="store_true", help="also count with lines at other places")
    ap.add_argument("--host-only", action="store_true")
    ap.add_argument("--board-only", action="store_true")
    args = ap.parse_args()

    from traffic import IMAGE_SIZE

    size = IMAGE_SIZE
    line = args.line if args.line is not None else size // 2
    sources = {}
    if not args.host_only:
        det = args.workdir / "Detections.0.p.sds"
        if det.exists():
            sources["board"] = board_detections(det)
        else:
            print(f"{det}: no playback result, host only")
    if not args.board_only:
        sources["host"] = host_detections(args.workdir)

    for name, dets in sources.items():
        n = sum(len(d) for d in dets)
        print(f"{name}: {len(dets)} frames, {n} detections, {sum(1 for d in dets if len(d))} frames with one")
        tr = run(dets, size, line, args.vertical)
        tr.report(f"  {'vertical' if args.vertical else 'horizontal'} line at {line}")
        print(f"  {motion(tr, size)}")
        if args.sweep:
            for vertical in (False, True):
                for pos in range(size // 8, size, size // 8):
                    run(dets, size, pos, vertical).report(f"  {'vertical' if vertical else 'horizontal'} line at {pos}")


if __name__ == "__main__":
    main()
