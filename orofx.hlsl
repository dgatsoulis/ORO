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
uniform extern float  fShear;      // 0..1 how hard the airflow is stripping the pane
                                   //   (dynamic pressure, host-sensed). Drops thin out,
                                   //   runners multiply, the film comes up - one number,
                                   //   three consequences, so they cannot disagree
uniform extern float  fFilm;       // the FILM's own trim, 0 = off (the user's taste on
                                   //   top of the physics - 25i)
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
uniform extern float  fFrost;      // ICE ON THE GLASS (snow round 3, step C, 2026-09-20): the
                                   //   host's frost STATE 0..1 - grown on sim time while snow
                                   //   falls, melted when it stops, 0 with its pill off
uniform extern float  fFrostReach; // how far in from the frame a full state reaches, 0..1
uniform extern float  fFrostBlur;  // how much the world scatters through the ice, 0..2
uniform extern float  fDropDbg;    // MASK DEBUG (the panel's own row): 1 = ignore the mask,
                                   //   2 = VISUALIZE THE BUFFER instead of drawing
                                   //   drops - GREEN where the client marked authored
                                   //   window glass with only sky beyond it, TEAL where it
                                   //   marked glass AND holds the depth of something beyond
                                   //   the pane (a building, the ground), BLUE where it
                                   //   holds interior/hull/world with no glass, untouched
                                   //   where 0 (nothing) - so "did the mask arrive" and
                                   //   "did the world behind the glass survive" are both
                                   //   answered by eye (the second is the 09-18 fix)

// ⚠️ PATCH (h). GBUF_DEPTH: .a is the CAMERA DISTANCE in metres of the nearest OPAQUE
// thing at this pixel - hull, cockpit, building, terrain - and 0 means "nothing was
// drawn here" (sky). Part 2 (2026-08-26, re-encoded 2026-09-18): AUTHORED WINDOW GLASS -
// a mesh group the author marked `RAIN 1`, or the RAINSURFACES picker declared - is
// drawn by the client in a MARKER pass at the END of its depth pass and writes ITS OWN
// distance NEGATED into .b, leaving .a to the world beyond the pane. So `.b < 0` IS the
// mask: drops live exactly there and nowhere else - not on the panel, not on the
// coaming, not on an astronaut three metres down the cabin (the 1.6 m glass-plane guess
// this replaced failed precisely there: looking aft, the whole interior sat beyond the
// plane and read as sky). Occlusion of the mask is inherited from the pass's own
// z-buffer, so a seat back or helmet IN FRONT of a window holes it per pixel for free.
// Real geometry's .b is a camera-space normal component, >= 0 by construction, which is
// what makes the sign free to take.
// !! Until 09-18 the mask was the sign of .a ITSELF, and that cost every OTHER consumer
// of the buffer the depth behind the glass: from the VC, the patch-(g) clip had nothing
// to cut the rain deck, the bolts, the aurora or the plasma against, and they drew
// through buildings (triage 260916 item 25). Read .b for the glass, .a for the world.
sampler tDepth;

float2 dhash2(float2 p)
{
	float2 q = float2(dot(p, float2(127.1f, 311.7f)), dot(p, float2(269.5f, 183.3f)));
	return frac(sin(q) * 43758.5453f);
}
// a scalar value noise on the direction chart (step C: the frost's ragged boundary and
// its crystal texture) - bilinear on a hashed lattice, no period needed here: the chart's
// only seam is at the back of the head, where there is no glass
float fhash1(float2 p) { return frac(sin(dot(p, float2(127.1f, 311.7f))) * 43758.5453f); }
float fnoise(float2 p)
{
	float2 i = floor(p), f = frac(p);
	f = f * f * (3.0f - 2.0f * f);
	float a = fhash1(i), b = fhash1(i + float2(1, 0)), c = fhash1(i + float2(0, 1)), d = fhash1(i + float2(1, 1));
	return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float4 PSGloom(float x : TEXCOORD0, float y : TEXCOORD1) : COLOR
{
	float2 suv  = float2(x, y);      // where this pixel SAMPLES the world (a drop moves it)
	float3 tint = float3(1, 1, 1);   // rim darkening x caustic gain
	float  frostF = 0.0f, frostT = 1.0f;   // step C: this pixel's ice, and its crystal texture

	// A uniform branch: coherent across the whole frame, so it costs nothing when the
	// rain is external, dry, or running on a client without patch (h).
	if (fDrop > 0.001f || fFrost > 0.001f)
	{
		// Is this pixel AUTHORED WINDOW GLASS? The sign of the depth answers it
		// (patch h part 2 - see tDepth's comment above). -0.25 rather than 0: a
		// pane nearer than 25 cm is the visor of a helmet you are wearing, not a
		// window, and fp16 noise around zero stays out.
		// Mask Debug >= 1 bypasses the mask, which answers the one question a
		// screenshot cannot: are the drops absent, or merely masked away? It was
		// built as scaffolding and KEPT (2026-09-12) because that turned out to be
		// the thing users actually need when a windscreen looks wrong.
		float4 sdn = tex2D(tDepth, suv);
		float  gd  = sdn.b;      // < -0.25: authored window glass here, -gd = its distance
		float  sd  = sdn.a;      // the world beyond it (0 = sky), whatever the glass says
		// Mask Debug 2 paints the buffer itself - see fDropDbg's comment.
		if (fDropDbg >= 1.5f && fDropDbg < 2.5f) {   // 2 only - 3 is the frost's, further down
			float4 s2 = tex2D(tSrc, suv);
			if (gd < -0.25f)     return float4(s2.rgb * 0.3f + float3(0, 0.7f, (sd > 0.1f) ? 0.5f : 0.0f), s2.a);
			else if (sd > 0.1f)  return float4(s2.rgb * 0.5f + float3(0, 0, 0.4f), s2.a);
			return s2;
		}
		if (gd < -0.25f || fDropDbg >= 0.5f)
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

			// STANDING coverage, which is NOT the same as how much water is arriving
			// (2026-09-12, triage A4). Past ~44 m/s of dynamic pressure the airflow
			// strips sitting drops faster than they can gather, so the lattice empties -
			// in REVERSE BIRTH ORDER, for free, because coverage is already a birth rank
			// (see below): the last drops to appear are the first to be blown away, and
			// no per-drop state was needed to say so.
			// ⚠️ fDrop itself is left alone, because the RUNNERS key off it: they need
			// water ARRIVING, not water sitting, and scaling their gate with this would
			// have made them thin out exactly when they should be multiplying.
			float dropCov = fDrop * (1.0f - 0.85f * fShear);

			if (fDrop > 0.001f)   // step C: ice runs this block's chart without water on the glass
			{
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
						if (h1.x >= dropCov) continue;

						// ... and a newborn GROWS rather than appearing full-size: maturity
						// ramps over the last ~8% of coverage rise, scaling the radius (and
						// with it the lens, which rides bestR). At GLASS_RISE = 16 s that is
						// roughly a second of swelling per drop - visible, not a pop.
						float m = saturate((dropCov - h1.x) * 12.5f);

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
					// ... AND THE SHEAR (2026-09-12, A4), which is the other half of the
					// same statement as the thinning drops above: the water the airflow
					// tears off the lattice has to go somewhere, and where it goes is into
					// running columns. At full shear every column carries one, which is the
					// point at which the pane stops being a field of drops and the film
					// below takes over. The fill gate stays on fDrop - water ARRIVING.
					// ... AND THE PLACE ON THE PANE (2026-09-19, Stebb: "the bottom right and
					// left of the front screen remain free of runners, but there are loads
					// of runners towards the centre of the window... an 'un-weighted' random
					// distribution so all parts of the windows get a similar distribution").
					// The sectors are equal in AZIMUTH and converge at the radiant, so the
					// occupied columns pack as 1/sin(cv) toward it: with the radiant under
					// the glass in flight, the pane's bottom centre ran 2-4x denser than its
					// corners. A runner is born where a drop LANDS, and drops land evenly -
					// so the occupancy is scaled by sin(cv) (matched at 45 deg: fewer columns
					// near the radiant, more far from it) and the occupied columns per unit
					// of arc - runners per pane area - come out constant. The gate is per
					// pixel, so a column picks its runner up partway along, with a soft start
					// over a few degrees rather than an edge; the Runner amount slider still
					// sets the overall level.
					float occ  = saturate((0.08f + 0.30f * fRunAmt) * (1.0f + 2.2f * fShear))
					           * saturate(fDrop * 1.6f) * (sv * 1.4142136f);
					float occD = occ - h1r.x;
					if (occD > 0.0f)
					{
						float born = smoothstep(0.0f, 0.04f, occD);   // the partway start
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
							float aT = (1.0f - db / TR) * (1.0f - nx * nx) * sp * born;
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
							float ah  = smoothstep(1.0f, 0.90f, hdd) * sp * born;
							float2 offEN = -(pp2 * hq.x + rd * hq.y)
							             * (rh * fDropLens) * sec2;
							suv += float2(dot(offEN, Ju), dot(offEN, Jv)) * ah;
							float rim   = smoothstep(0.62f, 1.0f, hdd);
							float caust = (1.0f - hd2) * (1.0f - hd2);
							tint *= lerp(1.0f, 1.0f - 0.55f * rim + 0.30f * caust, ah);
						}
					}
				}

				// --- THE WATER FILM (2026-09-12, triage A4's second half) --------------
				// Past the point where the airflow strips drops faster than they can sit,
				// what is left on the glass is not a field of objects at all - it is a thin
				// SHEET being dragged across the pane, and what you see through it is not
				// bent by individual lenses but rippled continuously. Stebb asked for it as
				// "a 'vision disturbance'... to mimic a film of water on the glass", which
				// is the right description: it disturbs, it does not draw.
				//
				// So it is DISPLACEMENT ONLY, with no shape of its own and no edges - two
				// ripple trains streaming down-flow at different rates, plus the faintest
				// loss of contrast where the sheet is thick. It could not be built out of
				// the drop or runner primitives: both are objects with rims, and a rim is
				// exactly what a sheet does not have.
				//
				// ⚠️ THE AZIMUTH MULTIPLIERS ARE INTEGERS. Azimuth wraps at 2pi, so a
				// fractional one seams down the middle of the pane - the integer-cycle law
				// (28e) in its fifth outfit, and the same rule the runner sectors obey.
				// ⚠️ AND THE PHASE IS THE HOST-INTEGRATED fRunPh, never a rate x fTime: that
				// product tracks the DERIVATIVE, which ran every runner backwards when the
				// engine was cut on the runway (2026-09-06). One integral, two consumers.
				float fw = fShear * saturate(fFilm);
				if (fw > 0.002f)
				{
					float fcv = acos(clamp(-dot(rayV, vGlRun), -1.0f, 1.0f));  // down-flow
					float faz = atan2(dot(rayV, vGlRunA), dot(rayV, vGlRunB)); // around it
					float fph = fRunPh * 3.4f;
					float r1 = sin(fcv * 52.0f - fph * 5.0f + faz *  7.0f);
					float r2 = sin(fcv * 31.0f - fph * 3.1f + faz * 11.0f + 2.2f);
					float rr = r1 * 0.62f + r2 * 0.38f;
					// Along the flow, because that is the direction the sheet is moving.
					float2 offF = rd * (rr * 0.0020f * fDropLens * fw);
					suv += float2(dot(offF, Ju), dot(offF, Jv));
					// Water scatters a little of what passes through it. Keyed to the same
					// ripple so the thick parts are the dim parts - one number again.
					tint *= 1.0f - 0.09f * fw * (0.5f + 0.5f * rr);
				}
			}

			// --- THE FROST (snow round 3, step C, 2026-09-20 - his item 7: "icing on the
			// windows, starting from the rims of the panels and going inwards, which should
			// also affect the way the world looks outside") ------------------------------
			// The ice grows from the PANE'S OWN RIM in - client patch (h) part 5, his call the
			// same day: the glass marker pass writes each pane vertex's distance to the pane's
			// open edges (metres, from the group's topology, once at mesh load) into the mask's
			// spare RED channel, so a glass pixel reads exactly how far it stands from its frame,
			// camera-independent. Until then the distance was ESTIMATED from 24 taps of the mask
			// at fixed screen offsets - the silhouette summed 24 times: a staircase for a
			// boundary, ghosts that moved with the camera, the HUD arm read as a frame; his
			// "why so many overlapping copies of the mask" in Mask Debug 3 was that sum.
			// fFrost is the ice's STATE (the host: grown on sim time while snow falls, melted
			// when it stops); fFrostReach is METRES of glass a full state ices in from the rim.
			// The ice line is ragged with a value noise on the direction chart (the drops' own
			// lat/lon, anchored to the cockpit), the ice denser toward the rim, its crystal
			// texture two finer octaves. The world seen through it is the sampling at the end.
			if (fFrost > 0.001f)
			{
				const float rim = max(sdn.r, 0.0f);                  // metres from the pane's rim
				const float R   = saturate(fFrost) * max(fFrostReach, 0.0f);
				float2 ch = float2(lon, lat);
				if (R > 1e-3f)
				{
					float nB   = fnoise(ch * 40.0f + 11.3f);
					float edge = R * (1.0f + 0.30f * (nB - 0.5f));      // the ragged ice line, metres
					float inn  = saturate(1.0f - rim / R);              // 1 at the rim, 0 at the line
					frostF = smoothstep(edge * 1.15f, edge * 0.85f, rim) * (0.55f + 0.45f * inn);
					frostT = 0.45f + 0.55f * (0.5f * fnoise(ch * 110.0f + 3.7f) + 0.5f * fnoise(ch * 300.0f + 9.1f));
				}
				// MASK DEBUG 3 (the RAIN page's row): the mechanism on the glass - the rim distance
				// in RED (a metre reads full), the ice in GREEN - so "nothing" and "everywhere"
				// each have a picture.
				if (fDropDbg >= 2.5f) return float4(saturate(rim), saturate(frostF), 0.0f, 1.0f);
			}
		}
	}

	float4 src = tex2D(tSrc, suv);
	// THE WORLD THROUGH THE ICE (step C): scattered - a four-tap blur whose radius rides the
	// ice and the Frost blur slider - and whitened toward the frost's own colour, lit by what
	// comes through it (a dark night keeps dark ice), modulated by the crystal texture. A
	// pixel with no frost is bit for bit as before.
	if (frostF > 0.001f)
	{
		float2 bo = float2(0.007f / fAspect, 0.007f) * fFrostBlur * frostF;
		float3 bl = (tex2Dlod(tSrc, float4(suv + bo * float2( 1.0f,  0.35f), 0, 0)).rgb
		           + tex2Dlod(tSrc, float4(suv + bo * float2(-1.0f, -0.35f), 0, 0)).rgb
		           + tex2Dlod(tSrc, float4(suv + bo * float2( 0.35f, -1.0f), 0, 0)).rgb
		           + tex2Dlod(tSrc, float4(suv + bo * float2(-0.35f,  1.0f), 0, 0)).rgb) * 0.25f;
		float  lb = dot(bl, float3(0.299f, 0.587f, 0.114f));
		// ONE COLOUR, DAY OR NIGHT (2026-09-20, his rule for now: "the frost must look the same on
		// the windows, day or night"). Keyed on the light through the glass it was black on a
		// black night and, once the flakes showed at night, lit up wherever a flake sat - squares;
		// keyed on the sky's light it was a dark grey ice he did not want. A lit ice, then, and
		// the crystal texture alone varies it; only the world seen THROUGH it changes with the light.
		float3 fc = float3(0.90f, 0.93f, 0.98f) * 0.85f * frostT;
		src.rgb = lerp(src.rgb, lerp(bl, fc, 0.90f * frostF), saturate(frostF * 1.4f));
	}
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


// ============================================================================
// PSLensFlare - THE CAMERA'S OWN ARTEFACT (2026-09-12)
// ----------------------------------------------------------------------------
// EXTERNAL VIEWS ONLY, and that is a physical ruling rather than a scope cut. A lens
// flare is made INSIDE a lens, by light bouncing between the elements of the optic;
// a healthy human eye has no such elements and does not produce one. So in a cockpit,
// where you are looking through the pilot's eyes, there is no flare - in any of the
// three internal views - and outside, where the camera IS the lens, there is. His rule,
// and it also settles every awkward case for free: no VC glass to reason about, no HUD
// ordering, nothing to explain in the help.
//
// DRAWN LAST of everything ORO does to the frame, for the same reason: the god rays are
// scattering in the air AHEAD of the lens and the shimmer is heat ahead of it, so both
// have to be in the image before the glass gets to smear it. The practical half of that
// is that the god ray march cannot turn our ghosts into shafts.
//
// WHAT IT MEASURES, AND WHY IT IS CONTRAST AND NOT BRIGHTNESS.
// The client has already drawn its own sun disc into the frame we captured, and has
// already occluded it against hull, terrain and limb and dimmed it through the storm
// light, the fog, the rings and the eclipse. So "is the sun visible, and how hard" is
// answered by READING THOSE PIXELS - the god rays' trick (invariant 24a), and it means
// this effect carries no brightness rule of its own. That matters here specifically:
// every rule the 2026-09-01 sun-disc round wrote misread a hazy morning as a sunset,
// because nothing in the sun pipeline knows about clouds.
// The quantity is the sun's CONCENTRATION - its own pixels against the ring of sky
// around them. Over a uniformly bright cloud deck both are bright, the contrast
// collapses and the flare fades, which is exactly what haze does to a real lens.
// Brightness alone would have made a bright hazy day flare HARDER, which is backwards.
// The depth tap is the backstop the contrast probe cannot provide on its own: a
// SUNLIT WHITE HULL crossing the disc is bright against black space and would read as
// a sun. Five taps rather than one so the flare fades across the edge instead of
// snapping off (the OroSunTerrainVis lesson - a centred probe must not model a hard
// edge as a step).
// ============================================================================
uniform extern float  fLFMode;     // WHICH LENS: 0 CLASSIC, 1 ANAMORPHIC, 2 CLEAN,
                                   //   3 VINTAGE (uncoated - mostly veiling glare). One
                                   //   value for the whole frame, so the branch it drives
                                   //   is free; every slider below means the same thing in
                                   //   all three, which is why this is one effect with an
                                   //   optic to choose rather than three effects
uniform extern float  fLFStr;      // master intensity (0..2; 1 = the reference look)
uniform extern float  fLFSize;     // scales the ghost chain and the reach of the rays
uniform extern float  fLFGhost;    // opacity of the ghost chain alone
uniform extern float  fLFRay;      // strength of the starburst alone
uniform extern float  fLFDisp;     // how far the chain's colours spread from white
uniform extern float  fLFFade;     // host budget: screen proximity x air x eclipse
uniform extern float  fLFSamp;     // 1 = the sun's UV is inside the frame, so the probe
                                   //   below means something; 0 = it is off-screen and
                                   //   there is nothing to read, so trust the budget
uniform extern float  fLFDepth;    // 1 = tDepth is bound (patch (h) AND Sun glare on)

// The blade count lives per-lens now (see `blades` below). MANY AND THIN is the whole
// difference between the reference photographs and the 2016 client's eight fat wedges -
// same primitive, and the count plus the sharpening exponent is what separates "a lens"
// from "a video game". It is also most of what separates the three lenses from each other.
#define LF_PROBE  0.0065f          // centre-tap radius, in units of screen HEIGHT
#define LF_RING   0.0900f          // ... and the surround the contrast is measured against.
                                   //   Wide enough to clear the client's own glare sprite,
                                   //   which has real angular size; see the min() below,
                                   //   which is what makes the exact number forgiving

// One element of the chain: a translucent disc with a brighter aperture RIM. The rim is
// what makes a ghost read as a piece of glass rather than as a blob, and `ring` blends
// the filled body away entirely to leave the bare annulus - the big faint circles.
// Branchless on purpose: both terms fall to zero on their own past the radius, so an
// early-out would only add divergence to a shader every pixel runs.
float lfElem(float2 p, float2 c, float R, float rimW, float ring)
{
	float r    = length(p - c) / max(R, 1e-4f);
	float body = saturate(1.0f - r); body *= body;
	float rim  = saturate(1.0f - abs(r - 0.88f) / max(rimW, 1e-3f));
	rim = rim * rim * rim;
	// Low amplitudes on purpose: these are TRANSLUCENT. Eight of them at a peak near 1
	// would wash the frame into fog; in the reference the largest disc is a quarter-tone
	// over the sky and you read it by its edge, not its fill.
	return lerp(body * 0.26f + rim * 0.34f, rim * 0.38f, saturate(ring));
}

// SCREEN, not additive. A flare is an artefact laid over a finished image and it cannot
// make a pixel brighter than white; additive would blow the frame out around the disc,
// where it is already saturated.
float3 lfScreen(float3 a, float3 b) { return 1.0f - (1.0f - saturate(a)) * (1.0f - saturate(b)); }

float4 PSLensFlare(float x : TEXCOORD0, float y : TEXCOORD1) : COLOR
{
	float2 uv  = float2(x, y);
	float4 src = tex2D(tSrc, uv);

	if (fLFFade <= 0.002f) return src;

	const float asp = max(fAspect, 0.001f);

	// --- is there a concentrated sun in this frame, and how hard? ------------
	float3 sunCol = float3(1.0f, 1.0f, 1.0f);
	float  conc   = 1.0f;

	// A UNIFORM branch - the same for every pixel in the frame, so it costs nothing.
	// fLFSamp is how far the disc's UV is INSIDE the captured frame, ramped over the
	// probe's own reach. Past the edge the taps clamp and start measuring the border
	// texel instead of the sky, so the measurement is lerped back toward "assume
	// visible" as it loses meaning, leaving the host budget in charge. A hard switch
	// here would pop the whole flare on as the sun crossed the frame edge.
	if (fLFSamp > 0.002f)
	{
		float2 rp = float2(LF_PROBE / asp, LF_PROBE);
		float2 rr = float2(LF_RING  / asp, LF_RING);

		float3 cen = (tex2D(tSrc, vGRSun).rgb
		            + tex2D(tSrc, vGRSun + float2( rp.x, 0.0f)).rgb
		            + tex2D(tSrc, vGRSun + float2(-rp.x, 0.0f)).rgb
		            + tex2D(tSrc, vGRSun + float2( 0.0f, rp.y)).rgb
		            + tex2D(tSrc, vGRSun + float2( 0.0f,-rp.y)).rgb) * 0.2f;

		// THE SURROUND IS THE DIMMEST OF THE RING, NOT ITS AVERAGE, and that is the one
		// part of this probe that had to be thought about rather than written. The
		// client's own glare sprite has real angular size, and if it spills past the
		// ring then an AVERAGE would be measuring the sprite against itself and the
		// contrast would collapse in deep space - the effect dying exactly where it
		// should be at its best. The minimum finds whichever taps cleared the sprite.
		// It is also the right answer for a mixed scene (a bright limb on one side,
		// black sky on the other): high local contrast is precisely when a lens flares.
		float l0 = dot(tex2D(tSrc, vGRSun + float2( rr.x,        0.0f      )).rgb, float3(0.299f, 0.587f, 0.114f));
		float l1 = dot(tex2D(tSrc, vGRSun + float2(-rr.x,        0.0f      )).rgb, float3(0.299f, 0.587f, 0.114f));
		float l2 = dot(tex2D(tSrc, vGRSun + float2( rr.x * 0.5f, rr.y*0.87f)).rgb, float3(0.299f, 0.587f, 0.114f));
		float l3 = dot(tex2D(tSrc, vGRSun + float2(-rr.x * 0.5f, rr.y*0.87f)).rgb, float3(0.299f, 0.587f, 0.114f));
		float l4 = dot(tex2D(tSrc, vGRSun + float2( rr.x * 0.5f,-rr.y*0.87f)).rgb, float3(0.299f, 0.587f, 0.114f));
		float l5 = dot(tex2D(tSrc, vGRSun + float2(-rr.x * 0.5f,-rr.y*0.87f)).rgb, float3(0.299f, 0.587f, 0.114f));

		float lc = dot(cen, float3(0.299f, 0.587f, 0.114f));
		float ls = min(min(min(l0, l1), min(l2, l3)), min(l4, l5));

		// Squared, so a merely-bright sky never quite gets there and a clean disc
		// against black reaches full strength immediately.
		conc = saturate((lc - ls) * 1.60f);
		conc = conc * conc;

		// Borrow the sun's own hue at half weight, so a low red sun throws a warm
		// flare without the coating's colours being overpainted by it.
		sunCol = lerp(float3(1.0f, 1.0f, 1.0f), cen / max(lc, 1e-3f), 0.5f);

		// Is something SOLID standing in front of the disc? GBUF_DEPTH carries the
		// vessels, the terrain (patch ab) and the base structures (z2); .a is the
		// camera distance in metres and 0 means nothing was drawn, i.e. sky. The
		// > 0.1 guard is this channel's standing convention. (Authored window glass
		// marks .b, not .a, since 2026-09-18 - and this pass is external-only anyway.)
		if (fLFDepth > 0.5f)
		{
			float sky = step(tex2D(tDepth, vGRSun).a, 0.1f)
			          + step(tex2D(tDepth, vGRSun + float2( rp.x, 0.0f)).a, 0.1f)
			          + step(tex2D(tDepth, vGRSun + float2(-rp.x, 0.0f)).a, 0.1f)
			          + step(tex2D(tDepth, vGRSun + float2( 0.0f, rp.y)).a, 0.1f)
			          + step(tex2D(tDepth, vGRSun + float2( 0.0f,-rp.y)).a, 0.1f);
			conc *= sky * 0.2f;
		}

		// ... and hand authority back to the host as the probe runs out of frame.
		const float t = saturate(fLFSamp);
		conc   = lerp(1.0f, conc, t);
		sunCol = lerp(float3(1.0f, 1.0f, 1.0f), sunCol, t);
	}

	if (conc <= 0.002f) return src;

	// --- the chain ----------------------------------------------------------
	// Centred and aspect-corrected, so the ghosts are round on a widescreen viewport
	// and the axis through the screen centre is the real one.
	float2 p = (uv     - 0.5f) * float2(asp, 1.0f);
	float2 s = (vGRSun - 0.5f) * float2(asp, 1.0f);

	const float size = max(fLFSize, 0.02f);
	// Ghost DISTANCES and the blade count are properties of a LENS, so they are baked;
	// what you tune is the look (25i - the sim owns where things land). d = +1 is the
	// sun itself, 0 is the screen centre, negative is the far side of it.
	const float dw = saturate(fLFDisp * 0.5f);   // white -> the coating's own colour

	// ------------------------------------------------------------------------
	// THE LENS. fLFMode picks WHICH OPTIC you are looking through - a different
	// element stack, a different coating, a different aperture - and every slider
	// keeps its meaning across all of them, which is the whole point of doing it
	// this way rather than as three separate effects. Each branch fills the same
	// eight variables and the composite at the bottom is written once.
	//
	// THE BRANCH IS UNIFORM - one value for the entire frame - so it costs nothing
	// at run time; only the compiler pays, in instruction slots for the bodies it
	// will never both take.
	//
	// !! MODE 0 IS ARITHMETICALLY THE BUILD HE APPROVED ON 2026-09-12. Its table,
	// its constants and its composite are untouched; the generalisation is defaults
	// (gain 1, no streak, white veil) that evaluate to exactly what was there. A new
	// lens must never move the one that has already been flown.
	// ------------------------------------------------------------------------
	float3 acc       = 0.0f;
	float  ghostGain = 1.0f;
	float3 rayTint   = float3(1.00f, 0.93f, 0.86f);
	float3 veilTint  = float3(1.00f, 1.00f, 1.00f);
	float  veilAmt   = 0.10f;
	float  veilReach = 2.2f;    // how far the wash spreads; SMALLER covers more frame
	float  blades    = 19.0f;   // iris leaves -> how many rays
	float  raySharp  = 26.0f;   // how thin each one is
	float  ray2W     = 0.45f;   // weight of the second, beating set
	float  ray2Sharp = 44.0f;   // ... and its own thinness. Its OWN variable rather than a
	                            //   multiple of raySharp: a factor of 1.7 would have made
	                            //   mode 0 evaluate 44.2 instead of the 44.0 it was flown
	                            //   with, and "arithmetically unchanged" has to be true
	float  rayReach  = 3.2f;    // radial decay; SMALLER reaches further
	float  streakW   = 0.0f;    // anamorphic horizontal bar, 0 = this lens has none

	if (fLFMode < 0.5f)
	{
		// ---- 0: CLASSIC -----------------------------------------------------
		// Warm-coated stills lens: a few LARGE, varied, well-separated ghosts. The
		// table is MEASURED off his reference frame rather than invented - each
		// ghost's distance along the sun->centre axis and its radius read off that
		// image in units of screen height, which is why the big olive disc sits at
		// d = -1.00 (as far the other side of centre as the sun is this side, where
		// a real lens puts it) and the wide violet annulus past it at -1.30. Entry 1
		// is the warm bloom over the core; entries 3 and 5 are the tiny specks.
		acc += lfElem(p, s *  0.45f, 0.130f * size, 0.20f, 0.00f) * lerp(1.0f.xxx, float3(1.00f, 0.62f, 0.28f), dw);
		acc += lfElem(p, s *  0.22f, 0.055f * size, 0.30f, 0.00f) * lerp(1.0f.xxx, float3(1.00f, 0.55f, 0.18f), dw);
		acc += lfElem(p, s *  0.05f, 0.014f * size, 0.45f, 0.00f) * lerp(1.0f.xxx, float3(0.35f, 1.00f, 0.45f), dw);
		acc += lfElem(p, s * -0.30f, 0.060f * size, 0.28f, 0.00f) * lerp(1.0f.xxx, float3(0.95f, 0.85f, 0.30f), dw);
		acc += lfElem(p, s * -0.55f, 0.030f * size, 0.40f, 0.00f) * lerp(1.0f.xxx, float3(0.35f, 0.55f, 1.00f), dw);
		acc += lfElem(p, s * -0.75f, 0.075f * size, 0.26f, 0.00f) * lerp(1.0f.xxx, float3(1.00f, 0.72f, 0.35f), dw);
		acc += lfElem(p, s * -1.00f, 0.185f * size, 0.14f, 0.00f) * lerp(1.0f.xxx, float3(0.72f, 0.86f, 0.42f), dw);
		acc += lfElem(p, s * -1.30f, 0.340f * size, 0.045f, 0.90f) * lerp(1.0f.xxx, float3(0.62f, 0.55f, 1.00f), dw);
	}
	else if (fLFMode < 1.5f)
	{
		// ---- 1: ANAMORPHIC --------------------------------------------------
		// The cine look, measured off his second reference: cool blue-white, a long
		// HORIZONTAL streak through the source (an anamorphic element is squeezed on
		// one axis, so its flare smears on the other), far more and finer rays, and a
		// long chain of MANY SMALL ghosts of similar size rather than a few big ones -
		// which is the real difference between the two optics, not the colour.
		rayTint  = float3(0.62f, 0.80f, 1.00f);
		veilTint = float3(0.62f, 0.78f, 1.00f);
		veilAmt  = 0.07f;
		blades   = 26.0f; raySharp = 16.0f; ray2Sharp = 30.0f; ray2W = 0.55f; rayReach = 2.4f;
		streakW  = 1.0f;
		acc += lfElem(p, s *  0.35f, 0.020f * size, 0.35f, 0.00f) * lerp(1.0f.xxx, float3(0.75f, 0.85f, 1.00f), dw);
		acc += lfElem(p, s *  0.20f, 0.024f * size, 0.32f, 0.00f) * lerp(1.0f.xxx, float3(0.85f, 0.80f, 0.95f), dw);
		acc += lfElem(p, s *  0.05f, 0.026f * size, 0.30f, 0.00f) * lerp(1.0f.xxx, float3(0.70f, 0.85f, 1.00f), dw);
		acc += lfElem(p, s * -0.10f, 0.029f * size, 0.28f, 0.00f) * lerp(1.0f.xxx, float3(0.90f, 0.85f, 0.80f), dw);
		acc += lfElem(p, s * -0.28f, 0.034f * size, 0.26f, 0.00f) * lerp(1.0f.xxx, float3(0.75f, 0.90f, 0.95f), dw);
		acc += lfElem(p, s * -0.48f, 0.036f * size, 0.26f, 0.00f) * lerp(1.0f.xxx, float3(0.95f, 0.85f, 0.70f), dw);
		acc += lfElem(p, s * -0.70f, 0.022f * size, 0.32f, 0.00f) * lerp(1.0f.xxx, float3(0.70f, 0.80f, 1.00f), dw);
		acc += lfElem(p, s * -0.92f, 0.016f * size, 0.38f, 0.00f) * lerp(1.0f.xxx, float3(0.85f, 0.90f, 1.00f), dw);
		acc += lfElem(p, s * -1.15f, 0.010f * size, 0.50f, 0.00f) * lerp(1.0f.xxx, float3(1.00f, 0.95f, 0.85f), dw);
		acc += lfElem(p, s * -1.40f, 0.049f * size, 0.22f, 0.00f) * lerp(1.0f.xxx, float3(0.70f, 0.85f, 1.00f), dw);
		acc += lfElem(p, s * -1.65f, 0.014f * size, 0.40f, 0.00f) * lerp(1.0f.xxx, float3(0.90f, 0.80f, 0.95f), dw);
		acc += lfElem(p, s * -1.88f, 0.011f * size, 0.45f, 0.00f) * lerp(1.0f.xxx, float3(0.80f, 0.90f, 1.00f), dw);
		// The two wide, nearly-bare rings the chain sits inside.
		acc += lfElem(p, s * -0.55f, 0.245f * size, 0.040f, 0.92f) * lerp(1.0f.xxx, float3(0.55f, 0.95f, 0.75f), dw);
		acc += lfElem(p, s * -1.40f, 0.196f * size, 0.045f, 0.92f) * lerp(1.0f.xxx, float3(0.95f, 0.70f, 0.60f), dw);
	}
	else if (fLFMode < 2.5f)
	{
		// ---- 2: CLEAN -------------------------------------------------------
		// A modern multi-coated optic, and the restrained one on purpose: coatings
		// exist to kill exactly this artefact, so what survives is a crisp star from
		// the aperture, two faint coating reflections and almost no veil. This is the
		// setting for anyone who finds a full flare too much but still wants the sun
		// to read as having been photographed.
		ghostGain = 0.45f;
		rayTint   = float3(1.00f, 0.98f, 0.95f);
		veilAmt   = 0.045f;
		blades    = 14.0f; raySharp = 36.0f; ray2Sharp = 60.0f; ray2W = 0.20f; rayReach = 4.2f;
		acc += lfElem(p, s * -0.55f, 0.070f * size, 0.30f, 0.00f) * lerp(1.0f.xxx, float3(0.80f, 0.90f, 1.00f), dw);
		acc += lfElem(p, s * -1.10f, 0.110f * size, 0.20f, 0.00f) * lerp(1.0f.xxx, float3(0.75f, 0.95f, 0.85f), dw);
		acc += lfElem(p, s * -0.80f, 0.300f * size, 0.050f, 0.95f) * lerp(1.0f.xxx, float3(0.70f, 0.80f, 1.00f), dw);
	}
	else
	{
		// ---- 3: VINTAGE -----------------------------------------------------
		// An UNCOATED optic, and it is a different failure of glass rather than a
		// restyle of the same one - which is why it earns a row instead of being three
		// slider positions of CLASSIC.
		//
		// Every bare air-glass surface reflects about 4% of what hits it, and an old
		// lens has a lot of them, so what you get is not a tidy chain of ghosts: it is
		// VEILING GLARE. Light scattered over the whole frame, blacks lifted, contrast
		// gone. That is the dominant term here (0.30 against CLASSIC's 0.10) and it
		// spreads much further (veilReach 0.85), so the wash reaches the corners.
		//
		// AND THE GHOSTS COME OUT NEARLY COLOURLESS ON PURPOSE - that is physics, not
		// taste. A ghost's colour is thin-film interference in the COATING; with no
		// coating there is no interference and no colour, just soft warm-grey discs
		// (warm because old glass yellows and uncoated transmission is warmer). The
		// nice consequence: DISPERSION barely moves this lens, because there is
		// almost nothing for it to spread. The slider still works, it just has less
		// to say - which is exactly true of the real thing.
		//
		// Few blades and soft ones, the way an old iris was built. This is the one
		// place the 2016 client's "eight fat wedges" shape is the CORRECT answer -
		// the difference is that here they are dim and sit inside a big warm wash
		// instead of being the brightest thing on screen.
		ghostGain = 0.90f;
		rayTint   = float3(1.00f, 0.90f, 0.76f);
		veilTint  = float3(1.00f, 0.88f, 0.70f);
		veilAmt   = 0.30f;  veilReach = 0.85f;
		blades    = 8.0f; raySharp = 20.0f; ray2Sharp = 34.0f; ray2W = 0.15f; rayReach = 2.0f;
		acc += lfElem(p, s *  0.60f, 0.150f * size, 0.60f, 0.00f) * lerp(1.0f.xxx, float3(1.00f, 0.93f, 0.84f), dw);
		acc += lfElem(p, s *  0.30f, 0.095f * size, 0.55f, 0.00f) * lerp(1.0f.xxx, float3(1.00f, 0.90f, 0.78f), dw);
		acc += lfElem(p, s *  0.08f, 0.120f * size, 0.62f, 0.00f) * lerp(1.0f.xxx, float3(0.98f, 0.94f, 0.88f), dw);
		acc += lfElem(p, s * -0.15f, 0.080f * size, 0.58f, 0.00f) * lerp(1.0f.xxx, float3(1.00f, 0.88f, 0.74f), dw);
		acc += lfElem(p, s * -0.38f, 0.145f * size, 0.65f, 0.00f) * lerp(1.0f.xxx, float3(0.96f, 0.92f, 0.86f), dw);
		acc += lfElem(p, s * -0.60f, 0.105f * size, 0.55f, 0.00f) * lerp(1.0f.xxx, float3(1.00f, 0.91f, 0.80f), dw);
		acc += lfElem(p, s * -0.85f, 0.170f * size, 0.60f, 0.00f) * lerp(1.0f.xxx, float3(0.97f, 0.90f, 0.82f), dw);
		acc += lfElem(p, s * -1.10f, 0.130f * size, 0.58f, 0.00f) * lerp(1.0f.xxx, float3(1.00f, 0.94f, 0.86f), dw);
		acc += lfElem(p, s * -1.45f, 0.200f * size, 0.70f, 0.00f) * lerp(1.0f.xxx, float3(0.98f, 0.90f, 0.80f), dw);
		acc += lfElem(p, s * -1.75f, 0.090f * size, 0.55f, 0.00f) * lerp(1.0f.xxx, float3(1.00f, 0.92f, 0.84f), dw);
	}

	// --- the starburst ------------------------------------------------------
	float2 dr  = p - s;
	float  rl  = length(dr);
	float  ang = atan2(dr.y, dr.x);
	float  v1  = pow(0.5f + 0.5f * cos(ang * blades), raySharp);
	// A second set at double the frequency and offset in phase, at under half weight.
	// A single perfectly regular star reads as a decal; two beating sets do not.
	float  v2  = pow(0.5f + 0.5f * cos(ang * (blades * 2.0f) + 1.10f), ray2Sharp);
	float  rays = (v1 + ray2W * v2) * exp(-rl * (rayReach / max(size, 0.05f)));

	// --- the anamorphic streak ----------------------------------------------
	// A thin horizontal bar through the source. Rides the Rays slider, because it is
	// the same thing the aperture does - light spread along one axis rather than
	// scattered evenly - and a lens either has this element or it does not.
	float sy = saturate(1.0f - abs(dr.y) / (0.010f * size));
	float st = sy * sy * sy * exp(-abs(dr.x) * (1.8f / max(size, 0.05f))) * streakW;

	// --- veiling glare ------------------------------------------------------
	// Light scattered across the whole element rather than reflected between two of
	// them: a broad, faint lift around the source. Deliberately not the tight coloured
	// halo of the 2016 effect, which is the thing he singled out.
	float veil = exp(-rl * veilReach) * veilAmt;

	float3 col = acc * (ghostGain * fLFGhost)
	           + (rays + st) * (fLFRay * 0.55f) * rayTint
	           + veil * veilTint;

	col *= sunCol * (fLFStr * fLFFade * conc);

	return float4(lfScreen(src.rgb, col), src.a);
}
