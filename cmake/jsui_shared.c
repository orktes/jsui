/* jsui_shared (-DJSUI_BUILD_SHARED=ON) is the static jsui library linked
 * whole into a shared library that exports the C API alone; CMake wants at
 * least one source of its own. */
typedef int jsui_shared_unused;
