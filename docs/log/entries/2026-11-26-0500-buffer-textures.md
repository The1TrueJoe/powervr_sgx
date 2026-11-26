# Buffer textures (ARB_texture_buffer_object) on hardware

2026-11-26, a CE5300 board (SGX545 rev 1014, the 7.3.3 kernel it boots, open microkernel
v0.2.1), our Mesa through glvnd, surfaceless EGL, desktop GL at 3.1 / GLSL 1.40 by override,
`SGX_DEBUG=tbo,glsl130,vtf`.

**What it is:** GL 3.1's buffer textures, behind `SGX_DEBUG=tbo`.
- `texelFetch` on a `samplerBuffer` becomes `ld.a.d` from the buffer's vertex-heap copy. The
  draw puts the copy's address in a driver constant (`SGX_K_TBO` + unit).
- The program unpacks the texel for the view's format and swizzles it (`sgx_program.c`
  `lower_tbo`). No sampler state is involved.
- A texel past the end reads 0 and loads texel 0.

**Hardware fact:** a fragment or vertex program's `ld.a.d` reads a buffer of the client's VM
at an address it computes in a temporary (ISA.md "Address").

**Tests:**
- `mesa/tests/sgx_tbo.c` passes:
  - 34 formats, bit for bit;
  - texels past the end;
  - `textureSize`;
  - `glTexBufferRange` at an offset of 16 texels;
  - a `glBufferSubData` between two draws of one frame;
  - a fetch in a vertex program.
- piglit's buffer-texture lists: 165 pass, against 2 on dev.
- dEQP-GLES2 with the default switches is the same as dev: 16,636 pass, 70 expected fails.

**Found on the way:** `arb_texture_buffer_range-ranges` failed where an `if (pos < size)`
around the fetch stayed a branch. Pixels whose condition was false got the fetch's result when
they shared a block of pixels with pixels whose condition was true (ISA.md "Branches",
mechanism unconfirmed). The load is now marked speculatable, so the `if` is flattened into a
select. Any other load or sample the compiler keeps behind a divergent branch may have the same
problem.
