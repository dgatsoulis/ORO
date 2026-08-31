# ============================================================================
# rainmuffle.py - the INTERIOR variants of the storm sounds (2026-08-27).
#
# His spec: inside the cockpit the rain and thunder must be MUFFLED, not merely
# turned down - "the interior of a spacecraft is supposed to be a pressurized
# cabin". XRSound has no runtime filter (volume is its only live control - see
# the module-volume-freeze note), so the muffle is PRE-BAKED: each exterior
# file gains a `_in` twin, low-passed the way a sealed hull actually filters -
# the high-frequency hiss and the thunder CRACK die, the rumble comes through.
#
# THE FILTER IS AN FFT MULTIPLY (zero-phase 4th-order Butterworth magnitude,
# fc 450 Hz), and the choice is load-bearing twice over:
#   - for the LOOPS, circular filtering is SEAMLESS BY CONSTRUCTION - the FFT
#     treats the signal as periodic, which a loop IS. A time-domain IIR would
#     break the loop point with its warm-up transient (the integer-cycle law's
#     audio cousin, again).
#   - zero phase means no group delay, so the `_in` twin stays sample-aligned
#     with its exterior sibling and the runtime crossfade cannot smear.
# One-shots (thunder) are padded a second before filtering so the circular
# wrap-around lands in silence, then trimmed back.
#
# Levels: the twin is normalised to 62% of the original's RMS - part of the
# muffle IS being quieter - with a peak guard. The runtime applies its own
# interior scalar on top (OroRain.cpp).
#
# Run from the Orbiter root:  python Orbitersdk\samples\ORO\tools\rainmuffle.py
# Writes XRSound\ORO\<name>_in.wav beside each source. Idempotent.
# ============================================================================
import struct
import wave
from pathlib import Path

import numpy as np

FC_HZ    = 450.0     # cutoff - keeps thunder's rumble, deletes the crack
ORDER    = 4         # Butterworth magnitude order (24 dB/oct rolloff)
RMS_KEEP = 0.62      # interior loudness relative to the exterior file
PEAK_CAP = 0.98

LOOPS    = ["Rain_light.wav", "Rain_medium.wav", "Rain_heavy.wav"]
ONESHOTS = [f"Thunder_{c}_{i}.wav" for c in ("close", "mid", "far") for i in (1, 2, 3)]
# Rain_hull.wav is deliberately NOT filtered: the taps are ON the hull,
# structure-borne - inside is exactly where they are bright.


def read_wav(p: Path):
    with wave.open(str(p), "rb") as w:
        nch, sw, rate, nfr = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(nfr)
    if sw != 2:
        raise SystemExit(f"{p.name}: expected 16-bit PCM, got {8*sw}-bit")
    x = np.frombuffer(raw, dtype=np.int16).astype(np.float64) / 32768.0
    return x.reshape(-1, nch), rate


def write_wav(p: Path, x: np.ndarray, rate: int):
    y = np.clip(np.round(x * 32767.0), -32768, 32767).astype(np.int16)
    with wave.open(str(p), "wb") as w:
        w.setnchannels(x.shape[1])
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(y.tobytes())


def lowpass(x: np.ndarray, rate: int) -> np.ndarray:
    n = x.shape[0]
    f = np.fft.rfftfreq(n, d=1.0 / rate)
    H = 1.0 / np.sqrt(1.0 + (f / FC_HZ) ** (2 * ORDER))
    return np.fft.irfft(np.fft.rfft(x, axis=0) * H[:, None], n=n, axis=0)


def level(dst: np.ndarray, src: np.ndarray) -> np.ndarray:
    r_src = np.sqrt(np.mean(src ** 2))
    r_dst = np.sqrt(np.mean(dst ** 2))
    if r_dst > 1e-9:
        dst = dst * (RMS_KEEP * r_src / r_dst)
    pk = np.max(np.abs(dst))
    if pk > PEAK_CAP:
        dst = dst * (PEAK_CAP / pk)
    return dst


def main():
    sdir = Path("XRSound") / "ORO"
    if not sdir.is_dir():
        raise SystemExit("run from the Orbiter root (XRSound\\ORO not found)")
    done = 0
    for name in LOOPS + ONESHOTS:
        src = sdir / name
        if not src.exists():
            print(f"  skip (missing): {name}")
            continue
        x, rate = read_wav(src)
        if name in LOOPS:
            y = lowpass(x, rate)                       # circular = seamless loop
        else:
            pad = int(rate)                            # 1 s of wrap room
            xp = np.vstack([x, np.zeros((pad, x.shape[1]))])
            y = lowpass(xp, rate)[: x.shape[0]]
        y = level(y, x)
        out = src.with_name(src.stem + "_in.wav")
        write_wav(out, y, rate)
        print(f"  {out.name}: fc {FC_HZ:.0f} Hz, rms {RMS_KEEP:.2f}x")
        done += 1
    print(f"{done} interior variant(s) written.")


if __name__ == "__main__":
    main()
