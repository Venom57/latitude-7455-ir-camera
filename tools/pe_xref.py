#!/usr/bin/env python3
"""Find code that references addresses in an ARM64 PE (objdump -d output).

pe_xref.py DISASSEMBLY ADDR_HEX...   prints instructions that build ADDR via
adrp+add (or adr), with their address, for each ADDR.
"""
import re, sys

dis, targets = sys.argv[1], {int(a, 16) for a in sys.argv[2:]}
adrp = {}
ins_re = re.compile(r'^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)\s+(.*)$')
for line in open(dis):
    m = ins_re.match(line)
    if not m:
        continue
    pc, op, args = int(m.group(1), 16), m.group(2), m.group(3)
    if op == 'adrp':
        m2 = re.match(r'(x\d+), (?:0x)?([0-9a-f]+)', args)
        if m2:
            adrp[m2.group(1)] = int(m2.group(2), 16)
    elif op == 'adr':
        m2 = re.match(r'(x\d+), (?:0x)?([0-9a-f]+)', args)
        if m2 and int(m2.group(2), 16) in targets:
            print(f'{int(m2.group(2),16):x} <- {pc:x} adr')
    elif op == 'add':
        mm = re.match(r'(x\d+), (x\d+), #0x([0-9a-f]+)$', args)
        if mm and mm.group(2) in adrp:
            a = adrp[mm.group(2)] + int(mm.group(3), 16)
            if a in targets:
                print(f'{a:x} <- {pc:x}')
