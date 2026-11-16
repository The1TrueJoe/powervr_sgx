/*
 * Copyright 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 *
 * sgx_fboslow: the same small renders into off-screen framebuffers, round after
 * round, timed. Every round does the same work, so a frame should take the same
 * time in round 30 as in round 3; something that builds up per render shows as
 * frames that get slower (and, on 0.2.1, a job timeout).
 *
 * The workload is a tester's (issue #5): five FBOs from 512x300 to 2048x1200;
 * each round, for each size, 8 frames of a clear and one full-screen blended
 * quad with a glFlush after each, then a glFinish. EGL surfaceless, GLES 2.
 *
 * usage: sgx_fboslow [-z WxH,...] [-s mask] [-f frames] [-R k] [-C k] [-D file] [-K file]
 *                    [-q x0,y0,x1,y1] [-n N] [-T] [rounds]
 *   -z sizes  up to five sizes of its own, in place of the five above
 *   -s mask   the sizes to draw, a bit each from the first (1) to the fifth (16);
 *             default all
 *   -f n      frames per size per round (default 8)
 *   -R k      delete the FBOs and their textures and make them again every k rounds
 *   -C k      a new GL context (and FBOs) every k rounds
 *   -D file   after each round, the lines of file (a driver's debugfs status, say)
 *             that name a DPM register
 *   -K file   after each round, words of file's ta3dctl dump (the driver's debugfs
 *             ctlblk): the out-of-memory state 0x1f, 0x21, 0x22, 0x24 and a UK_DPMLOG
 *             microkernel's DPM samples 0x35..0x39
 *   -n n      n quads a frame (default 1), blended over each other
 *   -T        sgx_gamefix bigfbo's fragment shader: four samples of an 8x8 texture,
 *             added (blend ONE, ONE), instead of the flat half-transparent colour
 *   -q x0,y0,x1,y1  the quad's corners in clip space (default -1,-1,1,1); x0 > x1: no
 *             draw, the clear alone
 *   rounds    default 30
 *
 * It prints the milliseconds a frame per size per round, the renders so far, the
 * system's Shmem and this process's powervr_sgx memory (DRM fdinfo), and FAILs
 * when the last round's frames at a size take more than twice as long as the
 * fastest of rounds 2 to 5 (and over 1 ms more).
 *
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -o sgx_fboslow sgx_fboslow.c -ldl
 */
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#define NSIZES 5
static int sizes[NSIZES][2] = {
   {512, 300}, {724, 424}, {1024, 600}, {1448, 848}, {2048, 1200}};

static void *egl_lib;
static __eglMustCastToProperFunctionPointerType (*get_proc)(const char *);

static void *
sym(const char *name)
{
   void *p = dlsym(egl_lib, name);

   if (!p && get_proc)
      p = (void *)get_proc(name);
   if (!p) {
      fprintf(stderr, "sgx_fboslow: no %s\n", name);
      exit(2);
   }
   return p;
}

#define FN(ret, name, args) static ret(*name##_) args
FN(EGLDisplay, eglGetPlatformDisplay, (EGLenum, void *, const EGLAttrib *));
FN(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *));
FN(EGLBoolean, eglBindAPI, (EGLenum));
FN(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *));
FN(EGLBoolean, eglDestroyContext, (EGLDisplay, EGLContext));
FN(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext));
FN(const GLubyte *, glGetString, (GLenum));
FN(void, glGenTextures, (GLsizei, GLuint *));
FN(void, glDeleteTextures, (GLsizei, const GLuint *));
FN(void, glBindTexture, (GLenum, GLuint));
FN(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *));
FN(void, glTexParameteri, (GLenum, GLenum, GLint));
FN(void, glGenFramebuffers, (GLsizei, GLuint *));
FN(void, glDeleteFramebuffers, (GLsizei, const GLuint *));
FN(void, glBindFramebuffer, (GLenum, GLuint));
FN(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint));
FN(GLenum, glCheckFramebufferStatus, (GLenum));
FN(void, glViewport, (GLint, GLint, GLsizei, GLsizei));
FN(void, glFinish, (void));
FN(void, glFlush, (void));
FN(void, glClear, (GLbitfield));
FN(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat));
FN(void, glDrawArrays, (GLenum, GLint, GLsizei));
FN(GLuint, glCreateShader, (GLenum));
FN(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *));
FN(void, glCompileShader, (GLuint));
FN(GLuint, glCreateProgram, (void));
FN(void, glAttachShader, (GLuint, GLuint));
FN(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *));
FN(void, glLinkProgram, (GLuint));
FN(void, glUseProgram, (GLuint));
FN(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *));
FN(void, glEnableVertexAttribArray, (GLuint));
FN(void, glEnable, (GLenum));
FN(void, glBlendFunc, (GLenum, GLenum));
FN(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *));
FN(GLenum, glGetError, (void));
FN(GLint, glGetUniformLocation, (GLuint, const GLchar *));
FN(void, glUniform1i, (GLint, GLint));

static void
load(void)
{
   egl_lib = dlopen("libEGL.so.1", RTLD_NOW);
   if (!egl_lib) {
      fprintf(stderr, "sgx_fboslow: %s\n", dlerror());
      exit(2);
   }
   get_proc = sym("eglGetProcAddress");
#define L(name) name##_ = sym(#name)
   L(eglGetPlatformDisplay); L(eglInitialize); L(eglBindAPI); L(eglCreateContext);
   L(eglDestroyContext); L(eglMakeCurrent); L(glGetString); L(glGenTextures);
   L(glDeleteTextures); L(glBindTexture); L(glTexImage2D); L(glTexParameteri);
   L(glGenFramebuffers); L(glDeleteFramebuffers); L(glBindFramebuffer);
   L(glFramebufferTexture2D); L(glCheckFramebufferStatus); L(glViewport); L(glFinish);
   L(glFlush); L(glClear); L(glClearColor); L(glDrawArrays); L(glCreateShader);
   L(glShaderSource); L(glCompileShader); L(glCreateProgram); L(glAttachShader);
   L(glBindAttribLocation); L(glLinkProgram); L(glUseProgram); L(glVertexAttribPointer);
   L(glEnableVertexAttribArray); L(glEnable); L(glBlendFunc); L(glReadPixels);
   L(glGetError); L(glGetUniformLocation); L(glUniform1i);
#undef L
}

static double
now_ms(void)
{
   struct timespec t;

   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* the system's Shmem, KiB */
static long
shmem_kb(void)
{
   char line[256];
   long v = -1;
   FILE *f = fopen("/proc/meminfo", "r");

   while (f && fgets(line, sizeof(line), f))
      if (sscanf(line, "Shmem: %ld", &v) == 1)
         break;
   if (f)
      fclose(f);
   return v;
}

/* this process's powervr_sgx memory (drm-total-*), KiB, a client counted once */
static long
drm_kb(void)
{
   long total = 0, seen[16];
   int n_seen = 0;
   DIR *d = opendir("/proc/self/fdinfo");
   struct dirent *e;

   while (d && (e = readdir(d))) {
      char path[64], line[256];
      long client = -1, sum = 0;
      bool ours = false;
      FILE *f;

      snprintf(path, sizeof(path), "/proc/self/fdinfo/%s", e->d_name);
      if (!(f = fopen(path, "r")))
         continue;
      while (fgets(line, sizeof(line), f)) {
         char unit[8] = "";
         long v;

         if (strstr(line, "drm-driver:") && strstr(line, "powervr_sgx"))
            ours = true;
         else if (sscanf(line, "drm-client-id: %ld", &v) == 1)
            client = v;
         else if (!strncmp(line, "drm-total-", 10) &&
                  sscanf(strchr(line, ':') + 1, "%ld %7s", &v, unit) >= 1)
            sum += !strcmp(unit, "MiB") ? v * 1024 : !strcmp(unit, "KiB") ? v : v / 1024;
      }
      fclose(f);
      if (!ours)
         continue;
      for (int i = 0; i < n_seen; i++)
         if (seen[i] == client)
            ours = false;
      if (ours && n_seen < 16) {
         seen[n_seen++] = client;
         total += sum;
      }
   }
   if (d)
      closedir(d);
   return total;
}

/* the lines of path that name a DPM register, on one line */
static void
print_dpm(const char *path)
{
   char line[256];
   FILE *f = fopen(path, "r");

   if (!f)
      return;
   printf("    ");
   while (fgets(line, sizeof(line), f)) {
      if (strncmp(line, "DPM", 3))
         continue;
      line[strcspn(line, "\n")] = 0;
      printf(" %s;", line);
   }
   printf("\n");
   fclose(f);
}

/* words 0x35..0x39 of the ta3dctl block in a ctlblk dump: 8 words a line */
static void
print_ctl(const char *path)
{
   char line[256];
   bool in = false;
   unsigned w = 0, v[8];
   FILE *f = fopen(path, "r");

   if (!f)
      return;
   printf("    ctl");
   while (fgets(line, sizeof(line), f)) {
      if (!strncmp(line, "ta3dctl", 7)) {
         in = true;
         continue;
      }
      if (!in)
         continue;
      int n = sscanf(line, " %x %x %x %x %x %x %x %x", &v[0], &v[1], &v[2], &v[3],
                     &v[4], &v[5], &v[6], &v[7]);
      if (n <= 0)
         break;
      for (int i = 0; i < n; i++, w++)
         if ((w >= 0x35 && w <= 0x39) || w == 0x1f || w == 0x21 || w == 0x22 || w == 0x24)
            printf(" [%#x]=%08x", w, v[i]);
   }
   printf("\n");
   fclose(f);
}

static GLuint tex[NSIZES], fbo[NSIZES];
static GLfloat quad_box[4] = {-1, -1, 1, 1};
static int layers = 1;
static bool textured;

static void
make_fbos(unsigned mask)
{
   for (int s = 0; s < NSIZES; s++) {
      if (!(mask & (1u << s)))
         continue;
      glGenTextures_(1, &tex[s]);
      glBindTexture_(GL_TEXTURE_2D, tex[s]);
      glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA, sizes[s][0], sizes[s][1], 0, GL_RGBA,
                    GL_UNSIGNED_BYTE, NULL);
      glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glGenFramebuffers_(1, &fbo[s]);
      glBindFramebuffer_(GL_FRAMEBUFFER, fbo[s]);
      glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[s], 0);
      if (glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
         fprintf(stderr, "sgx_fboslow: FBO %dx%d incomplete\n", sizes[s][0], sizes[s][1]);
         exit(2);
      }
   }
}

static void
free_fbos(unsigned mask)
{
   glBindFramebuffer_(GL_FRAMEBUFFER, 0);
   for (int s = 0; s < NSIZES; s++) {
      if (!(mask & (1u << s)))
         continue;
      glDeleteFramebuffers_(1, &fbo[s]);
      glDeleteTextures_(1, &tex[s]);
   }
}

static GLuint
shader(GLenum type, const char *src)
{
   GLuint s = glCreateShader_(type);

   glShaderSource_(s, 1, &src, NULL);
   glCompileShader_(s);
   return s;
}

/* a half-transparent full-screen quad, alpha blended over the clear */
static void
setup_state(void)
{
   static GLfloat quad[8];

   quad[0] = quad[4] = quad_box[0];
   quad[1] = quad[3] = quad_box[1];
   quad[2] = quad[6] = quad_box[2];
   quad[5] = quad[7] = quad_box[3];
   GLuint prog = glCreateProgram_();

   glAttachShader_(prog, shader(GL_VERTEX_SHADER,
      "attribute vec2 pos; varying vec2 uv;\n"
      "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }"));
   glAttachShader_(prog, shader(GL_FRAGMENT_SHADER,
      textured ? "precision mediump float; uniform sampler2D t; varying vec2 uv;\n"
                 "void main() { gl_FragColor = 0.25 * (texture2D(t, uv) + texture2D(t, uv.yx) +\n"
                 "   texture2D(t, uv * 2.0) + texture2D(t, 1.0 - uv)) * 0.1; }"
               : "precision mediump float; void main() { gl_FragColor = vec4(1.0, 0.5, 0.0, 0.5); }"));
   glBindAttribLocation_(prog, 0, "pos");
   glLinkProgram_(prog);
   glUseProgram_(prog);
   glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
   glEnableVertexAttribArray_(0);
   glEnable_(GL_BLEND);
   if (textured) {
      /* bigfbo's: an 8x8 pattern, added */
      static unsigned char px[8 * 8 * 4];
      GLuint t;

      for (unsigned i = 0; i < 64; i++) {
         px[i * 4] = (i % 8) * 32;
         px[i * 4 + 1] = (i / 8) * 32;
         px[i * 4 + 2] = i * 4;
         px[i * 4 + 3] = 255;
      }
      glGenTextures_(1, &t);
      glBindTexture_(GL_TEXTURE_2D, t);
      glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
      glUniform1i_(glGetUniformLocation_(prog, "t"), 0);
      glBlendFunc_(GL_ONE, GL_ONE);
   } else {
      glBlendFunc_(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   }
   glClearColor_(0.2f, 0.3f, 0.4f, 1.0f);
}

static double
draw_frames(int s, int frames)
{
   double t0;

   glBindFramebuffer_(GL_FRAMEBUFFER, fbo[s]);
   glViewport_(0, 0, sizes[s][0], sizes[s][1]);
   glFinish_();
   t0 = now_ms();
   for (int i = 0; i < frames; i++) {
      glClear_(GL_COLOR_BUFFER_BIT);
      for (int l = 0; l < layers && quad_box[0] < quad_box[2]; l++)
         glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
      glFlush_();
   }
   glFinish_();
   return (now_ms() - t0) / frames;
}

int
main(int argc, char **argv)
{
   static const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
   unsigned mask = 31;
   int frames = 8, rounds = 30, refbo = 0, rectx = 0, opt, big = -1;
   const char *dpm = NULL, *ctl = NULL;
   double best[NSIZES], last[NSIZES];
   long renders = 0;
   EGLDisplay dpy;
   EGLContext ctx;
   bool fail = false;

   while ((opt = getopt(argc, argv, "z:s:f:R:C:D:K:q:n:T")) != -1) {
      switch (opt) {
      case 'z': {
         char *p = optarg;
         unsigned n = 0;

         while (n < NSIZES && sscanf(p, "%dx%d", &sizes[n][0], &sizes[n][1]) == 2) {
            n++;
            if (!(p = strchr(p, ',')))
               break;
            p++;
         }
         if (!n)
            return 2;
         mask &= (1u << n) - 1;
         break;
      }
      case 's': mask = strtoul(optarg, NULL, 0) & 31; break;
      case 'f': frames = atoi(optarg); break;
      case 'R': refbo = atoi(optarg); break;
      case 'C': rectx = atoi(optarg); break;
      case 'D': dpm = optarg; break;
      case 'K': ctl = optarg; break;
      case 'n': layers = atoi(optarg); break;
      case 'T': textured = true; break;
      case 'q':
         if (sscanf(optarg, "%f,%f,%f,%f", &quad_box[0], &quad_box[1], &quad_box[2],
                    &quad_box[3]) != 4)
            return 2;
         break;
      default:
         fprintf(stderr, "usage: sgx_fboslow [-z WxH,...] [-s mask] [-f frames] [-R k] [-C k] "
                         "[-D file] [-K file] [-q x0,y0,x1,y1] [-n N] [-T] [rounds]\n");
         return 2;
      }
   }
   if (optind < argc)
      rounds = atoi(argv[optind]);
   if (!mask || frames < 1 || rounds < 1)
      return 2;

   load();
   dpy = eglGetPlatformDisplay_(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   if (!eglInitialize_(dpy, NULL, NULL)) {
      fprintf(stderr, "sgx_fboslow: eglInitialize failed\n");
      return 2;
   }
   eglBindAPI_(EGL_OPENGL_ES_API);
   ctx = eglCreateContext_(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
   if (!eglMakeCurrent_(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
      fprintf(stderr, "sgx_fboslow: eglMakeCurrent failed\n");
      return 2;
   }
   printf("sgx_fboslow: %s; sizes 0x%x, %d frames a size a round, %d rounds",
          glGetString_(GL_RENDERER), mask, frames, rounds);
   if (refbo)
      printf(", new FBOs every %d", refbo);
   if (rectx)
      printf(", a new context every %d", rectx);
   printf("\n");
   make_fbos(mask);
   setup_state();

   printf("round");
   for (int s = 0; s < NSIZES; s++) {
      best[s] = 1e9;
      if (mask & (1u << s)) {
         printf("  %4dx%-4d", sizes[s][0], sizes[s][1]);
         big = s;
      }
   }
   printf("  renders  Shmem KiB  sgx KiB   (ms a frame)\n");
   for (int r = 1; r <= rounds; r++) {
      if (rectx && r > 1 && (r - 1) % rectx == 0) {
         free_fbos(mask);
         eglMakeCurrent_(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
         eglDestroyContext_(dpy, ctx);
         ctx = eglCreateContext_(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
         eglMakeCurrent_(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
         make_fbos(mask);
         setup_state();
      } else if (refbo && r > 1 && (r - 1) % refbo == 0) {
         free_fbos(mask);
         make_fbos(mask);
      }
      printf("%5d", r);
      for (int s = 0; s < NSIZES; s++) {
         if (!(mask & (1u << s)))
            continue;
         last[s] = draw_frames(s, frames);
         renders += frames;
         /* the first round makes the shaders and the datasets; a reset's
          * lost frames later would look fast */
         if (r > 1 && r <= 5 && last[s] < best[s])
            best[s] = last[s];
         printf("  %9.1f", last[s]);
      }
      printf("  %7ld  %9ld  %7ld\n", renders, shmem_kb(), drm_kb());
      if (dpm)
         print_dpm(dpm);
      if (ctl)
         print_ctl(ctl);
      fflush(stdout);
   }

   {
      GLubyte px[4] = {0};

      glReadPixels_(sizes[big][0] / 2, sizes[big][1] / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
      printf("centre pixel of %dx%d: %d %d %d %d (expected about 153 102 51 191); GL error 0x%x\n",
             sizes[big][0], sizes[big][1], px[0], px[1], px[2], px[3], glGetError_());
   }
   for (int s = 0; s < NSIZES; s++) {
      if (!(mask & (1u << s)) || rounds < 2)
         continue;
      if (last[s] > 2 * best[s] && last[s] - best[s] > 1.0) {
         printf("%dx%d: %.1f ms a frame in the last round, %.1f at best\n", sizes[s][0],
                sizes[s][1], last[s], best[s]);
         fail = true;
      }
   }
   printf("%s\n", fail ? "FAIL" : "PASS");
   return fail;
}
