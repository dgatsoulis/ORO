# =============================================================================
# thunderprep.py - levels and finishes ORO's sourced thunder set, in place.
#
# The nine files (Thunder_close_1..3 / Thunder_mid_1..3 / Thunder_far_1..3 in
# XRSound\ORO) are freesound recordings picked by Dimitris (2026-08-23; the
# credit ledger is that folder's README.txt). Raw, they differ by up to 13 dB
# within a class and some end abruptly instead of decaying - so the same flash
# would sound wildly different depending on which variant the scheduler rolled.
#
# WHAT IT DOES, per file:
#   1. DC removal (field recordings drift).
#   2. Leading-silence trim to 30 ms (below -50 dBFS) - the strike's DELAY is
#      the scheduler's job (dist/340 s); silence baked into the file would
#      falsify it.
#   3. Loudness normalize to the CLASS target, measured over the LOUDEST 3 s
#      window (whole-file RMS punishes long quiet tails - a 19 s strike+roll
#      would be boosted 9 dB past its siblings).
#      ⚠️ XRSound volume only goes DOWN from 1.0, so files are mastered at the
#      LOUDEST they should ever sound and the scheduler only attenuates:
#         close -13 dBFS   (a strike on top of you - the reference)
#         mid   -18 dBFS
#         far   -23 dBFS   (the distance hierarchy baked into the files;
#                           runtime fine-scales within a class)
#   4. Soft peak limit (linear below 0.70, tanh knee to 0.97) so boosted cracks
#      cannot clip - G9's hard-clamp lesson, audio edition, same as raingen.
#   5. Tail fade: if the final 0.5 s is louder than -50 dBFS, a cosine fade
#      over the last ~1.2 s (Thunder_mid_1 ended mid-roll at -28 dBFS).
#   6. Rewrite 16-bit TPDF-dithered at the file's own sample rate (no resample
#      - 44.1 and 48 kHz both play; resampling buys nothing).
#
# Converges if re-run (gains land at ~1.0). Originals are NOT kept on disk -
# the freesound URLs in the README are the recovery path (the bolt-pack rule:
# raw sources stay out of the tree; only the derived files ship).
# =============================================================================

import wave
from pathlib import Path

import numpy as np

TARGETS_DB = {"close": -13.0, "mid": -18.0, "far": -23.0}
WINDOW_S   = 3.0      # loudness window
LEAD_KEEP  = 0.030    # s of silence left before the onset
FADE_S     = 1.2      # tail fade length
QUIET_DB   = -50.0    # "silence" threshold for trim + tail test
KNEE       = 0.70     # soft limiter knee
CAP        = 0.97


def read_wav(p):
    w = wave.open(str(p))
    ch, sr, sw, n = w.getnchannels(), w.getframerate(), w.getsampwidth(), w.getnframes()
    raw = w.readframes(n)
    w.close()
    if sw == 2:
        x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif sw == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        v = (b[:, 0].astype(np.int32)) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16)
        x = np.where(v >= 1 << 23, v - (1 << 24), v).astype(np.float64) / float(1 << 23)
    else:
        raise RuntimeError(f"{p.name}: unsupported sample width {sw}")
    return x.reshape(-1, ch), sr


def write_wav16(p, x, sr, rng):
    d = rng.uniform(-0.5, 0.5, x.shape) + rng.uniform(-0.5, 0.5, x.shape)
    q = np.clip(np.round(x * 32767.0 + d), -32768, 32767).astype("<i2")
    w = wave.open(str(p), "wb")
    w.setnchannels(x.shape[1])
    w.setsampwidth(2)
    w.setframerate(sr)
    w.writeframes(q.tobytes())
    w.close()


def loudest_window_db(mono, sr):
    win = min(int(WINDOW_S * sr), len(mono))
    hop = max(1, sr // 4)
    best = 0.0
    for i in range(0, len(mono) - win + 1, hop):
        seg = mono[i:i + win]
        best = max(best, float(np.mean(seg * seg)))
    return 10.0 * np.log10(best + 1e-24)


def prep(p, cls, rng):
    x, sr = read_wav(p)
    x -= x.mean(axis=0)                                   # DC

    mono = x.mean(axis=1)
    env = np.abs(mono)
    thr = 10.0 ** (QUIET_DB / 20.0)
    idx = np.where(env > thr)[0]
    if len(idx) and idx[0] > int(LEAD_KEEP * sr):         # leading trim
        x = x[idx[0] - int(LEAD_KEEP * sr):]
        mono = x.mean(axis=1)

    before = loudest_window_db(mono, sr)
    gain = 10.0 ** ((TARGETS_DB[cls] - before) / 20.0)
    x *= gain

    a = np.abs(x)                                         # soft peak limit
    over = a > KNEE
    span = CAP - KNEE
    x[over] = np.sign(x[over]) * (KNEE + span * np.tanh((a[over] - KNEE) / span))

    tail = x.mean(axis=1)[-int(0.5 * sr):]
    tail_db = 10.0 * np.log10(np.mean(tail * tail) + 1e-24)
    faded = ""
    if tail_db > QUIET_DB:                                # tail fade
        nf = min(int(FADE_S * sr), len(x))
        x[-nf:] *= (0.5 * (1.0 + np.cos(np.linspace(0.0, np.pi, nf))))[:, None]
        faded = f", tail faded (was {tail_db:+.0f} dB)"

    write_wav16(p, x, sr, rng)
    peak = 20.0 * np.log10(np.max(np.abs(x)) + 1e-12)
    print(f"  {p.name}: loudest-3s {before:+.1f} -> {TARGETS_DB[cls]:+.1f} dBFS "
          f"(gain {20*np.log10(gain):+.1f} dB), peak {peak:+.1f} dB, "
          f"{len(x)/sr:.2f} s{faded}")


def main():
    snd = Path(__file__).resolve().parents[4] / "XRSound" / "ORO"
    rng = np.random.default_rng(11)
    print(f"thunderprep: {snd}")
    for cls in ("close", "mid", "far"):
        for i in (1, 2, 3):
            p = snd / f"Thunder_{cls}_{i}.wav"
            if p.exists():
                prep(p, cls, rng)
            else:
                print(f"  {p.name}: MISSING")


if __name__ == "__main__":
    main()
