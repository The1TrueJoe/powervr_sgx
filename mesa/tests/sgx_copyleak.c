/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * Memory over frames that copy the back buffer into a texture, as a game's
 * title screen does every frame (Minecraft's panorama: glCopyTexSubImage2D
 * seven times a frame, drawing between) and a recorder does: a window
 * (an EGL one on a GBM surface, or with -x a GLX one on the X server in
 * $DISPLAY, whose back buffers X shares), desktop GL, a 256x256 texture.
 * Each frame draws, copies (unless "nocopy") and samples the copy -c times
 * (1 by default), then swaps.  -d asks for a depth buffer.  Each frame also
 * draws from a new vertex buffer of -v KiB (1024 by default; 0: none), off
 * screen, as a game streams its vertices and Mesa its uniforms and indices:
 * the frame before's buffer is deleted, the GPU waited for (glFinish, so
 * its device copy is idle in the winsys's cache of freed buffers, as at a
 * slow game's frame rate) and the new one's draw takes it from the cache
 * (0.2.0 and 0.2.1 lost every buffer that cache gave out: Minecraft's title
 * screen grew ~2 MB/s).  Every 50 frames it prints the system's Shmem, the
 * process's memory on each DRM driver (its fdinfo) and RSS; at the end the
 * growth a frame, and FAIL when Shmem or a driver's memory grew by more
 * than 4 KiB a frame over the second half.
 *
 *   sgx_copyleak [-x] [-d] [-c copies] [-v KiB] [frames] [nocopy|blit]
 *
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -I<mesa src/gbm/main>
 * -I<X11 include> -o sgx_copyleak sgx_copyleak.c -ldl
 */
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#define EGL_EGL_PROTOTYPES 0
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>

#define W 720
#define H 480
#define T 256
#define MAX2(a, b) ((a) > (b) ? (a) : (b))
#define MIN2(a, b) ((a) < (b) ? (a) : (b))

/* gbm, by dlsym */
struct gbm_device;
struct gbm_surface;
struct gbm_bo;
#define GBM_FORMAT_XRGB8888 0x34325258
#define GBM_BO_USE_SCANOUT (1 << 0)
#define GBM_BO_USE_RENDERING (1 << 2)

static void *(*gpa)(const char *);

static void *
L(const char *n)
{
   void *p = gpa(n);
   if (!p) {
      fprintf(stderr, "no %s\n", n);
      exit(2);
   }
   return p;
}

#define FUNCS(X)                                                               \
   X(glAttachShader)                                                           \
   X(glBindAttribLocation)                                                     \
   X(glBindBuffer)                                                             \
   X(glBufferData)                                                             \
   X(glBindTexture)                                                            \
   X(glBlitFramebuffer)                                                        \
   X(glBindFramebuffer)                                                        \
   X(glClear)                                                                  \
   X(glClearColor)                                                             \
   X(glCompileShader)                                                          \
   X(glCopyTexSubImage2D)                                                      \
   X(glCreateProgram)                                                          \
   X(glCreateShader)                                                           \
   X(glDeleteBuffers)                                                          \
   X(glDrawArrays)                                                             \
   X(glEnableVertexAttribArray)                                                \
   X(glFinish)                                                                 \
   X(glFramebufferTexture2D)                                                   \
   X(glGenBuffers)                                                             \
   X(glGenFramebuffers)                                                        \
   X(glGenTextures)                                                            \
   X(glGetUniformLocation)                                                     \
   X(glLinkProgram)                                                            \
   X(glShaderSource)                                                           \
   X(glTexImage2D)                                                             \
   X(glTexParameteri)                                                          \
   X(glUniform1f)                                                              \
   X(glUseProgram)                                                             \
   X(glVertexAttribPointer)                                                    \
   X(glViewport)

#define DECL(n) static __typeof__(&n) n##_;
FUNCS(DECL)
#undef DECL

static long
meminfo_kb(const char *key)
{
   char line[256];
   long v = -1;
   FILE *f = fopen("/proc/meminfo", "r");

   while (f && fgets(line, sizeof(line), f))
      if (!strncmp(line, key, strlen(key)) && line[strlen(key)] == ':')
         v = strtol(line + strlen(key) + 1, NULL, 10);
   if (f)
      fclose(f);
   return v;
}

static long
rss_kb(void)
{
   char line[256];
   long v = -1;
   FILE *f = fopen("/proc/self/status", "r");

   while (f && fgets(line, sizeof(line), f))
      if (!strncmp(line, "VmRSS:", 6))
         v = strtol(line + 6, NULL, 10);
   if (f)
      fclose(f);
   return v;
}

/* the process's memory on each DRM driver: the drm-total-* (or
 * drm-memory-*) lines of its DRM fds' fdinfo, in KiB, one client each */
#define MAX_DRV 4
struct drv_mem {
   char name[32];
   long kb;
   unsigned long clients[16];
   unsigned n_clients;
};

static unsigned
drm_kb(struct drv_mem *m)
{
   DIR *d = opendir("/proc/self/fdinfo");
   struct dirent *e;
   unsigned n = 0;

   while (d && (e = readdir(d))) {
      char path[64], line[256], drv[32] = "";
      unsigned long client = 0;
      long sum = 0;
      FILE *f;

      snprintf(path, sizeof(path), "/proc/self/fdinfo/%s", e->d_name);
      if (!(f = fopen(path, "r")))
         continue;
      while (fgets(line, sizeof(line), f)) {
         char *c = strchr(line, ':');
         if (!strncmp(line, "drm-driver:", 11))
            sscanf(line + 11, " %31s", drv);
         if (!strncmp(line, "drm-client-id:", 14))
            client = strtoul(line + 14, NULL, 10);
         if (c && (!strncmp(line, "drm-total-", 10) ||
                   !strncmp(line, "drm-memory-", 11))) {
            long v = strtol(c + 1, &c, 10);
            while (*c == ' ')
               c++;
            if (!strncmp(c, "MiB", 3))
               v *= 1024;
            else if (!strncmp(c, "KiB", 3))
               ;
            else
               v /= 1024; /* bytes */
            sum += v;
         }
      }
      fclose(f);
      if (!drv[0])
         continue;
      unsigned i;
      for (i = 0; i < n && strcmp(m[i].name, drv); i++)
         ;
      if (i == n) {
         if (n == MAX_DRV)
            continue;
         memset(&m[n], 0, sizeof(m[n]));
         snprintf(m[n].name, sizeof(m[n].name), "%s", drv);
         n++;
      }
      /* the same client through another fd (a dup) counts once */
      bool seen = false;
      for (unsigned k = 0; k < m[i].n_clients; k++)
         seen |= m[i].clients[k] == client;
      if (seen)
         continue;
      if (m[i].n_clients < 16)
         m[i].clients[m[i].n_clients++] = client;
      m[i].kb += sum;
   }
   if (d)
      closedir(d);
   return n;
}

static const char *vs = "#version 110\n"
                        "attribute vec2 p;\n"
                        "varying vec2 uv;\n"
                        "uniform float s;\n"
                        "void main() { uv = p * 0.5 + 0.5; gl_Position = vec4(p * s, 0.0, 1.0); }\n";
static const char *fs = "#version 110\n"
                        "uniform sampler2D t;\n"
                        "varying vec2 uv;\n"
                        "void main() { gl_FragColor = texture2D(t, uv) * 0.9 + vec4(uv, 0.2, 1.0) * 0.1; }\n";

/* the window: an EGL one on a GBM surface, or a GLX one on X */
struct win {
   /* EGL and GBM */
   void *egl_dpy, *egl_surf;
   unsigned (*egl_swap)(void *, void *);
   struct gbm_surface *gs;
   struct gbm_bo *prev;
   struct gbm_bo *(*lock_front)(struct gbm_surface *);
   void (*release)(struct gbm_surface *, struct gbm_bo *);
   /* GLX */
   Display *x;
   Window xw;
   void (*glx_swap)(Display *, GLXDrawable);
};

static void *egl_lib;

static void *
egl_gpa(const char *n)
{
   static PFNEGLGETPROCADDRESSPROC p;
   if (!p)
      p = (PFNEGLGETPROCADDRESSPROC)dlsym(egl_lib, "eglGetProcAddress");
   return (void *)p(n);
}

static void *glx_lib;

static void *
glx_gpa(const char *n)
{
   static void *(*p)(const GLubyte *);
   if (!p)
      p = (void *(*)(const GLubyte *))dlsym(glx_lib, "glXGetProcAddressARB");
   return p((const GLubyte *)n);
}

static bool
open_gbm(struct win *w, bool depth)
{
   void *gbm = dlopen("libgbm.so.1", RTLD_NOW);
   void *egl = egl_lib = dlopen("libEGL.so.1", RTLD_NOW);
   if (!gbm || !egl) {
      fprintf(stderr, "no libgbm.so.1 or libEGL.so.1\n");
      return false;
   }
   struct gbm_device *(*gbm_create_device)(int) = dlsym(gbm, "gbm_create_device");
   struct gbm_surface *(*gbm_surface_create)(struct gbm_device *, uint32_t,
                                             uint32_t, uint32_t, uint32_t) =
      dlsym(gbm, "gbm_surface_create");
   w->lock_front = dlsym(gbm, "gbm_surface_lock_front_buffer");
   w->release = dlsym(gbm, "gbm_surface_release_buffer");
   gpa = egl_gpa;
   PFNEGLGETPLATFORMDISPLAYEXTPROC getdisplay =
      (void *)gpa("eglGetPlatformDisplayEXT");
   PFNEGLINITIALIZEPROC initialize = (void *)dlsym(egl, "eglInitialize");
   PFNEGLBINDAPIPROC bindapi = (void *)dlsym(egl, "eglBindAPI");
   PFNEGLCHOOSECONFIGPROC chooseconfig = (void *)dlsym(egl, "eglChooseConfig");
   PFNEGLGETCONFIGATTRIBPROC getattr = (void *)dlsym(egl, "eglGetConfigAttrib");
   PFNEGLCREATECONTEXTPROC createcontext = (void *)dlsym(egl, "eglCreateContext");
   PFNEGLCREATEWINDOWSURFACEPROC createwindow =
      (void *)dlsym(egl, "eglCreateWindowSurface");
   PFNEGLMAKECURRENTPROC makecurrent = (void *)dlsym(egl, "eglMakeCurrent");
   w->egl_swap = (void *)dlsym(egl, "eglSwapBuffers");

   int fd = open(getenv("SGX_COPYLEAK_NODE") ? getenv("SGX_COPYLEAK_NODE")
                                             : "/dev/dri/renderD128",
                 O_RDWR | O_CLOEXEC);
   struct gbm_device *dev = fd >= 0 ? gbm_create_device(fd) : NULL;
   if (!dev || !getdisplay) {
      fprintf(stderr, "no gbm device\n");
      return false;
   }
   w->gs = gbm_surface_create(dev, W, H, GBM_FORMAT_XRGB8888,
                              GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
   if (!w->gs)
      w->gs = gbm_surface_create(dev, W, H, GBM_FORMAT_XRGB8888,
                                 GBM_BO_USE_RENDERING);
   EGLDisplay d = getdisplay(EGL_PLATFORM_GBM_KHR, dev, NULL);
   if (!w->gs || !initialize(d, NULL, NULL)) {
      fprintf(stderr, "no gbm surface or EGL display\n");
      return false;
   }
   bindapi(EGL_OPENGL_API);
   EGLint ca[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8,
                  EGL_DEPTH_SIZE, depth ? 24 : 0,
                  EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
   EGLConfig cfgs[64], cfg = NULL;
   EGLint n = 0;
   chooseconfig(d, ca, cfgs, 64, &n);
   for (EGLint i = 0; i < n && !cfg; i++) {
      EGLint id = 0;
      getattr(d, cfgs[i], EGL_NATIVE_VISUAL_ID, &id);
      if (id == GBM_FORMAT_XRGB8888)
         cfg = cfgs[i];
   }
   EGLSurface s = cfg ? createwindow(d, cfg, (EGLNativeWindowType)w->gs, NULL)
                      : EGL_NO_SURFACE;
   EGLContext c = cfg ? createcontext(d, cfg, EGL_NO_CONTEXT, NULL) : NULL;
   if (!s || !c || !makecurrent(d, s, s, c)) {
      fprintf(stderr, "no EGL window on the gbm surface\n");
      return false;
   }
   w->egl_dpy = d;
   w->egl_surf = s;
   return true;
}

static bool
open_glx(struct win *w, bool depth)
{
   void *xl = dlopen("libX11.so.6", RTLD_NOW);
   glx_lib = dlopen("libGL.so.1", RTLD_NOW);
   if (!xl || !glx_lib) {
      fprintf(stderr, "no libX11.so.6 or libGL.so.1\n");
      return false;
   }
   Display *(*xopen)(const char *) = dlsym(xl, "XOpenDisplay");
   Colormap (*xcmap)(Display *, Window, Visual *, int) =
      dlsym(xl, "XCreateColormap");
   Window (*xcreate)(Display *, Window, int, int, unsigned, unsigned, unsigned,
                     int, unsigned, Visual *, unsigned long,
                     XSetWindowAttributes *) = dlsym(xl, "XCreateWindow");
   int (*xmap)(Display *, Window) = dlsym(xl, "XMapWindow");
   int (*xsync)(Display *, Bool) = dlsym(xl, "XSync");
   XVisualInfo *(*choose)(Display *, int, int *) =
      dlsym(glx_lib, "glXChooseVisual");
   GLXContext (*create)(Display *, XVisualInfo *, GLXContext, Bool) =
      dlsym(glx_lib, "glXCreateContext");
   Bool (*makecurrent)(Display *, GLXDrawable, GLXContext) =
      dlsym(glx_lib, "glXMakeCurrent");
   w->glx_swap = dlsym(glx_lib, "glXSwapBuffers");
   gpa = glx_gpa;

   Display *x = xopen(NULL);
   if (!x) {
      fprintf(stderr, "no X display\n");
      return false;
   }
   int attr[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
                 GLX_BLUE_SIZE, 8, GLX_DEPTH_SIZE, depth ? 24 : 0, None};
   XVisualInfo *vi = choose(x, DefaultScreen(x), attr);
   if (!vi) {
      fprintf(stderr, "no GLX visual\n");
      return false;
   }
   Window root = RootWindow(x, vi->screen);
   XSetWindowAttributes swa = {
      .colormap = xcmap(x, root, vi->visual, AllocNone),
      .border_pixel = 0,
   };
   w->xw = xcreate(x, root, 0, 0, W, H, 0, vi->depth, InputOutput, vi->visual,
                   CWColormap | CWBorderPixel, &swa);
   xmap(x, w->xw);
   xsync(x, False);
   GLXContext c = create(x, vi, NULL, True);
   if (!c || !makecurrent(x, w->xw, c)) {
      fprintf(stderr, "no GLX context\n");
      return false;
   }
   w->x = x;
   return true;
}

static void
swap(struct win *w)
{
   if (w->x) {
      w->glx_swap(w->x, w->xw);
      return;
   }
   w->egl_swap(w->egl_dpy, w->egl_surf);
   struct gbm_bo *bo = w->lock_front(w->gs);
   if (w->prev)
      w->release(w->gs, w->prev);
   w->prev = bo;
}

int
main(int argc, char **argv)
{
   bool x11 = false, depth = false;
   unsigned copies = 1, stream_kib = 1024;
   int o;

   while ((o = getopt(argc, argv, "xdc:v:")) != -1) {
      switch (o) {
      case 'x': x11 = true; break;
      case 'd': depth = true; break;
      case 'c': copies = (unsigned)atoi(optarg); break;
      case 'v': stream_kib = (unsigned)atoi(optarg); break;
      default:
         fprintf(stderr, "sgx_copyleak [-x] [-d] [-c copies] [-v KiB] [frames] [nocopy|blit]\n");
         return 2;
      }
   }
   argc -= optind;
   argv += optind;
   unsigned frames = argc > 0 ? (unsigned)atoi(argv[0]) : 1000;
   bool copy = !(argc > 1 && !strcmp(argv[1], "nocopy"));
   bool blit = argc > 1 && !strcmp(argv[1], "blit");
   struct win win = {0};

   if (!(x11 ? open_glx(&win, depth) : open_gbm(&win, depth)))
      return 2;
#define GET(n) n##_ = L(#n);
   FUNCS(GET)
#undef GET

   GLuint tex, fb = 0, p = glCreateProgram_();
   const char *src[2] = {vs, fs};
   GLenum type[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
   static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};

   for (int i = 0; i < 2; i++) {
      GLuint sh = glCreateShader_(type[i]);
      glShaderSource_(sh, 1, &src[i], NULL);
      glCompileShader_(sh);
      glAttachShader_(p, sh);
   }
   glBindAttribLocation_(p, 0, "p");
   glLinkProgram_(p);
   glUseProgram_(p);
   glGenTextures_(1, &tex);
   glBindTexture_(GL_TEXTURE_2D, tex);
   glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, T, T, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 NULL);
   if (blit) {
      glGenFramebuffers_(1, &fb);
      glBindFramebuffer_(GL_FRAMEBUFFER, fb);
      glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_TEXTURE_2D, tex, 0);
      glBindFramebuffer_(GL_FRAMEBUFFER, 0);
   }
   glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
   glEnableVertexAttribArray_(0);
   GLint su = glGetUniformLocation_(p, "s");
   /* the streamed buffer's vertices: whole triangles, every vertex off
    * screen (clipped: nothing to rasterize), 4095 a draw at most */
   unsigned stream_verts = stream_kib * 1024 / 8 / 3 * 3;
   GLfloat *stream = malloc(MAX2(stream_kib, 1) * 1024);
   GLuint sb = 0;
   if (!stream)
      return 2;
   for (unsigned i = 0; i < stream_kib * 1024 / 4; i++)
      stream[i] = 4.0f + (i % 7);

   long shm0 = 0, shm_half = 0;
   struct drv_mem m0[MAX_DRV], mh[MAX_DRV];
   unsigned n0 = 0, nh = 0;

   printf("sgx_copyleak: %ux%u %s window%s, %ux%u texture, %s x%u a frame, "
          "a new %u KiB vertex buffer a frame, %u frames\n",
          W, H, x11 ? "GLX" : "GBM", depth ? " with depth" : "", T, T,
          blit ? "glBlitFramebuffer into an FBO"
          : copy ? "glCopyTexSubImage2D" : "no copy",
          copy ? copies : 0, stream_kib, frames);
   for (unsigned f = 0; f <= frames; f++) {
      if (f % 50 == 0) {
         struct drv_mem m[MAX_DRV];
         unsigned n = drm_kb(m);
         long shm = meminfo_kb("Shmem");

         printf("frame %4u: Shmem %ld KiB,", f, shm);
         for (unsigned i = 0; i < n; i++)
            printf(" %s %ld KiB,", m[i].name, m[i].kb);
         printf(" RSS %ld KiB\n", rss_kb());
         fflush(stdout);
         if (f == 0) {
            shm0 = shm;
            n0 = n;
            memcpy(m0, m, sizeof(m));
         }
         if (f == frames / 2) {
            shm_half = shm;
            nh = n;
            memcpy(mh, m, sizeof(m));
         }
         if (f == frames) {
            unsigned half = frames - frames / 2;
            double ds = (double)(shm - shm_half) / half;
            bool fail = ds > 4;

            printf("growth a frame over the second half: Shmem %.1f KiB", ds);
            for (unsigned i = 0; i < n; i++) {
               long a = 0, b = 0;
               for (unsigned k = 0; k < nh; k++)
                  if (!strcmp(mh[k].name, m[i].name))
                     a = mh[k].kb;
               for (unsigned k = 0; k < n0; k++)
                  if (!strcmp(m0[k].name, m[i].name))
                     b = m0[k].kb;
               double dg = (double)(m[i].kb - a) / half;
               printf(", %s %.1f KiB (whole run %+ld KiB)", m[i].name, dg,
                      m[i].kb - b);
               fail |= dg > 4;
            }
            printf(" (whole run: Shmem %+ld KiB)\n%s\n", shm - shm0,
                   fail ? "FAIL" : "PASS");
            return fail;
         }
      }
      glViewport_(0, 0, W, H);
      glClearColor_((f % 7) / 7.0f, 0.3f, 0.5f, 1);
      glClear_(GL_COLOR_BUFFER_BIT | (depth ? GL_DEPTH_BUFFER_BIT : 0));
      glUniform1f_(su, 0.8f);
      glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
      for (unsigned k = 0; k < (copy ? copies : 0); k++) {
         if (blit) {
            glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, fb);
            glBlitFramebuffer_(0, 0, T, T, 0, 0, T, T, GL_COLOR_BUFFER_BIT,
                               GL_NEAREST);
            glBindFramebuffer_(GL_FRAMEBUFFER, 0);
         } else {
            glCopyTexSubImage2D_(GL_TEXTURE_2D, 0, 0, 0, 0, 0, T, T);
         }
         /* a draw that samples the copy, into the back buffer, between
          * copies (Minecraft's blur passes) */
         glUniform1f_(su, 0.3f + 0.05f * k);
         glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
      }
      if (!copy) {
         glUniform1f_(su, 0.3f);
         glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
      }
      /* the streamed buffer: the last one deleted, the GPU waited for, a
       * new one drawn from, and the quad's client array back */
      if (stream_kib) {
         if (sb)
            glDeleteBuffers_(1, &sb);
         glFinish_();
         glGenBuffers_(1, &sb);
         stream[0] = 4.0f + f % 3;
         glBindBuffer_(GL_ARRAY_BUFFER, sb);
         glBufferData_(GL_ARRAY_BUFFER, stream_kib * 1024, stream,
                       GL_STREAM_DRAW);
         glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
         for (unsigned v = 0; v < stream_verts; v += 4095)
            glDrawArrays_(GL_TRIANGLES, v, MIN2(4095, stream_verts - v));
         glBindBuffer_(GL_ARRAY_BUFFER, 0);
         glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
      }
      swap(&win);
   }
   return 0;
}
