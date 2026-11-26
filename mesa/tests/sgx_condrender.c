/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * Conditional rendering (NV_conditional_render, ARB_conditional_render_inverted)
 * on occlusion queries: a query around a quad that passes or one that fails
 * the depth test, then a conditional clear-coloured quad, read back.  Every
 * mode, both query targets (SAMPLES_PASSED, ANY_SAMPLES_PASSED), with the
 * query's draw in the same frame as the conditional one; and a conditional
 * glClear.  The no-wait modes may draw either way when the result isn't in
 * yet, so they are checked only after a glFinish.
 *
 *   sgx_condrender
 *
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -o sgx_condrender
 * sgx_condrender.c -ldl
 */
#include <dlfcn.h>
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
G(glClearDepth);
G(glEnable);
G(glDisable);
G(glDepthFunc);
G(glReadPixels);
G(glBegin);
G(glEnd);
G(glVertex3f);
G(glColor3f);
G(glGetString);
G(glGenQueries);
G(glBeginQuery);
G(glEndQuery);
G(glBeginConditionalRender);
G(glEndConditionalRender);
G(glColorMask);
G(glDepthMask);
G(glFinish);
G(glGetError);

#define S 32

static void
quad(float z)
{
   glBegin_(GL_QUADS);
   glVertex3f_(-1, -1, z);
   glVertex3f_(1, -1, z);
   glVertex3f_(1, 1, z);
   glVertex3f_(-1, 1, z);
   glEnd_();
}

/* the centre pixel's red */
static int
red(void)
{
   unsigned char px[4];

   glReadPixels_(S / 2, S / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
   return px[0] > 128;
}

static const struct {
   GLenum mode;
   const char *name;
   int inverted, wait;
} modes[] = {
   {GL_QUERY_WAIT, "WAIT", 0, 1},
   {GL_QUERY_NO_WAIT, "NO_WAIT", 0, 0},
   {GL_QUERY_BY_REGION_WAIT, "BY_REGION_WAIT", 0, 1},
   {GL_QUERY_BY_REGION_NO_WAIT, "BY_REGION_NO_WAIT", 0, 0},
   {GL_QUERY_WAIT_INVERTED, "WAIT_INVERTED", 1, 1},
   {GL_QUERY_NO_WAIT_INVERTED, "NO_WAIT_INVERTED", 1, 0},
   {GL_QUERY_BY_REGION_WAIT_INVERTED, "BY_REGION_WAIT_INVERTED", 1, 1},
   {GL_QUERY_BY_REGION_NO_WAIT_INVERTED, "BY_REGION_NO_WAIT_INVERTED", 1, 0},
};

/* one case: the query around a quad that passes (visible) or not, then a
 * conditional red quad (or glClear to red) */
static int
one(GLuint q, GLenum target, unsigned m, int visible, int clear, int inverted_ok)
{
   if (modes[m].inverted && !inverted_ok)
      return 0;
   glClearColor_(0, 0, 0, 0);
   glClearDepth_(0.5);
   glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   glEnable_(GL_DEPTH_TEST);
   glDepthFunc_(GL_LESS);
   /* the query's draw: z -0.5 passes depth 0.5 (window 0.25), z 0.5 fails */
   glColorMask_(0, 0, 0, 0);
   glDepthMask_(0);
   glBeginQuery_(target, q);
   quad(visible ? -0.5f : 0.5f);
   glEndQuery_(target);
   glColorMask_(1, 1, 1, 1);
   glDisable_(GL_DEPTH_TEST);
   if (!modes[m].wait)
      glFinish_();
   glBeginConditionalRender_(q, modes[m].mode);
   if (clear) {
      glClearColor_(1, 0, 0, 1);
      glClear_(GL_COLOR_BUFFER_BIT);
   } else {
      glColor3f_(1, 0, 0);
      quad(0);
   }
   glEndConditionalRender_();
   glDepthMask_(1);

   int want = visible != modes[m].inverted, got = red();
   GLenum e = glGetError_();
   if (got == want && !e)
      return 0;
   printf("FAIL %s %s, %s query, %s: drew %d, want %d (error %#x)\n",
          target == GL_SAMPLES_PASSED ? "SAMPLES_PASSED" : "ANY_SAMPLES_PASSED",
          modes[m].name, visible ? "visible" : "hidden",
          clear ? "glClear" : "quad", got, want, e);
   return 1;
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
                  EGL_DEPTH_SIZE, 24, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
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
   GET(glClearColor);
   GET(glClear);
   GET(glClearDepth);
   GET(glEnable);
   GET(glDisable);
   GET(glDepthFunc);
   GET(glReadPixels);
   GET(glBegin);
   GET(glEnd);
   GET(glVertex3f);
   GET(glColor3f);
   GET(glGetString);
   GET(glGenQueries);
   GET(glBeginQuery);
   GET(glEndQuery);
   GET(glColorMask);
   GET(glDepthMask);
   GET(glFinish);
   GET(glGetError);
   glBeginConditionalRender_ = (void *)gpa("glBeginConditionalRenderNV");
   glEndConditionalRender_ = (void *)gpa("glEndConditionalRenderNV");
   const char *ext = (const char *)glGetString_(GL_EXTENSIONS);
   if (!strstr(ext, "GL_NV_conditional_render")) {
      printf("SKIP: no GL_NV_conditional_render\n");
      return 77;
   }
   int inverted = !!strstr(ext, "GL_ARB_conditional_render_inverted");
   printf("renderer: %s, inverted modes %s\n", glGetString_(GL_RENDERER),
          inverted ? "yes" : "no");

   GLuint q[2];
   GLenum targets[2] = {GL_SAMPLES_PASSED, GL_ANY_SAMPLES_PASSED};
   int fails = 0, cases = 0;
   glGenQueries_(2, q);
   for (unsigned t = 0; t < 2; t++)
      for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++)
         for (int vis = 0; vis < 2; vis++)
            for (int clear = 0; clear < 2; clear++) {
               fails += one(q[t], targets[t], m, vis, clear, inverted);
               cases++;
            }
   printf("%s: %d of %d cases failed\n", fails ? "FAIL" : "PASS", fails, cases);
   return !!fails;
}
