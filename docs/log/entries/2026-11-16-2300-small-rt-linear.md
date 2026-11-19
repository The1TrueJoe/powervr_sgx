# Small non-square render targets landed scrambled: Xfce's Mint-Y window borders (issue #13)

2026-11-16, a CE5300 board. Issue #13 (an MSI U180, Cedarview): with Xfce's window manager and the
Mint-Y theme, every window had a dashed line along its bottom edge. The Default theme drew it
cleanly; turning off the compositor didn't help.

**Reproduced** on the board under the Debian root's Xorg (modesetting, glamor on our GPU),
v0.2 kernel and the packaged mesa-sgx 0.2.1, with xfwm4 and Mint-Y's xfwm4 theme files (linuxmint/
mint-themes `src/Mint-Y/xfwm4`). Mint-Y builds its frame from small RGBA PNG pieces: the bottom is
a 16x2 tile, the sides 2x36. In the screenshot, both rows of the bottom border had black pixels at
the same 10 of every 16 columns (offsets 2-3, 6-7, 9-11, 13-15), and Default's XPM frame was clean.
With `AccelMethod "none"` the Mint-Y border was clean too, so the cause is our GPU path.

**In GL**, `sgx_gamefix rtsmall` renders each texel's own coordinates into a small texture, then
tiles it over a 64x64 target with `GL_REPEAT`:

| texture | dev, before | the fix |
|---|---|---|
| 16x2 | FAIL, 3584 of 4096 | PASS |
| 2x16 | FAIL, 3072 of 4096 | PASS |
| 16x4 | FAIL, 3072 of 4096 | PASS |
| 8x8, 4x4, 16x16 | PASS | PASS |

**Cause:** the sampler's twiddled tiles are min(w, h) square (TEX.md 1), but a render writes a
twiddled destination in tiles 16 high. So a render into a non-square power-of-two texture with a
side under 16 puts texels where the sampler doesn't look. glamor keeps X pixmaps as textures and
draws into them, so xfwm4's 16x2 border piece was rendered scrambled, then tiled.

**Fix:** a 2D texture that may be rendered into (`PIPE_BIND_RENDER_TARGET`), with a side under 16 and
more than one row, is laid out linear, as an NPOT texture is (`sgx_tex_twiddled`). Linear sampling
wraps correctly with `GL_REPEAT` at these sizes. Every other `sgx_gamefix` part still passes, with no
timeouts or faults. Under the root's Xorg with dev's kernel and this Mesa ("glamor X acceleration
enabled on PowerVR SGX545"), xfwm4's Mint-Y borders are clean: 0 black pixels on the bottom rows,
against the pattern above on 0.2.1. Not covered: a large render target's small mip levels, which stay twiddled (a
render into one would have the same problem; nothing seen renders into them).
