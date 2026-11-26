/*
 * Copyright © 2026 servidorjarvis1-svg
 * SPDX-License-Identifier: MIT
 */

/*
 * EXT_texture_compression_rgtc on an ES 3.0 context (SGX_DEBUG=glsl130).
 * The driver has no RGTC format: st offers the extension through its
 * compressed fallback (allow_compressed_fallback, on by default) once
 * R8 / RG8 UNORM and SNORM sample, and decodes each upload on the CPU into
 * one of them. This samples each of the four formats back and checks every
 * texel against a CPU decode of the blocks:
 *  1. RED_RGTC1, SIGNED_RED_RGTC1, RED_GREEN_RGTC2, SIGNED_RED_GREEN_RGTC2:
 *     a 16x16 texture of random blocks (both palette modes, -128 included),
 *     NEAREST, each texel on 4x4 pixels of a 64x64 RGBA8 FBO; a signed
 *     texel comes out as v * 0.5 + 0.5
 *  2. the same through glCompressedTexSubImage2D into an 8x8 corner
 *  3. LINEAR between two texels of RED_RGTC1
 *
 *   MESA_GLES_VERSION_OVERRIDE=3.0 SGX_DEBUG=glsl130 EGL_PLATFORM=surfaceless
 *   sgx_rgtc
 *
 * zig cc -target x86-linux-gnu.2.36 -isystem <root>/usr/include -o sgx_rgtc
 * sgx_rgtc.c <root>/usr/lib/i386-linux-gnu/libEGL.so.1
 * <root>/usr/lib/i386-linux-gnu/libGLESv2.so.2 -lm
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define GL_COMPRESSED_RED_RGTC1_EXT 0x8DBB
#define GL_COMPRESSED_SIGNED_RED_RGTC1_EXT 0x8DBC
#define GL_COMPRESSED_RED_GREEN_RGTC2_EXT 0x8DBD
#define GL_COMPRESSED_SIGNED_RED_GREEN_RGTC2_EXT 0x8DBE

#define W 64
#define H 64
#define T 16

static int fails;
static uint32_t seed = 0x2026u;

static unsigned
rnd(void)
{
   seed = seed * 1103515245u + 12345u;
   return seed >> 16;
}

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
   return p;
}

static void
quad(void)
{
   const float v[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};

   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
   glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* one BC4 block's 16 values, as GL decodes it: 0..255 unsigned, or -127..127
 * signed (-128 read as -127) */
static void
bc4(const uint8_t *b, int sgn, float out[16])
{
   float pal[8];
   int r0 = sgn ? (int8_t)b[0] : b[0], r1 = sgn ? (int8_t)b[1] : b[1];

   if (sgn) {
      if (r0 == -128)
         r0 = -127;
      if (r1 == -128)
         r1 = -127;
   }
   pal[0] = r0;
   pal[1] = r1;
   if (r0 > r1) {
      for (int i = 2; i < 8; i++)
         pal[i] = ((8 - i) * r0 + (i - 1) * r1) / 7.0f;
   } else {
      for (int i = 2; i < 6; i++)
         pal[i] = ((6 - i) * r0 + (i - 1) * r1) / 5.0f;
      pal[6] = sgn ? -127 : 0;
      pal[7] = sgn ? 127 : 255;
   }
   uint64_t bits = 0;
   for (int i = 0; i < 6; i++)
      bits |= (uint64_t)b[2 + i] << (8 * i);
   for (int i = 0; i < 16; i++)
      out[i] = pal[bits >> (3 * i) & 7] / (sgn ? 127.0f : 255.0f);
}

/* random blocks for a w x h texture of c channels (1 or 2), half of them in
 * each palette mode, some ends at -128 / 127 / 0 / 255 */
static void
make(uint8_t *blk, int w, int h, int c)
{
   int n = (w / 4) * (h / 4) * c;

   for (int i = 0; i < n; i++) {
      uint8_t *b = blk + 8 * i;
      for (int j = 0; j < 8; j++)
         b[j] = rnd();
      if (i % 4 == 1) {
         b[0] = 0x80;
         b[1] = 0x7f;
      } else if (i % 4 == 2) {
         b[0] = 0xff;
         b[1] = 0x00;
      }
   }
}

/* texel (x, y) channel ch of w x h blocks */
static float
texel(const uint8_t *blk, int w, int c, int sgn, int x, int y, int ch)
{
   float v[16];
   int bi = (y / 4) * (w / 4) + x / 4;

   bc4(blk + 8 * (bi * c + ch), sgn, v);
   return v[(y % 4) * 4 + x % 4];
}

static int
out8(float v, int sgn)
{
   return (int)lrintf((sgn ? v * 0.5f + 0.5f : v) * 255.0f);
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
   int has = strstr(ext, "GL_EXT_texture_compression_rgtc") != NULL;
   printf("GL_EXT_texture_compression_rgtc: %s\n", has ? "yes" : "no");
   if (!has) {
      printf("result: not exposed, nothing to check\n");
      return 0;
   }

   GLuint ct, fb;
   glGenTextures(1, &ct);
   glBindTexture(GL_TEXTURE_2D, ct);
   glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                          ct, 0);
   glViewport(0, 0, W, H);
   GLuint pu = prog("#version 300 es\nprecision highp float;\n"
                    "uniform highp sampler2D s;\nin vec2 uv;\nout vec4 o;\n"
                    "void main() { o = vec4(texture(s, uv).rg, 0.0, 1.0); }\n");
   GLuint ps = prog("#version 300 es\nprecision highp float;\n"
                    "uniform highp sampler2D s;\nin vec2 uv;\nout vec4 o;\n"
                    "void main() { o = vec4(texture(s, uv).rg * 0.5 + 0.5,"
                    " 0.0, 1.0); }\n");
   static const struct {
      const char *name;
      GLenum fmt;
      int c, sgn;
   } f[] = {
      {"RED_RGTC1", GL_COMPRESSED_RED_RGTC1_EXT, 1, 0},
      {"SIGNED_RED_RGTC1", GL_COMPRESSED_SIGNED_RED_RGTC1_EXT, 1, 1},
      {"RED_GREEN_RGTC2", GL_COMPRESSED_RED_GREEN_RGTC2_EXT, 2, 0},
      {"SIGNED_RED_GREEN_RGTC2", GL_COMPRESSED_SIGNED_RED_GREEN_RGTC2_EXT, 2, 1},
   };
   static uint8_t blk[T * T / 16 * 2 * 8], sub[8 * 8 / 16 * 2 * 8];
   static uint8_t px[W * H * 4];
   const int tol = 3;

   for (int pass = 0; pass < 2; pass++)
      for (unsigned i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
         GLuint tx;
         int size = T * T / 16 * f[i].c * 8, sub_size = 8 * 8 / 16 * f[i].c * 8;

         make(blk, T, T, f[i].c);
         glGenTextures(1, &tx);
         glBindTexture(GL_TEXTURE_2D, tx);
         glCompressedTexImage2D(GL_TEXTURE_2D, 0, f[i].fmt, T, T, 0, size, blk);
         if (pass) {
            /* 2: an 8x8 corner replaced, at (4, 8) */
            make(sub, 8, 8, f[i].c);
            glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 4, 8, 8, 8, f[i].fmt,
                                      sub_size, sub);
         }
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
         glUseProgram(f[i].sgn ? ps : pu);
         glUniform1i(glGetUniformLocation(f[i].sgn ? ps : pu, "s"), 0);
         quad();
         glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
         int bad = 0, fx = -1, fy = -1, got = 0, want = 0;
         for (int y = 0; y < T; y++)
            for (int x = 0; x < T; x++) {
               int in_sub = pass && x >= 4 && x < 12 && y >= 8 && y < 16;
               int pxx = x * (W / T) + 1, pyy = y * (H / T) + 1;
               for (int ch = 0; ch < f[i].c; ch++) {
                  float v = in_sub ? texel(sub, 8, f[i].c, f[i].sgn, x - 4,
                                           y - 8, ch)
                                   : texel(blk, T, f[i].c, f[i].sgn, x, y, ch);
                  int w8 = out8(v, f[i].sgn), g = px[4 * (pyy * W + pxx) + ch];
                  if (abs(g - w8) > tol && !bad++) {
                     fx = x, fy = y, got = g, want = w8;
                  }
               }
            }
         printf("%d %-24s %3d of %d texel channels wrong", pass + 1,
                f[i].name, bad, T * T * f[i].c);
         if (bad)
            printf(" (first %d,%d got %d want %d)", fx, fy, got, want);
         printf(": %s\n", bad ? "FAIL" : "OK");
         fails += bad != 0;
         glDeleteTextures(1, &tx);
      }

   /* 3: LINEAR halfway between texels 0 and 1 of a block 0..255 */
   {
      uint8_t b[8] = {255, 0, 0, 0, 0, 0, 0, 0};
      GLuint tx;

      /* texel 1 (bits 3-5) index 1, 0; every other texel index 0, 255 */
      b[2] = 1 << 3;
      glGenTextures(1, &tx);
      glBindTexture(GL_TEXTURE_2D, tx);
      glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RED_RGTC1_EXT, 4, 4,
                             0, 8, b);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      GLuint pl = prog("#version 300 es\nprecision highp float;\n"
                       "uniform highp sampler2D s;\nout vec4 o;\n"
                       "void main() { o = vec4(texture(s, vec2(0.25, 0.125)).r,"
                       " 0.0, 0.0, 1.0); }\n");
      glUseProgram(pl);
      glUniform1i(glGetUniformLocation(pl, "s"), 0);
      quad();
      unsigned char q[4];
      glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q);
      int ok = abs(q[0] - 128) <= tol;
      printf("3 RED_RGTC1 LINEAR halfway 255 / 0: got %u want 128: %s\n", q[0],
             ok ? "OK" : "FAIL");
      fails += !ok;
      glDeleteTextures(1, &tx);
   }

   GLenum e = glGetError();
   printf("GL error 0x%x\n", e);
   printf("result: %d failed\n", fails + (e != GL_NO_ERROR));
   return fails || e != GL_NO_ERROR;
}
