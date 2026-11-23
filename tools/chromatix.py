#!/usr/bin/env python3
"""Read Qualcomm "QTI Chromatix" sensor module files (com.qti.sensormodule.*.bin).

Layout, worked out from the Latitude 7455 driver pack and checked against the
register list in Linux's ov02e10 driver (15 of 16 writes match; Windows writes
0x34 to 0xc5 where Linux writes 0x04):

  0x00  "QTI Chromatix Header", padded; u32 at 0x1c is the end of the data
  then the field table, 56-byte blocks starting at the first 0xffffffff:
          +0   u32 0xffffffff
          +4   u32 pool offset   } of the field named in the PREVIOUS block
          +8   u32 size          }
          +12  u32 tag (field index)
          +16  char name[40]
  the pool starts right after the last block's size word (block + 12).
  A scalar field's value is at pool + offset. A structure field's value is a
  run of tagged words inside the pool: [1][tag][value]... where each tag is a
  field index from the table.

Usage:
  chromatix.py FILE fields        every field: tag, name, pool offset, size, value
  chromatix.py FILE regs          every register write: slave, address, data, delay
"""
import struct
import sys

BLOCK = 56


def load(path):
    b = open(path, "rb").read()
    first = b.find(b"\xff\xff\xff\xff", 0x58)
    blocks = []
    p = first
    while p + BLOCK <= len(b) and b[p:p + 4] == b"\xff\xff\xff\xff":
        off, size, tag = struct.unpack_from("<III", b, p + 4)
        name = b[p + 16:p + 56].split(b"\0")[0].decode("ascii", "replace")
        blocks.append((p, off, size, tag, name))
        p += BLOCK
    # The last block holds only the previous field's offset and size.
    pool = blocks[-1][0] + 12
    fields = {}
    for i in range(len(blocks) - 1):
        _, _, _, tag, name = blocks[i]
        _, off, size, _, _ = blocks[i + 1]
        fields[tag] = (name, off, size)
    return b, pool, fields


def value(b, pool, off, size):
    if size == 0:
        return 0
    return int.from_bytes(b[pool + off:pool + off + min(size, 8)], "little")


def register_records(b, start, end):
    """Register-write records: words 1, A, 0, A+1, addr, 1, A+2, ..."""
    o = start
    while o + 40 <= end:
        w = struct.unpack_from("<10I", b, o)
        if w[0] == 1 and w[2] == 0 and w[3] == w[1] + 1 and w[6] == w[1] + 2 and w[5] == 1:
            yield o, w
            o += 40
        else:
            o += 1


def main():
    path, what = sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "fields"
    b, pool, fields = load(path)
    if what == "fields":
        print(f"# pool at 0x{pool:x}, {len(fields)} fields")
        for tag in sorted(fields):
            name, off, size = fields[tag]
            v = value(b, pool, off, size) if 0 < size <= 8 else ""
            vs = f"0x{v:x}" if v != "" else f"[{size} bytes]" if size else ""
            print(f"{tag:5d} {name:40s} off 0x{off:05x} size {size:5d} {vs}")
    elif what == "regs":
        names = lambda t: fields.get(t, ("?",))[0]
        print(f"# pool at 0x{pool:x}")
        for o, w in register_records(b, pool, len(b)):
            slave = value(b, pool, *fields[w[1]][1:]) if w[1] in fields else None
            data = value(b, pool, *fields[w[6]][1:]) if w[6] in fields else None
            nxt = struct.unpack_from("<4I", b, o + 40)
            print(f"0x{o:05x} tags {w[1]}/{w[3]}/{w[6]} ({names(w[1])}/{names(w[3])}/{names(w[6])}) "
                  f"slave {slave if slave is None else hex(slave)} addr 0x{w[4]:04x} data "
                  f"{data if data is None else hex(data)} tail {w[7]},{w[8]},{w[9]}")


if __name__ == "__main__":
    main()
