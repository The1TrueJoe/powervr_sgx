/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * Buffer textures (SGX_DEBUG=tbo,glsl130, docs/GL3.md): texelFetch of a
 * samplerBuffer, isamplerBuffer or usamplerBuffer, every texel's bits
 * rendered into a 32x32 RGBA32UI FBO (pixel p reads texel p + base) and
 * compared on the CPU with what the buffer holds, decoded as GL says:
 *   - each GL 3.1 buffer format (8-, 16- and 32-bit channels; unorm, snorm,
 *     half, float, signed and unsigned integers; one, two and four
 *     channels), RGB32F (ARB_texture_buffer_object_rgb32) and three legacy
 *     ones (GL_ALPHA8, GL_LUMINANCE8_ALPHA8, GL_INTENSITY16);
 *   - texels past the end (a buffer of 1000 texels) read 0;
 *   - glTexBufferRange at an offset (ARB_texture_buffer_range);
 *   - textureSize;
 *   - a glBufferSubData between two draws of one frame: the first draw keeps
 *     the old texels;
 *   - a vertex program's texelFetch (SGX_DEBUG=vtf): a point a pixel.
 * Unorm and snorm texels are compared within 1e-6, the rest bit for bit.
 *
 *   SGX_DEBUG=tbo,glsl130,vtf MESA_GL_VERSION_OVERRIDE=3.1 \
 *   MESA_GLSL_VERSION_OVERRIDE=140 sgx_tbo
 *
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -o sgx_tbo sgx_tbo.c
 * -ldl -lm
 */
#include <dlfcn.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EGL_EGL_PROTOTYPES 0
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <GL/gl.h>
#include <GL/glext.h>

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
#define G(n) static __typeof__(&n) n##_
#define GET(n) n##_ = L(#n)

G(glClearBufferuiv);
G(glViewport);
G(glReadPixels);
G(glGenTextures);
G(glBindTexture);
G(glTexImage2D);
G(glTexBuffer);
G(glTexBufferRange);
G(glActiveTexture);
G(glGenFramebuffers);
G(glBindFramebuffer);
G(glFramebufferTexture2D);
G(glCheckFramebufferStatus);
G(glCreateShader);
G(glShaderSource);
G(glCompileShader);
G(glGetShaderiv);
G(glGetShaderInfoLog);
G(glCreateProgram);
G(glAttachShader);
G(glLinkProgram);
G(glGetProgramiv);
G(glGetProgramInfoLog);
G(glUseProgram);
G(glGetString);
G(glGetIntegerv);
G(glVertexPointer);
G(glEnableClientState);
G(glDisableClientState);
G(glDrawArrays);
G(glGenBuffers);
G(glBindBuffer);
G(glBufferData);
G(glBufferSubData);
G(glGetUniformLocation);
G(glUniform1i);
G(glGetError);
G(glFinish);
G(glBindFragDataLocation);

#define S 32
#define N 1000 /* texels in a buffer: pixels from 1000 on read past it */

enum kind { UN, SN, HF, FL, SI, UI };

static const struct fmt {
   const char *name;
   GLenum gl;
   enum kind kind;
   unsigned size, n;   /* bytes a channel, channels stored */
   const char *swz;    /* GL's r, g, b, a from the stored channels (x y z w),
                          0 and 1 */
} fmts[] = {
   {"R8", GL_R8, UN, 1, 1, "x001"},
   {"RG8", GL_RG8, UN, 1, 2, "xy01"},
   {"RGBA8", GL_RGBA8, UN, 1, 4, "xyzw"},
   {"R16", GL_R16, UN, 2, 1, "x001"},
   {"RG16", GL_RG16, UN, 2, 2, "xy01"},
   {"RGBA16", GL_RGBA16, UN, 2, 4, "xyzw"},
   {"R16F", GL_R16F, HF, 2, 1, "x001"},
   {"RG16F", GL_RG16F, HF, 2, 2, "xy01"},
   {"RGBA16F", GL_RGBA16F, HF, 2, 4, "xyzw"},
   {"R32F", GL_R32F, FL, 4, 1, "x001"},
   {"RG32F", GL_RG32F, FL, 4, 2, "xy01"},
   {"RGB32F", GL_RGB32F, FL, 4, 3, "xyz1"},
   {"RGBA32F", GL_RGBA32F, FL, 4, 4, "xyzw"},
   {"R8I", GL_R8I, SI, 1, 1, "x001"},
   {"RG8I", GL_RG8I, SI, 1, 2, "xy01"},
   {"RGBA8I", GL_RGBA8I, SI, 1, 4, "xyzw"},
   {"R16I", GL_R16I, SI, 2, 1, "x001"},
   {"RG16I", GL_RG16I, SI, 2, 2, "xy01"},
   {"RGBA16I", GL_RGBA16I, SI, 2, 4, "xyzw"},
   {"R32I", GL_R32I, SI, 4, 1, "x001"},
   {"RG32I", GL_RG32I, SI, 4, 2, "xy01"},
   {"RGBA32I", GL_RGBA32I, SI, 4, 4, "xyzw"},
   {"R8UI", GL_R8UI, UI, 1, 1, "x001"},
   {"RG8UI", GL_RG8UI, UI, 1, 2, "xy01"},
   {"RGBA8UI", GL_RGBA8UI, UI, 1, 4, "xyzw"},
   {"R16UI", GL_R16UI, UI, 2, 1, "x001"},
   {"RG16UI", GL_RG16UI, UI, 2, 2, "xy01"},
   {"RGBA16UI", GL_RGBA16UI, UI, 2, 4, "xyzw"},
   {"R32UI", GL_R32UI, UI, 4, 1, "x001"},
   {"RG32UI", GL_RG32UI, UI, 4, 2, "xy01"},
   {"RGBA32UI", GL_RGBA32UI, UI, 4, 4, "xyzw"},
   /* GL_ARB_texture_buffer_object's legacy formats (compatibility) */
   {"ALPHA8", GL_ALPHA8, UN, 1, 1, "000x"},
   {"LUMINANCE8_ALPHA8", GL_LUMINANCE8_ALPHA8, UN, 1, 2, "xxxy"},
   {"INTENSITY16", GL_INTENSITY16, UN, 2, 1, "xxxx"},
};

static float
half_to_float(uint16_t h)
{
   unsigned e = h >> 10 & 31, m = h & 1023;
   float v = e == 0    ? ldexpf((float)m, -24)
             : e == 31 ? (m ? NAN : INFINITY)
                       : ldexpf((float)(m | 1024), (int)e - 25);
   return h & 0x8000 ? -v : v;
}

static uint32_t
fbits(float f)
{
   uint32_t u;
   memcpy(&u, &f, 4);
   return u;
}

static float
bitsf(uint32_t u)
{
   float f;
   memcpy(&f, &u, 4);
   return f;
}

/* texel i's stored channel c: raw bits, varied per texel and channel */
static uint32_t
raw(const struct fmt *f, unsigned i, unsigned c, unsigned salt)
{
   uint32_t v = (i * 2654435761u) ^ (c * 0x9e3779b9u) ^ (salt * 0x85ebca6bu);

   v ^= v >> 15;
   if (f->kind == HF) {
      /* finite halves, denormals and negative zero among them */
      uint16_t h = (uint16_t)v;
      if ((h >> 10 & 31) == 31)
         h &= ~0x4000;
      return h;
   }
   if (f->kind == FL)
      return fbits(((float)(int)(v % 20001) - 10000.0f) * 0.0625f);
   return f->size == 4 ? v : v & ((1u << (8 * f->size)) - 1);
}

/* what texelFetch returns for stored channel bits w, as the shader's output
 * bits (floatBitsToUint for float results) */
static uint32_t
decode(const struct fmt *f, uint32_t w)
{
   unsigned bits = 8 * f->size;

   switch (f->kind) {
   case UN:
      return fbits((float)w / (float)((1ull << bits) - 1));
   case SN: {
      int32_t s = (int32_t)(w << (32 - bits)) >> (32 - bits);
      float v = (float)s / (float)((1u << (bits - 1)) - 1);
      return fbits(v < -1.0f ? -1.0f : v);
   }
   case HF:
      return fbits(half_to_float((uint16_t)w));
   case FL:
   case UI:
      return w;
   case SI:
      return bits == 32 ? w : (uint32_t)((int32_t)(w << (32 - bits)) >> (32 - bits));
   }
   return 0;
}

static GLuint
shader(GLenum type, const char *src)
{
   GLuint sh = glCreateShader_(type);
   GLint ok = 0;
   char log[2048];

   glShaderSource_(sh, 1, &src, NULL);
   glCompileShader_(sh);
   glGetShaderiv_(sh, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      glGetShaderInfoLog_(sh, sizeof(log), NULL, log);
      printf("compile failed:\n%s\n%s\n", src, log);
      exit(1);
   }
   return sh;
}

static GLuint
link(GLuint vs, GLuint fs)
{
   GLuint p = glCreateProgram_();
   GLint ok = 0;
   char log[2048];

   glAttachShader_(p, vs);
   glAttachShader_(p, fs);
   glBindFragDataLocation_(p, 0, "o");
   glLinkProgram_(p);
   glGetProgramiv_(p, GL_LINK_STATUS, &ok);
   if (!ok) {
      glGetProgramInfoLog_(p, sizeof(log), NULL, log);
      printf("link failed: %s\n", log);
      exit(1);
   }
   return p;
}

static const char *vs_quad =
   "#version 140\n"
   "void main() { gl_Position = gl_Vertex; }\n";

/* the fragment program for a sampler type ("", "i", "u") */
static GLuint
fs_fetch(const char *t)
{
   static char src[1024];

   snprintf(src, sizeof(src),
            "#version 140\n"
            "#extension GL_ARB_shader_bit_encoding : require\n"
            "uniform %ssamplerBuffer b;\n"
            "uniform int base, size_check;\n"
            "out uvec4 o;\n"
            "void main() {\n"
            "   int i = int(gl_FragCoord.x) + %d * int(gl_FragCoord.y) + base;\n"
            "   o = %s(texelFetch(b, i));\n"
            "   if (size_check != 0) o = uvec4(textureSize(b));\n"
            "}\n",
            t, S, *t ? "uvec4" : "floatBitsToUint");
   return shader(GL_FRAGMENT_SHADER, src);
}

static uint32_t px[S * S * 4];

/* run program p over the FBO and read it back */
static void
draw(GLuint p, int base, int size_check)
{
   static const GLuint zero[4];

   glClearBufferuiv_(GL_COLOR, 0, zero);
   glUseProgram_(p);
   glUniform1i_(glGetUniformLocation_(p, "b"), 0);
   glUniform1i_(glGetUniformLocation_(p, "base"), base);
   glUniform1i_(glGetUniformLocation_(p, "size_check"), size_check);
   glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
   glReadPixels_(0, 0, S, S, GL_RGBA_INTEGER, GL_UNSIGNED_INT, px);
}

/* the 4 channels GL returns for texel i of format f (0 past n texels) */
static void
want_texel(const struct fmt *f, unsigned i, unsigned n, unsigned salt,
           uint32_t out[4])
{
   uint32_t one = f->kind == SI || f->kind == UI ? 1 : fbits(1.0f);

   for (unsigned c = 0; c < 4; c++) {
      char s = f->swz[c];

      out[c] = i >= n      ? 0
               : s == '0' ? 0
               : s == '1' ? one
                          : decode(f, raw(f, i, s == 'w' ? 3 : s - 'x', salt));
   }
}

static bool
close_enough(const struct fmt *f, uint32_t got, uint32_t want)
{
   if (got == want)
      return true;
   if (f->kind != UN && f->kind != SN)
      return false;
   return fabsf(bitsf(got) - bitsf(want)) <= 1e-6f;
}

/* pixels [first, first + count) against texels base + p, n of them */
static unsigned
check(const struct fmt *f, unsigned first, unsigned count, int base,
      unsigned n, unsigned salt, const char *what)
{
   unsigned wrong = 0;

   for (unsigned p = first; p < first + count; p++) {
      uint32_t w[4];

      want_texel(f, p + base, n, salt, w);
      for (unsigned c = 0; c < 4; c++) {
         if (!close_enough(f, px[4 * p + c], w[c])) {
            if (!wrong)
               printf("  %s %s: pixel %u channel %u got %08x want %08x\n",
                      f->name, what, p, c, px[4 * p + c], w[c]);
            wrong++;
         }
      }
   }
   return wrong;
}

/* the buffer's bytes for format f: n texels from texel 0 */
static void *
fill(const struct fmt *f, unsigned n, unsigned salt)
{
   uint8_t *d = calloc(n, f->size * f->n);

   for (unsigned i = 0; i < n; i++)
      for (unsigned c = 0; c < f->n; c++) {
         uint32_t v = raw(f, i, c, salt);
         memcpy(d + (i * f->n + c) * f->size, &v, f->size);
      }
   return d;
}

int
main(void)
{
   void *egl = dlopen("libEGL.so.1", RTLD_NOW);
   if (!egl)
      return 2;
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
   if (!initialize(d, NULL, NULL))
      return 2;
   bindapi(EGL_OPENGL_API);
   EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8,
                  EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
   EGLConfig cfg;
   EGLint n = 0;
   if (!chooseconfig(d, ca, &cfg, 1, &n) || !n)
      return 2;
   EGLint sa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
   EGLSurface s = createpbuffer(d, cfg, sa);
   EGLContext c = createcontext(d, cfg, EGL_NO_CONTEXT, NULL);
   if (!s || !c || !makecurrent(d, s, s, c))
      return 2;
   GET(glClearBufferuiv);
   GET(glViewport);
   GET(glReadPixels);
   GET(glGenTextures);
   GET(glBindTexture);
   GET(glTexImage2D);
   GET(glTexBuffer);
   GET(glTexBufferRange);
   GET(glActiveTexture);
   GET(glGenFramebuffers);
   GET(glBindFramebuffer);
   GET(glFramebufferTexture2D);
   GET(glCheckFramebufferStatus);
   GET(glCreateShader);
   GET(glShaderSource);
   GET(glCompileShader);
   GET(glGetShaderiv);
   GET(glGetShaderInfoLog);
   GET(glCreateProgram);
   GET(glAttachShader);
   GET(glLinkProgram);
   GET(glGetProgramiv);
   GET(glGetProgramInfoLog);
   GET(glUseProgram);
   GET(glGetString);
   GET(glGetIntegerv);
   GET(glVertexPointer);
   GET(glEnableClientState);
   GET(glDisableClientState);
   GET(glDrawArrays);
   GET(glGenBuffers);
   GET(glBindBuffer);
   GET(glBufferData);
   GET(glBufferSubData);
   GET(glGetUniformLocation);
   GET(glUniform1i);
   GET(glGetError);
   GET(glFinish);
   GET(glBindFragDataLocation);

   const char *ext = (const char *)glGetString_(GL_EXTENSIONS);
   GLint max = 0, align = 0, vunits = 0;
   glGetIntegerv_(GL_MAX_TEXTURE_BUFFER_SIZE, &max);
   glGetIntegerv_(GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT, &align);
   glGetIntegerv_(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &vunits);
   printf("renderer: %s, GL %s, GLSL %s, %d texels a buffer, offsets of %d\n",
          glGetString_(GL_RENDERER), glGetString_(GL_VERSION),
          glGetString_(GL_SHADING_LANGUAGE_VERSION), max, align);
   if (!ext || !strstr(ext, "GL_ARB_texture_buffer_object")) {
      printf("FAIL: no ARB_texture_buffer_object (SGX_DEBUG=tbo,glsl130?)\n");
      return 1;
   }
   bool range = strstr(ext, "GL_ARB_texture_buffer_range") && align > 0;

   GLuint rt, fb;
   glGenTextures_(1, &rt);
   glBindTexture_(GL_TEXTURE_2D, rt);
   glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA32UI, S, S, 0, GL_RGBA_INTEGER,
                 GL_UNSIGNED_INT, NULL);
   glGenFramebuffers_(1, &fb);
   glBindFramebuffer_(GL_FRAMEBUFFER, fb);
   glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, rt, 0);
   if (glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      printf("FAIL: RGBA32UI FBO incomplete\n");
      return 2;
   }
   glViewport_(0, 0, S, S);
   static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
   glVertexPointer_(2, GL_FLOAT, 0, quad);
   glEnableClientState_(GL_VERTEX_ARRAY);

   GLuint vs = shader(GL_VERTEX_SHADER, vs_quad);
   GLuint pf = link(vs, fs_fetch("")), pi = link(vs, fs_fetch("i")),
          pu = link(vs, fs_fetch("u"));
   GLuint tex, buf;
   glGenTextures_(1, &tex);
   glGenBuffers_(1, &buf);
   glActiveTexture_(GL_TEXTURE0);
   glBindTexture_(GL_TEXTURE_BUFFER, tex);
   glBindBuffer_(GL_TEXTURE_BUFFER, buf);

   int bad = 0, skipped = 0;
   for (unsigned k = 0; k < sizeof(fmts) / sizeof(fmts[0]); k++) {
      const struct fmt *f = &fmts[k];
      GLuint p = f->kind == SI ? pi : f->kind == UI ? pu : pf;
      unsigned bytes = f->size * f->n, wrong = 0;
      void *data = fill(f, N, k);

      while (glGetError_())
         ;
      glBufferData_(GL_TEXTURE_BUFFER, N * bytes, data, GL_STATIC_DRAW);
      glTexBuffer_(GL_TEXTURE_BUFFER, f->gl, buf);
      if (glGetError_()) {
         printf("skip %s: glTexBuffer refused it\n", f->name);
         skipped++;
         free(data);
         continue;
      }
      /* every texel and 24 past the end */
      draw(p, 0, 0);
      wrong += check(f, 0, S * S, 0, N, k, "texels");
      /* textureSize */
      draw(p, 0, 1);
      if (px[0] != N) {
         printf("  %s textureSize: %u, want %u\n", f->name, px[0], N);
         wrong++;
      }
      /* from texel 16 on (an offset of 16 texels, so a multiple of 16 bytes
       * for every texel size), its last pixels past the range's end */
      if (range && 16 * bytes % align == 0) {
         unsigned first = 16, n_range = 600;

         glTexBufferRange_(GL_TEXTURE_BUFFER, f->gl, buf, first * bytes,
                           n_range * bytes);
         draw(p, 0, 0);
         /* (want: texel first + p of the buffer, n_range from first) */
         for (unsigned q = 0; q < S * S; q++) {
            uint32_t w[4];

            want_texel(f, q < n_range ? q + first : N, N, k, w);
            for (unsigned c = 0; c < 4; c++)
               if (!close_enough(f, px[4 * q + c], w[c])) {
                  if (!wrong)
                     printf("  %s range: pixel %u channel %u got %08x want %08x\n",
                            f->name, q, c, px[4 * q + c], w[c]);
                  wrong++;
               }
         }
         glTexBuffer_(GL_TEXTURE_BUFFER, f->gl, buf);
      }
      printf("%s %s: %u wrong\n", wrong ? "FAIL" : "ok  ", f->name, wrong);
      bad += !!wrong;
      free(data);
   }

   /* a rewrite between two draws of one frame: the left half drawn with
    * salt 100's texels, the buffer rewritten with salt 101's, the right
    * half drawn; one readback */
   {
      const struct fmt *f = &fmts[30]; /* RGBA32UI */
      void *a = fill(f, N, 100), *b2 = fill(f, N, 101);
      static const GLfloat left[] = {-1, -1, 0, -1, -1, 1, 0, 1};
      static const GLfloat right[] = {0, -1, 1, -1, 0, 1, 1, 1};
      static const GLuint zero[4];
      unsigned wrong = 0;

      glBufferData_(GL_TEXTURE_BUFFER, N * 16, a, GL_DYNAMIC_DRAW);
      glTexBuffer_(GL_TEXTURE_BUFFER, f->gl, buf);
      glClearBufferuiv_(GL_COLOR, 0, zero);
      glUseProgram_(pu);
      glUniform1i_(glGetUniformLocation_(pu, "base"), 0);
      glUniform1i_(glGetUniformLocation_(pu, "size_check"), 0);
      glVertexPointer_(2, GL_FLOAT, 0, left);
      glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
      glBufferSubData_(GL_TEXTURE_BUFFER, 0, N * 16, b2);
      glVertexPointer_(2, GL_FLOAT, 0, right);
      glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
      glReadPixels_(0, 0, S, S, GL_RGBA_INTEGER, GL_UNSIGNED_INT, px);
      for (unsigned y = 0; y < S; y++) {
         wrong += check(f, y * S, S / 2, 0, N, 100, "before the rewrite");
         wrong += check(f, y * S + S / 2, S / 2, 0, N, 101, "after it");
      }
      printf("%s rewrite between two draws: %u wrong\n", wrong ? "FAIL" : "ok  ",
             wrong);
      bad += !!wrong;
      glVertexPointer_(2, GL_FLOAT, 0, quad);
      free(a);
      free(b2);
   }

   /* a vertex program's fetch: point v at pixel v takes texel v in a flat
    * varying */
   if (vunits >= 1) {
      const struct fmt *f = &fmts[30]; /* RGBA32UI */
      void *a = fill(f, N, 102);
      static const char *vsrc =
         "#version 140\n"
         "uniform usamplerBuffer b;\n"
         "flat out uvec4 t;\n"
         "void main() {\n"
         "   int v = gl_VertexID;\n"
         "   gl_Position = vec4((vec2(v % 32, v / 32) + 0.5) / 16.0 - 1.0, 0.0, 1.0);\n"
         "   gl_PointSize = 1.0;\n"
         "   t = texelFetch(b, v);\n"
         "}\n";
      static const char *fsrc = "#version 140\n"
                                "flat in uvec4 t;\n"
                                "out uvec4 o;\n"
                                "void main() { o = t; }\n";
      static const GLuint zero[4];
      GLuint pv = link(shader(GL_VERTEX_SHADER, vsrc),
                       shader(GL_FRAGMENT_SHADER, fsrc));
      unsigned wrong;

      glBufferData_(GL_TEXTURE_BUFFER, N * 16, a, GL_STATIC_DRAW);
      glTexBuffer_(GL_TEXTURE_BUFFER, f->gl, buf);
      glDisableClientState_(GL_VERTEX_ARRAY);
      glClearBufferuiv_(GL_COLOR, 0, zero);
      glUseProgram_(pv);
      glUniform1i_(glGetUniformLocation_(pv, "b"), 0);
      glDrawArrays_(GL_POINTS, 0, S * S);
      glReadPixels_(0, 0, S, S, GL_RGBA_INTEGER, GL_UNSIGNED_INT, px);
      wrong = check(f, 0, S * S, 0, N, 102, "vertex fetch");
      printf("%s vertex program texelFetch: %u wrong\n", wrong ? "FAIL" : "ok  ",
             wrong);
      bad += !!wrong;
      free(a);
   } else {
      printf("skip vertex program texelFetch: no vertex units (SGX_DEBUG=vtf)\n");
   }
   if (glGetError_())
      printf("GL error\n");
   printf("%s (%d skipped)\n", bad ? "FAIL" : "PASS", skipped);
   return bad ? 1 : 0;
}
