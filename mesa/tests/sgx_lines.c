/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * Lines that might lock up the 3D core (issue #5: a fishing line in
 * Minecraft 1.8.9 timed every frame out). Each case runs in its own process
 * (a reset marks the context guilty): desktop GL 2.1 fixed function, a
 * 1024x600 RGBA8 FBO with a 24-bit depth buffer, Minecraft's projection
 * (fovy 70, near 0.05, far 256), 20 frames of a clear, a grey floor and the
 * case's line, each frame ended with glFinish. A frame that takes over a
 * second is a timeout: HANG. Every case is drawn as Minecraft draws a line:
 * client arrays, a float position and an unsigned byte color per vertex.
 *
 *   inview      a 17-vertex strip, all in front of the near plane
 *   fish        RenderFish's strip: from the bobber 8 units out to the hand
 *               0.35 in front of the eye, width 2, depth write on
 *   fishlist    the same as GL_LINES
 *   blend       the same, blended and without depth write (as the block
 *               outline draws)
 *   alpha       fish with alpha test on (GREATER 0.1), as Minecraft draws
 *               its entities: st makes the test a discard
 *   alphalist   fishlist with alpha test on
 *   alphatri    control: a thin quad (triangles) with alpha test on
 *   discard     fish drawn by a GLSL program whose discard is never taken
 *   fragdepth   fish drawn by a GLSL program writing gl_FragDepth
 *   nearcross   one end between the eye and the near plane
 *   nearplane   one end on the near plane
 *   wzero       one end at the eye's depth (w = 0)
 *   behind      one end behind the eye
 *   bothbehind  both ends behind the eye
 *   degen       a line of length 0
 *   tiny        a line a thousandth of a pixel long
 *   offscreen   one end far past the guard band
 *   farcross    a line through the far plane
 *
 *   sgx_lines [case...]   (all of them by default)
 *
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -o sgx_lines
 * sgx_lines.c -ldl
 */
#include <dlfcn.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
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
   X(glAlphaFunc)                                                              \
   X(glAttachShader)                                                           \
   X(glCompileShader)                                                          \
   X(glCreateProgram)                                                          \
   X(glCreateShader)                                                           \
   X(glLinkProgram)                                                            \
   X(glShaderSource)                                                           \
   X(glUseProgram)                                                             \
   X(glBindFramebuffer)                                                        \
   X(glBindRenderbuffer)                                                       \
   X(glBindTexture)                                                            \
   X(glBlendFunc)                                                              \
   X(glCheckFramebufferStatus)                                                 \
   X(glClear)                                                                  \
   X(glClearColor)                                                             \
   X(glColorPointer)                                                           \
   X(glDepthFunc)                                                              \
   X(glDepthMask)                                                              \
   X(glDisable)                                                                \
   X(glDrawArrays)                                                             \
   X(glEnable)                                                                 \
   X(glEnableClientState)                                                      \
   X(glDisableClientState)                                                     \
   X(glFinish)                                                                 \
   X(glFramebufferRenderbuffer)                                                \
   X(glFramebufferTexture2D)                                                   \
   X(glGenFramebuffers)                                                        \
   X(glGenRenderbuffers)                                                       \
   X(glGenTextures)                                                            \
   X(glGetString)                                                              \
   X(glLineWidth)                                                              \
   X(glLoadIdentity)                                                           \
   X(glLoadMatrixf)                                                            \
   X(glMatrixMode)                                                             \
   X(glReadPixels)                                                             \
   X(glRenderbufferStorage)                                                    \
   X(glTexImage2D)                                                             \
   X(glVertexPointer)                                                          \
   X(glViewport)

#define DECL(n) static __typeof__(&n) n##_;
FUNCS(DECL)
#undef DECL

#define W 1024
#define H 600
#define FRAMES 20

struct vtx {
   float x, y, z;
   unsigned char c[4];
};

/* a case's vertices, in eye space (the eye looks down -z) */
struct lines {
   GLenum mode;
   unsigned n;
   struct vtx v[34];
   float width;
   bool blend, alpha;
   int prog; /* 1 a discard never taken, 2 gl_FragDepth */
};

static void
seg(struct lines *l, float x0, float y0, float z0, float x1, float y1, float z1)
{
   l->mode = GL_LINES;
   l->n = 2;
   l->v[0] = (struct vtx){x0, y0, z0, {0, 0, 0, 255}};
   l->v[1] = (struct vtx){x1, y1, z1, {0, 0, 0, 255}};
   l->width = 1.0f;
}

/* RenderFish (1.8.9): 17 vertices from the bobber to the hand, sagging */
static void
fish(struct lines *l, float hx, float hy, float hz)
{
   const float bx = 0.0f, by = -1.62f + 0.25f, bz = -8.0f;
   const float dx = hx - bx, dy = hy - by, dz = hz - bz;

   l->mode = GL_LINE_STRIP;
   l->n = 17;
   for (unsigned i = 0; i <= 16; i++) {
      float f = i / 16.0f;
      l->v[i] = (struct vtx){bx + dx * f, by + dy * (f * f + f) * 0.5f,
                             bz + dz * f, {0, 0, 0, 255}};
   }
   l->width = 2.0f;
}

static bool
build(const char *name, struct lines *l)
{
   memset(l, 0, sizeof(*l));
   if (!strcmp(name, "inview")) {
      fish(l, 0.36f, 0.03f, -0.5f);
      l->width = 1.0f;
   } else if (!strcmp(name, "fish")) {
      fish(l, 0.36f, 0.03f, -0.35f);
   } else if (!strcmp(name, "fishlist")) {
      struct lines s;
      fish(&s, 0.36f, 0.03f, -0.35f);
      l->mode = GL_LINES;
      l->n = 2 * (s.n - 1);
      for (unsigned i = 0; i < l->n; i++)
         l->v[i] = s.v[(i + 1) / 2];
      l->width = s.width;
   } else if (!strcmp(name, "blend")) {
      fish(l, 0.36f, 0.03f, -0.35f);
      l->blend = true;
   } else if (!strcmp(name, "alpha")) {
      fish(l, 0.36f, 0.03f, -0.35f);
      l->alpha = true;
   } else if (!strcmp(name, "alphalist")) {
      build("fishlist", l);
      l->alpha = true;
   } else if (!strcmp(name, "alphatri")) {
      /* two triangles 0.02 wide along the line's chord */
      static const float q[4][3] = {{0.0f, -1.37f, -8.0f},
                                    {0.02f, -1.37f, -8.0f},
                                    {0.36f, 0.03f, -0.35f},
                                    {0.38f, 0.03f, -0.35f}};
      l->mode = GL_TRIANGLE_STRIP;
      l->n = 4;
      for (unsigned i = 0; i < 4; i++)
         l->v[i] = (struct vtx){q[i][0], q[i][1], q[i][2], {0, 0, 0, 255}};
      l->width = 1.0f;
      l->alpha = true;
   } else if (!strcmp(name, "discard")) {
      fish(l, 0.36f, 0.03f, -0.35f);
      l->prog = 1;
   } else if (!strcmp(name, "fragdepth")) {
      fish(l, 0.36f, 0.03f, -0.35f);
      l->prog = 2;
   } else if (!strcmp(name, "nearcross")) {
      seg(l, 0.01f, 0.0f, -0.02f, 0.0f, 0.0f, -5.0f);
   } else if (!strcmp(name, "nearplane")) {
      seg(l, 0.01f, 0.0f, -0.05f, 0.0f, 0.0f, -5.0f);
   } else if (!strcmp(name, "wzero")) {
      seg(l, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, -5.0f);
   } else if (!strcmp(name, "behind")) {
      seg(l, 0.2f, 0.1f, 0.5f, 0.0f, 0.0f, -5.0f);
   } else if (!strcmp(name, "bothbehind")) {
      seg(l, 0.2f, 0.1f, 0.5f, -0.3f, 0.0f, 2.0f);
   } else if (!strcmp(name, "degen")) {
      seg(l, 0.1f, 0.1f, -2.0f, 0.1f, 0.1f, -2.0f);
   } else if (!strcmp(name, "tiny")) {
      /* a pixel is about 2 * tan(35°) * 2 / 600 = 0.0047 units at z -2 */
      seg(l, 0.1f, 0.1f, -2.0f, 0.1f + 4.7e-6f, 0.1f, -2.0f);
   } else if (!strcmp(name, "offscreen")) {
      seg(l, 1000.0f, 0.0f, -1.0f, 0.0f, 0.0f, -5.0f);
   } else if (!strcmp(name, "farcross")) {
      seg(l, 0.0f, 0.0f, -5.0f, 0.5f, 0.0f, -400.0f);
   } else {
      return false;
   }
   return true;
}

static const char *const all[] = {
   "inview",    "fish",      "fishlist", "blend",     "nearcross",
   "nearplane", "wzero",     "behind",   "bothbehind", "degen",
   "tiny",      "offscreen", "farcross", "alphatri",  "alpha",
   "alphalist", "discard",   "fragdepth",
};

static double
now(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec + t.tv_nsec * 1e-9;
}

static int
run(const char *name)
{
   struct lines l;
   if (!build(name, &l)) {
      fprintf(stderr, "no case %s\n", name);
      return 2;
   }

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
#define GET(n) n##_ = L(#n);
   FUNCS(GET)
#undef GET

   GLuint tex, fb, depth;
   glGenTextures_(1, &tex);
   glBindTexture_(GL_TEXTURE_2D, tex);
   glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
   glGenFramebuffers_(1, &fb);
   glBindFramebuffer_(GL_FRAMEBUFFER, fb);
   glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex, 0);
   glGenRenderbuffers_(1, &depth);
   glBindRenderbuffer_(GL_RENDERBUFFER, depth);
   glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
   glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, depth);
   if (glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      return 2;

   /* gluPerspective(70, W/H, 0.05, 256), column major */
   const float fovy = 70.0f * (float)M_PI / 180.0f, zn = 0.05f, zf = 256.0f;
   const float f = 1.0f / tanf(fovy / 2), a = (float)W / H;
   const float proj[16] = {f / a, 0, 0, 0, 0, f, 0, 0,
                           0, 0, (zf + zn) / (zn - zf), -1,
                           0, 0, 2 * zf * zn / (zn - zf), 0};
   glViewport_(0, 0, W, H);
   glMatrixMode_(GL_PROJECTION);
   glLoadMatrixf_(proj);
   glMatrixMode_(GL_MODELVIEW);
   glLoadIdentity_();
   glEnable_(GL_DEPTH_TEST);
   glDepthFunc_(GL_LEQUAL);
   glEnableClientState_(GL_VERTEX_ARRAY);
   glEnableClientState_(GL_COLOR_ARRAY);

   /* a floor 1.62 below the eye, so the line has depth to test against */
   static const struct vtx floor[4] = {
      {-50, -1.62f, 1, {90, 90, 90, 255}},
      {50, -1.62f, 1, {90, 90, 90, 255}},
      {-50, -1.62f, -100, {90, 90, 90, 255}},
      {50, -1.62f, -100, {90, 90, 90, 255}},
   };

   /* the GLSL cases: fixed function's transform and color */
   GLuint prog = 0;
   if (l.prog) {
      const char *vs = "#version 120\n"
                       "void main() {\n"
                       "   gl_FrontColor = gl_Color;\n"
                       "   gl_Position = ftransform();\n"
                       "}\n";
      const char *fs =
         l.prog == 1 ? "#version 120\n"
                       "void main() {\n"
                       "   if (gl_Color.a < -1.0) discard;\n"
                       "   gl_FragColor = gl_Color;\n"
                       "}\n"
                     : "#version 120\n"
                       "void main() {\n"
                       "   gl_FragDepth = gl_FragCoord.z;\n"
                       "   gl_FragColor = gl_Color;\n"
                       "}\n";
      const char *src[2] = {vs, fs};
      GLenum type[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
      prog = glCreateProgram_();
      for (int i = 0; i < 2; i++) {
         GLuint sh = glCreateShader_(type[i]);
         glShaderSource_(sh, 1, &src[i], NULL);
         glCompileShader_(sh);
         glAttachShader_(prog, sh);
      }
      glLinkProgram_(prog);
   }
   glAlphaFunc_(GL_GREATER, 0.1f);

   double worst = 0;
   for (int fr = 0; fr < FRAMES; fr++) {
      double t0 = now();
      glClearColor_(0.5f, 0.7f, 1.0f, 1.0f);
      glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glDisable_(GL_BLEND);
      glDepthMask_(GL_TRUE);
      glLineWidth_(1.0f);
      glVertexPointer_(3, GL_FLOAT, sizeof(struct vtx), &floor[0].x);
      glColorPointer_(4, GL_UNSIGNED_BYTE, sizeof(struct vtx), floor[0].c);
      glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);

      if (l.blend) {
         glEnable_(GL_BLEND);
         glBlendFunc_(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
         glDepthMask_(GL_FALSE);
      }
      glLineWidth_(l.width);
      if (l.alpha)
         glEnable_(GL_ALPHA_TEST);
      if (prog)
         glUseProgram_(prog);
      glVertexPointer_(3, GL_FLOAT, sizeof(struct vtx), &l.v[0].x);
      glColorPointer_(4, GL_UNSIGNED_BYTE, sizeof(struct vtx), l.v[0].c);
      glDrawArrays_(l.mode, 0, l.n);
      glDisable_(GL_ALPHA_TEST);
      glUseProgram_(0);
      glFinish_();
      double dt = now() - t0;
      if (dt > worst)
         worst = dt;
   }

   /* the line's pixels: black ones */
   static unsigned char px[W * H * 4];
   glReadPixels_(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
   unsigned black = 0;
   for (unsigned i = 0; i < W * H; i++)
      black += px[i * 4] < 16 && px[i * 4 + 1] < 16 && px[i * 4 + 2] < 16;
   printf("%s: %s (slowest frame %.0f ms, %u line pixels)\n", name,
          worst > 1.0 ? "HANG" : "ok", worst * 1000, black);
   fflush(stdout);
   return worst > 1.0;
}

int
main(int argc, char **argv)
{
   const char *const *names = argc > 1 ? (const char *const *)argv + 1 : all;
   int count = argc > 1 ? argc - 1 : (int)(sizeof(all) / sizeof(all[0]));
   int hangs = 0;

   for (int i = 0; i < count; i++) {
      pid_t pid = fork();
      if (pid == 0)
         _exit(run(names[i]));
      int st = 0;
      waitpid(pid, &st, 0);
      if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
         hangs++;
         if (!WIFEXITED(st) || WEXITSTATUS(st) != 1)
            printf("%s: exited with status 0x%x\n", names[i], st);
      }
   }
   return hangs != 0;
}
