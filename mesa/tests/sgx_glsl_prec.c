/* sgx_glsl_prec: GLSL ES 1.00 default precision statements, as ioquake3's
 * OpenGL 2 renderer with r_PreferOpenglES 1 writes them: a default precision
 * for sampler2DShadow, with and without #extension GL_EXT_shadow_samplers
 * (mesa/patches/0007), against sampler2D (allowed) and vec4 (an error).
 * Surfaceless EGL, GLES2.
 *
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -o sgx_glsl_prec sgx_glsl_prec.c -ldl
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#define F(r, n, a) static r(*n##_) a;
F(EGLDisplay, eglGetPlatformDisplay, (EGLenum, void *, const EGLAttrib *))
F(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *))
F(EGLBoolean, eglBindAPI, (EGLenum))
F(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))
F(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *))
F(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))
F(GLuint, glCreateShader, (GLenum))
F(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))
F(void, glCompileShader, (GLuint))
F(void, glGetShaderiv, (GLuint, GLenum, GLint *))
F(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))
F(const GLubyte *, glGetString, (GLenum))
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
#define glGetString glGetString_

static int try(const char *name, const char *src)
{
   GLuint s = glCreateShader(GL_VERTEX_SHADER);
   GLint ok = 0;
   char log[512] = "";

   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   glGetShaderInfoLog(s, sizeof(log), NULL, log);
   printf("%-14s %s %s\n", name, ok ? "OK  " : "FAIL", log);
   return ok;
}

int main(void)
{
   void *e = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL), *g = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
   if (!e || !g)
      return printf("no libEGL/libGLESv2\n"), 1;
#define L(h, n) n##_ = dlsym(h, #n)
   L(e, eglGetPlatformDisplay); L(e, eglInitialize); L(e, eglBindAPI); L(e, eglChooseConfig);
   L(e, eglCreateContext); L(e, eglMakeCurrent); L(g, glCreateShader); L(g, glShaderSource);
   L(g, glCompileShader); L(g, glGetShaderiv); L(g, glGetShaderInfoLog); L(g, glGetString);
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
   printf("EXT_shadow_samplers: %s\n",
          strstr((const char *)glGetString(GL_EXTENSIONS), "GL_EXT_shadow_samplers") ? "yes" : "no");
#define BODY "attribute vec4 p; void main() { gl_Position = p; }\n"
   /* ioquake3's header as it is: sampler2DShadow without the #extension is
    * not a type in GLSL ES 1.00, an error by the spec (force_glsl_extensions_warn
    * turns every extension on and makes it compile) */
   int ioq3 = try("ioq3", "#version 100\nprecision mediump float;\nprecision mediump sampler2DShadow;\n" BODY);
   int ext = try("with-ext", "#version 100\n#extension GL_EXT_shadow_samplers : enable\nprecision mediump float;\n"
                             "precision mediump sampler2DShadow;\n" BODY);
   int s2d = try("sampler2D", "#version 100\nprecision mediump float;\nprecision mediump sampler2D;\n" BODY);
   int v4 = try("vec4", "#version 100\nprecision mediump vec4;\n" BODY);
   int force = getenv("force_glsl_extensions_warn") != NULL;
   int pass = ioq3 == force && ext && s2d && !v4;

   printf("%s\n", pass ? "PASS" : "FAIL");
   return !pass;
}
