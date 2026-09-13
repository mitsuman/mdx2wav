// End-to-end verification of the browser port.
//
//   node web/verify.js [--song web/songs/SION/sion00.mdx] [--seconds 6] [--out web/dist/..]
//
// Checks that
//   1. the WASM engine boots in a real browser,
//   2. the page's default song renders audio (non silent, no underruns),
//   3. the canvas shows the visualizer (screenshot is written to disk),
//   4. the DSP output matches the native mdx2wav binary bit-for-bit-ish.
//
// Set CHROME_BIN to point at a Chrome/Chromium build.

const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');
const { Browser, sleep } = require('./tools/cdp');

const ROOT = path.resolve(__dirname, '..');
const PORT = Number(process.env.PORT || 8099);
const URL = `http://127.0.0.1:${PORT}/`;

function arg(name, fallback) {
  const i = process.argv.indexOf(name);
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback;
}

// Native-binary reference path, relative to the repository root.
const SONG = path.resolve(arg('--song', path.join(ROOT, 'web', 'songs', 'SION', 'sion00.mdx')));
const SECONDS = Number(arg('--seconds', '6'));
const OUT_DIR = path.resolve(arg('--out', path.join(ROOT, 'web', 'verify-out')));

const log = (msg) => console.log(msg);
const fail = (msg) => { console.error(`FAIL: ${msg}`); process.exitCode = 1; };

async function startServer() {
  const proc = spawn(process.execPath, [path.join(__dirname, 'serve.js'), '--port', String(PORT)], {
    stdio: ['ignore', 'pipe', 'pipe'],
  });
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
  fs.mkdirSync(OUT_DIR, { recursive: true });
  log(`==> starting server on ${URL}`);
  const server = await startServer();
  const browser = new Browser({ port: 9333 });
  const messages = [];
  try {
    await browser.start();
    browser.on((msg) => {
      if (msg.method === 'Runtime.consoleAPICalled') {
        const text = (msg.params.args || []).map((a) => a.value ?? a.description ?? '').join(' ');
        messages.push(`[${msg.params.type}] ${text}`);
        if (process.env.DUMP_LOGS) log(`    [page:${msg.params.type}] ${text}`);
      } else if (msg.method === 'Log.entryAdded') {
        messages.push(`[log:${msg.params.entry.level}] ${msg.params.entry.text}`);
      }
    });
    log('==> opening page');
    await browser.newPage(URL);

    log('==> waiting for the engine to initialize');
    await browser.waitFor('!!(window.__mdxweb && window.__mdxweb.api && window.__mdxweb.api.step)', 60000,
      'engine API');

    if (process.env.DUMP_LOGS) {
      for (const m of messages.slice(-25)) log(`    ${m}`);
    }

    // Load and start the song through the same path the UI uses.
    log(`==> loading the page default song and starting playback`);
    const info = await browser.evaluate(`(async () => {
      const m = window.__mdxweb;
      const raw = await m.defaultSong();
      await m.play(raw);
      return { title: m.api.title(), duration: m.api.duration(), running: m.api.running() };
    })()`, { awaitPromise: true });
    log(`    title: ${JSON.stringify(info.title)}`);
    log(`    duration: ${info.duration.toFixed(2)}s`);

    log(`==> playing for ${SECONDS}s`);
    await sleep(SECONDS * 1000);

    const diag = await browser.evaluate(`(() => {
      const m = window.__mdxweb;
      const ring = m.state.ring;
      const cap = ring[2];
      let w = Atomics.load(ring, 0) % cap;
      let r = Atomics.load(ring, 1) % cap;
      let avail = w - r; if (avail < 0) avail += cap;
      // Peak/RMS over what is currently buffered.
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
        canvas: { w: document.getElementById("screen").width,
                  h: document.getElementById('screen').height },
        status: document.getElementById('status').textContent.trim(),
      };
    })()`);

    if (process.env.DUMP_LOGS) {
      for (const m of messages.filter((x) => /\[web\]|\[load\]/.test(x)).slice(0, 5)) log(`    ${m}`);
    }
    log('==> player diagnostics');
    log(`    elapsed:     ${diag.elapsed.toFixed(2)}s`);
    log(`    audio ctx:   ${diag.ctxState}, buffered ${diag.bufferedMs.toFixed(0)}ms, underruns ${diag.underruns}`);
    log(`    ring peak:   ${diag.peak}, rms ${diag.rms.toFixed(0)}`);
    log(`    status:      ${diag.status}`);

    if (!(diag.elapsed > SECONDS * 0.5)) fail(`engine advanced only ${diag.elapsed.toFixed(2)}s of audio`);
    if (diag.peak < 500) fail(`audio looks silent (peak ${diag.peak})`);
    if (diag.underruns > 0) fail(`audio underran ${diag.underruns} time(s)`);
    if (!/再生中/.test(diag.status)) fail(`unexpected status: ${diag.status}`);

    const shot = path.join(OUT_DIR, 'screenshot.png');
    await browser.screenshot(shot);
    log(`==> screenshot: ${shot}`);

    // Numerical check against the native binary.  This runs on a *fresh* page so
    // the engine starts from exactly the same state as the native binary.
    log('==> comparing DSP output with the native build');
    const frames = 44100 * 3;
    await browser.newPage(URL);
    await browser.waitFor('!!(window.__mdxweb && window.__mdxweb.api)', 60000, 'engine API (page 2)');
    const dumpInfo = await browser.evaluate(`(async () => {
      const m = window.__mdxweb;
      const raw = await m.defaultSong();
      await m.ready;
      m.api.init(8, 8, 1.0, 1400, 880);
      const mdx = new Uint8Array(raw.mdx), pdx = new Uint8Array(raw.pdx);
      const mp = window.Module._malloc(mdx.length); window.Module.HEAPU8.set(mdx, mp);
      const pp = window.Module._malloc(pdx.length); window.Module.HEAPU8.set(pdx, pp);
      const ok = m.api.load(mp, mdx.length, pp, pdx.length);
      window.Module._free(mp); window.Module._free(pp);
      const frames = ${frames};
      const ptr = window.Module._malloc(frames * 4);
      const got = m.api.dumpPcm(ptr, frames);
      const bytes = window.Module.HEAPU8.subarray(ptr, ptr + got * 4);
      let s = '';
      const chunk = 32768;
      for (let i = 0; i < bytes.length; i += chunk) {
        s += String.fromCharCode.apply(null, bytes.subarray(i, i + chunk));
      }
      window.Module._free(ptr);
      return { ok, got, b64: btoa(s) };
    })()`, { awaitPromise: true });
    log(`    dump: load=${dumpInfo.ok} frames=${dumpInfo.got}`);
    const webPcm = path.join(OUT_DIR, 'web.pcm');
    fs.writeFileSync(webPcm, Buffer.from(dumpInfo.b64, 'base64'));
    log(`    wrote ${webPcm} (${fs.statSync(webPcm).size} bytes)`);

    const nativePcm = path.join(OUT_DIR, 'native.pcm');
    const native = spawn(path.join(ROOT, 'build', 'mdx2wav'), ['-d', '3', SONG], {
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    const chunks = [];
    native.stdout.on('data', (d) => chunks.push(d));
    await new Promise((resolve) => native.on('close', resolve));
    fs.writeFileSync(nativePcm, Buffer.concat(chunks));
    log(`    wrote ${nativePcm} (${fs.statSync(nativePcm).size} bytes)`);

    const cmp = spawn('python3', [path.join(__dirname, 'tools', 'compare_pcm.py'), webPcm, nativePcm], {
      stdio: 'inherit',
    });
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
