# =============================================================================
# raingen.py - synthesizes ORO's three rain loops (the bellgen/boltatlas pattern:
# a repo tool that writes runtime assets, so the shipped files are re-derivable).
#
#   Rain_light.wav   - drizzle: sparse discrete patter over a thin hiss
#   Rain_medium.wav  - steady rain: dense patter fusing into a full wash
#   Rain_heavy.wav   - downpour: roaring wash with low-mid body, splatter on top
#
# 44100 Hz, 16-bit stereo. All three are mastered to EQUAL RMS (-20 dBFS) so the
# runtime crossfade weights in UpdateRainSound() ARE the mix - the loop files
# carry timbre only, never loudness policy.
#
# WHY GENERATED RATHER THAN RECORDED (2026-08-23, his call): rain is acoustically
# filtered noise plus drop transients - the one sound category where synthesis is
# not a compromise - and generating buys three things a recording cannot:
#   1. SEAMLESS LOOPS BY CONSTRUCTION. The wash is an inverse FFT of a shaped
#      magnitude spectrum with random phases, which is EXACTLY periodic over the
#      loop length - the integer-cycle law (invariant 28e), audio edition. The
#      slow gust undulation is built from sinusoids with INTEGER cycles per loop,
#      and drop transients that spill past the end WRAP to the start. There is no
#      loop seam anywhere because nothing in the file knows where the ends are.
#   2. MATCHED INTENSITY TIERS. Three loops cut from the same synthesis crossfade
#      cleanly; three recordings from three storms do not.
#   3. NO LICENCE. Fully ours; ships in repo and zip. (Thunder is the opposite
#      call - a synthesized crack reads as "a boom" - so thunder is SOURCED, from
#      freesound with redistribution-clean licences. The split is deliberate.)
#
# Deterministic: fixed per-tier seeds, so re-running reproduces the shipped wavs
# bit for bit. Tuning a tier = editing its TIERS entry and re-running.
# =============================================================================

import argparse
import struct
import wave
from pathlib import Path

import numpy as np

SR = 44100

# ---------------------------------------------------------------------------
# The tier tables - the whole look (sound) of each loop lives here.
# Spectrum points are (Hz, dB) shape controls for the wash, log-log interpolated;
# only the SHAPE matters (mastering renormalizes), plus the wash/patter gains.
# Rates are events per second; the gust modulation thins/densifies them live.
#
# ROUND 2 (2026-08-23, his verdict on round 1: "too hissing"): the energy moved
# DOWN. Round 1's plateaus sat at 4.5-8 kHz, which is the spectrum of hiss, not
# of rain heard in the world - real recordings carry their body at 200-1500 Hz
# (aggregate impact roar, ground reflection) and roll the top off with air
# absorption. Everything above 5 kHz dropped 5-10 dB, the low-mid rose, the
# tick band lowered, the splat tilt softened. Loudness is untouched (mastering
# renormalizes) - only the timbre darkened.
# ---------------------------------------------------------------------------
TIERS = {
    "light": dict(
        seed=101, dur=16.0,          # longest loop: sparse patter is where a
                                     # repeating pattern would be recognized
        spectrum=[(30, -52), (100, -40), (300, -28), (800, -20), (1500, -17),
                  (3000, -16), (5000, -18), (8000, -23), (12000, -30),
                  (16000, -38), (20000, -46)],
        wash=0.30,                   # thin wash; the patter carries the tier
        gust_depth=0.20,             # drizzle audibly comes and goes in waves
        tick_rate=38.0,  tick_gain=1.00,
        blop_rate=1.4,   blop_gain=0.90,
        splat_rate=0.0,  splat_gain=0.0,
    ),
    "medium": dict(
        seed=202, dur=12.0,
        spectrum=[(30, -38), (100, -26), (300, -16), (800, -10), (1500, -8),
                  (3000, -8), (5000, -11), (8000, -16), (12000, -23),
                  (16000, -31), (20000, -39)],
        wash=0.85,
        gust_depth=0.15,
        tick_rate=240.0, tick_gain=0.70,
        blop_rate=2.2,   blop_gain=0.80,
        splat_rate=45.0, splat_gain=0.55,
    ),
    "heavy": dict(
        seed=303, dur=12.0,
        spectrum=[(30, -22), (80, -14), (200, -8), (500, -5), (1200, -4),
                  (2500, -5), (5000, -8), (8000, -13), (12000, -19),
                  (16000, -26), (20000, -33)],
        wash=1.00,                   # the roar dominates; patter is fused texture
        gust_depth=0.10,             # a downpour is closer to a steady wall
        tick_rate=650.0, tick_gain=0.45,
        blop_rate=2.8,   blop_gain=0.60,
        splat_rate=230.0, splat_gain=0.70,
    ),
    # THE HULL TAPS (2026-08-23, his ask after the first VC flight: "metal taps").
    # Heard ONLY inside (UpdateRainSound's interior branch): drops drumming the
    # skin - a low membrane thump with a fast contact tick, over a faint low bed.
    # The exterior storm loops drop to 45% inside; this is the sound that takes
    # their place, because the hull is the instrument the rain is playing.
    "hull": dict(
        seed=404, dur=12.0,
        spectrum=[(30, -34), (80, -26), (200, -24), (500, -27), (1200, -31),
                  (3000, -37), (8000, -46), (16000, -58), (20000, -64)],
        wash=0.18, gust_depth=0.18,
        tick_rate=0.0,  tick_gain=0.0,
        blop_rate=0.0,  blop_gain=0.0,
        splat_rate=0.0, splat_gain=0.0,
        tap_rate=13.0,  tap_gain=1.00,
    ),
}

TARGET_RMS_DB = -20.0    # every tier lands here; runtime weights own loudness
SOFT_KNEE     = 0.60     # soft ceiling: linear below, tanh knee above, cap ~0.97


# ---------------------------------------------------------------------------
# The wash: exact-period noise from a shaped spectrum. Independent phases per
# channel = decorrelated L/R = the wide diffuse image real rain has.
# ---------------------------------------------------------------------------
def spectral_wash(n, points, rng):
    nb = n // 2 + 1
    freqs = np.fft.rfftfreq(n, 1.0 / SR)
    fh = np.array([p[0] for p in points], dtype=np.float64)
    db = np.array([p[1] for p in points], dtype=np.float64)
    lf = np.log10(np.maximum(freqs, 1e-3))
    mag_db = np.interp(lf, np.log10(fh), db)         # log-f, linear-dB interp
    mag = 10.0 ** (mag_db / 20.0)
    mag[0] = 0.0                                      # no DC
    out = np.empty((n, 2))
    for ch in range(2):
        phase = rng.uniform(0.0, 2.0 * np.pi, nb)
        phase[0] = 0.0
        phase[-1] = 0.0                               # Nyquist bin must be real
        spec = mag * np.exp(1j * phase)
        x = np.fft.irfft(spec, n)
        out[:, ch] = x / (np.sqrt(np.mean(x * x)) + 1e-12)   # unit RMS per channel
    return out


# ---------------------------------------------------------------------------
# The gust undulation: integer cycles per loop => periodic by construction.
# Shared by the wash amplitude AND the patter density, because a gust drives
# both - two independent clocks would give rain that thickens while quieting,
# which nothing in weather does (the vapour cone's one-number rule, 25j).
# ---------------------------------------------------------------------------
def gust_curve(n, depth, rng):
    t = np.arange(n) / n                              # 0..1 over the loop
    m = np.zeros(n)
    for k, w in ((1, 1.0), (2, 0.6), (3, 0.35)):      # 3 integer harmonics
        m += w * np.sin(2.0 * np.pi * (k * t + rng.uniform()))
    m /= np.max(np.abs(m)) + 1e-12
    return 1.0 + depth * m                            # >0 for any depth < 1


# --- drop grains -----------------------------------------------------------
def grain_tick(rng):
    # A drop on a hard surface is a broadband "tick": a damped sinusoid (the
    # impulse response of a bandpass) too short to register pitch, plus a whiff
    # of noise so it never reads as a pure tone. Band lowered in round 2 - the
    # 8 kHz top end of round 1 read as sizzle, not patter.
    fc = np.exp(rng.uniform(np.log(1200.0), np.log(5200.0)))
    tau = rng.uniform(0.0005, 0.0020)
    n = max(8, int(6.0 * tau * SR))
    t = np.arange(n) / SR
    env = np.exp(-t / tau)
    g = 0.75 * np.sin(2.0 * np.pi * fc * t + rng.uniform(0, 2 * np.pi)) * env
    g += 0.25 * rng.standard_normal(n) * env
    return g

def grain_blop(rng):
    # The classic drip: a low chirped sine, pitch falling ~25% across its life.
    # These DO have pitch, and should - they are the singles you pick out of
    # light rain.
    f0 = np.exp(rng.uniform(np.log(380.0), np.log(950.0)))
    tau = rng.uniform(0.0035, 0.008)
    n = max(16, int(6.0 * tau * SR))
    t = np.arange(n) / SR
    f = f0 * (1.0 - 0.25 * t / t[-1])
    ph = 2.0 * np.pi * np.cumsum(f) / SR
    return np.sin(ph + rng.uniform(0, 2 * np.pi)) * np.exp(-t / tau)

def grain_tap(rng):
    # A drop on aluminum skin, heard from inside: the panel answers as a small
    # membrane - a low thump ringing out over ~10 ms - topped with the brief
    # bright tick of the contact itself.
    f0 = np.exp(rng.uniform(np.log(150.0), np.log(420.0)))
    tau = rng.uniform(0.008, 0.018)
    n = max(32, int(6.0 * tau * SR))
    t = np.arange(n) / SR
    g = np.sin(2.0 * np.pi * f0 * t + rng.uniform(0, 2 * np.pi)) * np.exp(-t / tau)
    ftick = np.exp(rng.uniform(np.log(1200.0), np.log(2800.0)))
    g += 0.5 * np.sin(2.0 * np.pi * ftick * t) * np.exp(-t / rng.uniform(0.001, 0.003))
    return g


def grain_splat(rng):
    # Heavy-rain splatter: a tilted noise burst with a fast decay - the crush
    # of water hitting water. Round 1 used a full first difference (+6 dB/oct),
    # which was half the hiss; the tilt is milder now.
    dur = rng.uniform(0.0025, 0.007)
    n = max(16, int(dur * SR))
    t = np.arange(n) / SR
    x = rng.standard_normal(n + 1)
    x = x[1:] - 0.45 * x[:-1]        # gentle highpass, not a razor
    return x * np.exp(-t / (dur / 3.0))


def scatter(buf, n_total, rate, dur, gust, gain, maker, rng):
    # Poisson events on the CIRCLE: times uniform, thinned by the gust curve
    # (patter responds a bit harder than the wash: ^1.5), tails wrapping past
    # the loop end back to the start. Amplitudes heavy-tailed (u^2.4): many
    # quiet drops, few loud ones - the real distribution.
    if rate <= 0.0 or gain <= 0.0:
        return 0
    gp = gust ** 1.5
    gmax = np.max(gp)
    count = 0
    for _ in range(int(rate * dur * gmax + 0.5)):
        t0 = rng.uniform(0.0, dur)
        i0 = int(t0 * SR) % n_total
        if rng.uniform() * gmax > gp[i0]:
            continue
        g = maker(rng) * (gain * rng.uniform() ** 2.4)
        p = rng.uniform(-0.85, 0.85)                  # equal-power pan
        gl, gr = np.sqrt((1.0 - p) * 0.5), np.sqrt((1.0 + p) * 0.5)
        ln = len(g)
        if i0 + ln <= n_total:
            buf[i0:i0 + ln, 0] += g * gl
            buf[i0:i0 + ln, 1] += g * gr
        else:
            k = n_total - i0                          # the wrap - seamless drops
            buf[i0:, 0] += g[:k] * gl
            buf[i0:, 1] += g[:k] * gr
            buf[:ln - k, 0] += g[k:] * gl
            buf[:ln - k, 1] += g[k:] * gr
        count += 1
    return count


def master(x):
    # Equal-RMS across tiers, then a soft ceiling: linear below the knee, tanh
    # above it, asymptote just under full scale. Shaves only the rare loudest
    # drop peaks - G9's lesson that a HARD clamp flattens texture applies to
    # audio exactly as it did to Gouraud alpha.
    rms = np.sqrt(np.mean(x * x))
    x = x * (10.0 ** (TARGET_RMS_DB / 20.0) / (rms + 1e-12))
    a = np.abs(x)
    over = a > SOFT_KNEE
    span = 1.0 - SOFT_KNEE - 0.03
    x[over] = np.sign(x[over]) * (SOFT_KNEE + span * np.tanh((a[over] - SOFT_KNEE) / span))
    return x


def write_wav(path, x, rng):
    # 16-bit PCM with 1-LSB TPDF dither (correct practice; for rain it is
    # academic, but it costs one line).
    d = (rng.uniform(-0.5, 0.5, x.shape) + rng.uniform(-0.5, 0.5, x.shape))
    q = np.clip(np.round(x * 32767.0 + d), -32768, 32767).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(q.tobytes())


def build_tier(name, p, out_dir):
    rng = np.random.default_rng(p["seed"])
    n = int(p["dur"] * SR)

    gust = gust_curve(n, p["gust_depth"], rng)
    buf = spectral_wash(n, p["spectrum"], rng) * (p["wash"] * gust[:, None])

    nt = scatter(buf, n, p["tick_rate"],  p["dur"], gust, p["tick_gain"],  grain_tick,  rng)
    nb = scatter(buf, n, p["blop_rate"],  p["dur"], gust, p["blop_gain"],  grain_blop,  rng)
    ns = scatter(buf, n, p["splat_rate"], p["dur"], gust, p["splat_gain"], grain_splat, rng)
    ntp = scatter(buf, n, p.get("tap_rate", 0.0), p["dur"], gust,
                  p.get("tap_gain", 0.0), grain_tap, rng)

    buf = master(buf)

    path = out_dir / f"Rain_{name}.wav"
    write_wav(path, buf, rng)

    # -- verification (a generated asset gets checked, not assumed) --
    rms = 20 * np.log10(np.sqrt(np.mean(buf * buf)))
    peak = 20 * np.log10(np.max(np.abs(buf)) + 1e-12)
    # seam: the wrap-around step vs the typical successive-sample step. Same
    # order of magnitude = no click at the loop point.
    step_typ = np.mean(np.abs(np.diff(buf[:, 0])))
    step_seam = abs(buf[-1, 0] - buf[0, 0])
    mag = np.abs(np.fft.rfft(buf[:, 0]))
    fr = np.fft.rfftfreq(n, 1.0 / SR)
    centroid = float(np.sum(fr * mag) / np.sum(mag))
    print(f"  {path.name}: {p['dur']:.0f} s, RMS {rms:+.1f} dBFS, peak {peak:+.1f} dB, "
          f"centroid {centroid:.0f} Hz")
    print(f"    drops: {nt} ticks, {nb} blops, {ns} splats, {ntp} taps | "
          f"seam step {step_seam:.5f} vs typical {step_typ:.5f} "
          f"({'OK' if step_seam < 6 * step_typ else 'CHECK'})")
    return path


def main():
    # XRSound\ORO is where ORO sounds live (the Orbiter convention - textures in
    # Textures\, meshes in Meshes\, sounds under XRSound\<addon>\; his call 2026-08-23).
    default_out = Path(__file__).resolve().parents[4] / "XRSound" / "ORO"
    ap = argparse.ArgumentParser(description="Generate ORO's three seamless rain loops.")
    ap.add_argument("--out", type=Path, default=default_out,
                    help=f"output folder (default: {default_out})")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    print(f"raingen: {SR} Hz stereo 16-bit -> {args.out}")
    for name, p in TIERS.items():
        build_tier(name, p, args.out)
    print("done. Loops are exactly periodic - no seam editing needed, ever.")


if __name__ == "__main__":
    main()
