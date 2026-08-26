#!/usr/bin/env bash
# Build the in-browser (WASM) engine: libmgba + the DualBoy bridge.
#
# Produces (in DualBoy/web/):
#   dualboy_web.c   - the bridge source (committed)
#   dualboy-web.js  - emscripten glue + exports (committed, deployed as-is)
#   dualboy-web.wasm- the compiled core (committed, deployed as-is)
#
# Prereqs: Emscripten SDK on PATH (emsdk_env.sh sourced).
#
# IMPORTANT: the bridge MUST be compiled with the SAME layout defines as
# libmgba. In particular ENABLE_DIRECTORIES adds `struct mDirectorySet dirs`
# to `struct mCore`, moving every vtable slot by ~4100 bytes; a mismatch
# produces "null function or function signature mismatch" traps at the first
# `core->init` call. Keep the two define sets in sync with
# build-wasm/CMakeFiles/mgba.dir/flags.make.
set -euo pipefail
cd "$(dirname "$0")/../.."

BUILD_DIR=build-wasm
DEFINES="-DBUILD_STATIC -DENABLE_DIRECTORIES -DENABLE_VFS -DENABLE_VFS_FD \
 -DHAVE_LOCALTIME_R -DHAVE_STRDUP -DHAVE_STRLCPY -DHAVE_STRTOF_L -DHAVE_XLOCALE \
 -DM_CORE_GB -DM_CORE_GBA -DUSE_PTHREADS"

# 1. Configure + build the static core library for wasm (single-threaded; the
#    fork's lockstep is cooperative so no pthreads are used at runtime).
emcmake cmake -S . -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_STATIC=ON -DBUILD_SHARED=OFF \
  -DBUILD_GL=OFF -DBUILD_GLES2=OFF -DBUILD_GLES3=OFF \
  -DUSE_PTHREADS=OFF -DENABLE_SCRIPTING=OFF -DENABLE_GDB_STUB=OFF \
  -DENABLE_DEBUGGERS=OFF -DUSE_FFMPEG=OFF -DUSE_ZLIB=OFF -DUSE_PNG=OFF \
  -DUSE_LZMA=OFF -DUSE_SQLITE3=OFF -DUSE_ELF=OFF -DUSE_EPOXY=OFF \
  -DUSE_LIBZIP=OFF -DUSE_MINIZIP=OFF \
  -DCMAKE_C_FLAGS="-D_GNU_SOURCE" >/dev/null
emmake make -C "$BUILD_DIR" -j"$(nproc)" >/dev/null

# 2. Link the bridge + libmgba into the module. Layout defines MUST match step 1.
# Fixed heap (no memory-growth bounds checks in every access) sized for 4 cores
# plus the largest (32MB) ROM copies: 4x32MB + states + core state.
emcc -O3 -D_GNU_SOURCE -DNDEBUG $DEFINES \
  -Iinclude -I"$BUILD_DIR/include" -I"$BUILD_DIR" \
  -sENVIRONMENT=web \
  -sEXPORTED_FUNCTIONS=_db_init,_db_load_rom,_db_run_frame,_db_get_video,_db_set_keys,_db_get_audio,_db_audio_frames,_db_set_audio_source,_db_save_state,_db_state_ptr,_db_load_state,_db_load_state_bytes,_db_quit,_db_enable_debug,_db_get_stats,_malloc,_free,_fflush \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAP8,HEAP16,HEAPU8,HEAP32,HEAPU32,HEAP64 \
  -sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY=268435456 -sSTACK_SIZE=1048576 \
  -sMODULARIZE=1 -sEXPORT_NAME=DualBoyWasm \
  -sNO_EXIT_RUNTIME=1 -sERROR_ON_UNDEFINED_SYMBOLS=1 -sASSERTIONS=0 \
  -sSTRICT=0 \
  DualBoy/web/dualboy_web.c "$BUILD_DIR/libmgba.a" -o DualBoy/web/dualboy-web.js

# 3. Deploy copies into the frontend bundle (desktop embeds DualBoy/src; the
#    web + GitHub Pages serve it directly).
cp DualBoy/web/dualboy-web.js DualBoy/web/dualboy-web.wasm DualBoy/src/
echo "Built DualBoy/web/dualboy-web.{js,wasm} and copied into DualBoy/src/"
