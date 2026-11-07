// Wayland EGL frame-cost probe: xdg toplevel, draws N full-screen quads per frame,
// times glFinish (GPU render) and eglSwapBuffers (present) separately.
// Usage: wlgl [seconds] [quads-per-frame]
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "xdg-shell-client-protocol.h"
static struct wl_compositor *comp; static struct xdg_wm_base *wm;
static int W = 800, H = 600, configured, closed, resized;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void reg_global(void *d, struct wl_registry *r, uint32_t n, const char *i, uint32_t v) {
  if (!strcmp(i, "wl_compositor")) comp = wl_registry_bind(r, n, &wl_compositor_interface, 4);
  else if (!strcmp(i, "xdg_wm_base")) wm = wl_registry_bind(r, n, &xdg_wm_base_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };
static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t s) { xdg_wm_base_pong(b, s); }
static const struct xdg_wm_base_listener wm_l = { wm_ping };
static void surf_conf(void *d, struct xdg_surface *s, uint32_t serial) { xdg_surface_ack_configure(s, serial); configured = 1; }
static const struct xdg_surface_listener surf_l = { surf_conf };
static void top_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { if (w > 0 && h > 0 && (w != W || h != H)) { W = w; H = h; resized = 1; } }
static void top_close(void *d, struct xdg_toplevel *t) { closed = 1; }
static const struct xdg_toplevel_listener top_l = { top_conf, top_close };
static GLuint sh(GLenum t, const char *s) { GLuint o = glCreateShader(t); glShaderSource(o, 1, &s, NULL); glCompileShader(o); return o; }
int main(int argc, char **argv) {
  double secs = argc > 1 ? atof(argv[1]) : 10; int quads = argc > 2 ? atoi(argv[2]) : 4;
  struct wl_display *dpy = wl_display_connect(NULL); if (!dpy) { fprintf(stderr, "no wayland\n"); return 1; }
  struct wl_registry *reg = wl_display_get_registry(dpy); wl_registry_add_listener(reg, &reg_l, NULL); wl_display_roundtrip(dpy);
  xdg_wm_base_add_listener(wm, &wm_l, NULL);
  struct wl_surface *s = wl_compositor_create_surface(comp);
  struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm, s); xdg_surface_add_listener(xs, &surf_l, NULL);
  struct xdg_toplevel *top = xdg_surface_get_toplevel(xs); xdg_toplevel_add_listener(top, &top_l, NULL);
  xdg_toplevel_set_title(top, "wlgl"); xdg_toplevel_set_app_id(top, "wlgl");
  wl_surface_commit(s); while (!configured) wl_display_dispatch(dpy);
  EGLDisplay ed = eglGetDisplay((EGLNativeDisplayType)dpy); eglInitialize(ed, NULL, NULL);
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLint ca[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
  EGLConfig cfg; EGLint nc; eglChooseConfig(ed, ca, &cfg, 1, &nc);
  EGLint cx[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  EGLContext ctx = eglCreateContext(ed, cfg, EGL_NO_CONTEXT, cx);
  struct wl_egl_window *ew = wl_egl_window_create(s, W, H);
  EGLSurface es = eglCreateWindowSurface(ed, cfg, (EGLNativeWindowType)ew, NULL);
  eglMakeCurrent(ed, es, es, ctx);
  if (getenv("WLGL_SWAP0")) eglSwapInterval(ed, 0);
  printf("size %dx%d renderer %s\n", W, H, glGetString(GL_RENDERER));
  GLuint p = glCreateProgram();
  glAttachShader(p, sh(GL_VERTEX_SHADER, "attribute vec2 a; varying vec2 v; void main(){v=a;gl_Position=vec4(a,0.,1.);}"));
  glAttachShader(p, sh(GL_FRAGMENT_SHADER, "precision mediump float; varying vec2 v; uniform float t; void main(){gl_FragColor=vec4(0.5+0.5*sin(t+v.x*3.),0.5+0.5*v.y,0.3,0.25);}"));
  glBindAttribLocation(p, 0, "a"); glLinkProgram(p); glUseProgram(p);
  GLint ut = glGetUniformLocation(p, "t");
  float vtx[] = { -1, -1, 3, -1, -1, 3 }; glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, vtx); glEnableVertexAttribArray(0);
  glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  double t0 = now(), tr = 0, tsw = 0; int frames = 0;
  while (!closed && now() - t0 < secs) {
    wl_display_dispatch_pending(dpy);
    if (getenv("WLGL_RESIZE") && (frames == 20 || frames == 40)) { int h = frames == 20 ? H / 2 : H; wl_egl_window_resize(ew, W, h, 0, 0); printf("frame %d: wl_egl_window_resize %dx%d\n", frames, W, h); }
    if (getenv("WLGL_RESIZE") && (frames == 25 || frames == 45)) { EGLint qw, qh; eglQuerySurface(ed, es, EGL_WIDTH, &qw); eglQuerySurface(ed, es, EGL_HEIGHT, &qh); printf("frame %d: egl surface %dx%d\n", frames, qw, qh); }
    if (resized) { resized = 0; wl_egl_window_resize(ew, W, H, 0, 0); printf("resize %dx%d\n", W, H); }
    double a = now();
    glViewport(0, 0, W, H); glClearColor(0.1, 0.1, 0.2, 1); glClear(GL_COLOR_BUFFER_BIT);
    glUniform1f(ut, (float)(a - t0));
    for (int i = 0; i < quads; i++) glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish(); double b = now();
    if (frames == 30) { GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp); EGLint qw, qh; eglQuerySurface(ed, es, EGL_WIDTH, &qw); eglQuerySurface(ed, es, EGL_HEIGHT, &qh); printf("frame 30: egl surface %dx%d\n", qw, qh); }
    eglSwapBuffers(ed, es); double c = now();
    tr += b - a; tsw += c - b; frames++;
  }
  double el = now() - t0;
  printf("frames %d in %.1fs = %.1f fps; render+finish %.1f ms, swap %.1f ms per frame\n", frames, el, frames / el, tr * 1000 / frames, tsw * 1000 / frames);
  return 0;
}
