// jsui-wl on Ubuntu Touch: zwp_linux_dmabuf_v1 for Vulkan apps (Mesa Turnip
// on KGSL), shown as GL textures.
//
// Turnip's Wayland WSI hands its swapchain images over as dma-bufs. jsui-wl
// renders with libhybris' Android EGL, which has no EGL_EXT_image_dma_buf_import
// (Adreno's Android driver doesn't list it), and wlroots runs the pixman
// renderer, which can't import them either. So the protocol is served here
// (see "protocol" below for why not wlroots') and each committed buffer is
// imported here, trying in order:
//
//  1. EGL_EXT_image_dma_buf_import, if this EGL ever has it.
//  2. Gralloc: the dma-buf becomes an Android native buffer through libhybris
//     (eglHybrisCreateRemoteBuffer, the call android_wlegl's server half uses
//     for client buffers), then an EGLImage (EGL_NATIVE_BUFFER_ANDROID) bound
//     to a GL texture. No copy. Gralloc wants its own handle, not a bare fd:
//     a buffer of the same size and format is allocated once per import as
//     a template, its serialized handle gets the client's fd, stride, size
//     and offset (and its metadata the stride, which is where Adreno reads
//     it), and gralloc imports that. The template's layout is checked
//     against QTI's private_handle_t before any of it is trusted.
//  3. CPU: mmap the dma-buf and upload the damaged rows into a texture of
//     the window's own (glTexSubImage2D). Only 8-bit RGBA formats and RGB565,
//     and slow (KGSL's buffers are uncached for the CPU): a last resort.
//
// Only LINEAR is offered: UBWC (QCOM_COMPRESSED) buffers would need the
// handle's UBWC flags and metadata to match Turnip's layout, which hasn't
// been verified.
//
// Sync: the kernel is 4.19, so there is no DMA_BUF_IOCTL_EXPORT_SYNC_FILE and
// wlroots 0.17 has no linux-drm-syncobj-v1 (KGSL has no DRM syncobjs anyway).
// Nothing orders the shell's sampling after the client's rendering but
// time: Turnip submits a frame before it commits the buffer, and the shell's
// GL work for it reaches the same GPU later (no torn or stale frames seen
// with vkcube). The CPU path brackets its reads with DMA_BUF_IOCTL_SYNC.
#pragma once

#ifdef JSUI_WL_MIR_GL

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pixman.h>
#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/memfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

extern "C" {
#define static
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/dmabuf.h>
#undef static
}

#include "linux-dmabuf-unstable-v1-protocol.h"

namespace dmabuf {

// jsui-wl's log line (its logf is in main.cpp's anonymous namespace).
inline void logf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::fprintf(stderr, "[jsui-wl] ");
  std::vfprintf(stderr, fmt, ap);
  std::fprintf(stderr, "\n");
  va_end(ap);
}

struct Format {
  uint32_t drm;
  int hal;            // HAL_PIXEL_FORMAT_* for gralloc
  GLenum glFormat;    // CPU upload (0: not on the CPU path)
  GLenum glType;
  int bpp;
  bool opaque;
};

constexpr GLenum kBgra = 0x80E1;  // GL_BGRA_EXT (GL_EXT_texture_format_BGRA8888)
inline const Format kFormats[] = {
    {DRM_FORMAT_ARGB8888, 5 /*BGRA_8888*/, kBgra, GL_UNSIGNED_BYTE, 4, false},
    {DRM_FORMAT_XRGB8888, 5 /*BGRA_8888*/, kBgra, GL_UNSIGNED_BYTE, 4, true},
    {DRM_FORMAT_ABGR8888, 1 /*RGBA_8888*/, GL_RGBA, GL_UNSIGNED_BYTE, 4, false},
    {DRM_FORMAT_XBGR8888, 2 /*RGBX_8888*/, GL_RGBA, GL_UNSIGNED_BYTE, 4, true},
    {DRM_FORMAT_ABGR2101010, 0x2B /*RGBA_1010102*/, 0, 0, 4, false},
    {DRM_FORMAT_XBGR2101010, 0x2B /*RGBA_1010102*/, 0, 0, 4, true},
    {DRM_FORMAT_ABGR16161616F, 0x16 /*RGBA_FP16*/, 0, 0, 8, false},
    {DRM_FORMAT_RGB565, 4 /*RGB_565*/, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2, true},
};
// Not AR30 / XR30 (blue in the low bits): Android has no such gralloc
// format, and Skia resets a texture's swizzle, so red and blue can't be
// swapped when sampling.
inline const Format* findFormat(uint32_t drm) {
  for (const Format& f : kFormats)
    if (f.drm == drm) return &f;
  return nullptr;
}

// EGL_HYBRIS_native_buffer2 (libhybris' eglplatformcommon.cpp)
using HyCreateNativeFn = EGLBoolean (*)(EGLint w, EGLint h, EGLint usage, EGLint format, EGLint* stride,
                                        EGLClientBuffer* buffer);
using HyCreateRemoteFn = EGLBoolean (*)(EGLint w, EGLint h, EGLint usage, EGLint format, EGLint stride, int numInts,
                                        int* ints, int numFds, int* fds, EGLClientBuffer* buffer);
using HyInfoFn = void (*)(EGLClientBuffer buffer, int* numInts, int* numFds);
using HySerializeFn = void (*)(EGLClientBuffer buffer, int* ints, int* fds);
using HyReleaseFn = EGLBoolean (*)(EGLClientBuffer buffer);
using CreateImageFn = void* (*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*);
using DestroyImageFn = EGLBoolean (*)(EGLDisplay, void*);
using ImageTargetFn = void (*)(GLenum, void*);

constexpr EGLenum kNativeBufferAndroid = 0x3140;  // EGL_NATIVE_BUFFER_ANDROID
constexpr int kUsageTexture = 0x100;             // GRALLOC_USAGE_HW_TEXTURE
constexpr int kUsageSwReadOften = 0x3;           // GRALLOC_USAGE_SW_READ_OFTEN: keeps gralloc linear
constexpr int kQtiMagic = 0x676d736d;            // 'gmsm', QTI private_handle_t
constexpr int kQtiUbwcFlag = 0x08000000;         // PRIV_FLAGS_UBWC_ALIGNED

// Ints of QTI's private_handle_t after its two fds (fd, fd_metadata), as
// gralloc 4 on this Android 11 vendor lays it out: packed, the 64-bit base
// addresses straight after offset_metadata. Checked against a template buffer.
enum QtiInt {
  kMagic = 0, kFlags = 1, kWidth = 2, kHeight = 3, kUnalignedWidth = 4, kUnalignedHeight = 5, kFormat = 6,
  kSize = 13, kOffset = 14, kBase = 16, kBaseMetadata = 18, kGpuAddr = 20, kQtiInts = 22,
};

struct Import {
  wlr_buffer* buf = nullptr;
  wl_listener destroy{};
  const Format* fmt = nullptr;
  EGLClientBuffer native = nullptr;
  void* image = nullptr;
  GLuint tex = 0;
  bool failed = false;
};

struct State {
  wl_global* global = nullptr;
  // What the global offers (all LINEAR) and its feedback's format table.
  std::vector<uint32_t> formats;
  int formatTable = -1;
  size_t formatTableSize = 0;
  dev_t mainDevice = 0;
  EGLDisplay dpy = EGL_NO_DISPLAY;
  std::function<bool()> makeCurrent;
  bool eglImport = false, eglModifiers = false, gralloc = false, cpu = false, bgra = false, unpackRowLength = false;
  HyCreateNativeFn hyCreateNative = nullptr;
  HyCreateRemoteFn hyCreateRemote = nullptr;
  HyInfoFn hyInfo = nullptr;
  HySerializeFn hySerialize = nullptr;
  HyReleaseFn hyRelease = nullptr;
  CreateImageFn createImage = nullptr;
  DestroyImageFn destroyImage = nullptr;
  ImageTargetFn imageTarget = nullptr;
  std::vector<const Format*> grallocFormats;
  std::map<wlr_buffer*, Import*> imports;
  // CPU path: the window's texture (by window id) and its size.
  struct CpuTex {
    GLuint tex = 0;
    int w = 0, h = 0;
    GLenum format = 0;
  };
  std::map<uint32_t, CpuTex> cpuTex;
  int imported = 0;  // log the first few imports
};
inline State s;

inline bool hasExt(const char* list, const char* name) {
  if (!list) return false;
  size_t n = std::strlen(name);
  for (const char* p = list; (p = std::strstr(p, name)); p += n)
    if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return true;
  return false;
}

// libhybris' own entry points: through glvnd's eglGetProcAddress when it
// passes them on, else from libhybris' EGL directly (already loaded).
inline void* hybrisProc(const char* name) {
  if (void* p = reinterpret_cast<void*>(eglGetProcAddress(name))) return p;
  static void* lib = dlopen("libEGL_libhybris.so.0", RTLD_NOW | RTLD_NOLOAD);
  if (!lib) return nullptr;
  using GetProc = void* (*)(const char*);
  static auto gp = reinterpret_cast<GetProc>(dlsym(lib, "eglGetProcAddress"));
  return gp ? gp(name) : dlsym(lib, name);
}

// A gralloc handle for `fd` (gralloc's own fds are dup'd: the caller closes
// the ints/fds it gets). False when this gralloc's handle isn't QTI's.
// Releases a native buffer and closes the fds in its handle. libhybris'
// release leaves them open, and with them the dma-bufs they name (the
// client's buffer, ~10 MB, and gralloc's 32 KB metadata): every import of a
// Firefox / Claude buffer left two fds behind, 212 after an evening. Each fd
// is closed only if it still names the same file after the release.
inline void releaseNative(EGLClientBuffer b) {
  if (!b) return;
  struct Held {
    int fd;
    dev_t dev;
    ino_t ino;
  };
  std::vector<Held> held;
  int ni = 0, nf = 0;
  s.hyInfo(b, &ni, &nf);
  if (ni >= 0 && nf > 0 && ni <= 256 && nf <= 16) {
    std::vector<int> ints(static_cast<size_t>(ni)), fds(static_cast<size_t>(nf), -1);
    s.hySerialize(b, ints.data(), fds.data());
    for (int fd : fds) {
      struct stat st;
      if (fd >= 0 && fstat(fd, &st) == 0) held.push_back({fd, st.st_dev, st.st_ino});
    }
  }
  s.hyRelease(b);
  for (const Held& h : held) {
    struct stat st;
    if (fstat(h.fd, &st) == 0 && st.st_dev == h.dev && st.st_ino == h.ino) close(h.fd);
  }
}

inline bool grallocTemplate(const Format* f, int w, int h, std::vector<int>* ints, std::vector<int>* fds) {
  EGLint stride = 0;
  EGLClientBuffer t = nullptr;
  if (!s.hyCreateNative(w, h, kUsageTexture | kUsageSwReadOften, f->hal, &stride, &t) || !t) return false;
  int ni = 0, nf = 0;
  s.hyInfo(t, &ni, &nf);
  bool ok = false;
  if (nf == 2 && ni == kQtiInts) {
    ints->assign(size_t(ni), 0);
    fds->assign(size_t(nf), -1);
    s.hySerialize(t, ints->data(), fds->data());
    const std::vector<int>& v = *ints;
    ok = v[kMagic] == kQtiMagic && v[kUnalignedWidth] == w && v[kUnalignedHeight] == h && v[kWidth] >= w &&
         v[kHeight] >= h && v[kFormat] == f->hal && !(v[kFlags] & kQtiUbwcFlag);
    if (ok) {
      for (int& fd : *fds) fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    }
  }
  static bool dumped = false;
  if (!ok && !dumped) {
    // What this gralloc's handle looks like, to tell why it isn't QTI's.
    dumped = true;
    std::string d;
    std::vector<int> v(size_t(std::max(0, std::min(ni, 64)))), fv(size_t(std::max(0, std::min(nf, 8))));
    if (ni == int(v.size()) && nf == int(fv.size())) {
      s.hySerialize(t, v.data(), fv.data());
      for (int x : v) d += " " + std::to_string(x);
    }
    logf("dma-buf: gralloc template %dx%d hal %d: %d fds, %d ints:%s", w, h, f->hal, nf, ni, d.c_str());
  }
  releaseNative(t);
  return ok;
}

inline void freeImport(Import* im) {
  if ((im->tex || im->image) && eglGetCurrentContext() == EGL_NO_CONTEXT && s.makeCurrent) s.makeCurrent();
  if (im->tex) glDeleteTextures(1, &im->tex);
  if (im->image) s.destroyImage(s.dpy, im->image);
  releaseNative(im->native);
  wl_list_remove(&im->destroy.link);
  delete im;
}

inline GLuint newTexture() {
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return tex;
}

inline bool importEgl(Import* im, const wlr_dmabuf_attributes& a) {
  std::vector<EGLint> at = {EGL_WIDTH, a.width, EGL_HEIGHT, a.height, EGL_LINUX_DRM_FOURCC_EXT, EGLint(a.format)};
  static const EGLint planeAttrs[4][5] = {
      {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
       EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
      {EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
       EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
      {EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
       EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
      {EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
       EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT},
  };
  for (int i = 0; i < a.n_planes && i < 4; i++) {
    at.insert(at.end(), {planeAttrs[i][0], a.fd[i], planeAttrs[i][1], EGLint(a.offset[i]), planeAttrs[i][2],
                         EGLint(a.stride[i])});
    if (s.eglModifiers && a.modifier != DRM_FORMAT_MOD_INVALID)
      at.insert(at.end(), {planeAttrs[i][3], EGLint(a.modifier & 0xffffffff), planeAttrs[i][4],
                           EGLint(a.modifier >> 32)});
  }
  at.push_back(EGL_NONE);
  im->image = s.createImage(s.dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, at.data());
  return im->image != nullptr;
}

// Adreno's GL takes a gralloc buffer's layout from gralloc's metadata (the
// plane layout written when it was allocated), not from the handle's width:
// with the template's padded stride (gralloc pads 2000 px rows to 2304 px)
// every row of the client's picture sheared. Its stride, pitch and size there
// become the client's. Returns how many fields changed, or -1.
inline int patchMetadata(int fd, int32_t fromStride, int32_t fromPitch, int32_t fromSize, int32_t stride,
                         int32_t pitch, int32_t size) {
  off_t len = lseek(fd, 0, SEEK_END);
  if (len <= 0 || len > (1 << 20)) return -1;
  void* m = mmap(nullptr, size_t(len), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (m == MAP_FAILED) return -1;
  auto* v = static_cast<int32_t*>(m);
  int n = 0;
  for (size_t i = 0; i < size_t(len) / 4; i++) {
    int32_t to = v[i] == fromStride ? stride : v[i] == fromPitch ? pitch : v[i] == fromSize ? size : v[i];
    if (to != v[i]) {
      v[i] = to;
      n++;
    }
  }
  munmap(m, size_t(len));
  return n;
}

inline bool importGralloc(Import* im, const wlr_dmabuf_attributes& a) {
  const Format* f = im->fmt;
  if (a.n_planes != 1 || a.modifier != DRM_FORMAT_MOD_LINEAR || a.stride[0] % uint32_t(f->bpp)) return false;
  std::vector<int> ints, fds;
  if (!grallocTemplate(f, a.width, a.height, &ints, &fds)) return false;
  // The client's buffer in place of the template's: its fd, its pitch (in
  // pixels, gralloc's aligned width), size and offset, in the handle and in
  // the metadata. Mappings of the template mean nothing for the import,
  // which maps for itself.
  off_t size = lseek(a.fd[0], 0, SEEK_END);
  lseek(a.fd[0], 0, SEEK_SET);
  if (size <= 0) size = off_t(a.offset[0]) + off_t(a.stride[0]) * a.height;
  close(fds[0]);
  fds[0] = fcntl(a.fd[0], F_DUPFD_CLOEXEC, 0);
  const int stridePx = int(a.stride[0]) / f->bpp;
  int patched = 0;
  if (ints[kWidth] != stridePx || ints[kSize] != int(size))
    patched = patchMetadata(fds[1], ints[kWidth], ints[kWidth] * f->bpp, ints[kSize], stridePx, int(a.stride[0]),
                            int(size));
  if (s.imported < 8)
    logf("dma-buf gralloc: template stride %d px, size %d; client %d px, %lld (%d metadata fields)", ints[kWidth],
         ints[kSize], stridePx, (long long)size, patched);
  if (patched < 0) {
    for (int fd : fds) close(fd);
    return false;
  }
  ints[kWidth] = stridePx;
  ints[kHeight] = a.height;
  ints[kSize] = int(size);
  ints[kOffset] = int(a.offset[0]);
  for (int i : {int(kBase), kBase + 1, int(kBaseMetadata), kBaseMetadata + 1, int(kGpuAddr), kGpuAddr + 1})
    ints[size_t(i)] = 0;
  // CreateRemoteBuffer closes the fds it is given, imported or not.
  if (!s.hyCreateRemote(a.width, a.height, kUsageTexture | kUsageSwReadOften, f->hal, stridePx, int(ints.size()),
                        ints.data(), int(fds.size()), fds.data(), &im->native) ||
      !im->native) {
    im->native = nullptr;
    return false;
  }
  const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
  im->image = s.createImage(s.dpy, EGL_NO_CONTEXT, kNativeBufferAndroid, im->native, attrs);
  return im->image != nullptr;
}

// `buf` was imported before (successfully or not): import() needs no GL.
inline bool isImported(wlr_buffer* buf) { return s.imports.count(buf) != 0; }

// The import of `buf` (made on first use), or null: not a dma-buf, or
// neither EGL nor gralloc takes it (the CPU path is the caller's then).
inline Import* import(wlr_buffer* buf, bool* isDmabuf) {
  wlr_dmabuf_attributes a{};
  *isDmabuf = s.global && wlr_buffer_get_dmabuf(buf, &a);
  if (!*isDmabuf) return nullptr;
  auto it = s.imports.find(buf);
  if (it != s.imports.end()) return it->second->failed ? nullptr : it->second;
  auto* im = new Import();
  im->buf = buf;
  im->fmt = findFormat(a.format);
  im->destroy.notify = [](wl_listener* l, void*) {
    Import* im = wl_container_of(l, im, destroy);
    s.imports.erase(im->buf);
    freeImport(im);
  };
  wl_signal_add(&buf->events.destroy, &im->destroy);
  s.imports[buf] = im;
  const char* path = nullptr;
  if (im->fmt && s.eglImport && importEgl(im, a)) path = "EGL dma-buf import";
  else if (im->fmt && s.gralloc && importGralloc(im, a)) path = "gralloc";
  if (path) {
    im->tex = newTexture();
    s.imageTarget(GL_TEXTURE_2D, im->image);
    glBindTexture(GL_TEXTURE_2D, 0);
  } else {
    im->failed = true;
  }
  if (s.imported++ < 8)
    logf("dma-buf %dx%d %.4s mod 0x%llx pitch %u offset %u: %s", a.width, a.height,
         reinterpret_cast<const char*>(&a.format), (unsigned long long)a.modifier, a.stride[0], a.offset[0],
         path ? path : "no GPU import, CPU upload");
  return im->failed ? nullptr : im;
}

// CPU path: the damaged rows of `buf` into window `id`'s texture. 0 when
// the format or the mapping isn't possible.
inline GLuint upload(uint32_t id, wlr_buffer* buf, const pixman_region32_t* damage, bool* opaque) {
  wlr_dmabuf_attributes a{};
  if (!s.cpu || !wlr_buffer_get_dmabuf(buf, &a) || a.n_planes != 1 || a.modifier != DRM_FORMAT_MOD_LINEAR) return 0;
  const Format* f = findFormat(a.format);
  if (!f || !f->glFormat || (f->glFormat == kBgra && !s.bgra)) return 0;
  if (a.stride[0] % uint32_t(f->bpp) || (!s.unpackRowLength && a.stride[0] != uint32_t(a.width * f->bpp))) return 0;
  const size_t len = size_t(a.offset[0]) + size_t(a.stride[0]) * size_t(a.height);
  void* map = mmap(nullptr, len, PROT_READ, MAP_SHARED, a.fd[0], 0);
  if (map == MAP_FAILED) return 0;
  State::CpuTex& t = s.cpuTex[id];
  bool full = !t.tex || t.w != a.width || t.h != a.height || t.format != f->glFormat;
  if (!t.tex) t.tex = newTexture();
  glBindTexture(GL_TEXTURE_2D, t.tex);
  int y0 = 0, y1 = a.height;
  if (!full && damage) {
    const pixman_box32_t* e = pixman_region32_extents(const_cast<pixman_region32_t*>(damage));
    y0 = std::max(0, e->y1);
    y1 = std::min(a.height, e->y2);
  }
  dma_buf_sync sync{DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
  ioctl(a.fd[0], DMA_BUF_IOCTL_SYNC, &sync);
  const auto* px = static_cast<const uint8_t*>(map) + a.offset[0];
  glPixelStorei(GL_UNPACK_ALIGNMENT, f->bpp >= 4 ? 4 : 2);
  if (s.unpackRowLength) glPixelStorei(0x0CF2 /*GL_UNPACK_ROW_LENGTH*/, int(a.stride[0]) / f->bpp);
  if (full) {
    glTexImage2D(GL_TEXTURE_2D, 0, GLint(f->glFormat), a.width, a.height, 0, f->glFormat, f->glType, px);
    t.w = a.width;
    t.h = a.height;
    t.format = f->glFormat;
  } else if (y1 > y0) {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y0, a.width, y1 - y0, f->glFormat, f->glType,
                    px + size_t(y0) * a.stride[0]);
  }
  if (s.unpackRowLength) glPixelStorei(0x0CF2, 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
  ioctl(a.fd[0], DMA_BUF_IOCTL_SYNC, &sync);
  munmap(map, len);
  glBindTexture(GL_TEXTURE_2D, 0);
  *opaque = f->opaque;
  return t.tex;
}

// Window `id` is gone or no longer on the CPU path.
inline void forget(uint32_t id) {
  auto it = s.cpuTex.find(id);
  if (it == s.cpuTex.end()) return;
  if (it->second.tex) glDeleteTextures(1, &it->second.tex);
  s.cpuTex.erase(it);
}

// ------------------------------------------------------------- protocol
//
// zwp_linux_dmabuf_v1 itself, not wlroots' wlr_linux_dmabuf_v1: that one
// opens the feedback's render node (renderD128) to check client fds, and
// fails to start without it. jsui-wl may not open it at all (the node's ACL
// follows the logind seat session, and the shell can run outside one), and
// its display engine has nothing to do with Turnip's buffers anyway.

struct Buffer {
  wlr_buffer base;
  wl_resource* resource = nullptr;
  wlr_dmabuf_attributes attr{};
  wl_listener release{};
};

inline void closeAttr(wlr_dmabuf_attributes* a) {
  for (int i = 0; i < a->n_planes; i++)
    if (a->fd[i] >= 0) close(a->fd[i]);
  a->n_planes = 0;
}

inline const wlr_buffer_impl kBufferImpl = {
    /*destroy*/
    [](wlr_buffer* wb) {
      auto* b = reinterpret_cast<Buffer*>(wb);
      if (b->resource) wl_resource_set_user_data(b->resource, nullptr);
      closeAttr(&b->attr);
      wl_list_remove(&b->release.link);
      delete b;
    },
    /*get_dmabuf*/
    [](wlr_buffer* wb, wlr_dmabuf_attributes* out) -> bool {
      *out = reinterpret_cast<Buffer*>(wb)->attr;
      return true;
    },
    /*get_shm*/ nullptr,
    /*begin_data_ptr_access*/ nullptr,
    /*end_data_ptr_access*/ nullptr,
};

inline const struct wl_buffer_interface kWlBufferImpl = {
    /*destroy*/ [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
};

// Ours: a wl_buffer made by create / create_immed below.
inline bool isBuffer(wl_resource* r) { return wl_resource_instance_of(r, &wl_buffer_interface, &kWlBufferImpl); }

inline const wlr_buffer_resource_interface kResourceInterface = {
    /*name*/ "zwp_linux_dmabuf_v1",
    /*is_instance*/ isBuffer,
    /*from_resource*/
    [](wl_resource* r) -> wlr_buffer* {
      auto* b = static_cast<Buffer*>(wl_resource_get_user_data(r));
      return b ? &b->base : nullptr;
    },
};

struct Params {
  wlr_dmabuf_attributes attr{};
  bool used = false;
  bool set[4] = {};
  uint64_t modifier[4] = {};
};

inline Params* params(wl_resource* r) { return static_cast<Params*>(wl_resource_get_user_data(r)); }

inline void createBuffer(wl_client* client, wl_resource* r, uint32_t bufferId, int32_t w, int32_t h, uint32_t format,
                         uint32_t flags) {
  Params* p = params(r);
  if (p->used) {
    wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "params already used");
    return;
  }
  p->used = true;
  int n = 0;
  while (n < 4 && p->set[n]) n++;
  for (int i = n; i < 4; i++)
    if (p->set[i]) {
      wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE, "plane %d missing", n);
      return;
    }
  if (n == 0) {
    wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE, "no planes");
    return;
  }
  if (w <= 0 || h <= 0) {
    wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS, "invalid size %dx%d", w, h);
    return;
  }
  for (int i = 0; i < n; i++) {
    if (p->modifier[i] != p->modifier[0]) {
      wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT, "planes' modifiers differ");
      return;
    }
    off_t size = lseek(p->attr.fd[i], 0, SEEK_END);
    if (size >= 0 && uint64_t(p->attr.offset[i]) + uint64_t(p->attr.stride[i]) * uint64_t(h) > uint64_t(size) &&
        i == 0) {
      wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS, "plane %d out of bounds", i);
      return;
    }
  }
  // Only what the feedback offered, and no y-invert / interlacing.
  // DRM_FORMAT_MOD_INVALID (no modifier) is taken as LINEAR: Mesa sends it
  // when its driver has no modifier list, and Turnip's buffers are linear
  // unless a modifier says otherwise.
  if (p->modifier[0] == DRM_FORMAT_MOD_INVALID)
    for (uint64_t& m : p->modifier) m = DRM_FORMAT_MOD_LINEAR;
  bool offered = std::find(s.formats.begin(), s.formats.end(), format) != s.formats.end() &&
                 p->modifier[0] == DRM_FORMAT_MOD_LINEAR && flags == 0;
  if (!offered) {
    if (bufferId) {
      wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER, "format %.4s mod 0x%llx not offered",
                             reinterpret_cast<const char*>(&format), (unsigned long long)p->modifier[0]);
    } else {
      zwp_linux_buffer_params_v1_send_failed(r);
    }
    return;
  }
  wl_resource* br = wl_resource_create(client, &wl_buffer_interface, 1, bufferId);
  if (!br) {
    wl_client_post_no_memory(client);
    return;
  }
  auto* b = new Buffer();
  b->resource = br;
  b->attr = p->attr;
  b->attr.n_planes = n;
  b->attr.width = w;
  b->attr.height = h;
  b->attr.format = format;
  b->attr.modifier = p->modifier[0];
  p->attr.n_planes = 0;  // the fds are the buffer's now
  for (bool& set : p->set) set = false;
  wlr_buffer_init(&b->base, &kBufferImpl, w, h);
  wl_resource_set_implementation(br, &kWlBufferImpl, b, [](wl_resource* r) {
    // The client's wl_buffer is gone; the wlr_buffer stays while it is shown.
    if (auto* b = static_cast<Buffer*>(wl_resource_get_user_data(r))) {
      b->resource = nullptr;
      wlr_buffer_drop(&b->base);
    }
  });
  b->release.notify = [](wl_listener* l, void*) {
    Buffer* b = wl_container_of(l, b, release);
    if (b->resource) wl_buffer_send_release(b->resource);
  };
  wl_signal_add(&b->base.events.release, &b->release);
  if (!bufferId) zwp_linux_buffer_params_v1_send_created(r, br);
}

inline const struct zwp_linux_buffer_params_v1_interface kParamsImpl = {
    /*destroy*/ [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
    /*add*/
    [](wl_client*, wl_resource* r, int32_t fd, uint32_t idx, uint32_t offset, uint32_t stride, uint32_t hi,
       uint32_t lo) {
      Params* p = params(r);
      if (p->used) {
        close(fd);
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "params already used");
      } else if (idx >= 4) {
        close(fd);
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX, "plane %u", idx);
      } else if (p->set[idx]) {
        close(fd);
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET, "plane %u set twice", idx);
      } else {
        p->set[idx] = true;
        p->attr.fd[idx] = fd;
        p->attr.offset[idx] = offset;
        p->attr.stride[idx] = stride;
        p->modifier[idx] = (uint64_t(hi) << 32) | lo;
        p->attr.n_planes = std::max(p->attr.n_planes, int(idx) + 1);
      }
    },
    /*create*/
    [](wl_client* c, wl_resource* r, int32_t w, int32_t h, uint32_t format, uint32_t flags) {
      createBuffer(c, r, 0, w, h, format, flags);
    },
    /*create_immed*/
    [](wl_client* c, wl_resource* r, uint32_t id, int32_t w, int32_t h, uint32_t format, uint32_t flags) {
      createBuffer(c, r, id, w, h, format, flags);
    },
};

// The feedback (v4): one tranche, every offered format with LINEAR.
inline void sendFeedback(wl_resource* r) {
  zwp_linux_dmabuf_feedback_v1_send_format_table(r, s.formatTable, uint32_t(s.formatTableSize));
  wl_array dev;
  wl_array_init(&dev);
  if (void* d = wl_array_add(&dev, sizeof(dev_t))) std::memcpy(d, &s.mainDevice, sizeof(dev_t));
  zwp_linux_dmabuf_feedback_v1_send_main_device(r, &dev);
  zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(r, &dev);
  wl_array_release(&dev);
  zwp_linux_dmabuf_feedback_v1_send_tranche_flags(r, 0);
  wl_array idx;
  wl_array_init(&idx);
  for (size_t i = 0; i < s.formats.size(); i++)
    if (auto* v = static_cast<uint16_t*>(wl_array_add(&idx, sizeof(uint16_t)))) *v = uint16_t(i);
  zwp_linux_dmabuf_feedback_v1_send_tranche_formats(r, &idx);
  wl_array_release(&idx);
  zwp_linux_dmabuf_feedback_v1_send_tranche_done(r);
  zwp_linux_dmabuf_feedback_v1_send_done(r);
}

inline void newFeedback(wl_client* client, wl_resource* manager, uint32_t id) {
  wl_resource* r = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface,
                                      wl_resource_get_version(manager), id);
  if (!r) return wl_client_post_no_memory(client);
  static const struct zwp_linux_dmabuf_feedback_v1_interface impl = {
      /*destroy*/ [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
  };
  wl_resource_set_implementation(r, &impl, nullptr, nullptr);
  sendFeedback(r);
}

inline const struct zwp_linux_dmabuf_v1_interface kManagerImpl = {
    /*destroy*/ [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
    /*create_params*/
    [](wl_client* client, wl_resource* manager, uint32_t id) {
      wl_resource* r = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
                                          wl_resource_get_version(manager), id);
      if (!r) return wl_client_post_no_memory(client);
      auto* p = new Params();
      for (int& fd : p->attr.fd) fd = -1;
      wl_resource_set_implementation(r, &kParamsImpl, p, [](wl_resource* r) {
        Params* p = params(r);
        for (int i = 0; i < 4; i++)
          if (p->set[i] && p->attr.fd[i] >= 0) close(p->attr.fd[i]);
        delete p;
      });
    },
    /*get_default_feedback*/ newFeedback,
    /*get_surface_feedback*/
    [](wl_client* client, wl_resource* manager, uint32_t id, wl_resource*) { newFeedback(client, manager, id); },
};

inline void bind(wl_client* client, void*, uint32_t version, uint32_t id) {
  wl_resource* r = wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, int(version), id);
  if (!r) return wl_client_post_no_memory(client);
  wl_resource_set_implementation(r, &kManagerImpl, nullptr, nullptr);
  // Before v4 the formats come as events (v3: with their modifier).
  if (version < 4) {
    for (uint32_t f : s.formats) {
      if (version >= 3) zwp_linux_dmabuf_v1_send_modifier(r, f, uint32_t(DRM_FORMAT_MOD_LINEAR >> 32),
                                                          uint32_t(DRM_FORMAT_MOD_LINEAR & 0xffffffff));
      else zwp_linux_dmabuf_v1_send_format(r, f);
    }
  }
}

// The v4 format table: {u32 format, u32 pad, u64 modifier} per entry, in a
// sealed memfd every client maps.
inline bool makeFormatTable() {
  struct Entry {
    uint32_t format, pad;
    uint64_t modifier;
  };
  std::vector<Entry> table;
  for (uint32_t f : s.formats) table.push_back({f, 0, DRM_FORMAT_MOD_LINEAR});
  int fd = memfd_create("jsui-wl-dmabuf-formats", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) return false;
  const size_t size = table.size() * sizeof(Entry);
  if (write(fd, table.data(), size) != ssize_t(size)) {
    close(fd);
    return false;
  }
  fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
  s.formatTable = fd;
  s.formatTableSize = size;
  return true;
}

// Offers zwp_linux_dmabuf_v1 (v4, with feedback) on `display`, importing
// through the GL context current now (the shell's, on `dpy`). False when
// nothing could import a dma-buf.
inline bool create(wl_display* display, EGLDisplay dpy, std::function<bool()> makeCurrent) {
  s.dpy = dpy;
  s.makeCurrent = std::move(makeCurrent);
  const char* eglExts = eglQueryString(dpy, EGL_EXTENSIONS);
  logf("EGL extensions: %s", eglExts ? eglExts : "(none)");
  s.createImage = reinterpret_cast<CreateImageFn>(eglGetProcAddress("eglCreateImageKHR"));
  s.destroyImage = reinterpret_cast<DestroyImageFn>(eglGetProcAddress("eglDestroyImageKHR"));
  s.imageTarget = reinterpret_cast<ImageTargetFn>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  const bool images = s.createImage && s.destroyImage && s.imageTarget;
  const char* env = getenv("JSUI_WL_DMABUF_IMPORT");  // egl | gralloc | cpu: only that path
  const std::string only = env ? env : "";
  s.eglImport = images && hasExt(eglExts, "EGL_EXT_image_dma_buf_import") && (only.empty() || only == "egl");
  s.eglModifiers = s.eglImport && hasExt(eglExts, "EGL_EXT_image_dma_buf_import_modifiers");
  s.hyCreateNative = reinterpret_cast<HyCreateNativeFn>(hybrisProc("eglHybrisCreateNativeBuffer"));
  s.hyCreateRemote = reinterpret_cast<HyCreateRemoteFn>(hybrisProc("eglHybrisCreateRemoteBuffer"));
  s.hyInfo = reinterpret_cast<HyInfoFn>(hybrisProc("eglHybrisGetNativeBufferInfo"));
  s.hySerialize = reinterpret_cast<HySerializeFn>(hybrisProc("eglHybrisSerializeNativeBuffer"));
  s.hyRelease = reinterpret_cast<HyReleaseFn>(hybrisProc("eglHybrisReleaseNativeBuffer"));
  if (images && s.hyCreateNative && s.hyCreateRemote && s.hyInfo && s.hySerialize && s.hyRelease &&
      (only.empty() || only == "gralloc")) {
    // Each format gralloc allocates with a handle we understand.
    for (const Format& f : kFormats) {
      std::vector<int> ints, fds;
      if (!grallocTemplate(&f, 72, 40, &ints, &fds)) continue;
      for (int fd : fds) close(fd);
      s.grallocFormats.push_back(&f);
    }
    s.gralloc = !s.grallocFormats.empty();
    if (!s.gralloc) logf("dma-buf: gralloc's buffer handles aren't QTI's; no gralloc import");
  }
  const char* glExts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
  s.bgra = hasExt(glExts, "GL_EXT_texture_format_BGRA8888");
  s.unpackRowLength = (glVersion && std::strstr(glVersion, "OpenGL ES 3")) || hasExt(glExts, "GL_EXT_unpack_subimage");
  s.cpu = only.empty() || only == "cpu";

  struct stat st {};
  // Feedback names a DRM device; renderD128 is the display's (msm_drm), the
  // only one there is. Turnip renders through KGSL and only compares it.
  if (stat("/dev/dri/renderD128", &st) != 0 && stat("/dev/dri/card0", &st) != 0) {
    logf("dma-buf: no DRM device for feedback; no zwp_linux_dmabuf_v1");
    return false;
  }
  s.mainDevice = st.st_rdev;
  std::string list;
  for (const Format& f : kFormats) {
    bool gpu = s.eglImport || std::find(s.grallocFormats.begin(), s.grallocFormats.end(), &f) != s.grallocFormats.end();
    bool cpu = s.cpu && f.glFormat && (f.glFormat != kBgra || s.bgra);
    if (!gpu && !cpu) continue;
    s.formats.push_back(f.drm);
    list += std::string(list.empty() ? "" : " ") + std::string(reinterpret_cast<const char*>(&f.drm), 4) +
            (gpu ? "" : "(cpu)");
  }
  if (list.empty()) {
    logf("dma-buf: nothing can import; no zwp_linux_dmabuf_v1");
    return false;
  }
  if (!makeFormatTable()) {
    logf("dma-buf: no memfd for the format table; no zwp_linux_dmabuf_v1");
    return false;
  }
  s.global = wl_global_create(display, &zwp_linux_dmabuf_v1_interface, 4, nullptr, bind);
  if (!s.global) return false;
  wlr_buffer_register_resource_interface(&kResourceInterface);
  wlegl::g.other = isBuffer;
  logf("zwp_linux_dmabuf_v1: LINEAR %s; import %s", list.c_str(),
       s.eglImport ? "EGL" : s.gralloc ? "gralloc (hybris)" : "CPU upload");
  return true;
}

}  // namespace dmabuf

#endif  // JSUI_WL_MIR_GL
