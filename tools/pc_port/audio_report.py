#!/usr/bin/env python3
"""Checks a PPL_AUDIO_DUMP recording (raw 32768 Hz stereo s16) without
listening to it: levels, clipping, DC offset, silences, dropouts (sudden
silence in the middle of music), clicks (sample jumps), stereo, pitch
content per segment, and a spectrogram picture.

    PPL_AUDIO_DUMP=out.raw pokeplatinum ...
    python tools/pc_port/audio_report.py out.raw [--png spec.png] [--start S] [--len S]

Pure Python (no numpy), so it runs on any machine with the game's build
tools. Prints a report and exits 1 when the recording is silent, clips
heavily or has dropouts.
"""
import argparse
import array
import cmath
import math
import struct
import sys
import zlib

RATE = 32768


def fft(x):
    """Iterative radix-2 FFT of a list of floats (len = power of 2)."""
    n = len(x)
    a = [complex(v, 0) for v in x]
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    size = 2
    while size <= n:
        w = cmath.exp(-2j * math.pi / size)
        half = size // 2
        tw = [w ** k for k in range(half)]
        for start in range(0, n, size):
            for k in range(half):
                u = a[start + k]
                v = a[start + k + half] * tw[k]
                a[start + k] = u + v
                a[start + k + half] = u - v
        size *= 2
    return a


def note_name(f):
    if f <= 0:
        return "-"
    n = round(12 * math.log2(f / 440.0)) + 69
    return ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"][n % 12] + str(n // 12 - 1)


def write_png(path, w, h, rgb):
    raw = b"".join(b"\0" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(t, b):
        c = struct.pack(">I", len(b)) + t + b
        return c + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def colormap(v):
    """0..1 -> dark blue, purple, orange, yellow."""
    v = max(0.0, min(1.0, v))
    stops = [(0, (0, 0, 20)), (0.35, (80, 20, 120)), (0.65, (220, 80, 40)), (0.85, (250, 180, 30)), (1, (255, 255, 200))]
    for (a, ca), (b, cb) in zip(stops, stops[1:]):
        if v <= b:
            t = (v - a) / (b - a)
            return tuple(int(ca[i] + (cb[i] - ca[i]) * t) for i in range(3))
    return stops[-1][1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("raw")
    ap.add_argument("--png")
    ap.add_argument("--start", type=float, default=0)
    ap.add_argument("--len", type=float, default=0, help="seconds analysed (0 = all)")
    ap.add_argument("--fmax", type=float, default=8000)
    a = ap.parse_args()

    data = array.array("h")
    with open(a.raw, "rb") as f:
        data.frombytes(f.read())
    if sys.byteorder != "little":
        data.byteswap()
    L, R = data[0::2], data[1::2]
    s0 = int(a.start * RATE)
    s1 = len(L) if a.len <= 0 else min(len(L), s0 + int(a.len * RATE))
    L, R = L[s0:s1], R[s0:s1]
    n = len(L)
    if n < RATE // 2:
        print("recording too short (%d samples)" % n)
        return 1
    dur = n / RATE
    problems = []

    peak = max(max(L), -min(L), max(R), -min(R))
    clip = sum(1 for v in L if abs(v) >= 32760) + sum(1 for v in R if abs(v) >= 32760)
    dcL, dcR = sum(L) / n, sum(R) / n
    print("duration %.1f s, peak %d (%.1f dBFS), clipped samples %d (%.3f%%), DC offset L %.1f R %.1f"
          % (dur, peak, 20 * math.log10(max(peak, 1) / 32768), clip, 100 * clip / (2 * n), dcL, dcR))
    if clip / (2 * n) > 0.001:
        problems.append("clipping")

    # Loudness per 100 ms; silence and dropouts.
    win = RATE // 10
    rms = []
    for i in range(0, n - win + 1, win):
        acc = 0
        for k in range(i, i + win, 4):
            acc += L[k] * L[k] + R[k] * R[k]
        rms.append(math.sqrt(acc / (win // 2)))
    loud = [20 * math.log10(max(v, 1) / 32768) for v in rms]
    silent = [v < -60 for v in loud]
    print("level: median %.1f dBFS, max %.1f dBFS, silent 100 ms windows %d / %d"
          % (sorted(loud)[len(loud) // 2], max(loud), sum(silent), len(silent)))
    if all(silent):
        problems.append("silent")
    # Dropout: a short silence (<= 300 ms) between two loud stretches.
    drops = []
    i = 0
    while i < len(silent):
        if silent[i]:
            j = i
            while j < len(silent) and silent[j]:
                j += 1
            if 0 < i and j < len(silent) and j - i <= 3 and loud[i - 1] > -35 and loud[j] > -35:
                drops.append(i / 10)
            i = j
        else:
            i += 1
    print("dropouts (silence <= 300 ms inside loud music): %s" % (["%.1f s" % t for t in drops[:10]] or "none"))
    if drops:
        problems.append("dropouts")

    # Clicks: a jump between consecutive samples much larger than the local signal.
    clicks = []
    for i in range(1, n):
        d = abs(L[i] - L[i - 1])
        if d > 20000:
            clicks.append(i / RATE)
    print("sample jumps > 20000: %d%s" % (len(clicks), (" (first at %.2f s)" % clicks[0]) if clicks else ""))

    # Stereo.
    num = sum(L[i] * R[i] for i in range(0, n, 8))
    den = math.sqrt(sum(L[i] * L[i] for i in range(0, n, 8)) * sum(R[i] * R[i] for i in range(0, n, 8))) or 1
    print("L/R correlation %.2f (1 = mono, < 1 = stereo panning)" % (num / den))

    # Spectrum per segment + spectrogram.
    N = 4096
    hop = max(N // 2, (n - N) // 1200 if n > N else N)
    hann = [0.5 - 0.5 * math.cos(2 * math.pi * k / (N - 1)) for k in range(N)]
    kmax = int(a.fmax * N / RATE)
    cols = []
    seg_info = []
    for start in range(0, n - N, hop):
        frame = [(L[start + k] + R[start + k]) * 0.5 * hann[k] for k in range(N)]
        sp = fft(frame)
        mags = [abs(sp[k]) for k in range(kmax)]
        cols.append(mags)
        seg_info.append((start / RATE, mags))
    # Dominant pitch every second.
    print("dominant frequency per second (time: Hz note / spectral centroid Hz):")
    line = []
    last = -1
    for t, mags in seg_info:
        if int(t) == last:
            continue
        last = int(t)
        k = max(range(2, len(mags)), key=lambda k: mags[k])
        f = k * RATE / N
        tot = sum(mags[2:]) or 1
        cen = sum(mags[k2] * k2 * RATE / N for k2 in range(2, len(mags))) / tot
        line.append("%3ds:%5.0f %-3s/%5.0f" % (t, f, note_name(f), cen))
    for i in range(0, len(line), 4):
        print("  " + "   ".join(line[i:i + 4]))

    # Tuning: strong spectral peaks (300-4000 Hz), refined by parabolic
    # interpolation, and their offset from the nearest equal-tempered note.
    # Music played at the right speed clusters near 0 cents; a wrong output
    # rate shifts every note by the same amount.
    cents = []
    for _, mags in seg_info:
        top = max(mags[2:]) or 1
        for k in range(int(300 * N / RATE), min(len(mags) - 1, int(4000 * N / RATE))):
            m = mags[k]
            if m > 0.25 * top and m > mags[k - 1] and m >= mags[k + 1]:
                y0, y1, y2 = math.log(mags[k - 1] + 1e-9), math.log(m), math.log(mags[k + 1] + 1e-9)
                d = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2) if (y0 - 2 * y1 + y2) != 0 else 0
                f = (k + d) * RATE / N
                c = 1200 * math.log2(f / 440.0)
                cents.append(c - 100 * round(c / 100))
    if cents:
        # circular mean of the offsets (they wrap at +-50 cents)
        sx = sum(math.cos(2 * math.pi * c / 100) for c in cents)
        sy = sum(math.sin(2 * math.pi * c / 100) for c in cents)
        mean = 100 * math.atan2(sy, sx) / (2 * math.pi)
        conc = math.hypot(sx, sy) / len(cents)
        print("tuning: %d peaks, mean offset %+.1f cents from A440 equal temperament, concentration %.2f "
              "(near 0 cents and > 0.3: in tune)" % (len(cents), mean, conc))

    if a.png and cols:
        H = 256
        W = len(cols)
        gmax = max(max(c) for c in cols) or 1
        rgb = bytearray(W * H * 3)
        for x, c in enumerate(cols):
            for y in range(H):
                k = int((H - 1 - y) * (len(c) - 1) / (H - 1))
                v = c[k] / gmax
                db = 20 * math.log10(v) if v > 0 else -120
                r, g, b = colormap((db + 80) / 80)
                o = (y * W + x) * 3
                rgb[o:o + 3] = bytes((r, g, b))
        write_png(a.png, W, H, rgb)
        print("spectrogram: %s (%d columns = %.1f s, 0-%d Hz bottom to top)" % (a.png, W, dur, a.fmax))

    print("RESULT: " + ("OK" if not problems else "PROBLEMS: " + ", ".join(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
