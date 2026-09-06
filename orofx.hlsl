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
//   vPlumeP[i] = (radius_uv, strength, turb_peak, unused)
//   Strength is PER CAPSULE since 2026-09-04 (the fold, his call): the dialog
//   slider is baked into each plume's own vPlumeP.y by the host, so two engine
//   groups can haze at different strengths in one frame. There is no frame-wide
//   master any more - fShimmer was removed with the fold.
// Deliberately placed LAST in this file (not first in the render order) because
// it reuses fTime / fAspect, which are declared above - HLSL globals must be
// declared before use, and the whole file compiles as ONE unit per entry point.
// PLUME_N must match OroModule.h MAX_PLUMES. Inactive plumes carry strength 0
// (a constant loop count keeps the shader SM3-safe - no dynamic branching).
// ----------------------------------------------------------------------------
uniform extern float  fShimWave;   // x WAVELENGTH (2026-09-04): spatial scale of the
                                   //   ripple texture; 1 = the 2026-07-30 lab look
uniform extern float  fShimFreq;   // x FREQUENCY: temporal churn rate; 0 freezes.
                                   //   Both come from the strongest contributing
                                   //   plume's block (the 26e reduction) - the wave
                                   //   TEXTURE is the one thing still frame-wide.
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
		// The wave knobs (2026-09-04): time scales by fShimFreq, every spatial term
		// divides by fShimWave - ph carries both, so the octave harmonics (2.7/2.3/
		// 1.13) inherit them and the whole texture scales coherently instead of
		// detuning. Both at 1 = the old constants bit for bit.
		float sw = max(fShimWave, 0.05f);
		float ph = fTime * 9.0f * fShimFreq + (h * 26.0f) / sw + (float)i * 2.3f;
		float n1 = sin(ph)               + 0.5f * sin(ph * 2.7f + uv.y * 90.0f / sw);
		float n2 = cos(ph * 1.13f + 1.7f) + 0.5f * cos(ph * 2.3f + uv.x * 80.0f / sw);

		disp += float2(n1, n2) * w;
	}

	// SHIMMER_MAX alone since the 2026-09-04 fold: the dialog strength is already
	// inside each capsule's s (clamped host-side, where the old saturate lived).
	disp *= SHIMMER_MAX;
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
uniform extern float  fCabin;      // 0..1 wash balance: 0 = directional pool, 1 = flat cabin
uniform extern float  fFlash;      // >=1 flare envelope, shared with the VC sheath geometry
uniform extern float2 vPlasmaUV;   // where the plasma projects, in UV (may be off-screen)
uniform extern float3 vPlasmaCol;  // plasma colour for this heat band

// Tuned down 2026-08-01: the first values (0.85 / 0.16) blew the VC out at 90% heat -
// panel labels were washing away and the cabin read as overexposed rather than lit.
// Roughly halved, and the glow now RESPECTS what it is lighting: it scales with the
// surface it lands on, so dark corners stay dark instead of everything flooding to white.
#define PLASMA_SPREAD 1.30f        // bloom falloff - lower = broader wash
#define PLASMA_BLOOM  0.42f        // directional component at full heat, balance 0
#define PLASMA_AMB    0.07f        // uniform cabin lift at full heat, balance 0
#define PLASMA_FLOOR  0.35f        // how much glow reaches an unlit surface (rest is modulated)

// CABIN WASH BALANCE (fCabin, 2026-08-20). The two terms above shipped at 6:1 in favour
// of the DIRECTIONAL one, which means the cockpit only knows it is on fire while the fire
// is on screen: look at the instruments during a reentry and the cabin light goes with the
// streaks. fCabin slides energy from the pool into the flat term.
//
// ⚠️ IT IS A BALANCE, NOT A GAIN, AND THAT IS THE WHOLE DESIGN. The PEAK is held roughly
// constant (0.49 -> 0.42) while the PERIPHERY rises about 4.5x, so the knob changes WHERE
// the light lands and not how much there is - the aurora's Thickness law (invariant 19b)
// applied to a wash. Brightness already has an owner: the Reentry trim.
// At fCabin 0 both terms are their shipped values, so 0 is the old look bit for bit.
#define PLASMA_BLOOM1 0.10f        // directional component at balance 1
#define PLASMA_AMB1   0.32f        // uniform cabin lift at balance 1

float4 PSPlasma(float x : TEXCOORD0, float y : TEXCOORD1) : COLOR
{
	float4 src = tex2D(tSrc, float2(x, y));
	float2 d   = float2(x, y) - vPlasmaUV;
	d.x *= fAspect;                                  // circular in SCREEN space, not UV space
	float  bal  = saturate(fCabin);
	float  bl   = lerp(PLASMA_BLOOM, PLASMA_BLOOM1, bal);
	float  am   = lerp(PLASMA_AMB,   PLASMA_AMB1,   bal);
	float  r    = length(d);
	float  glow = exp(-r * r * PLASMA_SPREAD) * bl + am;
	// THE FLARE. Applied AFTER the saturate, which is the whole reason it is its own
	// uniform: heat x trim drives fPlasma several times past 1.0 in any real entry, so a
	// flash folded in there would be clamped away before it did anything. Damped to 55%
	// of the geometry's envelope - the sheath outside the glass is light arriving
	// directly, the cabin is that light bounced off panels, and it should read as the
	// quieter half of the same event. The final saturate() below clamps the peak, which
	// is what a flare looks like anyway.
	float  k    = saturate(fPlasma) * glow * (1.0f + (fFlash - 1.0f) * 0.55f);
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
// ----------------------------------------------------------------------------
// GLOOM - the overcast, and the cheapest half of making it rain.
//
// Orbiter has no weather, so a storm cannot actually put cloud between you and the sun.
// What it CAN do is model what that does to the light reaching the camera, which is the
// eclipse's lesson applied to a different cause: three things happen under a heavy
// overcast and none of them needs a cloud to exist.
//   1. COLOUR DRAINS. Diffuse skylight is nearly white and it swamps the warm direct
//      component, so saturated things go grey - and slightly COOL grey, because what is
//      left is scattered blue.
//   2. IT GETS DARK. A thick deck cuts the ground illumination by most of a stop.
//   3. CONTRAST COLLAPSES. Light arrives from every direction at once, so shadows fill
//      in. That is the lifted black floor below, and it is the term that stops the result
//      reading as "someone turned the brightness down".
// EXTERNAL view only in this slice - which is also the easy domain, because there is no
// cockpit interior that must stay unfogged.
// ----------------------------------------------------------------------------
uniform extern float fGloom;       // 0..1 overcast strength

// ⚠️ DEMOTED 2026-08-22 to a light touch, and the reason is the lesson. This pass used
// to carry the whole overcast - and a post-process CANNOT: the sharp sun shadows, the
// warm directional light and the speculars are already baked into every pixel by the
// time it runs, so all it could do was dim a sunny day. The overcast now happens at the
// SOURCE (client patch (s) part 2: the Gloom slider collapses gSun into a lifted ambient
// in the surface shaders, fades the projected shadows and kills the glare). What remains
// here is the one thing the source change does not do: the slight cool desaturation of a
// scene lit through water.
// ----------------------------------------------------------------------------
// RAINDROPS ON THE GLASS (2026-08-26) - and the reason they live inside PSGloom
// rather than in a pass of their own is that this pass is ALREADY RUNNING whenever
// rain is live, in both view domains. One copy of the frame, one full-screen draw,
// two jobs: refract, then grey. That order is also the physical one - the gloom is
// what the world outside looks like, and the drop is a lens held in front of it.
//
// ⚠️ WHY THIS IS A RESAMPLE AND NOT GEOMETRY. A drop on glass is a short-focus lens
// showing an inverted, magnified image of what is behind it, and the giveaway is that
// its interior swings hugely for a small head turn. Sketchpad can add light or blend a
// colour; it cannot displace what is already in the frame. So no amount of decal work
// reaches this look - the same shape of verdict as graveyard G13.
//
// ⚠️ ANCHORING IS THE WHOLE RISK, AND INVARIANT 28(n) ALREADY WROTE THE WARNING: a
// camera-glued field survives unnoticed in a cockpit BECAUSE the camera hardly
// translates there. It does ROTATE, and a drop stuck to a windscreen is maximally
// sensitive to exactly that. So the lattice is keyed on the pixel's view ray rotated
// into the VESSEL frame - the drops stay put on the glass while you look around, which
// is the entire illusion. Eye TRANSLATION (a seat shift) makes them lag slightly; that
// is the one honest approximation here, and it is small because a VC camera pivots.
//
// ⚠️ AND 28(n)'s SECOND HALF TOO: being vessel-fixed is not enough, the coordinate must
// also measure distance the same way in every direction. Plate carree does not - its
// longitude carries a cos(lat) factor - so cell WIDTH is scaled by the cosine of each
// ROW's own centre latitude. Per row, not per pixel: a per-pixel cosine would shear
// every cell. Cells come out square in arc measure at any latitude.
//
// ⚠️ AND THE INTEGER-CYCLE LAW (invariant 28e), for the third time in this project:
// longitude wraps at +-pi, so a lattice whose column count is not a whole number seams
// along the line directly behind the ship. Each row's column count is ROUNDED to an
// integer, which costs a few percent of squareness and makes the field seamless
// everywhere instead of nearly everywhere.
// ----------------------------------------------------------------------------
uniform extern float  fDrop;       // 0..1 OCCUPIED FRACTION of lattice cells (0 = block
                                   //   off). The host ramps it with the canopy soak, so
                                   //   it is the fill level, not just a strength.
uniform extern float  fDropCell;   // lattice cells per RADIAN - the size knob, inverted
uniform extern float  fDropLens;   // refraction gain: how far a drop displaces what it shows
uniform extern float  fTanAp;      // tan(camera aperture), for the ray reconstruction
uniform extern float3 vGlR;        // camera RIGHT   axis, expressed in the VESSEL frame
uniform extern float3 vGlU;        // camera UP      axis, expressed in the VESSEL frame
uniform extern float3 vGlF;        // camera FORWARD axis, expressed in the VESSEL frame
uniform extern float3 vGlRun;      // where a drop runs: gravity + airflow, VESSEL frame
uniform extern float3 vGlRunA;     // with vGlRunB: an orthonormal basis AROUND vGlRun,
uniform extern float3 vGlRunB;     //   host-built - the streaks' polar chart axes
uniform extern float  fRunAmt;     // RUNNERS: how many columns carry one (0 = none)
uniform extern float  fRunSize;    // ... their thickness - rides the DROP SIZE slider
                                   //   (his ask: one size control for the whole glass)
uniform extern float  fRunPh;      // ... and how far they have travelled: the run RATE
                                   //   (rad/s, derived host-side from |gravity + airflow|
                                   //   at the glass - parked runners crawl, in-flight ones
                                   //   whip aft, no threshold, invariant 25e) INTEGRATED
                                   //   over real time on the host. Was a rate the shader
                                   //   multiplied by fTime until 2026-09-06: that product
                                   //   tracks its DERIVATIVE, so a falling airspeed (engine
                                   //   cut on the runway) ran every runner BACKWARD - the
                                   //   sheet's 08-27 bug, not swept onto the runners.
uniform extern float  fDropDbg;    // TEMPORARY scaffold: 1 = ignore the depth mask,
                                   //   2 = VISUALIZE THE BUFFER instead of drawing
                                   //   drops - green where the depth is NEGATIVE (an
                                   //   authored window), blue-tinted where positive
                                   //   (interior/hull), untouched where 0 (nothing) -
                                   //   so "did the negation arrive" is answered by eye

// ⚠️ PATCH (h). GBUF_DEPTH: .a is the CAMERA DISTANCE in metres; 0 means "nothing was
// drawn here", and - part 2, 2026-08-26 - NEGATIVE means AUTHORED WINDOW GLASS: a mesh
// group carrying FLAG 1000 (his design: the burden is on the author to mark the panes)
// is written into the depth pass with its distance sign-flipped. The sign IS the mask:
// drops live exactly where sd < 0 and nowhere else - not on the panel, not on the
// coaming, not on an astronaut three metres down the cabin (the 1.6 m glass-plane guess
// this replaces failed precisely there: looking aft, the whole interior sat beyond the
// plane and read as sky). Occlusion is inherited from the pass's own z-buffer, so a
// seat back or helmet IN FRONT of a window holes the mask per pixel for free. Every
// other consumer of this channel guards with `sd > 0.1` and reads a negative as
// "nothing drawn" - which is exactly how unflagged glass already reads to them.
sampler tDepth;

float2 dhash2(float2 p)
{
	float2 q = float2(dot(p, float2(127.1f, 311.7f)), dot(p, float2(269.5f, 183.3f)));
	return frac(sin(q) * 43758.5453f);
}

float4 PSGloom(float x : TEXCOORD0, float y : TEXCOORD1) : COLOR
{
	float2 suv  = float2(x, y);      // where this pixel SAMPLES the world (a drop moves it)
	float3 tint = float3(1, 1, 1);   // rim darkening x caustic gain

	// A uniform branch: coherent across the whole frame, so it costs nothing when the
	// rain is external, dry, or running on a client without patch (h).
	if (fDrop > 0.001f)
	{
		// Is this pixel AUTHORED WINDOW GLASS? The sign of the depth answers it
		// (patch h part 2 - see tDepth's comment above). -0.25 rather than 0: a
		// pane nearer than 25 cm is the visor of a helmet you are wearing, not a
		// window, and fp16 noise around zero stays out.
		// TEMPORARY (2026-08-26): fDropDbg >= 1 bypasses the mask, so one flight says
		// whether the drops are absent or merely masked away. Scaffolding - out on
		// sign-off, like the wet mirror's MIR X toggle.
		float sd = tex2D(tDepth, suv).a;
		// TEMPORARY: dbg 2 paints the buffer itself - see fDropDbg's comment.
		if (fDropDbg >= 1.5f) {
			float4 s2 = tex2D(tSrc, suv);
			if (sd < -0.25f)     return float4(s2.rgb * 0.3f + float3(0, 0.7f, 0), s2.a);
			else if (sd > 0.1f)  return float4(s2.rgb * 0.5f + float3(0, 0, 0.4f), s2.a);
			return s2;
		}
		if (sd < -0.25f || fDropDbg >= 0.5f)
		{
			// --- the pixel's view ray, in the VESSEL frame -------------------------
			// Inverse of ProjectToUV: u = 0.5 + 0.5*(cx/cz)/(tanAp*aspect), v likewise
			// with the sign flipped because UV y grows downward.
			float2 ndc  = float2(x * 2.0f - 1.0f, 1.0f - y * 2.0f);
			float3 rayC = float3(ndc.x * fTanAp * fAspect, ndc.y * fTanAp, 1.0f);
			float  sec2 = 1.0f + dot(rayC.xy, rayC.xy);          // sec^2 of the off-axis angle
			float  sec  = sqrt(sec2);
			float3 rayV = normalize(rayC.x * vGlR + rayC.y * vGlU + rayC.z * vGlF);

			// --- lattice coordinates ---------------------------------------------
			float lat = asin(clamp(rayV.y, -0.9995f, 0.9995f));
			float lon = atan2(rayV.x, rayV.z);
			float rowsF = max(fDropCell * 3.1415927f, 4.0f);
			float bN    = (lat * 0.3183099f + 0.5f) * rowsF;      // 1/pi

			// The local tangent frame - east along +lon, north along +lat. Degenerate
			// only looking straight along the vessel's own Y axis, i.e. through the
			// roof or the floor, where the mask has already said no.
			float2 hor = float2(rayV.z, -rayV.x);
			float  hl  = length(hor);
			float3 east  = (hl > 1e-4f) ? float3(hor.x / hl, 0.0f, hor.y / hl) : float3(1, 0, 0);
			float3 north = cross(rayV, east);

			// Lattice-space -> UV, exactly (no small-angle assumption): perturb the
			// camera-space ray along each tangent and differentiate the projection.
			// rayC.z == 1 by construction, which is what makes these two lines short.
			float3 eC = float3(dot(east,  vGlR), dot(east,  vGlU), dot(east,  vGlF));
			float3 nC = float3(dot(north, vGlR), dot(north, vGlU), dot(north, vGlF));
			float2 Ju = float2(eC.x - rayC.x * eC.z, nC.x - rayC.x * nC.z) * ( 0.5f * sec / (fTanAp * fAspect));
			float2 Jv = float2(eC.y - rayC.y * eC.z, nC.y - rayC.y * nC.z) * (-0.5f * sec /  fTanAp);

			// Where a drop runs on this piece of glass, in lattice space. Gravity and
			// airflow are summed as forces in the VESSEL frame by the host (invariant
			// 25e), so nothing here assumes a hull axis; the component along the view
			// ray is simply not on the glass and drops out with the projection.
			float2 rd = float2(dot(vGlRun, east), dot(vGlRun, north));
			float  rl = length(rd);
			rd = (rl > 1e-4f) ? rd / rl : float2(0.0f, -1.0f);

			// --- the 3x3 neighbourhood -------------------------------------------
			float best = 0.0f;          // the winning drop's height
			float2 bestN = float2(0, 0);// ... its in-plane normal, lattice space
			float  bestR = 1.0f;        // ... its radius, in cells
			float  br    = floor(bN);

			for (int dr = -1; dr <= 1; dr++)
			{
				float row  = br + dr;
				float phi  = ((row + 0.5f) / rowsF - 0.5f) * 3.1415927f;
				float cols = max(floor(6.2831853f * fDropCell * cos(phi) + 0.5f), 4.0f);
				float aN   = (lon * 0.1591549f + 0.5f) * cols;   // 1/2pi
				float ba   = floor(aN);

				for (int dc = -1; dc <= 1; dc++)
				{
					float col = ba + dc;
					// WRAPPED id, so the column past the last one hashes as the first.
					float2 id = float2(col - floor(col / cols) * cols, row);
					// THREE independent hashes, and that is not extravagance. Sharing a
					// channel between two properties correlates them, and a correlation
					// between size and position inside the cell is exactly what the eye
					// picks up as a diagonal grain in an otherwise random field - the
					// lesson invariant 28(k) paid for with three rejected pool patterns.
					float2 h1 = dhash2(id);                          // occupancy, x jitter
					float2 h2 = dhash2(id + float2( 37.7f, 91.3f));  // radius, y jitter
					float2 h3 = dhash2(id + float2(-53.1f, 17.9f));  // elongation

					// COVERAGE IS A BIRTH ORDER (2026-08-26, the build-up). fDrop is now
					// the plain occupied fraction, and each cell's hash IS its birth
					// rank: as the glass fills (the host ramps fDrop with the canopy's
					// own soak scalar), cells cross the threshold one at a time in hash
					// order - drops pop in one by one across the pane, and thin out in
					// reverse when it dries. No per-drop state, no clock: a pure
					// function of (cell, fDrop), so it is warp-proof and freezes
					// correctly under pause with the scalar that drives it.
					if (h1.x >= fDrop) continue;

					// ... and a newborn GROWS rather than appearing full-size: maturity
					// ramps over the last ~8% of coverage rise, scaling the radius (and
					// with it the lens, which rides bestR). At GLASS_RISE = 16 s that is
					// roughly a second of swelling per drop - visible, not a pop.
					float m = saturate((fDrop - h1.x) * 12.5f);

					// Radius 0.22 .. 0.50 of a cell, biased small - a real windscreen
					// is mostly small drops with a few fat ones, and the max() below
					// lets neighbours that overlap read as one merged blob.
					float rad = (0.22f + 0.28f * h2.x * h2.x) * m;
					float str = 1.0f + 0.85f * h3.x;             // elongation along the run

					// Jitter across MOST of the cell (0.15 .. 0.85), not a timid ±0.2.
					// A tight jitter leaves the lattice legible underneath, which is the
					// failure mode that reads as "a pattern" rather than as weather.
					float2 q = float2(aN - (col + 0.15f + 0.70f * h1.y),
					                  bN - (row + 0.15f + 0.70f * h2.y));
					// into the run frame, then stretched along it
					float2 qr = float2(dot(q, float2(-rd.y, rd.x)), dot(q, rd) / str);
					float  d  = length(qr) / rad;
					if (d >= 1.0f) continue;

					float f = 1.0f - d * d;                       // paraboloid height
					if (f > best) { best = f; bestN = qr / rad; bestR = rad; }
				}
			}

			if (best > 0.0f)
			{
				float d = sqrt(saturate(1.0f - best));            // 0 centre .. 1 rim

				// THE LENS. A droplet's focal length is far shorter than its distance
				// to anything outside, so it does not merely smear - it INVERTS. The
				// displacement is therefore several times the drop's own angular size,
				// which is what makes the interior swing as the view moves.
				float2 offA = -bestN * (bestR * fDropLens / fDropCell) * sec2;
				float2 duv  = float2(dot(offA, Ju), dot(offA, Jv));

				// Soft edge, so a drop is not a cut-out at any size.
				float a = smoothstep(1.0f, 0.90f, d);
				suv += duv * a;

				// THE RIM IS DARK because at a grazing angle the surface stops
				// transmitting and reflects the dark surround instead - total internal
				// reflection, and it is the outline that makes each drop legible.
				// The CENTRE IS BRIGHT because the same lens concentrates what it
				// gathers. Both ride the same d, so they can never disagree.
				float rim   = smoothstep(0.62f, 1.00f, d);
				float caust = (1.0f - d * d) * (1.0f - d * d);
				tint = lerp(float3(1, 1, 1),
				            float3(1, 1, 1) * (1.0f - 0.55f * rim + 0.30f * caust), a);
			}

			// --- THE RUN STREAKS (2026-08-27, chart v2 the same day) ---------------
			// Runners: drops that broke loose and carve a wet track down-run. THE SOOT
			// IDIOM (invariant 23d): every runner is a pure function of (sector, fTime)
			// - born, travels, fades, reseeds - so nothing accumulates (G10) and pause
			// freezes them with the clock, exactly like the fill.
			// ⚠️ THE CHART IS ANCHORED TO THE RUN AXIS, NOT TO A PER-PIXEL PROJECTION.
			// v1 built a flat chart from the run direction AS PROJECTED AT EACH PIXEL -
			// and that projection DEGENERATES where the run axis points into the screen,
			// which in flight is the middle of the FORWARD WINDOW: the columns closed
			// into concentric rings that shimmered with attitude (his report, verbatim:
			// "circular concentric motion/rings... vibrating"). The v2 chart is polar
			// AROUND the flow axis: AZIMUTH sectors as columns, angle-from-stagnation as
			// the travel coordinate. Smooth everywhere but the two poles; in the forward
			// view the streaks RADIATE from the impact point - the driving-into-rain
			// look he asked for, and also what flow over a canopy really does, diverging
			// from stagnation. A small calm disc at the exact centre is the stagnation
			// region, honestly, and it is also what hides the pole.
			// The sector count is an INTEGER - azimuth wraps, so this is the
			// integer-cycle law (28e) in its fourth outfit.
			#define ORO_RUN_SEC 140.0f       /* azimuth sectors around the run axis */
			if (fRunAmt > 0.001f)
			{
				// local tangent axes of the polar chart: rd (the run direction's
				// tangent projection, already computed for the drops) IS the meridian
				// away from stagnation; pp2 is the azimuthal direction beside it.
				float2 pp2 = float2(-rd.y, rd.x);
				// the pixel's ray in the RUN FRAME (vGlRunA/B = host-built basis)
				float ca = dot(rayV, vGlRunA);
				float cb = dot(rayV, vGlRunB);
				float cr = dot(rayV, vGlRun);
				float cv = acos(clamp(-cr, -1.0f, 1.0f));     // 0 = stagnation point,
				                                              // grows DOWNSTREAM
				float sv = sin(cv);                           // sector width factor
				float cu = atan2(ca, cb) * (ORO_RUN_SEC / 6.2831853f);
				float ci = floor(cu);
				float id = ci - floor(ci / ORO_RUN_SEC) * ORO_RUN_SEC;   // wrap the seam
				float2 h1r = dhash2(float2(id, 17.31f));
				float2 h2r = dhash2(float2(id + 91.7f, 3.13f));
				float2 h3r = dhash2(float2(id - 53.1f, 17.9f));   // trail length seed -
				                                  // its OWN channel (the drops' three-
				                                  // hash lesson: sharing correlates)
				// Occupancy rides the knob AND the fill: runners need water on the
				// glass before they have anything to gather.
				if (h1r.x < saturate(0.08f + 0.30f * fRunAmt) * saturate(fDrop * 1.6f))
				{
					const float L = 0.45f;                    // wrap period, radians
					// per-runner pace x the host-integrated phase (a constant times an
					// integral is the integral of the constant times the rate)
					float vh  = fmod(fRunPh * (0.6f + 0.8f * h2r.x) + h1r.y * L, L);   // the head
					float va  = cv - floor(cv / L) * L;                 // this pixel
					// column-local ACROSS offset, in RADIANS of arc (sector width is
					// (2pi/N)*sin(cv)); curvature is SPATIAL and seed-keyed, never
					// temporal - a time wiggle would slide the laid trail sideways,
					// G12(c)'s chord-polyline cousin.
					// SIZED AS ONE FAMILY WITH THE DROPS (his ask, round 2): a static
					// drop's radius is (0.22..0.50 cells) x (fDropSize/100 rad per
					// cell); the head lands in the upper half of that range - a runner
					// IS a gathered drop, so it reads as one of the bigger ones, never
					// as a different species. The track is ~65% of its head's width
					// (the channel a drop carves is narrower than the drop). rs is
					// capped so slider-max heads cannot outgrow their azimuth column.
					// Base = the MIDPOINT of the two flown rounds (too thick / too
					// small); fRunSize now also carries his Runner size multiplier,
					// so the final word on this number is the slider's, not a guess.
					float rs = min(fRunSize, 2.2f);
					float rh = (0.0048f + 0.0027f * h1r.x) * rs;   // head radius, rad
					float w  = (0.0032f + 0.0018f * h2r.y) * rs;   // track half-width
					float xl = (cu - ci - 0.5f) * (6.2831853f / ORO_RUN_SEC) * sv
					         - 0.0035f * rs * sin(cv * 11.0f + h2r.y * 6.2832f);
					// the two pole fades: the stagnation disc, and the convergence
					// behind the run pole
					float sp = smoothstep(0.05f, 0.16f, cv)
					         * smoothstep(0.08f, 0.22f, 3.1416f - cv);

					// THE TRAIL: a thin wet channel fading behind the head. A point at
					// distance db behind was passed db/spd seconds ago; fading with db
					// is the same statement with no second clock.
					float db = vh - va; if (db < 0.0f) db += L;
					// PER-RUNNER trail length (his ask: "some runners produce longer
					// trails than others... so it seems random like the real thing") -
					// 0.30..0.90 of the wrap, its own seed, and longer on average than
					// the old fixed 0.42 so parked runners visibly DRAG something.
					float TR = (0.30f + 0.60f * h3r.x) * L;
					if (db < TR && abs(xl) < w)
					{
						float nx = xl / w;                    // cross-channel normal
						float aT = (1.0f - db / TR) * (1.0f - nx * nx) * sp;
						float2 offEN = pp2 * (nx * 0.010f * fDropLens);
						suv += float2(dot(offEN, Ju), dot(offEN, Jv)) * aT;
						tint *= 1.0f - 0.28f * aT * smoothstep(0.25f, 1.0f, abs(nx));
					}

					// THE HEAD: a fat drop on the move - the drop shading restated in
					// the streak chart, with the same rim and caustic so a runner's
					// head and a sitting drop are visibly the same kind of thing.
					float dv = va - vh; dv -= L * floor(dv / L + 0.5f);  // wrap +-L/2
					float2 hq = float2(xl, dv) / rh;
					float hd2 = dot(hq, hq);
					if (hd2 < 1.0f)
					{
						float hdd = sqrt(hd2);
						float ah  = smoothstep(1.0f, 0.90f, hdd) * sp;
						float2 offEN = -(pp2 * hq.x + rd * hq.y)
						             * (rh * fDropLens) * sec2;
						suv += float2(dot(offEN, Ju), dot(offEN, Jv)) * ah;
						float rim   = smoothstep(0.62f, 1.0f, hdd);
						float caust = (1.0f - hd2) * (1.0f - hd2);
						tint *= lerp(1.0f, 1.0f - 0.55f * rim + 0.30f * caust, ah);
					}
				}
			}
		}
	}

	float4 src = tex2D(tSrc, suv);
	float  lum = dot(src.rgb, float3(0.299f, 0.587f, 0.114f));
	float3 grey = lum * float3(0.94f, 0.98f, 1.08f);        // cool, not neutral
	float3 c = lerp(src.rgb, grey, saturate(fGloom) * 0.45f);
	c = lerp(c, c * 0.90f + 0.012f, saturate(fGloom));      // a whisper of the old dimmer
	return float4(saturate(c * tint), src.a);
}

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
