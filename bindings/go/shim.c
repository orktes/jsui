// C side of the callbacks: libjsui calls these, they call the exported Go
// functions in callbacks.go with the runtime's cgo.Handle as `user`.
#include <stdint.h>
#include "shim.h"
#include "_cgo_export.h"

static void on_log(void* user, JsuiLogLevel level, const char* message) {
    goJsuiLog((uintptr_t)user, (int)level, (char*)message);
}
static void on_message(void* user, const char* name, const char* json) {
    goJsuiMessage((uintptr_t)user, (char*)name, (char*)json);
}
static void on_state(void* user, const char* key, const char* json) {
    goJsuiState((uintptr_t)user, (char*)key, (char*)json);
}
static void on_call(void* user, uint64_t call_id, const char* name, const char* args_json) {
    goJsuiCall((uintptr_t)user, call_id, (char*)name, (char*)args_json);
}
static void on_call_cancel(void* user, uint64_t call_id) {
    goJsuiCallCancel((uintptr_t)user, call_id);
}
static void on_call_ui_done(void* user, uint64_t call_id, int ok, const char* result) {
    goJsuiCallUIDone((uintptr_t)user, call_id, ok, (char*)result);
}

void gojsui_set_callbacks(JsuiConfig* cfg, uintptr_t handle) {
    void* user = (void*)handle;
    cfg->log = on_log;
    cfg->log_user = user;
    cfg->on_message = on_message;
    cfg->message_user = user;
    cfg->on_state = on_state;
    cfg->state_user = user;
    cfg->on_call = on_call;
    cfg->on_call_cancel = on_call_cancel;
    cfg->call_user = user;
}

uint64_t gojsui_call_ui(JsuiRuntime* rt, const char* name, const char* args_json, uintptr_t handle) {
    return jsui_call_ui(rt, name, args_json, on_call_ui_done, (void*)handle);
}
