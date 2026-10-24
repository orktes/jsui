#include <cstdio>
#include <cstring>

#include "test.h"

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// clang-format off
#include <dbghelp.h>
// clang-format on
#pragma comment(lib, "dbghelp.lib")

namespace {

const char* g_running = "";

// A crash (access violation and the like) names the test and prints the
// stack, symbolized when the PDB is next to the executable.
LONG WINAPI onCrash(EXCEPTION_POINTERS* e) {
  std::printf("CRASH in %s: exception 0x%08lx at %p\n", g_running, e->ExceptionRecord->ExceptionCode,
              e->ExceptionRecord->ExceptionAddress);
  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);
  CONTEXT ctx = *e->ContextRecord;
  STACKFRAME64 frame{};
  frame.AddrPC.Offset = ctx.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = ctx.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = ctx.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;
  for (int i = 0; i < 40; i++) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &ctx, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
        !frame.AddrPC.Offset)
      break;
    char buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisp = 0;
    const bool named = SymFromAddr(process, frame.AddrPC.Offset, &disp, sym);
    const bool lined = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisp, &line);
    char module[MAX_PATH] = "?";
    HMODULE m = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(frame.AddrPC.Offset), &m))
      GetModuleFileNameA(m, module, MAX_PATH);
    std::printf("  #%d %s+0x%llx (%s:%lu) [%s+0x%llx]\n", i, named ? sym->Name : "?",
                (unsigned long long)disp, lined ? line.FileName : "?", lined ? line.LineNumber : 0ul,
                std::strrchr(module, '\\') ? std::strrchr(module, '\\') + 1 : module,
                (unsigned long long)(frame.AddrPC.Offset - reinterpret_cast<DWORD64>(m)));
  }
  std::fflush(stdout);
  return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace
#endif

int main(int argc, char** argv) {
  // Unbuffered so CI logs show progress even if a test crashes the process.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
#if defined(_WIN32)
  SetUnhandledExceptionFilter(onCrash);
#endif
  // Arguments are name filters (substring match; a test runs if any matches).
  int ran = 0, failed = 0;
  for (auto& c : jsui_test::registry()) {
    bool selected = argc < 2;
    for (int i = 1; i < argc && !selected; i++) selected = std::strstr(c.name, argv[i]) != nullptr;
    if (!selected) continue;
    int before = jsui_test::failures();
#if defined(_WIN32)
    g_running = c.name;
#endif
    c.fn();
    ran++;
    bool ok = jsui_test::failures() == before;
    if (!ok) failed++;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", c.name);
  }
  std::printf("%d tests, %d failed\n", ran, failed);
  return failed ? 1 : 0;
}
