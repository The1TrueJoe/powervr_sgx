#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""A clear made of USSE stores, for a core whose PBE path doesn't work yet.

    clear_stores.py ADDR COLOUR W H PITCH_PIXELS > clear.s
    sgx-asm -L clear.s | awk '{print $2, $3}' > clear.hex
    powervr_sgx_blit -c -w W -S @clear.hex

The background object's pixel program runs this shader, and every instance
stores COLOUR into the W x H block at ADDR (32-bit pixels) with st.a.d. The store
offset is 7 bits (up to 127 dwords), so the pointer moves on once a row. Every
instance writes the whole block: slow but idempotent. The program must fit one
code base window (32 KiB on an SGX530, so at most about 32 x 32 pixels).
See docs/porting/first-output.md.
"""
import sys

addr, colour, w, h, pitch = (int(a, 0) for a in sys.argv[1:6])
if w > 128:
    sys.exit("a row wider than 128 pixels needs more than one pointer per row")
print("phas.i #0, #0 [wait=7]")
print(f"limm r0, #{addr:#x}")
print(f"limm r1, #{colour:#x}")
print(f"limm r3, #{pitch * 4}")
for _ in range(h):
    for x in range(w):
        print(f"st.a.d r0, #{x}, r1 [moe]")
    print("imae r0, r3, #1, r0 [t2=u32]")
print("idf")
print("wdf")
print("nop [end]")
