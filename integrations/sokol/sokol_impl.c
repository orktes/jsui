// The sokol implementations (compiled as Objective-C on Apple platforms).
// The GL backend: Skia draws the UI with OpenGL on sokol_app's context.
#define SOKOL_IMPL
#define SOKOL_GLCORE
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "util/sokol_gl.h"
