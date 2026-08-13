// ==============================================================
// OroReentry.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - reentry plasma  (its own translation unit)
// ----------------------------------------------------------------------------
// Stock Orbiter's reentry effect is two camera-facing additive billboards from
// Reentry.dds. It reads as a flat brown smear with a circular halo, it reaches
// as far ahead of the nose as behind, and - because billboards illuminate
// nothing - the hull sits DARK inside a supposedly brilliant plasma.
//
// WHAT WE ARE AIMING AT (user's reference, KSP 0.19, reviewed 2026-08-01): the
// plasma is not a sheath or a blob, it is MANY FINE STREAKS that start on the
// windward surface and shear off downstream for many body lengths. The filament
// structure IS the effect - reproduce the silhouette without it and it still
// reads wrong. (Note the reference does NOT vary plasma hue by planet, which is
// what I had assumed from the title: it stays orange -> yellow-white throughout
// and only the sky behind it changes.)
//
// ROUND 2.5 WIDENS THAT AIM (forum feedback, DaveS + STS footage, 2026-08-01):
// filaments are the ONSET and FADE look. At PEAK heating the real thing is a
// near-solid emissive envelope - "the coma of a comet" - the vehicle swallowed
// inside a teardrop of light that reads opaque because it is BRIGHT, not because
// it blocks anything: enough overlapping additive layers saturate the frame,
// which is exactly what the night footage shows. Heat crossfades the two regimes
// (see `coma` in BuildPlasmaGeometry); the hundreds-of-km persistent trail from
// the same feedback is the next round, not this one.
//
// ROUND 3 (2026-08-01) IS A CLEAN SLATE ON THE DRAW LIST: recreate the Starship
// VFX reference (20 frames studied) and NOTHING else. Every RING-based element
// accreted over rounds 2.0-2.8 - teardrop envelope, coma fill, silhouette rim
// band, glow coat, stagnation core, wake blobs - is DELETED: stacked together
// they read as a translucent bubble wrapped around the ship with a detached
// pulsing oval at the nose (user's screenshots), which the reference never
// shows. What draws now is exactly the reference's anatomy: a per-hull-point
// EDGE LIGHT (white, magenta-fringed, on the windward silhouette) + a faint
// face wash, the STREAMS (root flares, striations, sparks), and the knob-gated
// trail. Infrastructure - heat model, stock suppression, hull sampling, the
// projection pipeline, buffer padding - is untouched.
// (THE TRAIL IS GONE, 2026-08-02. It survived rounds 2.6 through 5.12 and never
// stopped being fragile, for a structural reason: it was the only element built
// on state that ACCUMULATED across seconds. Long streaks - length knob to 20 -
// carry the downstream story now. Everything below that still says "trail" is
// history, not code.)
//
// THE MECHANISM IS ORBITER'S OWN PARTICLE SYSTEM. PARTICLESTREAMSPEC's EMISSIVE
// mode is documented with the example "plasma stream", and `atmslowdown` makes
// particles decelerate against the air so a fast vessel simply leaves them
// behind - the trail forms itself. That is the same mechanism the reference uses,
// and it is a fraction of the work of hand-rolling particles in a render proc.
//
// THE ANGLE-OF-ATTACK PROBLEM AND ITS FIX. Spaceplanes reenter at ~40 deg AoA, so
// the hot face is the BELLY, not the nose. AddParticleStream's position and
// direction are fixed at creation and explicitly cannot be redefined - but its
// LEVEL pointer can be modulated continuously. So we attach a FAN of emitters
// spread over the hull and each frame set each one's level from how windward it
// is (dot of its outward direction against the airflow). Particles then stream
// from whichever face is into the wind, following AoA with no recreation.
//
// THREE THINGS MAKE THIS DIFFERENT FROM THE REST OF ORO:
//  1. NO CLIENT PATCH. AddParticleStream / AddPointLight / SetReentryTexture are
//     core Orbiter API, not gcCore. This family would run on a stock client.
//  2. PER-VESSEL, which ORO has never been - all vessels are tracked, so you can
//     watch another ship come down.
//  3. BOTH DOMAINS. The hull light is external; the cockpit glow is screen-space
//     (see the note on plasmaGlow below). Compare invariant 10, where every other
//     effect picks a side.
//
// !! THE TABLE'S ADDRESSES ARE LOAD-BEARING !! SetPositionRef / SetIntensityRef /
// AddParticleStream all bind Orbiter to OUR variables and require them to stay
// valid for the object's lifetime. The table is a fixed array of slots that are
// never compacted or moved - a std::vector would dangle every live light and
// stream the moment it reallocated - and a slot must delete its streams BEFORE it
// is reused. hV == NULL marks a free slot.
//
// MAIN THREAD ONLY (clbkPreStep). Makes VESSEL calls - never reachable from the
// render callback (invariant 1).
// ============================================================================

#include "OroModule.h"
#include "OroState.h"
#include "gcCoreAPI.h"       // gcCore::SuppressReentry (client patch c)

#include <math.h>
#include <string.h>          // memset (the vertex-buffer tail pad)
#include <vector>            // BuildShell PRECOMPUTE only (transient, once per
#include <unordered_map>     // vessel) - never in per-frame paths
#include <unordered_set>

namespace {

	// Heat curve. Sutton-Graves stagnation heating is q ~ sqrt(rho / Rn) * v^3 - note the
	// SQUARE ROOT of density, where stock uses rho * v^3 straight. That moves the peak up
	// the trajectory, so the glow builds high and thin rather than waiting for thick air.
	// Rn (effective nose radius) is exposed by nothing; GetSize() is documented as
	// unrelated to the visual mesh, so rather than fake a precision we do not have, Rn is
	// folded into these two thresholds. Both are lab-tunable and the dialog shows the
	// resulting heat, so a bad threshold shows up as a number rather than as "nothing".
	const double Q_ON   = 2.0e9;    // first visible glow
	const double Q_FULL = 1.2e10;   // full plasma (~peak heating on a shallow entry)

	const double V_MIN     = 1000.0;  // [m/s] cheap reject
	const double STANDOFF  = 0.75;    // stagnation point, in units of GetSize(), UPSTREAM
	const double RANGE_K   = 12.0;    // light range in units of GetSize()
	const double RANGE_MIN = 150.0;   // [m]
	const double INTENS_K  = 2.5;     // light intensity at full heat, before the dialog trim

	// Round 2.5.1 (user's call, 2026-08-01): the dialog trim gets 10x HEADROOM - at the
	// old full trim the whole effect read as "weak", and with additive blending the way
	// to brightness is saturation, which IS the coma look. Slider 10 now reproduces the
	// old slider 100 exactly. Per-vertex alpha clamps at 255 (PCol) and the cockpit
	// shader saturate()s its own input (PSPlasma), so the top of the range brightens
	// the LAYERING, not any single element past its ceiling. Applied at EVERY point
	// the reentry family consumes g_fx.reentry - geometry, hull light, cockpit glow -
	// so the slider keeps one meaning.
	const float REN_TRIM_GAIN = 10.0f;

	inline double sat01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

	// Plasma colour by heat. STAYS IN THE ORANGE FAMILY all the way up: an earlier version
	// went blue-white at 70% and the ship visibly turned GREY at peak heating, which is
	// exactly backwards (rejected 2026-08-01). Blue-white belongs to interplanetary-return
	// speeds, not a LEO entry. COLOUR4 is fixed when an emitter is created - LightEmitter
	// has no SetColour - so a hue change means recreating the light; three bands with a
	// dead zone keeps that to two or three recreations across an entire entry.
	const COLOUR4 BAND_COL[3] = {
		{ 1.00f, 0.30f, 0.08f, 1.0f },   // first heat - deep orange-red
		{ 1.00f, 0.55f, 0.20f, 1.0f },   // building   - orange
		{ 1.00f, 0.80f, 0.55f, 1.0f },   // peak       - yellow-white, still warm
	};

	int HeatBand(float heat, int cur)
	{
		const float H = 0.05f;
		float lo = 0.33f, hi = 0.70f;
		if      (cur == 1) { lo -= H; hi += H; }   // widen the band we are already in
		else if (cur == 0) { lo += H; }
		else if (cur == 2) { hi -= H; }
		if (heat < lo) return 0;
		if (heat < hi) return 1;
		return 2;
	}

	// The emitter fan: the 12 vertices of an icosahedron, normalised. Evenly spread over
	// the sphere, so whatever attitude the vessel holds, some of them face into the wind.
	// (phi = 1.6180339887, |(1,phi,0)| = 1.9021130326)
	const VECTOR3 EMIT_DIR[12] = {
		{  0.0000,  0.5257,  0.8507 }, {  0.0000,  0.5257, -0.8507 },
		{  0.0000, -0.5257,  0.8507 }, {  0.0000, -0.5257, -0.8507 },
		{  0.5257,  0.8507,  0.0000 }, {  0.5257, -0.8507,  0.0000 },
		{ -0.5257,  0.8507,  0.0000 }, { -0.5257, -0.8507,  0.0000 },
		{  0.8507,  0.0000,  0.5257 }, { -0.8507,  0.0000,  0.5257 },
		{  0.8507,  0.0000, -0.5257 }, { -0.8507,  0.0000, -0.5257 },
	};

	const double EMIT_R    = 0.55;    // emitter ring radius, in units of GetSize()
	const double WIND_POW  = 1.5;     // windward falloff - higher = tighter fan on the hot face
	const double HEAT_POW  = 0.60;    // heat -> emission shaping; <1 so the first streaks
	                                  // arrive early and softly

	float ReentryHeat(VESSEL* v)
	{
		const double rho = v->GetAtmDensity();
		if (rho <= 0.0) return 0.0f;
		const double vel = v->GetAirspeed();
		if (vel < V_MIN) return 0.0f;
		const double q = sqrt(rho) * vel * vel * vel;
		return (float)sat01((q - Q_ON) / (Q_FULL - Q_ON));
	}

	// Would STOCK draw its own reentry effect right now? The SUPPRESSION window must
	// cover STOCK's visibility window, which is not ours. All three stock triggers,
	// read out of the sources (2026-08-01, after the "puffs at 89 km" report):
	//  - client billboards (OVP VVessel.cpp RenderReentry):      rho * v^3 >= 1.05e8
	//  - the core's DEFAULT particle stream, given to EVERY
	//    vessel (Vessel.cpp SetDefaultReentryStream, ATM_PLOG):  0.5 * rho^0.6 * v^3 >= ~2e8
	//  - Atlantis's own entry stream (Atlantis.cpp, ATM_PLIN
	//    6e7..12e7) - the EARLIEST of the family, ~95 km at
	//    entry speed:                                            0.5 * rho^0.6 * v^3 >= ~6e7
	// Our Sutton-Graves sqrt(rho) glow peaks HIGHER up the trajectory than all of
	// these, so gating the slot on OUR heat both starts too late (stock puffed from
	// ~89 km while Q_ON waited for ~79 km) and lets go too early (stock billboards
	// outlive our heat at the bottom of the entry). `margin` scales the thresholds:
	// enlist at TRACK_ON (suppression in place BEFORE anything of stock's shows, with
	// the 0.5 s scan latency inside the margin), release at TRACK_OFF - the gap
	// between them is hysteresis, so the boundary cannot flap on the rescan cadence.
	const double TRACK_ON  = 0.35;
	const double TRACK_OFF = 0.25;

	bool StockReentryWants(VESSEL* v, double margin)
	{
		const double rho = v->GetAtmDensity();
		if (rho <= 0.0) return false;
		const double vel = v->GetAirspeed();
		if (vel < 250.0) return false;   // below this neither formula can reach its
		                                 // threshold at any density (margins >= 0.25)
		const double v3 = vel * vel * vel;
		if (rho * v3 >= 1.05e8 * margin) return true;                 // billboards
		if (0.5 * pow(rho, 0.6) * v3 >= 6.0e7 * margin) return true;  // particle streams
		return false;
	}

	// Project a GLOBAL position to viewport UV, for the cockpit glow. Orbiter's camera
	// frame looks along +z and oapiCameraAperture() is the VERTICAL SEMI-aperture, so the
	// frustum half-height at depth z is z*tan(ap) and the half-width is that x aspect.
	// (Same construction as the exhaust shimmer's projector.) Returns false behind camera.
	bool ProjectUV(const VECTOR3& gpos, double aspect, float& u, float& vv)
	{
		VECTOR3 cpos; oapiCameraGlobalPos(&cpos);
		MATRIX3 Rcam; oapiCameraRotationMatrix(&Rcam);
		const VECTOR3 c = tmul(Rcam, gpos - cpos);
		const double tanAp = tan(oapiCameraAperture());
		if (c.z < 0.1) return false;
		u  = (float)(0.5 + 0.5 * ((c.x / c.z) / (tanAp * aspect)));
		vv = (float)(0.5 - 0.5 * ((c.y / c.z) /  tanAp));    // UV y grows downward
		return true;
	}

	// ------------------------------------------------------------------------
	// Plasma GEOMETRY helpers (round 2)
	// ------------------------------------------------------------------------
	const float WAKE_LIFE = 3.4f;     // [s] blob lifetime (sim time - it is a physical thing)

	// (the TRAIL constants - cadence, catch-up cap, TrailLife() - were removed with
	// the trail itself on 2026-08-02.)

	// (the round-2.5 coma regime constants lived here - retired with the whole
	// ring-based layer stack in round 3, the clean-slate rebuild)

	// Round 2.1: the particle-fan spark under-layer is OFF. Even dialled to 40% the
	// dust-textured sprites read as brown smoke against the geometry's orange (user's
	// screenshots, 2026-08-01) - Orbiter offers no way to recolour them. The machinery
	// stays; flip this if a use for faint sparks returns.
	const bool REENTRY_SPARKS = false;

	// VC BRIGHTNESS, reworked 2026-08-07 alongside the patch-(g) depth clip.
	// THE PER-PIXEL BRIGHTNESS NEVER CHANGED THIS ROUND - the AREA did. Before the clip
	// the VC plasma painted the whole cabin, so a flat 0.85 visibility read as plenty;
	// now it is confined to the window apertures and the same 0.85 reads as barely there.
	// So two separate things, deliberately split:
	//   VC_VIS  - the old flat stand-in for occlusion. From inside the hull every outward
	//             normal faces away, so the facing fades would blank the show (round 3.5) -
	//             hence a constant. The depth clip now does that job properly, per pixel,
	//             so the artificial reduction has nothing left to model: it goes to 1.0.
	//   VC_GAIN - an honest brightness gain for the sheet and the ribbons, to pay back the
	//             area the clip correctly took away.
	// ⚠️ VC_GAIN IS NOT APPLIED TO THE ORIGIN GLOWS OR THE SPARKS. Both clamp to 235
	// BEFORE this multiply, so a gain there cannot brighten a gradient - it can only push
	// an already-saturated interior past 255 and flatten it into the hard-edged disc that
	// round 3.3 removed ("the balls grew with the Reentry plasma slider"). They still get
	// the 0.85 -> 1.0 visibility lift, which is safe because it lands under the clamp.
	const float VC_VIS  = 1.00f;
	const float VC_GAIN = 3.00f;   // shell sheet + stream ribbons only
	                               // 2.00 -> 3.00 (2026-08-07, his call after flying 2.00)

	// VC ORIGIN-GLOW SKIRT LIFT (2026-08-07, after flying VC_GAIN 3.0: "the glows look
	// dull now"). VC_GAIN put the ribbons at 3.5x while the glows stayed at 1.18x, which
	// inverted the design's own hierarchy - the glow is the ANCHOR ("the money shot is not
	// the streams alone but their ORIGIN"), so a root that reads as a dull spot inside a
	// bright streak is backwards.
	// ⚠️ THE LIFT CANNOT GO ON THE CORE, and that is the whole reason these two constants
	// exist instead of one glow gain. aF is clamped to 235 and Ag reaches 561, so every
	// strong stream's core is ALREADY PINNED: raising it buys 1.09x and spends it on
	// exactly the saturated flat disc with the creeping hard edge that round 3.3 removed.
	// The headroom is all in the SKIRT - halo 0.26 (alpha ~61 of 255) and bloom 0.09
	// (~21 of 255). Lifting those reads as both brighter AND bigger without touching rF,
	// which is the other lever 3.3 warned about. Layer order (core > halo > bloom) holds.
	const float VC_GLOW_HALO  = 0.60f;   // vs 0.26 external -> alpha ~141
	const float VC_GLOW_BLOOM = 0.24f;   // vs 0.09 external -> alpha ~56

	// THE SHELL AS A VOLUME - concentric copies of the hull shell, additively stacked.
	// See the long note at the draw site. Offsets are MULTIPLES of the Shock dist knob,
	// so his own standoff setting still sets the scale and the stack spreads around it:
	// BACK TO ONE LAYER (2026-08-08, user's call after the standoff was baked at
	// 0.015: "do we really need 3 shell copies?"). The 3-layer volume (invariant
	// 15a, added the same day) was tuned for LARGE standoffs; at 0.015 the copies
	// nearly coincide and stop earning their ~6k triangles, and the SHOCK ENVELOPE
	// now carries the volume story. 1/sqrt(N) alpha law kept - N=1 makes it 1.0,
	// so the single sheet keeps the stack's perceived brightness.
	const int   SHELL_LAYERS       = 1;
	const float SHELL_LAYER_OFF[1] = { 1.00f };
	const float SHELL_LAYER_A      = 1.0f;      // 1/sqrt(1)

	// Camera context, fetched ONCE per build (each field is an oapi call).
	struct CamCtx { VECTOR3 pos; MATRIX3 rot; double tanAp; };
	void GetCam(CamCtx& c)
	{
		oapiCameraGlobalPos(&c.pos);
		oapiCameraRotationMatrix(&c.rot);
		c.tanAp = tan(oapiCameraAperture());
	}

	// Global position -> viewport PIXELS + camera depth. False behind the camera.
	bool ProjPx(const CamCtx& cc, const VECTOR3& gpos, DWORD vw, DWORD vh,
	            float& px, float& py, double& z)
	{
		const VECTOR3 c = tmul(cc.rot, gpos - cc.pos);
		if (c.z < 1.0) return false;
		z = c.z;
		const double aspect = vh ? (double)vw / (double)vh : 1.0;
		px = (float)((0.5 + 0.5 * (c.x / c.z) / (cc.tanAp * aspect)) * vw);
		py = (float)((0.5 - 0.5 * (c.y / c.z) /  cc.tanAp) * vh);
		return true;
	}

	inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }

	// Cheap deterministic hash -> 0..1 (per-ribbon / per-blob variation).
	inline float hashf(float s)
	{
		const float t = sinf(s * 12.9898f) * 43758.547f;
		return t - floorf(t);
	}

	// Pack 0xAABBGGRR (invariant 5).
	// Pack, with CLAMPING - the palette shift below can drive a channel out of
	// range, and the old bare `& 0xFF` would have wrapped -85 to 171 (a green
	// spike in the middle of a red plume).
	inline DWORD PColRaw(int r, int g, int b, int a)
	{
		if (a < 0) a = 0; else if (a > 255) a = 255;
		if (r < 0) r = 0; else if (r > 255) r = 255;
		if (g < 0) g = 0; else if (g > 255) g = 255;
		if (b < 0) b = 0; else if (b > 255) b = 255;
		return ((DWORD)a << 24) | ((DWORD)b << 16) | ((DWORD)g << 8) | (DWORD)r;
	}

	// PALETTE SATURATION (round 5.6, REWORKED 2026-08-07). One knob for how vivid the
	// whole palette is. 1.0 is identity.
	// ⚠️ IT USED TO BE A GAIN ON THE DISTANCE FROM RED, not a saturation, and that is worth
	// recording because the reasoning was sound and the result still wrong. 5.6 argued: red
	// pins at 255 through most of the effect, so hue is decided by how far g and b sit below
	// r - make the knob a gain on exactly that distance. True, but the consequence is that
	// BELOW 1.0 every colour is dragged toward (255,255,255): the reference orange lands on
	// salmon at 0.75, not on a milder orange. And ABOVE 1.0 g and b hit zero and everything
	// collapses to pure red. Both ends were reported from the sim, and both are that line.
	// It is a real HSV saturation now, around each colour's own value: below 1.0 drifts
	// toward its own grey, above 1.0 deepens toward its own pure hue. The 5.5 palette laws
	// are untouched - they are about the RAMP CONSTANTS and additive stacking, not this knob.
	float g_palSat = 1.0f;               // set once per build from the dialog knob

	// THE SECOND COLOUR (2026-08-07): the plasma's MAGENTA CAST, picked separately from the
	// overall Tint. There was never a single hard-coded magenta to expose - the cast turns up
	// in the edge light's fringe (255,120,225), the origin glow's corona (255,85,125), the
	// shell's windward shoulder and the streak roots, all different colours serving one idea.
	// So this does not patch those four sites. It rides round 5.5's OWN test for the cast -
	// A COLOUR IS MAGENTA EXACTLY WHEN b > g - and weights the tint by how far past green the
	// blue runs. Every magenta in the effect follows automatically; orange (b < g) and
	// white-hot (b ~ g) samples are untouched BY CONSTRUCTION, so it cannot wash the embers
	// or grey the hot core, and there is no band edge for it to tear at because the weight is
	// continuous in the colour itself. A site added later inherits it for free.
	// THE COLOUR MODEL IS HUE-BASED (2026-08-07). What was here before was a per-channel
	// MULTIPLY, and a multiply cannot recolour this palette - every ramp pins r at 255, so
	// scaling channels can only ever darken toward red. Picking light blue produced a
	// slightly darker salmon, which is exactly what was reported. Hue rotation can, and it
	// preserves the whole internal structure the round-5 arc bought: the hot/cool contrast,
	// the white core staying white (rotation does nothing to an unsaturated colour), and the
	// magenta cast keeping its offset from the body colour.
	// Rotation in DEGREES, derived from the picker in ReloadPalette below.
	float g_palHueRot = 0.0f;    // whole palette
	float g_palHueRot2 = 0.0f;   // extra rotation for the MAGENTA CAST only

	// RGB <-> HSV. Cheap, and PCol runs ~10k times a frame (the shell caches per vertex).
	inline void RgbToHsv(float r, float g, float b, float& h, float& s, float& v)
	{
		const float mx = (r > g ? (r > b ? r : b) : (g > b ? g : b));
		const float mn = (r < g ? (r < b ? r : b) : (g < b ? g : b));
		const float d  = mx - mn;
		v = mx;
		s = (mx > 1e-6f) ? d / mx : 0.0f;
		if (d < 1e-6f) { h = 0.0f; return; }        // grey: hue undefined, and rotating it
		if      (mx == r) h = 60.0f * fmodf((g - b) / d + 6.0f, 6.0f);   // must stay a no-op
		else if (mx == g) h = 60.0f * ((b - r) / d + 2.0f);
		else              h = 60.0f * ((r - g) / d + 4.0f);
	}
	inline void HsvToRgb(float h, float s, float v, float& r, float& g, float& b)
	{
		h = fmodf(h, 360.0f); if (h < 0.0f) h += 360.0f;
		const float c = v * s;
		const float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
		const float m = v - c;
		if      (h <  60.0f) { r = c; g = x; b = 0; }
		else if (h < 120.0f) { r = x; g = c; b = 0; }
		else if (h < 180.0f) { r = 0; g = c; b = x; }
		else if (h < 240.0f) { r = 0; g = x; b = c; }
		else if (h < 300.0f) { r = x; g = 0; b = c; }
		else                 { r = c; g = 0; b = x; }
		r += m; g += m; b += m;
	}

	inline DWORD PCol(int r, int g, int b, int a)
	{
		// Magenta-ness on the RAW palette colour, BEFORE anything is applied. The b > g test
		// (round 5.5's own definition of the cast) only identifies it in the palette's own
		// frame - measuring it after a rotation, or after the old saturation shift, scored
		// almost the whole palette at zero and was why the Fringe pick barely registered.
		const float m = clampf((float)(b - g) / 128.0f, 0.0f, 1.0f);
		float h, s, v;
		RgbToHsv((float)r / 255.0f, (float)g / 255.0f, (float)b / 255.0f, h, s, v);
		h += g_palHueRot + g_palHueRot2 * m;
		// A REAL saturation now, around the colour's own value rather than toward red.
		// The old form was g = r - (r - g) * k, which with r pinned at 255 dragged the whole
		// palette toward WHITE below 1.0 (hence salmon at 0.75) and to pure RED above it
		// (hence red at 2.0). k = 1 is identity in both, so a default look is unchanged.
		s = clampf(s * g_palSat, 0.0f, 1.0f);
		float fr, fg, fb;
		HsvToRgb(h, s, v, fr, fg, fb);
		return PColRaw((int)(fr * 255.0f + 0.5f), (int)(fg * 255.0f + 0.5f),
		               (int)(fb * 255.0f + 0.5f), a);
	}

	// The picker -> rotation. WHAT YOU PICK IS THE HUE YOU GET: the family lands on the
	// picked hue in full, whether the pick is pale or vivid. Grey and white are the identity,
	// so the default is the reference palette exactly and no saved setting changes meaning.
	// ⚠️ SCALING THE ANGLE BY THE PICK'S SATURATION WAS TRIED AND IS WRONG. It reads as a
	// reasonable "how far to shift", but a partial rotation lands on an intermediate HUE, not
	// on a partial version of the target: a pale blue (sat 0.41) rotated 41% of the way from
	// orange came out GREEN, and a pale cyan came out YELLOW. Picking a colour and getting a
	// different one is the exact complaint this rework exists to fix, so the angle is never
	// scaled. Use the Saturation knob for how vivid, not the pick.
	float HueRotFromPick(DWORD col, float refHue)
	{
		float h, s, v;
		RgbToHsv((float)( col        & 0xFF) / 255.0f,
		         (float)((col >>  8) & 0xFF) / 255.0f,
		         (float)((col >> 16) & 0xFF) / 255.0f, h, s, v);
		if (s < 0.04f) return 0.0f;          // grey/white: hue is meaningless, leave it alone
		float d = h - refHue;
		while (d >  180.0f) d -= 360.0f;
		while (d < -180.0f) d += 360.0f;
		return d;
	}
	// Reference hues: the palette's own body ORANGE (255,85,30) and its MAGENTA (255,120,225).
	// Picking a colour maps the corresponding family ONTO that colour.
	const float PAL_HUE_BODY   =  15.0f;
	const float PAL_HUE_FRINGE = 313.0f;

	// The plasma colour ramp: white-hot -> orange -> violet. The KSP-reference family -
	// it stays WARM through the whole middle and only the thin fading tails go violet.
	// (An earlier light went blue-white at high heat and the ship turned grey at peak -
	// rejected; that mistake is not repeated here.)
	void PlasmaRamp(float t, int& r, int& g, int& b)
	{
		struct K { float t; int r, g, b; };
		static const K key[4] = {
			{ 0.00f, 255, 246, 228 },   // white-hot
			{ 0.35f, 255, 150,  48 },   // orange
			{ 0.75f, 186,  92, 232 },   // violet
			{ 1.00f, 118,  58, 188 },   // deep violet (fading tail)
		};
		t = clampf(t, 0.0f, 1.0f);
		int k = 0;
		while (k < 2 && t > key[k + 1].t) k++;
		const float u = (t - key[k].t) / (key[k + 1].t - key[k].t);
		r = key[k].r + (int)((key[k + 1].r - key[k].r) * u);
		g = key[k].g + (int)((key[k + 1].g - key[k].g) * u);
		b = key[k].b + (int)((key[k + 1].b - key[k].b) * u);
	}

}  // namespace

// ----------------------------------------------------------------------------
// Slot management. Slots are NEVER moved (see the header note).
// ----------------------------------------------------------------------------
int OroModule::ReentryFindSlot(OBJHANDLE h) const
{
	for (int i = 0; i < MAX_RENTRY; i++)
		if (rentry[i].hV == h) return i;
	return -1;
}

// The windward particle fan. One emitter per icosahedron direction, positioned on a
// shell around the hull and pointing outward; the level pointers are driven per frame.
void OroModule::ReentryMakeStreams(int i, VESSEL* v)
{
	ReentryVessel& e = rentry[i];
	if (e.streamsMade) return;
	const double size = v->GetSize() > 1.0 ? v->GetSize() : 1.0;

	// Sizes scale with the vessel, so the spec is PER SLOT (and its address is stable).
	// Round-2 note: these are UNDER-LAYER values - small, sparse, short-lived sparks
	// beneath the custom-drawn geometry, not the main effect they briefly tried to be.
	e.pspec.flags       = 0;
	e.pspec.srcsize     = size * 0.06;
	e.pspec.srcrate     = 22.0;                 // per emitter, at level 1
	e.pspec.v0          = 6.0;                  // gentle push off the surface...
	e.pspec.srcspread   = 0.25;
	e.pspec.lifetime    = 0.30;
	e.pspec.growthrate  = size * 0.22;          // sparks broaden slightly as they cool
	e.pspec.atmslowdown = 6.0;                  // ...then the AIR does the work: particles
	                                            // brake hard, the vessel runs out from under
	                                            // them, and the trail forms itself
	e.pspec.ltype       = PARTICLESTREAMSPEC::EMISSIVE;   // SDK's own example: "plasma stream"
	e.pspec.levelmap    = PARTICLESTREAMSPEC::LVL_SQRT;   // low heat still shows
	e.pspec.lmin        = 0.0;
	e.pspec.lmax        = 1.0;
	e.pspec.atmsmap     = PARTICLESTREAMSPEC::ATM_FLAT;
	e.pspec.amin        = 0.0;
	e.pspec.amax        = 1.0;
	e.pspec.tex         = NULL;                 // Orbiter's default particle texture

	int made = 0;
	for (int k = 0; k < N_EMIT; k++) {
		e.lvl[k]  = 0.0;
		e.strm[k] = v->AddParticleStream(&e.pspec, EMIT_DIR[k] * (size * EMIT_R),
		                                 EMIT_DIR[k], &e.lvl[k]);
		if (e.strm[k]) made++;
	}
	e.streamsMade = true;
	// The Launchpad has a global "particle streams" switch; with it off every Add returns
	// NULL and the module is required to cope. Say so once - otherwise the plasma silently
	// having no streaks looks like our bug.
	if (made == 0 && !reentryNoParticles) {
		oapiWriteLogV("ORO: particle streams are disabled in the Launchpad - reentry plasma "
		              "will show its light and cockpit glow but no streaks.");
		reentryNoParticles = true;
	}
}

// MUST run before a slot is reused or the module unloads: Orbiter keeps reading the lvl[]
// pointers we handed it, so a surviving stream would read freed memory.
void OroModule::ReentryKillStreams(int i, VESSEL* v)
{
	ReentryVessel& e = rentry[i];
	for (int k = 0; k < N_EMIT; k++) {
		if (e.strm[k]) {
			if (v) v->DelExhaustStream(e.strm[k]);
			e.strm[k] = NULL;
		}
		e.lvl[k] = 0.0;
	}
	e.streamsMade = false;
}

// Drop everything a slot borrowed and free it. `live` says whether the vessel is still
// callable: on clbkDeleteVessel it is (the handle dies after we return), but a vessel that
// vanished without a callback must not be touched.
void OroModule::ReentryFreeSlot(int i, bool live)
{
	if (i < 0 || i >= MAX_RENTRY) return;
	ReentryVessel& e = rentry[i];

	// oapiIsVessel is the real guard - `live` is only a caller's extra veto. At session
	// teardown vessels are being destroyed around us, and calling into one that has already
	// gone is a use-after-free; leaking on a vessel that is about to die costs nothing.
	const bool ok = live && e.hV && oapiIsVessel(e.hV);
	VESSEL* v = ok ? oapiGetVesselInterface(e.hV) : nullptr;

	ReentryKillStreams(i, v);
	if (e.light && v) v->DelLightEmitter(e.light);
	// Hand stock's reentry billboards back. This goes through gcCore, NOT the vessel: the
	// suppression lives in the CLIENT's own list (see below), and it must be cleared even
	// if the vessel is already gone - the client would otherwise keep suppressing a handle
	// that will be reused.
	if (e.stockOff && pCore) pCore->SuppressReentry(e.hV, false);

	e.hV        = NULL;
	e.light     = nullptr;
	e.heat      = 0.0f;
	e.band      = -1;
	e.intensity = 0.0;
	e.pos       = _V(0, 0, 0);
	e.stockOff  = false;
	e.blobT     = 0.0;
	for (int b = 0; b < MAX_BLOB; b++) e.blob[b].age = -1.0f;
	// Trail spawn cursor only - the PARTICLES ARE NOT KILLED. They are air parcels
	// with no vessel reference, and outliving the vessel is the point: a breakup
	// addon deletes the burning ship mid-entry, and its train must keep glowing
	// while the debris pieces grow their own. They expire on their own clock.
	e.trailSeen = false;
	e.trailAcc  = 0.0;
	e.trailS    = 0.0;
	e.trailRef  = NULL;
	e.trailSeq  = 0;
	e.nHull       = 0;
	e.hullSampled = false;
	e.nShellV     = 0;
	e.nShellT     = 0;
	e.shellBuilt  = false;
}

void OroModule::ReleaseReentry()
{
	for (int i = 0; i < MAX_RENTRY; i++)
		if (rentry[i].hV) ReentryFreeSlot(i, true);
	g_fx.reentryHeat = 0.0f;
	plasmaGlow       = 0.0f;
	plasVtxN         = 0;      // nothing for the render proc to draw
	// The trail pool DOES clear here, unlike on a slot death: this is the disarm /
	// pill-off / sim-end path, where every effect vanishes at once (Ctrl+G law), and
	// at sim end it also stops stale GLOBAL positions leaking ghost particles into
	// the next scenario's sky for their remaining lifetime.
	for (int k = 0; k < TRAIL_MAX; k++) trail[k].age = -1.0f;
	trailVtxN        = 0;
}

// A vessel is about to be destroyed. Its handle is valid for the length of this call and
// no longer, so everything we borrowed from it MUST go now.
void OroModule::ReentryForget(OBJHANDLE h)
{
	const int i = ReentryFindSlot(h);
	if (i >= 0) ReentryFreeSlot(i, true);
}

// Create (or recreate, on a colour-band change) the hull light. EXTERNAL ONLY: this used
// to be VIS_ALWAYS to light the cockpit too, which technically worked but looked wrong -
// Orbiter's local lights have no occlusion, so at 40 deg AoA the belly plasma shone up
// through the FLOOR. The cockpit gets a screen-space glow instead; see UpdateReentry.
void OroModule::ReentryMakeLight(int i, VESSEL* v, int band)
{
	ReentryVessel& e = rentry[i];

	// ⚠️ INVARIANT 23(k), AND THIS SITE WAS MISSED WHEN IT WAS WRITTEN (2026-08-12).
	// A light emitter is a LONG-LIVED OBJECT handed to a VESSEL: Vessel::AddPointLight
	// grows the vessel's emitter list with a new[], exactly the shape that threw
	// std::bad_array_new_length on a scenario reload when AddExhaustStream did it. The
	// rule as written is "does it give someone else an object that outlives the call?
	// Then it waits" - and 23(k) was applied only to the particles because that is where
	// the bisection happened to point. Reading state is fine during load; lending is not.
	//
	// Returning WITHOUT touching e.band is deliberate: the caller retries while
	// (!e.light || band != e.band), so the light simply arrives on the first frame after
	// the scene is real, and the entry is diagnosable instead of silent.
	if (!sceneRendered) {
		if (!lendDeferLogged) {
			lendDeferLogged = true;
			oapiWriteLogV("ORO: deferring AddPointLight (reentry hull light) - scene has not "
			              "rendered a frame yet (invariant 23k). Will attach once it has.");
		}
		return;
	}

	if (e.light) { v->DelLightEmitter(e.light); e.light = nullptr; }

	const double size = v->GetSize() > 1.0 ? v->GetSize() : 1.0;
	double range = size * RANGE_K;
	if (range < RANGE_MIN) range = RANGE_MIN;
	const COLOUR4 col = BAND_COL[(band < 0 || band > 2) ? 1 : band];

	// I = 1 / (att0 + d*att1 + d^2*att2): near-constant across the hull, falling away
	// over a few hundred metres.
	e.light = v->AddPointLight(e.pos, range, 1e-3, 0.0, 1e-3, col, col, col);
	if (e.light) {
		e.light->SetVisibility(LightEmitter::VIS_EXTERNAL);
		e.light->SetPositionRef(&e.pos);         // stable slot storage - never moved
		e.light->SetIntensityRef(&e.intensity);
	}
	e.band = band;
}

// ----------------------------------------------------------------------------
// Per-frame update. Main thread (clbkPreStep).
// ----------------------------------------------------------------------------
void OroModule::UpdateReentry()
{
	// Master arm and the WORLD pill both kill it - and killing it must HAND BACK
	// everything borrowed: lights, particle streams, and stock's own reentry texture.
	// Same obligation the camera shake carries in invariant 9.
	if (!(g_fx.masterArmed && g_fx.reentryEnabled && g_fx.reentry > 0.001f)) {
		ReleaseReentry();
		return;
	}

	// One-shot report on whether we can hide the stock billboards at all. VESSEL::
	// SetReentryTexture(NULL) is the documented way and it DOES NOT WORK under D3D9Client:
	// vVessel::RenderReentry gates only on the client's own globally-loaded defreentrytex
	// and never reads vessel->reentry.do_render (which the INLINE renderer does honour) nor
	// the user's CfgVisualPrm.bReentryFlames option. Verified in the client source
	// 2026-08-01 after the stock smear survived our first suppression attempt. Client patch
	// (c) adds gcCore::SuppressReentry to close that hole; without it our plasma simply
	// composites over stock's, which is degraded but not broken.
	if (!reentrySuppressChecked) {
		reentrySuppressChecked = true;
		reentryCanSuppress = (pCore && pCore->CanSuppressReentry());
		if (!reentryCanSuppress)
			oapiWriteLogV("ORO: this D3D9Client cannot suppress stock reentry effects "
			              "(billboards need client patch c, particle puffs patch e) - "
			              "our plasma will draw OVER them.");
	}

	// Periodic rescan for vessels that have STARTED heating. Reentry is a minutes-long
	// event, so half a second of latency is invisible and the scan costs nothing.
	// (clbkNewVessel alone is not enough: it is explicitly NOT sent for vessels created by
	// the scenario at session start, which is most of them. The scan covers both, so only
	// the DELETE callback is needed - and that one is mandatory, see ReentryForget.)
	reentryScanT += oapiGetSysStep();
	if (reentryScanT >= 0.5) {
		reentryScanT = 0.0;
		const DWORD n = oapiGetVesselCount();
		for (DWORD k = 0; k < n; k++) {
			OBJHANDLE h = oapiGetVesselByIndex(k);
			if (!h || ReentryFindSlot(h) >= 0) continue;
			VESSEL* v = oapiGetVesselInterface(h);
			// Enlist when OUR glow wants it OR when STOCK's own effect is about to
			// show - the slot is what carries the suppression, so it must exist
			// BEFORE stock's first puff, which fires far above Q_ON (see
			// StockReentryWants).
			if (!v || (ReentryHeat(v) <= 0.0f && !StockReentryWants(v, TRACK_ON))) continue;
			const int slot = ReentryFindSlot(NULL);      // first free
			if (slot < 0) {
				if (!reentryFullWarned) {
					oapiWriteLogV("ORO: reentry table full (%d vessels) - further vessels unlit.", MAX_RENTRY);
					reentryFullWarned = true;
				}
				break;
			}
			rentry[slot].hV   = h;
			rentry[slot].band = -1;
		}
	}

	OBJHANDLE hFocus = oapiGetFocusObject();
	OBJHANDLE hCam   = oapiCameraTarget();
	float camHeat = 0.0f;
	plasmaGlow = 0.0f;
	plasVtxN   = 0;                          // rebuilt below for the camera-target vessel
	const double simdt = oapiGetSimStep();   // SIM time: wake blobs are physical objects

	// (rounds 3-4: NOTHING trail-related happens in pre-step any more. State moved
	// to clbkPostStep -> UpdateTrailPost; projection moved all the way into the
	// RENDER PATH with patch (k)'s camera - the round-3 diagnostic proved the
	// camera is stale even at post-step, so only the renderer knows the truth.)

	for (int i = 0; i < MAX_RENTRY; i++) {
		ReentryVessel& e = rentry[i];
		if (!e.hV) continue;
		VESSEL* v = oapiGetVesselInterface(e.hV);
		if (!v) { ReentryFreeSlot(i, false); continue; }      // vanished without a callback

		e.heat = ReentryHeat(v);
		// Free only when OUR glow is done AND stock's own effect is clearly out of
		// its envelope (TRACK_OFF < TRACK_ON = hysteresis) - releasing on heat alone
		// handed stock its billboards back for the tail of the entry, where rho*v^3
		// is still hot after sqrt(rho)*v^3 has died.
		if (e.heat <= 0.0f && !StockReentryWants(v, TRACK_OFF)) {
			ReentryFreeSlot(i, true);                     // cooled - give it all back
			continue;
		}
		if (e.hV == hCam) camHeat = e.heat;

		// Build the shock shell EARLY (round 4): the one-frame precompute cost
		// lands the moment heating BEGINS - before any plasma is on screen.
		if (!e.shellBuilt && e.heat > 0.005f) BuildShell(i, v);

		// Suppress stock's reentry effects the moment this vessel is ours - the
		// billboards (client patch c) AND the reentry particle streams (patch e; the
		// core gives EVERY vessel a default one, and e.g. Atlantis adds its own).
		// Restored in ReentryFreeSlot, which every exit path goes through.
		if (!e.stockOff && reentryCanSuppress) { pCore->SuppressReentry(e.hV, true); e.stockOff = true; }
		if (REENTRY_SPARKS && !e.streamsMade) ReentryMakeStreams(i, v);

		// The airflow. Everything below is oriented by THIS, not by the nose - at 40 deg
		// AoA they are 40 deg apart, and that gap is the whole reason stock looks wrong.
		VECTOR3 va;
		VECTOR3 flow = _V(0, 0, 1);
		if (v->GetAirspeedVector(FRAME_LOCAL, va)) {
			const double L = length(va);
			if (L > 1.0) flow = va / L;
		}

		// Stagnation point: upstream along the flow, on the windward face.
		e.pos = flow * (v->GetSize() * STANDOFF);
		// A slot can now exist with NO heat (suppression-only, tracking stock's
		// window) - borrow no light for it. One that already has a light keeps it
		// and just dims to zero through the intensity ref.
		// THE STAGNATION LIGHT, on its own knob at last (round 5.6). It is the one
		// piece of the effect that was never tunable, and the one whose contribution
		// is easiest to lose track of: it does NOT light our own geometry - the shell
		// and streaks are emissive triangles with baked colours - so everything it
		// does is on the vessel's REAL mesh. Which is not nothing: the shell only
		// covers WINDWARD triangles, so the sides and the leeward hull are lit by
		// this and by nothing else, and a vessel that is not the camera target gets
		// no geometry at all (BuildPlasmaGeometry is camera-target only) and is
		// carried by this light alone.
		// At zero the emitter is DELETED, not dimmed: "off" should mean the vessel is
		// not carrying a borrowed light, not carrying one set to zero.
		const float lightGain = clampf(g_fx.plasLight, 0.0f, 2.0f);
		if (lightGain < 0.001f) {
			if (e.light) { v->DelLightEmitter(e.light); e.light = nullptr; e.band = -1; }
		} else if (e.heat > 0.0f) {
			const int band = HeatBand(e.heat, e.band);
			if (!e.light || band != e.band) ReentryMakeLight(i, v, band);
		}
		e.intensity = INTENS_K * pow((double)e.heat, 0.7)
		            * (double)(g_fx.reentry * REN_TRIM_GAIN) * (double)lightGain;

		// THE FAN (only if the spark under-layer is enabled - see REENTRY_SPARKS).
		// Each emitter's level is how windward it is: dot(its outward direction, the
		// flow), so the sparks live on the face actually into the wind.
		if (e.streamsMade) {
			const double drive = 0.40 * pow((double)e.heat, HEAT_POW) * (double)g_fx.reentry;
			for (int k = 0; k < N_EMIT; k++) {
				const double d = dotp(EMIT_DIR[k], flow);
				e.lvl[k] = (d > 0.0) ? drive * pow(d, WIND_POW) : 0.0;
			}
		}

		// (wake-blob advection lived here - RETIRED in round 3 with the blob draw)
		// (TRAIL shedding lived here for one round - moved to UpdateTrailPost with
		//  the rest of the trail, so sheds anchor to the vessel's POST-step position,
		//  the one the frame actually renders.)

		// The custom-drawn plasma (envelope + filaments + wake) is built for the
		// CAMERA-TARGET vessel only - it is the one filling the screen, and distant
		// ships are carried by their stagnation light alone.
		if (e.hV == hCam && (extGate || vcGate)) {   // 3.5: geometry now also serves the VC
			if (!e.hullSampled) SampleHull(i, v);
			if (!e.shellBuilt) BuildShell(i, v);     // fallback trigger (round 4)
			BuildPlasmaGeometry(i, v, flow);
		}

		// COCKPIT GLOW (focus vessel only) - screen-space, computed here because the render
		// callback makes no oapi calls (invariant 1). A point light was tried for this and
		// rejected: with no occlusion it lights the cabin through the floor.
		if (e.hV == hFocus && viewW > 0 && viewH > 0) {
			VECTOR3 gpos; v->Local2Global(e.pos, gpos);
			float u = 0.5f, vv = 0.5f;
			const double aspect = (double)viewW / (double)viewH;
			if (!ProjectUV(gpos, aspect, u, vv)) {
				// Plasma is behind the camera - keep the ambient lift, park the blob well
				// off-screen so only the wash remains.
				u = 0.5f; vv = 2.5f;
			}
			plasmaUV[0] = u;
			plasmaUV[1] = vv;
			plasmaGlow  = e.heat * g_fx.reentry * REN_TRIM_GAIN;   // PSPlasma saturate()s this
			const COLOUR4 c = BAND_COL[(e.band < 0 || e.band > 2) ? 1 : e.band];
			plasmaCol[0] = c.r; plasmaCol[1] = c.g; plasmaCol[2] = c.b;
		}
	}

	// Zero-pad the vertex buffer's unused tail (alpha 0, degenerate at origin).
	// The render proc hands the client the FULL buffer every frame: the client's
	// D3D9Triangle::Update Locks with D3DLOCK_DISCARD (fresh UNINITIALIZED memory
	// each lock) and its Draw always draws the CREATION count - an unwritten tail
	// is random VRAM drawn as random flashing triangles (the "green flashes",
	// 2026-08-01). Invariant 3's dark-spots rule, enforced for the plasma.
	// plasDepth is padded with it: the depth array is handed over at the same full count,
	// and a tail of uninitialised distances would clip live triangles unpredictably.
	if (plasVtxN > 0 && plasVtxN < PLAS_MAX_TRI * 3) {
		memset(&plasVtx[plasVtxN],   0, sizeof(PlasVtx) * (PLAS_MAX_TRI * 3 - plasVtxN));
		memset(&plasDepth[plasVtxN], 0, sizeof(float)   * (PLAS_MAX_TRI * 3 - plasVtxN));
	}
	// (the trail buffer's twin pad moved to UpdateTrailPost with the trail - round 3)

	g_fx.reentryHeat = camHeat;
}

// ----------------------------------------------------------------------------
// Sample the vessel's OWN mesh into the hull point field (positions + normals,
// vessel-local). THE hull-conformance mechanism, and it needs NO client patch:
// the mesh is never rendered, only READ - a walk over the mesh TEMPLATES (which
// the SDK forbids modifying; we don't touch them). External-visible meshes only,
// stride-sampled so wings, tail and pods all contribute points, plus each mesh's
// vessel-frame offset. Main thread only.
//
// Sampled ONCE per slot, lazily (first time the vessel is the camera target): an
// external mesh set practically never changes mid-entry, and a stale field would
// only misplace some glow. Cleared when the slot is freed.
// ----------------------------------------------------------------------------
void OroModule::SampleHull(int i, VESSEL* v)
{
	ReentryVessel& e = rentry[i];
	e.nHull = 0;
	e.hullSampled = true;

	// The heatshield override applies here too ("the shell and anything else we
	// need" - the user's spec): when the authored envelope exists, the hull point
	// field samples IT, so the edge light and every hull-point consumer see the
	// same clean geometry the shell is built from. Same lookup as BuildShell.
	MESHHANDLE hOverride = NULL;
	{
		char cls[64];  OroClassFileName(v->GetClassNameA(), cls, sizeof(cls));
		char mfile[160]; sprintf_s(mfile, "Meshes\\ORO\\%s.msh", cls);
		if (GetFileAttributesA(mfile) != INVALID_FILE_ATTRIBUTES) {
			char mres[96]; sprintf_s(mres, "ORO\\%s", cls);
			hOverride = oapiLoadMeshGlobal(mres);
		}
	}

	// Pass 1: count candidate vertices so the stride lands near MAX_HULLPT.
	const UINT nm = hOverride ? 1 : v->GetMeshCount();
	DWORD total = 0;
	for (UINT m = 0; m < nm; m++) {
		MESHHANDLE hM;
		if (hOverride) hM = hOverride;
		else {
			if (!(v->GetMeshVisibilityMode(m) & MESHVIS_EXTERNAL)) continue;
			hM = v->GetMeshTemplate(m);
			if (!hM) continue;                   // dynamically-created mesh - no template
		}
		const DWORD ng = oapiMeshGroupCount(hM);
		for (DWORD g = 0; g < ng; g++) {
			MESHGROUP* gr = oapiMeshGroup(hM, g);
			if (gr) total += gr->nVtx;
		}
	}
	const DWORD stride = (total > (DWORD)MAX_HULLPT) ? (total / MAX_HULLPT + 1) : 1;

	// Pass 2: collect every stride-th vertex, skipping degenerate normals.
	DWORD walk = 0;
	for (UINT m = 0; m < nm && e.nHull < MAX_HULLPT; m++) {
		MESHHANDLE hM;
		VECTOR3 ofs = _V(0, 0, 0);
		if (hOverride) hM = hOverride;
		else {
			if (!(v->GetMeshVisibilityMode(m) & MESHVIS_EXTERNAL)) continue;
			hM = v->GetMeshTemplate(m);
			if (!hM) continue;
			v->GetMeshOffset(m, ofs);
		}
		const DWORD ng = oapiMeshGroupCount(hM);
		for (DWORD g = 0; g < ng && e.nHull < MAX_HULLPT; g++) {
			MESHGROUP* gr = oapiMeshGroup(hM, g);
			if (!gr || !gr->Vtx) continue;
			for (DWORD k = 0; k < gr->nVtx && e.nHull < MAX_HULLPT; k++, walk++) {
				if (walk % stride) continue;
				const NTVERTEX& n = gr->Vtx[k];
				const double nl = (double)n.nx * n.nx + (double)n.ny * n.ny + (double)n.nz * n.nz;
				if (nl < 0.25) continue;         // no usable normal
				const VECTOR3 p = _V(n.x + ofs.x, n.y + ofs.y, n.z + ofs.z);
				// Templates hold the UN-ANIMATED pose: gear / control-surface vertices can
				// sit where the runtime animation moves them FROM, which may be nowhere
				// near the real hull - one such root gave a streak firing from empty space
				// (user's screenshots, 2026-08-01). Anything outside the official bounding
				// radius is suspect; drop it.
				const double sz = v->GetSize() > 1.0 ? v->GetSize() : 1.0;
				if (length(p) > sz * 1.15) continue;
				HullPt& hp = e.hull[e.nHull++];
				hp.pos = p;
				hp.nrm = _V(n.nx, n.ny, n.nz);   // template normals are unit-ish; good enough
			}
		}
	}

	// Fallback: nothing readable (all-NULL templates). Synthesize a bounding shell so
	// the effect still runs - it just degrades to the pre-conformance look.
	if (e.nHull < 12) {
		e.nHull = 0;
		const double R = (v->GetSize() > 1.0 ? v->GetSize() : 1.0) * 0.55;
		for (int k = 0; k < N_EMIT; k++) {       // reuse the icosahedron directions
			HullPt& hp = e.hull[e.nHull++];
			hp.pos = EMIT_DIR[k] * R;
			hp.nrm = EMIT_DIR[k];
		}
		oapiWriteLogV("ORO: no readable mesh for hull sampling - plasma uses a bounding shell.");
	}
}

// ----------------------------------------------------------------------------
// ROUND 4: the SHOCK SHELL precompute. The mesh templates expose their full
// TRIANGLE INDEX LISTS (MESHGROUP.Idx - confirmed in OrbiterAPI.h), so instead
// of guessing the windward surface from sparse point samples (the splat and
// heightfield eras, both dead), we take the author's own triangulation and
// DECIMATE it: vertices weld into spatial-grid clusters (hemisphere-split so
// thin wings keep separate top/bottom skins), source triangles collapse onto
// cluster triples, duplicates drop, and what remains is a <=2800-triangle copy
// of the real hull with INHERITED connectivity. Grid cell sizing is derived
// from SOURCE vertex statistics (so the ISS decimates per-module instead of to
// mush), overflow coarsens the cell and retries (loudly-partial only as the
// very last resort), crumpled clusters (cancelling interior detail) are culled,
// near-coincident same-side clusters are stitched (hemisphere splits at hard
// creases would otherwise leave cracks), degenerate/zero-area triangles drop,
// and TRUE open mesh boundaries are flagged so the per-frame pass can feather
// them. Runs ONCE per slot, main thread, triggered the moment heat crosses
// 0.005 - before any plasma is visible. Transient STL is allowed HERE only.
// ----------------------------------------------------------------------------
#define SHELL_CULL_CRUMPLED 1

void OroModule::BuildShell(int i, VESSEL* v)
{
	ReentryVessel& e = rentry[i];
	e.nShellV = 0; e.nShellT = 0;
	e.shellBuilt = true;                     // one attempt per slot life
	const double size = v->GetSize() > 1.0 ? v->GetSize() : 1.0;

	// Fallback: a 16x8 UV sphere (224 tris), radial normals, no open edges.
	// LOUDLY logged - a smooth bubble on screen must never be misread as the
	// main path regressing.
	auto fallbackSphere = [&]() {
		const double R = size * 0.55;
		const int SEG = 16, RING = 7;
		int nv = 0;
		auto addV = [&](const VECTOR3& p) {
			e.shellPos[nv * 3 + 0] = (float)p.x;
			e.shellPos[nv * 3 + 1] = (float)p.y;
			e.shellPos[nv * 3 + 2] = (float)p.z;
			const VECTOR3 n = unit(p);
			e.shellNrm[nv * 3 + 0] = (float)n.x;
			e.shellNrm[nv * 3 + 1] = (float)n.y;
			e.shellNrm[nv * 3 + 2] = (float)n.z;
			e.shellFlg[nv] = 0;
			nv++;
		};
		addV(_V(0, 0, R));
		addV(_V(0, 0, -R));
		for (int r = 1; r <= RING; r++) {
			const double th = 3.14159265 * r / (RING + 1);
			for (int s = 0; s < SEG; s++) {
				const double ph = 6.2831853 * s / SEG;
				addV(_V(R * sin(th) * cos(ph), R * sin(th) * sin(ph), R * cos(th)));
			}
		}
		int nt = 0;
		auto ringAt = [&](int r, int s) -> WORD { return (WORD)(2 + (r - 1) * SEG + (s % SEG)); };
		for (int s = 0; s < SEG; s++) {
			e.shellIdx[nt * 3 + 0] = 0; e.shellIdx[nt * 3 + 1] = ringAt(1, s);        e.shellIdx[nt * 3 + 2] = ringAt(1, s + 1);    nt++;
			e.shellIdx[nt * 3 + 0] = 1; e.shellIdx[nt * 3 + 1] = ringAt(RING, s + 1); e.shellIdx[nt * 3 + 2] = ringAt(RING, s);     nt++;
		}
		for (int r = 1; r < RING; r++) for (int s = 0; s < SEG; s++) {
			const WORD a = ringAt(r, s), b = ringAt(r, s + 1), c2 = ringAt(r + 1, s), d = ringAt(r + 1, s + 1);
			e.shellIdx[nt * 3 + 0] = a; e.shellIdx[nt * 3 + 1] = b; e.shellIdx[nt * 3 + 2] = d; nt++;
			e.shellIdx[nt * 3 + 0] = a; e.shellIdx[nt * 3 + 1] = d; e.shellIdx[nt * 3 + 2] = c2; nt++;
		}
		e.nShellV = nv; e.nShellT = nt;
		oapiWriteLogV("ORO: no readable mesh - shock shell is a bounding sphere (%s).", v->GetName());
	};

	// --- PER-CLASS HEATSHIELD OVERRIDE (2026-08-08, the user's design - and
	// exactly Firefly's own "envelope" system: their wiki has part authors ship a
	// shrinkwrapped clean envelope mesh with the raw model as fallback, because
	// raw meshes carry gear wells, antennas and greebles the shock should never
	// resolve. The DG's template is authored GEAR DOWN, so the welded shell keeps
	// stubs where the wells are - no smoothing budget can remove what the source
	// geometry insists on, but an authored envelope can.)
	//   Meshes\ORO\<class>.msh   (same name sanitiser as Config\ORO\<class>.cfg,
	// so the cfg and the mesh always pair up). Authored clean: gear up, holes
	// closed, ~5-10% inflated, geometry lowered. When present it is the ONLY
	// source for the shell; when absent, the vessel's own meshes are walked
	// exactly as before. Release-notes material: addon authors ship one to
	// upgrade their vessel's reentry look, no code involved.
	MESHHANDLE hOverride = NULL;
	{
		char cls[64];  OroClassFileName(v->GetClassNameA(), cls, sizeof(cls));
		char mfile[160]; sprintf_s(mfile, "Meshes\\ORO\\%s.msh", cls);
		if (GetFileAttributesA(mfile) != INVALID_FILE_ATTRIBUTES) {
			char mres[96]; sprintf_s(mres, "ORO\\%s", cls);
			hOverride = oapiLoadMeshGlobal(mres);    // core-cached template; never ours to delete
			if (hOverride)
				oapiWriteLogV("ORO: heatshield mesh %s - shell source override (%s).",
				              mfile, v->GetName());
		}
	}

	// --- Step 0: source scan (SampleHull's walk, but with TRIANGLES) ----------
	struct SrcMesh { MESHHANDLE hM; VECTOR3 ofs; };
	std::vector<SrcMesh> src;
	const UINT nm = hOverride ? 1 : v->GetMeshCount();
	int nSrc = 0;
	VECTOR3 bbMin = _V(1e9, 1e9, 1e9), bbMax = _V(-1e9, -1e9, -1e9);
	for (UINT m = 0; m < nm; m++) {
		MESHHANDLE hM;
		VECTOR3 ofs = _V(0, 0, 0);
		if (hOverride) {
			hM = hOverride;                  // authored envelope, vessel-origin frame
		} else {
			if (!(v->GetMeshVisibilityMode(m) & MESHVIS_EXTERNAL)) continue;
			hM = v->GetMeshTemplate(m);
			if (!hM) continue;               // load-on-demand mesh - no template
			v->GetMeshOffset(m, ofs);
		}
		bool any = false;
		const DWORD ng = oapiMeshGroupCount(hM);
		for (DWORD g = 0; g < ng; g++) {
			MESHGROUP* gr = oapiMeshGroup(hM, g);
			if (!gr || !gr->Vtx || !gr->Idx || gr->nIdx < 3) continue;
			any = true;
			for (DWORD k = 0; k < gr->nVtx; k++) {
				const VECTOR3 p = _V(gr->Vtx[k].x + ofs.x, gr->Vtx[k].y + ofs.y, gr->Vtx[k].z + ofs.z);
				if (length(p) > size * 1.15) continue;        // the gear landmine
				nSrc++;
				if (p.x < bbMin.x) bbMin.x = p.x; if (p.x > bbMax.x) bbMax.x = p.x;
				if (p.y < bbMin.y) bbMin.y = p.y; if (p.y > bbMax.y) bbMax.y = p.y;
				if (p.z < bbMin.z) bbMin.z = p.z; if (p.z > bbMax.z) bbMax.z = p.z;
			}
		}
		if (any) src.push_back({ hM, ofs });
	}
	if (nSrc < 12) { fallbackSphere(); return; }

	double span = bbMax.x - bbMin.x;
	if (bbMax.y - bbMin.y > span) span = bbMax.y - bbMin.y;
	if (bbMax.z - bbMin.z > span) span = bbMax.z - bbMin.z;
	if (span < 0.1) span = size;

	// --- Step 1: starting cell from SOURCE statistics (ISS-aware). Round 5.2 put
	// the FLOOR back where round 4 had it and then some (4.1 went to size/96, i.e.
	// ~0.29 m on the DG - fine enough to weld the landing gear into its own
	// clusters and draw it). size/36 is ~0.54 m: the shock envelope keeps the
	// wings, fuselage and nose and stops resolving anything hand-sized. The
	// attempt ladder still coarsens further if a mesh overflows the caps.
	const float divf = clampf(sqrtf((float)nSrc / 2.6f), 12.0f, 36.0f);
	double cell = span / (double)divf;
	if (cell < size / 36.0) cell = size / 36.0;
	if (cell > size / 12.0) cell = size / 12.0;

	std::unordered_map<unsigned long long, int> cmap;
	std::vector<VECTOR3> cSum, cN;
	std::vector<int>     cCnt;
	std::vector<float>   cNL;
	std::vector<int>     tlist;
	std::unordered_set<unsigned long long> tset;
	cmap.reserve(SHELL_MAX_VTX * 4);
	cSum.reserve(SHELL_MAX_VTX + 8); cN.reserve(SHELL_MAX_VTX + 8);
	cCnt.reserve(SHELL_MAX_VTX + 8); cNL.reserve(SHELL_MAX_VTX + 8);
	tlist.reserve(SHELL_MAX_TRI * 3); tset.reserve(SHELL_MAX_TRI * 2);

	const VECTOR3 bias = _V(size * 2.0, size * 2.0, size * 2.0);   // floor-quantize on
	                                                               // non-negative coords
	bool partial = false;
	int attempt = 0;

	// --- Steps 2+3: cluster, up to 8 attempts, cell *= 1.30 on overflow -------
	for (attempt = 0; attempt < 8; attempt++) {
		cmap.clear(); cSum.clear(); cN.clear(); cCnt.clear(); cNL.clear();
		tlist.clear(); tset.clear();
		partial = false;
		bool overflow = false;
		const bool lastTry = (attempt == 7);

		auto clusterOf = [&](const VECTOR3& p, const VECTOR3& bn) -> int {
			int hemi;                        // dominant axis+sign: thin skins stay 2-sided
			const double ax = fabs(bn.x), ay = fabs(bn.y), az = fabs(bn.z);
			if (ax >= ay && ax >= az) hemi = (bn.x >= 0) ? 0 : 1;
			else if (ay >= az)        hemi = (bn.y >= 0) ? 2 : 3;
			else                      hemi = (bn.z >= 0) ? 4 : 5;
			const long long qx = (long long)floor((p.x + bias.x) / cell);
			const long long qy = (long long)floor((p.y + bias.y) / cell);
			const long long qz = (long long)floor((p.z + bias.z) / cell);
			const unsigned long long key =
				((unsigned long long)(qx & 0xFFFFF) << 43) |
				((unsigned long long)(qy & 0xFFFFF) << 23) |
				((unsigned long long)(qz & 0xFFFFF) << 3) | (unsigned long long)hemi;
			auto it = cmap.find(key);
			if (it != cmap.end()) return it->second;
			if ((int)cSum.size() >= SHELL_MAX_VTX) {
				if (!lastTry) { overflow = true; return -2; }
				// endgame: weld into a face-neighbour cell's cluster if one exists
				static const long long NB[6][3] =
					{ {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
				for (int n2 = 0; n2 < 6; n2++) {
					const unsigned long long k2 =
						((unsigned long long)((qx + NB[n2][0]) & 0xFFFFF) << 43) |
						((unsigned long long)((qy + NB[n2][1]) & 0xFFFFF) << 23) |
						((unsigned long long)((qz + NB[n2][2]) & 0xFFFFF) << 3) |
						(unsigned long long)hemi;
					auto i2 = cmap.find(k2);
					if (i2 != cmap.end()) return i2->second;
				}
				partial = true;              // drop this triangle; log below
				return -2;
			}
			const int id = (int)cSum.size();
			cmap.emplace(key, id);
			cSum.push_back(_V(0, 0, 0)); cN.push_back(_V(0, 0, 0));
			cCnt.push_back(0); cNL.push_back(0.0f);
			return id;
		};

		for (size_t ms = 0; ms < src.size() && !overflow; ms++) {
			const VECTOR3 ofs = src[ms].ofs;
			const DWORD ng = oapiMeshGroupCount(src[ms].hM);
			for (DWORD g = 0; g < ng && !overflow; g++) {
				MESHGROUP* gr = oapiMeshGroup(src[ms].hM, g);
				if (!gr || !gr->Vtx || !gr->Idx || gr->nIdx < 3) continue;
				const DWORD ntr = gr->nIdx / 3;
				for (DWORD ti = 0; ti < ntr && !overflow; ti++) {
					const WORD ia = gr->Idx[ti * 3], ib = gr->Idx[ti * 3 + 1], ic2 = gr->Idx[ti * 3 + 2];
					if (ia >= gr->nVtx || ib >= gr->nVtx || ic2 >= gr->nVtx) continue;
					const NTVERTEX& va = gr->Vtx[ia];
					const NTVERTEX& vb = gr->Vtx[ib];
					const NTVERTEX& vc = gr->Vtx[ic2];
					const VECTOR3 pa = _V(va.x + ofs.x, va.y + ofs.y, va.z + ofs.z);
					const VECTOR3 pb = _V(vb.x + ofs.x, vb.y + ofs.y, vb.z + ofs.z);
					const VECTOR3 pc = _V(vc.x + ofs.x, vc.y + ofs.y, vc.z + ofs.z);
					const double gearLim = size * 1.15;
					if (length(pa) > gearLim || length(pb) > gearLim || length(pc) > gearLim) continue;
					const VECTOR3 fn = crossp(pb - pa, pc - pa);   // area-weighted
					const double fnl = length(fn);
					if (fnl < 1e-9) continue;
					// bucket normals: the stored one if usable, else the face normal
					VECTOR3 na = _V(va.nx, va.ny, va.nz);
					VECTOR3 nb2 = _V(vb.nx, vb.ny, vb.nz);
					VECTOR3 nc2 = _V(vc.nx, vc.ny, vc.nz);
					if (dotp(na, na) < 0.25) na = fn;
					if (dotp(nb2, nb2) < 0.25) nb2 = fn;
					if (dotp(nc2, nc2) < 0.25) nc2 = fn;
					const int ca = clusterOf(pa, na);  if (overflow) break;
					const int cb = clusterOf(pb, nb2); if (overflow) break;
					const int cc2 = clusterOf(pc, nc2); if (overflow) break;
					if (ca < 0 || cb < 0 || cc2 < 0) continue;
					cSum[ca] = cSum[ca] + pa; cCnt[ca]++;
					cSum[cb] = cSum[cb] + pb; cCnt[cb]++;
					cSum[cc2] = cSum[cc2] + pc; cCnt[cc2]++;
					cN[ca] = cN[ca] + fn; cNL[ca] += (float)fnl;
					cN[cb] = cN[cb] + fn; cNL[cb] += (float)fnl;
					cN[cc2] = cN[cc2] + fn; cNL[cc2] += (float)fnl;
					if (ca == cb || cb == cc2 || ca == cc2) continue;   // collapsed =
					                                                    // the decimation
					int s0 = ca, s1 = cb, s2 = cc2, sw;
					if (s0 > s1) { sw = s0; s0 = s1; s1 = sw; }
					if (s1 > s2) { sw = s1; s1 = s2; s2 = sw; }
					if (s0 > s1) { sw = s0; s0 = s1; s1 = sw; }
					const unsigned long long tk = ((unsigned long long)s0 << 42)
					                            | ((unsigned long long)s1 << 21)
					                            | (unsigned long long)s2;
					if (!tset.insert(tk).second) continue;
					if ((int)tlist.size() / 3 >= SHELL_MAX_TRI) {
						if (!lastTry) { overflow = true; break; }
						partial = true;
						continue;
					}
					tlist.push_back(ca); tlist.push_back(cb); tlist.push_back(cc2);
				}
			}
		}
		if (!overflow) break;
		if (attempt == 6) {
			// measured cell for the final attempt, instead of blind geometric growth
			double f = sqrt((double)cSum.size() / (0.9 * SHELL_MAX_VTX));
			if (f < 1.1) f = 1.1;
			cell *= f;
		} else cell *= 1.30;
	}

	const int NC = (int)cSum.size();
	if (NC < 3 || (int)tlist.size() < 9) { fallbackSphere(); return; }

	std::vector<VECTOR3> cMean(NC);
	for (int c = 0; c < NC; c++)
		cMean[c] = (cCnt[c] > 0) ? cSum[c] / (double)cCnt[c] : _V(0, 0, 0);

	// --- Step 4: crumpled-cluster cull (cancelling interior detail) -----------
	std::vector<char> cDead(NC, 0);
#if SHELL_CULL_CRUMPLED
	for (int c = 0; c < NC; c++) {
		if (cNL[c] > 1e-9f && (float)length(cN[c]) / cNL[c] < 0.30f) cDead[c] = 1;
	}
#endif

	// --- Step 5: crack-stitch + degenerate/area filters -----------------------
	std::vector<int> uf(NC);
	for (int c = 0; c < NC; c++) uf[c] = c;
	auto ufind = [&](int x) -> int {
		while (uf[x] != x) { uf[x] = uf[uf[x]]; x = uf[x]; }
		return x;
	};
	{
		std::unordered_map<unsigned long long, std::vector<int>> shash;
		shash.reserve(NC * 2);
		auto skey = [](long long qx, long long qy, long long qz) -> unsigned long long {
			return ((unsigned long long)(qx & 0x1FFFFF) << 42) |
			       ((unsigned long long)(qy & 0x1FFFFF) << 21) |
			        (unsigned long long)(qz & 0x1FFFFF);
		};
		std::vector<long long> cq(NC * 3);
		for (int c = 0; c < NC; c++) {
			cq[c * 3 + 0] = (long long)floor((cMean[c].x + bias.x) / cell);
			cq[c * 3 + 1] = (long long)floor((cMean[c].y + bias.y) / cell);
			cq[c * 3 + 2] = (long long)floor((cMean[c].z + bias.z) / cell);
			if (!cDead[c]) shash[skey(cq[c * 3], cq[c * 3 + 1], cq[c * 3 + 2])].push_back(c);
		}
		const double stitchR2 = (0.6 * cell) * (0.6 * cell);
		for (int c = 0; c < NC; c++) {
			if (cDead[c]) continue;
			for (long long ox = -1; ox <= 1; ox++)
			for (long long oy = -1; oy <= 1; oy++)
			for (long long oz = -1; oz <= 1; oz++) {
				auto it = shash.find(skey(cq[c * 3] + ox, cq[c * 3 + 1] + oy, cq[c * 3 + 2] + oz));
				if (it == shash.end()) continue;
				for (int d : it->second) {
					if (d <= c || cDead[d]) continue;
					const VECTOR3 dd = cMean[d] - cMean[c];
					if (dotp(dd, dd) > stitchR2) continue;
					const double n1 = length(cN[c]), n2 = length(cN[d]);
					// same-side normals stitch (heals crease splits); thin skins
					// (dot ~ -1) correctly stay split
					if (n1 > 1e-9 && n2 > 1e-9 && dotp(cN[c], cN[d]) / (n1 * n2) > -0.2) {
						const int r1 = ufind(c), r2 = ufind(d);
						if (r1 != r2) uf[r2] = r1;
					}
				}
			}
		}
	}
	for (int c = 0; c < NC; c++) {           // merge accumulators into roots
		const int r = ufind(c);
		if (r == c) continue;
		cSum[r] = cSum[r] + cSum[c]; cCnt[r] += cCnt[c];
		cN[r] = cN[r] + cN[c];       cNL[r] += cNL[c];
	}
	for (int c = 0; c < NC; c++)
		if (ufind(c) == c && cCnt[c] > 0) cMean[c] = cSum[c] / (double)cCnt[c];

	std::vector<int> fin;
	fin.reserve(tlist.size());
	tset.clear();
	const double areaMin = 1e-6 * size * size;
	for (size_t ti = 0; ti + 2 < tlist.size(); ti += 3) {
		const int a = ufind(tlist[ti]), b = ufind(tlist[ti + 1]), c2 = ufind(tlist[ti + 2]);
		if (cDead[a] || cDead[b] || cDead[c2]) continue;
		if (a == b || b == c2 || a == c2) continue;
		int s0 = a, s1 = b, s2 = c2, sw;
		if (s0 > s1) { sw = s0; s0 = s1; s1 = sw; }
		if (s1 > s2) { sw = s1; s1 = s2; s2 = sw; }
		if (s0 > s1) { sw = s0; s0 = s1; s1 = sw; }
		const unsigned long long tk = ((unsigned long long)s0 << 42)
		                            | ((unsigned long long)s1 << 21) | (unsigned long long)s2;
		if (!tset.insert(tk).second) continue;
		if (0.5 * length(crossp(cMean[b] - cMean[a], cMean[c2] - cMean[a])) < areaMin) continue;
		fin.push_back(a); fin.push_back(b); fin.push_back(c2);
		if ((int)fin.size() / 3 >= SHELL_MAX_TRI) { partial = true; break; }
	}
	if ((int)fin.size() < 9) { fallbackSphere(); return; }

	// --- Step 5b: SMALL-ISLAND CULL (round 5.2) -------------------------------
	// Landing gear, antennae and other bolt-ons weld into their own connected
	// components, and a shock envelope has no business wrapping them. Cull by TWO
	// tests, not one: a component must be both a small share of the triangles AND
	// physically small. The size test is what makes this safe - a wing that lost
	// its stitch to the fuselage is a small share of the triangles too, and
	// dropping it would be a far worse bug than the gear it was meant to fix.
	{
		std::vector<int> uf2(NC);
		for (int c = 0; c < NC; c++) uf2[c] = c;
		auto find2 = [&](int x) -> int {
			while (uf2[x] != x) { uf2[x] = uf2[uf2[x]]; x = uf2[x]; }
			return x;
		};
		for (size_t ti = 0; ti + 2 < fin.size(); ti += 3) {
			const int r0 = find2(fin[ti]);
			const int r1 = find2(fin[ti + 1]);
			if (r0 != r1) uf2[r1] = r0;
			const int r2 = find2(fin[ti]), r3 = find2(fin[ti + 2]);
			if (r2 != r3) uf2[r3] = r2;
		}
		std::unordered_map<int, int> cnt;
		std::unordered_map<int, VECTOR3> lo, hi;
		for (size_t ti = 0; ti + 2 < fin.size(); ti += 3) {
			const int r = find2(fin[ti]);
			cnt[r]++;
			for (int q = 0; q < 3; q++) {
				const VECTOR3& p = cMean[fin[ti + q]];
				auto il = lo.find(r);
				if (il == lo.end()) { lo[r] = p; hi[r] = p; continue; }
				VECTOR3& a = il->second; VECTOR3& b = hi[r];
				if (p.x < a.x) a.x = p.x; if (p.x > b.x) b.x = p.x;
				if (p.y < a.y) a.y = p.y; if (p.y > b.y) b.y = p.y;
				if (p.z < a.z) a.z = p.z; if (p.z > b.z) b.z = p.z;
			}
		}
		int biggest = 0;
		for (auto& kv : cnt) if (kv.second > biggest) biggest = kv.second;
		const int    minTri  = (int)(0.04f * (float)biggest) > 8 ? (int)(0.04f * (float)biggest) : 8;
		const double minDiag = size * 0.20;
		std::vector<int> keep;
		keep.reserve(fin.size());
		int dropped = 0;
		for (size_t ti = 0; ti + 2 < fin.size(); ti += 3) {
			const int r = find2(fin[ti]);
			const double diag = length(hi[r] - lo[r]);
			if (cnt[r] < minTri && diag < minDiag) { dropped++; continue; }
			keep.push_back(fin[ti]); keep.push_back(fin[ti + 1]); keep.push_back(fin[ti + 2]);
		}
		if ((int)keep.size() >= 9 && dropped > 0) {
			oapiWriteLogV("ORO: shell island cull - dropped %d of %d tris (%s).",
			              dropped, (int)fin.size() / 3, v->GetName());
			fin.swap(keep);
		}
	}

	// --- Step 6: compact into the slot arrays + open-edge flags ---------------
	std::vector<int> remap(NC, -1);
	int nv = 0;
	for (int idx : fin) {
		if (remap[idx] >= 0) continue;
		remap[idx] = nv;
		e.shellPos[nv * 3 + 0] = (float)cMean[idx].x;
		e.shellPos[nv * 3 + 1] = (float)cMean[idx].y;
		e.shellPos[nv * 3 + 2] = (float)cMean[idx].z;
		VECTOR3 n = cN[idx];
		const double nl = length(n);
		if (nl > 1e-6) n = n / nl;
		else {
			const double ml = length(cMean[idx]);
			n = (ml > 1e-6) ? cMean[idx] / ml : _V(0, 0, 1);   // radial fallback
		}
		e.shellNrm[nv * 3 + 0] = (float)n.x;
		e.shellNrm[nv * 3 + 1] = (float)n.y;
		e.shellNrm[nv * 3 + 2] = (float)n.z;
		e.shellFlg[nv] = 0;
		nv++;
	}
	int nt = 0;
	for (size_t ti = 0; ti + 2 < fin.size(); ti += 3) {
		e.shellIdx[nt * 3 + 0] = (WORD)remap[fin[ti]];
		e.shellIdx[nt * 3 + 1] = (WORD)remap[fin[ti + 1]];
		e.shellIdx[nt * 3 + 2] = (WORD)remap[fin[ti + 2]];
		nt++;
	}
	e.nShellV = nv; e.nShellT = nt;

	{
		// open-edge flags from FINAL edge use counts (post-stitch, so healed crack
		// lips have use-count 2 and correctly lose the feather)
		std::unordered_map<unsigned int, int> euse;
		euse.reserve(nt * 4);
		auto ekey = [](int a2, int b2) -> unsigned int {
			if (a2 > b2) { const int t2 = a2; a2 = b2; b2 = t2; }
			return ((unsigned int)a2 << 16) | (unsigned int)b2;
		};
		for (int ti = 0; ti < nt; ti++) {
			euse[ekey(e.shellIdx[ti * 3], e.shellIdx[ti * 3 + 1])]++;
			euse[ekey(e.shellIdx[ti * 3 + 1], e.shellIdx[ti * 3 + 2])]++;
			euse[ekey(e.shellIdx[ti * 3], e.shellIdx[ti * 3 + 2])]++;
		}
		for (auto& kv : euse) {
			if (kv.second != 1) continue;
			e.shellFlg[kv.first >> 16] |= SH_OPENEDGE;
			e.shellFlg[kv.first & 0xFFFF] |= SH_OPENEDGE;
		}
	}

	// --- Step 7: TAUBIN SMOOTHING (round 5.2) ---------------------------------
	// Coarser cells alone cannot fix "too detailed": welding turns a landing gear
	// leg into a chunkier landing gear leg, and every mesh has greebles that no
	// cell size flatters. What the shock envelope actually wants is a SMOOTH
	// surface, so we relax the welded shell like a soap film.
	// TAUBIN (lambda/mu), not plain Laplacian: plain smoothing shrinks a closed
	// surface toward its centroid every pass, and a shell that shrinks stops being
	// registered with the hull it is supposed to be wrapping (it would sink INSIDE
	// the skin - and the round-5.1 depth map would then correctly occlude it, so
	// the two changes would fight). The negative mu pass re-inflates at a slightly
	// larger magnitude, so high-frequency detail dies while gross shape survives.
	// Thin protrusions lose because their neighbourhoods pull inward from all
	// sides; the wing, fuselage and nose are locally near-flat and barely move.
	// Open-edge vertices are PINNED: they are the feathered boundary, and dragging
	// a boundary inward would eat exactly the wrapping rim G5 warns about.
	if (nv > 3 && nt > 3) {
		std::vector<float> acc(nv * 3);
		std::vector<int>   deg(nv);
		const float LAM = 0.50f, MU = -0.53f;
		for (int pass = 0; pass < 6; pass++) {
			const float w = (pass & 1) ? MU : LAM;
			std::fill(acc.begin(), acc.end(), 0.0f);
			std::fill(deg.begin(), deg.end(), 0);
			auto edge = [&](int a2, int b2) {
				acc[a2 * 3 + 0] += e.shellPos[b2 * 3 + 0];
				acc[a2 * 3 + 1] += e.shellPos[b2 * 3 + 1];
				acc[a2 * 3 + 2] += e.shellPos[b2 * 3 + 2];
				deg[a2]++;
			};
			for (int ti = 0; ti < nt; ti++) {
				const int a2 = e.shellIdx[ti * 3], b2 = e.shellIdx[ti * 3 + 1],
				          c3 = e.shellIdx[ti * 3 + 2];
				edge(a2, b2); edge(b2, a2);
				edge(b2, c3); edge(c3, b2);
				edge(c3, a2); edge(a2, c3);
			}
			for (int k = 0; k < nv; k++) {
				if (deg[k] < 2 || (e.shellFlg[k] & SH_OPENEDGE)) continue;   // pinned
				const float inv = 1.0f / (float)deg[k];
				for (int d = 0; d < 3; d++)
					e.shellPos[k * 3 + d] += w * (acc[k * 3 + d] * inv - e.shellPos[k * 3 + d]);
			}
		}
		// --- Step 7b: DETAIL CLAMP against a smoothed BASE (round 5.4) --------
		// 5.3 clamped each vertex against its IMMEDIATE neighbours along its own
		// normal. That thins a landing gear leg - the neighbours are further up and
		// down the leg, so the spike it measures is the leg's RADIUS - but it can
		// never retract one, which is exactly the stubs that survived (user,
		// 2026-08-02, "we need a way to smooth them even more into the belly").
		// A protrusion is only meaningful as a deviation from the surface it
		// protrudes FROM, so build that surface explicitly: a heavily smoothed copy
		// of the shell, twelve Taubin passes, enough for the kernel to reach across
		// several weld cells. Then limit every vertex to a fixed distance from it.
		// Hull panels deviate from their own low-frequency shape by less than a cell
		// and do not move at all; a gear leg deviates by a metre and a half and
		// collapses onto the belly. It is ISOTROPIC, so unlike 5.3 it no longer
		// matters which way a bolt-on's normals happen to point - and it is exactly
		// the "relatively flat shell" the reference shows.
		// Large features survive on their own: a smoothing kernel this wide still
		// FOLLOWS a tail fin (the base bends with it, so its deviation stays small),
		// while an isolated 1 m bump is gone from the base entirely.
		{
			std::vector<float> base(&e.shellPos[0], &e.shellPos[0] + nv * 3);
			std::vector<float> acc2(nv * 3);
			std::vector<int>   deg2(nv);
			for (int pass = 0; pass < 12; pass++) {
				const float w = (pass & 1) ? -0.63f : 0.60f;
				std::fill(acc2.begin(), acc2.end(), 0.0f);
				std::fill(deg2.begin(), deg2.end(), 0);
				auto edge2 = [&](int a2, int b2) {
					acc2[a2 * 3 + 0] += base[b2 * 3 + 0];
					acc2[a2 * 3 + 1] += base[b2 * 3 + 1];
					acc2[a2 * 3 + 2] += base[b2 * 3 + 2];
					deg2[a2]++;
				};
				for (int ti = 0; ti < nt; ti++) {
					const int a2 = e.shellIdx[ti * 3], b2 = e.shellIdx[ti * 3 + 1],
					          c3 = e.shellIdx[ti * 3 + 2];
					edge2(a2, b2); edge2(b2, a2);
					edge2(b2, c3); edge2(c3, b2);
					edge2(c3, a2); edge2(a2, c3);
				}
				for (int k = 0; k < nv; k++) {
					if (deg2[k] < 2 || (e.shellFlg[k] & SH_OPENEDGE)) continue;
					const float inv = 1.0f / (float)deg2[k];
					for (int d = 0; d < 3; d++)
						base[k * 3 + d] += w * (acc2[k * 3 + d] * inv - base[k * 3 + d]);
				}
			}
			const float lim = 0.70f * (float)cell;
			int clamped = 0;
			for (int k = 0; k < nv; k++) {
				if (e.shellFlg[k] & SH_OPENEDGE) continue;
				const float dx = e.shellPos[k * 3]     - base[k * 3];
				const float dy = e.shellPos[k * 3 + 1] - base[k * 3 + 1];
				const float dz = e.shellPos[k * 3 + 2] - base[k * 3 + 2];
				const float dl = sqrtf(dx * dx + dy * dy + dz * dz);
				if (dl <= lim || dl < 1e-6f) continue;
				const float s = lim / dl;
				e.shellPos[k * 3 + 0] = base[k * 3]     + dx * s;
				e.shellPos[k * 3 + 1] = base[k * 3 + 1] + dy * s;
				e.shellPos[k * 3 + 2] = base[k * 3 + 2] + dz * s;
				clamped++;
			}
			if (clamped > 0)
				oapiWriteLogV("ORO: shell detail clamp - %d of %d vertices pulled in (%s).",
				              clamped, nv, v->GetName());
		}

		// Normals must follow the geometry they shade - the inherited mesh normals
		// describe a surface that no longer exists. Rebuilt area-weighted from the
		// shell's own faces, then FLIPPED to agree with the old normal per vertex:
		// the decimated winding is not guaranteed consistent, and an inside-out
		// normal would read as windward on the leeward side.
		std::vector<float> nrm(nv * 3, 0.0f);
		for (int ti = 0; ti < nt; ti++) {
			const int a2 = e.shellIdx[ti * 3], b2 = e.shellIdx[ti * 3 + 1],
			          c3 = e.shellIdx[ti * 3 + 2];
			const VECTOR3 pa = _V(e.shellPos[a2 * 3], e.shellPos[a2 * 3 + 1], e.shellPos[a2 * 3 + 2]);
			const VECTOR3 pb = _V(e.shellPos[b2 * 3], e.shellPos[b2 * 3 + 1], e.shellPos[b2 * 3 + 2]);
			const VECTOR3 pc = _V(e.shellPos[c3 * 3], e.shellPos[c3 * 3 + 1], e.shellPos[c3 * 3 + 2]);
			const VECTOR3 fn = crossp(pb - pa, pc - pa);          // area-weighted
			const int ix[3] = { a2, b2, c3 };
			for (int q = 0; q < 3; q++) {
				// orientation is per-FACE against the vertex's inherited normal, so a
				// flipped triangle cannot poison its neighbours
				const double s = (e.shellNrm[ix[q] * 3]     * fn.x
				                + e.shellNrm[ix[q] * 3 + 1] * fn.y
				                + e.shellNrm[ix[q] * 3 + 2] * fn.z) < 0.0 ? -1.0 : 1.0;
				nrm[ix[q] * 3 + 0] += (float)(fn.x * s);
				nrm[ix[q] * 3 + 1] += (float)(fn.y * s);
				nrm[ix[q] * 3 + 2] += (float)(fn.z * s);
			}
		}
		for (int k = 0; k < nv; k++) {
			const double l = sqrt((double)nrm[k * 3] * nrm[k * 3]
			                    + (double)nrm[k * 3 + 1] * nrm[k * 3 + 1]
			                    + (double)nrm[k * 3 + 2] * nrm[k * 3 + 2]);
			if (l < 1e-9) continue;                    // keep the inherited one
			e.shellNrm[k * 3 + 0] = (float)(nrm[k * 3]     / l);
			e.shellNrm[k * 3 + 1] = (float)(nrm[k * 3 + 1] / l);
			e.shellNrm[k * 3 + 2] = (float)(nrm[k * 3 + 2] / l);
		}
	}

	oapiWriteLogV("ORO: shock shell %s: V=%d T=%d att=%d cell=%.2fm%s",
	              v->GetName(), nv, nt, attempt + 1, cell,
	              partial ? " - coverage PARTIAL" : "");
}

// ----------------------------------------------------------------------------
// Wake blobs: spawn (camera-target vessel only; nobody sees the others' wakes
// closely enough to pay for them). Advection happens in UpdateReentry's slot
// loop for EVERY tracked vessel, on SIM time - shed plasma is a physical
// object, so at time-warp it sheds and recedes faster, which is correct. The
// inverse convention from the eye's animations (invariant 4).
// ----------------------------------------------------------------------------
void OroModule::SpawnWakeBlobs(int i, VESSEL* v, const VECTOR3& flowLocal, double simdt)
{
	ReentryVessel& e = rentry[i];
	if (e.heat < 0.20f) return;              // no visible wake until the plasma is real

	e.blobT -= simdt;
	if (e.blobT > 0.0) return;
	e.blobT = 0.55 - 0.35 * e.heat;          // cadence quickens with heat (~0.2..0.5 s)
	                                         // (round 2.1: denser - the blobs own the trail
	                                         // now that the particle fan is off)

	int slot = -1;
	for (int b = 0; b < MAX_BLOB; b++)
		if (e.blob[b].age < 0.0f) { slot = b; break; }
	if (slot < 0) return;                    // all alive - the trail is already dense

	const double size = v->GetSize() > 1.0 ? v->GetSize() : 1.0;
	VECTOR3 Cg;    v->GetGlobalPos(Cg);
	VECTOR3 flowG; v->GlobalRot(flowLocal, flowG);
	VECTOR3 shipVel; v->GetGlobalVel(shipVel);
	VECTOR3 vaG = _V(0, 0, 0);
	v->GetAirspeedVector(FRAME_GLOBAL, vaG);

	// Two perpendiculars of the flow axis, for lateral shedding jitter.
	const VECTOR3 ref = (fabs(flowG.y) < 0.9) ? _V(0, 1, 0) : _V(1, 0, 0);
	const VECTOR3 p1 = unit(crossp(flowG, ref));
	const VECTOR3 p2 = crossp(flowG, p1);

	const float h1 = hashf((float)animT * 3.17f + i * 7.7f);
	const float h2 = hashf(h1 * 91.7f + 1.3f);
	const float h3 = hashf(h2 * 57.3f + 2.9f);

	WakeBlob& wb = e.blob[slot];
	wb.gpos  = Cg - flowG * (size * (1.8 + 1.4 * h1))
	              + p1 * (size * 1.1 * (h2 - 0.5)) + p2 * (size * 1.1 * (h3 - 0.5));
	wb.gvel  = shipVel - vaG;                // the AIR's velocity: the blob rides the wind
	wb.age   = 0.0f;
	wb.size0 = (float)(size * (0.50 + 0.45 * h2));
	wb.seed  = h1 + h3;
}

// ============================================================================
// THE PLASMA TRAIL, take 2 (2026-08-08) - the particle pool. Architecture note
// in OroModule.h: G10 convicted CONNECTIVITY through accumulation, not
// accumulation itself, so this is the other family - Firefly's own (their long
// trail is their smoke particle system): independent expiring world-anchored
// particles, each drawn ALONE. No polyline, no mitre, no neighbour lookup, ever.
//   UpdateTrail  - once per frame, BEFORE the slot loop: age/advect/expire the
//                  whole pool, emit the survivors, prime the frame context.
//   SpawnTrail   - per tracked vessel, in the loop: DISTANCE-cadence shedding,
//                  sub-frame interpolated along the path flown this step (the
//                  structural kill for 5.12's cadence collapse - at time-warp
//                  you get MORE particles spaced evenly, never one clump).
//   EmitTrailPt  - one particle -> one soft stretched sprite: 4 triangles fanned
//                  around a bright centre, every border vertex alpha 0, so a hard
//                  edge cannot print (the fin feather law, invariant 20b).
// ----------------------------------------------------------------------------
static const float TRAIL_HEAT_ON   = 0.15f;   // shed gate - a touch before the visible
                                              // plasma (blobs used 0.20); alpha scales
                                              // with heat-at-shed so early sheds are faint
static const int   TRAIL_SHARE     = 1600;    // pool slots a LONE vessel's full trail aims
                                              // for: spacing >= speed*life/SHARE keeps one
                                              // ship from exhausting the 4096 ring, so a
                                              // breakup's debris still fit their trails
static const int   TRAIL_SPAWN_CAP = 64;      // per vessel per frame: extreme time-warp
                                              // degrades to SPARSER dashes, never clumps
// (TRAIL_IN, the fixed head fade-in time, is GONE - it is the Trail start knob's
//  job now (s_tInT below). HISTORY, because this number wore four hats: 0.30 s
//  killed round 1's origin flashing but hid the first 1-2 km; 0.04 s covered the
//  sprite era's newborn pop-in; 0.012 s was the ribbon's blend-under-the-streaks;
//  and then the user reported the Trail start slider "barely does anything" -
//  because the slider moved a 0-20 m standoff while THIS fixed ramp owned the
//  ~60-80 m where the trail actually becomes visible. The knob now drives both
//  terms, so "Trail start" finally means where the trail starts.)

// Frame context for EmitTrailPt. s_tCam/s_tProj are set by ProjectTrail in the
// RENDER PATH (round 4 - the only place the true render camera exists); the emit
// parameters that need oapi/g_fx gathering are set by UpdateTrailPost on the main
// thread. Single sim/render thread by construction (OroState.h's rule).
static CamCtx s_tCam;
static bool   s_tProj = false;                // camera resolved this frame?
static float  s_tTrim = 0.0f;
static float  s_tWidK = 1.0f;
static CamCtx s_tPostCam;                     // POST-STEP camera - the FALLBACK only
static bool   s_tPostCamValid = false;        // (one step stale; round 3 proved it - kept
                                              // so an unpatched client degrades to the old
                                              // jumpy behaviour instead of to nothing)
// Reference-body position table (round 7): particles store PLANET-RELATIVE positions
// and the render path may make NO oapi calls (invariant 1), so the bodies' CURRENT
// global positions are gathered here each post-step. Post-step body state == the
// state the frame renders (only the CAMERA updates later - round 3's measurement),
// so reconstruction with these positions is epoch-exact.
static const int TRAIL_MAX_REF = 4;
static OBJHANDLE s_tRef[TRAIL_MAX_REF];
static VECTOR3   s_tRefPos[TRAIL_MAX_REF];
static int       s_tRefN = 0;
// Ribbon chain-head cache (phase A): the live anchor point of each ACTIVELY SHEDDING
// vessel's ribbon - the hull standoff, planet-relative - refreshed every post-step by
// SpawnTrail so the strip's newest end tucks under the plasma streaks. A chain with no
// entry (vessel dead, cooled, or knob off) simply ends at its newest surviving knot.
static const int TRAIL_MAX_CHAINS = 16;
static DWORD     s_tcId[TRAIL_MAX_CHAINS];
static VECTOR3   s_tcHead[TRAIL_MAX_CHAINS];
static OBJHANDLE s_tcRef[TRAIL_MAX_CHAINS];
static double    s_tcSize[TRAIL_MAX_CHAINS];  // hull size, for the ramp length (A.6)
static int       s_tcN = 0;
static DWORD     s_tChainNext = 1;            // 0 = "no chain" in TrailPt
// TRAIL COLOUR PICKS (A.3): hue rotations for the HEAD and TAIL ends, computed once per
// frame (main thread) from the two swatches via 15b's HueRotFromPick - white = identity.
// The emitters blend the rotation head->tail by ttv (shortest arc), so two picks author
// the ribbon's whole colour journey. Reference hues = the raw ramp's own families.
static const float TRAIL_HUE_HEAD = 24.0f;    // hue of the young orange family (255,160,75)
static const float TRAIL_HUE_TAIL = 12.0f;    // hue of the ember family (215,70,40)
static float s_tRotHead = 0.0f;
static float s_tRotTail = 0.0f;
// THE IGNITION RAMP IS PART OF THE RIBBON'S SHAPE (A.6). Rounds A.4/A.5 keyed the
// fade to particle AGE - which is always zero at the head, wherever the head sits,
// so the Trail start slider could move the root OR keep the fade but never both
// ("can't the slider simply be a move of the whole ribbon with the faint
// included?" - yes). The fade now keys to DISTANCE FROM THE HEAD ANCHOR: a fixed
// soft ramp, TRAIL_RAMP hull-sizes long, that translates with the root. The
// slider is a pure position control at last.
static const float TRAIL_RAMP = 7.0f;         // ignition ramp length, x hull size (~70 m
                                              // on the DG - the scale the user called "a
                                              // nice fade" on the positive side of A.5)
static int       s_tShedFrame = 0;            // sheds THIS frame, all vessels (horizon feed)
static double    s_tShedRate  = 0.0;          // smoothed sheds/s, scene-wide
static double    s_tHorizon   = 1e9;          // age at which ring eviction bites [s] -
                                              // TRAIL_MAX / rate; 1e9 = pool not saturated
// (s_tAnchorDelta - the render-vs-post-step body position gap - lived here for the
//  round-7/8 diagnostic. Its verdict is preserved where it matters: ~2 km at 10x warp,
//  measured 2026-08-08, which is why ProjectTrail overwrites the anchors with the
//  renderer's own numbers below. The diagnostic retired when the user confirmed
//  "rock solid".)

// ============================================================================
// PHASE A - THE RIBBON (2026-08-08, the substrate discussion's verdict). The
// sprite era (rounds 8-13: rhombus, capsule, five brightness scalars, geometric
// LOD) is BURIED, and its tombstone is G1/G2's law reconfirmed at trail scale:
// disconnected additive sprites are the same primitive class as stock's
// billboards and can never read as a continuous luminous medium - "we seem to
// have ended up with what Orbiter's default reentry flames were" (the user,
// correctly). Connectivity is the fix, exactly as G2 said: ONE mitred,
// cross-feathered strip threaded through each chain's knots, rebuilt from
// scratch every frame. A ribbon deposits its alpha ONCE per pixel - there is no
// stacking sum to normalize, so the entire rounds-8-13 brightness war is
// structurally impossible here.
//
// G10, addressed rather than sidestepped: this connects particles, which the
// design's own law forbade. The law's PURPOSE was to stop bad accumulated
// samples being amplified by neighbours; the samples are now deterministic
// (smooth meander, distance cadence), frame-exact (patches k/k2), teleport-
// guarded, and diag-MEASURED on-path to metres - and the geometry through them
// persists for exactly one frame. Screen-space knot DECIMATION (~6 px between
// kept knots, stateless, per frame) kills the 5.12 mitre-degeneracy class by
// construction. What made the 2026-08-02 trail unfixable was persisted geometry
// through untrusted samples; this is per-frame geometry through proven ones.
//
// The user's traveling knots: a brightness lump keyed to a PARTICLE is fixed in
// the air, so the vessel leaves it behind and it marches down the ribbon
// automatically - no clock, no state, and NO WIND (the round-6/7 lesson stands:
// nothing advects, ever).
// ============================================================================
void OroModule::EmitTrailChains()
{
	if (!s_tProj) return;

	// The trail borrows the PCol pipeline with its OWN picks (A.3): stash the plasma's
	// palette rotation state, zero the fringe term (the trail ramp has no b > g cast to
	// weight), and restore both on the way out. Single sim/render thread - no race.
	const float savedRot  = g_palHueRot;
	const float savedRot2 = g_palHueRot2;
	g_palHueRot2 = 0.0f;

	// One decimated RUN of knots (a chain may split into several runs at breaks).
	static const int RUN_MAX = 512;
	static float rx[RUN_MAX], ry[RUN_MAX], rd[RUN_MAX], rw[RUN_MAX];
	static DWORD rcC[RUN_MAX], rc0[RUN_MAX];
	static float pxv[RUN_MAX], pyv[RUN_MAX];
	int runN = 0;

	// Emit the current run as one feathered strip: mitred per-knot perpendiculars
	// (invariant 15's ribbon law), cross-section edge-0 | bright spine | edge-0
	// (20b - no border can print). Gouraud carries colour/alpha between knots.
	auto emitRun = [&]() {
		if (runN >= 2) {
			// Belt-and-braces against residual fold-over: no knot may be wider than
			// ~1.15x its shorter neighbouring segment (the adaptive decimation should
			// already guarantee this; run ends and width wobble can still conspire).
			for (int j = 0; j < runN; j++) {
				float seg = 1e9f;
				if (j > 0) {
					const float sx = rx[j] - rx[j - 1], sy = ry[j] - ry[j - 1];
					const float l = sqrtf(sx * sx + sy * sy); if (l < seg) seg = l;
				}
				if (j < runN - 1) {
					const float sx = rx[j + 1] - rx[j], sy = ry[j + 1] - ry[j];
					const float l = sqrtf(sx * sx + sy * sy); if (l < seg) seg = l;
				}
				if (rw[j] > seg * 1.15f) rw[j] = seg * 1.15f;
			}
			for (int j = 0; j < runN; j++) {
				float dpx = 0.0f, dpy = 0.0f;
				if (j > 0) {
					const float sx = rx[j] - rx[j - 1], sy = ry[j] - ry[j - 1];
					const float l = sqrtf(sx * sx + sy * sy);
					if (l > 0.01f) { dpx += -sy / l; dpy += sx / l; }
				}
				if (j < runN - 1) {
					const float sx = rx[j + 1] - rx[j], sy = ry[j + 1] - ry[j];
					const float l = sqrtf(sx * sx + sy * sy);
					if (l > 0.01f) { dpx += -sy / l; dpy += sx / l; }
				}
				const float l = sqrtf(dpx * dpx + dpy * dpy);
				if (l > 0.01f) { pxv[j] = dpx / l; pyv[j] = dpy / l; }
				else           { pxv[j] = 0.0f;    pyv[j] = 1.0f;   }
			}
			auto tvv = [&](float X, float Y, float D, DWORD C) {
				trailVtx[trailVtxN].x = X; trailVtx[trailVtxN].y = Y; trailVtx[trailVtxN].c = C;
				trailDepth[trailVtxN] = D; trailVtxN++;
			};
			for (int j = 0; j + 1 < runN; j++) {
				if (trailVtxN + 12 > TRAIL_MAX_TRI * 3) break;
				const float Lax = rx[j]     + pxv[j]     * rw[j],     Lay = ry[j]     + pyv[j]     * rw[j];
				const float Rax = rx[j]     - pxv[j]     * rw[j],     Ray = ry[j]     - pyv[j]     * rw[j];
				const float Lbx = rx[j + 1] + pxv[j + 1] * rw[j + 1], Lby = ry[j + 1] + pyv[j + 1] * rw[j + 1];
				const float Rbx = rx[j + 1] - pxv[j + 1] * rw[j + 1], Rby = ry[j + 1] - pyv[j + 1] * rw[j + 1];
				tvv(Lax, Lay, rd[j], rc0[j]);  tvv(Lbx, Lby, rd[j + 1], rc0[j + 1]);  tvv(rx[j + 1], ry[j + 1], rd[j + 1], rcC[j + 1]);
				tvv(Lax, Lay, rd[j], rc0[j]);  tvv(rx[j + 1], ry[j + 1], rd[j + 1], rcC[j + 1]);  tvv(rx[j], ry[j], rd[j], rcC[j]);
				tvv(Rax, Ray, rd[j], rc0[j]);  tvv(rx[j], ry[j], rd[j], rcC[j]);  tvv(rx[j + 1], ry[j + 1], rd[j + 1], rcC[j + 1]);
				tvv(Rax, Ray, rd[j], rc0[j]);  tvv(rx[j + 1], ry[j + 1], rd[j + 1], rcC[j + 1]);  tvv(Rbx, Rby, rd[j + 1], rc0[j + 1]);
			}
		}
		runN = 0;
	};

	// Per-knot attributes - the surviving laws, unchanged: ttv (the visible-trail
	// coordinate), the (1-ttv)^1.4 profile, the fade-in, the near fade, the hot
	// colour stations through PCol (Tint/Fringe/Saturation live), the width floor
	// + spread + the 20(d) two-octave WIDTH wobble, and the seed lump (the
	// traveling knots). Base alpha 0.22: a ribbon has no stacking to build on, so
	// it carries its brightness itself (the streaks' sheath-coefficient scale).
	auto knotAttribs = [&](float age, float dIn, float life, float heat0, float seed,
	                       float size0, double cz, DWORD& cC, DWORD& c0, float& widPx) {
		const float tvis = (float)((s_tHorizon < (double)life) ? s_tHorizon : (double)life);
		const float ttv  = clampf(age / (tvis > 0.75f ? tvis : 0.75f), 0.0f, 1.0f);
		const float fade  = powf(1.0f - ttv, 1.4f);
		const float nearF = clampf((float)((cz - 8.0) / 40.0), 0.0f, 1.0f);
		const float lump  = 0.72f + 0.56f * hashf(seed * 13.7f);
		const float aRaw  = 255.0f * heat0 * s_tTrim * 0.22f * fade * dIn * nearF * lump;
		const float aC = 255.0f * (1.0f - expf(-aRaw / 255.0f));
		const float wob = 1.0f + 0.18f * sinf(ttv * 19.5f + seed * 41.0f)
		                       + 0.10f * sinf(ttv * 47.0f + seed * 17.0f);
		const float wWorld = size0 * (1.35f + 1.9f * ttv) * s_tWidK * wob;
		widPx = (float)((double)wWorld / (cz * s_tCam.tanAp) * ((double)viewH * 0.5));
		if (widPx > (float)viewH * 0.35f) widPx = (float)viewH * 0.35f;
		if (widPx < 0.7f) widPx = 0.7f;
		int kr, kg, kb;
		if      (ttv < 0.10f) { const float u = ttv / 0.10f;            kr = 255;                     kg = 235 - (int)( 75.0f * u); kb = 210 - (int)(135.0f * u); }
		else if (ttv < 0.55f) { const float u = (ttv - 0.10f) / 0.45f;  kr = 255;                     kg = 160 - (int)( 50.0f * u); kb =  75 - (int)( 25.0f * u); }
		else if (ttv < 0.85f) { const float u = (ttv - 0.55f) / 0.30f;  kr = 255 - (int)( 40.0f * u); kg = 110 - (int)( 40.0f * u); kb =  50 - (int)( 10.0f * u); }
		else                  { const float u = (ttv - 0.85f) / 0.15f;  kr = 215 - (int)( 45.0f * u); kg =  70 - (int)( 25.0f * u); kb =  40 - (int)( 10.0f * u); }
		// The trail's OWN hue rotation (A.3): head pick -> tail pick, blended along the
		// visible trail by shortest arc. A deliberate GRADIENT between two full-rotation
		// targets - not 15b(c)'s forbidden partial rotation toward one - so each END
		// lands exactly on its picked hue. g_palHueRot is per-knot here and restored by
		// EmitTrailChains; the plasma's own picks are untouched.
		float d = fmodf(s_tRotTail - s_tRotHead + 540.0f, 360.0f) - 180.0f;
		g_palHueRot = s_tRotHead + d * ttv;
		cC = PCol(kr, kg, kb, (int)aC);
		c0 = PCol(kr, kg, kb, 0);
	};

	// Chains present this frame (ring order = shed order; per chain, oldest first).
	DWORD ids[TRAIL_MAX_CHAINS]; int nIds = 0;
	for (int i = 0; i < TRAIL_MAX && nIds < TRAIL_MAX_CHAINS; i++) {
		const TrailPt& p = trail[(trailHead + i) % TRAIL_MAX];
		if (p.age < 0.0f || p.chain == 0) continue;
		bool have = false;
		for (int q = 0; q < nIds; q++) if (ids[q] == p.chain) { have = true; break; }
		if (!have) ids[nIds++] = p.chain;
	}

	for (int c = 0; c < nIds; c++) {
		runN = 0;
		bool  haveKept = false; float keptX = 0.0f, keptY = 0.0f;
		bool  havePrev = false; VECTOR3 prevRp = _V(0, 0, 0); float prevHL = 1.0f;
		float lastLife = 1.0f, lastHeat = 0.0f, lastSize = 1.0f, lastSeed = 0.0f;

		// This chain's live head anchor, resolved UP FRONT (A.6): the ignition ramp
		// is distance-from-here, so it translates with the Trail start knob. An
		// orphan chain (vessel dead/cooled) has no head and no ramp - its whole
		// trail is mature and the ttv fade owns its ends.
		int hci = -1;
		for (int q = 0; q < s_tcN; q++) if (s_tcId[q] == ids[c]) { hci = q; break; }
		const bool haveHead = (hci >= 0);
		VECTOR3 headRel = _V(0, 0, 0);
		float rampLen = 1.0f;
		if (haveHead) {
			headRel = s_tcHead[hci];
			rampLen = (float)(s_tcSize[hci] * (double)TRAIL_RAMP);
			if (rampLen < 1.0f) rampLen = 1.0f;
		}

		for (int i = 0; i < TRAIL_MAX; i++) {
			const TrailPt& p = trail[(trailHead + i) % TRAIL_MAX];
			if (p.age < 0.0f || p.chain != ids[c]) continue;

			int ri = -1;
			for (int q = 0; q < s_tRefN; q++) if (s_tRef[q] == p.hRef) { ri = q; break; }
			if (ri < 0) { emitRun(); haveKept = false; havePrev = false; continue; }

			// A spatial gap inside one chain (belt and braces - reprime makes a new
			// chain, but never bridge what looks broken) splits the strip.
			if (havePrev && length(p.rpos - prevRp) > 6.0 * (double)prevHL) { emitRun(); haveKept = false; }
			prevRp = p.rpos; prevHL = p.halfLen; havePrev = true;

			const VECTOR3 gp = s_tRefPos[ri] + p.rpos;
			float cx, cy; double cz;
			if (!ProjPx(s_tCam, gp, viewW, viewH, cx, cy, cz)) { emitRun(); haveKept = false; continue; }

			// The ignition ramp: distance from the head anchor, translated by the knob.
			const float dIn = haveHead
			                ? clampf((float)length(p.rpos - headRel) / rampLen, 0.0f, 1.0f)
			                : 1.0f;
			float widPx; DWORD cCk, c0k;
			knotAttribs(p.age, dIn, p.life, p.heat0, p.seed, p.size0, cz, cCk, c0k, widPx);

			// ADAPTIVE SCREEN-SPACE DECIMATION (A.1): keep knots apart by at least the
			// ribbon's OWN half-width (floor 8 px). The first build used a flat 6 px and
			// maxed sliders put ~100 px of half-width on 6 px segments - consecutive
			// cross-bars intersected at every meander bend, quads folded into bowties,
			// and the feather landed inside while the bright spine printed hard edges
			// (the "triangular shards" screenshot). A segment can never be shorter
			// than the ribbon is wide now, so fold-over is geometrically impossible
			// for the meander's gentle curvature. Stateless, per frame, per view.
			if (haveKept) {
				const float ddx = cx - keptX, ddy = cy - keptY;
				const float need = (widPx * 0.9f > 8.0f) ? widPx * 0.9f : 8.0f;
				if (ddx * ddx + ddy * ddy < need * need) continue;
			}
			if (runN >= RUN_MAX) emitRun();          // overflow: flush, restart here
			rx[runN] = cx;  ry[runN] = cy;
			rd[runN] = (float)length(gp - s_tCam.pos);
			rw[runN] = widPx;
			rcC[runN] = cCk;
			rc0[runN] = c0k;
			runN++;
			haveKept = true; keptX = cx; keptY = cy;
			lastLife = p.life; lastHeat = p.heat0; lastSize = p.size0; lastSeed = p.seed;
		}

		// The synthetic HEAD: the anchor itself - distance-from-head ZERO, so the
		// ribbon always tapers softly to nothing exactly AT the knob's position,
		// wherever that is. The whole fade translates with it (A.6).
		if (haveHead && runN > 0) {
			int ri = -1;
			for (int w = 0; w < s_tRefN; w++) if (s_tRef[w] == s_tcRef[hci]) { ri = w; break; }
			if (ri >= 0) {
				const VECTOR3 gp = s_tRefPos[ri] + headRel;
				float cx, cy; double cz;
				if (ProjPx(s_tCam, gp, viewW, viewH, cx, cy, cz)) {
					if (runN >= RUN_MAX) emitRun();
					float widPx; DWORD cCk, c0k;
					knotAttribs(0.0f, 0.0f, lastLife, lastHeat, lastSeed, lastSize, cz, cCk, c0k, widPx);
					rx[runN] = cx;  ry[runN] = cy;
					rd[runN] = (float)length(gp - s_tCam.pos);
					rw[runN] = widPx;
					rcC[runN] = cCk;
					rc0[runN] = c0k;
					runN++;
				}
			}
		}
		emitRun();
	}

	g_palHueRot  = savedRot;                 // hand the PCol pipeline back to the plasma
	g_palHueRot2 = savedRot2;
}

void OroModule::UpdateTrail(double simdt)
{
	// AGE AND EXPIRE - that is all (round 7). There is NO advection any more:
	// positions are planet-relative and the frame-keeping happens at
	// reconstruction (EmitTrailPt adds the body's current position), so a
	// particle needs no velocity at all to stay over its planet. The phase-2
	// smoke column will advect rpos directly with the planet-frame wind.
	for (int k = 0; k < TRAIL_MAX; k++) {
		TrailPt& p = trail[k];
		if (p.age < 0.0f) continue;
		p.age += (float)simdt;
		if (p.life <= 0.0f || p.age >= p.life) p.age = -1.0f;
	}
}

void OroModule::SpawnTrail(int i, VESSEL* v, const VECTOR3& flowLocal)
{
	ReentryVessel& e = rentry[i];
	const float density = clampf(g_fx.plasTrail, 0.0f, 2.0f);
	if (density <= 0.001f || e.heat < TRAIL_HEAT_ON) { e.trailSeen = false; return; }

	// ROUND 7: EVERYTHING CURSOR- AND PARTICLE-SIDE IS PLANET-RELATIVE (inertial,
	// non-rotating). Orbiter's global frame is solar-system BARYCENTRIC - working
	// there put 29.9 km/s of Earth's orbital velocity into every "path" quantity:
	// the round-6 trail recorded the vessel's path around the SUN (diag7: oldest
	// particle 100 km back at 3.4 s = exactly Earth's orbital speed), and the shed
	// cadence measured the barycentric path, shedding 4x too fast and draining the
	// ring at 3.4 s of a 6 s life. See TrailPt::rpos in OroModule.h for the law.
	const OBJHANDLE hRef = v->GetSurfaceRef();
	if (!hRef) { e.trailSeen = false; return; }
	VECTOR3 refPos; oapiGetGlobalPos(hRef, &refPos);
	VECTOR3 Cg; v->GetGlobalPos(Cg);
	const VECTOR3 CgRel = Cg - refPos;
	if (!e.trailSeen || hRef != e.trailRef) {    // first hot frame / SOI handover
		e.trailLast = CgRel; e.trailRef = hRef; e.trailAcc = 0.0; e.trailSeen = true;
		e.trailChain = s_tChainNext++;           // a NEW ribbon: gaps break, never bridge
		return;
	}

	const VECTOR3 dP  = CgRel - e.trailLast;     // the step through the PLANET frame
	const double step = length(dP);
	VECTOR3 vaG = _V(0, 0, 0);
	v->GetAirspeedVector(FRAME_GLOBAL, vaG);
	const double spd   = length(vaG);
	const double simdt = oapiGetSimStep();

	// Refresh this chain's HEAD anchor (phase A) - every frame, spawn or not, so the
	// ribbon's newest end rides the hull standoff and overlaps the plasma streaks.
	{
		const double sizeH = v->GetSize() > 1.0 ? v->GetSize() : 1.0;
		VECTOR3 flowGH; v->GlobalRot(flowLocal, flowGH);
		int ci = -1;
		for (int q = 0; q < s_tcN; q++) if (s_tcId[q] == e.trailChain) { ci = q; break; }
		if (ci < 0 && s_tcN < TRAIL_MAX_CHAINS) { ci = s_tcN++; s_tcId[ci] = e.trailChain; }
		if (ci >= 0) {
			// Trail start knob, bipolar, RECENTRED (A.7): effective standoff = knob - 2.5,
			// so the dial's 0 is the old -2.5 and the DG's best fit lands at -2.5 on the
			// dial ("make the current -2.5 the new 0", 2026-08-08). Dial still -5..+5.
			s_tcHead[ci] = CgRel - flowGH * (sizeH * (double)(clampf(g_fx.plasTrailStart, -5.0f, 5.0f) - 2.5f));
			s_tcRef[ci]  = hRef;
			s_tcSize[ci] = sizeH;
		}
	}

	// Teleport guard: a step the vessel's own velocity cannot explain (scenario jump,
	// editor reposition) re-primes the cursor instead of drawing a shed line across
	// the sky. Scaled by simdt, so honest high time-warp steps pass at any rate.
	if (step > spd * simdt * 3.0 + 1000.0) { e.trailLast = CgRel; e.trailAcc = 0.0; return; }

	// Shed spacing: the coarser of "a fraction of the hull" (small pieces shed close,
	// so debris get dense little trails) and "the pool's fair share of the full trail
	// length" (a lone fast vessel must not exhaust the ring). Density divides it.
	const float  life = clampf(g_fx.plasTrailLife, 0.5f, 12.0f);
	const double size = v->GetSize() > 1.0 ? v->GetSize() : 1.0;
	double spacing = size * 0.60;
	const double fair = (spd > 1.0 ? spd : 1.0) * (double)life / (double)TRAIL_SHARE;
	if (fair > spacing) spacing = fair;
	spacing /= (double)density;

	e.trailAcc += step;
	int n = (int)(e.trailAcc / spacing);
	if (n <= 0) { e.trailS += step; e.trailLast = CgRel; return; }
	if (n > TRAIL_SPAWN_CAP) n = TRAIL_SPAWN_CAP;

	const double accPrev = e.trailAcc - step;    // distance already walked at frame start
	VECTOR3 flowG; v->GlobalRot(flowLocal, flowG);
	const VECTOR3 ref = (fabs(flowG.y) < 0.9) ? _V(0, 1, 0) : _V(1, 0, 0);
	const VECTOR3 p1 = unit(crossp(flowG, ref));
	const VECTOR3 p2 = crossp(flowG, p1);
	// (No particle velocity of any kind since round 7. Rounds 5-6 fought over "the
	//  wind" without noticing that shipVel - vaG was 98% Earth's ORBITAL velocity -
	//  barycentric frame-keeping - and 2% actual rotating-atmosphere wind. Both jobs
	//  are gone now: frame-keeping is structural (planet-relative rpos), and the
	//  true wind bend stays out of the luminous trail by the round-6 decision.)

	for (int k = 1; k <= n; k++) {
		const double want = (double)k * spacing;
		double t = (want - accPrev) / (step > 1e-6 ? step : 1e-6);
		if (t < 0.0) t = 0.0; if (t > 1.0) t = 1.0;

		TrailPt& p = trail[trailHead];
		trailHead = (trailHead + 1) % TRAIL_MAX; // ring: oldest-first eviction, scene-wide
		s_tShedFrame++;                          // horizon feed (round 10)

		const float h1 = hashf((float)animT * 3.17f + (float)i * 7.7f + (float)k * 13.1f);
		const float h2 = hashf(h1 * 91.7f + 1.3f);

		// ROUND 2: the shed point is DETERMINISTIC and SMOOTH. Round 1 rolled a fresh
		// random annulus point and standoff per shed - uncorrelated between consecutive
		// sheds - and since newborns were also the brightest sprites, the trail's
		// visible origin DANCED to a new position every shed (the user's "jumpy...
		// origin near but not in the vessel"). Now the standoff is FIXED and the
		// centreline is a slow two-octave meander in CUMULATIVE PATH DISTANCE - the
		// honest phase coordinate: identical at any time-warp and any framerate, and
		// per-slot phase offsets keep a debris field from sharing one waveform.
		// Randomness survives only as a small residual here and the per-particle
		// WIDTH (h2 below) - the channel invariant 20(d) proved safe. G12(c) check:
		// the meander is sampled at shed spacing, so its wavelengths must stay LONG
		// against the spacing - 16 and 41 hull radii against 0.6-4 radii spacing.
		const double sShed = e.trailS + (want - accPrev);
		const double w1 = sShed / (size * 16.0) * 6.2831853 + (double)i * 2.1;
		const double w2 = sShed / (size * 41.0) * 6.2831853 + (double)i * 4.7;
		const double jx = 0.26 * sin(w1) + 0.15 * sin(w2 * 1.71);
		const double jy = 0.26 * cos(w1 * 0.83) + 0.15 * cos(w2);

		p.rpos = e.trailLast + dP * t            // where the ship WAS at this shed,
		                                         // PLANET-RELATIVE (round 7)
		       - flowG * (size * (double)(clampf(g_fx.plasTrailStart, -5.0f, 5.0f) - 2.5f))
		                                         // standoff = the Trail start knob (A.7:
		                                         // recentred, effective = knob - 2.5),
		                                         // matching the head anchor
		       + p1 * (size * (jx + 0.06 * ((double)h1 - 0.5)))
		       + p2 * (size * (jy + 0.06 * ((double)h2 - 0.5)));
		p.hRef  = hRef;
		p.gdir  = flowG;                         // per-particle constant: the column bends
		                                         // because SUCCESSIVE sheds rotate, never
		                                         // because anything reads a neighbour
		p.age   = (float)((1.0 - t) * simdt);    // sub-frame: earlier sheds are older
		p.life  = life;
		p.size0 = (float)(size * (0.30 + 0.30 * (double)h2));
		p.halfLen = (float)(spacing * 1.25);     // round 8: 0.85 -> 1.25 - each capsule
		                                         // spans 2.5 sheds, so the flat cores
		                                         // genuinely overlap; neighbours fuse by
		                                         // overlap, which is the ONLY joining there is
		p.heat0 = e.heat;
		p.seed  = h1 * 7.3f + h2;
		p.seq   = e.trailSeq++;              // shed ordinal (reserved)
		p.chain = e.trailChain;              // ribbon membership (phase A)
		// (no emit here - ProjectTrail sweeps the WHOLE pool later this same frame,
		//  during the render, newborns included: still no root gap, one code path.)
	}
	e.trailS   += step;
	e.trailAcc -= (double)n * spacing;
	if (e.trailAcc >= spacing) e.trailAcc = fmod(e.trailAcc, spacing);  // cap dropped sheds
	e.trailLast = CgRel;
}

// ----------------------------------------------------------------------------
// clbkPostStep driver (rounds 3-4). STATE side of the trail: advect/expire the
// pool, shed from every tracked hot vessel, and gather the oapi-side emit
// parameters for the render path. Post-step is the right epoch for the STATE
// (vessel positions here are what the frame renders) - but round 3 measured
// that the CAMERA is still one step stale even here, so the projection itself
// lives in ProjectTrail, called from the render callback with patch (k)'s
// render-camera snapshot. The vessel-anchored plasma stays in pre-step, where
// a tracking camera cancels the epoch and always has.
// ----------------------------------------------------------------------------
void OroModule::UpdateTrailPost(double simdt)
{
	// Mirror of UpdateReentry's master gate. ReleaseReentry (pre-step) already
	// cleared the pool when disarmed; this just keeps the draw buffer empty.
	if (!(g_fx.masterArmed && g_fx.reentryEnabled && g_fx.reentry > 0.001f)) {
		trailVtxN = 0;
		return;
	}

	// Existing population first: ages, advects, expires, emits - and primes the
	// frame context (s_tCam and friends) that the spawners' newborn emits reuse.
	UpdateTrail(simdt);

	// Shedding pass - every tracked hot vessel (full scene, the breakup case).
	// oapiIsVessel guards the pre/post teardown race: a vessel can die inside the
	// step between UpdateReentry's scan and this hook (invariant 14's discipline).
	s_tcN = 0;                               // chain-head cache refills each frame
	for (int i = 0; i < MAX_RENTRY; i++) {
		ReentryVessel& e = rentry[i];
		if (!e.hV || e.heat <= 0.0f) continue;
		if (!oapiIsVessel(e.hV)) continue;
		VESSEL* v = oapiGetVesselInterface(e.hV);
		if (!v) continue;
		VECTOR3 va, flow = _V(0, 0, 1);
		if (v->GetAirspeedVector(FRAME_LOCAL, va)) {
			const double L = length(va);
			if (L > 1.0) flow = va / L;
		}
		SpawnTrail(i, v, flow);
	}

	// Emit parameters for ProjectTrail. The render path makes NO oapi calls
	// (invariant 1), so everything oapi-flavoured is gathered here: the trim and
	// width knobs, and the post-step camera - which is only the FALLBACK now, for
	// clients without patch (k). (The round-3 diagnostic lived here for exactly one
	// flight: it proved the camera does NOT advance by post-step, which is why the
	// projection moved to the render path at all. It has done its job and is gone.)
	s_tTrim = g_fx.reentry * REN_TRIM_GAIN;
	s_tWidK = clampf(g_fx.plasTrailWid, 0.0f, 8.0f);   // range x2'd on request (A.2)
	s_tRotHead = HueRotFromPick(g_fx.plasTrailTint,  TRAIL_HUE_HEAD);   // trail colour picks
	s_tRotTail = HueRotFromPick(g_fx.plasTrailTint2, TRAIL_HUE_TAIL);   // (A.3, 15b's law)
	// (A.6: no ramp state here any more - the ignition ramp is distance-from-head,
	//  computed per knot in EmitTrailChains, and translates with the Trail start knob.)
	GetCam(s_tPostCam);
	s_tPostCamValid = true;

	// THE POOL HORIZON (round 10): where ring eviction actually bites, from the
	// measured scene-wide shed rate (~2 s exponential smoothing - slow enough that
	// the horizon fade never pumps, fast enough to follow a slider drag or a
	// breakup's rate spike within seconds). The emitters ramp brightness to zero
	// approaching this age, so the trail always ENDS BY FADING - the eviction
	// cliff of still-lit sprites was rounds 8-9's bright terminal dash.
	if (simdt > 0.0) {
		const double inst = (double)s_tShedFrame / simdt;
		double k = simdt / 2.0; if (k > 1.0) k = 1.0;
		s_tShedRate += (inst - s_tShedRate) * k;
		s_tHorizon = (s_tShedRate > 1.0) ? (double)TRAIL_MAX / s_tShedRate : 1e9;
	}
	s_tShedFrame = 0;

	// Gather the reference bodies' CURRENT positions for the render path's
	// reconstruction (round 7): particles are planet-relative, ProjectTrail may
	// make no oapi calls (invariant 1), and post-step body state == the state the
	// frame renders (only the CAMERA updates later - round 3's measurement), so
	// these are epoch-exact for the render.
	s_tRefN = 0;
	for (int k = 0; k < TRAIL_MAX && s_tRefN < TRAIL_MAX_REF; k++) {
		if (trail[k].age < 0.0f || !trail[k].hRef) continue;
		bool have = false;
		for (int q = 0; q < s_tRefN; q++) if (s_tRef[q] == trail[k].hRef) { have = true; break; }
		if (!have) {
			s_tRef[s_tRefN] = trail[k].hRef;
			oapiGetGlobalPos(trail[k].hRef, &s_tRefPos[s_tRefN]);
			s_tRefN++;
		}
	}

	// (The round-7 diagnostic lived here and RETIRED on the user's "rock solid"
	//  confirmation, 2026-08-08. Its two verdicts, preserved because they were
	//  bought with a day of jumping trails: (1) "target speed 29906 m/s" - Orbiter's
	//  global frame is BARYCENTRIC, and any absolute-position bookkeeping must be
	//  planet-relative or it records the path around the Sun; (2) the render runs
	//  a step of body state ahead of clbkPostStep (~2 km of Earth motion at 10x
	//  warp), so world-anchored reconstruction must take camera AND anchors from
	//  the renderer itself - patches (k) and (k2).)

	// One-shot announce of the patch-(k) binding (the b/d/f/g logging discipline:
	// a silent degradation - here, back to the one-step-stale camera and its origin
	// jumps - must name itself in the log). Probed by BINDING, invariant 18(a).
	static int kLogged = -1;
	const int kNow = (pCore && pCore->CanGetRenderCam()) ? 1 : 0;
	if (kLogged != kNow) {
		kLogged = kNow;
		oapiWriteLogV(kNow
			? "ORO: render-camera snapshot (patch k) bound - trail projects with the exact render camera."
			: "ORO: render-camera snapshot (patch k) NOT available - trail falls back to the post-step camera (expect origin jitter).");
	}
}

// ----------------------------------------------------------------------------
// Render-path projection (round 4, patch k). Called from the render callbacks
// (DrawPreResolve, or the HUD_2ND fallback slots) just before DrawTrailPoly -
// the ONLY place the camera the frame is actually rendered with exists. Round 3
// measured pre-step AND post-step cameras one full step stale; at entry speed
// that is ~120 m of parallax on close world-anchored particles, the origin-jump
// bug that hit BOTH trail implementations.
// INVARIANT-1 AUDIT: zero oapi calls. gcCore::GetRenderCam is a client call (the
// CopyResource / GetBackBufferHandle precedent - both already run mid-render),
// viewW/viewH are the cached copies, PCol is pure math, and the g_fx reads are
// the same single-thread reads every render callback has always done.
// ----------------------------------------------------------------------------
void OroModule::ProjectTrail()
{
	trailVtxN = 0;
	if (!(g_fx.masterArmed && g_fx.reentryEnabled && g_fx.reentry > 0.001f)) return;
	if (!(extGate || vcGate) || viewW == 0 || viewH == 0) return;

	// THE camera. Patch (k) = the render camera, exact; fallback = the post-step
	// snapshot, one step stale (the old behaviour) - degraded, not broken.
	s_tProj = false;
	VECTOR3 cp; MATRIX3 cr; double ct;
	if (pCore && pCore->CanGetRenderCam() && pCore->GetRenderCam(&cp, &cr, &ct)) {
		s_tCam.pos = cp; s_tCam.rot = cr; s_tCam.tanAp = ct;
		s_tProj = true;
	} else if (s_tPostCamValid) {
		s_tCam = s_tPostCam;
		s_tProj = true;
	}
	if (!s_tProj) return;

	// THE ANCHOR, render-epoch (patch k2). The reconstruction is body position +
	// rpos, and the body position must be the one THE RENDERER is using this frame
	// - the post-step values gathered in UpdateTrailPost can lag the render by a
	// step, which shifts the whole trail by the body's 30 km/s barycentric motion
	// x the gap (~500 m) and jitters it with frame pacing ("jumpy and offset",
	// 2026-08-08, the report that forced camera AND anchor onto the renderer's own
	// numbers). Falls back to the post-step values on a client without (k2).
	if (pCore && pCore->CanGetRenderObjPos()) {
		for (int q = 0; q < s_tRefN; q++) {
			VECTOR3 rp;
			if (pCore->GetRenderObjPos(s_tRef[q], &rp)) s_tRefPos[q] = rp;
		}
	}

	// PHASE A: one mitred feathered ribbon per chain, threaded through the pool -
	// newborns to the synthetic head, no root gap, no stacking. See EmitTrailChains.
	EmitTrailChains();

	// Invariant 3: the render proc hands the client the FULL buffer - pad the tail.
	if (trailVtxN > 0 && trailVtxN < TRAIL_MAX_TRI * 3) {
		memset(&trailVtx[trailVtxN],   0, sizeof(PlasVtx) * (TRAIL_MAX_TRI * 3 - trailVtxN));
		memset(&trailDepth[trailVtxN], 0, sizeof(float)   * (TRAIL_MAX_TRI * 3 - trailVtxN));
	}
}

// ----------------------------------------------------------------------------
// The custom-drawn plasma (round 2): envelope + stagnation core + filament
// ribbons + wake blobs, as ONE screen-space triangle list. Everything here is
// PROJECTED GEOMETRY - the render callback draws it blind (invariant 1), so
// this runs on the main thread with full oapi access. Screen-space because
// gcCore's clrVtx is 2D by construction; the projection lags the camera by one
// frame exactly like the exhaust shimmer, which nobody has ever noticed.
//
// Hull occlusion is geometric (invariant 11 - there is no depth buffer): the
// windward/leeward facing of the CAMERA decides what fades. Emissive fog is
// the most forgiving case there is - in the reference shots the glow visibly
// blooms ACROSS the craft's silhouette, so soft errors read as plasma, not bugs.
// ----------------------------------------------------------------------------
void OroModule::BuildPlasmaGeometry(int i, VESSEL* v, const VECTOR3& flowLocal)
{
	static_assert(sizeof(PlasVtx) == sizeof(gcCore::clrVtx),
	              "PlasVtx must mirror gcCore::clrVtx - the render proc casts between them");

	ReentryVessel& e = rentry[i];
	// COLD reads as EXACTLY zero, never as a small number: every vessel-centred
	// layer scales off heat (directly or through A below), so zero is what switches
	// them all off at once.
	const float heat = (e.heat > 0.01f) ? e.heat : 0.0f;
	const float trim = g_fx.reentry * REN_TRIM_GAIN;
	// The palette knob is read ONCE per build and every PCol below obeys it.
	// Floored at 0.25: at exactly zero every channel collapses onto red and the
	// effect would be a flat monochrome silhouette, which is not a look anyone wants
	// at the bottom of a slider.
	g_palSat = clampf(g_fx.plasSat, 0.25f, 2.0f);
	// TINT read once per build too (COLORREF 0x00BBGGRR -> 0..1 per channel). White = no-op.
	g_palHueRot  = HueRotFromPick(g_fx.plasmaTint,  PAL_HUE_BODY);
	g_palHueRot2 = HueRotFromPick(g_fx.plasmaTint2, PAL_HUE_FRINGE);
	if (viewW == 0 || viewH == 0) return;
	if (heat <= 0.0f) return;                // nothing left to draw once cold

	CamCtx cc; GetCam(cc);
	VECTOR3 Cg;    v->GetGlobalPos(Cg);
	VECTOR3 flowG; v->GlobalRot(flowLocal, flowG);
	const double size = v->GetSize() > 1.0 ? v->GetSize() : 1.0;

	// Pixels of a world length w seen at depth z (round 3.5). Element sizes used
	// to scale off ONE vessel-wide Rpx - valid only with the whole ship at a
	// similar depth. The VC view puts the camera INSIDE the hull, where that
	// anchor is degenerate (the old build simply returned); now every element
	// projects its own size at its own depth, which is also more correct in
	// close external shots.
	auto pxAt = [&](double z, double w) -> float {
		return (float)(w / (z * cc.tanAp) * (viewH * 0.5));
	};

	float c0x = 0.0f, c0y = 0.0f; double z0 = 1.0;
	const bool haveC0 = ProjPx(cc, Cg, viewW, viewH, c0x, c0y, z0);
	if (!vcGate) {
		if (!haveC0) return;                     // vessel behind the camera
		if (pxAt(z0, size) < 3.0f) return;       // too small - the light carries it
	}

	// Screen flow directions, with safe fallbacks for the VC (the centre may not
	// project when the camera sits inside the hull).
	float fdx = 1.0f, fdy = 0.0f;
	if (haveC0) {
		float f1x, f1y; double z1;
		if (ProjPx(cc, Cg + flowG * size, viewW, viewH, f1x, f1y, z1)) {
			const float lx = f1x - c0x, ly = f1y - c0y;
			const float l = sqrtf(lx * lx + ly * ly);
			if (l > 0.5f) { fdx = lx / l; fdy = ly / l; }
		}
	}
	const float ddx = -fdx, ddy = -fdy;      // downstream (wake) screen direction
	const float pdx = -ddy, pdy = ddx;       // perpendicular

	// The hull point field -> screen, once. Windwardness comes from the LOCAL normal
	// against the local flow (no transform needed); per-point camera-facing from the
	// GLOBAL normal - a point on the far side of the skin fades out. That per-point
	// backface fade is the cheap substitute for the depth buffer we don't have
	// (invariant 11), and for emissive fog it is enough.
	MATRIX3 Rv; v->GetRotationMatrix(Rv);
	float hpx[MAX_HULLPT], hpy[MAX_HULLPT], hpz[MAX_HULLPT], hwd[MAX_HULLPT], hcam[MAX_HULLPT];
	float hped[MAX_HULLPT];                        // EUCLIDEAN camera distance, for the
	                                               // patch-(g) clip (hpz is the camera-space
	                                               // z, which pxAt wants and the clip does not)
	bool  hok[MAX_HULLPT];
	for (int k = 0; k < e.nHull; k++) {
		const HullPt& hp = e.hull[k];
		const VECTOR3 pg = Cg + mul(Rv, hp.pos);
		double hz;
		hok[k] = ProjPx(cc, pg, viewW, viewH, hpx[k], hpy[k], hz);
		hwd[k] = (float)dotp(hp.nrm, flowLocal);   // camera-independent; the stream
		                                           // selection reads it for ALL
		                                           // points, on-screen or not (2.8.1)
		if (!hok[k]) continue;
		hpz[k] = (float)hz;                        // per-point depth (3.5 sizing)
		hped[k] = (float)length(pg - cc.pos);
		const VECTOR3 ng = mul(Rv, hp.nrm);
		hcam[k] = (float)dotp(ng, unit(cc.pos - pg));
	}

	const float A = 255.0f * heat * trim;
	const float t = (float)animT;            // real-time flicker clock

	// DEPTH FOR THE PATCH-(g) CLIP (2026-08-07). Every vertex carries its EUCLIDEAN
	// distance from the camera, to match the client's GBUF_DEPTH.a = length(posW).
	// Two emitters rather than one, because the draw list splits cleanly in two:
	//   emit3  - the FLAT elements (edge dashes, face wash, origin glow, sparks). Each is a
	//            small screen-space figure built around ONE projected point, so all three
	//            vertices share that point's distance. It reads emitZ, set once per element
	//            just before the group - which is 5 assignments instead of threading a
	//            third coordinate through 22 call sites that would all pass the same value.
	//   emit3z - the elements that genuinely SPAN depth: the shock shell (a triangle across
	//            the hull) and the streak ribbons (knots kilometres apart). One depth for
	//            those would clip a whole streak on its near end or none of it.
	// emitZ defaults to 0 = at the camera = never clipped, so a site that somehow reached
	// an emitter without setting it degrades to the old paint-over behaviour rather than
	// vanishing. The clip is off in external view anyway (see DrawPlasmaPoly's call sites).
	float emitZ = 0.0f;
	auto emit3 = [&](float ax, float ay, DWORD ac, float bx, float by, DWORD bc,
	                 float cx2, float cy2, DWORD cc2) {
		if (plasVtxN + 3 > PLAS_MAX_TRI * 3) return;
		plasVtx[plasVtxN].x = ax;  plasVtx[plasVtxN].y = ay;  plasVtx[plasVtxN].c = ac;  plasDepth[plasVtxN] = emitZ; plasVtxN++;
		plasVtx[plasVtxN].x = bx;  plasVtx[plasVtxN].y = by;  plasVtx[plasVtxN].c = bc;  plasDepth[plasVtxN] = emitZ; plasVtxN++;
		plasVtx[plasVtxN].x = cx2; plasVtx[plasVtxN].y = cy2; plasVtx[plasVtxN].c = cc2; plasDepth[plasVtxN] = emitZ; plasVtxN++;
	};
	auto emit3z = [&](float ax, float ay, float az, DWORD ac, float bx, float by, float bz, DWORD bc,
	                  float cx2, float cy2, float cz2, DWORD cc2) {
		if (plasVtxN + 3 > PLAS_MAX_TRI * 3) return;
		plasVtx[plasVtxN].x = ax;  plasVtx[plasVtxN].y = ay;  plasVtx[plasVtxN].c = ac;  plasDepth[plasVtxN] = az;  plasVtxN++;
		plasVtx[plasVtxN].x = bx;  plasVtx[plasVtxN].y = by;  plasVtx[plasVtxN].c = bc;  plasDepth[plasVtxN] = bz;  plasVtxN++;
		plasVtx[plasVtxN].x = cx2; plasVtx[plasVtxN].y = cy2; plasVtx[plasVtxN].c = cc2; plasDepth[plasVtxN] = cz2; plasVtxN++;
	};

	// --- 1. EDGE LIGHT (round 3 - the clean slate). What the reference shows at
	// the vehicle is exactly one thing: the WINDWARD SILHOUETTE EDGE burning
	// near-white inside a thin magenta fringe, the dark hull crisply visible
	// within, and the windward face washed faintly behind it. No ring geometry:
	// screen-space edge DETECTION per hull point instead. A point is ON the
	// silhouette when its normal is perpendicular to the view ray (|hcam| ~ 0),
	// and it BURNS when it also faces the wind (hwd > 0). Each such point draws a
	// short soft dash ALIGNED WITH THE LOCAL SILHOUETTE DIRECTION (normal x view,
	// projected per point) - additively, neighbouring dashes chain into a
	// continuous lit edge that follows the actual hull: nose cone, wing leading
	// edges, pods. Leeward and face-on points simply do not light, so there is
	// nothing to wrap around and no bubble from any angle.
	{
		const float edgeGain = clampf(g_fx.plasComa, 0.0f, 2.0f);   // "Edge light" knob
		const float heatK    = 0.25f + 0.75f * heat;
		for (int k = 0; k < e.nHull; k++) {
			if (!hok[k] || hwd[k] <= 0.03f) continue;
			const float edgeW = clampf(1.0f - fabsf(hcam[k]) / 0.35f, 0.0f, 1.0f);
			const float faceW = clampf(hcam[k], 0.0f, 1.0f);
			const float aE = A * edgeGain * heatK * powf(hwd[k], 1.2f) * edgeW * edgeW;
			const float aW = A * 0.06f * heatK * hwd[k] * faceW;
			if (aE < 2.0f && aW < 2.0f) continue;

			// local silhouette direction on screen: the surface tangent that is
			// perpendicular to the VIEW ray, projected for THIS point (per-point 3D
			// projection - the invariant-15 rule; a shared screen direction would
			// be degenerate somewhere)
			const VECTOR3 pg = Cg + mul(Rv, e.hull[k].pos);
			const VECTOR3 ng = mul(Rv, e.hull[k].nrm);
			const VECTOR3 vd = unit(pg - cc.pos);
			VECTOR3 tg = crossp(ng, vd);
			const double tgl = length(tg);
			float ux = pdx, uy = pdy;                    // fallback: screen flow-perp
			if (tgl > 0.05) {
				tg = tg / tgl;
				float tpx, tpy; double tz;
				if (ProjPx(cc, pg + tg * (size * 0.06), viewW, viewH, tpx, tpy, tz)) {
					const float tdx = tpx - hpx[k], tdy = tpy - hpy[k];
					const float tdl = sqrtf(tdx * tdx + tdy * tdy);
					if (tdl > 0.3f) { ux = tdx / tdl; uy = tdy / tdl; }
				}
			}
			const float vx = -uy, vy = ux;
			const float cx = hpx[k], cy = hpy[k];
			const float h  = hashf((float)k * 3.7f);
			const float n  = 1.0f + 0.12f * sinf(t * 5.7f + k * 2.3f);
			emitZ = hped[k];   // both the dash/fringe and the wash sit on this hull point

			if (aE >= 2.0f) {
				// the white dash: bright centre, alpha-0 tips and sides - chained
				// additively into the reference's saturated edge line
				const float L = pxAt(hpz[k], size * 0.060) * (0.8f + 0.4f * h) * n;
				const float W = pxAt(hpz[k], size * 0.014) * (0.8f + 0.4f * h);
				const DWORD cw = PCol(255, 245, 236, (int)aE);
				const DWORD c0 = PCol(255, 245, 236, 0);
				const float ex0 = cx - ux * L, ey0 = cy - uy * L;
				const float ex1 = cx + ux * L, ey1 = cy + uy * L;
				emit3(ex0, ey0, c0, cx, cy, cw, cx + vx * W, cy + vy * W, c0);
				emit3(ex0, ey0, c0, cx, cy, cw, cx - vx * W, cy - vy * W, c0);
				emit3(ex1, ey1, c0, cx, cy, cw, cx + vx * W, cy + vy * W, c0);
				emit3(ex1, ey1, c0, cx, cy, cw, cx - vx * W, cy - vy * W, c0);
				// the magenta fringe: the same dash, longer, much wider, fainter -
				// the reference's pink halo hugging the white line
				const DWORD cp  = PCol(255, 120, 225, (int)(aE * 0.35f));
				const DWORD cp0 = PCol(255, 110, 220, 0);
				const float Lf = L * 1.6f, Wf = W * 3.2f;
				const float fx0 = cx - ux * Lf, fy0 = cy - uy * Lf;
				const float fx1 = cx + ux * Lf, fy1 = cy + uy * Lf;
				emit3(fx0, fy0, cp0, cx, cy, cp, cx + vx * Wf, cy + vy * Wf, cp0);
				emit3(fx0, fy0, cp0, cx, cy, cp, cx - vx * Wf, cy - vy * Wf, cp0);
				emit3(fx1, fy1, cp0, cx, cy, cp, cx + vx * Wf, cy + vy * Wf, cp0);
				emit3(fx1, fy1, cp0, cx, cy, cp, cx - vx * Wf, cy - vy * Wf, cp0);
			}
			if (aW >= 2.0f) {
				// the face wash: a small dim warm fan on the camera-facing windward
				// skin - the reference's belly is lit, softly, behind the edge
				int r, g, b; PlasmaRamp(0.30f, r, g, b);
				const float gr = pxAt(hpz[k], size * 0.10) * (0.75f + 0.5f * h);
				const DWORD f0 = PCol(r, g, b, (int)aW);
				const DWORD f1 = PCol(r, g, b, 0);
				const int NF = 5;
				for (int q = 0; q < NF; q++) {
					const float a0r = (float)q / NF * 6.2831853f;
					const float a1r = (float)(q + 1) / NF * 6.2831853f;
					emit3(cx, cy, f0,
					      cx + cosf(a0r) * gr, cy + sinf(a0r) * gr, f1,
					      cx + cosf(a1r) * gr, cy + sinf(a1r) * gr, f1);
				}
			}
		}
	}

	// --- 2. SHOCK SHELL (ROUND 4 - the ultracode design study, 2026-08-02; the
	// splat and heightfield eras are both closed). This is a welded, decimated
	// COPY OF THE VESSEL'S OWN MESH TRIANGLES (BuildShell - the templates expose
	// their full index lists), flow-shifted by the Shock dist knob and drawn as
	// connected Gouraud triangles. Coverage is the author's own topology: the
	// splat-era blobs and the grid-era shards have no mechanism to exist here.
	// Settled laws all kept: the sheet LEADS the entry (full by ~35% heat),
	// ORANGE -> WHITE-HOT by windwardness, LIMB-BRIGHTENED emissive visibility
	// (edge-on burns, only clearly-face-away fades - the rim wraps the silhouette
	// from every angle), the standoff rides the FLOW AXIS only (3.6.6), and true
	// mesh boundaries feather. Per-vertex projection at per-vertex depth: VC-safe.
	{
		const float shB = clampf(g_fx.plasShockBright, 0.0f, 3.0f);
		// The bake lasted half a day (2026-08-08): 0.015 was the DG's number, and the
		// Atlantis promptly SANK the shell into its hull. Standoff is a fact about
		// each hull - like the VC shadow box - so it is a per-class knob again
		// ("Shell dist", default 0.015). The 5.11 pattern only applies when the found
		// number is a LAW, not a property.
		const float shD = clampf(g_fx.plasShellDist, 0.0f, 0.08f);
		if (shB > 0.01f && heat > 0.0f && e.shellBuilt && e.nShellT > 0) {
			const float heatE = clampf(heat / 0.35f, 0.0f, 1.0f);

			// pass 1: windwardness per shell vertex - camera-independent
			for (int k = 0; k < e.nShellV; k++) {
				shellWd[k] = e.shellNrm[k * 3]     * (float)flowLocal.x
				           + e.shellNrm[k * 3 + 1] * (float)flowLocal.y
				           + e.shellNrm[k * 3 + 2] * (float)flowLocal.z;
				shellNeed[k] = 0;
			}
			// pass 2: a triangle lives if ANY corner is windward - terminator-
			// straddling triangles carry the rim past the silhouette and feather
			// to zero alpha there (no silhouette machinery, no screen bins:
			// camera-motion stable by construction)
			for (int ti = 0; ti < e.nShellT; ti++) {
				const int a2 = e.shellIdx[ti * 3], b2 = e.shellIdx[ti * 3 + 1], c3 = e.shellIdx[ti * 3 + 2];
				float mw = shellWd[a2];
				if (shellWd[b2] > mw) mw = shellWd[b2];
				if (shellWd[c3] > mw) mw = shellWd[c3];
				if (mw > 0.02f) { shellNeed[a2] = 1; shellNeed[b2] = 1; shellNeed[c3] = 1; }
			}
			// pass 2a: THE HULL DEPTH MAP (round 5.1) - the see-through-the-hull fix.
			// With no depth buffer (invariant 11) the shell painted its far side
			// straight over its near side: at high AoA the belly sheet seen from
			// above is EDGE-ON, which is exactly what limb brightening makes
			// BRIGHTEST, so it wrote a full-strength wash across the DG's top hull
			// (user's screenshots, 2026-08-02). No normal-based term can fix that -
			// an edge-on surface at the silhouette (visible, must stay: G5) and an
			// edge-on surface mid-hull (hidden, must go) have the SAME normal.
			// So we build the missing discriminator out of the mesh copy we already
			// own: a min-depth grid over the vessel's screen box, rasterized from the
			// UN-OFFSET shell triangles - the real skin, LEEWARD SIDE INCLUDED (the
			// occluder is the top hull, which is never in the windward draw set).
			// Three properties fall out for free: outside the silhouette there is no
			// occluder at all, so the wrapping rim survives BY CONSTRUCTION; taking
			// the MINIMUM depth makes the near surface win with no winding
			// assumption; and the test below compares SKIN to SKIN, so it is
			// independent of the standoff knob.
			bool  hzOn = false;
			float hzX0 = 0.0f, hzY0 = 0.0f, hzIW = 0.0f, hzIH = 0.0f, hzCW = 0.0f, hzCH = 0.0f;
			if (!vcGate && e.nShellV > 0) {
				float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
				for (int k = 0; k < e.nShellV; k++) {
					const VECTOR3 lp2 = _V(e.shellPos[k * 3], e.shellPos[k * 3 + 1],
					                       e.shellPos[k * 3 + 2]);
					const VECTOR3 gp2 = Cg + mul(Rv, lp2);
					double zz2;
					hullOk[k] = ProjPx(cc, gp2, viewW, viewH, hullPx[k], hullPy[k], zz2) ? 1 : 0;
					if (!hullOk[k]) continue;
					hullPz[k] = (float)zz2;
					if (hullPx[k] < bx0) bx0 = hullPx[k];
					if (hullPx[k] > bx1) bx1 = hullPx[k];
					if (hullPy[k] < by0) by0 = hullPy[k];
					if (hullPy[k] > by1) by1 = hullPy[k];
				}
				if (bx1 > bx0 + 1.0f && by1 > by0 + 1.0f) {
					// one cell of margin all round, so rim vertices land INSIDE the
					// grid and the edge cells stay empty (= never occluding)
					hzCW = (bx1 - bx0) / (float)(HZ_N - 2);
					hzCH = (by1 - by0) / (float)(HZ_N - 2);
					hzX0 = bx0 - hzCW;  hzY0 = by0 - hzCH;
					hzIW = 1.0f / hzCW; hzIH = 1.0f / hzCH;
					for (int c = 0; c < HZ_N * HZ_N; c++) hullZ[c] = 1e30f;
					for (int ti = 0; ti < e.nShellT; ti++) {
						const int a2 = e.shellIdx[ti * 3], b2 = e.shellIdx[ti * 3 + 1],
						          c3 = e.shellIdx[ti * 3 + 2];
						if (!hullOk[a2] || !hullOk[b2] || !hullOk[c3]) continue;
						const float ax = hullPx[a2], ay = hullPy[a2];
						const float bx = hullPx[b2], by = hullPy[b2];
						const float cx3 = hullPx[c3], cy3 = hullPy[c3];
						const float ar = (bx - ax) * (cy3 - ay) - (by - ay) * (cx3 - ax);
						if (fabsf(ar) < 1e-3f) continue;         // degenerate on screen
						const float inv = 1.0f / ar;
						float mnx = ax < bx ? ax : bx; if (cx3 < mnx) mnx = cx3;
						float mxx = ax > bx ? ax : bx; if (cx3 > mxx) mxx = cx3;
						float mny = ay < by ? ay : by; if (cy3 < mny) mny = cy3;
						float mxy = ay > by ? ay : by; if (cy3 > mxy) mxy = cy3;
						int i0 = (int)((mnx - hzX0) * hzIW), i1 = (int)((mxx - hzX0) * hzIW);
						int j0 = (int)((mny - hzY0) * hzIH), j1 = (int)((mxy - hzY0) * hzIH);
						if (i0 < 0) i0 = 0; if (i1 > HZ_N - 1) i1 = HZ_N - 1;
						if (j0 < 0) j0 = 0; if (j1 > HZ_N - 1) j1 = HZ_N - 1;
						for (int j = j0; j <= j1; j++) {
							const float py = hzY0 + ((float)j + 0.5f) * hzCH;
							for (int i2 = i0; i2 <= i1; i2++) {
								const float px = hzX0 + ((float)i2 + 0.5f) * hzCW;
								// edge functions -> barycentric weights; the sign of
								// inv carries the winding, so both faces rasterise
								const float wC = ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) * inv;
								const float wA = ((cx3 - bx) * (py - by) - (cy3 - by) * (px - bx)) * inv;
								const float wB = 1.0f - wA - wC;
								if (wA < 0.0f || wB < 0.0f || wC < 0.0f) continue;
								const float zc = hullPz[a2] * wA + hullPz[b2] * wB + hullPz[c3] * wC;
								if (zc < hullZ[j * HZ_N + i2]) hullZ[j * HZ_N + i2] = zc;
							}
						}
					}
					hzOn = true;
				}
			}
			// Nearest skin depth at a screen point, ERODED by one cell: a cell counts
			// as an occluder only if its four neighbours are covered too. That trades
			// a one-cell leak just inside the silhouette for ZERO rim erosion -
			// deliberately, because G5 says eating the rim is the worse failure.
			auto hullDepth = [&](float px, float py) -> float {
				if (!hzOn) return 1e30f;
				const int i2 = (int)((px - hzX0) * hzIW), j = (int)((py - hzY0) * hzIH);
				if (i2 < 1 || j < 1 || i2 >= HZ_N - 1 || j >= HZ_N - 1) return 1e30f;
				float z = hullZ[j * HZ_N + i2];
				const float zl = hullZ[j * HZ_N + i2 - 1], zr = hullZ[j * HZ_N + i2 + 1];
				const float zu = hullZ[(j - 1) * HZ_N + i2], zd = hullZ[(j + 1) * HZ_N + i2];
				if (zl > z) z = zl;
				if (zr > z) z = zr;
				if (zu > z) z = zu;
				if (zd > z) z = zd;
				return z;
			};
			// The occlusion band. eps covers the grid's own interpolation error only -
			// NOT hull thickness - because the comparison is a vertex's SKIN point
			// against the nearest skin in its cell. Surfaces thinner than the band
			// still bleed (a wing's own belly glow reaching its top face), which the
			// reference shows wrapping anyway; the fuselage, which is what wrote the
			// wash, is metres thick and goes fully opaque.
			const float hzEps  = (float)size * 0.005f + 0.20f;
			const float hzSpan = (float)size * 0.020f + 0.40f;

			// THE SHELL IS DRAWN AS A VOLUME, NOT A SURFACE (2026-08-07, after his KSP
			// reference shot: "the air around the vessel has caught fire and it WRAPS
			// AROUND THE HULL"). One surface can only ever be a tinted skin - you see the
			// hull through it and the far side through the near side, which is exactly how
			// ours read: coloured glass, not fire. Fire needs DEPTH, so the shell is drawn
			// as several concentric copies at growing standoff and additively stacked.
			// THIS IS THE AURORA THICKNESS TRICK (invariant 19b), which is proven and, more
			// importantly, is NOT graveyard G3: G3 was a screen-space silhouette RING with no
			// relation to hull shape, and it read as a bubble. These are the vessel's own
			// welded mesh triangles - the thing that already works - just repeated outward.
			// Per-layer alpha falls as 1/sqrt(N) (19b again), so N layers give sqrt(N) times
			// the brightness, not N: the stack buys DENSITY and limb brightening (edge-on you
			// look through more layers than face-on, which is what makes the rim burn) rather
			// than simply multiplying everything into a white blob.
			for (int sl = 0; sl < SHELL_LAYERS; sl++) {
				// pass 3: project + shade each needed vertex ONCE (colour cached -
				// a shared vertex is shaded once, not once per triangle)
				for (int k = 0; k < e.nShellV; k++) {
					shellOk[k] = 0;
					if (!shellNeed[k]) continue;
					const float swdk = shellWd[k];
					// standoff STRICTLY along the flow axis (the 3.6.6 law): a smooth
					// scalar times one constant direction cannot zigzag. Bows out at
					// the stagnation region, relaxes toward the rim.
					const float dOff = shD * (float)size * (0.65f + 0.35f * clampf(swdk, 0.0f, 1.0f))
				                 * SHELL_LAYER_OFF[sl];
					const VECTOR3 lp = _V(e.shellPos[k * 3]     + flowLocal.x * dOff,
					                      e.shellPos[k * 3 + 1] + flowLocal.y * dOff,
					                      e.shellPos[k * 3 + 2] + flowLocal.z * dOff);
					const VECTOR3 gp = Cg + mul(Rv, lp);
					double zz;
					if (!ProjPx(cc, gp, viewW, viewH, shellSx[k], shellSy[k], zz)) continue;
					shellSz[k] = (float)length(gp - cc.pos);   // patch-(g) clip depth, at the
					                                           // OFFSET point - the surface drawn
					const VECTOR3 ng = mul(Rv, _V(e.shellNrm[k * 3], e.shellNrm[k * 3 + 1],
					                              e.shellNrm[k * 3 + 2]));
					const float hc = (float)dotp(ng, unit(cc.pos - gp));
					// the alpha chain - T6 limb model plus the study's grafted guards
					const float wq   = clampf((swdk - 0.02f) / 0.98f, 0.0f, 1.0f);  // gate
					                                                 // and alpha-zero coincide
					const float wf   = clampf((wq - 0.30f) / 0.70f, 0.0f, 1.0f);
					const float acS  = fabsf(hc);
					const float limb = 0.55f + 0.75f * (1.0f - acS) * (1.0f - acS);
					const float occl = clampf((hc + 0.5f) / 0.5f, 0.0f, 1.0f);
					// ... and the real occluder (round 5.1): this vertex's own SKIN point
					// against the nearest skin in its cell. On the near side it IS the
					// nearest, so nothing changes; on the far side the hull in front of it
					// wins and it fades out. Off the silhouette there is no hull to lose to.
					float occH = 1.0f;
					if (hzOn && hullOk[k]) {
						const float zNear = hullDepth(hullPx[k], hullPy[k]);
						occH = clampf(1.0f - (hullPz[k] - (zNear + hzEps)) / hzSpan, 0.0f, 1.0f);
					}
					const float visS = vcGate ? VC_VIS * VC_GAIN : limb * occl * occH;
					const float fe   = (e.shellFlg[k] & SH_OPENEDGE) ? 0.65f : 1.0f;
					// near-plane fade: dim to nothing approaching the ProjPx z=1 gate
					// instead of popping (the VC case). TIGHTER IN THE VC (2026-08-07): at the
					// 2 m span the shell just outside the canopy - a metre or two from the eye,
					// and precisely what you now see through the window - was landing at half
					// alpha or less. The fade exists to stop a pop at the z=1 gate, and 0.6 m
					// still does that; the wide span was buying safety the depth clip now
					// provides. External keeps the generous span, where nothing is that close.
					const double nfSpan = vcGate ? 0.6
					                    : ((0.02 * size > 2.0) ? 0.02 * size : 2.0);
					const float nearFade = clampf((float)((zz - 1.0) / nfSpan), 0.0f, 1.0f);
					// real-time flicker whose phase rides the flow coordinate: one
					// coherent ripple travelling downstream, not per-vertex sparkle
					const float flick = 0.90f + 0.10f * sinf(t * 3.7f
					                  - 6.0f * (float)dotp(lp, flowLocal) / (float)size);
					const float aRaw = A * 0.55f * shB * heatE * powf(wq, 0.7f)
					                 * visS * fe * nearFade * flick * SHELL_LAYER_A;
					// SOFT clamp: A reaches 2550 at full trim, and a hard clamp would
					// flatten the Gouraud sheet into a saturated cutout - the
					// exponential compresses instead
					const float aSoft = 255.0f * (1.0f - expf(-aRaw / 255.0f));
					// Shell palette. 5.5 built this as orange -> PINK and STOPPED THERE: the
					// old top end was (255,135,195), so the shell could not render white at
					// any windwardness, at any heat, with any slider. That is most of why
					// ours read as tinted glass while the reference is a vessel inside fire -
					// its whole forward face is blown out white.
					// THREE bands now, and the middle one is 5.5's law, not a casualty of it:
					// the route to white goes THROUGH MAGENTA (b > g), never through cream,
					// because cream is what additive stacking degenerates into and pink is
					// what the real thing does. So: deep orange over the body, magenta as it
					// turns into the wind, white-hot only on the most windward fifth - which
					// is where the reference puts it too.
					int shR, shG, shB;
					if (wf < 0.55f) {                    // body: deep orange
						const float s = wf / 0.55f;
						shR = 255; shG = 80 + (int)(45 * s); shB = 26 + (int)(24 * s);
					} else if (wf < 0.80f) {             // shoulder: through MAGENTA (b > g)
						const float s = (wf - 0.55f) / 0.25f;
						shR = 255; shG = 125 + (int)(25 * s); shB = 50 + (int)(150 * s);
					} else {                             // stagnation: WHITE-HOT
						const float s = (wf - 0.80f) / 0.20f;
						shR = 255; shG = 150 + (int)(100 * s); shB = 200 + (int)(45 * s);
					}
					shellCol[k] = PCol(shR, shG, shB, (int)aSoft);
					shellOk[k] = 1;
				}
				// pass 4: emit from the caches. The oversize guard kills near-plane
				// stretched triangles (belt-and-braces with nearFade, mainly the VC);
				// a corner behind the near plane drops its triangles - in the VC that
				// opens a small hole exactly at the camera, the accepted graceful mode.
				const float ovLim = 1.5f * (float)viewW;
				for (int ti = 0; ti < e.nShellT; ti++) {
					const int a2 = e.shellIdx[ti * 3], b2 = e.shellIdx[ti * 3 + 1], c3 = e.shellIdx[ti * 3 + 2];
					if (!shellOk[a2] || !shellOk[b2] || !shellOk[c3]) continue;
					float mw = shellWd[a2];
					if (shellWd[b2] > mw) mw = shellWd[b2];
					if (shellWd[c3] > mw) mw = shellWd[c3];
					if (mw <= 0.02f) continue;
					if (fabsf(shellSx[a2] - shellSx[b2]) > ovLim || fabsf(shellSy[a2] - shellSy[b2]) > ovLim ||
					    fabsf(shellSx[b2] - shellSx[c3]) > ovLim || fabsf(shellSy[b2] - shellSy[c3]) > ovLim ||
					    fabsf(shellSx[a2] - shellSx[c3]) > ovLim || fabsf(shellSy[a2] - shellSy[c3]) > ovLim)
						continue;
					emit3z(shellSx[a2], shellSy[a2], shellSz[a2], shellCol[a2],
					       shellSx[b2], shellSy[b2], shellSz[b2], shellCol[b2],
					       shellSx[c3], shellSy[c3], shellSz[c3], shellCol[c3]);
				}
			}
		}
	}

	// --- 3. FINS + WRAP + BOWL + SKIRT (FIREFLY REWORK 2, 2026-08-08; round 2.1
	// after the first fly-and-report). The 18-bin stream system (rounds 2.7-5.12)
	// is GONE - replaced by the geometry model read out of Firefly's own shader
	// source (MainPass/BowshockPass.cginc, investigation 2026-08-08), reimplemented
	// from the technique, not copied (Firefly is GPL-3).
	//
	// THE MODEL:
	//  - Fins spawn from the RIM BAND: a shell corner qualifies when it is
	//    tangent-to-slightly-leeward (windwardness < 0.1) AND it still SEES the
	//    oncoming air (the airstream map below). The trail grows from the
	//    silhouette rim - Firefly's "creating a rim from which the plasma trail
	//    is created". Each fin SPANS A REAL MESH EDGE, which is what fuses
	//    neighbours into a sheet (the verified fix for G2's root cause).
	//  - Everything is vessel-frame off the flow axis (G6/G7), rebuilt from
	//    scratch every frame (G10).
	//  - The WRAP layer re-emits each fin longer, offset downstream, folded
	//    INWARD toward the wake axis, in the cool contrast colour - Firefly's
	//    "double-layered plasma trail".
	//  - The BOWL is the windward shell re-emitted TRANSLATED UPSTREAM - the
	//    detached bow shock is the ship's own nose shape standing off ahead.
	//  - The SKIRT (round 2.1, and the answer to "do the streaks start at the
	//    bowl?"): in Firefly the main trails root ON THE HULL, verified in their
	//    source - but their bowshock pass ALSO extrudes low-alpha fins from the
	//    shock's attachment rim, outward and downstream, in the shockwave colour.
	//    That skirt is the connective tissue that makes the shock blend into the
	//    wake instead of floating. Deferred in round 2.0; its absence was
	//    user-visible immediately ("the bowl seems wrong"), so here it is.
	//  - The REGIME RAMP: every colour lerps from WHITE (mach vapour) into the
	//    plasma palette as heat develops; deep-entry white re-earns itself through
	//    fp16 BLOOM where layers stack (patch i), which is where Firefly gets it.
	//
	// ROUND 2.1 LAWS (all three bought with the first screenshot round):
	//  - EVERY FIN IS FEATHERED ACROSS ITS WIDTH (3 columns: edge-0, centre-full,
	//    edge-0). Round 2.0 gave both lateral edges full alpha, and hundreds of
	//    fins at ribbon-scale alpha printed their outlines as hard triangle edges
	//    - the old ribbons ALWAYS carried a cross-section feather, and Firefly
	//    only survives without one because its per-fin alpha is ~0.004 under
	//    per-pixel noise. Fewer, softer fins over more triangles.
	//  - THE END-ON FADE IS BACK (invariant 15, the streams' own law). Looking
	//    down the flow axis a fin's projected length collapses; without the fade
	//    they stacked into the opaque angular star of the user's top-down shot.
	//  - THE BOWL FEATHERS AT ITS RIM like the shell always has: any-corner gate
	//    + per-vertex windwardness feather. The all-corner gate drew the subset
	//    boundary as a hard cutoff line - the shell's oldest lesson, re-learned.
	{
		const float shB = clampf(g_fx.plasShockBright, 0.0f, 3.0f);
		if (e.shellBuilt && e.nShellT > 0) {
			const double cosTh  = flowLocal.z < -1.0 ? -1.0 : (flowLocal.z > 1.0 ? 1.0 : flowLocal.z);
			const float  aoaDeg = (float)(acos(cosTh) * 57.29577951);
			// White -> colour as the entry develops (Firefly's FxState; our heat).
			const float regime  = clampf(heat / 0.30f, 0.0f, 1.0f);
			const float aoaGate = clampf(powf(clampf(aoaDeg / 20.0f, 0.0f, 1.0f), 4.0f)
			                             + regime * 0.7f + 0.30f, 0.0f, 1.0f);

			// Windwardness per shell vertex - own pass, NOT the shell block's copy
			// (that one is gated on the Shock bright knob and may not have run).
			for (int k = 0; k < e.nShellV; k++) {
				shellWd[k] = e.shellNrm[k * 3]     * (float)flowLocal.x
				           + e.shellNrm[k * 3 + 1] * (float)flowLocal.y
				           + e.shellNrm[k * 3 + 2] * (float)flowLocal.z;
			}

			// --- THE AIRSTREAM MAP - invariant 16's rasterizer aimed along the FLOW.
			// Firefly's single occlusion primitive is a 512^2 ortho depth camera
			// upstream along the velocity ("does this point see the oncoming air?").
			// Same idea, built with the rasterizer we already own: the shell's own
			// triangles into a max-depth-along-flow grid over a plane PERPENDICULAR
			// to the flow. Vessel frame, camera-free. It answers fin spawning and
			// the mid-fin kill; the camera-space map above keeps answering the
			// shell's see-through-the-hull question.
			static float s_flowZ[HZ_N * HZ_N];
			VECTOR3 fu = (fabs(flowLocal.y) < 0.9) ? _V(0, 1, 0) : _V(1, 0, 0);
			fu = unit(crossp(flowLocal, fu));
			const VECTOR3 fv = crossp(flowLocal, fu);
			const float fR  = (float)size * 0.60f;             // grid half-extent
			const float fCW = (2.0f * fR) / (float)(HZ_N - 2); // one margin cell all round
			const float fIW = 1.0f / fCW;
			for (int c = 0; c < HZ_N * HZ_N; c++) s_flowZ[c] = -1e30f;
			{
				float su[SHELL_MAX_VTX], sv[SHELL_MAX_VTX], sdd[SHELL_MAX_VTX];
				for (int k = 0; k < e.nShellV; k++) {
					const VECTOR3 p = _V(e.shellPos[k * 3], e.shellPos[k * 3 + 1], e.shellPos[k * 3 + 2]);
					su[k]  = (float)dotp(p, fu);
					sv[k]  = (float)dotp(p, fv);
					sdd[k] = (float)dotp(p, flowLocal);        // + = upstream
				}
				for (int ti = 0; ti < e.nShellT; ti++) {
					const int a2 = e.shellIdx[ti * 3], b2 = e.shellIdx[ti * 3 + 1], c3 = e.shellIdx[ti * 3 + 2];
					const float ax = su[a2], ay = sv[a2], bx = su[b2], by = sv[b2], cx3 = su[c3], cy3 = sv[c3];
					const float ar = (bx - ax) * (cy3 - ay) - (by - ay) * (cx3 - ax);
					if (fabsf(ar) < 1e-6f) continue;
					const float inv = 1.0f / ar;
					float mnx = ax < bx ? ax : bx; if (cx3 < mnx) mnx = cx3;
					float mxx = ax > bx ? ax : bx; if (cx3 > mxx) mxx = cx3;
					float mny = ay < by ? ay : by; if (cy3 < mny) mny = cy3;
					float mxy = ay > by ? ay : by; if (cy3 > mxy) mxy = cy3;
					int i0 = (int)((mnx + fR) * fIW), i1 = (int)((mxx + fR) * fIW);
					int j0 = (int)((mny + fR) * fIW), j1 = (int)((mxy + fR) * fIW);
					if (i0 < 0) i0 = 0; if (i1 > HZ_N - 1) i1 = HZ_N - 1;
					if (j0 < 0) j0 = 0; if (j1 > HZ_N - 1) j1 = HZ_N - 1;
					for (int j = j0; j <= j1; j++) {
						const float py = -fR + ((float)j + 0.5f) * fCW;
						for (int i2 = i0; i2 <= i1; i2++) {
							const float px = -fR + ((float)i2 + 0.5f) * fCW;
							const float wC = ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) * inv;
							const float wA = ((cx3 - bx) * (py - by) - (cy3 - by) * (px - bx)) * inv;
							const float wB = 1.0f - wA - wC;
							if (wA < 0.0f || wB < 0.0f || wC < 0.0f) continue;
							const float dc = sdd[a2] * wA + sdd[b2] * wB + sdd[c3] * wC;
							if (dc > s_flowZ[j * HZ_N + i2]) s_flowZ[j * HZ_N + i2] = dc;
						}
					}
				}
			}
			// "Does this point see the air?" 5-tap fraction (their 5x5 PCF's job).
			const float fEps = (float)size * 0.015f + 0.10f;
			auto airOcc = [&](const VECTOR3& p) -> float {
				const float pu = (float)dotp(p, fu), pv = (float)dotp(p, fv);
				const float pd = (float)dotp(p, flowLocal);
				const int i2 = (int)((pu + fR) * fIW), j = (int)((pv + fR) * fIW);
				if (i2 < 1 || j < 1 || i2 >= HZ_N - 1 || j >= HZ_N - 1) return 1.0f;
				float occ = 0.0f;
				const int di[5] = { 0, -1, 1, 0, 0 }, dj[5] = { 0, 0, 0, -1, 1 };
				for (int s = 0; s < 5; s++) {
					const float z = s_flowZ[(j + dj[s]) * HZ_N + i2 + di[s]];
					occ += (z > pd + fEps) ? 0.0f : 1.0f;
				}
				return occ * 0.2f;
			};

			// The rim profiles are shared consumers now (2026-08-08): the shock
			// envelope lofts from them AND the contour fins below spawn from them,
			// so they are computed once here, outside the envelope's own gate.
			// 1. The two angular profiles, from the flow map's occupied cells.
			const int NA = 48;
			float rProf[NA], dProf[NA];
			float sEnvCU = 0.0f, sEnvCV = 0.0f, sEnvDN = 0.0f;
			{
				float rMax[NA]; int have[NA];
				for (int b3 = 0; b3 < NA; b3++) { rMax[b3] = -1.0f; dProf[b3] = -1e30f; have[b3] = 0; }
				double dNoseA = -1e30, cu = 0.0, cv = 0.0; double cw = 0.0;
				for (int j = 0; j < HZ_N; j++) {
					for (int i2 = 0; i2 < HZ_N; i2++) {
						const float d = s_flowZ[j * HZ_N + i2];
						if (d < -1e29f) continue;
						const float pu = -fR + ((float)i2 + 0.5f) * fCW;
						const float pv = -fR + ((float)j + 0.5f) * fCW;
						const float r  = sqrtf(pu * pu + pv * pv);
						int b3 = (int)((atan2f(pv, pu) + 3.1415927f) / 6.2831853f * NA);
						if (b3 < 0) b3 = 0; else if (b3 >= NA) b3 = NA - 1;
						if (r > rMax[b3]) { rMax[b3] = r; dProf[b3] = d; have[b3] = 1; }
						if (d > dNoseA) dNoseA = d;
					}
				}
				// stagnation centroid: where the windwardmost 20% of the depth
				// range actually sits - the cap apexes over the NOSE, not over
				// the vessel origin, which matters at any real AoA
				double dLo = dNoseA - (double)size * 0.10;
				for (int j = 0; j < HZ_N; j++) {
					for (int i2 = 0; i2 < HZ_N; i2++) {
						const float d = s_flowZ[j * HZ_N + i2];
						if (d < (float)dLo) continue;
						const float pu = -fR + ((float)i2 + 0.5f) * fCW;
						const float pv = -fR + ((float)j + 0.5f) * fCW;
						const double w = (double)(d - (float)dLo);
						cu += (double)pu * w; cv += (double)pv * w; cw += w;
					}
				}
				if (cw > 1e-9) { cu /= cw; cv /= cw; }
				sEnvCU = (float)cu; sEnvCV = (float)cv; sEnvDN = (float)dNoseA;
				// circular fill for any empty bin (possible only on degenerate
				// silhouettes), then a circular low-pass - THE smoothing step:
				// 1D, wrap-around, resolution-independent
				for (int b3 = 0; b3 < NA; b3++) {
					if (have[b3]) { rProf[b3] = rMax[b3]; continue; }
					int p = b3, n2 = b3;
					for (int s = 1; s < NA; s++) { p = (b3 - s + NA) % NA; if (have[p]) break; }
					for (int s = 1; s < NA; s++) { n2 = (b3 + s) % NA; if (have[n2]) break; }
					rProf[b3] = 0.5f * (rMax[p] + rMax[n2]);
					dProf[b3] = 0.5f * (dProf[p] + dProf[n2]);
				}
				float tmpR[NA], tmpD[NA];
				for (int pass = 0; pass < 3; pass++) {
					for (int b3 = 0; b3 < NA; b3++) {
						const int bp = (b3 + NA - 1) % NA, bn = (b3 + 1) % NA;
						tmpR[b3] = 0.25f * rProf[bp] + 0.50f * rProf[b3] + 0.25f * rProf[bn];
						tmpD[b3] = 0.25f * dProf[bp] + 0.50f * dProf[b3] + 0.25f * dProf[bn];
					}
					memcpy(rProf, tmpR, sizeof(tmpR));
					memcpy(dProf, tmpD, sizeof(tmpD));
				}
			}
			// LEADING-NESS per bin (round 2.3c, from the second high-AoA report:
			// "the bowl seems the same as before"). The cone loft had fixed the
			// COVERAGE, but the alpha still peaked uniformly around the whole rim
			// and was dimmest at the apex - inverted physics at high AoA, where
			// the compression is strongest at the stagnation zone and the LEADING
			// rim and weakest on the trailing rim. (At low AoA the rim IS the
			// leading circle, which is why that case always looked right.) Each
			// bin scores how far UPSTREAM its rim sits; brightness follows it.
			float lead[NA];
			{
				float dMn = 1e30f, dMx = -1e30f;
				for (int b3 = 0; b3 < NA; b3++) {
					if (dProf[b3] < dMn) dMn = dProf[b3];
					if (dProf[b3] > dMx) dMx = dProf[b3];
				}
				const float dRg = (dMx - dMn > 1e-3f) ? dMx - dMn : 1.0f;
				for (int b3 = 0; b3 < NA; b3++)
					lead[b3] = powf(clampf((dProf[b3] - dMn) / dRg, 0.0f, 1.0f), 1.2f);
			}



			// --- THE FEATHERED FIN EMITTER (round 2.1). One fin = 3 rows (base /
			// mid / tip) x 3 columns (edge-ZERO / centre-FULL / edge-ZERO) = 8
			// triangles. The cross feather is what dissolves the fin outlines; the
			// end-on fade (old streams law, invariant 15) is what stops fins seen
			// down the flow axis stacking into an opaque star. All per-vertex 3D
			// projected with per-vertex clip depth (patch g).
			struct FinRow { VECTOR3 a, b; double w; int r, g, bb; float alpha; };
			auto emitFin3 = [&](const FinRow rw[3], const VECTOR3& sideV, double endonLen) {
				float  X[3][3], Y[3][3]; double Z[3][3]; float D[3][3];
				for (int r = 0; r < 3; r++) {
					const VECTOR3 pos[3] = { rw[r].a - sideV * rw[r].w,
					                         (rw[r].a + rw[r].b) * 0.5,
					                         rw[r].b + sideV * rw[r].w };
					for (int q = 0; q < 3; q++) {
						const VECTOR3 pg = Cg + mul(Rv, pos[q]);
						if (!ProjPx(cc, pg, viewW, viewH, X[r][q], Y[r][q], Z[r][q])) return;
						D[r][q] = (float)length(pg - cc.pos);
					}
				}
				const float ovL = 1.5f * (float)viewW;
				if (fabsf(X[0][1] - X[2][1]) > ovL || fabsf(Y[0][1] - Y[2][1]) > ovL) return;
				// End-on fade: projected centre-line length vs the side-on length
				// this fin would show at its mid depth.
				float fade = 1.0f;
				if (endonLen > 0.0) {
					const float pl = sqrtf((X[2][1] - X[0][1]) * (X[2][1] - X[0][1])
					                     + (Y[2][1] - Y[0][1]) * (Y[2][1] - Y[0][1]));
					const float el = pxAt(Z[1][1], endonLen * 0.8);
					fade = clampf(pl / (el > 1.0f ? el : 1.0f), 0.0f, 1.0f);
					if (fade < 0.02f) return;
				}
				DWORD cOut[3], cMid[3];
				for (int r = 0; r < 3; r++) {
					cOut[r] = PCol(rw[r].r, rw[r].g, rw[r].bb, 0);
					cMid[r] = PCol(rw[r].r, rw[r].g, rw[r].bb, (int)(rw[r].alpha * fade));
				}
				for (int r = 0; r < 2; r++) {
					emit3z(X[r][0], Y[r][0], D[r][0], cOut[r],   X[r][1],   Y[r][1],   D[r][1],   cMid[r],   X[r+1][0], Y[r+1][0], D[r+1][0], cOut[r+1]);
					emit3z(X[r][1], Y[r][1], D[r][1], cMid[r],   X[r+1][1], Y[r+1][1], D[r+1][1], cMid[r+1], X[r+1][0], Y[r+1][0], D[r+1][0], cOut[r+1]);
					emit3z(X[r][1], Y[r][1], D[r][1], cMid[r],   X[r][2],   Y[r][2],   D[r][2],   cOut[r],   X[r+1][1], Y[r+1][1], D[r+1][1], cMid[r+1]);
					emit3z(X[r][2], Y[r][2], D[r][2], cOut[r],   X[r+1][2], Y[r+1][2], D[r+1][2], cOut[r+1], X[r+1][1], Y[r+1][1], D[r+1][1], cMid[r+1]);
				}
			};

			// Fin colour stations, hot -> cool, all through PCol so Tint/Fringe/
			// Saturation keep working. Violet tip = the reference's universal cool
			// fringe (b > g, follows the Fringe pick per invariant 15b); the regime
			// lerp whitens everything at mach-vapour heat.
			auto finCol = [&](int st, int& r, int& g, int& b) {
				if      (st == 0) { r = 255; g = 190; b = 205; }   // root: white-pink
				else if (st == 1) { r = 255; g =  92; b =  34; }   // body: deep orange
				else              { r = 150; g =  70; b = 215; }   // tip:  violet
				r = 255 - (int)((255 - r) * regime);
				g = 255 - (int)((255 - g) * regime);
				b = 255 - (int)((255 - b) * regime);
			};
			auto wrapCol = [&](int st, int& r, int& g, int& b) {
				if      (st == 0) { r = 105; g = 120; b = 255; }
				else if (st == 1) { r =  90; g =  95; b = 240; }
				else              { r = 160; g =  80; b = 230; }
				r = 255 - (int)((255 - r) * regime);
				g = 255 - (int)((255 - g) * regime);
				b = 255 - (int)((255 - b) * regime);
			};

			const float knobLen = clampf(g_fx.plasStreakLen, 0.0f, 20.0f);
			const float knobWid = clampf(g_fx.plasStreakWid, 0.0f, 6.0f) * (1.0f / 2.0f);
			const float knobWan = clampf(g_fx.plasWander, 0.0f, 3.0f);
			// Default knob (3.0) at full heat = Firefly's reference proportion
			// (~2.6 hull radii before per-fin noise).
			const double Lbase  = size * 0.87 * (double)knobLen * (double)heat;
			const float nzAmp   = 0.45f + 0.55f * clampf(knobWan / 1.5f, 0.0f, 1.0f);

			const float sparkFrac = clampf(g_fx.plasSpark, 0.0f, 6.0f) * 0.06f;
			const float sLife     = clampf(g_fx.plasSparkLife, 0.1f, 3.0f);

			const float CEIL = 255.0f;    // soft alpha ceiling, unconditional
			                              // (invariant 15: soft, never hard)

			int finBudget = 460;          // 460 x 8 x 2 layers = 7360 tris ceiling -
			                              // the round-2.3 shock envelope is ~1k tris,
			                              // far under the taps-bowl it replaced, so the
			                              // fins get their round-2.1 budget back

			// --- FAIR ANGULAR SPREAD (2026-08-08, user: "the streaks seem mainly
			// focused on the front and aft ... spread them around the vessel more
			// evenly", worst on Atlantis). Two causes, both structural: the rim band
			// is DENSER in triangles wherever curvature is high (nose, tail), and the
			// budget used to be spent in MESH INDEX ORDER, so on a big shell it ran
			// out before the walk ever reached whole sectors of the rim - fins landed
			// wherever the mesh author put the first triangles. Now every qualifying
			// triangle is COLLECTED first, binned by its root's angle AROUND THE FLOW
			// AXIS (the same camera-free frame as all selection since G6), and the
			// budget is spent ROUND-ROBIN across the bins: every sector of the rim
			// gets fins before any sector gets seconds. Length variety is untouched -
			// it stays the per-corner noise.
			struct FinCand { WORD ti; BYTE ki, kj; };
			const int FBIN = 24, FPER = 64;
			static FinCand s_fc[FBIN][FPER];
			int fcN[FBIN];
			for (int b3 = 0; b3 < FBIN; b3++) fcN[b3] = 0;

			for (int ti = 0; ti < e.nShellT; ti++) {
				const int ia = e.shellIdx[ti * 3], ib = e.shellIdx[ti * 3 + 1], ic = e.shellIdx[ti * 3 + 2];
				int ki = -1;
				const int corn[3] = { ia, ib, ic };
				float cOcc[3];
				for (int s = 0; s < 3; s++) {
					const VECTOR3 p = _V(e.shellPos[corn[s] * 3], e.shellPos[corn[s] * 3 + 1], e.shellPos[corn[s] * 3 + 2]);
					cOcc[s] = airOcc(p);
				}
				// GRADED windward acceptance (2026-08-08, from the Atlantis report:
				// fins crowded fore/aft because the welded shell smears wing-leading-
				// edge normals windward past the old hard 0.10 cutoff, so whole flat
				// spans had zero candidates and no fair budget could help them. This
				// is Lindner's own law finally implemented properly: her extrusion
				// accepts windward faces too and scales LENGTH by (1-dot)^3, so only
				// true rim roots throw the long streamers. Acceptance widens to 0.45;
				// the grading lives in emitFin. Roots prefer the most TANGENT corner.
				float kiWd = 1e9f;
				for (int s = 0; s < 3; s++) {
					if (shellWd[corn[s]] < 0.45f && cOcc[s] > 0.85f && shellWd[corn[s]] < kiWd) {
						ki = s; kiWd = shellWd[corn[s]];
					}
				}
				if (ki < 0) continue;
				int kj = (ki + 1) % 3;
				const int kk = (ki + 2) % 3;
				if (cOcc[kk] > cOcc[kj] || shellWd[corn[kk]] < shellWd[corn[kj]]) kj = kk;

				const VECTOR3 pRoot = _V(e.shellPos[corn[ki] * 3], e.shellPos[corn[ki] * 3 + 1], e.shellPos[corn[ki] * 3 + 2]);
				const float th = atan2f((float)dotp(pRoot, fv), (float)dotp(pRoot, fu));
				int b3 = (int)((th + 3.1415927f) / 6.2831853f * FBIN);
				if (b3 < 0) b3 = 0; else if (b3 >= FBIN) b3 = FBIN - 1;
				if (fcN[b3] < FPER) {
					s_fc[b3][fcN[b3]].ti = (WORD)ti;
					s_fc[b3][fcN[b3]].ki = (BYTE)ki;
					s_fc[b3][fcN[b3]].kj = (BYTE)kj;
					fcN[b3]++;
				}
			}

			auto emitFin = [&](int ti, int kiS, int kjS) {
				const int corn[3] = { e.shellIdx[ti * 3], e.shellIdx[ti * 3 + 1], e.shellIdx[ti * 3 + 2] };
				const int A2 = corn[kiS], B2 = corn[kjS];

				const VECTOR3 pA = _V(e.shellPos[A2 * 3], e.shellPos[A2 * 3 + 1], e.shellPos[A2 * 3 + 2]);
				const VECTOR3 pB = _V(e.shellPos[B2 * 3], e.shellPos[B2 * 3 + 1], e.shellPos[B2 * 3 + 2]);
				const VECTOR3 nA = _V(e.shellNrm[A2 * 3], e.shellNrm[A2 * 3 + 1], e.shellNrm[A2 * 3 + 2]);
				const VECTOR3 nB = _V(e.shellNrm[B2 * 3], e.shellNrm[B2 * 3 + 1], e.shellNrm[B2 * 3 + 2]);
				const float edgeLen = (float)length(pB - pA);
				if (edgeLen < 1e-4f) return;

				// Tessellation-density compensation (their edgeMul), gentle because
				// the weld grid is near-uniform.
				const float em = clampf(edgeLen / ((float)size * 0.045f), 0.30f, 2.0f);

				const float hA = hashf((float)A2 * 17.31f + 0.7f);
				const float hB = hashf((float)B2 * 17.31f + 0.7f);
				const float nzA = 0.5f + 0.5f * sinf(t * (0.9f + hA * 0.7f) + hA * 6.2832f);
				const float nzB = 0.5f + 0.5f * sinf(t * (0.9f + hB * 0.7f) + hB * 6.2832f);
				// Lindner's length law: (1 - windwardness)^1.5 - a true rim root
				// (wd <= 0) keeps its full streamer, a windward-ish root makes a
				// SHORT sheet. Coverage everywhere, length graded by physics.
				const float lenGrade = powf(clampf(1.0f - shellWd[A2], 0.0f, 1.0f), 1.5f);
				const double lenA = Lbase * (double)((0.30f + 1.70f * nzA * nzA * nzAmp) * lenGrade);
				const double lenB = Lbase * (double)((0.30f + 1.70f * nzB * nzB * nzAmp) * lenGrade);
				if (lenA + lenB < size * 0.02) return;

				// Mid-fin kill (their second Shadow() probe).
				{
					const VECTOR3 probe = (pA + pB) * 0.5 - flowLocal * (0.1 * (lenA + lenB) * 0.5);
					if (airOcc(probe) < 0.5f) return;
				}

				VECTOR3 sideV = crossp(flowLocal, nA);
				const double sl = length(sideV);
				if (sl < 0.05) sideV = fu; else sideV = sideV / sl;
				const VECTOR3 down = flowLocal * (-1.0);
				const double spread = size * 0.38 * (double)heat;
				const double lift   = size * 0.012;

				// Facing fade (limb law, G5-safe), folded into the fin alpha.
				float fr = VC_VIS;
				if (!vcGate) {
					const VECTOR3 gA = Cg + mul(Rv, pA);
					const VECTOR3 ngA = mul(Rv, nA);
					const float fc2 = (float)dotp(ngA, unit(cc.pos - gA));
					fr = clampf(0.40f + 0.75f * (1.0f - fabsf(fc2)) * (1.0f - fabsf(fc2)) + 0.20f * nzA, 0.0f, 1.0f);
				}

				float aFin = A * 0.15f * em * aoaGate * fr * (vcGate ? VC_GAIN : 1.0f)
				           * (0.55f + 0.45f * lenGrade)   // windward-rooted sheets run fainter
				           * (0.85f + 0.15f * sinf(t * 2.6f + (float)ti * 1.9f));
				aFin = CEIL * (1.0f - expf(-aFin / CEIL));
				if (aFin < 2.0f) return;

				for (int layer = 0; layer < 2; layer++) {
					if (layer == 1 && regime < 0.6f) break;
					double lA = lenA, lB = lenB, sprd = spread, dOfs = 0.0;
					float  aL = aFin;
					if (layer == 1) {
						const float wf = clampf((regime - 0.6f) / 0.4f, 0.0f, 1.0f);
						lA *= 1.45; lB *= 1.45;
						sprd = -(double)(size * 0.22);      // folds toward the wake axis
						dOfs = size * 0.05;
						aL  *= 0.42f * wf;
					}
					const double wp0 = (double)(edgeLen * 0.30f * knobWid);
					const double wp1 = (double)(edgeLen * 1.00f * knobWid);
					const double wp2 = (double)(edgeLen * 2.00f * knobWid) * (0.6 + 0.8 * (double)heat);

					FinRow rw[3];
					int r0, g0, b0, r1, g1, b1, r2, g2, b2;
					if (layer == 0) { finCol(0, r0, g0, b0); finCol(1, r1, g1, b1); finCol(2, r2, g2, b2); }
					else            { wrapCol(0, r0, g0, b0); wrapCol(1, r1, g1, b1); wrapCol(2, r2, g2, b2); }
					rw[0] = { pA + nA * lift + down * dOfs,
					          pB + nB * lift + down * dOfs,
					          wp0, r0, g0, b0, aL };
					rw[1] = { pA + nA * lift + down * (lA * 0.2 + dOfs) + nA * (sprd * 0.25),
					          pB + nB * lift + down * (lB * 0.2 + dOfs) + nB * (sprd * 0.25),
					          wp1, r1, g1, b1, aL * 0.55f };
					rw[2] = { pA + nA * lift + down * (lA + dOfs) + nA * sprd,
					          pB + nB * lift + down * (lB + dOfs) + nB * sprd,
					          wp2, r2, g2, b2, 0.0f };
					emitFin3(rw, sideV, (lenA + lenB) * 0.5);
				}
				finBudget--;

				// SPARK on carrier fins (stream-era look, knobs intact).
				if (hashf((float)ti * 3.31f + 0.4f) < sparkFrac) {
					const float hp   = hashf((float)ti * 9.13f + 1.7f);
					float prog = t * (0.8f + 0.4f * hp) / sLife + hp * 7.0f;
					prog -= floorf(prog);
					const VECTOR3 c0v = (pA + pB) * 0.5 + (nA + nB) * (lift * 0.5);
					const VECTOR3 c1v = c0v + down * ((lenA + lenB) * 0.5) + (nA + nB) * (spread * 0.5);
					const VECTOR3 sp  = c0v + (c1v - c0v) * (double)prog
					                  + sideV * ((double)(hashf(hp * 31.7f) - 0.5f) * (double)edgeLen * 1.5 * (double)prog);
					const VECTOR3 pg  = Cg + mul(Rv, sp);
					float sx2, sy2; double sz2;
					if (ProjPx(cc, pg, viewW, viewH, sx2, sy2, sz2)) {
						emitZ = (float)length(pg - cc.pos);
						const float sr2 = (1.3f + 2.2f * (1.0f - prog))
						                * clampf(g_fx.plasSparkSize, 0.0f, 4.0f);
						const float aSp = clampf(A * 0.65f, 0.0f, 235.0f)
						                * (0.55f + 0.45f * (1.0f - prog))
						                * (0.7f + 0.3f * sinf(t * 9.0f + hp * 40.0f));
						if (aSp >= 3.0f && sr2 > 0.3f) {
							const DWORD sC = PCol(255, 215, 165, (int)aSp);
							const DWORD sE = PCol(255, 120, 45, 0);
							const int NFS = 4;
							for (int q = 0; q < NFS; q++) {
								const float a0r = (float)q / NFS * 6.2831853f;
								const float a1r = (float)(q + 1) / NFS * 6.2831853f;
								emit3(sx2, sy2, sC,
								      sx2 + cosf(a0r) * sr2, sy2 + sinf(a0r) * sr2, sE,
								      sx2 + cosf(a1r) * sr2, sy2 + sinf(a1r) * sr2, sE);
							}
						}
					}
				}
			};

			// The round-robin spend: sweep s takes the s-th candidate of every
			// non-empty bin before sweep s+1 starts anywhere.
			for (int s = 0; s < FPER && finBudget > 0; s++)
				for (int b3 = 0; b3 < FBIN && finBudget > 0; b3++)
					if (s < fcN[b3])
						emitFin(s_fc[b3][s].ti, s_fc[b3][s].ki, s_fc[b3][s].kj);

			// --- CONTOUR FINS (2026-08-08, the Atlantis spread, second round). The
			// shell-triangle spawner can never be even on a hull whose WELD destroyed
			// its tangent band - the Atlantis's wing leading edge folds under the weld
			// cell, so no graded gate can conjure mid-span roots. The RIM CONTOUR is
			// the even distribution, by construction: the same smoothed silhouette
			// profile the shock envelope lofts from, one full-length streamer fin per
			// angular bin, fused edge-to-edge along the ring (G2's connectivity law -
			// adjacent fins share their bin boundary). The shell-triangle fins above
			// keep covering INTERIOR silhouettes (tail fin, OMS pods) the outer
			// contour cannot see; sparks stay with them.
			for (int b3 = 0; b3 < 48 && finBudget > 0; b3++) {
				const int bn = (b3 + 1) % 48;
				const float thA = ((float)b3 + 0.5f) / 48.0f * 6.2831853f - 3.1415927f;
				const float thB = ((float)bn + 0.5f) / 48.0f * 6.2831853f - 3.1415927f;
				const VECTOR3 radA = fu * (double)cosf(thA) + fv * (double)sinf(thA);
				const VECTOR3 radB = fu * (double)cosf(thB) + fv * (double)sinf(thB);
				const VECTOR3 pA = radA * (double)rProf[b3] + flowLocal * (double)dProf[b3];
				const VECTOR3 pB = radB * (double)rProf[bn] + flowLocal * (double)dProf[bn];
				const VECTOR3 nA = radA, nB = radB;      // rim normals ~ radial outward
				const float edgeLen = (float)length(pB - pA);
				if (edgeLen < 1e-3f) continue;
				const float em = clampf(edgeLen / ((float)size * 0.045f), 0.30f, 2.0f);
				const float hA = hashf((float)b3 * 23.7f + 0.9f);
				const float hB = hashf((float)bn * 23.7f + 0.9f);
				const float nzA = 0.5f + 0.5f * sinf(t * (0.9f + hA * 0.7f) + hA * 6.2832f);
				const float nzB = 0.5f + 0.5f * sinf(t * (0.9f + hB * 0.7f) + hB * 6.2832f);
				const double lenA = Lbase * (double)(0.30f + 1.70f * nzA * nzA * nzAmp);
				const double lenB = Lbase * (double)(0.30f + 1.70f * nzB * nzB * nzAmp);
				if (lenA + lenB < size * 0.02) continue;

				VECTOR3 sideV = crossp(flowLocal, nA);
				const double sl = length(sideV);
				if (sl < 0.05) continue; else sideV = sideV / sl;
				const VECTOR3 down = flowLocal * (-1.0);
				const double spread = size * 0.38 * (double)heat;
				const double lift   = size * 0.012;

				float fr = VC_VIS;
				if (!vcGate) {
					const VECTOR3 gA = Cg + mul(Rv, pA);
					const VECTOR3 ngA = mul(Rv, nA);
					const float fc2 = (float)dotp(ngA, unit(cc.pos - gA));
					fr = clampf(0.40f + 0.75f * (1.0f - fabsf(fc2)) * (1.0f - fabsf(fc2)) + 0.20f * nzA, 0.0f, 1.0f);
				}

				float aFin = A * 0.15f * em * aoaGate * fr * (vcGate ? VC_GAIN : 1.0f)
				           * (0.85f + 0.15f * sinf(t * 2.6f + (float)b3 * 2.3f));
				aFin = CEIL * (1.0f - expf(-aFin / CEIL));
				if (aFin < 2.0f) continue;

				for (int layer = 0; layer < 2; layer++) {
					if (layer == 1 && regime < 0.6f) break;
					double lA = lenA, lB = lenB, sprd = spread, dOfs = 0.0;
					float  aL = aFin;
					if (layer == 1) {
						const float wf = clampf((regime - 0.6f) / 0.4f, 0.0f, 1.0f);
						lA *= 1.45; lB *= 1.45;
						sprd = -(double)(size * 0.22);
						dOfs = size * 0.05;
						aL  *= 0.42f * wf;
					}
					const double wp0 = (double)(edgeLen * 0.30f * knobWid);
					const double wp1 = (double)(edgeLen * 1.00f * knobWid);
					const double wp2 = (double)(edgeLen * 2.00f * knobWid) * (0.6 + 0.8 * (double)heat);

					FinRow rw[3];
					int r0, g0, b0, r1, g1, b1, r2, g2, b2;
					if (layer == 0) { finCol(0, r0, g0, b0); finCol(1, r1, g1, b1); finCol(2, r2, g2, b2); }
					else            { wrapCol(0, r0, g0, b0); wrapCol(1, r1, g1, b1); wrapCol(2, r2, g2, b2); }
					rw[0] = { pA + nA * lift + down * dOfs,
					          pB + nB * lift + down * dOfs,
					          wp0, r0, g0, b0, aL };
					rw[1] = { pA + nA * lift + down * (lA * 0.2 + dOfs) + nA * (sprd * 0.25),
					          pB + nB * lift + down * (lB * 0.2 + dOfs) + nB * (sprd * 0.25),
					          wp1, r1, g1, b1, aL * 0.55f };
					rw[2] = { pA + nA * lift + down * (lA + dOfs) + nA * sprd,
					          pB + nB * lift + down * (lB + dOfs) + nB * sprd,
					          wp2, r2, g2, b2, 0.0f };
					emitFin3(rw, sideV, (lenA + lenB) * 0.5);
				}
				finBudget--;
			}

			// --- THE SHOCK ENVELOPE (round 2.3) - bowl + skirt REPLACED by ONE
			// lofted smooth surface. Two rounds of mesh-copy bowls read as "a too
			// crisp copy of the mesh" and then "3 copies of it": any surface that
			// inherits mesh TESSELLATION shows its facets and its silhouette at
			// close range, at any smoothing or tap budget. So the shock front no
			// longer comes from triangles at all. The airstream map (dense,
			// rasterized from the whole shell) is reduced to two 1D ANGULAR
			// PROFILES around the flow axis - rim radius r(theta) and rim
			// upstream-depth d(theta) - circularly smoothed, and the envelope is
			// LOFTED from them: a cap sagging from the stagnation apex to the rim,
			// flaring past it into the trailing skirt, one continuous surface.
			// A 1D-smoothed profile is glassy at ANY tessellation by construction:
			// the facet problem cannot exist here. This is the aurora's smooth-
			// ribbon discipline pointed at the shock, and it is also the physical
			// shape - a detached shock is one smooth sheet from apex to skirt.
			// (G4's graveyard verdict stands for what it judged: heightfields from
			// SPARSE UNSTRUCTURED samples produced shards via cell dropout and
			// normal-offset scatter. This is a dense full-mesh rasterization,
			// reduced to 1D, offset along the FLOW AXIS only - all three of G4's
			// verified causes are structurally absent.)
			if (shB > 0.01f && heat > 0.25f) {
				const float bowlIn = clampf((heat - 0.25f) / 0.35f, 0.0f, 1.0f);
				const float aBase  = A * 0.20f * bowlIn * clampf(shB, 0.0f, 3.0f)
				                   * (vcGate ? VC_GAIN : 1.0f);
				// The repurposed knob (2026-08-08): "Bowl dist" scales the envelope's
				// automatic standoff law. 0.10 = exactly the old automatic; the range
				// spans 0..3x it. NOTE for saved class cfgs: values saved under the
				// knob's OLD meaning (shell standoff, typically 0.01-0.02) read as a
				// tiny bowl here - re-tune once (start at 0.10) and SAVE.
				const double stand = size * (0.10 + 0.30 * (double)heat)
				                   * (double)(clampf(g_fx.plasShockDist, 0.0f, 0.30f) * 10.0f);
				// The sculpting knobs (user request 2026-08-08): scale the finished
				// loft in VESSEL axes around the origin - the frame a person can
				// reason about while shaping it by eye. 1/1/1 = untouched.
				const double bsx = (double)clampf(g_fx.plasBowlSX, 0.0f, 2.0f);
				const double bsy = (double)clampf(g_fx.plasBowlSY, 0.0f, 2.0f);
				const double bsz = (double)clampf(g_fx.plasBowlSZ, 0.0f, 2.0f);

				// 2. The loft. Rings of NA vertices from the apex (ring 0, a point)
				// through the cap to the rim (ring NR_CAP), then flaring past it
				// into the skirt. Per-vertex: position, approximate normal (axial
				// at the apex tilting radial-downstream through the skirt) for the
				// signed facing fade - seen from UPSTREAM the sheet glows, from
				// dead leeward it dims to a trace instead of painting the hull -
				// plus the travelling-wave boil and the regime whitening.
				const int NR_CAP = 6, NR_SK = 5, NR = NR_CAP + NR_SK;
				const double dNose = (double)sEnvDN;
				float  eX[2][NA + 1], eY[2][NA + 1]; float eD[2][NA + 1]; DWORD eC[2][NA + 1];
				bool   eOk[2][NA + 1];
				float apexX = 0, apexY = 0, apexDist = 0; DWORD apexC = 0; bool apexOk = false;
				{
					VECTOR3 Pap = fu * (double)sEnvCU + fv * (double)sEnvCV
					            + flowLocal * (dNose + stand);
					Pap.x *= bsx; Pap.y *= bsy; Pap.z *= bsz;
					const VECTOR3 pg = Cg + mul(Rv, Pap);
					double zz;
					apexOk = ProjPx(cc, pg, viewW, viewH, apexX, apexY, zz);
					if (apexOk) {
						apexDist = (float)length(pg - cc.pos);
						float sFace = 1.0f;
						if (!vcGate) {
							const VECTOR3 ng = mul(Rv, flowLocal);
							const float fc = (float)dotp(ng, unit(cc.pos - pg));
							sFace = clampf(0.08f + 0.92f * clampf(fc * 0.5f + 0.5f, 0.0f, 1.0f), 0.0f, 1.0f);
						}
						float aA = aBase * (0.55f + 0.75f * heat) * sFace;   // the STAGNATION
						                                   // glow - heat-driven since 2.3e:
						                                   // the gas cap's hottest point
						aA = 255.0f * (1.0f - expf(-aA / 255.0f));
						int r = 150 + (int)(85 * heat), g = 165 + (int)(75 * heat), b = 255;
						r = 255 - (int)((255 - r) * regime);
						g = 255 - (int)((255 - g) * regime);
						apexC = PCol(r, g, b, (int)aA);
					}
				}
				int prev = 0;
				for (int ir = 1; ir <= NR; ir++) {
					const int cur = ir & 1;
					const bool inCap = (ir <= NR_CAP);
					const float rho = inCap ? (float)ir / (float)NR_CAP : 1.0f;
					const float ext = inCap ? 0.0f : (float)(ir - NR_CAP) / (float)NR_SK;
					// ring intensity: soft at the apex, PEAK AT THE RIM (the
					// compression line), fading through the skirt
					// THE GAS CAP (round 2.3e, from the physics discussion): the sheet
					// alone read as an empty soap bubble - bright rim, hollow middle -
					// but the real shock LAYER is a compressed self-luminous volume,
					// thickest and hottest at the stagnation zone. The cap interior
					// now fills in and WHITENS with heat: a faint film early in the
					// entry, a near-opaque wall of light at peak (additive cannot
					// darken, but enough added light saturates and blooms - which is
					// how "opaque" is spelled on this substrate).
					const float stagA = powf(1.0f - rho, 1.6f) * (0.25f + 1.05f * heat);
					const float ringA = inCap ? (0.35f + 0.65f * powf(rho, 2.0f) + stagA)
					                          : powf(1.0f - ext, 1.6f) * 1.15f;
					const float band  = inCap
					    ? clampf(0.35f + 0.65f * rho + powf(1.0f - rho, 1.6f) * (0.30f + 0.70f * heat), 0.0f, 1.0f)
					    : 1.0f - 0.5f * ext;
					// normal tilt: axial (flow) at the apex -> 45deg at the rim ->
					// radial-downstream in the skirt
					const float nAx = inCap ? (1.0f - 0.5f * rho) : 0.5f - 0.40f * ext;
					for (int b3 = 0; b3 <= NA; b3++) {
						const int bb = b3 % NA;
						const float th = ((float)bb + 0.5f) / (float)NA * 6.2831853f - 3.1415927f;
						const float ct = cosf(th), st = sinf(th);
						const VECTOR3 rad = fu * (double)ct + fv * (double)st;
						// THE CONE LOFT (round 2.3b, from the high-AoA report: "the
						// bowl doesn't cover all the side exposed to the airstream -
						// the front needs work"). The first loft grew its rings as
						// circles around the VESSEL ORIGIN axis with only the apex
						// pulled over the nose, so at high AoA the sheet tented at
						// the nose and folded back over mid-body, leaving the
						// forward windward span bare (at low AoA origin axis ~ nose
						// axis, which is why that case looked right). Now every ring
						// interpolates from the APEX POSITION to the RIM CURVE
						// itself - a true generalized cone, apex-to-rim coverage on
						// every side at any attitude.
						// RIM OVERSHOOT (round 2.3d, the user's own call: "perhaps we
						// should scale the bowl"). The forward cap was never missing -
						// it was DROWNED: an additive sheet over the blown-out hull
						// glow adds to white, so the envelope only READS where it
						// clears the silhouette against dark sky. Aft it always did
						// (the keel is thin); forward it hugged too close. The rim now
						// overshoots the silhouette on every side so the sheet's edge
						// detaches visibly at any attitude.
						const float rimX = 1.15f + 0.10f * heat;   // 15-25% (user's call, 2.3e)
						const float rimU = ct * rProf[bb] * rimX, rimV = st * rProf[bb] * rimX;
						double pu, pv;
						if (inCap) {
							const double w = (double)powf(rho, 1.05f);
							pu = (double)sEnvCU * (1.0 - w) + (double)rimU * w;
							pv = (double)sEnvCV * (1.0 - w) + (double)rimV * w;
						} else {
							const double ov = 1.0 + (0.30 + 0.35 * (double)heat) * (double)ext;
							pu = (double)rimU * ov;
							pv = (double)rimV * ov;
						}
						// cap: apex height sags to the rim height (the classic
						// detached-shock profile); skirt: runs downstream, a little
						// further than 2.3 so the striations below have room to read
						const double mixv = (double)powf(rho, 1.25f);   // flatter sag (2.3c):
						                                   // at high AoA the windward face is
						                                   // near-planar and 1.7 bulged the
						                                   // mid-cap upstream of it
						const double dd  = inCap
						    ? (dNose + stand) * (1.0 - mixv) + ((double)dProf[bb] + stand * 0.75) * mixv
						    : (double)dProf[bb] + stand * 0.75
						      - (double)(ext * ext) * (double)size * (0.42 + 0.62 * (double)heat);
						// the boil: a travelling wave in theta and ring, REAL time
						const float wave = sinf(th * 3.0f + t * 1.2f + rho * 4.0f + ext * 3.0f)
						                 * sinf(th * 5.0f - t * 0.8f + 1.7f);
						VECTOR3 P = fu * pu + fv * pv
						          + flowLocal * (dd + (double)wave * stand * 0.08);
						P.x *= bsx; P.y *= bsy; P.z *= bsz;
						const VECTOR3 pg = Cg + mul(Rv, P);
						double zz;
						eOk[cur][b3] = ProjPx(cc, pg, viewW, viewH, eX[cur][b3], eY[cur][b3], zz);
						if (!eOk[cur][b3]) continue;
						eD[cur][b3] = (float)length(pg - cc.pos);
						float sFace = 1.0f;
						if (!vcGate) {
							const VECTOR3 nLoc = unit(flowLocal * (double)nAx + rad * (double)(1.0f - nAx));
							const VECTOR3 ng = mul(Rv, nLoc);
							const float fc = (float)dotp(ng, unit(cc.pos - pg));
							sFace = clampf(0.08f + 0.92f * clampf(fc * 0.6f + 0.5f, 0.0f, 1.0f), 0.0f, 1.0f);
						}
						// SKIRT STRIATIONS (round 2.3b, user: "make the bowl's streaks
						// more pronounced"). The flare breaks into soft radiating
						// streaks: an angular pattern that is SMOOTH at the rim and
						// grows toward the tail, drifting slowly in REAL time. Pure
						// alpha modulation on the smooth loft - the surface itself
						// stays continuous, so no facet or gap can ever open.
						float stria = 1.0f;
						if (!inCap) {
							const float sp = 0.5f + 0.5f * sinf(th * 14.0f + t * 0.6f
							               + 1.3f * sinf(th * 5.0f - t * 0.35f));
							stria = 1.0f - 0.60f * powf(ext, 0.7f) * (1.0f - sp);
						}
						// the lead weighting (2.3c): full at the apex ring, fading to
						// lead-scored at the rim - the compression glows over the nose
						// and leading edges and lets the trailing rim go quiet. The
						// skirt streaks inherit it wholesale: they pour off the
						// LEADING side, which is where the flow actually separates.
						const float leadW = inCap
						    ? (1.0f - rho) + rho * (0.30f + 0.70f * lead[bb])
						    : (0.30f + 0.70f * lead[bb]);
						float aV = aBase * ringA * leadW * sFace * stria * (0.76f + 0.24f * wave);
						aV = 255.0f * (1.0f - expf(-aV / 255.0f));
						int r = 95 + (int)(90 * band), g = 115 + (int)(80 * band), b = 255;
						r = 255 - (int)((255 - r) * regime);
						g = 255 - (int)((255 - g) * regime);
						eC[cur][b3] = PCol(r, g, b, (int)aV);
					}
					if (ir == 1) {
						if (apexOk) {
							for (int b3 = 0; b3 < NA; b3++) {
								if (!eOk[cur][b3] || !eOk[cur][b3 + 1]) continue;
								emit3z(apexX, apexY, apexDist, apexC,
								       eX[cur][b3], eY[cur][b3], eD[cur][b3], eC[cur][b3],
								       eX[cur][b3 + 1], eY[cur][b3 + 1], eD[cur][b3 + 1], eC[cur][b3 + 1]);
							}
						}
					} else {
						for (int b3 = 0; b3 < NA; b3++) {
							if (!eOk[prev][b3] || !eOk[prev][b3 + 1] || !eOk[cur][b3] || !eOk[cur][b3 + 1]) continue;
							emit3z(eX[prev][b3], eY[prev][b3], eD[prev][b3], eC[prev][b3],
							       eX[cur][b3],  eY[cur][b3],  eD[cur][b3],  eC[cur][b3],
							       eX[prev][b3 + 1], eY[prev][b3 + 1], eD[prev][b3 + 1], eC[prev][b3 + 1]);
							emit3z(eX[cur][b3], eY[cur][b3], eD[cur][b3], eC[cur][b3],
							       eX[cur][b3 + 1], eY[cur][b3 + 1], eD[cur][b3 + 1], eC[cur][b3 + 1],
							       eX[prev][b3 + 1], eY[prev][b3 + 1], eD[prev][b3 + 1], eC[prev][b3 + 1]);
						}
					}
					prev = cur;
				}
			}
		}
	}

	// (--- 4. WAKE BLOBS lived here - RETIRED in round 3: detached orbs are nothing
	// the reference shows. SpawnWakeBlobs and the WakeBlob machinery stay in the
	// codebase, uncalled, in case a debris-glow use returns.)

	// (--- 5. The TRAIL lived here. REMOVED 2026-08-02 with all its machinery -
	// the knot ring, the shed cadence, the despiking, the alpha-blended smoke
	// band. It was the only ORO element built on state that ACCUMULATED over
	// seconds, which is exactly why it stayed fragile: every other element is
	// rebuilt from scratch each frame so a bad sample disappears, while one bad
	// knot survived as a permanent spike in the ribbon. The user's call after
	// the cardiogram spiking: abandon it and let the STREAKS run long instead -
	// their length knob went 9 -> 20 in the same breath. See the graveyard.)
}
