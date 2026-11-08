// Offscreen GLES through EGL surfaceless: draw a triangle into an FBO, read it back,
// and time a batch of draws with glFinish.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static GLuint sh(GLenum t, const char *s) { GLuint o = glCreateShader(t); glShaderSource(o, 1, &s, NULL); glCompileShader(o); return o; }
int main(void) {
  PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
  EGLDisplay d = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint ma, mi;
  if (!eglInitialize(d, &ma, &mi)) { fprintf(stderr, "eglInitialize failed 0x%x\n", eglGetError()); return 1; }
  printf("EGL %d.%d vendor=%s\n", ma, mi, eglQueryString(d, EGL_VENDOR));
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
  if (!c || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) { fprintf(stderr, "context failed 0x%x\n", eglGetError()); return 1; }
  printf("GL_RENDERER=%s\nGL_VERSION=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
  const int W = 1024, H = 1024;
  GLuint tex, fbo; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  GLuint p = glCreateProgram();
  glAttachShader(p, sh(GL_VERTEX_SHADER, "attribute vec2 a; void main(){gl_Position=vec4(a,0.,1.);}"));
  glAttachShader(p, sh(GL_FRAGMENT_SHADER, "precision mediump float; void main(){gl_FragColor=vec4(0.,1.,0.,1.);}"));
  glBindAttribLocation(p, 0, "a"); glLinkProgram(p); glUseProgram(p);
  float v[] = { -1, -1, 3, -1, -1, 3 };
  glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, v); glEnableVertexAttribArray(0);
  glViewport(0, 0, W, H); glClearColor(1, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  unsigned char px[4]; glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
  printf("center pixel=%u,%u,%u,%u (want 0,255,0,255)\n", px[0], px[1], px[2], px[3]);
  const int N = 100; double t0 = now();
  for (int i = 0; i < N; i++) { glClear(GL_COLOR_BUFFER_BIT); for (int k = 0; k < 10; k++) glDrawArrays(GL_TRIANGLES, 0, 3); glFinish(); }
  printf("1024x1024 clear + 10 full-screen draws + finish: %.2f ms\n", (now() - t0) * 1000 / N);
  int ok = px[1] == 255 && px[0] == 0;
  puts(ok ? "GL OFFSCREEN OK" : "GL OFFSCREEN WRONG PIXELS");
  return !ok;
}
