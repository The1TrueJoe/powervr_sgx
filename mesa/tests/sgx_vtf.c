/*
 * Copyright © 2026 servidorjarvis1-svg
 * SPDX-License-Identifier: MIT
 */

/*
 * Vertex texture fetch (not with SGX_DEBUG=novtf or noglsl130, docs/GL3.md),
 * checked by reading pixels back from a 64x64 FBO:
 *  0. GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS
 *  1. colour per quad from a 4x1 RGBA8 texture read in the vertex shader
 * (NEAREST)
 *  2. the vertex sampler's filter honoured: LINEAR halfway between black and
 * white
 *  3. displacement (terrain): the quad's top edge from the texel's red
 *  4. vertex unit 0 and fragment unit 0 bound to different textures
 *  5. (ES 3.0 context) texelFetch and textureSize in the vertex shader
 *  6. (ES 3.0 context) textureLod at level 1 of a 2x2 texture: informative,
 *     a texture under 16 on a side that may be rendered into is linear and
 *     has no mipmaps (sgx_tex_twiddled)
 *  7. (ES 3.0 context) textureLod at levels 0-3 of a 32x32 texture, in the
 *     vertex shader (checked) and the fragment shader (for comparison)
 *  8. (ES 3.0 context) implicit LOD in a fragment shader (for comparison)
 *
 *   MESA_GLES_VERSION_OVERRIDE=3.0 DISPLAY=:0 sgx_vtf
 *
 * zig cc -target x86-linux-gnu.2.36 -isystem <root>/usr/include -o sgx_vtf
 * sgx_vtf.c <root>/usr/lib/i386-linux-gnu/libEGL.so.1
 * <root>/usr/lib/i386-linux-gnu/libGLESv2.so.2
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#define W 64
#define H 64

static GLuint
prog(const char *vs, const char *fs)
{
   GLuint p = glCreateProgram();
   const char *src[2] = {vs, fs};
   GLenum ty[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
   for (int i = 0; i < 2; i++) {
      GLuint s = glCreateShader(ty[i]);
      glShaderSource(s, 1, &src[i], NULL);
      glCompileShader(s);
      GLint ok;
      glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
      if (!ok) {
         char l[2048];
         glGetShaderInfoLog(s, sizeof l, NULL, l);
         printf("compile: %s\n", l);
      }
      glAttachShader(p, s);
   }
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
px(int x, int y, unsigned char *o)
{
   glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, o);
}

static int
near(const unsigned char *p, int r, int g, int b, int tol)
{
   return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol;
}

static GLuint
tex(int w, int h, const unsigned char *rgba, GLenum filter)
{
   GLuint t;
   glGenTextures(1, &t);
   glBindTexture(GL_TEXTURE_2D, t);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                rgba);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   return t;
}

/* 4 quads side by side, each vertex carrying (x, y, u) */
static GLuint
quads(void)
{
   float v[4 * 6 * 3];
   int k = 0;
   for (int q = 0; q < 4; q++) {
      float x0 = -1 + q * 0.5f, x1 = x0 + 0.5f, u = (q + 0.5f) / 4;
      float c[6][2] = {{x0, -1}, {x1, -1}, {x0, 1}, {x0, 1}, {x1, -1}, {x1, 1}};
      for (int i = 0; i < 6; i++) {
         v[k++] = c[i][0];
         v[k++] = c[i][1];
         v[k++] = u;
      }
   }
   GLuint b;
   glGenBuffers(1, &b);
   glBindBuffer(GL_ARRAY_BUFFER, b);
   glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
   return b;
}

static void
attribs(GLuint p)
{
   GLint a = glGetAttribLocation(p, "a");
   glEnableVertexAttribArray(a);
   glVertexAttribPointer(a, 3, GL_FLOAT, GL_FALSE, 12, 0);
}

static const char *FS2 =
   "precision mediump float;\nvarying vec4 c;\nvoid main(){ gl_FragColor = c; }\n";

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
   const EGLint xa2[] = {EGL_CONTEXT_MAJOR_VERSION, 2, EGL_NONE};
   EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa3);
   int es3 = c != EGL_NO_CONTEXT;
   if (!es3)
      c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa2);
   if (c == EGL_NO_CONTEXT ||
       !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
      printf("no context 0x%x\n", eglGetError());
      return 1;
   }
   printf("%s (%s)\n", glGetString(GL_VERSION), es3 ? "ES3" : "ES2");

   GLuint rb, fb;
   glGenRenderbuffers(1, &rb);
   glBindRenderbuffer(GL_RENDERBUFFER, rb);
   glRenderbufferStorage(GL_RENDERBUFFER, es3 ? GL_RGBA8 : GL_RGBA4, W, H);
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_RENDERBUFFER, rb);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      printf("fbo incomplete\n");
      return 1;
   }
   glViewport(0, 0, W, H);
   int tol = es3 ? 3 : 18; /* RGBA4 without ES 3.0 */
   int fails = 0;
   unsigned char p[4];

   /* 0 */
   GLint vu = 0, cu = 0;
   glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &vu);
   glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &cu);
   printf("0 MAX_VERTEX_TEXTURE_IMAGE_UNITS %d, COMBINED %d: %s\n", vu, cu,
          vu >= 16 ? "OK" : "FAIL");
   if (vu < 1) {
      printf("result: no vertex texture units\n");
      return 1;
   }
   fails += vu < 16;

   quads();

   /* 1 */
   const unsigned char t4[] = {255, 0, 0,   255, 0,   255, 0,   255,
                               0,   0, 255, 255, 255, 255, 255, 255};
   GLuint tx4 = tex(4, 1, t4, GL_NEAREST);
   GLuint p1 = prog(
      "attribute vec3 a;\nuniform sampler2D t;\nvarying vec4 c;\n"
      "void main(){ gl_Position = vec4(a.xy, 0.0, 1.0); c = texture2DLod(t, vec2(a.z, 0.5), 0.0); }\n",
      FS2);
   glUseProgram(p1);
   attribs(p1);
   glUniform1i(glGetUniformLocation(p1, "t"), 0);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, tx4);
   glClearColor(0, 0, 0, 1);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLES, 0, 24);
   int ok1 = 1;
   for (int q = 0; q < 4; q++) {
      px(q * 16 + 8, H / 2, p);
      int r = near(p, t4[4 * q], t4[4 * q + 1], t4[4 * q + 2], tol);
      ok1 &= r;
      printf("   quad %d: %d,%d,%d (want %d,%d,%d)\n", q, p[0], p[1], p[2],
             t4[4 * q], t4[4 * q + 1], t4[4 * q + 2]);
   }
   printf("1 a colour a vertex from a NEAREST texture: %s\n",
          ok1 ? "OK" : "FAIL");
   fails += !ok1;

   /* 2: LINEAR at u = 0.5 of black|white = mid grey */
   const unsigned char t2[] = {0, 0, 0, 255, 255, 255, 255, 255};
   GLuint tx2 = tex(2, 1, t2, GL_LINEAR);
   GLuint p2 = prog(
      "attribute vec3 a;\nuniform sampler2D t;\nvarying vec4 c;\n"
      "void main(){ gl_Position = vec4(a.xy, 0.0, 1.0); c = texture2DLod(t, vec2(0.5, 0.5), 0.0); }\n",
      FS2);
   glUseProgram(p2);
   attribs(p2);
   glBindTexture(GL_TEXTURE_2D, tx2);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLES, 0, 24);
   px(W / 2, H / 2, p);
   int ok2 = near(p, 128, 128, 128, tol + 4);
   printf("2 the vertex sampler's LINEAR filter: %s (%d,%d,%d, want ~128)\n",
          ok2 ? "OK" : "FAIL", p[0], p[1], p[2]);
   fails += !ok2;

   /* 3: displacement: top edge of each quad at y = 2 * red - 1 (heights 0,
    * 0.25, 0.5, 1) */
   const unsigned char th[] = {0,   0, 0, 255, 64,  0, 0, 255,
                               128, 0, 0, 255, 255, 0, 0, 255};
   GLuint txh = tex(4, 1, th, GL_NEAREST);
   GLuint p3 = prog(
      "attribute vec3 a;\nuniform sampler2D t;\nvarying vec4 c;\n"
      "void main(){ float h = texture2DLod(t, vec2(a.z, 0.5), 0.0).r;\n"
      " float y = a.y > 0.0 ? 2.0 * h - 1.0 : -1.0;\n"
      " gl_Position = vec4(a.x, y, 0.0, 1.0); c = vec4(1.0, 1.0, 0.0, 1.0); }\n",
      FS2);
   glUseProgram(p3);
   attribs(p3);
   glBindTexture(GL_TEXTURE_2D, txh);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLES, 0, 24);
   int ok3 = 1;
   for (int q = 0; q < 4; q++) {
      int top = 0;
      for (int y = 0; y < H; y++) {
         px(q * 16 + 8, y, p);
         if (p[0] > 128)
            top = y + 1;
      }
      int want = (int)(th[4 * q] / 255.0 * H + 0.5);
      ok3 &= abs(top - want) <= 2;
      printf("   quad %d: height %d px (want %d)\n", q, top, want);
   }
   printf("3 displacement by a height texture (terrain): %s\n",
          ok3 ? "OK" : "FAIL");
   fails += !ok3;

   /* 4: vertex unit 0 = tx4 (colour), fragment unit 1 = tx2 (grey) multiplied */
   GLuint p4 = prog(
      "attribute vec3 a;\nuniform sampler2D tv;\nvarying vec4 c;\n"
      "void main(){ gl_Position = vec4(a.xy, 0.0, 1.0); c = texture2DLod(tv, vec2(a.z, 0.5), 0.0); }\n",
      "precision mediump float;\nuniform sampler2D tf;\nvarying vec4 c;\n"
      "void main(){ gl_FragColor = c * texture2D(tf, vec2(0.75, 0.5)); }\n");
   glUseProgram(p4);
   attribs(p4);
   glUniform1i(glGetUniformLocation(p4, "tv"), 0);
   glUniform1i(glGetUniformLocation(p4, "tf"), 1);
   glActiveTexture(GL_TEXTURE1);
   glBindTexture(GL_TEXTURE_2D, tx2);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, tx4);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLES, 0, 24);
   int ok4 = 1;
   for (int q = 0; q < 4; q++) {
      px(q * 16 + 8, H / 2, p);
      ok4 &= near(p, t4[4 * q], t4[4 * q + 1], t4[4 * q + 2], tol);
   }
   printf("4 vertex and fragment units apart: %s\n", ok4 ? "OK" : "FAIL");
   fails += !ok4;

   if (es3) {
      /* 5: texelFetch + textureSize */
      GLuint p5 = prog(
         "#version 300 es\nin vec3 a;\nuniform sampler2D t;\nout vec4 c;\n"
         "void main(){ ivec2 s = textureSize(t, 0); int i = int(a.z * 4.0);\n"
         " gl_Position = vec4(a.xy, 0.0, 1.0);\n"
         " c = s == ivec2(4, 1) ? texelFetch(t, ivec2(3 - i, 0), 0) : vec4(1.0, 0.0, 1.0, 1.0); }\n",
         "#version 300 es\nprecision mediump float;\nin vec4 c;\nout vec4 o;\nvoid main(){ o = c; }\n");
      glUseProgram(p5);
      attribs(p5);
      glUniform1i(glGetUniformLocation(p5, "t"), 0);
      glBindTexture(GL_TEXTURE_2D, tx4);
      glClear(GL_COLOR_BUFFER_BIT);
      glDrawArrays(GL_TRIANGLES, 0, 24);
      int ok5 = 1;
      for (int q = 0; q < 4; q++) {
         px(q * 16 + 8, H / 2, p);
         int r = near(p, t4[4 * (3 - q)], t4[4 * (3 - q) + 1],
                      t4[4 * (3 - q) + 2], tol);
         ok5 &= r;
         printf("   quad %d: %d,%d,%d\n", q, p[0], p[1], p[2]);
      }
      printf("5 texelFetch and textureSize in a vertex shader: %s\n",
             ok5 ? "OK" : "FAIL");
      fails += !ok5;

      /* 6: textureLod level 1 of a 2x2 mipmapped texture: level 0 red, level 1
       * green */
      GLuint tm;
      glGenTextures(1, &tm);
      glBindTexture(GL_TEXTURE_2D, tm);
      const unsigned char l0[16] = {255, 0, 0, 255, 255, 0, 0, 255,
                                    255, 0, 0, 255, 255, 0, 0, 255};
      const unsigned char l1[4] = {0, 255, 0, 255};
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, l0);
      glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 1, 1, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, l1);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                      GL_NEAREST_MIPMAP_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      GLuint p6 = prog(
         "#version 300 es\nin vec3 a;\nuniform sampler2D t;\nuniform float lod;\nout vec4 c;\n"
         "void main(){ gl_Position = vec4(a.xy, 0.0, 1.0); c = textureLod(t, vec2(0.5), lod); }\n",
         "#version 300 es\nprecision mediump float;\nin vec4 c;\nout vec4 o;\nvoid main(){ o = c; }\n");
      glUseProgram(p6);
      attribs(p6);
      glUniform1i(glGetUniformLocation(p6, "t"), 0);
      int ok6 = 1;
      for (int lv = 0; lv < 2; lv++) {
         glUniform1f(glGetUniformLocation(p6, "lod"), (float)lv);
         glClear(GL_COLOR_BUFFER_BIT);
         glDrawArrays(GL_TRIANGLES, 0, 24);
         px(W / 2, H / 2, p);
         int r = lv ? near(p, 0, 255, 0, tol) : near(p, 255, 0, 0, tol);
         ok6 &= r;
         printf("   lod %d: %d,%d,%d\n", lv, p[0], p[1], p[2]);
      }
      printf(
         "6 textureLod level 0/1 of a 2x2 texture in a vertex shader (informative): %s\n",
         ok6 ? "OK" : "level 0");

      /* 7 (control, not VTF): the same textureLod in a fragment shader, and
       * an 8x8 texture's level 1 / 2 in both stages */
      GLuint p7 = prog(
         "#version 300 es\nin vec3 a;\nvoid main(){ gl_Position = vec4(a.xy, 0.0, 1.0); }\n",
         "#version 300 es\nprecision highp float;\nuniform sampler2D t;\nuniform float lod;\nout vec4 o;\n"
         "void main(){ o = textureLod(t, vec2(0.5), lod); }\n");
      glUseProgram(p7);
      attribs(p7);
      glUniform1i(glGetUniformLocation(p7, "t"), 0);
      for (int lv = 0; lv < 2; lv++) {
         glUniform1f(glGetUniformLocation(p7, "lod"), (float)lv);
         glClear(GL_COLOR_BUFFER_BIT);
         glDrawArrays(GL_TRIANGLES, 0, 24);
         px(W / 2, H / 2, p);
         printf("   7 fragment 2x2 lod %d: %d,%d,%d\n", lv, p[0], p[1], p[2]);
      }
      /* (32x32: a texture under 16 on a side that may be rendered into is
       * linear, so it has no mipmaps on this driver: sgx_tex_twiddled) */
      GLuint t8;
      glGenTextures(1, &t8);
      glBindTexture(GL_TEXTURE_2D, t8);
      static unsigned char lv8[4][32 * 32 * 4];
      const unsigned char col[4][3] = {{255, 0, 0},
                                       {0, 255, 0},
                                       {0, 0, 255},
                                       {255, 255, 0}};
      for (int l = 0; l < 4; l++) {
         int s8 = 32 >> l;
         for (int i = 0; i < s8 * s8; i++) {
            lv8[l][4 * i] = col[l][0];
            lv8[l][4 * i + 1] = col[l][1];
            lv8[l][4 * i + 2] = col[l][2];
            lv8[l][4 * i + 3] = 255;
         }
         glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA, s8, s8, 0, GL_RGBA,
                      GL_UNSIGNED_BYTE, lv8[l]);
      }
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                      GL_NEAREST_MIPMAP_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 3);
      for (int st = 0; st < 2; st++) {
         GLuint pp = st ? p7 : p6;
         glUseProgram(pp);
         attribs(pp);
         for (int lv = 0; lv < 4; lv++) {
            glUniform1f(glGetUniformLocation(pp, "lod"), (float)lv);
            glClear(GL_COLOR_BUFFER_BIT);
            glDrawArrays(GL_TRIANGLES, 0, 24);
            px(W / 2, H / 2, p);
            int r = near(p, col[lv][0], col[lv][1], col[lv][2], tol);
            if (!st)
               fails += !r;
            printf("   7 %s 32x32 lod %d: %d,%d,%d (want %d,%d,%d) %s\n",
                   st ? "fragment" : "vertex", lv, p[0], p[1], p[2], col[lv][0],
                   col[lv][1], col[lv][2], r ? "OK" : "FAIL");
         }
      }
   }

   if (es3) {
      /* 8 (control): implicit LOD in a fragment shader: the 32x32 texture
       * drawn into smaller viewports (16: level 1, 8: level 2, 4: level 3) */
      GLuint p8 = prog(
         "#version 300 es\nin vec3 a;\nout vec2 uv;\nvoid main(){ gl_Position = vec4(a.xy, 0.0, 1.0); uv = a.xy * 0.5 + 0.5; }\n",
         "#version 300 es\nprecision highp float;\nuniform sampler2D t;\nin vec2 uv;\nout vec4 o;\n"
         "void main(){ o = texture(t, uv); }\n");
      glUseProgram(p8);
      attribs(p8);
      glUniform1i(glGetUniformLocation(p8, "t"), 0);
      for (int vp = 64; vp >= 1; vp /= 2) {
         glViewport(0, 0, vp, vp);
         glClear(GL_COLOR_BUFFER_BIT);
         glDrawArrays(GL_TRIANGLES, 0, 24);
         px(0, 0, p);
         printf("   8 fragment, implicit LOD, viewport %dx%d: %d,%d,%d\n", vp,
                vp, p[0], p[1], p[2]);
      }
      glViewport(0, 0, W, H);
   }
   printf("glGetError 0x%x\nresult: %d failed\n", glGetError(), fails);
   return fails;
}
