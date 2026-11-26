# Lines without discard

2026-11-26, a CE5300 board (SGX545 rev 1014, open microkernel 0.2.1), our Mesa in the Debian
root, surfaceless EGL, desktop GL 2.1. Issue #71 (reported on #5): every cast of Minecraft
1.8.9's fishing rod timed the 3D out, with each frame reset.

**Cause:** the fishing line is a 17-vertex line strip drawn in the entity pass, with alpha test
on (GREATER 0.1). The screen has no alpha test of its own, so st lowers it to a discard. A line
drawn by a discarding program locks the 3D core up, also when the discard is never taken.
That matches what the vendor driver showed: "Lines with any discarding shader render a broken
frame" (ISA.md, "Phases, visibility feedback and discard"). The block outline, the other line
Minecraft draws, turns alpha test off first, so it never hung.

**Change:** for a line draw whose fragment program discards, the gallium driver uses a variant
of the program without its discards (`drop_discards` in sgx_program.c; a `no_discard` variant
next to the sRGB ones). Every pixel of the line is kept. That is exact for the fishing line,
whose alpha is 255. gl_FragDepth on lines stays: it renders correctly (pass type 5, depthf).

**Tests:**
- `mesa/tests/sgx_lines.c`, 18 cases, each in its own process.
  - On dev (93484698), `alpha`, `alphalist` and `discard` time out on every frame. `fragdepth`
    and the line cases without a discard (near-plane crossings, w = 0, lines behind the eye,
    degenerate and off-screen lines) render.
  - With this change, all 18 pass with no timeouts, default and `SGX_DEBUG=glsl130`. The three
    discard cases draw the same 885 line pixels as `fish`.
- `meson test --suite sgx`: 5 of 5.
- piglit: 566 tests (the GL 1.x and 2.x tests, and the line, alpha-test, discard and
  frag-depth ones), dev against this change, default and glsl130. Nothing goes from pass to
  fail. `rasterpos` and `streaming-texture-leak` go from fail or timeout to pass, which is
  unrelated to this change.

**Open:** a run that ends in many lockups in a row can still lose the device, when the restart
after a reset finds a late parameter-buffer fault from the TA. That is a separate recovery
weakness of the kernel driver, which this change doesn't touch.
