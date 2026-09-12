// AudioWorklet processor: drains a SharedArrayBuffer ring that the WASM engine
// fills from the main thread.
//
// Ring layout (Int32 header, then interleaved int16 stereo samples):
//   i32[0] = write index (stereo frames)
//   i32[1] = read index  (stereo frames)
//   i32[2] = capacity    (stereo frames)
//   i32[3] = underrun counter
// Samples start at byte offset 16.

class MdxPlayerProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const opts = (options && options.processorOptions) || {};
    this.sab = opts.sab;
    this.frame = opts.frame || 0;
    this.ring = new Int32Array(this.sab);
    this.samples = new Int16Array(this.sab, 16);
    this.capacity = this.ring[2];
    this.underruns = 0;
    this.port.onmessage = (e) => {
      if (e.data && e.data.type === 'reset') {
        Atomics.store(this.ring, 1, Atomics.load(this.ring, 0));
        Atomics.store(this.ring, 3, 0);
        this.underruns = 0;
      }
    };
  }

  process(inputs, outputs) {
    const out = outputs[0];
    if (!out || out.length === 0) return true;
    const left = out[0];
    const right = out.length > 1 ? out[1] : null;
    const frames = left.length;

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
