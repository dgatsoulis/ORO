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
		float   fGate;
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
	if (!live)                       { strcpy_s(g_fx.vapWhy, sizeof(g_fx.vapWhy), "off");   return; }
	if (g_fx.vapStrength <= 0.001f)  { strcpy_s(g_fx.vapWhy, sizeof(g_fx.vapWhy), "str 0"); return; }
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
	// TEST bypasses both, and says so in the readout rather than pretending the
	// conditions are met.
	double mUse = mach;
	float  fGate = 1.0f;

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
		double mLo = (double)g_fx.vapMachMin, mHi = (double)g_fx.vapMachMax;
		if (mHi < mLo + 0.02) mHi = mLo + 0.02;
		const double win = mHi - mLo;
		const float fM = ramp(mach, mLo,       mLo + win * VAP_RAMP_IN)
		               * (1.0f - ramp(mach, mHi - win * VAP_RAMP_OUT, mHi));
		if (fM <= 0.001f) {
			strcpy_s(g_fx.vapWhy, sizeof(g_fx.vapWhy), mach < mHi ? "subsonic" : "past band");
			return;
		}
		fGate = fAir * fM;
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
	s_vap.size = size;   s_vap.mUse = mUse;   s_vap.fGate = fGate;
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
	float         fGate = s_vap.fGate;              // the near fade below still scales it
	// ⚠️ RENDER-EPOCH ANCHOR (invariant 21a). Cg was sampled at pre-step in the
	// BARYCENTRIC frame and this build runs a step later, by which time Earth has moved
	// ~500 m at 60 fps. Pairing the render camera with a pre-step anchor put the cone
	// several hundred metres off the hull and made it jitter with frame pacing.
	// fwdG / downG are DIRECTIONS - a translation leaves them alone.
	const VECTOR3 Cg = s_vap.Cg + RenderEpochShift(s_vap.hV, s_vap.Cg);
	const VECTOR3 fwdG = s_vap.fwdG, downG = s_vap.downG;
	const MATRIX3 Rv = s_vap.Rv;
	const VECTOR3 sunG = s_vap.sunG;
	const bool    haveSun = s_vap.haveSun;
	float         dayF = s_vap.dayF;                // TEST lifts it below

	// --- THE FLICKER: one number, TWO consequences ----------------------------
	// Three octaves of REAL time (invariant 4) at the user's rate. The same value drives
	// opacity below AND the size right here, because they are one event: a stronger
	// condensation is denser and bigger at the same instant. Splitting them into two
	// clocks would give a shroud that grows while thinning, which is the one combination
	// nothing in nature does - and he specified the coupling himself ("a small size
	// variation INCLUDED in the flicker").
	//
	// ⚠️ IT USED TO HAVE A THIRD CONSEQUENCE AND HE CUT IT. Round 2 also deformed the
	// SURFACE - a roving mottle plus a meridian boil - reasoning that a global pulse
	// reads as a brightness knob rather than as material in motion. That reasoning was
	// not wrong about brightness knobs; it was wrong about this shape. His verdict: *"I
	// don't like the distortion of the disk... we keep the disk as it was but we make
	// small changes in its size and opacity."* THE LESSON: a clean analytic surface is
	// part of what makes this effect read as a shock front rather than as smoke, and
	// liveliness bought by roughening it costs more than it buys. Scale and opacity are
	// the honest degrees of freedom here.
	//
	// The octave ratios (x1, x1.9, x2.8) are deliberately tight rather than the usual
	// doubling: at the top of the 8 Hz slider the fastest term is ~22 Hz, which is still
	// resolved at ordinary frame rates. Spread them further and the maximum setting
	// aliases into a strobe instead of getting faster.
	const float tF   = animT;
	const float fHz  = clampf(g_fx.vapFlickHz, 0.0f, 8.0f) * 6.2831853f;   // -> rad/s
	const float fk = 0.55f * sinf(tF * fHz)
	               + 0.30f * sinf(tF * fHz * 1.9f + 1.3f)
	               + 0.15f * sinf(tF * fHz * 2.8f + 2.7f);       // ~ -1 .. +1
	const float flick = 1.0f + VAP_FLICK * fk;                   // 0.68 .. 1.32

	// --- the Mach angle, and therefore the shape ------------------------------
	// mu = asin(1/M): 90 deg at M = 1 (a flat collar), tightening as the vehicle
	// outruns its own pressure waves. THIS is why the length is not a slider.
	const double mEff = (mUse > 1.02) ? mUse : 1.02;
	const double mu   = clampd(asin(1.0 / mEff), VAP_MU_MIN, VAP_MU_MAX);

	// The size breathes with the flicker at a fraction of its amplitude. Scaling the
	// RADIUS is enough to scale the whole cone, because the axial reach is derived from
	// it below - so the Mach-angle shape is preserved exactly and only the scale moves.
	const double Rmax = size * (double)clampf(g_fx.vapSize, 0.0f, 3.0f)
	                  * (double)(1.0f + VAP_FLICK_SZ * VAP_FLICK * fk);
	if (Rmax < 0.5) { strcpy_s(g_fx.vapWhy, sizeof(g_fx.vapWhy), "size 0"); return; }
	const double Lax  = Rmax / tan(mu);             // axial reach, apex -> rim

	// The apex station along the flow axis. Positive = upstream, ahead of the hull's
	// centre. A per-hull fact (where the flow first goes supersonic depends entirely
	// on the shape of the nose), so it stays a knob - the shell-standoff lesson.
	const VECTOR3 apexG = Cg + fwdG * ((double)clampf(g_fx.vapPos, -2.0f, 2.0f) * size);

	// --- near fade ------------------------------------------------------------
	// You cannot see a cone you are inside. Fading before that case arises is
	// cheaper and more honest than defining what an inside-out surface of
	// revolution should look like.
	{
		const double dCam = length(Cg - cc.pos);
		const float  fNear = ramp(dCam, Rmax * VAP_NEAR_OUT, Rmax * VAP_NEAR_IN);
		if (fNear <= 0.001f) { strcpy_s(g_fx.vapWhy, sizeof(g_fx.vapWhy), "too close"); return; }
		fGate *= fNear;
	}

	// --- the light ------------------------------------------------------------
	// A cloud has no light of its own; it shows because the sun is on it. Two terms:
	// a per-vertex lambert that gives the shroud its ROUNDNESS (without it a
	// surface of revolution reads as a flat ring), and a day factor, because on the
	// night side there is nothing illuminating it and a bright white cone hanging off
	// a darkened ship would be the most obvious lie in the addon.
	// (sunG / haveSun / dayF are gathered on the main thread - see UpdateVapour.)
	// TEST keeps it lit: "I turned it on at night and saw nothing" is the same
	// silent failure the god rays' backwards Threshold produced (invariant 24d).
	if (g_fx.vapTest && dayF < 0.85f) dayF = 0.85f;

	const float strK = clampf(g_fx.vapStrength, 0.0f, 2.0f);
	const float tauK = VAP_TAU * strK * fGate * flick;   // the SAME flick that sized it

	// The readout reports the GATE, deliberately not the flickering value: he is reading
	// it to know where he is in the transonic window, and a number jittering 30% twice a
	// second is unreadable and says nothing he can act on.
	g_fx.vapVis = fGate;
	if (tauK <= 0.002f) {
		if (!g_fx.vapWhy[0]) strcpy_s(g_fx.vapWhy, sizeof(g_fx.vapWhy), "faint");
		return;
	}

	// --- the loft -------------------------------------------------------------
	// Rings from the apex (ring 0, a point) out to the rim (ring VAP_NR - 1), plus
	// one FEATHER ring at alpha 0 so the outer edge dissolves instead of ending on a
	// hard circle. Radius runs as t^0.70 against a linear axial march, which bells
	// the profile outward near the apex the way a real collar bulges - a straight
	// cone reads as a paper party hat.
	const int NA = VAP_NA, NR = VAP_NR;
	const float t2 = animT;                          // REAL time (invariant 4): the
	                                                 // shimmer must not freeze at 10x

	// Per-ring geometry, computed once and shared by both rings of every quad band -
	// which is also what guarantees no seam can open at a joint (invariant 23d's
	// safety argument: a pure function of the ring/segment index cannot disagree
	// with itself across a shared edge).
	double rr[VAP_NR + 1], xx[VAP_NR + 1];
	float  aa[VAP_NR + 1];
	for (int ir = 0; ir <= NR; ir++) {
		const float t = (float)ir / (float)(NR - 1);      // 1.0 AT THE RIM, > 1 = feather
		const float tc = clampf(t, 0.0f, 1.0f);
		rr[ir] = Rmax * (double)powf(tc, 0.70f) * (ir >= NR ? 1.10 : 1.0);
		xx[ir] = Lax  * (double)tc              * (ir >= NR ? 1.10 : 1.0);
		// Density along the meridian: nothing at the apex, densest at the rim - which
		// is the compression edge and is what the photographs show as a bright ring
		// around a translucent middle.
		aa[ir] = (ir >= NR) ? 0.0f : powf(tc, 1.30f);
	}

	// Radial basis perpendicular to the flow axis, ANCHORED TO THE VESSEL rather than to
	// global space - the aurora's discipline, where the ring's basis is tied to the
	// planet's prime meridian so the curtain co-rotates with the world instead of with
	// the coordinate system.
	// ⚠️ IT WAS ANCHORED TO GLOBAL +Y FOR ONE ROUND, and that is a latent artefact: a
	// vessel flying near the global Y axis makes the projection tiny, so the normalised
	// basis swings wildly for a small attitude change and the theta-keyed boil pattern
	// SPINS around the cone. Amplitude is only +-3% so it would have been a faint
	// mystery rather than an obvious bug, which is exactly the kind that survives for
	// months. Anchored to the hull, the basis is degenerate only in pure 90-degree
	// sideslip (flow exactly along the vessel's own X), where it falls back to vessel Y -
	// and it also puts the boil pattern in the airframe's frame, which is where the flow
	// structure it stands for actually lives.
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

	// Scratch for two rings at a time (the loft only ever needs the previous one).
	float px[2][VAP_NA + 1], py[2][VAP_NA + 1], pd[2][VAP_NA + 1];
	DWORD pc[2][VAP_NA + 1];
	bool  pk[2][VAP_NA + 1];

	auto emit3 = [&](int ra, int ia, int rb, int ib, int rc2, int ic) {
		if (vapVtxN + 3 > VAP_MAX_TRI * 3) return;
		vapVtx[vapVtxN].x = px[ra][ia]; vapVtx[vapVtxN].y = py[ra][ia];
		vapVtx[vapVtxN].c = pc[ra][ia]; vapDepth[vapVtxN] = pd[ra][ia]; vapVtxN++;
		vapVtx[vapVtxN].x = px[rb][ib]; vapVtx[vapVtxN].y = py[rb][ib];
		vapVtx[vapVtxN].c = pc[rb][ib]; vapDepth[vapVtxN] = pd[rb][ib]; vapVtxN++;
		vapVtx[vapVtxN].x = px[rc2][ic]; vapVtx[vapVtxN].y = py[rc2][ic];
		vapVtx[vapVtxN].c = pc[rc2][ic]; vapDepth[vapVtxN] = pd[rc2][ic]; vapVtxN++;
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
			const VECTOR3 rad = e1 * (double)ct + e2 * (double)st;

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

			const VECTOR3 gp = apexG + downG * xx[ir] + rad * (rr[ir] * (double)boil);

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
			const VECTOR3 nG = unit(downG * nAx + rad * nRa);
			const VECTOR3 vd = unit(gp - cc.pos);
			const float   fc = (float)fabs(dotp(nG, vd));
			const float   thick = 1.0f / (fc > 0.12f ? fc : 0.12f);       // 1 .. 8.3

			// Beer-Lambert: alpha = 1 - exp(-tau). Saturates on its own, so no hard
			// clamp is needed and none of the Gouraud gradient gets flattened into a
			// cutout on the way (G9's law, restated for opacity).
			// (Round 2's roving MOTTLE multiplied in here and was cut with the meridian
			//  boil - see the flicker block above. Opacity varies globally now, not
			//  across the sheet.)
			const float aF = 1.0f - expf(-tauK * aa[ir] * thick);

			// Sun shading gives the shroud its roundness. Never fully black on the
			// dark side - a real cloud still catches skylight and the planet below.
			float shade = 0.55f;
			if (haveSun) {
				const VECTOR3 sd = unit(sunG - gp);
				shade = 0.30f + 0.70f * (0.5f + 0.5f * (float)dotp(nG, sd));
			}
			shade *= (0.10f + 0.90f * dayF);

			// Condensed water is white. The faint cool cast is the sky it is sitting
			// in, not a stylistic choice - and it is the reason the cone reads as
			// vapour rather than as smoke.
			const int r8 = (int)(232.0f * shade + 0.5f);
			const int g8 = (int)(240.0f * shade + 0.5f);
			const int b8 = (int)(250.0f * shade + 0.5f);
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

	// Invariant 3: the client's D3D9Triangle::Update Locks with D3DLOCK_DISCARD and
	// its Draw always draws the CREATION count, so the tail past what we filled must
	// be zeroed or it is whatever VRAM the discard pool recycled - the green flashes.
	for (int k = vapVtxN; k < VAP_MAX_TRI * 3; k++) {
		vapVtx[k].x = 0.0f; vapVtx[k].y = 0.0f; vapVtx[k].c = 0; vapDepth[k] = 0.0f;
	}

	vapActive = (vapVtxN > 0);
}
