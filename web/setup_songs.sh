#!/usr/bin/env bash
# Collect the sample songs used by the browser player.
#
# The copyrighted MDX/PDX files are NOT part of this repository.  This script
# gathers the SION and SION2 sets from a local MDX collection into web/songs/ so
# the dev server (and web/build_pages.sh) can serve them.
#
#   ./web/setup_songs.sh [/path/to/mdx]
#
# Default source directory: ../../mdx relative to the repository root.
# The directory is also copied into web/dist/songs/ when dist/ already exists, so
# a GitHub Pages build can pick it up.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${1:-$ROOT/../../mdx}"
DEST="$ROOT/web/songs"

if [ ! -d "$SRC/SION" ] || [ ! -d "$SRC/SION2" ]; then
  echo "SION/ and SION2/ not found under: $SRC" >&2
  echo "usage: $0 [/path/to/mdx]" >&2
  exit 1
fi

rm -rf "$DEST"
mkdir -p "$DEST/SION" "$DEST/SION2"

# SION (1991, J.Yamada / Z.Nishikawa): six tracks sharing SION.pdx
cp "$SRC/SION"/*.mdx "$DEST/SION/" 2>/dev/null || true
cp "$SRC/SION/SION.pdx" "$DEST/SION/" 2>/dev/null || cp "$SRC/SION"/[Ss][Ii][Oo][Nn].[Pp][Dd][Xx] "$DEST/SION/"

# SION2 (1992): eight tracks sharing SION2.PDX
cp "$SRC/SION2"/*.MDX "$SRC/SION2"/*.mdx "$DEST/SION2/" 2>/dev/null || true
cp "$SRC/SION2/SION2.PDX" "$DEST/SION2/" 2>/dev/null || cp "$SRC/SION2"/[Ss][Ii][Oo][Nn]2.[Pp][Dd][Xx] "$DEST/SION2/"

echo "songs copied to $DEST:"
ls -1 "$DEST/SION" "$DEST/SION2"

if [ -d "$ROOT/web/dist" ]; then
  rm -rf "$ROOT/web/dist/songs"
  cp -R "$DEST" "$ROOT/web/dist/songs"
  echo "also copied into web/dist/songs"
fi
