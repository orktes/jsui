#!/usr/bin/env python3
"""A LINKER_LAUNCHER that records a link's archives instead of linking.

    capture_link.py OUT.txt FIRST_NAME <linker> <args...>

Writes the absolute paths of the archives on the link line to OUT.txt, one
per line, in link order, with the one whose file name is FIRST_NAME first,
and creates the link's output (-o) empty so the build tool sees it done.
integrations/godot/CMakeLists.txt (jsui_godot_web) feeds the list to
link_relocatable.py.
"""
import os
import sys


def main():
    out, first = sys.argv[1], sys.argv[2]
    args = sys.argv[4:]
    archives, output = [], None
    i = 0
    while i < len(args):
        a = args[i]
        if a == "-o" and i + 1 < len(args):
            output = args[i + 1]
            i += 2
            continue
        if a.endswith(".a"):
            p = os.path.abspath(a)
            if p not in archives:
                archives.append(p)
        i += 1
    archives.sort(key=lambda p: os.path.basename(p) != first)  # stable: FIRST_NAME first
    if not archives or os.path.basename(archives[0]) != first:
        sys.exit(f"capture_link: {first} is not on the link line")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w") as f:
        f.write("\n".join(archives) + "\n")
    if output:
        open(output, "w").close()


if __name__ == "__main__":
    main()
