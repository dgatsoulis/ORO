// ==============================================================
// OroRain.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - RAIN (2026-08-20). THE RUNWAY SLICE.
// ----------------------------------------------------------------------------
// The dream, in his words: you are sitting on the runway, the clouds darken, the
// first drops fall, and it builds to a downpour. This is the first slice of that -
// gloom, falling rain, and drops landing in water. External view only for now, Earth
// only, and TRIGGERED rather than found: a storm you can summon is a storm you can
// judge, and where it is allowed to happen is a later question (the lightning's
// CmCoverage already knows where the cloud is).
//
// GREW ACROSS 2026-08-22: wet ground and a wet hull landed as client patch (s), the
// overcast light as its part 2 (the Gloom slider collapses the sun at the source), and
// this file gained the STORM DECK (section 0) and a SECOND splash field centred on the
// focus vessel. Still deliberately absent: lightning at ground level, thunder, and the
// internal view.
//
// ⚠️ CAMERA DISTANCE DECIDED THE ARCHITECTURE, and it was settled before any code.
// The user can put the camera kilometres from the vessel, so rain cannot be one thing
// scaled - it is a different phenomenon at each distance. Inside it you see streaks; at
// a kilometre you see no drops at all, only a grey shaft under a dark cloud base. The
// near field is here; the far shaft is still a separate element for later. Same lesson
// as the eclipse (model the observer) and as the VC glow.
//
// ⚠️ AND ROUND 2 REPLACED THE WORLD-ANCHORED DROP FIELD WITH A SCREEN-SPACE SHEET.
// Round 1 put real drops at real positions in a 75 m box, which is the honest way to do
// it and is arithmetically hopeless: real rain runs to several hundred drops per cubic
// metre, so filling that box takes millions. His verdict was "far too few streaks",
// which is exactly what the arithmetic predicts. The density that makes rain look like
// rain is not reachable that way at any setting.
// So the drops are a SHEET between the viewer and the world - which is the right fake,
// because that is literally where rain is. Three layers at different speeds and sizes
// give back the depth the fake gave away. See the sheet section below for the two
// consequences that had to be accepted knowingly.
//
// THE SPLASH RINGS ARE STILL WORLD-ANCHORED, and they have to be: they sit ON the
// ground, so they must hold their place as the camera moves. Positions are pure
// functions of (slot, epoch), wrapped modulo a box that travels with the camera, with
// the camera's own offset subtracted BEFORE the wrap - so a ring stays where it landed
// instead of sliding with the viewer. Nothing accumulates (G10) and it is pause-correct.
//
// ⚠️ FRAMES: the ring fields are tens to hundreds of metres across and Earth moves
// ~500 m per frame in Orbiter's barycentric frame, so pairing a render-epoch camera with
// a pre-step planet centre would throw them whole field-widths sideways every frame. Positions are built
// relative to the PLANET with its centre taken at the RENDER epoch (patch k2) to match
// the render camera (patch k). Invariant 21(a)/(b) - the bug that cost the trail an
// entire arc, not being rediscovered here.
// ============================================================================

#include "OroModule.h"
#include "OroState.h"
#include "gcCoreAPI.h"       // gcCore::GetRenderObjPos (client patch k2)
#include "XRSound.h"         // UpdateRainSound - the loop crossfade (invariant 12;
                             //   OroModule.h only forward-declares the class)
#include <math.h>
#include <string.h>          // memset (the vertex-buffer tail pad)
#include <vector>            // the rain shield's extracted roof triangles

namespace {

	// Per-file camera helpers. Duplicated across the effect files on purpose - see the
	// note in OroVapour.cpp: a few lines of duplication beats a header dependency between
	// effects that have nothing else to say to each other.
	struct CamCtx { VECTOR3 pos; MATRIX3 rot; double tanAp; };

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

	// 0xAABBGGRR with clamping (invariant 5).
	inline DWORD RCol(int r, int g, int b, int a)
	{
		if (r < 0) r = 0; if (r > 255) r = 255;
		if (g < 0) g = 0; if (g > 255) g = 255;
		if (b < 0) b = 0; if (b > 255) b = 255;
		if (a < 0) a = 0; if (a > 255) a = 255;
		return ((DWORD)a << 24) | ((DWORD)b << 16) | ((DWORD)g << 8) | (DWORD)r;
	}

	// Deterministic 0..1 hash. The soot / lightning idiom: the field is a pure function
	// of its slot index and the clock, so there is no state to seed, drift or leak.
	inline float hashf(int n)
	{
		n = (n << 13) ^ n;
		const int m = (n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff;
		return (float)(m % 4096) * (1.0f / 4096.0f);
	}

	// Wrap v into [-B/2, +B/2). This is what makes the field world-anchored: the camera's
	// own offset is subtracted BEFORE wrapping, so a drop keeps its place in the world
	// while the box slides over it.
	inline float wrapc(float v, float B)
	{
		return v - B * floorf(v / B + 0.5f);
	}

	// TILEABLE value noise on a wrapping power-of-two lattice - the cloud-deck texture's
	// substance. Wrapping the lattice is what makes the texture tile, and tiling is what
	// lets the deck's UVs run unbounded across the planet under the pad's WRAP sampler.
	inline float VNoise(float fx, float fy, int per, int seed)
	{
		const int   xi = (int)floorf(fx), yi = (int)floorf(fy);
		const float tx = fx - (float)xi,  ty = fy - (float)yi;
		const float sx = tx * tx * (3.0f - 2.0f * tx);
		const float sy = ty * ty * (3.0f - 2.0f * ty);
		const int   m  = per - 1;                    // per is a power of two
		auto h = [&](int x, int y) {
			int n = (x & m) * 73856093 ^ (y & m) * 19349663 ^ seed * 83492791;
			n = (n << 13) ^ n;
			return (float)(((n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff) % 4096)
			     * (1.0f / 4096.0f);
		};
		const float a = h(xi, yi),     b = h(xi + 1, yi);
		const float c = h(xi, yi + 1), d = h(xi + 1, yi + 1);
		const float top = a + (b - a) * sx, bot = c + (d - c) * sx;
		return top + (bot - top) * sy;
	}

	// RAIN LIGHTNING (his item 3, part 1): the flash scheduler. A PURE function of
	// (time, slot, rate) - the soot idiom - so the RENDER path (deck brightening) and
	// the MAIN thread (the borrowed scene light) evaluate the SAME storm without
	// sharing any state. Event types per his spec: most are single flashes, some
	// double or triple, and ~1 in 7 is a STROBE - a burst of 5..8 strokes at fast
	// irregular intervals. Strokes combine by MAX (invariant 22d: the same cloud lit
	// again, not two light sources). Real time, so warp cannot strobe the sky and a
	// paused sim keeps flickering (22d again).
	// Part 2 added the EVENT TYPE (the real taxonomy, discussed and mapped):
	//   type 0 = in-cloud only (the majority - the flash we built first);
	//   type 1 = CG bolt: a channel to the ground, re-strikes down the IDENTICAL
	//            channel (the seed picks the atlas slot, so this is free), some with
	//            CONTINUING CURRENT - the slow bolt that hangs instead of snapping;
	//   type 2 = the positive giant: rare, far out, SINGLE stroke, much brighter,
	//            always with the long tail.
	struct RainLtgEv { float az, dist, I; int type, tex; int ep; };
	float RainLtgFlash(double tAnim, int slot, float rate, RainLtgEv* ev)
	{
		ev->I = 0.0f; ev->type = 0; ev->tex = 0; ev->az = 0.0f; ev->dist = 4000.0f;
		ev->ep = 0;
		if (rate <= 0.01f) return 0.0f;
		const float cyc = (14.0f + 22.0f * hashf(slot * 97 + 11)) / rate;
		const float u   = (float)(tAnim / (double)cyc) + hashf(slot * 97 + 3);
		const int   ep  = (int)floorf(u);
		ev->ep = ep;                                  // the THUNDER scheduler keys on
		                                              // epoch EDGES: new epoch = new event
		const float tt  = (u - (float)ep) * cyc;      // seconds into this slot's cycle
		const int   seed = slot * 131071 + ep * 8191;
		ev->az   = hashf(seed + 1) * 6.2831853f;
		ev->dist = 2000.0f + 7000.0f * hashf(seed + 2);
		ev->tex  = (int)(15.99f * hashf(seed + 5));
		const float hty = hashf(seed + 60);
		if      (hty < 0.68f) ev->type = 0;
		else if (hty < 0.95f) ev->type = 1;
		else                { ev->type = 2; ev->dist = 4500.0f + 4500.0f * hashf(seed + 2); }
		const float ty = hashf(seed + 3);
		int nStk;
		if      (ev->type == 2) nStk = 1;             // the giant is one long stroke
		else if (ty < 0.55f) nStk = 1;
		else if (ty < 0.85f) nStk = 2 + (hashf(seed + 4) > 0.5f ? 1 : 0);
		else                 nStk = 5 + (int)(3.99f * hashf(seed + 4));
		const float gAmp = (ev->type == 2) ? 1.55f : 1.0f;
		// continuing current: the LAST stroke of some CG events, and always the giant,
		// holds a decaying glow for a few hundred ms - the "slow" bolt
		const bool  tail = (ev->type == 2) || (ev->type == 1 && hashf(seed + 70) < 0.45f);
		float I = 0.0f, t0 = 0.0f;
		for (int s = 0; s < nStk; s++) {
			if (s) t0 += 0.045f + 0.24f * hashf(seed + 10 + s * 7);
			const float dt = tt - t0;
			if (dt >= 0.0f && dt < 0.9f) {
				const float atk = 0.014f;
				float env = (dt < atk) ? (dt / atk)
				          : expf(-(dt - atk) * (9.0f + 6.0f * hashf(seed + 30 + s)));
				if (tail && s == nStk - 1 && dt >= atk) {
					const float slow = 0.30f * expf(-(dt - atk) * 3.0f);
					if (slow > env) env = slow;
				}
				const float amp = (0.55f + 0.45f * hashf(seed + 50 + s)) * gAmp;
				if (env * amp > I) I = env * amp;
			}
		}
		ev->I = I;
		return I;
	}

	// the STRIKE test rig's envelope: IDENTICAL every press - three strokes and a
	// continuing-current tail, ~1.2 s - so the sixteen textures compare fairly.
	float RainLtgTestEnv(float age)
	{
		if (age < 0.0f || age > 1.6f) return 0.0f;
		static const float T0[3] = { 0.0f, 0.12f, 0.31f };
		float I = 0.0f;
		for (int s = 0; s < 3; s++) {
			const float dt = age - T0[s];
			if (dt < 0.0f) continue;
			float env = (dt < 0.014f) ? dt / 0.014f : expf(-(dt - 0.014f) * 10.0f);
			if (s == 2 && dt >= 0.014f) {
				const float slow = 0.30f * expf(-(dt - 0.014f) * 2.6f);
				if (slow > env) env = slow;
			}
			if (env > I) I = env;
		}
		return I;
	}

	// The main-thread -> render-path handoff (invariant 1: the render path makes no oapi
	// calls, so everything it needs about the world arrives here).
	struct RainSnap {
		OBJHANDLE hRef;        // the planet, for the render-epoch centre (patch k2)
		VECTOR3   pC;          // its centre, PRE-STEP - the fallback without (k2)
		VECTOR3   axis;        // its spin axis, global: the stable horizontal basis
		double    groundR;     // planet centre -> ground, under the vessel
		float     intensity;   // 0..1, the event envelope
		float     wet;         // 0..1, the lagging ground wetness (build B will use it)
		OBJHANDLE hV;          // the focus vessel - the SECOND splash field centres on it
		VECTOR3   vPos;        // its position, PRE-STEP (k2 overrides in the build)
		float     dayF;        // 0..1 daylight at the camera - the deck is lit by the sky
		float     lightF;      // 0..1 LIGHT ON THE RAIN near the camera (2026-09-05, his
		                       //   note: "falling raindrops are not visible at night unless
		                       //   you shine a light on them") - the sky's daylight plus
		                       //   the focus vessel's own lamps sampled along the view; the
		                       //   storm's flashes join in the build (the pure scheduler)
		double    vSize;       // hull bounding radius - the splash footprint mask (a hull
		                       //   is an umbrella; the ground under it stays mostly dry)
		VECTOR3   pax1;        // the planet's OWN x axis, global - with `axis` it gives a
		                       //   PLANET-FIXED lon/lat frame for the deck texture's UVs,
		                       //   so the cloud cover rotates with the ground it rains on
		MATRIX3   vRot;        // the focus vessel's rotation - the rain shield's roof
		                       //   triangles live in the VESSEL frame (attitude is slow,
		                       //   so a pre-step rotation is epoch-safe; POSITION is not,
		                       //   and takes patch k2 in the build)
		VECTOR3   vAirG;       // the vessel's airspeed vector, GLOBAL frame (2026-08-27):
		                       //   the sheet's fall vector is rain RELATIVE TO THE VESSEL,
		                       //   so streaks slant aft and rush at speed - invariant
		                       //   25(e) reaching the last effect that predated it
		float     cloudAGL;    // the planet's own cloud-layer altitude above the LOCAL
		                       //   ground - where the storm deck pins so you can fly
		                       //   through it (his design, 2026-08-27)
	};
	// (WATER ON THE HULL lived here 2026-08-22, for one day: rivulets, drips and pour
	//  streamers off a dripline grid. Three rounds could not make it read as water -
	//  "small puffs of smoke", strips "in the wrong places" - and HIS CALL was to cut
	//  it whole: "a small miss on an otherwise excellent effect." The reusable lesson
	//  survives in SampleHullPoints and in the graveyard; do not rebuild the feature
	//  without a new idea for what a stream of water LOOKS like at these pixel sizes.)
	RainSnap s_rn;
	bool     s_rnValid = false;
	// ⚠️ THE GATE FACTOR FOR CLIENT-SIDE PUSHES (2026-08-22, his reentry screenshot).
	// The altitude gate silenced the DRAWN geometry but PushSurfaceWet rode the raw
	// envelope - so a reentry started with the rain pill still on had a glinting,
	// wet-darkened hull in ORBIT. Every push now multiplies by this: 0 on any early
	// exit (above the weather, wrong world, no surface), altF otherwise, so climbing
	// through the deck dries the world out and descent re-wets it.
	float    s_gateF = 0.0f;

	// --- THE RAIN SHIELD (2026-08-23, his design after the first VC storm) -----
	// "A roof plane, under which the rain effect doesn't render." One depth bound
	// cannot describe INDOORS for a vessel with a real interior (his screenshots:
	// rain on the DG's passenger seats, rain through the whole XR2 cabin), because
	// inside/outside is a SHAPE, not a scalar. So the shape is AUTHORED:
	// Meshes\ORO\<class>_rainshield.msh - the heatshield/bell pattern, file
	// presence = opt-in, absent = the depth bound alone. Every triangle is a roof
	// panel in VESSEL coordinates; the sheet build places each streak's rain at
	// its depth along its own ray and casts straight UP (vessel +Y): a panel
	// within RAINSHIELD_H overhead = indoors, streak killed. The height cap is
	// the second half of the trick - rain BELOW the belly seen through a side
	// window has the entire hull overhead, but at 4-5 m, and that is outdoors.
	// Extracted as OUR OWN triangle copies at focus-class change (23m: the mesh
	// HANDLE is session-scoped and never cached; the floats are ours), and the
	// class tag clears at every session start so the mesh can be iterated on
	// between runs.
	const float RAINSHIELD_H = 3.5f;   // m: how far overhead a roof still seals
	// The VC depth bound - DECOUPLED from the VC shadows' cabin box (2026-08-23,
	// his report: tuning shadow sharpness on the XR2 dragged the rain boundary
	// to ~1 m and it rained on the panels; one knob, two meanings, both wrong).
	// 2.5 m clears a fighter-class cockpit; anything larger is the SHIELD's job,
	// not this number's.
	const float RAIN_VC_Z = 2.5f;
	std::vector<VECTOR3> s_shieldTri;  // vessel frame, 3 verts per triangle
	char s_shieldClass[64] = "";

	// (THE RAIN GLASS discovery lived here for a few hours on 2026-08-26 - a per-class
	//  RainGlassMesh cfg key, a text parse, and a gcCore::SetRainGlass push. His call
	//  killed it the same day: "we want this to work for ANY vessel that has a VC mesh
	//  group with RAIN 1". It does now, with NO ORO involvement at all: the CLIENT is
	//  the one party told every preloaded mesh's filename (clbkStoreMeshPersistent),
	//  so the client reads the author's `RAIN 1` tokens itself and instances inherit
	//  the group list at construction. See RainGlassStoreScan in the client's Mesh.cpp.
	//  The addon's whole contribution is the DRAWING: the sign test in PSGloom.)

	// --- THE EVENT ENVELOPE ---------------------------------------------------
	// REAL time, not sim time. Two reasons, and they are the eclipse TEST's reasons: a
	// storm you trigger is something you are watching rather than something the world is
	// doing, and at 100x time acceleration a sim-time arc would be over before the frame
	// after you pressed the button. When rain becomes weather rather than a test - found
	// in the cloud map instead of summoned - the ARRIVAL of a cell will be sim time and
	// this envelope will still be real, exactly as the lightning splits them (22d).
	const float RAIN_RISE = 10.0f;   // s, off -> full downpour
	const float RAIN_FALL = 16.0f;   // s, and it stops faster than the ground dries
	const float WET_RISE  = 22.0f;   // s, ground soaking up
	// (GLASS_RISE lived here as a 16 s constant for a few hours on 2026-08-26; the fill
	//  time is his slider now - g_fx.rainGlassRise, 10..60 s, read in UpdateRain.)
	const float WET_FALL  = 150.0f;  // s, drying out

} // namespace

// Session-boundary reset (the 23m sweep, OroBell_Reset's sibling): forget the
// class so the next storm re-probes the shield mesh - he iterates on the file
// between runs, and a stale cache would show him last session's roof.
void OroRain_ShieldReset() { s_shieldClass[0] = 0; s_shieldTri.clear(); }

// ============================================================================
// UpdateRain - main thread, from clbkPreStep. Owns the envelope, the gates and the
// snapshot. Publishes rainI / rainWet / rainWhy for the panel.
// ============================================================================
void OroModule::UpdateRain()
{
	// (s_rnValid / s_gateF are reset by SenseRain, which owns them now - see the split
	//  note above that function. This half only EVOLVES the storm; it decides nothing
	//  about where or whether it can be drawn.)
	const float dt = (float)oapiGetSysStep();   // REAL time (invariant 4)
	const bool  want = (g_fx.rainEnabled || g_fx.rainTest) && g_fx.masterArmed;

	// The deck's cloud texture follows the Cloud detail notch (0 = the plain Gouraud
	// deck, 1/2/3 = 256/512/1024 texels). Rebuilt only when the slider lands on a NEW
	// notch; the POLY goes first because it holds the texture bound (23l ordering).
	// Main thread, device resources; the shutdown proc resets rainCloudBuilt with them.
	// THE STRIKE BUTTON (his test rig): consume the click, advance the cycle, stamp
	// the fire time; the render path and the light manager read the members.
	if (g_fx.boltTestFire) {
		g_fx.boltTestFire = false;
		g_fx.boltTestSlot = (g_fx.boltTestSlot + 1) % 16;
		boltTestT0 = animT;
	}

	// the bolt atlas: one probe per session; missing file = flashes only, logged once
	if (!boltTexTried && pCore && pCore->CanDrawTexPoly()) {
		boltTexTried = true;
		hBoltTex = oapiLoadTexture("ORO\\bolt_atlas.dds");
		oapiWriteLogV("ORO: rain bolt atlas %s.",
		              hBoltTex ? "loaded (16 slots)" : "missing - in-cloud flashes only");
	}
	if (pCore && pCore->CanDrawTexPoly()) {
		const int lvl = (int)(clampf(g_fx.rainCloudLvl, 0.0f, 3.0f) + 0.5f);
		if (lvl != rainCloudBuilt) {
			if (hRainDeckPoly) { pCore->DeletePoly(hRainDeckPoly); hRainDeckPoly = NULL; }
			if (hRainCloudTex) { oapiDestroySurface(hRainCloudTex); hRainCloudTex = NULL; }
			rainCloudN = 0;
			if (lvl > 0) BuildRainCloudTex(128 << lvl);   // 256 / 512 / 1024
			rainCloudBuilt = lvl;
		}
	}

	// The envelope runs whether or not we can DRAW it, so that walking out of the gate
	// and back does not restart the storm from zero. Same reasoning as the bell glow's
	// thermal model: the metal is hot whether or not anyone is looking (23f).
	// ⚠️ SWITCHING IT OFF IS INSTANT; ONLY THE BUILD-UP RAMPS (2026-08-22, his call).
	// A weather event that faded out over sixteen seconds was the honest behaviour and the
	// wrong tool: the pill is how you A/B the wet ground against the dry one, and you
	// cannot compare two things when one of them takes a quarter of a minute to arrive.
	// The build-up still ramps, because that is the part you WATCH. When rain becomes
	// found-in-the-cloud-map rather than summoned, its natural end can fade again - that
	// is a different event from a person reaching for the off switch.
	if (!want) {
		g_fx.rainI = 0.0f;
		g_fx.rainWet = 0.0f;
		g_fx.rainGlassWet = 0.0f;
	} else {
		g_fx.rainI = clampf(g_fx.rainI + dt / RAIN_RISE, 0.0f, 1.0f);
	}

	// Wetness LAGS the rain going UP, and that lag is what makes the ground darken behind
	// the storm arriving rather than with it.
	// ⚠️ This is accumulated state, which is G10's alarm - but G10 buried a POOL of
	// independent samples that could each go stale and amplify through their neighbours.
	// One self-decaying scalar is the bell glow's thermal model, which has been correct
	// for months. Different thing.
	if (want && g_fx.rainWet < g_fx.rainI)
		g_fx.rainWet = clampf(g_fx.rainWet + dt / WET_RISE, 0.0f, g_fx.rainI);

	// THE GLASS FILLS BEHIND THE STORM TOO (2026-08-26, his ask) - rainWet's exact shape
	// with its own clock, because a canopy collects faster than concrete soaks. The
	// COVERAGE follows this scalar, so drops pop in one by one (each cell's hash is its
	// birth order) rather than the whole field arriving with the first gust. Same rules
	// as the ground: rises toward the event, frozen under pause (no sim step = nothing
	// accumulates), and the pill snaps it to zero - the A/B rule above.
	// The clock is HIS SLIDER (Build up, 10..60 s) - a rare case where a time constant
	// stays a control: how long a windscreen takes to fill is taste, not physics the sim
	// knows better (the 25(i) rule: the user owns the LOOK's bounds).
	if (want && g_fx.rainGlassWet < g_fx.rainI) {
		float gr = g_fx.rainGlassRise; if (gr < 10.0f) gr = 10.0f; if (gr > 60.0f) gr = 60.0f;
		g_fx.rainGlassWet = clampf(g_fx.rainGlassWet + dt / gr, 0.0f, g_fx.rainI);
	}
}

// ============================================================================
// SENSE THE RAIN - where we are, and whether the storm can be drawn from here.
//
// ⚠️ SPLIT OUT OF UpdateRain 2026-08-24, AND THE SPLIT IS THE WHOLE FIX. A public-beta
// tester found three separate symptoms with one cause: pause near the ground and fly the
// camera to orbit and you keep the grey sky and the raindrops in space; pause in an
// external view, switch to the VC, and the drops are inside the cabin. Both are this
// function's answers going STALE, because it used to live in UpdateRain, and
// clbkPreStep is not called while paused (Orbiter.cpp's UpdateWorld: "if (bRunning)
// ModulePreStep()").
//
// THE LINE IS: WHAT SENSES THE WORLD RUNS EVERY FRAME; WHAT EVOLVES OVER TIME DOES NOT.
// Everything here is a question about NOW - which view, which world, how high is the
// camera, is the sun up - so it is re-asked every frame from clbkProcessKeyboardImmediate
// (see OroModule.cpp), which the core calls whether or not the sim is running. Nothing
// here accumulates, so calling it twice in a frame is harmless and that is what makes the
// split safe. The storm's own build-up, and the wetness soaking in behind it, stay in
// UpdateRain and stay frozen under pause - HIS RULE, and the better one: no sim time
// passes, so nothing should get wetter.
// ============================================================================
void OroModule::SenseRain()
{
	s_rnValid = false;
	s_gateF   = 0.0f;
	g_fx.rainWhy[0] = 0;
	rainIntensityLive = 0.0f;
	rainGateLive = 0.0f;
	rainGlassOK = false;
	if (g_fx.rainI <= 0.002f) return;

	// EXTERNAL, or the VIRTUAL COCKPIT (2026-08-23 - rainVC embeds the depth-clip
	// requirement; see its comment in clbkPreStep). When the VC is the view and the
	// blocker is the missing depth buffer, SAY SO (invariant 20g: the degradation
	// must not be silent) - "external only" would send someone outside when the fix
	// is turning Sun glare on.
	if (!extGate && !rainVC && !rainPanel) {
		const bool vcv = oapiCameraInternal() && (oapiCockpitMode() == COCKPIT_VIRTUAL);
		strcpy_s(g_fx.rainWhy, sizeof(g_fx.rainWhy), vcv ? "VC: SunGlare off" : "external only");
		return;
	}

	VESSEL* v = oapiGetFocusInterface();
	if (!v) return;
	OBJHANDLE hRef = v->GetSurfaceRef();
	if (!hRef) { strcpy_s(g_fx.rainWhy, sizeof(g_fx.rainWhy), "no surface"); return; }

	// EARTH ONLY for v1 (his call). When other worlds arrive it will be a line in the
	// per-body cfg beside the aurora's, not a code change - invariant 17's third scope.
	char rname[64] = "";
	oapiGetObjectName(hRef, rname, 64);
	if (_stricmp(rname, "Earth") != 0) {
		strcpy_s(g_fx.rainWhy, sizeof(g_fx.rainWhy), "Earth only");
		return;
	}

	// THE RAIN SHIELD's per-class extract (see the block at the top). Main thread,
	// the heatshield's exact probe: sanitized name, file-presence check, template
	// loaded into a LOCAL and walked immediately - never cached (23m).
	{
		char cls[64]; OroClassFileName(v->GetClassNameA(), cls, sizeof(cls));
		if (_stricmp(cls, s_shieldClass) != 0) {
			strcpy_s(s_shieldClass, sizeof(s_shieldClass), cls);
			s_shieldTri.clear();
			char mfile[160]; sprintf_s(mfile, "Meshes\\ORO\\%s_rainshield.msh", cls);
			if (GetFileAttributesA(mfile) != INVALID_FILE_ATTRIBUTES) {
				char mres[96]; sprintf_s(mres, "ORO\\%s_rainshield", cls);
				MESHHANDLE hM = oapiLoadMeshGlobal(mres);
				if (hM) {
					const DWORD ng = oapiMeshGroupCount(hM);
					for (DWORD g = 0; g < ng; g++) {
						MESHGROUP* gr = oapiMeshGroup(hM, g);
						if (!gr || !gr->Vtx || !gr->Idx) continue;
						for (DWORD i = 0; i + 2 < gr->nIdx; i += 3) {
							s_shieldTri.push_back(_V(gr->Vtx[gr->Idx[i]].x,     gr->Vtx[gr->Idx[i]].y,     gr->Vtx[gr->Idx[i]].z));
							s_shieldTri.push_back(_V(gr->Vtx[gr->Idx[i + 1]].x, gr->Vtx[gr->Idx[i + 1]].y, gr->Vtx[gr->Idx[i + 1]].z));
							s_shieldTri.push_back(_V(gr->Vtx[gr->Idx[i + 2]].x, gr->Vtx[gr->Idx[i + 2]].y, gr->Vtx[gr->Idx[i + 2]].z));
						}
					}
				}
				oapiWriteLogV("ORO: rain shield %s - %d roof triangle(s) (%s).",
				              mfile, (int)(s_shieldTri.size() / 3), v->GetName());
			}
		}
	}

	// (The rain-glass discovery that lived here moved WHOLLY into the client on the
	//  same day it was written - see the tombstone note at the top of this file. The
	//  client logs `D3D9: rain glass - N RAIN group(s) in <mesh>` at mesh load, which
	//  is the line to look for when drops are missing on a hull.)

	VECTOR3 pC; oapiGetGlobalPos(hRef, &pC);
	VECTOR3 vp; v->GetGlobalPos(vp);
	// ⚠️ ALTMODE_GROUND, NOT THE NO-ARGUMENT OVERLOAD. GetAltitude() returns altitude
	// above the MEAN RADIUS - the SDK header says so and points here for altitude above
	// TERRAIN. With the mean radius the splash rings land at SEA LEVEL, which is why they
	// were visible on water and buried under the concrete on a runway a few metres above
	// it. The two screenshots said exactly that before the header confirmed it.
	const double groundR = length(vp - pC) - v->GetAltitude(ALTMODE_GROUND);

	VECTOR3 cam; oapiCameraGlobalPos(&cam);
	const double camAGL = length(cam - pC) - groundR;

	// ALTITUDE IS PHYSICS, NOT BUDGET - the same ruling as the vapour cone's density gate
	// (25f) and the god rays' vacuum gate (24b). Rain is a BELOW-DECK phenomenon: climb
	// through the cloud and you should be looking at cloud tops, not at falling drops.
	// That makes it the exact complement of the lightning, which is above-deck only (22g).
	const float altF = 1.0f - clampf(((float)camAGL - 5000.0f) / 4000.0f, 0.0f, 1.0f);
	if (altF <= 0.001f) { strcpy_s(g_fx.rainWhy, sizeof(g_fx.rainWhy), "above the weather"); return; }
	if (camAGL < -50.0)  return;                    // under the terrain: nothing to draw

	MATRIX3 prot; oapiGetRotationMatrix(hRef, &prot);

	// Daylight at the camera, for the deck's own brightness: a storm ceiling is lit by
	// the sky above it, so at night it must go near-black rather than glow grey.
	float dayF = 1.0f;
	{
		OBJHANDLE hS = OroFindStar();
		if (hS) {
			VECTOR3 sp; oapiGetGlobalPos(hS, &sp);
			const VECTOR3 upC = unit(cam - pC);
			const float elev = (float)dotp(unit(sp - cam), upC);
			dayF = clampf((elev + 0.04f) / 0.22f, 0.0f, 1.0f);
		}
	}

	// LIGHT ON THE RAIN (2026-09-05, his note). The sheet and the splashes are lit by
	// whatever lights the air a few metres in front of the eye: the sky by day, and at
	// night only what the focus vessel's own LAMPS put there - a landing light aims
	// down the runway, a docking light ahead, the cabin flood barely reaches the glass.
	// Each active emitter this VIEW can see (VIS_COCKPIT inside, VIS_EXTERNAL outside,
	// ALWAYS both) is evaluated with Orbiter's own attenuation (1/(a0 + a1 d + a2 d^2),
	// range-cut) and the spot's cone at three points along the view direction - 2, 6
	// and 14 m, where the three parallax layers actually sit - and the brightest wins.
	// The storm's own flash light is skipped here: the build reads the flash from the
	// pure scheduler, so it lights the rain paused too, and from the VC as well (the
	// borrowed scene light is VIS_EXTERNAL).
	float lightF = dayF;
	{
		VECTOR3 cdir; oapiCameraGlobalDir(&cdir);
		MATRIX3 vR; v->GetRotationMatrix(vR);
		const bool inside = oapiCameraInternal();
		const DWORD nle = v->LightEmitterCount();
		float best = 0.0f;
		for (DWORD i = 0; i < nle; i++) {
			const LightEmitter* le = v->GetLightEmitter(i);
			if (!le || le == rainLtgLight || !le->IsActive()) continue;
			const int vis = (int)le->GetVisibility();
			if (inside  && !(vis & LightEmitter::VIS_COCKPIT))  continue;
			if (!inside && !(vis & LightEmitter::VIS_EXTERNAL)) continue;
			const LightEmitter::TYPE ty = le->GetType();
			if (ty != LightEmitter::LT_POINT && ty != LightEmitter::LT_SPOT) continue;
			const PointLight* pl = (const PointLight*)le;
			const COLOUR4& cd = le->GetDiffuseColour();
			float cmax = cd.r; if (cd.g > cmax) cmax = cd.g; if (cd.b > cmax) cmax = cd.b;
			const float I0 = (float)le->GetIntensity() * cmax;
			if (I0 <= 0.001f) continue;
			const double* att = pl->GetAttenuation();
			const double range = pl->GetRange();
			const VECTOR3 lp = vp + mul(vR, le->GetPosition());
			VECTOR3 ld = _V(0, 0, 1);
			double cosU = -2.0, cosP = -2.0;
			if (ty == LightEmitter::LT_SPOT) {
				const SpotLight* sl = (const SpotLight*)le;
				ld = unit(mul(vR, sl->GetDirection()));
				cosU = cos(sl->GetUmbra() * 0.5);
				cosP = cos(sl->GetPenumbra() * 0.5);
			}
			static const double PROBE[3] = { 2.0, 6.0, 14.0 };
			for (int k = 0; k < 3; k++) {
				const VECTOR3 P = cam + cdir * PROBE[k];
				const VECTOR3 dv = P - lp;
				const double d = length(dv);
				if (d < 1e-3 || (range > 0.0 && d > range)) continue;
				double f = 1.0 / (att[0] + att[1] * d + att[2] * d * d);
				if (range > 0.0 && d > 0.8 * range) f *= (range - d) / (0.2 * range);
				if (ty == LightEmitter::LT_SPOT) {
					const double c = dotp(dv / d, ld);
					if (c <= cosP) continue;
					if (c < cosU && cosU > cosP) f *= (c - cosP) / (cosU - cosP);
				}
				const float L = I0 * (float)f;
				if (L > best) best = L;
			}
		}
		lightF += best * 0.6f;
		if (lightF > 1.0f) lightF = 1.0f;
	}

	s_rn.hV        = v->GetHandle();
	s_rn.vPos      = vp;
	v->GetRotationMatrix(s_rn.vRot);
	{
		VECTOR3 wg = _V(0, 0, 0);
		v->GetAirspeedVector(FRAME_GLOBAL, wg);
		s_rn.vAirG = wg;
		// The deck's ceiling: the planet's own cloud altitude (the lightning's source,
		// OroLightning.cpp), converted to height above the LOCAL ground and floored so
		// odd terrain can never pin the deck below its designed 1400 m.
		const double* pCA = (const double*)oapiGetObjectParam(hRef, OBJPRM_PLANET_CLOUDALT);
		float cAGL = (float)(((pCA && *pCA > 0.0) ? *pCA : 7000.0)
		                     + oapiGetSize(hRef) - groundR);
		s_rn.cloudAGL = (cAGL < 2500.0f) ? 2500.0f : cAGL;
	}
	s_rn.dayF      = dayF;
	s_rn.lightF    = lightF;
	s_rn.hRef      = hRef;
	s_rn.pC        = pC;
	s_rn.axis      = _V(prot.m12, prot.m22, prot.m32);   // planet +Y in global = spin axis
	s_rn.pax1      = _V(prot.m11, prot.m21, prot.m31);   // planet +X - the lon=0 reference
	s_rn.groundR   = groundR;
	s_rn.intensity = g_fx.rainI * altF;
	s_rn.wet       = g_fx.rainWet;
	s_rn.vSize     = v->GetSize();
	s_gateF        = altF;
	rainGateLive   = altF;   // the gate alone, for what the storm DEPOSITED (glass drops)
	s_rnValid      = true;

	// --- THE GLASS (2026-08-26): what pushes a drop sitting on the canopy ---------
	// SENSING, so it belongs here and it runs every frame, paused included (invariant
	// 1's law). GRAVITY AND AIRFLOW ARE SUMMED AS FORCES in the VESSEL frame: parked,
	// the run direction is straight down the glass; at ~20 m/s the two terms are equal;
	// in flight it is essentially straight aft. A sum rather than a blend means there is
	// no threshold to tune and no airspeed at which the direction jumps.
	// ⚠️ Bound to GetAirspeedVector, never to a hull axis - invariant 25(e). A tail-sitter
	// on hover engines gets drops running over its canopy the way its own air actually
	// moves, and nothing here had to know that such a vessel exists.
	rainGlassRot = s_rn.vRot;
	{
		VECTOR3 wind = _V(0, 0, 0);
		v->GetAirspeedVector(FRAME_LOCAL, wind);                 // already vessel frame
		const VECTOR3 downV = tmul(s_rn.vRot, unit(pC - vp));    // toward the planet centre
		const VECTOR3 run   = downV * 9.81 - wind * 0.5;
		const double  rl    = length(run);
		rainGlassRun    = (rl > 1e-6) ? run / rl : downV;
		rainGlassRunMag = (float)rl;   // the RUNNERS' speed source: ~9.8 parked (gravity
		                               //   alone), growing with airspeed - so streaks
		                               //   crawl on the pad and whip aft in flight, with
		                               //   no threshold anywhere (invariant 25e)
		rainGlassOK  = true;
	}

	// ⚠️ PUBLISH THE INTENSITY ONLY - THE SLIDER IS APPLIED IN THE RENDER PATH.
	// The first build folded g_fx.rainGloom in here, and here is clbkPreStep, which DOES
	// NOT RUN WHILE PAUSED. So dragging the Gloom slider in a paused sim changed nothing
	// and the control looked dead - which is precisely the bug the whole 2026-08-15 pause
	// session existed to remove (invariant 1). Everything else on this tab already reads
	// g_fx live in the render path and therefore already responded while paused; gloom was
	// the one value I precomputed, and it was the one control he reported as inert.
	rainIntensityLive = s_rn.intensity;
}

// ============================================================================
// BuildRainCloudTex - MAIN THREAD, once per session. Synthesizes the deck's tileable
// billow texture: a 4-octave luminance field (the billows) and an independent 3-octave
// coverage field (the raggedness) packed as 0xAABBGGRR (the lightning atlas's format).
// ⚠️ OAPISURFACE_ALPHA IS LOAD-BEARING - the recorded 23(j) landmine, now on the path
// it was recorded FOR: the deck draws ALPHA-BLENDED, so without the flag the surface
// has no alpha channel and the ragged coverage becomes an opaque square sky.
// ============================================================================
void OroModule::BuildRainCloudTex(int N)
{
	// Round 7 ("a bit blurry... crispier"), round 8 harder still, round 13 the NOTCH:
	// N texels (256/512/1024 - the Cloud detail slider), six octaves capped at the
	// resolution, steep curves - contrast draws a billow's edge; resolution keeps it
	// from smearing.
	for (int y = 0; y < N; y++) {
		for (int x = 0; x < N; x++) {
			const float fx = (float)x * (1.0f / (float)N);
			const float fy = (float)y * (1.0f / (float)N);
			// THE NOTCHES DIFFER IN CONTENT, NOT JUST SAMPLING (round 13: "not seeing
			// much difference between the settings"): every notch used to hold the
			// SAME six octaves, so 256 vs 1024 was a sharpness nuance. The octave
			// count now scales with N - the finest luminance cell stays ~4 texels,
			// the coverage keeps notch 3 at the approved 4..32 - so each notch ADDS
			// a layer of finer cloud filigree: 256 = soft blobby masses, 512 = the
			// middle, 1024 = the full torn detail, bit-identical to the look he
			// signed off in rounds 8-12.
			float n = 0.0f, amp = 0.5f; int per = 8;
			for (int o = 0; o < 8 && per * 4 <= N; o++) {
				n += amp * VNoise(fx * (float)per, fy * (float)per, per, 7 + o);
				amp *= 0.5f; per <<= 1;
			}
			float c2 = 0.0f; amp = 0.6f; per = 4;
			for (int o = 0; o < 8 && per * 32 <= N; o++) {
				c2 += amp * VNoise(fx * (float)per, fy * (float)per, per, 91 + o);
				amp *= 0.5f; per <<= 1;
			}
			// luminance: billow mottling around a mid grey (the vertex colour carries
			// the hue and the gloom; the texture carries the STRUCTURE)
			const float lum = clampf(0.55f + 1.75f * (n - 0.47f), 0.13f, 1.18f);   // round 8: darker bottoms
			int v = (int)(165.0f * lum); if (v > 255) v = 255;
			// coverage: mostly solid, ragged edges TORN rather than faded - the steep
			// slope is what makes a cloud boundary a boundary
			const float cov = clampf(0.34f + 4.20f * (c2 - 0.40f), 0.12f, 1.0f);   // round 8: harder edges
			const int a = (int)(255.0f * cov);
			rainCloudImg[y * N + x] =
				((DWORD)a << 24) | ((DWORD)v << 16) | ((DWORD)v << 8) | (DWORD)v;
		}
	}
	hRainCloudTex = oapiCreateSurfaceEx(N, N,
		OAPISURFACE_TEXTURE | OAPISURFACE_ALPHA | OAPISURFACE_NOMIPMAPS);
	if (hRainCloudTex && !pCore->UpdateTexture2D(hRainCloudTex, rainCloudImg, N, N)) {
		oapiDestroySurface(hRainCloudTex);
		hRainCloudTex = NULL;
	}
	rainCloudN = hRainCloudTex ? N : 0;
	oapiWriteLogV("ORO: rain cloud-deck texture %s (%d px).",
	              hRainCloudTex ? "synthesized (patch l)" : "unavailable - Gouraud deck fallback", N);
}

// ============================================================================
// UpdateRainFlashLight - MAIN THREAD, called unconditionally from clbkPreStep. The
// scene half of the rain lightning: a borrowed point light on the FOCUS vessel, so a
// flash in the deck also lights the ship and the wet ground under it (the client's
// local-light path reaches both). The reentry hull light's pattern exactly:
// SetPositionRef / SetIntensityRef into STABLE members, 23(k)-gated attach, returned
// on every exit path (gates fail, focus changes, vessel dies, teardown). The light
// stands 700 m toward the flash rather than AT it - a point light 8 km out would
// need absurd intensity against quadratic attenuation; direction is what the eye
// reads, and 700 m preserves it. VIS_EXTERNAL - G9: never light the cabin through
// the floor.
// ============================================================================
void OroModule::UpdateRainFlashLight()
{
	const bool want = g_fx.masterArmed && (g_fx.rainEnabled || g_fx.rainTest)
	               && (g_fx.rainLtg > 0.01f || g_fx.boltTestSlot >= 0)
	               && s_rnValid && s_gateF > 0.01f;

	VESSEL* v = want ? oapiGetFocusInterface() : NULL;
	const OBJHANDLE hV = v ? v->GetHandle() : NULL;

	// release when gated off or when the focus moved (reattach below, next frame)
	if (rainLtgLight && (!want || hV != rainLtgLightV)) {
		if (rainLtgLightV && oapiIsVessel(rainLtgLightV)) {
			VESSEL* lv = oapiGetVesselInterface(rainLtgLightV);
			if (lv) lv->DelLightEmitter(rainLtgLight);
		}
		rainLtgLight = NULL; rainLtgLightV = NULL; rainLtgLI = 0.0;
	}
	if (!want || !v) return;

	// the strongest active flash, from the SAME pure scheduler the deck reads
	const float lrate = clampf(g_fx.rainLtg, 0.0f, 2.0f)
	                  * clampf((g_fx.rainI - 0.2f) / 0.6f, 0.0f, 1.0f);
	float bestI = 0.0f, bestAz = 0.0f;
	for (int s = 0; s < 6 && lrate > 0.01f; s++) {
		RainLtgEv ev;
		if (RainLtgFlash(animT, s, lrate, &ev) > bestI) { bestI = ev.I; bestAz = ev.az; }
	}
	if (g_fx.boltTestSlot >= 0) {
		const float ei = RainLtgTestEnv((float)(animT - boltTestT0));
		if (ei > bestI) { bestI = ei; bestAz = 0.0f; }   // the strike is beside the ship
	}

	// attach (23k: only after the scene is real - the bell-mesh lesson)
	if (!rainLtgLight && sceneRendered) {
		const DWORD c = g_fx.ltgColour;
		const COLOUR4 col = { (float)(c & 0xFF) * (1.0f / 255.0f),
		                      (float)((c >> 8) & 0xFF) * (1.0f / 255.0f),
		                      (float)((c >> 16) & 0xFF) * (1.0f / 255.0f), 1.0f };
		rainLtgLPos = _V(0, 0, 0);
		rainLtgLI = 0.0;
		rainLtgLight = v->AddPointLight(rainLtgLPos, 15000.0, 0.9, 0.0, 2.5e-6, col, col, col);
		if (rainLtgLight) {
			rainLtgLight->SetVisibility(LightEmitter::VIS_EXTERNAL);
			rainLtgLight->SetPositionRef(&rainLtgLPos);
			rainLtgLight->SetIntensityRef(&rainLtgLI);
			rainLtgLightV = hV;
		}
	}
	if (!rainLtgLight) { rainLtgLI = 0.0; return; }

	// position: 700 m toward the flash, lifted toward the deck; vessel-LOCAL because
	// that is the frame a vessel's emitters live in
	VECTOR3 vp2; v->GetGlobalPos(vp2);
	const VECTOR3 upG = unit(vp2 - s_rn.pC);
	VECTOR3 eG = crossp(s_rn.axis, upG);
	const double el2 = length(eG);
	if (el2 < 1e-6) { rainLtgLI = 0.0; return; }
	eG = eG / el2;
	const VECTOR3 nG = crossp(upG, eG);
	const VECTOR3 world = vp2 + (eG * cos((double)bestAz) + nG * sin((double)bestAz)) * 700.0
	                    + upG * 450.0;
	MATRIX3 vR; v->GetRotationMatrix(vR);
	rainLtgLPos = tmul(vR, world - vp2);
	rainLtgLI = (double)bestI * 2.2;
}

// ============================================================================
// PushSurfaceWet - patch (s). ORO does not draw the wet ground; the CLIENT does, in two
// different shaders, and this hands it one number. That puts it in invariant 18's
// category with the VC shadows, and it inherits that section's three rules:
//   (a) PROBE BY BINDING (CanSetSurfaceWetness), never by build date.
//   (b) PUSH ON CHANGE, not per frame - it is client state, not a frame parameter.
//   (c) BORROW AND RETURN. Ctrl+G, the pill, leaving the gate or ending the session all
//       hand the client its stock behaviour back, because a world left permanently wet by
//       an addon that has been switched off is a bug the user cannot even attribute.
// ============================================================================
void OroModule::PushSurfaceWet()
{
	if (!pCore) return;
	// The dry/clear values are what the client had before we existed, so disarming,
	// disabling or walking out of the gate all resolve to exactly 0.
	const bool on = g_fx.masterArmed && (g_fx.rainEnabled || g_fx.rainTest);

	if (pCore->CanSetSurfaceWetness()) {
		// x s_gateF: the altitude/world gate reaches the CLIENT values too - above the
		// deck there is no wet ground and no hull glint, whatever the envelope holds.
		float w = on ? clampf(g_fx.rainWet, 0.0f, 1.0f) * s_gateF : 0.0f;
		if (fabsf(w - wetPushed) >= 0.002f) { wetPushed = w; pCore->SetSurfaceWetness(w); }
	}

	// THE GLOOM SLIDER DRIVES THE STORM LIGHT (2026-08-22). It used to drive only a
	// full-frame post-darken, which could never remove the sharp sun shadows or the warm
	// directional light already baked into the pixels - "someone turned the brightness
	// down", not "storm overhead". Now it collapses the sun at the SOURCE (client patch s
	// part 2); PSGloom survives only as a mild desaturation on top. Same control, more
	// honest lever - the bake-the-slider preference applied to a repoint instead.
	if (pCore->CanSetStormLight()) {
		float gs = clampf(g_fx.rainGloom, 0.0f, 2.0f);
		float st = on ? clampf(g_fx.rainI * gs * 0.5f, 0.0f, 1.0f) * s_gateF : 0.0f;
		if (fabsf(st - stormPushed) >= 0.002f) { stormPushed = st; pCore->SetStormLight(st); }
	}

	// The wet-darkness gain (part 3). Pushed even while the rain is off - it only DOES
	// anything multiplied by a nonzero wetness, and pushing it unconditionally means the
	// slider previews live during the drying-out tail.
	if (pCore->CanSetWetDarkness()) {
		float wd = clampf(g_fx.rainWetDark, 0.0f, 2.0f);
		if (fabsf(wd - wetDarkPushed) >= 0.002f) { wetDarkPushed = wd; pCore->SetWetDarkness(wd); }
	}
	if (pCore->CanSetWetGlint()) {
		float gl2 = clampf(g_fx.rainGlint, 0.0f, 2.0f);
		if (fabsf(gl2 - glintPushed) >= 0.002f) { glintPushed = gl2; pCore->SetWetGlint(gl2); }
	}
	if (pCore->CanSetWetReflection()) {
		float rf = clampf(g_fx.rainRefl,      0.0f, 2.0f);
		float sa = clampf(g_fx.rainSwimAmp,   0.0f, 2.0f);
		float sr = clampf(g_fx.rainSwimRate,  0.0f, 2.0f);
		// ⚠️ POOL SIZE IS REMAPPED (2026-08-25, his spec): the slider still reads 0..2, but
		// it drives -0.1..2 underneath, and the shaders fade the pools out across that
		// negative tenth. So 0 finally means NO STANDING WATER - previously the bottom of
		// the range still produced a fine mesh of small pools, because the control only
		// ever set the lattice SCALE and the shader floors that at 0.35. Today's bottom
		// end is now the 0.1 mark, exactly as he specified.
		float ps = -0.1f + 1.05f * clampf(g_fx.rainPoolSize, 0.0f, 2.0f);
		float pr = clampf(g_fx.rainPoolReach, 0.0f, 2.0f);
		float bl = clampf(g_fx.rainReflBlur,  0.0f, 2.0f);
		if (fabsf(rf - reflPushed)      >= 0.002f ||
		    fabsf(sa - swimAmpPushed)   >= 0.002f ||
		    fabsf(sr - swimRatePushed)  >= 0.002f ||
		    fabsf(ps - poolSizePushed)  >= 0.002f ||
		    fabsf(pr - poolReachPushed) >= 0.002f ||
		    fabsf(bl - reflBlurPushed)  >= 0.002f) {
			reflPushed = rf; swimAmpPushed = sa; swimRatePushed = sr;
			poolSizePushed = ps; poolReachPushed = pr; reflBlurPushed = bl;
			pCore->SetWetReflection(rf, sa, sr, ps, pr, bl);
		}
	}
	if (pCore->CanSetWetGrain()) {
		float go  = clampf(g_fx.rainGrainOp,   0.0f, 2.0f);
		float gsz = clampf(g_fx.rainGrainSize, 0.0f, 2.0f);
		if (fabsf(go - grainOpPushed) >= 0.002f || fabsf(gsz - grainSizePushed) >= 0.002f) {
			grainOpPushed = go; grainSizePushed = gsz;
			pCore->SetWetGrain(go, gsz);
		}
	}
}

// ============================================================================
// UpdateRainSound - main thread, from clbkPreStep, right after UpdateRain
// (invariant 12: XRSound never runs in the render proc). The three generated
// loops (tools/raingen.py) crossfade as the storm envelope ramps: the light
// patter carries the build-up, the medium wash the middle, the downpour the
// full storm. All three are mastered to EQUAL RMS, so these weights ARE the
// mix - the files carry timbre, this function carries loudness policy.
//
// The driver is rainIntensityLive = envelope x altitude gate, which embeds
// every gate the visuals obey (masterArmed, external view, Earth, below the
// weather) - so the sound can never contradict what is on screen. The VC is
// therefore SILENT for now, deliberately: the internal view currently shows a
// dry bright world (the extGate early-out zeroes the client pushes too), and
// rain you hear over sunshine reads as a bug. The cockpit hears the rain the
// day the cockpit sees it (F2, the windscreen half).
//
// A short slew (~0.35 s) keeps the instant transitions - view switch, Ctrl+G,
// the pill going off - from clicking; the 10 s envelope does the real ramp.
// Loops at zero are STOPPED rather than parked at volume 0, and restarted from
// silence by the slew, so starting can never click either. While PAUSED none
// of this runs (invariant 1): the loops simply keep sounding at their last
// volume, which matches the visual rain continuing to animate on real time.
//
// ⚠️⚠️ XRSOUND MODULE SOUNDS FREEZE AT THEIR STARTING VOLUME (found 2026-08-23,
// diagnosed from the DIAG log + the XRSound source in the orbiter repo).
// XRSoundDLL::clbkPreStep updates sound state for VESSEL engines only - module
// engines get no per-timestep update - and PlayWav on an already-playing sound
// does not apply the new volume itself, it stores it for "the next timestep",
// which for a module sound NEVER COMES. XRSound.h's "this call will only alter
// its loop or volume settings" is therefore FALSE for module sounds: the volume
// of the FIRST PlayWav is the volume forever. It never bit ORO before because
// the heartbeat re-fires per beat and the scenario clips play once - every
// sound we ever played STARTED at its final volume. The first fly-and-report
// round here played the whole storm at the slew's first step, 0.004 - looping,
// position advancing, engine reporting "playing", perfectly inaudible.
// (Worth reporting upstream to Doug Beachy - it contradicts the header's
// documented contract and is invisible until someone ramps a module sound.)
//
// SO EVERY VOLUME CHANGE IS A FRESH START: stop -> PlayWav at the new volume
// (a NEW sound applies its volume inline) -> SetPlayPosition back to where it
// was. Exactly the call shape the H6 scenario-resume fix proved audible on
// module sounds on 2026-08-16. Pushed ON CHANGE with a 0.045 quantum
// (invariant 18's push-on-change discipline): a splice every ~0.7 s during the
// 10 s build-up, ZERO calls in steady state, and a mid-noise splice at the
// preserved position is masked by the rain itself.
// ============================================================================
void OroModule::UpdateRainSound()
{
	if (!pXRSound || !rainSndLoaded) return;

	const float dt = (float)oapiGetSysStep();     // REAL time (invariant 4)
	const float e  = clampf(rainIntensityLive, 0.0f, 1.0f);
	const float uv = clampf(g_fx.rainSoundVol, 0.0f, 2.0f);   // the OUTSIDE mix (RAIN page)
	// THE CABIN HAS ITS OWN VOLUME (2026-09-06, his design): inside you hear the VIRTUAL
	// COCKPIT page's Rain in cabin, and only that - the outside slider does not reach
	// the seat, so a silent cabin in a storm you can still hear from outside is one
	// slider at 0. Each family below carries its own volume now; `overall` is the
	// envelope alone. At 1/1/1 the mix is bit-identical to before the split.
	const float iv = clampf(g_fx.vcRainSound, 0.0f, 2.0f);   // the INSIDE mix (VC page)

	// Crossfade weights over the envelope. They deliberately overlap (noise
	// powers add as sqrt of squares, so the hand-off holds roughly constant
	// energy); at e = 1 only the downpour remains.
	auto sstep = [](float a, float b, float x) {
		float t = clampf((x - a) / (b - a), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	};
	// THROUGH THE HULL, PROPERLY MUFFLED SINCE 2026-08-27 (his call: a volume cut "is
	// just as if the volume was slightly turned down. It doesn't have that 'muffle'
	// effect... the interior of a spacecraft is supposed to be a pressurized cabin").
	// XRSound has no runtime filter, so the muffle is PRE-BAKED: channels 4-6 are the
	// same three tiers low-passed by tools/rainmuffle.py, and going inside crossfades
	// the families through the existing slew. Without the _in files (rainSndInLoaded
	// false) the old 45% duck of the exterior tiers stands in. The HULL TAPS (ch 3)
	// stay unfiltered and interior-only: the drops are ON the hull, structure-borne -
	// inside is exactly where they are bright, and with the storm now genuinely dark
	// behind them they read more distinct at the same volume, for free.
	const bool  interior = !extGate;   // e > 0 while internal = the rainVC case
	const bool  useIn    = interior && rainSndInLoaded;
	const float exGain   = useIn ? 0.0f : (interior ? 0.45f * iv : uv);
	// The _in files are levelled at 62% of the exterior RMS by the tool, so the
	// runtime factor is nearly neutral - the FILES carry the muffle, this only trims.
	const float inGain   = useIn ? 0.85f * iv : 0.0f;
	float tier[3];
	tier[0] = 1.0f - sstep(0.30f, 0.65f, e);                          // light
	tier[1] = sstep(0.12f, 0.45f, e) * (1.0f - sstep(0.60f, 0.92f, e)); // medium
	tier[2] = sstep(0.50f, 0.88f, e);                                   // heavy
	float w[SND_RAIN_CH];
	for (int i = 0; i < 3; i++) { w[i] = tier[i] * exGain; w[4 + i] = tier[i] * inGain; }
	// ⚠️ THE HULL LOOP CARRIES ITS OWN VOLUME (2026-08-25, a tester's ask). It is the
	// one layer that is about the SHIP rather than the weather - some people want the
	// storm without the drumming - and folding it into Rain sound meant the whole
	// outside mix had to move to quiet it. Its own volume outright since the 2026-09-06
	// split (0..3 - his range; it lives on the VIRTUAL COCKPIT page now).
	const float hv = clampf(g_fx.rainHullVol, 0.0f, 3.0f);
	w[3] = interior ? (0.55f + 0.45f * sstep(0.10f, 0.60f, e)) * hv : 0.0f;   // hull taps

	// ^0.7 so the patter is audibly present early in the build-up (a linear map
	// leaves the first seconds inaudible); 0.65 is the designed full-storm
	// volume at slider 1, leaving slider headroom up to XRSound's 1.0 cap. The
	// volumes themselves ride the family gains above since the cabin split.
	const float overall = powf(e, 0.7f) * 0.65f;

	for (int i = 0; i < SND_RAIN_CH; i++) {
		// channel -> XRSound id: 0-3 are the original set, 4-6 the interior twins
		const int sid = (i < SND_RAIN_N) ? (SND_RAIN_BASE + i) : (SND_RAIN_IN_BASE + (i - 4));
		const float tgt = clampf(w[i] * overall, 0.0f, 1.0f);
		rainSndLvl[i] += (tgt - rainSndLvl[i]) * clampf(dt / 0.35f, 0.0f, 1.0f);
		if (tgt <= 0.002f && rainSndLvl[i] <= 0.002f) rainSndLvl[i] = 0.0f;
		const float vol = clampf(rainSndLvl[i], 0.0f, 1.0f);
		const bool  on  = vol > 0.002f;

		// push only when the state actually moved (on/off flip, or >= one quantum)
		if (on == rainSndOn[i] && (!on || fabsf(vol - rainSndPushed[i]) < 0.045f))
			continue;

		if (!on) {
			pXRSound->StopWav(sid);
			rainSndOn[i] = false;
			rainSndPushed[i] = 0.0f;
			continue;
		}
		// THE SPLICE (see the header note): a playing module sound cannot change
		// volume, so restart it at the new volume and put it back where it was.
		const int pos = rainSndOn[i] ? pXRSound->GetPlayPosition(sid) : -1;
		if (rainSndOn[i]) pXRSound->StopWav(sid);
		rainSndOn[i] = pXRSound->PlayWav(sid, true, vol);
		if (rainSndOn[i] && pos > 0)
			pXRSound->SetPlayPosition(sid, (unsigned int)pos);
		rainSndPushed[i] = vol;
	}
	// (A throttled DIAG line lived here for the 2026-08-23 silence hunt - it is what
	//  caught the loops playing at the slew's first step, 0.004, forever. Removed
	//  after use, the VC-glow readout's precedent.)
}

// ============================================================================
// UpdateThunder - main thread, right after UpdateRainSound. THE FOURTH CONSUMER
// of the flash event (28m's one-event-one-clock law: deck brightening, bolt,
// scene light - and now the sound): every event the pure scheduler produces is
// heard delayed by ITS OWN distance, dist/340 s, so the sky answers the eye
// 6-26 seconds later exactly as weather does.
//
// DETECTION IS EPOCH EDGES on the six slots (RainLtgEv.ep): a new epoch is a
// new event, caught on the frame it begins and enqueued for later. The cursor
// is PRIMED when the gates open (21e's chain rule) so re-entering a storm
// schedules nothing retroactively; and a detection must show real intensity
// (I > 0.05), because dragging the Lightning-rate slider rebases every slot's
// cycle and would otherwise schedule phantom booms.
//
// THE PICK: class by distance (close < 3.5 km, mid < 6.2 km, far beyond),
// EXCEPT that an in-cloud flash never cracks - its channel is buried in the
// deck, so type 0 is mid at best. Variant from the event's own seed (ev.tex),
// volume falling as (2.2 km / dist)^0.85 - the FILES carry the per-class
// timbre and mastering hierarchy (tools/thunderprep.py), the law here only
// fine-scales. Each fire is a ONE-SHOT at its final volume, which the XRSound
// module-volume freeze (see UpdateRainSound) cannot touch. At fire time a
// variant already sounding yields to a silent sibling, so overlapping strikes
// do not restart each other.
//
// THE STRIKE BUTTON is the timing instrument he kept it for: its bolt lands on
// the focus vessel, so the crack follows ~0.7 s (250 m) after the flash.
// Pending thunders whose gates have closed by fire time (pill off, Ctrl+G,
// cockpit view, climbed out) are DROPPED - you hear what happens while you are
// out in it. animT freezes under pause, so the queue simply waits.
// ============================================================================
void OroModule::UpdateThunder()
{
	if (!pXRSound) return;

	// -- fire what is due (runs even while the scheduler's gates are closed) --
	for (int q = 0; q < THUN_Q; q++) {
		if (thunQ[q].vol <= 0.0f || animT < thunQ[q].fireT) continue;
		if (animT - thunQ[q].fireT > 3.0f || rainIntensityLive <= 0.02f) {
			thunQ[q].vol = 0.0f;                     // stale, or the storm gate closed
			continue;
		}
		// THE FAMILY IS PICKED AT FIRE TIME BY THE CURRENT VIEW (2026-08-27): inside,
		// the clap plays its rainmuffle.py twin - the CRACK dies in the pressurized
		// hull, the rumble comes through, which is what thunder indoors actually is.
		// A clap keeps its family for its whole length (they run seconds; a view
		// switch mid-boom re-filtering it would be stranger than the mismatch).
		const bool  inFam = !extGate && thunInLoaded[thunQ[q].file];
		const bool* fam   = inFam ? thunInLoaded : thunLoaded;
		const int   base  = inFam ? SND_THUNDER_IN_BASE : SND_THUNDER_BASE;
		int f = thunQ[q].file;
		const int cls = f / 3;
		for (int k = 0; k < 3; k++) {                // prefer a silent sibling
			const int cand = cls * 3 + ((f % 3) + k) % 3;
			if (fam[cand] && !pXRSound->IsWavPlaying(base + cand)) { f = cand; break; }
		}
		// through the hull (2026-08-23): thunder penetrates far better than rain
		// - it is low frequency - but still comes down inside. With the muffled twin
		// the FILE carries most of the attenuation, so the duck relaxes 0.60 -> 0.80.
		float fv = thunQ[q].vol;
		if (!extGate) fv *= inFam ? 0.80f : 0.60f;
		if (fam[f]) pXRSound->PlayWav(base + f, false, clampf(fv, 0.0f, 1.0f));
		thunQ[q].vol = 0.0f;
	}

	const float uv = clampf(g_fx.rainThunder, 0.0f, 2.0f);

	// -- the STRIKE rig (UpdateRain stamped boltTestT0 earlier this same frame) --
	// ⚠️ THE EAR IS THE CAMERA (his catch, 2026-08-23 - the first build hardcoded
	// 250 m, the "camera beside the vessel" assumption as a constant). The bolt
	// lands on the FOCUS VESSEL, but the listener is wherever the view is: zoomed
	// kilometres out, the same strike is a distant boom arriving that much later,
	// through the mid/far files. The eclipse's model-the-observer law, for sound.
	// The storm events above need none of this - their distances are synthesized
	// in the observer's frame to begin with. (Main thread: oapi calls are legal
	// here; invariant 1 bans only the render path.)
	if (g_fx.boltTestSlot >= 0 && boltTestT0 != thunLastStrikeT0) {
		thunLastStrikeT0 = boltTestT0;
		if (uv > 0.01f && rainIntensityLive > 0.02f) {
			VECTOR3 cam; oapiCameraGlobalPos(&cam);
			double d = length(cam - s_rn.vPos);
			if (d < 180.0) d = 180.0;                // never closer than the bolt itself
			const float dist = (float)d;
			const int   cls  = (dist < 3500.0f) ? 0 : (dist < 6200.0f ? 1 : 2);
			const float vol  = clampf(powf(2200.0f / dist, 0.85f) * uv, 0.0f, 1.0f);
			if (vol > 0.02f)
				ThunderEnqueue((float)(boltTestT0 + dist * (1.0 / 340.0)),
				               cls * 3 + (g_fx.boltTestSlot % 3), vol);
		}
	}

	// -- new events: epoch edges, under the same gates as the scene light --
	const bool want = g_fx.masterArmed && (g_fx.rainEnabled || g_fx.rainTest)
	               && s_rnValid && s_gateF > 0.01f && rainIntensityLive > 0.02f;
	const float lrate = clampf(g_fx.rainLtg, 0.0f, 2.0f)
	                  * clampf((g_fx.rainI - 0.2f) / 0.6f, 0.0f, 1.0f);
	if (!want || lrate <= 0.01f || uv <= 0.01f) { thunPrimed = false; return; }

	if (!thunPrimed) {
		for (int s = 0; s < 6; s++) {
			RainLtgEv ev; RainLtgFlash(animT, s, lrate, &ev);
			thunLastEp[s] = ev.ep;
		}
		thunPrimed = true;
		return;
	}
	for (int s = 0; s < 6; s++) {
		RainLtgEv ev;
		RainLtgFlash(animT, s, lrate, &ev);
		if (ev.ep == thunLastEp[s]) continue;
		thunLastEp[s] = ev.ep;
		if (ev.I <= 0.05f) continue;                 // slider drag, not an event
		int cls = (ev.dist < 3500.0f) ? 0 : (ev.dist < 6200.0f ? 1 : 2);
		if (ev.type == 0 && cls == 0) cls = 1;       // in-cloud never cracks
		float vol = powf(2200.0f / ev.dist, 0.85f);
		if (ev.type == 2) vol *= 1.25f;              // the positive giant
		vol = clampf(vol * uv, 0.0f, 1.0f);
		if (vol > 0.02f)
			ThunderEnqueue(animT + ev.dist * (1.0f / 340.0f), cls * 3 + (ev.tex % 3), vol);
	}
}

void OroModule::ThunderEnqueue(float fireT, int file, float vol)
{
	for (int q = 0; q < THUN_Q; q++) {
		if (thunQ[q].vol <= 0.0f) { thunQ[q].fireT = fireT; thunQ[q].file = file; thunQ[q].vol = vol; return; }
	}
	// full = twelve thunders already in the air; dropping the newest is inaudible
}

// ============================================================================
// BuildRainGeometry - RENDER PATH (patch k camera, patch k2 anchor).
// INVARIANT-1 AUDIT: zero oapi calls in this function.
// ============================================================================
void OroModule::BuildRainGeometry()
{
	rainVtxN = 0;
	gndVtxN = 0;
	ringCN = 0;
	ringVN = 0;
	deckN = 0;
	boltN = 0;
	rainActive = false;
	if (!s_rnValid || viewW == 0 || viewH == 0) return;

	const float I = s_rn.intensity;
	if (I <= 0.002f) return;

	// LIGHT ON THE RAIN (2026-09-05, his note: "the rain streak glow should also be
	// dependent on the slider + the light"). The sky and the ship's own lamps were
	// sensed (s_rn.lightF); the storm's flashes join here from the pure scheduler, so a
	// flash lights the falling rain paused too and from the VC (the borrowed scene light
	// is VIS_EXTERNAL). Floored just above zero so an unlit night keeps a hint that it
	// is raining at all. Scales the sheet's streaks and the splash rings; the deck and
	// the bolts have their own light.
	float lightNow = s_rn.lightF;
	{
		const float lrate = clampf(g_fx.rainLtg, 0.0f, 2.0f)
		                  * clampf((I - 0.2f) / 0.6f, 0.0f, 1.0f);
		float fI = 0.0f;
		for (int s2 = 0; s2 < 6 && lrate > 0.01f; s2++) {
			RainLtgEv ev;
			if (RainLtgFlash(animT, s2, lrate, &ev) > fI) fI = ev.I;
		}
		if (g_fx.boltTestSlot >= 0) {
			const float ei = RainLtgTestEnv((float)(animT - boltTestT0));
			if (ei > fI) fI = ei;
		}
		lightNow = clampf(lightNow + fI * 1.4f, 0.04f, 1.0f);
	}

	CamCtx cc;
	if (!FillProjCam(cc.pos, cc.rot, cc.tanAp)) return;

	// THE ANCHOR at the render epoch. Without this the field is thrown hundreds of metres
	// sideways every frame - see the header note on frames.
	VECTOR3 pC = s_rn.pC;
	if (pCore && pCore->CanGetRenderObjPos()) {
		VECTOR3 rp;
		if (pCore->GetRenderObjPos(s_rn.hRef, &rp)) pC = rp;
	}

	// The local horizontal basis. Derived from the PLANET's spin axis rather than from
	// anything about the camera, so it does not rotate as you look around - the same
	// discipline as the aurora tying its ring basis to the prime meridian, and the vapour
	// cone anchoring its radial basis to the hull (25e's warning about a basis that swings).
	const VECTOR3 camRel = cc.pos - pC;
	const double  camR   = length(camRel);
	if (camR < 1.0) return;
	const VECTOR3 up = camRel / camR;
	VECTOR3 east = crossp(s_rn.axis, up);
	double el = length(east);
	if (el < 1e-6) { east = crossp(_V(1, 0, 0), up); el = length(east); if (el < 1e-6) return; }
	east = east / el;
	const VECTOR3 north = crossp(up, east);

	// ⚠️ camE/camN LIVED HERE AND WERE IDENTICALLY ZERO. BOTH CONSUMERS ARE ANCHORED
	// NOW, so they are gone (2026-08-25). east and north were both built PERPENDICULAR
	// TO up = camRel/camR just above, so dotp(camRel, east) was camR * dotp(up, east) = 0,
	// always - two places added them to a world coordinate and therefore added nothing.
	// The splash lattice was fixed first, after a tester reported the rings sliding (see
	// the anchor down in the splash section); the CLOUD-DECK BILLOW was knowingly left
	// camera-anchored for one session, because the deck look was already approved and
	// nobody had reported it. His call to sweep it was the right one.
	// ⚠️ THE TWO FIXES ARE DELIBERATELY NOT THE SAME FIX. The lattice needed a stored
	// planet-fixed reference because it has no other world coordinate to ride. The deck
	// already had one - its TEXTURE has always been laid out in lon/lat metres - so the
	// billow simply joined it. That is strictly better here: no stored reference means
	// no ~10 km re-anchor, and a 1500 m billow would have shown that jump far more
	// plainly than a field of sub-pixel rings ever could.
	const float camU = (float)camR;
	const float camAGL = camU - (float)s_rn.groundR;

	// ⚠️ THE CAMERA'S PLANET-FIXED HORIZONTAL POSITION (2026-08-25), and the splash
	// lattice was going without it. The header note above promises that ring positions
	// have "the camera's own offset subtracted BEFORE the wrap - so a ring stays where it
	// landed instead of sliding with the viewer". What was being subtracted was camE/camN,
	// which are zero by construction (see immediately above), so the subtraction was a
	// no-op and the entire splash field was GLUED TO THE CAMERA. A public-beta tester
	// reported it exactly: "splashes on the ground move relative to the ground towards the
	// camera as it rotates in the external view. In VC it looks correct." The second half
	// of that sentence is the proof rather than an aside - in a cockpit the camera hardly
	// translates, so a camera-glued field and a world-anchored one look the same.
	//
	// The fix is to measure the camera against a planet-fixed point ON THE GROUND - see
	// the lattice anchor down in the splash section, where the vessel offset already is.
	//
	// ⚠️ AND THE FIRST ATTEMPT AT THAT FIX WAS WRONG IN A WAY WORTH KEEPING WRITTEN DOWN
	// (2026-08-25). It used the camera's arc-length position in the planet's own rotating
	// frame - R*cos(lat)*lon east, R*lat north - which IS planet-fixed and does track the
	// camera. But plate-carree is NOT LOCALLY ISOMETRIC: the east coordinate carries a
	// cos(lat) factor, so moving NORTH changes it too, by -sin(lat)*lon per radian. At KSC
	// that is 0.67 m of spurious east shift per metre travelled north. He found it in one
	// pass and described it exactly - rotating in place was clean (lat and lon do not
	// change), translating slid sideways. A COORDINATE BEING PLANET-FIXED IS NOT ENOUGH;
	// it also has to measure distance the same way in every direction.

	const float t = (float)animT;                    // REAL time (invariant 4)

	auto emit = [&](float ax, float ay, float az, DWORD ac,
	                float bx, float by, float bz, DWORD bc,
	                float cx, float cy, float cz, DWORD ccol) {
		if (rainVtxN + 3 > RAIN_MAX_TRI * 3) return;
		rainVtx[rainVtxN].x = ax; rainVtx[rainVtxN].y = ay; rainVtx[rainVtxN].c = FogColNear(ac, az);
		rainDepth[rainVtxN] = az; rainVtxN++;
		rainVtx[rainVtxN].x = bx; rainVtx[rainVtxN].y = by; rainVtx[rainVtxN].c = FogColNear(bc, bz);
		rainDepth[rainVtxN] = bz; rainVtxN++;
		rainVtx[rainVtxN].x = cx; rainVtx[rainVtxN].y = cy; rainVtx[rainVtxN].c = FogColNear(ccol, cz);
		rainDepth[rainVtxN] = cz; rainVtxN++;
	};
	// the GROUND poly's emitter - the storm deck. Separate buffer, separate 65535
	// ceiling (see RAIN_GND_TRI in OroModule.h), drawn BEFORE everything else here.
	auto emitG = [&](float ax, float ay, float az, DWORD ac,
	                 float bx, float by, float bz, DWORD bc,
	                 float cx, float cy, float cz, DWORD ccol) {
		if (gndVtxN + 3 > RAIN_GND_TRI * 3) return;
		gndVtx[gndVtxN].x = ax; gndVtx[gndVtxN].y = ay; gndVtx[gndVtxN].c = FogColNearGround(ac, az);
		gndDepth[gndVtxN] = az; gndVtxN++;
		gndVtx[gndVtxN].x = bx; gndVtx[gndVtxN].y = by; gndVtx[gndVtxN].c = FogColNearGround(bc, bz);
		gndDepth[gndVtxN] = bz; gndVtxN++;
		gndVtx[gndVtxN].x = cx; gndVtx[gndVtxN].y = cy; gndVtx[gndVtxN].c = FogColNearGround(ccol, cz);
		gndDepth[gndVtxN] = cz; gndVtxN++;
	};
	// ORO 2026-09-05 (client patch ab): TERRAIN WRITES THE DEPTH BUFFER NOW, and the rings
	// lie ON the ground - a ring published at its true distance z-fights the tile it
	// sits on at every bump and the pad's per-pixel clip eats it. Published 4% nearer:
	// at 50 m that is 2 m of slack over the terrain mesh, and nothing else is ever
	// within 4% of a ring's distance. The bolts' feet get the same treatment below.
	const float RING_DEPTH_K = 0.96f;
	// the SPLASH-RING emitters - one poly per field (the more-groups trick: the
	// 65,535-vertex ceiling is per HPOLY, and LOD-by-screen-size makes a ring's cost
	// camera-dependent, so each field gets its own generous cap).
	auto emitRing = [&](int f, float ax, float ay, float az, DWORD ac,
	                    float bx, float by, float bz, DWORD bc,
	                    float cx, float cy, float cz, DWORD ccol) {
		if (f == 0) {
			if (ringCN + 3 > RAIN_RINGP_TRI * 3) return;
			ringVtxC[ringCN].x = ax; ringVtxC[ringCN].y = ay; ringVtxC[ringCN].c = FogColNear(ac, az);
			ringDepC[ringCN] = az * RING_DEPTH_K; ringCN++;
			ringVtxC[ringCN].x = bx; ringVtxC[ringCN].y = by; ringVtxC[ringCN].c = FogColNear(bc, bz);
			ringDepC[ringCN] = bz * RING_DEPTH_K; ringCN++;
			ringVtxC[ringCN].x = cx; ringVtxC[ringCN].y = cy; ringVtxC[ringCN].c = FogColNear(ccol, cz);
			ringDepC[ringCN] = cz * RING_DEPTH_K; ringCN++;
		} else {
			if (ringVN + 3 > RAIN_RINGP_TRI * 3) return;
			ringVtxV[ringVN].x = ax; ringVtxV[ringVN].y = ay; ringVtxV[ringVN].c = FogColNear(ac, az);
			ringDepV[ringVN] = az * RING_DEPTH_K; ringVN++;
			ringVtxV[ringVN].x = bx; ringVtxV[ringVN].y = by; ringVtxV[ringVN].c = FogColNear(bc, bz);
			ringDepV[ringVN] = bz * RING_DEPTH_K; ringVN++;
			ringVtxV[ringVN].x = cx; ringVtxV[ringVN].y = cy; ringVtxV[ringVN].c = FogColNear(ccol, cz);
			ringDepV[ringVN] = cz * RING_DEPTH_K; ringVN++;
		}
	};

	// ---- 0. THE STORM DECK ---------------------------------------------------
	// The dark cloud ceiling. Orbiter's own sky stays sunny-blue whatever the gloom does
	// (the overcast collapse changes the LIGHT, not the sky), so no post-process and no
	// lighting change can put a storm overhead - this draws one. Alpha-blended (cloud
	// occludes; it is not light), emitted FIRST in this buffer so the streaks and rings
	// paint over it in the same draw call.
	//
	// Built as rings of constant ELEVATION from the camera, not constant world radius:
	// a ring shares its projection behaviour, so the near-plane failures that shredded
	// the VC plasma's point field cannot land mid-sky - a vertex only fails when it is
	// essentially outside the view anyway. The rim feathers to zero by ~4 deg elevation,
	// which leaves the horizon lighter than overhead - the classic look of standing
	// under a storm cell, and the reference picture has exactly that bright band.
	//
	// No knob of its own: it rides the storm envelope and the Gloom slider (the deck IS
	// the overcast made visible). Billows are sampled in WORLD east/north coordinates,
	// so the pattern holds still while the camera moves under it, and drifts slowly in
	// real time on its own.
	// ROUND 10 ("make it more 3d... I just can't feel the WEIGHT of the clouds"):
	// round 9's anti-tiling pair was two textures on ONE surface - no parallax, no
	// depth. Now they are two REAL DECKS: the main ceiling at 1400 m and a darker,
	// ragged SCUD layer hanging under it at 950 m. Camera motion separates them, the
	// scud slides beneath the ceiling on its counter-drift, and it is darker because
	// the deck above shades it. On top of that, the low-frequency billow now SAGS the
	// surface itself - heavy masses hang DOWN, and hang darker (self-shading) - which
	// is the cue that turns a painted dome into a mass overhead. Round 9's anti-tiling
	// law still holds between the layers: unrelated repeats + a 37-deg rotation.
	const float  LAY_H[2]     = { 1400.0f, 950.0f };   // deck base altitudes, m
	const double LAY_REP[2]   = { 13000.0, 5100.0 };   // m of ground per texture repeat
	const double LAY_ROTC[2]  = { 1.0, 0.7986 };
	const double LAY_ROTS[2]  = { 0.0, 0.6018 };
	const float  LAY_DRIFT[2] = { 2.20f, -1.30f };     // texels/s, counter-drifting
	const float  LAY_A[2]     = { 1.00f, 0.62f };      // scud is translucent
	const float  LAY_LUM[2]   = { 1.00f, 0.66f };      // ...and shaded by the deck above
	const float  LAY_SAG[2]   = { 110.0f, 70.0f };     // m of vertical billow relief
	// THE DECK RISES WITH YOU (2026-08-27, his design). Fixed at 1400 m, the whole
	// storm roof vanished the moment a climb crossed ~1300 m - one condition, gone in
	// a frame. Now the ceiling stays ~700 m overhead as you climb, PINS at the
	// planet's own cloud-layer altitude (where Orbiter draws its cloud sphere), and
	// you punch THROUGH it there - the deck fading over the last few hundred metres
	// rather than blinking off. Parked, camAGL+700 < 1400, so nothing about the
	// approved ground-level look moves. The scud keeps its designed 450 m separation.
	float deckH0 = camAGL + 700.0f;
	if (deckH0 < LAY_H[0])        deckH0 = LAY_H[0];
	if (deckH0 > s_rn.cloudAGL)   deckH0 = s_rn.cloudAGL;
	const float layH[2]  = { deckH0, deckH0 - (LAY_H[0] - LAY_H[1]) };
	const float below    = deckH0 - camAGL;
	float punch = clampf((below - 80.0f) / 400.0f, 0.0f, 1.0f);
	punch = punch * punch * (3.0f - 2.0f * punch);
	const float dStorm = I * clampf(g_fx.rainGloom * 0.5f, 0.0f, 1.0f) * punch;
	if (dStorm > 0.01f && below > 80.0f) {
		const int   DK_AZ = 48;
		const float ELEV[13] = { 89.0f, 75.0f, 62.0f, 50.0f, 39.0f, 29.0f,
		                         21.0f, 14.5f, 9.5f, 6.0f, 3.5f, 2.0f, 1.0f };
		const bool useTex = (hRainCloudTex != NULL);
		const VECTOR3 pe1 = s_rn.pax1;
		const VECTOR3 pe2 = crossp(s_rn.axis, pe1);
		const float texBoost = useTex ? 1.55f : 1.0f;

		auto emitDeck = [&](float x1, float y1, float d1, DWORD c1, float u1, float v1,
		                    float x2, float y2, float d2, DWORD c2, float u2, float v2,
		                    float x3, float y3, float d3, DWORD c3, float u3, float v3) {
			if (useTex) {
				if (deckN + 3 > RAIN_GND_TRI * 3) return;
				deckVtx[deckN].x = x1; deckVtx[deckN].y = y1; deckVtx[deckN].u = u1;
				deckVtx[deckN].v = v1; deckVtx[deckN].c = FogColNearGround(c1, d1); deckDepth[deckN] = d1; deckN++;
				deckVtx[deckN].x = x2; deckVtx[deckN].y = y2; deckVtx[deckN].u = u2;
				deckVtx[deckN].v = v2; deckVtx[deckN].c = FogColNearGround(c2, d2); deckDepth[deckN] = d2; deckN++;
				deckVtx[deckN].x = x3; deckVtx[deckN].y = y3; deckVtx[deckN].u = u3;
				deckVtx[deckN].v = v3; deckVtx[deckN].c = FogColNearGround(c3, d3); deckDepth[deckN] = d3; deckN++;
			} else {
				emitG(x1, y1, d1, c1, x2, y2, d2, c2, x3, y3, d3, c3);
			}
		};

		// ACTIVE FLASHES this frame, offsets precomputed once (camera-basis anchor:
		// over a flash's ~1 s life the camera drift is invisible). Gated on the storm
		// being properly built - lightning belongs to a real storm.
		struct FlashXY { float px, py, I; int type, tex; };
		FlashXY fl[6]; int nFl = 0;
		{
			const float lrate = clampf(g_fx.rainLtg, 0.0f, 2.0f)
			                  * clampf((I - 0.2f) / 0.6f, 0.0f, 1.0f);
			for (int s2 = 0; s2 < 6 && lrate > 0.01f; s2++) {
				RainLtgEv ev;
				if (RainLtgFlash(t, s2, lrate, &ev) > 0.02f) {
					fl[nFl].px = cosf(ev.az) * ev.dist;
					fl[nFl].py = sinf(ev.az) * ev.dist;
					fl[nFl].I = ev.I; fl[nFl].type = ev.type; fl[nFl].tex = ev.tex;
					nFl++;
				}
			}
			// THE STRIKE TEST (his rig): the last-pressed bolt, planted ~60 m beside
			// the FOCUS vessel so the texture can be judged up close. Bypasses the
			// rate gate - a test is a test - but not the storm itself.
			if (g_fx.boltTestSlot >= 0 && nFl < 6) {
				const float ei = RainLtgTestEnv((float)(t - boltTestT0));
				if (ei > 0.02f) {
					VECTOR3 vP2 = s_rn.vPos;
					if (pCore && pCore->CanGetRenderObjPos()) {
						VECTOR3 rp;
						if (pCore->GetRenderObjPos(s_rn.hV, &rp)) vP2 = rp;
					}
					const VECTOR3 vRel = vP2 - cc.pos;
					// ON the target (round 2 of the rig): orbiting the camera around
					// the strike is the billboard test, so the strike IS the vessel
					fl[nFl].px = (float)dotp(vRel, east);
					fl[nFl].py = (float)dotp(vRel, north);
					fl[nFl].I = ei; fl[nFl].type = 1;
					fl[nFl].tex = g_fx.boltTestSlot % 16;
					nFl++;
				}
			}
		}

		const int NLAY = useTex ? 2 : 1;
		for (int Ld = 0; Ld < NLAY; Ld++) {
			const float dhL = layH[Ld] - camAGL;     // this layer's height above the CAMERA
			                                         // (layH: the RISEN deck, not the design
			                                         // constants - see deckH0 above)
			if (dhL < 80.0f) continue;               // flown into/above this deck
			// ... and each SHEET dissolves as the camera reaches it rather than
			// blinking at the 80 m line - the scud is crossed 450 m before the main
			// deck, while the punch-through fade above is still nearly full.
			float layF = clampf((dhL - 80.0f) / 300.0f, 0.0f, 1.0f);
			layF = layF * layF * (3.0f - 2.0f * layF);
			// fade the layer as the camera climbs toward it - flying INTO a ceiling pops
			const float nearF = clampf(dhL / 400.0f, 0.0f, 1.0f);

			float dkx[13][49], dky[13][49], dkd[13][49], dku[13][49], dkv[13][49];
			DWORD dkc[13][49]; bool dko[13][49];
			for (int e2 = 0; e2 < 13; e2++) {
				const float el = ELEV[e2] * 0.0174532925f;
				const float rH = dhL / tanf(el);
				const float dist = sqrtf(rH * rH + dhL * dhL);
				// elevation feather (round 7: the overcast runs to the horizon like the
				// reference - only a thin bright sliver survives at the very bottom)
				const float aEl = clampf((ELEV[e2] - 0.8f) / 7.0f, 0.0f, 1.0f);
				for (int az = 0; az <= DK_AZ; az++) {
					const float th = (float)(az % DK_AZ) / (float)DK_AZ * 6.2831853f;
					const float ca = cosf(th), sa = sinf(th);

					// THE DECK SAMPLE'S PLANET-FIXED GROUND COORDINATE (2026-08-25).
					// It is the coordinate the deck's TEXTURE has always ridden, and the
					// billow now rides it too - so texels and billow cannot disagree about
					// where a metre lies (the lightning's one-CellBasis rule), and it needs
					// no stored reference, so unlike the splash lattice it can never pop on
					// a re-anchor. A 1500 m billow would have shown that far more plainly
					// than a field of sub-pixel rings ever could.
					// Taken from the UNSAGGED point, which is what breaks the circle - the
					// sag needs the billow and the billow needs this. Displacing a sample
					// along the camera's up by <= 110 m moves its ground track by sag*rH/R,
					// about 1.4 m out at the horizon ring, against a 13 km texture repeat.
					const VECTOR3 P0 = cc.pos + east * (ca * rH) + north * (sa * rH)
					                 + up * (double)dhL;
					VECTOR3 dpd = P0 - pC;
					dpd = dpd / length(dpd);
					double sl = dotp(dpd, s_rn.axis);
					if (sl > 1.0) sl = 1.0; else if (sl < -1.0) sl = -1.0;
					const double lat = asin(sl);
					const double lon = atan2(dotp(dpd, pe2), dotp(dpd, pe1));
					const double um  = lon * s_rn.groundR * cos(lat);
					const double vm  = lat * s_rn.groundR;

					// the billow comes FIRST: it displaces the surface (the sag)
					const float wx = (float)(um * (1.0 / 1500.0));
					const float wy = (float)(vm * (1.0 / 1500.0));
					const float b1 = sinf(wx * 1.9f + sinf(wy * 2.6f) + t * 0.020f)
					               * sinf(wy * 1.5f + sinf(wx * 2.1f) - t * 0.014f);
					const float b2 = sinf(wx * 5.3f - t * 0.031f) * sinf(wy * 4.7f + t * 0.026f);
					const float billow = 0.80f + 0.28f * b1 + 0.10f * b2;
					// heavy masses hang DOWN, weighted by elevation so the horizon
					// rings stay put and the feather stays a feather
					const float sag = LAY_SAG[Ld] * b1 * aEl;
					const VECTOR3 P = P0 - up * (double)sag;
					double pz;
					dko[e2][az] = ProjPx(cc, P, viewW, viewH, dkx[e2][az], dky[e2][az], pz);
					dkd[e2][az] = dist;

					// the texture UV: the SAME planet-fixed metres the billow just used,
					// per-layer repeat + rotation + drift (round 9's anti-tiling law)
					if (useTex) {
						const double ur = um * LAY_ROTC[Ld] - vm * LAY_ROTS[Ld];
						const double vr = um * LAY_ROTS[Ld] + vm * LAY_ROTC[Ld];
						dku[e2][az] = (float)(ur * ((double)rainCloudN / LAY_REP[Ld])) + t * LAY_DRIFT[Ld] * ((float)rainCloudN * (1.0f / 1024.0f));
						dkv[e2][az] = (float)(vr * ((double)rainCloudN / LAY_REP[Ld]));
					} else {
						dku[e2][az] = 0.0f; dkv[e2][az] = 0.0f;
					}

					// LIGHTNING lights a REGION of the deck from within (his item 3) -
					// a Gaussian patch around each active flash, and the billow texture
					// shapes the lit mass irregular for free (the orbital lightning's
					// "the flash IS the cloud image lighting up", on our own deck)
					float fw = 0.0f;
					for (int q2 = 0; q2 < nFl; q2++) {
						const float dfx = fl[q2].px - ca * rH;
						const float dfy = fl[q2].py - sa * rH;
						const float w2 = fl[q2].I * expf(-(dfx * dfx + dfy * dfy) * (1.0f / (1500.0f * 1500.0f)));
						if (w2 > fw) fw = w2;
					}
					// Lit by the sky above it: near-black at night, blue-grey slate by
					// day; the scud darker (LAY_LUM), and a sagging mass shades ITSELF
					const float lum = (0.16f + 0.84f * s_rn.dayF) * billow * texBoost
					                * LAY_LUM[Ld] * (1.0f - 0.18f * clampf(b1, 0.0f, 1.0f));
					const int cr = (int)(52.0f * lum + fw * 430.0f);
					const int cg = (int)(57.0f * lum + fw * 450.0f);
					const int cb = (int)(66.0f * lum + fw * 490.0f);
					float aa = 235.0f * dStorm * aEl * nearF * (0.90f + 0.10f * b1) * LAY_A[Ld] * layF
					         + fw * 70.0f;      // lit cloud reads denser
					if (aa > 255.0f) aa = 255.0f;
					dkc[e2][az] = RCol(cr, cg, cb, (int)aa);
				}
			}
			// the zenith cap - without it the 89-deg ring leaves a pinhole of sky
			{
				float cxz, cyz; double pzz;
				const VECTOR3 Pz = cc.pos + up * (double)dhL;
				if (ProjPx(cc, Pz, viewW, viewH, cxz, cyz, pzz)) {
					const float lumZ = (0.16f + 0.84f * s_rn.dayF) * 0.80f * texBoost * LAY_LUM[Ld];
					float aaZ = 235.0f * dStorm * nearF * LAY_A[Ld] * layF;
					if (aaZ > 255.0f) aaZ = 255.0f;
					const DWORD czC = RCol((int)(52.0f * lumZ), (int)(57.0f * lumZ),
					                       (int)(66.0f * lumZ), (int)aaZ);
					float ucap = 0.0f, vcap = 0.0f;
					if (useTex) {
						VECTOR3 dpd = Pz - pC;
						dpd = dpd / length(dpd);
						double sl = dotp(dpd, s_rn.axis);
						if (sl > 1.0) sl = 1.0; else if (sl < -1.0) sl = -1.0;
						const double lat = asin(sl);
						const double lon = atan2(dotp(dpd, pe2), dotp(dpd, pe1));
						const double um = lon * s_rn.groundR * cos(lat);
						const double vm = lat * s_rn.groundR;
						const double ur = um * LAY_ROTC[Ld] - vm * LAY_ROTS[Ld];
						const double vr = um * LAY_ROTS[Ld] + vm * LAY_ROTC[Ld];
						ucap = (float)(ur * ((double)rainCloudN / LAY_REP[Ld])) + t * LAY_DRIFT[Ld] * ((float)rainCloudN * (1.0f / 1024.0f));
						vcap = (float)(vr * ((double)rainCloudN / LAY_REP[Ld]));
					}
					for (int az = 0; az < DK_AZ; az++) {
						if (!dko[0][az] || !dko[0][az + 1]) continue;
						emitDeck(cxz, cyz, dhL, czC, ucap, vcap,
						         dkx[0][az],     dky[0][az],     dkd[0][az],     dkc[0][az],     dku[0][az],     dkv[0][az],
						         dkx[0][az + 1], dky[0][az + 1], dkd[0][az + 1], dkc[0][az + 1], dku[0][az + 1], dkv[0][az + 1]);
					}
				}
			}
			for (int e2 = 0; e2 < 12; e2++) {
				for (int az = 0; az < DK_AZ; az++) {
					if (!dko[e2][az] || !dko[e2][az + 1] || !dko[e2 + 1][az] || !dko[e2 + 1][az + 1]) continue;
					emitDeck(dkx[e2][az],     dky[e2][az],     dkd[e2][az],     dkc[e2][az],     dku[e2][az],     dkv[e2][az],
					         dkx[e2][az + 1], dky[e2][az + 1], dkd[e2][az + 1], dkc[e2][az + 1], dku[e2][az + 1], dkv[e2][az + 1],
					         dkx[e2 + 1][az], dky[e2 + 1][az], dkd[e2 + 1][az], dkc[e2 + 1][az], dku[e2 + 1][az], dkv[e2 + 1][az]);
					emitDeck(dkx[e2][az + 1],     dky[e2][az + 1],     dkd[e2][az + 1],     dkc[e2][az + 1],     dku[e2][az + 1],     dkv[e2][az + 1],
					         dkx[e2 + 1][az + 1], dky[e2 + 1][az + 1], dkd[e2 + 1][az + 1], dkc[e2 + 1][az + 1], dku[e2 + 1][az + 1], dkv[e2 + 1][az + 1],
					         dkx[e2 + 1][az],     dky[e2 + 1][az],     dkd[e2 + 1][az],     dkc[e2 + 1][az],     dku[e2 + 1][az],     dkv[e2 + 1][az]);
				}
			}
		}

	// ---- 0.7 THE BOLTS (rain lightning part 2) -------------------------------
	// Camera-facing textured quads off the baked atlas, ground to deck base at the
	// event's azimuth/distance. Additive - a bolt IS light, and the premultiplied
	// black background adds nothing, so transparency is free. The event seed picked
	// the slot, so every re-strike of the flicker re-lights the IDENTICAL channel.
	// Per-vertex depth: a bolt 5 km out passes behind the vessel per pixel.
	if (nFl > 0 && hBoltTex) {
		const DWORD lc = g_fx.ltgColour;
		const float tR = (float)(lc & 0xFF), tG = (float)((lc >> 8) & 0xFF), tB = (float)((lc >> 16) & 0xFF);
		for (int q2 = 0; q2 < nFl; q2++) {
			if (fl[q2].type < 1 || fl[q2].I <= 0.03f) continue;
			const VECTOR3 G0 = cc.pos + east * fl[q2].px + north * fl[q2].py - up * camAGL;
			const VECTOR3 G1 = G0 + up * 1350.0;
			float bx0, by0, bx1, by1; double bz0, bz1;
			if (!ProjPx(cc, G0, viewW, viewH, bx0, by0, bz0)) continue;
			if (!ProjPx(cc, G1, viewW, viewH, bx1, by1, bz1)) continue;
			// screen half-width from the slot aspect (256 wide : 1024 tall = 337 m
			// for a 1350 m channel), at the bolt's own depth
			const double zm = (bz0 + bz1) * 0.5;
			float whx = (float)(169.0 / (zm * cc.tanAp) * (viewH * 0.5));
			if (whx < 0.8f) continue;
			float ex2 = bx1 - bx0, ey2 = by1 - by0;
			const float el3 = sqrtf(ex2 * ex2 + ey2 * ey2);
			if (el3 < 2.0f) continue;
			ex2 /= el3; ey2 /= el3;
			const float ppx = -ey2, ppy = ex2;
			const int   sx2 = fl[q2].tex % 8, sy2 = fl[q2].tex / 8;
			const float u0 = (float)(sx2 * 256), u1 = u0 + 256.0f;
			const float v0 = (float)(sy2 * 1024), v1 = v0 + 1024.0f;
			const float Iq = fl[q2].I;
			const DWORD bc = RCol((int)(tR * Iq), (int)(tG * Iq), (int)(tB * Iq), 255);
			DWORD boltCol = bc;              // per-pass colour; emitB reads this
			auto emitB = [&](float x1f, float y1f, float d1f, float uu1, float vv1,
			                 float x2f, float y2f, float d2f, float uu2, float vv2,
			                 float x3f, float y3f, float d3f, float uu3, float vv3) {
				if (boltN + 3 > RAIN_BOLT_TRI * 3) return;
				boltVtx[boltN].x = x1f; boltVtx[boltN].y = y1f; boltVtx[boltN].u = uu1;
				boltVtx[boltN].v = vv1; boltVtx[boltN].c = FogColNearGround(boltCol, d1f); boltDepth[boltN] = d1f * 0.95f; boltN++;
				boltVtx[boltN].x = x2f; boltVtx[boltN].y = y2f; boltVtx[boltN].u = uu2;
				boltVtx[boltN].v = vv2; boltVtx[boltN].c = FogColNearGround(boltCol, d2f); boltDepth[boltN] = d2f * 0.95f; boltN++;
				boltVtx[boltN].x = x3f; boltVtx[boltN].y = y3f; boltVtx[boltN].u = uu3;
				boltVtx[boltN].v = vv3; boltVtx[boltN].c = FogColNearGround(boltCol, d3f); boltDepth[boltN] = d3f * 0.95f; boltN++;   // (ab): the foot on the ground
			};
			// FIVE PASSES (round 4 - the WIDENED copies fanned apart, because a
			// leaning bolt's filament sits off-centre in its slot, and scaling the
			// quad about its centreline slides the filament sideways). Bloom is a
			// SCREEN-SPACE phenomenon, so the glow passes are small PIXEL OFFSETS of
			// the same-size quad - a literal 5-tap blur: core + two taps each side,
			// concentric around every filament by construction. The Bolt bloom
			// slider scales the tap intensities; 0 = the crisp core only.
			const float bloomK = clampf(g_fx.rainBoltBloom, 0.0f, 2.0f);
			const float PASS_OFF[5] = { 0.0f, -2.6f, 2.6f, -6.5f, 6.5f };
			const float PASS_I[5]   = { 1.0f, 0.42f * bloomK, 0.42f * bloomK,
			                            0.20f * bloomK, 0.20f * bloomK };
			for (int ps2 = 0; ps2 < 5; ps2++) {
				if (PASS_I[ps2] <= 0.01f) continue;
				const float ox2 = ppx * PASS_OFF[ps2], oy2 = ppy * PASS_OFF[ps2];
				const DWORD pc2 = RCol((int)(tR * Iq * PASS_I[ps2]),
				                       (int)(tG * Iq * PASS_I[ps2]),
				                       (int)(tB * Iq * PASS_I[ps2]), 255);
				// corners: bottom(G0) gets v1 (texture bottom), top(G1) gets v0
				const float ax = bx0 - ppx * whx + ox2, ay = by0 - ppy * whx + oy2;
				const float bx = bx0 + ppx * whx + ox2, by = by0 + ppy * whx + oy2;
				const float cx = bx1 + ppx * whx + ox2, cy = by1 + ppy * whx + oy2;
				const float dx = bx1 - ppx * whx + ox2, dy = by1 - ppy * whx + oy2;
				boltCol = pc2;
				emitB(ax, ay, (float)bz0, u0, v1,  bx, by, (float)bz0, u1, v1,  cx, cy, (float)bz1, u1, v0);
				emitB(ax, ay, (float)bz0, u0, v1,  cx, cy, (float)bz1, u1, v0,  dx, dy, (float)bz1, u0, v0);
			}
		}
	}
	}

	// ---- 1. THE RAIN SHEET ---------------------------------------------------
	// ⚠️ ROUND 2 REPLACED A WORLD-ANCHORED DROP FIELD WITH A SCREEN-SPACE SHEET, and the
	// reason is a number. Real rain is on the order of several hundred drops per cubic
	// metre; filling the 75 m box the first build used would take millions of them. The
	// physical approach cannot reach the DENSITY that makes rain look like rain, and no
	// amount of tuning gets there - his verdict on round 1 was "far too few streaks",
	// which is exactly what that arithmetic predicts.
	//
	// So the sheet is a fake, and it is the RIGHT fake: rain is between the viewer and
	// the world, so a layer between the viewer and the world is what it is. (His
	// reference was Project Zomboid, which does the same thing, and it reads as rain.)
	// One consequence to accept knowingly: it draws OVER the vessels rather than being
	// clipped by them, and that is correct - a drop a metre from your eye really is in
	// front of a ship fifty metres away.
	//
	// THREE LAYERS, because a single flat sheet reads as a decal on the lens. Different
	// speeds, lengths and brightnesses are exactly what parallax does to a real curtain,
	// and giving the eye that gradient is what buys back the depth the fake gave away.
	// Near: few, long, fast, bright. Far: many, short, slow, dim.
	const float dens  = clampf(g_fx.rainDensity, 0.0f, 2.0f);
	const float lenK  = clampf(g_fx.rainStreak,  0.0f, 2.0f);
	const float spdK  = clampf(g_fx.rainSpeed,   0.0f, 2.0f);
	const float glowK = clampf(g_fx.rainStreakA, 0.0f, 2.0f);

	// ⚠️ THE SHEET IS DIRECTION-AWARE (round 3, his report). Screen-space top-to-bottom
	// was right only for a LEVEL camera: point the camera at the sky and real rain comes
	// AT you - streaks radiate outward from the fall direction's vanishing point,
	// foreshortening to near-dots dead ahead; look straight down and they converge into
	// the point under you. The sheet keeps its fake density, but each streak now takes
	// its direction from the WORLD fall vector projected at its own screen position:
	//     screen flow at normalized (x, y)  ~  (d.x - x*d.z,  d.y - y*d.z)
	// with d the fall direction in CAMERA axes - the exact perspective flow field of a
	// parallel velocity. A level camera reproduces the old vertical fall identically
	// (d.z ~ 0 collapses the formula to a constant direction), and every tilted view
	// gets the correct radial pattern from the same three lines.
	// The projected magnitude also scales streak LENGTH: rain seen along its own motion
	// is specks, not lines - which is what looking up into rain actually shows.
	const float angR = clampf(g_fx.rainAngle, -15.0f, 15.0f) * 0.0174532925f;
	// The slant is a WORLD tilt now (about the east axis - the wind blows somewhere),
	// not a screen skew, so it survives the camera turning.
	// ⚠️ AND SINCE 2026-08-27 THE FALL VECTOR IS RELATIVE TO THE VESSEL - invariant
	// 25(e) reaching the one effect that predated it (his report: "even at full speed
	// they fall vertically... a big mismatch between what's going on ON the window and
	// what is happening outside of it"). Rain falls at ~9 m/s terminal velocity
	// relative to the AIR; what the vessel - and every camera that travels with it -
	// actually meets is that minus its own airspeed vector. Parked, the subtraction is
	// zero and nothing about the approved look moves; at 100 m/s the streaks come at
	// you near-horizontal and RUSH, which is what the VC runners already do, so the
	// two sides of the glass finally tell one story. Apparent speed and streak length
	// ride the magnitude (capped - at Mach the honest 30x would be a strobe).
	const VECTOR3 vFall = (up * (-cosf(angR)) + east * sinf(angR)) * 9.0;
	const VECTOR3 relV  = vFall - s_rn.vAirG;
	const double  relM  = length(relV);
	const VECTOR3 fallW = (relM > 1e-3) ? relV / relM : up * (-1.0);
	const float   relK  = (float)(relM / 9.0);           // 1.0 parked
	// ⚠️ COMPRESSED, NOT LINEAR (his report: "100 knots in the rain is like the
	// lightspeed effect in Star Wars"). Linear pinned the 6x cap by ~55 m/s, because
	// relative speed is already 5.7x terminal there - the DIRECTION carries the truth
	// of the physics; the visual aggression has to arrive on a slower curve. Roots
	// put 100 kn at ~2.6x speed / ~1.8x length and save the caps for near-Mach.
	const float   spdMul = fminf(powf(fmaxf(relK, 0.01f), 0.40f), 3.0f);
	const float   lenMul = fminf(powf(fmaxf(relK, 0.01f), 0.25f), 1.7f);
	// ... and the phase INTEGRATES spdMul instead of multiplying the clock by it -
	// see rainSheetPh's comment in OroModule.h (his thrust/backwards diagnosis).
	{
		const float dtp = (rainSheetPhT >= 0.0f && t > rainSheetPhT) ? (t - rainSheetPhT) : 0.0f;
		rainSheetPhT = t;
		rainSheetPh += dtp * spdMul;
		if (rainSheetPh > 1.0e4f) rainSheetPh -= 1.0e4f;   // bounded; u wraps anyway
	}
	const VECTOR3 dC = tmul(cc.rot, fallW);          // fall direction, camera axes
	// THE VANISHING-POINT MASK, RE-SHAPED BY SPEED (his ask: "gently mask the center").
	// Parked (spdMul 1) these reproduce the approved 0.50-floor/0.40-radius mask
	// exactly; as the flow speeds up the masked disc WIDENS and DEEPENS, so the
	// starburst's focus is a soft clearing rather than a knot of sharp spokes.
	const float vpR = 0.40f + 0.28f * (spdMul - 1.0f);
	const float vpF = clampf(0.50f - 0.16f * (spdMul - 1.0f), 0.15f, 0.50f);

	const float W = (float)viewW, H = (float)viewH;
	const float margin = H * 0.20f + 40.0f;
	const float aspect = (H > 0.0f) ? W / H : 1.0f;
	const float tAp = (float)cc.tanAp;

	struct Layer { float frac, len, spd, alpha, wid; };
	const Layer LAY[3] = {
		//  share  length(xH)  speed   alpha   width(px)
		{   0.56f,   0.022f,   0.85f,  0.42f,  1.05f },   // far: a haze of short ticks
		{   0.30f,   0.048f,   1.45f,  0.68f,  1.35f },   // mid
		{   0.14f,   0.105f,   2.40f,  1.00f,  1.90f },   // near: the ones you notice
	};

	const int NTOT = (int)(RAIN_MAX_STREAK * clampf(dens * 0.5f, 0.0f, 1.0f)
	                       * (0.35f + 0.65f * I));

	// THE RAIN SHIELD precompute (render path: snapshot data only, invariant 1).
	// The camera is the RENDER camera, so the vessel position must be the render
	// epoch too (21a: mixing epochs threw the plume 500 m off its own hull) -
	// patch (k2) provides it, the pre-step position is the fallback.
	const bool shieldOn = rainVC && !s_shieldTri.empty();
	VECTOR3 shVp = s_rn.vPos;
	if (shieldOn && pCore && pCore->CanGetRenderObjPos()) {
		VECTOR3 rp;
		if (pCore->GetRenderObjPos(s_rn.hV, &rp)) shVp = rp;
	}

	int base = 0;
	for (int L = 0; L < 3; L++) {
		const int NL = (int)(NTOT * LAY[L].frac);
		const float slen = H * LAY[L].len * (0.25f + 0.95f * lenK) * lenMul;
		const float wpx  = LAY[L].wid;
		const float spd  = LAY[L].spd * (0.35f + 0.85f * spdK);   // per-layer rate; the
		                                                          // physics factor lives
		                                                          // in rainSheetPh now

		const float travel = 1.30f * H;

		for (int k = 0; k < NL; k++) {
			const int i = base + k;
			// Progress along the flow: a pure function of a hashed phase and the clock,
			// wrapping at 1. No state, so it survives a pause and cannot drift.
			const float ph = hashf(i * 5 + 13);
			float u = ph + rainSheetPh * spd;   // integrated phase - never runs backward
			u -= floorf(u);

			// A fixed screen ANCHOR; the streak slides through it along its local flow
			// direction and respawns there, the pop hidden by the end fades below.
			const float axp = hashf(i * 5 + 29) * (W + 2.0f * margin) - margin;
			const float ayp = hashf(i * 5 + 41) * (H + 2.0f * margin) - margin;

			// The perspective flow of the fall vector at this anchor (see the header).
			// ⚠️ WITH PER-STREAK WIND SCATTER (round 4, his report). The pure formula
			// aims every streak at ONE exact pixel - mathematically right for perfectly
			// parallel velocities, and real rain is not perfectly parallel: turbulence
			// spreads the fall directions a few degrees. A small hashed offset per streak
			// scatters the individual vanishing points over a patch of sky, so looking
			// straight up or down gives a churn of short dashes instead of a starburst
			// with a crisp focus. At a level view the same offset is ~4 degrees of
			// direction noise - invisible, or rather: it just looks like rain.
			const float jx = (hashf(i * 7 + 53) - 0.5f) * 0.15f;
			const float jy = (hashf(i * 7 + 67) - 0.5f) * 0.15f;
			const float xn = ((axp / W) - 0.5f) * 2.0f * tAp * aspect;
			const float yn = -(((ayp / H) - 0.5f) * 2.0f * tAp);
			const float sfx = (float)(dC.x - xn * dC.z) + jx;
			const float sfy = (float)(dC.y - yn * dC.z) + jy;
			const float sm = sqrtf(sfx * sfx + sfy * sfy);
			if (sm < 1e-4f) continue;                 // dead on the vanishing point
			const float pdx = sfx / sm;               // pixel axes: screen y runs down
			const float pdy = -sfy / sm;

			const float disp = (u - 0.5f) * travel;
			const float x0 = axp + pdx * disp;
			const float y0 = ayp + pdy * disp;
			if (x0 < -margin || x0 > W + margin || y0 < -margin || y0 > H + margin) continue;

			// ⚠️ FORESHORTENING AND THE CENTRE MASK ARE SAMPLED WHERE THE STREAK IS
			// DRAWN, NOT AT ITS ANCHOR (2026-08-27, his second report). A streak slides
			// up to +-0.65 H from its anchor along the flow line - and at speed the
			// flow lines are RADIAL, so full-length full-alpha streaks anchored at the
			// screen edges slid backwards THROUGH the centre carrying their
			// edge-sampled properties. The mask only ever caught streaks ANCHORED
			// there, which is why the starburst's focus stayed full of sharp spokes
			// however the mask was tuned. Direction stays the anchor's (the dash is
			// straight along its own flow line); length and mask follow the dash.
			const float xn2 = ((x0 / W) - 0.5f) * 2.0f * tAp * aspect;
			const float yn2 = -(((y0 / H) - 0.5f) * 2.0f * tAp);
			const float sfx2 = (float)(dC.x - xn2 * dC.z) + jx;
			const float sfy2 = (float)(dC.y - yn2 * dC.z) + jy;
			const float sm2 = sqrtf(sfx2 * sfx2 + sfy2 * sfy2);
			const float fLen = clampf(sm2, 0.16f, 1.45f);  // foreshortening
			// ... and the centre mask he asked for: near the vanishing point the dashes
			// thin out gently rather than piling into a bright knot. Smoothstepped and
			// speed-shaped (vpR/vpF above) since 2026-08-27.
			float vpT = clampf(sm2 / vpR, 0.0f, 1.0f);
			vpT = vpT * vpT * (3.0f - 2.0f * vpT);
			const float vpMask = vpF + (1.0f - vpF) * vpT;

			const float sl2 = slen * fLen;
			const float x1 = x0 - pdx * sl2;
			const float y1 = y0 - pdy * sl2;

			// THE RAIN SHIELD kill: place this streak's rain at its own depth along
			// its own ray, move to the VESSEL frame, and look straight up - a roof
			// panel within RAINSHIELD_H overhead means indoors, and indoors it does
			// not rain. (Roof height per panel as the centroid: shield panels are
			// authored near-flat, so barycentric exactness buys nothing.)
			if (shieldOn) {
				const float sxn = ((x0 / W) - 0.5f) * 2.0f * tAp * aspect;
				const float syn = -(((y0 / H) - 0.5f) * 2.0f * tAp);
				const VECTOR3 dW = mul(cc.rot, unit(_V((double)sxn, (double)syn, 1.0)));
				const VECTOR3 P = tmul(s_rn.vRot, (cc.pos + dW * (double)RAIN_VC_Z) - shVp);
				bool indoors = false;
				for (size_t st = 0; st + 2 < s_shieldTri.size(); st += 3) {
					const VECTOR3& a = s_shieldTri[st];
					const VECTOR3& b = s_shieldTri[st + 1];
					const VECTOR3& c = s_shieldTri[st + 2];
					const double d1 = (P.x - b.x) * (a.z - b.z) - (a.x - b.x) * (P.z - b.z);
					const double d2 = (P.x - c.x) * (b.z - c.z) - (b.x - c.x) * (P.z - c.z);
					const double d3 = (P.x - a.x) * (c.z - a.z) - (c.x - a.x) * (P.z - a.z);
					const bool neg = (d1 < 0.0) || (d2 < 0.0) || (d3 < 0.0);
					const bool pos = (d1 > 0.0) || (d2 > 0.0) || (d3 > 0.0);
					if (neg && pos) continue;               // outside this panel's footprint
					const double yTri = (a.y + b.y + c.y) * (1.0 / 3.0);
					if (yTri > P.y && yTri - P.y < (double)RAINSHIELD_H) { indoors = true; break; }
				}
				if (indoors) continue;
			}

			// end fade: the respawn at the travel ends must never pop in view
			const float uf = (u < 0.10f) ? u * 10.0f
			               : ((u > 0.90f) ? (1.0f - u) * 10.0f : 1.0f);

			const float A = 190.0f * I * LAY[L].alpha * glowK * lightNow * uf * vpMask;   // lightNow: the light on the rain
			if (A < 1.5f) continue;

			// Rain reads LIGHT against a dark sky and merely grey against a bright one,
			// which is what a low-alpha near-white gives for free. Cool, not neutral.
			const DWORD cHi = RCol(200, 212, 230, (int)(A > 255.0f ? 255.0f : A));
			const DWORD cLo = RCol(200, 212, 230, 0);   // the tail fades to nothing

			// Perpendicular in screen space.
			const float perpx = -pdy * (wpx * 0.5f);
			const float perpy =  pdx * (wpx * 0.5f);

			// Depth: EXTERNAL keeps the deliberate 0.5 m - nearer than anything the
			// scene holds, so the clip never removes it (the 28b trade: a drop a
			// metre from your eye IS in front of a ship fifty metres away).
			// ⚠️ IN THE VC THAT EXACT PROPERTY WAS THE BUG (2026-08-23, his
			// screenshots: rain over his knees) - 0.5 m beats every cabin surface
			// past half a metre, so only the overhead panel clipped it and it
			// rained inside. There the sheet sits at RAIN_VC_Z, just beyond a
			// cockpit's interior: the cabin wins everywhere, while the world
			// through the glass (always farther than the canopy) still cannot
			// occlude the rain in front of it. Cabins BIGGER than that bound are
			// the RAIN SHIELD's job (the authored roof, above) - one scalar was
			// never going to describe a real interior.
			const float Z = rainVC ? RAIN_VC_Z : 0.5f;
			emit(x0 + perpx, y0 + perpy, Z, cHi,  x0 - perpx, y0 - perpy, Z, cHi,
			     x1 + perpx, y1 + perpy, Z, cLo);
			emit(x0 - perpx, y0 - perpy, Z, cHi,  x1 - perpx, y1 - perpy, Z, cLo,
			     x1 + perpx, y1 + perpy, Z, cLo);
		}
		base += NL;
	}

	// ---- 2. DROPS LANDING IN WATER -------------------------------------------
	// Expanding rings on the ground. Only worth drawing from close to it: from altitude
	// they are sub-pixel and would just be noise in the budget.
	//
	// ⚠️ STRATIFIED BY DISTANCE (round 3). One field with a 34 m wrap period meant no
	// ring could exist further than 17 m away, so the splashes stopped in a hard circle a
	// few metres out and the rest of the runway was dry - which is exactly what he saw.
	// Simply enlarging the field would have thinned the near ground to nothing, because
	// the same count would be spread over the square of the radius.
	// So there are three fields at growing wrap periods, sharing the budget. World density
	// therefore FALLS with distance, which sounds wrong and is very nearly right on screen:
	// a patch of ground at 200 m packs its splashes into a small fraction of the pixels it
	// would occupy at 20 m, so a lower world density there lands at a similar density in
	// the frame. Same shape of reasoning as the streak sheet's three layers.
	if (camAGL < 150.0f && camAGL > -5.0f && g_fx.rainPuddle > 0.001f) {
		const float RING_LIFE = 0.55f;               // s, birth -> gone
		const float RING_R    = 0.85f;               // m, final radius
		const float STRAT_FIELD[3] = { 26.0f, 82.0f, 240.0f };   // m, wrap period
		// LOD GEOMETRY (round 4, his design: "hexagons close, rhombuses mid range and
		// triangles farther away") - the vertex ceiling made more splashes impossible
		// at a flat 36 tris/ring, and the band cut saves more than the corner cut.
		// ⚠️ ROUND 5: the LOD key is the ring's PROJECTED SIZE, not its stratum. The
		// strata are WRAP FIELDS - the 240 m field tiles right up to the camera - so
		// stratum-tied shapes put full-size triangles among the hexagons and broke
		// the illusion on sight. Screen size cannot: every ring big enough to have a
		// readable shape is a hexagon, wherever it spawned.
		const int   STRAT_N[3]    = { RAIN_RING_N0, RAIN_RING_N1, RAIN_RING_N2 };

		// ⚠️ TWO FIELDS (his ask, 2026-08-22). The camera field carries the "I am
		// standing in rain" close-up; a SECOND, identical field centres on the FOCUS
		// VESSEL, so a chase camera hundreds of metres out still sees the ground fizzing
		// around the ship - which is where the eye looks. The vessel position is taken at
		// the RENDER epoch (patch k2) like every other world anchor here.
		// Each field runs the FULL ring budget: the rings live in their own HPOLY now
		// (his per-mesh-group observation applied - the 65535 ceiling is per POLY), so
		// the halving that ducked the single-buffer ceiling is gone. The vessel field
		// still stands down within 40 m of the camera, where the two would coincide.
		float vfE = 0.0f, vfN = 0.0f;
		bool  vField = false;
		{
			VECTOR3 vP = s_rn.vPos;
			if (pCore && pCore->CanGetRenderObjPos()) {
				VECTOR3 rp;
				if (pCore->GetRenderObjPos(s_rn.hV, &rp)) vP = rp;
			}
			const VECTOR3 vRel = vP - cc.pos;
			vfE = (float)dotp(vRel, east);
			vfN = (float)dotp(vRel, north);
			vField = (vfE * vfE + vfN * vfN) > 40.0f * 40.0f;
		}
		const int   nFld = vField ? 2 : 1;

		// ⚠️ THE LATTICE ANCHOR (2026-08-25, third attempt and the one that is actually
		// right) - what makes the rings stay on the ground.
		//
		// Attempt 1 subtracted camE/camN, which are structurally zero, so the field was
		// glued to the CAMERA. Attempt 2 used the VESSEL, which is a real planet-fixed
		// point measured through an orthonormal basis - correct while parked, and he
		// immediately found the hole by taxiing: the anchor moves with the ship, so the
		// splashes taxi with it.
		//
		// What is needed is a point on the WORLD that neither the camera nor the vessel
		// drags around, and a sphere offers no globally isometric flat coordinate - so we
		// keep our own reference and measure against it. lon/lat against a FIXED reference
		// latitude is isometric to second order over the field's few hundred metres: with
		// cos() evaluated at the REFERENCE rather than at the moving point, travelling
		// north no longer bleeds into the east coordinate, which is precisely what broke
		// the plate-carree attempt.
		// Re-anchored only after ~10 km of travel, so a taxi never triggers it; when it
		// does fire the lattice reshuffles for one frame, at a distance and speed where
		// the rings are sub-pixel or gated off anyway.
		const VECTOR3 pY = crossp(s_rn.axis, s_rn.pax1);
		double sLat = dotp(camRel, s_rn.axis) / camR;
		if (sLat >  1.0) sLat =  1.0;
		if (sLat < -1.0) sLat = -1.0;
		const double camLat = asin(sLat);
		const double camLon = atan2(dotp(camRel, pY), dotp(camRel, s_rn.pax1));

		double dLon = camLon - ringRefLon;
		while (dLon >  PI) dLon -= PI2;          // the antimeridian, handled once here
		while (dLon < -PI) dLon += PI2;
		double refE = camR * cos(ringRefLat) * dLon;
		double refN = camR * (camLat - ringRefLat);
		if (ringRefBody != s_rn.hRef || fabs(refE) > 10000.0 || fabs(refN) > 10000.0) {
			ringRefBody = s_rn.hRef;
			ringRefLon  = camLon;
			ringRefLat  = camLat;
			refE = 0.0;
			refN = 0.0;
		}
		const float camAE = (float)refE;         // metres east of the reference
		const float camAN = (float)refN;         // metres north of it

	  for (int fld = 0; fld < nFld; fld++) {
		const float fE = (fld == 1) ? vfE : 0.0f;
		const float fN = (fld == 1) ? vfN : 0.0f;
	  for (int st = 0; st < 3; st++) {
		const float FIELD = STRAT_FIELD[st];
		const float camFE = camAE;      // the lattice anchor - see the note above
		const float camFN = camAN;
		const int   NRING = (int)(STRAT_N[st]
		                          * clampf(g_fx.rainPuddle * 0.5f, 0.0f, 1.0f) * I);
		const int   jofs  = st * 7919 + fld * 33331; // separate hash streams per stratum+field

		for (int j0 = 0; j0 < NRING; j0++) {
			const int j = j0 + jofs;
			// Lifecycle by EPOCH, so a ring is born, expands, fades and is replaced
			// somewhere else - no stored state, nothing to expire (the soot idiom).
			const float ph = hashf(j * 17 + 5);
			const float u  = t / RING_LIFE + ph;
			const int   ep = (int)floorf(u);
			const float tt = u - (float)ep;

			const float ox = hashf(j * 31 + ep * 977 + 3) * FIELD;
			const float oy = hashf(j * 31 + ep * 977 + 9) * FIELD;
			// fE/fN slide the wrap window so the field tiles around its OWN centre -
			// the camera for field 0, the vessel for field 1 - while the ring positions
			// stay functions of world coordinates either way.
			const float dx = fE + wrapc(ox - camFE - fE, FIELD);
			const float dy = fN + wrapc(oy - camFN - fN, FIELD);

			// ⚠️ THE HULL IS AN UMBRELLA (round 4, his screenshot: splashes under the
			// vessel "exactly where the raindrops shouldn't be able to reach"). The
			// VESSEL field masks its own footprint: inside the hull's plan radius most
			// rings are suppressed - a few survive, because water running off the hull
			// does land there - ramping back to full density just past the wingtips.
			// The CAMERA field is untouched: a camera is a viewpoint, not an umbrella.
			// Epoch-hashed, so the surviving few move spot to spot like everything else.
			if (fld == 1) {
				const float ux = dx - vfE, uy = dy - vfN;
				const float footR = 0.65f * (float)s_rn.vSize;
				const float dv2 = ux * ux + uy * uy;
				if (dv2 < footR * footR * 1.44f) {
					const float dv = sqrtf(dv2);
					const float keepP = (dv < footR) ? 0.15f
					                  : 0.15f + 0.85f * (dv - footR) / (0.2f * footR);
					if (hashf(j * 53 + ep * 389 + 11) > keepP) continue;
				}
			}

			const float r = RING_R * (0.10f + 0.90f * tt);

			// The ground point directly under that spot.
			const VECTOR3 G = cc.pos + east * dx + north * dy - up * camAGL;

			// ⚠️ THE G13 GUARD, again. A distant ring projects to a few pixels, and an
			// alpha-blended figure crammed into them deposits its full opacity there -
			// which is how a far field turns into a carpet of hard dots. Fade it out with
			// projected size and drop it once it cannot be a ring any more.
			float gx, gy; double gz;
			if (!ProjPx(cc, G, viewW, viewH, gx, gy, gz)) continue;
			const float rpx = (float)(r / (gz * cc.tanAp) * (viewH * 0.5));
			if (rpx < 1.1f) continue;
			const float sizeF = (rpx < 4.0f) ? (rpx - 1.1f) / 2.9f : 1.0f;

			// the LOD pick (see the round-5 note above): hexagon while the shape can
			// be read, rhombus in the blur zone, triangle where only flicker survives
			int NSEG, NB;
			if      (rpx > 8.0f) { NSEG = 6; NB = 4; }
			else if (rpx > 3.5f) { NSEG = 4; NB = 3; }
			else                 { NSEG = 3; NB = 2; }

			const float a = powf(1.0f - tt, 1.6f) * 150.0f * I * sizeF
			              * clampf(g_fx.rainPuddle, 0.0f, 2.0f) * lightNow;   // unlit ground shows no rings
			if (a < 2.0f) continue;

			// ⚠️ A SPLASH IS A CONTRAST FEATURE, NOT A BRIGHT ONE - round 2, and the first
			// build got this wrong in a way that made it invisible rather than subtle.
			// The rings were near-white and alpha-blended, which over SUNLIT CONCRETE is
			// near-white over near-white: no visible change at any setting. They only
			// read in the reference photographs because the ground there is dark wet
			// asphalt. So the ring is now an EDGE - a dark trough with a bright crest,
			// which is also what a real one is (disturbed water, then a lit rim) and
			// which shows up on a light background and a dark one alike.
			float px[4][7], py[4][7]; double pz[4][7]; bool ok[4][7] = { { false } };
			// radii and colours per stratum: NEAR keeps the full transparent -> dark
			// trough -> bright crest -> transparent profile; MID drops the inner fade;
			// FAR is one dark->bright band - at a few pixels, a twinkle is the story
			const DWORD cz  = RCol( 40,  48,  62, 0);
			const DWORD cd  = RCol( 40,  48,  62, (int)(a * 0.85f));
			const DWORD cb2 = RCol(228, 238, 250, (int)a);
			const DWORD cz2 = RCol(228, 238, 250, 0);
			float rr[4]; DWORD band[4];
			if (NB == 4)      { rr[0] = r * 0.34f; rr[1] = r * 0.70f; rr[2] = r * 0.89f; rr[3] = r;
			                    band[0] = cz; band[1] = cd; band[2] = cb2; band[3] = cz2; }
			else if (NB == 3) { rr[0] = r * 0.55f; rr[1] = r * 0.85f; rr[2] = r;
			                    band[0] = cd; band[1] = cb2; band[2] = cz2; }
			else              { rr[0] = r * 0.62f; rr[1] = r;
			                    band[0] = cd; band[1] = cb2; }
			bool any = false;
			for (int b = 0; b < NB; b++) {
				for (int s = 0; s <= NSEG; s++) {
					const float th = (float)s / (float)NSEG * 6.2831853f;
					const VECTOR3 P = G + east * (cosf(th) * rr[b]) + north * (sinf(th) * rr[b]);
					ok[b][s] = ProjPx(cc, P, viewW, viewH, px[b][s], py[b][s], pz[b][s]);
					any = any || ok[b][s];
				}
			}
			if (!any) continue;

			for (int s = 0; s < NSEG; s++) {
				for (int b = 0; b < NB - 1; b++) {
					if (!ok[b][s] || !ok[b][s + 1] || !ok[b + 1][s] || !ok[b + 1][s + 1]) continue;
					const DWORD ca = band[b], cbb = band[b + 1];
					emitRing(fld,
					      px[b][s],     py[b][s],     (float)pz[b][s],     ca,
					      px[b][s + 1], py[b][s + 1], (float)pz[b][s + 1], ca,
					      px[b + 1][s], py[b + 1][s], (float)pz[b + 1][s], cbb);
					emitRing(fld,
					      px[b][s + 1],     py[b][s + 1],     (float)pz[b][s + 1],     ca,
					      px[b + 1][s + 1], py[b + 1][s + 1], (float)pz[b + 1][s + 1], cbb,
					      px[b + 1][s],     py[b + 1][s],     (float)pz[b + 1][s],     cbb);
				}
			}
		}
	  }
	  }  // fld
	}

	rainActive = (rainVtxN > 0 || gndVtxN > 0 || ringCN > 0 || ringVN > 0 || deckN > 0 || boltN > 0);

	// Zero-pad the tails (invariant 3): the client locks with D3DLOCK_DISCARD and always
	// draws the CREATION count, so an unwritten tail is random VRAM on screen.
	if (rainVtxN > 0 && rainVtxN < RAIN_MAX_TRI * 3) {
		memset(&rainVtx[rainVtxN],   0, sizeof(PlasVtx) * (RAIN_MAX_TRI * 3 - rainVtxN));
		memset(&rainDepth[rainVtxN], 0, sizeof(float)   * (RAIN_MAX_TRI * 3 - rainVtxN));
	}
	if (gndVtxN > 0 && gndVtxN < RAIN_GND_TRI * 3) {
		memset(&gndVtx[gndVtxN],   0, sizeof(PlasVtx) * (RAIN_GND_TRI * 3 - gndVtxN));
		memset(&gndDepth[gndVtxN], 0, sizeof(float)   * (RAIN_GND_TRI * 3 - gndVtxN));
	}
	if (ringCN > 0 && ringCN < RAIN_RINGP_TRI * 3) {
		memset(&ringVtxC[ringCN], 0, sizeof(PlasVtx) * (RAIN_RINGP_TRI * 3 - ringCN));
		memset(&ringDepC[ringCN], 0, sizeof(float)   * (RAIN_RINGP_TRI * 3 - ringCN));
	}
	if (ringVN > 0 && ringVN < RAIN_RINGP_TRI * 3) {
		memset(&ringVtxV[ringVN], 0, sizeof(PlasVtx) * (RAIN_RINGP_TRI * 3 - ringVN));
		memset(&ringDepV[ringVN], 0, sizeof(float)   * (RAIN_RINGP_TRI * 3 - ringVN));
	}
	if (deckN > 0 && deckN < RAIN_GND_TRI * 3) {
		memset(&deckVtx[deckN],   0, sizeof(TexVtxP) * (RAIN_GND_TRI * 3 - deckN));
		memset(&deckDepth[deckN], 0, sizeof(float)   * (RAIN_GND_TRI * 3 - deckN));
	}
	if (boltN > 0 && boltN < RAIN_BOLT_TRI * 3) {
		memset(&boltVtx[boltN],   0, sizeof(TexVtxP) * (RAIN_BOLT_TRI * 3 - boltN));
		memset(&boltDepth[boltN], 0, sizeof(float)   * (RAIN_BOLT_TRI * 3 - boltN));
	}
}


