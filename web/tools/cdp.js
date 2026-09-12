// Tiny Chrome DevTools Protocol driver built on Node's built-in fetch/WebSocket.
// Used by web/verify.js; has no npm dependencies.

const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const CHROME_CANDIDATES = [
  process.env.CHROME_BIN,
  path.resolve(__dirname, '..', '..', 'tools', 'chrome', 'chrome-headless-shell-mac-arm64', 'chrome-headless-shell'),
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
].filter(Boolean);

function findChrome() {
  for (const c of CHROME_CANDIDATES) {
    try {
      fs.accessSync(c, fs.constants.X_OK);
      return c;
    } catch (e) { /* keep looking */ }
  }
  throw new Error('no Chrome binary found (set CHROME_BIN)');
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

class Browser {
  constructor({ port = 9333, headless = true, extraArgs = [] } = {}) {
    this.port = port;
    this.headless = headless;
    this.extraArgs = extraArgs;
    this.nextId = 1;
    this.pending = new Map();
    this.listeners = [];
  }

  async start() {
    this.profile = fs.mkdtempSync(path.join(os.tmpdir(), 'mdxweb-chrome-'));
    const args = [
      `--remote-debugging-port=${this.port}`,
      `--user-data-dir=${this.profile}`,
      '--no-first-run',
      '--no-default-browser-check',
      '--disable-gpu',
      '--no-sandbox',
      '--disable-dev-shm-usage',
      '--mute-audio',
      '--autoplay-policy=no-user-gesture-required',
      '--use-fake-device-for-media-stream',
      '--window-size=1440,1000',
      'about:blank',
    ];
    if (this.headless) {
      args.unshift('--headless=new');
    }
    args.push(...this.extraArgs);
    this.proc = spawn(findChrome(), args, { stdio: ['ignore', 'pipe', 'pipe'] });
    this.proc.stdout.on('data', () => {});
    this.proc.stderr.on('data', () => {});

    const deadline = Date.now() + 20000;
    let version = null;
    while (Date.now() < deadline) {
      try {
        const res = await fetch(`http://127.0.0.1:${this.port}/json/version`);
        version = await res.json();
        break;
      } catch (e) {
        await sleep(150);
      }
    }
    if (!version) throw new Error('Chrome did not expose the DevTools endpoint');

    this.ws = new WebSocket(version.webSocketDebuggerUrl);
    await new Promise((resolve, reject) => {
      this.ws.addEventListener('open', resolve);
      this.ws.addEventListener('error', reject);
    });
    this.ws.addEventListener('message', (ev) => this.onMessage(ev.data));
    return this;
  }

  onMessage(raw) {
    const msg = JSON.parse(raw);
    if (msg.id && this.pending.has(msg.id)) {
      const { resolve, reject } = this.pending.get(msg.id);
      this.pending.delete(msg.id);
      if (msg.error) reject(new Error(JSON.stringify(msg.error)));
      else resolve(msg.result);
      return;
    }
    for (const l of this.listeners) l(msg);
  }

  send(method, params = {}, sessionId) {
    const id = this.nextId++;
    const payload = { id, method, params };
    if (sessionId) payload.sessionId = sessionId;
    this.ws.send(JSON.stringify(payload));
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      setTimeout(() => {
        if (this.pending.has(id)) {
          this.pending.delete(id);
          reject(new Error(`CDP timeout: ${method}`));
        }
      }, 120000);
    });
  }

  on(fn) { this.listeners.push(fn); }

  async newPage(url) {
    const { targetId } = await this.send('Target.createTarget', { url: 'about:blank' });
    const { sessionId } = await this.send('Target.attachToTarget', { targetId, flatten: true });
    this.sessionId = sessionId;
    await this.send('Page.enable', {}, sessionId);
    await this.send('Runtime.enable', {}, sessionId);
    await this.send('Log.enable', {}, sessionId);
    await this.send('Page.navigate', { url }, sessionId);
    return sessionId;
  }

  async evaluate(expression, { awaitPromise = false } = {}) {
    const res = await this.send('Runtime.evaluate', {
      expression,
      returnByValue: true,
      awaitPromise,
      userGesture: true,
    }, this.sessionId);
    if (res.exceptionDetails) {
      throw new Error(res.exceptionDetails.exception?.description || JSON.stringify(res.exceptionDetails));
    }
    return res.result.value;
  }

  async waitFor(expression, timeoutMs = 30000, label = expression) {
    const deadline = Date.now() + timeoutMs;
    let last;
    while (Date.now() < deadline) {
      try {
        last = await this.evaluate(expression);
        if (last) return last;
      } catch (e) {
        last = `error: ${e.message}`;
      }
      await sleep(200);
    }
    throw new Error(`timeout waiting for ${label} (last value: ${JSON.stringify(last)})`);
  }

  async screenshot(file) {
    const res = await this.send('Page.captureScreenshot', { format: 'png' }, this.sessionId);
    fs.writeFileSync(file, Buffer.from(res.data, 'base64'));
    return file;
  }

  async close() {
    try { this.ws.close(); } catch (e) { /* ignore */ }
    try { this.proc.kill('SIGKILL'); } catch (e) { /* ignore */ }
    try { fs.rmSync(this.profile, { recursive: true, force: true }); } catch (e) { /* ignore */ }
  }
}

module.exports = { Browser, sleep };
