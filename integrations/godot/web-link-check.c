// Link check for the threaded web build (CI, README "Threaded web build"):
// a threaded program that, like a Godot web template, has its own HarfBuzz /
// FreeType / libpng / zlib symbols links libjsui_godot.web.threads.wasm32.a
// without clashes, and its calls still reach its own copies.
//
//   emcc -O2 -pthread -sSUPPORT_LONGJMP=wasm --use-port=emdawnwebgpu \
//     integrations/godot/web-link-check.c \
//     integrations/godot/demo/bin/libjsui_godot.web.threads.wasm32.a -o check.js
//   node check.js   # prints "ok"
#include <stdio.h>

typedef int GDExtensionBool;
GDExtensionBool jsui_godot_init(void* get_proc_address, void* library, void* initialization);

// The "engine's" copies of names JSUI's Skia also defines.
int hb_buffer_create(void) { return 1; }
int FT_Init_FreeType(void* library) { return 2; }
void* png_create_read_struct(void) { return (void*)3; }
int inflate(void* strm, int flush) { return 4; }
int u_errorName_74(int code) { return 5; }

int main(int argc, char** argv) {
  (void)argv;
  // Referenced (so the whole integration is linked), never called.
  if (argc > 1000) return jsui_godot_init(0, 0, 0);
  int ok = hb_buffer_create() == 1 && FT_Init_FreeType(0) == 2 && png_create_read_struct() == (void*)3 &&
           inflate(0, 0) == 4 && u_errorName_74(0) == 5;
  printf("%s\n", ok ? "ok" : "FAIL: a call went to JSUI's copy");
  return ok ? 0 : 1;
}
