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
 * NEAREST, every texel compared. Then what GL may do to an imported plane
 * besides sampling it, each checked on the dma-buf itself (a CPU mapping,
 * every byte against a shadow copy):
 *  4. glReadPixels of an FBO with the UV plane attached: U and V, not Y
 *  5. glTexSubImage2D into the UV plane: the box written, and nothing else
 *  6. a draw into an FBO with the Y plane attached: the UV plane untouched
 *     (R8 renders, at 4 bytes a pixel it would run into UV), and one into
 *     the UV plane: the Y plane untouched
 *  7. glBlitFramebuffer from an RGBA texture into the UV plane (where the
 *     context has it): the box written, and nothing else
 * Each of 5 to 7 starts from the pattern written again.
 * DESKTOP_GL=1 does the same in a desktop GL
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

static uint8_t Qp(int x, int y) { return (uint8_t)(x * 17 + y * 29 + 3); }

/* the dma-buf compared with the shadow copy of what it should hold: over
 * [lo, hi) bytes, or all of it.  Prints the first difference. */
static int check_buf(const char *name, int dfd, const uint8_t *want, size_t size,
                     size_t lo, size_t hi)
{
   uint8_t *m = mmap(NULL, size, PROT_READ, MAP_SHARED, dfd, 0);
   struct dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
   size_t bad = 0, first = 0;

   if (m == MAP_FAILED) {
      perror("mmap of the dma-buf");
      exit(2);
   }
   ioctl(dfd, DMA_BUF_IOCTL_SYNC, &sync);
   for (size_t i = lo; i < hi; i++)
      if (m[i] != want[i] && !bad++)
         first = i;
   printf("%s: %s, %zu of %zu bytes differ", name, bad ? "FAIL" : "OK", bad, hi - lo);
   if (bad)
      printf(" (first at byte %zu: %02x, want %02x)", first, m[first], want[first]);
   printf("\n");
   sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
   ioctl(dfd, DMA_BUF_IOCTL_SYNC, &sync);
   munmap(m, size);
   return bad != 0;
}

/* the dma-buf rewritten from the shadow copy, as a decoder hands a frame */
static void fill_buf(int dfd, const uint8_t *want, size_t size)
{
   uint8_t *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, 0);
   struct dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};

   if (m == MAP_FAILED) {
      perror("mmap of the dma-buf");
      exit(2);
   }
   ioctl(dfd, DMA_BUF_IOCTL_SYNC, &sync);
   memcpy(m, want, size);
   sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
   ioctl(dfd, DMA_BUF_IOCTL_SYNC, &sync);
   munmap(m, size);
}

/* an FBO with tex (W x H) as its colour buffer, bound; 0 if incomplete */
static GLuint plane_fbo(GLuint tex)
{
   GLuint fb;

   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
      return fb;
   glBindFramebuffer(GL_FRAMEBUFFER, 0);
   glDeleteFramebuffers(1, &fb);
   return 0;
}

/* a full-viewport magenta quad into the plane tex (w x h) through an FBO,
 * then the bytes [lo, hi) of the dma-buf checked against the shadow copy */
static int draw_into(const char *name, GLuint tex, int w, int h, int dfd,
                     const uint8_t *want, size_t size, size_t lo, size_t hi)
{
   static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
   GLuint fb = plane_fbo(tex);
   char what[128];

   if (fb) {
      GLuint pr = prog("void main(){ gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0); }");

      glViewport(0, 0, w, h);
      glUseProgram(pr);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
      glEnableVertexAttribArray(0);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      glFinish();
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glDeleteFramebuffers(1, &fb);
   }
   snprintf(what, sizeof(what), "%s%s", name, fb ? "" : " (FBO incomplete, no draw)");
   return check_buf(what, dfd, want, size, lo, hi);
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
   uint8_t *ref = malloc(size); /* what the dma-buf should hold */
   memset(ref, 0x55, size);
   for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
         ref[y * pitch + x] = Yp(x, y);
   for (int y = 0; y < H / 2; y++)
      for (int x = 0; x < W / 2; x++) {
         ref[uv_off + y * pitch + 2 * x] = Up(x, y);
         ref[uv_off + y * pitch + 2 * x + 1] = Vp(x, y);
      }
   memcpy(m, ref, size);
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
   if (tex[1]) {
      int w = W / 2, h = H / 2;
      GLuint fb = plane_fbo(tex[1]);

      /* 4. a CPU read of the UV plane */
      if (fb) {
         uint8_t *px = malloc((size_t)w * h * 4);

         glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
         fails += !!report("4 glReadPixels of the UV plane", px, w, h, 2, want_uv);
         free(px);
         glBindFramebuffer(GL_FRAMEBUFFER, 0);
         glDeleteFramebuffers(1, &fb);
      } else
         printf("4 glReadPixels of the UV plane: no FBO on it (incomplete), skipped\n");

      /* 5. a texture upload into the UV plane: a 16x8 box at (8, 4) */
      fill_buf(dfd, ref, size);
      enum { BX = 8, BY = 4, BW = 16, BH = 8 };
      uint8_t sub[BW * BH * 2];
      for (int y = 0; y < BH; y++)
         for (int x = 0; x < BW; x++) {
            sub[(y * BW + x) * 2] = Qp(x, y);
            sub[(y * BW + x) * 2 + 1] = (uint8_t)~Qp(x, y);
            ref[uv_off + (BY + y) * pitch + 2 * (BX + x)] = Qp(x, y);
            ref[uv_off + (BY + y) * pitch + 2 * (BX + x) + 1] = (uint8_t)~Qp(x, y);
         }
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, tex[1]);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      while (glGetError())
         ;
      glTexSubImage2D(GL_TEXTURE_2D, 0, BX, BY, BW, BH, 0x8227 /* GL_RG */,
                      GL_UNSIGNED_BYTE, sub);
      GLenum e = glGetError();
      glFinish();
      if (e) {
         printf("5 glTexSubImage2D into the UV plane: 0x%x, skipped\n", e);
         /* nothing written: the shadow copy goes back */
         for (int y = 0; y < BH; y++)
            for (int x = 0; x < 2 * BW; x++)
               ref[uv_off + (BY + y) * pitch + 2 * BX + x] =
                  x & 1 ? Vp(BX + x / 2, BY + y) : Up(BX + x / 2, BY + y);
      } else
         fails += check_buf("5 glTexSubImage2D into the UV plane, whole dma-buf", dfd,
                            ref, size, 0, size);

      /* 6. draws into the planes: whatever each does to its own plane, the
       *    other one stays */
      fill_buf(dfd, ref, size);
      if (tex[0])
         fails += draw_into("6 a draw into the Y plane, the UV plane", tex[0], W, H, dfd,
                            ref, size, uv_off, size);
      fill_buf(dfd, ref, size);
      fails += draw_into("6 a draw into the UV plane, the Y plane", tex[1], w, h, dfd, ref,
                         size, 0, uv_off);
   }
   /* 7. a blit into the UV plane: a 16x8 box at (24, 12) */
   typedef void (*blit_fn)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,
                           GLbitfield, GLenum);
   /* GL 3.0 or ES 3.0: an ES 2.0 context resolves the name but refuses it */
   blit_fn blit = desk || strstr((const char *)glGetString(GL_VERSION), "ES 3")
                     ? (blit_fn)eglGetProcAddress("glBlitFramebuffer")
                     : NULL;
   if (tex[1])
      fill_buf(dfd, ref, size);
   if (tex[1] && blit) {
      enum { BX = 24, BY = 12, BW = 16, BH = 8 };
      uint8_t src[BW * BH * 4];
      GLuint st, sfb, dfb;

      for (int y = 0; y < BH; y++)
         for (int x = 0; x < BW; x++) {
            uint8_t *p = src + (y * BW + x) * 4;

            p[0] = Qp(x, y) ^ 0x5a, p[1] = Qp(y, x), p[2] = 0, p[3] = 255;
            ref[uv_off + (BY + y) * pitch + 2 * (BX + x)] = p[0];
            ref[uv_off + (BY + y) * pitch + 2 * (BX + x) + 1] = p[1];
         }
      glActiveTexture(GL_TEXTURE6);
      glGenTextures(1, &st);
      glBindTexture(GL_TEXTURE_2D, st);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, BW, BH, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
      glGenFramebuffers(1, &sfb);
      glBindFramebuffer(GL_FRAMEBUFFER, sfb);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, st, 0);
      dfb = plane_fbo(tex[1]);
      if (dfb) {
         glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, sfb);
         glBindFramebuffer(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, dfb);
         while (glGetError())
            ;
         blit(0, 0, BW, BH, BX, BY, BX + BW, BY + BH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
         GLenum e = glGetError();
         glFinish();
         glBindFramebuffer(GL_FRAMEBUFFER, 0);
         if (e)
            printf("7 glBlitFramebuffer into the UV plane: 0x%x\n", e), fails++;
         else
            fails += check_buf("7 glBlitFramebuffer into the UV plane, whole dma-buf", dfd,
                               ref, size, 0, size);
         glDeleteFramebuffers(1, &dfb);
      } else
         printf("7 glBlitFramebuffer into the UV plane: no FBO on it, skipped\n");
      glDeleteFramebuffers(1, &sfb);
      glDeleteTextures(1, &st);
   } else if (tex[1])
      printf("7 glBlitFramebuffer into the UV plane: not in this context, skipped\n");
   free(ref);
   printf("glGetError 0x%x\nresult: %d failed\n", glGetError(), fails);
   return fails ? 1 : 0;
}
