/*
 * Copyright © 2026 servidorjarvis1-svg
 * SPDX-License-Identifier: MIT
 */

/*
 * sRGB render targets (the native integers, an ES 3.0 context: EXT_sRGB's
 * SRGB8_ALPHA8 is colour-renderable there), checked by reading back the
 * stored bytes of a 64x64 FBO (ReadPixels doesn't convert on ES):
 *  1. a clear: the clear colour encoded
 *  2. an opaque draw: the colour encoded (lower_srgb_output, rt_srgb 1)
 *  3. SRC_ALPHA, ONE_MINUS_SRC_ALPHA blended in linear (rt_srgb 2): 188, not
 *     the 181 of a blend of the encoded bytes
 *  4. ONE, ONE in linear: 0.25 + 0.25 = 0.5, 188 (encoded bytes: 255)
 *  5. CONSTANT_COLOR, ONE_MINUS_CONSTANT_COLOR (the blend colour)
 *  6. MIN and MAX
 *  7. REVERSE_SUBTRACT
 *  8. a colour mask with a blend: G kept
 *  9. a discard with a blend (pass type 2): the discarded half kept
 * 10. the sRGB texture sampled into a linear RGBA8 target: decoded; 10b
 *     LINEAR, filtered after the decode (lower_srgb_filter)
 * 11. a linear RGBA8 target: as before, not encoded
 * 12. MRT, an sRGB and a linear buffer from one program
 *
 *   MESA_GLES_VERSION_OVERRIDE=3.0 DISPLAY=:0 sgx_srgb
 *
 * zig cc -target x86-linux-gnu.2.36 -isystem <root>/usr/include -o sgx_srgb
 * sgx_srgb.c <root>/usr/lib/i386-linux-gnu/libEGL.so.1
 * <root>/usr/lib/i386-linux-gnu/libGLESv2.so.2 -lm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define W 64
#define H 64

static int fails;

static GLuint
prog(const char *fs)
{
   static const char *vs = "#version 300 es\n"
                           "in vec2 a;\n"
                           "void main() { gl_Position = vec4(a, 0.0, 1.0); }\n";
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

/* linear to the stored sRGB byte */
static int
enc(float x)
{
   float s = x <= 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1.0f / 2.4f) - 0.055f;
   return (int)lrintf(s * 255.0f);
}

static int
un(float x)
{
   return (int)lrintf(x * 255.0f);
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
   printf("%-44s (%2d,%2d) got %3u %3u %3u %3u want %3d %3d %3d %3d: %s\n",
          name, x, y, p[0], p[1], p[2], p[3], r, g, b, a, ok ? "OK" : "FAIL");
   fails += !ok;
}

static void
quad(void)
{
   static const float v[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};

   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
   glDrawArrays(GL_TRIANGLES, 0, 6);
}

static GLuint
target(GLenum ifmt, GLuint *tex)
{
   GLuint t, fb;

   glGenTextures(1, &t);
   glBindTexture(GL_TEXTURE_2D, t);
   glTexStorage2D(GL_TEXTURE_2D, 1, ifmt, W, H);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                          t, 0);
   if (tex)
      *tex = t;
   return fb;
}

static void
colour(GLuint p, float r, float g, float b, float a)
{
   glUseProgram(p);
   glUniform4f(glGetUniformLocation(p, "u"), r, g, b, a);
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
   const EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
   EGLConfig cfg;
   EGLint n = 0;
   eglChooseConfig(d, ca, &cfg, 1, &n);
   const EGLint xa3[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
   EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa3);
   if (c == EGL_NO_CONTEXT ||
       !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
      printf("no ES 3.0 context 0x%x (MESA_GLES_VERSION_OVERRIDE=3.0, "
             "SGX_DEBUG=noglsl130?)\n",
             eglGetError());
      return 1;
   }
   printf("%s\n", glGetString(GL_VERSION));

   GLuint stex, sfb = target(GL_SRGB8_ALPHA8, &stex);
   GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
   GLint enc_attr = 0;
   glGetFramebufferAttachmentParameteriv(
      GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
      GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &enc_attr);
   printf("SRGB8_ALPHA8 FBO: status 0x%x, encoding 0x%x: %s\n", st, enc_attr,
          st == GL_FRAMEBUFFER_COMPLETE && enc_attr == GL_SRGB ? "OK" : "FAIL");
   if (st != GL_FRAMEBUFFER_COMPLETE) {
      printf("result: no sRGB render target\n");
      return 1;
   }
   glViewport(0, 0, W, H);

   GLuint pu = prog("#version 300 es\nprecision highp float;\n"
                    "uniform vec4 u;\nout vec4 o;\n"
                    "void main() { o = u; }\n");
   const int t = 1;
   const int q = enc(0.25f), h = enc(0.5f);

   /* 1 */
   glClearColor(0.5f, 0.25f, 0.0f, 0.5f);
   glClear(GL_COLOR_BUFFER_BIT);
   check("1 clear (0.5, 0.25, 0, 0.5)", 8, 8, h, q, 0, un(0.5f), t);

   /* 2 */
   colour(pu, 0.2f, 0.5f, 0.8f, 1.0f);
   quad();
   check("2 opaque draw (0.2, 0.5, 0.8, 1)", 32, 32, enc(0.2f), h, enc(0.8f),
         255, t);

   /* 3 */
   glClearColor(0.25f, 0.25f, 0.25f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glEnable(GL_BLEND);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   colour(pu, 0.75f, 0.75f, 0.75f, 0.5f);
   quad();
   check("3 SRC_ALPHA, 1-SRC_ALPHA: 0.25 / 0.75 at 0.5", 32, 32, h, h, h,
         un(0.75f), t);

   /* 4 */
   glClear(GL_COLOR_BUFFER_BIT);
   glBlendFunc(GL_ONE, GL_ONE);
   colour(pu, 0.25f, 0.25f, 0.25f, 0.0f);
   quad();
   check("4 ONE, ONE: 0.25 + 0.25", 32, 32, h, h, h, 255, t);

   /* 5 */
   glClearColor(0, 0, 0, 0);
   glClear(GL_COLOR_BUFFER_BIT);
   glBlendColor(0.5f, 0.25f, 1.0f, 0.5f);
   glBlendFunc(GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR);
   colour(pu, 1.0f, 1.0f, 1.0f, 1.0f);
   quad();
   check("5 CONSTANT_COLOR (0.5, 0.25, 1, 0.5)", 32, 32, h, q, 255, un(0.5f),
         t);

   /* 6 */
   glClearColor(0.25f, 0.5f, 0.25f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glBlendFunc(GL_ONE, GL_ONE);
   glBlendEquation(GL_MAX);
   colour(pu, 0.5f, 0.25f, 0.0f, 1.0f);
   quad();
   check("6a MAX", 32, 32, h, h, q, 255, t);
   glClear(GL_COLOR_BUFFER_BIT);
   glBlendEquation(GL_MIN);
   quad();
   check("6b MIN", 32, 32, q, q, 0, 255, t);

   /* 7 */
   glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
   colour(pu, 0.25f, 0.25f, 0.25f, 0.0f);
   quad();
   check("7 REVERSE_SUBTRACT: 0.5 - 0.25", 32, 32, q, q, q, 255, t);
   glBlendEquation(GL_FUNC_ADD);

   /* 8 */
   glClearColor(0.25f, 0.25f, 0.25f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_TRUE);
   colour(pu, 0.25f, 0.25f, 0.25f, 0.0f);
   quad();
   glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
   check("8 ONE, ONE with G masked", 32, 32, h, q, h, 255, t);

   /* 9 */
   GLuint pd = prog("#version 300 es\nprecision highp float;\n"
                    "uniform vec4 u;\nout vec4 o;\n"
                    "void main() { if (gl_FragCoord.x < 32.0) discard; o = u; }\n");
   glClear(GL_COLOR_BUFFER_BIT);
   colour(pd, 0.25f, 0.25f, 0.25f, 0.0f);
   quad();
   check("9a discard + ONE, ONE: kept half", 8, 32, q, q, q, 255, t);
   check("9b discard + ONE, ONE: drawn half", 56, 32, h, h, h, 255, t);
   glDisable(GL_BLEND);

   /* 10: stex holds 0.5 encoded (188); sampled, it's 0.5 again */
   glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   GLuint lfb = target(GL_RGBA8, NULL);
   GLuint ps = prog("#version 300 es\nprecision highp float;\n"
                    "uniform sampler2D s;\nout vec4 o;\n"
                    "void main() { o = texture(s, gl_FragCoord.xy / 64.0); }\n");
   glUseProgram(ps);
   glUniform1i(glGetUniformLocation(ps, "s"), 0);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, stex);
   quad();
   check("10 sRGB texture sampled into RGBA8", 32, 32, un(0.5f), un(0.5f),
         un(0.5f), 255, t);

   /* 10b: LINEAR between sRGB 0 and 255 is linear 0.5 (128), decoded before
    * it's filtered (lower_srgb_filter); the TPU's filter of the encoded
    * bytes would give 55.  The coordinate from a varying */
   {
      static const char *vsv = "#version 300 es\n"
                               "in vec2 a;\nout vec2 uv;\n"
                               "void main() { uv = a * 0.5 + 0.5;"
                               " gl_Position = vec4(a, 0.0, 1.0); }\n";
      static const char *fsv = "#version 300 es\nprecision highp float;\n"
                               "uniform sampler2D s;\nin vec2 uv;\nout vec4 o;\n"
                               "void main() { o = texture(s, uv); }\n";
      GLuint pv = glCreateProgram();
      const char *src[2] = {vsv, fsv};
      GLenum ty[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
      for (int i = 0; i < 2; i++) {
         GLuint sh = glCreateShader(ty[i]);
         glShaderSource(sh, 1, &src[i], NULL);
         glCompileShader(sh);
         glAttachShader(pv, sh);
      }
      glBindAttribLocation(pv, 0, "a");
      glLinkProgram(pv);
      const unsigned char px2[8] = {0, 0, 0, 255, 255, 255, 255, 255};
      GLuint t2;
      glGenTextures(1, &t2);
      glBindTexture(GL_TEXTURE_2D, t2);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, 2, 1, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, px2);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glUseProgram(pv);
      glUniform1i(glGetUniformLocation(pv, "s"), 0);
      quad();
      check("10b sRGB texture LINEAR halfway, varying coord", 32, 32,
            un(0.5f), un(0.5f), un(0.5f), 255, 4);
      glBindTexture(GL_TEXTURE_2D, stex);
   }

   /* 11 */
   colour(pu, 0.2f, 0.5f, 0.8f, 1.0f);
   quad();
   check("11 RGBA8 target, not encoded", 32, 32, un(0.2f), un(0.5f), un(0.8f),
         255, t);
   glEnable(GL_BLEND);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   glClearColor(0.25f, 0.25f, 0.25f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   colour(pu, 0.75f, 0.75f, 0.75f, 0.5f);
   quad();
   check("11b RGBA8 target, blended as before", 32, 32, un(0.5f), un(0.5f),
         un(0.5f), -1, t);
   glDisable(GL_BLEND);

   /* 12 */
   GLuint mt[2], mfb;
   glGenTextures(2, mt);
   glGenFramebuffers(1, &mfb);
   glBindFramebuffer(GL_FRAMEBUFFER, mfb);
   for (int i = 0; i < 2; i++) {
      glBindTexture(GL_TEXTURE_2D, mt[i]);
      glTexStorage2D(GL_TEXTURE_2D, 1, i ? GL_RGBA8 : GL_SRGB8_ALPHA8, W, H);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i,
                             GL_TEXTURE_2D, mt[i], 0);
   }
   const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
   glDrawBuffers(2, bufs);
   GLuint pm = prog("#version 300 es\nprecision highp float;\n"
                    "uniform vec4 u;\n"
                    "layout(location = 0) out vec4 o0;\n"
                    "layout(location = 1) out vec4 o1;\n"
                    "void main() { o0 = u; o1 = u; }\n");
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
      colour(pm, 0.5f, 0.25f, 0.2f, 1.0f);
      quad();
      glReadBuffer(GL_COLOR_ATTACHMENT0);
      check("12a MRT: sRGB buffer", 32, 32, h, q, enc(0.2f), 255, t);
      glReadBuffer(GL_COLOR_ATTACHMENT1);
      check("12b MRT: RGBA8 buffer", 32, 32, un(0.5f), un(0.25f), un(0.2f),
            255, t);
   } else {
      printf("12 MRT FBO incomplete: FAIL\n");
      fails++;
   }

   GLenum e = glGetError();
   printf("GL error 0x%x\n", e);
   printf("result: %d failed\n", fails + (e != GL_NO_ERROR));
   (void)sfb;
   (void)lfb;
   return fails || e != GL_NO_ERROR;
}
