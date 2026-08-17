// ==============================================================
// OroGodRays.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - GOD RAYS: crepuscular shafts from the sun
// ----------------------------------------------------------------------------
// Sunlight scattering out of the beam on its way past an occluder. The shafts
// are drawn by PSGodRay (orofx.hlsl) as the classic radial-occlusion post-
// process; everything in this file is the MAIN-thread half - where the sun is on
// screen, and whether there is any air to scatter in.
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
// Per frame, MAIN thread. Publishes the sun's UV position, the combined fade and
// the tint, and leaves grActive telling the render path whether to spend a pass.
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// THE SPLIT (2026-08-15, the pause fix). UpdateGodRays decides how much light there is
// to scatter - air, solar elevation, eclipse - all of which need oapi and none of which
// depends on where the camera is pointing. BuildGodRayScreen finds the sun on screen.
// See ProjCam in OroModule.h.
// ----------------------------------------------------------------------------
namespace {
	struct GrSnap {
		VECTOR3 spos;                  // the star, global
		float   fAir, fElev, fEcl;     // the light budget
	};
	GrSnap s_gr;
	bool   s_grValid = false;
}

void OroModule::UpdateGodRays()
{
	s_grValid = false;
	grActive = false;
	g_fx.grayVis = 0.0f;
	g_fx.grayWhy[0] = '\0';

	const bool live = g_fx.masterArmed && (g_fx.grayEnabled || g_fx.grayTest);
	if (!live) { strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy), "off"); return; }
	if (g_fx.grayStrength <= 0.001f) { strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy), "str 0"); return; }

	OBJHANDLE hSun = OroFindStar();
	if (!hSun) { strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy), "no star"); return; }

	// WHERE THE SUN LANDS ON SCREEN moved to BuildGodRayScreen (2026-08-15): it is the
	// one part of this that depends on where the camera is POINTING, and clbkPreStep
	// does not run while paused - so the shafts used to keep radiating from wherever the
	// sun was on screen when the sim stopped. Everything below is about how much light
	// there is to scatter, which no amount of looking around changes.
	if (!preStepCamValid) return;
	const VECTOR3 cpos = preStepCam.pos;
	VECTOR3 spos;
	oapiGetGlobalPos(hSun, &spos);
	VECTOR3 vS = spos - cpos;
	const double dS = length(vS);
	if (dS < 1.0) { strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy), "no star"); return; }
	vS /= dS;

	// --- the atmosphere gate, and the elevation band ------------------------
	// TEST bypasses both: judging a look must not require a sunset in an atmosphere.
	float fAir = 1.0f, fElev = 1.0f;
	double elevDeg = 0.0;

	if (!g_fx.grayTest) {
		// Ambient density AT THE CAMERA, asked of the BODY rather than of a vessel, so
		// this is correct whether the camera is in a cockpit, chasing from outside, or
		// parked on a mountain with no vessel near it at all.
		ATMPARAM prm; prm.rho = 0.0; prm.p = 0.0; prm.T = 0.0;

		OBJHANDLE hBody = NULL;
		{
			// The body we are AT: camera target's surface reference, else the nearest
			// body measured in its own radii (the eclipse's FindPrimary reasoning).
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
		}

		if (!hBody) { strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy), "no air"); return; }

		// Camera position in the body's own frame -> longitude/latitude/radius, which is
		// what the atmosphere query wants.
		VECTOR3 bpos; oapiGetGlobalPos(hBody, &bpos);
		VECTOR3 rel = cpos - bpos;
		MATRIX3 Rb; oapiGetRotationMatrix(hBody, &Rb);
		const VECTOR3 loc = tmul(Rb, rel);
		const double  r   = length(loc);
		if (r < 1.0) { strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy), "no air"); return; }
		const double lng = atan2(loc.z, loc.x);
		const double lat = asin(clampd(loc.y / r, -1.0, 1.0));
		const double alt = r - oapiGetSize(hBody);

		// !! ARGUMENT ORDER IS (alt, LNG, LAT) !! - not the lat/lng most APIs take, and
		// the compiler cannot catch the swap because all three are doubles.
		oapiGetPlanetAtmParams(hBody, alt > 0.0 ? alt : 0.0, lng, lat, &prm);

		fAir = ramp(prm.rho, GR_RHO_MIN, GR_RHO_FULL);
		if (fAir <= 0.001f) {
			strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy), "vacuum");
			return;
		}

		// Solar elevation at the camera: the angle of the sun above the local horizon,
		// i.e. above the plane perpendicular to the local "up".
		const VECTOR3 up = rel / length(rel);
		elevDeg = asin(clampd(dotp(up, vS), -1.0, 1.0)) * DEG;

		// TWO ends to this band, and the LOWER one is not decoration. Once the sun is
		// properly below the horizon the planet itself is the occluder and there is no
		// beam left to scatter - so a night side must produce nothing. That case cannot
		// be left to the eclipse term below, because eclipseObsc is only computed while
		// the ECLIPSE effect is running: with it switched off, obscAll stays 0 and
		// midnight would look to us exactly like noon with the sun conveniently low.
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

	// --- the eclipse takes the light with it --------------------------------
	// If something is covering the sun's disc there is proportionally less beam to
	// scatter, so a transit or a total eclipse kills the shafts for free. This is the
	// one place the two solar effects talk: UpdateEclipse runs first, so the number is
	// this frame's.
	// ⚠ ONLY VALID WHEN THE ECLIPSE IS ACTUALLY RUNNING. UpdateEclipse leaves
	// eclipseObsc at 0 when its own effect is switched off - "nothing is covering the
	// sun" and "nobody looked" are the same value - so reading it unconditionally would
	// make the god rays quietly depend on an unrelated pill. The elevation band above is
	// what covers the important case (night) either way; this term is the refinement.
	const bool  eclLive = g_fx.masterArmed && (g_fx.eclipseEnabled || g_fx.eclipseTest);
	const float fEcl    = eclLive ? (1.0f - clampf(g_fx.eclipseObsc, 0.0f, 1.0f)) : 1.0f;

	// --- hand the light budget to the render path -----------------------------
	s_gr.spos = spos; s_gr.fAir = fAir; s_gr.fElev = fElev; s_gr.fEcl = fEcl;
	s_grValid = true;
}

// ----------------------------------------------------------------------------
// BuildGodRayScreen - THE RENDER PATH HALF (2026-08-15). Where the sun is on screen,
// and therefore whether there is anything to radiate from. The light BUDGET (air,
// elevation, eclipse) is decided on the main thread and arrives in s_gr.
//
// INVARIANT-1 AUDIT: zero oapi calls. The g_fx writes are the readout strings, which
// are plain single-thread member writes.
// ----------------------------------------------------------------------------
void OroModule::BuildGodRayScreen()
{
	grActive = false;
	if (!s_grValid) return;

	VECTOR3 cpos; MATRIX3 R; double tanAp;
	if (!FillProjCam(cpos, R, tanAp)) return;
	if (tanAp < 1e-6) { strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy), "no cam"); return; }

	VECTOR3 vS = s_gr.spos - cpos;
	const double dS = length(vS);
	if (dS < 1.0) { strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy), "no star"); return; }
	vS /= dS;

	// The camera's rotation matrix takes GLOBAL to CAMERA-LOCAL; z is forward, x right,
	// y up. tmul() applies the TRANSPOSE - global -> local, which is what we want.
	const VECTOR3 cs = tmul(R, vS);
	if (cs.z <= 1e-4) {
		// Sun is behind the camera plane. Nothing to radiate from, and the projection
		// would flip the shafts to the wrong side of the screen.
		strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy), "behind");
		return;
	}

	// Perspective divide with Orbiter's aperture (the vertical half-angle).
	const double aspect = (viewH > 0) ? ((double)viewW / (double)viewH) : 1.3333;

	// NDC in [-1,+1], then to UV in [0,1] with V flipped (screen y grows downward).
	const double ndcX = (cs.x / (cs.z * tanAp * aspect));
	const double ndcY = (cs.y / (cs.z * tanAp));
	grSunU = (float)(0.5 + 0.5 * ndcX);
	grSunV = (float)(0.5 - 0.5 * ndcY);

	// --- screen-proximity fade ----------------------------------------------
	// Measured from the CENTRE in aspect-corrected UV, so a sun just off the side of a
	// widescreen viewport is treated as just-off, not far away.
	const double du  = (grSunU - 0.5) * aspect;
	const double dv  = (grSunV - 0.5);
	const double duv = sqrt(du * du + dv * dv);
	const float  fScreen = 1.0f - ramp(duv, GR_UV_FULL, GR_UV_MIN);
	if (fScreen <= 0.001f) {
		strcpy_s(g_fx.grayWhy, sizeof(g_fx.grayWhy), "off-view");
		return;
	}

	grFade = fScreen * s_gr.fAir * s_gr.fElev * s_gr.fEcl;
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
