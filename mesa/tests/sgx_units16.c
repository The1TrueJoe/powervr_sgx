/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */
/* sgx_units16: one fragment program reading 16 texture units, as Source's
 * D3D9-to-GL layer (ToGL) binds D3D9's 16 samplers without checking
 * GL_MAX_TEXTURE_IMAGE_UNITS (issue #5, Half-Life: Source). The odd units
 * are cube maps and the even ones 2D, all 8x8 and mipmapped
 * (LINEAR_MIPMAP_LINEAR), each a colour of its own at every level and face,
 * and each read through a coordinate from a uniform matrix, so a dependent
 * sample that takes its LOD from derivatives (lower_dep_lod). With 8 units the
 * texture-size constants held units 0-7 only and the program didn't compile
 * for a cube on units 8-15.
 *
 * Column k of the 64x4 target is drawn with u_sel picking unit k alone and
 * must be unit k's colour: a unit that reads another's texture, or a skipped
 * draw (the clear colour), shows in its column. Also checks
 * GL_MAX_TEXTURE_IMAGE_UNITS >= 16 and that glActiveTexture(GL_TEXTURE15)
 * and glUniform1i(sampler, 15) raise no error. Surfaceless EGL, GLES2.
 *
 * zig cc -target x86_64-linux-gnu.2.36 -O2 -I<mesa include> -o sgx_units16 sgx_units16.c -ldl
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EGLF(X)                                                                                    \
   X(EGLDisplay, eglGetPlatformDisplay, (EGLenum, void *, const EGLAttrib *))                     \
   X(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *))                                 \
   X(EGLBoolean, eglBindAPI, (EGLenum))                                                            \
   X(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))    \
   X(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *))           \
   X(EGLSurface, eglCreatePbufferSurface, (EGLDisplay, EGLConfig, const EGLint *))                \
   X(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))
#define GLF(X)                                                                                     \
   X(void, glGetIntegerv, (GLenum, GLint *))                                                       \
   X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))                                           \
   X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                     \
   X(void, glClear, (GLbitfield))                                                                  \
   X(GLuint, glCreateShader, (GLenum))                                                             \
   X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))                \
   X(void, glCompileShader, (GLuint))                                                              \
   X(void, glGetShaderiv, (GLuint, GLenum, GLint *))                                               \
   X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                            \
   X(GLuint, glCreateProgram, (void))                                                              \
   X(void, glAttachShader, (GLuint, GLuint))                                                       \
   X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *))                                 \
   X(void, glLinkProgram, (GLuint))                                                                \
   X(void, glUseProgram, (GLuint))                                                                 \
   X(void, glGetProgramiv, (GLuint, GLenum, GLint *))                                              \
   X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                           \
   X(GLint, glGetUniformLocation, (GLuint, const GLchar *))                                        \
   X(void, glUniform1i, (GLint, GLint))                                                            \
   X(void, glUniform1fv, (GLint, GLsizei, const GLfloat *))                                        \
   X(void, glUniformMatrix3fv, (GLint, GLsizei, GLboolean, const GLfloat *))                       \
   X(void, glGenTextures, (GLsizei, GLuint *))                                                     \
   X(void, glActiveTexture, (GLenum))                                                              \
   X(void, glBindTexture, (GLenum, GLuint))                                                        \
   X(void, glTexImage2D,                                                                           \
     (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))                \
   X(void, glTexParameteri, (GLenum, GLenum, GLint))                                               \
   X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))       \
   X(void, glEnableVertexAttribArray, (GLuint))                                                    \
   X(void, glDrawArrays, (GLenum, GLint, GLsizei))                                                 \
   X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))                 \
   X(GLenum, glGetError, (void))

#define DECL(r, n, a) static r(*p_##n) a;
EGLF(DECL)
GLF(DECL)

#define UNITS 16
#define COL   4 /* each unit's column, in pixels */
#define W     (UNITS * COL)
#define H     4
#define TEX   8

static const char *vs_src = "attribute vec2 pos;\n"
                            "varying vec2 v_coord;\n"
                            "void main() {\n"
                            "   gl_Position = vec4(pos, 0.0, 1.0);\n"
                            "   v_coord = pos * 0.5;\n"
                            "}\n";

/* the unit's colour, distinct for each, the same at every level and face */
static void
unit_color(int u, unsigned char c[4])
{
   c[0] = 16 * u + 8;
   c[1] = 248 - 16 * u;
   c[2] = u & 1 ? 200 : 40;
   c[3] = 255;
}

int
main(void)
{
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
   const EGLint pa[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};

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

   GLint units = 0;
   p_glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
   printf("GL_MAX_TEXTURE_IMAGE_UNITS %d\n", units);
   if (units < UNITS) {
      printf("FAIL\n");
      return 1;
   }

   /* the fragment shader: sum over units of u_sel[i] * the unit's sample */
   static char fs_src[8192];
   int n = snprintf(fs_src, sizeof(fs_src),
                    "precision mediump float;\nvarying vec2 v_coord;\n"
                    "uniform float u_sel[%d];\n",
                    UNITS);
   for (int u = 0; u < UNITS; u++)
      n += snprintf(fs_src + n, sizeof(fs_src) - n,
                    "uniform %s u_s%d;\nuniform mat3 u_t%d;\n",
                    u & 1 ? "samplerCube" : "sampler2D", u, u);
   n += snprintf(fs_src + n, sizeof(fs_src) - n,
                 "void main() {\n   vec4 c = vec4(0.0);\n");
   for (int u = 0; u < UNITS; u++)
      n += snprintf(fs_src + n, sizeof(fs_src) - n,
                    u & 1 ? "   c += u_sel[%d] * textureCube(u_s%d, u_t%d * vec3(v_coord, 1.0));\n"
                          : "   c += u_sel[%d] * texture2D(u_s%d, (u_t%d * vec3(v_coord, 1.0)).xy);\n",
                    u, u, u);
   snprintf(fs_src + n, sizeof(fs_src) - n, "   gl_FragColor = c;\n}\n");

   const char *fsp = fs_src;
   GLuint vs = p_glCreateShader(GL_VERTEX_SHADER), fs = p_glCreateShader(GL_FRAGMENT_SHADER),
          prog = p_glCreateProgram();
   GLint ok;
   char log[2048] = "";
   p_glShaderSource(vs, 1, &vs_src, NULL);
   p_glCompileShader(vs);
   p_glShaderSource(fs, 1, &fsp, NULL);
   p_glCompileShader(fs);
   p_glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      p_glGetShaderInfoLog(fs, sizeof(log), NULL, log);
      printf("compile failed: %s\nFAIL\n", log);
      return 1;
   }
   p_glAttachShader(prog, vs);
   p_glAttachShader(prog, fs);
   p_glBindAttribLocation(prog, 0, "pos");
   p_glLinkProgram(prog);
   p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
   if (!ok) {
      p_glGetProgramInfoLog(prog, sizeof(log), NULL, log);
      printf("link failed: %s\nFAIL\n", log);
      return 1;
   }
   p_glUseProgram(prog);

   /* unit u: its own texture, sampler u_s<u> on it; unit 15 last, so the
    * error check after it is the one ToGL's bind hits */
   GLuint tex[UNITS];
   static unsigned char texels[TEX * TEX * 4];
   static const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
   p_glGenTextures(UNITS, tex);
   for (int u = 0; u < UNITS; u++) {
      GLenum target = u & 1 ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
      unsigned char col[4];
      char name[16];

      unit_color(u, col);
      for (int i = 0; i < TEX * TEX; i++)
         memcpy(&texels[i * 4], col, 4);
      p_glActiveTexture(GL_TEXTURE0 + u);
      p_glBindTexture(target, tex[u]);
      for (int f = 0; f < (u & 1 ? 6 : 1); f++)
         for (int l = 0, sz = TEX; sz; l++, sz >>= 1)
            p_glTexImage2D(u & 1 ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + f : GL_TEXTURE_2D, l, GL_RGBA,
                           sz, sz, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
      p_glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
      p_glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      p_glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      p_glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      snprintf(name, sizeof(name), "u_s%d", u);
      p_glUniform1i(p_glGetUniformLocation(prog, name), u);
      snprintf(name, sizeof(name), "u_t%d", u);
      p_glUniformMatrix3fv(p_glGetUniformLocation(prog, name), 1, GL_FALSE, ident);
   }
   GLenum err = p_glGetError();
   if (err) {
      printf("GL error 0x%x binding the 16 units\nFAIL\n", err);
      return 1;
   }

   static const float pos[] = {-1, -1, 1, -1, -1, 1, 1, 1};
   p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
   p_glEnableVertexAttribArray(0);
   p_glClearColor(0, 0, 0, 0);
   p_glViewport(0, 0, W, H);
   p_glClear(GL_COLOR_BUFFER_BIT);
   GLint sel = p_glGetUniformLocation(prog, "u_sel");
   for (int k = 0; k < UNITS; k++) {
      float w[UNITS] = {0};

      w[k] = 1;
      p_glUniform1fv(sel, UNITS, w);
      p_glViewport(k * COL, 0, COL, H);
      p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   }

   static unsigned char px[W * H * 4];
   p_glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
   int bad_units = 0;
   for (int k = 0; k < UNITS; k++) {
      unsigned char want[4];
      int bad = 0;
      const unsigned char *p = NULL;

      unit_color(k, want);
      for (int y = 0; y < H; y++)
         for (int x = k * COL; x < (k + 1) * COL; x++) {
            const unsigned char *q = &px[(y * W + x) * 4];
            if (abs(q[0] - want[0]) > 2 || abs(q[1] - want[1]) > 2 || abs(q[2] - want[2]) > 2) {
               if (!bad)
                  p = q;
               bad++;
            }
         }
      if (bad) {
         printf("unit %2d (%s): %d pixels wrong, first %d,%d,%d, want %d,%d,%d\n", k,
                k & 1 ? "cube" : "2D", bad, p[0], p[1], p[2], want[0], want[1], want[2]);
         bad_units++;
      }
   }
   err = p_glGetError();
   printf("%d of %d units wrong, GL error 0x%x\n%s\n", bad_units, UNITS, err,
          bad_units || err ? "FAIL" : "PASS");
   return bad_units || err ? 1 : 0;
}
