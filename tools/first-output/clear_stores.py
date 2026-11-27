#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""A clear made of USSE stores, for a core whose PBE path doesn't work yet.

    clear_stores.py [--unrolled] ADDR COLOUR W H PITCH_PIXELS > clear.s
    sgx-asm -L clear.s | awk '{print $2, $3}' > clear.hex
    powervr_sgx_blit -c -w W -S @clear.hex

The background object's pixel program runs this shader, and every instance
stores COLOUR into the W x H block at ADDR (32-bit pixels) with st.a.d. Every
instance writes the whole block: slow but idempotent.

A full-width block (PITCH = W) is one loop with a `(p0) br` back-edge, padded
with nops so that powervr_sgx_blit gives it a code object of its own: on the
SGX530 a taken branch hangs a program placed inline after the context's other
programs. It uses r0-r3 only: on the SGX530 a pixel task launched this way has
no more temporaries. Any other block (or --unrolled) is one store per pixel (the
store offset is 7 bits, so the pointer moves on once a row) and must fit one code
base window (32 KiB on an SGX530: about 32 x 32 pixels).
See docs/porting/first-output.md.
"""
import sys

unrolled = sys.argv[1:2] == ["--unrolled"]
addr, colour, w, h, pitch = (int(a, 0) for a in sys.argv[1 + unrolled:6 + unrolled])
if not unrolled and pitch == w:
    if not 0 < w * h <= 0xffff:
        sys.exit("W x H must be 1-65535 for the loop's 16-bit counter")
    print("phas.i #0, #0 [wait=7]")
    print(f"limm r0, #{addr:#x}")
    print(f"limm r1, #{colour:#x}")
    print(f"limm r2, #{w * h}")
    print("limm r3, #4")
    print("st.a.d r0, #0, r1 [moe]")             # each pixel
    print("imae r0, r3, #1, r0 [t2=u32]")        # r0 += 4 (the 32-bit addend)
    print("test.isubu16.ne p0, r2, r2, #1")
    print("(p0) br #-3")                         # relative to the branch, in instructions
    print("idf")
    print("wdf")
    print("nop [end]")
    for _ in range(16):
        print("nop")
    sys.exit(0)
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
