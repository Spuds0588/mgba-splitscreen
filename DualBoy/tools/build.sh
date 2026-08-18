#!/usr/bin/env bash
# Build the threaded lockstep reference harness against the libmgba that
# DualBoy's build.rs already compiled (static, LIBMGBA_ONLY).
set -euo pipefail

cd "$(dirname "$0")/../.."   # repo root

OUT_DIR=$(ls -d DualBoy/src-tauri/target/release/build/dualboy-*/out | head -1)
# cmake outputs libmgba.a directly under build/; the lib/ copy is the install
# destination and can be stale after a `cmake --build`. Prefer the fresh one.
LIB="$OUT_DIR/build/libmgba.a"
if [[ ! -f "$LIB" ]]; then
  LIB="$OUT_DIR/lib/libmgba.a"
fi
if [[ ! -f "$LIB" ]]; then
  echo "libmgba.a not found at $LIB -- run 'cargo build --release' in DualBoy/src-tauri first" >&2
  exit 1
fi

# Replicate the exact -D defines the cmake build used so struct layouts match
# the compiled libmgba (USE_PTHREADS vs no-op Mutex, ENABLE_VFS gates, etc.).
DEFINES=$(grep -rh "C_DEFINES = " "$OUT_DIR/build/CMakeFiles"/*/flags.make | head -1 | sed 's/^C_DEFINES = //')
# shellcheck disable=SC2086
gcc -O2 -std=c11 -pthread $DEFINES \
  -I include -I src -I "$OUT_DIR/include" -I DualBoy/tools \
  DualBoy/tools/threaded_link.c \
  DualBoy/tools/rendezvous.c \
  "$LIB" \
  -lpthread -lm \
  -o DualBoy/tools/threaded_link

echo "built DualBoy/tools/threaded_link"
