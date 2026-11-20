/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * Volume textures (SGX_DEBUG=tex3d, docs/TEX.md section 14) through GLES2's
 * OES_texture_3D: RGBA8 volumes whose texel (x, y, z) of level l is the
 * bytes (x, y, z, 0x80 | l), read back slice by slice with texture3D at
 * texel centres into a pbuffer:
 *
 *   size   every texel of power-of-two, flat, deep and NPOT volumes, up to
 *          256 on a side (256^3: every 15th slice)
 *   mip    each level of a mipmapped 32x16x8 and 16x16x16, drawn at the
 *          level's size (NEAREST_MIPMAP_NEAREST picks it), the coordinate
 *          a varying
 *   wrap   r outside [0, 1] with REPEAT, CLAMP_TO_EDGE and MIRRORED_REPEAT
 *   linear LINEAR half way between two slices: the mean of their bytes
 *   sub    a glTexSubImage3DOES box, every texel after it
 *   rgb565 a 16-bit volume (RGB565), every texel
 *   nondep sizes again with the coordinate a varying the PDS samples from
 *          (the cases above compute it: dependent samples)
 *
 *   SGX_DEBUG=tex3d sgx_tex3d [case...]
 *
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -o sgx_tex3d
 * sgx_tex3d.c -ldl
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EGL_EGL_PROTOTYPES 0
#define GL_GLES_PROTOTYPES 0
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

static PFNEGLGETPROCADDRESSPROC gpa;

static void *
L(const char *n)
{
   void *p = (void *)gpa(n);
   if (!p) {
      fprintf(stderr, "no %s\n", n);
      exit(2);
   }
   return p;
}
#define G(n, t) static t n##_
#define GET(n) n##_ = L(#n)

G(glClearColor, PFNGLCLEARCOLORPROC);
G(glClear, PFNGLCLEARPROC);
G(glViewport, PFNGLVIEWPORTPROC);
G(glGetString, PFNGLGETSTRINGPROC);
G(glGetError, PFNGLGETERRORPROC);
G(glGetIntegerv, PFNGLGETINTEGERVPROC);
G(glReadPixels, PFNGLREADPIXELSPROC);
G(glPixelStorei, PFNGLPIXELSTOREIPROC);
G(glGenTextures, PFNGLGENTEXTURESPROC);
G(glBindTexture, PFNGLBINDTEXTUREPROC);
G(glTexParameteri, PFNGLTEXPARAMETERIPROC);
G(glDeleteTextures, PFNGLDELETETEXTURESPROC);
G(glCreateShader, PFNGLCREATESHADERPROC);
G(glShaderSource, PFNGLSHADERSOURCEPROC);
G(glCompileShader, PFNGLCOMPILESHADERPROC);
G(glGetShaderiv, PFNGLGETSHADERIVPROC);
G(glGetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC);
G(glCreateProgram, PFNGLCREATEPROGRAMPROC);
G(glAttachShader, PFNGLATTACHSHADERPROC);
G(glBindAttribLocation, PFNGLBINDATTRIBLOCATIONPROC);
G(glLinkProgram, PFNGLLINKPROGRAMPROC);
G(glGetProgramiv, PFNGLGETPROGRAMIVPROC);
G(glGetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC);
G(glUseProgram, PFNGLUSEPROGRAMPROC);
G(glGetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC);
G(glUniform1i, PFNGLUNIFORM1IPROC);
G(glUniform3f, PFNGLUNIFORM3FPROC);
G(glUniform1f, PFNGLUNIFORM1FPROC);
G(glVertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC);
G(glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC);
G(glDrawArrays, PFNGLDRAWARRAYSPROC);
G(glTexImage3DOES, PFNGLTEXIMAGE3DOESPROC);
G(glTexSubImage3DOES, PFNGLTEXSUBIMAGE3DOESPROC);

#define FB 256

static int failures, checked;
static GLint u_size, u_r;

/* (x, y) of the target samples (x + 0.5) / size.x, (y + 0.5) / size.y at r:
 * computed in the fragment program (a dependent sample), or a varying */
static GLuint
program(int varying)
{
   static const char *vs[2] = {
      "attribute vec2 p;\n"
      "void main() { gl_Position = vec4(p, 0.0, 1.0); }\n",
      "attribute vec2 p;\n"
      "uniform float r;\n"
      "varying highp vec3 v;\n"
      "void main() {\n"
      "   v = vec3(p * 0.5 + 0.5, r);\n"
      "   gl_Position = vec4(p, 0.0, 1.0);\n"
      "}\n"};
   static const char *fs[2] = {
      "#extension GL_OES_texture_3D : require\n"
      "precision highp float;\n"
      "uniform highp sampler3D tex;\n"
      "uniform vec3 size;\n" /* the level's w, h; z unused */
      "uniform float r;\n"
      "void main() {\n"
      "   gl_FragColor = texture3D(tex, vec3(gl_FragCoord.xy / size.xy, r));\n"
      "}\n",
      "#extension GL_OES_texture_3D : require\n"
      "precision highp float;\n"
      "uniform highp sampler3D tex;\n"
      "varying highp vec3 v;\n"
      "void main() { gl_FragColor = texture3D(tex, v); }\n"};
   const char *src[2] = {vs[varying], fs[varying]};
   GLenum type[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
   GLuint p = glCreateProgram_();
   GLint ok = 0;
   char log[2048];

   for (int i = 0; i < 2; i++) {
      GLuint s = glCreateShader_(type[i]);

      glShaderSource_(s, 1, &src[i], NULL);
      glCompileShader_(s);
      glGetShaderiv_(s, GL_COMPILE_STATUS, &ok);
      if (!ok) {
         glGetShaderInfoLog_(s, sizeof(log), NULL, log);
         printf("  compile: %s\n", log);
         exit(1);
      }
      glAttachShader_(p, s);
   }
   glBindAttribLocation_(p, 0, "p");
   glLinkProgram_(p);
   glGetProgramiv_(p, GL_LINK_STATUS, &ok);
   if (!ok) {
      glGetProgramInfoLog_(p, sizeof(log), NULL, log);
      printf("  link: %s\n", log);
      exit(1);
   }
   return p;
}

static uint32_t
texel(unsigned x, unsigned y, unsigned z, unsigned l)
{
   return x | y << 8 | z << 16 | (0x80u | l) << 24;
}

static uint8_t *
volume(unsigned w, unsigned h, unsigned d, unsigned l)
{
   uint32_t *v = malloc((size_t)w * h * d * 4);

   for (unsigned z = 0; z < d; z++)
      for (unsigned y = 0; y < h; y++)
         for (unsigned x = 0; x < w; x++)
            v[((size_t)z * h + y) * w + x] = texel(x, y, z, l);
   return (uint8_t *)v;
}

static GLuint
make(unsigned w, unsigned h, unsigned d, unsigned levels, GLenum filter)
{
   GLuint t;

   glGenTextures_(1, &t);
   glBindTexture_(GL_TEXTURE_3D_OES, t);
   for (unsigned l = 0; l < levels; l++) {
      unsigned lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1,
               ld = d >> l ? d >> l : 1;
      uint8_t *v = volume(lw, lh, ld, l);

      glTexImage3DOES_(GL_TEXTURE_3D_OES, l, GL_RGBA, lw, lh, ld, 0, GL_RGBA,
                       GL_UNSIGNED_BYTE, v);
      free(v);
   }
   glTexParameteri_(GL_TEXTURE_3D_OES, GL_TEXTURE_MIN_FILTER, filter);
   glTexParameteri_(GL_TEXTURE_3D_OES, GL_TEXTURE_MAG_FILTER,
                    filter == GL_LINEAR ? GL_LINEAR : GL_NEAREST);
   glTexParameteri_(GL_TEXTURE_3D_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri_(GL_TEXTURE_3D_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glTexParameteri_(GL_TEXTURE_3D_OES, GL_TEXTURE_WRAP_R_OES,
                    GL_CLAMP_TO_EDGE);
   return t;
}

/* draw a w x h rectangle sampling r, return the pixels: one triangle past
 * the viewport, so that no 2x2 quad straddles an edge (a computed
 * coordinate's LOD comes from the quad) */
static uint32_t *
draw(unsigned w, unsigned h, float r)
{
   static const float tri[] = {-1, -1, 3, -1, -1, 3};
   uint32_t *px = malloc((size_t)w * h * 4);

   glViewport_(0, 0, w, h);
   glClearColor_(0, 0, 0, 0);
   glClear_(GL_COLOR_BUFFER_BIT);
   glUniform3f_(u_size, w, h, 1);
   glUniform1f_(u_r, r);
   glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
   glEnableVertexAttribArray_(0);
   glDrawArrays_(GL_TRIANGLES, 0, 3);
   glReadPixels_(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
   return px;
}

/* slice z of a w x h x d level against want(x, y) */
static int
check_slice(const char *name, unsigned w, unsigned h, unsigned d, unsigned z,
            uint32_t (*want)(unsigned, unsigned, unsigned, void *), void *data)
{
   uint32_t *px = draw(w, h, (z + 0.5f) / d);
   int bad = 0;

   for (unsigned y = 0; y < h; y++)
      for (unsigned x = 0; x < w; x++) {
         uint32_t g = px[y * w + x], e = want(x, y, z, data);

         checked++;
         if (g != e) {
            if (bad < 4)
               printf("  %s (%u,%u,%u): got %08x want %08x\n", name, x, y, z,
                      g, e);
            bad++;
         }
      }
   free(px);
   return bad;
}

static uint32_t
want_level(unsigned x, unsigned y, unsigned z, void *data)
{
   return texel(x, y, z, (unsigned)(uintptr_t)data);
}

static void
result(const char *name, int bad)
{
   GLenum e = glGetError_();

   printf("%-24s %s", name, bad || e ? "FAIL" : "ok");
   if (bad)
      printf(" (%d texels)", bad);
   if (e)
      printf(" (GL error %#x)", e);
   printf("\n");
   failures += bad || e;
}

static void
case_size(unsigned w, unsigned h, unsigned d, unsigned every)
{
   char name[64];
   GLuint t = make(w, h, d, 1, GL_NEAREST);
   int bad = 0;

   snprintf(name, sizeof(name), "size %ux%ux%u", w, h, d);
   for (unsigned z = 0; z < d; z += every)
      bad += check_slice(name, w, h, d, z, want_level, (void *)0);
   if (every > 1 && (d - 1) % every)
      bad += check_slice(name, w, h, d, d - 1, want_level, (void *)0);
   result(name, bad);
   glDeleteTextures_(1, &t);
}

static void
case_mip(unsigned w, unsigned h, unsigned d)
{
   unsigned levels = 1, m = w > h ? w : h;
   char name[64];
   int bad = 0;

   m = m > d ? m : d;
   while (m >> levels)
      levels++;
   GLuint t = make(w, h, d, levels, GL_NEAREST_MIPMAP_NEAREST);
   snprintf(name, sizeof(name), "mip %ux%ux%u", w, h, d);
   /* drawn at the level's w x h, the sampled LOD is l (while w and h both
    * still halve) */
   for (unsigned l = 0; l < levels && (w >> l) && (h >> l); l++) {
      unsigned lw = w >> l, lh = h >> l, ld = d >> l ? d >> l : 1;

      for (unsigned z = 0; z < ld; z++)
         bad += check_slice(name, lw, lh, ld, z, want_level,
                            (void *)(uintptr_t)l);
   }
   result(name, bad);
   glDeleteTextures_(1, &t);
}

struct wrap {
   GLenum mode;
   int z;
};

static uint32_t
want_wrap(unsigned x, unsigned y, unsigned z, void *data)
{
   (void)z;
   return texel(x, y, ((struct wrap *)data)->z, 0);
}

static void
case_wrap(void)
{
   static const struct {
      GLenum mode;
      const char *name;
      float slice; /* in slices of 4 */
      int z;
   } cases[] = {
      {GL_REPEAT, "repeat", 5.5f, 1},
      {GL_REPEAT, "repeat", -0.5f, 3},
      {GL_CLAMP_TO_EDGE, "clamp", 5.5f, 3},
      {GL_CLAMP_TO_EDGE, "clamp", -0.5f, 0},
      {GL_MIRRORED_REPEAT, "mirror", 5.5f, 2},
      {GL_MIRRORED_REPEAT, "mirror", -1.5f, 1},
   };
   GLuint t = make(4, 4, 4, 1, GL_NEAREST);
   int bad = 0;

   for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      struct wrap wr = {cases[i].mode, cases[i].z};
      uint32_t *px;

      glTexParameteri_(GL_TEXTURE_3D_OES, GL_TEXTURE_WRAP_R_OES,
                       cases[i].mode);
      px = draw(4, 4, cases[i].slice / 4);
      for (unsigned y = 0; y < 4; y++)
         for (unsigned x = 0; x < 4; x++) {
            checked++;
            if (px[y * 4 + x] != want_wrap(x, y, 0, &wr)) {
               if (!bad)
                  printf("  wrap %s at slice %.1f (%u,%u): got %08x want %08x\n",
                         cases[i].name, cases[i].slice, x, y, px[y * 4 + x],
                         want_wrap(x, y, 0, &wr));
               bad++;
            }
         }
      free(px);
   }
   result("wrap r", bad);
   glDeleteTextures_(1, &t);
}

/* LINEAR at r half way between slices 1 and 2 of 8x8x4: x, y as they are
 * (texel centres), z the mean 1.5 -> the byte between, alpha 0x80 */
static void
case_linear(void)
{
   GLuint t = make(8, 8, 4, 1, GL_LINEAR);
   uint32_t *px = draw(8, 8, 0.5f);
   int bad = 0;

   for (unsigned y = 0; y < 8; y++)
      for (unsigned x = 0; x < 8; x++) {
         uint32_t g = px[y * 8 + x];
         unsigned z = g >> 16 & 0xff;

         checked++;
         if ((g & 0xffff) != (x | y << 8) || (z != 1 && z != 2) ||
             g >> 24 != 0x80) {
            if (!bad)
               printf("  linear (%u,%u): got %08x\n", x, y, g);
            bad++;
         }
      }
   /* (1 + 2) / 2 rounds to 1 or 2: the filter's rounding, not checked */
   free(px);
   result("linear r", bad);
   glDeleteTextures_(1, &t);
}

static uint32_t
want_sub(unsigned x, unsigned y, unsigned z, void *data)
{
   (void)data;
   if (x >= 3 && x < 11 && y >= 2 && y < 7 && z >= 1 && z < 6)
      return 0xff000000u | (x * 7 + y * 3 + z) * 0x010101u;
   return texel(x, y, z, 0);
}

static void
case_sub(void)
{
   GLuint t = make(16, 8, 8, 1, GL_NEAREST);
   uint32_t box[8 * 5 * 5];
   int bad = 0;

   for (unsigned z = 1; z < 6; z++)
      for (unsigned y = 2; y < 7; y++)
         for (unsigned x = 3; x < 11; x++)
            box[((z - 1) * 5 + (y - 2)) * 8 + (x - 3)] =
               want_sub(x, y, z, NULL);
   glTexSubImage3DOES_(GL_TEXTURE_3D_OES, 0, 3, 2, 1, 8, 5, 5, GL_RGBA,
                       GL_UNSIGNED_BYTE, box);
   for (unsigned z = 0; z < 8; z++)
      bad += check_slice("sub", 16, 8, 8, z, want_sub, NULL);
   result("sub 16x8x8", bad);
   glDeleteTextures_(1, &t);
}

/* RGB565, x and y in red and green (5 and 6 bits), z in blue */
static uint32_t
want_565(unsigned x, unsigned y, unsigned z, void *data)
{
   (void)data;
   unsigned r = x & 31, g = y & 63, b = z & 31;

   return (r << 3 | r >> 2) | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2) << 16 |
          0xffu << 24;
}

static void
case_565(unsigned d)
{
   const unsigned w = 32, h = 16;
   char name[64];
   uint16_t *v = malloc(w * h * d * 2);
   GLuint t;
   int bad = 0;

   for (unsigned z = 0; z < d; z++)
      for (unsigned y = 0; y < h; y++)
         for (unsigned x = 0; x < w; x++)
            v[(z * h + y) * w + x] = (x & 31) << 11 | (y & 63) << 5 | (z & 31);
   glGenTextures_(1, &t);
   glBindTexture_(GL_TEXTURE_3D_OES, t);
   glTexImage3DOES_(GL_TEXTURE_3D_OES, 0, GL_RGB, w, h, d, 0, GL_RGB,
                    GL_UNSIGNED_SHORT_5_6_5, v);
   free(v);
   glTexParameteri_(GL_TEXTURE_3D_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri_(GL_TEXTURE_3D_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   snprintf(name, sizeof(name), "rgb565 %ux%ux%u", w, h, d);
   for (unsigned z = 0; z < d; z++)
      bad += check_slice(name, w, h, d, z, want_565, NULL);
   result(name, bad);
   glDeleteTextures_(1, &t);
}

static int
want(int argc, char **argv, const char *c)
{
   if (argc < 2)
      return 1;
   for (int i = 1; i < argc; i++)
      if (!strcmp(argv[i], c))
         return 1;
   return 0;
}

int
main(int argc, char **argv)
{
   void *egl = dlopen("libEGL.so.1", RTLD_NOW);
   if (!egl) {
      fprintf(stderr, "no libEGL.so.1\n");
      return 2;
   }
   gpa = (PFNEGLGETPROCADDRESSPROC)dlsym(egl, "eglGetProcAddress");
   PFNEGLGETDISPLAYPROC getdisplay = (void *)dlsym(egl, "eglGetDisplay");
   PFNEGLINITIALIZEPROC initialize = (void *)dlsym(egl, "eglInitialize");
   PFNEGLBINDAPIPROC bindapi = (void *)dlsym(egl, "eglBindAPI");
   PFNEGLCHOOSECONFIGPROC chooseconfig = (void *)dlsym(egl, "eglChooseConfig");
   PFNEGLCREATECONTEXTPROC createcontext =
      (void *)dlsym(egl, "eglCreateContext");
   PFNEGLCREATEPBUFFERSURFACEPROC createpbuffer =
      (void *)dlsym(egl, "eglCreatePbufferSurface");
   PFNEGLMAKECURRENTPROC makecurrent = (void *)dlsym(egl, "eglMakeCurrent");

   EGLDisplay d = getdisplay(EGL_DEFAULT_DISPLAY);
   if (!initialize(d, NULL, NULL)) {
      fprintf(stderr, "eglInitialize\n");
      return 2;
   }
   bindapi(EGL_OPENGL_ES_API);
   EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8,
                  EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                  EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
   EGLConfig cfg;
   EGLint n = 0;
   if (!chooseconfig(d, ca, &cfg, 1, &n) || !n) {
      fprintf(stderr, "no config\n");
      return 2;
   }
   EGLint cta[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
   EGLContext c = createcontext(d, cfg, EGL_NO_CONTEXT, cta);
   EGLint pa[] = {EGL_WIDTH, FB, EGL_HEIGHT, FB, EGL_NONE};
   EGLSurface s = createpbuffer(d, cfg, pa);
   if (!c || !s || !makecurrent(d, s, s, c)) {
      fprintf(stderr, "no context\n");
      return 2;
   }

   GET(glClearColor);
   GET(glClear);
   GET(glViewport);
   GET(glGetString);
   GET(glGetError);
   GET(glGetIntegerv);
   GET(glReadPixels);
   GET(glPixelStorei);
   GET(glGenTextures);
   GET(glBindTexture);
   GET(glTexParameteri);
   GET(glDeleteTextures);
   GET(glCreateShader);
   GET(glShaderSource);
   GET(glCompileShader);
   GET(glGetShaderiv);
   GET(glGetShaderInfoLog);
   GET(glCreateProgram);
   GET(glAttachShader);
   GET(glBindAttribLocation);
   GET(glLinkProgram);
   GET(glGetProgramiv);
   GET(glGetProgramInfoLog);
   GET(glUseProgram);
   GET(glGetUniformLocation);
   GET(glUniform1i);
   GET(glUniform3f);
   GET(glUniform1f);
   GET(glVertexAttribPointer);
   GET(glEnableVertexAttribArray);
   GET(glDrawArrays);

   const char *ext = (const char *)glGetString_(GL_EXTENSIONS);
   GLint max3d = 0;

   if (!strstr(ext, "GL_OES_texture_3D")) {
      printf("no GL_OES_texture_3D\n");
      return 2;
   }
   GET(glTexImage3DOES);
   GET(glTexSubImage3DOES);
   glGetIntegerv_(GL_MAX_3D_TEXTURE_SIZE_OES, &max3d);
   printf("%s, GL_MAX_3D_TEXTURE_SIZE %d\n", glGetString_(GL_RENDERER), max3d);
   if (max3d < 256) {
      printf("needs SGX_DEBUG=tex3d\n");
      return 2;
   }
   glPixelStorei_(GL_UNPACK_ALIGNMENT, 1);
   glPixelStorei_(GL_PACK_ALIGNMENT, 1);
   GLuint prog[2] = {program(0), program(1)};
   glUseProgram_(prog[0]);
   u_size = glGetUniformLocation_(prog[0], "size");
   u_r = glGetUniformLocation_(prog[0], "r");
   glUniform1i_(glGetUniformLocation_(prog[0], "tex"), 0);

   if (want(argc, argv, "size")) {
      static const unsigned sizes[][4] = {
         {4, 4, 4, 1},    {8, 8, 8, 1},     {16, 16, 16, 1}, {32, 32, 32, 1},
         {64, 32, 8, 1},  {16, 64, 4, 1},   {256, 2, 2, 1},  {2, 256, 2, 1},
         {2, 2, 256, 1},  {64, 64, 64, 1},  {5, 7, 3, 1},    {24, 40, 20, 1},
         {63, 63, 63, 1}, {1, 1, 16, 1},    {256, 256, 256, 15},
      };

      for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
         case_size(sizes[i][0], sizes[i][1], sizes[i][2], sizes[i][3]);
   }
   /* (through a varying: a computed coordinate's LOD is off on some 2x2
    * quads for 2D textures too, which is not the volumes') */
   if (want(argc, argv, "mip")) {
      glUseProgram_(prog[1]);
      u_size = -1;
      u_r = glGetUniformLocation_(prog[1], "r");
      glUniform1i_(glGetUniformLocation_(prog[1], "tex"), 0);
      case_mip(16, 16, 16);
      case_mip(32, 16, 8);
      glUseProgram_(prog[0]);
      u_size = glGetUniformLocation_(prog[0], "size");
      u_r = glGetUniformLocation_(prog[0], "r");
   }
   if (want(argc, argv, "wrap"))
      case_wrap();
   if (want(argc, argv, "linear"))
      case_linear();
   if (want(argc, argv, "sub"))
      case_sub();
   if (want(argc, argv, "rgb565")) {
      case_565(1); /* a 2D texture's layout (one slice) */
      case_565(8);
   }
   if (want(argc, argv, "nondep")) {
      glUseProgram_(prog[1]);
      u_size = -1;
      u_r = glGetUniformLocation_(prog[1], "r");
      glUniform1i_(glGetUniformLocation_(prog[1], "tex"), 0);
      printf("nondep:\n");
      case_size(16, 16, 16, 1);
      case_size(24, 40, 20, 1);
      case_size(256, 256, 256, 51);
   }
   if (glGetError_())
      printf("GL error %#x\n", glGetError_());
   printf("%d texels checked, %d case(s) failed\n", checked, failures);
   return failures != 0;
}
