// ==============================================================
// OroFog.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - FOG (2026-09-05). THE WEATHER CHAPTER OPENS.
// ----------------------------------------------------------------------------
// ORO draws none of it. Client patch (aa) integrates two analytic height-fog layers in
// every shader family - terrain, base tiles, the five vessel paths, particles, runway
// lights, the sky dome, the cloud layer seen from below - and this file hands it the
// LAYERS (invariant 18's category, beside the VC shadows and the wet ground):
//   layer 0 = the GROUND FOG, the FOG leaf's own event (pill or TEST), a slab standing
//             on the terrain under the vessel with an exponential profile the Fade
//             slider shapes, its density from Visibility (Koschmieder: 3.9 / V);
//   layer 1 = the STORM'S MIST, driven by the rain envelope and its Gloom slider - the
//             distance fog that patch (s) part 2 baked into the two GROUND shaders is
//             this now, so hulls, particles and the sky take it too, and it stands as a
//             uniform slab from the ground to the storm deck, so climbing out of the
//             deck clears it: the cloud crossing, for the storm case.
// The client owns the colour (its own sun colour and daylight at the camera - warm at
// dawn, grey under the deck, dark at night), the sun attenuation and the ambient lift;
// the two taste numbers (Brightness, Sun glow) ride along.
//
// THE SPLIT (invariant 1's law, the rain's precedent): UpdateFog EVOLVES (the envelope
// on real time, the anchor slew on sim time - main thread, frozen under pause), SenseFog
// SENSES (world, air, the ground under the vessel, the camera's altitude - every frame,
// paused included, idempotent), PushFog carries the result to the client ON CHANGE
// wherever the sensing ran (output derived from sensing runs where the sensing runs).
//
// THE ANCHOR. Fog sits on the ground, and "the ground" has to be one number for the
// whole layer: the ground elevation under the vessel, sampled where the fog was summoned
// and re-sampled only after ~10 km of surface travel (the splash lattice's stored-
// reference rule, invariant 28n), then SLEWED to the new value on sim time rather than
// jumped, so a re-anchor is never a visible step. The base sits a few tens of metres
// UNDER that ground, so the apron's own relief and small dips stay inside the layer.
// Known and accepted: fly 10 km up a rising plateau at low level and the fog stays at
// the old base until the re-anchor, then climbs to meet you over a few tens of seconds.
//
// ORO'S OWN GEOMETRY (the plume, the vapour cones, the storm deck, the bolts) is drawn
// after the scene as Sketchpad triangles and sees none of the client's fog, so it takes
// a per-vertex transmittance from FogTransNear() - the layer densities at the CAMERA's
// height, which is exact for anything within a few hundred metres of the eye.
// ============================================================================

#include "OroModule.h"
#include "OroState.h"
#include "gcCoreAPI.h"
#include <math.h>
#include <string.h>

namespace {

	inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }

	// The event envelope runs on REAL time (invariant 4): a fog bank you summon builds
	// over ~20 s; released (TEST off, or the pill off after a TEST), it clears over the
	// same; the PILL going off is instant - the rain's A/B rule, a fade-out makes
	// comparison impossible - and so is Ctrl+G, which is a kill switch.
	const float FOG_BUILD_S   = 20.0f;
	const float FOG_CLEAR_S   = 20.0f;
	const double ANCHOR_REKM  = 10.0;      // re-anchor after this many km of surface travel
	const double ANCHOR_SLEW  = 4.0;       // m/s of sim time the live base may move
	const float GATE_TOP_M    = 25000.0f;  // camera AGL above which the push is zero -
	                                       //   nothing this thin is worth the exps from there
	const float BASE_MARGIN_M = 40.0f;     // the layer base sits this far under the anchor's
	                                       //   ground (see THE ANCHOR above)
	const float MIST_DENS     = 0.00045f;  // the storm mist at full overcast, 1/m - the
	                                       //   constant patch (s) part 2 baked, kept so the
	                                       //   approved rain look reproduces
	const float MIST_ABOVE_M  = 400.0f;    // the mist reaches this far past the deck, so the
	                                       //   punch-through is a fade, not a step

	// EVOLVED (main thread)
	float     s_envT      = 0.0f;          // the ground fog's envelope, 0..1
	bool      s_prevWant  = false;
	bool      s_prevPill  = false;
	bool      s_prevArmed = true;
	double    s_baseLive  = 0.0;           // slewed toward s_baseTarget on sim time

	// SENSED (every frame)
	bool      s_anchored  = false;
	OBJHANDLE s_hRef      = NULL;          // the world the anchor belongs to
	double    s_ancLng = 0.0, s_ancLat = 0.0;
	double    s_baseTarget = 0.0;          // planet radius + anchor ground elevation - margin
	double    s_rBaseStorm = 0.0;          // the mist's base: the ground under the vessel, live
	double    s_camR       = 0.0;          // the camera's geocentric radius
	float     s_cloudAGL   = 7000.0f;      // the planet's cloud layer over the ground
	bool      s_valid      = false;        // a world with air, a vessel on it, the camera in range
	char      s_why[32]    = "";

	// PUSHED (on change - client state, not a frame parameter; invariant 18)
	float  p_dens[2]  = { -1.0f, -1.0f };
	float  p_top[2]   = { -1.0f, -1.0f };
	float  p_scale[2] = { -1.0f, -1.0f };
	double p_base[2]  = { -1.0, -1.0 };
	float  p_bright   = -1.0f, p_glow = -1.0f;
}

// Session boundary: forget the anchor and the envelope, and make the next push
// unconditional. Called at simulation start AND end (23m's rule - a crash must not leave
// last session's world anchored under this one's).
// BASE LIGHTS, the IN WEATHER latch (2026-09-19): whether the weather currently has the
// bases' lights switched on. Hysteresis state, so it lives across frames and is cleared at
// both session boundaries below (23m's rule) - a storm from last session must not leave
// this session's airfield lit.
static bool s_blWxLit = false;

void OroFog_Reset()
{
	s_blWxLit = false;
	s_envT = 0.0f; s_prevWant = false; s_prevPill = false; s_prevArmed = true;
	s_anchored = false; s_hRef = NULL; s_valid = false; s_why[0] = 0;
	for (int i = 0; i < 2; i++) { p_dens[i] = p_top[i] = p_scale[i] = -1.0f; p_base[i] = -1.0; }
	p_bright = p_glow = -1.0f;
	g_fx.fogI = 0.0f; g_fx.fogWhy[0] = 0;
}

// ============================================================================
// UpdateFog - main thread, clbkPreStep. EVOLVES: the envelope and the anchor slew.
// ============================================================================
void OroModule::UpdateFog()
{
	const float dt   = (float)oapiGetSysStep();   // REAL time (invariant 4)
	const bool  pill = g_fx.fogEnabled;
	const bool  want = g_fx.masterArmed && (pill || g_fx.fogTest);

	if (want) {
		s_envT = clampf(s_envT + dt / FOG_BUILD_S, 0.0f, 1.0f);
	} else {
		// the pill going OFF (or the master arm) is instant; a released TEST ramps down
		const bool killed = (s_prevPill && !pill) || (s_prevArmed && !g_fx.masterArmed);
		s_envT = killed ? 0.0f : clampf(s_envT - dt / FOG_CLEAR_S, 0.0f, 1.0f);
	}
	s_prevWant = want; s_prevPill = pill; s_prevArmed = g_fx.masterArmed;
	g_fx.fogI = s_envT;

	// The anchor slew rides SIM time: the world's ground does not move while paused.
	if (s_anchored) {
		const double sdt = oapiGetSimStep();
		const double d = s_baseTarget - s_baseLive;
		const double m = ANCHOR_SLEW * (sdt < 0.0 ? 0.0 : sdt);
		s_baseLive += (d > m) ? m : (d < -m ? -m : d);
	}
}

// ============================================================================
// SenseFog - every frame (clbkPreStep AND clbkProcessKeyboardImmediate, invariant 1's
// paused-gates law). SENSES: which world, is there air, the ground under the vessel,
// the camera's altitude. Idempotent - the double call is a harmless second look.
// ============================================================================
void OroModule::SenseFog()
{
	s_valid = false;
	s_why[0] = 0;

	VESSEL* v = oapiGetFocusInterface();
	if (!v) { strcpy_s(s_why, "no vessel"); return; }
	OBJHANDLE hRef = v->GetSurfaceRef();
	if (!hRef) { strcpy_s(s_why, "no world"); return; }
	if (!oapiPlanetHasAtmosphere(hRef)) { strcpy_s(s_why, "no air here"); return; }
	// The client evaluates the layers against ITS camera proxy; a layer anchored to one
	// world and read against another would be metres under some other planet's crust.
	if (oapiCameraProxyGbody() != hRef) { strcpy_s(s_why, "camera at another world"); return; }

	double lng, lat, rad;
	v->GetEquPos(lng, lat, rad);
	const double size = oapiGetSize(hRef);
	const double elev = v->GetSurfaceElevation();

	if (!s_anchored || s_hRef != hRef) {
		s_anchored = true; s_hRef = hRef;
		s_ancLng = lng; s_ancLat = lat;
		s_baseTarget = size + elev - BASE_MARGIN_M;
		s_baseLive   = s_baseTarget;                // the first anchor is not slewed
	} else {
		// great-circle distance from the anchor, on the planet's own sphere
		double c = sin(lat) * sin(s_ancLat) + cos(lat) * cos(s_ancLat) * cos(lng - s_ancLng);
		c = c < -1.0 ? -1.0 : (c > 1.0 ? 1.0 : c);
		if (acos(c) * size * 0.001 > ANCHOR_REKM) {
			s_ancLng = lng; s_ancLat = lat;
			s_baseTarget = size + elev - BASE_MARGIN_M;   // UpdateFog slews toward it
		}
	}
	s_rBaseStorm = size + elev - BASE_MARGIN_M;         // the mist follows the vessel's ground live

	const double* pCA = (const double*)oapiGetObjectParam(hRef, OBJPRM_PLANET_CLOUDALT);
	s_cloudAGL = (float)((pCA && *pCA > 0.0) ? *pCA : 7000.0);

	VECTOR3 cam, pp;
	oapiCameraGlobalPos(&cam);
	oapiGetGlobalPos(hRef, &pp);
	s_camR = length(cam - pp);
	const float camAGL = (float)(s_camR - (size + elev));
	if (camAGL > GATE_TOP_M) { strcpy_s(s_why, "too high"); return; }
	s_valid = true;
}

// ============================================================================
// PushFog - wherever the sensing ran. Hands the client the two layers and the look ON
// CHANGE, and works out the near-field density for ORO's own geometry.
// ============================================================================
void OroModule::PushFog()
{
	// what the readout says, before any gate
	if (!s_valid)                       strcpy_s(g_fx.fogWhy, s_why);
	else                                g_fx.fogWhy[0] = 0;

	if (!pCore || !pCore->CanSetFogLayer()) { fogNearDens = 0.0f; fogSunColumn = 0.0f; return; }

	const bool on = g_fx.masterArmed && s_valid;

	// LAYER 0 - the ground fog. Visibility -> extinction by Koschmieder; the Fade slider
	// sets the scale height as a fraction of the top: 0 = a solid slab with a hard
	// ceiling, 1 = the density has fallen to ~4% by the top, 2 = a wispy layer that is
	// mostly gone halfway up.
	float dens[2] = { 0.0f, 0.0f }, top[2] = { 0.0f, 0.0f }, scale[2] = { 1.0f, 1.0e5f };
	double base[2] = { 0.0, 0.0 };
	if (on && s_envT > 0.001f) {
		const float vis  = clampf(g_fx.fogVis,  20.0f, 5000.0f);
		const float ftop = clampf(g_fx.fogTop,  20.0f, 3000.0f);
		const float fade = clampf(g_fx.fogFade,  0.0f,    2.0f);
		dens[0]  = (3.9f / vis) * s_envT;
		top[0]   = ftop;
		scale[0] = ftop / (0.25f + 3.0f * fade);
		base[0]  = s_baseLive;
	}
	// LAYER 1 - the storm's mist, on the same numbers the rain hands the client as storm
	// light (rainIntensityLive already carries the rain's altitude/world gate).
	// SNOW (2026-09-13): the same layer carries the falling snow's mist - an order denser
	// than the rain's (moderate snow is ~1-2 km of visibility against the rain's ~9) and
	// driven by what is FALLING (a standing cover on a clear day has no mist) through the
	// snow's own Mist trim. The two storms are mutually exclusive, so the max is simply
	// whichever is on; snowIntensityLive carries the rain's gate like its sibling.
	{
		const float st = on ? clampf(rainIntensityLive * clampf(g_fx.rainGloom, 0.0f, 2.0f) * 0.5f, 0.0f, 1.0f) : 0.0f;
		const float k  = clampf(st * 1.4f, 0.0f, 1.0f);
		// ... on a LOG scale (2026-09-14, The Long Dark reference): a blizzard is a hundred
		// metres of visibility and a light snow two kilometres, twenty times apart, which
		// no linear slider reaches. Mist 0 = none; 0.5 = ~1.3 km; 1 = ~550 m; 1.5 = ~230 m;
		// 2 = 100 m (Koschmieder 3.9 / V), at full snowfall; the first tenth fades it in.
		const float m  = clampf(g_fx.snowMist, 0.0f, 2.0f);
		const float sn = (on && m > 0.001f)
		               ? clampf(snowIntensityLive * clampf(g_fx.snowFall, 0.0f, 1.0f), 0.0f, 1.0f) * clampf(m * 10.0f, 0.0f, 1.0f)
		               : 0.0f;
		const float dS = (3.9f / (3000.0f * powf(1.0f / 30.0f, m * 0.5f))) * sn;
		const float d1 = fmaxf(MIST_DENS * k, dS);
		if (d1 > 1.0e-7f) {
			dens[1] = d1;
			top[1]  = s_cloudAGL + BASE_MARGIN_M + MIST_ABOVE_M;
			base[1] = s_rBaseStorm;
		}
	}

	for (int i = 0; i < 2; i++) {
		const bool moved = fabsf(dens[i] - p_dens[i]) > 1e-6f || fabsf(top[i] - p_top[i]) > 0.01f
		                || fabsf(scale[i] - p_scale[i]) > 0.01f || fabs(base[i] - p_base[i]) > 0.01;
		if (!moved) continue;
		p_dens[i] = dens[i]; p_top[i] = top[i]; p_scale[i] = scale[i]; p_base[i] = base[i];
		pCore->SetFogLayer(i, base[i], top[i], scale[i], dens[i]);
	}
	if (pCore->CanSetFogLook()) {
		const float b = clampf(g_fx.fogBright, 0.0f, 2.0f), g = clampf(g_fx.fogGlow, 0.0f, 2.0f);
		if (fabsf(b - p_bright) > 0.002f || fabsf(g - p_glow) > 0.002f) {
			p_bright = b; p_glow = g;
			pCore->SetFogLook(b, g);
		}
	}

	// The near-field density for ORO's own geometry: each layer's density at the
	// CAMERA's height, zero when the camera is outside the slab.
	float nd = 0.0f, nd0 = 0.0f;
	for (int i = 0; i < 2; i++) {
		if (dens[i] <= 0.0f || top[i] <= 0.0f) continue;
		const float hc = (float)(s_camR - base[i]);
		if (hc < 0.0f || hc > top[i]) continue;
		const float di = dens[i] * expf(-hc / scale[i]);
		nd += di;
		if (i == 0) nd0 = di;
	}
	fogNearDens  = nd;
	fogNearDens0 = nd0;

	// The sun column for the cabin (SenseVCNight): each layer's VERTICAL optical depth
	// from the camera's height to its top - rho0 * H * (exp(-h/H) - exp(-top/H)), the
	// closed form of the exponential profile the client integrates per pixel.
	float col = 0.0f;
	for (int i = 0; i < 2; i++) {
		if (dens[i] <= 0.0f || top[i] <= 0.0f) continue;
		float hc = (float)(s_camR - base[i]);
		if (hc > top[i]) continue;
		if (hc < 0.0f) hc = 0.0f;
		col += dens[i] * scale[i] * (expf(-hc / scale[i]) - expf(-top[i] / scale[i]));
	}
	fogSunColumn = col;
}

// Per-vertex transmittance for ORO's own geometry (render path - no oapi call in here,
// invariant 1). Exact for anything within a few hundred metres of the eye; the
// client's per-pixel integral is what the scene itself gets.
float OroModule::FogTransNear(float dist) const
{
	return fogNearDens > 0.0f ? expf(-fogNearDens * (dist < 0.0f ? 0.0f : dist)) : 1.0f;
}

namespace {
	inline DWORD ScaleAlpha(DWORD c, float T)
	{
		if (T >= 0.999f) return c;
		const DWORD a = (DWORD)((float)(c >> 24) * (T < 0.0f ? 0.0f : T) + 0.5f);
		return (c & 0x00FFFFFFu) | (a << 24);
	}
}
DWORD OroModule::FogColNear(DWORD c, float dist) const
{
	return ScaleAlpha(c, FogTransNear(dist));
}
DWORD OroModule::FogColNearGround(DWORD c, float dist) const
{
	return ScaleAlpha(c, fogNearDens0 > 0.0f ? expf(-fogNearDens0 * (dist < 0.0f ? 0.0f : dist)) : 1.0f);
}

// ============================================================================
// PushBaseLights - patch (ac). ORO draws none of it: the client owns the base's
// day/night flip and its light sprites; this hands it the user's choice and the glow
// gain, on change. Borrow-and-return: the session sweeps push (false, 1, 1).
// ⚠️ STOCK IS STOCK AT ANY SLIDER VALUE (2026-09-17, his ruling). Until then the glow
// gain applied to whatever lights were lit - the stock night state too - so the pill
// off was stock only with the gain at 1. The settings file that shipped in 260913
// carried the glow at 0.41, and every runway light, PAPI, VASI and base night texture
// on every tester's install rendered at 41% (a forum report of "burgundy" PAPI). A
// slider tuned for the fog-halo look must not dim the world's lights when the feature
// it belongs to is off: stock pushes the same (false, 1, 1) the disarm does, the two
// lit modes push the user's sliders.
//
// THREE MODES since 2026-09-19 (triage 37 - the same tester, with the pill on: "they
// seem to be always turned on when they should only be on during the night"; the pill
// WAS the force, and its help had promised weather). STOCK / IN WEATHER / ALWAYS:
// ALWAYS is the old pill; IN WEATHER is the airfield rule the help described - the
// lights switch on when the gloom justifies it and off again when it clears.
// ⚠️ "THE GLOOM" IS READ OFF THE TWO NUMBERS THIS FRAME ALREADY HANDED THE CLIENT, so
// the switch flips on exactly what is on screen and there is no second weather model:
//   - the STORM LIGHT (stormPushed, PushSurfaceWet): the sun collapse the rain or the
//     falling snow pushes, envelope x Gloom x 0.5 through the altitude gate. On past a
//     quarter of the sun - with Gloom at 1 that is half the envelope, five seconds into
//     a storm's ten-second build; a drizzle with Gloom under 0.5 never gets there.
//   - the VISIBILITY from the two fog layers' pushed ground densities (p_dens: the
//     ground fog plus the storm or snow mist), Koschmieder's 3.9 / density. On under
//     5 km. Falling snow gets this for free (its mist at Mist 1 is 550 m); a standing
//     cover on a clear day has no mist and no storm light, and stays stock.
// Either trigger lights them; HYSTERESIS (off again only under 15% of the sun AND over
// 6.5 km) so a hovering envelope cannot flicker; and a FLIP, not a fade - his 09-01
// ruling ("that is a flip, not a ramp"), which is also what the client does with it,
// one texture state per base, at most a second late (vBase::Update checks once a sim
// second). Runs from both sensing sites AFTER PushSurfaceWet and PushFog (the order
// in clbkPreStep and the keyboard tick), so the numbers it reads are this frame's.
// The readout (baseLightsLive) is the instrument: stock / waiting / lit / forced.
// ============================================================================
static const float BL_STORM_ON  = 0.25f;    // storm light (0..1 of the sun taken) to switch on
static const float BL_STORM_OFF = 0.15f;    // ... and to let go
static const float BL_VIS_ON    = 5000.0f;  // m, the fog layers' ground visibility to switch on
static const float BL_VIS_OFF   = 6500.0f;  // ... and to let go

void OroModule::PushBaseLights()
{
	if (!pCore || !pCore->CanSetBaseLights()) { g_fx.baseLightsLive = 0; return; }
	int mode = g_fx.masterArmed ? g_fx.baseLightsMode : 0;   // Ctrl+G hands stock back (18c)
	if (mode < 0 || mode > 2) mode = 0;

	bool force = false;
	if (mode == 2) {
		force = true;
	} else if (mode == 1) {
		const float st = (stormPushed > 0.0f) ? stormPushed : 0.0f;   // -1 = never pushed
		float d = 0.0f;
		for (int i = 0; i < 2; i++) if (p_dens[i] > 0.0f) d += p_dens[i];
		const float vis = (d > 1.0e-7f) ? 3.9f / d : 1.0e9f;
		if (!s_blWxLit) { if (st >= BL_STORM_ON  || vis <= BL_VIS_ON)  s_blWxLit = true;  }
		else            { if (st <= BL_STORM_OFF && vis >= BL_VIS_OFF) s_blWxLit = false; }
		force = s_blWxLit;
	}
	if (mode != 1) s_blWxLit = false;         // the latch belongs to IN WEATHER alone
	g_fx.baseLightsLive = (mode == 2) ? 3 : (mode == 1 ? (force ? 2 : 1) : 0);

	const int   on = force ? 1 : 0;
	const float g  = on ? clampf(g_fx.baseLightsGlow, 0.25f, 3.0f) : 1.0f;
	const float h  = on ? clampf(g_fx.baseLightsHalo, 0.0f, 3.0f) : 1.0f;
	if (on != blPushedOn || fabsf(g - blPushedGlow) > 0.002f || fabsf(h - blPushedHalo) > 0.002f) {
		blPushedOn = on; blPushedGlow = g; blPushedHalo = h;
		pCore->SetBaseLights(on != 0, g, h);
	}
}
