#!/usr/bin/env bash
# Build the in-browser (WASM) engine: libmgba + the mgba-splitscreen bridge.
#
# Produces (in mgba-splitscreen/web/):
#   mgba_splitscreen_web.c   - the bridge source (committed)
#   mgba-splitscreen-web.js  - emscripten glue + exports (tracked; GitHub Pages
#                     stages these alongside mgba-splitscreen/src at deploy time)
#   mgba-splitscreen-web.wasm- the compiled core (tracked; deployed as-is)
#
# The desktop (Tauri) app NEVER ships these: it embeds only mgba-splitscreen/src and
# runs the native Rust backend. The copy into src/ below is a gitignored
# convenience for local previews of the web UI.
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
  -sEXPORTED_FUNCTIONS=_mgs_init,_mgs_load_rom,_mgs_run_frame,_mgs_get_video_width,_mgs_get_video_height,_mgs_get_platform,_mgs_get_video,_mgs_set_keys,_mgs_get_audio,_mgs_audio_frames,_mgs_get_audio_rate,_mgs_set_audio_source,_mgs_save_state,_mgs_state_ptr,_mgs_load_state,_mgs_load_state_bytes,_mgs_reset_sio,_mgs_quit,_mgs_enable_debug,_mgs_get_stats,_mgs_set_fs_assist,_mgs_gb_test_transfer,_mgs_gb_read_sb,_mgs_gb_read_sc,_malloc,_free,_fflush \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAP8,HEAP16,HEAPU8,HEAP32,HEAPU32,HEAP64 \
  -sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY=268435456 -sSTACK_SIZE=1048576 \
  -sMODULARIZE=1 -sEXPORT_NAME=MgbaSplitScreenWasm \
  -sNO_EXIT_RUNTIME=1 -sERROR_ON_UNDEFINED_SYMBOLS=1 -sASSERTIONS=0 \
  -sSTRICT=0 \
  mgba-splitscreen/web/mgba_splitscreen_web.c "$BUILD_DIR/libmgba.a" -o mgba-splitscreen/web/mgba-splitscreen-web.js

# 3. Local-preview copies into src/ (gitignored, so the desktop bundle stays
#    wasm-free; GitHub Pages stages from web/ instead).
cp mgba-splitscreen/web/mgba-splitscreen-web.js mgba-splitscreen/web/mgba-splitscreen-web.wasm mgba-splitscreen/src/
echo "Built mgba-splitscreen/web/mgba-splitscreen-web.{js,wasm} and copied into mgba-splitscreen/src/ (gitignored, local preview only)"
