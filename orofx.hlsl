// ==============================================================
// orofx.hlsl
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - image-space (frame-resampling) effects, run through D3D9Client's
// gcIPInterface (Image Processing Interface). This is the "premium" pipeline:
// the module hands the client a COPY of the live backbuffer as a texture, the
// shader below transforms it, and the result is written back over the frame.
//
// The VERTEX shader is NOT here - ImageProcessing always compiles its vertex
// stage from the stock "Modules/D3D9Client/IPI.hlsl" (VSMain), which emits a
// full-screen quad and feeds each pixel its texture coordinate as two scalars
// (x -> TEXCOORD0, y -> TEXCOORD1). This file supplies PIXEL shaders only.
//
// One PS entry point per effect; ORO compiles each into its own gcIPInterface.
//   sampler tSrc  - the captured frame (a render-target-texture copy of the
//                   backbuffer); sampled clamped + linear by the host.
// Output alpha = 1 (each pass OVERWRITES the backbuffer, blendop = 0).
//
// This file is RECOMPILED from disk on every session start - tune the constants
// below and just reload the scenario, no DLL rebuild.
// ============================================================================

sampler tSrc;

// ----------------------------------------------------------------------------
// GREY-OUT (physiological): under sustained +Gz, colour vision fades toward
// monochrome before the black-out closes in. PURE desaturation - collapses each
// pixel toward its own luminance, brightness preserved (darkening is the
// black-out overlay's job, layered on top separately).
// fGrey: 0 = full colour, 1 = fully monochrome.
// ----------------------------------------------------------------------------
uniform extern float fGrey;

float4 PSGrey(float x : TEXCOORD0, float y : TEXCOORD1) : COLOR
{
	float4 c   = tex2D(tSrc, float2(x, y));
	float  lum = dot(c.rgb, float3(0.299f, 0.587f, 0.114f));   // Rec.601 luma
	float3 rgb = lerp(c.rgb, lum.xxx, saturate(fGrey));        // desaturate only
	return float4(rgb, 1.0f);
}

// ----------------------------------------------------------------------------
// BLUR (physiological): vision softens and smears under G stress. A SEPARABLE
// Gaussian - ORO runs this pass TWICE (horizontal, then vertical, feeding the
// H result back in as tSrc). vBlurStep is the one-texel UV step along the
// current axis (host sets {1/w,0} then {0,1/h}); fBlur 0..1 scales the spread.
// 17-tap Gaussian (weights sum to 1). BLUR_MAX_SPAN / tap count are tuned here.
// ----------------------------------------------------------------------------
uniform extern float2 vBlurStep;   // one-texel UV step along the blur axis
uniform extern float  fBlur;       // 0..1 blur amount

#define BLUR_TAPS     8            // samples each side (2*TAPS+1 = 17 total)
#define BLUR_MAX_SPAN 3.5f         // outermost tap lands TAPS*SPAN texels out at fBlur=1

// sigma ~4 tap-units: wide, heavy tails so the extra reach actually reads as a
// stronger blur (a tight sigma would make the outer taps ~0 and look unchanged).
static const float BLUR_W[9] = {
	0.10316f, 0.09998f, 0.09104f, 0.07786f, 0.06256f, 0.04723f, 0.03350f, 0.02229f, 0.01396f
};

float4 PSBlur(float x : TEXCOORD0, float y : TEXCOORD1) : COLOR
{
	float2 uv   = float2(x, y);
	float2 step = vBlurStep * (saturate(fBlur) * BLUR_MAX_SPAN);
	float3 sum  = tex2D(tSrc, uv).rgb * BLUR_W[0];
	[unroll] for (int i = 1; i <= BLUR_TAPS; i++) {
		float2 o = step * i;
		sum += tex2D(tSrc, uv + o).rgb * BLUR_W[i];
		sum += tex2D(tSrc, uv - o).rgb * BLUR_W[i];
	}
	return float4(sum, 1.0f);
}

// ----------------------------------------------------------------------------
// CHROMATIC ABERRATION (physiological/cinematic): the RGB channels separate
// radially, worsening toward the periphery - the ocular "lens" distorting under
// load. Offset grows linearly with distance from centre (clean centre, strong
// edges): red sampled further OUT, blue further IN, green stays put.
// fChroma 0..1 scales it. CHROMA_MAX tuned here (hot-reloads from file).
// ----------------------------------------------------------------------------
uniform extern float fChroma;      // 0..1 aberration strength

#define CHROMA_MAX 0.020f          // per-channel UV shift at the screen corner (radius ~0.7)

float4 PSChroma(float x : TEXCOORD0, float y : TEXCOORD1) : COLOR
{
	float2 uv  = float2(x, y);
	float2 off = (uv - 0.5f) * (saturate(fChroma) * CHROMA_MAX);  // radial, 0 at centre
	float r = tex2D(tSrc, uv + off).r;   // red pushed outward
	float g = tex2D(tSrc, uv).g;         // green reference
	float b = tex2D(tSrc, uv - off).b;   // blue pulled inward
	return float4(r, g, b, 1.0f);
}

// ----------------------------------------------------------------------------
// PERIPHERAL SWIM (physiological): near G-LOC / disorientation the visual field
// "swims" - a slow woozy warp, strongest in the PERIPHERY (central vision holds
// longest). A time-animated sum-of-sines UV displacement, weighted to the edges
// so the centre stays stable. fSwim 0..1 strength; fTime = seconds (animation).
// ----------------------------------------------------------------------------
uniform extern float fSwim;        // 0..1 swim strength
uniform extern float fTime;        // seconds, drives the wobble

#define SWIM_MAX 0.008f            // max peripheral UV displacement at fSwim=1

float4 PSSwim(float x : TEXCOORD0, float y : TEXCOORD1) : COLOR
{
	float2 uv = float2(x, y);
	float  w  = saturate(length(uv - 0.5f) * 2.0f);   // ~0 centre, ~1 toward the edges
	float2 disp;
	disp.x = sin(uv.y * 9.0f + fTime * 1.7f) + 0.5f * sin(uv.y * 17.0f - fTime * 2.3f);
	disp.y = cos(uv.x * 8.0f + fTime * 1.9f) + 0.5f * cos(uv.x * 15.0f - fTime * 2.1f);
	float2 off = disp * (saturate(fSwim) * SWIM_MAX * w);
	return tex2D(tSrc, uv + off);
}

// ----------------------------------------------------------------------------
// TILT / SWAY (roll): the field rocks about its centre - a woozy head/vestibular
// SWAY (the "leans") under G / disorientation. A true (aspect-corrected) rotation
// so the horizon stays straight-not-skewed; the angle OSCILLATES slowly (a slow
// rock + a weak second harmonic so it's not a metronome). The zoom breathes with
// the sway so there's no crop as it passes through level. fTilt 0..1 = sway
// amplitude; fAspect = viewport w/h; fTime = seconds. (Physics phase: a signed
// steady lean from lateral G can be added on top of this sway.)
// ----------------------------------------------------------------------------
uniform extern float fTilt;        // 0..1 sway amplitude
uniform extern float fAspect;      // viewport width / height
uniform extern float fLean;        // -1..+1 SIGNED steady lean (felt-G model: lateral G
                                   // lolls the head, so the horizon rolls with it). The
                                   // lab slider and the scenarios never write this.

#define TILT_MAX  0.28f            // roll angle (rad) at the sway extremes (~16 deg)
#define LEAN_MAX  0.35f            // roll angle (rad) at |fLean| = 1 (~20 deg)
#define TILT_ZOOM 0.22f            // zoom-in at the extremes, keeps rolled corners covered
#define SWAY_FREQ 1.0f            // rad/s - a slow, disorienting rock

float4 PSTilt(float x : TEXCOORD0, float y : TEXCOORD1) : COLOR
{
	float f    = saturate(fTilt);
	float ln   = clamp(fLean, -1.0f, 1.0f);
	float sway = 0.85f * sin(fTime * SWAY_FREQ) + 0.15f * sin(fTime * SWAY_FREQ * 2.3f + 1.0f);
	float a    = f * TILT_MAX * sway + ln * LEAN_MAX;           // rocking sway + steady lean
	float2 c = float2(x, y) - 0.5f;
	c.x *= fAspect;                                              // -> aspect-correct (square) space
	float  s = sin(a), co = cos(a);
	float2 r = float2(c.x * co - c.y * s, c.x * s + c.y * co);   // roll about centre
	r.x /= fAspect;                                             // -> back to UV space
	r /= (1.0f + TILT_ZOOM * (f * abs(sway) + abs(ln)));        // zoom covers BOTH contributions
	return tex2D(tSrc, r + 0.5f);
}

// ----------------------------------------------------------------------------
// EXHAUST SHIMMER (environmental, NOT physiological): hot engine exhaust in
// atmosphere bends the light passing through it - the view BEHIND the plume
// ripples. A screen-space refraction: the host projects each engine plume to a
// screen-space CAPSULE (root a -> tip b, radius) and we offset the sampling UV
// with animated turbulence, weighted by nearness to the plume axis.
//   vPlume[i]  = (ax, ay, bx, by)  plume axis in UV
//   vPlumeP[i] = (radius_uv, strength, unused, unused)
//   fShimmer   = 0..1 master strength (the dialog slider)
// Deliberately placed LAST in this file (not first in the render order) because
// it reuses fTime / fAspect, which are declared above - HLSL globals must be
// declared before use, and the whole file compiles as ONE unit per entry point.
// PLUME_N must match OroModule.h MAX_PLUMES. Inactive plumes carry strength 0
// (a constant loop count keeps the shader SM3-safe - no dynamic branching).
// ----------------------------------------------------------------------------
uniform extern float  fShimmer;    // 0..1 master strength
uniform extern float4 vPlume[6];   // plume axes  (ax,ay,bx,by) in UV
uniform extern float4 vPlumeP[6];  // plume params (radius_uv, strength, -, -)

#define PLUME_N     6
#define SHIMMER_MAX 0.010f         // max UV displacement at full strength

float4 PSShimmer(float x : TEXCOORD0, float y : TEXCOORD1) : COLOR
{
	float2 uv   = float2(x, y);
	float2 disp = 0.0f;

	[unroll] for (int i = 0; i < PLUME_N; i++)
	{
		float2 a = vPlume[i].xy;
		float2 b = vPlume[i].zw;
		float  r  = vPlumeP[i].x;
		float  s  = vPlumeP[i].y;                      // 0 for unused slots -> contributes nothing
		float  hp = vPlumeP[i].z;                      // where turbulence peaks along the plume

		// Nearest point on the plume axis (capsule distance), aspect-corrected so the
		// haze envelope is round on screen rather than stretched horizontally.
		float2 ba = b - a;
		float  h  = saturate(dot(uv - a, ba) / max(dot(ba, ba), 1e-6f));
		float2 cl = a + ba * h;
		float  d  = length((uv - cl) * float2(fAspect, 1.0f));

		// Envelope: quadratic falloff out to the radius, times a soft BUMP along the plume
		// centred at hp - the turbulent mixing that bends the light peaks somewhere down
		// the plume, and that point migrates aft as thrust rises (host sets hp).
		float w    = saturate(1.0f - d / max(r, 1e-6f));
		float bump = saturate(1.0f - abs(h - hp) / 0.75f);
		w = w * w * bump * bump * s;

		// Turbulence: two octaves per axis, scrolling ALONG the plume (h) so the ripple
		// visibly streams aft instead of shimmering in place. Phase-offset per plume.
		float ph = fTime * 9.0f + h * 26.0f + (float)i * 2.3f;
		float n1 = sin(ph)               + 0.5f * sin(ph * 2.7f + uv.y * 90.0f);
		float n2 = cos(ph * 1.13f + 1.7f) + 0.5f * cos(ph * 2.3f + uv.x * 80.0f);

		disp += float2(n1, n2) * w;
	}

	disp *= SHIMMER_MAX * saturate(fShimmer);
	return tex2D(tSrc, uv + disp);
}

// ----------------------------------------------------------------------------
// COCKPIT PLASMA GLOW (reentry, INTERNAL view) - the reentry effect's second half.
//
// WHY THIS IS A SHADER AND NOT A LIGHT. The obvious implementation is an Orbiter
// point light at the stagnation point set VIS_ALWAYS, and it does technically
// light the VC. It was built, tested and REJECTED on sight (2026-08-01): Orbiter's
// local lights have NO OCCLUSION, so with a spaceplane at its usual ~40 deg
// reentry AoA the plasma sits under the BELLY and lights the cabin up through the
// floor. Real light only reaches the cockpit through the windows. No amount of
// repositioning fixes that, because the mechanism is wrong - so the hull keeps its
// point light (external only) and the cabin gets this instead.
//
// Two parts: a broad directional BLOOM centred where the plasma actually projects
// on screen, plus a small uniform lift so the whole cabin warms. The host projects
// the stagnation point on the main thread (invariant 1) and hands us the UV; when
// the plasma is behind the camera it parks the centre off-screen and only the
// uniform lift survives, which is exactly right - the cabin still glows, but there
// is no hot spot in front of you.
//
// Placed last: reuses fAspect, declared far above. Additive, so it must run AFTER
// the resample stack and before the physiological washes darken the frame.
// ----------------------------------------------------------------------------
uniform extern float  fPlasma;     // 0..1 glow intensity (heat x dialog trim)
uniform extern float2 vPlasmaUV;   // where the plasma projects, in UV (may be off-screen)
uniform extern float3 vPlasmaCol;  // plasma colour for this heat band

// Tuned down 2026-08-01: the first values (0.85 / 0.16) blew the VC out at 90% heat -
// panel labels were washing away and the cabin read as overexposed rather than lit.
// Roughly halved, and the glow now RESPECTS what it is lighting: it scales with the
// surface it lands on, so dark corners stay dark instead of everything flooding to white.
#define PLASMA_SPREAD 1.30f        // bloom falloff - lower = broader wash
#define PLASMA_BLOOM  0.42f        // directional component at full heat
#define PLASMA_AMB    0.07f        // uniform cabin lift at full heat
#define PLASMA_FLOOR  0.35f        // how much glow reaches an unlit surface (rest is modulated)

float4 PSPlasma(float x : TEXCOORD0, float y : TEXCOORD1) : COLOR
{
	float4 src = tex2D(tSrc, float2(x, y));
	float2 d   = float2(x, y) - vPlasmaUV;
	d.x *= fAspect;                                  // circular in SCREEN space, not UV space
	float  r    = length(d);
	float  glow = exp(-r * r * PLASMA_SPREAD) * PLASMA_BLOOM + PLASMA_AMB;
	float  k    = saturate(fPlasma) * glow;
	// Light falling on a surface reveals what is already there - a purely additive wash
	// crushes everything toward white and eats the panel text. Mix a flat term with one
	// that scales by local luminance so bright surfaces catch the light and dark ones do
	// not. Cheap stand-in for the diffuse response we have no geometry to compute.
	float  lum  = dot(src.rgb, float3(0.299f, 0.587f, 0.114f));
	float  resp = PLASMA_FLOOR + (1.0f - PLASMA_FLOOR) * lum;
	return float4(saturate(src.rgb + vPlasmaCol * (k * resp)), src.a);
}

// ----------------------------------------------------------------------------
// ECLIPSE - the observer inside another body's shadow. The LAST shader added, and
// the only one whose entire job is to model an instrument the renderer does not
// have: the eye. The host (OroEclipse.cpp) computes what fraction of the solar
// disc is covered at the camera and runs the adaptation model; all that arrives
// here is the result.
//
// THREE things happen, in this order, and the order is physiological:
//   1. COLOUR LOSS. Below cone threshold the rods take over and they carry no
//      colour at all. They also peak blue-green (~507 nm), so what is left is not
//      a neutral grey - it is the cool grey of the Purkinje shift, which is why
//      moonlight photographs blue. Done FIRST, because it happens in the retina
//      before any gain is applied.
//   2. GAIN. One multiplier, straight from the host's lit/adapted ratio. Below 1
//      going into shadow, above 1 coming out. Uniform across the field, because
//      adaptation IS uniform - the one exception is below.
//   3. GLARE. A gain above 1 alone just clips to white, which reads as a flat
//      wash rather than as being dazzled. A small additive veil on top of the
//      multiply gives it the flooded look, and it lives and dies with the same
//      1.2 s light-adaptation constant that produced it.
//
// HIGHLIGHT PROTECTION (fEclProt) is the one deliberate lie. Physically the eye's
// gain applies to the panel exactly as it applies to the world, and MFDs really
// would go dim. But this frame already carries the HUD and the MFDs (we run at
// RENDERPROC_HUD_2ND), and twenty seconds of unreadable instruments is a usability
// cost with no drama to pay for it. So near-white pixels keep more of their
// brightness. It applies ONLY while dimming: on the way out the highlights are
// exactly what SHOULD blow first.
// ----------------------------------------------------------------------------
uniform extern float fEclGain;     // eye's brightness multiplier (1 = adapted, no change)
uniform extern float fEclDesat;    // 0..1 scotopic colour loss
uniform extern float fEclProt;     // 0..1 how much near-white pixels resist the dimming

#define ECL_ROD_TINT  float3(0.82f, 0.97f, 1.28f)  // cool grey of rod vision (Purkinje);
                                                   //   luma ~0.96 of neutral - rod vision
                                                   //   IS dimmer, and that is deliberate
#define ECL_PROT_LO   0.72f        // luminance where highlight protection starts
#define ECL_PROT_HI   0.97f        // ... and where it is complete
#define ECL_VEIL      0.18f        // additive white per unit of gain above 1 (the dazzle)

float4 PSEclipse(float x : TEXCOORD0, float y : TEXCOORD1) : COLOR
{
	float4 src = tex2D(tSrc, float2(x, y));
	float  lum = dot(src.rgb, float3(0.299f, 0.587f, 0.114f));

	// 1 - rods take over: colour drains toward a blue-shifted grey.
	float3 rgb = lerp(src.rgb, lum * ECL_ROD_TINT, saturate(fEclDesat));

	// 2 - adaptation gain, spared on self-lit highlights while (and only while) dark.
	float dimMask = saturate((1.0f - fEclGain) * 8.0f);        // 1 when dimming, 0 when glaring
	float prot    = smoothstep(ECL_PROT_LO, ECL_PROT_HI, lum) * saturate(fEclProt) * dimMask;
	float g       = lerp(fEclGain, 1.0f, prot);

	// 3 - dazzle: the veil only exists when the eye is behind the light coming back.
	float glare = max(fEclGain - 1.0f, 0.0f) * ECL_VEIL;

	return float4(saturate(rgb * g + glare), src.a);
}

// ----------------------------------------------------------------------------
// GOD RAYS / crepuscular shafts (environment). Sunlight scattering out of the
// beam on its way past an occluder: shafts that radiate from the sun's screen
// position, broken by whatever stands in front of it.
//
// The classic radial-occlusion post-process (Mitchell, GPU Gems 3), and it fits
// ORO exactly because of WHERE we sit in the frame. D3D9Client draws its sun
// glare into the backbuffer at the END of Scene::RenderMainScene - after the
// bloom resolve, and BEFORE the HUD stages where ORO captures. So tSrc already
// holds a bright, correctly-occluded sun disc: the light source comes free, and
// with it the client's own answer to "is the sun behind the hull / the terrain /
// the limb", which we would otherwise have had to solve ourselves.
//
// The march is in UV straight toward vGRSun, so shafts converge on the sun in
// SCREEN space, which is what they physically do. Only the falloff radius is
// aspect-corrected - correcting the march direction would bend the shafts.
//
// THREE THINGS KEEP IT HONEST, and each is load-bearing:
//   * THRESHOLD - only pixels brighter than fGRThresh cast anything. Without it
//     every lit cloud smears and the frame turns to soup. This is what makes the
//     effect read as "light through gaps" rather than "radial blur".
//   * OFF-SCREEN FADE - the algorithm degrades once the source leaves the frame
//     (the march runs off the clamped edge and every sample returns the same
//     texel). fGRFade is the host's screen-proximity term; it reaches zero before
//     that happens.
//   * ATMOSPHERE - folded into fGRStr on the CPU, not here. No medium, no
//     scattering: in vacuum this pass never runs at all.
// ----------------------------------------------------------------------------
uniform extern float2 vGRSun;      // sun position in UV (may be outside [0,1])
uniform extern float  fGRStr;      // master strength; already carries the atmosphere gate
uniform extern float  fGRLen;      // 0..1 fraction of the pixel->sun span the march covers
uniform extern float  fGRDecay;    // per-sample falloff along the ray (<1)
uniform extern float  fGRThresh;   // luminance below which a texel contributes nothing
uniform extern float  fGRFade;     // 0..1 off-screen / elevation fade from the host
uniform extern float3 vGRTint;     // scattered-light colour (reddens as the sun sets)

#define GR_SAMPLES  24             // full-screen texture fetches per pixel - the whole
                                   //   cost of the effect lives on this number
#define GR_FALLOFF  2.50f          // radial reach around the sun, in aspect-corrected UV.
                                   //   WIDE ON PURPOSE: real crepuscular rays cross the whole
                                   //   sky - in the reference photographs the shafts run from
                                   //   a cloud on the horizon clear off the top of the frame.
                                   //   This started at 1.15 AND was squared, which killed them
                                   //   within a third of a screen of the sun and made the
                                   //   effect look like a halo. The classic algorithm has no
                                   //   radial term at all; this one is kept only as a soft
                                   //   backstop so the far corners of the frame do not pick up
                                   //   shafts aimed at a sun that is nowhere near them.
#define GR_KNEE     0.12f          // luminance band over which a texel goes from casting
                                   //   nothing to casting fully
#define GR_GAIN     2.0f           // per-sample weight. The sum is divided by GR_SAMPLES so
                                   //   the look does not change if the tap count does; this
                                   //   is what puts the result back on a visible scale.
                                   //   CALIBRATION: a pixel whose whole march lies on the
                                   //   sun accumulates ~0.9*GR_SAMPLES, so the addition
                                   //   peaks near 1.8*fGRStr - blown out against the disc at
                                   //   full strength, which is right, and still graded
                                   //   further out. The first version divided by GR_SAMPLES
                                   //   alone and peaked around 0.05: invisible.

float4 PSGodRay(float x : TEXCOORD0, float y : TEXCOORD1) : COLOR
{
	float2 uv  = float2(x, y);
	float4 src = tex2D(tSrc, uv);

	// March from this pixel toward the sun. fGRLen < 1 keeps the samples bunched
	// near the pixel, which shortens the shafts without thinning them.
	float2 delta = (vGRSun - uv) * (saturate(fGRLen) / GR_SAMPLES);

	float2 pos   = uv;
	float  decay = 1.0f;
	float3 acc   = 0.0f;

	[unroll] for (int i = 0; i < GR_SAMPLES; i++) {
		pos += delta;
		float3 s   = tex2D(tSrc, pos).rgb;
		float  lum = dot(s, float3(0.299f, 0.587f, 0.114f));
		// A MASK, not a subtraction. `max(lum - thresh, 0)` was the first version and it
		// was wrong twice over: a bright texel just above the threshold contributed almost
		// nothing, and raising the threshold dimmed the shafts that DID survive instead of
		// simply admitting fewer of them. Smoothstep separates the two jobs - what casts
		// (this) from how strongly it casts (fGRStr) - so a texel that qualifies
		// contributes its FULL colour, and a warm low sun throws warm shafts before
		// vGRTint is applied at all.
		acc += s * smoothstep(fGRThresh, fGRThresh + GR_KNEE, lum) * decay;
		decay *= fGRDecay;
	}

	acc *= (GR_GAIN / GR_SAMPLES);

	// Radial falloff around the sun, aspect-corrected so the pool of light is round
	// on a widescreen viewport rather than a horizontal ellipse. LINEAR, not squared:
	// the square was a second reach control fighting the two real ones (Reach, which
	// sets how far along the pixel->sun span the march runs, and Softness, which sets
	// how much the far end of that march still counts). Attenuation with distance is
	// those two knobs' job - this term only has to stop the effect wrapping the frame.
	float2 d    = (uv - vGRSun) * float2(max(fAspect, 0.001f), 1.0f);
	float  fall = saturate(1.0f - length(d) / GR_FALLOFF);

	// ADDITIVE: shafts are light arriving, never light removed. Saturating here (not
	// in the accumulator) lets the shafts stack into white where they overlap near
	// the disc, which is what the reference photographs do.
	return float4(saturate(src.rgb + acc * vGRTint * (fGRStr * fGRFade * fall)), src.a);
}
