// ==============================================================
// OroVapour.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - THE VAPOUR CONE: transonic condensation
// ----------------------------------------------------------------------------
// The Prandtl-Glauert singularity, as a thing you can photograph. Crossing Mach 1
// the flow accelerating over the hull expands; pressure and temperature fall with
// it; and if the air carries enough water the vapour CONDENSES into a shroud that
// hangs around and behind the vehicle for as long as the condition lasts.
//
// It is the one famous aerodynamic visual that nothing in Orbiter has, and - unlike
// an eclipse, an aurora or a polar storm - it happens on flights he is already
// making. Every ascent crosses the band and so does every descent.
//
// ============================================================================
// THREE DECISIONS, each of which shaped everything after it.
// ============================================================================
//
// 1. IT IS A CLOUD, SO IT DRAWS ALPHA-BLENDED. Everything else ORO draws is
//    emissive and additive - plasma, aurora, lightning, plume, god rays - because
//    everything else IS light. Condensed water is not: it scatters, and above all
//    it OCCLUDES. Additive light cannot darken anything, so an additive cone could
//    only ever have brightened the hull behind it, which is the opposite of what a
//    cloud does. This is the first live use of the recipe graveyard G11 left on the
//    shelf when the trail's smoke layer died with it, and it needs no client patch:
//    the pad's DEFAULT blend state, drawn BEFORE the additive layers.
//
// 2. THE SHAPE IS THE MACH ANGLE, WHICH IS WHY IT READS AS SPEED. The cone's
//    half-angle is mu = asin(1/M) - 90 degrees at M = 1 (a flat collar standing
//    perpendicular to the flight path), narrowing as the vehicle outruns its own
//    pressure waves. So the OUTER RADIUS is a knob (it is a fact about the hull)
//    but the LENGTH is not: it falls out of the radius and the Mach angle together.
//    Give the length a slider and the cone becomes a decal that happens to be
//    there; derive it, and the shroud visibly stretches back as the ship
//    accelerates. That is the whole effect.
//
// 3. IT IS A SURFACE OF REVOLUTION, SO IT IS SMOOTH BY CONSTRUCTION. Invariant
//    20(e) was bought with three dead mesh-copy bowls: anything built FROM MESH
//    TRIANGLES shows its tessellation at close range, at any smoothing budget. This
//    surface is lofted from an analytic profile around an analytic axis and touches
//    the vessel's mesh nowhere at all, so the facet problem cannot exist here - the
//    same reason the shock envelope's two 1D angular profiles work.
//
// ----------------------------------------------------------------------------
// EXTERNAL ONLY (invariant 10). The cone forms around and behind the hull, so from
// the pilot's seat most of it is behind your head; and a screen-space sheet in an
// internal view would paint the canopy frame exactly the way the shimmer would.
//
// VESSEL-ANCHORED, so invariant 21(b)'s render-epoch trap does not apply here: a
// camera tracking the vessel cancels the epoch, which is why none of the attached
// plasma needed patch (k) either. Built on the MAIN thread with the pre-step camera
// (invariant 1); the render path only pushes and draws.
//
// ⚠️ NO DEPTH SORT, AND THAT IS FINE HERE. The near and far halves of the shell
// blend in emission order rather than depth order. For an opaque surface that would
// be a bug; for a translucent shell it is very nearly right, because you genuinely
// do see through both halves and the limb term below already accounts for the path
// length through them. Sorting a surface of revolution per frame would cost more
// than the error is worth.
// ============================================================================

#include "OroModule.h"
#include "OroState.h"
#include <math.h>

namespace {

	// --- the condensation window ---------------------------------------------
	// Real cones are a low-altitude phenomenon and that is not a budget decision:
	// the effect needs WATER in the air, and the water is in the troposphere. Every
	// photograph of one - and there are thousands, because it is the shot every
	// airshow photographer wants - is taken low and usually over the sea.
	const double VAP_RHO_FULL = 0.60;    // [kg/m^3] full strength at or above (~6 km)
	const double VAP_RHO_MIN  = 0.10;    // [kg/m^3] nothing at or below   (~18 km)

	// The Mach band. Condensation starts a little before M = 1 (the flow over the wing
	// goes supersonic well before the vehicle does - that is what "transonic" means) and
	// dies away once the shock system attaches to the airframe.
	// ⚠️ NARROWED HARD on 2026-08-11, his first look: *"the transition should be shorter,
	// the vapor cone is on for too long."* The first build spanned M 0.82 -> 1.60, which
	// is 0.78 Mach of cone and reads as a permanent fixture rather than as the fleeting
	// thing it is - a real one is a few seconds of an airshow pass, which is exactly why
	// photographers prize the shot. 0.90 -> 1.22 now (0.32 Mach, under half), and the
	// FULL plateau is only 0.97-1.03 - about a second of flight. Baked constants, not a
	// slider: this is a law about air, not a fact about a hull, so it is the origin-tilt
	// pattern (find the number, delete the control) rather than the shell-standoff one.
	//
	// ⚠️ ROUND 3 HANDED THE WINDOW TO THE USER instead - his design: *"add a double pill
	// slider, min/max Mach, so the user can adjust the range... they can tighten it or
	// loosen it."* The bounds now come from g_fx.vapMachMin/Max (default 0.85-1.15, which
	// is round 2's width recentred); what stays here is the SHAPE of the response inside
	// the window. Read his framing as the general rule, because it settles a tension this
	// project has carried since the first slider: *"everything is physics based, just the
	// user has some control over the look of the effect."* The sim owns what HAPPENS - the
	// Mach angle, the density gate, the shading - and the user owns the BOUNDS. That is
	// the LAB→PHYSICS shape promised for the reentry automatic mode (NEXT item 1),
	// arriving early on a much smaller effect.
	//
	// RAMPS AS FIXED FRACTIONS OF THE WINDOW - invariant 23(b)'s law, transferred intact
	// from the EXPANSION BAND. Fade-in takes the first 30% of the window, fade-out the
	// last 30%, so the two can never overlap however tightly the handles are closed: a
	// narrow window gives a short sharp flash rather than a fade-in that is still
	// finishing when the fade-out begins.
	const float VAP_RAMP_IN  = 0.30f;
	const float VAP_RAMP_OUT = 0.30f;

	// TEST drives this Mach regardless of what the vessel is doing. 1.15 is chosen to be
	// JUDGEABLE rather than to be the peak of the band: right at M 1 the cone is a
	// near-flat collar, which is physically correct and which tells you almost nothing
	// about the shape from three of the four directions you might be looking from. At
	// 1.15 it is unmistakably three-dimensional and still recognisably the reference
	// photograph. Judging a look must not require flying an ascent profile first - the
	// god rays' TEST reasoning (invariant 24b).
	const double VAP_TEST_M   = 1.15;

	// Half-angle clamps. Unclamped, mu -> 90 deg at M = 1 gives a zero-length disc
	// (a ring seen edge-on, i.e. nothing) and mu -> small at high M gives a needle
	// stretching to the horizon. Neither is a look; both are the maths being taken
	// past where it describes anything.
	const double VAP_MU_MIN   = 26.0 * RAD;
	const double VAP_MU_MAX   = 84.0 * RAD;

	// How close the camera may get before the shroud fades out. Inside the cone
	// there is no cone to see, and the near-plane behaviour of a surface wrapped
	// around the eye is not worth defining - so it fades before it can arise. In
	// multiples of the cone's own outer radius.
	const float  VAP_NEAR_OUT = 1.05f;   // fully gone at or inside this
	const float  VAP_NEAR_IN  = 2.10f;   // untouched at or beyond this

	// Optical depth: how much of the shroud one edge-on pass through the sheet
	// removes at Strength 1. Fed through Beer-Lambert below, so it SATURATES rather
	// than clipping - G9's law about hard alpha clamps flattening Gouraud gradients
	// into cutouts applies to opacity exactly as it did to emission.
	const float  VAP_TAU      = 0.42f;

	// --- THE FLICKER (2026-08-11, his first look: "it is also too static") -----
	// A vapour cone is not a decal, it is a condensation event standing in a turbulent
	// flow: it breathes, it pulses, and it shifts as the flow over the hull changes by
	// fractions of a degree. THREE modulations, and they are ONE PHYSICAL EVENT rather
	// than three effects - the opacity, the size and the surface all ride the same
	// number, because a stronger condensation event is denser AND fills more volume at
	// the same moment. His words: "a small size variation (INCLUDED in the flicker)".
	//
	// ⚠️ ALL OF IT IS A PURE FUNCTION OF (ring, theta, REAL time). That is what makes it
	// safe, and the argument is invariant 23(d)'s verbatim: two quads meeting on an edge
	// evaluate the identical expression from the identical indices, so no seam can open;
	// and nothing is stored between frames, so G10's accumulated-state disease has
	// nowhere to live. REAL time (invariant 4) - a shroud must not freeze at 10x nor
	// strobe.
	const float  VAP_FLICK    = 0.32f;   // opacity swing, +-32% about full
	const float  VAP_FLICK_SZ = 0.45f;   // ... and the size rides 45% of that (+-14%),
	                                     //   so it BREATHES rather than pumps. Because
	                                     //   the axial reach is derived from the radius,
	                                     //   the whole cone scales uniformly and the
	                                     //   Mach-angle relationship survives untouched.
	// (VAP_MOTTLE lived here for one round. It roughened the SHEET - thick and thin
	//  patches drifting across it - and he cut it: "I don't like the distortion of the
	//  disk". The clean analytic surface is part of why this reads as a shock front.)

	inline double clampd(double x, double a, double b) { return x < a ? a : (x > b ? b : x); }
	inline float  clampf(float  x, float  a, float  b) { return x < a ? a : (x > b ? b : x); }
	inline float  sstepf(float t) { t = clampf(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }

	// Smoothstepped ramp between two thresholds, either direction.
	float ramp(double v, double atZero, double atOne)
	{
		if (fabs(atOne - atZero) < 1e-30) return v >= atOne ? 1.0f : 0.0f;
		return sstepf((float)((v - atZero) / (atOne - atZero)));
	}

	// Camera context, fetched ONCE per build (each field is an oapi call). Its own
	// copy rather than a shared one, exactly as OroAurora.cpp keeps its own: three
	// lines of duplication against a header dependency between two effects that have
	// nothing else to say to each other.
	struct CamCtx { VECTOR3 pos; MATRIX3 rot; double tanAp; };
	void GetCam(CamCtx& c)
	{
		oapiCameraGlobalPos(&c.pos);
		oapiCameraRotationMatrix(&c.rot);
		c.tanAp = tan(oapiCameraAperture());
	}

	// Global position -> viewport PIXELS + camera-space forward distance.
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

	// Pack 0xAABBGGRR with clamping (invariant 5).
	inline DWORD VCol(int r, int g, int b, int a)
	{
		if (r < 0) r = 0; if (r > 255) r = 255;
		if (g < 0) g = 0; if (g > 255) g = 255;
		if (b < 0) b = 0; if (b > 255) b = 255;
		if (a < 0) a = 0; if (a > 255) a = 255;
		return ((DWORD)a << 24) | ((DWORD)b << 16) | ((DWORD)g << 8) | (DWORD)r;
	}
}

// ----------------------------------------------------------------------------
// Per frame, MAIN thread. Builds the shroud into vapVtx / vapDepth and sets
// vapActive; publishes the Mach number and the gate for the dialog readout.
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// THE SPLIT (2026-08-15, the pause fix). UpdateVapour does the oapi work - which
// vessel, its Mach, the gates, the flow axis, the sun. BuildVapourGeometry lofts and
// projects the cone in the render path. See ProjCam in OroModule.h.
// ----------------------------------------------------------------------------
namespace {
	struct VapSnap {
		OBJHANDLE hV;      // for the render-epoch anchor (invariant 21a)
		double  size, mUse;
		float   fGate1, fGate2;   // per cone since 2026-08-29: two Mach windows, one air
		VECTOR3 Cg, fwdG, downG;
		MATRIX3 Rv;        // the radial basis is anchored to the HULL (invariant 25e) -
		                   //   without it the theta-keyed boil spins on attitude change
		VECTOR3 sunG;
		bool    haveSun;
		float   dayF;
	};
	VapSnap s_vap;
	bool    s_vapValid = false;
}

void OroModule::UpdateVapour()
{
	s_vapValid = false;
	vapVtxN   = 0;
	vapActive = false;
	g_fx.vapMach   = 0.0f;
	g_fx.vapVis    = 0.0f;
	g_fx.vapWhy[0] = '\0';

	const bool live = g_fx.masterArmed && (g_fx.vapEnabled || g_fx.vapTest);
	if (!live) { strcpy_s(g_fx.vapWhy, sizeof(g_fx.vapWhy), "off"); return; }
	// ONE pill, TWO cones (2026-08-29, his design): each cone's OPACITY is its own
	// visibility, so the effect as a whole is off only when both are at zero.
	const bool cone1 = g_fx.vapStrength  > 0.001f;
	const bool cone2 = g_fx.vapStrength2 > 0.001f;
	if (!cone1 && !cone2) { strcpy_s(g_fx.vapWhy, sizeof(g_fx.vapWhy), "opacity 0"); return; }
	if (viewW == 0 || viewH == 0) return;

	// The vessel the camera is looking at - invariant 15's camera-target rule. A cone
	// on a ship forty kilometres away is a subpixel smudge nobody asked to pay for.
	OBJHANDLE hV = oapiCameraTarget();
	if (!hV || oapiGetObjectType(hV) != OBJTP_VESSEL) hV = oapiGetFocusObject();
	if (!hV || oapiGetObjectType(hV) != OBJTP_VESSEL) {
		strcpy_s(g_fx.vapWhy, sizeof(g_fx.vapWhy), "no target");
		return;
	}
	VESSEL* v = oapiGetVesselInterface(hV);
	if (!v) { strcpy_s(g_fx.vapWhy, sizeof(g_fx.vapWhy), "no target"); return; }

	const double size = v->GetSize() > 1.0 ? v->GetSize() : 1.0;
	const double mach = v->GetMachNumber();
	g_fx.vapMach = (float)mach;

	// THE VIEW GATE SITS HERE, AFTER THE MACH READ, AND THAT ORDERING IS THE POINT.
	// Drawing is external-only (invariant 10), but the READOUT has to keep working from
	// the cockpit or it reports "M 0.00" for the entire ascent - which is exactly the
	// window in which he needs to know whether to switch to an external view. Same shape
	// as the aurora's rule that identifying the world must precede every gate that only
	// decides drawing (invariant 17): a number and a picture are different questions.
	if (!extGate) { strcpy_s(g_fx.vapWhy, sizeof(g_fx.vapWhy), "internal"); return; }

	// --- the gates ------------------------------------------------------------
	// TEST bypasses all of them, and says so in the readout rather than pretending
	// the conditions are met. Since the second cone the Mach WINDOW is per cone -
	// two bands, two gates, so the collars can appear at different vehicle Mach -
	// while the air is one fact both share.
	double mUse = mach;
	float  fGate1 = 1.0f, fGate2 = 1.0f;

	if (g_fx.vapTest) {
		mUse = VAP_TEST_M;
	} else {
		const double rho = v->GetAtmDensity();
		const float fAir = ramp(rho, VAP_RHO_MIN, VAP_RHO_FULL);
		if (fAir <= 0.001f) {
			// Two honest reasons, and they are worth telling apart: a ship in orbit and
			// a ship at 25 km are both "no cone" but only one of them is ever going to
			// produce one by flying differently.
			strcpy_s(g_fx.vapWhy, sizeof(g_fx.vapWhy), rho < 1e-6 ? "vacuum" : "thin air");
			return;
		}
		// The user's window, with the ramps as fixed fractions INSIDE it (invariant 23b).
		// Guarded rather than trusted: a class cfg written by hand, or one saved before
		// these keys existed, can hand us a reversed or zero-width pair.
		auto band = [&](float lo0, float hi0) -> float {
			double mLo = (double)lo0, mHi = (double)hi0;
			if (mHi < mLo + 0.02) mHi = mLo + 0.02;
			const double win = mHi - mLo;
			return ramp(mach, mLo,       mLo + win * VAP_RAMP_IN)
			     * (1.0f - ramp(mach, mHi - win * VAP_RAMP_OUT, mHi));
		};
		const float fM1 = cone1 ? band(g_fx.vapMachMin,  g_fx.vapMachMax)  : 0.0f;
		const float fM2 = cone2 ? band(g_fx.vapMachMin2, g_fx.vapMachMax2) : 0.0f;
		if (fM1 <= 0.001f && fM2 <= 0.001f) {
			// "subsonic" vs "past band" judged against the highest window any VISIBLE
			// cone has - the readout answers for the effect, not for one collar.
			double hiAll = cone1 ? (double)g_fx.vapMachMax : 0.0;
			if (cone2 && (double)g_fx.vapMachMax2 > hiAll) hiAll = (double)g_fx.vapMachMax2;
			strcpy_s(g_fx.vapWhy, sizeof(g_fx.vapWhy), mach < hiAll ? "subsonic" : "past band");
			return;
		}
		fGate1 = fAir * fM1;
		fGate2 = fAir * fM2;
	}

	// --- the flow axis --------------------------------------------------------
	// THE CONE RIDES THE RELATIVE WIND. Not the hull's long axis, not +Z, and - the part
	// that matters for unconventional vessels - NOT THE ENGINES: this asks the atmosphere
	// which way the ship is moving through it, so how the vessel defines its thruster
	// groups never enters the calculation. A tail-sitter climbing on hover engines has an
	// airspeed vector along its own +Y, so it gets its cone around +Y with nothing special
	// done for it; a lifting body at 40 degrees AoA gets a cone canted off its nose by 40
	// degrees, which is where the shock actually stands. Both fall out of asking the right
	// question rather than from a special case (his question, 2026-08-11).
	//
	// The +Z fallback below is the ONE axis assumption in the file and it is deliberately
	// unreachable in flight: at M 0.9 the ship is doing ~300 m/s, so the only way to be
	// under 5 m/s is to be parked - where there is no relative wind and therefore no
	// correct answer, only a conventional one. It exists so TEST is judgeable from a
	// runway (the lightning TEST's reasoning, invariant 22g).
	VECTOR3 flowLocal = _V(0, 0, 1);
	{
		VECTOR3 va;
		if (v->GetAirspeedVector(FRAME_LOCAL, va)) {
			const double L = length(va);
			if (L > 5.0) flowLocal = va / L;
		}
	}

	VECTOR3 Cg;  oapiGetGlobalPos(hV, &Cg);
	MATRIX3 Rv;  oapiGetRotationMatrix(hV, &Rv);
	const VECTOR3 fwdG  = mul(Rv, flowLocal);       // direction of travel, global
	const VECTOR3 downG = -fwdG;                    // downstream, global

	// --- the sun -------------------------------------------------------------
	// HOISTED HERE 2026-08-15: it needs oapi and does NOT need the camera, so it belongs
	// on the main thread with the rest of the world state. What it is FOR is explained
	// where it is used, in the build.
	VECTOR3 sunG = Cg + fwdG;                       // harmless placeholder
	bool    haveSun = false;
	float   dayF = 1.0f;
	{
		OBJHANDLE hSun = OroFindStar();
		if (hSun) {
			oapiGetGlobalPos(hSun, &sunG);
			haveSun = true;
			OBJHANDLE hRef = v->GetSurfaceRef();
			if (hRef) {
				VECTOR3 bp; oapiGetGlobalPos(hRef, &bp);
				const VECTOR3 up = unit(Cg - bp);
				const double  el = dotp(up, unit(sunG - Cg));
				dayF = ramp(el, -0.12, 0.04);       // sun below the horizon -> dark
			}
		}
	}

	// --- hand the world state to the render path and stop ---------------------
	s_vap.hV   = hV;
	s_vap.size = size;   s_vap.mUse = mUse;
	s_vap.fGate1 = fGate1;  s_vap.fGate2 = fGate2;
	s_vap.Cg   = Cg;     s_vap.fwdG = fwdG;   s_vap.downG = downG;
	s_vap.Rv   = Rv;
	s_vap.sunG = sunG;   s_vap.haveSun = haveSun; s_vap.dayF = dayF;
	s_vapValid = true;
}

// ----------------------------------------------------------------------------
// BuildVapourGeometry - THE RENDER PATH HALF (2026-08-15). The cone is a screen-space
// surface of revolution, so like every other projected effect it has to be rebuilt
// against the camera the frame is actually drawn with; clbkPreStep does not run while
// PAUSED. See ProjCam in OroModule.h.
//
// INVARIANT-1 AUDIT: zero oapi calls. The world state arrives in s_vap; the g_fx writes
// (the vapWhy readout) are plain single-thread member writes, not oapi.
// ----------------------------------------------------------------------------
void OroModule::BuildVapourGeometry()
{
	vapVtxN   = 0;
	vapActive = false;
	if (!s_vapValid) return;
	if (viewW == 0 || viewH == 0) return;

	CamCtx cc;
	if (!FillProjCam(cc.pos, cc.rot, cc.tanAp)) return;

	const double  size = s_vap.size, mUse = s_vap.mUse;
	// ⚠️ RENDER-EPOCH ANCHOR (invariant 21a). Cg was sampled at pre-step in the
	// BARYCENTRIC frame and this build runs a step later, by which time Earth has moved
	// ~500 m at 60 fps. Pairing the render camera with a pre-step anchor put the cone
	// several hundred metres off the hull and made it jitter with frame pacing.
	// fwdG / downG are DIRECTIONS - a translation leaves them alone.
	const VECTOR3 Cg = s_vap.Cg + RenderEpochShift(s_vap.hV, s_vap.Cg);
	const VECTOR3 fwdG = s_vap.fwdG, downG = s_vap.downG;
	const MATRIX3 Rv = s_vap.Rv;
	// The vessel's own X/Y axes in global space - the FULL PLACEMENT controls'
	// frame (2026-08-30, his fix round): Position x/y nudge the apex along these,
	// Pitch/Yaw tilt each cone's axis about them. Orbiter's convention throughout.
	const VECTOR3 axXg = mul(Rv, _V(1, 0, 0));
	const VECTOR3 axYg = mul(Rv, _V(0, 1, 0));
	const VECTOR3 sunG = s_vap.sunG;
	const bool    haveSun = s_vap.haveSun;
	float         dayF = s_vap.dayF;

	// --- the light (shared) ---------------------------------------------------
	// A cloud has no light of its own; it shows because the sun is on it. Two terms:
	// a per-vertex lambert that gives the shroud its ROUNDNESS (without it a
	// surface of revolution reads as a flat ring), and a day factor, because on the
	// night side there is nothing illuminating it and a bright white cone hanging off
	// a darkened ship would be the most obvious lie in the addon.
	// (sunG / haveSun / dayF are gathered on the main thread - see UpdateVapour.)
	// TEST keeps it lit: "I turned it on at night and saw nothing" is the same
	// silent failure the god rays' backwards Threshold produced (invariant 24d).
	if (g_fx.vapTest && dayF < 0.85f) dayF = 0.85f;

	// --- the Mach angle, and therefore the shape (shared: one vehicle, one Mach,
	// both cones take their length from it) ------------------------------------
	// mu = asin(1/M): 90 deg at M = 1 (a flat collar), tightening as the vehicle
	// outruns its own pressure waves. THIS is why the length is not a raw slider.
	const double mEff = (mUse > 1.02) ? mUse : 1.02;
	const double mu   = clampd(asin(1.0 / mEff), VAP_MU_MIN, VAP_MU_MAX);

	const int NA = VAP_NA, NR = VAP_NR;
	const float t2 = animT;                          // REAL time (invariant 4): the
	                                                 // shimmer must not freeze at 10x

	// Radial basis perpendicular to the flow axis (shared - one hull), ANCHORED TO THE
	// VESSEL rather than to global space - the aurora's discipline, where the ring's
	// basis is tied to the planet's prime meridian so the curtain co-rotates with the
	// world instead of with the coordinate system.
	// ⚠️ IT WAS ANCHORED TO GLOBAL +Y FOR ONE ROUND, and that is a latent artefact: a
	// vessel flying near the global Y axis makes the projection tiny, so the normalised
	// basis swings wildly for a small attitude change and the theta-keyed boil pattern
	// SPINS around the cone. Amplitude is only +-3% so it would have been a faint
	// mystery rather than an obvious bug, which is exactly the kind that survives for
	// months. Anchored to the hull, the basis is degenerate only in pure 90-degree
	// sideslip (flow exactly along the vessel's own X), where it falls back to vessel Y -
	// and it also puts the boil pattern in the airframe's frame, which is where the flow
	// structure it stands for actually lives.
	// (The basis itself moved INSIDE BuildCone on 2026-08-30, his fix round: once a
	//  cone can be tilted by its own Pitch/Yaw the axes differ PER CONE, and a basis
	//  built for the shared wind would smear a tilted cone's elliptical section. The
	//  vessel-anchored rule and its sideslip fallback travelled with it, unchanged.)

	// ------------------------------------------------------------------------
	// ONE PILL, TWO CONES (2026-08-29, his design - the Concorde photo). The loft
	// below runs once per cone with its own knob block; each cone's OPACITY is its
	// own visibility, so there is no second pill to invent. Everything the cones
	// SHARE (the Mach angle, the air, the light, the radial basis) is computed
	// above; everything a cone OWNS lives in VapKnobs.
	// ------------------------------------------------------------------------
	struct VapKnobs {
		float opacity, sx, sy, sz, streaks, churn, flickHz, pos;
		float posX, posY;        // apex nudge along vessel X/Y [hull sizes] (2026-08-30)
		float pitchDeg, yawDeg;  // axis tilt about vessel X/Y [deg, +-30], pivot = apex
		DWORD col, colStk;
		bool  baseOn;      // the BASE FILL pill - the closing disc, per cone
		float baseOfs;     // BASE FILL OFFSET (2026-09-04): the cap centre's axial
		                   //   displacement, x this cone's own reach, -1..+0.5
	};
	float visMax = 0.0f;

	auto BuildCone = [&](const VapKnobs& K, float fGate) {
		if (K.opacity <= 0.001f || fGate <= 0.001f) return;

		// --- THE FLICKER: one number, TWO consequences (per cone - its own rate) --
		// Three octaves of REAL time (invariant 4) at the user's rate. The same value
		// drives opacity below AND the size right here, because they are one event: a
		// stronger condensation is denser and bigger at the same instant ("a small size
		// variation INCLUDED in the flicker" - his coupling). The octave ratios
		// (x1, x1.9, x2.8) are deliberately tight: at the top of the 8 Hz slider the
		// fastest term is ~22 Hz, still resolved at ordinary frame rates - spread them
		// further and the maximum setting aliases into a strobe.
		// (Round 2's surface mottle was cut here and stays cut - invariant 25k: scale
		// and opacity are the honest degrees of freedom, the analytic surface is the
		// look. The streaks below paint COLOUR on it and deform nothing.)
		const float fHz  = clampf(K.flickHz, 0.0f, 8.0f) * 6.2831853f;   // -> rad/s
		const float fk = 0.55f * sinf(t2 * fHz)
		               + 0.30f * sinf(t2 * fHz * 1.9f + 1.3f)
		               + 0.15f * sinf(t2 * fHz * 2.8f + 2.7f);       // ~ -1 .. +1
		const float flick = 1.0f + VAP_FLICK * fk;                   // 0.68 .. 1.32

		// THE THREE SIZES (his spec: "full control of the size, not just the radius").
		// X and Y are BOTH radii in hull sizes - "same slider values for x and y = a
		// circular cone" - and both breathe with the same flicker. Z is a RATIO of the
		// Mach-angle reach (1 = the physics length, 0 = a flat collar disc), a ratio ON
		// PURPOSE: the length still stretches with Mach - invariant 25(b) survives as
		// the thing Z multiplies rather than replaces. A cfg from the one-Size era
		// loads circular via the LoadClass sentinel (Y := X when the file has no
		// VapourSizeY key).
		// (Y is floored at 5% of X: at exactly zero the elliptical normal below would
		//  degenerate on the horizontal meridian, and 5% is already a blade.)
		const double breath = (double)(1.0f + VAP_FLICK_SZ * VAP_FLICK * fk);
		const double RmaxX = size * (double)clampf(K.sx, 0.0f, 3.0f) * breath;
		if (RmaxX < 0.5) {
			if (!g_fx.vapWhy[0]) strcpy_s(g_fx.vapWhy, sizeof(g_fx.vapWhy), "size 0");
			return;
		}
		double RmaxY = size * (double)clampf(K.sy, 0.0f, 3.0f) * breath;
		if (RmaxY < 0.05 * RmaxX) RmaxY = 0.05 * RmaxX;
		const double Lax   = (RmaxX / tan(mu))
		                   * (double)clampf(K.sz, 0.0f, 2.0f);   // axial reach
		const double RmaxE = (RmaxX > RmaxY) ? RmaxX : RmaxY;    // envelope, for the fades

		// The apex station: Position z slides along the FLOW axis as it always did
		// (positive = upstream, ahead of the hull's centre - a per-hull fact, per
		// CONE, which is what puts one collar at the canopy and one at the tail),
		// and since 2026-08-30 Position x/y nudge it along the VESSEL's own axes -
		// his fix round: "for some vessels that's not enough". All in hull sizes.
		const VECTOR3 apexG = Cg + fwdG * ((double)clampf(K.pos,  -2.0f, 2.0f) * size)
		                    + axXg * ((double)clampf(K.posX, -2.0f, 2.0f) * size)
		                    + axYg * ((double)clampf(K.posY, -2.0f, 2.0f) * size);

		// --- THIS CONE'S OWN AXIS (2026-08-30) --------------------------------
		// The shared wind, tilted by the cone's Pitch/Yaw about the VESSEL's own
		// axes, pivoting at the apex above: +Pitch tips the apex end toward vessel
		// +Y (up), +Yaw toward vessel +X (right). Rodrigues rotation - pure math,
		// render-path safe. At 0/0 fwdK IS the wind and everything below is the
		// pre-fix cone bit for bit. Roll is deliberately absent: this is a surface
		// of revolution, and a roll knob would be a control that does nothing.
		VECTOR3 fwdK = fwdG;
		{
			auto rotAbout = [](const VECTOR3& vv, const VECTOR3& ax, double ang) {
				const double c = cos(ang), s = sin(ang);
				return vv * c + crossp(ax, vv) * s + ax * (dotp(ax, vv) * (1.0 - c));
			};
			const double pr = (double)clampf(K.pitchDeg, -30.0f, 30.0f) * RAD;
			const double yr = (double)clampf(K.yawDeg,   -30.0f, 30.0f) * RAD;
			if (pr != 0.0) fwdK = rotAbout(fwdK, axXg, -pr);
			if (yr != 0.0) fwdK = rotAbout(fwdK, axYg,  yr);
		}
		// ⚠️ DELIBERATE SHADOWING: downG here hides the outer (wind) downG so the
		// whole loft below - stations, normals, streaks, base - follows the tilted
		// axis with zero renames. The outer name has no other consumer inside this
		// lambda; anything that needs the raw wind reads fwdG explicitly above.
		const VECTOR3 downG = -fwdK;

		// Radial basis perpendicular to THIS cone's axis, ANCHORED TO THE VESSEL
		// rather than to global space (the aurora's discipline; the global-+Y round
		// span the boil pattern - see the 2026-08-11 note in the file history).
		// Degenerate only in pure 90-degree sideslip, where it falls back to vessel
		// +Y; unreachable third fallback kept for the same reason as ever.
		VECTOR3 ref1 = mul(Rv, _V(1, 0, 0));                 // vessel +X (spanwise)
		VECTOR3 e1   = ref1 - downG * dotp(downG, ref1);
		double  e1l  = length(e1);
		if (e1l < 1e-6) {
			ref1 = mul(Rv, _V(0, 1, 0));                     // vessel +Y (up)
			e1   = ref1 - downG * dotp(downG, ref1);
			e1l  = length(e1);
			if (e1l < 1e-6) { e1 = _V(1, 0, 0); e1l = 1.0; } // unreachable: two orthogonal
			                                                 // axes cannot both align
		}
		e1 = e1 / e1l;
		const VECTOR3 e2 = crossp(downG, e1);

		// --- near fade --------------------------------------------------------
		// You cannot see a cone you are inside. Fading before that case arises is
		// cheaper and more honest than defining what an inside-out surface of
		// revolution should look like.
		{
			const double dCam = length(Cg - cc.pos);
			const float  fNear = ramp(dCam, RmaxE * VAP_NEAR_OUT, RmaxE * VAP_NEAR_IN);
			if (fNear <= 0.001f) {
				if (!g_fx.vapWhy[0]) strcpy_s(g_fx.vapWhy, sizeof(g_fx.vapWhy), "too close");
				return;
			}
			fGate *= fNear;
		}

		// OPACITY (renamed from Strength 2026-08-29 - his spec: "I want it to be able
		// to hide the vessel behind the vapor cone at a high opacity. At 0 we should
		// not see a cone."). Up to 1.0 the response is the original curve, untouched.
		// ABOVE 1.0 two things climb together: the optical depth (quadratically -
		// face-on alpha reaches ~0.98 at the top, the reference photos' fuselage-
		// swallowing disc) and a FILL term that lifts the translucent middle of the
		// density profile toward the rim's - hiding needs the whole face dense, not a
		// bright ring around a see-through middle.
		const float strK   = clampf(K.opacity, 0.0f, 2.0f);
		const float over   = (strK > 1.0f) ? (strK - 1.0f) : 0.0f;
		const float strEff = strK + 7.5f * over * over;      // <=1 identical; 2 -> 9.5
		const float fill   = sstepf(over);                   // 0 below 1, 1 at the top
		const float tauK   = VAP_TAU * strEff * fGate * flick;  // the SAME flick that sized it

		// The readout reports the strongest GATE, deliberately not the flickering
		// value: he reads it to know where he is in the transonic window, and a number
		// jittering 30% twice a second says nothing he can act on.
		if (fGate > visMax) visMax = fGate;
		if (tauK <= 0.002f) return;

		// --- the loft ---------------------------------------------------------
		// Rings from the apex (ring 0, a point) out to the rim (ring VAP_NR - 1), plus
		// one FEATHER ring at alpha 0 so the outer edge dissolves instead of ending on
		// a hard circle. Radius runs as t^0.70 against a linear axial march, which
		// bells the profile outward near the apex the way a real collar bulges - a
		// straight cone reads as a paper party hat.
		// Per-ring geometry, computed once and shared by both rings of every quad
		// band - which is also what guarantees no seam can open at a joint (invariant
		// 23d's safety argument: a pure function of the ring/segment index cannot
		// disagree with itself across a shared edge).
		double rrX[VAP_NR + 1], rrY[VAP_NR + 1], rr[VAP_NR + 1], xx[VAP_NR + 1];
		float  aa[VAP_NR + 1];
		for (int ir = 0; ir <= NR; ir++) {
			const float t = (float)ir / (float)(NR - 1);      // 1.0 AT THE RIM, > 1 = feather
			const float tc = clampf(t, 0.0f, 1.0f);
			const double pf = (double)powf(tc, 0.70f) * (ir >= NR ? 1.10 : 1.0);
			rrX[ir] = RmaxX * pf;                             // elliptical cross-section:
			rrY[ir] = RmaxY * pf;                             //   ONE profile, two radii
			rr[ir]  = 0.5 * (rrX[ir] + rrY[ir]);              // mean, for the meridian slope
			xx[ir] = Lax  * (double)tc              * (ir >= NR ? 1.10 : 1.0);
			// Density along the meridian: nothing at the apex, densest at the rim -
			// the compression edge, the photographs' bright ring around a translucent
			// middle. The FILL term lifts that middle toward the rim's density as the
			// opacity climbs past 1 (see strEff above); the feather ring stays at zero
			// regardless, or the outer edge would harden into a circle.
			aa[ir] = (ir >= NR) ? 0.0f : (powf(tc, 1.30f) * (1.0f - fill) + fill);
		}

	// --- THE STREAKS (2026-08-29; reworked the same day on his first look) -----
	// "A few darker streaks length-wise on the vapor" became, after round 1 ("too few
	// and thick"): MANY SLIM filaments, the slider simply ADDS them, and the churn is
	// "violent and erratic" rather than a drift. A per-THETA modulation only - darker,
	// denser lines running apex -> rim - computed once per frame into a table that the
	// loft and the base cap both read, so the two surfaces stripe identically.
	// ⚠️ COLOUR AND DENSITY ONLY, NEVER GEOMETRY. Invariant 25(k)'s mottle was cut for
	// deforming the disc; these paint on the analytic surface and leave it exactly
	// where it was, which is the half of that lesson that survives his new ask.
	// Pure functions of (theta, REAL time) - 23(d)'s seam-and-accumulation argument:
	// nothing stored, and ib = NA evaluates the same theta as ib = 0, so the wrap
	// column cannot disagree with itself.
	float strk[VAP_NA + 1];
	const float stkK = clampf(K.streaks, 0.0f, 2.0f);
	const float chK  = clampf(K.churn,   0.0f, 2.0f);
	int NS = (int)(stkK * 16.0f + 0.5f);            // the slider IS the count (0..32)
	if (NS > 32) NS = 32;
	if (stkK > 0.001f && NS < 2) NS = 2;
	if (NS > 0) {
		// CHURN SCALES THE CLOCK of everything the streaks do - drift, two jitter
		// octaves and the flare/die gate all run on tt, so 0 freezes the pattern
		// outright (the Soot churn convention) and 2 runs it doubly fast. The
		// "violent" is two things together: position jitter with real amplitude at
		// ~1-3 Hz, and an INTERMITTENCY gate (squared, so it is peaky) that makes
		// filaments flare and die rather than parade - the VC glow's lesson (27g)
		// that erratic reads as things being torn apart, not as things sliding.
		const float tt = t2 * chK;
		float cth[32], shp[32], gk[32];
		for (int k = 0; k < NS; k++) {
			const float ph  = (float)k * 2.399963f + 0.7f;   // golden-angle spread: even
			                                                 //   coverage, never regular
			const float dir = (k & 1) ? 1.0f : -1.0f;
			cth[k] = ph + dir * 0.25f * tt
			       + 0.30f * sinf(tt * (1.9f + 0.33f * (float)k) + ph * 2.3f)
			       + 0.14f * sinf(tt * (5.1f + 0.71f * (float)k) + ph * 5.9f);
			// SLIM: half-widths ~3-4 deg (the reason VAP_NA rose to 160 - a Gouraud
			// feature cannot be narrower than ~2 segments however sharp the maths).
			shp[k] = 260.0f + 200.0f * (0.5f + 0.5f * sinf(ph * 9.1f));
			const float fl = 0.5f + 0.5f * sinf(tt * (2.6f + 0.47f * (float)k) + ph * 3.1f);
			gk[k] = (0.55f + 0.45f * sinf(ph * 12.7f))       // per-streak depth variety
			      * (0.25f + 0.75f * fl * fl);               // the flare/die gate
		}
		for (int ib = 0; ib <= NA; ib++) {
			const float th = (float)(ib % NA) / (float)NA * 6.2831853f;
			float s = 0.0f;
			for (int k = 0; k < NS; k++) {
				const float c = cosf(th - cth[k]);
				if (c > 0.60f) s += gk[k] * powf(c, shp[k]);   // below 0.6 the pow is ~0
			}
			strk[ib] = (s > 1.2f) ? 1.2f : s;
		}
	} else {
		for (int ib = 0; ib <= NA; ib++) strk[ib] = 0.0f;
	}

	// The colour picks, unpacked once (COLORREF 0x00BBGGRR - R in the low byte). The
	// vapour body and the streak filaments each follow their swatch; the default pair
	// reproduces the baked look (cool white vapour, darker grey-blue streaks).
	const float vRc = (float)( K.col         & 0xFF);
	const float vGc = (float)((K.col   >> 8) & 0xFF);
	const float vBc = (float)((K.col  >> 16) & 0xFF);
	const float kRc = (float)( K.colStk        & 0xFF);
	const float kGc = (float)((K.colStk  >> 8) & 0xFF);
	const float kBc = (float)((K.colStk >> 16) & 0xFF);

	// Scratch for two rings at a time (the loft only ever needs the previous one).
	float px[2][VAP_NA + 1], py[2][VAP_NA + 1], pd[2][VAP_NA + 1];
	DWORD pc[2][VAP_NA + 1];
	bool  pk[2][VAP_NA + 1];

	auto emit3 = [&](int ra, int ia, int rb, int ib, int rc2, int ic) {
		if (vapVtxN + 3 > VAP_MAX_TRI * 3) return;
		vapVtx[vapVtxN].x = px[ra][ia]; vapVtx[vapVtxN].y = py[ra][ia];
		vapVtx[vapVtxN].c = FogColNear(pc[ra][ia], pd[ra][ia]); vapDepth[vapVtxN] = pd[ra][ia]; vapVtxN++;
		vapVtx[vapVtxN].x = px[rb][ib]; vapVtx[vapVtxN].y = py[rb][ib];
		vapVtx[vapVtxN].c = FogColNear(pc[rb][ib], pd[rb][ib]); vapDepth[vapVtxN] = pd[rb][ib]; vapVtxN++;
		vapVtx[vapVtxN].x = px[rc2][ic]; vapVtx[vapVtxN].y = py[rc2][ic];
		vapVtx[vapVtxN].c = FogColNear(pc[rc2][ic], pd[rc2][ic]); vapDepth[vapVtxN] = pd[rc2][ic]; vapVtxN++;
	};

	for (int ir = 0; ir <= NR; ir++) {
		const int cur = ir & 1;
		// Meridian tangent (dx, dr) -> outward normal (-dr, dx) in the (downstream,
		// radial) basis, i.e. pointing away from the axis and tilted UPSTREAM, which
		// is what the outside of a cone opening aft actually faces.
		const int  irp = (ir > 0) ? ir - 1 : 0, irn = (ir < NR) ? ir + 1 : NR;
		const double dx = xx[irn] - xx[irp], dr = rr[irn] - rr[irp];
		const double dl = sqrt(dx * dx + dr * dr);
		const double nAx = (dl > 1e-9) ? (-dr / dl) : 0.0;   // along DOWNSTREAM
		const double nRa = (dl > 1e-9) ? ( dx / dl) : 1.0;   // along RADIAL

		for (int ib = 0; ib <= NA; ib++) {
			const int   bb = ib % NA;
			const float th = (float)bb / (float)NA * 6.2831853f;
			const float ct = cosf(th), st = sinf(th);
			// THE BOIL, back at round 1's strength - the version he approved on sight.
			// Two counter-drifting octaves in THETA ONLY: every ring deforms the same
			// way, so the disc stays a clean surface of revolution and this reads as a
			// faint breathing of the rim rather than as a deformed shape.
			// It modulates the RADIUS - a property of the surface, evaluated from the
			// same (ir, ib) by both quads that meet on this segment - so it cannot open
			// a gap, and it is a pure function of (theta, time) so it cannot accumulate
			// (invariant 23d's argument; and G12(c)'s prohibition is on a CENTRELINE
			// wander, which this is not - the axis never moves).
			const float boil = 1.0f
			                 + 0.030f * sinf(th * 3.0f + t2 * 0.9f)
			                 + 0.018f * sinf(th * 7.0f - t2 * 1.5f);

			// Elliptical cross-section: one angular parameter, two radii (Size x/y).
			const VECTOR3 gp = apexG + downG * xx[ir]
			                 + (e1 * ((double)ct * rrX[ir])
			                  + e2 * ((double)st * rrY[ir])) * (double)boil;

			double zz;
			pk[cur][ib] = ProjPx(cc, gp, viewW, viewH, px[cur][ib], py[cur][ib], zz);
			if (!pk[cur][ib]) { pc[cur][ib] = 0; pd[cur][ib] = 0.0f; continue; }
			pd[cur][ib] = (float)length(gp - cc.pos);

			// LIMB THICKENING. Edge-on you look along the sheet and see much more
			// water than you do face-on; that is what gives a thin shell its bright
			// rim, and it is the same physics invariant 19(b) buys with stacked
			// aurora sheets and invariant 15 calls limb brightening - here it buys
			// OPACITY rather than emission, because this is the one ORO surface
			// that absorbs.
			// The radial part of the normal is the ELLIPSE's gradient (ct*b, st*a),
			// not the position direction - on a circle they coincide, on a stretched
			// section only the gradient stays perpendicular to the surface.
			const VECTOR3 nrad = unit(e1 * ((double)ct * RmaxY) + e2 * ((double)st * RmaxX));
			const VECTOR3 nG = unit(downG * nAx + nrad * nRa);
			const VECTOR3 vd = unit(gp - cc.pos);
			const float   fc = (float)fabs(dotp(nG, vd));
			const float   thick = 1.0f / (fc > 0.12f ? fc : 0.12f);       // 1 .. 8.3

			// Beer-Lambert: alpha = 1 - exp(-tau). Saturates on its own, so no hard
			// clamp is needed and none of the Gouraud gradient gets flattened into a
			// cutout on the way (G9's law, restated for opacity).
			// The streaks land here as DENSITY (and as colour below): one factor,
			// two consequences - the flicker's own coupling argument at small scale.
			const float sF = strk[ib];
			const float aF = 1.0f - expf(-tauK * aa[ir] * thick * (1.0f + 0.70f * sF));

			// Sun shading gives the shroud its roundness. Never fully black on the
			// dark side - a real cloud still catches skylight and the planet below.
			float shade = 0.55f;
			if (haveSun) {
				const VECTOR3 sd = unit(sunG - gp);
				shade = 0.30f + 0.70f * (0.5f + 0.5f * (float)dotp(nG, sd));
			}
			shade *= (0.10f + 0.90f * dayF);

			// The vapour follows its picker; streak filaments BLEND toward theirs -
			// darker by default because the default pick is darker, not by a baked
			// multiplier, so the pair can also do stained or sunset-lit vapour.
			const float w  = (sF > 1.0f) ? 1.0f : sF;
			const int r8 = (int)((vRc + (kRc - vRc) * w) * shade + 0.5f);
			const int g8 = (int)((vGc + (kGc - vGc) * w) * shade + 0.5f);
			const int b8 = (int)((vBc + (kBc - vBc) * w) * shade + 0.5f);
			pc[cur][ib] = VCol(r8, g8, b8, (int)(aF * 255.0f + 0.5f));
		}

		if (ir == 0) continue;                       // apex ring: nothing to close yet
		const int prv = (ir - 1) & 1;
		for (int ib = 0; ib < NA; ib++) {
			const int ib2 = ib + 1;
			if (!pk[prv][ib] || !pk[prv][ib2] || !pk[cur][ib] || !pk[cur][ib2]) continue;
			emit3(prv, ib, cur, ib, cur, ib2);
			emit3(prv, ib, cur, ib2, prv, ib2);
		}
	}

	// --- THE BASE (2026-08-29, his reference photos: "it also has a base, something
	// that is missing from ours") ----------------------------------------------
	// A filled disc closing the cone's wide end - the thing every one of the photos
	// shows and the open loft never had. A fan of rings from the axis point out to
	// the rim circle, sharing the rim's radii, its theta samples and its boil, and -
	// through the blended normal below - the rim's exact limb response at s = 1, so
	// cap and cone meet without a seam BY CONSTRUCTION (the loft's own shared-formula
	// argument, applied across the junction).
	// Slightly soft at the centre, the rim's full density at the edge; the FILL term
	// lifts all of it with the opacity, and at the top of the slider the base is what
	// actually hides the hull - near the axis the cone's own sheet is edge-on
	// everywhere and the cap is the face you are looking at. At Size z = 0 the cone
	// collapses and the cap IS the effect: the Blue Angels' flat collar disc.
	// Behind the per-cone BASE FILL pill (his ask, same day): off = the open loft.
	if (K.baseOn) {
		const int    irRim = NR - 1;
		const double xxR   = xx[irRim];
		// The rim's meridian normal components - the same stencil the loft used there.
		const double dxR  = xx[NR] - xx[irRim - 1], drR = rr[NR] - rr[irRim - 1];
		const double dlR  = sqrt(dxR * dxR + drR * drR);
		const double nAxR = (dlR > 1e-9) ? (-drR / dlR) : 0.0;
		const double nRaR = (dlR > 1e-9) ? ( dxR / dlR) : 1.0;

		// THE BASE FILL OFFSET (2026-09-04, Buck Rogers's ask, promised on the
		// thread). The cap's CENTRE slides along the axis by offset x the cone's own
		// axial depth, the rings between interpolating linearly in s - the rim ring
		// (s = 1) never moves, so the seam-free junction with the loft survives BY
		// CONSTRUCTION. At -1 the centre lands exactly on the apex and the cap is an
		// inner second face of the cone; at 0 every term below collapses to the flat
		// disc BIT FOR BIT; +0.5 bulges it outward half the cone's depth (capped
		// there by his call - "maybe we cap the offset at +0.5"). Because the scale
		// is the cone's own reach, Size z 0 (the flat collar) has no depth to offset
		// and the knob is correctly inert there - a second face of a zero-depth cone
		// IS the disc.
		// The displaced cap is a shallow cone, so its face takes its OWN meridian
		// normal (the rim stencil's convention, run on the cap's centre->rim run) -
		// with the flat disc's axial normal a pushed-in cap would keep answering the
		// limb response as a flat sheet. At offset 0 the run is purely radial and
		// nCap IS downG - the identity case needs no branch.
		const float  kBOfs = clampf(K.baseOfs, -1.0f, 0.5f);
		const double dxCap = -(double)kBOfs * xxR;          // centre -> rim, axial
		const double drCap = rr[irRim];                     // centre -> rim, radial
		const double dlCap = sqrt(dxCap * dxCap + drCap * drCap);
		const double nAxC  = (dlCap > 1e-9) ? (drCap / dlCap) : 1.0;
		const double nRaC  = (dlCap > 1e-9) ? (-dxCap / dlCap) : 0.0;

		const float CS[VAP_NCAP + 1] = { 0.0f, 0.50f, 0.85f, 1.0f };
		for (int ir = 0; ir <= VAP_NCAP; ir++) {
			const int   cur = ir & 1;
			const float s   = CS[ir];
			const double capAx = xxR * (1.0 + (double)kBOfs * (double)(1.0f - s));
			for (int ib = 0; ib <= NA; ib++) {
				const int   bb = ib % NA;
				const float th = (float)bb / (float)NA * 6.2831853f;
				const float ct = cosf(th), st = sinf(th);
				const float boil = 1.0f
				                 + 0.030f * sinf(th * 3.0f + t2 * 0.9f)
				                 + 0.018f * sinf(th * 7.0f - t2 * 1.5f);
				const VECTOR3 gp = apexG + downG * capAx
				                 + (e1 * ((double)ct * rrX[irRim] * (double)s)
				                  + e2 * ((double)st * rrY[irRim] * (double)s)) * (double)boil;
				double zz;
				pk[cur][ib] = ProjPx(cc, gp, viewW, viewH, px[cur][ib], py[cur][ib], zz);
				if (!pk[cur][ib]) { pc[cur][ib] = 0; pd[cur][ib] = 0.0f; continue; }
				pd[cur][ib] = (float)length(gp - cc.pos);

				// The cap's own meridian normal at the centre (axial when the cap is
				// flat - see the offset block above), blending into the cone rim's
				// meridian normal at the edge - which makes the s = 1 ring's alpha
				// formula IDENTICAL to the cone rim's, and the junction cannot show.
				const VECTOR3 nrad = unit(e1 * ((double)ct * RmaxY) + e2 * ((double)st * RmaxX));
				const VECTOR3 nRim = unit(downG * nAxR + nrad * nRaR);
				const VECTOR3 nCap = unit(downG * nAxC + nrad * nRaC);
				const VECTOR3 nG   = unit(nCap * (double)(1.0f - s) + nRim * (double)s);
				const VECTOR3 vd = unit(gp - cc.pos);
				const float   fc = (float)fabs(dotp(nG, vd));
				const float   thick = 1.0f / (fc > 0.12f ? fc : 0.12f);

				// Cap density: 0.75 at the centre -> the rim's 1.0 at the edge, lifted
				// by the fill exactly as the sheet's own profile is.
				const float aaC = (0.75f + 0.25f * s) * (1.0f - fill) + fill;
				const float sF  = strk[ib];
				const float aF  = 1.0f - expf(-tauK * aaC * thick * (1.0f + 0.70f * sF));

				float shade = 0.55f;
				if (haveSun) {
					const VECTOR3 sd = unit(sunG - gp);
					shade = 0.30f + 0.70f * (0.5f + 0.5f * (float)dotp(nG, sd));
				}
				shade *= (0.10f + 0.90f * dayF);

				const float w  = (sF > 1.0f) ? 1.0f : sF;
				const int r8 = (int)((vRc + (kRc - vRc) * w) * shade + 0.5f);
				const int g8 = (int)((vGc + (kGc - vGc) * w) * shade + 0.5f);
				const int b8 = (int)((vBc + (kBc - vBc) * w) * shade + 0.5f);
				pc[cur][ib] = VCol(r8, g8, b8, (int)(aF * 255.0f + 0.5f));
			}
			if (ir == 0) continue;                   // centre ring: nothing to close yet
			const int prv = (ir - 1) & 1;
			for (int ib = 0; ib < NA; ib++) {
				const int ib2 = ib + 1;
				if (!pk[prv][ib] || !pk[prv][ib2] || !pk[cur][ib] || !pk[cur][ib2]) continue;
				emit3(prv, ib, cur, ib, cur, ib2);
				emit3(prv, ib, cur, ib2, prv, ib2);
			}
		}
	}

	};   // BuildCone

	// The two cones: identical knobs, separate numbers (his spec: "completely
	// separate tuning. Even the colors"). Cone 2 ships at opacity 0, so it exists
	// only where a hull has been given one; per-cone gates come from UpdateVapour
	// (two Mach windows, one air).
	const VapKnobs k1 = { g_fx.vapStrength,  g_fx.vapSize,  g_fx.vapSizeY,  g_fx.vapSizeZ,
	                      g_fx.vapStreaks,   g_fx.vapStreakChurn,  g_fx.vapFlickHz,
	                      g_fx.vapPos,       g_fx.vapPosX,  g_fx.vapPosY,
	                      g_fx.vapPitch,     g_fx.vapYaw,
	                      g_fx.vapColour,    g_fx.vapStreakCol,
	                      g_fx.vapBaseOn,    g_fx.vapBaseOfs };
	const VapKnobs k2 = { g_fx.vapStrength2, g_fx.vapSize2, g_fx.vapSizeY2, g_fx.vapSizeZ2,
	                      g_fx.vapStreaks2,  g_fx.vapStreakChurn2, g_fx.vapFlickHz2,
	                      g_fx.vapPos2,      g_fx.vapPosX2, g_fx.vapPosY2,
	                      g_fx.vapPitch2,    g_fx.vapYaw2,
	                      g_fx.vapColour2,   g_fx.vapStreakCol2,
	                      g_fx.vapBaseOn2,   g_fx.vapBaseOfs2 };
	BuildCone(k1, s_vap.fGate1);
	BuildCone(k2, s_vap.fGate2);

	g_fx.vapVis = visMax;
	if (vapVtxN == 0 && !g_fx.vapWhy[0])
		strcpy_s(g_fx.vapWhy, sizeof(g_fx.vapWhy), "faint");

	// Invariant 3: the client's D3D9Triangle::Update Locks with D3DLOCK_DISCARD and
	// its Draw always draws the CREATION count, so the tail past what we filled must
	// be zeroed or it is whatever VRAM the discard pool recycled - the green flashes.
	for (int k = vapVtxN; k < VAP_MAX_TRI * 3; k++) {
		vapVtx[k].x = 0.0f; vapVtx[k].y = 0.0f; vapVtx[k].c = 0; vapDepth[k] = 0.0f;
	}

	vapActive = (vapVtxN > 0);
}
