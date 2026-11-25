/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */
/* sgx_skin: a skinning vertex shader with the uniform footprint of librw's
 * gl3 skin shader (reVC and re3, issue #5): 64 bone matrices indexed per
 * vertex, the projection, view and world matrices, 8 lights of 4 vec4 each,
 * and the ambient, material, surface and fog vectors, about 304 vec4 in all.
 * Past 256 + 9 vec4 the link failed and librw got no program.
 *
 * The quad goes through bone 63 alone (the others are zero, so a wrong bone
 * draws nothing) and is lit by light 7 alone over the ambient: every pixel
 * of the 32x32 target must be (64, 128, 0). Prints the link log on failure
 * and GL_MAX_VERTEX_UNIFORM_VECTORS. Surfaceless EGL, GLES2.
 *
 * zig cc -target x86_64-linux-gnu.2.36 -O2 -I<mesa include> -o sgx_skin sgx_skin.c -ldl
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
   X(GLuint, glCreateProgram, (void))                                                              \
   X(void, glAttachShader, (GLuint, GLuint))                                                       \
   X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *))                                 \
   X(void, glLinkProgram, (GLuint))                                                                \
   X(void, glUseProgram, (GLuint))                                                                 \
   X(void, glGetProgramiv, (GLuint, GLenum, GLint *))                                              \
   X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                           \
   X(GLint, glGetUniformLocation, (GLuint, const GLchar *))                                        \
   X(void, glUniform4fv, (GLint, GLsizei, const GLfloat *))                                        \
   X(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *))                       \
   X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))       \
   X(void, glEnableVertexAttribArray, (GLuint))                                                    \
   X(void, glDrawArrays, (GLenum, GLint, GLsizei))                                                 \
   X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))                 \
   X(GLenum, glGetError, (void))

#define DECL(r, n, a) static r(*p_##n) a;
EGLF(DECL)
GLF(DECL)

#define SIZE   32
#define BONES  64
#define LIGHTS 8

static const char *vs_src =
   "attribute vec2 pos;\n"
   "attribute vec4 bone;\n"
   "attribute vec4 weight;\n"
   "uniform mat4 u_bones[64];\n"
   "uniform mat4 u_proj, u_view, u_world;\n"
   "uniform vec4 u_lightPos[8], u_lightDir[8], u_lightCol[8], u_lightParams[8];\n"
   "uniform vec4 u_amb, u_matColor, u_surf, u_fog;\n"
   "varying vec4 v_col;\n"
   "void main() {\n"
   "   vec4 p = vec4(0.0);\n"
   "   vec3 n = vec3(0.0);\n"
   "   for (int i = 0; i < 4; i++) {\n"
   "      mat4 m = u_bones[int(bone[i])];\n"
   "      p += m * vec4(pos, 0.0, 1.0) * weight[i];\n"
   "      n += mat3(m) * vec3(0.0, 0.0, 1.0) * weight[i];\n"
   "   }\n"
   "   vec4 world = u_world * p;\n"
   "   gl_Position = u_proj * u_view * world;\n"
   "   vec3 c = u_amb.rgb * u_surf.x;\n"
   "   for (int l = 0; l < 8; l++) {\n"
   "      vec3 d = u_lightDir[l].xyz + (u_lightPos[l].xyz - world.xyz) * u_lightPos[l].w;\n"
   "      c += u_lightCol[l].rgb * u_lightParams[l].x * max(dot(normalize(n), d), 0.0) * u_surf.y;\n"
   "   }\n"
   "   v_col = clamp(vec4(c, 1.0), 0.0, 1.0) * u_matColor;\n"
   "   v_col.a *= clamp((u_fog.y - gl_Position.z) * u_fog.z, 0.0, 1.0);\n"
   "}\n";
static const char *fs_src = "precision mediump float; varying vec4 v_col;\n"
                            "void main() { gl_FragColor = v_col; }\n";

static void uni4(GLuint prog, const char *name, int n, const float *v)
{
   p_glUniform4fv(p_glGetUniformLocation(prog, name), n, v);
}

int main(void)
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
   const EGLint pa[] = {EGL_WIDTH, SIZE, EGL_HEIGHT, SIZE, EGL_NONE};

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

   GLint maxv = 0;
   p_glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &maxv);
   printf("GL_MAX_VERTEX_UNIFORM_VECTORS %d\n", maxv);

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
   p_glBindAttribLocation(prog, 1, "bone");
   p_glBindAttribLocation(prog, 2, "weight");
   p_glLinkProgram(prog);
   p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
   if (!ok) {
      char log[2048] = "";
      p_glGetProgramInfoLog(prog, sizeof(log), NULL, log);
      printf("link failed: %s\nFAIL\n", log);
      return 1;
   }
   p_glUseProgram(prog);

   /* bone 63 scales the half-size quad to the whole target; the rest are 0 */
   static float bones[BONES][16];
   static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
   bones[BONES - 1][0] = bones[BONES - 1][5] = 2;
   bones[BONES - 1][10] = bones[BONES - 1][15] = 1;
   p_glUniformMatrix4fv(p_glGetUniformLocation(prog, "u_bones"), BONES, GL_FALSE, &bones[0][0]);
   p_glUniformMatrix4fv(p_glGetUniformLocation(prog, "u_proj"), 1, GL_FALSE, ident);
   p_glUniformMatrix4fv(p_glGetUniformLocation(prog, "u_view"), 1, GL_FALSE, ident);
   p_glUniformMatrix4fv(p_glGetUniformLocation(prog, "u_world"), 1, GL_FALSE, ident);

   /* light 7 shines along the normal with (0, 0.5, 0); the others are dark
    * and point elsewhere; the ambient is (0.25, 0, 0) */
   float lpos[LIGHTS][4] = {{0}}, ldir[LIGHTS][4] = {{0}}, lcol[LIGHTS][4] = {{0}},
         lpar[LIGHTS][4] = {{0}};
   for (int l = 0; l < LIGHTS; l++) {
      ldir[l][0] = 1;
      lpar[l][0] = 1;
      lpos[l][0] = 5; /* w = 0: the position isn't used */
   }
   ldir[LIGHTS - 1][0] = 0;
   ldir[LIGHTS - 1][2] = 1;
   lcol[LIGHTS - 1][1] = 0.5f;
   uni4(prog, "u_lightPos", LIGHTS, &lpos[0][0]);
   uni4(prog, "u_lightDir", LIGHTS, &ldir[0][0]);
   uni4(prog, "u_lightCol", LIGHTS, &lcol[0][0]);
   uni4(prog, "u_lightParams", LIGHTS, &lpar[0][0]);
   static const float amb[4] = {0.25f, 0, 0, 0}, mat[4] = {1, 1, 1, 1}, surf[4] = {1, 1, 0, 0},
                      fog[4] = {0, 2, 1, 0};
   uni4(prog, "u_amb", 1, amb);
   uni4(prog, "u_matColor", 1, mat);
   uni4(prog, "u_surf", 1, surf);
   uni4(prog, "u_fog", 1, fog);

   static const float pos[] = {-0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f};
   static const float bone[] = {63, 0, 1, 2, 63, 3, 4, 5, 63, 6, 7, 8, 63, 9, 10, 11};
   static const float weight[] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
   p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
   p_glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, bone);
   p_glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 0, weight);
   for (int a = 0; a < 3; a++)
      p_glEnableVertexAttribArray(a);
   p_glViewport(0, 0, SIZE, SIZE);
   p_glClearColor(0, 0, 1, 1);
   p_glClear(GL_COLOR_BUFFER_BIT);
   p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

   static unsigned char px[SIZE * SIZE * 4];
   p_glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, px);
   int bad = 0, first = -1;
   for (int i = 0; i < SIZE * SIZE; i++) {
      const unsigned char *p = &px[i * 4];
      if (abs(p[0] - 64) > 2 || abs(p[1] - 128) > 2 || p[2] > 2) {
         if (first < 0)
            first = i;
         bad++;
      }
   }
   if (first >= 0)
      printf("first wrong pixel %d,%d: %d,%d,%d\n", first % SIZE, first / SIZE, px[first * 4],
             px[first * 4 + 1], px[first * 4 + 2]);
   printf("%d of %d pixels wrong, GL error 0x%x\n%s\n", bad, SIZE * SIZE, p_glGetError(),
          bad ? "FAIL" : "PASS");
   return bad ? 1 : 0;
}
