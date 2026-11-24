/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */
/* sgx_submitbench: the per-submit CPU cost of the kernel driver. Each
 * iteration draws one small triangle into a small render target and flushes
 * it, so each one is a render job of its own (one SUBMIT_JOBS); every -k
 * iterations a glFinish keeps the queue short. Prints submits per second and
 * the user and system CPU time per submit (getrusage), the system part being
 * the kernel's:
 *
 *   sgx_submitbench [-n iterations] [-k finish every] [-s size] [-t targets]
 *
 * -t cycles that many render targets (FBOs), one per iteration, as a game
 * alternates between its framebuffer and render-to-texture targets.
 *
 * Surfaceless EGL, OpenGL ES 2.0 on a pbuffer.
 * zig cc -target x86_64-linux-gnu.2.36 -O2 -I<mesa include> -o sgx_submitbench sgx_submitbench.c -ldl
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

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
   X(void, glGenTextures, (GLsizei, GLuint *))                                                     \
   X(void, glBindTexture, (GLenum, GLuint))                                                        \
   X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
   X(void, glTexParameteri, (GLenum, GLenum, GLint))                                               \
   X(void, glGenFramebuffers, (GLsizei, GLuint *))                                                 \
   X(void, glBindFramebuffer, (GLenum, GLuint))                                                    \
   X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                        \
   X(GLenum, glCheckFramebufferStatus, (GLenum))                                                   \
   X(GLuint, glCreateShader, (GLenum))                                                             \
   X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))                \
   X(void, glCompileShader, (GLuint))                                                              \
   X(GLuint, glCreateProgram, (void))                                                              \
   X(void, glAttachShader, (GLuint, GLuint))                                                       \
   X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *))                                 \
   X(void, glLinkProgram, (GLuint))                                                                \
   X(void, glUseProgram, (GLuint))                                                                 \
   X(void, glGetProgramiv, (GLuint, GLenum, GLint *))                                              \
   X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))       \
   X(void, glEnableVertexAttribArray, (GLuint))                                                    \
   X(void, glDrawArrays, (GLenum, GLint, GLsizei))                                                 \
   X(void, glFlush, (void))                                                                        \
   X(void, glFinish, (void))                                                                       \
   X(GLenum, glGetError, (void))

#define DECL(r, n, a) static r(*p_##n) a;
EGLF(DECL)
GLF(DECL)

static double now(void)
{
   struct timespec t;

   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec + t.tv_nsec * 1e-9;
}

static double cpu(int who_sys)
{
   struct rusage u;

   getrusage(RUSAGE_SELF, &u);
   const struct timeval *t = who_sys ? &u.ru_stime : &u.ru_utime;
   return t->tv_sec + t->tv_usec * 1e-6;
}

int main(int argc, char **argv)
{
   int n = 20000, k = 32, size = 64, targets = 1, o;

   while ((o = getopt(argc, argv, "n:k:s:t:")) != -1) {
      switch (o) {
      case 'n': n = atoi(optarg); break;
      case 'k': k = atoi(optarg); break;
      case 's': size = atoi(optarg); break;
      case 't': targets = atoi(optarg); break;
      default:
         fprintf(stderr, "usage: %s [-n iterations] [-k finish every] [-s size] [-t targets]\n",
                 argv[0]);
         return 2;
      }
   }
   if (targets < 1 || targets > 16 || k < 1)
      return 2;

   void *egl = dlopen("libEGL.so.1", RTLD_NOW), *gles = dlopen("libGLESv2.so.2", RTLD_NOW);
   if (!egl || !gles) {
      fprintf(stderr, "dlopen: %s\n", dlerror());
      return 1;
   }
#define LOADE(r, n, a) p_##n = (r(*) a)dlsym(egl, #n);
   EGLF(LOADE)
#define LOADG(r, n, a) p_##n = (r(*) a)dlsym(gles, #n);
   GLF(LOADG)

   EGLDisplay d = p_eglGetPlatformDisplay(0x31DD /* EGL_PLATFORM_SURFACELESS_MESA */,
                                        EGL_DEFAULT_DISPLAY, NULL);
   EGLint maj, min, nc;
   EGLConfig cfg;
   const EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                        EGL_RED_SIZE, 8, EGL_NONE};
   const EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
   const EGLint pa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};

   if (!p_eglInitialize(d, &maj, &min) || !p_eglBindAPI(EGL_OPENGL_ES_API) ||
       !p_eglChooseConfig(d, ca, &cfg, 1, &nc) || nc < 1) {
      fprintf(stderr, "EGL setup failed\n");
      return 1;
   }
   EGLContext c = p_eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
   EGLSurface s = p_eglCreatePbufferSurface(d, cfg, pa);
   if (!p_eglMakeCurrent(d, s, s, c)) {
      fprintf(stderr, "eglMakeCurrent failed\n");
      return 1;
   }

   GLuint tex[16], fbo[16];
   p_glGenTextures(targets, tex);
   p_glGenFramebuffers(targets, fbo);
   for (int i = 0; i < targets; i++) {
      p_glBindTexture(GL_TEXTURE_2D, tex[i]);
      p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      p_glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
      p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[i], 0);
      if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
         fprintf(stderr, "FBO %d incomplete\n", i);
         return 1;
      }
   }

   static const char *vs = "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }";
   static const char *fs = "precision mediump float; uniform vec4 c; void main() { gl_FragColor = vec4(0.2, 0.6, 0.9, 1.0); }";
   GLuint v = p_glCreateShader(GL_VERTEX_SHADER), f = p_glCreateShader(GL_FRAGMENT_SHADER);
   p_glShaderSource(v, 1, &vs, NULL);
   p_glCompileShader(v);
   p_glShaderSource(f, 1, &fs, NULL);
   p_glCompileShader(f);
   GLuint p = p_glCreateProgram();
   GLint ok;
   p_glAttachShader(p, v);
   p_glAttachShader(p, f);
   p_glBindAttribLocation(p, 0, "p");
   p_glLinkProgram(p);
   p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok) {
      fprintf(stderr, "link failed\n");
      return 1;
   }
   p_glUseProgram(p);
   static const GLfloat tri[] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f};
   p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
   p_glEnableVertexAttribArray(0);
   p_glViewport(0, 0, size, size);
   p_glClearColor(0, 0, 0, 1);

   /* Warm up: shader variants, datasets, BOs. */
   for (int i = 0; i < 256; i++) {
      p_glBindFramebuffer(GL_FRAMEBUFFER, fbo[i % targets]);
      p_glClear(GL_COLOR_BUFFER_BIT);
      p_glDrawArrays(GL_TRIANGLES, 0, 3);
      p_glFlush();
   }
   p_glFinish();

   double t0 = now(), u0 = cpu(0), s0 = cpu(1);
   for (int i = 0; i < n; i++) {
      p_glBindFramebuffer(GL_FRAMEBUFFER, fbo[i % targets]);
      p_glClear(GL_COLOR_BUFFER_BIT);
      p_glDrawArrays(GL_TRIANGLES, 0, 3);
      p_glFlush();
      if ((i + 1) % k == 0)
         p_glFinish();
   }
   p_glFinish();
   double t = now() - t0, u = cpu(0) - u0, sy = cpu(1) - s0;

   if (p_glGetError() != GL_NO_ERROR)
      fprintf(stderr, "GL error\n");
   printf("%d submits in %.3f s: %.0f/s, %.1f us wall, %.1f us user, %.1f us sys per submit\n", n,
          t, n / t, t * 1e6 / n, u * 1e6 / n, sy * 1e6 / n);
   return 0;
}
