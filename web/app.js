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
  const TARGET_FRAMES = Math.round(SAMPLE_RATE * 0.50);   // steady state
  const PREBUFFER_FRAMES = Math.round(SAMPLE_RATE * 0.75);
  const BYTES_PER_STEP = CHUNK_FRAMES * 4;

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
  const statusEl = $('status');

  const state = {
    module: null,
    ctx: null,
    node: null,
    ring: null,
    ringSamples: null,
    ready: false,
    running: false,
    pumping: false,
    files: [],          // [{mdx, name, title}]
    current: -1,
    loadedRaw: null,    // {mdx: ArrayBuffer, pdx: ArrayBuffer|null, name: string, title: string}
    started: false,
    underruns: 0,
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

  function ringFrames() {
    const cap = state.ring[2];
    const w = Atomics.load(state.ring, 0) % cap;
    const r = Atomics.load(state.ring, 1) % cap;
    let n = w - r;
    if (n < 0) n += cap;
    return n;
  }

  async function initAudio() {
    if (state.ctx) {
      if (state.ctx.state === 'suspended') await state.ctx.resume();
      return;
    }
    const ctx = new AudioContext({ sampleRate: SAMPLE_RATE, latencyHint: 'interactive' });
    await ctx.audioWorklet.addModule('worklet/player-processor.js');
    const sab = new SharedArrayBuffer(16 + RING_BYTES);
    const ring = new Int32Array(sab);
    ring[0] = 0;
    ring[1] = 0;
    ring[2] = RING_FRAMES;
    ring[3] = 0;
    const node = new AudioWorkletNode(ctx, 'mdx-player', {
      numberOfInputs: 0,
      numberOfOutputs: 1,
      outputChannelCount: [2],
      processorOptions: { sab },
    });
    node.port.onmessage = (e) => {
      if (e.data && e.data.type === 'underrun') {
        state.underruns = e.data.count;
        if (state.underruns === 1) {
          setStatus('音声バッファが不足しました（タブが非アクティブの可能性があります）', 'warn');
        }
      }
    };
    node.connect(ctx.destination);
    state.ctx = ctx;
    state.node = node;
    state.ring = ring;
    state.ringSamples = new Int16Array(sab, 16);
  }

  // Prefill the ring before unpausing the audio graph so playback starts clean.
  function prefill() {
    const cap = RING_FRAMES;
    let w = Atomics.load(state.ring, 0) % cap;
    let filled = ringFrames();
    const ptr = api.audioBuffer();
    while (filled + CHUNK_FRAMES <= PREBUFFER_FRAMES) {
      const got = api.step();
      if (got <= 0) return false;
      for (let i = 0; i < got * 2; i++) {
        state.ringSamples[w * 2 + i] = window.Module.HEAP16[(ptr >> 1) + i];
      }
      w = (w + got) % cap;
      Atomics.store(state.ring, 0, w);
      filled += got;
    }
    return true;
  }

  // ------------------------------------------------------------------ pump

  let lastElapsed = 0;
  let pumpTimer = 0;

  function pump() {
    if (state.pumping) return;
    state.pumping = true;
    const tick = () => {
      if (state.stopped || !state.running || !state.ring) {
        state.pumping = false;
        if (pumpTimer) { clearTimeout(pumpTimer); pumpTimer = 0; }
        return;
      }
      // Top up the ring.  Audio generation is much faster than real time, so the
      // only reason to be here repeatedly is to cover the audio thread's
      // consumption; a generous per-tick budget lets the loop recover quickly
      // after the browser stalls it (a long render, a hidden tab, a GC pause).
      let steps = 0;
      const maxSteps = 200;
      while (state.running && ringFrames() < TARGET_FRAMES && steps < maxSteps) {
        const got = api.step();
        if (got <= 0) {
          onSongEnd();
          break;
        }
        const cap = RING_FRAMES;
        let w = Atomics.load(state.ring, 0) % cap;
        const ptr = api.audioBuffer();
        for (let i = 0; i < got * 2; i++) {
          state.ringSamples[w * 2 + i] = window.Module.HEAP16[(ptr >> 1) + i];
        }
        w = (w + got) % cap;
        Atomics.store(state.ring, 0, w);
        steps++;
      }
      if (steps) lastElapsed = api.elapsed();
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
      const low = ringFrames() < TARGET_FRAMES / 2;
      pumpTimer = setTimeout(() => {
        if (state.stopped || !state.running) { state.pumping = false; return; }
        tick();
        schedule();
      }, low ? 2 : 20);
    };
    schedule();
  }

  function updateTime() {
    if (!state.running) return;
    const el = lastElapsed;
    const dur = api.duration();
    const mm = (t) => `${String(Math.floor(t / 60)).padStart(2, '0')}:${String(Math.floor(t % 60)).padStart(2, '0')}`;
    setStatus(`再生中 ${mm(el)} / ${dur > 0 ? mm(dur) : '--:--'} ・ バッファ ${(ringFrames() / SAMPLE_RATE * 1000).toFixed(0)}ms` +
      (state.underruns ? ` <span class="warn">underrun ${state.underruns}</span>` : ''));
  }

  // -------------------------------------------------------------- playback

  function resetRing() {
    if (!state.ring) return;
    Atomics.store(state.ring, 1, Atomics.load(state.ring, 0));
    Atomics.store(state.ring, 3, 0);
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
    state.currentIndex = state.files.findIndex((f) => f.name === raw.name);
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

  async function defaultSong() {
    // MHAWK3.MDX requires MHAWK.PDX for its ADPCM samples.
    const mdx = await fetchBytes('songs/MHAWK3.MDX');
    let pdx = null;
    try {
      pdx = await fetchBytes('songs/MHAWK.PDX');
    } catch (e) {
      pdx = null;
    }
    return { mdx, pdx, name: 'MHAWK3.MDX', title: '' };
  }

  async function loadSongEntry(entry) {
    setStatus(`${entry.name} を読み込み中…`);
    const mdx = await fetchBytes(`songs/${encodeURIComponent(entry.name)}`);
    let pdx = null;
    if (entry.pdx) {
      try {
        pdx = await fetchBytes(`songs/${encodeURIComponent(entry.pdx)}`);
      } catch (e) {
        pdx = null;
      }
    }
    return { mdx, pdx, name: entry.name, title: entry.title || '' };
  }

  // ------------------------------------------------------------------- boot

  async function boot() {
    await ready;
    bindApi(window.Module);
    api.verbose(1);

    if (!crossOriginIsolated || typeof SharedArrayBuffer === 'undefined') {
      setStatus('SharedArrayBuffer が使えません。Cross-Origin-Isolation 対応のサーバーで開いてください ' +
        '(web/serve.js を使用)。', 'err');
      return;
    }

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
    setStatus('準備完了 — MHAWK3.MDX を再生します');

    // Song list (only available when the dev server exposes it).
    try {
      const res = await fetch('songs/index.json');
      if (res.ok) {
        state.files = await res.json();
        songSelect.innerHTML = state.files
          .map((f, i) => `<option value="${i}">${f.title ? f.title.replace(/</g, '&lt;') : f.name}</option>`)
          .join('');
        const i = state.files.findIndex((f) => f.name === 'MHAWK3.MDX');
        if (i >= 0) songSelect.value = String(i);
      }
    } catch (e) {
      songSelect.innerHTML = '<option>MHAWK3.MDX</option>';
    }

    // Auto-start with MHAWK3.MDX once the user gestures (browser autoplay rule).
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
    ringFrames,
    play,
    stop,
    loadSongEntry,
    defaultSong,
    setStatus,
    initAudio,
  };

  boot().catch((e) => setStatus(`起動エラー: ${e.message}`, 'err'));
})();
