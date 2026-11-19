/*
 * Copyright © 2026 servidorjarvis1-svg
 * SPDX-License-Identifier: MIT
 */

/*
 * The two planes of an NV12 frame imported through
 * EGL_EXT_image_dma_buf_import, as a video decoder hands them over: one
 * dma-buf, Y as DRM_FORMAT_R8 at offset 0 and UV as DRM_FORMAT_GR88 at the
 * chroma offset, both with the frame's pitch. The buffer is a linear GBM BO of
 * the SGX itself, exported and filled with a known pattern through a CPU
 * mapping of the dma-buf (as a decoder writes it: not through gbm_bo_map,
 * which writes a staging copy), so no decoder is needed. Checked by reading pixels back from an FBO:
 *  1. the Y plane alone (R8 sampled as (Y, 0, 0, 1))
 *  2. the UV plane alone (GR88 sampled as (U, V, 0, 1))
 *  3. Y and UV in one fragment shader on two units, as mpv's GPU output
 *     samples a VA-API frame
 * NEAREST, every texel compared. DESKTOP_GL=1 does the same in a desktop GL
 * context (mpv's default on this driver).
 *
 *   EGL_PLATFORM=surfaceless sgx_dmabuf_planes [width height]   (1920 1080)
 *
 * zig cc -target x86-linux-gnu.2.36 -isystem <root>/usr/include -I<mesa>/include/drm-uapi
 * -I<mesa>/src/gbm/main -o sgx_dmabuf_planes sgx_dmabuf_planes.c
 * <root>/usr/lib/i386-linux-gnu/libEGL.so.1 <root>/usr/lib/i386-linux-gnu/libGLESv2.so.2
 * <root>/usr/lib/i386-linux-gnu/libgbm.so.1
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <linux/dma-buf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static uint8_t Yp(int x, int y) { return (uint8_t)(x * 3 + y * 7 + (x >> 4) * 11); }
static uint8_t Up(int x, int y) { return (uint8_t)(x * 5 + y * 13 + 40); }
static uint8_t Vp(int x, int y) { return (uint8_t)(x * 9 + y * 2 + 200); }

/* the render node whose device is bound to powervr_sgx */
static int open_sgx(void)
{
   char path[64], link[256];

   for (int i = 128; i < 192; i++) {
      snprintf(path, sizeof(path), "/sys/class/drm/renderD%d/device/driver", i);
      ssize_t len = readlink(path, link, sizeof(link) - 1);
      if (len < 0)
         continue;
      link[len] = 0;
      const char *name = strrchr(link, '/');
      if (strcmp(name ? name + 1 : link, "powervr_sgx"))
         continue;
      snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
      return open(path, O_RDWR | O_CLOEXEC);
   }
   return -1;
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
   int W = argc > 2 ? atoi(argv[1]) : 1920, H = argc > 2 ? atoi(argv[2]) : 1080;
   /* rows of 4-byte texels: the BO is (pitch / 4) x (Y rows + UV rows) */
   int bo_w = (W + 255) / 256 * 64, bo_h = H + H / 2;
   int fd = open_sgx();

   if (fd < 0) {
      fprintf(stderr, "no powervr_sgx render node\n");
      return 2;
   }
   struct gbm_device *gbm = gbm_create_device(fd);
   struct gbm_bo *bo = gbm ? gbm_bo_create(gbm, bo_w, bo_h, GBM_FORMAT_ABGR8888,
                                           GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING)
                           : NULL;
   if (!bo) {
      fprintf(stderr, "gbm_bo_create of a linear %dx%d ABGR8888 failed\n", bo_w, bo_h);
      return 2;
   }

   uint32_t pitch = gbm_bo_get_stride(bo), uv_off = pitch * H;
   size_t size = (size_t)pitch * bo_h;
   int dfd = gbm_bo_get_fd(bo);
   if (dfd < 0) {
      fprintf(stderr, "gbm_bo_get_fd failed\n");
      return 2;
   }
   uint8_t *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, 0);
   if (m == MAP_FAILED) {
      perror("mmap of the dma-buf");
      return 2;
   }
   struct dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
   ioctl(dfd, DMA_BUF_IOCTL_SYNC, &sync);
   memset(m, 0x55, size);
   for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
         m[y * pitch + x] = Yp(x, y);
   for (int y = 0; y < H / 2; y++)
      for (int x = 0; x < W / 2; x++) {
         m[uv_off + y * pitch + 2 * x] = Up(x, y);
         m[uv_off + y * pitch + 2 * x + 1] = Vp(x, y);
      }
   sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
   ioctl(dfd, DMA_BUF_IOCTL_SYNC, &sync);
   munmap(m, size);

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
   printf("%s, %s; NV12 %dx%d, pitch %u, UV at %u\n", glGetString(GL_RENDERER),
          glGetString(GL_VERSION), W, H, pitch, uv_off);

   int fails = 0;
   GLuint tex[2] = {0, 0};
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
   printf("glGetError 0x%x\nresult: %d failed\n", glGetError(), fails);
   return fails ? 1 : 0;
}
