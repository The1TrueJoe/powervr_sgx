# First full dEQP-GLES3 run

2026-11-26, a CE5300 board (rev 1014). dEQP-GLES3 (CTS 3.2.14.0 mustpass, 44,768 cases) in an
OpenGL ES 3.0 context, on dev with the texture arrays (#61), transform feedback (merged by hand,
`SGX_DEBUG=xfb`), the word-texture mipmaps (#62), the LOD control (#63) and the ES 3.0 switch (#64):
34,052 pass, 8,745 fail, 1,555 skip, 252 warn, 157 flake, 7 timeouts. No crashes or GPU hangs in
the 7 h run. The failing groups are listed in `docs/GL3.md` "The ES 3.0 switch".
