/*
 * Copyright © 2026 servidorjarvis1-svg
 * SPDX-License-Identifier: MIT
 */

/*
 * The formats and caps ES 3.0 asks of the driver short of texture arrays and
 * transform feedback (the native integers, an ES 3.0 context), each sampled
 * into a 64x64 RGBA8 FBO and read back:
 *  1. textureLod and textureGrad in a fragment shader (ARB_shader_texture_lod)
 *  2. DEPTH_COMPONENT32F and DEPTH32F_STENCIL8 attachments: depth and
 *     stencil tests (ARB_depth_buffer_float)
 *  3. RGBA32F: values outside [0, 1], NEAREST; LINEAR in the shader
 *  4. RGBA16F, uploaded HALF_FLOAT: NEAREST and LINEAR
 *  5. R11F_G11F_B10F sampled; rendered into and sampled back
 *  6. RGB9_E5 sampled
 *  7. RGB10_A2 sampled
 *  8. RGBA8_SNORM, R8_SNORM: -1..1, LINEAR by the TPU
 *  9. RGBA32F with mipmaps, NEAREST_MIPMAP_NEAREST, textureLod level 1
 * 10. REPEAT and MIRRORED_REPEAT on RGBA16F, RGBA32F and R32F (stored as F32
 *     words, read strided), sampled over [-0.5, 1.5]: NEAREST and LINEAR,
 *     every pixel against a CPU reference; RGBA8 as the control
 *
 *   MESA_GLES_VERSION_OVERRIDE=3.0 DISPLAY=:0 sgx_es3fmt
 *   (or EGL_PLATFORM=surfaceless in place of DISPLAY)
 *
 * zig cc -target x86-linux-gnu.2.36 -isystem <root>/usr/include -o sgx_es3fmt
 * sgx_es3fmt.c <root>/usr/lib/i386-linux-gnu/libEGL.so.1
 * <root>/usr/lib/i386-linux-gnu/libGLESv2.so.2 -lm
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define W 64
#define H 64
#define T10 16

static int fails;

static GLuint
prog(const char *fs)
{
   static const char *vs = "#version 300 es\n"
                           "in vec2 a;\nout vec2 uv;\n"
                           "void main() { uv = a * 0.5 + 0.5;"
                           " gl_Position = vec4(a, 0.0, 1.0); }\n";
   GLuint p = glCreateProgram();
   const char *src[2] = {vs, fs};
   GLenum ty[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};

   for (int i = 0; i < 2; i++) {
      GLuint s = glCreateShader(ty[i]);
      GLint ok;

      glShaderSource(s, 1, &src[i], NULL);
      glCompileShader(s);
      glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
      if (!ok) {
         char l[2048];
         glGetShaderInfoLog(s, sizeof l, NULL, l);
         printf("compile: %s\n", l);
      }
      glAttachShader(p, s);
   }
   glBindAttribLocation(p, 0, "a");
   glLinkProgram(p);
   GLint ok;
   glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok) {
      char l[2048];
      glGetProgramInfoLog(p, sizeof l, NULL, l);
      printf("link: %s\n", l);
   }
   return p;
}

static void
check(const char *name, int x, int y, int r, int g, int b, int a, int tol)
{
   unsigned char p[4];
   int e[4] = {r, g, b, a};
   int ok = 1;

   glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
   for (int c = 0; c < 4; c++)
      if (e[c] >= 0 && abs(p[c] - e[c]) > tol)
         ok = 0;
   printf("%-46s (%2d,%2d) got %3u %3u %3u %3u want %3d %3d %3d %3d: %s\n",
          name, x, y, p[0], p[1], p[2], p[3], r, g, b, a, ok ? "OK" : "FAIL");
   fails += !ok;
}

static void
quad(float z)
{
   const float v[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};

   (void)z;
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
   glDrawArrays(GL_TRIANGLES, 0, 6);
}

static GLuint
tex2d(GLenum ifmt, int w, int h, GLenum fmt, GLenum type, const void *data,
      GLenum filter)
{
   GLuint t;

   glGenTextures(1, &t);
   glBindTexture(GL_TEXTURE_2D, t);
   glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, data);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   return t;
}

/* draw p sampling t on unit 0, uniform "k" set to k */
static void
draw(GLuint p, GLuint t, const float *k)
{
   glUseProgram(p);
   glUniform1i(glGetUniformLocation(p, "s"), 0);
   if (k)
      glUniform4fv(glGetUniformLocation(p, "k"), 1, k);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, t);
   quad(0);
}

static uint16_t
half(float f)
{
   uint32_t x;
   memcpy(&x, &f, 4);
   uint32_t s = (x >> 16) & 0x8000, e = (x >> 23) & 0xff, m = x & 0x7fffff;

   if (e == 0)
      return s;
   int ne = (int)e - 127 + 15;
   if (ne <= 0)
      return s;
   if (ne >= 31)
      return s | 0x7c00;
   return s | ne << 10 | (m + 0x1000) >> 13;
}

/* section 10's wrap of texel index i, and its texel (x / 15, y / 15, 0.25, 1) */
static int
wrap10(int i, GLenum m)
{
   if (m == GL_REPEAT)
      return ((i % T10) + T10) % T10;
   int k = ((i % (2 * T10)) + 2 * T10) % (2 * T10);
   return k < T10 ? k : 2 * T10 - 1 - k;
}

static float
texel10(int x, int y, int c)
{
   return c == 0 ? x / (T10 - 1.0f) : c == 1 ? y / (T10 - 1.0f) : c == 2 ? 0.25f : 1.0f;
}

/* the reference texel at (u, v), NEAREST or LINEAR, channel c */
static float
ref10(double u, double v, GLenum m, bool linear, int c)
{
   if (!linear)
      return texel10(wrap10((int)floor(u * T10), m), wrap10((int)floor(v * T10), m), c);
   double x = u * T10 - 0.5, y = v * T10 - 0.5;
   int x0 = (int)floor(x), y0 = (int)floor(y);
   double fx = x - x0, fy = y - y0;
   float a = texel10(wrap10(x0, m), wrap10(y0, m), c),
         b = texel10(wrap10(x0 + 1, m), wrap10(y0, m), c),
         d = texel10(wrap10(x0, m), wrap10(y0 + 1, m), c),
         e = texel10(wrap10(x0 + 1, m), wrap10(y0 + 1, m), c);
   return (float)((a * (1 - fx) + b * fx) * (1 - fy) + (d * (1 - fx) + e * fx) * fy);
}

int
main(void)
{
   EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLint maj, min;

   if (!eglInitialize(d, &maj, &min)) {
      printf("eglInitialize failed\n");
      return 1;
   }
   eglBindAPI(EGL_OPENGL_ES_API);
   const EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                        EGL_SURFACE_TYPE, 0, EGL_NONE};
   EGLConfig cfg;
   EGLint n = 0;
   eglChooseConfig(d, ca, &cfg, 1, &n);
   const EGLint xa3[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
   EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa3);
   if (c == EGL_NO_CONTEXT ||
       !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
      printf("no ES 3.0 context 0x%x\n", eglGetError());
      return 1;
   }
   printf("%s\n", glGetString(GL_VERSION));
   const char *ext = (const char *)glGetString(GL_EXTENSIONS);
   static const char *want[] = {
      "GL_EXT_sRGB",         "GL_OES_texture_float",
      "GL_OES_texture_half_float", "GL_OES_texture_half_float_linear",
      "GL_EXT_texture_type_2_10_10_10_REV", "GL_OES_depth_texture_cube_map",
   };
   for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++)
      printf("  %-40s %s\n", want[i], strstr(ext, want[i]) ? "yes" : "no");

   GLuint ct, fb;
   glGenTextures(1, &ct);
   glBindTexture(GL_TEXTURE_2D, ct);
   glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                          ct, 0);
   glViewport(0, 0, W, H);
   const int t = 2;

   /* 1 */
   {
      GLuint tx;
      glGenTextures(1, &tx);
      glBindTexture(GL_TEXTURE_2D, tx);
      glTexStorage2D(GL_TEXTURE_2D, 4, GL_RGBA8, 32, 32);
      for (int l = 0; l < 4; l++) {
         int s = 32 >> l;
         unsigned char *px = malloc(s * s * 4);
         for (int i = 0; i < s * s; i++) {
            px[4 * i] = l * 60;
            px[4 * i + 1] = 255 - l * 60;
            px[4 * i + 2] = 0;
            px[4 * i + 3] = 255;
         }
         glTexSubImage2D(GL_TEXTURE_2D, l, 0, 0, s, s, GL_RGBA,
                         GL_UNSIGNED_BYTE, px);
         free(px);
      }
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                      GL_NEAREST_MIPMAP_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      GLuint p = prog("#version 300 es\nprecision highp float;\n"
                      "uniform sampler2D s;\nin vec2 uv;\nout vec4 o;\n"
                      "void main() { o = textureLod(s, uv, 2.0); }\n");
      draw(p, tx, NULL);
      check("1a textureLod level 2", 32, 32, 120, 135, 0, 255, t);
      GLuint p2 = prog("#version 300 es\nprecision highp float;\n"
                       "uniform sampler2D s;\nin vec2 uv;\nout vec4 o;\n"
                       "void main() { o = textureGrad(s, uv, vec2(4.0/32.0, 0.0),"
                       " vec2(0.0, 4.0/32.0)); }\n");
      draw(p2, tx, NULL);
      check("1b textureGrad (4 texels a pixel: level 2)", 32, 32, 120, 135, 0,
            255, t);
   }

   GLuint pc = prog("#version 300 es\nprecision highp float;\n"
                    "uniform vec4 k;\nout vec4 o;\n"
                    "void main() { o = k; }\n");

   /* 2 */
   static const GLenum zf[3] = {GL_DEPTH_COMPONENT32F, GL_DEPTH32F_STENCIL8,
                                GL_DEPTH24_STENCIL8};
   static const char *zn[3] = {"2a DEPTH_COMPONENT32F", "2b DEPTH32F_STENCIL8",
                               "2e DEPTH24_STENCIL8 (control)"};
   for (int zi = 0; zi < 3; zi++) {
      int st = zi > 0;
      GLuint rb, f2;
      glGenRenderbuffers(1, &rb);
      glBindRenderbuffer(GL_RENDERBUFFER, rb);
      glRenderbufferStorage(GL_RENDERBUFFER, zf[zi], W, H);
      glGenFramebuffers(1, &f2);
      glBindFramebuffer(GL_FRAMEBUFFER, f2);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, ct, 0);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                st ? GL_DEPTH_STENCIL_ATTACHMENT
                                   : GL_DEPTH_ATTACHMENT,
                                GL_RENDERBUFFER, rb);
      GLenum s = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      if (s != GL_FRAMEBUFFER_COMPLETE) {
         printf("%s FBO status 0x%x: FAIL\n", zn[zi], s);
         fails++;
         continue;
      }
      GLuint pz = prog("#version 300 es\nprecision highp float;\n"
                       "uniform vec4 k;\nout vec4 o;\n"
                       "void main() { o = vec4(k.rgb, 1.0); gl_FragDepth = k.a; }\n");
      glClearColor(0, 0, 0, 1);
      glClearDepthf(1.0f);
      glClearStencil(0);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
              (st ? GL_STENCIL_BUFFER_BIT : 0));
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(GL_LESS);
      const float near[4] = {1, 0, 0, 0.3f}, far[4] = {0, 1, 0, 0.30001f};
      glUseProgram(pz);
      glUniform4fv(glGetUniformLocation(pz, "k"), 1, near);
      quad(0);
      glUniform4fv(glGetUniformLocation(pz, "k"), 1, far);
      quad(0);
      char nm[80];
      snprintf(nm, sizeof nm, "%s: depth 0.3 < 0.30001", zn[zi]);
      check(nm, 32, 32, 255, 0, 0, 255, t);
      glDisable(GL_DEPTH_TEST);
      if (st) {
         glEnable(GL_STENCIL_TEST);
         glStencilFunc(GL_ALWAYS, 5, 0xff);
         glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
         glEnable(GL_SCISSOR_TEST);
         glScissor(0, 0, 32, 64);
         const float z[4] = {0, 0, 1, 1};
         glUseProgram(pc);
         glUniform4fv(glGetUniformLocation(pc, "k"), 1, z);
         quad(0);
         glDisable(GL_SCISSOR_TEST);
         glStencilFunc(GL_EQUAL, 5, 0xff);
         glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
         const float w[4] = {1, 1, 1, 1};
         glUniform4fv(glGetUniformLocation(pc, "k"), 1, w);
         quad(0);
         glDisable(GL_STENCIL_TEST);
         snprintf(nm, sizeof nm, "%s: stencil 5 drawn", zn[zi]);
         check(nm, 16, 32, 255, 255, 255, 255, t);
         snprintf(nm, sizeof nm, "%s: stencil 0 kept", zn[zi]);
         check(nm, 48, 32, 255, 0, 0, 255, t);
      }
      glBindFramebuffer(GL_FRAMEBUFFER, fb);
   }

   /* the float texel mapped into [0, 1]: (v - k0) * k1 */
   GLuint pm = prog("#version 300 es\nprecision highp float;\n"
                    "uniform highp sampler2D s;\nuniform vec4 k;\nin vec2 uv;\n"
                    "out vec4 o;\n"
                    "void main() { o = (texture(s, uv) - k.x) * k.y; }\n");
   const float k4[4] = {-4.0f, 0.125f, 0, 0}; /* -4..4 -> 0..1 */

   /* 3, 4 */
   for (int hf = 0; hf < 2; hf++) {
      const float v[8] = {1.5f, -2.0f, 3.0f, 0.25f, -1.5f, 2.0f, -3.0f, 3.75f};
      uint16_t hv[8];
      for (int i = 0; i < 8; i++)
         hv[i] = half(v[i]);
      GLuint tx = hf ? tex2d(GL_RGBA16F, 2, 1, GL_RGBA, GL_HALF_FLOAT, hv,
                             GL_NEAREST)
                     : tex2d(GL_RGBA32F, 2, 1, GL_RGBA, GL_FLOAT, v, GL_NEAREST);
      const char *nm = hf ? "4 RGBA16F" : "3 RGBA32F";
      char l[64];
      draw(pm, tx, k4);
      snprintf(l, sizeof l, "%sa NEAREST texel 0", nm);
      check(l, 8, 32, 175, 64, 223, 135, t);
      snprintf(l, sizeof l, "%sb NEAREST texel 1", nm);
      check(l, 56, 32, 80, 191, 32, 247, t);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      draw(pm, tx, k4);
      snprintf(l, sizeof l, "%sc LINEAR halfway", nm);
      check(l, 32, 32, 128, 128, 128, 191, 5);
   }

   /* 5 */
   {
      /* (1.5, 0.25, 3.0) in R11F_G11F_B10F: exact in all three */
      const float v[3] = {1.5f, 0.25f, 3.0f};
      GLuint tx = tex2d(GL_R11F_G11F_B10F, 1, 1, GL_RGB, GL_FLOAT, v, GL_NEAREST);
      draw(pm, tx, k4);
      check("5a R11F_G11F_B10F sampled", 32, 32, 175, 135, 223, 159, t);
      GLuint rt, f3;
      glGenTextures(1, &rt);
      glBindTexture(GL_TEXTURE_2D, rt);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_R11F_G11F_B10F, W, H);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glGenFramebuffers(1, &f3);
      glBindFramebuffer(GL_FRAMEBUFFER, f3);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, rt, 0);
      GLenum s = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      printf("5b R11F_G11F_B10F render target: status 0x%x (informative: ES 3.0"
             " doesn't ask it)\n", s);
      if (s == GL_FRAMEBUFFER_COMPLETE) {
         const float kk[4] = {2.5f, 0.5f, 3.0f, 1.0f};
         glUseProgram(pc);
         glUniform4fv(glGetUniformLocation(pc, "k"), 1, kk);
         quad(0);
         glBindFramebuffer(GL_FRAMEBUFFER, fb);
         draw(pm, rt, k4);
         check("5c R11F_G11F_B10F rendered and sampled", 32, 32, 207, 143, 223,
               159, t);
      }
      glBindFramebuffer(GL_FRAMEBUFFER, fb);
   }

   /* 6: RGB9_E5 of (1.5, 0.25, 3.0) */
   {
      const float v[3] = {1.5f, 0.25f, 3.0f};
      GLuint tx = tex2d(GL_RGB9_E5, 1, 1, GL_RGB, GL_FLOAT, v, GL_NEAREST);
      draw(pm, tx, k4);
      check("6 RGB9_E5 sampled", 32, 32, 175, 135, 223, 159, t);
   }

   /* 7: RGB10_A2 (1023, 512, 0, 3) */
   {
      uint32_t w = 1023u | 512u << 10 | 0u << 20 | 3u << 30;
      GLuint tx = tex2d(GL_RGB10_A2, 1, 1, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV,
                        &w, GL_NEAREST);
      draw(pm, tx, (const float[4]){0, 1, 0, 0});
      check("7 RGB10_A2 (1023, 512, 0, 3)", 32, 32, 255, 128, 0, 255, t);
   }

   /* 8 */
   {
      const int8_t v[8] = {127, -127, -128, 0, -127, 127, 0, 127};
      GLuint tx = tex2d(GL_RGBA8_SNORM, 2, 1, GL_RGBA, GL_BYTE, v, GL_NEAREST);
      const float km[4] = {-1, 0.5f, 0, 0};
      draw(pm, tx, km);
      check("8a RGBA8_SNORM texel 0 (1, -1, -1, 0)", 8, 32, 255, 0, 0, 128, t);
      check("8b RGBA8_SNORM texel 1 (-1, 1, 0, 1)", 56, 32, 0, 255, 128, 255, t);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      draw(pm, tx, km);
      check("8c RGBA8_SNORM LINEAR halfway (0, 0, -0.5, 0.5)", 32, 32, 128, 128,
            64, 191, 5);
      const int8_t r[1] = {-64};
      GLuint t2 = tex2d(GL_R8_SNORM, 1, 1, GL_RED, GL_BYTE, r, GL_NEAREST);
      draw(pm, t2, km);
      check("8d R8_SNORM -64: (-0.504, 0, 0, 1)", 32, 32, 63, 128, 128, 255, t);
   }

   /* 9 */
   {
      GLuint tx;
      glGenTextures(1, &tx);
      glBindTexture(GL_TEXTURE_2D, tx);
      glTexStorage2D(GL_TEXTURE_2D, 3, GL_RGBA32F, 4, 4);
      for (int l = 0; l < 3; l++) {
         int s = 4 >> l;
         float px[64];
         for (int i = 0; i < s * s; i++) {
            px[4 * i] = l;
            px[4 * i + 1] = -l;
            px[4 * i + 2] = 0;
            px[4 * i + 3] = 1;
         }
         glTexSubImage2D(GL_TEXTURE_2D, l, 0, 0, s, s, GL_RGBA, GL_FLOAT, px);
      }
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                      GL_NEAREST_MIPMAP_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      GLuint p = prog("#version 300 es\nprecision highp float;\n"
                      "uniform highp sampler2D s;\nuniform vec4 k;\nin vec2 uv;\n"
                      "out vec4 o;\n"
                      "void main() { o = (textureLod(s, uv, 1.0) - k.x) * k.y; }\n");
      draw(p, tx, k4);
      /* informative: F32 planes are strided, one level (sgx_tex_words) */
      unsigned char q[4];
      glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q);
      printf("9 RGBA32F mipmapped, textureLod level 1: got %u %u %u %u, level 1"
             " is 159 96 128 159 (informative: one level, F32 planes strided)\n",
             q[0], q[1], q[2], q[3]);
   }

   /* 10 */
   {
      static float f4[T10 * T10 * 4], f1[T10 * T10];
      static unsigned char b4[T10 * T10 * 4];
      static unsigned char px[W * H * 4];

      for (int y = 0; y < T10; y++)
         for (int x = 0; x < T10; x++)
            for (int c = 0; c < 4; c++) {
               f4[4 * (y * T10 + x) + c] = texel10(x, y, c);
               b4[4 * (y * T10 + x) + c] =
                  (unsigned char)lrintf(texel10(x, y, c) * 255.0f);
               if (!c)
                  f1[y * T10 + x] = texel10(x, y, 0);
            }
      GLuint p = prog("#version 300 es\nprecision highp float;\n"
                      "uniform highp sampler2D s;\nin vec2 uv;\nout vec4 o;\n"
                      "void main() { o = texture(s, uv * 2.0 - 0.5); }\n");
      static const struct {
         const char *name;
         GLenum ifmt, fmt, type;
         const void *data;
         int comps;
      } f[] = {
         {"RGBA8 (control)", GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, b4, 2},
         {"RGBA16F", GL_RGBA16F, GL_RGBA, GL_FLOAT, f4, 2},
         {"RGBA32F", GL_RGBA32F, GL_RGBA, GL_FLOAT, f4, 2},
         {"R32F", GL_R32F, GL_RED, GL_FLOAT, f1, 1},
      };
      static const GLenum mode[] = {GL_REPEAT, GL_MIRRORED_REPEAT};

      for (unsigned i = 0; i < sizeof(f) / sizeof(f[0]); i++)
         for (int lin = 0; lin < 2; lin++)
            for (int m = 0; m < 2; m++) {
               GLuint tx = tex2d(f[i].ifmt, T10, T10, f[i].fmt, f[i].type,
                                 f[i].data, lin ? GL_LINEAR : GL_NEAREST);
               int bad = 0, fx = -1, fy = -1;

               glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, mode[m]);
               glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, mode[m]);
               draw(p, tx, NULL);
               glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
               for (int y = 0; y < H; y++)
                  for (int x = 0; x < W; x++) {
                     double u = ((x + 0.5) / W) * 2.0 - 0.5,
                            v = ((y + 0.5) / H) * 2.0 - 0.5;
                     int ok = 1;

                     for (int c = 0; c < f[i].comps; c++) {
                        int want = (int)lrint(ref10(u, v, mode[m], lin, c) * 255.0);
                        if (abs(px[4 * (y * W + x) + c] - want) > t + 1)
                           ok = 0;
                     }
                     if (!ok && !bad++)
                        fx = x, fy = y;
                  }
               printf("10 %-15s %-7s %-15s %4d of %d pixels wrong (first %d,%d): %s\n",
                      f[i].name, lin ? "LINEAR" : "NEAREST",
                      m ? "MIRRORED_REPEAT" : "REPEAT", bad, W * H, fx, fy,
                      bad ? "FAIL" : "OK");
               fails += bad != 0;
               glDeleteTextures(1, &tx);
            }
   }

   GLenum e = glGetError();
   printf("GL error 0x%x\n", e);
   printf("result: %d failed\n", fails + (e != GL_NO_ERROR));
   return fails || e != GL_NO_ERROR;
}
