// LD_PRELOAD for apps started from jsui-wl: the shipped Camera / Barcode
// Reader (lomiri-camera-app) keeps a QGuiApplication alive in main() and then
// constructs its CameraApplication, a second one. The second shares the first
// one's platform plugin and initializes it again; under QtWayland 5.15 that
// restarts the display's event thread while the old one still waits on its
// condition, and the app hangs in its constructor (#247).
//
// Here the first QGuiApplication of that binary is made with the "minimal"
// platform (no Wayland connection), and torn down when the second is
// constructed, so the second starts afresh on Wayland. main()'s own
// destructor call on the first is then skipped.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void (*Ctor)(void*, int*, char**, int);
typedef void (*Dtor)(void*);

static void* first;  // the stand-in app while alive, then its address (destroyed)
static int firstAlive;

static int isTarget(void) {
  static int t = -1;
  if (t < 0) {
    char p[512];
    ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
    p[n > 0 ? n : 0] = 0;
    const char* b = strrchr(p, '/');
    t = strcmp(b ? b + 1 : p, "lomiri-camera-app") == 0;
  }
  return t;
}

static void construct(const char* sym, void* self, int* argc, char** argv, int flags) {
  Ctor real = (Ctor)dlsym(RTLD_NEXT, sym);
  if (!isTarget()) {
    real(self, argc, argv, flags);
    return;
  }
  if (!first) {
    const char* was = getenv("QT_QPA_PLATFORM");
    char* saved = was ? strdup(was) : NULL;
    setenv("QT_QPA_PLATFORM", "minimal", 1);
    real(self, argc, argv, flags);
    if (saved) setenv("QT_QPA_PLATFORM", saved, 1);
    else unsetenv("QT_QPA_PLATFORM");
    free(saved);
    first = self;
    firstAlive = 1;
    return;
  }
  if (firstAlive) {
    ((Dtor)dlsym(RTLD_NEXT, "_ZN15QGuiApplicationD1Ev"))(first);
    firstAlive = 0;
  }
  real(self, argc, argv, flags);
}

void _ZN15QGuiApplicationC1ERiPPci(void* self, int* argc, char** argv, int flags) {
  construct("_ZN15QGuiApplicationC1ERiPPci", self, argc, argv, flags);
}

void _ZN15QGuiApplicationC2ERiPPci(void* self, int* argc, char** argv, int flags) {
  construct("_ZN15QGuiApplicationC2ERiPPci", self, argc, argv, flags);
}

void _ZN15QGuiApplicationD1Ev(void* self) {
  if (self == first && !firstAlive) return;
  ((Dtor)dlsym(RTLD_NEXT, "_ZN15QGuiApplicationD1Ev"))(self);
}
