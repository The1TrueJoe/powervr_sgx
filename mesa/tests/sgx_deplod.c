/*
 * Copyright © 2026 servidorjarvis1-svg
 * SPDX-License-Identifier: MIT
 */

/*
 * A mipmapped sample whose coordinate the program computes (a dependent
 * one) against the same sample from the varying itself (the PDS's), on a
 * 64x64 pbuffer quad of two triangles: every pixel compared, and those
 * differing counted where the two triangles meet (the quads they share).
 *
 *   A texture2D(s, uv)         the PDS samples it
 *   B texture2D(s, uv * one)   one a uniform 1.0: the program samples it
 *
 * Without lower_dep_lod, B takes another LOD in the quads on the diagonal
 * (the TPU's LOD for a dependent sample in a quad its triangle covers in
 * part); with it, B's LOD comes from fdsx / fdsy through smp's gradient
 * mode. The coordinate spans (-1, -2.7)..(2.03, 4.76) over the quad (LOD
 * about 1.5 to 2.8, dEQP's texture.filtering case 0), on a 64x64 texture
 * whose every level has its own gradient. Two filters:
 *   1. NEAREST_MIPMAP_NEAREST   exact
 *   2. LINEAR_MIPMAP_LINEAR     within 2 a channel
 *
 *   sgx_deplod
 *
 * zig cc -target x86-linux-gnu.2.36 -isystem <root>/usr/include -o sgx_deplod
 * sgx_deplod.c <root>/usr/lib/i386-linux-gnu/libEGL.so.1
 * <root>/usr/lib/i386-linux-gnu/libGLESv2.so.2
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

static GLuint prog(const char *fs)
{
   const char *vs = "attribute vec2 a; attribute vec2 t; varying vec2 uv;\n"
                    "void main(){ uv = t; gl_Position = vec4(a, 0.0, 1.0); }";
   const char *src[2] = {vs, fs};
   GLenum ty[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
   GLuint p = glCreateProgram();

   for (int i = 0; i < 2; i++) {
      GLuint s = glCreateShader(ty[i]);
      GLint ok;

      glShaderSource(s, 1, &src[i], NULL);
      glCompileShader(s);
      glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
      if (!ok) {
         char l[1024];
         glGetShaderInfoLog(s, sizeof l, NULL, l);
         fprintf(stderr, "shader: %s\n", l);
         exit(2);
      }
      glAttachShader(p, s);
   }
   glBindAttribLocation(p, 0, "a");
   glBindAttribLocation(p, 1, "t");
   glLinkProgram(p);
   return p;
}

static void texel(int lv, int x, int y, unsigned char *c)
{
   c[0] = (unsigned char)(x * 7 + lv * 40);
   c[1] = (unsigned char)(y * 5 + lv * 60);
   c[2] = (unsigned char)((x + y) * 3 + lv * 90);
   c[3] = 255;
}

static void draw(GLuint p, unsigned char *px)
{
   static const float a[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
   const float s0 = -1.0f, t0 = -2.7f, s1 = 2.03143f, t1 = 4.76426f;
   const float t[] = {s0, t0, s1, t0, s0, t1, s0, t1, s1, t0, s1, t1};

   glUseProgram(p);
   glUniform1i(glGetUniformLocation(p, "s"), 0);
   glUniform1f(glGetUniformLocation(p, "one"), 1.0f);
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, a);
   glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, t);
   glClearColor(0, 0, 0, 0);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLES, 0, 6);
   glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

static int compare(const char *name, const unsigned char *a, const unsigned char *b, int tol)
{
   int diff = 0, diag = 0, first = -1;

   for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
         const unsigned char *p = a + (y * W + x) * 4, *q = b + (y * W + x) * 4;
         int bad = 0;

         for (int c = 0; c < 4; c++)
            bad |= abs(p[c] - q[c]) > tol;
         if (!bad)
            continue;
         int qx = x & ~1, qy = y & ~1;
         diff++;
         /* a quad the diagonal x + y = 63 runs through */
         diag += qx + qy <= 64 && qx + qy + 2 >= 62;
         if (first < 0)
            first = y * W + x;
      }
   printf("%-26s %s, %d of %d pixels differ (%d in a quad on the diagonal)", name,
          diff ? "FAIL" : "OK", diff, W * H, diag);
   if (first >= 0)
      printf(" (first at %d,%d: %02x %02x %02x, want %02x %02x %02x)", first % W, first / W,
             b[first * 4], b[first * 4 + 1], b[first * 4 + 2], a[first * 4], a[first * 4 + 1],
             a[first * 4 + 2]);
   printf("\n");
   return diff != 0;
}

int main(void)
{
   EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);

   if (!eglInitialize(d, NULL, NULL)) {
      fprintf(stderr, "eglInitialize failed\n");
      return 2;
   }
   eglBindAPI(EGL_OPENGL_ES_API);
   EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                  EGL_RED_SIZE, 8, EGL_NONE};
   EGLConfig c;
   EGLint n = 0;
   eglChooseConfig(d, ca, &c, 1, &n);
   EGLint pa[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
   EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
   EGLSurface s = n ? eglCreatePbufferSurface(d, c, pa) : EGL_NO_SURFACE;
   EGLContext x = n ? eglCreateContext(d, c, EGL_NO_CONTEXT, xa) : EGL_NO_CONTEXT;
   if (!eglMakeCurrent(d, s, s, x)) {
      fprintf(stderr, "no context (0x%x)\n", eglGetError());
      return 2;
   }
   printf("%s, %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

   GLuint t;
   static unsigned char img[64 * 64 * 4];
   glGenTextures(1, &t);
   glBindTexture(GL_TEXTURE_2D, t);
   for (int lv = 0; lv < 7; lv++) {
      int sz = 64 >> lv;

      for (int y = 0; y < sz; y++)
         for (int xx = 0; xx < sz; xx++)
            texel(lv, xx, y, img + (y * sz + xx) * 4);
      glTexImage2D(GL_TEXTURE_2D, lv, GL_RGBA, sz, sz, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
   }
   glViewport(0, 0, W, H);

   const char *hdr = "precision highp float; uniform sampler2D s; uniform float one;\n"
                     "varying vec2 uv;\n";
   char fa[512], fb[512];
   snprintf(fa, sizeof fa, "%svoid main(){ gl_FragColor = texture2D(s, uv); }", hdr);
   snprintf(fb, sizeof fb, "%svoid main(){ gl_FragColor = texture2D(s, uv * one); }", hdr);
   GLuint pA = prog(fa), pB = prog(fb);
   static unsigned char a[W * H * 4], b[W * H * 4];
   static const struct {
      const char *name;
      GLenum min, mag;
      int tol;
   } cases[] = {
      {"1 nearest_mipmap_nearest", GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST, 0},
      {"2 linear_mipmap_linear", GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, 2},
   };
   int fails = 0;

   for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, cases[k].min);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, cases[k].mag);
      draw(pA, a);
      draw(pB, b);
      fails += compare(cases[k].name, a, b, cases[k].tol);
   }
   printf("glGetError 0x%x\nresult: %d failed\n", glGetError(), fails);
   return fails ? 1 : 0;
}
