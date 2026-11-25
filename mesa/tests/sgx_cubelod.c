/*
 * Copyright © 2026 servidorjarvis1-svg
 * SPDX-License-Identifier: MIT
 */

/*
 * The mip level a dependent cube sample takes along the diagonal two
 * triangles share (sgx_program.c lower_dep_lod).  A 64x64 cube, every face
 * of level L one solid colour, drawn as a quad of two triangles looking
 * down +Z, with NEAREST_MIPMAP_NEAREST; the quad is 16x16 and 32x32 pixels,
 * so the LOD is about 2 and 1.  The reference is the sample of the varying
 * as it is (the PDS samples it); against it:
 *  1. textureCube(s, d * one): the coordinate computed in the program
 *  2. textureCube(s, vec3(-1, ..., ...)): a face's direction built from a
 *     vec2 in the program, mediump (dEQP's texture.specification cube shader)
 * Every pixel is compared; GLES2, SGX_DEBUG unset.
 *
 *   sgx_cubelod
 *
 * zig cc -target x86-linux-gnu.2.36 -isystem <root>/usr/include -o sgx_cubelod
 * sgx_cubelod.c <root>/usr/lib/i386-linux-gnu/libEGL.so.1
 * <root>/usr/lib/i386-linux-gnu/libGLESv2.so.2
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define T 64

static const unsigned char col[7][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255},
                                        {255, 255, 0}, {0, 255, 255},
                                        {255, 0, 255}, {255, 255, 255}};

static GLuint
prog(const char *fs)
{
   const char *vs = "attribute vec2 a; varying vec3 d;\n"
                    "void main(){ d = vec3(a * 0.9, 1.0);"
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
         char l[1024];
         glGetShaderInfoLog(s, sizeof l, NULL, l);
         printf("compile: %s\n", l);
      }
      glAttachShader(p, s);
   }
   glBindAttribLocation(p, 0, "a");
   glLinkProgram(p);
   return p;
}

/* draw p into a vw x vw viewport and read the 64x64 surface back */
static void
draw(GLuint p, int vw, unsigned char *px)
{
   glUseProgram(p);
   glUniform1i(glGetUniformLocation(p, "s"), 0);
   glUniform1f(glGetUniformLocation(p, "one"), 1.0f);
   glViewport(0, 0, vw, vw);
   glClearColor(0, 0, 0, 1);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLES, 0, 6);
   glReadPixels(0, 0, W, W, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

int
main(void)
{
   EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                  EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_NONE};
   EGLint pa[] = {EGL_WIDTH, W, EGL_HEIGHT, W, EGL_NONE};
   EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
   EGLConfig c;
   EGLint n;

   if (!eglInitialize(d, NULL, NULL)) {
      printf("eglInitialize failed\n");
      return 1;
   }
   eglBindAPI(EGL_OPENGL_ES_API);
   eglChooseConfig(d, ca, &c, 1, &n);
   EGLSurface s = eglCreatePbufferSurface(d, c, pa);
   EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa);
   if (!eglMakeCurrent(d, s, s, x)) {
      printf("no context 0x%x\n", eglGetError());
      return 1;
   }

   GLuint t;
   static unsigned char buf[T * T * 4];
   glGenTextures(1, &t);
   glBindTexture(GL_TEXTURE_CUBE_MAP, t);
   for (int l = 0; (T >> l) > 0; l++) {
      int sz = T >> l;

      for (int i = 0; i < sz * sz; i++) {
         memcpy(buf + 4 * i, col[l], 3);
         buf[4 * i + 3] = 255;
      }
      for (int f = 0; f < 6; f++)
         glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, l, GL_RGBA, sz, sz, 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, buf);
   }
   glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER,
                   GL_NEAREST_MIPMAP_NEAREST);
   glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

   static const float q[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, q);
   glEnableVertexAttribArray(0);

   GLuint ref = prog("precision mediump float; uniform samplerCube s;"
                     " varying vec3 d;\n"
                     "void main(){ gl_FragColor = textureCube(s, d); }\n");
   static const struct {
      const char *name, *fs;
   } cs[] = {
      {"1 d * one",
       "precision mediump float; uniform samplerCube s; uniform float one;"
       " varying vec3 d;\n"
       "void main(){ gl_FragColor = textureCube(s, d * one); }\n"},
      /* the same direction: v = d.xy * 0.5 + 0.5 gives (-1, -(2v.y - 1),
       * 2v.x - 1) = (-1, -d.y, d.x), the -X face's, which looks like +Z's
       * here: compare its level, not its texel */
      {"2 a face built from a vec2",
       "precision mediump float; uniform samplerCube s; varying vec3 d;\n"
       "void main(){ vec2 v = d.xy * 0.5 + 0.5;"
       " gl_FragColor = textureCube(s, vec3(-1.0, -(v.y * 2.0 - 1.0),"
       " v.x * 2.0 - 1.0)); }\n"},
   };
   static const int vws[] = {16, 32};
   static unsigned char a[W * W * 4], b[W * W * 4];
   int fails = 0;

   for (unsigned k = 0; k < sizeof(cs) / sizeof(cs[0]); k++) {
      GLuint p = prog(cs[k].fs);

      for (unsigned v = 0; v < 2; v++) {
         int vw = vws[v], bad = 0, ondiag = 0;

         draw(ref, vw, a);
         draw(p, vw, b);
         for (int y = 0; y < vw; y++)
            for (int xx = 0; xx < vw; xx++) {
               const unsigned char *pa_ = a + 4 * (y * W + xx),
                                   *pb = b + 4 * (y * W + xx);
               if (memcmp(pa_, pb, 3)) {
                  int s0 = (xx & ~1) + (y & ~1);

                  bad++;
                  ondiag += s0 <= vw - 1 && vw - 1 <= s0 + 2;
               }
            }
         printf("%-28s %2dx%-2d: %4d of %4d pixels differ (%d in quads on the"
                " diagonal): %s\n",
                cs[k].name, vw, vw, bad, vw * vw, ondiag, bad ? "FAIL" : "OK");
         fails += bad != 0;
      }
   }
   GLenum e = glGetError();
   printf("GL error 0x%x\n", e);
   printf("result: %d failed\n", fails + (e != GL_NO_ERROR));
   return fails || e != GL_NO_ERROR;
}
