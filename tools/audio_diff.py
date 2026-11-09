#!/usr/bin/env python3
"""What happened to a movie's sound on the way to the speakers.

    tools/audio_diff.py <captured.wav> <movie.bik> [--from SECONDS]

Decodes the movie's own soundtrack with FFmpeg (the reference) and follows it
through the captured output (make record's audio.wav, or a RECOMP_AUDIO_DUMP),
10 ms block by block: for each captured block, where in the reference it came
from. Played correctly, that position advances 10 ms per block. The report
names what went wrong, and when:

    hole     captured silence where the reference has sound (starved output)
    skip     the reference jumped forward (sound lost)
    repeat   the reference jumped back (sound played twice)
    drift    the long-run rate (reference ms per captured second); 1000 = right

Needs numpy and ffmpeg.
"""
import argparse
import subprocess
import sys
import wave

import numpy as np

RATE = 48000
BLOCK = 480                     # 10 ms
SEARCH = 4800                   # look +-100 ms around the expected position


def load_wav(path):
    with wave.open(path) as w:
        if w.getframerate() != RATE or w.getnchannels() != 2:
            sys.exit(f"{path}: want 48 kHz stereo")
        a = np.frombuffer(w.readframes(w.getnframes()), np.int16)
    return a.reshape(-1, 2).astype(np.float32).mean(axis=1)


def load_bik(path):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-vn", "-f", "s16le",
                          "-ac", "2", "-ar", str(RATE), "-"],
                         check=True, capture_output=True).stdout
    return np.frombuffer(raw, np.int16).reshape(-1, 2).astype(np.float32).mean(axis=1)


def xcorr(a, b):
    """Normalised cross-correlation of b slid over a: (best lag, score)."""
    n = len(a) + len(b)
    size = 1 << (n - 1).bit_length()
    a = a.astype(np.float64)
    b = b.astype(np.float64)
    c = np.fft.irfft(np.fft.rfft(a, size) * np.conj(np.fft.rfft(b, size)), size)[:len(a) - len(b) + 1]
    sq = np.concatenate(([0.0], np.cumsum(a * a)))
    win = sq[len(b):] - sq[:-len(b)]
    bb = (b * b).sum()
    # Silent stretches of `a` match nothing (their score would be noise / ~0).
    c = np.where(win > 1e-3 * bb, c / (np.sqrt(np.maximum(win, 1e-9)) * np.sqrt(bb) + 1e-9), 0.0)
    i = int(np.argmax(c))
    return i, float(c[i])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("movie")
    ap.add_argument("--from", dest="start", type=float, default=0.0,
                    help="skip this many seconds of the capture first")
    ap.add_argument("--events", type=int, default=25, help="events to list")
    args = ap.parse_args()

    cap = load_wav(args.capture)
    ref = load_bik(args.movie)
    loud = np.abs(ref).max() * 0.02

    # Where the movie starts: the reference's first loud 100 ms (short: the
    # capture may already have holes within half a second), found
    # anywhere in the capture after --from.
    r0 = int(np.argmax(np.abs(ref) > loud))
    probe = ref[r0:r0 + RATE // 10]
    c0 = int(args.start * RATE)
    lag, score = xcorr(cap[c0:], probe)
    if score < 0.5:
        sys.exit(f"movie not found in the capture (best match {score:.2f})")
    c = c0 + lag                     # capture sample that holds reference sample r0
    r = r0
    print(f"movie sound starts at {c / RATE:.2f} s of the capture (match {score:.2f})")

    events, holes, skips, repeats, blocks = [], 0, 0, 0, 0
    first_c, first_r = c, r
    while c + BLOCK <= len(cap) and r + BLOCK <= len(ref):
        blk = cap[c:c + BLOCK]
        refblk = ref[r:r + BLOCK]
        blocks += 1
        if np.abs(blk).max() == 0 and np.abs(refblk).max() > loud:
            # Silence inserted: the reference position stays put.
            holes += 1
            if not events or events[-1][1] != "hole" or events[-1][0] < c / RATE - 0.011:
                events.append([c / RATE, "hole", 10.0])
            else:
                events[-1][2] += 10.0
            c += BLOCK
            continue
        lo = max(0, r - SEARCH)
        win = ref[lo:r + BLOCK + SEARCH]
        if np.abs(blk).max() < loud or len(win) < BLOCK:
            c += BLOCK
            r += BLOCK              # too quiet to place: assume it played on
            continue
        lag, score = xcorr(win, blk)
        found = lo + lag
        if score > 0.8 and found != r:
            d = (found - r) / RATE * 1000
            if abs(d) >= 1.0:
                kind = "skip" if d > 0 else "repeat"
                skips += d > 0
                repeats += d < 0
                events.append([c / RATE, kind, abs(d)])
            r = found
        c += BLOCK
        r += BLOCK

    secs = (c - first_c) / RATE
    print(f"followed {secs:.1f} s of capture = {(r - first_r) / RATE:.1f} s of the movie's sound"
          f"  -> drift {1000 * (r - first_r) / max(1, c - first_c):.1f} ref ms per captured s")
    print(f"holes: {holes} blocks ({holes * 10} ms) | skips: {skips} | repeats: {repeats}")
    for t, kind, ms in events[:args.events]:
        print(f"  {t:8.3f} s  {kind:6s} {ms:6.1f} ms")
    if len(events) > args.events:
        print(f"  ... {len(events) - args.events} more")


if __name__ == "__main__":
    main()
