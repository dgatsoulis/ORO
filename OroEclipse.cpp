// ==============================================================
// OroEclipse.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - ECLIPSE: the camera inside another body's shadow
// ----------------------------------------------------------------------------
// The first ENVIRONMENT effect that is not about a vessel at all. Every frame we
// ask one question at the CAMERA's position: how much of the sun's disc is
// covered by something? The answer drives a model of the eye behind the camera.
//
// WHY AN EYE AND NOT A DIMMER - the whole design turns on this.
// D3D9Client is not blind to eclipses. It already:
//   * darkens a PLANET SURFACE under a moon's shadow, with a real penumbra ramp
//     (vPlanet::SetupEclipse + the 512-entry occlusion LUT in NewPlanet.hlsl);
//   * cuts a VESSEL's sunlight inside its primary's shadow, and inside the
//     shadow of that primary's PARENT (vVessel::ModLighting ->
//     vPlanet::GetObjectAtmoParams -> SunOcclusionByPlanet).
// So a naive "multiply the frame down when obscured" would fight the renderer on
// the night side of every orbit and read as "ORO made my nights too dark".
//
// What NO renderer models is the observer. The eye is a slow, asymmetric,
// self-calibrating instrument: it takes tens of seconds to open up when the light
// goes and about a second to close down when it comes back, and below cone
// threshold it stops carrying colour altogether. Those responses are ours to
// build, they are what an eclipse actually FEELS like, and - the structural
// point - a gain that converges to 1.0 as the eye adapts can never double-count
// what the client already did. Stare into a long orbital night and ORO ends up
// applying exactly nothing; it is the TRANSITIONS that it owns.
//
// TWO OBSCURATIONS, and the split is the design (mirrors invariant 17's spirit):
//   obscAll   - every body. Drives the eye's brightness ADAPTATION only. Always
//               correct to include the primary here: the renderer darkened the
//               world, and the eye is responding to that darkening. This term
//               self-cancels as the eye adapts (gain -> 1.0), so including the
//               primary costs nothing at steady state - it is only the transitions.
//   obscOther - every body EXCEPT the one you are at. Drives the STEADY DIM and the
//               COLOUR LOSS. Nothing in the client knows a moon's shadow has swept
//               over you in low orbit - vVessel tests exactly one occluder, its
//               primary's parent - so that case, and only that case, needs us to
//               darken/desaturate the frame ourselves.
//               COLOUR LOSS moved here from obscAll on 2026-08-05: tied to obscAll it
//               desaturated the whole frame at every ORBITAL NIGHT (primary blocks the
//               sun 100%), and UNLIKE adaptation a raw desaturation does NOT self-
//               cancel - so it read as "ORO washes out my night colours" (user
//               report). Scotopic colour loss belongs to the case the renderer cannot
//               help with (another body's shadow), the same reasoning as the dim.
//
// MAIN THREAD ONLY (clbkPreStep): oapi calls everywhere, none of them legal from
// the render path (invariant 1). The render callback reads the three published
// numbers and nothing else.
// ============================================================================

#include "OroModule.h"
#include "OroState.h"
#include <math.h>

namespace {

	// --- the eye ------------------------------------------------------------
	// Dark adaptation is minutes to complete; the fast cone phase - the part you
	// notice - runs in tens of seconds. Light adaptation is an order of magnitude
	// faster, which is exactly why emerging into sunlight blinds you and entering
	// shadow merely gropes.
	const double ECL_TAU_DARK   = 18.0;   // [s, REAL] eye opening up as the light goes
	const double ECL_TAU_LIGHT  =  1.2;   // [s, REAL] eye closing down as it returns
	const float  ECL_GAMMA      = 0.50f;  // compressive response: a 10x drop in light is
	                                      //   ~3x apparent. Straight ratios look absurd.
	const float  ECL_GMIN       = 0.10f;  // never fully black - you would call that a bug
	const float  ECL_GMAX       = 1.75f;  // glare ceiling on the way out. The raw ratio after a
	                                      //   full dark adaptation is ~7x, which is a white
	                                      //   screen: honest, unwatchable, and indistinguishable
	                                      //   from a bug. 1.75 still blows the highlights.
	const float  ECL_LIT_FLOOR  = 0.02f;  // earthshine/starlight: the scene is never at zero
	const float  ECL_DIM_MAX    = 0.85f;  // what a Dim of 1.0 actually removes

	// TEST cycle (REAL time): clear -> first contact -> totality -> emergence -> clear.
	// One loop exercises everything worth looking at, in a length that does not
	// outlast the user's patience.
	const double ECL_TEST_PERIOD = 40.0;

	inline double clampd(double x, double a, double b) { return x < a ? a : (x > b ? b : x); }
	inline float  clampf(float  x, float  a, float  b) { return x < a ? a : (x > b ? b : x); }

	// Smoothstep, so the synthetic ramps do not read as linear wipes.
	inline float sstep(float t) { t = clampf(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }

	// ------------------------------------------------------------------------
	// The fraction of disc 1 (the sun) hidden behind disc 2 (the occulter), both
	// as ANGULAR radii seen from the camera, separated by angle d.
	//
	// Working in angular radii rather than with a shadow CONE is what makes this
	// general and cheap: it needs no umbra apex, it is correct for an occulter
	// nearer OR further than the geometric umbra tip, and it hands back annular
	// eclipses for free (a small far moon can never take the whole disc, and the
	// ring it leaves is genuinely bright - the number says so).
	// ------------------------------------------------------------------------
	float DiscOverlap(double r1, double r2, double d)
	{
		if (r1 <= 1e-12) return 0.0f;
		if (d >= r1 + r2) return 0.0f;                       // clear
		if (d <= fabs(r1 - r2))                              // one disc wholly inside the other
			return (float)(r2 >= r1 ? 1.0 : (r2 * r2) / (r1 * r1));   // total, or annular
		if (d < 1e-12) return (float)((r2 * r2) / (r1 * r1));
		// Circular lens area (the standard two-circle intersection).
		const double a1 = clampd((d * d + r1 * r1 - r2 * r2) / (2.0 * d * r1), -1.0, 1.0);
		const double a2 = clampd((d * d + r2 * r2 - r1 * r1) / (2.0 * d * r2), -1.0, 1.0);
		const double t  = (-d + r1 + r2) * (d + r1 - r2) * (d - r1 + r2) * (d + r1 + r2);
		const double A  = r1 * r1 * acos(a1) + r2 * r2 * acos(a2) - 0.5 * sqrt(t > 0.0 ? t : 0.0);
		return (float)clampd(A / (3.14159265358979 * r1 * r1), 0.0, 1.0);
	}

	// (the star lookup lives outside this namespace now - see OroFindStar below.)

	// "The body you are at" - the one whose shadow the RENDERER has already dealt
	// with. Taken from the camera's target (a vessel hands us its surface
	// reference, a body is its own), with a fallback for the rare frame where
	// there is no target: nearest body measured in ITS OWN RADII, which is what
	// "at" means when the candidates are a moon 2000 km away and a sun 150 Gm away.
	OBJHANDLE FindPrimary()
	{
		OBJHANDLE hT = oapiCameraTarget();
		if (hT) {
			if (oapiGetObjectType(hT) == OBJTP_VESSEL) {
				VESSEL* v = oapiGetVesselInterface(hT);
				if (v) {
					OBJHANDLE hR = v->GetSurfaceRef();
					if (hR) return hR;
				}
			} else {
				return hT;
			}
		}
		VECTOR3 cpos; oapiCameraGlobalPos(&cpos);
		OBJHANDLE best = NULL; double bestR = 1e30;
		const DWORD n = oapiGetGbodyCount();
		for (DWORD i = 0; i < n; i++) {
			OBJHANDLE h = oapiGetGbodyByIndex((int)i);
			if (!h || oapiGetObjectType(h) == OBJTP_STAR) continue;
			const double R = oapiGetSize(h);
			if (R < 1.0) continue;
			VECTOR3 p; oapiGetGlobalPos(h, &p);
			const double rel = length(p - cpos) / R;
			if (rel < bestR) { bestR = rel; best = h; }
		}
		return best;
	}
}

// The star to measure against. Orbiter supports one lit star in practice and the
// client itself assumes index 0 (Scene.cpp: "generalise later"); we look for a real
// OBJTP_STAR first and fall back to the same assumption, so a non-standard system
// cannot leave us pointing at a planet.
//
// NOT file-static since 2026-08-10: the GOD RAYS need the same sun. "Which body is
// the star" is a POLICY, and if it ever has to change (a binary system, a scenario
// with no OBJTP_STAR at all) it must change in ONE place, or the two effects will
// quietly disagree about where the light is coming from.
OBJHANDLE OroFindStar()
{
	const DWORD n = oapiGetGbodyCount();
	for (DWORD i = 0; i < n; i++) {
		OBJHANDLE h = oapiGetGbodyByIndex((int)i);
		if (h && oapiGetObjectType(h) == OBJTP_STAR) return h;
	}
	return oapiGetGbodyByIndex(0);
}

// ----------------------------------------------------------------------------
// Per frame, MAIN thread. Publishes eclipseObsc / eclipseGain / eclipseBody into
// g_fx and leaves eclActive telling the render path whether there is anything to
// do this frame.
// ----------------------------------------------------------------------------
void OroModule::UpdateEclipse()
{
	const double sysdt = oapiGetSysStep();   // REAL time: invariant 4. An eclipse seen at
	                                         // 100x is still a 20-second dark adaptation
	                                         // for the pilot, and a 1-second glare.

	float obscAll = 0.0f, obscOther = 0.0f;
	char  bodyName[32] = "";

	if (g_fx.masterArmed && g_fx.eclipseTest) {
		// Synthetic cycle - the only way to test this without waiting for an
		// alignment. Deliberately drives BOTH obscurations: the point of the test
		// is to see the dim and the eye together.
		eclTestT += sysdt;
		const double p = fmod(eclTestT, ECL_TEST_PERIOD);
		float f = 0.0f;
		if      (p <  4.0) f = 0.0f;                                 // clear
		else if (p < 14.0) f = sstep((float)((p -  4.0) / 10.0));    // first contact -> totality
		else if (p < 22.0) f = 1.0f;                                 // totality
		else if (p < 28.0) f = 1.0f - sstep((float)((p - 22.0) / 6.0)); // emergence
		obscAll = obscOther = f;
		strcpy_s(bodyName, "TEST");
	} else if (g_fx.eclipseEnabled && g_fx.masterArmed) {
		OBJHANDLE hSun = OroFindStar();
		if (hSun) {
			VECTOR3 cpos, spos;
			oapiCameraGlobalPos(&cpos);
			oapiGetGlobalPos(hSun, &spos);
			const VECTOR3 vS = spos - cpos;
			const double  dS = length(vS);
			const double  Rs = oapiGetSize(hSun);
			if (dS > 1.0 && Rs > 1.0) {
				const VECTOR3 uS = vS / dS;
				const double  aS = asin(clampd(Rs / dS, 0.0, 1.0));   // sun's angular radius
				const OBJHANDLE hPrim = FindPrimary();

				const DWORD n = oapiGetGbodyCount();
				for (DWORD i = 0; i < n; i++) {
					OBJHANDLE h = oapiGetGbodyByIndex((int)i);
					if (!h || h == hSun) continue;
					const double Rb = oapiGetSize(h);
					if (Rb < 1.0) continue;
					VECTOR3 bpos; oapiGetGlobalPos(h, &bpos);
					const VECTOR3 vB = bpos - cpos;
					const double  dB = length(vB);
					if (dB >= dS) continue;        // behind the sun: cannot occult it
					if (dB <= Rb)  continue;       // camera inside the body - nothing sane to say
					const double aB  = asin(clampd(Rb / dB, 0.0, 1.0));
					const double cth = dotp(uS, vB / dB);
					if (cth < cos(aS + aB)) continue;             // cheap angular reject
					const float f = DiscOverlap(aS, aB, acos(clampd(cth, -1.0, 1.0)));
					if (f <= 0.0f) continue;
					// MAX, not sum: two bodies covering the same disc at once is not a
					// thing, and max can never manufacture an eclipse out of two grazes.
					if (f > obscAll) {
						obscAll = f;
						oapiGetObjectName(h, bodyName, sizeof(bodyName));
					}
					if (h != hPrim && f > obscOther) obscOther = f;
				}
			}
		}
	}

	g_fx.eclipseObsc = obscAll;
	strcpy_s(g_fx.eclipseBody, sizeof(g_fx.eclipseBody), bodyName);

	// --- the eye ------------------------------------------------------------
	// `lit` is the light reaching it; `eclAdapt` is what it is currently set up
	// for. The ratio of the two IS the perceived brightness change, and it is
	// self-cancelling: once adaptation catches up the gain is 1.0 and the whole
	// effect steps out of the way.
	const float lit = clampf(1.0f - obscAll * (1.0f - ECL_LIT_FLOOR), ECL_LIT_FLOOR, 1.0f);
	const bool  live = g_fx.masterArmed && (g_fx.eclipseEnabled || g_fx.eclipseTest);

	if (!live || !eclPrimed) {
		// Snap. Two cases, one answer: a fresh session must not open blind because
		// it happens to start on the night side, and enabling the effect mid-shadow
		// must not invent a transient that never happened.
		eclAdapt  = lit;
		eclPrimed = true;
	} else {
		const double tau = (lit < eclAdapt) ? ECL_TAU_DARK : ECL_TAU_LIGHT;
		eclAdapt += (lit - eclAdapt) * (float)(1.0 - exp(-sysdt / tau));
		eclAdapt  = clampf(eclAdapt, ECL_LIT_FLOOR, 1.0f);
	}

	float gain = powf(lit / (eclAdapt > 1e-4f ? eclAdapt : 1e-4f), ECL_GAMMA);
	gain = clampf(gain, ECL_GMIN, ECL_GMAX);
	// The Adaptation knob scales how far the eye is allowed to depart from neutral -
	// so 0 gives a plain physical dimmer and 1 gives the full blindness-and-glare.
	gain = 1.0f + (gain - 1.0f) * clampf(g_fx.eclipseAdapt, 0.0f, 1.0f);

	// The steady dim rides obscOther ONLY (see the header comment). In the TEST
	// cycle both are the same number, which is the point: the test shows the
	// worst case, the one the renderer cannot help with.
	gain *= 1.0f - obscOther * clampf(g_fx.eclipseDim, 0.0f, 1.0f) * ECL_DIM_MAX;

	g_fx.eclipseGain = live ? gain : 1.0f;
	// Colour loss rides obscOTHER, like the dim (see the header comment): tied to obscAll
	// it desaturated every orbital night, which does not self-cancel and read as washed-out
	// night colours (user, 2026-08-05). Now it only bites during a real eclipse by ANOTHER
	// body - the case the renderer does not model. The TEST cycle still drives both equally.
	eclDesat = live ? obscOther * clampf(g_fx.eclipseColour, 0.0f, 1.0f) : 0.0f;

	// Nothing to draw unless the eye is actually away from neutral. Costs a full
	// frame copy + shader pass when true, so the threshold is not decoration.
	eclActive = live && (fabsf(g_fx.eclipseGain - 1.0f) > 0.004f || eclDesat > 0.004f);
}
