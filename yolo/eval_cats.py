#!/usr/bin/env python3
# Copyright 2026 Arm Limited and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""How well the cat detector of model/yolo.py finds cats, in float and int8.

    python yolo/eval_cats.py <dataset> [<dataset> ...] [--sizes 320,416] [--threshold 0.4]

Each dataset is a directory in the YOLO layout (images/*.jpg, labels/*.txt,
class 15 is "cat"), for example the cat images of COCO val2017 plus some
without cats. For each input size it reports the cat AP at IoU 0.5 and the
precision / recall at the application's threshold, for the float module and
for the PT2E-quantized one that create_ai_layer.py hands to Vela (its fake
quantization runs on the host, so this is the int8 accuracy without the board).
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "model"))


def load_dataset(dirs: list[Path]) -> list[tuple[Path, np.ndarray]]:
    """(image, GT cat boxes (K, 4) as normalized cx, cy, w, h) per image."""
    items = []
    for d in dirs:
        for img in sorted((d / "images").glob("*.jpg")):
            label = d / "labels" / (img.stem + ".txt")
            rows = [line.split() for line in label.read_text().splitlines()] if label.is_file() else []
            cats = np.array([[float(v) for v in r[1:5]] for r in rows if r and r[0] == "15"], dtype=np.float32).reshape(-1, 4)
            items.append((img, cats))
    return items


def iou(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    tl = np.maximum(a[:, None, :2], b[None, :, :2])
    br = np.minimum(a[:, None, 2:], b[None, :, 2:])
    inter = np.prod(np.clip(br - tl, 0, None), 2)
    area = lambda x: np.prod(x[:, 2:] - x[:, :2], 1)  # noqa: E731
    return inter / (area(a)[:, None] + area(b)[None, :] - inter + 1e-9)


def evaluate(run, items, size: int, threshold: float) -> dict:
    """run(input) -> (box (4, N), score (N,)); AP50 over all images and P / R at `threshold`."""
    import cv2
    import torch

    from yolo import decode, letterbox, to_input

    scored, n_gt = [], 0  # (score, is_true_positive)
    tp_t = fp_t = 0
    for path, gt in items:
        bgr = cv2.imread(str(path))
        h, w = bgr.shape[:2]
        rgb, r, (px, py) = letterbox(bgr, size)
        with torch.no_grad():
            box, score = run(to_input(rgb))
        det = decode(box[0].numpy(), score[0, 0].numpy(), 0.01, size)
        det[:, [0, 2]] = (det[:, [0, 2]] - px) / r
        det[:, [1, 3]] = (det[:, [1, 3]] - py) / r
        g = np.concatenate([(gt[:, :2] - gt[:, 2:] / 2), (gt[:, :2] + gt[:, 2:] / 2)], 1) * [w, h, w, h]
        n_gt += len(g)
        det = det[np.argsort(-det[:, 4])]
        matched = np.zeros(len(g), bool)
        ious = iou(det[:, :4], g) if len(g) and len(det) else np.zeros((len(det), len(g)))
        for i, d in enumerate(det):
            j = int(np.argmax(ious[i])) if len(g) else -1
            hit = j >= 0 and ious[i, j] >= 0.5 and not matched[j]
            if hit:
                matched[j] = True
            scored.append((d[4], hit))
            if d[4] > threshold:
                tp_t += hit
                fp_t += not hit
    scored.sort(key=lambda s: -s[0])
    hits = np.array([s[1] for s in scored], dtype=np.float64)
    tp = np.cumsum(hits)
    recall = tp / max(n_gt, 1)
    precision = tp / np.arange(1, len(hits) + 1)
    envelope = np.maximum.accumulate(precision[::-1])[::-1]  # all-point interpolation
    ap = float(np.sum(np.diff(np.concatenate([[0.0], recall])) * envelope)) if len(hits) else 0.0
    return {"ap50": ap, "precision": tp_t / max(tp_t + fp_t, 1), "recall": tp_t / max(n_gt, 1), "gt": n_gt}


def quantized(size: int):
    """The module as create_ai_layer.py quantizes it, before Vela."""
    import yaml

    import create_ai_layer
    from yolo import get_yolo_methods

    mlops_file = ROOT / "ai_layer_yolo" / "cmsis-executorch.cbuild-mlops.yml"
    mlops = yaml.safe_load(mlops_file.read_text())["cbuild-mlops"]
    spec = create_ai_layer.compile_spec(mlops, mlops_file.parent)
    (method,) = get_yolo_methods()
    return method.module, create_ai_layer.quantize_method(spec, method)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("datasets", nargs="+", type=Path)
    parser.add_argument("--sizes", default=os.environ.get("YOLO_IMGSZ", "320"))
    parser.add_argument("--threshold", type=float, default=0.4)
    args = parser.parse_args()
    items = load_dataset(args.datasets)
    print(f"{len(items)} images, {sum(len(g) for _, g in items)} cats")
    for size in (int(s) for s in args.sizes.split(",")):
        os.environ["YOLO_IMGSZ"] = str(size)
        import yolo

        yolo.IMAGE_SIZE = size
        float_module, int8_module = quantized(size)
        for name, run in (("float", float_module), ("int8", int8_module)):
            r = evaluate(run, items, size, args.threshold)
            print(
                f"{size:4d} {name:5s}  AP50 {r['ap50']:.3f}   at {args.threshold}: "
                f"precision {r['precision']:.3f} recall {r['recall']:.3f}"
            )


if __name__ == "__main__":
    main()
