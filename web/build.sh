#!/usr/bin/env bash
# Build the browser (WebAssembly) port of mdx2wav.
#
# Requires the toolchain that lives in tools/ (see tools/README.md):
#   tools/upstream        emscripten + LLVM/binaryen
#   tools/node/bin/node   node used by emcc
#   tools/pylocal/python  python 3.10+ used by emcc
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS="$ROOT/tools"
OUT="$ROOT/web/dist"
BUILD="$ROOT/web/build"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

export EM_CONFIG="${EM_CONFIG:-$TOOLS/emscripten.config}"
export EM_CACHE="${EM_CACHE:-$TOOLS/cache}"
export EMSDK_PYTHON="${EMSDK_PYTHON:-$TOOLS/pylocal/python/bin/python3}"
export PATH="$TOOLS/bin:$TOOLS/node/bin:$PATH"
export LC_ALL=C

if [ ! -f "$TOOLS/upstream/emscripten/emcc.py" ]; then
  echo "emscripten not found under $TOOLS" >&2
  echo "see tools/README.md for how to install the toolchain" >&2
  exit 1
fi

CPP_SOURCES=(
  "$ROOT/web/src/mdxweb.cpp"
  "$ROOT/web/src/video_encoder_stub.cpp"
  "$ROOT/web/src/mdxweb_load.cpp"
  "$ROOT/src/visualizer/visualizer.cpp"
  "$ROOT/src/visualizer/visualizer_common.cpp"
  "$ROOT/src/visualizer/visualizer_adpcm.cpp"
  "$ROOT/src/visualizer/ym2151_state.cpp"
  "$ROOT/src/visualizer/spectrum_analyzer.cpp"
  "$ROOT/gamdx/mxdrvg/so.cpp"
  "$ROOT/gamdx/mxdrvg/opm_delegate.cpp"
  "$ROOT/gamdx/mxdrvg/opm_visualizer.cpp"
  "$ROOT/gamdx/mxdrvg/pcm8_visualizer.cpp"
  "$ROOT/gamdx/pcm8/pcm8.cpp"
  "$ROOT/gamdx/pcm8/x68pcm8.cpp"
  "$ROOT/gamdx/downsample/downsample.cpp"
  "$ROOT/gamdx/fmgen/fmgen.cpp"
  "$ROOT/gamdx/fmgen/fmtimer.cpp"
  "$ROOT/gamdx/fmgen/opm.cpp"
)
C_SOURCES=(
  "$ROOT/gamdx/mame/ym2151.c"
)

mkdir -p "$OUT" "$BUILD/obj"

PORTS_FLAGS=(
  -pthread
  -sUSE_SDL=2
  -sUSE_SDL_IMAGE=2
  -sSDL2_IMAGE_FORMATS='["png"]'
  -sUSE_SDL_TTF=2
  -sUSE_ZLIB=1
)

COMMON_FLAGS=(
  -O0   # -O1/-O2 miscompile the MXDRVG copy loop (see web/README.md)
  -g
  -Wno-deprecated-declarations
  -Wno-writable-strings
  -DENABLE_VISUALIZER
  -DMDXWEB_SAFE_RENDER
  -DMDXWEB_NO_ADPCM_WAVE
  -I"$ROOT"
)
CXX_FLAGS=(-std=c++11)

LINK_FLAGS=(
  -O0
  -pthread
  -sPTHREAD_POOL_SIZE=8
  -sALLOW_MEMORY_GROWTH=1
  -sINITIAL_MEMORY=536870912
  -sSTACK_SIZE=33554432
  -sDEFAULT_PTHREAD_STACK_SIZE=33554432
  -sEXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAP16","HEAPU8","HEAPF64","FS","UTF8ToString","stringToUTF8","lengthBytesUTF8"]'
  -sEXPORTED_FUNCTIONS='["_malloc","_free","_mdxweb_main"]'
  -sENVIRONMENT=web
  -sASSERTIONS=1
  -gsource-map
  -sNO_EXIT_RUNTIME=1
  --preload-file "$ROOT/data@/data"
  --pre-js "$ROOT/web/src/pre.js"
  -o "$OUT/mdxweb.js"
)

ALL_SOURCES=("${CPP_SOURCES[@]}" "${C_SOURCES[@]}")

echo "==> compiling (${#ALL_SOURCES[@]} files, -j$JOBS)"
index=0
for src in "${ALL_SOURCES[@]}"; do
  base="$(basename "${src%.*}")"
  if [ "$base" = "ym2151" ]; then base="ym2151_mame"; fi
  obj="$BUILD/obj/$base.o"
  case "$src" in
    *.c) "$TOOLS/bin/emcc" "${COMMON_FLAGS[@]}" "${PORTS_FLAGS[@]}" -c "$src" -o "$obj" & ;;
    *)   "$TOOLS/bin/em++" "${COMMON_FLAGS[@]}" "${CXX_FLAGS[@]}" "${PORTS_FLAGS[@]}" -c "$src" -o "$obj" & ;;
  esac
  index=$((index + 1))
  if [ $((index % JOBS)) -eq 0 ] || [ "$index" -eq "${#ALL_SOURCES[@]}" ]; then
    wait || exit 1
  fi
done

OBJS="$(find "$BUILD/obj" -name '*.o' | sort)"

echo "==> linking"
# shellcheck disable=SC2086
"$TOOLS/bin/em++" $OBJS "${PORTS_FLAGS[@]}" "${LINK_FLAGS[@]}"

echo "==> done"
ls -la "$OUT"
