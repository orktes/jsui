#!/bin/sh
# Bundles react-commit.jsx (React 19 production) into one ES2017 script any engine can run.
set -e
cd "$(dirname "$0")/../.."
mkdir -p bench/engines/out
# React's scheduler picks setImmediate / MessageChannel / setTimeout at load time and
# QuickJS has none of them: hand every engine the same never-firing setTimeout stub so
# all run the same code (the bench flushes synchronously, nothing needs to fire).
BANNER="// Same scheduler path on every engine: React's scheduler sees only a setTimeout stub (the bench flushes synchronously, so nothing needs to fire).
(function (setTimeout, clearTimeout, setImmediate, MessageChannel) {"
FOOTER="})(function () { return 0; }, function () {}, undefined, undefined);"
npx esbuild bench/engines/react-commit.jsx --bundle --format=iife --target=es2017 \
  --define:process.env.NODE_ENV='"production"' --jsx=automatic \
  --alias:react=./node_modules/react --alias:react-reconciler=./node_modules/react-reconciler \
  --outfile=bench/engines/out/react-commit.js --log-level=warning \
  --banner:js="$BANNER" --footer:js="$FOOTER"
