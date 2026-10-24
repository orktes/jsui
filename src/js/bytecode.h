// Precompiled bundles: `jsui build` compiles the bundle to QuickJS bytecode
// so the runtime skips parsing and keeps no function source in memory. The
// file is a small container that also carries the source as a fallback:
//
//   "\0JSUIBC\1"           magic (8 bytes; never valid JS source)
//   u32 le  key length,     key (bytecodeKey() of the compiling QuickJS)
//   u32 le  bytecode length, bytecode (JS_WriteObject of the compiled script)
//   rest    UTF-8 source (may be empty)
//
// Bytecode is only valid for the exact QuickJS it was compiled with (version
// and JSUI's patches), so a runtime whose key differs runs the source.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace jsui {

// Identifies the QuickJS build: its version plus a hash of the patched
// quickjs.c / quickjs.h / quickjs-opcode.h (JSUI_BYTECODE_KEY, CMake).
const char* bytecodeKey();

struct BundleParts {
  std::string_view key;
  std::string_view bytecode;
  std::string_view source;
};

// True when `data` is a container; fills `parts` (views into `data`).
bool isBundleContainer(std::string_view data);
bool parseBundleContainer(std::string_view data, BundleParts& parts);

// Compiles `source` (a global script) to a container with this runtime's
// key; `keepSource` embeds the source as the fallback. Returns false and
// sets `error` on a syntax error.
bool compileBundle(std::string_view source, const std::string& name, bool keepSource, std::string& out,
                   std::string& error);

}  // namespace jsui
