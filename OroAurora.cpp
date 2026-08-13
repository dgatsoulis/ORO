// ==============================================================
// OroAurora.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - AURORA: the auroral curtains
// ----------------------------------------------------------------------------
// DOMAIN: external always; INTERNAL too once patch (g) depth-clipping is live (depthClipOK).
// This is additive screen-space GEOMETRY, so without depth it would paint over the cockpit
// frame - which is why it was external-only on first build (the shimmer's reasoning). Patch
// (g) (2026-08-05) gives the client's Sketchpad a per-fragment depth clip against the scene
// depth buffer, so each vertex now carries its camera depth (aurDepth) and the curtains sit
// behind the glass and hull per pixel - through the windows in the VC, and occluding the
// ship exactly in external (retiring the bounding-sphere fallback). On a client without the
// patch, or with SunGlare off, depthClipOK is false and it degrades to external + the sphere.
//
// "A curtain IS a ribbon", so the entire round-5 plasma machinery transfers wholesale
// (GetCam/ProjPx per-vertex projection, mitred Gouraud ribbons, additive Sketchpad
// draw). It needs NO client patch - the poly draws additively where patch (d) is live
// and alpha-blends otherwise, exactly like the plasma.
//
// THE CURTAIN. An auroral display is a drape hanging in the sky around the magnetic
// pole. Orbiter models no magnetic field, so we hang it on the GEOGRAPHIC pole (a fair
// approximation for immersion - the offset is ~11 deg on Earth) as a ring at a fixed
// colatitude, extruded straight up from ~95 km to a few hundred km. We build the ring
// directly in 3D around the planet's spin axis (no lat/long convention to get wrong -
// a full ring has no origin), colour-ramp it by ALTITUDE (violet/pink nitrogen base ->
// oxygen green body -> oxygen red top - real physics, and the same "green decides the
// hue" discipline the plasma palette runs on), fold it with a few sines (draperies) and
// striate it with a higher frequency (rays).
//
// OCCLUSION is geometric (no depth buffer - invariant 11), and there are TWO occluders,
// both cheap ray-sphere tests: the PLANET (its bulk hides the far side of the oval and
// everything past the limb) and the CAMERA-TARGET VESSEL (so the ship you are looking
// at hides the curtains behind it, instead of being painted over - the fix for the
// external "aurora in front of the vessel" report). Both fade alpha rather than hard-
// cut, so a quad straddling an edge gets a soft Gouraud gradient for free. The vessel
// test is a bounding SPHERE - it slightly overshoots a thin airframe, invariant 11's
// known trade; the exact hull needs the depth buffer (patch g).
//
// MAIN THREAD ONLY (clbkPreStep -> UpdateAurora): oapi camera/planet queries, none of
// them legal from the render path (invariant 1). The render callback only pushes aurVtx
// into the poly and draws it (DrawAuroraPoly, external branch).
// ============================================================================

#include "OroModule.h"
#include "OroState.h"
#include <math.h>

namespace {

	// --- the oval -----------------------------------------------------------
	// The colatitude band the Reach knob spans, and the altitude band the Base/Top knobs
	// span, are NO LONGER CONSTANTS: they are properties of the WORLD, read from
	// Config\ORO\bodies\<name>.cfg into g_fx.aurColatMin/MaxDeg and aurBase/TopMin/MaxKm.
	// Earth's shipped file carries the numbers this file used to hard-code (colatitude
	// 12-50 deg, i.e. lat 78 down to 40, low enough for the ISS at 56 deg inclination to fly
	// under a storm oval; base 40-160 km, top 180-400 km). Jupiter's are an order larger,
	// which is exactly why they could not stay here.
	const double AUR_TEST_COLAT =  6.0 * RAD;   // the TEST ring, centred on the sub-camera
	                                            //   point (~670 km on Earth) - close enough to
	                                            //   loom on the horizon wherever you are
	const double AUR_ARC_SEP    =  3.5 * RAD;   // each extra ribbon's added colatitude
	const int    AUR_SEG        = 96;           // segments around the ring
	const int    AUR_VB         = 8;            // vertical bands (rows), so AUR_VB+1 knots high
	const float  AUR_ALPHA      = 190.0f;       // peak additive alpha at Activity 1 - kept
	                                            //   MODERATE so it reads as a translucent glow
	                                            //   under EITHER blend mode (the padAdditive gate
	                                            //   is shared with the plasma, so while the
	                                            //   gcAPIVer-0 bug stands this alpha-blends too)
	const double AUR_NEAR0      =  2.0e3;       // near fade: curtains within 2 km fade out, so
	const double AUR_NEAR1      = 18.0e3;       //   flying THROUGH one does not smear the near plane
	const double AUR_VOCC_IN    = 0.85;         // vessel-sphere occlusion: fully hidden inside
	const double AUR_VOCC_OUT   = 1.08;         //   0.85 x GetSize, fully visible past 1.08 - a
	                                            //   soft band so the ship's edge is not a hard disc

	inline float  clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
	inline double clampd(double x, double a, double b) { return x < a ? a : (x > b ? b : x); }
	inline float  sstepf(float t) { t = clampf(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
	inline float  hashf(float s) { float t = sinf(s * 12.9898f) * 43758.547f; return t - floorf(t); }

	// Projection: the reentry/shimmer projector, verbatim. Orbiter's camera looks along
	// +z and oapiCameraAperture() is the VERTICAL semi-aperture. Returns false behind cam.
	struct CamCtx { VECTOR3 pos; MATRIX3 rot; double tanAp; };
	void GetCam(CamCtx& c)
	{
		oapiCameraGlobalPos(&c.pos);
		oapiCameraRotationMatrix(&c.rot);
		c.tanAp = tan(oapiCameraAperture());
	}
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

	// Pack 0xAABBGGRR (invariant 5), clamped.
	inline DWORD ACol(int r, int g, int b, int a)
	{
		if (a < 0) a = 0; else if (a > 255) a = 255;
		if (r < 0) r = 0; else if (r > 255) r = 255;
		if (g < 0) g = 0; else if (g > 255) g = 255;
		if (b < 0) b = 0; else if (b > 255) b = 255;
		return ((DWORD)a << 24) | ((DWORD)b << 16) | ((DWORD)g << 8) | (DWORD)r;
	}

	inline void UnpackCR(DWORD c, int& r, int& g, int& b)  // COLORREF 0x00BBGGRR
	{
		r = (int)(c & 0xFF); g = (int)((c >> 8) & 0xFF); b = (int)((c >> 16) & 0xFF);
	}

	// Alpha PROFILE by altitude, bottom (v=0) -> top (v=1): a bright thin lower border
	// fading up through the body to nothing at the top. THIS is what gives the curtain its
	// shape and sharp bottom edge; it is scaled by Activity/rays/night/occlusion outside.
	// Split out from the colour so the two user colours can drive the RGB without disturbing
	// the profile the whole look rests on.
	float AuroraAlphaProfile(float v)
	{
		struct K { float v; float a; };
		static const K key[6] = {
			{ 0.00f, 0.00f },   // fades in from nothing
			{ 0.05f, 1.00f },   // sharp lower border
			{ 0.14f, 0.92f },   // just above the border
			{ 0.45f, 0.60f },   // the main body
			{ 0.70f, 0.30f },   // thinning
			{ 1.00f, 0.00f },   // diffuse top, fades out
		};
		v = clampf(v, 0.0f, 1.0f);
		int k = 0;
		while (k < 4 && v > key[k + 1].v) k++;
		const float u = (v - key[k].v) / (key[k + 1].v - key[k].v);
		return key[k].a + (key[k + 1].a - key[k].a) * u;
	}

	// Colour by ALTITUDE from the user's three colours (dialog swatches), because that is
	// what decides a real aurora's colour: how deep the particles get sets which species
	// emits and which of its lines. Earth reads bottom-to-top as nitrogen VIOLET -> oxygen
	// GREEN -> oxygen RED, and no two-colour scheme can say that - its two edges are
	// different colours - which is exactly why there are three.
	//
	// The BASE band is deliberately thin: a sharp lower border is the most recognisable
	// feature of a real curtain, and it lines up with the alpha profile's own spike at
	// v=0.05. The TOP gets the whole upper half to diffuse through.
	void AuroraRGB(float v, int& r, int& g, int& b)
	{
		int rB, gB, bB, rM, gM, bM, rT, gT, bT;
		UnpackCR(g_fx.auroraColBase, rB, gB, bB);
		UnpackCR(g_fx.auroraColBody, rM, gM, bM);
		UnpackCR(g_fx.auroraColTop,  rT, gT, bT);
		v = clampf(v, 0.0f, 1.0f);
		if (v < 0.12f) {                                   // lower border -> body
			const float u = sstepf(v / 0.12f);
			r = rB + (int)((rM - rB) * u + 0.5f);
			g = gB + (int)((gM - gB) * u + 0.5f);
			b = bB + (int)((bM - bB) * u + 0.5f);
		} else if (v > 0.55f) {                            // body -> diffuse top
			const float u = sstepf((v - 0.55f) / 0.45f);
			r = rM + (int)((rT - rM) * u + 0.5f);
			g = gM + (int)((gT - gM) * u + 0.5f);
			b = bM + (int)((bT - bM) * u + 0.5f);
		} else {
			r = rM; g = gM; b = bM;
		}
	}

	// The star, for the day/night fade. Same construction as the eclipse's FindStar:
	// prefer a real OBJTP_STAR, fall back to gbody 0 (the client's own assumption).
	OBJHANDLE FindStar()
	{
		const DWORD n = oapiGetGbodyCount();
		for (DWORD i = 0; i < n; i++) {
			OBJHANDLE h = oapiGetGbodyByIndex((int)i);
			if (h && oapiGetObjectType(h) == OBJTP_STAR) return h;
		}
		return oapiGetGbodyByIndex(0);
	}

	// The planet the curtains are drawn at: the body the camera is "at" (the eclipse's
	// primary logic) if it has an atmosphere, else the nearest atmospheric body measured
	// in its own radii. No atmosphere -> no aurora (nothing to glow).
	// Can this world glow? An atmosphere is the obvious test, but it is the WRONG one for
	// the moons: Ganymede is the only moon in the solar system with its own magnetic field,
	// so it has genuine polar ovals, and Europa's oxygen glow is part of the evidence for
	// its subsurface ocean - yet neither carries an atmosphere in Orbiter's configs. A
	// shipped aurora FILE is itself the statement that a world glows, so it also qualifies.
	bool BodyGlows(OBJHANDLE h)
	{
		if (!h || oapiGetObjectType(h) == OBJTP_STAR) return false;
		if (oapiPlanetHasAtmosphere(h)) return true;
		char nm[32];
		oapiGetObjectName(h, nm, sizeof(nm));
		return OroSettings_BodyHasFile(nm);
	}

	OBJHANDLE FindAuroraBody()
	{
		OBJHANDLE hT = oapiCameraTarget();
		if (hT) {
			OBJHANDLE h = NULL;
			if (oapiGetObjectType(hT) == OBJTP_VESSEL) {
				VESSEL* v = oapiGetVesselInterface(hT);
				if (v) h = v->GetSurfaceRef();
			} else h = hT;
			if (BodyGlows(h)) return h;
		}
		VECTOR3 cpos; oapiCameraGlobalPos(&cpos);
		OBJHANDLE best = NULL; double bestR = 1e30;
		const DWORD n = oapiGetGbodyCount();
		for (DWORD i = 0; i < n; i++) {
			OBJHANDLE h = oapiGetGbodyByIndex((int)i);
			if (!BodyGlows(h)) continue;
			const double R = oapiGetSize(h);
			if (R < 1.0) continue;
			VECTOR3 p; oapiGetGlobalPos(h, &p);
			const double rel = length(p - cpos) / R;
			if (rel < bestR) { bestR = rel; best = h; }
		}
		return best;
	}

	// Is the segment camera->P blocked by the planet (sphere O,R) before reaching P?
	// Standard line-sphere: the near intersection must be in front of the camera and
	// nearer than P. This is the whole occlusion model - the far side of the oval and
	// everything behind the limb drops out here.
	bool Occluded(const VECTOR3& cam, const VECTOR3& P, const VECTOR3& O, double R)
	{
		const VECTOR3 d = P - cam;
		const double L = length(d);
		if (L < 1.0) return false;
		const VECTOR3 u = d / L;
		const VECTOR3 m = O - cam;
		const double tca = dotp(m, u);
		const double d2  = dotp(m, m) - tca * tca;
		const double R2  = R * R;
		if (d2 > R2) return false;                 // ray misses the planet entirely
		const double thc = sqrt(R2 - d2);
		const double t0  = tca - thc;              // near hit
		return (t0 > 1.0 && t0 < L - 1.0);         // in front of us and before P
	}

	// Soft visibility of an aurora vertex P past the CAMERA-TARGET VESSEL (sphere Vc,Vr):
	// 1 = clear, 0 = fully behind the ship, with a soft band across the silhouette so the
	// bounding sphere does not read as a hard disc. Only bites when the sphere is genuinely
	// between the camera and P (tca in (0,L)) - so it no-ops in the VC, where the camera is
	// inside the sphere (tca < 0), which is one more reason the effect is external-only.
	float VesselVis(const VECTOR3& cam, const VECTOR3& P, const VECTOR3& Vc, double Vr)
	{
		if (Vr <= 0.0) return 1.0f;
		const VECTOR3 d = P - cam;
		const double L = length(d);
		if (L < 1.0) return 1.0f;
		const VECTOR3 u = d / L;
		const VECTOR3 m = Vc - cam;
		const double tca = dotp(m, u);
		if (tca <= 0.0 || tca >= L) return 1.0f;   // ship not between the camera and P
		double s = dotp(m, m) - tca * tca;
		if (s < 0.0) s = 0.0;
		const double dperp = sqrt(s);
		const double a = Vr * AUR_VOCC_IN, b = Vr * AUR_VOCC_OUT;
		if (dperp <= a) return 0.0f;
		if (dperp >= b) return 1.0f;
		return sstepf((float)((dperp - a) / (b - a)));
	}

}  // namespace

// ----------------------------------------------------------------------------
// Per frame, MAIN thread. Builds the curtain triangles into aurVtx and sets
// aurActive; the render callback only pushes and draws (DrawAuroraPoly).
// ----------------------------------------------------------------------------
void OroModule::UpdateAurora()
{
	aurVtxN   = 0;
	aurActive = false;
	g_fx.auroraBody[0] = 0;

	// EXTERNAL always; INTERNAL only when patch (g) depth-clipping is live (depthClipOK) -
	// without real depth a screen-space curtain would paint the cockpit frame, so like the
	// shimmer it stays external. With depth it sits behind the glass and the VC is fair game.
	if (!g_fx.masterArmed || !g_fx.auroraEnabled) return;
	if (viewW == 0 || viewH == 0) return;

	// ---- IDENTIFY THE WORLD FIRST, BEFORE ANY DRAW GATE ----------------------
	// Which world we are at, and therefore which settings the DIALOG should be showing, is
	// a different question from whether we are currently drawing anything. Every gate below
	// (view, activity, opt-in) only decides the drawing - so they must come AFTER the load,
	// or the sliders latch: with the load behind the `activity <= 0` gate, arriving at a
	// world whose saved Activity was 0 could never load the settings that would raise it.
	OBJHANDLE hP = FindAuroraBody();
	if (!hP) { g_fx.auroraBody[0] = 0; return; }   // name cleared: the readout must not lie

	VECTOR3 O; oapiGetGlobalPos(hP, &O);
	const double R = oapiGetSize(hP);
	if (R < 1.0) { g_fx.auroraBody[0] = 0; return; }

	CamCtx cc; GetCam(cc);
	if (length(O - cc.pos) > 40.0 * R) {           // too far to matter - a sub-degree ring
		g_fx.auroraBody[0] = 0;
		return;
	}

	oapiGetObjectName(hP, g_fx.auroraBody, sizeof(g_fx.auroraBody));
	// Swap in THIS WORLD'S aurora - ranges, look sliders, colours, ribbons - the moment the
	// target body changes. Same pattern as the per-vessel-class load on a focus change, and
	// cheap for the same reason: the compare is a string already in memory and the read only
	// fires on an actual change. A world with no file gets the built-in DEFAULTS back.
	OroSettings_LoadBody(g_fx.auroraBody);

	// ---- from here on it is only about DRAWING -------------------------------
	if (!extGate && !(viewGate && depthClipOK)) return;

	// ACTIVITY IS THE OPT-IN. A world with no file loaded the defaults just above, and the
	// default activity is zero - so an unconfigured world is silent, turning this up at any
	// world gives it curtains, and saving makes that the world's aurora. There is no second
	// enable flag to disagree with it.
	const float act = clampf(g_fx.auroraActivity, 0.0f, 1.0f);
	if (act <= 0.001f) return;

	MATRIX3 Rp; oapiGetRotationMatrix(hP, &Rp);

	// The vessel to occlude behind: the one the camera is looking at (external), falling
	// back to the focus vessel. Its bounding sphere hides the curtains behind it.
	VECTOR3 Vc = { 0, 0, 0 }; double Vr = -1.0;
	{
		OBJHANDLE hv = oapiCameraTarget();
		if (!hv || oapiGetObjectType(hv) != OBJTP_VESSEL) hv = oapiGetFocusObject();
		if (hv && oapiGetObjectType(hv) == OBJTP_VESSEL) {
			oapiGetGlobalPos(hv, &Vc);
			Vr = oapiGetSize(hv);
		}
	}

	OBJHANDLE hSun = FindStar();
	VECTOR3 Sg = { 0, 0, 0 };
	if (hSun) oapiGetGlobalPos(hSun, &Sg);

	// Planet spin axis + prime-meridian direction, in global. The basis is tied to the
	// planet frame (Xaxis), so the ring co-rotates with the planet and the night fade
	// sweeps across it as it turns - exactly how a real oval behaves relative to the sun.
	// THE MAGNETIC AXIS. The ovals ring the MAGNETIC pole, not the geographic one, and on
	// Earth those are ~11 deg apart - so the tilt knobs are the physical case before they are
	// the exotic one. Built in the PLANET frame: start at the spin axis (0,1,0), swing it
	// tiltX toward the prime meridian and tiltY toward 90 deg east, then carry it to global
	// with Rp so the whole thing still co-rotates with the planet.
	// The SOUTH oval keeps using -Naxis, which makes the pair a proper DIPOLE: tilting the
	// north oval one way moves the south one exactly opposite, because the AXIS is what
	// tilted. The result stays a unit vector at any angle (sin^2 + cos^2 = 1), so no
	// renormalising and no degenerate case except tiltX = +-90 with tiltY = 0, where the
	// axis lands on Xaxis - which the basis construction below already falls back for.
	const double tX = clampd((double)g_fx.auroraTiltX, -90.0, 90.0) * RAD;
	const double tY = clampd((double)g_fx.auroraTiltY, -90.0, 90.0) * RAD;
	const VECTOR3 poleP = _V(sin(tX), cos(tX) * cos(tY), cos(tX) * sin(tY));
	const VECTOR3 Naxis = mul(Rp, poleP);
	const VECTOR3 Xaxis = mul(Rp, _V(1, 0, 0));

	const float t = animT;   // REAL-time clock (invariant 4): the curtains must not freeze
	                         // at 100x nor strobe - the dance is decorative, not physical.

	// Altitudes span the RANGES this world's own file gives (Config\ORO\bodies\<name>.cfg),
	// so the same 0..1 knob means 40-160 km at Earth and thousands of km at Jupiter.
	const double H0 = 1000.0 * ((double)g_fx.aurBaseMinKm
	                + ((double)g_fx.aurBaseMaxKm - g_fx.aurBaseMinKm) * clampd(g_fx.auroraBase,   0.0, 1.0));
	const double H1 = 1000.0 * ((double)g_fx.aurTopMinKm
	                + ((double)g_fx.aurTopMaxKm  - g_fx.aurTopMinKm)  * clampd(g_fx.auroraHeight, 0.0, 1.0));
	const float  foldK = clampf(g_fx.auroraFold, 0.0f, 1.0f);
	const float  rayK  = clampf(g_fx.auroraRays, 0.0f, 1.0f);
	const float  breakK = clampf(g_fx.auroraBreakup, 0.0f, 1.0f);

	// The oval centres. TEST: one ring on the SUB-CAMERA point (night off) so it looms
	// wherever you are. Normal: both spin poles - aurora borealis AND australis, each
	// self-culling by occlusion, so the far one costs nothing.
	VECTOR3 centres[2]; int nc = 0;
	if (g_fx.auroraTest) {
		VECTOR3 up = cc.pos - O; const double L = length(up);
		if (L < 1.0) return;
		centres[nc++] = up / L;
	} else {
		centres[nc++] =  Naxis;
		centres[nc++] = -Naxis;
	}
	// Reach drives how far equatorward the real ovals sit; the TEST ring keeps its own
	// small colatitude (it is centred on you, so "latitude" does not apply to it).
	// Colatitude spans this world's own range too - a gas giant's oval is far tighter
	// relative to its size than Earth's.
	const double reach = clampd(g_fx.auroraReach, 0.0, 1.0);
	const double baseColat = g_fx.auroraTest
	                       ? AUR_TEST_COLAT
	                       : RAD * ((double)g_fx.aurColatMinDeg
	                              + ((double)g_fx.aurColatMaxDeg - g_fx.aurColatMinDeg) * reach);

	// Each vertex carries its screen x,y, its CAMERA-SPACE depth az (into aurDepth, for the
	// patch-g depth clip) and its colour.
	auto emit3 = [&](float ax, float ay, float az, DWORD ac, float bx, float by, float bz, DWORD bc,
	                 float cx2, float cy2, float cz2, DWORD cc2) {
		if (aurVtxN + 3 > AUR_MAX_TRI * 3) return;
		aurVtx[aurVtxN].x = ax;  aurVtx[aurVtxN].y = ay;  aurVtx[aurVtxN].c = ac;  aurDepth[aurVtxN] = az;  aurVtxN++;
		aurVtx[aurVtxN].x = bx;  aurVtx[aurVtxN].y = by;  aurVtx[aurVtxN].c = bc;  aurDepth[aurVtxN] = bz;  aurVtxN++;
		aurVtx[aurVtxN].x = cx2; aurVtx[aurVtxN].y = cy2; aurVtx[aurVtxN].c = cc2; aurDepth[aurVtxN] = cz2; aurVtxN++;
	};

	for (int oc = 0; oc < nc; oc++) {
		const VECTOR3 C = centres[oc];
		// Orthonormal basis perpendicular to C, tied to the planet's prime meridian.
		VECTOR3 e1 = Xaxis - C * dotp(C, Xaxis);
		double e1l = length(e1);
		if (e1l < 1e-6) { e1 = _V(0, 0, 1) - C * dotp(C, _V(0, 0, 1)); e1l = length(e1); }
		e1 = e1 / e1l;
		const VECTOR3 e2 = crossp(C, e1);

		// Ribbon count is the user's live knob (1..6), and THICKNESS stacks 1..4 parallel
		// sheets across each ribbon. The two multiply into a flat list of "walls" so the
		// column builder below needs no extra nesting - w decomposes back into which ribbon
		// and which sheet within it.
		int nArcs = g_fx.auroraRibbons;
		if (nArcs < 1) nArcs = 1; else if (nArcs > 6) nArcs = 6;
		const float thickK  = clampf(g_fx.auroraThick, 0.0f, 1.0f);
		const int   nSheets = 1 + (int)(thickK * 3.0f + 0.5f);          // 1..4
		// How far apart the sheets sit, in COLATITUDE - so it scales with the planet for
		// free, exactly like the oval itself. 0.8 deg is ~89 km on Earth: far thicker than a
		// real curtain (which is a km or so) but a real one would be sub-pixel from orbit,
		// and thickness here is about reading as a volume rather than measuring one.
		const double sheetSpread = (double)thickK * 0.8 * RAD;
		// Per-sheet alpha falls as 1/sqrt(n), so a thick curtain IS brighter than a thin one
		// (it is more emitting gas) but 4 sheets give 2x, not 4x - the knob changes the
		// LOOK rather than doubling as a second brightness control.
		const float sheetAmp = 1.0f / sqrtf((float)nSheets);
		const int nWalls = nArcs * nSheets;
		for (int w = 0; w < nWalls; w++) {
			const int arc = w / nSheets;
			const int sh  = w % nSheets;
			// Sheets straddle the ribbon's own colatitude, so thickening grows outward from
			// where the single sheet was rather than shifting the whole oval.
			const double shOff = (nSheets <= 1) ? 0.0
			                   : (((double)sh / (double)(nSheets - 1)) - 0.5) * sheetSpread;
			const double arcColat = baseColat + arc * AUR_ARC_SEP + shOff;
			// arc 0 full; companions taper from 0.55 (== the shipped 2-ribbon look) with a
			// 0.30 floor, so adding ribbons reads as fainter nested curtains, not a wall.
			const float  arcAmp   = ((arc == 0) ? 1.0f
			                                    : clampf(0.55f - 0.05f * (arc - 1), 0.30f, 0.55f))
			                        * sheetAmp;
			const float  aph      = arc * 1.9f;                   // per-arc phase
			// per-pole+arc seed so each ring breaks into bands in DIFFERENT places. Keyed on
			// ARC, not w, so the sheets of one ribbon break in the SAME places - they are the
			// same curtain seen through its thickness, not independent curtains.
			const float  aseed    = hashf((float)oc * 4.7f + (float)arc * 2.3f + 1.3f) * 6.2831853f;

			// Previous column, carried so consecutive columns close into quads. The ring
			// wraps: column AUR_SEG re-evaluates theta=2pi == column 0, so it seals.
			float pX[AUR_VB + 1], pY[AUR_VB + 1], pZ[AUR_VB + 1], pA[AUR_VB + 1];
			DWORD pC[AUR_VB + 1]; bool pOK[AUR_VB + 1];

			for (int i = 0; i <= AUR_SEG; i++) {
				const double th = 6.28318530718 * (double)i / (double)AUR_SEG;

				// FOLD: wave the ring in and out (draperies). A sum of a few harmonics,
				// amplitude-normalised, growing with the Fold knob.
				const double fold = (double)foldK * baseColat * 0.35 *
					( sin(3.0 * th + aph * 1.3 + 0.10 * t)
					+ 0.5 * sin(7.0 * th - aph * 0.9 + 0.17 * t)
					+ 0.3 * sin(13.0 * th + 0.23 * t) ) / 1.8;
				const double colat = arcColat + fold;
				const double sc = sin(colat), cs = cos(colat);
				const VECTOR3 d = C * cs + (e1 * cos(th) + e2 * sin(th)) * sc;

				// RAYS: sharp vertical brightness striations (a whole-column term - a ray
				// runs top to bottom), gathered into active patches by a slow envelope, with
				// a faint shimmer. rayK=0 -> uniform; rayK=1 -> full striation.
				float ray = 0.55f + 0.45f * sinf((float)th * 38.0f + 0.7f * t + aph);
				ray = powf(clampf(ray, 0.0f, 1.0f), 1.7f);
				const float env = 0.60f + 0.40f * sinf((float)th * 4.0f - 0.13f * t + aph * 1.1f);
				const float shim = 0.85f + 0.15f * sinf((float)th * 17.0f + 3.1f * t + hashf((float)i) * 6.28f);
				const float rayMod = ((1.0f - rayK) + rayK * ray * env) * shim;

				// BREAKUP: open the ring into disconnected BANDS. A slow wander along theta at
				// incommensurate frequencies (so it never repeats) thresholded so stretches drop
				// to zero - the whole vertical curtain vanishes there, leaving separate segments
				// with soft ends. The threshold rises with breakK (wider/more gaps); the per-arc
				// seed makes each ring break elsewhere; the time drift migrates the gaps.
				float seg = 1.0f;
				if (breakK > 0.01f) {
					const float sv = sinf((float)th * 2.0f + aseed + t * 0.040f)
					               + 0.7f * sinf((float)th * 3.3f - aseed * 1.7f - t * 0.055f)
					               + 0.5f * sinf((float)th * 5.7f + aseed * 0.6f + t * 0.030f);
					const float band = 0.5f + 0.5f * (sv / 2.2f);   // ~0..1 along the ring
					seg = sstepf((band - breakK * 0.9f) / 0.15f);    // 0 in gaps, 1 in bands
				}

				// NIGHT: the aurora is drowned by daylight (additive over a bright sky is
				// lost anyway; this makes it explicit and fades cleanly across the
				// terminator). TEST forces full brightness.
				const VECTOR3 baseP = O + d * R;
				const double elev = dotp(d, unit(Sg - baseP));   // sun elevation at the point
				const float night = g_fx.auroraTest ? 1.0f
				                  : (1.0f - sstepf((float)((elev + 0.15) / 0.25)));

				float cX[AUR_VB + 1], cY[AUR_VB + 1], cZ[AUR_VB + 1], cA[AUR_VB + 1];
				DWORD cC[AUR_VB + 1]; bool cOK[AUR_VB + 1];

				for (int j = 0; j <= AUR_VB; j++) {
					const float v = (float)j / (float)AUR_VB;
					const double h = H0 + (H1 - H0) * (double)v;
					const VECTOR3 P = O + d * (R + h);
					double pz = 1.0;
					cOK[j] = ProjPx(cc, P, viewW, viewH, cX[j], cY[j], pz);
					// Depth for the patch-g clip = EUCLIDEAN distance from the camera, matching
					// the client's GBUF_DEPTH.a = length(posW) (NewMesh.hlsl NormalDepth_PS) -
					// NOT the camera-space z. (pz, the z, still drives the near fade below.)
					cZ[j] = (float)length(P - cc.pos);
					int r, g, b;
					AuroraRGB(v, r, g, b);
					const float aprof = AuroraAlphaProfile(v);
					float alpha = 0.0f;
					if (cOK[j]) {
						const float occ    = Occluded(cc.pos, P, O, R) ? 0.0f : 1.0f;
						// When depth-clipping is live the client hides the curtains behind the
						// ship per-pixel, so the coarse bounding-sphere fallback is off; without
						// it (external, no depth buffer) the sphere keeps the ship from being
						// painted over.
						const float vis    = depthClipOK ? 1.0f : VesselVis(cc.pos, P, Vc, Vr);
						const float nearFd = sstepf((float)((pz - AUR_NEAR0) / (AUR_NEAR1 - AUR_NEAR0)));
						alpha = aprof * AUR_ALPHA * act * arcAmp * rayMod * seg * night * occ * vis * nearFd;
					}
					cA[j] = alpha;
					cC[j] = ACol(r, g, b, (int)(alpha + 0.5f));
				}

				if (i > 0) {
					for (int j = 0; j < AUR_VB; j++) {
						if (!(pOK[j] && cOK[j] && cOK[j + 1] && pOK[j + 1])) continue;
						// Skip a quad only if every corner is dark (occluded / daylight /
						// dim ray gap). A quad with ONE bright corner still emits, so the
						// limb and the ray edges get a soft Gouraud gradient.
						float amax = pA[j];
						if (cA[j]     > amax) amax = cA[j];
						if (cA[j + 1] > amax) amax = cA[j + 1];
						if (pA[j + 1] > amax) amax = pA[j + 1];
						if (amax < 2.0f) continue;
						emit3(pX[j],     pY[j],     pZ[j],     pC[j],
						      cX[j],     cY[j],     cZ[j],     cC[j],
						      cX[j + 1], cY[j + 1], cZ[j + 1], cC[j + 1]);
						emit3(pX[j],     pY[j],     pZ[j],     pC[j],
						      cX[j + 1], cY[j + 1], cZ[j + 1], cC[j + 1],
						      pX[j + 1], pY[j + 1], pZ[j + 1], pC[j + 1]);
					}
				}
				for (int j = 0; j <= AUR_VB; j++) {
					pX[j] = cX[j]; pY[j] = cY[j]; pZ[j] = cZ[j]; pA[j] = cA[j]; pC[j] = cC[j]; pOK[j] = cOK[j];
				}
			}
		}
	}

	// If the pool ever fills, the curtains are being CLIPPED - emit3 drops silently, which
	// on screen looks like a missing chunk rather than a budget. Say so once, so it is
	// diagnosable instead of mysterious. (Back off Ribbons or Thickness; see AUR_MAX_TRI.)
	{
		static bool warned = false;
		if (!warned && aurVtxN >= AUR_MAX_TRI * 3) {
			warned = true;
			oapiWriteLogV("ORO: aurora triangle pool FULL (%d tri) - curtains are being clipped. "
			              "Lower Ribbons or Thickness.", AUR_MAX_TRI);
		}
	}

	// Zero-pad the unused tail (invariant 3): the client's D3D9Triangle::Update Locks with
	// D3DLOCK_DISCARD and draws the full CREATION count, so an unwritten tail is random
	// triangles. DrawAuroraPoly pushes the whole buffer.
	if (aurVtxN > 0 && aurVtxN < AUR_MAX_TRI * 3) {
		memset(&aurVtx[aurVtxN],   0, sizeof(PlasVtx) * (AUR_MAX_TRI * 3 - aurVtxN));
		memset(&aurDepth[aurVtxN], 0, sizeof(float)   * (AUR_MAX_TRI * 3 - aurVtxN));
	}

	aurActive = (aurVtxN > 0);
}

// The geographic latitude the current Reach knob puts the real ovals at, for the dialog
// readout - same MIN/MAX mapping the build uses, so degrees show without the dialog
// duplicating the constants. Independent of view/enable, so the readout tracks the
// slider even when the aurora is not currently drawing.
float OroAurora_OvalLatDeg()
{
	const float reach = clampf(g_fx.auroraReach, 0.0f, 1.0f);
	const float colat = g_fx.aurColatMinDeg + (g_fx.aurColatMaxDeg - g_fx.aurColatMinDeg) * reach;
	return 90.0f - colat;
}

// Base / top curtain altitudes in km, for the dialog readouts (same mapping the build uses,
// so these follow the current world's ranges).
float OroAurora_BaseAltKm()
{
	return g_fx.aurBaseMinKm + (g_fx.aurBaseMaxKm - g_fx.aurBaseMinKm) * clampf(g_fx.auroraBase, 0.0f, 1.0f);
}
float OroAurora_TopAltKm()
{
	return g_fx.aurTopMinKm + (g_fx.aurTopMaxKm - g_fx.aurTopMinKm) * clampf(g_fx.auroraHeight, 0.0f, 1.0f);
}
