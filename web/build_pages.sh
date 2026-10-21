#!/usr/bin/env bash
# Assemble the static site for GitHub Pages into web/pages/.
#
# GitHub Pages serves plain static files and cannot send the COOP/COEP headers
# that SharedArrayBuffer needs, so the page uses its postMessage transport there
# (see web/README.md).  Relative paths only, so the site also works from a
# repository subpath such as https://user.github.io/mdx2wav/.
#
#   ./web/build.sh          # build the engine first
#   ./web/setup_songs.sh    # collect the sample songs (not in the repository)
#   ./web/build_pages.sh    # assemble web/pages/
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/web/pages"
DIST="$ROOT/web/dist"
SONGS="$ROOT/web/songs"

if [ ! -f "$DIST/mdxweb.js" ]; then
  echo "web/dist/mdxweb.js is missing - run ./web/build.sh first" >&2
  exit 1
fi
if [ ! -d "$SONGS" ]; then
  echo "web/songs/ is missing - run ./web/setup_songs.sh first" >&2
  exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT"

cp -R "$DIST/." "$OUT/"
mkdir -p "$OUT/worklet"
cp "$ROOT/web/worklet/player-processor.js" "$OUT/worklet/"
cp -R "$SONGS" "$OUT/songs"

# The song list is generated at build time on a static host; the dev server
# creates it on the fly instead.
node - "$OUT" <<'NODE'
const fs = require('fs');
const path = require('path');
const out = process.argv[2];

function spcTitle(file) {
  try {
    const b = fs.readFileSync(file);
    if (b.length < 0x4E || b[0x23] === 0) return '';
    let end = 0x2E;
    while (end < 0x4E && b[end] !== 0) end++;
    return new TextDecoder('shift_jis').decode(b.subarray(0x2E, end))
      .replace(/[\r\n\0]/g, '').trim();
  } catch (e) {
    return '';
  }
}
const songsDir = path.join(out, 'songs');
const list = [];
for (const lib of fs.readdirSync(songsDir).sort()) {
  const dir = path.join(songsDir, lib);
  if (!fs.statSync(dir).isDirectory()) continue;
  const files = fs.readdirSync(dir).sort();
  const pdx = files.find((f) => /\.pdx$/i.test(f)) || null;
  for (const f of files) {
    if (/\.mdx$/i.test(f)) {
      list.push({ name: f, path: `${lib}/${f}`, pdx: pdx ? `${lib}/${pdx}` : null, title: '' });
    } else if (/\.spc$/i.test(f)) {
      list.push({
        name: f,
        path: `${lib}/${f}`,
        pdx: null,
        title: spcTitle(path.join(dir, f)),
      });
    }
  }
}
fs.writeFileSync(path.join(songsDir, 'index.json'), JSON.stringify(list, null, 1));
console.log(`  songs/index.json: ${list.length} songs`);
NODE

# GitHub Pages runs Jekyll unless told not to; the underscore-prefixed helper
# files would be skipped otherwise.
touch "$OUT/.nojekyll"

echo "pages site assembled in $OUT:"
find "$OUT" -maxdepth 2 -type f | sed "s|$OUT/||" | sort | head -30
