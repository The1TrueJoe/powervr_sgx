/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * Occlusion queries (SGX_DEBUG=occlusion, docs/GL3.md H5) through GLES2's
 * EXT_occlusion_query_boolean: GL_ANY_SAMPLES_PASSED around draws into a
 * 64x64 pbuffer with depth and stencil, each against what GL says:
 *
 *   visible       a quad in the viewport                         true
 *   none          a query with no draw                           false
 *   clipped       a quad outside the viewport                    false
 *   depth         a quad behind the depth buffer (LESS)          false
 *   hidden-later  a quad drawn, then covered by a nearer one
 *                 after the query ends                           true
 *   behind        a quad drawn behind one drawn before it        false
 *   colormask     colour mask 0, depth writes off (a bounding
 *                 box), in front / behind                        true / false
 *   discard       every pixel discarded / half of them           false / true
 *   stencil       the stencil test failing everywhere            false
 *   scissor       a scissor box the quad misses / hits           false / true
 *   sliver        a triangle that covers no pixel centre         false
 *   many          200 queries in one frame (no depth writes), every
 *                 other one behind the depth buffer              alternating
 *   poll          GL_QUERY_RESULT_AVAILABLE goes true without
 *                 a wait, then the result                        true
 *
 *   SGX_DEBUG=occlusion sgx_occlusion
 *
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -o sgx_occlusion
 * sgx_occlusion.c -ldl
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
G(glClearDepthf, PFNGLCLEARDEPTHFPROC);
G(glClearStencil, PFNGLCLEARSTENCILPROC);
G(glClear, PFNGLCLEARPROC);
G(glViewport, PFNGLVIEWPORTPROC);
G(glGetString, PFNGLGETSTRINGPROC);
G(glGetError, PFNGLGETERRORPROC);
G(glEnable, PFNGLENABLEPROC);
G(glDisable, PFNGLDISABLEPROC);
G(glDepthFunc, PFNGLDEPTHFUNCPROC);
G(glDepthMask, PFNGLDEPTHMASKPROC);
G(glColorMask, PFNGLCOLORMASKPROC);
G(glStencilFunc, PFNGLSTENCILFUNCPROC);
G(glScissor, PFNGLSCISSORPROC);
G(glFinish, PFNGLFINISHPROC);
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
G(glUseProgram, PFNGLUSEPROGRAMPROC);
G(glGetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC);
G(glUniform1f, PFNGLUNIFORM1FPROC);
G(glVertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC);
G(glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC);
G(glDrawArrays, PFNGLDRAWARRAYSPROC);
G(glGenQueriesEXT, PFNGLGENQUERIESEXTPROC);
G(glBeginQueryEXT, PFNGLBEGINQUERYEXTPROC);
G(glEndQueryEXT, PFNGLENDQUERYEXTPROC);
G(glGetQueryObjectuivEXT, PFNGLGETQUERYOBJECTUIVEXTPROC);

#define FB 64
#define glEnable glEnable_
#define glDisable glDisable_

static int failures, cases;
static GLuint prog[2]; /* plain, discarding x < 0.5 when "cut" > 0.5 */
static GLint u_z[2], u_cut;

static GLuint
program(const char *fs)
{
   static const char *vs = "attribute vec2 p;\n"
                           "uniform float z;\n"
                           "varying vec2 v;\n"
                           "void main() {\n"
                           "   v = p * 0.5 + 0.5;\n"
                           "   gl_Position = vec4(p, z, 1.0);\n"
                           "}\n";
   const char *src[2] = {vs, fs};
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
      printf("  link failed\n");
      exit(1);
   }
   return p;
}

/* a rectangle in clip space at depth z (-1 near .. 1 far) */
static void
rect(int which, float x0, float y0, float x1, float y1, float z)
{
   const float v[] = {x0, y0, x1, y0, x0, y1, x1, y1};

   glUseProgram_(prog[which]);
   glUniform1f_(u_z[which], z);
   glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, 0, v);
   glEnableVertexAttribArray_(0);
   glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
}

static void
full(float z)
{
   rect(0, -1, -1, 1, 1, z);
}

/* the depth buffer at 0.5 (z 0 in clip space) everywhere */
static void
reset(void)
{
   glDisable(GL_SCISSOR_TEST);
   glDisable(GL_STENCIL_TEST);
   glColorMask_(1, 1, 1, 1);
   glDepthMask_(1);
   glClearColor_(0, 0, 0, 1);
   glClearDepthf_(1);
   glClearStencil_(0);
   glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
   glEnable(GL_DEPTH_TEST);
   glDepthFunc_(GL_LESS);
   full(0);
}

static GLuint
result(GLuint q)
{
   GLuint r = 0xdead;

   glGetQueryObjectuivEXT_(q, GL_QUERY_RESULT_EXT, &r);
   return r;
}

static void
check(const char *name, GLuint q, GLuint want)
{
   GLuint got = result(q);
   GLenum e = glGetError_();

   cases++;
   printf("%-26s %s", name, got == want && !e ? "ok" : "FAIL");
   if (got != want)
      printf(" (got %u, want %u)", got, want);
   if (e)
      printf(" (GL error %#x)", e);
   printf("\n");
   failures += got != want || e;
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
                  EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24,
                  EGL_STENCIL_SIZE, 8, EGL_RENDERABLE_TYPE,
                  EGL_OPENGL_ES2_BIT, EGL_NONE};
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
   GET(glClearDepthf);
   GET(glClearStencil);
   GET(glClear);
   GET(glViewport);
   GET(glGetString);
   GET(glGetError);
   glEnable_ = L("glEnable");
   glDisable_ = L("glDisable");
   GET(glDepthFunc);
   GET(glDepthMask);
   GET(glColorMask);
   GET(glStencilFunc);
   GET(glScissor);
   GET(glFinish);
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
   GET(glVertexAttribPointer);
   GET(glEnableVertexAttribArray);
   GET(glDrawArrays);

   if (!strstr((const char *)glGetString_(GL_EXTENSIONS),
               "GL_EXT_occlusion_query_boolean")) {
      printf("no GL_EXT_occlusion_query_boolean (SGX_DEBUG=occlusion)\n");
      return 2;
   }
   GET(glGenQueriesEXT);
   GET(glBeginQueryEXT);
   GET(glEndQueryEXT);
   GET(glGetQueryObjectuivEXT);
   printf("%s\n", glGetString_(GL_RENDERER));

   prog[0] = program("precision mediump float;\n"
                     "void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n");
   prog[1] = program("precision mediump float;\n"
                     "uniform float cut;\n"
                     "varying vec2 v;\n"
                     "void main() {\n"
                     "   if (v.x < cut) discard;\n"
                     "   gl_FragColor = vec4(0.0, 0.0, 1.0, 1.0);\n"
                     "}\n");
   for (int i = 0; i < 2; i++)
      u_z[i] = glGetUniformLocation_(prog[i], "z");
   u_cut = glGetUniformLocation_(prog[1], "cut");
   glViewport_(0, 0, FB, FB);

   GLuint q[256];
   glGenQueriesEXT_(256, q);

#define Q(i) glBeginQueryEXT_(GL_ANY_SAMPLES_PASSED_EXT, q[i])
#define E() glEndQueryEXT_(GL_ANY_SAMPLES_PASSED_EXT)

   reset();
   Q(0), full(-0.5f), E();
   check("visible", q[0], 1);

   reset();
   Q(1), E();
   check("none", q[1], 0);

   reset();
   Q(2), rect(0, 2, 2, 3, 3, -0.5f), E();
   check("clipped", q[2], 0);

   reset();
   Q(3), full(0.5f), E();
   check("depth", q[3], 0);

   reset();
   Q(4), rect(0, -0.5f, -0.5f, 0.5f, 0.5f, -0.2f), E();
   full(-0.9f); /* covers it, after the query */
   check("hidden-later", q[4], 1);

   reset();
   full(-0.9f);
   Q(5), rect(0, -0.5f, -0.5f, 0.5f, 0.5f, -0.2f), E();
   check("behind", q[5], 0);

   reset();
   glColorMask_(0, 0, 0, 0);
   glDepthMask_(0);
   Q(6), full(-0.5f), E();
   Q(7), full(0.5f), E();
   glColorMask_(1, 1, 1, 1);
   glDepthMask_(1);
   check("colormask in front", q[6], 1);
   check("colormask behind", q[7], 0);

   reset();
   glUseProgram_(prog[1]);
   glUniform1f_(u_cut, 2.0f);
   Q(8), rect(1, -1, -1, 1, 1, -0.5f), E();
   glUseProgram_(prog[1]);
   glUniform1f_(u_cut, 0.5f);
   Q(9), rect(1, -1, -1, 1, 1, -0.5f), E();
   check("discard all", q[8], 0);
   check("discard half", q[9], 1);

   reset();
   glEnable(GL_STENCIL_TEST);
   glStencilFunc_(GL_EQUAL, 1, 0xff); /* the stencil is 0 */
   Q(10), full(-0.5f), E();
   glDisable(GL_STENCIL_TEST);
   check("stencil", q[10], 0);

   reset();
   glEnable(GL_SCISSOR_TEST);
   glScissor_(0, 0, 16, 16);
   Q(11), rect(0, 0, 0, 1, 1, -0.5f), E(); /* the top right quarter */
   Q(12), rect(0, -1, -1, 0, 0, -0.5f), E();
   glDisable(GL_SCISSOR_TEST);
   check("scissor miss", q[11], 0);
   check("scissor hit", q[12], 1);

   /* between the centres of columns 31 and 32 (x 31.5 and 32.5): clip x
    * 0.0 to 0.015 is pixels 32.0 to 32.48 */
   reset();
   Q(13);
   {
      const float v[] = {0.0f, -1.0f, 0.015f, -1.0f, 0.0f, 1.0f};

      glUseProgram_(prog[0]);
      glUniform1f_(u_z[0], -0.5f);
      glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, 0, v);
      glDrawArrays_(GL_TRIANGLES, 0, 3);
   }
   E();
   check("sliver", q[13], 0);

   reset();
   glDepthMask_(0); /* a strip drawn twice is tested against the depth buffer */
   for (int i = 0; i < 200; i++) {
      Q(20 + i);
      rect(0, -1 + (i % 20) * 0.1f, -1, -0.9f + (i % 20) * 0.1f, 1,
           i & 1 ? 0.5f : -0.5f);
      E();
   }
   {
      int bad = 0;

      for (int i = 0; i < 200; i++)
         bad += result(q[20 + i]) != !(i & 1);
      cases++;
      printf("%-26s %s\n", "many", bad ? "FAIL" : "ok");
      if (bad)
         printf("  %d of 200 wrong\n", bad);
      failures += bad != 0;
   }

   reset();
   Q(14), full(-0.5f), E();
   {
      GLuint avail = 0;
      int polls = 0;

      while (!avail && polls < 1000000) {
         glGetQueryObjectuivEXT_(q[14], GL_QUERY_RESULT_AVAILABLE_EXT, &avail);
         polls++;
      }
      cases++;
      printf("%-26s %s (%d polls)\n", "poll", avail ? "ok" : "FAIL", polls);
      failures += !avail;
   }
   check("poll result", q[14], 1);

   printf("%d case(s), %d failed\n", cases, failures);
   return failures != 0;
}
