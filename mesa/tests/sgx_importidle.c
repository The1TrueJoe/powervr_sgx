/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */
/* sgx_importidle: one import of another device's buffer, sampled, then the
 * client gone. A dumb buffer of the display device (a KMS node, card0 by
 * default) is filled by the CPU, exported as a dma-buf, imported through
 * EGL_EXT_image_dma_buf_import, sampled into an FBO and checked, and the
 * program exits. The dumb buffer's handle is closed after the export, so the
 * dma-buf lives only as long as this process's fd and the GPU driver's
 * import. Run it, let the GPU idle past its autosuspend delay, then look for
 * the buffer in /sys/kernel/debug/dma_buf/bufinfo: it must be gone without
 * any further GPU work.
 *
 *   sgx_importidle [/dev/dri/cardN [ms]]   prints PASS/FAIL, exit 0 on PASS
 *
 * ms: wait that long after the last render before exiting, so the client's
 * teardown can land just before or after the GPU's autosuspend.
 *
 * zig cc -target x86_64-linux-gnu.2.36 -O2 -I<mesa include> -o sgx_importidle sgx_importidle.c -ldl
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#define EGL_EGL_PROTOTYPES 0
#define GL_GLES_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "drm-uapi/drm.h"
#include "drm-uapi/drm_mode.h"

#define W 256
#define H 256
#define FOURCC_XR24 0x34325258 /* DRM_FORMAT_XRGB8888 */

static PFNEGLGETPROCADDRESSPROC gpa;
static void *
L(const char *n)
{
   void *p = (void *)gpa(n);
   if (!p) {
      fprintf(stderr, "no %s\n", n);
      exit(2);
   }
   return p;
}
#define G(t, n) static t n##_
#define GET(n) n##_ = L(#n)

G(PFNEGLGETPLATFORMDISPLAYPROC, eglGetPlatformDisplay);
G(PFNEGLINITIALIZEPROC, eglInitialize);
G(PFNEGLBINDAPIPROC, eglBindAPI);
G(PFNEGLCHOOSECONFIGPROC, eglChooseConfig);
G(PFNEGLCREATECONTEXTPROC, eglCreateContext);
G(PFNEGLCREATEPBUFFERSURFACEPROC, eglCreatePbufferSurface);
G(PFNEGLMAKECURRENTPROC, eglMakeCurrent);
G(PFNEGLCREATEIMAGEKHRPROC, eglCreateImageKHR);
G(PFNEGLDESTROYIMAGEKHRPROC, eglDestroyImageKHR);
G(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC, glEGLImageTargetTexture2DOES);
G(PFNGLGENTEXTURESPROC, glGenTextures);
G(PFNGLBINDTEXTUREPROC, glBindTexture);
G(PFNGLTEXPARAMETERIPROC, glTexParameteri);
G(PFNGLTEXIMAGE2DPROC, glTexImage2D);
G(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers);
G(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer);
G(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D);
G(PFNGLCREATESHADERPROC, glCreateShader);
G(PFNGLSHADERSOURCEPROC, glShaderSource);
G(PFNGLCOMPILESHADERPROC, glCompileShader);
G(PFNGLCREATEPROGRAMPROC, glCreateProgram);
G(PFNGLATTACHSHADERPROC, glAttachShader);
G(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation);
G(PFNGLLINKPROGRAMPROC, glLinkProgram);
G(PFNGLUSEPROGRAMPROC, glUseProgram);
G(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer);
G(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray);
G(PFNGLVIEWPORTPROC, glViewport);
G(PFNGLDRAWARRAYSPROC, glDrawArrays);
G(PFNGLREADPIXELSPROC, glReadPixels);
G(PFNGLFINISHPROC, glFinish);
G(PFNGLGETERRORPROC, glGetError);

static uint32_t
pattern(int x, int y)
{
   return 0xff000000u | (uint32_t)(x & 0xff) << 16 | (uint32_t)(y & 0xff) << 8 |
          (uint32_t)((x ^ y) & 0xff);
}

int
main(int argc, char **argv)
{
   const char *node = argc > 1 ? argv[1] : "/dev/dri/card0";
   int kms = open(node, O_RDWR | O_CLOEXEC);

   if (kms < 0) {
      perror(node);
      return 2;
   }
   struct drm_mode_create_dumb cd = {.width = W, .height = H, .bpp = 32};
   if (ioctl(kms, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) {
      perror("CREATE_DUMB");
      return 2;
   }
   struct drm_mode_map_dumb md = {.handle = cd.handle};
   if (ioctl(kms, DRM_IOCTL_MODE_MAP_DUMB, &md)) {
      perror("MAP_DUMB");
      return 2;
   }
   uint8_t *map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, kms, md.offset);
   if (map == MAP_FAILED) {
      perror("mmap");
      return 2;
   }
   for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
         ((uint32_t *)(map + (size_t)y * cd.pitch))[x] = pattern(x, y);
   munmap(map, cd.size);
   struct drm_prime_handle ph = {.handle = cd.handle, .flags = DRM_CLOEXEC | DRM_RDWR};
   if (ioctl(kms, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph)) {
      perror("PRIME_HANDLE_TO_FD");
      return 2;
   }
   struct drm_mode_destroy_dumb dd = {.handle = cd.handle};
   ioctl(kms, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
   close(kms);

   void *egl = dlopen("libEGL.so.1", RTLD_NOW);
   if (!egl) {
      fprintf(stderr, "%s\n", dlerror());
      return 2;
   }
   gpa = (PFNEGLGETPROCADDRESSPROC)dlsym(egl, "eglGetProcAddress");
   GET(eglGetPlatformDisplay);
   GET(eglInitialize);
   GET(eglBindAPI);
   GET(eglChooseConfig);
   GET(eglCreateContext);
   GET(eglCreatePbufferSurface);
   GET(eglMakeCurrent);
   GET(eglCreateImageKHR);
   GET(eglDestroyImageKHR);
   EGLDisplay d = eglGetPlatformDisplay_(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   const EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                        EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_NONE};
   const EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
   const EGLint pa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
   EGLConfig cfg;
   EGLint nc = 0;
   if (!eglInitialize_(d, NULL, NULL) || !eglBindAPI_(EGL_OPENGL_ES_API) ||
       !eglChooseConfig_(d, ca, &cfg, 1, &nc) || nc < 1) {
      fprintf(stderr, "EGL setup failed\n");
      return 2;
   }
   EGLContext c = eglCreateContext_(d, cfg, EGL_NO_CONTEXT, xa);
   EGLSurface s = eglCreatePbufferSurface_(d, cfg, pa);
   if (!eglMakeCurrent_(d, s, s, c)) {
      fprintf(stderr, "eglMakeCurrent failed\n");
      return 2;
   }
   GET(glEGLImageTargetTexture2DOES);
   GET(glGenTextures);
   GET(glBindTexture);
   GET(glTexParameteri);
   GET(glTexImage2D);
   GET(glGenFramebuffers);
   GET(glBindFramebuffer);
   GET(glFramebufferTexture2D);
   GET(glCreateShader);
   GET(glShaderSource);
   GET(glCompileShader);
   GET(glCreateProgram);
   GET(glAttachShader);
   GET(glBindAttribLocation);
   GET(glLinkProgram);
   GET(glUseProgram);
   GET(glVertexAttribPointer);
   GET(glEnableVertexAttribArray);
   GET(glViewport);
   GET(glDrawArrays);
   GET(glReadPixels);
   GET(glFinish);
   GET(glGetError);

   const EGLint ia[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_LINUX_DRM_FOURCC_EXT, FOURCC_XR24,
                        EGL_DMA_BUF_PLANE0_FD_EXT, (EGLint)ph.fd,
                        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)cd.pitch, EGL_NONE};
   EGLImageKHR img = eglCreateImageKHR_(d, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, ia);
   if (img == EGL_NO_IMAGE_KHR) {
      fprintf(stderr, "eglCreateImageKHR failed\n");
      return 2;
   }
   GLuint src, dst, fbo;
   glGenTextures_(1, &src);
   glBindTexture_(GL_TEXTURE_2D, src);
   glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, img);
   glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glGenTextures_(1, &dst);
   glBindTexture_(GL_TEXTURE_2D, dst);
   glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glGenFramebuffers_(1, &fbo);
   glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);

   static const char *vs = "attribute vec2 p; varying vec2 t; void main() {"
                           " t = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }";
   static const char *fs = "precision highp float; varying vec2 t; uniform sampler2D s;"
                           " void main() { gl_FragColor = texture2D(s, t); }";
   GLuint v = glCreateShader_(GL_VERTEX_SHADER), f = glCreateShader_(GL_FRAGMENT_SHADER),
          p = glCreateProgram_();
   glShaderSource_(v, 1, &vs, NULL);
   glCompileShader_(v);
   glShaderSource_(f, 1, &fs, NULL);
   glCompileShader_(f);
   glAttachShader_(p, v);
   glAttachShader_(p, f);
   glBindAttribLocation_(p, 0, "p");
   glLinkProgram_(p);
   glUseProgram_(p);
   static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
   glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
   glEnableVertexAttribArray_(0);
   glBindTexture_(GL_TEXTURE_2D, src);
   glViewport_(0, 0, W, H);
   glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
   uint8_t *px = malloc((size_t)W * H * 4);
   glReadPixels_(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
   glFinish_();

   long bad = 0;
   for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
         uint32_t want = pattern(x, y);
         const uint8_t *q = px + ((size_t)y * W + x) * 4;
         /* XRGB8888 is B, G, R, X in memory; RGBA reads R, G, B, A */
         if (q[0] != (want >> 16 & 0xff) || q[1] != (want >> 8 & 0xff) ||
             q[2] != (want & 0xff))
            bad++;
      }
   GLenum err = glGetError_();
   printf("%s import %ux%u pitch %u: %ld texels wrong, glerror %#x\n",
          !bad && !err ? "PASS" : "FAIL", W, H, cd.pitch, bad, err);
   eglDestroyImageKHR_(d, img);
   close(ph.fd);
   free(px);
   if (argc > 2)
      usleep(atoi(argv[2]) * 1000);
   return !bad && !err ? 0 : 1;
}
