// AudioWorklet processor for the MDX player.
//
// Two transports are supported:
//
//   * "shared"  – a SharedArrayBuffer ring that the WASM engine fills from the
//                 main thread.  Lowest overhead, but the page must be served
//                 with COOP/COEP headers, which GitHub Pages cannot send.
//   * "message" – the main thread posts blocks of samples with
//                 port.postMessage({type:'samples', ...}, [buf]) and the
//                 processor reassembles them into a queue.  Works anywhere a
//                 normal static host serves the page.
//
// Shared ring layout (Int32 header, then interleaved int16 stereo samples):
//   i32[0] = write index (stereo frames)
//   i32[1] = read index  (stereo frames)
//   i32[2] = capacity    (stereo frames)
//   i32[3] = underrun counter
// Samples start at byte offset 16.

class MdxPlayerProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const opts = (options && options.processorOptions) || {};

    this.mode = opts.sab ? 'shared' : 'message';
    this.underruns = 0;

    if (this.mode === 'shared') {
      this.ring = new Int32Array(opts.sab);
      this.samples = new Int16Array(opts.sab, 16);
      this.capacity = this.ring[2];
    } else {
      // Message mode: a small queue of Int16Array chunks, plus the position
      // inside the first one.
      this.queue = [];
      this.queuedFrames = 0;
      this.offset = 0;
      this.highWater = 0;
    }

    this.port.onmessage = (e) => this.onMessage(e.data);
  }

  onMessage(msg) {
    if (!msg) return;
    switch (msg.type) {
      case 'reset':
        this.consumed = 0;
        if (this.mode === 'shared') {
          Atomics.store(this.ring, 1, Atomics.load(this.ring, 0));
          Atomics.store(this.ring, 3, 0);
        } else {
          this.queue.length = 0;
          this.queuedFrames = 0;
          this.offset = 0;
        }
        this.underruns = 0;
        break;

      case 'samples':
        if (this.mode !== 'message') break;
        this.queue.push(new Int16Array(msg.buffer));
        this.queuedFrames += msg.frames;
        if (this.queuedFrames > this.highWater) this.highWater = this.queuedFrames;
        break;

      default:
        break;
    }
  }

  pullMessageMode(left, right, frames) {
    let written = 0;
    while (written < frames && this.queue.length > 0) {
      const chunk = this.queue[0];
      const available = (chunk.length / 2) - this.offset;
      const take = Math.min(available, frames - written);
      let src = this.offset * 2;
      for (let i = 0; i < take; i++) {
        const l = chunk[src] / 32768;
        const r = chunk[src + 1] / 32768;
        left[written + i] = l;
        if (right) right[written + i] = r;
        src += 2;
      }
      written += take;
      this.offset += take;
      if (this.offset >= chunk.length / 2) {
        this.queue.shift();
        this.offset = 0;
      }
    }
    this.queuedFrames -= written;
    this.consumed = (this.consumed || 0) + written;

    // Report progress often enough that the producer can keep the queue topped
    // up, but not on every 3 ms quantum.
    if (this.consumed - (this.lastReport || 0) >= 4096) {
      this.lastReport = this.consumed;
      this.port.postMessage({ type: 'progress', consumed: this.consumed, queued: this.queuedFrames });
    }

    if (written < frames) {
      for (let i = written; i < frames; i++) {
        left[i] = 0;
        if (right) right[i] = 0;
      }
      this.underruns++;
      this.port.postMessage({ type: 'underrun', count: this.underruns, consumed: this.consumed, queued: this.queuedFrames });
    }
    return true;
  }

  process(inputs, outputs) {
    const out = outputs[0];
    if (!out || out.length === 0) return true;
    const left = out[0];
    const right = out.length > 1 ? out[1] : null;
    const frames = left.length;

    if (this.mode === 'message') {
      return this.pullMessageMode(left, right, frames);
    }

    const cap = this.capacity;
    let read = Atomics.load(this.ring, 1) % cap;
    const write = Atomics.load(this.ring, 0) % cap;

    let available = write - read;
    if (available < 0) available += cap;

    if (available < frames) {
      // Not enough data: emit what we have, then silence.
      this.underruns++;
      Atomics.store(this.ring, 3, this.underruns);
      this.port.postMessage({ type: 'underrun', count: this.underruns });
    }

    const n = Math.min(available, frames);
    for (let i = 0; i < n; i++) {
      const s = this.samples[(read + i) * 2];
      left[i] = s / 32768;
      if (right) right[i] = this.samples[(read + i) * 2 + 1] / 32768;
    }
    for (let i = n; i < frames; i++) {
      left[i] = 0;
      if (right) right[i] = 0;
    }

    read = (read + n) % cap;
    Atomics.store(this.ring, 1, read);
    return true;
  }
}

registerProcessor('mdx-player', MdxPlayerProcessor);
