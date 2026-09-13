// Minimal dev server for the browser port.
//
// Serves web/dist plus the MDX files, with the Cross-Origin-Opener-Policy /
// Cross-Origin-Embedder-Policy headers that SharedArrayBuffer (and therefore
// Emscripten's pthread support) requires.
//
//   node web/serve.js [--port 8099] [--songs /path/to/mdx/dir]

const http = require('http');
const fs = require('fs');
const path = require('path');

function arg(name, fallback) {
  const i = process.argv.indexOf(name);
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback;
}

const ROOT = path.resolve(__dirname, '..');
const DIST = path.resolve(arg('--dist', path.join(__dirname, 'dist')));
const WORKLET = path.join(__dirname, 'worklet');
// index.html and app.js live in web/ (source), the WASM output in web/dist/.
const WEB_ASSETS = ['index.html', 'app.js'];

const PORT = Number(arg('--port', '8099'));
// Songs live one directory per library, e.g. songs/SION/sion00.mdx.  Point
// --songs at a single directory to serve that instead.
const SONGS_DIR = path.resolve(arg('--songs', process.env.MDX_DIR || path.join(__dirname, 'songs')));

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm',
  '.data': 'application/octet-stream',
  '.json': 'application/json; charset=utf-8',
  '.png': 'image/png',
  '.mdx': 'application/octet-stream',
  '.pdx': 'application/octet-stream',
};

function readLibrary(dir, prefix) {
  let names = [];
  try {
    names = fs.readdirSync(dir);
  } catch (e) {
    return [];
  }
  const pdx = names.find((n) => /\.pdx$/i.test(n)) || null;
  return names
    .filter((n) => /\.mdx$/i.test(n))
    .sort()
    .map((name) => ({ name, path: `${prefix}${name}`, pdx: pdx ? `${prefix}${pdx}` : null, title: '' }));
}

// Each immediate subdirectory of SONGS_DIR is a library; if there are no
// subdirectories, treat SONGS_DIR itself as one.
function listSongs() {
  let entries = [];
  try {
    entries = fs.readdirSync(SONGS_DIR, { withFileTypes: true });
  } catch (e) {
    return [];
  }
  const dirs = entries.filter((e) => e.isDirectory()).map((e) => e.name).sort();
  if (dirs.length === 0) {
    return readLibrary(SONGS_DIR, '');
  }
  return dirs.flatMap((d) => readLibrary(path.join(SONGS_DIR, d), `${d}/`));
}

function send(res, status, body, type) {
  res.writeHead(status, {
    'Content-Type': type || 'text/plain; charset=utf-8',
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp',
    'Cross-Origin-Resource-Policy': 'same-origin',
    'Cache-Control': 'no-store',
  });
  res.end(body);
}

function serveFile(res, file) {
  fs.readFile(file, (err, data) => {
    if (err) {
      send(res, 404, `not found: ${file}`);
      return;
    }
    send(res, 200, data, MIME[path.extname(file).toLowerCase()] || 'application/octet-stream');
  });
}

const server = http.createServer((req, res) => {
  const url = new URL(req.url, `http://${req.headers.host}`);
  let pathname = decodeURIComponent(url.pathname);

  if (pathname === '/songs/index.json') {
    send(res, 200, JSON.stringify(listSongs()), MIME['.json']);
    return;
  }
  if (pathname.startsWith('/songs/')) {
    // Resolve inside SONGS_DIR and reject anything that escapes it, including
    // through '..' segments or absolute paths.
    const rel = path.normalize(pathname.slice('/songs/'.length));
    const file = path.join(SONGS_DIR, rel);
    if (!file.startsWith(SONGS_DIR + path.sep) || rel.startsWith('..')) {
      send(res, 403, 'forbidden');
      return;
    }
    serveFile(res, file);
    return;
  }
  if (pathname.startsWith('/worklet/')) {
    serveFile(res, path.join(WORKLET, path.basename(pathname)));
    return;
  }
  if (pathname === '/') {
    pathname = '/index.html';
  }
  const base = DIST;   // dist/ is the complete site (see build.sh)
  const file = path.join(base, pathname);
  if (!file.startsWith(base)) {
    send(res, 403, 'forbidden');
    return;
  }
  serveFile(res, file);
});

server.listen(PORT, '127.0.0.1', () => {
  console.log(`mdx web player: http://127.0.0.1:${PORT}/`);
  console.log(`songs: ${SONGS_DIR} (${listSongs().length} files)`);
});
