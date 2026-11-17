/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * Shaders.  The NIR -> USSE backend (src/sgx/compiler) compiles each one
 * to code plus a struct sgx_shader that says what it needs (docs/ISA.md
 * "What a compiler needs"); this file finalises and uploads the programs and
 * keeps what a draw needs: the constants' places in secondary attributes,
 * the attribute slots, the vertex outputs, the fragment iterators.
 *
 * Varyings get texture coordinate sets in the order of their slots, the same
 * on both sides when the two stages pass the same slots; a vertex shader that
 * writes others is compiled again to its fragment shader's sets
 * (sgx_vs_for).
 *
 * Before the compiler (SGX_DEBUG=novs,nofs still selects this), a shader was
 * matched against the fixed programs the winsys carries (sgx_fixed.c):
 * gl_Position = attribute 0, gl_FragColor = one vec4 uniform or constant.
 */

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_builtin_builder.h"
#include "compiler/nir/nir_format_convert.h"
#include "nir/tgsi_to_nir.h"
#include "util/format/u_format.h"
#include "util/os_time.h"
#include "util/u_memory.h"

#include "sgx_compiler.h"
#include "sgx_context.h"
#include "sgx_core.h"
#include "sgx_csb.h"
#include "sgx_program.h"
#include "sgx_screen.h"
#include "sgx_texture.h"

const nir_shader_compiler_options *
sgx_get_nir_options(void)
{
   /* the compiler's own, so the frontend builds what the compiler (and sgxc)
    * expect */
   return sgx_nir_options();
}

static unsigned
type_size(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

/* the one store_output of a shader, or NULL */
static nir_intrinsic_instr *
single_output(nir_shader *nir)
{
   nir_intrinsic_instr *out = NULL;

   nir_foreach_function_impl (impl, nir) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

            switch (intr->intrinsic) {
            case nir_intrinsic_store_output:
               if (out)
                  return NULL;
               out = intr;
               break;
            case nir_intrinsic_load_input:
            case nir_intrinsic_load_ubo:
            case nir_intrinsic_load_ubo_vec4:
            case nir_intrinsic_load_uniform:
            case nir_intrinsic_load_const_buf_base_addr_lvp:
               break;
            default:
               return NULL;
            }
         }
      }
   }
   return out;
}

/* is component c of the stored value channel `comp` of a load intrinsic of this
 * kind? */
static nir_intrinsic_instr *
source_load(nir_intrinsic_instr *store, unsigned c, unsigned *comp)
{
   nir_scalar s = nir_scalar_chase_movs(nir_get_scalar(store->src[0].ssa, c));

   if (!nir_scalar_is_intrinsic(s))
      return NULL;
   *comp = s.comp;
   return nir_def_as_intrinsic(s.def);
}

static bool
source_const(nir_intrinsic_instr *store, unsigned c, float *v)
{
   nir_scalar s = nir_scalar_chase_movs(nir_get_scalar(store->src[0].ssa, c));

   if (!nir_scalar_is_const(s))
      return false;
   *v = nir_scalar_as_float(s);
   return true;
}

static void
match_vs(struct sgx_pipe_shader *so)
{
   nir_intrinsic_instr *st = single_output(so->nir), *load = NULL;

   if (!st || nir_intrinsic_io_semantics(st).location != VARYING_SLOT_POS ||
       nir_intrinsic_write_mask(st) != 0xf ||
       nir_intrinsic_component(st) != 0 || st->src[0].ssa->num_components != 4)
      return;
   for (unsigned c = 0; c < 4; c++) {
      unsigned comp;
      nir_intrinsic_instr *l = source_load(st, c, &comp);

      if (!l || l->intrinsic != nir_intrinsic_load_input ||
          nir_intrinsic_base(l) != 0 || !nir_src_is_const(l->src[0]) ||
          nir_src_as_uint(l->src[0]) != 0 ||
          nir_intrinsic_component(l) + comp != c || (load && l != load))
         return;
      load = l;
   }
   so->kind = SGX_SHADER_VS_POSITION;
}

static void
match_fs(struct sgx_pipe_shader *so)
{
   nir_intrinsic_instr *st = single_output(so->nir), *load = NULL;
   unsigned loc;
   float k[4];
   bool constant = true;

   if (!st)
      return;
   /* (the fixed programs write a float colour: an integer one is compiled,
    * packed for its target) */
   nir_alu_type ot = nir_alu_type_get_base_type(nir_intrinsic_src_type(st));
   if (ot == nir_type_int || ot == nir_type_uint)
      return;
   loc = nir_intrinsic_io_semantics(st).location;
   if ((loc != FRAG_RESULT_COLOR && loc != FRAG_RESULT_DATA0) ||
       nir_intrinsic_write_mask(st) != 0xf ||
       st->src[0].ssa->num_components != 4)
      return;
   for (unsigned c = 0; c < 4; c++)
      constant &= source_const(st, c, &k[c]);
   if (constant) {
      memcpy(so->color, k, sizeof(k));
      so->kind = SGX_SHADER_FS_CONSTANT;
      return;
   }
   for (unsigned c = 0; c < 4; c++) {
      unsigned comp;
      nir_intrinsic_instr *l = source_load(st, c, &comp);

      if (!l || comp != c || (load && l != load))
         return;
      /* (a load_ubo is a uniform block's, compiled: st's default uniforms
       * are load_uniform, as nothing lowers them to constant buffer 0) */
      if (l->intrinsic == nir_intrinsic_load_uniform) {
         if (!nir_src_is_const(l->src[0]))
            return;
      } else {
         return;
      }
      load = l;
   }
   /* load_uniform's vec4 slots */
   so->ubo_offset =
      (nir_intrinsic_base(load) + nir_src_as_uint(load->src[0])) * 16;
   so->kind = SGX_SHADER_FS_UNIFORM;
}

/* ---------------------------------------------------------------- compiled
 * shaders */

/* st hands uniforms over as load_uniform in vec4 slots; the compiler numbers
 * constants in components (constant k = constant buffer 0 float k), a
 * dynamic offset (a[i]) too */
static bool
rescale_uniform(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   if (in->intrinsic != nir_intrinsic_load_uniform)
      return false;
   b->cursor = nir_before_instr(&in->instr);
   if (!nir_src_is_const(in->src[0])) {
      nir_def *v = nir_load_uniform(b, in->def.num_components, in->def.bit_size,
                                    nir_imul_imm(b, in->src[0].ssa, 4),
                                    .base = nir_intrinsic_base(in) * 4,
                                    .range = nir_intrinsic_range(in) * 4,
                                    .dest_type = nir_intrinsic_dest_type(in));
      nir_def_replace(&in->def, v);
      return true;
   }
   nir_def *v = nir_load_uniform(
      b, in->def.num_components, in->def.bit_size, nir_imm_int(b, 0),
      .base = (nir_intrinsic_base(in) + nir_src_as_uint(in->src[0])) * 4,
      .range = nir_intrinsic_range(in) * 4,
      .dest_type = nir_intrinsic_dest_type(in));
   nir_def_replace(&in->def, v);
   return true;
}

/* Uses of the integer def by those of f, a float the draw fills: an i2f32 of
 * it takes f itself, anything else f2i32(f).  The integer is whole and well
 * within F32's 24 bits (a size, an instance number), so the conversions are
 * exact. */
static void
rewrite_int_as_float(nir_builder *b, nir_def *def, nir_def *f)
{
   nir_def *i = NULL;

   nir_foreach_use_including_if_safe (use, def) {
      nir_instr *u = nir_src_is_if(use) ? NULL : nir_src_use_instr(use);

      if (u && u->type == nir_instr_type_alu &&
          nir_instr_as_alu(u)->op == nir_op_i2f32) {
         nir_alu_instr *alu = nir_instr_as_alu(u);
         unsigned swz[NIR_MAX_VEC_COMPONENTS];

         for (unsigned c = 0; c < alu->def.num_components; c++)
            swz[c] = alu->src[0].swizzle[c];
         b->cursor = nir_after_instr(u);
         nir_def_rewrite_uses(&alu->def,
                              nir_swizzle(b, f, swz, alu->def.num_components));
      } else {
         if (!i) {
            b->cursor = nir_after_instr(nir_def_instr(f));
            i = nir_f2i32(b, f);
         }
         nir_src_rewrite(use, i);
      }
   }
}

/* textureSize of a program compiled with native integers: the base level's
 * size from driver constants (SGX_K_SIZE + 3u: width, height, depth or
 * layers, raw integers), shifted down to the level asked for; an array's
 * layer count isn't */
static bool
lower_txs_int(nir_builder *b, nir_tex_instr *tex)
{
   int li = nir_tex_instr_src_index(tex, nir_tex_src_lod);
   unsigned n = tex->def.num_components;

   if (tex->texture_index >= SGX_K_SIZE_UNITS || n > 3)
      return false;
   b->cursor = nir_before_instr(&tex->instr);
   nir_def *size = nir_load_uniform(
      b, n, 32, nir_imm_int(b, 0), .base = SGX_K_SIZE + 3 * tex->texture_index,
      .range = n, .dest_type = nir_type_int32);

   if (li >= 0 && !(nir_src_is_const(tex->src[li].src) &&
                    nir_src_as_uint(tex->src[li].src) == 0)) {
      nir_def *lod = tex->src[li].src.ssa;
      nir_def *c[3];
      unsigned mips = n - (tex->is_array ? 1 : 0);

      for (unsigned i = 0; i < n; i++) {
         c[i] = nir_channel(b, size, i);
         if (i < mips)
            c[i] = nir_imax(b, nir_ushr(b, c[i], lod), nir_imm_int(b, 1));
      }
      size = nir_vec(b, c, n);
   }
   nir_def_replace(&tex->def, size);
   return true;
}

/* gl_FragCoord's x and y as the hardware rasterizes them (no window
 * transform: lower_tex_array only needs their parity), declaring the input
 * if the program doesn't read it */
static nir_def *
array_frag_xy(nir_builder *b)
{
   nir_shader *s = b->shader;
   nir_variable *v =
      nir_find_variable_with_location(s, nir_var_shader_in, VARYING_SLOT_POS);

   if (!v) {
      v = nir_variable_create(s, nir_var_shader_in, glsl_vec4_type(),
                              "gl_FragCoord");
      v->data.location = VARYING_SLOT_POS;
      v->data.interpolation = INTERP_MODE_SMOOTH;
   }
   return nir_load_input(b, 2, 32, nir_imm_int(b, 0), .base = 0,
                         .component = 0, .dest_type = nir_type_float32,
                         .io_semantics.location = VARYING_SLOT_POS,
                         .io_semantics.num_slots = 1);
}

/* layer clamp(floor(z + 0.5), 0, n - 1), and the state with T2, the
 * address, moved on to it */
static nir_def *
array_layer(nir_builder *b, nir_def *z, nir_def *sl)
{
   nir_def *layer = nir_f2i32(b, nir_ffloor(b, nir_fadd_imm(b, z, 0.5f)));

   return nir_imin(b, nir_imax(b, layer, nir_imm_int(b, 0)),
                   nir_iadd_imm(b, nir_channel(b, sl, 1), -1));
}

static nir_def *
array_state(nir_builder *b, nir_def *words, nir_def *sl, nir_def *layer)
{
   nir_def *t2 = nir_iadd(b, nir_channel(b, words, 2),
                          nir_imul(b, layer, nir_channel(b, sl, 0)));

   return nir_vec4(b, nir_channel(b, words, 0), nir_channel(b, words, 1), t2,
                   nir_channel(b, words, 3));
}

/* A sample of a 2D array (docs/GL3.md "texture arrays"): no layer field is
 * known in the texture words, so each layer is a 2D texture of its own at a
 * layer stride (sgx_tex_offset), and the sample is a 2D one whose state the
 * program builds: the unit's words (SGX_K_TEXSTATE, the texelFetch ones for a
 * fetch) with T2, the address, moved on to layer clamp(floor(z + 0.5), 0,
 * n - 1).  The compiler takes the state from the texture_handle source
 * (smp's state operand in temporaries; hardware: hwt_smp, ISA.md).  Needs
 * the native integers.
 *
 * The TPU reads a sample's state once a 2x2 quad, from its first pixel, so
 * with the layer changing inside a quad the other three read the first
 * one's layer.  When z may differ between pixels (divergence analysis) and
 * the sample isn't in divergent flow, the quad's four layers are made
 * known to all four pixels and the sample is made once for each, every pixel
 * keeping the one of its own layer.  The derivatives are coarse (measured:
 * dFdx the first row's difference, dFdy the first column's, for all four), so
 * a derivative of a value kept by one pixel only reads that pixel's: z of the
 * first pixel, of the one beside it and of the one below.  The fourth's is
 * z10 + z01 - z00, exact when z is affine or separable in x and y across the
 * quad (a varying, a uniform, gl_FragCoord math); otherwise that pixel can
 * read a neighbour's layer.  R0, the row parity of the quad's first row,
 * comes from a dFdx (no sign, whatever the target's y direction), and dFdy's
 * sign from a dFdy of a row flag. */
static bool
lower_tex_array(nir_builder *b, nir_tex_instr *tex, void *data)
{
   int ci = nir_tex_instr_src_index(tex, nir_tex_src_coord);

   if (!tex->is_array || tex->sampler_dim != GLSL_SAMPLER_DIM_2D ||
       nir_tex_instr_is_query(tex) || ci < 0 ||
       tex->texture_index >= SGX_K_SIZE_UNITS ||
       nir_tex_instr_src_index(tex, nir_tex_src_texture_handle) >= 0 ||
       (tex->op != nir_texop_tex && tex->op != nir_texop_txb &&
        tex->op != nir_texop_txl))
      return false;
   nir_def *coord = tex->src[ci].src.ssa;
   nir_scalar zs = nir_scalar_chase_movs(nir_get_scalar(coord, 2));
   bool per_quad = b->shader->info.stage == MESA_SHADER_FRAGMENT &&
                   zs.def->divergent && !tex->instr.block->divergent;

   b->cursor = nir_before_instr(&tex->instr);
   unsigned base = SGX_K_TEXSTATE + SGX_K_TEXSTATE_N * tex->texture_index;
   bool fetch = tex->backend_flags & SGX_TEX_FETCH;
   nir_def *words =
      nir_load_uniform(b, 4, 32, nir_imm_int(b, 0), .base = base + (fetch ? 4 : 0),
                       .range = 4, .dest_type = nir_type_int32);
   nir_def *sl =
      nir_load_uniform(b, 2, 32, nir_imm_int(b, 0), .base = base + 8, .range = 2,
                       .dest_type = nir_type_int32);
   nir_def *z = nir_channel(b, coord, 2);
   nir_def *layer = array_layer(b, z, sl);

   nir_src_rewrite(&tex->src[ci].src, nir_trim_vector(b, coord, 2));
   tex->coord_components = 2;
   tex->is_array = false;
   if (!per_quad) {
      nir_tex_instr_add_src(tex, nir_tex_src_texture_handle,
                            array_state(b, words, sl, layer));
      return true;
   }

   nir_def *xy = nir_ffloor(b, array_frag_xy(b));
   nir_def *xo = nir_ffract(b, nir_fmul_imm(b, nir_channel(b, xy, 0), 0.5f));
   nir_def *yo = nir_ffract(b, nir_fmul_imm(b, nir_channel(b, xy, 1), 0.5f));
   nir_def *zero = nir_imm_float(b, 0.0f);
   /* 0.5 for an odd x or y: xo, yo in {0, 0.5} */
   nir_def *r0 = nir_ddx_coarse(b, nir_fmul(b, yo, xo)); /* row0's yo * 0.5 */
   nir_def *row0 = nir_feq(b, nir_fmul_imm(b, yo, 0.5f), r0);
   nir_def *left = nir_feq(b, xo, zero);
   nir_def *rs = nir_ddy_coarse(b, nir_bcsel(b, row0, zero, nir_imm_float(b, 1.0f)));
   nir_def *z00 = nir_fneg(b, nir_ddx_coarse(b, nir_bcsel(b, nir_iand(b, row0, left), z, zero)));
   nir_def *z10 = nir_ddx_coarse(
      b, nir_bcsel(b, nir_iand(b, row0, nir_inot(b, left)), z, zero));
   nir_def *z01 = nir_fdiv(
      b, nir_ddy_coarse(b, nir_bcsel(b, nir_iand(b, nir_inot(b, row0), left), z, zero)),
      rs);
   nir_def *zq[4] = {z00, z10, z01, nir_fsub(b, nir_fadd(b, z10, z01), z00)};

   /* the four samples: the instruction itself the first, three clones put
    * after it, each in the shader before its sources change; their states
    * are built before the first */
   nir_def *lq[4], *st[4];
   nir_tex_instr *tq[4] = {tex};
   for (unsigned q = 0; q < 4; q++) {
      lq[q] = array_layer(b, zq[q], sl);
      st[q] = array_state(b, words, sl, lq[q]);
   }
   b->cursor = nir_after_instr(&tex->instr);
   for (unsigned q = 1; q < 4; q++) {
      tq[q] = nir_instr_as_tex(nir_instr_clone(b->shader, &tex->instr));
      nir_builder_instr_insert(b, &tq[q]->instr);
   }
   for (unsigned q = 0; q < 4; q++)
      nir_tex_instr_add_src(tq[q], nir_tex_src_texture_handle, st[q]);
   /* each pixel its own layer's: the fourth when none of the others is */
   nir_def *res = &tq[3]->def;
   for (int q = 2; q >= 0; q--)
      res = nir_bcsel(b, nir_ieq(b, layer, lq[q]), &tq[q]->def, res);
   nir_def_rewrite_uses_after(&tex->def, res);
   return true;
}

/* Without SGX_DEBUG=tex3d a 3D texture is one texel (max_texture_3d_levels
 * = 1), kept as a 1x1 2D texture
 * (sgx_resource), which every (s, t, r) reads whatever the wrap and filter:
 * its samples become 2D ones at (s, t). Sizes and level counts keep the
 * volume's (lower_txs). */
static bool
lower_tex_3d(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_tex)
      return false;
   nir_tex_instr *tex = nir_instr_as_tex(instr);

   if (tex->sampler_dim != GLSL_SAMPLER_DIM_3D || tex->op == nir_texop_txs ||
       tex->op == nir_texop_query_levels || tex->op == nir_texop_lod ||
       tex->op == nir_texop_texture_samples)
      return false;
   b->cursor = nir_before_instr(instr);
   for (unsigned i = 0; i < tex->num_srcs; i++) {
      switch (tex->src[i].src_type) {
      case nir_tex_src_coord:
      case nir_tex_src_ddx:
      case nir_tex_src_ddy:
      case nir_tex_src_offset:
         if (tex->src[i].src.ssa->num_components > 2)
            nir_src_rewrite(&tex->src[i].src,
                            nir_trim_vector(b, tex->src[i].src.ssa, 2));
         break;
      default:
         break;
      }
   }
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   return true;
}

/* textureSize (level 0) of a fragment program, which st's lowering of
 * rectangle lookups asks for (coords / textureSize): the size from driver
 * constants (SGX_K_TEXSIZE) the draw fills, as floats, where the program
 * converts it to float anyway.  With native integers (data), lower_txs_int. */
static bool
lower_txs(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_tex)
      return false;
   nir_tex_instr *tex = nir_instr_as_tex(instr);
   int li = nir_tex_instr_src_index(tex, nir_tex_src_lod);

   if (tex->op == nir_texop_txs && *(const bool *)data)
      return lower_txs_int(b, tex);
   /* the view's level count, from the draw (SGX_K_LEVELS) */
   if (tex->op == nir_texop_query_levels && *(const bool *)data &&
       tex->texture_index < SGX_K_SIZE_UNITS) {
      b->cursor = nir_before_instr(instr);
      nir_def_replace(&tex->def,
                      nir_load_uniform(b, 1, 32, nir_imm_int(b, 0),
                                       .base = SGX_K_LEVELS + tex->texture_index,
                                       .range = 1, .dest_type = nir_type_int32));
      return true;
   }
   if (tex->op != nir_texop_txs || tex->def.num_components > 2 ||
       tex->texture_index >= SGX_K_TEXSIZE_UNITS ||
       (li >= 0 && (!nir_src_is_const(tex->src[li].src) ||
                    nir_src_as_uint(tex->src[li].src))))
      return false;
   b->cursor = nir_before_instr(instr);
   nir_def *f = nir_load_uniform(
      b, tex->def.num_components, 32, nir_imm_int(b, 0),
      .base = SGX_K_TEXSIZE + 2 * tex->texture_index,
      .range = tex->def.num_components, .dest_type = nir_type_float32);

   rewrite_int_as_float(b, &tex->def, f);
   nir_instr_remove(instr);
   return true;
}

/* gl_InstanceID of a vertex program: an instanced draw is one draw per
 * instance (sgx_draw_vbo), each with its instance number in a driver
 * constant: SGX_K_INSTANCE as a float, or with native integers (data)
 * SGX_K_INSTANCE_INT as an integer */
static bool
lower_instance_id(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   if (in->intrinsic != nir_intrinsic_load_instance_id)
      return false;
   b->cursor = nir_before_instr(&in->instr);
   if (*(const bool *)data) {
      nir_def_replace(&in->def,
                      nir_load_uniform(b, 1, 32, nir_imm_int(b, 0),
                                       .base = SGX_K_INSTANCE_INT, .range = 1,
                                       .dest_type = nir_type_int32));
      return true;
   }
   nir_def *f =
      nir_load_uniform(b, 1, 32, nir_imm_int(b, 0), .base = SGX_K_INSTANCE,
                       .range = 1, .dest_type = nir_type_float32);

   rewrite_int_as_float(b, &in->def, f);
   nir_instr_remove(&in->instr);
   return true;
}

/* gl_VertexID of a vertex program (docs/GL3.md, "Crossing the
 * driver/compiler line"): not the PDS's in0, as the draw rebases or expands
 * the indices it gathers, but an extra vertex input the draw fills with the
 * vertex's own index into the buffers, the base vertex included
 * (sgx_draw.c fetch_vertex), in the attribute slot after the shader's own */
struct vertex_id_data {
   nir_variable *var;
   bool ints;
};

static bool
lower_vertex_id(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   struct vertex_id_data *d = data;

   if (in->intrinsic != nir_intrinsic_load_vertex_id)
      return false;
   if (!d->var) {
      unsigned loc = VERT_ATTRIB_GENERIC0, slot = 0;

      nir_foreach_shader_in_variable (var, b->shader) {
         loc = MAX2(loc, var->data.location +
                            glsl_count_attribute_slots(var->type, true));
         if (var->name)
            slot = MAX2(slot, var->data.driver_location +
                                 glsl_count_attribute_slots(var->type, true));
      }
      d->var = nir_variable_create(b->shader, nir_var_shader_in,
                                   d->ints ? glsl_int_type() : glsl_float_type(),
                                   "sgx_vertex_id");
      d->var->data.location = loc;
      d->var->data.driver_location = slot;
      b->shader->info.inputs_read |= BITFIELD64_BIT(loc);
   }
   b->cursor = nir_before_instr(&in->instr);
   nir_def *v = nir_load_var(b, d->var);

   if (d->ints) {
      nir_def_replace(&in->def, v);
      return true;
   }
   rewrite_int_as_float(b, &in->def, v);
   nir_instr_remove(&in->instr);
   return true;
}

/*
 * varying -> set location - VAR0, the same on both sides.  Entries are named
 * the way the compiler looks varyings up (sgx_nir.c var_names): st names a
 * packed varying "packed:a,b", one name per varying it holds.
 */
/* Desktop GL's built-in varyings (colours, back colours, fog, texture
 * coordinates; also st's fixed-function shaders and GLES1 translation layers)
 * are in the legacy slots, which the compiler doesn't assign: move them to VAR
 * slots the program's own varyings leave free, each built-in its own whether
 * this stage has it or not, so that two stages with the same varyings (a linked
 * program's) place them alike; named after the built-in, so that a vertex
 * shader compiled to its fragment shader's sets finds them anyway. st's
 * two-sided colour reads the back colours in the fragment shader. */
#define SGX_LEGACY_NAME "sgx_legacy"

static void
remap_legacy_varyings(nir_shader *nir)
{
   static const gl_varying_slot map[] = {
      VARYING_SLOT_COL0, VARYING_SLOT_COL1, VARYING_SLOT_BFC0,
      VARYING_SLOT_BFC1, VARYING_SLOT_FOGC, VARYING_SLOT_TEX0,
      VARYING_SLOT_TEX1, VARYING_SLOT_TEX2, VARYING_SLOT_TEX3,
      VARYING_SLOT_TEX4, VARYING_SLOT_TEX5, VARYING_SLOT_TEX6,
      VARYING_SLOT_TEX7,
   };
   nir_variable_mode mode = nir->info.stage == MESA_SHADER_VERTEX
                               ? nir_var_shader_out
                               : nir_var_shader_in;
   int slot[ARRAY_SIZE(map)];
   uint32_t used = 0; /* VAR slots of the program's own varyings */
   bool changed = false;

   nir_foreach_variable_with_modes (var, nir, mode) {
      if (var->data.location >= VARYING_SLOT_VAR0 &&
          var->data.location < VARYING_SLOT_VAR0 + 32) {
         for (unsigned r = 0;
              r < glsl_count_vec4_slots(var->type, false, false); r++)
            used |=
               BITFIELD_BIT((var->data.location - VARYING_SLOT_VAR0 + r) & 31);
      }
   }
   for (unsigned i = 0, s = 0; i < ARRAY_SIZE(map); i++) {
      while (s < 32 && (used & BITFIELD_BIT(s)))
         s++;
      slot[i] = s < 32 ? (int)s++ : -1;
   }
   nir_foreach_variable_with_modes (var, nir, mode) {
      for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
         char name[32];

         if (var->data.location != (int)map[i])
            continue;
         if (slot[i] < 0) {
            fprintf(stderr, "sgx: no VAR slot left for a built-in varying\n");
            break;
         }
         var->data.location = VARYING_SLOT_VAR0 + slot[i];
         snprintf(name, sizeof(name), SGX_LEGACY_NAME "%u", i);
         var->name = ralloc_strdup(nir, name);
         changed = true;
         break;
      }
   }
   if (changed)
      nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
}

static int
cmp_varying(const void *a, const void *b)
{
   const nir_variable *x = *(nir_variable *const *)a,
                      *y = *(nir_variable *const *)b;

   if (x->data.location != y->data.location)
      return x->data.location - y->data.location;
   return (int)x->data.location_frac - (int)y->data.location_frac;
}

/* The varyings' sets.  sprite: a fragment shader that reads gl_PointCoord
 * (found here), or a vertex shader to go with one: the ISP writes the
 * coordinate into set 0 (STATE.md "Lines and points"), so everything else
 * moves up one set. */
static void
assign_varyings(nir_shader *nir, struct sgx_varyings *vary, bool sprite)
{
   nir_variable_mode mode = nir->info.stage == MESA_SHADER_VERTEX
                               ? nir_var_shader_out
                               : nir_var_shader_in;
   nir_variable *vars[ARRAY_SIZE(vary->v)];
   unsigned n = 0;
   uint64_t rows_used = 0; /* by location - VAR0 */
   bool psize = false;
   static bool warned;

   memset(vary, 0, sizeof(*vary));
   nir_foreach_variable_with_modes (var, nir, mode) {
      if (nir->info.stage == MESA_SHADER_FRAGMENT &&
          var->data.location == VARYING_SLOT_PNTC)
         sprite = true;
      /* the point size is the output dword after the sets */
      if (nir->info.stage == MESA_SHADER_VERTEX &&
          var->data.location == VARYING_SLOT_PSIZ)
         psize = true;
      if (var->data.location < VARYING_SLOT_VAR0 || n == ARRAY_SIZE(vars))
         continue;
      vars[n++] = var;
      for (unsigned r = 0; r < glsl_count_vec4_slots(var->type, false, false);
           r++)
         rows_used |=
            BITFIELD64_BIT((var->data.location - VARYING_SLOT_VAR0 + r) & 63);
   }
   /* a set per slot in slot order (after gl_PointCoord's set 0), each
    * variable at its component in the slot; two for a slot that holds an
    * integer: the compiler passes an integer row as its 16-bit halves, exact
    * floats, the low ones in the first set and the high ones in the next
    * (sgx_varying.split). A raw word iterated as an F32 doesn't survive: -1
    * is a NaN, and small ones are denormals */
   qsort(vars, n, sizeof(vars[0]), cmp_varying);
   uint8_t slot_sets[64] = {0};
   for (unsigned s = 0; s < 64; s++)
      slot_sets[s] = rows_used & BITFIELD64_BIT(s) ? 1 : 0;
   for (unsigned i = 0; i < n; i++) {
      unsigned slot = vars[i]->data.location - VARYING_SLOT_VAR0;

      if (!glsl_base_type_is_integer(
             glsl_get_base_type(glsl_without_array(vars[i]->type))))
         continue;
      for (unsigned r = 0;
           r < glsl_count_vec4_slots(vars[i]->type, false, false); r++)
         slot_sets[(slot + r) & 63] = 2;
   }
   unsigned n_sets = 0;
   for (unsigned s = 0; s < 64; s++)
      n_sets += slot_sets[s];
   for (unsigned i = 0; i < n; i++) {
      nir_variable *var = vars[i];
      const struct glsl_type *t = var->type;
      unsigned slot = var->data.location - VARYING_SLOT_VAR0,
               frac = var->data.location_frac;
      unsigned rows = glsl_count_vec4_slots(t, false, false), comps, set;
      struct sgx_varying *v = &vary->v[vary->n++];
      char name[32];

      t = glsl_without_array(t);
      comps = glsl_type_is_matrix(t)
                 ? glsl_get_vector_elements(glsl_get_column_type(t))
                 : glsl_get_vector_elements(t);
      /* a built-in the two stages may declare apart (gl_FogFragCoord a float
       * against fog's vec4): the whole slot, under its own name */
      bool legacy = var->name && !strncmp(var->name, SGX_LEGACY_NAME,
                                          strlen(SGX_LEGACY_NAME));
      if (legacy) {
         frac = 0;
         comps = 4;
      }
      set = sprite ? 1 : 0;
      for (unsigned s = 0; s < slot && s < 64; s++)
         set += slot_sets[s];
      bool split = glsl_base_type_is_integer(glsl_get_base_type(t));
      /* a float sharing an integer's slot (another location_frac) is in the
       * slot's first set; one over several such slots would need every
       * other set, which a varying can't say */
      if (!split && rows > 1)
         for (unsigned r = 0; r < rows; r++)
            if (slot_sets[(slot + r) & 63] == 2 && !warned) {
               fprintf(stderr, "sgx: a float array shares slots with an "
                               "integer varying: not laid out\n");
               warned = true;
            }
      /* The compiler finds a varying's set by name.  st's packed slots are
       * named after what they hold ("packed:a,b"), and a varying split over
       * two slots names both: two vec3 texture coordinates and the fog had
       * the second slot written over the first.
       * Each variable is named after its slot instead, the same in both
       * stages. */
      if (legacy)
         snprintf(name, sizeof(name), "%s", var->name);
      else
         snprintf(name, sizeof(name), frac ? "sgx_v%u_%u" : "sgx_v%u", slot,
                  frac);
      var->name = ralloc_strdup(nir, name);
      snprintf(v->name, sizeof(v->name), "%s", name);
      v->set = set;
      v->comp = frac;
      v->comps = comps;
      v->precision = var->data.precision;
      v->noperspective =
         var->data.interpolation == INTERP_MODE_NOPERSPECTIVE;
      if (split) {
         /* (whole numbers to 65535: an F32 iteration) */
         v->split = true;
         rows *= 2;
         v->precision = GLSL_PRECISION_HIGH;
      }
      v->rows = rows;
      for (unsigned r = 0; r < rows && set + r < SGX_MAX_SETS; r++)
         vary->set_comps[set + r] =
            MAX2(vary->set_comps[set + r], frac + comps);
   }
   /* noperspective: the compiler's own w varying (SGX_NOPERSPECTIVE_W, one
    * highp component), in the set after the slots', the same in both stages
    * (the vertex program writes v * w and w, the fragment one divides) */
   bool noperspective = false;
   for (unsigned i = 0; i < n; i++)
      noperspective |=
         vars[i]->data.interpolation == INTERP_MODE_NOPERSPECTIVE;
   if (noperspective && vary->n < ARRAY_SIZE(vary->v)) {
      struct sgx_varying *v = &vary->v[vary->n++];
      unsigned set = (sprite ? 1 : 0) + n_sets;

      snprintf(v->name, sizeof(v->name), "%s", SGX_NOPERSPECTIVE_W);
      v->set = set;
      v->comp = 0;
      v->rows = 1;
      v->comps = 1;
      v->precision = GLSL_PRECISION_HIGH;
      if (set < SGX_MAX_SETS)
         vary->set_comps[set] = MAX2(vary->set_comps[set], 1);
   }
   /* A flat primary colour (glShadeModel(GL_FLAT): st makes it a flat
    * input) goes to set 0 as the TA's vertex colour, the one source the PDS
    * iterates flat; the sets before it move up one.  Not with a sprite's
    * coordinate in set 0. */
   for (unsigned i = 0; nir->info.stage == MESA_SHADER_FRAGMENT && !sprite &&
                        i < n && i < vary->n;
        i++) {
      struct sgx_varying *c = &vary->v[i];

      if (strcmp(c->name, SGX_LEGACY_NAME "0") ||
          vars[i]->data.interpolation != INTERP_MODE_FLAT || c->rows != 1)
         continue;
      for (unsigned j = 0; j < vary->n; j++) {
         if (j != i && vary->v[j].set < c->set)
            vary->v[j].set++;
      }
      c->set = 0;
      memset(vary->set_comps, 0, sizeof(vary->set_comps));
      for (unsigned j = 0; j < vary->n; j++) {
         const struct sgx_varying *v = &vary->v[j];

         for (unsigned r = 0; r < v->rows && v->set + r < SGX_MAX_SETS; r++)
            vary->set_comps[v->set + r] =
               MAX2(vary->set_comps[v->set + r], v->comp + v->comps);
      }
      vary->colour = true;
      break;
   }
   /* past ten sets: rows into the free components of earlier sets */
   vary->point_size = sprite;
   if (sprite)
      vary->set_comps[0] = 2;
   if (!sgx_pack_varyings(vary) && !warned) {
      fprintf(stderr, "sgx: varyings past ten sets of four components\n");
      warned = true;
   }
   vary->point_size = sprite || psize;
}

/* the same sets for the same varyings (the stages' precisions may differ) */
static bool
same_varyings(const struct sgx_varyings *a, const struct sgx_varyings *b)
{
   if (a->n != b->n || a->colour != b->colour ||
       memcmp(a->set_comps, b->set_comps, sizeof(a->set_comps)))
      return false;
   for (unsigned i = 0; i < a->n; i++) {
      const struct sgx_varying *x = &a->v[i], *y = &b->v[i];

      if (strcmp(x->name, y->name) || x->set != y->set || x->comp != y->comp ||
          x->rows != y->rows || x->comps != y->comps ||
          x->noperspective != y->noperspective)
         return false;
   }
   return true;
}

#define SGX_FRAGCOORD_ITER 14

/* what the draw needs from the compiled program besides its code: the
 * dependent samples (where each smp goes in the flat program, its template,
 * sampler and raw texel register) and the branches whose offsets move when
 * the smps go in */
struct prog_info {
   const char *why; /* what read_program refused */
   struct {
      unsigned at, sampler, raw;
      unsigned phase; /* split: at indexes code1 */
      bool f16;       /* the texel returns F16, two registers from raw */
      bool fetch;     /* a texelFetch: state of its own (sgx_draw.c) */
      unsigned plane; /* an RG32 / RGBA32 unit's plane (channel) */
      unsigned state; /* its texture state block */
      /* the program's state, in r<state_r> on (a 2D array layer's) */
      bool state_in_r;
      unsigned state_r;
      uint64_t smp;
   } deps[SGX_MAX_DEP_SAMPLES];
   unsigned n_deps;
   unsigned n_states; /* texture state blocks (sgx_shader.n_dep_states) */
   unsigned branch_at[64]; /* flat indices of the branches */
   unsigned n_branches;
};

/* The flat program with each dependent sample's smp and its wdf put in
 * (ISA.md "The texture-sample sequence"): the template with the raw texel's
 * temporary as destination (l21-27, h7 clear: r) and the sampler's 4 state
 * words, in SA from `sa` on, as the texture state (src1 l7-13, bank l30-31 =
 * 3: sa; h17 clear); then `wdf` on the template's DRC before the compiler's
 * unpack reads the texel. Branch offsets are moved with the code. NULL on
 * allocation failure; *n is the new length. */
static uint64_t *
insert_samples(const uint64_t *code, unsigned count,
               const struct prog_info *info, const unsigned *sa, unsigned *n)
{
   unsigned total = count + 2 * info->n_deps, j = 0, k = 0;
   uint64_t *w = malloc(total * sizeof(*w));
   unsigned *map = malloc((count + 1) * sizeof(*map));

   if (!w || !map) {
      free(w);
      free(map);
      return NULL;
   }
   for (unsigned i = 0; i <= count; i++) {
      while (j < info->n_deps && info->deps[j].at == i) {
         uint32_t lo = (uint32_t)info->deps[j].smp,
                  hi = info->deps[j].smp >> 32;

         /* (the program's own state: r, bank 0) */
         lo = (lo & ~(0x7fu << 21 | 0x7fu << 7 | 0xc0000000u)) |
              info->deps[j].raw << 21 |
              (info->deps[j].state_in_r ? info->deps[j].state_r << 7
                                        : sa[j] << 7 | 0xc0000000u);
         hi &= ~(0x80u | 1u << 17);
         w[k++] = (uint64_t)hi << 32 | lo;
         w[k++] = (uint64_t)(0xf9200000u | (hi & 1)) << 32; /* wdf drc */
         j++;
      }
      map[i] = k;
      if (i < count)
         w[k++] = code[i];
   }
   /* the branches' relative offsets (l0-17, signed) */
   for (unsigned b = 0; b < info->n_branches; b++) {
      unsigned i = info->branch_at[b];
      uint32_t lo = (uint32_t)code[i], hi = code[i] >> 32;
      int off = (int)(lo << 14) >> 14, t = (int)i + off;

      if (i >= count || t < 0 || t > (int)count)
         continue;
      off = (int)map[t] - (int)map[i];
      w[map[i]] =
         (uint64_t)hi << 32 | (lo & ~0x3ffffu) | ((uint32_t)off & 0x3ffffu);
   }
   free(map);
   *n = k;
   return w;
}

static_assert(SGX_MAX_CONST_MAP == SGX_MAX_CONST_MAP,
              "the driver holds the compiler's whole constant map");

static bool
read_program(const struct sgx_shader *out, struct sgx_pipe_shader *so,
             struct prog_info *info)
{
   /* list C, decoded: kind 1 f32, 2 F16 half, 3 a C10 channel; ours
    * (opts.driver_consts): 0x10 an F16 half of the C10 value, 0x11 a literal,
    * 0x12 a vec4 packed as the BGRA8 colour */
   for (unsigned i = 0; i < out->n_consts; i++) {
      const struct sgx_const_map *m = &out->consts[i];
      struct sgx_const *k = &so->consts[so->n_consts];

      if (so->n_consts == SGX_MAX_CONST_MAP)
         return info->why = "too many constants", false;
      k->fmt = m->fmt;
      k->k = m->k;
      k->value = m->value;
      k->slot = m->slot;
      k->shift = m->shift;
      k->mem = m->mem;
      if (k->slot != SGX_SLOT_NONE)
         so->const_dwords = MAX2(so->const_dwords, k->slot + 1u);
      so->n_consts++;
   }
   if (out->mem_dwords > SGX_MAX_CONST_MAP + 1)
      return info->why = "constant memory block too large", false;
   so->mem_dwords = out->mem_dwords;
   so->n_mem_bases = MIN2(out->n_mem_bases, ARRAY_SIZE(so->mem_base_slot));
   for (unsigned j = 0; j < so->n_mem_bases; j++) {
      so->mem_base_slot[j] = out->mem_base_slot[j];
      so->const_dwords = MAX2(so->const_dwords, out->mem_base_slot[j] + 1u);
   }
   for (unsigned i = 0; i < out->n_samples; i++) {
      const struct sgx_sample_info *x = &out->samples[i];

      if (!x->dependent)
         continue;
      if (info->n_deps == SGX_MAX_DEP_SAMPLES)
         return info->why = "too many dependent texture samples", false;
      info->deps[info->n_deps].at = x->at;
      info->deps[info->n_deps].sampler = x->sampler;
      info->deps[info->n_deps].raw = x->reg;
      info->deps[info->n_deps].smp = x->smp;
      info->deps[info->n_deps].phase = x->phase;
      info->deps[info->n_deps].f16 = x->f16;
      info->deps[info->n_deps].fetch = x->fetch;
      info->deps[info->n_deps].plane = x->plane;
      info->deps[info->n_deps].state = x->state;
      info->deps[info->n_deps].state_in_r = x->state_in_r;
      info->deps[info->n_deps].state_r = x->state_r;
      if (x->state >= out->n_dep_states)
         return info->why = "dependent sample without texture state", false;
      info->n_deps++;
   }
   info->n_states = info->n_deps ? out->n_dep_states : 0;
   if (info->n_states > SGX_MAX_DEP_STATES)
      return info->why = "too many dependent texture states", false;
   for (unsigned i = 0; i < out->n_blocks; i++) {
      if (out->blocks[i].kind != SGX_BLOCK_BRANCH)
         continue;
      if (info->n_branches == ARRAY_SIZE(info->branch_at))
         return info->why = "too many branches", false;
      info->branch_at[info->n_branches++] = out->blocks[i].at;
   }
   return true;
}

/* the compiler's layout: vertex attributes where st put them
 * (driver_location); uniforms and samplers are the compiler's */
static int
layout_place(void *data, enum sgx_layout_kind kind, const char *name)
{
   nir_shader *nir = data;

   if (kind != SGX_LAYOUT_ATTRIB)
      return -1;
   nir_foreach_shader_in_variable (var, nir) {
      if (var->name && !strcmp(var->name, name))
         return var->data.driver_location;
   }
   return -1;
}

static unsigned
layout_first_free(void *data, enum sgx_layout_kind kind)
{
   nir_shader *nir = data;
   unsigned next = 0;

   if (kind == SGX_LAYOUT_ATTRIB) {
      nir_foreach_shader_in_variable (var, nir) {
         if (var->name)
            next = MAX2(next, var->data.driver_location +
                                 glsl_count_attribute_slots(var->type, true));
      }
   }
   return next;
}

/*
 * Linear filtering of an sRGB texture, in the shader: the TPU filters the
 * encoded texels and the decode comes after, where GL decodes first.  The four
 * texels are sampled at their centres (where the TPU's filter returns the
 * texel itself), decoded (nir_lower_tex's lower_srgb, after this) and mixed
 * here.  Only for textures sampled at one level (sgx_fs_for): the level a
 * minified lookup takes isn't known to the shader.  data: the units.
 */
static bool
lower_srgb_filter(nir_builder *b, nir_instr *instr, void *data)
{
   uint32_t units = *(const uint32_t *)data;

   if (instr->type != nir_instr_type_tex)
      return false;
   nir_tex_instr *tex = nir_instr_as_tex(instr);
   int ci = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   bool rect = tex->sampler_dim == GLSL_SAMPLER_DIM_RECT;

   if (tex->op != nir_texop_tex || tex->texture_index >= SGX_K_TEXSIZE_UNITS ||
       !(units & (1u << tex->texture_index)) || tex->is_shadow ||
       tex->is_array || ci < 0 || tex->coord_components != 2 ||
       (tex->sampler_dim != GLSL_SAMPLER_DIM_2D && !rect) ||
       nir_tex_instr_src_index(tex, nir_tex_src_projector) >= 0 ||
       nir_tex_instr_src_index(tex, nir_tex_src_bias) >= 0 ||
       nir_tex_instr_src_index(tex, nir_tex_src_offset) >= 0)
      return false;
   b->cursor = nir_before_instr(instr);
   /* in texels: a rectangle texture's coordinates already are */
   nir_def *sz =
      rect ? NULL
           : nir_load_uniform(b, 2, 32, nir_imm_int(b, 0),
                              .base = SGX_K_TEXSIZE + 2 * tex->texture_index,
                              .range = 2, .dest_type = nir_type_float32);
   nir_def *coord = tex->src[ci].src.ssa;
   nir_def *u = nir_fadd_imm(b, sz ? nir_fmul(b, coord, sz) : coord, -0.5);
   nir_def *i0 = nir_ffloor(b, u);
   nir_def *f = nir_fsub(b, u, i0);
   nir_def *t[4];

   for (unsigned k = 0; k < 4; k++) {
      nir_tex_instr *tap = nir_instr_as_tex(nir_instr_clone(b->shader, instr));
      nir_def *c =
         nir_fadd(b, i0, nir_imm_vec2(b, (k & 1) + 0.5f, (k >> 1) + 0.5f));

      /* the coordinate before the tap that reads it */
      if (sz)
         c = nir_fdiv(b, c, sz);
      nir_builder_instr_insert(b, &tap->instr);
      nir_src_rewrite(&tap->src[ci].src, c);
      t[k] = &tap->def;
   }
   unsigned n = tex->def.num_components;
   nir_def *fx = nir_replicate(b, nir_channel(b, f, 0), n);
   nir_def *fy = nir_replicate(b, nir_channel(b, f, 1), n);
   nir_def *r =
      nir_flrp(b, nir_flrp(b, t[0], t[1], fx), nir_flrp(b, t[2], t[3], fx), fy);

   nir_def_replace(&tex->def, r);
   return true;
}

/* REPEAT and MIRRORED_REPEAT on a texture stored as F32 words
 * (sgx_tex_word32: depth, R32, the RG32 / RGBA32 and float planes): it is
 * read strided (T1 layout 6), and there the TPU doesn't wrap as the T0 codes
 * say (measured: RGBA16F and RGBA32F sampled over [-0.5, 1.5] are wrong in
 * every pixel outside [0, 1]; R32F in one axis), so the coordinate is
 * wrapped here and the TPU clamps (sgx_texture.c).  After
 * lower_srgb_filter, so each of a LINEAR filter's taps is wrapped on its
 * own; exact for the texel a NEAREST read picks.  Projected, 1D and array
 * samples, and units from 16, aren't wrapped: they keep the TPU's clamp.
 * data: the shader variant (wrap_rep, wrap_mir). */
static bool
lower_word32_wrap(nir_builder *b, nir_instr *instr, void *data)
{
   const struct sgx_pipe_shader *so = data;

   if (instr->type != nir_instr_type_tex)
      return false;
   nir_tex_instr *tex = nir_instr_as_tex(instr);
   int ci = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   unsigned u = tex->texture_index;

   if (u >= 16 || ci < 0 || tex->sampler_dim != GLSL_SAMPLER_DIM_2D ||
       tex->is_array || nir_tex_instr_is_query(tex) ||
       tex->op == nir_texop_txf || tex->op == nir_texop_txf_ms ||
       nir_tex_instr_src_index(tex, nir_tex_src_projector) >= 0 ||
       !((so->wrap_rep | so->wrap_mir) >> (2 * u) & 3))
      return false;
   b->cursor = nir_before_instr(instr);
   nir_def *coord = tex->src[ci].src.ssa;
   nir_def *c[NIR_MAX_VEC_COMPONENTS];

   for (unsigned i = 0; i < coord->num_components; i++) {
      c[i] = nir_channel(b, coord, i);
      if (i > 1)
         continue;
      if (so->wrap_rep >> (2 * u + i) & 1) {
         c[i] = nir_ffract(b, c[i]);
      } else if (so->wrap_mir >> (2 * u + i) & 1) {
         /* m in [0, 2): m below 1, 2 - m from there (1 itself the last
          * texel: the TPU clamps it) */
         nir_def *m = nir_fmul_imm(b, nir_ffract(b, nir_fmul_imm(b, c[i], 0.5)),
                                   2.0);
         c[i] = nir_bcsel(b, nir_flt_imm(b, m, 1.0), m,
                          nir_fsub_imm(b, 2.0, m));
      }
   }
   nir_src_rewrite(&tex->src[ci].src, nir_vec(b, c, coord->num_components));
   return true;
}

/* A texel of an snorm texture (sgx_tex_snorm), stored offset binary: the
 * unorm value v the TPU returns (filtered: the map is linear) is
 * (v * 255 - 128) / 127, clamped to -1 as GL clamps -128.  After
 * nir_lower_tex: the view's swizzle then reads the decoded texel, and its 0
 * and 1 are GL's own.  data: the units. */
static bool
lower_snorm_texel(nir_builder *b, nir_instr *instr, void *data)
{
   uint32_t units = *(const uint32_t *)data;

   if (instr->type != nir_instr_type_tex)
      return false;
   nir_tex_instr *tex = nir_instr_as_tex(instr);

   if (tex->texture_index >= 32 || !(units >> tex->texture_index & 1) ||
       nir_tex_instr_is_query(tex) || tex->is_shadow)
      return false;
   b->cursor = nir_after_instr(instr);
   unsigned bits = tex->def.bit_size;
   nir_def *v = nir_fmax(
      b,
      nir_ffma(b, &tex->def, nir_imm_floatN_t(b, 255.0 / 127.0, bits),
               nir_imm_floatN_t(b, -128.0 / 127.0, bits)),
      nir_imm_floatN_t(b, -1.0, bits));

   nir_def_rewrite_uses_after(&tex->def, v);
   return true;
}

/* the units a shader takes shadow samples of */
static uint32_t
shadow_units(nir_shader *nir)
{
   uint32_t units = 0;

   nir_foreach_function_impl (impl, nir) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_tex ||
                !nir_instr_as_tex(instr)->is_shadow)
               continue;
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            int d = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
            nir_variable *var = d >= 0 ? nir_deref_instr_get_variable(
                                            nir_src_as_deref(tex->src[d].src))
                                       : NULL;
            unsigned unit = var ? var->data.binding : tex->texture_index;

            if (unit < 32)
               units |= 1u << unit;
         }
      }
   }
   return units;
}

/* A shadow sample: the depth sampled as a value, compared in the shader
 * (the TPU doesn't filter the F32 depth, so a compare of the nearest texel)
 * with the sampler's function (compare: PIPE_FUNC + 1 by unit, 0 or NULL
 * LEQUAL, GL's default) and swizzled as the view says (swz, sgx_fs_for). */
static void
lower_shadow(nir_shader *nir, const uint8_t *compare, const uint16_t *swz)
{
   enum compare_func func[PIPE_MAX_SAMPLERS];
   nir_lower_tex_shadow_swizzle sw[PIPE_MAX_SAMPLERS];

   for (unsigned u = 0; u < PIPE_MAX_SAMPLERS; u++) {
      func[u] = compare && compare[u] ? (enum compare_func)(compare[u] - 1)
                                      : COMPARE_FUNC_LEQUAL;
      sw[u] = (nir_lower_tex_shadow_swizzle){PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y,
                                             PIPE_SWIZZLE_Z, PIPE_SWIZZLE_W};
      if (swz && swz[u])
         sw[u] = (nir_lower_tex_shadow_swizzle){
            swz[u] & 7, swz[u] >> 3 & 7, swz[u] >> 6 & 7, swz[u] >> 9 & 7};
   }
   /* (the reference clamped to [0, 1], as for the fixed-point depths) */
   NIR_PASS(_, nir, nir_lower_tex_shadow, PIPE_MAX_SAMPLERS, func, sw, true);
}

/* fs_vary: a vertex shader compiled to a fragment shader's sets; srgb: the
 * samplers whose texels a fragment shader decodes from sRGB, srgb_filter
 * those of them it filters itself (lower_srgb_filter) */
/* An integer colour buffer's word, which the PBE stores unchanged
 * (sgx_resource_target; the compiler's rt_packed): output 0's channels as
 * the colour word's B, G, R, A bytes for a target of 8 bits a channel, their
 * low bytes (PBE format 1 stores them R, G, B, A); for R32 the value
 * itself; for plane c of an RG32 or RGBA32 one, channel c (kind: the
 * variant's rt_int). */
static nir_def *
pack_word(nir_builder *b, nir_def *v, unsigned wm, unsigned kind)
{
   unsigned n = v->num_components;

   if (kind >= 2) {
      /* R32: the value; a plane of RG32 / RGBA32 (kind 3 + c): channel c */
      unsigned c = kind == 2 ? 0 : kind - 3;
      return c < n ? nir_channel(b, v, c) : nir_imm_int(b, 0);
   }
   static const unsigned shift[4] = {16, 8, 0, 24}; /* r, g, b, a */
   nir_def *word = nir_imm_int(b, 0);

   for (unsigned c = 0; c < n; c++) {
      if (!(wm >> c & 1))
         continue;
      word = nir_ior(b, word,
                     nir_ishl_imm(b, nir_iand_imm(b, nir_channel(b, v, c), 0xff),
                                  shift[c]));
   }
   return word;
}

/* The program as the compiler takes it still has its outputs as variables
 * (sgx_lower lowers I/O itself): output 0 is brought down to one store of
 * the variable at the end, which is returned (NULL: none) */
static nir_variable *
colour_output(nir_shader *nir)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_variable *out = NULL;

   NIR_PASS(_, nir, nir_lower_io_vars_to_temporaries, impl,
            nir_var_shader_out);
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_lower_var_copies);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   /* after the lowering: it makes the variable that was the output the
    * temporary and the output a new variable */
   nir_foreach_shader_out_variable (var, nir) {
      if (var->data.location == FRAG_RESULT_COLOR ||
          var->data.location == FRAG_RESULT_DATA0)
         out = var;
   }
   return out;
}

/* Output 0's store becomes one of the packed word, the variable a uint (the
 * compiler's rt_packed: one word, mask 1). */
static bool
pack_int_outputs(nir_shader *nir, unsigned kind)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_variable *out = colour_output(nir);

   if (!out || !glsl_type_is_32bit(glsl_without_array(out->type)))
      return false;

   nir_builder b = nir_builder_create(impl);
   bool progress = false;

   nir_foreach_block (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *st = nir_instr_as_intrinsic(instr);

         if (st->intrinsic != nir_intrinsic_store_deref ||
             nir_deref_instr_get_variable(nir_src_as_deref(st->src[0])) != out)
            continue;
         /* the new store goes in before the old one goes (the cursor is
          * the old one's place) */
         b.cursor = nir_before_instr(instr);
         nir_def *word = pack_word(&b, st->src[1].ssa,
                                   nir_intrinsic_write_mask(st), kind);
         out->type = glsl_uint_type();
         nir_store_deref(&b, nir_build_deref_var(&b, out), word, 0x1);
         nir_instr_remove(instr);
         progress = true;
      }
   }
   if (progress) {
      out->type = glsl_uint_type();
      NIR_PASS(_, nir, nir_opt_dce);
   }
   return progress;
}

/* channel c of a blend factor f (pipe_blendfactor) for source s, the tile's
 * colour d and the blend colour k, all in linear */
static nir_def *
blend_factor(nir_builder *b, unsigned f, nir_def *s, nir_def *d, nir_def *k,
             unsigned c)
{
   nir_def *v;

   switch (util_blendfactor_without_invert(f)) {
   case PIPE_BLENDFACTOR_SRC_COLOR:
      v = nir_channel(b, s, c);
      break;
   case PIPE_BLENDFACTOR_SRC_ALPHA:
      v = nir_channel(b, s, 3);
      break;
   case PIPE_BLENDFACTOR_DST_ALPHA:
      v = nir_channel(b, d, 3);
      break;
   case PIPE_BLENDFACTOR_DST_COLOR:
      v = nir_channel(b, d, c);
      break;
   case PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE:
      v = c == 3 ? nir_imm_float(b, 1.0f)
                 : nir_fmin(b, nir_channel(b, s, 3),
                            nir_fsub_imm(b, 1.0f, nir_channel(b, d, 3)));
      break;
   case PIPE_BLENDFACTOR_CONST_COLOR:
      v = nir_channel(b, k, c);
      break;
   case PIPE_BLENDFACTOR_CONST_ALPHA:
      v = nir_channel(b, k, 3);
      break;
   default: /* ONE, and ZERO inverted (no dual source: none is exposed) */
      v = nir_imm_float(b, 1.0f);
      break;
   }
   return util_blendfactor_is_inverted(f) ? nir_fsub_imm(b, 1.0f, v) : v;
}

static nir_def *
blend_func(nir_builder *b, unsigned func, nir_def *s, nir_def *fs, nir_def *d,
           nir_def *fd)
{
   switch (func) {
   case PIPE_BLEND_SUBTRACT:
      return nir_fsub(b, nir_fmul(b, s, fs), nir_fmul(b, d, fd));
   case PIPE_BLEND_REVERSE_SUBTRACT:
      return nir_fsub(b, nir_fmul(b, d, fd), nir_fmul(b, s, fs));
   case PIPE_BLEND_MIN:
      return nir_fmin(b, s, d);
   case PIPE_BLEND_MAX:
      return nir_fmax(b, s, d);
   default:
      return nir_fadd(b, nir_fmul(b, s, fs), nir_fmul(b, d, fd));
   }
}

/* An sRGB colour buffer (EXT_sRGB, EXT_framebuffer_sRGB; the PBE writes the
 * word it gets, docs/GL3.md): output 0's colour encoded from linear before it
 * goes out.  Blended (rt_srgb 2), the blend is done here instead of by the
 * sop2 epilogue, which would mix encoded bytes: the tile's word (o0, a load
 * of the output: the compiler's rt_packed) decoded, blended in linear with
 * the clamped colour, encoded and packed B, G, R, A again. */
static bool
lower_srgb_output(nir_shader *nir, const struct sgx_pipe_shader *so)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_variable *out = colour_output(nir);
   bool blend = so->rt_srgb == 2;

   if (!out || !glsl_type_is_32bit(glsl_without_array(out->type)) ||
       glsl_get_base_type(glsl_without_array(out->type)) != GLSL_TYPE_FLOAT)
      return false;

   nir_builder b = nir_builder_create(impl);
   bool progress = false;

   nir_foreach_block (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *st = nir_instr_as_intrinsic(instr);

         if (st->intrinsic != nir_intrinsic_store_deref ||
             nir_deref_instr_get_variable(nir_src_as_deref(st->src[0])) != out)
            continue;
         b.cursor = nir_before_instr(instr);
         nir_def *v = st->src[1].ssa;
         unsigned n = v->num_components, wm = nir_intrinsic_write_mask(st);
         nir_def *ch[4];

         for (unsigned c = 0; c < 4; c++)
            ch[c] = c < n ? nir_fsat(&b, nir_channel(&b, v, c))
                          : nir_imm_float(&b, c == 3 ? 1.0f : 0.0f);
         if (!blend) {
            /* the colour's channels, R, G, B encoded, as the store had them */
            nir_def *rgb =
               nir_format_linear_to_srgb(&b, nir_vec(&b, ch, 3));

            for (unsigned c = 0; c < MIN2(n, 3); c++)
               ch[c] = nir_channel(&b, rgb, c);
            nir_store_deref(&b, nir_build_deref_var(&b, out),
                            nir_vec(&b, ch, n), wm);
            nir_instr_remove(instr);
            progress = true;
            continue;
         }
         nir_def *s = nir_vec(&b, ch, 4);
         out->type = glsl_uint_type();
         /* the tile's word as the compiler reads it (o0): a load_output
          * itself, as a load of the variable would read the temporary
          * nir_lower_io_vars_to_temporaries makes of it, undefined here */
         nir_intrinsic_instr *lo =
            nir_intrinsic_instr_create(b.shader, nir_intrinsic_load_output);
         nir_io_semantics sem = {0};

         lo->num_components = 1;
         lo->src[0] = nir_src_for_ssa(nir_imm_int(&b, 0));
         sem.location = out->data.location;
         sem.num_slots = 1;
         nir_intrinsic_set_io_semantics(lo, sem);
         nir_def_init(&lo->instr, &lo->def, 1, 32);
         nir_builder_instr_insert(&b, &lo->instr);
         nir_def *tile = &lo->def;
         static const unsigned shift[4] = {16, 8, 0, 24}; /* r, g, b, a */
         nir_def *dc[4];

         for (unsigned c = 0; c < 4; c++)
            dc[c] = nir_fmul_imm(
               &b,
               nir_u2f32(&b, nir_iand_imm(&b, nir_ushr_imm(&b, tile, shift[c]),
                                          0xff)),
               1.0f / 255.0f);
         nir_def *dl = nir_format_srgb_to_linear(&b, nir_vec(&b, dc, 3));
         nir_def *d = nir_vec4(&b, nir_channel(&b, dl, 0),
                               nir_channel(&b, dl, 1), nir_channel(&b, dl, 2),
                               dc[3]);
         nir_def *k = nir_imm_vec4(&b, so->rt_blend_color[0],
                                   so->rt_blend_color[1], so->rt_blend_color[2],
                                   so->rt_blend_color[3]);
         const struct pipe_rt_blend_state *rb = &so->rt_blend;
         nir_def *r[4];

         for (unsigned c = 0; c < 4; c++) {
            bool a = c == 3;
            unsigned func = a ? rb->alpha_func : rb->rgb_func;
            unsigned fsrc = a ? rb->alpha_src_factor : rb->rgb_src_factor;
            unsigned fdst = a ? rb->alpha_dst_factor : rb->rgb_dst_factor;

            r[c] = nir_fsat(&b, blend_func(&b, func, nir_channel(&b, s, c),
                                           blend_factor(&b, fsrc, s, d, k, c),
                                           nir_channel(&b, d, c),
                                           blend_factor(&b, fdst, s, d, k, c)));
         }
         nir_def *rgb = nir_format_linear_to_srgb(&b, nir_vec(&b, r, 3));
         nir_def *enc = nir_vec4(&b, nir_channel(&b, rgb, 0),
                                 nir_channel(&b, rgb, 1),
                                 nir_channel(&b, rgb, 2), r[3]);
         static const unsigned bits[4] = {8, 8, 8, 8};
         nir_def *word =
            pack_word(&b, nir_format_float_to_unorm(&b, enc, bits), 0xf, 1);

         nir_store_deref(&b, nir_build_deref_var(&b, out), word, 0x1);
         nir_instr_remove(instr);
         progress = true;
      }
   }
   if (progress) {
      if (blend)
         out->type = glsl_uint_type();
      NIR_PASS(_, nir, nir_opt_dce);
   }
   return progress;
}

/* MRT, a pass a colour buffer: the pass for buffer t keeps FRAG_RESULT_DATA0
 * + t as output 0 and drops the other DATA outputs (gl_FragColor stays: it
 * writes every buffer) */
static void
remap_outputs(nir_shader *nir, unsigned t)
{
   bool demoted = false;

   /* gl_FragData is one array variable at FRAG_RESULT_DATA0: an element a
    * variable first, each at its own location */
   NIR_PASS(_, nir, nir_lower_io_array_vars_to_elements_no_indirects, true);

   nir_foreach_shader_out_variable (var, nir) {
      int loc = var->data.location;

      if (loc < FRAG_RESULT_DATA0 || loc >= FRAG_RESULT_DATA0 + 8)
         continue;
      if (loc == FRAG_RESULT_DATA0 + (int)t) {
         var->data.location = FRAG_RESULT_DATA0;
      } else {
         var->data.mode = nir_var_shader_temp;
         demoted = true;
      }
   }
   if (demoted) {
      nir_fixup_deref_modes(nir);
      NIR_PASS(_, nir, nir_lower_vars_to_ssa);
      NIR_PASS(_, nir, nir_opt_dce);
      NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_shader_temp, NULL);
   }
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
}

/* nir_lower_clip_vs stores CLIP_DIST0 (planes 0-3) only when one of them is
 * enabled, CLIP_DIST1 (4-7) only when one of those is.  The compiler counts
 * the planes from the variables (0..n-1, the TA's output select), so with
 * planes 4 and 5 alone it enables 0-5 and planes 0-3 read whatever cp
 * registers held: those distances written 0.0 (inside), as the lowering
 * itself writes a disabled plane below the last one. */
static void
fill_clip_dist0(nir_shader *nir)
{
   bool d0 = false, d1 = false;

   nir_foreach_shader_out_variable (var, nir) {
      d0 |= var->data.location == VARYING_SLOT_CLIP_DIST0;
      d1 |= var->data.location == VARYING_SLOT_CLIP_DIST1;
   }
   if (!d1 || d0)
      return;
   nir_variable *v = nir_variable_create(nir, nir_var_shader_out,
                                         glsl_vec4_type(), "sgx_clipdist0");
   v->data.location = VARYING_SLOT_CLIP_DIST0;
   nir_builder b = nir_builder_at(nir_after_impl(nir_shader_get_entrypoint(nir)));
   nir_store_var(&b, v, nir_imm_vec4(&b, 0, 0, 0, 0), 0xf);
   nir->info.outputs_written |= BITFIELD64_BIT(VARYING_SLOT_CLIP_DIST0);
}

/* A non-dependent sample (the PDS samples it, the compiler's
 * sample_only_load): a plain one whose coordinate is a varying as loaded,
 * every use of that load a sample of the same kind.  Otherwise the program
 * samples it (dependent), the TPU taking its LOD from the quad. */
static bool
pds_sample(nir_tex_instr *tex)
{
   int ci = nir_tex_instr_src_index(tex, nir_tex_src_coord);

   if (tex->op != nir_texop_tex || tex->is_array || ci < 0 ||
       nir_tex_instr_src_index(tex, nir_tex_src_bias) >= 0 ||
       nir_tex_instr_src_index(tex, nir_tex_src_projector) >= 0)
      return false;
   nir_instr *pi = nir_def_instr(tex->src[ci].src.ssa);
   if (pi->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *ld = nir_instr_as_intrinsic(pi);
   /* (before nir_lower_io, the compiler's, a varying is a load_deref of an
    * input variable) */
   bool input = ld->intrinsic == nir_intrinsic_load_input ||
                ld->intrinsic == nir_intrinsic_load_interpolated_input;
   if (ld->intrinsic == nir_intrinsic_load_deref) {
      nir_deref_instr *d = nir_src_as_deref(ld->src[0]);
      input = d && d->deref_type == nir_deref_type_var &&
              d->var->data.mode == nir_var_shader_in;
   }
   if (!input || ld->def.num_components != tex->coord_components)
      return false;
   nir_foreach_use_including_if (u, &ld->def) {
      if (nir_src_is_if(u))
         return false;
      nir_instr *ui = nir_src_use_instr(u);
      if (ui->type != nir_instr_type_tex)
         return false;
      nir_tex_instr *t = nir_instr_as_tex(ui);
      int tci = nir_tex_instr_src_index(t, nir_tex_src_coord);
      if (t->op != nir_texop_tex || tci < 0 || u != &t->src[tci].src ||
          nir_tex_instr_src_index(t, nir_tex_src_bias) >= 0 ||
          nir_tex_instr_src_index(t, nir_tex_src_projector) >= 0 ||
          t->coord_components != ld->def.num_components)
         return false;
   }
   return true;
}

/* the units a sample may read: its texture_index, or before the samplers
 * are lowered its variable's binding (an array of them indexed by a value:
 * all of its units) */
static uint32_t
tex_units(nir_tex_instr *tex)
{
   int d = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);

   if (d < 0)
      return tex->texture_index < 32 ? 1u << tex->texture_index : 0;
   nir_deref_instr *deref = nir_src_as_deref(tex->src[d].src);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   if (!var)
      return ~0u;
   unsigned first = var->data.binding, n = 1;
   if (deref->deref_type == nir_deref_type_array) {
      if (nir_src_is_const(deref->arr.index))
         first += nir_src_as_uint(deref->arr.index);
      else
         n = MAX2(glsl_get_aoa_size(var->type), 1);
   }
   return first < 32 ? BITFIELD_RANGE(first, MIN2(n, 32 - first)) : 0;
}

/* an implicit-LOD 2D or cube sample the program makes itself */
static bool
dep_lod_sample(nir_tex_instr *tex)
{
   return (tex->op == nir_texop_tex || tex->op == nir_texop_txb) &&
          (tex->sampler_dim == GLSL_SAMPLER_DIM_2D ||
           (tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE && !tex->is_array)) &&
          nir_tex_instr_src_index(tex, nir_tex_src_coord) >= 0 &&
          nir_tex_instr_src_index(tex, nir_tex_src_projector) < 0 &&
          nir_tex_instr_src_index(tex, nir_tex_src_texture_handle) < 0 &&
          !pds_sample(tex);
}

/* the units a fragment shader's implicit-LOD dependent samples read */
static uint32_t
dep_lod_units(nir_shader *nir)
{
   uint32_t units = 0;

   if (nir->info.stage != MESA_SHADER_FRAGMENT)
      return 0;
   nir_foreach_function_impl (impl, nir) {
      nir_foreach_block (b, impl) {
         nir_foreach_instr (instr, b) {
            if (instr->type == nir_instr_type_tex &&
                dep_lod_sample(nir_instr_as_tex(instr)))
               units |= tex_units(nir_instr_as_tex(instr));
         }
      }
   }
   return units;
}

/* An implicit-LOD dependent sample of a mipmapped texture takes the wrong
 * LOD in a quad its primitive covers only in part: the TPU seems to take it
 * from the covered pixels' coordinates alone (measured: a texture2D(s,
 * uv * one) against texture2D(s, uv), NEAREST_MIPMAP_NEAREST, differs in
 * the 128 pixels of the quads on a quad's diagonal, and nowhere else; the
 * helpers hold the coordinate, dFdx of it is the same everywhere).  With
 * the gradients from fdsx / fdsy (coarse: every pixel of the quad gets the
 * same) it matches the PDS sample: the sample becomes a txd, which smp
 * takes as it is (LOD mode 3, hwt_grad).  A bias scales the gradients by
 * 2^bias.  Not in divergent flow, where derivatives are undefined; only on
 * the units in data (dep_mip).
 *
 * A cube sample does the same (measured: textureCube(s, d * one) takes
 * level 0 in the quads on the diagonal, mesa/tests/sgx_cubelod.c), but smp
 * takes no cube gradients (ISA.md, hwt_cubegrad: the 3D form returns the
 * last level, the 2D one reads the direction's x and y derivatives
 * unprojected).  So its LOD is computed here and the sample becomes a txl:
 * log2 of the longer derivative of the direction times the face's width
 * over 2|ma|, the face's major axis.  That leaves out the change of ma
 * across the quad, which nir_lower_tex's exact cube gradients keep; it
 * takes half their instructions (25 against 50 for a textureCube), and
 * dEQP-GLES2's 557 cube cases give the same result with either. */
static bool
lower_dep_lod(nir_builder *b, nir_tex_instr *tex, void *data)
{
   uint32_t units = *(const uint32_t *)data;

   if (!dep_lod_sample(tex) || !(units & tex_units(tex)) ||
       tex->instr.block->divergent)
      return false;
   b->cursor = nir_before_instr(&tex->instr);
   int ci = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   bool cube = tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE;
   nir_def *coord = nir_trim_vector(b, tex->src[ci].src.ssa, cube ? 3 : 2);

   /* (a cube's maths in 32 bits: a mediump direction's would mix 16-bit
    * constants into 32-bit instructions) */
   if (cube && coord->bit_size != 32) {
      coord = nir_f2f32(b, coord);
      nir_src_rewrite(&tex->src[ci].src, coord);
   }
   nir_def *dx = nir_ddx_coarse(b, coord), *dy = nir_ddy_coarse(b, coord);
   int bi = nir_tex_instr_src_index(tex, nir_tex_src_bias);

   if (bi >= 0) {
      nir_def *s = nir_fexp2(b, nir_f2f32(b, tex->src[bi].src.ssa));

      dx = nir_fmul(b, dx, s);
      dy = nir_fmul(b, dy, s);
      nir_tex_instr_remove_src(tex, bi);
   }
   if (cube) {
      nir_def *a = nir_fabs(b, coord);
      nir_def *ma = nir_fmax(b, nir_fmax(b, nir_channel(b, a, 0),
                                         nir_channel(b, a, 1)),
                             nir_channel(b, a, 2));
      /* level 0's width: the draw's float constant when the unit is known
       * (txs's would go through an integer and back) */
      nir_def *size =
         tex->texture_index < SGX_K_TEXSIZE_UNITS &&
               nir_tex_instr_src_index(tex, nir_tex_src_texture_deref) < 0
            ? nir_load_uniform(b, 1, 32, nir_imm_int(b, 0),
                               .base = SGX_K_TEXSIZE + 2 * tex->texture_index,
                               .range = 1, .dest_type = nir_type_float32)
            : nir_i2f32(b, nir_channel(b, nir_get_texture_size(b, tex), 0));
      nir_def *rho = nir_fmax(b, nir_fdot(b, dx, dx), nir_fdot(b, dy, dy));
      nir_def *k = nir_fmul(b, size, nir_frcp(b, nir_fmul_imm(b, ma, 2.0)));
      nir_def *lod = nir_fmul_imm(
         b, nir_flog2(b, nir_fmul(b, rho, nir_fmul(b, k, k))), 0.5);

      tex->op = nir_texop_txl;
      nir_tex_instr_add_src(tex, nir_tex_src_lod, lod);
      return true;
   }
   tex->op = nir_texop_txd;
   nir_tex_instr_add_src(tex, nir_tex_src_ddx, dx);
   nir_tex_instr_add_src(tex, nir_tex_src_ddy, dy);
   return true;
}

static bool
compile_shader(struct sgx_context *ctx, struct sgx_pipe_shader *so,
               nir_shader *orig, const struct sgx_varyings *fs_vary,
               unsigned srgb, uint32_t srgb_filter, const uint16_t *swz,
               uint32_t not_bgra, const uint8_t *vtx_fmt, bool points,
               uint32_t depth, const uint8_t *compare, uint32_t dep_mip)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   nir_shader *nir = nir_shader_clone(NULL, orig);
   struct sgx_compile_options opts = {0};
   struct sgx_varyings vary;
   struct sgx_shader out;
   struct prog_info info = {0};
   bool vertex = nir->info.stage == MESA_SHADER_VERTEX;

   opts.verbose = (sgx_drv_debug & SGX_DEBUG_NIR) != 0;
   /* The point size (st gives every vertex program one) is an output only
    * for points: elsewhere it costs a dword a vertex in the parameter buffer
    * and rounds the vertex up to the next four (the DDK's triangles carry
    * none) */
   if (vertex && !points) {
      bool demoted = false;

      nir_foreach_shader_out_variable (var, nir) {
         if (var->data.location == VARYING_SLOT_PSIZ) {
            var->data.mode = nir_var_shader_temp;
            demoted = true;
         }
      }
      if (demoted) {
         nir_fixup_deref_modes(nir);
         NIR_PASS(_, nir, nir_lower_vars_to_ssa);
         NIR_PASS(_, nir, nir_opt_dce);
         NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_shader_temp, NULL);
      }
   }
   opts.f16 = ctx->base.screen->shader_caps[nir->info.stage].fp16;
   /* list C kinds 0x10 and 0x11 (fill_consts): lowp colours pre-converted to
    * F16 and literals in SA, where libusc converts in a secondary program */
   opts.driver_consts = true;
   /* GLSL 1.30's integers, once the screen has them: the cap is the
    * screen's, so GLSL ES 1.00 programs come with integers then too */
   opts.native_integers = so->native_integers =
      screen->base.shader_caps[nir->info.stage].integers;
   /* uniform block n's address in constant SGX_K_UBO + n (fill_consts) */
   if (nir->info.num_ubos)
      opts.ubo_base_k = SGX_K_UBO;
   if (vtx_fmt) {
      memcpy(opts.vtx_fmt, vtx_fmt, sizeof(opts.vtx_fmt));
      memcpy(so->vtx_fmt, vtx_fmt, sizeof(so->vtx_fmt));
   }
   /* st's fixed-function fragment programs only owe the framebuffer's
    * precision: mediump (texture coordinates stay F32); dEQP-GLES2 mustpass
    * and piglit unchanged with it and F16 samples (CE5310 board, 2026-11-10) */
   /* the units sampling 8888 texels get them B, G, R, A ordered
    * (sgx_tex_format): all of them unless the variant says otherwise */
   /* a unit sampled for a compare holds a depth texture */
   uint32_t shadow = shadow_units(nir);
   /* (a vertex program's samples too, SGX_DEBUG=vtf: its units' variant from
    * sgx_vs_for) */
   opts.tex_bgra = ~not_bgra & ~(depth | shadow);
   opts.tex_f32 = depth | shadow;
   /* RGBA32 and RG32: a read of each channel's R32 plane (each read's
    * plane in its sample info, sgx_draw.c adds it to the address) */
   opts.tex_r32x4 = so->r32x4;
   opts.tex_r32x2 = so->r32x2;
   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      /* a coordinate only samples read: the PDS samples from the set
       * itself (a pure-sample DOUTI, source 0x24, below) */
      opts.pds_samples_alone = screen->core->pds_samples;
   }
   /* texels a mediump read takes come back F16 (the sample's T3, below) */
   opts.f16_samples = opts.f16 && nir->info.stage == MESA_SHADER_FRAGMENT &&
                      screen->core->f16_sample_return &&
                      !(sgx_drv_debug & SGX_DEBUG_NO_F16_SAMPLES);
   /* and those of dependent samples (sgx_draw.c sets their T3) */
   opts.dep_f16_samples =
      opts.f16_samples && !(sgx_drv_debug & SGX_DEBUG_NO_DEP_F16_SAMPLES);
   opts.fs_mediump = opts.f16 && nir->info.stage == MESA_SHADER_FRAGMENT &&
                     nir->info.name && !strcmp(nir->info.name, "ff-fs") &&
                     !(sgx_drv_debug & SGX_DEBUG_NO_FF_MEDIUMP);
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, rescale_uniform,
            nir_metadata_control_flow, NULL);
   {
      /* a 1D texture is a 2D one a texel high (sgx_resource): its samples
       * become 2D ones on that row; a rectangle texture's coordinates are
       * divided by its size (lower_txs), which st's fixed-function programs
       * leave to the driver (TFP's rectangle binding: xfwm4's compositor) */
      nir_lower_tex_options tex = {.lower_1d = true,
                                   .lower_srgb = srgb,
                                   .lower_rect = true};

      /* a sampler view's swizzle, as the texel comes out (sgx_fs_for); a
       * shadow sample's goes on its result (lower_shadow) */
      if (shadow)
         lower_shadow(nir, compare, swz);
      for (unsigned u = 0; swz && u < PIPE_MAX_SAMPLERS; u++) {
         if (!swz[u] || (shadow >> u & 1))
            continue;
         tex.swizzle_result |= 1u << u;
         for (unsigned c = 0; c < 4; c++)
            tex.swizzles[u][c] = swz[u] >> (3 * c) & 7;
      }
      if (srgb_filter)
         NIR_PASS(_, nir, nir_shader_instructions_pass, lower_srgb_filter,
                  nir_metadata_control_flow, &srgb_filter);
      if (so->wrap_rep | so->wrap_mir)
         NIR_PASS(_, nir, nir_shader_instructions_pass, lower_word32_wrap,
                  nir_metadata_control_flow, so);
      if (!(sgx_drv_debug & SGX_DEBUG_TEX3D))
         NIR_PASS(_, nir, nir_shader_instructions_pass, lower_tex_3d,
                  nir_metadata_control_flow, NULL);
      NIR_PASS(_, nir, nir_lower_tex, &tex);
      /* after it: the swizzle's uses of the texel become the decoded one's,
       * its 0 and 1 stay GL's */
      if (so->snorm)
         NIR_PASS(_, nir, nir_shader_instructions_pass, lower_snorm_texel,
                  nir_metadata_control_flow, &so->snorm);
   }
   if (dep_mip && !vertex) {
      nir_lower_tex_options txd = {.lower_txd_cb = sgx_txd_lowered,
                                   .lower_txd_clamp = true};

      nir_divergence_analysis(nir);
      NIR_PASS(_, nir, nir_shader_tex_pass, lower_dep_lod,
               nir_metadata_control_flow, &dep_mip);
      /* (sgx_lower_tex lowers the others with native integers) */
      if (!opts.native_integers)
         NIR_PASS(_, nir, nir_lower_tex, &txd);
   }
   /* textureGrad, the Offset variants and texelFetch as smp can do them;
    * the sizes they ask for are lower_txs's */
   if (opts.native_integers) {
      NIR_PASS(_, nir, sgx_lower_tex, &opts);
      /* (after it: a texelFetch of an array is a txl at the texel's centre
       * by then, its layer the coordinate's z) */
      if (!vertex) {
         /* (which layer coordinates may differ between pixels) */
         nir_divergence_analysis(nir);
         NIR_PASS(_, nir, nir_shader_tex_pass, lower_tex_array,
                  nir_metadata_control_flow, NULL);
      }
   }
   /* (a vertex program's sizes from its own units, fill_consts) */
   NIR_PASS(_, nir, nir_shader_instructions_pass, lower_txs,
            nir_metadata_control_flow, &opts.native_integers);
   if (vertex) {
      struct vertex_id_data vid = {.ints = opts.native_integers};

      NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_instance_id,
               nir_metadata_control_flow, &opts.native_integers);
      NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_vertex_id,
               nir_metadata_control_flow, &vid);
      so->vertex_id_slot = vid.var ? vid.var->data.driver_location + 1 : 0;
      so->vertex_id_int = vid.ints;
   }
   NIR_PASS(_, nir, nir_opt_dce);
   remap_legacy_varyings(nir);
   /* (names the variables the way the list does) */
   assign_varyings(nir, &vary, false);
   bool sprite = fs_vary && fs_vary->point_size;
   if (fs_vary) {
      bool psize = vary.point_size;

      vary = *fs_vary;
      vary.point_size = sprite || psize;
      /* Interpolation is the fragment shader's to say: a TGSI vertex
       * shader's outputs (u_blitter's, st's) carry none, and one left
       * perspective under a noperspective input never wrote the w the
       * fragment shader divides by, so every pixel read the same texel (a
       * blit from an FBO to the window one colour: OpenArena's
       * renderer_opengl2, issue #5) */
      nir_foreach_shader_out_variable (var, nir) {
         for (unsigned i = 0; var->name && i < fs_vary->n; i++)
            if (fs_vary->v[i].noperspective &&
                !strcmp(var->name, fs_vary->v[i].name))
               var->data.interpolation = INTERP_MODE_NOPERSPECTIVE;
      }
   }
   so->reads_pointcoord =
      nir->info.stage == MESA_SHADER_FRAGMENT && vary.point_size;
   if (!so->vary)
      so->vary = MALLOC_STRUCT(sgx_varyings);
   if (so->vary)
      *so->vary = fs_vary ? *fs_vary : vary;
   struct sgx_layout layout = {layout_place, layout_first_free, nir};

   /* gl_FrontFacing against the winding gallium calls front, sgx_draw.c writes
    * it */
   opts.front_sa = SGX_SA_FRONT;

   opts.layout = &layout;
   if (vertex) {
      /* a matrix attribute arrives as a variable per column, all with the
       * matrix's name: name the later columns apart so each one gets its own
       * slot */
      nir_foreach_shader_in_variable (var, nir) {
         nir_foreach_shader_in_variable (prev, nir) {
            if (prev == var)
               break;
            if (var->name && prev->name && !strcmp(var->name, prev->name)) {
               /* (a variable isn't a ralloc context: nir_variable_create
                * takes it from the shader's gc allocator; set_namef frees
                * the old name first, so it's copied out) */
               char name[256];

               snprintf(name, sizeof(name), "%s", var->name);
               nir_variable_set_namef(nir, var, "%s#%d", name,
                                      var->data.driver_location);
               break;
            }
         }
      }
      nir_foreach_shader_in_variable (var, nir) {
         if (var->name)
            so->n_attribs = MAX2(
               so->n_attribs, var->data.driver_location +
                                 glsl_count_attribute_slots(var->type, true));
      }
   }
   if (sgx_drv_debug & SGX_DEBUG_NIR)
      nir_print_shader(nir, stderr);
   /* a discarding fragment program in two phases, as the DDK runs its
    * alpha-tested ones: the test before the visibility feedback, the colour
    * after it for the pixels that pass (out.code1); the compiler decides */
   opts.split_phases = !vertex && !(sgx_drv_debug & SGX_DEBUG_NO_PHASE_SPLIT);
   if (vertex)
      fill_clip_dist0(nir);
   /* MRT: this variant's colour buffer's output as output 0 */
   if (!vertex && so->rt_target)
      remap_outputs(nir, so->rt_target - 1);
   /* an integer colour buffer: output 0 packed for it, as one word */
   if (!vertex && so->rt_int) {
      unsigned kind = so->rt_int;

      if (pack_int_outputs(nir, kind))
         opts.rt_packed = 1;
      if (sgx_drv_debug & SGX_DEBUG_NIR) {
         fprintf(stderr, "sgx: after pack_int_outputs (kind %u):\n", kind);
         nir_print_shader(nir, stderr);
      }
   }
   /* an sRGB colour buffer: output 0 encoded, or blended and packed */
   if (!vertex && so->rt_srgb) {
      so->reads_tile = lower_srgb_output(nir, so) && so->rt_srgb == 2;
      if (so->reads_tile)
         opts.rt_packed = 1;
      if (sgx_drv_debug & SGX_DEBUG_NIR) {
         fprintf(stderr, "sgx: after lower_srgb_output (%u):\n", so->rt_srgb);
         nir_print_shader(nir, stderr);
      }
   }
   if (sgx_compile(nir, &opts, &vary, &out)) {
      fprintf(stderr, "sgx: %s shader did not compile:\n%s\n",
              vertex ? "vertex" : "fragment", out.log ? out.log : "");
      sgx_shader_free(&out);
      ralloc_free(nir);
      return false;
   }
   if (sgx_dump_code_dir()) {
      static unsigned seq;
      char path[512];
      FILE *fp;

      /* "hi lo" hex per instruction, as sgxc's .code (dis.py -x reads the
       * pairs) */
      snprintf(path, sizeof(path), "%s/%s-%u.code", sgx_dump_code_dir(),
               vertex ? "vs" : "fs", seq);
      if ((fp = fopen(path, "w"))) {
         for (unsigned i = 0; i < out.n_code; i++)
            fprintf(fp, "%08x %08x\n", (unsigned)(out.code[i] >> 32),
                    (unsigned)out.code[i]);
         fclose(fp);
      }
      /* a split program's phase 1, as sgxc -P names it */
      snprintf(path, sizeof(path), "%s/fs-%u.code1", sgx_dump_code_dir(), seq);
      if (out.n_code1 && (fp = fopen(path, "w"))) {
         for (unsigned i = 0; i < out.n_code1; i++)
            fprintf(fp, "%08x %08x\n", (unsigned)(out.code1[i] >> 32),
                    (unsigned)out.code1[i]);
         fclose(fp);
      }
      seq++;
   }
   /* phase 1 is straight-line (the compiler splits only such programs): no
    * branch offsets to move there */
   for (unsigned i = 0; i < out.n_blocks1; i++) {
      if (out.blocks1[i].kind == SGX_BLOCK_BRANCH) {
         info.why = "a branch in phase 1";
         out.n_code1 = 0;
         break;
      }
   }
   if (info.why || !read_program(&out, so, &info)) {
      fprintf(stderr, "sgx: %s shader: not handled yet (%s)\n",
              vertex ? "vertex" : "fragment", info.why);
      sgx_shader_free(&out);
      ralloc_free(nir);
      return false;
   }

   {
      /* An instruction names an SA register in 7 bits: a program's constants
       * (from sa9, sa10 in a vertex program) and the texture state of its
       * dependent samples (after them) end at sa127.  Past it the numbers wrap,
       * onto registers the program never loaded, and the 3D locks up (piglit
       * glsl-uniform-interstage-limits).  The compiler moves constants that
       * don't fit to memory (mem_dwords), so this only catches a program
       * whose SA use still runs past (a guard, not drawn). */
      unsigned end = (vertex ? 10 : 9) + MAX2(so->const_dwords, out.sa_slots) +
                     4 * info.n_states;

      if (end > 128) {
         fprintf(
            stderr,
            "sgx: %s shader needs SA registers up to sa%u (sa127 is the last): not drawn\n",
            vertex ? "vertex" : "fragment", end - 1);
         sgx_shader_free(&out);
         ralloc_free(nir);
         return false;
      }
   }

   uint32_t old_code = so->prog.code;

   memset(&so->prog, 0, sizeof(so->prog));
   simple_mtx_lock(&screen->lock);
   sgxws_code_free(ctx->hw, old_code);
   if (vertex) {
      unsigned dwords = 4, tsp[SGX_MAX_SETS] = {0};

      so->prog.vertex = true;
      if (info.n_deps) {
         /* vertex texture fetch: each texture state's 4 words in SA after
          * every register the program uses, as a fragment program's (the
          * draw writes them, sgx_draw.c), and the smps put in */
         unsigned sa[SGX_MAX_DEP_SAMPLES], n;
         uint64_t *code;

         so->const_dwords = MAX2(so->const_dwords, out.sa_slots);
         for (unsigned k = 0; k < info.n_states; k++)
            so->dep_sa[k] = 10 + so->const_dwords + 4 * k;
         for (unsigned j = 0; j < info.n_deps; j++) {
            unsigned k = info.deps[j].state;

            so->dep_sampler[k] = info.deps[j].sampler;
            so->dep_f16[k] = info.deps[j].f16;
            so->dep_fetch[k] = info.deps[j].fetch;
            so->dep_plane[k] = info.deps[j].plane;
            sa[j] = so->dep_sa[k];
         }
         so->n_deps = info.n_states;
         so->const_dwords += 4 * info.n_states;
         code = insert_samples(out.code, out.n_code, &info, sa, &n);
         so->prog.code =
            code ? sgxws_upload_vs(ctx->hw, code, n, out.n_temps) : 0;
         free(code);
      } else {
         so->prog.code =
            sgxws_upload_vs(ctx->hw, out.code, out.n_code, out.n_temps);
      }
      so->prog.in_dwords = so->prog.attr_dwords = 4 * MAX2(so->n_attribs, 1);
      for (unsigned s = 0; s < SGX_MAX_SETS; s++) {
         unsigned n = vary.set_comps[s];

         if (!n)
            continue;
         dwords += n;
         /* TSP word 0: 1, 2, 3, or 5 for 4 components (STATE.md section 3) */
         tsp[s] = n == 4 ? 5 : n;
      }
      STATIC_ASSERT(SGX_MAX_SETS == 10);
      sgx_pack(&so->prog.tsp_sizes, TA_STATE_TSP0, t0)
      {
         t0.set0 = tsp[0];
         t0.set1 = tsp[1];
         t0.set2 = tsp[2];
         t0.set3 = tsp[3];
         t0.set4 = tsp[4];
         t0.set5 = tsp[5];
         t0.set6 = tsp[6];
         t0.set7 = tsp[7];
         t0.set8 = tsp[8];
         t0.set9 = tsp[9];
      }
      so->prog.point_size = vary.point_size;
      so->prog.colour = vary.colour;
      /* TSP word 1 stays 0, as the DDK's for a sampled set: with a set's bit
       * in it, the TPU samples at a constant coordinate; the DDK's for a
       * sprite's coordinate set (`sprite`) is 1 */
      sgx_pack(&so->prog.tsp_prec, TA_STATE_TSP1, t1) t1.f16 = sprite ? 1 : 0;
      /* user clip planes 0..n_clip-1 (st's clip distances), their results
       * after the point size */
      so->prog.clip_planes = BITFIELD_MASK(MIN2(out.n_clip, 6));
      so->prog.out_dwords = dwords + (vary.point_size ? 1 : 0) + out.n_clip;
      so->prog.const_dwords = so->const_dwords;
   } else {
      /* dependent samples: their state words go in SA after the constants */
      if (info.n_deps) {
         unsigned sa[SGX_MAX_DEP_SAMPLES], n;

         if (MAX2(so->const_dwords, out.sa_slots) + 4 * info.n_states >
             SGX_MAX_CONSTS) {
            fprintf(stderr,
                    "sgx: %u SA slots and %u dependent texture states: not drawn\n",
                    out.sa_slots, info.n_states);
            simple_mtx_unlock(&screen->lock);
            sgx_shader_free(&out);
            ralloc_free(nir);
            return false;
         }
         /* after every SA register the program uses: its constants (list C)
          * and the slots the compiler keeps for itself */
         if (out.sa_slots > so->const_dwords)
            SGX_DBG(NIR, "fragment program: %u SA slots, %u in list C\n",
                    out.sa_slots, so->const_dwords);
         so->const_dwords = MAX2(so->const_dwords, out.sa_slots);
         /* one block of 4 words per texture state, the dependent samples
          * that share it reading the same registers */
         for (unsigned k = 0; k < info.n_states; k++)
            so->dep_sa[k] = 9 + so->const_dwords + 4 * k;
         for (unsigned j = 0; j < info.n_deps; j++) {
            unsigned k = info.deps[j].state;

            so->dep_sampler[k] = info.deps[j].sampler;
            so->dep_f16[k] = info.deps[j].f16;
            so->dep_fetch[k] = info.deps[j].fetch;
            so->dep_plane[k] = info.deps[j].plane;
            sa[j] = so->dep_sa[k];
         }
         so->n_deps = info.n_states;
         so->const_dwords += 4 * info.n_states;
         if (out.n_code1) {
            /* each phase's dependent samples into its own block */
            struct prog_info i0 = info, i1 = info;
            unsigned sa0[SGX_MAX_DEP_SAMPLES], sa1[SGX_MAX_DEP_SAMPLES], n1;

            i0.n_deps = i1.n_deps = 0;
            i1.n_branches = 0;
            for (unsigned j = 0; j < info.n_deps; j++) {
               struct prog_info *pi = info.deps[j].phase ? &i1 : &i0;
               unsigned *ps = info.deps[j].phase ? sa1 : sa0;

               ps[pi->n_deps] = sa[j];
               pi->deps[pi->n_deps++] = info.deps[j];
            }
            so->code = insert_samples(out.code, out.n_code, &i0, sa0, &n);
            so->code1 = insert_samples(out.code1, out.n_code1, &i1, sa1, &n1);
            if (!so->code || !so->code1) {
               free(so->code);
               free(so->code1);
               so->code = so->code1 = NULL;
            }
            so->n_code1 = so->code1 ? n1 : 0;
         } else {
            so->code = insert_samples(out.code, out.n_code, &info, sa, &n);
         }
         so->n_code = so->code ? n : 0;
      } else if (out.n_code1) {
         /* phase 0's code as the program's (it ends at the feedback nop, so
          * the discard handling below applies), phase 1's after the feedback */
         so->code = malloc(out.n_code * sizeof(*so->code));
         so->code1 = malloc(out.n_code1 * sizeof(*so->code1));
         if (so->code && so->code1) {
            memcpy(so->code, out.code, out.n_code * sizeof(*so->code));
            memcpy(so->code1, out.code1, out.n_code1 * sizeof(*so->code1));
            so->n_code = out.n_code;
            so->n_code1 = out.n_code1;
         } else {
            free(so->code);
            free(so->code1);
            so->code = so->code1 = NULL;
         }
      } else {
         so->code = malloc(MAX2(out.n_code, 1) * sizeof(*so->code));
         if (so->code) {
            memcpy(so->code, out.code, out.n_code * sizeof(*so->code));
            so->n_code = out.n_code;
         }
      }
      /* a program that discards: its feedback nop is the last instruction
       * (the compiler's layout); the depth plane goes in after the iterated
       * inputs, by one more PDS iteration. The compiled program's discards,
       * not the shader's: a sampler view's swizzle can fold one away (an RGB
       * texture's alpha is 1) */
      so->discard_pa = -1;
      so->depth_pa = -1;
      if (out.discards) {
         if (!so->n_code || so->code[so->n_code - 1] != (uint64_t)0xf8000140u
                                                           << 32) {
            fprintf(
               stderr,
               "sgx: fragment shader with discard: no feedback point, not drawn\n");
            free(so->code);
            free(so->code1);
            so->code = so->code1 = NULL;
         }
      }
      /* gl_FragDepth waits in pa<depth_pa> at the feedback nop for a depthf
       * there (ISA.md, test H10) */
      if (out.writes_depth && so->code && so->n_code &&
          so->code[so->n_code - 1] == (uint64_t)0xf8000140u << 32) {
         so->depth_pa = out.depth_pa;
      } else if (out.writes_depth && !ctx->warned_frag_depth) {
         fprintf(stderr, "sgx: gl_FragDepth with no feedback point: the "
                         "triangle's depth is used\n");
         ctx->warned_frag_depth = true;
      }
      so->uses_front_face = out.uses_front_face;
      so->color_reg = out.colour_pa;
      so->temps = out.n_temps;
      so->prog.temps = out.n_temps;
      so->prog.pa_dwords = out.n_pa;
      so->prog.sec_dwords = so->const_dwords ? 9 + so->const_dwords : 0;
      /* One DOUTI per iteration, in PA order (docs/PDS.md "DOUTI"): a
       * varying's iterator is its set + 1, or, with the vertex colour in set
       * 0, the set itself (the PDS numbers the sets after the colour from 1:
       * seen for a sample, an iteration assumed alike).
       * The DDK's driver also iterates a
       * placeholder colour (source 0x20) in a shader without varyings; that
       * hangs the GPU when the vertex program outputs no colour, and ours
       * never do, so there is none. */
      int last_plain = -1, last_sample = -1;

      for (unsigned i = 0; i < out.n_iters && i < ARRAY_SIZE(so->prog.douti);
           i++) {
         unsigned it = out.iters[i].set + 1;
         unsigned tc = out.iters[i].set + (vary.colour ? 0 : 1);

         if (out.iters[i].sample) {
            /* sample half: coordinate set + 1, 2-D or projected (docs/PDS.md).
             * The sample half is independent of the iterate half, so a sample
             * is merged into the previous DOUTI when that one is a plain
             * iteration of a coordinate set, of this set or another, as the
             * DDK's generator does: its texel lands after that iteration, where
             * a DOUTI of its own would put it.  Not into gl_FragCoord's or a
             * point coordinate's (its w1 bits are the sample half's), nor a
             * flat one's (b25-26 2 may take the sample's coordinate too).
             * Otherwise a pure sample (source 0x24). */
            bool proj = out.iters[i].proj;
            unsigned prev = so->prog.n_douti - 1;
            struct sgx_pds_douti *d =
               so->prog.n_douti ? &so->prog.douti[prev] : NULL;
            bool other_set = !(sgx_drv_debug & SGX_DEBUG_NO_PDS_MERGE);

            if (d && !so->prog.douti_tex[prev] && !d->sample_set && !d->dim2 &&
                !d->proj && d->iterate && !d->w1 && d->b25_26 == 3 &&
                (d->src == tc ||
                 (other_set && d->src >= 1 && d->src <= SGX_MAX_SETS))) {
               d->sample_set = tc;
               d->dim2 = !proj;
               d->proj = proj;
               so->prog.douti_tex[prev] = out.iters[i].sampler + 1;
               last_sample = prev;
            } else {
               last_sample = so->prog.n_douti;
               so->prog.douti_tex[so->prog.n_douti] = out.iters[i].sampler + 1;
               so->prog.douti[so->prog.n_douti++] = (struct sgx_pds_douti){
                  .sample_set = tc,
                  .dim2 = !proj,
                  .proj = proj,
                  .src = 0x24,
                  .b25_26 = 3,
               };
            }
            /* returned F16, two registers (opts.f16_samples): the DDK's T3
             * for a sample its program does math on, as a whole word but a
             * volume's depth */
            if (out.iters[i].f16) {
               so->prog.douti_t3_clr[last_sample] = ~SGX_TEX_T3_DEPTH;
               so->prog.douti_t3_set[last_sample] = 0x01028800u;
            }
            continue;
         }
         /* gl_FragCoord (the compiler's set 13) is source 0: x and y at pixel
          * centres in the target's memory order, z, 1/w (docs/PDS.md "DOUTI") */
         if (it == SGX_FRAGCOORD_ITER)
            tc = 0;
         /* set 0 as the vertex colour: source 0x20, four components into
          * one register as BGRA8 */
         bool colour =
            vary.colour && out.iters[i].set == 0 && !out.iters[i].pointcoord;

         last_plain = so->prog.n_douti;
         so->prog.douti[so->prog.n_douti++] = (struct sgx_pds_douti){
            .src = colour ? 0x20 : tc,
            .comps = colour ? 4 : out.iters[i].comps,
            .iterate = true,
            /* 2: the provoking (last) vertex's value, 3 interpolated; only
             * the colour source iterates flat */
            .b25_26 = out.iters[i].flat ? 2 : 3,
            .prec1 = out.iters[i].f16,
            .w1 = out.iters[i].pointcoord ? 0x10 : 0,
         };
      }
      if (so->code && out.discards) {
         /* the triangle's depth plane (source 0x22, 3 components) after the
          * inputs: pa<n> on, n = the registers the iterations above fill (an
          * F32 component 1, an F16 pair 1, a sampled texel or the vertex
          * colour 1) */
         unsigned pa = 0;

         for (unsigned i = 0; i < so->prog.n_douti; i++) {
            const struct sgx_pds_douti *d = &so->prog.douti[i];

            if (d->iterate)
               pa += d->src == 0x20 ? 1
                     : d->prec1     ? (d->comps + 1) / 2
                                    : d->comps;
            if (d->dim2 || d->proj)
               pa++;
         }
         /* (an F16 sample fills two) */
         for (unsigned i = 0; i < out.n_iters; i++)
            pa = MAX2(pa, (unsigned)out.iters[i].pa + out.iters[i].regs);
         if (so->prog.n_douti == ARRAY_SIZE(so->prog.douti)) {
            free(so->code);
            free(so->code1);
            so->code = so->code1 = NULL;
         } else {
            last_plain = so->prog.n_douti;
            so->prog.douti[so->prog.n_douti++] = (struct sgx_pds_douti){
               .src = 0x22,
               .comps = 3,
               .iterate = true,
               .b25_26 = 3,
            };
            so->discard_pa = pa;
            so->prog.pa_dwords = MAX2(so->prog.pa_dwords, pa + 3);
            /* the visibility test's words in sa3, sa4 come with the secondary
             * DMA */
            so->prog.sec_dwords = 9 + so->const_dwords;
         }
      }
      if (so->code && so->depth_pa >= 0) {
         /* depthf reads its depth, and the depth words in sa3, sa4 */
         so->prog.pa_dwords = MAX2(so->prog.pa_dwords, so->depth_pa + 1);
         so->prog.sec_dwords = 9 + so->const_dwords;
      }
      struct sgxws_fs_feedback fb = {so->discard_pa, so->depth_pa};
      so->prog.code =
         !so->code ? 0
         : so->code1 && so->discard_pa >= 0
            ? sgxws_upload_fs_split(ctx->hw, so->code, so->n_code, so->code1,
                                    so->n_code1, so->color_reg, NULL, &fb,
                                    so->temps)
            : sgxws_upload_fs(ctx->hw, so->code, so->n_code, so->color_reg,
                              NULL, &fb, so->temps);
      if (last_plain >= 0)
         so->prog.douti[last_plain].last_iter = true;
      if (last_sample >= 0)
         so->prog.douti[last_sample].last_sample = true;
   }
   simple_mtx_unlock(&screen->lock);
   if (sgx_drv_debug & SGX_DEBUG_NIR)
      fprintf(
         stderr,
         "sgx: compiled %s shader: %u instructions, %u temps, %u constants, %u iterators\n",
         vertex ? "vertex" : "fragment", out.n_code, out.n_temps, so->n_consts,
         out.n_iters);
   so->kind = so->prog.code ? SGX_SHADER_COMPILED : SGX_SHADER_UNSUPPORTED;
   sgx_shader_free(&out);
   ralloc_free(nir);
   return so->kind == SGX_SHADER_COMPILED;
}

static void *
sgx_create_shader_state(struct pipe_context *pctx,
                        const struct pipe_shader_state *cso)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_pipe_shader *so = CALLOC_STRUCT(sgx_pipe_shader);
   nir_shader *nir;

   if (!so)
      return NULL;
   so->discard_pa = -1;
   so->depth_pa = -1;
   if (cso->type == PIPE_SHADER_IR_NIR)
      nir = cso->ir.nir;
   else
      nir = tgsi_to_nir(cso->tokens, pctx->screen, false);
   so->nir = nir;
   nir_foreach_shader_out_variable (var, nir)
      so->writes_psize |= var->data.location == VARYING_SLOT_PSIZ;

   if (!nir->info.io_lowered) {
      uint64_t t = os_time_get_nano();
      /* a shader for several colour buffers (gl_FragData[1..]): the base is
       * the first buffer's variant (remap_outputs), so that a draw into one
       * buffer has a program and a draw into several gets a variant each */
      if (nir->info.stage == MESA_SHADER_FRAGMENT &&
          (nir->info.outputs_written &
           BITFIELD64_RANGE(FRAG_RESULT_DATA1, 7)))
         so->rt_target = 1;
      so->dep_lod_units = dep_lod_units(nir);
      bool ok = compile_shader(ctx, so, nir, NULL, 0, 0, NULL, 0, NULL, false,
                               0, NULL, 0);

      ctx->stats.compile_ns += os_time_get_nano() - t;
      if (ok)
         return so;
   }

   if (!nir->info.io_lowered)
      NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
               type_size, 0);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_dce);

   if (nir->info.stage == MESA_SHADER_VERTEX)
      match_vs(so);
   else if (nir->info.stage == MESA_SHADER_FRAGMENT)
      match_fs(so);

   if ((sgx_drv_debug & SGX_DEBUG_NIR) || (!so->kind && !ctx->warned_shader)) {
      if (!so->kind)
         fprintf(stderr,
                 "sgx: no program for this %s shader yet (drawn as nothing):\n",
                 mesa_shader_stage_name(nir->info.stage));
      else
         fprintf(stderr, "sgx: %s shader -> fixed program %d\n",
                 mesa_shader_stage_name(nir->info.stage), so->kind);
      if (!so->kind || (sgx_drv_debug & SGX_DEBUG_NIR))
         nir_print_shader(nir, stderr);
      if (!so->kind)
         ctx->warned_shader = !(sgx_drv_debug & SGX_DEBUG_NIR);
   }
   return so;
}

static void
sgx_bind_vs_state(struct pipe_context *pctx, void *hwcso)
{
   sgx_context(pctx)->vs = hwcso;
}

static void
sgx_bind_fs_state(struct pipe_context *pctx, void *hwcso)
{
   sgx_context(pctx)->fs = hwcso;
}

/* a shader's programs (its blend variants too) go: the winsys frees their
 * code once no render runs it */
static void
free_programs(struct sgx_context *ctx, struct sgx_pipe_shader *so)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);

   if (!ctx->hw)
      return;
   simple_mtx_lock(&screen->lock);
   sgxws_code_free(ctx->hw, so->prog.code);
   for (unsigned i = 0; i < so->n_variants; i++)
      sgxws_code_free(ctx->hw, so->variants[i].code);
   simple_mtx_unlock(&screen->lock);
}

/* a variant (sgx_vs_for, sgx_fs_for) */
static void
destroy_variant(struct sgx_context *ctx, struct sgx_pipe_shader *v)
{
   if (!v)
      return;
   /* (no memoized answer outlives it) */
   ctx->fs_gen++;
   ctx->vs_gen++;
   free_programs(ctx, v);
   free(v->code);
   free(v->code1);
   FREE(v->vary);
   FREE(v);
}

static void
sgx_delete_shader_state(struct pipe_context *pctx, void *hwcso)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_pipe_shader *so = hwcso;

   for (unsigned i = 0; i < ARRAY_SIZE(so->variants_vs); i++)
      destroy_variant(ctx, so->variants_vs[i]);
   for (unsigned i = 0; i < ARRAY_SIZE(so->variants_srgb); i++)
      destroy_variant(ctx, so->variants_srgb[i]);
   ralloc_free(so->nir);
   so->nir = NULL;
   destroy_variant(ctx, so);
}

/* how a shader's texture units are read, from the views and samplers bound
 * to them: what its variants are compiled for (sgx_fs_for, sgx_vs_for) */
struct tex_key {
   unsigned srgb;
   uint32_t srgb_filter;
   uint16_t swz[PIPE_MAX_SAMPLERS];
   uint32_t not_bgra, depth;
   uint32_t r32x4, r32x2;
   uint32_t snorm;
   uint32_t wrap_rep, wrap_mir;
   uint8_t compare[PIPE_MAX_SAMPLERS];
   uint32_t dep_mip;
};

/* false when every unit reads as a variant-free program does; dep_lod the
 * units an implicit-LOD dependent sample reads (dep_lod_units) */
static bool
tex_key_of(const nir_shader *nir, uint32_t dep_lod,
           struct pipe_sampler_view *const *views,
           struct pipe_sampler_state *const *samplers, struct tex_key *k)
{
   bool any_swz = false;
   unsigned unit;

   memset(k, 0, sizeof(*k));
   BITSET_FOREACH_SET (unit, nir->info.textures_used, PIPE_MAX_SAMPLERS) {
      const struct pipe_sampler_view *v = views[unit];

      if (!v)
         continue;
      /* mipmapped: lower_dep_lod's units */
      if ((dep_lod >> unit & 1) && samplers[unit] &&
          samplers[unit]->min_mip_filter != PIPE_TEX_MIPFILTER_NONE &&
          v->u.tex.last_level > v->u.tex.first_level)
         k->dep_mip |= 1u << unit;
      if (util_format_is_srgb(v->format)) {
         const struct pipe_sampler_state *ss = samplers[unit];

         k->srgb |= 1u << unit;
         /* filtered at one level: decoded before it's filtered */
         if (ss &&
             (ss->mag_img_filter == PIPE_TEX_FILTER_LINEAR ||
              ss->min_img_filter == PIPE_TEX_FILTER_LINEAR) &&
             (ss->min_mip_filter == PIPE_TEX_MIPFILTER_NONE ||
              v->u.tex.first_level == v->u.tex.last_level))
            k->srgb_filter |= 1u << unit;
      }
      unsigned plane = sgx_tex_plane_format(v->texture);

      if (!sgx_tex_bgra(v->format) || plane)
         k->not_bgra |= 1u << unit;
      if (sgx_tex_snorm(v->format))
         k->snorm |= 1u << unit;
      /* a strided F32 texture's REPEAT / MIRRORED_REPEAT axes
       * (lower_word32_wrap) */
      if (sgx_tex_word32(v->texture->format) && samplers[unit] && unit < 16) {
         const struct pipe_sampler_state *ss = samplers[unit];
         unsigned w[2] = {ss->wrap_s, ss->wrap_t};

         for (unsigned i = 0; i < 2; i++) {
            if (w[i] == PIPE_TEX_WRAP_REPEAT)
               k->wrap_rep |= 1u << (2 * unit + i);
            else if (w[i] == PIPE_TEX_WRAP_MIRROR_REPEAT)
               k->wrap_mir |= 1u << (2 * unit + i);
         }
      }
      /* float planes, which the TPU doesn't filter: LINEAR at one level is
       * four taps in the shader, as for sRGB (lower_srgb_filter) */
      if (sgx_tex_float_planes(v->format)) {
         const struct pipe_sampler_state *ss = samplers[unit];

         if (ss &&
             (ss->mag_img_filter == PIPE_TEX_FILTER_LINEAR ||
              ss->min_img_filter == PIPE_TEX_FILTER_LINEAR) &&
             (ss->min_mip_filter == PIPE_TEX_MIPFILTER_NONE ||
              v->u.tex.first_level == v->u.tex.last_level))
            k->srgb_filter |= 1u << unit;
      }
      /* one F32 word a texel (tex_f32): depth, and R32 (raw); RG32 and
       * RGBA32 a read a plane (tex_r32x2 / x4) */
      if (sgx_tex_raw32(v->format) == 1)
         k->depth |= 1u << unit;
      if (sgx_tex_raw32(v->format) == 4)
         k->r32x4 |= 1u << unit;
      else if (sgx_tex_raw32(v->format) == 2)
         k->r32x2 |= 1u << unit;
      if (sgx_tex_depth(v->format)) {
         const struct pipe_sampler_state *ss = samplers[unit];

         k->depth |= 1u << unit;
         if (ss && ss->compare_mode == PIPE_TEX_COMPARE_R_TO_TEXTURE)
            k->compare[unit] = ss->compare_func + 1;
      }
      /* An X8 colour format is sampled as its alpha twin (sgx_tex_format), so
       * the X byte would come back as alpha: GL reads it as 1.  The state
       * tracker's views say so in their swizzle, u_blitter's source views
       * don't (a CopyTexSubImage from an XRGB window drawn on the GPU took
       * the X byte, 0, as alpha: Minecraft's title-screen panorama) */
      unsigned sw[4] = {v->swizzle_r, v->swizzle_g, v->swizzle_b, v->swizzle_a};

      if (util_format_get_blocksize(v->format) == 4 &&
          !util_format_has_alpha(v->format) &&
          !util_format_is_depth_or_stencil(v->format) &&
          util_format_description(v->format)->channel[3].type ==
             UTIL_FORMAT_TYPE_VOID) {
         for (unsigned c = 0; c < 4; c++)
            if (sw[c] == PIPE_SWIZZLE_W)
               sw[c] = PIPE_SWIZZLE_1;
      }
      /* An imported video plane comes back as L8 (L, L, L, 1) or LA88
       * (L, L, L, A): GL's R8 is (r, 0, 0, 1) and RG8 (r, g, 0, 1), g being
       * the second byte, A */
      if (plane) {
         static const uint8_t r8[4] = {PIPE_SWIZZLE_X, PIPE_SWIZZLE_0,
                                       PIPE_SWIZZLE_0, PIPE_SWIZZLE_1};
         static const uint8_t rg8[4] = {PIPE_SWIZZLE_X, PIPE_SWIZZLE_W,
                                        PIPE_SWIZZLE_0, PIPE_SWIZZLE_1};
         const uint8_t *m =
            plane == SGX_TEXSTATE_FORMAT_L8 ? r8 : rg8;

         for (unsigned c = 0; c < 4; c++)
            if (sw[c] <= PIPE_SWIZZLE_W)
               sw[c] = m[sw[c]];
      }
      /* XYZW: none, 0 */
      if (sw[0] != PIPE_SWIZZLE_X || sw[1] != PIPE_SWIZZLE_Y ||
          sw[2] != PIPE_SWIZZLE_Z || sw[3] != PIPE_SWIZZLE_W) {
         /* b15 marks one: XXXX packs to 0 */
         k->swz[unit] = 0x8000 | sw[0] | sw[1] << 3 | sw[2] << 6 | sw[3] << 9;
         any_swz = true;
      }
   }
   /* (depth alone, as before: a depth unit is not B, G, R, A either) */
   return k->srgb || k->srgb_filter || any_swz || k->not_bgra || k->r32x4 ||
          k->r32x2 || k->snorm || k->wrap_rep || k->wrap_mir || k->dep_mip;
}

static bool
tex_key_is(const struct sgx_pipe_shader *v, const struct tex_key *k)
{
   return v->srgb == k->srgb && v->srgb_filter == k->srgb_filter &&
          v->not_bgra == k->not_bgra && v->depth == k->depth &&
          v->r32x4 == k->r32x4 && v->r32x2 == k->r32x2 &&
          v->snorm == k->snorm && v->wrap_rep == k->wrap_rep &&
          v->wrap_mir == k->wrap_mir &&
          !memcmp(v->compare, k->compare, sizeof(k->compare)) &&
          !memcmp(v->swz, k->swz, sizeof(k->swz)) && v->dep_mip == k->dep_mip;
}

static void
tex_key_set(struct sgx_pipe_shader *v, const struct tex_key *k)
{
   v->srgb = k->srgb;
   v->srgb_filter = k->srgb_filter;
   memcpy(v->swz, k->swz, sizeof(k->swz));
   v->not_bgra = k->not_bgra;
   v->depth = k->depth;
   v->r32x4 = k->r32x4;
   v->r32x2 = k->r32x2;
   v->snorm = k->snorm;
   v->wrap_rep = k->wrap_rep;
   v->wrap_mir = k->wrap_mir;
   memcpy(v->compare, k->compare, sizeof(k->compare));
   v->dep_mip = k->dep_mip;
}

/* vs_for: the vertex shader for fs's sets (NULL: its own), these input
 * formats and points; and with vertex texture fetch, for the views and
 * samplers bound to its units (tex_key) */
static struct sgx_pipe_shader *
vs_for(struct sgx_context *ctx, struct sgx_pipe_shader *vs,
       const struct sgx_pipe_shader *fs, const uint8_t *vtx_fmt, bool points)
{
   static const uint8_t f32x4[16];
   const struct sgx_varyings *vary = fs && fs->vary ? fs->vary : vs->vary;
   struct sgx_pipe_shader **slot;
   struct tex_key k;
   bool tex =
      vs->nir && tex_key_of(vs->nir, 0, ctx->vs_views, ctx->vs_samplers, &k);

   if (!vtx_fmt)
      vtx_fmt = f32x4;
   points = points && vs->writes_psize;
   if ((!vs->vary || !vary || same_varyings(vs->vary, vary)) &&
       !memcmp(vtx_fmt, f32x4, sizeof(f32x4)) && !points && !tex)
      return vs;
   if (!tex)
      memset(&k, 0, sizeof(k));
   for (unsigned i = 0; i < ARRAY_SIZE(vs->variants_vs); i++) {
      struct sgx_pipe_shader *v = vs->variants_vs[i];

      if (v && v->points == points &&
          (!vary || (same_varyings(v->vary, vary) &&
                     v->vary->point_size == vary->point_size)) &&
          !memcmp(v->vtx_fmt, vtx_fmt, sizeof(v->vtx_fmt)) && tex_key_is(v, &k))
         return v;
   }
   if (!vs->nir)
      return NULL;
   /* the oldest goes */
   slot = &vs->variants_vs[vs->next_variant_vs++ % ARRAY_SIZE(vs->variants_vs)];
   destroy_variant(ctx, *slot);
   if (!(*slot = CALLOC_STRUCT(sgx_pipe_shader)))
      return NULL;
   (*slot)->discard_pa = -1;
   (*slot)->depth_pa = -1;
   (*slot)->points = points;
   tex_key_set(*slot, &k);
   if (!compile_shader(ctx, *slot, vs->nir, fs ? fs->vary : NULL, k.srgb,
                       k.srgb_filter, k.swz, k.not_bgra, vtx_fmt, points,
                       k.depth, k.compare, 0) ||
       !(*slot)->prog.code) {
      FREE((*slot)->vary);
      FREE(*slot);
      *slot = NULL;
   }
   return *slot;
}

/* the texture units a shader samples (all, when it isn't known) */
uint32_t
sgx_fs_textures_used(const struct sgx_pipe_shader *fs)
{
   uint32_t mask = 0;
   unsigned unit;

   if (fs->kind != SGX_SHADER_COMPILED || !fs->nir)
      return ~0u;
   BITSET_FOREACH_SET (unit, fs->nir->info.textures_used, PIPE_MAX_SAMPLERS)
      mask |= 1u << unit;
   return mask;
}

static struct sgx_pipe_shader *
fs_for(struct sgx_context *ctx, struct sgx_pipe_shader *fs)
{
   struct sgx_pipe_shader **slot;
   struct tex_key k;
   uint8_t rt_int = 0, rt_srgb = 0;
   struct pipe_rt_blend_state rt_blend;
   float rt_blend_color[4] = {0};
   /* MRT: a variant a colour buffer (remap_outputs) */
   uint8_t rt_target =
      ctx->framebuffer.nr_cbufs > 1 ? ctx->rt_index + 1 : 0;

   if (fs->kind != SGX_SHADER_COMPILED || !fs->nir)
      return fs;
   /* an integer colour buffer takes its own packing (pack_int_outputs), and
    * a float planar one its channel's F32 word a plane */
   if (sgx_cbuf(ctx)->texture &&
       (util_format_is_pure_integer(sgx_cbuf(ctx)->format) ||
        sgx_tex_raw32(sgx_cbuf(ctx)->format)))
      rt_int = sgx_tex_raw32(sgx_cbuf(ctx)->format) > 1
                  ? 3 + ctx->rt_plane /* a plane: channel rt_plane */
               : sgx_tex_raw32(sgx_cbuf(ctx)->format) ? 2
                                                                 : 1;
   /* an sRGB colour buffer: encoded, and blended in the program
    * (lower_srgb_output), with the blend the variant's */
   memset(&rt_blend, 0, sizeof(rt_blend));
   if (sgx_cbuf(ctx)->texture && util_format_is_srgb(sgx_cbuf(ctx)->format)) {
      unsigned br = sgx_blend_rt(ctx->blend, ctx->rt_index);

      rt_srgb = 1;
      if (ctx->blend && ctx->blend->base.rt[br].blend_enable) {
         const struct pipe_rt_blend_state *rb = &ctx->blend->base.rt[br];
         unsigned f[4] = {rb->rgb_src_factor, rb->rgb_dst_factor,
                          rb->alpha_src_factor, rb->alpha_dst_factor};

         rt_srgb = 2;
         rt_blend.blend_enable = 1;
         rt_blend.rgb_func = rb->rgb_func;
         rt_blend.rgb_src_factor = rb->rgb_src_factor;
         rt_blend.rgb_dst_factor = rb->rgb_dst_factor;
         rt_blend.alpha_func = rb->alpha_func;
         rt_blend.alpha_src_factor = rb->alpha_src_factor;
         rt_blend.alpha_dst_factor = rb->alpha_dst_factor;
         /* the blend colour, clamped (a fixed-point target), only when read */
         for (unsigned i = 0; i < 4; i++) {
            unsigned u = util_blendfactor_without_invert(f[i]);

            if (u == PIPE_BLENDFACTOR_CONST_COLOR ||
                u == PIPE_BLENDFACTOR_CONST_ALPHA) {
               for (unsigned c = 0; c < 4; c++)
                  rt_blend_color[c] =
                     CLAMP(ctx->blend_color.color[c], 0.0f, 1.0f);
            }
         }
      }
   }
   if (!tex_key_of(fs->nir, fs->dep_lod_units, ctx->views, ctx->samplers,
                   &k) &&
       !rt_int && !rt_target && !rt_srgb)
      return fs;
   for (unsigned i = 0; i < ARRAY_SIZE(fs->variants_srgb); i++) {
      if (fs->variants_srgb[i] && tex_key_is(fs->variants_srgb[i], &k) &&
          fs->variants_srgb[i]->rt_int == rt_int &&
          fs->variants_srgb[i]->rt_target == rt_target &&
          fs->variants_srgb[i]->rt_srgb == rt_srgb &&
          !memcmp(&fs->variants_srgb[i]->rt_blend, &rt_blend,
                  sizeof(rt_blend)) &&
          !memcmp(fs->variants_srgb[i]->rt_blend_color, rt_blend_color,
                  sizeof(rt_blend_color)))
         return fs->variants_srgb[i];
   }
   /* the oldest goes */
   slot = &fs->variants_srgb[fs->next_variant_srgb++ %
                             ARRAY_SIZE(fs->variants_srgb)];
   destroy_variant(ctx, *slot);
   if (!(*slot = CALLOC_STRUCT(sgx_pipe_shader)))
      return NULL;
   (*slot)->discard_pa = -1;
   (*slot)->depth_pa = -1;
   tex_key_set(*slot, &k);
   (*slot)->rt_int = rt_int;
   (*slot)->rt_target = rt_target;
   (*slot)->rt_srgb = rt_srgb;
   memcpy(&(*slot)->rt_blend, &rt_blend, sizeof(rt_blend));
   memcpy((*slot)->rt_blend_color, rt_blend_color, sizeof(rt_blend_color));
   if (k.dep_mip)
      SGX_DBG(DRAW,
              "fragment variant: units %#x take their LOD from derivatives (lower_dep_lod)\n",
              k.dep_mip);
   if (!compile_shader(ctx, *slot, fs->nir, NULL, k.srgb, k.srgb_filter, k.swz,
                       k.not_bgra, NULL, false, k.depth, k.compare, k.dep_mip) ||
       !(*slot)->code) {
      FREE((*slot)->vary);
      FREE(*slot);
      *slot = NULL;
   }
   return *slot;
}

struct sgx_pipe_shader *
sgx_vs_for(struct sgx_context *ctx, struct sgx_pipe_shader *vs,
           const struct sgx_pipe_shader *fs, const uint8_t *vtx_fmt,
           bool points)
{
   static const uint8_t f32x4[16];
   const uint8_t *fmt = vtx_fmt ? vtx_fmt : f32x4;
   struct sgx_pipe_shader *v;

   if (ctx->vs_memo.out && ctx->vs_memo.gen == ctx->vs_gen &&
       ctx->vs_memo.vs == vs && ctx->vs_memo.fs == fs &&
       ctx->vs_memo.points == points &&
       !memcmp(ctx->vs_memo.vtx_fmt, fmt, sizeof(ctx->vs_memo.vtx_fmt)))
      return ctx->vs_memo.out;
   v = vs_for(ctx, vs, fs, vtx_fmt, points);
   /* (the counter after the lookup: a variant it compiled may have ended
    * another) */
   ctx->vs_memo.gen = ctx->vs_gen;
   ctx->vs_memo.vs = vs;
   ctx->vs_memo.fs = fs;
   ctx->vs_memo.points = points;
   memcpy(ctx->vs_memo.vtx_fmt, fmt, sizeof(ctx->vs_memo.vtx_fmt));
   ctx->vs_memo.out = v;
   return v;
}

struct sgx_pipe_shader *
sgx_fs_for(struct sgx_context *ctx, struct sgx_pipe_shader *fs)
{
   struct sgx_pipe_shader *v;

   if (ctx->fs_memo.out && ctx->fs_memo.gen == ctx->fs_gen &&
       ctx->fs_memo.fs == fs && ctx->fs_memo.rt_index == ctx->rt_index &&
       ctx->fs_memo.rt_plane == ctx->rt_plane)
      return ctx->fs_memo.out;
   v = fs_for(ctx, fs);
   ctx->fs_memo.gen = ctx->fs_gen;
   ctx->fs_memo.fs = fs;
   ctx->fs_memo.rt_index = ctx->rt_index;
   ctx->fs_memo.rt_plane = ctx->rt_plane;
   ctx->fs_memo.out = v;
   return v;
}

/* the compiled fragment shader's program with its colour blended (b), or
 * without (NULL); 0 when there is none */

uint32_t
sgx_fs_program(struct sgx_context *ctx, struct sgx_pipe_shader *so,
               const struct sgxws_blend *b)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   uint32_t va;

   if (!b)
      return so->prog.code;
   for (unsigned i = 0; i < so->n_variants; i++) {
      if (!memcmp(&so->variants[i].key, b, sizeof(*b)))
         return so->variants[i].code;
   }
   if (!so->code)
      return 0;
   simple_mtx_lock(&screen->lock);
   struct sgxws_fs_feedback fb = {so->discard_pa, so->depth_pa};
   va = so->code1 && so->discard_pa >= 0
           ? sgxws_upload_fs_split(ctx->hw, so->code, so->n_code, so->code1,
                                   so->n_code1, so->color_reg, b, &fb,
                                   so->temps)
           : sgxws_upload_fs(ctx->hw, so->code, so->n_code, so->color_reg, b,
                             &fb, so->temps);
   /* past the table's size, the slots are reused in turn, their programs
    * freed (dEQP's fragment_ops.interaction draws one shader with dozens of
    * blends) */
   if (va) {
      unsigned i = so->n_variants < ARRAY_SIZE(so->variants)
                      ? so->n_variants++
                      : so->next_variant++ % ARRAY_SIZE(so->variants);

      sgxws_code_free(ctx->hw, so->variants[i].code); /* 0: a new slot */
      so->variants[i].key = *b;
      so->variants[i].code = va;
   }
   simple_mtx_unlock(&screen->lock);
   return va;
}

void
sgx_program_init(struct sgx_context *ctx)
{
   ctx->base.create_vs_state = sgx_create_shader_state;
   ctx->base.bind_vs_state = sgx_bind_vs_state;
   ctx->base.delete_vs_state = sgx_delete_shader_state;
   ctx->base.create_fs_state = sgx_create_shader_state;
   ctx->base.bind_fs_state = sgx_bind_fs_state;
   ctx->base.delete_fs_state = sgx_delete_shader_state;
}
