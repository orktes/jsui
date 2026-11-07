// jsui-wl on Ubuntu Touch: apps' GPU buffers (android_wlegl), shown in place.
//
// Halium apps get EGL from libhybris, whose Wayland platform draws into
// Android gralloc buffers and hands them to the compositor over the
// android_wlegl protocol. The server half of that protocol is in libhybris
// too: eglBindWaylandDisplayWL on the compositor's EGL display adds the
// android_wlegl global, its wl_buffers can be queried with
// eglQueryWaylandBufferWL, and eglCreateImageKHR(EGL_WAYLAND_BUFFER_WL) turns
// one into an EGLImage. jsui-wl binds one GL texture to each buffer and gives
// it to JSUI as the window's host texture: no copy, nothing read on the CPU.
//
// wlroots doesn't know these buffers: wlegl::bind registers a buffer resource
// interface for them, so surfaces commit and map as usual. The pixman renderer
// can't make a texture from them (it logs "Failed to upload buffer" and keeps
// none), so jsui-wl takes each committed buffer straight off the surface
// (wlr_surface.current.buffer in the commit event) and keeps it locked while
// it is shown.
#pragma once

#ifdef JSUI_WL_MIR_GL

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

namespace wlegl {

// EGL_WL_bind_wayland_display / EGL_KHR_image_base (libhybris' own entry points)
constexpr EGLenum kWaylandBuffer = 0x31D5;  // EGL_WAYLAND_BUFFER_WL
constexpr EGLint kTextureFormat = 0x3080;   // EGL_TEXTURE_FORMAT
constexpr EGLint kTextureRgb = 0x305D;      // EGL_TEXTURE_RGB
using BindFn = EGLBoolean (*)(EGLDisplay, wl_display*);
using QueryFn = EGLBoolean (*)(EGLDisplay, wl_resource*, EGLint, EGLint*);
using CreateImageFn = void* (*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*);
using DestroyImageFn = EGLBoolean (*)(EGLDisplay, void*);
using ImageTargetFn = void (*)(GLenum, void*);

struct Gl {
  EGLDisplay dpy = EGL_NO_DISPLAY;
  QueryFn query = nullptr;
  CreateImageFn createImage = nullptr;
  DestroyImageFn destroyImage = nullptr;
  ImageTargetFn imageTarget = nullptr;
  // wl_buffers of another protocol of jsui-wl's own (dmabuf.h), which must
  // never reach libhybris' query.
  bool (*other)(wl_resource*) = nullptr;
};
inline Gl g;

struct Buffer {
  wlr_buffer base;
  wl_resource* resource = nullptr;
  wl_listener resourceDestroy{}, release{};
  void* image = nullptr;  // EGLImageKHR
  GLuint tex = 0;
  bool opaque = false;  // RGBX / RGB565: no alpha
};

inline void handleResourceDestroy(wl_listener* l, void*) {
  Buffer* b = wl_container_of(l, b, resourceDestroy);
  wl_list_remove(&b->resourceDestroy.link);
  wl_list_init(&b->resourceDestroy.link);
  b->resource = nullptr;
  // The EGLImage holds its own reference to the gralloc buffer: a texture
  // still shown stays valid until the buffer is unlocked.
  wlr_buffer_drop(&b->base);
}

inline const wlr_buffer_impl kImpl = {
    /*destroy*/
    [](wlr_buffer* wb) {
      auto* b = reinterpret_cast<Buffer*>(wb);
      // Called from the Wayland loop, where the window's GL context is current.
      if (b->tex && eglGetCurrentContext() != EGL_NO_CONTEXT) glDeleteTextures(1, &b->tex);
      if (b->image) g.destroyImage(g.dpy, b->image);
      wl_list_remove(&b->resourceDestroy.link);
      wl_list_remove(&b->release.link);
      delete b;
    },
    /*get_dmabuf*/ nullptr,
    /*get_shm*/ nullptr,
    /*begin_data_ptr_access*/ nullptr,
    /*end_data_ptr_access*/ nullptr,
};

// `base` comes first: a wlr_buffer of ours is the Buffer.
inline Buffer* from(wlr_buffer* b) { return b && b->impl == &kImpl ? reinterpret_cast<Buffer*>(b) : nullptr; }

inline const wlr_buffer_resource_interface kResourceInterface = {
    /*name*/ "android_wlegl",
    // libhybris doesn't check what it is given, so rule out wl_shm first
    // (wlroots asks it before us anyway; no other buffer type is offered).
    /*is_instance*/
    [](wl_resource* r) -> bool {
      EGLint w = 0;
      if (g.other && g.other(r)) return false;
      return !wl_shm_buffer_get(r) && g.query(g.dpy, r, EGL_WIDTH, &w) && w > 0;
    },
    /*from_resource*/
    [](wl_resource* r) -> wlr_buffer* {
      if (wl_listener* l = wl_resource_get_destroy_listener(r, handleResourceDestroy)) {
        Buffer* b = wl_container_of(l, b, resourceDestroy);
        return &b->base;
      }
      EGLint w = 0, h = 0, fmt = 0;
      if (!g.query(g.dpy, r, EGL_WIDTH, &w) || !g.query(g.dpy, r, EGL_HEIGHT, &h)) return nullptr;
      auto* b = new Buffer();
      b->resource = r;
      b->opaque = g.query(g.dpy, r, kTextureFormat, &fmt) && fmt == kTextureRgb;
      wlr_buffer_init(&b->base, &kImpl, w, h);
      b->resourceDestroy.notify = handleResourceDestroy;
      wl_resource_add_destroy_listener(r, &b->resourceDestroy);
      b->release.notify = [](wl_listener* l, void*) {
        Buffer* b = wl_container_of(l, b, release);
        if (b->resource) wl_buffer_send_release(b->resource);
      };
      wl_signal_add(&b->base.events.release, &b->release);
      return &b->base;
    },
};

// Offers android_wlegl on `display` through the EGL display jsui-wl renders
// with. False when that EGL isn't libhybris (or lacks the extensions).
inline bool bind(wl_display* display, EGLDisplay dpy) {
  auto bindFn = reinterpret_cast<BindFn>(eglGetProcAddress("eglBindWaylandDisplayWL"));
  g.query = reinterpret_cast<QueryFn>(eglGetProcAddress("eglQueryWaylandBufferWL"));
  g.createImage = reinterpret_cast<CreateImageFn>(eglGetProcAddress("eglCreateImageKHR"));
  g.destroyImage = reinterpret_cast<DestroyImageFn>(eglGetProcAddress("eglDestroyImageKHR"));
  g.imageTarget = reinterpret_cast<ImageTargetFn>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  if (!bindFn || !g.query || !g.createImage || !g.destroyImage || !g.imageTarget) return false;
  if (!bindFn(dpy, display)) return false;
  g.dpy = dpy;
  wlr_buffer_register_resource_interface(&kResourceInterface);
  return true;
}

// The GL texture showing `b` (made on first use), in the current context.
inline GLuint texture(Buffer* b) {
  if (b->tex) return b->tex;
  if (!b->resource) return 0;
  const EGLint attrs[] = {EGL_NONE};
  b->image = g.createImage(g.dpy, EGL_NO_CONTEXT, kWaylandBuffer, reinterpret_cast<EGLClientBuffer>(b->resource), attrs);
  if (!b->image) return 0;
  glGenTextures(1, &b->tex);
  glBindTexture(GL_TEXTURE_2D, b->tex);
  g.imageTarget(GL_TEXTURE_2D, b->image);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  return b->tex;
}

}  // namespace wlegl

#endif  // JSUI_WL_MIR_GL
