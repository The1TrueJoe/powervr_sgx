/*
 * Copyright © 2026 servidorjarvis1-svg
 * SPDX-License-Identifier: MIT
 */

/*
 * 2D texture arrays (SGX_DEBUG=glsl130, an ES 3.0 context): a layer a 2D
 * texture at a layer stride, the program moving the texture state's address
 * to the layer (sgx_program.c lower_tex_array).  Read back from a 64x64 RGBA8
 * FBO:
 *  1. the layer from a uniform, NEAREST and LINEAR (layers never blend);
 *     clamped below 0 and past the last, rounded to nearest
 *  2. the layer per pixel, from a varying (four columns, four layers)
 *  3. texelFetch
 *  4. textureSize
 *  5. a mipmapped array, textureLod level 1
 *  6. rendered into one layer (glFramebufferTextureLayer), the others kept
 *  7. NPOT layers (linear layout)
 *  8. 64 layers: the last one
 *  9. sampler2DArrayShadow: the depth of a DEPTH_COMPONENT32F array's layer
 * 10. the layer changing inside a 2x2 quad (floor(x / 3)): the texture
 *     state is read once a quad, so each pixel of it must still get its own
 *     layer; every pixel of the 64x64 target checked
 *
 *   MESA_GLES_VERSION_OVERRIDE=3.0 SGX_DEBUG=glsl130 DISPLAY=:0 sgx_texarray
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define W 64
#define H 64

static int fails;

static GLuint
prog(const char *fs)
{
   static const char *vs = "#version 300 es\n"
                           "in vec2 a;\nout vec2 uv;\n"
                           "void main() { uv = a * 0.5 + 0.5;"
                           " gl_Position = vec4(a, 0.0, 1.0); }\n";
   GLuint p = glCreateProgram();
   const char *src[2] = {vs, fs};
   GLenum ty[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};

   for (int i = 0; i < 2; i++) {
      GLuint s = glCreateShader(ty[i]);
      GLint ok;

      glShaderSource(s, 1, &src[i], NULL);
      glCompileShader(s);
      glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
      if (!ok) {
         char l[2048];
         glGetShaderInfoLog(s, sizeof l, NULL, l);
         printf("compile: %s\n", l);
      }
      glAttachShader(p, s);
   }
   glBindAttribLocation(p, 0, "a");
   glLinkProgram(p);
   GLint ok;
   glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok) {
      char l[2048];
      glGetProgramInfoLog(p, sizeof l, NULL, l);
      printf("link: %s\n", l);
   }
   return p;
}

static void
check(const char *name, int x, int y, int r, int g, int b, int a, int tol)
{
   unsigned char p[4];
   int e[4] = {r, g, b, a};
   int ok = 1;

   glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
   for (int c = 0; c < 4; c++)
      if (e[c] >= 0 && abs(p[c] - e[c]) > tol)
         ok = 0;
   printf("%-44s (%2d,%2d) got %3u %3u %3u %3u want %3d %3d %3d %3d: %s\n",
          name, x, y, p[0], p[1], p[2], p[3], r, g, b, a, ok ? "OK" : "FAIL");
   fails += !ok;
}

static void
quad(void)
{
   static const float v[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};

   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
   glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* layer l's colour */
static void
lc(int l, unsigned char *c)
{
   c[0] = (l * 37 + 20) & 0xff;
   c[1] = (l * 91 + 200) & 0xff;
   c[2] = (l * 53) & 0xff;
   c[3] = 255;
}

static GLuint
array_tex(int w, int h, int layers, int levels)
{
   GLuint t;
   unsigned char *px = malloc((size_t)w * h * 4);

   glGenTextures(1, &t);
   glBindTexture(GL_TEXTURE_2D_ARRAY, t);
   glTexStorage3D(GL_TEXTURE_2D_ARRAY, levels, GL_RGBA8, w, h, layers);
   for (int lv = 0; lv < levels; lv++) {
      int lw = w >> lv ? w >> lv : 1, lh = h >> lv ? h >> lv : 1;
      for (int l = 0; l < layers; l++) {
         unsigned char c[4];
         lc(l + 16 * lv, c);
         for (int i = 0; i < lw * lh; i++)
            memcpy(px + 4 * i, c, 4);
         glTexSubImage3D(GL_TEXTURE_2D_ARRAY, lv, 0, 0, l, lw, lh, 1, GL_RGBA,
                         GL_UNSIGNED_BYTE, px);
      }
   }
   free(px);
   glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER,
                   levels > 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   return t;
}

static void
want_layer(const char *name, int x, int y, int l)
{
   unsigned char c[4];
   lc(l, c);
   check(name, x, y, c[0], c[1], c[2], 255, 1);
}

int
main(void)
{
   EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLint maj, min;

   if (!eglInitialize(d, &maj, &min)) {
      printf("eglInitialize failed\n");
      return 1;
   }
   eglBindAPI(EGL_OPENGL_ES_API);
   const EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
   EGLConfig cfg;
   EGLint n = 0;
   eglChooseConfig(d, ca, &cfg, 1, &n);
   const EGLint xa3[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
   EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa3);
   if (c == EGL_NO_CONTEXT ||
       !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
      printf("no ES 3.0 context 0x%x\n", eglGetError());
      return 1;
   }
   printf("%s\n", glGetString(GL_VERSION));
   GLint ml = 0;
   glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &ml);
   printf("GL_MAX_ARRAY_TEXTURE_LAYERS %d: %s\n", ml, ml >= 256 ? "OK" : "FAIL");
   fails += ml < 256;

   GLuint ct, fb;
   glGenTextures(1, &ct);
   glBindTexture(GL_TEXTURE_2D, ct);
   glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                          ct, 0);
   glViewport(0, 0, W, H);

   GLuint pu = prog("#version 300 es\nprecision highp float;\n"
                    "uniform highp sampler2DArray s;\nuniform float z;\n"
                    "in vec2 uv;\nout vec4 o;\n"
                    "void main() { o = texture(s, vec3(uv, z)); }\n");
   glUseProgram(pu);
   glUniform1i(glGetUniformLocation(pu, "s"), 0);
   GLint zl = glGetUniformLocation(pu, "z");
   array_tex(8, 8, 4, 1);

   /* 1 */
   for (int l = 0; l < 4; l++) {
      char nm[64];
      glUniform1f(zl, (float)l);
      quad();
      snprintf(nm, sizeof nm, "1 layer %d (uniform), NEAREST", l);
      want_layer(nm, 32, 32, l);
   }
   glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glUniform1f(zl, 1.4f);
   quad();
   want_layer("1 z 1.4, LINEAR: layer 1 (no blend)", 32, 32, 1);
   glUniform1f(zl, 1.6f);
   quad();
   want_layer("1 z 1.6: layer 2", 32, 32, 2);
   glUniform1f(zl, -3.0f);
   quad();
   want_layer("1 z -3: layer 0 (clamped)", 32, 32, 0);
   glUniform1f(zl, 9.0f);
   quad();
   want_layer("1 z 9: layer 3 (clamped)", 32, 32, 3);

   /* 2 */
   GLuint pv = prog("#version 300 es\nprecision highp float;\n"
                    "uniform highp sampler2DArray s;\nin vec2 uv;\nout vec4 o;\n"
                    "void main() { o = texture(s, vec3(uv, floor(uv.x * 4.0))); }\n");
   glUseProgram(pv);
   glUniform1i(glGetUniformLocation(pv, "s"), 0);
   quad();
   for (int l = 0; l < 4; l++) {
      char nm[64];
      snprintf(nm, sizeof nm, "2 layer per pixel: column %d", l);
      want_layer(nm, l * 16 + 8, 32, l);
   }

   /* 3 */
   GLuint pf = prog("#version 300 es\nprecision highp float;\n"
                    "uniform highp sampler2DArray s;\nin vec2 uv;\nout vec4 o;\n"
                    "void main() { o = texelFetch(s, ivec3(3, 5, 2), 0); }\n");
   glUseProgram(pf);
   glUniform1i(glGetUniformLocation(pf, "s"), 0);
   quad();
   want_layer("3 texelFetch layer 2", 32, 32, 2);

   /* 4 */
   GLuint ps = prog("#version 300 es\nprecision highp float;\n"
                    "uniform highp sampler2DArray s;\nin vec2 uv;\nout vec4 o;\n"
                    "void main() { ivec3 q = textureSize(s, 0);"
                    " o = vec4(vec3(q) / 255.0, 1.0); }\n");
   glUseProgram(ps);
   glUniform1i(glGetUniformLocation(ps, "s"), 0);
   quad();
   check("4 textureSize (8, 8, 4)", 32, 32, 8, 8, 4, 255, 0);

   /* 5 */
   GLuint tm = array_tex(16, 16, 3, 2);
   GLuint pl = prog("#version 300 es\nprecision highp float;\n"
                    "uniform highp sampler2DArray s;\nin vec2 uv;\nout vec4 o;\n"
                    "void main() { o = textureLod(s, vec3(uv, 2.0), 1.0); }\n");
   glUseProgram(pl);
   glUniform1i(glGetUniformLocation(pl, "s"), 0);
   quad();
   want_layer("5 mipmapped: layer 2, level 1", 32, 32, 2 + 16);
   (void)tm;

   /* 6 */
   {
      GLuint t6 = array_tex(32, 32, 3, 1);
      GLuint f6;
      glGenFramebuffers(1, &f6);
      glBindFramebuffer(GL_FRAMEBUFFER, f6);
      glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, t6, 0, 1);
      GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      if (st != GL_FRAMEBUFFER_COMPLETE) {
         printf("6 layer FBO status 0x%x: FAIL\n", st);
         fails++;
      } else {
         glViewport(0, 0, 32, 32);
         glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
         glClear(GL_COLOR_BUFFER_BIT);
         glViewport(0, 0, W, H);
         glBindFramebuffer(GL_FRAMEBUFFER, fb);
         glUseProgram(pu);
         glBindTexture(GL_TEXTURE_2D_ARRAY, t6);
         glUniform1f(zl, 1.0f);
         quad();
         check("6a rendered layer 1", 32, 32, 255, 0, 255, 255, 1);
         glUniform1f(zl, 0.0f);
         quad();
         want_layer("6b layer 0 kept", 32, 32, 0);
         glUniform1f(zl, 2.0f);
         quad();
         want_layer("6c layer 2 kept", 32, 32, 2);
      }
      glBindFramebuffer(GL_FRAMEBUFFER, fb);
   }

   /* 7 */
   {
      GLuint t7 = array_tex(6, 5, 3, 1);
      glUseProgram(pu);
      glBindTexture(GL_TEXTURE_2D_ARRAY, t7);
      glUniform1f(zl, 2.0f);
      quad();
      want_layer("7 NPOT 6x5: layer 2", 32, 32, 2);
   }

   /* 8 */
   {
      GLuint t8 = array_tex(16, 16, 64, 1);
      glUseProgram(pu);
      glBindTexture(GL_TEXTURE_2D_ARRAY, t8);
      glUniform1f(zl, 63.0f);
      quad();
      want_layer("8 64 layers: layer 63", 32, 32, 63);
      glUniform1f(zl, 31.0f);
      quad();
      want_layer("8 64 layers: layer 31", 32, 32, 31);
   }

   /* 9 */
   {
      GLuint t9;
      glGenTextures(1, &t9);
      glBindTexture(GL_TEXTURE_2D_ARRAY, t9);
      glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_DEPTH_COMPONENT32F, 8, 8, 3);
      float dz[64];
      for (int l = 0; l < 3; l++) {
         for (int i = 0; i < 64; i++)
            dz[i] = 0.25f * (l + 1);
         glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, l, 8, 8, 1,
                         GL_DEPTH_COMPONENT, GL_FLOAT, dz);
      }
      glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE,
                      GL_COMPARE_REF_TO_TEXTURE);
      glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
      GLuint p9 = prog("#version 300 es\nprecision highp float;\n"
                       "uniform highp sampler2DArrayShadow s;\nin vec2 uv;\n"
                       "out vec4 o;\n"
                       "void main() { o = vec4(texture(s, vec4(uv, 1.0, 0.6)),"
                       " texture(s, vec4(uv, 1.0, 0.4)), 0.0, 1.0); }\n");
      glUseProgram(p9);
      glUniform1i(glGetUniformLocation(p9, "s"), 0);
      quad();
      /* layer 1 holds 0.5: 0.6 <= 0.5 false, 0.4 <= 0.5 true */
      check("9 shadow array: layer 1 (0.5) vs 0.6 / 0.4", 32, 32, 0, 255, 0,
            255, 1);
   }

   /* 10 */
   {
      GLuint t10 = array_tex(4, 4, 8, 1);
      GLuint p10 = prog("#version 300 es\nprecision highp float;\n"
                        "uniform highp sampler2DArray s;\nin vec2 uv;\n"
                        "out vec4 o;\n"
                        "void main() { o = texture(s, vec3(uv,"
                        " mod(floor(gl_FragCoord.x / 3.0), 8.0))); }\n");
      static unsigned char px[W * H * 4];
      int bad = 0, fx = -1, fy = -1;

      glUseProgram(p10);
      glUniform1i(glGetUniformLocation(p10, "s"), 0);
      glBindTexture(GL_TEXTURE_2D_ARRAY, t10);
      quad();
      glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
      for (int y = 0; y < H; y++)
         for (int x = 0; x < W; x++) {
            unsigned char c[4];
            const unsigned char *q = px + 4 * (y * W + x);

            lc((x / 3) % 8, c);
            if (abs(q[0] - c[0]) > 1 || abs(q[1] - c[1]) > 1 ||
                abs(q[2] - c[2]) > 1) {
               if (!bad++)
                  fx = x, fy = y;
            }
         }
      printf("10 layer floor(x / 3) inside a quad: %d of %d pixels wrong"
             " (first at %d,%d): %s\n", bad, W * H, fx, fy,
             bad ? "FAIL" : "OK");
      fails += bad != 0;
   }

   GLenum e = glGetError();
   printf("GL error 0x%x\n", e);
   printf("result: %d failed\n", fails + (e != GL_NO_ERROR));
   return fails || e != GL_NO_ERROR;
}
