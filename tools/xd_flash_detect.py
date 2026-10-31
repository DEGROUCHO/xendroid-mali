#!/usr/bin/env python3
"""Find corrupted frames in a guest-output capture sequence.

Reads <evidence dir>/seq/guest-output-seq-<frame>.rgbx (raw RGBX, size from
xe-after.log [GUEST-OUTPUT-SEQ] lines) and reports:
  - flash frames: a frame far from both neighbours while the neighbours are
    close to each other (one-frame colour flashes);
  - black specks: isolated near-black 4x4 blocks whose surroundings are bright
    in this frame but not black in the neighbouring frames (stale cache lines).
"""
import re
import sys
from pathlib import Path

import numpy as np


def load(evidence):
    evidence = Path(evidence)
    log = (evidence / "xe-after.log").read_text(errors="replace")
    sizes = {}
    for m in re.finditer(r"\[GUEST-OUTPUT-SEQ\] status=ok frame=(\d+) width=(\d+) height=(\d+)", log):
        sizes[int(m.group(1))] = (int(m.group(2)), int(m.group(3)))
    frames = []
    for path in sorted((evidence / "seq").glob("guest-output-seq-*.rgbx"),
                       key=lambda p: int(re.search(r"(\d+)", p.stem.split("-")[-1]).group(1))):
        n = int(path.stem.split("-")[-1])
        if n not in sizes:
            continue
        w, h = sizes[n]
        data = np.fromfile(path, dtype=np.uint8)
        if data.size != w * h * 4:
            continue
        frames.append((n, data.reshape(h, w, 4)[:, :, :3].astype(np.float32)))
    return frames


def luma(img):
    return img[:, :, 0] * 0.299 + img[:, :, 1] * 0.587 + img[:, :, 2] * 0.114


def block_mean(y, b=4):
    h, w = (y.shape[0] // b) * b, (y.shape[1] // b) * b
    return y[:h, :w].reshape(h // b, b, w // b, b).mean(axis=(1, 3))


def main():
    frames = load(sys.argv[1])
    if len(frames) < 3:
        print(f"seq: {len(frames)} frames - not enough to check")
        return 0
    small = [block_mean(luma(f), 8) for _, f in frames]
    diffs = [float(np.abs(small[i + 1] - small[i]).mean()) for i in range(len(small) - 1)]
    med = float(np.median(diffs)) or 1.0
    flashes = []
    for i in range(1, len(small) - 1):
        a, b = diffs[i - 1], diffs[i]
        skip = float(np.abs(small[i + 1] - small[i - 1]).mean())
        if a > 4 * med and b > 4 * med and skip < 2 * med:
            flashes.append(frames[i][0])
    specks_per_frame = []
    blocks = [block_mean(luma(f), 4) for _, f in frames]
    for i in range(1, len(blocks) - 1):
        cur = blocks[i]
        pad = np.pad(cur, 1, mode="edge")
        neigh = (pad[:-2, 1:-1] + pad[2:, 1:-1] + pad[1:-1, :-2] + pad[1:-1, 2:]) / 4
        dark = (cur < 12) & (neigh > 70)
        steady = (blocks[i - 1] > 40) & (blocks[i + 1] > 40)
        specks_per_frame.append(int((dark & steady).sum()))
    print(f"seq: {len(frames)} frames {frames[0][0]}..{frames[-1][0]}, "
          f"median frame diff {med:.2f}")
    print(f"flash frames: {flashes if flashes else 'none'}")
    print(f"black specks per frame: max {max(specks_per_frame)}, "
          f"frames with specks {sum(1 for s in specks_per_frame if s)}/{len(specks_per_frame)}")
    return 1 if flashes or max(specks_per_frame) > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
