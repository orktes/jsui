#!/bin/sh
# Push the staged Mesa (stage/home/phablet/gpu-stack) + env.sh + launchers to tablet ~/gpu-stack.
set -e
cd "$(dirname "$0")"
S=stage/home/phablet
cp env.sh $S/gpu-stack/env.sh
for L in launchers bin; do [ -d $L ] && cp $L/* $S/gpu-stack/bin/; done
COPYFILE_DISABLE=1 tar czf gpu-stack.tgz --no-xattrs -C $S --exclude=include --exclude=lib/pkgconfig gpu-stack
adb push gpu-stack.tgz /home/phablet/gpu-stack.tgz >/dev/null
adb shell 'cd ~ && tar xzf gpu-stack.tgz 2>/dev/null; rm gpu-stack.tgz'
rm gpu-stack.tgz
echo deployed
