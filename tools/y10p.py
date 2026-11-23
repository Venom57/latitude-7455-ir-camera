#!/usr/bin/env python3
"""Convert the brightest frame of a raw MIPI Y10P capture to an 8-bit PGM.

With the IR emitter pulsing (two frames lit, two dark) the last frame is
often a dark one, so the frame with the highest mean is kept.

y10p.py RAW WIDTH HEIGHT STRIDE OUT.pgm

Y10P packs 4 pixels in 5 bytes: bytes 0-3 hold the high 8 bits of each pixel,
byte 4 the low 2 bits. The PGM keeps the high 8 bits, stretched so the
darkest pixel is black and the brightest white (the stats line gives the raw
range). Also prints how many frames the file holds.
"""
import sys


def main():
    raw, w, h, stride, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
    data = open(raw, 'rb').read()
    frame_size = stride * h
    frames = len(data) // frame_size
    if not frames:
        sys.exit(f'{raw}: {len(data)} bytes, less than one {frame_size}-byte frame')
    def mean(i):  # sampled high bytes, cheap
        f = data[i * frame_size:(i + 1) * frame_size]
        return sum(f[y * stride + x] for y in range(0, h, 4) for x in range(0, w * 5 // 4, 5))
    best = max(range(frames), key=mean)
    frame = data[best * frame_size:(best + 1) * frame_size]

    px = bytearray(w * h)
    for y in range(h):
        row = frame[y * stride:y * stride + w * 5 // 4]
        o = y * w
        for x in range(w):
            px[o + x] = row[x // 4 * 5 + x % 4]

    lo, hi = min(px), max(px)
    mean = sum(px) / len(px)
    print(f'{frames} frames; brightest is frame {best} (high 8 bits): min {lo} max {hi} mean {mean:.1f}')
    span = max(hi - lo, 1)
    img = bytes((p - lo) * 255 // span for p in px)
    with open(out, 'wb') as f:
        f.write(b'P5\n%d %d\n255\n' % (w, h))
        f.write(img)


if __name__ == '__main__':
    main()
