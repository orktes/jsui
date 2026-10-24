#!/bin/sh
# Bundles react-commit.jsx (React 19 production) into one ES2017 script any engine can run.
set -e
cd "$(dirname "$0")/../.."
mkdir -p bench/engines/out
npx esbuild bench/engines/react-commit.jsx --bundle --format=iife --target=es2017 \
  --define:process.env.NODE_ENV='"production"' --jsx=automatic \
  --alias:react=./node_modules/react --alias:react-reconciler=./node_modules/react-reconciler \
  --outfile=bench/engines/out/react-commit.js --log-level=warning
