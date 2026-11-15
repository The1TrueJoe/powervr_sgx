/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * What games reported on 0.2.0 (issue #5) hit, on desktop GL in a 64x64
 * pbuffer, each part checked pixel by pixel:
 *
 *   box16    a 16-tap box filter of one texture (ioquake3's downsample):
 *            16 dependent samples sharing one texture state block
 *   taps24   24 dependent samples of two textures (two blocks)
 *   tex3d    a 1x1x1 3D texture (wined3d's start-up textures): created with
 *            no GL error and sampled, plainly and at a computed coordinate
 *   stream   1024 draws a frame, each from a buffer given new data first
 *            (glBufferData, GL_STREAM_DRAW: arrays and 16-bit indices), the
 *            polygon offset changed every 8 draws so the frame is submitted
 *            mid-draw; every cell's colour checked
 *   blit     an FBO with an RGBA8 and a 24-bit depth texture blitted to the
 *            default framebuffer, straight and flipped (ioquake3's
 *            renderer_opengl2 and wined3d present their frames that way)
 *
 *   sgx_gamefix [part...]
 *
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -o sgx_gamefix
 * sgx_gamefix.c -ldl
 */
#include <dlfcn.h>
#include <stdbool.h>
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

#define FUNCS(X)                                                               \
   X(glActiveTexture)                                                          \
   X(glAttachShader)                                                           \
   X(glBindAttribLocation)                                                     \
   X(glBindBuffer)                                                             \
   X(glBindFramebuffer)                                                        \
   X(glBindTexture)                                                            \
   X(glBlitFramebuffer)                                                        \
   X(glBufferData)                                                             \
   X(glCheckFramebufferStatus)                                                 \
   X(glClear)                                                                  \
   X(glClearColor)                                                             \
   X(glClearDepth)                                                             \
   X(glCompileShader)                                                          \
   X(glCreateProgram)                                                          \
   X(glCreateShader)                                                           \
   X(glDeleteBuffers)                                                          \
   X(glDepthFunc)                                                              \
   X(glDisable)                                                                \
   X(glDrawArrays)                                                             \
   X(glDrawElements)                                                           \
   X(glEnable)                                                                 \
   X(glEnableVertexAttribArray)                                                \
   X(glFinish)                                                                 \
   X(glFramebufferTexture2D)                                                   \
   X(glGenBuffers)                                                             \
   X(glGenFramebuffers)                                                        \
   X(glGenTextures)                                                            \
   X(glGetError)                                                               \
   X(glGetIntegerv)                                                            \
   X(glGetProgramInfoLog)                                                      \
   X(glGetProgramiv)                                                           \
   X(glGetShaderInfoLog)                                                       \
   X(glGetString)                                                              \
   X(glGetUniformLocation)                                                     \
   X(glLinkProgram)                                                            \
   X(glPolygonOffset)                                                          \
   X(glReadPixels)                                                             \
   X(glShaderSource)                                                           \
   X(glTexImage2D)                                                             \
   X(glTexImage3D)                                                             \
   X(glTexParameteri)                                                          \
   X(glUniform1i)                                                              \
   X(glUniform2f)                                                              \
   X(glUseProgram)                                                             \
   X(glVertexAttribPointer)                                                    \
   X(glViewport)

#define DECL(n) static __typeof__(&n) n##_;
FUNCS(DECL)
#undef DECL

#define S 64

static int failures;

static void
result(const char *part, unsigned bad, unsigned total)
{
   printf("%s: %s (%u of %u wrong)\n", part, bad ? "FAIL" : "PASS", bad, total);
   failures += bad != 0;
}

static GLuint
program(const char *vs, const char *fs)
{
   const char *src[2] = {vs, fs};
   GLenum type[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
   GLuint p = glCreateProgram_();
   char log[2048];

   for (int i = 0; i < 2; i++) {
      GLuint sh = glCreateShader_(type[i]);
      glShaderSource_(sh, 1, &src[i], NULL);
      glCompileShader_(sh);
      glGetShaderInfoLog_(sh, sizeof(log), NULL, log);
      if (log[0])
         printf("shader log: %s\n", log);
      glAttachShader_(p, sh);
   }
   glBindAttribLocation_(p, 0, "p");
   glBindAttribLocation_(p, 1, "c");
   glLinkProgram_(p);
   GLint ok = 0;
   glGetProgramiv_(p, GL_LINK_STATUS, &ok);
   if (!ok) {
      glGetProgramInfoLog_(p, sizeof(log), NULL, log);
      printf("link failed: %s\n", log);
   }
   glUseProgram_(p);
   return p;
}

/* a 64x64 RGBA8 colour texture (and a 24-bit depth one) as the FBO's */
static GLuint
fbo(GLuint *colour, bool depth)
{
   GLuint fb, t[2];

   glGenFramebuffers_(1, &fb);
   glBindFramebuffer_(GL_FRAMEBUFFER, fb);
   glGenTextures_(2, t);
   glBindTexture_(GL_TEXTURE_2D, t[0]);
   glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 NULL);
   glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           t[0], 0);
   if (depth) {
      glBindTexture_(GL_TEXTURE_2D, t[1]);
      glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexImage2D_(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, S, S, 0,
                    GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
      glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_TEXTURE_2D, t[1], 0);
   }
   if (glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      printf("framebuffer incomplete\n");
   if (colour)
      *colour = t[0];
   glViewport_(0, 0, S, S);
   return fb;
}

/* a whole-target quad from client arrays (attribute 0) */
static void
fullscreen(void)
{
   static const GLfloat v[] = {-1, -1, 1, -1, -1, 1, 1, 1};

   glBindBuffer_(GL_ARRAY_BUFFER, 0);
   glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, 0, v);
   glEnableVertexAttribArray_(0);
   glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
}

static const char *vs_uv = "#version 110\n"
                           "attribute vec2 p;\n"
                           "varying vec2 uv;\n"
                           "void main() {\n"
                           "   uv = p * 0.5 + 0.5;\n"
                           "   gl_Position = vec4(p, 0.0, 1.0);\n"
                           "}\n";

/* texel (x, y) of pattern k, 8x8 */
static void
texel(unsigned k, unsigned x, unsigned y, unsigned char c[4])
{
   c[0] = k ? 255 - x * 32 : x * 32;
   c[1] = k ? y * 24 : y * 32;
   c[2] = k ? 200 - (x + y) * 8 : (x + y) * 16;
   c[3] = 255;
}

static GLuint
pattern(unsigned k)
{
   static unsigned char px[8 * 8 * 4];
   GLuint t;

   for (unsigned y = 0; y < 8; y++)
      for (unsigned x = 0; x < 8; x++)
         texel(k, x, y, &px[(y * 8 + x) * 4]);
   glGenTextures_(1, &t);
   glBindTexture_(GL_TEXTURE_2D, t);
   glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 px);
   return t;
}

static int
clampi(int v, int lo, int hi)
{
   return v < lo ? lo : v > hi ? hi : v;
}

static unsigned
check(const char *part, void (*want)(unsigned x, unsigned y, float c[4]),
      unsigned tol)
{
   static unsigned char px[S * S * 4];
   unsigned bad = 0;

   glReadPixels_(0, 0, S, S, GL_RGBA, GL_UNSIGNED_BYTE, px);
   for (unsigned y = 0; y < S; y++)
      for (unsigned x = 0; x < S; x++) {
         float w[4];
         want(x, y, w);
         for (unsigned k = 0; k < 4; k++) {
            int d = (int)px[(y * S + x) * 4 + k] - (int)(w[k] + 0.5f);
            if (d > (int)tol || d < -(int)tol) {
               if (bad < 4)
                  printf("%s: (%u,%u) channel %u = %u, want %.1f\n", part, x,
                         y, k, px[(y * S + x) * 4 + k], w[k]);
               bad++;
               break;
            }
         }
      }
   return bad;
}

/* the 8x8 texel a 64x64 pixel's centre falls in, moved by (dx, dy) and
 * clamped to the edge */
static void
tap(unsigned k, unsigned x, unsigned y, int dx, int dy, float acc[4])
{
   unsigned char c[4];

   texel(k, clampi((int)(x / 8) + dx, 0, 7), clampi((int)(y / 8) + dy, 0, 7), c);
   for (unsigned i = 0; i < 4; i++)
      acc[i] += c[i];
}

/* box16: taps at texel offsets -1..2 in x and y (texel centres) */
static void
want_box16(unsigned x, unsigned y, float c[4])
{
   memset(c, 0, 4 * sizeof(float));
   for (int j = 0; j < 4; j++)
      for (int i = 0; i < 4; i++)
         tap(0, x, y, i - 1, j - 1, c);
   for (unsigned i = 0; i < 4; i++)
      c[i] /= 16;
}

static void
box16(void)
{
   char fs[4096];
   size_t n = 0;

   fbo(NULL, false);
   n += snprintf(fs + n, sizeof(fs) - n,
                 "#version 110\n"
                 "uniform sampler2D t;\n"
                 "uniform vec2 inv;\n"
                 "varying vec2 uv;\n"
                 "void main() {\n"
                 "   vec2 c = (floor(uv * 8.0) + 0.5) * inv;\n"
                 "   vec4 s = vec4(0.0);\n");
   for (int j = 0; j < 4; j++)
      for (int i = 0; i < 4; i++)
         n += snprintf(fs + n, sizeof(fs) - n,
                       "   s += texture2D(t, c + inv * vec2(%d.0, %d.0));\n",
                       i - 1, j - 1);
   snprintf(fs + n, sizeof(fs) - n, "   gl_FragColor = s * 0.0625;\n}\n");
   GLuint p = program(vs_uv, fs);
   glActiveTexture_(GL_TEXTURE0);
   pattern(0);
   glUniform1i_(glGetUniformLocation_(p, "t"), 0);
   glUniform2f_(glGetUniformLocation_(p, "inv"), 1.0f / 8, 1.0f / 8);
   fullscreen();
   result("box16", check("box16", want_box16, 2), S * S);
}

/* taps24: 12 taps of each texture, offsets (k % 4 - 1, k / 4 - 1) */
static void
want_taps24(unsigned x, unsigned y, float c[4])
{
   memset(c, 0, 4 * sizeof(float));
   for (unsigned t = 0; t < 2; t++)
      for (int k = 0; k < 12; k++)
         tap(t, x, y, k % 4 - 1, k / 4 - 1, c);
   for (unsigned i = 0; i < 4; i++)
      c[i] /= 24;
}

static void
taps24(void)
{
   char fs[8192];
   size_t n = 0;

   fbo(NULL, false);
   n += snprintf(fs + n, sizeof(fs) - n,
                 "#version 110\n"
                 "uniform sampler2D t0, t1;\n"
                 "uniform vec2 inv;\n"
                 "varying vec2 uv;\n"
                 "void main() {\n"
                 "   vec2 c = (floor(uv * 8.0) + 0.5) * inv;\n"
                 "   vec4 s = vec4(0.0);\n");
   for (int k = 0; k < 12; k++)
      for (int t = 0; t < 2; t++)
         n += snprintf(fs + n, sizeof(fs) - n,
                       "   s += texture2D(t%d, c + inv * vec2(%d.0, %d.0));\n",
                       t, k % 4 - 1, k / 4 - 1);
   snprintf(fs + n, sizeof(fs) - n, "   gl_FragColor = s / 24.0;\n}\n");
   GLuint p = program(vs_uv, fs);
   glActiveTexture_(GL_TEXTURE0);
   pattern(0);
   glActiveTexture_(GL_TEXTURE1);
   pattern(1);
   glActiveTexture_(GL_TEXTURE0);
   glUniform1i_(glGetUniformLocation_(p, "t0"), 0);
   glUniform1i_(glGetUniformLocation_(p, "t1"), 1);
   glUniform2f_(glGetUniformLocation_(p, "inv"), 1.0f / 8, 1.0f / 8);
   fullscreen();
   result("taps24", check("taps24", want_taps24, 3), S * S);
}

static void
want_tex3d(unsigned x, unsigned y, float c[4])
{
   c[0] = 200, c[1] = 100, c[2] = 50, c[3] = 255;
}

static void
tex3d(void)
{
   static const unsigned char v[4] = {200, 100, 50, 255};
   const char *fs[2] = {"#version 110\n"
                        "uniform sampler3D t;\n"
                        "varying vec2 uv;\n"
                        "void main() { gl_FragColor = texture3D(t, vec3(uv, 0.7)); }\n",
                        "#version 110\n"
                        "uniform sampler3D t;\n"
                        "varying vec2 uv;\n"
                        "void main() {\n"
                        "   gl_FragColor = texture3D(t, vec3(uv * 3.0 + 0.2, uv.x * uv.y));\n"
                        "}\n"};
   GLint max3d = 0;
   GLuint t;

   glGetIntegerv_(GL_MAX_3D_TEXTURE_SIZE, &max3d);
   printf("tex3d: GL_MAX_3D_TEXTURE_SIZE %d\n", max3d);
   while (glGetError_() != GL_NO_ERROR)
      ;
   glActiveTexture_(GL_TEXTURE0);
   glGenTextures_(1, &t);
   glBindTexture_(GL_TEXTURE_3D, t);
   glTexParameteri_(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri_(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexImage3D_(GL_TEXTURE_3D, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, v);
   GLenum e = glGetError_();
   result("tex3d glTexImage3D 1x1x1", e != GL_NO_ERROR, 1);
   if (e != GL_NO_ERROR) {
      printf("tex3d: glTexImage3D error %#x\n", e);
      return;
   }
   for (unsigned i = 0; i < 2; i++) {
      fbo(NULL, false);
      glClearColor_(0, 0, 0, 0);
      glClear_(GL_COLOR_BUFFER_BIT);
      GLuint p = program(vs_uv, fs[i]);
      glUniform1i_(glGetUniformLocation_(p, "t"), 0);
      fullscreen();
      result(i ? "tex3d dependent" : "tex3d", check("tex3d", want_tex3d, 1),
             S * S);
   }
}

/* stream: cell i of a 32x32 grid (2x2 pixels), frame f's colour */
#define CELLS 32
static unsigned stream_frame;

static void
cell_colour(unsigned i, unsigned f, unsigned char c[4])
{
   c[0] = (i * 37 + f * 11) & 0xff;
   c[1] = (i * 13 + f * 29) & 0xff;
   c[2] = (i * 7 + f * 53) & 0xff;
   c[3] = 255;
}

static void
want_stream(unsigned x, unsigned y, float c[4])
{
   unsigned char u[4];

   cell_colour((y / 2) * CELLS + x / 2, stream_frame, u);
   for (unsigned k = 0; k < 4; k++)
      c[k] = u[k];
}

static void
stream(void)
{
   const char *vs = "#version 110\n"
                    "attribute vec2 p;\n"
                    "attribute vec4 c;\n"
                    "varying vec4 col;\n"
                    "void main() { col = c; gl_Position = vec4(p, 0.0, 1.0); }\n";
   const char *fs = "#version 110\n"
                    "varying vec4 col;\n"
                    "void main() { gl_FragColor = col; }\n";
   const unsigned frames = 12;
   GLuint vb, ib;
   unsigned bad = 0;

   fbo(NULL, true);
   program(vs, fs);
   glGenBuffers_(1, &vb);
   glGenBuffers_(1, &ib);
   glEnable_(GL_DEPTH_TEST);
   glDepthFunc_(GL_ALWAYS);
   glEnable_(GL_POLYGON_OFFSET_FILL);
   for (unsigned f = 0; f < frames; f++) {
      glClearColor_(0, 0, 0, 0);
      glClearDepth_(1.0);
      glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      for (unsigned i = 0; i < CELLS * CELLS; i++) {
         /* a quad (6 vertices: x, y, rgba8), then padding that changes the
          * buffer's size draw to draw */
         struct {
            GLfloat x, y;
            GLubyte c[4];
         } v[6 + 64];
         unsigned cx = i % CELLS, cy = i / CELLS, pad = (i * 7 + f) % 64;
         float x0 = cx * 2.0f / CELLS - 1, y0 = cy * 2.0f / CELLS - 1,
               x1 = x0 + 2.0f / CELLS, y1 = y0 + 2.0f / CELLS;
         const float q[6][2] = {{x0, y0}, {x1, y0}, {x0, y1},
                                {x1, y0}, {x1, y1}, {x0, y1}};
         unsigned char c[4];

         cell_colour(i, f, c);
         memset(v, 0, sizeof(v));
         for (unsigned k = 0; k < 6; k++) {
            v[k].x = q[k][0];
            v[k].y = q[k][1];
            memcpy(v[k].c, c, 4);
         }
         if (i % 8 == 0)
            glPolygonOffset_(0, (float)(i / 8 % 2 + 1));
         glBindBuffer_(GL_ARRAY_BUFFER, vb);
         glBufferData_(GL_ARRAY_BUFFER, (6 + pad) * sizeof(v[0]), v,
                       GL_STREAM_DRAW);
         glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, sizeof(v[0]), 0);
         glVertexAttribPointer_(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(v[0]),
                                (void *)8);
         glEnableVertexAttribArray_(0);
         glEnableVertexAttribArray_(1);
         if (i % 2) {
            const GLushort idx[6] = {0, 1, 2, 3, 4, 5};
            glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, ib);
            glBufferData_(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx) + pad * 2, NULL,
                          GL_STREAM_DRAW);
            glBufferData_(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx,
                          GL_STREAM_DRAW);
            glDrawElements_(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
         } else {
            glDrawArrays_(GL_TRIANGLES, 0, 6);
         }
      }
      if (f % 4 == 3 || f == frames - 1) {
         stream_frame = f;
         bad += check("stream", want_stream, 0);
      }
   }
   glDisable_(GL_POLYGON_OFFSET_FILL);
   glDisable_(GL_DEPTH_TEST);
   glBindBuffer_(GL_ARRAY_BUFFER, 0);
   glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, 0);
   result("stream", bad, S * S * 4);
}

/* blit: the FBO holds a 4-colour quadrant pattern (drawn with depth) */
static unsigned blit_flip;

static void
quadrant(unsigned x, unsigned y, float c[4])
{
   unsigned q = (x >= S / 2) + 2 * (y >= S / 2);
   const float v[4][4] = {
      {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}};

   memcpy(c, v[q], sizeof(v[q]));
}

static void
want_blit(unsigned x, unsigned y, float c[4])
{
   quadrant(x, blit_flip ? S - 1 - y : y, c);
}

static void
blit(void)
{
   const char *fs = "#version 110\n"
                    "varying vec2 uv;\n"
                    "void main() {\n"
                    "   vec2 h = step(0.5, uv);\n"
                    "   gl_FragColor = mix(mix(vec4(1.0, 0.0, 0.0, 1.0), "
                    "vec4(0.0, 1.0, 0.0, 1.0), h.x), mix(vec4(0.0, 0.0, 1.0, 1.0), "
                    "vec4(1.0, 1.0, 0.0, 1.0), h.x), h.y);\n"
                    "}\n";
   GLuint fb = fbo(NULL, true);

   program(vs_uv, fs);
   glClearColor_(0.5f, 0.5f, 0.5f, 1);
   glClearDepth_(1.0);
   glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   glEnable_(GL_DEPTH_TEST);
   glDepthFunc_(GL_LESS);
   fullscreen();
   glDisable_(GL_DEPTH_TEST);
   result("blit fbo", check("blit fbo", quadrant, 1), S * S);
   for (blit_flip = 0; blit_flip < 2; blit_flip++) {
      glBindFramebuffer_(GL_READ_FRAMEBUFFER, fb);
      glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, 0);
      glClearColor_(0, 0, 0, 0);
      glClear_(GL_COLOR_BUFFER_BIT);
      glBlitFramebuffer_(0, 0, S, S, 0, blit_flip ? S : 0, S, blit_flip ? 0 : S,
                         GL_COLOR_BUFFER_BIT, GL_NEAREST);
      glBindFramebuffer_(GL_FRAMEBUFFER, 0);
      result(blit_flip ? "blit to default, flipped" : "blit to default",
             check("blit", want_blit, 1), S * S);
   }
}

int
main(int argc, char **argv)
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
                  EGL_ALPHA_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                  EGL_NONE};
   EGLConfig cfg;
   EGLint n = 0;
   if (!chooseconfig(d, ca, &cfg, 1, &n) || !n)
      return 2;
   EGLint sa[] = {EGL_WIDTH, S, EGL_HEIGHT, S, EGL_NONE};
   EGLSurface s = createpbuffer(d, cfg, sa);
   EGLContext c = createcontext(d, cfg, EGL_NO_CONTEXT, NULL);
   if (!s || !c || !makecurrent(d, s, s, c))
      return 2;
#define GET(n) n##_ = L(#n);
   FUNCS(GET)
#undef GET
   printf("renderer: %s, %s\n", glGetString_(GL_RENDERER),
          glGetString_(GL_VERSION));

   static const struct {
      const char *name;
      void (*run)(void);
   } parts[] = {{"box16", box16},
                {"taps24", taps24},
                {"tex3d", tex3d},
                {"stream", stream},
                {"blit", blit}};
   for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
      bool on = argc < 2;
      for (int a = 1; a < argc; a++)
         on |= !strcmp(argv[a], parts[i].name);
      if (!on)
         continue;
      parts[i].run();
      glBindFramebuffer_(GL_FRAMEBUFFER, 0);
      glFinish_();
   }
   printf("%s\n", failures ? "FAIL" : "PASS");
   return failures ? 1 : 0;
}
