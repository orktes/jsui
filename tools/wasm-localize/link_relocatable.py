#!/usr/bin/env python3
"""Link archives into one relocatable wasm object with local symbols.

    link_relocatable.py --wasm-ld PATH --inputs LIST --entry SYMBOL --output OUT.o

LIST names the inputs (archives, one per line). The first one (the archive
that defines SYMBOL) is linked whole, the others as in a normal link: only
the members something needs (`wasm-ld -r`; -r does not take --undefined).
Then wasm_localize.py makes every defined symbol but SYMBOL local. See
wasm_localize.py for why.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wasm-ld", required=True)
    ap.add_argument("--inputs", required=True)
    ap.add_argument("--entry", required=True)
    ap.add_argument("--keep", action="append", default=[], help="another symbol to keep global")
    ap.add_argument("--output", required=True)
    args = ap.parse_args()
    inputs = [l.strip() for l in open(args.inputs) if l.strip()]
    for i in inputs:
        if not os.path.exists(i):
            sys.exit(f"link_relocatable: missing input {i}")
    tmp = args.output + ".r.o"
    cmd = [args.wasm_ld, "-r", "-o", tmp, "--whole-archive", inputs[0], "--no-whole-archive"] + inputs[1:]
    subprocess.run(cmd, check=True)
    subprocess.run([sys.executable, os.path.join(HERE, "wasm_localize.py"), tmp, args.output, "--keep", args.entry] +
                   [a for k in args.keep for a in ("--keep", k)],
                   check=True)
    os.remove(tmp)


if __name__ == "__main__":
    main()
