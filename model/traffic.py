# Copyright 2026 Arm Limited and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Vehicle detection with Ultralytics YOLO26n on the Ethos-U55, for the traffic counter.

The same cut of YOLO26n as model/yolo.py (the NMS-free one-to-one head, the
network up to the raw head outputs on the NPU, the threshold and the box
decode on the CPU), with the classification branch ending in the vehicle
classes of COCO instead of the cat alone:

  detect(image)  int8   image   (1, S, S, 3)  RGB in [0, 1], interleaved
                        -> box  (1, 4, N)     left, top, right, bottom distances
                                              from each anchor, in anchor strides
                        -> cls  (1, C, N)     sigmoid score per class, C = len(CLASSES)

The N anchors are the cells of the stride 8, 16 and 32 maps, row by row,
stride 8 first. The CPU (traffic/detector.cpp) takes the best class of each
anchor, thresholds it and turns the distances into boxes; traffic/tracker.c
follows the boxes from frame to frame and counts them across a line.

The Ethos-U55 (the E7's NPU) has no TRANSPOSE: the NHWC to NCHW permute that
the Ethos-U85 ran as its first operator is handled by the Arm backend's
layout passes, and whatever it cannot fold stays on the CPU, so the export
reports what runs where.
"""

from __future__ import annotations

import copy
import os
from pathlib import Path

import numpy as np
import torch
from torch import nn

from model import MethodSpec
from yolo import (  # noqa: F401  (re-exported for the scripts in traffic/)
    LETTERBOX_FILL,
    STRIDES,
    anchors,
    coco128_images,
    letterbox,
    load_detection_model,
    num_anchors,
    to_input,
)
import yolo

# COCO index and name of each output channel, in order.
CLASSES = ((1, "bicycle"), (2, "car"), (3, "motorcycle"), (5, "bus"), (7, "truck"))
CLASS_INDICES = [c for c, _ in CLASSES]
CLASS_NAMES = [n for _, n in CLASSES]
IMAGE_SIZE = int(os.environ.get("YOLO_IMGSZ", "416"))  # square input, a multiple of 32
CALIBRATION_IMAGES = 64

CACHE = Path(__file__).resolve().parent / ".cache"


class YoloVehicles(nn.Module):
    """YOLO26n up to the raw one-to-one head outputs, the vehicle classes only; see the module docstring."""

    def __init__(self, detection_model: nn.Module, classes: list[int] | None = None) -> None:
        super().__init__()
        classes = classes or CLASS_INDICES
        self.layers = detection_model.model[:-1]
        self.save = set(detection_model.save)
        head = detection_model.model[-1]
        self.from_ = list(head.f)
        self.box_head = head.one2one_cv2
        cls_head = copy.deepcopy(head.one2one_cv3)
        for branch in cls_head:
            full = branch[-1]
            few = nn.Conv2d(full.in_channels, len(classes), kernel_size=1, bias=True)
            few.weight.data = full.weight.data[classes].clone()
            few.bias.data = full.bias.data[classes].clone()
            branch[-1] = few
        self.cls_head = cls_head

    def forward(self, image: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        x = image.permute(0, 3, 1, 2)  # NHWC -> NCHW
        saved: list[torch.Tensor | None] = []
        for m in self.layers:
            if m.f != -1:
                x = saved[m.f] if isinstance(m.f, int) else [x if j == -1 else saved[j] for j in m.f]
            x = m(x)
            saved.append(x if m.i in self.save else None)
        feats = [saved[j] for j in self.from_]
        box = torch.cat([b(f).flatten(2) for b, f in zip(self.box_head, feats)], dim=2)
        cls = torch.cat([c(f).flatten(2) for c, f in zip(self.cls_head, feats)], dim=2)
        return box, torch.sigmoid(cls)


def decode(box: np.ndarray, score: np.ndarray, threshold: float, size: int | None = None) -> np.ndarray:
    """Head outputs of one image, (4, N) and (C, N), to detections (K, 6): x1, y1, x2, y2 in input pixels, score, class.

    What traffic/detector.cpp does on the board: the best class per anchor, then the threshold."""
    size = size or IMAGE_SIZE
    centre, stride = anchors(size)
    best = score.argmax(0)
    conf = score[best, np.arange(score.shape[1])]
    keep = np.nonzero(conf > threshold)[0]
    ltrb = box[:, keep].T
    c = centre[keep]
    xyxy = np.concatenate([c - ltrb[:, :2], c + ltrb[:, 2:]], 1) * stride[keep, None]
    return np.concatenate([xyxy, conf[keep, None], best[keep, None].astype(np.float32)], 1)


def _samples() -> list[tuple[torch.Tensor]]:
    """Letterboxed COCO128 images, those with vehicles first (they pin the score output's range)."""
    import cv2

    images = coco128_images()
    labels = CACHE / "coco128" / "labels" / "train2017"
    wanted = {str(c) for c in CLASS_INDICES}

    def vehicles(p: Path) -> int:
        f = labels / (p.stem + ".txt")
        return sum(line.split()[0] in wanted for line in f.read_text().splitlines()) if f.is_file() else 0

    ordered = sorted(images, key=lambda p: -vehicles(p))[:CALIBRATION_IMAGES]
    return [(to_input(letterbox(cv2.imread(str(p)), IMAGE_SIZE)[0]),) for p in ordered]


def get_traffic_methods() -> list[MethodSpec]:
    yolo.IMAGE_SIZE = IMAGE_SIZE
    return [MethodSpec("detect", YoloVehicles(load_detection_model()).eval(), _samples())]
