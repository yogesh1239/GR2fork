#!/usr/bin/env python3
"""Measures the FSR 4.1.1 still-camera test (Home key) from the test copy's log and screenshots.

Usage: still_test.py <shad_log.txt> <out_dir>
Prints, per mode, the flicker (per-pixel standard deviation over the 8 consecutive frames) and
the edge strength, on the pixels that are edges and that do not move in the no-jitter mode. Writes
mean images and crop sheets (nearest 4x) to out_dir.
"""
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image

MODES = ["FSR, jitter", "FSR, jitter sign reversed", "FSR, no jitter", "game AA"]


def load(path):
    rgb = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
    return rgb, rgb @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def gradient(luma):
    gx = np.zeros_like(luma)
    gy = np.zeros_like(luma)
    gx[:, 1:-1] = luma[:, 2:] - luma[:, :-2]
    gy[1:-1, :] = luma[2:, :] - luma[:-2, :]
    return np.hypot(gx, gy)


def main():
    log, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    shots, saved, started = [], [], False
    for line in log.read_text(errors="replace").splitlines():
        m = re.search(r"FSR 4\.1\.1 test: mode (\d) shot (\d)", line)
        if m and m[0].endswith("mode 0 shot 0"):
            shots, saved = [], []  # the last run counts
        if m:
            shots.append((int(m[1]), int(m[2])))
            started = True
        m = re.search(r"Saved screenshot: (.*\.png)", line)
        if m and started:
            saved.append(Path(m[1]))
    assert len(shots) == 32 and len(saved) >= 32, (len(shots), len(saved))
    frames = {mode: [] for mode in range(4)}
    for (mode, _), path in zip(shots, saved[:32]):
        frames[mode].append(load(path))

    lumas = {m: np.stack([f[1] for f in frames[m]]) for m in frames}
    for m in frames:
        same = sum(np.array_equal(lumas[m][i], lumas[m][i + 1]) for i in range(7))
        if same:
            print(f"warning: mode {m} has {same} identical neighbour frames")
    # Edges from the no-jitter FSR mean; "still" = pixels that barely change in that mode.
    ref = lumas[2].mean(0)
    edge = gradient(ref) > 0.08
    still = lumas[2].std(0) < 0.01
    mask = edge & still
    print(f"{mask.sum()} static edge pixels of {mask.size}")
    print(f"{'mode':28} {'flicker mean':>12} {'flicker p95':>11} {'edge strength':>13}")
    for m in frames:
        std = lumas[m].std(0)[mask]
        g = gradient(lumas[m].mean(0))[mask]
        print(f"{MODES[m]:28} {std.mean():12.5f} {np.percentile(std, 95):11.5f} {g.mean():13.5f}")
        Image.fromarray((np.stack([f[0] for f in frames[m]]).mean(0) * 255).astype(np.uint8)).save(
            out / f"mean_mode{m}.png")
    # Crop sheet: the 4 windows with the most static edge pixels, one column per mode, frame 0.
    h, w = mask.shape
    cw, ch = 200, 120
    density = [(mask[y:y + ch, x:x + cw].sum(), x, y)
               for y in range(0, h - ch, ch) for x in range(0, w - cw, cw)]
    picks = sorted(density, reverse=True)[:4]
    rows = []
    for _, x, y in picks:
        rows.append(np.concatenate([frames[m][0][0][y:y + ch, x:x + cw] for m in range(4)], 1))
    sheet = (np.concatenate(rows, 0) * 255).astype(np.uint8)
    Image.fromarray(sheet).resize((sheet.shape[1] * 3, sheet.shape[0] * 3), Image.NEAREST).save(
        out / "crops.png")
    print("crops (x, y):", [(x, y) for _, x, y in picks], "columns:", MODES)


if __name__ == "__main__":
    main()
