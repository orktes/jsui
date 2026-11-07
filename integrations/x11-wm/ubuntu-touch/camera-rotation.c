// LD_PRELOAD for apps started from jsui-wl: the camera preview the right way
// up on a turned desktop.
//
// Lomiri keeps Camera's window in the panel's own orientation (the app has
// X-Lomiri-Rotates-Window-Contents) and the app turns the preview by the
// sensor's mounting angle alone. jsui-wl gives it a window as turned as the
// desktop (--rotate, auto-rotate), so on the Tab A7's portrait panel shown
// landscape the preview came out a quarter turn off. qtubuntu-camera takes an
// override of a camera's angle from the property aal.camera.orientations.N;
// it is answered here with the mounting angle less the desktop's turn
// (jsui-wl writes it to $XDG_RUNTIME_DIR/jsui-wl.rotation), plus it for a
// front camera, whose image is mirrored.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*PropertyGet)(const char*, char*, const char*);
typedef int (*DeviceInfo)(int, int*, int*);

static int desktopRotation(void) {
  const char* rt = getenv("XDG_RUNTIME_DIR");
  if (!rt) return -1;
  char path[512];
  snprintf(path, sizeof path, "%s/jsui-wl.rotation", rt);
  FILE* f = fopen(path, "r");
  if (!f) return -1;
  int deg = -1;
  if (fscanf(f, "%d", &deg) != 1) deg = -1;
  fclose(f);
  return deg;
}

int property_get(const char* key, char* value, const char* default_value) {
  static PropertyGet real;
  if (!real) real = (PropertyGet)dlsym(RTLD_NEXT, "property_get");
  static const char kPrefix[] = "aal.camera.orientations.";
  if (key && strncmp(key, kPrefix, sizeof kPrefix - 1) == 0) {
    int rot = desktopRotation();
    // libcamera (libhybris) is loaded by the camera plugin, not globally.
    void* lib = dlopen("libcamera.so.1", RTLD_NOW | RTLD_NOLOAD);
    DeviceInfo info = lib ? (DeviceInfo)dlsym(lib, "android_camera_get_device_info") : NULL;
    int facing = -1, mount = -1;
    if (rot >= 0 && info && info(atoi(key + sizeof kPrefix - 1), &facing, &mount) == 0 && mount >= 0) {
      int deg = facing == 1 ? (mount + rot) % 360 : (mount - rot + 360) % 360;
      return snprintf(value, 92, "%d", deg);  // PROP_VALUE_MAX
    }
  }
  return real ? real(key, value, default_value) : 0;
}
