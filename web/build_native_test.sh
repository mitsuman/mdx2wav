#!/usr/bin/env bash
# Build the native harness test (host build, no SDL / no visualizer).
#
#   ./web/build_native_test.sh && ./web/build/native_harness_test \
#       ../mdx/metalhawk/MHAWK3.MDX ../mdx/metalhawk/MHAWK.PDX 132300 web/verify-out/harness.pcm
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/web/build/native_harness_test"

mkdir -p "$ROOT/web/build"

CXXFLAGS=(-O2 -std=c++11 -Wall -I"$ROOT")

SOURCES=(
  "$ROOT/web/test/native_harness_test.cpp"
  "$ROOT/web/src/mdxweb_load.cpp"
  "$ROOT/gamdx/mxdrvg/so.cpp"
  "$ROOT/gamdx/mxdrvg/opm_delegate.cpp"
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

echo "==> building $OUT"
OBJ="$ROOT/web/build/native_obj"
rm -rf "$OBJ" && mkdir -p "$OBJ"
for src in "${C_SOURCES[@]}"; do
  cc -O2 -c "$src" -o "$OBJ/$(basename "${src%.*}").o"
done
c++ "${CXXFLAGS[@]}" "${SOURCES[@]}" "$OBJ"/*.o -o "$OUT"
echo "==> done"
