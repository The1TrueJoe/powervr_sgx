/*
 * Copyright © 2026 servidorjarvis1-svg
 * SPDX-License-Identifier: MIT
 */

/* Compare every sampled R8/GR88 texel of an external NV12 DMA-BUF with
 * its read-only CPU mapping. No texture upload or software renderer is allowed.
 * Arguments: dma-buf-fd width height pitch UV-offset.
 * EGL_PLATFORM=surfaceless; DESKTOP_GL=1 also checks desktop OpenGL.
 * Based on powervr_sgx mesa/tests/sgx_dmabuf_planes.c by servidorjarvis1-svg.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

static const uint8_t *expected;
static size_t pitch, uv_off;
static uint8_t Yp(int x, int y) { return expected[y * pitch + x]; }
static uint8_t Up(int x, int y) { return expected[uv_off + y * pitch + x * 2]; }
static uint8_t Vp(int x, int y) { return expected[uv_off + y * pitch + x * 2 + 1]; }

static unsigned number(const char *text)
{
   char *end;
   errno = 0;
   unsigned long n = strtoul(text, &end, 10);
   if (errno || !*text || *end || n > INT_MAX) {
      fprintf(stderr, "invalid argument: %s\n", text);
      exit(2);
   }
   return (unsigned)n;
}

static GLuint prog(const char *fs)
{
   const char *vs = "attribute vec2 p; void main(){ gl_Position = vec4(p, 0.0, 1.0); }";
   const char *src[2] = {vs, fs};
   GLenum type[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
   GLuint pr = glCreateProgram();

   for (int i = 0; i < 2; i++) {
      GLuint s = glCreateShader(type[i]);
      GLint ok;

      glShaderSource(s, 1, &src[i], NULL);
      glCompileShader(s);
      glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
      if (!ok) {
         char log[512];
         glGetShaderInfoLog(s, sizeof(log), NULL, log);
         fprintf(stderr, "shader: %s\n", log);
         exit(2);
      }
      glAttachShader(pr, s);
   }
   glBindAttribLocation(pr, 0, "p");
   glLinkProgram(pr);
   return pr;
}

/* draws with the bound program into a w x h RGBA FBO and reads it back */
static uint8_t *draw(GLuint pr, int w, int h)
{
   static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
   GLuint rt, fb;
   uint8_t *px = malloc((size_t)w * h * 4);

   /* on a unit of its own: units 0 and 1 hold the planes being sampled */
   glActiveTexture(GL_TEXTURE7);
   glGenTextures(1, &rt);
   glBindTexture(GL_TEXTURE_2D, rt);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      fprintf(stderr, "FBO incomplete\n");
      exit(2);
   }
   glViewport(0, 0, w, h);
   glUseProgram(pr);
   glUniform2f(glGetUniformLocation(pr, "sz"), (float)w, (float)h);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
   glEnableVertexAttribArray(0);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
   glBindFramebuffer(GL_FRAMEBUFFER, 0);
   glDeleteFramebuffers(1, &fb);
   glDeleteTextures(1, &rt);
   return px;
}

static void bind_nearest(int unit, GLuint tex)
{
   glActiveTexture(GL_TEXTURE0 + unit);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* prints the mismatches of a w x h readback; want(x, y, out) fills the 3
 * channels expected, compared up to n of them */
static int report(const char *name, const uint8_t *px, int w, int h, int n,
                  void (*want)(int x, int y, uint8_t *out))
{
   int bad = 0, first = -1;

   for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
         const uint8_t *p = px + ((size_t)y * w + x) * 4;
         uint8_t e[3];

         want(x, y, e);
         if (memcmp(p, e, n)) {
            if (first < 0)
               first = y * w + x;
            bad++;
         }
      }
   printf("%s %dx%d: %s, %d of %d texels differ", name, w, h, bad ? "FAIL" : "OK", bad, w * h);
   if (first >= 0) {
      const uint8_t *p = px + (size_t)first * 4;
      uint8_t e[3];

      want(first % w, first / w, e);
      printf(" (first at %d,%d: %02x %02x %02x, want %02x %02x %02x)", first % w,
             first / w, p[0], p[1], p[2], e[0], e[1], e[2]);
   }
   printf("\n");
   return bad;
}

static void want_y(int x, int y, uint8_t *e) { e[0] = Yp(x, y), e[1] = 0, e[2] = 0; }
static void want_uv(int x, int y, uint8_t *e) { e[0] = Up(x, y), e[1] = Vp(x, y), e[2] = 0; }
static void want_yuv(int x, int y, uint8_t *e)
{
   e[0] = Yp(x, y), e[1] = Up(x / 2, y / 2), e[2] = Vp(x / 2, y / 2);
}

int main(int argc, char **argv)
{
   if (argc != 6 && (argc != 7 || strcmp(argv[6], "--write"))) {
      fprintf(stderr, "usage: %s dma-buf-fd width height pitch UV-offset [--write]\n", argv[0]);
      return 2;
   }
   int dfd = number(argv[1]), W = number(argv[2]), H = number(argv[3]);
   pitch = number(argv[4]); uv_off = number(argv[5]);
   off_t size = lseek(dfd, 0, SEEK_END);
   if (W < 2 || H < 2 || W > 4096 || H > 4096 || (W & 1) || (H & 1) ||
       pitch < (unsigned)W || uv_off < pitch * H || size <= 0 ||
       uv_off + pitch * (H / 2) > (uint64_t)size) {
      fprintf(stderr, "invalid NV12 layout\n");
      return 2;
   }
   expected = mmap(NULL, size, PROT_READ, MAP_SHARED, dfd, 0);
   struct dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
   if (expected == MAP_FAILED || ioctl(dfd, DMA_BUF_IOCTL_SYNC, &sync)) {
      perror("read-only CPU reference mapping"); return 2;
   }
   EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   int desk = getenv("DESKTOP_GL") != NULL;
   if (!eglInitialize(dpy, NULL, NULL)) {
      fprintf(stderr, "eglInitialize failed\n");
      return 2;
   }
   eglBindAPI(desk ? EGL_OPENGL_API : EGL_OPENGL_ES_API);
   const EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                        desk ? EGL_OPENGL_BIT : EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_NONE};
   EGLConfig cfg;
   EGLint n = 0;
   eglChooseConfig(dpy, ca, &cfg, 1, &n);
   const EGLint pa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
   const EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
   EGLSurface surf = n ? eglCreatePbufferSurface(dpy, cfg, pa) : EGL_NO_SURFACE;
   EGLContext ctx = n ? eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, desk ? NULL : xa)
                      : EGL_NO_CONTEXT;
   if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
      fprintf(stderr, "no context (0x%x)\n", eglGetError());
      return 2;
   }
   PFNEGLCREATEIMAGEKHRPROC create_image =
      (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
   PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target =
      (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
   printf("%s, %s; NV12 %dx%d, pitch %zu, UV at %zu\n", glGetString(GL_RENDERER),
          glGetString(GL_VERSION), W, H, pitch, uv_off);

   if (!strstr((const char *)glGetString(GL_RENDERER), "PowerVR SGX")) {
      fprintf(stderr, "SGX hardware renderer required\n"); return 2;
   }
   int fails = 0;
   GLuint tex[2] = {0, 0};
   EGLImageKHR images[2] = {EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
   for (int plane = 0; plane < 2; plane++) {
      EGLint ia[] = {EGL_WIDTH, plane ? W / 2 : W,
                     EGL_HEIGHT, plane ? H / 2 : H,
                     EGL_LINUX_DRM_FOURCC_EXT, plane ? DRM_FORMAT_GR88 : DRM_FORMAT_R8,
                     EGL_DMA_BUF_PLANE0_FD_EXT, dfd,
                     EGL_DMA_BUF_PLANE0_OFFSET_EXT, plane ? (EGLint)uv_off : 0,
                     EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)pitch,
                     EGL_NONE};
      EGLImageKHR img = create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, ia);

      if (img == EGL_NO_IMAGE_KHR) {
         printf("%s: eglCreateImageKHR failed (0x%x)\n", plane ? "UV/GR88" : "Y/R8",
                eglGetError());
         fails++;
         continue;
      }
      images[plane] = img;
      glGenTextures(1, &tex[plane]);
      glBindTexture(GL_TEXTURE_2D, tex[plane]);
      target(GL_TEXTURE_2D, img);
      GLenum e = glGetError();
      if (e) {
         printf("%s: glEGLImageTargetTexture2DOES 0x%x\n", plane ? "UV/GR88" : "Y/R8", e);
         tex[plane] = 0;
         fails++;
      }
   }

   /* Write attempts must fail in GL before a CPU store or GPU submission.
    * The original exact sampling below then proves the source stayed intact. */
   for (int plane = 0; (fcntl(dfd, F_GETFL) & O_ACCMODE) == O_RDONLY && plane < 2; plane++) {
      if (!tex[plane]) continue;
      glBindTexture(GL_TEXTURE_2D, tex[plane]);
      const uint8_t replacement[4] = {255, 0, 255, 255};
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA,
                      GL_UNSIGNED_BYTE, replacement);
      GLenum error = glGetError();
      if (error != GL_INVALID_OPERATION) {
         fprintf(stderr, "read-only upload: expected INVALID_OPERATION, got %#x\n", error);
         fails++;
      }
      GLuint fbo;
      glGenFramebuffers(1, &fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, tex[plane], 0);
      if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
         fprintf(stderr, "read-only texture unexpectedly renderable\n");
         fails++;
      }
      glClear(GL_COLOR_BUFFER_BIT);
      error = glGetError();
      if (error != GL_INVALID_FRAMEBUFFER_OPERATION) {
         fprintf(stderr, "read-only clear: expected framebuffer error, got %#x\n", error);
         fails++;
      }
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glDeleteFramebuffers(1, &fbo);
   }

   static const char *hdr = "#ifdef GL_ES\nprecision highp float;\n#endif\nuniform vec2 sz;";
   char fs[512];
   for (int plane = 0; plane < 2; plane++) {
      if (!tex[plane])
         continue;
      snprintf(fs, sizeof(fs),
               "%s uniform sampler2D t;"
               " void main(){ gl_FragColor = texture2D(t, gl_FragCoord.xy / sz); }",
               hdr);
      GLuint pr = prog(fs);
      bind_nearest(0, tex[plane]);
      glUseProgram(pr);
      glUniform1i(glGetUniformLocation(pr, "t"), 0);
      uint8_t *px = draw(pr, plane ? W / 2 : W, plane ? H / 2 : H);
      fails += !!report(plane ? "2 UV plane, GR88" : "1 Y plane, R8", px,
                        plane ? W / 2 : W, plane ? H / 2 : H, plane ? 2 : 3,
                        plane ? want_uv : want_y);
      free(px);
   }
   if (tex[0] && tex[1]) {
      snprintf(fs, sizeof(fs),
               "%s uniform sampler2D ty, tuv; void main(){ vec2 c = gl_FragCoord.xy / sz;"
               " gl_FragColor = vec4(texture2D(ty, c).r, texture2D(tuv, c).rg, 1.0); }",
               hdr);
      GLuint pr = prog(fs);
      bind_nearest(0, tex[0]);
      bind_nearest(1, tex[1]);
      glUseProgram(pr);
      glUniform1i(glGetUniformLocation(pr, "ty"), 0);
      glUniform1i(glGetUniformLocation(pr, "tuv"), 1);
      uint8_t *px = draw(pr, W, H);
      fails += !!report("3 Y and UV in one shader", px, W, H, 3, want_yuv);
      free(px);
   }
   /* Optional destructive check for a standalone writable probe surface.
    * Never use on a frame still needed by a decoder reference picture. */
   if (argc == 7 && tex[0] && tex[1]) {
      uint8_t *snapshot = malloc(size);
      if (!snapshot) return 2;
      memcpy(snapshot, expected, size);
      sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
      if (ioctl(dfd, DMA_BUF_IOCTL_SYNC, &sync)) return 2;
      for (int plane = 0; plane < 2; plane++) {
         GLuint fbo;
         glGenFramebuffers(1, &fbo);
         glBindFramebuffer(GL_FRAMEBUFFER, fbo);
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, tex[plane], 0);
         if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "writable plane %d is not renderable\n", plane);
            fails++;
         } else {
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, 1, 1);
            glClearColor(1, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
            if (glGetError()) fails++;
         }
         glFinish();
         glBindFramebuffer(GL_FRAMEBUFFER, 0);
         glDeleteFramebuffers(1, &fbo);
      }
      glFinish();
      sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
      if (ioctl(dfd, DMA_BUF_IOCTL_SYNC, &sync)) return 2;
      if (expected[0] != 255 || expected[uv_off] != 255 || expected[uv_off + 1] != 0) {
         fprintf(stderr, "GPU plane write bytes: Y=%u U=%u V=%u; expected 255,255,0\n", expected[0], expected[uv_off], expected[uv_off + 1]);
         fails++;
      }
      for (off_t i = 0; i < size; i++)
         if (i != 0 && i != (off_t)uv_off && i != (off_t)uv_off + 1 && expected[i] != snapshot[i]) {
            fprintf(stderr, "GPU plane writes damaged neighboring byte %ld\n", (long)i);
            fails++; break;
         }
      sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
      if (ioctl(dfd, DMA_BUF_IOCTL_SYNC, &sync)) return 2;
      void *writeback = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, 0);
      sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
      if (writeback == MAP_FAILED || ioctl(dfd, DMA_BUF_IOCTL_SYNC, &sync)) return 2;
      memcpy(writeback, snapshot, size);
      sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
      if (ioctl(dfd, DMA_BUF_IOCTL_SYNC, &sync)) return 2;
      munmap(writeback, size);
      free(snapshot);
      sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
      if (ioctl(dfd, DMA_BUF_IOCTL_SYNC, &sync)) return 2;
      printf("Writable Y/UV GPU access, CPU visibility and neighboring bytes checked\n");
   }
   printf("glGetError 0x%x\nresult: %d failed\n", glGetError(), fails);
   glFinish();
   glDeleteTextures(2, tex);
   PFNEGLDESTROYIMAGEKHRPROC destroy_image =
      (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
   for (int i = 0; i < 2; i++)
      if (images[i] != EGL_NO_IMAGE_KHR) destroy_image(dpy, images[i]);
   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(dpy, ctx);
   eglDestroySurface(dpy, surf);
   eglTerminate(dpy);
   sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
   if (ioctl(dfd, DMA_BUF_IOCTL_SYNC, &sync)) fails++;
   munmap((void *)expected, size);
   close(dfd);
   return fails ? 1 : 0;
}
