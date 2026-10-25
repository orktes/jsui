#!/usr/bin/env python3
"""Make every symbol a relocatable WebAssembly object defines local, but a few.

    wasm_localize.py in.o out.o --keep jsui_godot_init [--keep NAME ...]

Static linking JSUI into another program (the Godot integration inside a
Godot web export template, integrations/godot) puts two copies of several
libraries in one link: Skia's HarfBuzz, FreeType, ICU, libpng, zlib, libwebp,
libjpeg-turbo... next to the engine's own, at other versions. wasm-ld has no
--exclude-libs / -Bsymbolic and llvm-objcopy cannot localize wasm symbols, so
JSUI's side is first linked into one relocatable object (wasm-ld -r) and this
tool then rewrites that object's symbol table (the "linking" custom section):

* every defined symbol that is not local becomes local (binding LOCAL, not
  WEAK), so it neither clashes with nor binds to a symbol of the same name in
  the rest of the program. References inside the object were already
  resolved to it by `wasm-ld -r`;
* undefined symbols stay as they are: libc, libc++, Emscripten's runtime and
  the engine's Emdawnwebgpu (wgpu*) resolve against the program;
* kept global: the --keep names (the entry point, functions JS calls) and
  Emscripten's EM_JS / EM_ASM markers (__em_js__*, __em_lib_deps*), which its
  linker looks up by name. Other exported / no-strip symbols
  (EMSCRIPTEN_KEEPALIVE: JSUI's C API on the web) lose those marks too, so the
  program's linker drops what nothing calls;
* COMDAT groups are dropped: with every member local there is nothing to fold
  with the program's copies of the same inline functions (folding would
  discard the sections the local symbols point into).

Relocations refer to symbols by index and sections by offset; neither
changes, so only the linking section is rewritten.
"""
import argparse
import sys

WASM_SYMBOL_TABLE = 8
WASM_COMDAT_INFO = 7

KIND_FUNCTION, KIND_DATA, KIND_GLOBAL, KIND_SECTION, KIND_TAG, KIND_TABLE = range(6)

F_WEAK = 0x1
F_LOCAL = 0x2
F_UNDEFINED = 0x10
F_EXPORTED = 0x20
F_EXPLICIT_NAME = 0x40
F_NO_STRIP = 0x80

KEEP_PREFIXES = ("__em_js__", "__em_lib_deps", "__em_asm")


def uleb(data, pos):
    result = shift = 0
    while True:
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return result, pos


def enc_uleb(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def name_at(data, pos):
    n, pos = uleb(data, pos)
    return data[pos:pos + n].decode("utf-8", "replace"), pos + n


def rewrite_symtab(payload, keep, stats):
    pos = 0
    count, pos = uleb(payload, pos)
    out = bytearray(enc_uleb(count))
    for _ in range(count):
        kind = payload[pos]
        pos += 1
        flags, pos = uleb(payload, pos)
        start = pos
        name = None
        if kind in (KIND_FUNCTION, KIND_GLOBAL, KIND_TAG, KIND_TABLE):
            _, pos = uleb(payload, pos)
            if not flags & F_UNDEFINED or flags & F_EXPLICIT_NAME:
                name, pos = name_at(payload, pos)
        elif kind == KIND_DATA:
            name, pos = name_at(payload, pos)
            if not flags & F_UNDEFINED:
                for _ in range(3):
                    _, pos = uleb(payload, pos)
        elif kind == KIND_SECTION:
            _, pos = uleb(payload, pos)
        else:
            sys.exit(f"wasm_localize: unknown symbol kind {kind}")
        body = payload[start:pos]
        if (kind != KIND_SECTION and not flags & (F_UNDEFINED | F_LOCAL)):
            if name in keep or (name and name.startswith(KEEP_PREFIXES)):
                stats["kept"].append(name)
            else:
                flags = (flags & ~(F_WEAK | F_EXPORTED | F_NO_STRIP)) | F_LOCAL
                stats["localized"] += 1
        out.append(kind)
        out += enc_uleb(flags)
        out += body
    if pos != len(payload):
        sys.exit("wasm_localize: trailing bytes in the symbol table")
    return bytes(out)


def rewrite_linking(payload, keep, stats):
    pos = 0
    version, pos = uleb(payload, pos)
    if version != 2:
        sys.exit(f"wasm_localize: linking section version {version} (expected 2)")
    out = bytearray(enc_uleb(version))
    while pos < len(payload):
        sub = payload[pos]
        pos += 1
        size, pos = uleb(payload, pos)
        body = payload[pos:pos + size]
        pos += size
        if sub == WASM_COMDAT_INFO:
            stats["comdats_dropped"] = True
            continue
        if sub == WASM_SYMBOL_TABLE:
            body = rewrite_symtab(body, keep, stats)
        out.append(sub)
        out += enc_uleb(len(body))
        out += body
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--keep", action="append", default=[], help="symbol to keep global (repeatable)")
    args = ap.parse_args()
    data = open(args.input, "rb").read()
    if data[:8] != b"\0asm\x01\0\0\0":
        sys.exit("wasm_localize: not a WebAssembly object")
    keep = set(args.keep)
    stats = {"localized": 0, "kept": [], "comdats_dropped": False}
    out = bytearray(data[:8])
    pos = 8
    found = False
    while pos < len(data):
        sid = data[pos]
        size, p = uleb(data, pos + 1)
        payload = data[p:p + size]
        end = p + size
        if sid == 0:
            name, q = name_at(payload, 0)
            if name == "linking":
                found = True
                body = rewrite_linking(payload[q:], keep, stats)
                payload = payload[:q] + body
        out.append(sid)
        out += enc_uleb(len(payload))
        out += payload
        pos = end
    if not found:
        sys.exit("wasm_localize: no linking section (not a relocatable object?)")
    missing = keep - set(stats["kept"])
    if missing:
        sys.exit(f"wasm_localize: --keep symbols not defined in {args.input}: {', '.join(sorted(missing))}")
    open(args.output, "wb").write(out)
    print(f"wasm_localize: {stats['localized']} symbols made local; kept global: {', '.join(sorted(stats['kept']))}")


if __name__ == "__main__":
    main()
