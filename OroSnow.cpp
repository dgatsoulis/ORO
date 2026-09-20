// ==============================================================
// OroSnow.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - SNOW (2026-09-13). THE WEATHER CHAPTER'S SECOND EFFECT.
// ----------------------------------------------------------------------------
// Two halves, and this file is the one ORO does not draw. The COVER: client patch (aa)
// shipped SetSnowCover dormant with the fog round, and the snow round gave it its look -
// up-facing terrain above a snow line, base tiles, pavement and hulls whiten in every
// shader family, each hull and base carrying its own share (above the line, and a hull
// not blown clean by the airflow), no snow on the water mask, none inside the cockpit.
// This file hands the client ONE number, the cover, and the line. The FLAKES are the
// rain sheet in snow mode, in OroRain.cpp: same anchors, same phase integral, same
// shield, same light, because rain and snow are MUTUALLY EXCLUSIVE by his rule ("if we
// have rain, snow is off and vice versa. Keep the geometry budget for the effect").
//
// THE COVER MODEL (his calls, 2026-09-13): a STANDING cover - the Cover slider, applied
// at once, which is how you get an alpine snow line with nothing falling and how you A/B
// without waiting - PLUS what the snowfall LAYS DOWN, accumulating on SIM time (the
// agreed law: no sim time passes, nothing piles up - frozen under pause) toward a full
// cover over the Build-up slider's minutes, and melting at three times that when the
// fall stops. The pill going OFF clears everything at once: the rain's A/B rule.
//
// THE SIM CANNOT SAY WHERE IT IS COLD. Earth's atmosphere module is a US Standard
// atmosphere below 90 km, and it zeroes latitude and longitude after reading them -
// +15 C at sea level at the pole in January, a freezing level of 2308 m everywhere,
// forever. So the snow line is a BOUND the user sets (the vapour cone's shape, 25i), the
// readout shows the sim's honest number, and the physics half - where and when it
// snows, from latitude and the sun's declination - is the weather model's, later.
//
// THE SPLIT (invariant 1's law): UpdateSnow EVOLVES (main thread, frozen under pause);
// the SENSING is SenseRain's, which runs for either storm and publishes both envelopes
// through the rain's altitude/world gate ("do the same we do for the rain" - his call:
// the cover is gone by ~9 km up, exactly as the wet ground is); PushSnow carries the
// result to the client ON CHANGE wherever the sensing ran.
// ============================================================================

#include "OroModule.h"
#include "OroLog.h"
#include "OroState.h"
#include "gcCoreAPI.h"
#include <math.h>
#include <string.h>

namespace {

	inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }

	const float SNOW_RISE = 10.0f;   // s of REAL time, off -> full snowfall (the rain's own)

	// the last values handed to the client, so the push happens ON CHANGE (invariant 18)
	float  s_pCover = -1.0f, s_pLine = -1.0e9f, s_pLineW = -1.0f;
	float  s_pFall  = -1.0f, s_pBright = -1.0f;   // the LOOK setter's last push (round 3)
	float  s_pRelief = -1.0f, s_pSparkle = -1.0f; // ... and its step-B lanes
	int    s_pTracks = -1; float s_pTrackFade = -1.0f;   // the TRACKS setter's last push (step D)
	double s_frzT   = -1e9;          // sim time of the last freezing-level probe
	bool   s_shedNew = true;         // step E: the pool and its sampled hull belong to the session (23m)
}

// Every session start (and every teardown): the envelope, the fallen cover and the
// pushed markers all belong to the session, never to the next one (23m's rule).
void OroSnow_Reset()
{
	s_pCover = -1.0f; s_pLine = -1.0e9f; s_pLineW = -1.0f;
	s_pFall = -1.0f; s_pBright = -1.0f; s_pRelief = -1.0f; s_pSparkle = -1.0f;
	s_pTracks = -1; s_pTrackFade = -1.0f;
	s_frzT = -1e9;
	s_shedNew = true;
	g_fx.snowI = 0.0f; g_fx.snowFallCov = 0.0f; g_fx.snowCoverNow = 0.0f;
	g_fx.snowFrzLive = -1.0f; g_fx.snowAirC = 0.0f; g_fx.snowHullLineF = 1.0f; g_fx.snowFrost = 0.0f;
	g_fx.snowHullCov = -1.0f; g_fx.snowShedRate = 0.0f; g_fx.snowShedLive = 0;
}

// ============================================================================
// UpdateSnow - main thread, clbkPreStep. EVOLVES: the fall envelope on REAL time
// (invariant 4), the cover on SIM time.
// ============================================================================
void OroModule::UpdateSnow()
{
	const float dt   = (float)oapiGetSysStep();     // REAL time
	const bool  want = g_fx.masterArmed && (g_fx.snowEnabled || g_fx.snowTest);

	// ⚠️ OFF IS INSTANT (the rain's rule, 2026-08-22): the pill is how you A/B the white
	// world against the bare one. The build-up ramps, because that is the part you watch.
	if (!want) {
		g_fx.snowI = 0.0f;
		g_fx.snowFallCov = 0.0f;
		g_fx.snowCoverNow = 0.0f;
		g_fx.snowFrost = 0.0f;
	} else {
		g_fx.snowI = clampf(g_fx.snowI + dt / SNOW_RISE, 0.0f, 1.0f);

		// THE COVER ACCUMULATES ON SIM TIME. What is actually falling is the envelope times
		// the Snowfall slider (capped at 1 - past that the slider only adds flakes to the
		// sheet, it does not snow harder on the ground); a full cover takes the Build-up
		// slider's minutes at snowfall 1.0; with nothing falling it melts at a third of
		// that rate. One self-decaying scalar - the bell glow's class of state, the rain's
		// wetness precedent, not G10's pool.
		const double sdt    = oapiGetSimStep();
		const float  fall   = g_fx.snowI * clampf(g_fx.snowFall, 0.0f, 1.0f);
		const float  buildS = clampf(g_fx.snowBuild, 1.0f, 120.0f) * 60.0f;
		if (sdt > 0.0) {
			if (fall > 0.001f) g_fx.snowFallCov = clampf(g_fx.snowFallCov + (float)sdt * fall / buildS, 0.0f, 1.0f);
			else               g_fx.snowFallCov = clampf(g_fx.snowFallCov - (float)sdt / (3.0f * buildS), 0.0f, 1.0f);
		}
		g_fx.snowCoverNow = clampf(clampf(g_fx.snowCover, 0.0f, 1.0f) + g_fx.snowFallCov, 0.0f, 1.0f);

		// THE FROST (step C, 2026-09-20): ice on the VC glass - the cover's own law on its own
		// clock. It grows while snow falls, over the Frost time minutes at snowfall 1, and melts
		// at a third of that when the fall stops; the pill off clears it at once (the A/B rule).
		// The airflow strips loose snow, not ICE, so there is no shear term - sim time alone.
		if (sdt > 0.0) {
			const float frostS = clampf(g_fx.snowFrostMin, 1.0f, 120.0f) * 60.0f;
			if (g_fx.snowFrostOn && fall > 0.001f) g_fx.snowFrost = clampf(g_fx.snowFrost + (float)sdt * fall / frostS, 0.0f, 1.0f);
			else                                   g_fx.snowFrost = clampf(g_fx.snowFrost - (float)sdt / (3.0f * frostS), 0.0f, 1.0f);
		}
	}

	// THE SHED FLAKES (step E, 2026-09-20): the focus hull's mirrored cover, and what blows off it
	UpdateShed(oapiGetFocusInterface(), want ? oapiGetSimStep() : 0.0, want);

	// THE READOUTS: the air at the vessel, and the sim's freezing level - probed once a
	// second by walking the atmosphere model up from the ground until T crosses 0 C.
	// Honest and (on the standard atmosphere) constant: the number says what the sim
	// knows, which is why the line is a slider and not derived from it.
	const double simt = oapiGetSimTime();
	if (simt - s_frzT > 1.0 || simt < s_frzT) {
		s_frzT = simt;
		g_fx.snowFrzLive = -1.0f;
		VESSEL* v = oapiGetFocusInterface();
		OBJHANDLE hRef = v ? v->GetSurfaceRef() : NULL;
		if (v && hRef && oapiPlanetHasAtmosphere(hRef)) {
			ATMPARAM ap;
			oapiGetAtm(v->GetHandle(), &ap);
			g_fx.snowAirC = (float)(ap.T - 273.15);
			// bisection over 0..12 km of ALTITUDE for the first crossing of 273.15 K.
			// ⚠️ The five-argument overload: the three-argument one takes a RADIUS from
			// the planet's centre, not an altitude. Longitude and latitude are passed as
			// zero - Earth's model discards them anyway (see the header note).
			double lo = 0.0, hi = 12000.0;
			ATMPARAM a0, a1;
			oapiGetPlanetAtmParams(hRef, lo, 0.0, 0.0, &a0);
			oapiGetPlanetAtmParams(hRef, hi, 0.0, 0.0, &a1);
			if (a0.T > 273.15 && a1.T < 273.15) {
				for (int it = 0; it < 18; it++) {
					const double mid = 0.5 * (lo + hi);
					ATMPARAM am; oapiGetPlanetAtmParams(hRef, mid, 0.0, 0.0, &am);
					if (am.T > 273.15) lo = mid; else hi = mid;
				}
				g_fx.snowFrzLive = (float)(0.5 * (lo + hi));
			} else if (a0.T <= 273.15) {
				g_fx.snowFrzLive = 0.0f;                  // freezing at the ground already
			}
		}
	}

	// THE FOCUS HULL AGAINST THE SNOW LINE (round 3, 2026-09-19 - his "snow doesn't build
	// back up on the vessels": the line stood at 155 m over a 3 m apron, and nothing at
	// KSC could take snow by the law the hulls, the pavement and the terrain all obey).
	// The same factor the client computes per hull, for the Cover now readout.
	{
		VESSEL* v = oapiGetFocusInterface();
		if (v) {
			const float lw = clampf(g_fx.snowLineW, 1.0f, 3000.0f);
			g_fx.snowHullLineF = clampf(((float)v->GetAltitude() - g_fx.snowLine) / lw + 0.5f, 0.0f, 1.0f);
		}
	}
}

// ============================================================================
// UpdateShed - main thread, from UpdateSnow. SNOW BLOWING OFF THE HULL ON THE TAKE-OFF
// ROLL (snow round 3, step E, 2026-09-20 - his item 2).
// THE MIRROR. ORO cannot read the client's per-hull cover state (vVessel::oroSnow, patch
// (aq)); a getter was weighed against mirroring the law, and the law is deterministic with
// every input already on this side - the hull's dynamic pressure, the fall rate PushSnow
// hands the client, the snow-line factor, the frame cover through the same gate - so it
// is mirrored, step for step, on the same SIM time: an e-fold every 2 s at full shear on
// the 150 Pa..1.5 kPa smoothstep, laid back at the pushed fall rate, capped at the line
// factor, the instantaneous law for a hull seen for the first time, the state kept and
// re-timed while no cover is pushed. What the mirror is FOR is the DIFFERENCE: what the
// airflow took this step is snow leaving the hull, and that is the spawn - a full cover
// blown off is SHED_FULL flakes at Blow-off 1, weighted by the frame cover the hull
// actually wears. (A recreated visual restarts the client's state on the instantaneous
// law where the mirror does not; the two can differ by one shed's worth once a session,
// and the flakes are an effect, not a ledger.)
// THE FLAKES are born on the hull's UP-FACING sample points (SampleHullPoints, 28h's walk,
// this file's own buffer, resampled once per focus hull) - up along the LOCAL VERTICAL
// rather than the vessel's +Y, so a hull standing on its gear at any attitude sheds off
// whatever faces the sky - carried at most of the hull's ground speed, pushed off along
// the surface normal, and left behind as the air's drag takes them (an EXACT exponential
// toward the wind, so a warp step cannot overshoot), sinking at a flake's terminal speed,
// dead at the ground or at the end of a two-to-five-second life. Stored PLANET-LOCAL, in
// doubles: the air co-rotates with the planet, so that is the frame a free flake is at
// rest in (the trail's inertial rpos would sweep the ground past at 460 m/s). The wind
// is the sheet's (Wind + Wind from, the same construction, so flakes and sheet agree on
// which way it blows). Paused: nothing moves, nothing is born. Warp: the same physics,
// faster.
// ============================================================================
void OroModule::UpdateShed(VESSEL* v, double sdt, bool want)
{
	const OBJHANDLE hRef = (v && want) ? v->GetSurfaceRef() : NULL;
	const float amount = clampf(g_fx.snowShed, 0.0f, 2.0f);
	if (!hRef || amount <= 0.001f) {
		if (shedLive) { for (int i = 0; i < SHED_MAX; i++) shed[i].live = false; }
		shedLive = 0; shedCov = -1.0f; shedSpawnAcc = 0.0; shedVesL = _V(0.0, 0.0, 0.0);
		g_fx.snowHullCov = -1.0f; g_fx.snowShedRate = 0.0f; g_fx.snowShedLive = 0;
		return;
	}
	// a new session, a new world, a new focus hull: the pool is in the old frame, the
	// sampled points belong to the old hull (23m: a handle outlives what it points at)
	if (s_shedNew || hRef != shedRef) {
		for (int i = 0; i < SHED_MAX; i++) shed[i].live = false;
		shedLive = 0; shedRef = hRef; shedHullV = NULL; shedCov = -1.0f; shedSpawnAcc = 0.0;
		s_shedNew = false;
	}
	if (v->GetHandle() != shedHullV) {
		shedHullN = SampleHullPoints(v, shedHull, MAX_HULLPT);
		shedHullV = v->GetHandle();
		shedCov   = -1.0f;
	}

	// THE MIRROR of vVessel::Update's law (patch (aq)); coverF is the frame cover PushSnow
	// hands the client, and what the hull WEARS is coverF x its share
	const float  gate   = clampf(rainGateLive, 0.0f, 1.0f);
	const float  buildS = clampf(g_fx.snowBuild, 1.0f, 120.0f) * 60.0f;
	const float  fall   = g_fx.snowI * clampf(g_fx.snowFall, 0.0f, 1.0f) * gate / buildS;
	const float  lw     = clampf(g_fx.snowLineW, 1.0f, 3000.0f);
	const float  lineF  = clampf(((float)v->GetAltitude() - g_fx.snowLine) / lw + 0.5f, 0.0f, 1.0f);
	const float  coverF = clampf(g_fx.snowCoverNow, 0.0f, 1.0f) * gate;
	double ts = (v->GetDynPressure() - 150.0) / 1350.0;
	ts = (ts < 0.0) ? 0.0 : (ts > 1.0 ? 1.0 : ts);
	ts = ts * ts * (3.0 - 2.0 * ts);
	float lost = 0.0f;
	if (coverF > 0.0f) {
		if (shedCov < 0.0f) shedCov = lineF * (float)(1.0 - ts);
		else if (sdt > 0.0) {
			// the client's law verbatim (VVessel::Update): fresh snow settles through still air
			// only - the fall term scaled by the square of what the shear leaves, so the fixed
			// point under full shear is zero - and the floor takes the tail: under shear the
			// last five percent goes with the airflow (his third flight: at the full rate the
			// law balanced at 3.4% and the residue sat on the hull as static lattice cells)
			const double keep   = exp(-ts * sdt * 0.5);
			const double settle = (1.0 - ts) * (1.0 - ts);
			lost = (float)((double)shedCov * (1.0 - keep));
			double s = (double)shedCov * keep + (double)fall * sdt * settle;
			if (s > (double)lineF) s = (double)lineF;
			if (ts > 0.3 && s < 0.05) { lost += (float)s; s = 0.0; }
			shedCov = (s < 0.0) ? 0.0f : (float)s;
		}
	}
	g_fx.snowHullCov  = shedCov;
	g_fx.snowShedRate = (sdt > 0.0) ? lost / (float)sdt : 0.0f;

	// THE WORLD this step: the planet's frame, the hull, the local vertical, the ground,
	// the hull's ground velocity and the wind - all planet-local
	MATRIX3 prot, vrot; VECTOR3 pP, vP, gv;
	oapiGetRotationMatrix(hRef, &prot);
	oapiGetGlobalPos(hRef, &pP);
	v->GetGlobalPos(vP);
	v->GetRotationMatrix(vrot);
	v->GetGroundspeedVector(FRAME_GLOBAL, gv);
	const VECTOR3 vL  = tmul(prot, vP - pP);
	const double  vR  = length(vL);
	if (vR < 1.0) return;
	const VECTOR3 upL = vL / vR;
	const double  groundR = vR - v->GetAltitude(ALTMODE_GROUND);
	const VECTOR3 gvL = tmul(prot, gv);
	shedVesL = gvL;                                                       // the render path streaks against it
	VECTOR3 northL = _V(0.0, 1.0, 0.0) - upL * upL.y;
	if (length(northL) < 1e-6) northL = _V(1.0, 0.0, 0.0) - upL * upL.x;
	northL = unit(northL);
	const VECTOR3 eastL = crossp(upL, northL);
	const float   wdR   = clampf(g_fx.snowWindDir, 0.0f, 360.0f) * 0.0174532925f;
	const float   wsp   = clampf(g_fx.snowWind, 0.0f, 30.0f) * 0.82f;     // the sheet's mean gust
	const VECTOR3 windL = (northL * (-cos((double)wdR)) + eastL * (-sin((double)wdR))) * (double)wsp;

	// THE SPAWN: what left the hull this step, weighted by the cover it wears
	shedSpawnAcc += (double)((float)SHED_FULL * amount * coverF * lost);
	int nSpawn = (int)shedSpawnAcc;
	shedSpawnAcc -= nSpawn;
	if (nSpawn > 400) nSpawn = 400;                                       // a warp step's burst, bounded
	if (nSpawn > 0 && shedHullN > 0) {
		const VECTOR3 upV = tmul(vrot, mul(prot, upL));                   // the local vertical in the vessel frame
		static unsigned int seed = 12345u;
		auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) * (1.0f / 16777216.0f); };
		int tries = 0;
		for (int n = 0; n < nSpawn && tries < nSpawn * 6; tries++) {
			const HullPt& hp = shedHull[(int)(rnd() * (float)shedHullN) % shedHullN];
			const double fUp = dotp(hp.nrm, upV);
			if (fUp < 0.25) continue;                                     // up-facing surfaces only
			ShedPt& sp = shed[shedCur];
			shedCur = (shedCur + 1) % SHED_MAX;
			const VECTOR3 nG = mul(vrot, hp.nrm);
			const VECTOR3 pG = vP + mul(vrot, hp.pos) + nG * 0.30;   // clear of the skin from the first frame
			sp.p = tmul(prot, pG - pP);
			const VECTOR3 nL = tmul(prot, nG);
			const VECTOR3 jit = northL * (double)(rnd() - 0.5f) + eastL * (double)(rnd() - 0.5f);
			// CLUMPS, NOT SPECKS (his first flight), and one in fourteen a CHUNK (his second): a
			// slab of crust half a metre to a metre across, lifted more gently off the surface,
			// carried further with the hull before the air peels it away, tumbling as it goes
			// (the render path turns it about a hashed phase)
			sp.chunk = (rnd() < 0.07f);
			if (sp.chunk) {
				sp.v = gvL * (0.85 + 0.13 * (double)rnd()) + nL * (0.8 + 0.8 * (double)rnd()) + jit * 0.5;   // peels off with the hull, then the air takes it
				sp.life = 1.6f + 2.0f * rnd(); sp.rad = 0.40f + 0.60f * rnd();
			} else {
				sp.v = gvL * (0.80 + 0.18 * (double)rnd()) + nL * (1.0 + 1.2 * (double)rnd()) + jit * 0.8;   // lifts clear before it falls behind
				sp.life = 2.0f + 3.0f * rnd(); sp.rad = 0.10f + 0.30f * rnd();
			}
			sp.age = 0.0f; sp.hue = rnd();
			sp.live = true;
			n++;
		}
	}

	// THE ADVECTION: the drag toward the wind exactly, the fall at terminal speed, the ground
	int live = 0;
	if (sdt > 0.0) {
		const double tau = 0.45, k = exp(-sdt / tau);
		for (int i = 0; i < SHED_MAX; i++) {
			ShedPt& sp = shed[i];
			if (!sp.live) continue;
			sp.age += (float)sdt;
			if (sp.age >= sp.life) { sp.live = false; continue; }
			const VECTOR3 dv = sp.v - windL;
			sp.p = sp.p + windL * sdt + dv * (tau * (1.0 - k)) - upL * (1.5 * sdt);
			sp.v = windL + dv * k;
			if (length(sp.p) < groundR + 0.03) { sp.live = false; continue; }
			live++;
		}
	} else {
		for (int i = 0; i < SHED_MAX; i++) if (shed[i].live) live++;
	}
	shedLive = live;
	g_fx.snowShedLive = live;
}

// ============================================================================
// PushSnow - wherever the sensing ran (clbkPreStep AND the keyboard tick). The cover
// to the client on change, through the rain's gate: 0 above the weather, on the wrong
// world, with no surface - the wet ground's exact discipline (28g).
// ============================================================================
void OroModule::PushSnow()
{
	if (!pCore || !pCore->CanSetSnowCover()) return;
	const bool  on    = g_fx.masterArmed && (g_fx.snowEnabled || g_fx.snowTest);
	const float gate  = clampf(rainGateLive, 0.0f, 1.0f);
	const float cover = on ? clampf(g_fx.snowCoverNow, 0.0f, 1.0f) * gate : 0.0f;
	const float line  = clampf(g_fx.snowLine,  -1000.0f, 8000.0f);
	const float lineW = clampf(g_fx.snowLineW,    10.0f, 3000.0f);
	if (fabsf(cover - s_pCover) >= 0.002f || fabsf(line - s_pLine) > 0.5f || fabsf(lineW - s_pLineW) > 0.5f) {
		s_pCover = cover; s_pLine = line; s_pLineW = lineW;
		pCore->SetSnowCover(cover, line, lineW);
	}

	// THE LOOK, AND THE RATE THE HULLS RE-COVER AT (round 3, 2026-09-19). The client keeps a
	// cover STATE per hull now (VVessel::Update) and lays snow back on a hull that shed it
	// only while snow FALLS - at exactly the rate the ground takes it here (UpdateSnow:
	// envelope x Snowfall capped at 1, over the Build-up minutes), through the same altitude
	// gate, so hull and apron whiten together by construction. Brightness is ONE slider the
	// client splits into two calibrated lanes (the terrain's chain 2.2x, the hulls' 1x - the
	// two read alike then; 1 = the calibrated look). Relief and Sparkle ride the two lanes
	// the setter reserved for them (step B). On change, as everything pushed from here;
	// guard #29 (probe by binding).
	if (pCore->CanSetSnowLook()) {
		const float buildS = clampf(g_fx.snowBuild, 1.0f, 120.0f) * 60.0f;
		const float fall   = on ? g_fx.snowI * clampf(g_fx.snowFall, 0.0f, 1.0f) * gate / buildS : 0.0f;
		const float bright = clampf(g_fx.snowBright,  0.0f, 3.0f);
		const float relief = clampf(g_fx.snowRelief,  0.0f, 2.0f);
		const float spark  = clampf(g_fx.snowSparkle, 0.0f, 2.0f);
		if (fabsf(fall - s_pFall) > 1.0e-5f || fabsf(bright - s_pBright) > 0.002f
		 || fabsf(relief - s_pRelief) > 0.002f || fabsf(spark - s_pSparkle) > 0.002f) {
			s_pFall = fall; s_pBright = bright; s_pRelief = relief; s_pSparkle = spark;
			pCore->SetSnowLook(fall, bright, relief, spark);
		}
	}

	// TIRE MARKS (step D, 2026-09-20): the client's TRACK MAP - its switch and its fade,
	// guard #30 (probe by binding). The pill follows the snow's own: a map nobody can see
	// is not stamped either. The fade is the SIM-minutes a fresh track takes to vanish with
	// nothing falling; the client adds the build-up rate it already receives while snow
	// falls. On change, as everything pushed from here.
	if (pCore->CanSetSnowTracks()) {
		const int   trk  = (on && g_fx.snowTracksOn) ? 1 : 0;
		const float fade = clampf(g_fx.snowTrackFade, 1.0f, 120.0f);
		if (trk != s_pTracks || fabsf(fade - s_pTrackFade) > 0.01f) {
			s_pTracks = trk; s_pTrackFade = fade;
			pCore->SetSnowTracks(trk != 0, fade);
		}
	}
}
