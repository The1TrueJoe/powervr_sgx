/* sgx_gl1bench: a frame drawn the way GoldSrc's OpenGL renderer (Half-Life,
 * Counter-Strike 1.6) draws one: desktop GL 1.x in immediate mode, every
 * world face its own glBegin(GL_POLYGON) with a base texture on unit 0 and a
 * lightmap on unit 1 (GL_ARB_multitexture), the textures switched every few
 * faces, a few lightmap blocks updated with glTexSubImage2D (dynamic
 * lights), then alpha-tested sprites. Prints frames per second; with
 * SGX_DEBUG=stats the driver says where the time goes.  Each switch takes
 * one part away, to see what costs what:
 *
 *   sgx_gl1bench [-f faces] [-v verts/face] [-n frames] [-b faces/bind]
 *                [-l lightmap updates] [-s sprites] [-1 (one texture unit)]
 *                [-t (GL_TRIANGLE_FAN, not GL_POLYGON)] [-a (glDrawArrays from
 *                client arrays, one call a face)] [-w width -h height]
 *
 * Surfaceless EGL, desktop GL (compatibility) on a pbuffer.
 * zig cc -target x86_64-linux-gnu.2.36 -O2 -I<mesa include> -o sgx_gl1bench sgx_gl1bench.c -ldl
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef GL_TEXTURE0_ARB
#define GL_TEXTURE0_ARB 0x84C0
#define GL_TEXTURE1_ARB 0x84C1
#endif

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
   X(const GLubyte *, glGetString, (GLenum))                                                        \
   X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))                                           \
   X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                     \
   X(void, glClear, (GLbitfield))                                                                  \
   X(void, glEnable, (GLenum))                                                                     \
   X(void, glDisable, (GLenum))                                                                    \
   X(void, glMatrixMode, (GLenum))                                                                 \
   X(void, glLoadIdentity, (void))                                                                 \
   X(void, glFrustum, (GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble))                \
   X(void, glGenTextures, (GLsizei, GLuint *))                                                     \
   X(void, glBindTexture, (GLenum, GLuint))                                                        \
   X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
   X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
   X(void, glTexParameteri, (GLenum, GLenum, GLint))                                               \
   X(void, glTexEnvi, (GLenum, GLenum, GLint))                                                     \
   X(void, glBegin, (GLenum))                                                                      \
   X(void, glEnd, (void))                                                                          \
   X(void, glVertex3f, (GLfloat, GLfloat, GLfloat))                                                \
   X(void, glTexCoord2f, (GLfloat, GLfloat))                                                       \
   X(void, glColor4f, (GLfloat, GLfloat, GLfloat, GLfloat))                                        \
   X(void, glAlphaFunc, (GLenum, GLclampf))                                                        \
   X(void, glBlendFunc, (GLenum, GLenum))                                                          \
   X(void, glDepthFunc, (GLenum))                                                                  \
   X(void, glFinish, (void))                                                                       \
   X(void, glEnableClientState, (GLenum))                                                          \
   X(void, glDisableClientState, (GLenum))                                                         \
   X(void, glVertexPointer, (GLint, GLenum, GLsizei, const void *))                                \
   X(void, glTexCoordPointer, (GLint, GLenum, GLsizei, const void *))                              \
   X(void, glDrawArrays, (GLenum, GLint, GLsizei))                                                 \
   X(GLenum, glGetError, (void))                                                                   \
   X(void, glActiveTextureARB, (GLenum))                                                           \
   X(void, glClientActiveTextureARB, (GLenum))                                                     \
   X(void, glMultiTexCoord2fARB, (GLenum, GLfloat, GLfloat))
#define DECL(r, n, a) static r(*p_##n) a;
EGLF(DECL)
GLF(DECL)

static double now(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec + t.tv_nsec * 1e-9;
}

static unsigned rnd_state = 12345;
static unsigned rnd(void)
{
   rnd_state = rnd_state * 1103515245u + 12345u;
   return rnd_state >> 8;
}

int main(int argc, char **argv)
{
   int faces = 1500, verts = 5, frames = 100, per_bind = 6, lm_updates = 8, sprites = 100;
   int one_unit = 0, fans = 0, arrays = 0, w = 640, h = 480, c;

   while ((c = getopt(argc, argv, "f:v:n:b:l:s:1taw:h:")) != -1) {
      switch (c) {
      case 'f': faces = atoi(optarg); break;
      case 'v': verts = atoi(optarg); break;
      case 'n': frames = atoi(optarg); break;
      case 'b': per_bind = atoi(optarg); break;
      case 'l': lm_updates = atoi(optarg); break;
      case 's': sprites = atoi(optarg); break;
      case '1': one_unit = 1; break;
      case 't': fans = 1; break;
      case 'a': arrays = 1; break;
      case 'w': w = atoi(optarg); break;
      case 'h': h = atoi(optarg); break;
      default: return 2;
      }
   }
   if (verts < 3 || verts > 16 || per_bind < 1)
      return 2;

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
                    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24,
                    EGL_NONE};
   EGLConfig cfg;
   EGLint n = 0;
   p_eglChooseConfig(d, attr, &cfg, 1, &n);
   if (!n)
      return printf("no config\n"), 1;
   EGLint pa[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE};
   EGLSurface s = p_eglCreatePbufferSurface(d, cfg, pa);
   EGLContext ctx = p_eglCreateContext(d, cfg, EGL_NO_CONTEXT, NULL);
   if (!ctx || !p_eglMakeCurrent(d, s, s, ctx))
      return printf("no context\n"), 1;
   printf("%s / %s\n", p_glGetString(GL_RENDERER), p_glGetString(GL_VERSION));
   printf("faces %d x %d verts, bind every %d, %d lightmap updates, %d sprites, %s, %s%s\n",
          faces, verts, per_bind, lm_updates, sprites, one_unit ? "1 unit" : "2 units",
          fans ? "fans" : "polygons", arrays ? ", glDrawArrays" : "");

   /* 32 base textures 64x64 and 16 lightmap blocks 128x128, as GoldSrc */
   enum { NBASE = 32, NLM = 16 };
   GLuint base[NBASE], lm[NLM];
   static unsigned char pix[128 * 128 * 4], sub[16 * 16 * 4];
   p_glGenTextures(NBASE, base);
   p_glGenTextures(NLM, lm);
   for (int i = 0; i < NBASE; i++) {
      for (int j = 0; j < 64 * 64 * 4; j++)
         pix[j] = (unsigned char)(rnd() | 0x40);
      p_glBindTexture(GL_TEXTURE_2D, base[i]);
      p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
      p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   }
   for (int i = 0; i < NLM; i++) {
      for (int j = 0; j < 128 * 128 * 4; j++)
         pix[j] = (unsigned char)(rnd() | 0x80);
      p_glBindTexture(GL_TEXTURE_2D, lm[i]);
      p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
      p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   }

   /* the faces: small polygons scattered in front of the camera */
   float *fv = malloc(sizeof(float) * faces * verts * 7);
   for (int f = 0; f < faces; f++) {
      float cx = (rnd() % 2000) / 100.0f - 10.0f, cy = (rnd() % 1500) / 100.0f - 7.5f;
      float cz = -5.0f - (rnd() % 2000) / 100.0f, r = 0.4f + (rnd() % 100) / 100.0f;
      float lu = (rnd() % 112) / 128.0f, lv = (rnd() % 112) / 128.0f;
      for (int k = 0; k < verts; k++) {
         float a = k * 6.2831853f / verts, ca = __builtin_cosf(a), sa = __builtin_sinf(a);
         float *v = &fv[(f * verts + k) * 7];
         v[0] = cx + r * ca, v[1] = cy + r * sa, v[2] = cz;
         v[3] = 0.5f + 0.5f * ca, v[4] = 0.5f + 0.5f * sa;
         v[5] = lu + (0.5f + 0.5f * ca) / 8.0f, v[6] = lv + (0.5f + 0.5f * sa) / 8.0f;
      }
   }

   p_glViewport(0, 0, w, h);
   p_glMatrixMode(GL_PROJECTION);
   p_glLoadIdentity();
   p_glFrustum(-0.8, 0.8, -0.6, 0.6, 1.0, 100.0);
   p_glMatrixMode(GL_MODELVIEW);
   p_glLoadIdentity();
   p_glEnable(GL_DEPTH_TEST);
   p_glDepthFunc(GL_LEQUAL);
   p_glClearColor(0.1f, 0.1f, 0.2f, 1.0f);

   double t0 = 0, t_world = 0, t_lm = 0, t_spr = 0, t_fin = 0;
   int warm = frames > 10 ? 5 : 0;
   for (int fr = 0; fr < frames + warm; fr++) {
      if (fr == warm)
         t0 = now(), t_world = t_lm = t_spr = t_fin = 0;
      double a = now();
      p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      /* lightmap updates first (GoldSrc uploads dirty blocks as it draws) */
      for (int i = 0; i < lm_updates; i++) {
         for (int j = 0; j < 16 * 16 * 4; j++)
            sub[j] = (unsigned char)(fr * 7 + i * 13 + j);
         if (!one_unit)
            p_glActiveTextureARB(GL_TEXTURE1_ARB);
         p_glBindTexture(GL_TEXTURE_2D, lm[i % NLM]);
         p_glTexSubImage2D(GL_TEXTURE_2D, 0, (i * 16) % 112, (i * 48) % 112, 16, 16, GL_RGBA,
                           GL_UNSIGNED_BYTE, sub);
      }
      double b = now();
      /* the world: unit 0 base REPLACE, unit 1 lightmap MODULATE */
      p_glActiveTextureARB(GL_TEXTURE0_ARB);
      p_glEnable(GL_TEXTURE_2D);
      p_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
      if (!one_unit) {
         p_glActiveTextureARB(GL_TEXTURE1_ARB);
         p_glEnable(GL_TEXTURE_2D);
         p_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
      }
      if (arrays) {
         p_glEnableClientState(GL_VERTEX_ARRAY);
         p_glClientActiveTextureARB(GL_TEXTURE0_ARB);
         p_glEnableClientState(GL_TEXTURE_COORD_ARRAY);
         p_glTexCoordPointer(2, GL_FLOAT, 7 * sizeof(float), fv + 3);
         if (!one_unit) {
            p_glClientActiveTextureARB(GL_TEXTURE1_ARB);
            p_glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            p_glTexCoordPointer(2, GL_FLOAT, 7 * sizeof(float), fv + 5);
         }
         p_glVertexPointer(3, GL_FLOAT, 7 * sizeof(float), fv);
      }
      for (int f = 0; f < faces; f++) {
         if (f % per_bind == 0) {
            p_glActiveTextureARB(GL_TEXTURE0_ARB);
            p_glBindTexture(GL_TEXTURE_2D, base[(f / per_bind) % NBASE]);
            if (!one_unit) {
               p_glActiveTextureARB(GL_TEXTURE1_ARB);
               p_glBindTexture(GL_TEXTURE_2D, lm[(f / per_bind / 3) % NLM]);
            }
         }
         if (arrays) {
            p_glDrawArrays(fans ? GL_TRIANGLE_FAN : GL_POLYGON, f * verts, verts);
            continue;
         }
         p_glBegin(fans ? GL_TRIANGLE_FAN : GL_POLYGON);
         for (int k = 0; k < verts; k++) {
            const float *v = &fv[(f * verts + k) * 7];
            if (one_unit) {
               p_glTexCoord2f(v[3], v[4]);
            } else {
               p_glMultiTexCoord2fARB(GL_TEXTURE0_ARB, v[3], v[4]);
               p_glMultiTexCoord2fARB(GL_TEXTURE1_ARB, v[5], v[6]);
            }
            p_glVertex3f(v[0], v[1], v[2]);
         }
         p_glEnd();
      }
      if (arrays) {
         p_glDisableClientState(GL_VERTEX_ARRAY);
         p_glClientActiveTextureARB(GL_TEXTURE1_ARB);
         p_glDisableClientState(GL_TEXTURE_COORD_ARRAY);
         p_glClientActiveTextureARB(GL_TEXTURE0_ARB);
         p_glDisableClientState(GL_TEXTURE_COORD_ARRAY);
      }
      if (!one_unit) {
         p_glActiveTextureARB(GL_TEXTURE1_ARB);
         p_glDisable(GL_TEXTURE_2D);
         p_glActiveTextureARB(GL_TEXTURE0_ARB);
      }
      double c2 = now();
      /* sprites and models: alpha-tested, blended quads, one texture */
      p_glEnable(GL_ALPHA_TEST);
      p_glAlphaFunc(GL_GREATER, 0.25f);
      p_glEnable(GL_BLEND);
      p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      p_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
      for (int i = 0; i < sprites; i++) {
         float x = ((i * 37) % 200) / 10.0f - 10.0f, y = ((i * 53) % 150) / 10.0f - 7.5f;
         p_glBindTexture(GL_TEXTURE_2D, base[i % NBASE]);
         p_glColor4f(1, 1, 1, 0.8f);
         p_glBegin(GL_QUADS);
         p_glTexCoord2f(0, 0), p_glVertex3f(x, y, -6);
         p_glTexCoord2f(1, 0), p_glVertex3f(x + 0.5f, y, -6);
         p_glTexCoord2f(1, 1), p_glVertex3f(x + 0.5f, y + 0.5f, -6);
         p_glTexCoord2f(0, 1), p_glVertex3f(x, y + 0.5f, -6);
         p_glEnd();
      }
      p_glDisable(GL_BLEND);
      p_glDisable(GL_ALPHA_TEST);
      p_glColor4f(1, 1, 1, 1);
      double d2 = now();
      p_glFinish();
      double e2 = now();
      t_lm += b - a, t_world += c2 - b, t_spr += d2 - c2, t_fin += e2 - d2;
   }
   double t = now() - t0;
   printf("%.1f fps (%.1f ms a frame: lightmaps %.2f, world %.2f, sprites %.2f, finish %.2f) err 0x%x\n",
          frames / t, 1000 * t / frames, 1000 * t_lm / frames, 1000 * t_world / frames,
          1000 * t_spr / frames, 1000 * t_fin / frames, p_glGetError());
   return 0;
}
