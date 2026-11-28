/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * User clip planes with no shader at all (issue #5: glClipPlane and
 * GL_CLIP_PLANE0 drew both halves of a quad).  Plane 0 keeps x >= 0; each
 * case draws white over black into a 64x64 pbuffer and reads both halves
 * back:
 *
 *   quad      glBegin(GL_QUADS), identity matrices
 *   strip     a client-array strip
 *   ortho     glOrtho(0, 64, 0, 64) with the plane set under a translate,
 *             so it's transformed by the modelview at glClipPlane
 *   line      a horizontal line across the plane
 *   points    a row of points, 1 px
 *   plane5    plane 5 alone (keeps x >= 0)
 *   plane6    plane 6 alone, plane7 plane 7 alone (GL_MAX_CLIP_PLANES 8:
 *             GLSL 1.30), clip7 a GLSL 1.30 gl_ClipDistance[7]
 *   psize     points under a plane that keeps all (distance 4): still 1 px,
 *             so the TA doesn't take the size from a clip distance
 *   fbo       quad into an RGBA8 FBO
 *   glsl      quad with a GLSL 1.20 vertex shader writing gl_ClipVertex
 *
 *   sgx_ucp [case...]
 *
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -o sgx_ucp
 * sgx_ucp.c -ldl
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
G(glViewport);
G(glEnable);
G(glDisable);
G(glClipPlane);
G(glMatrixMode);
G(glLoadIdentity);
G(glOrtho);
G(glTranslatef);
G(glReadPixels);
G(glGenTextures);
G(glBindTexture);
G(glTexImage2D);
G(glGenFramebuffers);
G(glBindFramebuffer);
G(glFramebufferTexture2D);
G(glCreateShader);
G(glShaderSource);
G(glCompileShader);
G(glCreateProgram);
G(glAttachShader);
G(glLinkProgram);
G(glUseProgram);
G(glBegin);
G(glEnd);
G(glVertex2f);
G(glColor3f);
G(glGetString);
G(glVertexPointer);
G(glEnableClientState);
G(glDisableClientState);
G(glDrawArrays);
G(glFinish);
G(glGetIntegerv);
G(glGetError);
G(glGetProgramiv);
G(glGetProgramInfoLog);
G(glGetShaderInfoLog);

#define S 64

static const double keep_right[4] = {1, 0, 0, 0};

static void
quad(float x0, float y0, float x1, float y1)
{
   glBegin_(GL_QUADS);
   glVertex2f_(x0, y0);
   glVertex2f_(x1, y0);
   glVertex2f_(x1, y1);
   glVertex2f_(x0, y1);
   glEnd_();
}

static void
reset(void)
{
   for (unsigned i = 0; i < 8; i++)
      glDisable_(GL_CLIP_PLANE0 + i);
   glGetError_();
   glMatrixMode_(GL_PROJECTION);
   glLoadIdentity_();
   glMatrixMode_(GL_MODELVIEW);
   glLoadIdentity_();
   glUseProgram_(0);
   glBindFramebuffer_(GL_FRAMEBUFFER, 0);
   glViewport_(0, 0, S, S);
   glClearColor_(0, 0, 0, 0);
   glClear_(GL_COLOR_BUFFER_BIT);
   glColor3f_(1, 1, 1);
}

/* white pixels in the left and right halves of rows y0..y1 */
static void
count(int y0, int y1, int *left, int *right)
{
   static unsigned char px[S * S * 4];

   glFinish_();
   glReadPixels_(0, 0, S, S, GL_RGBA, GL_UNSIGNED_BYTE, px);
   *left = *right = 0;
   for (int y = y0; y <= y1; y++)
      for (int x = 0; x < S; x++)
         if (px[4 * (y * S + x)] > 128)
            (x < S / 2 ? (*left)++ : (*right)++);
}

static GLuint
program(const char *vs)
{
   const char *fs = "#version 120\nvoid main() { gl_FragColor = vec4(1.0); }\n";
   GLuint p = glCreateProgram_();
   GLuint v = glCreateShader_(GL_VERTEX_SHADER), f = glCreateShader_(GL_FRAGMENT_SHADER);

   glShaderSource_(v, 1, &vs, NULL);
   glCompileShader_(v);
   glShaderSource_(f, 1, &fs, NULL);
   glCompileShader_(f);
   glAttachShader_(p, v);
   glAttachShader_(p, f);
   glLinkProgram_(p);
   GLint ok = 0;
   glGetProgramiv_(p, GL_LINK_STATUS, &ok);
   if (!ok) {
      char log[1024] = "";
      glGetProgramInfoLog_(p, sizeof(log), NULL, log);
      printf("link failed: %s\n", log);
      glGetShaderInfoLog_(v, sizeof(log), NULL, log);
      printf("vs: %s\n", log);
   }
   return p;
}

static int
run(const char *name)
{
   int l, r, y0 = 0, y1 = S - 1, want_r = S / 2 * S, ok;

   reset();
   if (!strcmp(name, "quad")) {
      glClipPlane_(GL_CLIP_PLANE0, keep_right);
      glEnable_(GL_CLIP_PLANE0);
      quad(-1, -1, 1, 1);
   } else if (!strcmp(name, "strip")) {
      static const GLfloat v[] = {-1, -1, 1, -1, -1, 1, 1, 1};

      glClipPlane_(GL_CLIP_PLANE0, keep_right);
      glEnable_(GL_CLIP_PLANE0);
      glVertexPointer_(2, GL_FLOAT, 0, v);
      glEnableClientState_(GL_VERTEX_ARRAY);
      glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
      glDisableClientState_(GL_VERTEX_ARRAY);
   } else if (!strcmp(name, "ortho")) {
      /* x' = x - 32 >= 0 in the translated space: keeps x >= 32 */
      glMatrixMode_(GL_PROJECTION);
      glOrtho_(0, S, 0, S, -1, 1);
      glMatrixMode_(GL_MODELVIEW);
      glTranslatef_(S / 2, 0, 0);
      glClipPlane_(GL_CLIP_PLANE0, keep_right);
      glLoadIdentity_();
      glEnable_(GL_CLIP_PLANE0);
      quad(0, 0, S, S);
   } else if (!strcmp(name, "line")) {
      glClipPlane_(GL_CLIP_PLANE0, keep_right);
      glEnable_(GL_CLIP_PLANE0);
      glBegin_(GL_LINES);
      glVertex2f_(-1, 0.01f);
      glVertex2f_(1, 0.01f);
      glEnd_();
      y0 = S / 2 - 2, y1 = S / 2 + 2, want_r = S / 2;
   } else if (!strcmp(name, "points")) {
      glClipPlane_(GL_CLIP_PLANE0, keep_right);
      glEnable_(GL_CLIP_PLANE0);
      glBegin_(GL_POINTS);
      for (int x = 0; x < S; x += 2)
         glVertex2f_((x + 0.5f) / S * 2 - 1, (S / 2 + 0.5f) / S * 2 - 1);
      glEnd_();
      y0 = y1 = S / 2, want_r = S / 4;
   } else if (!strcmp(name, "plane5")) {
      glClipPlane_(GL_CLIP_PLANE5, keep_right);
      glEnable_(GL_CLIP_PLANE5);
      quad(-1, -1, 1, 1);
   } else if (!strcmp(name, "plane6") || !strcmp(name, "plane7")) {
      GLint max = 0;
      GLenum pl = GL_CLIP_PLANE0 + name[5] - '0';

      glGetIntegerv_(GL_MAX_CLIP_PLANES, &max);
      if (max < 8) {
         printf("%-7s SKIP: GL_MAX_CLIP_PLANES %d\n", name, max);
         return 0;
      }
      glClipPlane_(pl, keep_right);
      glEnable_(pl);
      quad(-1, -1, 1, 1);
   } else if (!strcmp(name, "clip7")) {
      GLint max = 0;

      glGetIntegerv_(GL_MAX_CLIP_DISTANCES, &max);
      if (max < 8) {
         printf("%-7s SKIP: GL_MAX_CLIP_DISTANCES %d\n", name, max);
         return 0;
      }
      GLuint p = program("#version 130\n"
                         "out float gl_ClipDistance[8];\n"
                         "void main() {\n"
                         "  gl_Position = gl_Vertex;\n"
                         "  for (int i = 0; i < 7; i++)\n"
                         "    gl_ClipDistance[i] = 1.0;\n"
                         "  gl_ClipDistance[7] = gl_Vertex.x;\n"
                         "}\n");

      glEnable_(GL_CLIP_DISTANCE0 + 7);
      glUseProgram_(p);
      quad(-1, -1, 1, 1);
      glDisable_(GL_CLIP_DISTANCE0 + 7);
   } else if (!strcmp(name, "psize")) {
      static const double all[4] = {0, 0, 0, 4};

      glClipPlane_(GL_CLIP_PLANE0, all);
      glEnable_(GL_CLIP_PLANE0);
      glBegin_(GL_POINTS);
      for (int x = 0; x < S; x += 8)
         glVertex2f_((x + 0.5f) / S * 2 - 1, (S / 2 + 0.5f) / S * 2 - 1);
      glEnd_();
      count(0, S - 1, &l, &r);
      ok = l == 4 && r == 4;
      printf("%-7s %s: %d and %d white (want 4 and 4: 1 px each)\n", name,
             ok ? "PASS" : "FAIL", l, r);
      return !ok;
   } else if (!strcmp(name, "fbo")) {
      GLuint tex, fb;

      glGenTextures_(1, &tex);
      glBindTexture_(GL_TEXTURE_2D, tex);
      glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, S, S, 0, GL_RGBA,
                    GL_UNSIGNED_BYTE, NULL);
      glGenFramebuffers_(1, &fb);
      glBindFramebuffer_(GL_FRAMEBUFFER, fb);
      glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_TEXTURE_2D, tex, 0);
      glClear_(GL_COLOR_BUFFER_BIT);
      glClipPlane_(GL_CLIP_PLANE0, keep_right);
      glEnable_(GL_CLIP_PLANE0);
      quad(-1, -1, 1, 1);
   } else if (!strcmp(name, "glsl")) {
      GLuint p = program("#version 120\n"
                         "void main() {\n"
                         "  gl_Position = gl_Vertex;\n"
                         "  gl_ClipVertex = gl_Vertex;\n"
                         "}\n");

      glClipPlane_(GL_CLIP_PLANE0, keep_right);
      glEnable_(GL_CLIP_PLANE0);
      glUseProgram_(p);
      quad(-1, -1, 1, 1);
   } else {
      fprintf(stderr, "no case %s\n", name);
      return 2;
   }
   count(y0, y1, &l, &r);
   /* the edge column may go either way */
   ok = l == 0 && r >= want_r - (y1 - y0 + 1) && r <= want_r;
   printf("%-7s %s: left %d white (want 0), right %d (want %d)\n", name,
          ok ? "PASS" : "FAIL", l, r, want_r);
   return !ok;
}

int
main(int argc, char **argv)
{
   static const char *all[] = {"quad", "strip", "ortho", "line", "points",
                               "plane5", "plane6", "plane7", "clip7",
                               "psize", "fbo", "glsl"};
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
   EGLint sa[] = {EGL_WIDTH, S, EGL_HEIGHT, S, EGL_NONE};
   EGLSurface s = createpbuffer(d, cfg, sa);
   EGLContext c = createcontext(d, cfg, EGL_NO_CONTEXT, NULL);
   if (!s || !c || !makecurrent(d, s, s, c))
      return 2;
   GET(glClearColor);
   GET(glClear);
   GET(glViewport);
   GET(glEnable);
   GET(glDisable);
   GET(glClipPlane);
   GET(glMatrixMode);
   GET(glLoadIdentity);
   GET(glOrtho);
   GET(glTranslatef);
   GET(glReadPixels);
   GET(glGenTextures);
   GET(glBindTexture);
   GET(glTexImage2D);
   GET(glGenFramebuffers);
   GET(glBindFramebuffer);
   GET(glFramebufferTexture2D);
   GET(glCreateShader);
   GET(glShaderSource);
   GET(glCompileShader);
   GET(glCreateProgram);
   GET(glAttachShader);
   GET(glLinkProgram);
   GET(glUseProgram);
   GET(glBegin);
   GET(glEnd);
   GET(glVertex2f);
   GET(glColor3f);
   GET(glGetString);
   GET(glVertexPointer);
   GET(glEnableClientState);
   GET(glDisableClientState);
   GET(glDrawArrays);
   GET(glFinish);
   GET(glGetIntegerv);
   GET(glGetError);
   GET(glGetProgramiv);
   GET(glGetProgramInfoLog);
   GET(glGetShaderInfoLog);
   printf("renderer: %s\n", glGetString_(GL_RENDERER));

   int fails = 0;
   if (argc > 1)
      for (int i = 1; i < argc; i++)
         fails += run(argv[i]);
   else
      for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++)
         fails += run(all[i]);
   printf("%s: %d failed\n", fails ? "FAIL" : "PASS", fails);
   return !!fails;
}
