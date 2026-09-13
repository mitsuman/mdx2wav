// Browser front end for the mdx2wav WebAssembly port.
//
// The WASM engine (mdxweb.cpp) exposes a step function that renders one
// visualizer frame and produces 10 ms of stereo PCM.  This file pumps that step
// function to keep an AudioWorklet fed through a SharedArrayBuffer ring.

(() => {
  'use strict';

  const SAMPLE_RATE = 44100;
  const CHUNK_FRAMES = SAMPLE_RATE / 100;      // 10 ms per engine step
  const RING_SECONDS = 2;
  const RING_FRAMES = SAMPLE_RATE * RING_SECONDS;
  const RING_BYTES = RING_FRAMES * 4;          // interleaved int16 stereo
  // Ring fill to keep ahead of playback.  Adjustable from the page so the
  // latency/stability trade-off can be felt directly; 250 ms is the default.
  const DEFAULT_BUFFER_MS = 250;
  const MIN_BUFFER_MS = 30;
  const MAX_BUFFER_MS = 1500;
  const BYTES_PER_STEP = CHUNK_FRAMES * 4;

  function targetFrames() {
    return Math.round(SAMPLE_RATE * state.bufferMs / 1000);
  }
  function prebufferFrames() {
    // Fill a little beyond the target before starting, so playback does not
    // begin on an almost-empty ring.
    return Math.round(targetFrames() * 1.4);
  }

  const $ = (id) => document.getElementById(id);
  const canvas = $('screen');
  const overlay = $('overlay');
  const overlayBtn = $('overlay-btn');
  const playBtn = $('play');
  const stopBtn = $('stop');
  const fileBtn = $('file-btn');
  const fileInput = $('file');
  const songSelect = $('song');
  const spectrumToggle = $('spectrum');
  const bufferRange = $('buffer');
  const bufferValue = $('buffer-value');
  const statusEl = $('status');

  const state = {
    module: null,
    ctx: null,
    node: null,
    transport: 'none',   // 'shared' or 'message'
    produced: 0,         // frames handed to the audio thread (message mode)
    consumed: 0,         // frames the audio thread reports having played
    pending: null,       // batch being accumulated for the next transfer
    pendingFrames: 0,
    underruns: 0,
    ready: false,
    running: false,
    pumping: false,
    files: [],          // [{mdx, name, title}]
    current: -1,
    loadedRaw: null,    // {mdx: ArrayBuffer, pdx: ArrayBuffer|null, name: string, title: string}
    started: false,
    bufferMs: DEFAULT_BUFFER_MS,
  };

  const setStatus = (text, cls) => {
    statusEl.innerHTML = text ? `<span class="${cls || ''}">${text}</span>` : '';
  };

  // ---------------------------------------------------------------- module

  // mdxweb.js is loaded before this file and exposes either a Module object or
  // a Module factory function, depending on how Emscripten emits it.
  const ready = (() => {
    const existing = window.Module;
    if (typeof existing === 'function') {
      const prev = existing.onRuntimeInitialized;
      return new Promise((resolve) => {
        const mod = existing({
          canvas,
          noInitialRun: true,
          onRuntimeInitialized: () => {
            if (prev) prev();
            resolve(window.Module);
          },
        });
        window.Module = mod;
      });
    }
    const mod = existing || {};
    window.Module = mod;
    const prev = mod.onRuntimeInitialized;
    return new Promise((resolve) => {
      mod.canvas = canvas;
      mod.noInitialRun = true;
      mod.onRuntimeInitialized = () => {
        if (prev) prev();
        resolve(mod);
      };
    });
  })();

  const api = {};

  function bindApi(mod) {
    api.init = mod.cwrap('mdxweb_init', 'number',
      ['number', 'number', 'number', 'number', 'number']);
    api.configure = mod.cwrap('mdxweb_configure', null,
      ['number', 'number', 'number', 'number']);
    api.verbose = mod.cwrap('mdxweb_verbose', null, ['number']);
    api.load = mod.cwrap('mdxweb_load', 'number',
      ['number', 'number', 'number', 'number']);
    api.step = mod.cwrap('mdxweb_step', 'number', []);
    api.audioBuffer = mod.cwrap('mdxweb_audio_buffer', 'number', []);
    api.running = mod.cwrap('mdxweb_running', 'number', []);
    api.elapsed = mod.cwrap('mdxweb_elapsed', 'number', []);
    api.duration = mod.cwrap('mdxweb_duration', 'number', []);
    api.title = mod.cwrap('mdxweb_title', 'string', []);
    api.dumpPcm = mod.cwrap('mdxweb_dump_pcm', 'number', ['number', 'number']);
    api.shutdown = mod.cwrap('mdxweb_shutdown', null, []);
    api.setSpectrum = mod.cwrap('mdxweb_set_spectrum', null, ['number']);
    api.windowWidth = mod.cwrap('mdxweb_window_width', 'number', []);
  }

  function copyToHeap(bytes) {
    const ptr = window.Module._malloc(bytes.byteLength);
    window.Module.HEAPU8.set(new Uint8Array(bytes), ptr);
    return ptr;
  }

  // Decode the raw Shift_JIS title (if present) using the browser's decoder.
  function decodeTitle(mdxBytes) {
    try {
      const buf = new Uint8Array(mdxBytes);
      const start = 10;
      let end = start;
      while (end < buf.length - 1 && !(buf[end] === 0x0d && buf[end + 1] === 0x0a)) end++;
      if (end >= buf.length - 1) end = Math.min(buf.length, start + 128);
      const slice = buf.subarray(start, end);
      return new TextDecoder('shift_jis').decode(slice).replace(/[\r\n\0]/g, '').trim();
    } catch (e) {
      return '';
    }
  }

  // ------------------------------------------------------------- audio ring

  // Frames currently held by the audio thread.  In shared mode this is derived
  // from the ring indices; in message mode it is reported by the worklet.
  function bufferedFrames() {
    if (state.transport === 'shared' && state.ring) {
      const cap = state.ring[2];
      const w = Atomics.load(state.ring, 0) % cap;
      const r = Atomics.load(state.ring, 1) % cap;
      let n = w - r;
      if (n < 0) n += cap;
      return n;
    }
    // Message mode: frames handed over minus frames the audio thread reports
    // having played.  Deriving it from counters means a dropped progress message
    // cannot leave the producer stuck believing the queue is full.
    const held = state.produced - state.consumed;
    return held > 0 ? held : 0;
  }

  // Run the engine until the audio thread holds `want` frames, or the song ends.
  // Steps are accumulated and handed over in one batch: in message mode each
  // step would otherwise be a separate postMessage, which is far too chatty to
  // keep up with playback.
  const BATCH_MAX_FRAMES = Math.round(SAMPLE_RATE * 0.2);  // <= 200 ms per transfer
  function generateUpTo(want, maxSteps) {
    let steps = 0;
    while (state.running || steps === 0) {
      const held = bufferedFrames() + state.pendingFrames;
      if (held >= want || steps >= maxSteps) break;
      const got = api.step();
      if (got <= 0) return false;
      pushAudio(got);
      steps++;
    }
    return true;
  }

  // Hand the engine's current chunk to the audio thread.
  function pushAudio(frames) {
    const ptr = api.audioBuffer();
    if (state.transport === 'shared') {
      const cap = RING_FRAMES;
      let w = Atomics.load(state.ring, 0) % cap;
      for (let i = 0; i < frames * 2; i++) {
        state.ringSamples[w * 2 + i] = window.Module.HEAP16[(ptr >> 1) + i];
      }
      w = (w + frames) % cap;
      Atomics.store(state.ring, 0, w);
      return;
    }
    // Message mode: accumulate into a batch and post it once it is big enough.
    if (!state.pending) {
      state.pending = new Int16Array(BATCH_MAX_FRAMES * 2);
      state.pendingFrames = 0;
    }
    let src = ptr >> 1;
    let remaining = frames;
    while (remaining > 0) {
      const room = BATCH_MAX_FRAMES - state.pendingFrames;
      const take = Math.min(room, remaining);
      state.pending.set(window.Module.HEAP16.subarray(src, src + take * 2),
                        state.pendingFrames * 2);
      state.pendingFrames += take;
      src += take * 2;
      remaining -= take;
      if (state.pendingFrames >= BATCH_MAX_FRAMES) flushAudio();
    }
  }

  async function initAudio() {
    if (state.ctx) {
      if (state.ctx.state === 'suspended') await state.ctx.resume();
      return;
    }
    const ctx = new AudioContext({ sampleRate: SAMPLE_RATE, latencyHint: 'interactive' });
    await ctx.audioWorklet.addModule('worklet/player-processor.js');

    const options = {
      numberOfInputs: 0,
      numberOfOutputs: 1,
      outputChannelCount: [2],
      processorOptions: {},
    };

    // SharedArrayBuffer is faster, but it needs COOP/COEP response headers that
    // static hosts such as GitHub Pages cannot set.  Fall back to transferring
    // sample blocks over postMessage when it is unavailable.  ?transport=...
    // forces one or the other, which is handy for testing and for hosts that
    // send the headers but where the shared ring misbehaves.
    const forced = new URLSearchParams(location.search).get('transport');
    const wantShared = forced ? forced === 'shared'
                              : typeof SharedArrayBuffer === 'function';
    if (wantShared && typeof SharedArrayBuffer === 'function') {
      const sab = new SharedArrayBuffer(16 + RING_BYTES);
      const ring = new Int32Array(sab);
      ring[0] = 0;
      ring[1] = 0;
      ring[2] = RING_FRAMES;
      ring[3] = 0;
      state.ring = ring;
      state.ringSamples = new Int16Array(sab, 16);
      state.transport = 'shared';
      options.processorOptions.sab = sab;
    } else {
      state.ring = null;
      state.ringSamples = null;
      state.transport = 'message';
    }

    const node = new AudioWorkletNode(ctx, 'mdx-player', options);
    node.port.onmessage = (e) => {
      const msg = e.data;
      if (!msg) return;
      if (typeof msg.consumed === 'number') {
        state.consumed = msg.consumed;
      }
      if (msg.type === 'underrun') {
        state.underruns = msg.count;
        // One gap of a few milliseconds is inaudible; only warn when it keeps
        // happening, which is when the buffer really is too small.
        if (state.underruns > 40 && !state.warnedUnderrun) {
          state.warnedUnderrun = true;
          setStatus('音声バッファが不足気味です — 下のスライダーで増やせます', 'warn');
        }
      }
    };
    node.connect(ctx.destination);
    state.ctx = ctx;
    state.node = node;
    state.produced = 0;
    state.consumed = 0;
    state.underruns = 0;
  }

  // Fill the audio thread before unpausing the graph so playback starts clean.
  function prefill() {
    return generateUpTo(prebufferFrames(), 4000);
  }

  function flushAudio() {
    if (!state.pending || state.pendingFrames === 0 || !state.node) return;
    const frames = state.pendingFrames;
    // A real copy, not a view: the buffer is transferred (detached) to the audio
    // thread, so the accumulation buffer has to stay usable afterwards.
    const out = new Int16Array(frames * 2);
    out.set(state.pending.subarray(0, frames * 2));
    state.pendingFrames = 0;
    state.produced += frames;
    state.node.port.postMessage({ type: 'samples', frames, buffer: out.buffer }, [out.buffer]);
  }

  // ------------------------------------------------------------------ pump

  let lastElapsed = 0;
  let pumpTimer = 0;

  function pump() {
    if (state.pumping) return;
    state.pumping = true;
    const tick = () => {
      if (state.stopped || !state.running || !state.node) {
        state.pumping = false;
        if (pumpTimer) { clearTimeout(pumpTimer); pumpTimer = 0; }
        return;
      }
      // Top up the audio thread's queue.  Generation is much faster than real
      // time, so the only reason to be here repeatedly is to cover its
      // consumption; the budget lets the loop recover after a stall.
      const before = api.elapsed();
      const ok = generateUpTo(targetFrames(), 400);
      flushAudio();
      if (!ok) {
        onSongEnd();
      } else if (api.elapsed() !== before) {
        lastElapsed = api.elapsed();
      }
      updateTime();
    };
    tick();
    // Drive the loop from requestAnimationFrame (smooth while the page is
    // visible) and from a self-rescheduling timer, so playback also survives a
    // hidden or busy tab.  The timer interval tightens when the ring runs low,
    // which is what lets the loop catch up after a stall.
    const raf = () => {
      if (state.stopped || !state.running) { state.pumping = false; return; }
      tick();
      requestAnimationFrame(raf);
    };
    requestAnimationFrame(raf);

    const schedule = () => {
      if (state.stopped || !state.running) { state.pumping = false; return; }
      // Tick quickly: without pthreads the audio thread and the renderer share
      // the main thread, so the loop has to refill in small, frequent steps to
      // ride out a slow frame.  It tightens further when the queue runs low.
      const held = bufferedFrames();
      const target = targetFrames();
      const interval = held < target / 2 ? 2 : 8;
      pumpTimer = setTimeout(() => {
        if (state.stopped || !state.running) { state.pumping = false; return; }
        tick();
        schedule();
      }, interval);
    };
    schedule();
  }

  function updateTime() {
    if (!state.running) return;
    const el = lastElapsed;
    const dur = api.duration();
    const mm = (t) => `${String(Math.floor(t / 60)).padStart(2, '0')}:${String(Math.floor(t % 60)).padStart(2, '0')}`;
    // The buffer setting itself is shown next to its slider below the canvas.
    const filledMs = bufferedFrames() / SAMPLE_RATE * 1000;
    setStatus(`再生中 ${mm(el)} / ${dur > 0 ? mm(dur) : '--:--'} ・ 実バッファ ${filledMs.toFixed(0)}ms` +
      (state.underruns ? ` ・ <span class="warn">underrun ${state.underruns}</span>` : ''));
  }

  // -------------------------------------------------------------- playback

  function resetRing() {
    if (!state.node) return;
    state.node.port.postMessage({ type: 'reset' });
    state.produced = 0;
    state.consumed = 0;
    state.pending = null;
    state.pendingFrames = 0;
    state.underruns = 0;
  }

  function loadIntoEngine(raw) {
    const mdxPtr = copyToHeap(raw.mdx);
    let pdxPtr = 0;
    if (raw.pdx && raw.pdx.byteLength) {
      pdxPtr = copyToHeap(raw.pdx);
    }
    const ok = api.load(mdxPtr, raw.mdx.byteLength, pdxPtr, raw.pdx ? raw.pdx.byteLength : 0);
    window.Module._free(mdxPtr);
    if (pdxPtr) window.Module._free(pdxPtr);
    return ok;
  }

  async function play(raw) {
    await ready;
    await initAudio();

    const engineTitle = raw.title || decodeTitle(raw.mdx);
    setStatus('読み込み中…');
    if (!loadIntoEngine(raw)) {
      setStatus('MDXの読み込みに失敗しました', 'err');
      return;
    }
    state.loadedRaw = raw;
    state.currentIndex = state.files.findIndex((f) => (f.path || f.name) === (raw.path || raw.name));
    if (state.currentIndex >= 0) songSelect.value = String(state.currentIndex);
    document.title = `MDX Player — ${engineTitle || raw.name}`;

    api.configure(100, 0, 2, 0);
    resetRing();
    prefill();
    await state.ctx.resume();
    state.running = true;
    state.stopped = false;
    state.started = true;
    lastElapsed = 0;
    overlay.style.display = 'none';
    playBtn.disabled = true;
    stopBtn.disabled = false;
    pump();
  }

  // The layout is fixed at init time, so a change of the spectrum setting needs
  // the engine restarted.  Playback resumes from the beginning of the song.
  async function restartForLayout() {
    if (!state.ready) return;
    const wasRunning = state.running;
    const raw = state.loadedRaw || state.pendingRaw;
    stop();
    await ready;
    try {
      // Tear the engine down so mdxweb_init() can build the new layout.
      api.shutdown();
      api.setSpectrum(spectrumToggle.checked ? 1 : 0);
      canvas.width = api.windowWidth();
      if (!api.init(8, 8, 1.0, canvas.width, canvas.height)) {
        setStatus('レイアウト変更に失敗しました', 'err');
        return;
      }
    } catch (e) {
      setStatus(`レイアウト変更に失敗: ${e && e.message ? e.message : e}`, 'err');
      return;
    }
    if (wasRunning && raw) {
      await play(raw);
    } else {
      setStatus(`レイアウトを変更しました（スペアナ${spectrumToggle.checked ? '表示' : '非表示'}）`);
    }
  }

  function stop() {
    state.running = false;
    state.pumping = false;
    state.stopped = true;
    if (pumpTimer) { clearTimeout(pumpTimer); pumpTimer = 0; }
    if (state.ready || state.module) { /* engine stop is optional */ }
    try { api.running && api.running(); } catch (e) { /* ignore */ }
    playBtn.disabled = false;
    stopBtn.disabled = true;
    setStatus('停止しました');
  }

  function onSongEnd() {
    state.running = false;
    state.pumping = false;
    if (pumpTimer) { clearTimeout(pumpTimer); pumpTimer = 0; }
    playBtn.disabled = false;
    stopBtn.disabled = true;
    setStatus('曲の終わりまで再生しました');
  }

  // ------------------------------------------------------------ file loading

  async function fetchBytes(url) {
    const res = await fetch(url);
    if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
    return res.arrayBuffer();
  }

  // Songs are served one directory per library (songs/SION/…, songs/SION2/…).
  const DEFAULT_SONG = { name: 'sion00.mdx', path: 'SION/sion00.mdx', pdx: 'SION/SION.pdx' };

  function songUrl(relPath) {
    return `songs/${relPath.split('/').map(encodeURIComponent).join('/')}`;
  }

  async function defaultSong() {
    // Fall back to SION2 if the SION set is not present.
    try {
      return await loadSongEntry(DEFAULT_SONG);
    } catch (e) {
      if (state.files.length > 0) {
        return loadSongEntry(state.files[0]);
      }
      throw e;
    }
  }

  async function loadSongEntry(entry) {
    const rel = entry.path || entry.name;
    setStatus(`${entry.name} を読み込み中…`);
    const mdx = await fetchBytes(songUrl(rel));
    let pdx = null;
    if (entry.pdx) {
      try {
        pdx = await fetchBytes(songUrl(entry.pdx));
      } catch (e) {
        pdx = null;
      }
    }
    return { mdx, pdx, name: entry.name, path: rel, title: entry.title || '' };
  }

  // ------------------------------------------------------------------- boot

  async function boot() {
    await ready;
    bindApi(window.Module);
    api.verbose(1);

    // The spectrum setting determines the layout width, so apply it first and
    // size the canvas to match (hiding the spectrum narrows the window).
    api.setSpectrum(spectrumToggle.checked ? 1 : 0);
    canvas.width = api.windowWidth();
    const ok = api.init(8, 8, 1.0, canvas.width, canvas.height);
    if (!ok) {
      setStatus('エンジンの初期化に失敗しました', 'err');
      return;
    }
    state.ready = true;
    playBtn.disabled = false;
    overlayBtn.textContent = 'クリックして再生';
    setStatus('準備完了');

    // Song list.  On a static host this is built when dist/ is assembled; the
    // dev server generates it on the fly.
    try {
      const res = await fetch('songs/index.json');
      if (res.ok) {
        state.files = await res.json();
        const label = (f) => (f.title ? f.title.replace(/</g, '&lt;') : f.name).trim();
        songSelect.innerHTML = state.files
          .map((f, i) => `<option value="${i}">${label(f)}</option>`)
          .join('');
        const di = state.files.findIndex((f) => (f.path || f.name) === DEFAULT_SONG.path);
        if (di >= 0) songSelect.value = String(di);
      }
    } catch (e) {
      songSelect.innerHTML = `<option>${DEFAULT_SONG.name}</option>`;
    }

    if (state.files.length === 0) {
      // No songs bundled with this build; let the user open one.
      setStatus('同梱の曲がありません — 「ファイルを開く…」で .mdx / .pdx を選んでください');
      overlayBtn.textContent = 'クリックして起動';
      return;
    }

    // Auto-start with the default song once the user gestures (autoplay rule).
    try {
      const raw = await defaultSong();
      state.pendingRaw = raw;
      setStatus(`準備完了 — ${raw.name} を再生できます`);
    } catch (e) {
      setStatus(`曲の取得に失敗: ${e.message}`, 'err');
    }
  }

  overlayBtn.addEventListener('click', async () => {
    overlayBtn.disabled = true;
    try {
      if (state.pendingRaw) {
        await play(state.pendingRaw);
        state.pendingRaw = null;
      } else {
        await initAudio();
        overlay.style.display = 'none';
      }
    } catch (e) {
      setStatus(`再生に失敗: ${e.message}`, 'err');
      overlayBtn.disabled = false;
    }
  });

  playBtn.addEventListener('click', async () => {
    if (state.pendingRaw) {
      const raw = state.pendingRaw;
      state.pendingRaw = null;
      await play(raw);
      return;
    }
    if (state.currentIndex >= 0) {
      await play(await loadSongEntry(state.files[state.currentIndex]));
    } else if (state.loadedRaw) {
      await play(state.loadedRaw);
    }
  });

  stopBtn.addEventListener('click', stop);

  songSelect.addEventListener('change', async () => {
    if (state.files.length === 0) return;
    const entry = state.files[Number(songSelect.value)];
    state.running = false;
    state.pumping = false;
    await play(await loadSongEntry(entry));
  });

  // The spectrum analyzers are off by default: they cost sixteen 512-point FFTs
  // per frame, which is the heaviest part of the visualizer, and hiding them lets
  // the window (and the waveform display) be narrower.
  spectrumToggle.checked = false;
  spectrumToggle.addEventListener('change', restartForLayout);

  // Buffer / latency control.  Changing it re-primes the ring from the current
  // playback position, so the effect on latency and on underruns is immediate.
  bufferRange.min = String(MIN_BUFFER_MS);
  bufferRange.max = String(MAX_BUFFER_MS);
  bufferRange.value = String(state.bufferMs);
  bufferValue.textContent = `${state.bufferMs}ms`;
  bufferRange.addEventListener('input', () => {
    setBufferMs(Number(bufferRange.value), false);
  });
  bufferRange.addEventListener('change', () => {
    setBufferMs(Number(bufferRange.value), true);
  });

  function setBufferMs(ms, reprime) {
    state.bufferMs = Math.max(MIN_BUFFER_MS, Math.min(MAX_BUFFER_MS, Math.round(ms)));
    bufferValue.textContent = `${state.bufferMs}ms`;
    if (reprime) reprimeRing();
  }

  // Drop whatever is buffered, then fill up to the new target so the change is
  // audible immediately instead of after the old contents drain.
  function reprimeRing() {
    if (!state.running || !state.node) return;
    state.node.port.postMessage({ type: 'reset' });
    state.underruns = 0;
    prefill();
    lastElapsed = api.elapsed();
    updateTime();
  }

  fileBtn.addEventListener('click', () => fileInput.click());

  fileInput.addEventListener('change', async () => {
    const files = Array.from(fileInput.files || []);
    if (files.length === 0) return;
    const mdxFile = files.find((f) => /\.mdx$/i.test(f.name));
    const pdxFile = files.find((f) => /\.pdx$/i.test(f.name));
    if (!mdxFile) {
      setStatus('.mdx ファイルを選んでください', 'err');
      return;
    }
    state.files = [];
    songSelect.innerHTML = '<option>ローカルファイル</option>';
    const raw = {
      mdx: await mdxFile.arrayBuffer(),
      pdx: pdxFile ? await pdxFile.arrayBuffer() : null,
      name: mdxFile.name,
      title: '',
    };
    await play(raw);
  });

  window.addEventListener('keydown', (e) => {
    if (e.target && /^(INPUT|SELECT|TEXTAREA)$/.test(e.target.tagName)) return;
    if (e.code === 'Space') {
      e.preventDefault();
      if (state.running) stop();
      else playBtn.click();
    }
  });

  // Test/debug hook used by web/verify.js.
  window.__mdxweb = {
    api,
    state,
    ready,
    bufferedFrames,
    play,
    stop,
    loadSongEntry,
    defaultSong,
    setStatus,
    initAudio,
  };

  boot().catch((e) => setStatus(`起動エラー: ${e.message}`, 'err'));
})();
