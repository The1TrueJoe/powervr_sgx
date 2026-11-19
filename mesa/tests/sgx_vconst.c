/* sgx_vconst: a vertex shader's uniform vec4 array as large as
 * GL_MAX_VERTEX_UNIFORM_VECTORS, as Source's GL layer declares its vertex
 * constants, indexed dynamically: pixel i of a 16x16 target is drawn by a
 * quad whose vertices read vc[i % n] by an attribute, and the colour must
 * be that element. Then the same with constant indices (the first, middle
 * and last elements), a mat4 array indexed by an attribute (skinning), and
 * one vec4 past the limit, which must fail to link. Surfaceless EGL, GLES2.
 *
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -o sgx_vconst sgx_vconst.c -ldl
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUNCS(X)                                                                                   \
   X(e, EGLDisplay, eglGetPlatformDisplay, (EGLenum, void *, const EGLAttrib *))                  \
   X(e, EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *))                              \
   X(e, EGLBoolean, eglBindAPI, (EGLenum))                                                         \
   X(e, EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *)) \
   X(e, EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *))        \
   X(e, EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))             \
   X(g, GLuint, glCreateShader, (GLenum))                                                          \
   X(g, void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))            \
   X(g, void, glCompileShader, (GLuint))                                                           \
   X(g, void, glGetShaderiv, (GLuint, GLenum, GLint *))                                            \
   X(g, void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                         \
   X(g, GLuint, glCreateProgram, (void))                                                           \
   X(g, void, glAttachShader, (GLuint, GLuint))                                                    \
   X(g, void, glBindAttribLocation, (GLuint, GLuint, const GLchar *))                             \
   X(g, void, glLinkProgram, (GLuint))                                                             \
   X(g, void, glGetProgramiv, (GLuint, GLenum, GLint *))                                           \
   X(g, void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                        \
   X(g, void, glUseProgram, (GLuint))                                                              \
   X(g, void, glDeleteProgram, (GLuint))                                                           \
   X(g, void, glDeleteShader, (GLuint))                                                            \
   X(g, GLint, glGetUniformLocation, (GLuint, const GLchar *))                                    \
   X(g, void, glUniform4fv, (GLint, GLsizei, const GLfloat *))                                     \
   X(g, void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *))                    \
   X(g, void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))   \
   X(g, void, glEnableVertexAttribArray, (GLuint))                                                 \
   X(g, void, glDrawArrays, (GLenum, GLint, GLsizei))                                              \
   X(g, void, glGenFramebuffers, (GLsizei, GLuint *))                                              \
   X(g, void, glBindFramebuffer, (GLenum, GLuint))                                                 \
   X(g, void, glGenRenderbuffers, (GLsizei, GLuint *))                                             \
   X(g, void, glBindRenderbuffer, (GLenum, GLuint))                                                \
   X(g, void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))                          \
   X(g, void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))                        \
   X(g, GLenum, glCheckFramebufferStatus, (GLenum))                                                \
   X(g, void, glViewport, (GLint, GLint, GLsizei, GLsizei))                                        \
   X(g, void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                  \
   X(g, void, glClear, (GLbitfield))                                                               \
   X(g, void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))             \
   X(g, void, glGetIntegerv, (GLenum, GLint *))                                                    \
   X(g, GLenum, glGetError, (void))                                                                \
   X(g, const GLubyte *, glGetString, (GLenum))
#define DECL(h, r, n, a) static r(*n##_) a;
FUNCS(DECL)
#define eglGetPlatformDisplay eglGetPlatformDisplay_
#define eglInitialize eglInitialize_
#define eglBindAPI eglBindAPI_
#define eglChooseConfig eglChooseConfig_
#define eglCreateContext eglCreateContext_
#define eglMakeCurrent eglMakeCurrent_
#define glCreateShader glCreateShader_
#define glShaderSource glShaderSource_
#define glCompileShader glCompileShader_
#define glGetShaderiv glGetShaderiv_
#define glGetShaderInfoLog glGetShaderInfoLog_
#define glCreateProgram glCreateProgram_
#define glAttachShader glAttachShader_
#define glBindAttribLocation glBindAttribLocation_
#define glLinkProgram glLinkProgram_
#define glGetProgramiv glGetProgramiv_
#define glGetProgramInfoLog glGetProgramInfoLog_
#define glUseProgram glUseProgram_
#define glDeleteProgram glDeleteProgram_
#define glDeleteShader glDeleteShader_
#define glGetUniformLocation glGetUniformLocation_
#define glUniform4fv glUniform4fv_
#define glUniformMatrix4fv glUniformMatrix4fv_
#define glVertexAttribPointer glVertexAttribPointer_
#define glEnableVertexAttribArray glEnableVertexAttribArray_
#define glDrawArrays glDrawArrays_
#define glGenFramebuffers glGenFramebuffers_
#define glBindFramebuffer glBindFramebuffer_
#define glGenRenderbuffers glGenRenderbuffers_
#define glBindRenderbuffer glBindRenderbuffer_
#define glRenderbufferStorage glRenderbufferStorage_
#define glFramebufferRenderbuffer glFramebufferRenderbuffer_
#define glCheckFramebufferStatus glCheckFramebufferStatus_
#define glViewport glViewport_
#define glClearColor glClearColor_
#define glClear glClear_
#define glReadPixels glReadPixels_
#define glGetIntegerv glGetIntegerv_
#define glGetError glGetError_
#define glGetString glGetString_

#define W 16
#define PIX (W * W)

static const char *fs_src = "precision mediump float; varying vec4 col;\n"
                            "void main() { gl_FragColor = col; }\n";

/* element k's value: distinct in every channel of a byte */
static void elem(unsigned k, float v[4])
{
   v[0] = (k % 256) / 255.0f;
   v[1] = (255 - k % 256) / 255.0f;
   v[2] = ((k * 37 + 11) % 256) / 255.0f;
   v[3] = ((k / 256) * 85 + 85) / 255.0f;
}

static GLuint build(const char *vs_src, int expect_link)
{
   GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER), p;
   GLint ok = 0;
   char log[1024] = "";

   glShaderSource(v, 1, &vs_src, NULL);
   glCompileShader(v);
   glShaderSource(f, 1, &fs_src, NULL);
   glCompileShader(f);
   p = glCreateProgram();
   glAttachShader(p, v);
   glAttachShader(p, f);
   glBindAttribLocation(p, 0, "p");
   glBindAttribLocation(p, 1, "i");
   glLinkProgram(p);
   glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok) {
      glGetShaderInfoLog(v, sizeof(log), NULL, log);
      if (!*log)
         glGetProgramInfoLog(p, sizeof(log), NULL, log);
      if (expect_link)
         printf("  link failed: %s\n", log);
      glDeleteProgram(p);
      p = 0;
   }
   glDeleteShader(v);
   glDeleteShader(f);
   return p;
}

/* a quad per pixel; attribute i = what the pixel's vertices index with */
static void draw(const float *idx)
{
   static float pos[PIX * 6 * 2], ix[PIX * 6];
   static const float corner[6][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}};

   for (unsigned i = 0; i < PIX; i++)
      for (unsigned c = 0; c < 6; c++) {
         pos[(i * 6 + c) * 2 + 0] = ((i % W) + corner[c][0]) * 2.0f / W - 1.0f;
         pos[(i * 6 + c) * 2 + 1] = ((i / W) + corner[c][1]) * 2.0f / W - 1.0f;
         ix[i * 6 + c] = idx[i];
      }
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 0, ix);
   glEnableVertexAttribArray(1);
   glClearColor(0, 0, 0, 0);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLES, 0, PIX * 6);
}

/* pixel i must be want[i] (to within 1 of 255) */
static int check(const char *name, const float (*want)[4])
{
   unsigned char px[PIX * 4];
   int bad = 0, first = -1;

   glReadPixels(0, 0, W, W, GL_RGBA, GL_UNSIGNED_BYTE, px);
   for (unsigned i = 0; i < PIX; i++)
      for (unsigned c = 0; c < 4; c++)
         if (fabsf(px[i * 4 + c] - want[i][c] * 255.0f) > 1.5f) {
            if (first < 0)
               first = i;
            bad++;
            break;
         }
   printf("%-24s %s", name, bad ? "FAIL" : "PASS");
   if (bad)
      printf(" (%d pixels; first %d: %u %u %u %u, want %.0f %.0f %.0f %.0f)", bad, first,
             px[first * 4], px[first * 4 + 1], px[first * 4 + 2], px[first * 4 + 3],
             want[first][0] * 255, want[first][1] * 255, want[first][2] * 255, want[first][3] * 255);
   printf(" err 0x%x\n", glGetError());
   return !bad;
}

int main(void)
{
   void *e = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL), *g = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
   if (!e || !g)
      return printf("no libEGL/libGLESv2\n"), 1;
#define LOAD(h, r, n, a) n##_ = dlsym(h, #n);
   FUNCS(LOAD)
   EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   EGLint n, ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
   EGLConfig cfg;

   if (!eglInitialize(d, NULL, NULL))
      return printf("no EGL\n"), 1;
   eglBindAPI(EGL_OPENGL_ES_API);
   EGLint attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
   eglChooseConfig(d, attr, &cfg, 1, &n);
   EGLContext c = eglCreateContext(d, n ? cfg : NULL, EGL_NO_CONTEXT, ca);
   eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c);
   printf("%s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

   GLuint fb, rb;
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glGenRenderbuffers(1, &rb);
   glBindRenderbuffer(GL_RENDERBUFFER, rb);
   /* RGBA8 (GLES2's own formats can't hold the values) */
   const char *ext = (const char *)glGetString(GL_EXTENSIONS);
   if (!ext || !strstr(ext, "GL_OES_rgb8_rgba8"))
      return printf("no GL_OES_rgb8_rgba8\n"), 1;
   glRenderbufferStorage(GL_RENDERBUFFER, 0x8058 /* GL_RGBA8_OES */, W, W);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      return printf("incomplete framebuffer\n"), 1;
   glViewport(0, 0, W, W);

   GLint max = 0;
   glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &max);
   printf("GL_MAX_VERTEX_UNIFORM_VECTORS %d\n", max);
   if (max < 128)
      return printf("FAIL: below GLES2's 128\n"), 1;

   int pass = 1;
   char vs[2048];
   static float vals[2048][4], want[PIX][4], idx[PIX];
   for (int k = 0; k < max && k < 2048; k++)
      elem(k, vals[k]);

   /* 1: vc[max], vc[i] for i from the attribute; pixel i reads element
    * i * max / PIX, then the last pixels read the last elements */
   snprintf(vs, sizeof(vs),
            "attribute vec2 p; attribute float i; uniform vec4 vc[%d]; varying vec4 col;\n"
            "void main() { col = vc[int(i)]; gl_Position = vec4(p, 0.0, 1.0); }\n",
            max);
   GLuint prog = build(vs, 1);
   if (!prog)
      return printf("dynamic                  FAIL (no program)\nFAIL\n"), 1;
   glUseProgram(prog);
   glUniform4fv(glGetUniformLocation(prog, "vc"), max, &vals[0][0]);
   for (unsigned i = 0; i < PIX; i++) {
      unsigned k = i < PIX - 8 ? i * (unsigned)max / PIX : (unsigned)max - (PIX - i);
      idx[i] = k + 0.25f; /* int() truncates */
      memcpy(want[i], vals[k], sizeof(want[i]));
   }
   draw(idx);
   pass &= check("dynamic", want);
   /* the same program, the elements written one at a time in reverse
    * (a later upload must replace every one) */
   for (int k = max - 1; k >= 0; k--) {
      char nm[32];
      float v[4];
      elem(max - 1 - k, v);
      memcpy(vals[k], v, sizeof(v));
      snprintf(nm, sizeof(nm), "vc[%d]", k);
      glUniform4fv(glGetUniformLocation(prog, nm), 1, v);
   }
   for (unsigned i = 0; i < PIX; i++)
      memcpy(want[i], vals[(unsigned)idx[i]], sizeof(want[i]));
   draw(idx);
   pass &= check("dynamic, rewritten", want);
   glDeleteProgram(prog);

   /* 2: constant indices into the same array: the first, middle and last
    * elements, picked by the attribute without indexing */
   snprintf(vs, sizeof(vs),
            "attribute vec2 p; attribute float i; uniform vec4 vc[%d]; varying vec4 col;\n"
            "void main() { col = i < 0.5 ? vc[0] : i < 1.5 ? vc[%d] : vc[%d];\n"
            " gl_Position = vec4(p, 0.0, 1.0); }\n",
            max, max / 2, max - 1);
   prog = build(vs, 1);
   if (prog) {
      glUseProgram(prog);
      for (int k = 0; k < max; k++)
         elem(k, vals[k]);
      glUniform4fv(glGetUniformLocation(prog, "vc"), max, &vals[0][0]);
      for (unsigned i = 0; i < PIX; i++) {
         unsigned s = i % 3, k = s == 0 ? 0 : s == 1 ? max / 2 : max - 1;
         idx[i] = s;
         memcpy(want[i], vals[k], sizeof(want[i]));
      }
      draw(idx);
      pass &= check("constant", want);
      glDeleteProgram(prog);
   } else
      pass = 0, printf("constant                 FAIL (no program)\n");

   /* 3: skinning: mat4 bones[(max - 8) / 4] indexed by the attribute, plus a
    * vec4 array for the rest of the limit; colour = bones[b] * (0,0,0,1),
    * i.e. column 3 of the bone */
   int nb = (max - 8) / 4;
   snprintf(vs, sizeof(vs),
            "attribute vec2 p; attribute float i; uniform mat4 bones[%d]; uniform vec4 more[8];\n"
            "varying vec4 col;\n"
            "void main() { col = bones[int(i)] * vec4(0.0, 0.0, 0.0, 1.0) + more[7] * 0.0;\n"
            " gl_Position = vec4(p, 0.0, 1.0); }\n",
            nb);
   prog = build(vs, 1);
   if (prog) {
      static float m[1024][16];
      glUseProgram(prog);
      for (int b = 0; b < nb; b++) {
         memset(m[b], 0, sizeof(m[b]));
         elem(b * 3 + 1, &m[b][12]);
      }
      glUniformMatrix4fv(glGetUniformLocation(prog, "bones"), nb, GL_FALSE, &m[0][0]);
      float zero[8][4] = {{0}};
      glUniform4fv(glGetUniformLocation(prog, "more"), 8, &zero[0][0]);
      for (unsigned i = 0; i < PIX; i++) {
         unsigned b = i % nb;
         idx[i] = b + 0.25f;
         memcpy(want[i], &m[b][12], sizeof(want[i]));
      }
      draw(idx);
      pass &= check("bones", want);
      glDeleteProgram(prog);
   } else
      pass = 0, printf("bones                    FAIL (no program)\n");

   /* 4: one past the limit must not link */
   snprintf(vs, sizeof(vs),
            "attribute vec2 p; attribute float i; uniform vec4 vc[%d]; varying vec4 col;\n"
            "void main() { col = vc[int(i)]; gl_Position = vec4(p, 0.0, 1.0); }\n",
            max + 1);
   prog = build(vs, 0);
   printf("%-24s %s\n", "past the limit", prog ? "FAIL (linked)" : "PASS (no link)");
   pass &= !prog;

   printf("%s\n", pass ? "PASS" : "FAIL");
   return !pass;
}
