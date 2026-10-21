#!/usr/bin/env python3
"""Generate a tiny, self-contained SPC file for testing mdx2wav.

The file contains a looping 500 Hz sine wave played by S-DSP voice 0.  It is
synthesised here rather than copied from a game, so it can live in the
repository and in automated tests.  Usage:

    python3 web/tools/make_test_spc.py out.spc
"""

import math
import struct
import sys

FILE_SIZE = 0x10200
RAM_OFFSET = 0x100
DSP_OFFSET = 0x10100
PROGRAM_ADDR = 0x0200
DIR_ADDR = 0x1000
BRR_ADDR = 0x2000


def brr_block(samples, shift=12, loop=False, end=False):
    header = (shift << 4) & 0xF0
    if loop:
        header |= 0x01
    if end:
        header |= 0x02
    data = bytearray()
    for i in range(0, 16, 2):
        def nibble(v):
            n = int(round(v))
            n = max(-8, min(7, n))
            return n & 0x0F
        data.append((nibble(samples[i]) << 4) | nibble(samples[i + 1]))
    assert len(data) == 8
    return bytes([header]) + bytes(data)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "test.spc"
    spc = bytearray(FILE_SIZE)

    # SPC header: signature, ID666 present, version 0.30, CPU registers.
    signature = b"SNES-SPC700 Sound File Data v0.30\x1a\x1a"
    spc[0:len(signature)] = signature
    spc[0x23] = 0x1A          # has ID666 tag
    spc[0x24] = 0x1E          # SPC version
    spc[0x25] = PROGRAM_ADDR & 0xFF
    spc[0x26] = (PROGRAM_ADDR >> 8) & 0xFF
    spc[0x27] = 0             # A
    spc[0x28] = 0             # X
    spc[0x29] = 0             # Y
    spc[0x2A] = 0             # PSW
    spc[0x2B] = 0xEF          # SP

    def tag(offset, text, size=32):
        raw = text.encode("ascii")[:size - 1]
        spc[offset:offset + len(raw)] = raw

    tag(0x2E, "mdx2wav SPC test")
    tag(0x4E, "mdx2wav")
    tag(0x6E, "mdx2wav")
    spc[0x9E:0xA1] = b"010"   # seconds until fadeout

    # RAM: an infinite BRA -2 loop at PROGRAM_ADDR.
    spc[RAM_OFFSET + PROGRAM_ADDR] = 0x2F
    spc[RAM_OFFSET + PROGRAM_ADDR + 1] = 0xFE

    # Sample directory entry 0: start and loop both point at BRR_ADDR.
    spc[RAM_OFFSET + DIR_ADDR + 0] = BRR_ADDR & 0xFF
    spc[RAM_OFFSET + DIR_ADDR + 1] = (BRR_ADDR >> 8) & 0xFF
    spc[RAM_OFFSET + DIR_ADDR + 2] = BRR_ADDR & 0xFF
    spc[RAM_OFFSET + DIR_ADDR + 3] = (BRR_ADDR >> 8) & 0xFF

    # One period (64 samples) of a 500 Hz sine at 32 kHz, split into 4 BRR
    # blocks.  The last block loops back to BRR_ADDR.
    samples = [7.0 * math.sin(2.0 * math.pi * i / 64.0) for i in range(64)]
    addr = RAM_OFFSET + BRR_ADDR
    for block in range(4):
        chunk = samples[block * 16:(block + 1) * 16]
        encoded = brr_block(chunk, shift=12, loop=(block == 3), end=(block == 3))
        spc[addr:addr + len(encoded)] = encoded
        addr += 9

    dsp = DSP_OFFSET

    def reg(offset, value):
        spc[dsp + offset] = value & 0xFF

    reg(0x0C, 0x7F)   # MVOL L
    reg(0x1C, 0x7F)   # MVOL R
    reg(0x5D, DIR_ADDR >> 8)  # DIR
    reg(0x4C, 0x01)   # KON voice 0
    reg(0x00, 0x7F)   # V0 VOL L
    reg(0x01, 0x7F)   # V0 VOL R
    reg(0x02, 0x00)   # V0 PITCH L
    reg(0x03, 0x10)   # V0 PITCH H (0x1000 = 32 kHz)
    reg(0x04, 0x00)   # V0 SRCN
    reg(0x06, 0x00)   # V0 ADSR1: GAIN mode
    reg(0x07, 0x7F)   # V0 GAIN

    with open(out, "wb") as f:
        f.write(spc)
    print(out)


if __name__ == "__main__":
    main()
