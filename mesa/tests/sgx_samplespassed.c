/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */
/* sgx_samplespassed: GL_SAMPLES_PASSED counts, as an engine that culls what
 * passes fewer than some number of samples reads them (Red Eclipse's oqfrags,
 * 8; issue #5). Into a 128x128 FBO with a depth buffer: a query with no draw,
 * one around a visible quad, one around a quad behind the depth buffer, and
 * a query whose quad is visible again after the hidden one. Prints each count
 * and GL_QUERY_COUNTER_BITS. A visible quad must count at least 8, a hidden
 * one and an empty query 0. SGX_DEBUG=noocclusion (Mesa's stand-in queries,
 * as before 0.2.4) counts 0 for all of them and fails.
 *
 *   sgx_samplespassed
 *   sgx_samplespassed bench [frames]
 *
 * bench: what the queries cost. Frames of 16 full-target quads into a
 * 1024x600 FBO, front to back and back to front, each quad in a query of its
 * own or none: a query makes its draws a translucent pass.
 *
 * Surfaceless EGL, desktop OpenGL.
 * zig cc -target x86_64-linux-gnu.2.36 -O2 -I<mesa include> -o sgx_samplespassed sgx_samplespassed.c -ldl
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EGLF(X)                                                                                    \
   X(EGLDisplay, eglGetPlatformDisplay, (EGLenum, void *, const EGLAttrib *))                     \
   X(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *))                                 \
   X(EGLBoolean, eglBindAPI, (EGLenum))                                                            \
   X(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))    \
   X(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *))           \
   X(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))                \
   X(void *, eglGetProcAddress, (const char *))
#define GLF(X)                                                                                     \
   X(const GLubyte *, glGetString, (GLenum))                                                        \
   X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))                                           \
   X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                     \
   X(void, glClearDepth, (GLdouble))                                                               \
   X(void, glClear, (GLbitfield))                                                                  \
   X(void, glEnable, (GLenum))                                                                     \
   X(void, glDepthFunc, (GLenum))                                                                  \
   X(void, glGenTextures, (GLsizei, GLuint *))                                                     \
   X(void, glBindTexture, (GLenum, GLuint))                                                        \
   X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
   X(void, glTexParameteri, (GLenum, GLenum, GLint))                                               \
   X(void, glGenFramebuffers, (GLsizei, GLuint *))                                                 \
   X(void, glBindFramebuffer, (GLenum, GLuint))                                                    \
   X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                        \
   X(void, glGenRenderbuffers, (GLsizei, GLuint *))                                                \
   X(void, glBindRenderbuffer, (GLenum, GLuint))                                                   \
   X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))                              \
   X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))                            \
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
   X(GLint, glGetUniformLocation, (GLuint, const GLchar *))                                        \
   X(void, glUniform1f, (GLint, GLfloat))                                                          \
   X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))       \
   X(void, glEnableVertexAttribArray, (GLuint))                                                    \
   X(void, glDrawArrays, (GLenum, GLint, GLsizei))                                                 \
   X(void, glGenQueries, (GLsizei, GLuint *))                                                      \
   X(void, glBeginQuery, (GLenum, GLuint))                                                         \
   X(void, glEndQuery, (GLenum))                                                                   \
   X(void, glGetQueryiv, (GLenum, GLenum, GLint *))                                                \
   X(void, glGetQueryObjectuiv, (GLuint, GLenum, GLuint *))                                        \
   X(void, glFinish, (void))                                                                       \
   X(GLenum, glGetError, (void))

#define DECL(r, n, a) static r(*p_##n) a;
EGLF(DECL)
GLF(DECL)

#define SIZE 128
#define BENCH_W 1024
#define BENCH_H 600
#define LAYERS 16

static const char *vs_src = "#version 110\n"
                            "attribute vec2 pos;\n"
                            "uniform float z;\n"
                            "void main() { gl_Position = vec4(pos, z, 1.0); }\n";
static const char *fs_src = "#version 110\n"
                            "void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";

static GLint z_loc;

static double now(void)
{
   struct timespec t;

   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec + t.tv_nsec * 1e-9;
}

/* a full-target quad at depth z in a query of its own (none: no draw) */
static GLuint count(GLuint q, int draw, float z)
{
   GLuint v = ~0u;

   p_glBeginQuery(GL_SAMPLES_PASSED, q);
   if (draw) {
      p_glUniform1f(z_loc, z);
      p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   }
   p_glEndQuery(GL_SAMPLES_PASSED);
   p_glGetQueryObjectuiv(q, GL_QUERY_RESULT, &v);
   return v;
}

/* ms a frame of LAYERS quads, front to back or back to front, with a query
 * around each or none; the results read after the frame, as a game does */
static double bench(const GLuint *q, int frames, int queries, int back_to_front)
{
   double t0 = 0;
   GLuint v;

   for (int f = -2; f < frames; f++) {
      if (f == 0) {
         p_glFinish();
         t0 = now();
      }
      p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      for (int l = 0; l < LAYERS; l++) {
         int i = back_to_front ? LAYERS - 1 - l : l;

         if (queries)
            p_glBeginQuery(GL_SAMPLES_PASSED, q[l]);
         p_glUniform1f(z_loc, -0.9f + 1.8f * i / LAYERS);
         p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
         if (queries)
            p_glEndQuery(GL_SAMPLES_PASSED);
      }
      for (int l = 0; queries && l < LAYERS; l++)
         p_glGetQueryObjectuiv(q[l], GL_QUERY_RESULT, &v);
      p_glFinish();
   }
   return (now() - t0) * 1e3 / frames;
}

int main(int argc, char **argv)
{
   int bench_frames = argc > 1 && !strcmp(argv[1], "bench") ? (argc > 2 ? atoi(argv[2]) : 100) : 0;
   int w = bench_frames ? BENCH_W : SIZE, h = bench_frames ? BENCH_H : SIZE;

   void *egl = dlopen("libEGL.so.1", RTLD_NOW);
   if (!egl) {
      fprintf(stderr, "dlopen: %s\n", dlerror());
      return 1;
   }
#define LOADE(r, n, a) p_##n = (r(*) a)dlsym(egl, #n);
   EGLF(LOADE)

   EGLDisplay d = p_eglGetPlatformDisplay(0x31DD /* EGL_PLATFORM_SURFACELESS_MESA */,
                                        EGL_DEFAULT_DISPLAY, NULL);
   EGLint maj, min, nc;
   EGLConfig cfg;
   const EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                        EGL_NONE};

   if (!p_eglInitialize(d, &maj, &min) || !p_eglBindAPI(EGL_OPENGL_API) ||
       !p_eglChooseConfig(d, ca, &cfg, 1, &nc) || nc < 1) {
      fprintf(stderr, "EGL setup failed\n");
      return 1;
   }
   EGLContext c = p_eglCreateContext(d, cfg, EGL_NO_CONTEXT, NULL);
   if (c == EGL_NO_CONTEXT || !p_eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
      fprintf(stderr, "no desktop GL context\n");
      return 1;
   }
#define LOADG(r, n, a) p_##n = (r(*) a)p_eglGetProcAddress(#n);
   GLF(LOADG)
   printf("%s, %s\n", p_glGetString(GL_RENDERER), p_glGetString(GL_VERSION));

   GLuint tex, fbo, rb;
   p_glGenTextures(1, &tex);
   p_glBindTexture(GL_TEXTURE_2D, tex);
   p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   p_glGenRenderbuffers(1, &rb);
   p_glBindRenderbuffer(GL_RENDERBUFFER, rb);
   p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
   p_glGenFramebuffers(1, &fbo);
   p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
   p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
   if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      fprintf(stderr, "FBO incomplete\n");
      return 1;
   }

   GLuint vs = p_glCreateShader(GL_VERTEX_SHADER), fs = p_glCreateShader(GL_FRAGMENT_SHADER),
          prog = p_glCreateProgram();
   GLint ok;
   p_glShaderSource(vs, 1, &vs_src, NULL);
   p_glCompileShader(vs);
   p_glShaderSource(fs, 1, &fs_src, NULL);
   p_glCompileShader(fs);
   p_glAttachShader(prog, vs);
   p_glAttachShader(prog, fs);
   p_glBindAttribLocation(prog, 0, "pos");
   p_glLinkProgram(prog);
   p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
   if (!ok) {
      fprintf(stderr, "link failed\n");
      return 1;
   }
   p_glUseProgram(prog);
   z_loc = p_glGetUniformLocation(prog, "z");

   static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
   p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
   p_glEnableVertexAttribArray(0);
   p_glViewport(0, 0, w, h);
   p_glEnable(GL_DEPTH_TEST);
   p_glDepthFunc(GL_LESS);
   p_glClearColor(0, 0, 0, 1);
   p_glClearDepth(1.0);
   p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

   GLint bits = -1;
   GLuint q[LAYERS];
   p_glGetQueryiv(GL_SAMPLES_PASSED, GL_QUERY_COUNTER_BITS, &bits);
   p_glGenQueries(LAYERS, q);
   printf("counter bits %d\n", bits);

   if (bench_frames > 0) {
      for (int rep = 0; rep < 2; rep++) {
         for (int order = 0; order < 2; order++) {
            double none = bench(q, bench_frames, 0, order),
                   with = bench(q, bench_frames, 1, order);

            printf("%s, %d quads %dx%d: no queries %.2f ms, a query each %.2f ms a frame\n",
                   order ? "back to front" : "front to back", LAYERS, w, h, none, with);
         }
      }
      return p_glGetError() ? 1 : 0;
   }

   /* none; the quad at z 0 over a depth of 1; again at 0.5, behind it; at -0.5 */
   GLuint none = count(q[0], 0, 0), vis = count(q[1], 1, 0), hid = count(q[2], 1, 0.5f),
          front = count(q[3], 1, -0.5f);
   GLenum err = p_glGetError();
   int pass = !err && none == 0 && vis >= 8 && front >= 8 && hid == 0;

   printf("no draw %u, visible %u, hidden %u, visible again %u (of %d pixels)\n", none, vis,
          hid, front, SIZE * SIZE);
   printf("GL error 0x%x\n", err);
   printf("%s\n", pass ? "PASS" : "FAIL");
   return pass ? 0 : 1;
}
