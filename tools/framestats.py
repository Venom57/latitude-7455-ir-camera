#!/usr/bin/env python3
"""Per-frame brightness of a raw Y10P capture.

framestats.py RAW WIDTH HEIGHT STRIDE
Prints "max-frame mean M (frame i), max pixel P, means: m0 m1 ..." using the
high 8 bits of every 4th pixel on every 2nd row (fast, plenty for a mean).
"""
import sys

raw, w, h, stride = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
d = open(raw, 'rb').read()
fs = stride * h
means, peak = [], 0
for i in range(len(d) // fs):
    f = d[i * fs:(i + 1) * fs]
    px = [f[y * stride + x // 4 * 5] for y in range(0, h, 2) for x in range(0, w, 4)]
    means.append(sum(px) / len(px))
    peak = max(peak, max(px))
if not means:
    sys.exit('no complete frames')
best = max(range(len(means)), key=means.__getitem__)
print(f'max-frame mean {means[best]:.1f} (frame {best}), max pixel {peak}, means: '
      + ' '.join(f'{m:.1f}' for m in means))
