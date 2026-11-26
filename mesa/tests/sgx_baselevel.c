/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * GL_TEXTURE_BASE_LEVEL and GL_TEXTURE_MAX_LEVEL at every level of a mip
 * chain uploaded from the CPU (no render into it), each level one colour of
 * its own, in formats of 1, 2 and 4 bytes a texel and in square and
 * non-square sizes:
 *
 *   one   BASE = MAX = l, NEAREST: a quad over the pbuffer reads level l
 *   down  BASE = l, MAX = the last, NEAREST_MIPMAP_NEAREST, the whole level
 *         onto one pixel: the last level
 *   tail  a draw at BASE = MAX = l - 1 with NEAREST_MIPMAP_NEAREST (its
 *         levels past l - 1 the driver's tail), then one as "one" at l
 *
 * A level's chain offset that the sampler's base address can't hold reads
 * another level's texels (docs/TEX.md "Mipmaps").
 *
 *   sgx_baselevel
 *
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -o sgx_baselevel
 * sgx_baselevel.c -ldl
 */
#include <dlfcn.h>
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

G(glClearColor);
G(glClear);
G(glViewport);
G(glReadPixels);
G(glPixelStorei);
G(glGenTextures);
G(glBindTexture);
G(glTexImage2D);
G(glTexParameteri);
G(glDeleteTextures);
G(glCreateShader);
G(glShaderSource);
G(glCompileShader);
G(glCreateProgram);
G(glAttachShader);
G(glBindAttribLocation);
G(glLinkProgram);
G(glUseProgram);
G(glVertexAttribPointer);
G(glEnableVertexAttribArray);
G(glDrawArrays);

/* level l's colour, every channel 0, 128 or 255 (within 8 in 565 and
 * 4444) */
static const uint8_t colours[12][4] = {
   {255, 0, 0, 255},     {0, 255, 0, 255},   {0, 0, 255, 255},
   {255, 255, 0, 255},   {0, 255, 255, 255}, {255, 0, 255, 255},
   {255, 255, 255, 255}, {0, 0, 0, 255},     {128, 0, 0, 255},
   {0, 128, 0, 255},     {0, 0, 128, 255},   {128, 128, 0, 255},
};

struct fmt {
   const char *name;
   GLenum internal, format, type;
   unsigned bpp;
};

static const struct fmt fmts[] = {
   {"rgba8", GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4},
   {"rgb565", GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2},
   {"rgba4", GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, 2},
   {"l8", GL_LUMINANCE8, GL_LUMINANCE, GL_UNSIGNED_BYTE, 1},
};

static int failures;

/* level l's texels in f, and the colour they read as */
static void *
level_data(const struct fmt *f, unsigned l, unsigned n, uint8_t *want)
{
   const uint8_t *c = colours[l % 12];
   uint8_t *p = malloc((size_t)n * 4);

   memcpy(want, c, 4);
   for (unsigned i = 0; i < n; i++) {
      switch (f->bpp) {
      case 4:
         memcpy(p + 4 * i, c, 4);
         break;
      case 2:
         ((uint16_t *)p)[i] =
            f->type == GL_UNSIGNED_SHORT_5_6_5
               ? (c[0] >> 3) << 11 | (c[1] >> 2) << 5 | c[2] >> 3
               : (c[0] >> 4) << 12 | (c[1] >> 4) << 8 | (c[2] >> 4) << 4 |
                    c[3] >> 4;
         break;
      default:
         p[i] = (uint8_t)(l * 20);
         want[0] = want[1] = want[2] = (uint8_t)(l * 20);
         break;
      }
   }
   return p;
}

/* the level the pixel's colour is, or -1 */
static int
level_of(const uint8_t (*want)[4], unsigned levels, const uint8_t *px)
{
   for (unsigned l = 0; l < levels; l++) {
      int d = 0;
      for (unsigned c = 0; c < 3; c++)
         d = d > abs(want[l][c] - px[c]) ? d : abs(want[l][c] - px[c]);
      if (d <= 8)
         return (int)l;
   }
   return -1;
}

static void
quad(void)
{
   static const GLfloat v[] = {-1, -1, 1, -1, -1, 1, 1, 1};

   glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, 0, v);
   glEnableVertexAttribArray_(0);
   glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
}

static void
run(const struct fmt *f, unsigned w, unsigned h)
{
   unsigned levels = 1, lw = w, lh = h;
   uint8_t want[16][4];
   GLuint t;

   while (lw > 1 || lh > 1) {
      lw = lw > 1 ? lw / 2 : 1;
      lh = lh > 1 ? lh / 2 : 1;
      levels++;
   }
   glGenTextures_(1, &t);
   glBindTexture_(GL_TEXTURE_2D, t);
   glPixelStorei_(GL_UNPACK_ALIGNMENT, 1);
   lw = w, lh = h;
   for (unsigned l = 0; l < levels; l++) {
      void *p = level_data(f, l, lw * lh, want[l]);

      glTexImage2D_(GL_TEXTURE_2D, (GLint)l, (GLint)f->internal, (GLsizei)lw,
                    (GLsizei)lh, 0, f->format, f->type, p);
      free(p);
      lw = lw > 1 ? lw / 2 : 1;
      lh = lh > 1 ? lh / 2 : 1;
   }
   glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   static const char *names[] = {"one", "down", "tail"};
   for (unsigned mode = 0; mode < 3; mode++) {
      bool down = mode == 1;

      printf("%-6s %4ux%-4u %-4s", f->name, w, h, names[mode]);
      for (unsigned l = 0; l < levels; l++) {
         uint8_t px[4];
         unsigned expect = down ? levels - 1 : l;

         if (mode == 2 && l) {
            glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL,
                             (GLint)l - 1);
            glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                             (GLint)l - 1);
            glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                             GL_NEAREST_MIPMAP_NEAREST);
            glViewport_(0, 0, 4, 4);
            quad();
         }
         glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, (GLint)l);
         glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                          (GLint)(down ? levels - 1 : l));
         glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                          down ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
         /* one pixel for the whole level: its LOD is past the chain */
         glViewport_(0, 0, down ? 1 : 4, down ? 1 : 4);
         glClearColor_(0.5f, 0.5f, 0.5f, 1);
         glClear_(GL_COLOR_BUFFER_BIT);
         quad();
         glReadPixels_(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
         int got = level_of((const uint8_t(*)[4])want, levels, px);
         if (got == (int)expect) {
            printf(" %u", l);
         } else {
            printf(" %u:FAIL(%d)", l, got);
            failures++;
         }
      }
      printf("\n");
   }
   glDeleteTextures_(1, &t);
}

int
main(void)
{
   void *egl = dlopen("libEGL.so.1", RTLD_NOW);
   if (!egl) {
      fprintf(stderr, "%s\n", dlerror());
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
   bindapi(EGL_OPENGL_API);
   EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8,
                  EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_RENDERABLE_TYPE,
                  EGL_OPENGL_BIT, EGL_NONE};
   EGLConfig cfg;
   EGLint n = 0;
   if (!chooseconfig(d, ca, &cfg, 1, &n) || !n) {
      fprintf(stderr, "no config\n");
      return 2;
   }
   EGLint sa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
   EGLSurface s = createpbuffer(d, cfg, sa);
   EGLContext c = createcontext(d, cfg, EGL_NO_CONTEXT, NULL);
   if (!s || !c || !makecurrent(d, s, s, c)) {
      fprintf(stderr, "context\n");
      return 2;
   }
   GET(glClearColor);
   GET(glClear);
   GET(glViewport);
   GET(glReadPixels);
   GET(glPixelStorei);
   GET(glGenTextures);
   GET(glBindTexture);
   GET(glTexImage2D);
   GET(glTexParameteri);
   GET(glDeleteTextures);
   GET(glCreateShader);
   GET(glShaderSource);
   GET(glCompileShader);
   GET(glCreateProgram);
   GET(glAttachShader);
   GET(glBindAttribLocation);
   GET(glLinkProgram);
   GET(glUseProgram);
   GET(glVertexAttribPointer);
   GET(glEnableVertexAttribArray);
   GET(glDrawArrays);

   static const char *vs = "attribute vec2 p; varying vec2 uv;\n"
                           "void main() { uv = p * 0.5 + 0.5;"
                           " gl_Position = vec4(p, 0.0, 1.0); }\n";
   static const char *fs = "uniform sampler2D t; varying vec2 uv;\n"
                           "void main() { gl_FragColor = texture2D(t, uv); }\n";
   const char *src[2] = {vs, fs};
   GLenum type[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
   GLuint p = glCreateProgram_();
   for (int i = 0; i < 2; i++) {
      GLuint sh = glCreateShader_(type[i]);
      glShaderSource_(sh, 1, &src[i], NULL);
      glCompileShader_(sh);
      glAttachShader_(p, sh);
   }
   glBindAttribLocation_(p, 0, "p");
   glLinkProgram_(p);
   glUseProgram_(p);

   static const unsigned sizes[][2] = {{32, 32}, {64, 64}, {64, 16},
                                       {16, 64}, {128, 8}, {256, 256}};
   for (unsigned i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++)
      for (unsigned j = 0; j < sizeof(sizes) / sizeof(sizes[0]); j++)
         run(&fmts[i], sizes[j][0], sizes[j][1]);
   printf("%s: %d failures\n", failures ? "FAIL" : "PASS", failures);
   return failures != 0;
}
