#!/usr/bin/env python3
"""Compare two raw 16-bit stereo PCM streams (the web port vs the native binary).

The browser and native builds share the same DSP code, but the browser may start
its stream a fraction of a millisecond earlier or later.  A small time offset
(up to ~30 ms) is therefore searched for and compensated; after that the two
streams must match closely.  Small residual differences are tolerated because
the two toolchains may fuse floating point operations differently.
"""

import struct
import sys


def read_samples(path, frames):
    with open(path, "rb") as fh:
        data = fh.read(frames * 4)
    return struct.unpack("<%dh" % (len(data) // 2), data)


def main():
    if len(sys.argv) != 3:
        print("usage: compare_pcm.py web.pcm native.pcm", file=sys.stderr)
        return 2
    a_path, b_path = sys.argv[1], sys.argv[2]

    frames = 44100 * 2
    a = read_samples(a_path, frames)
    b = read_samples(b_path, frames)
    if len(a) == 0 or len(b) == 0:
        print("no samples to compare")
        return 1

    # Search a small time offset (in samples, either direction) for the best
    # alignment; the browser stream can start a block earlier or later.
    search = 441 * 4
    probe = 44100 * 2
    best_shift = 0
    best_err = None
    for shift in range(-search, search + 1):
        if shift >= 0:
            if shift + probe > len(a) or probe > len(b):
                continue
            err = sum((a[shift + i] - b[i]) ** 2 for i in range(0, probe, 7))
        else:
            k = -shift
            if k + probe > len(b) or probe > len(a):
                continue
            err = sum((a[i] - b[k + i]) ** 2 for i in range(0, probe, 7))
        if best_err is None or err < best_err:
            best_err = err
            best_shift = shift
    if best_shift > 0:
        print(f"    compensated {best_shift * 1000 // 44100} ms start offset (web later)")
        a = a[best_shift:]
    elif best_shift < 0:
        print(f"    compensated {-best_shift * 1000 // 44100} ms start offset (web earlier)")
        b = b[-best_shift:]

    n = min(len(a), len(b))
    diff = 0
    max_diff = 0
    sq = 0
    ref_sq = 0
    for i in range(n):
        d = a[i] - b[i]
        if d:
            diff += 1
            max_diff = max(max_diff, abs(d))
        sq += d * d
        ref_sq += b[i] * b[i]

    rms_diff = (sq / n) ** 0.5
    ref_rms = (ref_sq / n) ** 0.5
    rel = rms_diff / ref_rms if ref_rms else 0.0
    print(f"    compared {n} samples ({n // 2} stereo frames)")
    print(f"    reference RMS : {ref_rms:.1f}")
    print(f"    difference RMS: {rms_diff:.2f} ({rel * 100:.3f}% of reference)")
    print(f"    differing samples: {diff} ({diff * 100.0 / n:.2f}%), max delta {max_diff}")

    if ref_rms < 1.0:
        print("    reference stream is (near) silent - comparison is meaningless")
        return 1
    if rel > 0.02:
        print("    MISMATCH: web output differs from the native build")
        return 1
    print("    OK: web output matches the native build")
    return 0


if __name__ == "__main__":
    sys.exit(main())
