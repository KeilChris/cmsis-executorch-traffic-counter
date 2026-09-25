#!/usr/bin/env python3
# Copyright 2026 Arm Limited and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Images to a CameraIn stream for playback into the cat detector, and the check of what it found.

    python yolo/images_to_sds.py make <out dir> <image> [<image> ...]   # -> <out dir>/CameraIn.0.sds
    python yolo/images_to_sds.py check <out dir>                        # board vs host, per image

make: every image letterboxed to the model input (416x416 RGB888, as
model/yolo.py and yolo/make_test_image.py do it), one record per image, 100
ms apart, plus the metadata files. Play it with
`python yolo/sds_session.py play --workdir <out dir>`.

check: reads CameraIn.0.sds and the board's Detections.0.p.sds, runs the
float model on the host on the same inputs and lists both side by side.
"""

from __future__ import annotations

import shutil
import struct
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "model"))

MAX_DETECTIONS = 16  # detector.h
RECORD = struct.Struct("<4I" + "5f" * MAX_DETECTIONS)  # detections_t


def records(path: Path):
    data = path.read_bytes()
    offset = 0
    while offset + 8 <= len(data):
        timeslot, size = struct.unpack_from("<II", data, offset)
        offset += 8
        yield timeslot, data[offset : offset + size]
        offset += size


def make(out: Path, images: list[Path]) -> None:
    import cv2

    from yolo import IMAGE_SIZE, letterbox

    out.mkdir(parents=True, exist_ok=True)
    with open(out / "CameraIn.0.sds", "wb") as f:
        for i, path in enumerate(images):
            bgr = cv2.imread(str(path))
            if bgr is None:
                sys.exit(f"{path}: not an image")
            rgb, _, _ = letterbox(bgr, IMAGE_SIZE)
            data = rgb.tobytes()
            f.write(struct.pack("<II", i * 100, len(data)))
            f.write(data)
    (out / "CameraIn.0.txt").write_text("\n".join(str(p) for p in images) + "\n")
    for meta in ("CameraIn.sds.yml", "Detections.sds.yml"):
        shutil.copy(ROOT / "recordings/yolo" / meta, out / meta)
    print(f"{out / 'CameraIn.0.sds'}: {len(images)} records of {IMAGE_SIZE}x{IMAGE_SIZE} RGB888")


def check(out: Path) -> None:
    import torch

    from yolo import IMAGE_SIZE, YoloCat, decode, load_detection_model

    model = YoloCat(load_detection_model()).eval()
    names = (out / "CameraIn.0.txt").read_text().split() if (out / "CameraIn.0.txt").exists() else []
    board = {ts: RECORD.unpack(data[: RECORD.size]) for ts, data in records(out / "Detections.0.p.sds")}
    for i, (ts, data) in enumerate(records(out / "CameraIn.0.sds")):
        rgb = np.frombuffer(data, np.uint8).reshape(IMAGE_SIZE, IMAGE_SIZE, 3)
        with torch.no_grad():
            box, score = model(torch.from_numpy(rgb.copy()).float().div(255).unsqueeze(0))
        host = decode(box[0].numpy(), score[0, 0].numpy(), 0.30)
        name = Path(names[i]).name if i < len(names) else f"#{i}"
        rec = board.get(ts)
        if rec is None:
            print(f"{name:24s} board: no result   host: {len(host)} cat(s)")
            continue
        count = rec[1]
        boxes = [rec[4 + 5 * k : 9 + 5 * k] for k in range(count)]
        b = "  ".join(f"{s:.2f}@({x1:.0f},{y1:.0f},{x2:.0f},{y2:.0f})" for x1, y1, x2, y2, s in boxes)
        h = "  ".join(f"{d[4]:.2f}@({d[0]:.0f},{d[1]:.0f},{d[2]:.0f},{d[3]:.0f})" for d in host)
        print(f"{name:24s} board {count}: {b:40s} host {len(host)}: {h}  (NPU {rec[2]} us)")


def main() -> None:
    if len(sys.argv) >= 4 and sys.argv[1] == "make":
        make(Path(sys.argv[2]), [Path(p) for p in sys.argv[3:]])
    elif len(sys.argv) == 3 and sys.argv[1] == "check":
        check(Path(sys.argv[2]))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
