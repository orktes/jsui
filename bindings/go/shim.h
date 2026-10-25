#ifndef GOJSUI_SHIM_H
#define GOJSUI_SHIM_H
#include <stdint.h>
#include "jsui/jsui.h"

void gojsui_set_callbacks(JsuiConfig* cfg, uintptr_t handle);
void gojsui_set_js_ready(JsuiConfig* cfg, uintptr_t handle);
uint64_t gojsui_call_ui(JsuiRuntime* rt, const char* name, const char* args_json, uintptr_t handle);

#endif
