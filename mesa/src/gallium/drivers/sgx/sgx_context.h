/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#ifndef SGX_CONTEXT_H
#define SGX_CONTEXT_H

#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "util/list.h"
#include "util/slab.h"
#include "util/u_blitter.h"

#include "sgx_compiler.h"
#include "sgxws.h"

struct sgx_resource;
struct nir_shader;
struct sgx_varyings;

/* what a shader turned out to be (sgx_program.c) */
enum sgx_shader_kind {
   SGX_SHADER_UNSUPPORTED,
   SGX_SHADER_COMPILED,    /* by mesa/src/sgx/compiler */
   SGX_SHADER_VS_POSITION, /* gl_Position = attribute 0 */
   SGX_SHADER_FS_UNIFORM,  /* gl_FragColor = a vec4 uniform */
   SGX_SHADER_FS_CONSTANT, /* gl_FragColor = a constant */
};

/* a constant in secondary attributes: sa[base + slot] = constant buffer 0 float
 * k */
struct sgx_const {
   uint16_t k, slot, fmt,
      shift;     /* fmt: 1 f32, 2 F16 half at shift, 3 C10 channel at shift,
                  * 0x10 F16 half of the C10 value, 0x11 the literal `value`;
                  * slot 0xffff: in the memory block only */
   uint16_t mem; /* word of the constant memory block, 0 none */
   uint32_t value;
};

#define SGX_MAX_CONSTS 256 /* SA dwords of constants (sa127 ends them) */
/* fragment constants the driver fills: texture unit u's level-0 width and
 * height at SGX_K_TEXSIZE + 2u (st's textureSize, as its rectangle lookups
 * scale by it); past every uniform (128 vec4) and under the compiler's 1024 */
#define SGX_K_TEXSIZE       1000
#define SGX_K_TEXSIZE_UNITS 8
/* the vertex constant the draw fills with gl_InstanceID, as a float
 * (sgx_program.c lower_instance_id) */
#define SGX_K_INSTANCE 1016
/* The driver's block of constant numbers for programs compiled with the
 * compiler's native integers (docs/GL3.md, "Crossing the driver/compiler
 * line"): unit u's base-level width, height and depth at SGX_K_SIZE + 3u,
 * gl_InstanceID at SGX_K_INSTANCE_INT, all raw integers; uniform block n's
 * GPU address at SGX_K_UBO + n */
#define SGX_K_SIZE         1024
#define SGX_K_SIZE_UNITS   16
#define SGX_K_INSTANCE_INT 1072
#define SGX_K_UBO          1080
#define SGX_UBO_BYTES      65536 /* a block's size, GL_MAX_UNIFORM_BLOCK_SIZE */
#define SGX_MAX_UBOS       12 /* load_ubo's blocks 0-11: constant buffers 1-12 */
/* unit u's level count (textureQueryLevels, and the compiler's own level
 * choice for word-by-word fetches) at SGX_K_LEVELS + u, a raw integer */
#define SGX_K_LEVELS       1096

/* a fragment program's dependent samples, and their texture state blocks
 * (the compiler's sgx_sample_info.state: one per way a unit is read, 4 SA
 * dwords each, so no more than SA holds) */
#define SGX_MAX_DEP_SAMPLES SGX_MAX_SAMPLES
#define SGX_MAX_DEP_STATES 32

/* the fragment driver block's register that sets gl_FrontFacing's sense:
 * bit 0 clear when gallium's front_ccw is set (A/B against the DDK) */
#define SGX_SA_FRONT 5

struct sgx_pipe_shader {
   struct nir_shader *nir;
   enum sgx_shader_kind kind;
   unsigned
      ubo_offset; /* FS_UNIFORM: byte offset of the vec4 in constant buffer 0 */
   float color[4]; /* FS_CONSTANT */
   /* COMPILED: */
   struct sgxws_prog prog;
   unsigned n_consts, const_dwords;
   struct sgx_const consts[SGX_MAX_CONST_MAP];
   /* constants past sa127 (the compiler's ld.a.d): a block of mem_dwords per
    * draw, its address + 512 * j in SA slot mem_base_slot[j] */
   unsigned mem_dwords, n_mem_bases;
   uint8_t mem_base_slot[SGX_MAX_MEM_BASES];
   unsigned n_attribs; /* vertex: attribute slots, 4 dwords each */
   /* vertex: gl_VertexID's attribute slot + 1 (0: not read), an input the
    * draw gathers itself (sgx_program.c lower_vertex_id), as a raw integer
    * under native integers, otherwise a float */
   unsigned vertex_id_slot;
   bool vertex_id_int;
   /* fragment: the shader's code, kept for the blend variants (the colour
    * epilogue is the blend, sgxws_blend) */
   uint64_t *code; /* with the dependent samples' smp put in */
   unsigned n_code, color_reg, temps;
   /* split around its discard (unless SGX_DEBUG=nophasesplit): code is phase 0
    * (the test), code1 phase 1 (the colour, into pa<color_reg>) */
   uint64_t *code1;
   unsigned n_code1;
   int discard_pa; /* discards: the depth plane's first PA register; -1 doesn't */
   /* writes gl_FragDepth: in pa<depth_pa> at the feedback nop for a depthf;
    * -1 doesn't */
   int depth_pa;
   /* dependent samples' texture state blocks (sgx_sample_info.state; the
    * samples that read a unit the same way share one): the SA register of
    * each block's 4 texture state words, and its sampler */
   unsigned dep_sa[SGX_MAX_DEP_STATES], dep_sampler[SGX_MAX_DEP_STATES],
      n_deps;
   /* the texel comes back F16 into two registers (T3 01028800) */
   bool dep_f16[SGX_MAX_DEP_STATES];
   /* its plane of an RG32 / RGBA32 texture (the state's address moves) */
   uint8_t dep_plane[SGX_MAX_DEP_STATES];
   /* a texelFetch (sgx_sample_info.fetch): sampled NEAREST, clamped */
   bool dep_fetch[SGX_MAX_DEP_STATES];
   struct {
      struct sgxws_blend key;
      uint32_t code;
   } variants[32];
   unsigned n_variants, next_variant;
   bool uses_front_face;      /* fragment: reads SGX_SA_FRONT */
   bool native_integers;      /* compiled with them: its driver constants in
                                 the SGX_K_SIZE block, raw integers */
   bool reads_pointcoord;     /* fragment: its varyings start at set 1,
                                 gl_PointCoord is set 0 */
   struct sgx_varyings *vary; /* compiled: the varyings' sets (a variant's: its
                                 fragment shader's) */
   /* vertex: the same compiled to the sets of a fragment shader whose
    * varyings differ from its own (sgx_vs_for) */
   struct sgx_pipe_shader *variants_vs[16];
   unsigned next_variant_vs;
   /* vertex: the input formats it unpacks (sgx_compile_options.vtx_fmt; all
    * 0, f32 x 4, unless SGX_DEBUG=zerocopy picked a variant) */
   uint8_t vtx_fmt[16];
   /* vertex: st's program writes gl_PointSize, and this variant keeps it as
    * an output (points draws; the others drop it, sgx_vs_for) */
   bool writes_psize, points;
   /* fragment: the same decoding what the samplers in its mask sample as
    * sRGB, and swizzling what each sampler view's swizzle says (formats st
    * stores in another, GL_R16 as RGBA8; 0: none) (sgx_fs_for) */
   unsigned srgb;
   uint32_t srgb_filter; /* of those, the ones it filters (lower_srgb_filter) */
   uint16_t swz[PIPE_MAX_SAMPLERS];
   uint32_t not_bgra; /* units bound to a texture not sampled B, G, R, A
                         ordered (sgx_tex_bgra) */
   uint32_t depth;    /* units bound to a depth texture (sgx_tex_depth) */
   uint32_t snorm;    /* units bound to an snorm texture, stored offset
                         binary (sgx_tex_snorm, lower_snorm_texel) */
   /* colour buffer 0 is an integer format (pack_int_outputs): 0 not, 1 one of
    * 8 bits a channel, 2 R32, 3 + c channel c's plane of RG32 / RGBA32 */
   uint8_t rt_int;
   /* MRT: 1 + the colour buffer this variant draws into, its DATA output
    * made output 0 (remap_outputs); 0 a single buffer */
   uint8_t rt_target;
   /* colour buffer 0 is sRGB (lower_srgb_output): 0 not, 1 the colour
    * encoded, 2 blended first, in linear, with the tile's colour decoded (as
    * rt_blend and rt_blend_color say; the compiler's rt_packed), the sop2
    * epilogue left out.  reads_tile: it does read the tile (pass type) */
   uint8_t rt_srgb;
   bool reads_tile;
   struct pipe_rt_blend_state rt_blend; /* colormask 0: the epilogue's */
   float rt_blend_color[4];             /* 0 unless a factor reads it */
   /* units bound to an RGBA32 / RG32 texture, read a plane at a time (the
    * compiler's tex_r32x4 / tex_r32x2) */
   uint32_t r32x4, r32x2;
   /* shadow samples: each unit's compare function (lower_shadow) */
   uint8_t compare[PIPE_MAX_SAMPLERS];
   /* fragment: the units an implicit-LOD sample reads dependently
    * (dep_lod_units, the base shader's), and of those the ones bound to a
    * mipmapped view and sampler: this variant's samples there take their
    * LOD from the coordinate's derivatives (lower_dep_lod) */
   uint32_t dep_lod_units, dep_mip;
   struct sgx_pipe_shader *variants_srgb[8];
   unsigned next_variant_srgb;
};

struct sgx_vertex_elements {
   unsigned num_elements;
   struct pipe_vertex_element elements[PIPE_MAX_ATTRIBS];
};

struct sgx_rasterizer {
   struct pipe_rasterizer_state base;
};

struct sgx_dsa {
   struct pipe_depth_stencil_alpha_state base;
};

/* a render target's blend in a draw's pass for it (sgx_blend_rt): each pass
 * has its own colour epilogue, so each target may blend and mask its own way
 * (indep_blend_enable, indep_blend_func: GL_EXT_draw_buffers2,
 * GL_ARB_draw_buffers_blend) */
#define SGX_BLEND_RTS 8

struct sgx_blend {
   struct pipe_blend_state base;
   bool enable[SGX_BLEND_RTS]; /* the target blends, in a way sop2 can */
   struct sgxws_blend hw[SGX_BLEND_RTS][2]; /* by whether it has alpha */
};

/* one render target's TA/3D structures, by size (sgxws_rt) */
#define SGX_MAX_RTS 8

struct vcache_chunk;

/* a sampler state, with an id no other one gets (sgx_view's cache) */
struct sgx_sampler {
   struct pipe_sampler_state base;
   uint32_t id;
};

/* a sampler view, with the texture state words (sgx_tex_words) the last
 * draw that sampled it made: the same while the sampler and the texture's
 * addresses are.  A draw made them again for every unit (Quake 3 switches
 * textures between nearly all of its draws, among a few hundred views). */
struct sgx_view {
   struct pipe_sampler_view base;
   uint32_t words_sampler, words_va, words_cube0_va;
   bool words_valid;
   uint32_t words[4];
};

struct sgx_context {
   struct pipe_context base;

   /* a GPU reset seen and not yet asked about, and st's callback (a context
    * made to lose itself on a reset has one) */
   enum pipe_reset_status reset;
   struct pipe_device_reset_callback reset_cb;

   struct sgxws_ctx *hw;
   struct sgxws_prog vs_xy, vs_xyz, fs_uniform;
   struct sgxws_rt *rts[SGX_MAX_RTS];
   uint32_t rt_va[SGX_MAX_RTS]; /* the target each was made for */
   uint32_t rt_w[SGX_MAX_RTS], rt_h[SGX_MAX_RTS]; /* and its size */
   uint8_t rt_samples[SGX_MAX_RTS];                /* and samples, 1 or 4 */
   /* the depth and stencil of a 4x target with no depth buffer bound: a 4x
    * render needs a ZLS buffer, one per sample (sgx_draw.c zls_target) */
   struct sgxws_bo ms_zls;
   uint32_t ms_zls_bytes;
   /* the colour buffer of a framebuffer with none (depth only): renders
    * need a target, so they write this one, which nothing reads.  Sized in
    * set_framebuffer_state to the framebuffer, with its depth buffer's
    * samples. */
   struct pipe_resource *dummy_cb;
   /* occlusion queries (SGX_DEBUG=occlusion, sgx_state.c): the one counting
    * (NULL when none is, or st has turned queries off around its own
    * draws), and the slots their draws store to, one a begin, taken in turn
    * (query_seq: the last frame each was drawn in; query_last: the last any
    * was, once query_pending) */
   struct sgx_query *occlusion;
   bool queries_off, query_pending;
   struct sgxws_bo query_bo;
   uint32_t query_next, *query_seq, query_last;
   uint64_t rt_used[SGX_MAX_RTS], rt_clock;       /* when each was last taken */
   unsigned num_rts;
   struct sgx_resource *recording; /* the target of the frame being recorded */
   struct sgx_resource *recording_zs; /* and its depth/stencil buffer */
   /* An RG32 or RGBA32 colour buffer is rendered a pass a channel into its
    * planes (sgx_tex_plane_stride): the plane the draw or clear is on, the
    * frame's plane, and whether an earlier pass of this draw already wrote
    * depth and stencil (a later one tests depth EQUAL, writes neither) */
   unsigned rt_plane, recording_plane;
   bool rt_later_pass;
   /* MRT, a pass a colour buffer: the one this pass draws or clears into */
   /* sgx_fs_for's and sgx_vs_for's last answers, good while the counter
    * for what each reads holds: fs_gen (fragment views and samplers, the
    * framebuffer, the blend state and colour), vs_gen (vertex views and
    * samplers); both go up when any variant ends.  A draw looked them up
    * again, texture key and all (Quake 3 and GoldSrc draw hundreds a frame;
    * the vertex shader's rarely changes between them). */
   uint32_t fs_gen, vs_gen;
   struct {
      uint32_t gen;
      const void *fs;
      unsigned rt_index, rt_plane;
      void *out;
   } fs_memo;
   struct {
      uint32_t gen;
      const void *vs, *fs;
      uint8_t vtx_fmt[16];
      bool points;
      void *out;
   } vs_memo;
   unsigned rt_index;
   /* one past the highest view bound to the fragment and vertex stages */
   unsigned n_views, n_vs_views;

   struct pipe_framebuffer_state framebuffer;
   struct pipe_viewport_state viewport;
   struct pipe_scissor_state scissor;
   struct pipe_blend_color blend_color;
   struct pipe_stencil_ref stencil_ref;
   struct sgx_rasterizer *rasterizer;
   struct sgx_dsa *dsa;
   struct sgx_blend *blend;
   struct sgx_vertex_elements *velems;
   struct sgx_pipe_shader *vs, *fs;
   struct pipe_vertex_buffer vertex_buffers[PIPE_MAX_ATTRIBS];
   unsigned num_vertex_buffers;
   struct pipe_constant_buffer constants[MESA_SHADER_STAGES];
   /* constant buffers 1-12, uniform blocks 0-11 (ubos[s][block]), and the
    * draw's copies of them (fill_consts) */
   struct pipe_constant_buffer ubos[MESA_SHADER_STAGES][SGX_MAX_UBOS];
   struct sgxws_ubo draw_ubos[SGXWS_MAX_UBOS];
   unsigned n_draw_ubos;
   struct pipe_sampler_state *samplers[PIPE_MAX_SAMPLERS]; /* fragment */
   struct pipe_sampler_view *views[PIPE_MAX_SHADER_SAMPLER_VIEWS]; /* fragment */
   /* the vertex stage's (SGX_DEBUG=vtf: sgx_vtf) */
   struct pipe_sampler_state *vs_samplers[PIPE_MAX_SAMPLERS];
   struct pipe_sampler_view *vs_views[PIPE_MAX_SHADER_SAMPLER_VIEWS];

   /* SGX_STATS=1: where the time goes, printed every 2 s */
   /* objects the frame being recorded has from clears (the stats tell a frame
    * of clears alone) */
   unsigned frame_clears;
   struct {
      bool on;
      uint64_t t0, cpu0, draw_ns, flush_ns, tex_ns, compile_ns, rb_ns, rb_bytes;
      unsigned frames, draws, verts, flushes, tex_writes, tex_flushes;
      unsigned fl_eof, fl_other, fl_switch, fl_read,
         fl_deferred;           /* flushes by cause */
      unsigned fl_switch_clear; /* of the switches, of a frame of clears only */
      unsigned vc_hits, vc_misses, vc_uncacheable, zc_draws;
      unsigned blit_gpu, blit_copy, blit_cpu;
      uint64_t blit_cpu_px, blit_cpu_ns;
   } stats;

   struct blitter_context
      *blitter; /* GPU blits: a quad through our own shaders */

   /* draws from unchanged buffers, their vertices gathered and indices
    * rebased once into device memory (sgx_draw.c) */
   struct hash_table *vcache;
   struct list_head vcache_lru; /* most recently used first */
   uint64_t vcache_bytes;
   struct list_head
      vcache_chunks; /* the device memory its entries are cut from */
   struct vcache_chunk *vcache_cur;

   struct slab_child_pool transfer_pool;
   uint32_t *staging; /* vertices gathered for one draw */
   size_t staging_size;
   uint16_t *idx_staging;   /* its 16-bit indices */
   uint32_t *mem_consts[2]; /* a draw's constants in memory, vertex and fragment
                               (sgx_draw.c) */
   unsigned idx_staging_size;
   /* the instance this draw is (an instanced draw is one draw per instance,
    * sgx_draw_vbo): gl_InstanceID, and the draw's first instance */
   unsigned instance, base_instance;
   bool warned_shader, warned_prim, warned_blend, warned_varyings, warned_cube,
      warned_frag_depth;
};

/* the blend state's render target for the pass being drawn (rt_index) */
static inline unsigned
sgx_blend_rt(const struct sgx_blend *b, unsigned rt_index)
{
   return b && b->base.independent_blend_enable ? MIN2(rt_index, SGX_BLEND_RTS - 1)
                                                : 0;
}

static inline struct sgx_context *
sgx_context(struct pipe_context *pctx)
{
   return (struct sgx_context *)pctx;
}

struct pipe_context *sgx_context_create(struct pipe_screen *pscreen, void *priv,
                                        unsigned flags);

void sgx_state_init(struct sgx_context *ctx);
/* while an occlusion query counts: its slot's address less 4, for
 * sa[SGXWS_VISIBLE_SA] (0 when none counts); and, once the draw is in the
 * frame it renders in, the frame noted and the slots listed */
uint32_t sgx_query_va(const struct sgx_context *ctx);
void sgx_query_drawn(struct sgx_context *ctx);
void sgx_draw_init(struct sgx_context *ctx);
void sgx_draw_fini(struct sgx_context *ctx);
void sgx_program_init(struct sgx_context *ctx);

void sgx_stats_report(struct sgx_context *ctx);

/* submit the frame being recorded (its render runs on while the CPU goes on) */
void sgx_flush_frame(struct sgx_context *ctx);
/* submit it and wait for every render submitted */
void sgx_finish(struct sgx_context *ctx);
/* before the CPU touches res: finish any frame that renders into it */
void sgx_flush_resource_writes(struct sgx_context *ctx,
                               struct sgx_resource *res);

/* The frame this context records, if it still is one: a frame whose target
 * was destroyed went at the destruction (sgx_resource.c), and another
 * context's frame on the shared render context ends ours (sgxws_begin).
 * Compares addresses only: the resource may be gone. */
struct sgx_resource *sgx_recording(struct sgx_context *ctx);

/* the colour buffer the draw or clear pass is on (MRT: a pass a buffer,
 * rt_index), or an empty surface */
static inline const struct pipe_surface *
sgx_cbuf(const struct sgx_context *ctx)
{
   static const struct pipe_surface none;

   return ctx->rt_index < ctx->framebuffer.nr_cbufs
             ? &ctx->framebuffer.cbufs[ctx->rt_index]
             : &none;
}
void sgx_check_reset(struct sgx_context *ctx);

#endif
