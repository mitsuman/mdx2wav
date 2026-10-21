// End-to-end verification of SPC playback in the browser port.
//
//   node web/verify_spc.js [--song /path/to/music.spc] [--seconds 5]
//
// Without --song this generates a tiny looping test tone with
// web/tools/make_test_spc.py, so the check also works in a clean checkout.
// The script starts web/serve.js, plays the file through the same WASM engine
// the page uses, checks that audio and the visualizer are alive, writes a
// screenshot, and compares the rendered PCM with the native build.

const { spawn, spawnSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const { Browser, sleep } = require('./tools/cdp');

const ROOT = path.resolve(__dirname, '..');
const PORT = Number(process.env.PORT || 8099);

function arg(name, fallback) {
  const i = process.argv.indexOf(name);
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback;
}

const SECONDS = Number(arg('--seconds', '5'));
const OUT_DIR = path.resolve(arg('--out', path.join(ROOT, 'web', 'verify-out')));
const URL = `http://127.0.0.1:${PORT}/`;

const log = (msg) => console.log(msg);
const fail = (msg) => { console.error(`FAIL: ${msg}`); process.exitCode = 1; };

fs.mkdirSync(OUT_DIR, { recursive: true });
let SONG = arg('--song', '');
if (!SONG) {
  SONG = path.join(OUT_DIR, 'test.spc');
  log(`==> generating test SPC: ${SONG}`);
  const made = spawnSync('python3', [path.join(__dirname, 'tools', 'make_test_spc.py'), SONG],
    { stdio: 'inherit' });
  if (made.status !== 0) process.exit(1);
}
if (!fs.existsSync(SONG) || !/\.spc$/i.test(SONG)) {
  console.error('usage: node web/verify_spc.js [--song /path/to/music.spc] [--seconds 5]');
  process.exit(1);
}

const SONG_NAME = path.basename(SONG);
const SONGS_DIR = path.dirname(SONG);

async function startServer() {
  const proc = spawn(process.execPath,
    [path.join(__dirname, 'serve.js'), '--port', String(PORT), '--songs', SONGS_DIR],
    { stdio: ['ignore', 'pipe', 'pipe'] });
  proc.stdout.on('data', (d) => process.stdout.write(`  [server] ${d}`));
  proc.stderr.on('data', (d) => process.stderr.write(`  [server] ${d}`));
  const deadline = Date.now() + 15000;
  while (Date.now() < deadline) {
    try {
      const res = await fetch(URL);
      if (res.ok) return proc;
    } catch (e) { /* retry */ }
    await sleep(150);
  }
  throw new Error('server did not start');
}

async function main() {
  log(`==> starting server on ${URL} (songs: ${SONGS_DIR})`);
  const server = await startServer();
  const browser = new Browser({ port: 9333 });
  try {
    await browser.start();
    browser.on((msg) => {
      if (process.env.DUMP_LOGS && msg.method === 'Runtime.consoleAPICalled') {
        const text = (msg.params.args || []).map((a) => a.value ?? a.description ?? '').join(' ');
        log(`    [page:${msg.params.type}] ${text}`);
      }
    });

    log('==> opening page');
    await browser.newPage(URL);
    await browser.waitFor('!!(window.__mdxweb && window.__mdxweb.api && window.__mdxweb.api.step)',
      60000, 'engine API');

    log(`==> loading ${SONG_NAME}`);
    const info = await browser.evaluate(`(async () => {
      const m = window.__mdxweb;
      const res = await fetch('songs/${encodeURIComponent(SONG_NAME)}');
      if (!res.ok) throw new Error('fetch failed: ' + res.status);
      const buf = await res.arrayBuffer();
      await m.play({ spc: buf, name: ${JSON.stringify(SONG_NAME)}, title: '' });
      return {
        title: m.api.title(),
        duration: m.api.duration(),
        running: m.api.running(),
        isSpc: m.api.isSpc ? m.api.isSpc() : -1,
        canvas: { w: document.getElementById('screen').width,
                  h: document.getElementById('screen').height },
      };
    })()`, { awaitPromise: true });
    log(`    title: ${JSON.stringify(info.title)}`);
    log(`    duration: ${info.duration.toFixed(2)}s, canvas ${info.canvas.w}x${info.canvas.h}`);

    log(`==> playing for ${SECONDS}s`);
    await sleep(SECONDS * 1000);

    const diag = await browser.evaluate(`(() => {
      const m = window.__mdxweb;
      const ring = m.state.ring;
      const cap = ring[2];
      let w = Atomics.load(ring, 0) % cap;
      let r = Atomics.load(ring, 1) % cap;
      let avail = w - r; if (avail < 0) avail += cap;
      let peak = 0, sum = 0, n = 0;
      for (let i = 0; i < avail; i++) {
        const l = m.state.ringSamples[((r + i) % cap) * 2];
        const rr = m.state.ringSamples[((r + i) % cap) * 2 + 1];
        peak = Math.max(peak, Math.abs(l), Math.abs(rr));
        sum += l * l + rr * rr; n += 2;
      }
      return {
        elapsed: m.api.elapsed(),
        running: m.api.running(),
        ctxState: m.state.ctx ? m.state.ctx.state : 'none',
        bufferedMs: avail / 44.1,
        underruns: Atomics.load(ring, 3),
        peak,
        rms: n ? Math.sqrt(sum / n) : 0,
        status: document.getElementById('status').textContent.trim(),
      };
    })()`);

    log('==> player diagnostics');
    log(`    elapsed:     ${diag.elapsed.toFixed(2)}s`);
    log(`    audio ctx:   ${diag.ctxState}, buffered ${diag.bufferedMs.toFixed(0)}ms, underruns ${diag.underruns}`);
    log(`    ring peak:   ${diag.peak}, rms ${diag.rms.toFixed(0)}`);
    log(`    status:      ${diag.status}`);

    if (!(diag.elapsed > SECONDS * 0.5)) fail(`engine advanced only ${diag.elapsed.toFixed(2)}s of audio`);
    if (diag.peak < 500) fail(`audio looks silent (peak ${diag.peak})`);
    if (diag.underruns > 0) fail(`audio underran ${diag.underruns} time(s)`);

    const shot = path.join(OUT_DIR, 'spc-screenshot.png');
    await browser.screenshot(shot);
    log(`==> screenshot: ${shot}`);

    log('==> comparing DSP output with the native build');
    const frames = Math.round(44100 * SECONDS);
    await browser.newPage(URL);
    await browser.waitFor('!!(window.__mdxweb && window.__mdxweb.api)', 60000, 'engine API (page 2)');
    const dumpInfo = await browser.evaluate(`(async () => {
      const m = window.__mdxweb;
      await m.ready;
      m.api.init(8, 8, 1.0, m.api.windowWidth(), 880);
      const res = await fetch('songs/${encodeURIComponent(SONG_NAME)}');
      const bytes = new Uint8Array(await res.arrayBuffer());
      const p = window.Module._malloc(bytes.length);
      window.Module.HEAPU8.set(bytes, p);
      const ok = m.api.loadSpc(p, bytes.length);
      window.Module._free(p);
      const frames = ${frames};
      const ptr = window.Module._malloc(frames * 4);
      const got = m.api.dumpPcm(ptr, frames);
      const out = window.Module.HEAPU8.subarray(ptr, ptr + got * 4);
      let s = '';
      const chunk = 32768;
      for (let i = 0; i < out.length; i += chunk) {
        s += String.fromCharCode.apply(null, out.subarray(i, i + chunk));
      }
      window.Module._free(ptr);
      return { ok, got, b64: btoa(s) };
    })()`, { awaitPromise: true });
    log(`    dump: load=${dumpInfo.ok} frames=${dumpInfo.got}`);
    if (!dumpInfo.ok) fail('SPC load failed on the comparison page');

    const webPcm = path.join(OUT_DIR, 'spc-web.pcm');
    fs.writeFileSync(webPcm, Buffer.from(dumpInfo.b64, 'base64'));
    log(`    wrote ${webPcm} (${fs.statSync(webPcm).size} bytes)`);

    const nativePcm = path.join(OUT_DIR, 'spc-native.pcm');
    const native = spawn(path.join(ROOT, 'build', 'mdx2wav'),
      ['-d', String(SECONDS), SONG], { stdio: ['ignore', 'pipe', 'pipe'] });
    const chunks = [];
    native.stdout.on('data', (d) => chunks.push(d));
    await new Promise((resolve) => native.on('close', resolve));
    fs.writeFileSync(nativePcm, Buffer.concat(chunks));
    log(`    wrote ${nativePcm} (${fs.statSync(nativePcm).size} bytes)`);

    const cmp = spawn('python3', [path.join(__dirname, 'tools', 'compare_pcm.py'), webPcm, nativePcm],
      { stdio: 'inherit' });
    const cmpCode = await new Promise((resolve) => cmp.on('close', resolve));
    if (cmpCode !== 0) fail('PCM mismatch against the native build');

    log('');
    log(process.exitCode ? '==> VERIFY FAILED' : '==> VERIFY OK');
  } finally {
    await browser.close();
    server.kill('SIGKILL');
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
