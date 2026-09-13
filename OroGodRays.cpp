// ==============================================================
// OroGodRays.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - THE TWO SOLAR EFFECTS: GOD RAYS, and THE LENS FLARE
// ----------------------------------------------------------------------------
// Both of them answer to the same three questions - where the sun is, how much air
// is between it and the camera, and how much of its disc is covered - so the sense
// lives in one place (UpdateSun / BuildSunScreen) and each effect brings its own
// gates. They are also, pleasingly, opposites: a SHAFT needs a medium to scatter in,
// a FLARE needs a concentrated source that an atmosphere is busy smearing across the
// sky. Rays low and thick, flare high and clean, handing over across an ascent.
//
// GOD RAYS: sunlight scattering out of the beam on its way past an occluder, drawn by
// PSGodRay (orofx.hlsl) as the classic radial-occlusion post-process.
// THE LENS FLARE (2026-09-12): ghosts, an iris starburst and a veil, drawn by
// PSLensFlare, EXTERNAL VIEWS ONLY - a flare is made inside a LENS and a healthy eye
// has none, so a cockpit view does not have one. See UpdateLensFlare.
//
// WHY THIS EFFECT IS CHEAP, AND IT IS ENTIRELY AN ACCIDENT OF FRAME ORDER.
// The hard part of screen-space god rays is normally the occlusion mask: you have
// to render the scene a second time, or read depth, to work out what is standing
// between the camera and the sun. ORO needs none of that, because of where it
// sits in D3D9Client's frame:
//
//     scene -> fp16 offscreen
//       |  RENDERPROC_PRE_RESOLVE      (patch (i) - the reentry plasma)
//       |  LightBlur (bloom + tonemap) -> backbuffer
//       |  Scene::RenderGlares()       <- the SUN DISC, drawn after the bloom
//       |  RENDERPROC_HUD_2ND          <- ORO captures the backbuffer HERE
//
// So the frame we resample ALREADY contains a bright sun disc that the client has
// already occluded against the hull, the terrain and the limb. The light source
// and its shadowing both arrive in the pixels, for free. This is the same shape of
// win as the eclipse finding the client had already darkened the planet surface -
// grep the client before designing: twice now it had already done most of the job.
//
// THE ATMOSPHERE IS A GATE, NOT A SLIDER, and that is the one physical ruling in
// here. Shafts are sunlight scattering off a MEDIUM; in vacuum there is nothing to
// scatter off and there are no rays, however photogenic they would be. So the pass
// does not run in orbit at all. Same class of decision as day-side lightning
// (invariant 22f): the sim already knows the answer, so the user is not asked.
// The TEST toggle bypasses it, because "fly to an atmosphere and wait for sunset"
// is not a way to judge a look.
//
// MAIN THREAD ONLY (clbkPreStep). The render path reads the published numbers and
// makes no oapi calls (invariant 1).
// ============================================================================

#include "OroModule.h"
#include "OroState.h"
#include <math.h>

namespace {

	// Below this ambient density there is not enough air to scatter anything worth
	// drawing. Earth sea level is ~1.225 kg/m^3 and this is ~0.6% of it, around
	// 50 km - comfortably above where an entry begins, so the shafts fade out on
	// the way up rather than snapping off.
	const double GR_RHO_FULL = 3.0e-2;   // [kg/m^3] full strength at or above this
	const double GR_RHO_MIN  = 7.0e-4;   // [kg/m^3] nothing at or below this

	// The sun has to be reasonably near the view axis. Past this the march runs off
	// the clamped edge of the capture and every sample returns the same texel, which
	// reads as a flat wash rather than shafts. Expressed as UV distance from the
	// screen centre, so it scales with any viewport.
	const float GR_UV_FULL = 0.35f;      // full strength while the sun is this close in
	const float GR_UV_MIN  = 1.30f;      // gone by here (well off-screen)

	// Elevation band. Shafts are a LOW-SUN phenomenon: near noon the beam is short,
	// the scattering angle is wrong and there is nothing to silhouette it against.
	const double GR_ELEV_MAX = 35.0;     // [deg] gone by this solar elevation
	const double GR_ELEV_LOW =  8.0;     // [deg] full strength at or below this
	// ... and the bottom of the band. The disc has to be up for its light to reach us;
	// a degree or so of slack keeps the last of the shafts alive right at sunset, where
	// they are at their best, instead of switching them off as the limb touches.
	const double GR_ELEV_UP  = -1.0;     // [deg] full strength at or above this
	const double GR_ELEV_SET = -4.0;     // [deg] nothing at or below this

	// ---- THE LENS FLARE (2026-09-12) ------------------------------------------
	// A MUCH wider screen band than the shafts'. The god rays correctly retire when
	// the sun leaves the frame, because a march that runs off the clamped edge has
	// nothing left to radiate FROM; a lens does not care - with the sun just out of
	// shot the ghosts are still strung across the frame on the far side of centre,
	// which is half of what a flare looks like in real footage.
	const float LF_UV_FULL = 0.60f;      // full strength while the sun is this close in
	const float LF_UV_MIN  = 1.90f;      // gone by here

	// The air fade's reference thickness - Earth at sea level. Deliberately a FIXED
	// number rather than the local surface density: the fade is about how much haze
	// stands between the camera and the sun, and Venus's 65 kg/m^3 should saturate it
	// rather than be normalised back to "one atmosphere's worth".
	const double LF_RHO_REF = 1.225;     // [kg/m^3]

	inline double clampd(double x, double a, double b) { return x < a ? a : (x > b ? b : x); }
	inline float  clampf(float  x, float  a, float  b) { return x < a ? a : (x > b ? b : x); }
	inline float  sstep(float t) { t = clampf(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }

	// Linear ramp between two thresholds, smoothstepped. Handles either direction so
	// the "more is more" and "more is less" gates read the same way at the call site.
	float ramp(double v, double atZero, double atOne)
	{
		if (fabs(atOne - atZero) < 1e-30) return v >= atOne ? 1.0f : 0.0f;
		return sstep((float)((v - atZero) / (atOne - atZero)));
	}
}

// ----------------------------------------------------------------------------
// THE SUN, SHARED (2026-09-12). Where the star is, how much air stands at the camera,
// how high the disc rides and how much of it something is covering are facts about the
// WORLD - not about either effect that wants them. So they are sensed ONCE here and
// each consumer applies its own gates to the result.
//
// !! AND THAT SPLIT IS A LESSON, NOT TIDINESS. All of it used to live inside
// UpdateGodRays, which returns at its first line the moment the god rays' pill is off -
// so the snapshot went stale and the lens flare would have died stone dead whenever
// someone switched the shafts off, for no reason a user could ever have guessed. That
// is exactly the trap the rings hit on 2026-09-12 with the shared per-body settings
// file: TWO EFFECTS THAT SHARE A VALUE MUST NOT SHARE A GATE.
//
// THE THREAD SPLIT IS THE 2026-08-15 PAUSE FIX, unchanged. UpdateSun does the oapi work
// on the MAIN thread (clbkPreStep, so it freezes while paused - which is right: no
// amount of looking around changes how much air there is), and BuildSunScreen finds the
// sun ON SCREEN in the RENDER PATH off the render camera, so a paused pan carries the
// shafts and the ghosts with it. See ProjCam in OroModule.h, and invariant 1.
// ----------------------------------------------------------------------------
namespace {
	struct SunSnap {
		VECTOR3 spos;          // the star, global
		double  rho;           // ambient density at the camera [kg/m^3]; 0 = vacuum/unknown
		double  elevDeg;       // solar elevation above the local horizon [deg]
		float   fEcl;          // 1 - obscuration (1 when the ECLIPSE effect is not running)
		bool    hasBody;       // a body was identified, so rho and elevDeg mean something
	};
	SunSnap s_sun;
	bool    s_sunValid = false;

	// Each consumer's own light budget, everything except where the sun lands on screen
	// (which only the render path knows). One float each - the terms only ever multiply.
	float s_grBudget = 0.0f;  bool s_grValid = false;
	float s_lfBudget = 0.0f;  bool s_lfValid = false;
}

void OroModule::UpdateSun()
{
	s_sunValid   = false;
	s_sun.rho    = 0.0;
	s_sun.elevDeg = 0.0;
	s_sun.fEcl   = 1.0f;
	s_sun.hasBody = false;

	if (!g_fx.masterArmed) return;

	// Nobody is asking: skip the body search rather than pay for a snapshot no
	// consumer will read. Both TEST toggles count as asking - a test that could not
	// find the sun would be a test of nothing.
	const bool wantGr = (g_fx.grayEnabled  || g_fx.grayTest);
	const bool wantLf = (g_fx.flareEnabled || g_fx.flareTest);
	if (!wantGr && !wantLf) return;

	OBJHANDLE hSun = OroFindStar();
	if (!hSun) return;

	// WHERE THE SUN LANDS ON SCREEN is BuildSunScreen's job (2026-08-15): it is the one
	// part of this that depends on where the camera is POINTING, and clbkPreStep does
	// not run while paused. Everything here is about the light itself.
	if (!preStepCamValid) return;
	const VECTOR3 cpos = preStepCam.pos;
	oapiGetGlobalPos(hSun, &s_sun.spos);
	VECTOR3 vS = s_sun.spos - cpos;
	const double dS = length(vS);
	if (dS < 1.0) return;
	vS /= dS;

	// --- the body we are at, its air, and the sun's elevation over it -------
	// Ambient density AT THE CAMERA, asked of the BODY rather than of a vessel, so this
	// is correct whether the camera is in a cockpit, chasing from outside, or parked on
	// a mountain with no vessel near it at all.
	{
		OBJHANDLE hBody = NULL;
		// The body we are AT: camera target's surface reference, else the nearest body
		// measured in its own radii (the eclipse's FindPrimary reasoning).
		OBJHANDLE hT = oapiCameraTarget();
		if (hT && oapiGetObjectType(hT) == OBJTP_VESSEL) {
			VESSEL* v = oapiGetVesselInterface(hT);
			if (v) hBody = v->GetSurfaceRef();
		} else if (hT) {
			hBody = hT;
		}
		if (!hBody) {
			double bestR = 1e30;
			const DWORD n = oapiGetGbodyCount();
			for (DWORD i = 0; i < n; i++) {
				OBJHANDLE h = oapiGetGbodyByIndex((int)i);
				if (!h || oapiGetObjectType(h) == OBJTP_STAR) continue;
				const double Rb = oapiGetSize(h);
				if (Rb < 1.0) continue;
				VECTOR3 p; oapiGetGlobalPos(h, &p);
				const double rel = length(p - cpos) / Rb;
				if (rel < bestR) { bestR = rel; hBody = h; }
			}
		}

		if (hBody) {
			// Camera position in the body's own frame -> longitude/latitude/radius,
			// which is what the atmosphere query wants.
			VECTOR3 bpos; oapiGetGlobalPos(hBody, &bpos);
			VECTOR3 rel = cpos - bpos;
			MATRIX3 Rb; oapiGetRotationMatrix(hBody, &Rb);
			const VECTOR3 loc = tmul(Rb, rel);
			const double  r   = length(loc);
			if (r >= 1.0) {
				const double lng = atan2(loc.z, loc.x);
				const double lat = asin(clampd(loc.y / r, -1.0, 1.0));
				const double alt = r - oapiGetSize(hBody);

				// !! ARGUMENT ORDER IS (alt, LNG, LAT) !! - not the lat/lng most APIs
				// take, and the compiler cannot catch the swap: all three are doubles.
				ATMPARAM prm; prm.rho = 0.0; prm.p = 0.0; prm.T = 0.0;
				oapiGetPlanetAtmParams(hBody, alt > 0.0 ? alt : 0.0, lng, lat, &prm);
				s_sun.rho = prm.rho;

				// Solar elevation at the camera: the angle of the sun above the local
				// horizon, i.e. above the plane perpendicular to the local "up".
				const VECTOR3 up = rel / length(rel);
				s_sun.elevDeg = asin(clampd(dotp(up, vS), -1.0, 1.0)) * DEG;
				s_sun.hasBody = true;
			}
		}
	}

	// --- something covering the disc ----------------------------------------
	// !! ONLY VALID WHEN THE ECLIPSE IS ACTUALLY RUNNING. UpdateEclipse leaves
	// eclipseObsc at 0 when its own effect is switched off - "nothing is covering the
	// sun" and "nobody looked" are the same value there - so reading it unconditionally
	// would make BOTH solar effects quietly depend on an unrelated pill. The god rays'
	// elevation band covers the important case (night) either way, and the flare's
	// contrast probe reads an eclipsed disc off the frame; this term is the refinement.
	{
		const bool eclLive = g_fx.masterArmed && (g_fx.eclipseEnabled || g_fx.eclipseTest);
		s_sun.fEcl = eclLive ? (1.0f - clampf(g_fx.eclipseObsc, 0.0f, 1.0f)) : 1.0f;
	}

	s_sunValid = true;
}

// ----------------------------------------------------------------------------
// GOD RAYS, main-thread half: how much light there is to scatter. Consumer of the
// snapshot above; the shafts' own gates - air, elevation - live here.
// ----------------------------------------------------------------------------
void OroModule::UpdateGodRays()
{
	s_grValid = false;
	grActive = false;
	g_fx.grayVis = 0.0f;
	g_fx.grayWhy[0] = '\0';

	const bool live = g_fx.masterArmed && (g_fx.grayEnabled || g_fx.grayTest);
	if (!live) { strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy), "off"); return; }
	if (g_fx.grayStrength <= 0.001f) { strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy), "str 0"); return; }
	if (!s_sunValid) { strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy), "no star"); return; }

	// --- the atmosphere gate, and the elevation band ------------------------
	// TEST bypasses both: judging a look must not require a sunset in an atmosphere.
	float  fAir = 1.0f, fElev = 1.0f;
	double elevDeg = 0.0;

	if (!g_fx.grayTest) {
		if (!s_sun.hasBody) { strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy), "no air"); return; }

		fAir = ramp(s_sun.rho, GR_RHO_MIN, GR_RHO_FULL);
		if (fAir <= 0.001f) {
			strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy), "vacuum");
			return;
		}

		elevDeg = s_sun.elevDeg;

		// TWO ends to this band, and the LOWER one is not decoration. Once the sun is
		// properly below the horizon the planet itself is the occluder and there is no
		// beam left to scatter - so a night side must produce nothing. That case cannot
		// be left to the eclipse term, because eclipseObsc is only computed while the
		// ECLIPSE effect is running: with it switched off, obscAll stays 0 and midnight
		// would look to us exactly like noon with the sun conveniently low.
		fElev = (1.0f - ramp(elevDeg, GR_ELEV_LOW, GR_ELEV_MAX))   // gone as the sun climbs
		      * ramp(elevDeg, GR_ELEV_SET, GR_ELEV_UP);            // gone as the sun sets
		if (fElev <= 0.001f) {
			strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy),
			         elevDeg > 0.0 ? "high sun" : "night");
			return;
		}
	}

	// --- tint ---------------------------------------------------------------
	// The lower the sun, the longer the path through air, the more of the blue end
	// Rayleigh scattering has already removed - so the shafts warm toward orange as
	// the sun sets. Driven by the SAME elevation term as the fade, so the reddening
	// and the strengthening happen together, as they do in the sky.
	{
		const float warm = clampf(g_fx.grayWarm, 0.0f, 1.0f) * (1.0f - (float)clampd(elevDeg / GR_ELEV_MAX, 0.0, 1.0));
		grTintR = 1.0f;
		grTintG = 1.0f - 0.30f * warm;
		grTintB = 1.0f - 0.62f * warm;
	}

	s_grBudget = fAir * fElev * s_sun.fEcl;
	s_grValid  = true;
}

// ----------------------------------------------------------------------------
// THE LENS FLARE, main-thread half (2026-09-12). Its whole budget is one line of
// arithmetic; what makes the effect honest is measured per pixel in the shader, off
// the frame the client already drew the occluded sun into.
//
// EXTERNAL VIEWS ONLY, and it is a physical ruling rather than a scope cut - his:
// a flare is made between the elements of a LENS and a healthy eye has none, so in
// a cockpit (any of the three) you are looking through the pilot's eyes and there is
// no flare, while outside the camera really is a camera. Every awkward case falls
// away with it: no VC glass to reason about, no HUD ordering, nothing to explain.
//
// AND THE AIR FADE IS THE GOD RAYS' GATE INVERTED, which is not a coincidence. A
// shaft needs a MEDIUM to scatter in; a flare needs a CONCENTRATED source, and what
// an atmosphere does is smear the sun's light across the sky and destroy the contrast
// the flare lives on. So the two effects hand over to each other across an ascent:
// shafts low and thick, flare high and clean. It is a real term rather than a taste
// knob for the same reason the shafts' density gate is.
// ----------------------------------------------------------------------------
void OroModule::UpdateLensFlare()
{
	s_lfValid = false;
	lfActive = false;
	g_fx.flareVis = 0.0f;
	g_fx.flareWhy[0] = '\0';

	const bool live = g_fx.masterArmed && (g_fx.flareEnabled || g_fx.flareTest);
	if (!live) { strcpy_s(g_fx.flareWhy, sizeof(g_fx.flareWhy), "off"); return; }
	if (!extGate) { strcpy_s(g_fx.flareWhy, sizeof(g_fx.flareWhy), "cockpit"); return; }
	if (g_fx.flareStr <= 0.001f) { strcpy_s(g_fx.flareWhy, sizeof(g_fx.flareWhy), "str 0"); return; }
	if (!s_sunValid) { strcpy_s(g_fx.flareWhy, sizeof(g_fx.flareWhy), "no star"); return; }

	// --- the air ------------------------------------------------------------
	// Linear in DENSITY, not in altitude, and that is what makes it read right: density
	// falls off exponentially, so almost all of the fade is spent in the lowest few
	// kilometres where the haze actually is, and by 20 km the flare is essentially back
	// to its vacuum strength. TEST bypasses it - waiting to reach orbit is not a way to
	// judge a look. (The disc still has to be in the frame: TEST cannot fake a sun.)
	float fAir = 1.0f;
	if (!g_fx.flareTest && s_sun.hasBody && s_sun.rho > 0.0) {
		const float thick = ramp(s_sun.rho, 0.0, LF_RHO_REF);
		fAir = 1.0f - clampf(g_fx.flareAir, 0.0f, 1.0f) * thick;
		if (fAir <= 0.02f) {
			strcpy_s(g_fx.flareWhy, sizeof(g_fx.flareWhy), "hazy");
			return;
		}
	}

	// NO ELEVATION BAND HERE, deliberately - unlike the shafts, a flare at high noon is
	// the most photographed case there is. A sun BELOW the horizon needs no rule either:
	// the client draws no disc there, so the shader's contrast probe reads nothing and
	// the flare is already gone. Keying it to what the frame CONTAINS rather than to a
	// brightness model of our own is the whole 2026-09-01 sun-disc lesson.
	s_lfBudget = fAir * s_sun.fEcl;
	if (s_lfBudget <= 0.004f) {
		strcpy_s(g_fx.flareWhy, sizeof(g_fx.flareWhy), "eclipsed");
		return;
	}
	s_lfValid = true;
}

// ----------------------------------------------------------------------------
// BuildSunScreen - THE RENDER PATH HALF (2026-08-15 for the shafts, shared since
// 2026-09-12). Where the sun is on screen, from the camera the frame is ACTUALLY
// being rendered with (patch (k)), so it is right while paused.
//
// INVARIANT-1 AUDIT: zero oapi calls here and in both consumers below. The g_fx
// writes are readout strings - plain single-thread member writes.
// ----------------------------------------------------------------------------
void OroModule::BuildSunScreen()
{
	sunScrValid = false;
	sunScrWhy   = "no star";
	if (!s_sunValid) return;

	VECTOR3 cpos; MATRIX3 R; double tanAp;
	if (!FillProjCam(cpos, R, tanAp)) return;
	if (tanAp < 1e-6) { sunScrWhy = "no cam"; return; }

	VECTOR3 vS = s_sun.spos - cpos;
	const double dS = length(vS);
	if (dS < 1.0) return;
	vS /= dS;

	// The camera's rotation matrix takes GLOBAL to CAMERA-LOCAL; z is forward, x right,
	// y up. tmul() applies the TRANSPOSE - global -> local, which is what we want.
	const VECTOR3 cs = tmul(R, vS);
	if (cs.z <= 1e-4) {
		// Sun is behind the camera plane. Nothing to radiate from and no lens facing it,
		// and the projection would flip everything to the wrong side of the screen.
		sunScrWhy = "behind";
		return;
	}

	// Perspective divide with Orbiter's aperture (the vertical half-angle).
	const double aspect = (viewH > 0) ? ((double)viewW / (double)viewH) : 1.3333;

	// NDC in [-1,+1], then to UV in [0,1] with V flipped (screen y grows downward).
	const double ndcX = (cs.x / (cs.z * tanAp * aspect));
	const double ndcY = (cs.y / (cs.z * tanAp));
	sunU = (float)(0.5 + 0.5 * ndcX);
	sunV = (float)(0.5 - 0.5 * ndcY);

	sunScrWhy   = NULL;
	sunScrValid = true;
}

// ----------------------------------------------------------------------------
// GOD RAYS, render-path half: the screen-proximity fade and the decision to spend a
// pass. MAY leave grSunU/grSunV outside [0,1] - the shafts still converge correctly
// on an off-screen source and grFade is what retires the effect before it degrades.
// ----------------------------------------------------------------------------
void OroModule::BuildGodRayScreen()
{
	grActive = false;
	if (!s_grValid) return;
	if (!sunScrValid) {
		if (sunScrWhy) strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy), sunScrWhy);
		return;
	}

	grSunU = sunU;
	grSunV = sunV;

	// --- screen-proximity fade ----------------------------------------------
	// Measured from the CENTRE in aspect-corrected UV, so a sun just off the side of a
	// widescreen viewport is treated as just-off, not far away.
	const double aspect = (viewH > 0) ? ((double)viewW / (double)viewH) : 1.3333;
	const double du  = (grSunU - 0.5) * aspect;
	const double dv  = (grSunV - 0.5);
	const double duv = sqrt(du * du + dv * dv);
	const float  fScreen = 1.0f - ramp(duv, GR_UV_FULL, GR_UV_MIN);
	if (fScreen <= 0.001f) {
		strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy), "off-view");
		return;
	}

	grFade = fScreen * s_grBudget;
	grStr  = clampf(g_fx.grayStrength, 0.0f, 1.0f);

	g_fx.grayVis = grFade;
	if (grFade <= 0.004f) {
		if (!g_fx.grayWhy[0]) strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy), "eclipsed");
		return;
	}

	// Costs a full frame copy + a 24-tap shader pass, so the threshold is not
	// decoration - it is what keeps the effect free when there is nothing to show.
	grActive = true;
}

// ----------------------------------------------------------------------------
// THE LENS FLARE, render-path half. Two things the shafts do not need:
//
// A MUCH WIDER SCREEN BAND. The god rays retire when the sun leaves the frame because
// a march that runs off the clamped edge has nothing left to radiate from. A lens does
// not care: with the sun just out of shot the ghost chain is still strung across the
// frame on the far side of centre, which is half of what a flare looks like in real
// footage. See LF_UV_FULL / LF_UV_MIN.
//
// AND lfSamp, WHICH SAYS WHETHER THE SHADER'S PROBE MEANS ANYTHING. The contrast probe
// reads the sun's own pixels and a ring of sky around them; once the disc's UV is near
// the edge of the captured frame those taps CLAMP and start measuring the edge of the
// texture instead of the sky. So this ramps to 0 across the border and the shader lerps
// its measurement back toward "assume visible", falling through to the host budget
// alone - which is the honest answer when there is nothing left to look at.
// ----------------------------------------------------------------------------
void OroModule::BuildLensFlareScreen()
{
	lfActive = false;
	if (!s_lfValid) return;
	if (!sunScrValid) {
		if (sunScrWhy) strcpy_s(g_fx.flareWhy, sizeof(g_fx.flareWhy), sunScrWhy);
		return;
	}

	lfSunU = sunU;
	lfSunV = sunV;

	const double aspect = (viewH > 0) ? ((double)viewW / (double)viewH) : 1.3333;
	const double du  = (lfSunU - 0.5) * aspect;
	const double dv  = (lfSunV - 0.5);
	const double duv = sqrt(du * du + dv * dv);
	const float  fScreen = 1.0f - ramp(duv, LF_UV_FULL, LF_UV_MIN);
	if (fScreen <= 0.001f) {
		strcpy_s(g_fx.flareWhy, sizeof(g_fx.flareWhy), "off-view");
		return;
	}

	// Distance from the nearest frame edge, in UV. The probe's outer ring reaches
	// LF_RING (0.09) of the screen HEIGHT, which is its widest excursion in v, so the
	// band has to clear that before the measurement is fully trusted.
	{
		const double mU = (lfSunU < 0.5f) ? lfSunU : (1.0 - lfSunU);
		const double mV = (lfSunV < 0.5f) ? lfSunV : (1.0 - lfSunV);
		lfSamp = ramp((mU < mV) ? mU : mV, 0.030, 0.130);
	}

	lfFade = fScreen * s_lfBudget;
	g_fx.flareVis = lfFade;
	if (lfFade <= 0.004f) {
		if (!g_fx.flareWhy[0]) strcpy_s(g_fx.flareWhy, sizeof(g_fx.flareWhy), "eclipsed");
		return;
	}

	lfActive = true;
}
