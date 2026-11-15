# An XRGB window copied into a texture got alpha 0 (Minecraft's title-screen panorama)

2026-11-15, a CE5300 board, RAM boot of an m2-drm kernel with the open microkernel; Xorg
(modesetting on simpledrm, glamor through kmsro) in a Debian root.

Minecraft 1.7.10's title screen on 0.2.1 shows its panorama only in the bottom-left 256x256: the
game draws the panorama cube into a 256x256 viewport of its XRGB window, copies it into a 256x256
RGBA texture with `glCopyTexSubImage2D` and draws that texture back blended (`SRC_ALPHA`) to blur
it and then stretched over the screen. Its `SGX_DEBUG=draw` log shows each copy as
`GPU blit b8g8r8x8_unorm 720x480 (0,480 256x-256) -> b8g8r8a8_unorm 256x256`.

**Cause:** the driver samples B8G8R8X8 and R8G8B8X8 as their alpha twins, so the X byte comes back as
alpha. The state tracker's sampler views map alpha to 1 in their swizzle; u_blitter's source views
don't. 0.2.1 draws blits into textures on the GPU (09fda556), so the copy put the window's X byte,
0, into the texture's alpha, and every blended pass with it drew nothing. 0.2.0 copied through the
CPU, converting the format.

`sgx_gamefix copyx` (green with alpha 0 written, copied into an RGBA texture, drawn blended over
red) with `SGX_GAMEFIX_RGBX=1`, an RGBX default framebuffer as a 24-bit window is:

| source | released 0.2.1 | the fix |
|---|---|---|
| the RGBX default framebuffer | red, 4096 of 4096 wrong (FAIL) | PASS |
| an RGB FBO | PASS | PASS |
| an RGBA default framebuffer (red is right) | PASS | PASS |

Every other `sgx_gamefix` part passes with the fix too (bigfbo left out: its 2048x1200 frames hit
the large-target parameter-buffer loss, 2026-11-15-1630). No job timeouts, page faults, WARNs or
BUGs.
