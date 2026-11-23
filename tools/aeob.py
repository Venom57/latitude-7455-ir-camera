#!/usr/bin/env python3
"""Decode Qualcomm "AeoB" camera resource files (CAMx_RES_*.bin, SCFG_*.bin).

Layout, worked out from the Latitude 7455 driver pack:
  "AeoB", u32 total size, u32 count, then elements.
  Each element is u16 type, u16 length, then `length` bytes:
    type 0  integer   (8 bytes, little-endian)
    type 1  string    (NUL-terminated)
    type 3  package   (nested elements)
Usage: aeob.py FILE [--flat]
"""
import struct
import sys


def parse(buf, off, end):
    items = []
    while off + 4 <= end:
        typ, ln = struct.unpack_from("<HH", buf, off)
        off += 4
        body = buf[off:off + ln]
        if typ == 0:
            items.append(int.from_bytes(body, "little"))
        elif typ == 1:
            items.append(body.rstrip(b"\0").decode("ascii", "replace"))
        elif typ == 3:
            items.append(parse(buf, off, off + ln))
        else:
            items.append(("?", typ, body.hex()))
        off += ln
    return items


def show(items, depth=0):
    pad = "  " * depth
    # A package whose first item is a string is printed as NAME(args...).
    if items and isinstance(items[0], str) and all(not isinstance(x, list) for x in items[1:]):
        args = ", ".join(fmt(x) for x in items[1:])
        print(f"{pad}{items[0]}({args})")
        return
    for x in items:
        if isinstance(x, list):
            if x and isinstance(x[0], str) and any(isinstance(y, list) for y in x[1:]):
                head = [fmt(y) for y in x[1:] if not isinstance(y, list)]
                print(f"{pad}{x[0]}" + (f" [{', '.join(head)}]" if head else ""))
                show([y for y in x[1:] if isinstance(y, list)], depth + 1)
            else:
                show(x, depth)
        else:
            print(f"{pad}{fmt(x)}")


def fmt(x):
    if isinstance(x, int):
        return f"{x} (0x{x:x})" if x > 9 else str(x)
    if isinstance(x, str):
        return repr(x)
    return str(x)


def main():
    buf = open(sys.argv[1], "rb").read()
    if buf[:4] != b"AeoB":
        sys.exit("not an AeoB file")
    size, count = struct.unpack_from("<II", buf, 4)
    print(f"# {sys.argv[1]}: size {size}, top-level count {count}")
    show(parse(buf, 12, len(buf)))


if __name__ == "__main__":
    main()
