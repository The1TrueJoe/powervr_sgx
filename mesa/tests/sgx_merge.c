/* sgx_merge: the draws one call merges into one (immediate mode's
 * glBegin/glEnd blocks between state changes, glMultiDrawArrays,
 * glMultiDrawElements), each primitive type in its own band of a pbuffer,
 * smooth and flat. Prints a hash and the lit pixels of each band: the same
 * with SGX_DEBUG=nomerge (the draws one by one) as without, and none empty.
 *
 * Surfaceless EGL, desktop GL (compatibility) on a 256x256 pbuffer.
 * zig cc -target x86_64-linux-gnu.2.36 -O2 -I<mesa include> -o sgx_merge sgx_merge.c -ldl
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EGLF(X)                                                                                    \
   X(EGLDisplay, eglGetPlatformDisplay, (EGLenum, void *, const EGLAttrib *))                     \
   X(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *))                                 \
   X(EGLBoolean, eglBindAPI, (EGLenum))                                                            \
   X(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))    \
   X(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *))           \
   X(EGLSurface, eglCreatePbufferSurface, (EGLDisplay, EGLConfig, const EGLint *))                \
   X(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))                \
   X(void *, eglGetProcAddress, (const char *))
#define GLF(X)                                                                                     \
   X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))                                           \
   X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                     \
   X(void, glClear, (GLbitfield))                                                                  \
   X(void, glMatrixMode, (GLenum))                                                                 \
   X(void, glLoadIdentity, (void))                                                                 \
   X(void, glOrtho, (GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble))                  \
   X(void, glShadeModel, (GLenum))                                                                 \
   X(void, glBegin, (GLenum))                                                                      \
   X(void, glEnd, (void))                                                                          \
   X(void, glVertex2f, (GLfloat, GLfloat))                                                         \
   X(void, glColor3f, (GLfloat, GLfloat, GLfloat))                                                 \
   X(void, glEnableClientState, (GLenum))                                                          \
   X(void, glDisableClientState, (GLenum))                                                         \
   X(void, glVertexPointer, (GLint, GLenum, GLsizei, const void *))                                \
   X(void, glColorPointer, (GLint, GLenum, GLsizei, const void *))                                 \
   X(void, glMultiDrawArrays, (GLenum, const GLint *, const GLsizei *, GLsizei))                  \
   X(void, glMultiDrawElements, (GLenum, const GLsizei *, GLenum, const void *const *, GLsizei)) \
   X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))                 \
   X(void, glFinish, (void))                                                                       \
   X(GLenum, glGetError, (void))
#define DECL(r, n, a) static r(*p_##n) a;
EGLF(DECL)
GLF(DECL)

enum { W = 256, H = 256, BAND = 18 }; /* 14 bands */

/* a vertex of primitive k of a band, at angle i of n around its centre */
static void
vtx(int band, int k, int i, int n)
{
   static const float c[8][2] = {{0, 0}, {1, 0}, {0.866f, 0.5f}, {0.5f, 0.866f},
                                 {0, 1}, {-0.5f, 0.866f}, {-0.866f, 0.5f}, {-1, 0}};
   float cx = 16 + k * 30, cy = band * BAND + BAND / 2;
   int j = (i * 7 / (n > 1 ? n : 1)) % 8;

   p_glColor3f((i % 3 == 0), (i % 3 == 1), (i % 3 == 2) * 0.5f + k * 0.06f);
   p_glVertex2f(cx + c[j][0] * 12, cy + c[j][1] * 9 - 4 + (i & 1) * 2);
}

/* count blocks of n vertices of mode in one batch (no state change between) */
static void
batch(int band, GLenum mode, int count, int n)
{
   for (int k = 0; k < count; k++) {
      p_glBegin(mode);
      for (int i = 0; i < n; i++)
         vtx(band, k, i, n);
      p_glEnd();
   }
}

int
main(void)
{
   void *e = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
   void *g = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_GLOBAL);
   if (!g)
      g = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
   if (!e || !g)
      return printf("no libEGL/libOpenGL\n"), 1;
#define LE(r, n, a) p_##n = dlsym(e, #n);
   EGLF(LE)
#define LG(r, n, a)                                                                                \
   p_##n = dlsym(g, #n);                                                                           \
   if (!p_##n)                                                                                     \
      p_##n = p_eglGetProcAddress(#n);
   GLF(LG)

   EGLDisplay d = p_eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   if (!p_eglInitialize(d, NULL, NULL))
      return printf("no EGL\n"), 1;
   p_eglBindAPI(EGL_OPENGL_API);
   EGLint attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
   EGLConfig cfg;
   EGLint n = 0;
   p_eglChooseConfig(d, attr, &cfg, 1, &n);
   if (!n)
      return printf("no config\n"), 1;
   EGLint pa[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
   EGLSurface s = p_eglCreatePbufferSurface(d, cfg, pa);
   EGLContext ctx = p_eglCreateContext(d, cfg, EGL_NO_CONTEXT, NULL);
   if (!ctx || !p_eglMakeCurrent(d, s, s, ctx))
      return printf("no context\n"), 1;

   p_glViewport(0, 0, W, H);
   p_glMatrixMode(GL_PROJECTION);
   p_glLoadIdentity();
   p_glOrtho(0, W, 0, H, -1, 1);
   p_glMatrixMode(GL_MODELVIEW);
   p_glLoadIdentity();
   p_glClearColor(0, 0, 0, 1);
   p_glClear(GL_COLOR_BUFFER_BIT);

   static const char *names[] = {
      "polygons",     "polygons flat",   "fans",         "strips odd",   "strips even",
      "quads",        "quad strips",     "triangles",    "line strips",  "line loops",
      "lines",        "multi arrays",    "multi elems",  "mixed sizes",
   };
   batch(0, GL_POLYGON, 8, 6);
   p_glShadeModel(GL_FLAT);
   batch(1, GL_POLYGON, 8, 5);
   p_glShadeModel(GL_SMOOTH);
   batch(2, GL_TRIANGLE_FAN, 8, 5);
   batch(3, GL_TRIANGLE_STRIP, 8, 5);
   batch(4, GL_TRIANGLE_STRIP, 8, 6);
   batch(5, GL_QUADS, 8, 6); /* 4 drawn, 2 trimmed */
   batch(6, GL_QUAD_STRIP, 8, 6);
   batch(7, GL_TRIANGLES, 8, 7); /* 6 drawn, 1 trimmed */
   batch(8, GL_LINE_STRIP, 8, 5);
   batch(9, GL_LINE_LOOP, 8, 4);
   batch(10, GL_LINES, 8, 5); /* 4 drawn, 1 trimmed */

   /* client arrays: glMultiDrawArrays of fans, glMultiDrawElements of strips */
   static GLfloat pos[64][2], col[64][3];
   for (int k = 0; k < 8; k++)
      for (int i = 0; i < 5; i++) {
         int j = k * 5 + i;
         pos[j][0] = 16 + k * 30 + (i % 2 ? 10 : -10) + (i > 2 ? 3 : 0);
         pos[j][1] = 11 * BAND + 2 + i * 4;
         col[j][0] = i * 0.2f, col[j][1] = k * 0.1f, col[j][2] = 1 - i * 0.2f;
      }
   p_glEnableClientState(GL_VERTEX_ARRAY);
   p_glEnableClientState(GL_COLOR_ARRAY);
   p_glVertexPointer(2, GL_FLOAT, 0, pos);
   p_glColorPointer(3, GL_FLOAT, 0, col);
   GLint first[8];
   GLsizei cnt[8];
   for (int k = 0; k < 8; k++)
      first[k] = k * 5, cnt[k] = 5;
   p_glMultiDrawArrays(GL_TRIANGLE_FAN, first, cnt, 8);

   static GLfloat pos2[40][2];
   for (int j = 0; j < 40; j++)
      pos2[j][0] = pos[j][0], pos2[j][1] = pos[j][1] + BAND;
   p_glVertexPointer(2, GL_FLOAT, 0, pos2);
   static GLushort idx[8][5];
   const void *ip[8];
   for (int k = 0; k < 8; k++) {
      for (int i = 0; i < 5; i++)
         idx[k][i] = (GLushort)(k * 5 + (4 - i));
      ip[k] = idx[k];
   }
   p_glMultiDrawElements(GL_TRIANGLE_STRIP, cnt, GL_UNSIGNED_SHORT, ip, 8);
   p_glDisableClientState(GL_COLOR_ARRAY);
   p_glDisableClientState(GL_VERTEX_ARRAY);

   /* polygons of 3 to 10 vertices in one batch */
   for (int k = 0; k < 8; k++) {
      p_glBegin(GL_POLYGON);
      for (int i = 0; i < 3 + k; i++)
         vtx(13, k, i, 3 + k);
      p_glEnd();
   }

   static uint8_t px[W * H * 4];
   p_glFinish();
   p_glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
   GLenum err = p_glGetError();
   uint32_t all = 2166136261u;
   int empty = 0;
   for (int b = 0; b < 14; b++) {
      uint32_t h = 2166136261u;
      int lit = 0;
      for (int y = b * BAND; y < (b + 1) * BAND; y++)
         for (int x = 0; x < W * 4; x++) {
            uint8_t v = px[y * W * 4 + x];
            h = (h ^ v) * 16777619u;
            all = (all ^ v) * 16777619u;
            lit += x % 4 < 3 && v;
         }
      empty += !lit;
      printf("%-14s %08x %6d lit\n", names[b], h, lit);
   }
   printf("all %08x, %d empty bands, err 0x%x\n", all, empty, err);
   return empty || err;
}
