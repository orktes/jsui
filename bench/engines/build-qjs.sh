#!/bin/bash
# build-qjs.sh <outname> <engine-dir> <quickjs.c path> <ng-dir (qjs.c/libc)> [extra defines...]
set -e
NAME=$1 ENG=$2 QC=$3 NG=$4; shift 4
B=~/Development/engine-bench/qjs-build/$NAME; mkdir -p $B; rm -f $B/*.o
F="-O3 -DNDEBUG -std=gnu11 -arch arm64 -fPIC -w -D_GNU_SOURCE $* -I$ENG -I$NG"
cc $F -c $QC -o $B/quickjs.o &
for f in libregexp libunicode dtoa; do cc $F -c $ENG/$f.c -o $B/$f.o & done
cc $F -c $NG/qjs.c -o $B/qjs.o &
cc $F -c $NG/quickjs-libc.c -o $B/quickjs-libc.o &
for g in repl standalone; do cc $F -c $NG/gen/$g.c -o $B/gen-$g.o & done
wait
cc -arch arm64 -o $B/qjs $B/*.o -lm
cp $B/qjs ~/Development/engine-bench/bin/$NAME
echo "$F" > $B/FLAGS
