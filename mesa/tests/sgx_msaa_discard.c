/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * Programs that can discard, on a one-sample and a 4x multisampled FBO
 * (issue #89). A 64x64 RGBA8 target with a 24-bit depth buffer, full-screen
 * quads at a constant depth, depth test LESS. Every check is run with 0 and
 * 4 samples (the 4-sample target resolved by glBlitFramebuffer) and must give
 * the same result:
 *
 *   varying    1, 2 and 3 float varyings (u = x / 2 + 1/2, v = 1 - u,
 *              w = u / 2) written as red, green and blue by a plain program,
 *              by one with a discard that is never taken, by one that
 *              discards where u < 0.5, and by the plain one with GL_ALPHA_TEST
 *              on (st makes the test a discard). Read at 8 columns: each
 *              color within 2 of its varying, the cut columns clear.
 *   fragcoord  a program without varyings that discards where gl_FragCoord.x
 *              is under half the width: the left half clear, the right drawn.
 *   depth      a quad at depth 0.75 drawn by each of those programs (0, 1
 *              and 2 varyings; the u cut needs one), by one that discards
 *              on gl_FragCoord and by one that also writes gl_FragDepth, then
 *              a plain quad at depth d: it must be hidden exactly when
 *              d >= 0.75 (read in the right half, where the cuts keep the
 *              first quad).
 *   slope      the same first quads with a depth from 0.25 at the left edge
 *              to 0.75 at the right, then a plain quad at 0.5: every pixel
 *              as with the plain first quad (the plane's slope, not only
 *              its constant).
 *
 *   sgx_msaa_discard [samples...]   (0 and 4 by default)
 *
 * GL and EGL through libEGL.so.1 (glvnd) and eglGetProcAddress:
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -o sgx_msaa_discard
 * sgx_msaa_discard.c -ldl -lm
 */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EGL_EGL_PROTOTYPES 0
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <GL/gl.h>
#include <GL/glext.h>

#define W 64

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
G(glClearDepth);
G(glViewport);
G(glEnable);
G(glDisable);
G(glDepthFunc);
G(glDepthMask);
G(glAlphaFunc);
G(glDrawArrays);
G(glReadPixels);
G(glGetIntegerv);
G(glGetString);
G(glGetError);
G(glGenFramebuffers);
G(glBindFramebuffer);
G(glGenRenderbuffers);
G(glBindRenderbuffer);
G(glRenderbufferStorage);
G(glRenderbufferStorageMultisample);
G(glFramebufferRenderbuffer);
G(glCheckFramebufferStatus);
G(glBlitFramebuffer);
G(glDeleteFramebuffers);
G(glDeleteRenderbuffers);
G(glCreateShader);
G(glShaderSource);
G(glCompileShader);
G(glGetShaderiv);
G(glGetShaderInfoLog);
G(glCreateProgram);
G(glAttachShader);
G(glBindAttribLocation);
G(glLinkProgram);
G(glGetProgramiv);
G(glUseProgram);
G(glGetUniformLocation);
G(glUniform1f);
G(glUniform4f);
G(glVertexAttribPointer);
G(glEnableVertexAttribArray);

static int failures;

static void
check(const char *name, int ok, const char *detail)
{
   printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, detail[0] ? ": " : "",
          detail);
   failures += !ok;
}

static GLuint
shader(GLenum type, const char *src)
{
   GLuint s = glCreateShader_(type);
   GLint ok = 0;
   char log[1024];

   glShaderSource_(s, 1, &src, NULL);
   glCompileShader_(s);
   glGetShaderiv_(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      glGetShaderInfoLog_(s, sizeof(log), NULL, log);
      fprintf(stderr, "compile: %s\n%s\n", log, src);
      exit(2);
   }
   return s;
}

/* n varyings (0-3); body: the fragment program's main */
static GLuint
program(unsigned n, const char *body)
{
   static const char *decl[] = {"", "varying float u;\n",
                                "varying float u, v;\n",
                                "varying float u, v, w;\n"};
   static const char *set[] = {"", "u = p.x * 0.5 + 0.5;",
                               "u = p.x * 0.5 + 0.5; v = 1.0 - u;",
                               "u = p.x * 0.5 + 0.5; v = 1.0 - u; w = u * 0.5;"};
   char vs[512], fs[512];
   GLuint p = glCreateProgram_();
   GLint ok = 0;

   snprintf(vs, sizeof(vs),
            "attribute vec3 p;\n%s"
            "void main() { %s gl_Position = vec4(p, 1.0); }\n",
            decl[n], set[n]);
   snprintf(fs, sizeof(fs), "uniform vec4 c;\nuniform float k, halfw;\n%s"
            "void main() { %s }\n", decl[n], body);
   glAttachShader_(p, shader(GL_VERTEX_SHADER, vs));
   glAttachShader_(p, shader(GL_FRAGMENT_SHADER, fs));
   glBindAttribLocation_(p, 0, "p");
   glLinkProgram_(p);
   glGetProgramiv_(p, GL_LINK_STATUS, &ok);
   if (!ok) {
      fprintf(stderr, "link failed\n");
      exit(2);
   }
   return p;
}

static void
quad(GLuint prog, float z, float r, float g, float b)
{
   const GLfloat v[] = {-1, -1, z, 1, -1, z, -1, 1, z, 1, 1, z};

   glUseProgram_(prog);
   glUniform4f_(glGetUniformLocation_(prog, "c"), r, g, b, 1);
   glUniform1f_(glGetUniformLocation_(prog, "k"), 2);
   glUniform1f_(glGetUniformLocation_(prog, "halfw"), W / 2.0f);
   glVertexAttribPointer_(0, 3, GL_FLOAT, GL_FALSE, 0, v);
   glEnableVertexAttribArray_(0);
   glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
}

/* the target (samples 0 or 4) and its one-sample resolve */
static GLuint fb, rfb, rb[3];
static unsigned char px[W * W * 4];

static int
target(int samples)
{
   glGenFramebuffers_(1, &fb);
   glBindFramebuffer_(GL_FRAMEBUFFER, fb);
   glGenRenderbuffers_(3, rb);
   glBindRenderbuffer_(GL_RENDERBUFFER, rb[0]);
   glRenderbufferStorageMultisample_(GL_RENDERBUFFER, samples, GL_RGBA8, W, W);
   glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, rb[0]);
   glBindRenderbuffer_(GL_RENDERBUFFER, rb[1]);
   glRenderbufferStorageMultisample_(GL_RENDERBUFFER, samples,
                                     GL_DEPTH_COMPONENT24, W, W);
   glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, rb[1]);
   if (glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      return 0;
   GLint s = -1;
   glGetIntegerv_(GL_SAMPLES, &s);
   if (s != samples)
      return 0;
   glGenFramebuffers_(1, &rfb);
   glBindFramebuffer_(GL_FRAMEBUFFER, rfb);
   glBindRenderbuffer_(GL_RENDERBUFFER, rb[2]);
   glRenderbufferStorage_(GL_RENDERBUFFER, GL_RGBA8, W, W);
   glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, rb[2]);
   return glCheckFramebufferStatus_(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static void
begin(void)
{
   glBindFramebuffer_(GL_FRAMEBUFFER, fb);
   glViewport_(0, 0, W, W);
   glEnable_(GL_DEPTH_TEST);
   glDepthFunc_(GL_LESS);
   glDepthMask_(GL_TRUE);
   glClearColor_(0, 0, 0, 1);
   glClearDepth_(1.0);
   glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static void
finish(void)
{
   glBindFramebuffer_(GL_READ_FRAMEBUFFER, fb);
   glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, rfb);
   glBlitFramebuffer_(0, 0, W, W, 0, 0, W, W, GL_COLOR_BUFFER_BIT, GL_NEAREST);
   glBindFramebuffer_(GL_FRAMEBUFFER, rfb);
   glReadPixels_(0, 0, W, W, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

static const unsigned char *
at(int x, int y)
{
   return px + (y * W + x) * 4;
}

enum { PLAIN, NEVER, CUT, ALPHA, FRAGCUT, FRAGDEPTH, N_KINDS };
static const char *kind_name[] = {"plain",           "never-taken discard",
                                  "discard u < 0.5", "alpha test",
                                  "fragcoord cut",   "gl_FragDepth + discard"};

static GLuint
kind_program(unsigned n, unsigned kind, int show)
{
   static const char *out[] = {
      "gl_FragColor = c;", "gl_FragColor = vec4(u, 0.0, 0.0, 1.0);",
      "gl_FragColor = vec4(u, v, 0.0, 1.0);",
      "gl_FragColor = vec4(u, v, w, 1.0);"};
   char body[256];

   snprintf(body, sizeof(body), "%s%s",
            kind == NEVER     ? "if (k < 0.0) discard; "
            : kind == CUT     ? "if (u < 0.5) discard; "
            : kind == FRAGCUT ? "if (gl_FragCoord.x < halfw) discard; "
            : kind == FRAGDEPTH
               ? "if (k < 0.0) discard; gl_FragDepth = gl_FragCoord.z; "
               : "",
            out[show ? n : 0]);
   return program(n, body);
}

static void
alpha_test(unsigned kind, int on)
{
   if (kind != ALPHA)
      return;
   if (on) {
      glEnable_(GL_ALPHA_TEST);
      glAlphaFunc_(GL_GREATER, 0.0f);
   } else {
      glDisable_(GL_ALPHA_TEST);
   }
}

static void
varyings(int samples)
{
   for (unsigned n = 1; n <= 3; n++) {
      for (unsigned kind = 0; kind <= ALPHA; kind++) {
         GLuint p = kind_program(n, kind, 1);
         char name[96], detail[256] = "", *d = detail;
         int ok = 1;

         begin();
         alpha_test(kind, 1);
         quad(p, 0, 0, 0, 0);
         alpha_test(kind, 0);
         finish();
         for (int i = 0; i < 8; i++) {
            int x = (2 * i + 1) * W / 16;
            float u = (x + 0.5f) / W;
            float want[3] = {u, n > 1 ? 1 - u : 0, n > 2 ? u / 2 : 0};
            const unsigned char *c = at(x, W / 2);
            int cut = kind == CUT && u < 0.5f;

            for (int ch = 0; ch < 3; ch++) {
               int e = cut ? 0 : (int)lroundf(want[ch] * 255);
               ok &= abs(c[ch] - e) <= 2;
            }
            d += snprintf(d, detail + sizeof(detail) - d, "%s%02x%02x%02x",
                          i ? " " : "", c[0], c[1], c[2]);
         }
         snprintf(name, sizeof(name), "varying %ux, %u varying%s, %s", samples,
                  n, n > 1 ? "s" : "", kind_name[kind]);
         check(name, ok, ok ? "" : detail);
      }
   }
}

static void
fragcoord(int samples)
{
   GLuint p = program(0, "if (gl_FragCoord.x < halfw) discard; "
                         "gl_FragColor = c;");
   int l = 0, r = 0;
   char name[64], detail[64];

   begin();
   quad(p, 0, 0, 1, 0);
   finish();
   for (int y = 0; y < W; y++)
      for (int x = 0; x < W; x++)
         if (at(x, y)[1] > 128)
            *(x < W / 2 ? &l : &r) += 1;
   snprintf(name, sizeof(name), "fragcoord %ux", samples);
   snprintf(detail, sizeof(detail), "%d | %d green, want 0 | %d", l, r,
            W * W / 2);
   check(name, l == 0 && r == W * W / 2, detail);
}

static void
depth(int samples)
{
   static const float ds[] = {0.05f, 0.25f, 0.5f, 0.7f, 0.8f, 0.95f};
   GLuint plain = kind_program(0, PLAIN, 0);

   for (unsigned n = 0; n <= 2; n++) {
      for (unsigned kind = 0; kind < N_KINDS; kind++) {
         if (kind == CUT && !n)
            continue;
         GLuint p = kind_program(n, kind, 0);
         char name[96], detail[256] = "", *d = detail;
         int ok = 1;

         for (unsigned i = 0; i < sizeof(ds) / sizeof(ds[0]); i++) {
            begin();
            alpha_test(kind, 1);
            quad(p, 0.5f, 0, 1, 0); /* depth 0.75 */
            alpha_test(kind, 0);
            quad(plain, ds[i] * 2 - 1, 0, 0, 1);
            finish();
            const unsigned char *c = at(3 * W / 4, W / 2);
            int shown = c[2] > 128, hid = c[1] > 128;

            ok &= ds[i] < 0.75f ? shown : hid;
            d += snprintf(d, detail + sizeof(detail) - d, "%s%.2f %s",
                          i ? ", " : "", ds[i],
                          shown ? "shown" : hid ? "hidden" : "neither");
         }
         snprintf(name, sizeof(name), "depth %ux, %u varying%s, %s", samples,
                  n, n == 1 ? "" : "s", kind_name[kind]);
         check(name, ok, ok ? "" : detail);
      }
   }
}

/* a quad from depth 0.25 (left) to 0.75 (right) */
static void
slope_quad(GLuint prog)
{
   const GLfloat v[] = {-1, -1, -0.5f, 1, -1, 0.5f, -1, 1, -0.5f, 1, 1, 0.5f};

   glUseProgram_(prog);
   glUniform4f_(glGetUniformLocation_(prog, "c"), 0, 1, 0, 1);
   glUniform1f_(glGetUniformLocation_(prog, "k"), 2);
   glUniform1f_(glGetUniformLocation_(prog, "halfw"), W / 2.0f);
   glVertexAttribPointer_(0, 3, GL_FLOAT, GL_FALSE, 0, v);
   glEnableVertexAttribArray_(0);
   glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
}

static void
slope(int samples)
{
   static unsigned char want[W * W * 4];
   GLuint plain = kind_program(0, PLAIN, 0);

   for (unsigned n = 0; n <= 2; n++) {
      for (unsigned kind = 0; kind < N_KINDS; kind++) {
         if ((kind == CUT && !n) || (kind == PLAIN && n))
            continue;
         GLuint p = kind_program(n, kind, 0);
         char name[96], detail[128] = "";
         int bad = 0, first = -1;

         begin();
         alpha_test(kind, 1);
         slope_quad(p);
         alpha_test(kind, 0);
         quad(plain, 0, 0, 0, 1); /* depth 0.5 */
         finish();
         if (kind == PLAIN) {
            memcpy(want, px, sizeof(px));
            continue;
         }
         /* the cuts drew nothing on the left: compare the right half */
         int x0 = kind == CUT || kind == FRAGCUT ? W / 2 : 0;
         for (int y = 0; y < W; y++) {
            for (int x = x0; x < W; x++) {
               if (memcmp(at(x, y), want + (y * W + x) * 4, 3)) {
                  if (first < 0)
                     first = y * W + x;
                  bad++;
               }
            }
         }
         if (bad)
            snprintf(detail, sizeof(detail),
                     "%d pixels differ from the plain quad's, first at %d,%d: "
                     "%02x%02x%02x, want %02x%02x%02x",
                     bad, first % W, first / W, px[first * 4],
                     px[first * 4 + 1], px[first * 4 + 2], want[first * 4],
                     want[first * 4 + 1], want[first * 4 + 2]);
         snprintf(name, sizeof(name), "slope %ux, %u varying%s, %s", samples,
                  n, n == 1 ? "" : "s", kind_name[kind]);
         check(name, !bad, detail);
      }
   }
}

int
main(int argc, char **argv)
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

   EGLDisplay dpy = getdisplay(EGL_DEFAULT_DISPLAY);
   if (!initialize(dpy, NULL, NULL)) {
      fprintf(stderr, "eglInitialize\n");
      return 2;
   }
   bindapi(EGL_OPENGL_API);
   EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8,
                  EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_RENDERABLE_TYPE,
                  EGL_OPENGL_BIT, EGL_NONE};
   EGLConfig cfg;
   EGLint n = 0;
   if (!chooseconfig(dpy, ca, &cfg, 1, &n) || !n) {
      fprintf(stderr, "no config\n");
      return 2;
   }
   EGLint sa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
   EGLSurface s = createpbuffer(dpy, cfg, sa);
   EGLContext c = createcontext(dpy, cfg, EGL_NO_CONTEXT, NULL);
   if (!s || !c || !makecurrent(dpy, s, s, c)) {
      fprintf(stderr, "context\n");
      return 2;
   }
   GET(glClearColor);
   GET(glClear);
   GET(glClearDepth);
   GET(glViewport);
   GET(glEnable);
   GET(glDisable);
   GET(glDepthFunc);
   GET(glDepthMask);
   GET(glAlphaFunc);
   GET(glDrawArrays);
   GET(glReadPixels);
   GET(glGetIntegerv);
   GET(glGetString);
   GET(glGetError);
   GET(glGenFramebuffers);
   GET(glBindFramebuffer);
   GET(glGenRenderbuffers);
   GET(glBindRenderbuffer);
   GET(glRenderbufferStorage);
   GET(glRenderbufferStorageMultisample);
   GET(glFramebufferRenderbuffer);
   GET(glCheckFramebufferStatus);
   GET(glBlitFramebuffer);
   GET(glDeleteFramebuffers);
   GET(glDeleteRenderbuffers);
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
   GET(glUseProgram);
   GET(glGetUniformLocation);
   GET(glUniform1f);
   GET(glUniform4f);
   GET(glVertexAttribPointer);
   GET(glEnableVertexAttribArray);

   printf("renderer: %s\n", glGetString_(GL_RENDERER));
   int list[8] = {0, 4}, nl = 2;
   if (argc > 1) {
      nl = 0;
      for (int i = 1; i < argc && nl < 8; i++)
         list[nl++] = atoi(argv[i]);
   }
   for (int i = 0; i < nl; i++) {
      char name[32];

      snprintf(name, sizeof(name), "target %dx", list[i]);
      if (!target(list[i])) {
         check(name, 0, "framebuffer incomplete");
         continue;
      }
      varyings(list[i]);
      fragcoord(list[i]);
      depth(list[i]);
      slope(list[i]);
      glBindFramebuffer_(GL_FRAMEBUFFER, 0);
      glDeleteFramebuffers_(1, &fb);
      glDeleteFramebuffers_(1, &rfb);
      glDeleteRenderbuffers_(3, rb);
   }
   GLenum e = glGetError_();
   if (e) {
      printf("FAIL glGetError %#x\n", e);
      failures++;
   }
   printf("%s: %d failure%s\n", failures ? "FAIL" : "PASS", failures,
          failures == 1 ? "" : "s");
   return failures ? 1 : 0;
}
