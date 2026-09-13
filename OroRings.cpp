// ==============================================================
// OroRings.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - PLANETARY RINGS (2026-09-12, client patch (aj); his effect, discussed
// 2026-09-11 - beta/reports/260911/RINGS_PLAN.md is the record).
// ----------------------------------------------------------------------------
// ROUND 1 DRAWS NOTHING HERE. Invariant 18's category: the client owns the ring - it
// always did - and this file hands it two things per ringed planet and takes them
// back on every exit path:
//   THE PROFILE  one N x 1 radial texture, LINEAR in radius, RGB brightness and
//                A = encoded optical depth, DERIVED by OroRingProfile.cpp from
//                whatever the planet ships. The legacy .tex alpha is a real optical
//                depth (put there by whoever made the planet), so ANY ringed planet -
//                stock or addon - gets the ORO ring with no per-planet file at all.
//                His rule: "we cannot rely on a custom way to do this for just these
//                two planets."
//   THE LOOK     blend (the pill: 0 = stock ARITHMETICALLY), a density trim, a lit
//                brightness, a backlit glow - and four reserved lanes.
// With those the client draws the sheet on the per-pixel radial path (opacity
// 1 - exp(-tau/mu), the lit/unlit inversion), the ring's SHADOW ON THE PLANET
// (absent from stock entirely), and shades every object inside that shadow through
// its own per-object sun. One pill, one A/B.
//
// SCOPE: the pill is GLOBAL (the pilot's, AuroraOn's rule); the three trims are PER
// BODY (Saturn and Uranus are nothing alike). Every ringed planet within range gets a
// push of its OWN derived profile and its OWN trims - live from the sliders for the
// world whose per-body file is loaded (OroSettings_Body(), kept current by SenseBody
// behind no effect's pill), and read from that planet's own file for every other one.
// So Saturn looks like Saturn from Titan, and leaving its SOI never drops it to stock.
//
// Invariant 1: everything here is MAIN THREAD - file I/O, oapi calls, the push.
// SenseRings runs every frame (pre-step AND the keyboard tick, the SenseView law), so
// the readout stays alive while paused; PushRings pushes on CHANGE.
// The profile is per SESSION: OBJHANDLEs die with the session, and the client copies
// the bytes the moment they are handed over - ORO keeps no device resource for it
// (nothing to return under 23l), only the derived arrays, freed at Reset.
//
// ROUND 2 (2026-09-12) ADDED THE CLOSE-UP - a camera-driven LOOK: the sheet's own
// texture grows grooves and grain, octave by octave, as the eye approaches. That lives in
// the client's ring shader; this file feeds it the amplitude and the grain's ANCHOR
// through the look's lanes 4..11, and still draws nothing. It lives at the bottom of this
// file under its own header; everything above it is still round 1. (A physical boulder
// swarm was built, flown twice and REVERTED; then a dust halo and bright specks were
// built, flown five times and CUT by him - RINGS_PLAN.md 11.8-11.9 and 12.8 record why.)
// ============================================================================

#include "OroModule.h"
#include "OroLog.h"
#include "OroState.h"
#include "OroRingProfile.h"
#include "gcCoreAPI.h"
#include <math.h>
#include <string.h>

namespace {

	// One slot per ringed planet seen this session. Small and fixed: a stock system has
	// two; an addon system with more than eight ringed planets is a curiosity we log.
	const int    RING_MAX      = 8;
	const double RING_RANGE_RP = 400.0;             // push while within 400 planet radii

	struct RingSlot {
		OBJHANDLE       hObj;
		char            name[32];
		double          Rkm, irad, orad;   // planet radius [km], ring radii [planet radii]
		OroRingProfile  prof;              // derived arrays (bright/tau/rgba), CPU only
		bool            profileSent;       // the client has this session's bytes
		float           trim[6];           // THIS planet's own numbers. [0..2] density /
		                                   //   bright / backlit; [3] detail (how fine), [4]
		                                   //   contrast, [5] relief (round 2). One array,
		                                   //   one read path.
		float           lastPrm[16];       // what was last pushed (-1 = never); [4..15] the close-up
		bool            lastOn;
	};
	RingSlot s_slot[RING_MAX];
	int      s_slotN     = 0;
	bool     s_scanned   = false;          // the body list is scanned ONCE per session
	int      s_target    = -1;             // the slot the readout speaks for
	bool     s_capLogged = false;
	char     s_texRoot[MAX_PATH] = "Textures";

	// Region names for the readout, by radius in km. Saturn's is real; every other world
	// gets the generic answer from the optical depth alone.
	struct Region { double r0, r1; const char* name; };
	const Region SATURN_REGIONS[] = {
		{ 66900,  74658, "D ring" }, { 74658, 87500, "C ring" }, { 87500, 87800, "Maxwell gap" },
		{ 87800,  91975, "C ring" }, { 91975, 117507, "B ring" }, { 117507, 117680, "Huygens gap" },
		{ 117680, 122050, "Cassini Division" }, { 122050, 133410, "A ring" },
		{ 133410, 133745, "Encke gap" }, { 133745, 136487, "A ring" }, { 136487, 136529, "Keeler gap" },
		{ 136529, 136774, "A ring" }, { 136774, 139500, "Roche Division" }, { 139500, 141000, "F ring" },
	};

	// Orbiter.cfg's TextureDir if present (the same key the client reads), else the stock
	// "Textures". The OroTree.cpp rule, verbatim - relative paths are fine, Orbiter runs
	// modules with the CWD at its root.
	void ResolveTexRoot()
	{
		strcpy_s(s_texRoot, "Textures");
		FILEHANDLE fh = oapiOpenFile("Orbiter.cfg", FILE_IN, ROOT);
		if (!fh) return;
		char buf[MAX_PATH];
		if (oapiReadItem_string(fh, (char*)"TextureDir", buf)) {
			char* p = buf;
			if (p[0] == '.' && (p[1] == '\\' || p[1] == '/')) p += 2;
			size_t n = strlen(p);
			while (n && (p[n-1] == '\\' || p[n-1] == '/')) p[--n] = 0;
			if (n) strcpy_s(s_texRoot, p);
		}
		oapiCloseFile(fh, FILE_IN);
	}

	// EACH RINGED PLANET'S OWN TRIMS, from its OWN Config\ORO\bodies\<name>.cfg (2026-09-12,
	// his call). Only ONE world's per-body block is live in g_fx at a time - the one you are
	// at - so before this, a ringed planet seen from elsewhere (Saturn from Titan) flew on
	// neutral 1.0s and looked untuned from every one of its own moons. Reading the file
	// directly gives every ring its own saved numbers wherever you are.
	// !! The same sanitiser the settings system uses (OroClassFileName), or we would read a
	// different filename than the one SAVE writes - a mismatch nothing would report.
	// A missing file or key leaves 1.0, which is the physics position, so an unconfigured
	// ringed planet renders from its derived profile alone. Clamped like the sliders.
	void ReadSlotTrims(RingSlot& s)
	{
		for (int k = 0; k < 6; k++) s.trim[k] = 1.0f;
		char fn[64], rel[160];
		OroClassFileName(s.name, fn, sizeof(fn));
		sprintf_s(rel, "ORO\\bodies\\%s.cfg", fn);
		FILEHANDLE f = oapiOpenFile(rel, FILE_IN, CONFIG);
		if (!f) return;
		double d;
		if (oapiReadItem_float(f, (char*)"RingDensity", d)) s.trim[0] = (float)d;
		if (oapiReadItem_float(f, (char*)"RingBright",  d)) s.trim[1] = (float)d;
		if (oapiReadItem_float(f, (char*)"RingBacklit", d)) s.trim[2] = (float)d;
		if (oapiReadItem_float(f, (char*)"RingDetail",   d)) s.trim[3] = (float)d;
		if (oapiReadItem_float(f, (char*)"RingContrast", d)) s.trim[4] = (float)d;
		if (oapiReadItem_float(f, (char*)"RingRelief",   d)) s.trim[5] = (float)d;
		oapiCloseFile(f, FILE_IN);
		for (int k = 0; k < 6; k++) {
			if (s.trim[k] < 0.0f) s.trim[k] = 0.0f;
			if (s.trim[k] > 2.0f) s.trim[k] = 2.0f;
		}
	}

	// Every planet with RingMinRadius + RingMaxRadius in its cfg - the core's own test for
	// bHasRings (Planet.cpp:305), read back through OBJPRM_PLANET_HASRINGS. Once per
	// session: the body list does not change and the parameters are constants.
	void ScanBodies()
	{
		s_scanned = true;
		s_slotN = 0;
		ResolveTexRoot();
		const DWORD n = oapiGetGbodyCount();
		for (DWORD i = 0; i < n && s_slotN < RING_MAX; i++) {
			OBJHANDLE h = oapiGetGbodyByIndex(i);
			if (!h || oapiGetObjectType(h) != OBJTP_PLANET) continue;
			const bool* has = (const bool*)oapiGetObjectParam(h, OBJPRM_PLANET_HASRINGS);
			if (!has || !*has) continue;
			const double* rmin = (const double*)oapiGetObjectParam(h, OBJPRM_PLANET_RINGMINRAD);
			const double* rmax = (const double*)oapiGetObjectParam(h, OBJPRM_PLANET_RINGMAXRAD);
			if (!rmin || !rmax || !(*rmax > *rmin)) continue;
			RingSlot& s = s_slot[s_slotN];
			memset(&s, 0, sizeof(s));
			s.hObj = h;
			oapiGetObjectName(h, s.name, sizeof(s.name));
			s.Rkm  = oapiGetSize(h) / 1000.0;
			s.irad = *rmin; s.orad = *rmax;
			for (int k = 0; k < 16; k++) s.lastPrm[k] = -1.0f;
			s.lastOn = false;
			ReadSlotTrims(s);
			s_slotN++;
		}
		if (n && s_slotN == RING_MAX) OroLog(1, "ORO rings: more than %d ringed planets - the rest keep stock rings.", RING_MAX);
	}

	// Derive the profile the first time a planet needs it. The device's texture-width
	// cap bounds the request: an 8192-texel profile on a 4096-cap card is resampled, the
	// same way the client itself only probes the _ring_<size>.dds its caps allow.
	bool EnsureProfile(RingSlot& s, gcCore2* core)
	{
		if (s.prof.n > 0) return true;
		int cap = 8192;
		if (core) {
			gcCore::SystemSpecs sp; memset(&sp, 0, sizeof(sp));
			core->GetSystemSpecs(&sp, sizeof(sp));
			if (sp.MaxTexSize >= 2048 && sp.MaxTexSize < 8192) cap = (int)sp.MaxTexSize;
		}
		// Source-driven N first (8192 with a hi-res profile, else 1024); shrink to the cap.
		if (!OroRingProfile_Derive(s_texRoot, s.name, s.Rkm, s.irad, s.orad, 0, true, &s.prof)) return false;
		if (s.prof.n > cap) {
			OroRingProfile_Free(&s.prof);
			if (!OroRingProfile_Derive(s_texRoot, s.name, s.Rkm, s.irad, s.orad, cap, true, &s.prof)) return false;
		}
		OroLog(1, "ORO rings: %s - %d-texel profile, brightness from %s, optical depth from %s%s.",
		              s.name, s.prof.n, s.prof.srcBright, s.prof.srcTau,
		              s.prof.fromOverride ? " (ORO override file)" : "");
		return true;
	}

	const char* RegionName(const RingSlot& s, double rkm, double tau)
	{
		if (_stricmp(s.name, "Saturn") == 0) {
			for (int i = 0; i < (int)(sizeof(SATURN_REGIONS) / sizeof(SATURN_REGIONS[0])); i++)
				if (rkm >= SATURN_REGIONS[i].r0 && rkm < SATURN_REGIONS[i].r1) return SATURN_REGIONS[i].name;
		}
		if (tau < 0.02) return "a gap";
		if (tau < 0.3)  return "thin ring";
		if (tau < 1.2)  return "ring";
		return "dense ring";
	}
}

// Forward declarations for round 2's half, which lives at the bottom of this file.
static void SenseNearField(int slotIdx, double rPlaneKm, double hKm);
static void NearFieldReset();
static bool NearFieldLanes(float* out12);     // the look's lanes 4..15: amplitudes + anchor

// Session start (23m): the profiles and the slot table are per session - a cached
// OBJHANDLE from the last one is the reload-CTD shape. Also the pill's push state.
void OroRings_Reset()
{
	NearFieldReset();
	for (int i = 0; i < s_slotN; i++) OroRingProfile_Free(&s_slot[i].prof);
	memset(s_slot, 0, sizeof(s_slot));
	s_slotN = 0; s_scanned = false; s_target = -1; s_capLogged = false;
	g_fx.ringBody[0] = 0; g_fx.ringRegion[0] = 0; g_fx.ringSrc[0] = 0; g_fx.ringTauLive = 0.0f;
}

// Every exit path (disarm, session end): every ringed planet back to stock, the
// client's copies dropped. Idempotent - it is called from two return-to-stock sites.
void OroRings_ReturnAll(gcCore2* core)
{
	if (!core || !core->CanSetRingLook()) return;
	for (int i = 0; i < s_slotN; i++) {
		RingSlot& s = s_slot[i];
		if (!s.hObj) continue;
		if (s.lastOn || s.profileSent) {
			const float off[4] = { 0.0f, 1.0f, 1.0f, 1.0f };
			core->SetRingLook(s.hObj, off, 4);
			core->SetRingProfile(s.hObj, NULL, 0);
		}
		s.lastOn = false; s.profileSent = false;
		for (int k = 0; k < 16; k++) s.lastPrm[k] = -1.0f;
	}
}

// ----------------------------------------------------------------------------
// SENSE, every frame: which ringed planet the readout speaks for, and what the ring
// is like under (or beside) the camera. Coarse range gate on the pre-step camera -
// it decides which world, not where anything lands on screen.
// ----------------------------------------------------------------------------
void OroModule::SenseRings()
{
	if (!s_scanned) ScanBodies();
	s_target = -1;
	// The near field resets FIRST and is re-armed only on the one path that reaches the
	// bottom of this function, so every early return below leaves it off. (The alternative
	// - arming it at each return - is the list-is-a-claim trap waiting to happen.)
	SenseNearField(-1, 0.0, 0.0);
	if (s_slotN == 0) { g_fx.ringBody[0] = 0; g_fx.ringRegion[0] = 0; return; }

	// !! oapiCameraGlobalPos, NOT the pre-step snapshot. This runs from the keyboard tick
	// as well so the readout stays true while paused - and preStepCam is refreshed only by
	// clbkPreStep, which pause does not run, so reading it here would have frozen exactly
	// the thing the every-frame call exists to keep alive.
	VECTOR3 camPos; oapiCameraGlobalPos(&camPos);

	// the nearest ringed planet, in planet radii
	double best = 1e30;
	for (int i = 0; i < s_slotN; i++) {
		VECTOR3 O; oapiGetGlobalPos(s_slot[i].hObj, &O);
		const double d = length(O - camPos) / (s_slot[i].Rkm * 1000.0);
		if (d < best) { best = d; s_target = i; }
	}
	if (s_target < 0 || best > RING_RANGE_RP) { s_target = -1; g_fx.ringBody[0] = 0; g_fx.ringRegion[0] = 0; return; }

	RingSlot& s = s_slot[s_target];
	strcpy_s(g_fx.ringBody, s.name);
	if (!EnsureProfile(s, pCore)) { strcpy_s(g_fx.ringRegion, "no ring texture"); g_fx.ringTauLive = 0.0f; return; }
	sprintf_s(g_fx.ringSrc, "%s + %s", s.prof.srcBright, s.prof.srcTau);

	// The camera's radius in the ring plane, and its height above it, in the planet's
	// own frame (grot's Y is the spin axis = the ring normal).
	VECTOR3 O; oapiGetGlobalPos(s.hObj, &O);
	MATRIX3 R; oapiGetRotationMatrix(s.hObj, &R);
	const VECTOR3 rel = tmul(R, camPos - O);                    // planet-local
	const double  rPlane = sqrt(rel.x * rel.x + rel.z * rel.z) / 1000.0;   // km
	const double  h      = fabs(rel.y) / 1000.0;                            // km above the plane
	const double  ir = s.irad * s.Rkm, orr = s.orad * s.Rkm;

	if (rPlane < ir || rPlane > orr) {
		sprintf_s(g_fx.ringRegion, rPlane < ir ? "inside the rings' inner edge" : "outside the rings");
		g_fx.ringTauLive = 0.0f;
		return;
	}
	const int i = OroRingProfile_Texel(&s.prof, rPlane);
	// THIS planet's density, not the loaded world's - the readout must describe the ring it
	// names. Computed here rather than read from s.trim so it is order-independent: PushRings
	// copies the live sliders into the slot, and it runs AFTER this.
	const double dens = (_stricmp(s.name, OroSettings_Body()) == 0) ? (double)g_fx.ringDensity
	                                                                : (double)s.trim[0];
	const double tau = s.prof.tau[i] * dens;
	g_fx.ringTauLive = (float)tau;
	const char* reg = RegionName(s, rPlane, tau);
	if (h > 50.0) sprintf_s(g_fx.ringRegion, "over the %s", reg);
	else          sprintf_s(g_fx.ringRegion, "in the %s", reg);

	// ROUND 2: the close-up's own sense pass - the camera in ring coordinates, the grain's
	// anchor, the Above-plane readout. Main thread (invariant 1); nothing is drawn from it.
	SenseNearField(s_target, rPlane, h);
}

// ----------------------------------------------------------------------------
// PUSH, on change: every ringed planet in range gets a look - the tuned trims when it
// is the world whose per-body file is loaded, neutral trims otherwise - and its
// profile once per session. Off (pill, Ctrl+G) = stock for all of them.
// ----------------------------------------------------------------------------
void OroModule::PushRings()
{
	if (!pCore) return;
	if (!pCore->CanSetRingLook()) {
		if (!s_capLogged) { s_capLogged = true; }
		return;
	}
	const bool on = g_fx.masterArmed && g_fx.ringsEnabled;
	if (!on) { OroRings_ReturnAll(pCore); return; }
	if (!s_scanned) ScanBodies();
	VECTOR3 camPos; oapiCameraGlobalPos(&camPos);       // live, not the pre-step snapshot - see SenseRings

	for (int i = 0; i < s_slotN; i++) {
		RingSlot& s = s_slot[i];
		if (!s.hObj) continue;
		// in range? (the profile is derived lazily, so a never-visited planet costs nothing)
		VECTOR3 O; oapiGetGlobalPos(s.hObj, &O);
		const bool inRange = length(O - camPos) / (s.Rkm * 1000.0) <= RING_RANGE_RP;
		if (!inRange) {
			if (s.lastOn) { const float off[4] = { 0, 1, 1, 1 }; pCore->SetRingLook(s.hObj, off, 4); s.lastOn = false; s.lastPrm[0] = -1.0f; }
			continue;
		}
		if (!EnsureProfile(s, pCore)) continue;
		if (!s.profileSent) {
			pCore->SetRingProfile(s.hObj, s.prof.rgba, s.prof.n);
			s.profileSent = true;
		}
		// EVERY RINGED PLANET FLIES ON ITS OWN TRIMS (2026-09-12, his call). The one whose
		// per-body file is LOADED tracks the live sliders, so an edit is visible the instant
		// it is made; every other one uses the values read from its own file at scan time.
		// The live copy-back is also what keeps a SAVE consistent with no cache to
		// invalidate: the numbers that get written are the ones already in the slot, and a
		// REVERT reloads g_fx and the slot follows on the next frame.
		// !! "Which world is loaded" is asked of the SETTINGS system (OroSettings_Body()),
		// never of an effect. It used to ask g_fx.auroraBody - the AURORA'S READOUT, which
		// is cleared whenever the aurora is off - so the aurora's pill dropped the ring
		// trims to neutral. Two effects that share a settings file must not share a gate.
		if (_stricmp(s.name, OroSettings_Body()) == 0) {
			s.trim[0] = g_fx.ringDensity; s.trim[1] = g_fx.ringBright; s.trim[2] = g_fx.ringBacklit;
			s.trim[3] = g_fx.ringDetail;  s.trim[4] = g_fx.ringContrast;  s.trim[5] = g_fx.ringRelief;
		}
		float prm[16];
		prm[0] = 1.0f;
		prm[1] = s.trim[0];
		prm[2] = s.trim[1];
		prm[3] = s.trim[2];
		for (int k = 1; k < 4; k++) { if (prm[k] < 0.0f) prm[k] = 0.0f; if (prm[k] > 2.0f) prm[k] = 2.0f; }
		// ROUND 2: lanes 4..15 - the close-up's amplitudes and the grain's anchor. The
		// anchor is a per-frame quantity (the camera's place in a pattern that rotates at the
		// orbital rate), so while a ringed planet is in range this pushes EVERY frame: sixteen
		// floats into a map, which is nothing. Only the planet the readout speaks for carries
		// a live anchor; any other ringed planet in range gets zeros = no detail, which is
		// also what an older client reads.
		if (i == s_target) NearFieldLanes(prm + 4);
		else               for (int k = 4; k < 16; k++) prm[k] = 0.0f;
		bool moved = !s.lastOn;
		for (int k = 0; k < 16 && !moved; k++) if (fabsf(prm[k] - s.lastPrm[k]) > 0.002f) moved = true;
		if (!moved) continue;
		pCore->SetRingLook(s.hObj, prm, 16);
		memcpy(s.lastPrm, prm, sizeof(prm));
		s.lastOn = true;
	}
}

// ============================================================================
// ROUND 2 (2026-09-12) - THE CLOSE-UP: the sheet's own texture. His rules, after the
// swarm was reverted: "the effect lives in the rings and is related to the position of the
// CAMERA, not the vessel. Rings from far: identical to what we have now. Closer: more
// texture." And after flight 6, HIS CUT: "lose the particles and the halo and just keep
// the grooved texture, but we must make it as good resolution as we can."
// ----------------------------------------------------------------------------
// Everything above this line hands the CLIENT a look and draws nothing (invariant 18), and
// so does this half. The close-up is grooves and grain in the SHEET'S OWN TEXTURE, faded
// in octave by octave with each pixel's camera distance, and it lives in the client's ring
// shader (RingTechOROPS, Mesh.fx) because that is what the reference frames are: the sheet
// gaining texture. This half computes the two things that shader cannot: the amplitude
// (the slider) and the ANCHOR of the grain - a pattern that rotates rigidly at the orbital
// rate of the camera's own radius in ORBITER'S OWN FIELD, its phase INTEGRATED on the CPU
// (never a rate times an epoch - flight 3's lesson) so no 1e5 rad angle ever reaches a
// float32 shader. Pushed through the look's lanes 4..11.
//
// !! WHAT THE TWO CUTS TAUGHT. THE SWARM (built, flown twice, reverted - RINGS_PLAN.md
// 11.8-11.9): anything expected to co-orbit with the ship must use Orbiter's gravity, not
// Kepler's, and a plain e = 0 ELEMENTS line is not even a circular orbit under J2. THE HALO
// AND THE SPECKS (built, flown five times, cut by him - 12.8): backlit, opaque specks read
// as holes in the sheet, and a medium that thickens as the eye nears the plane was only
// ever the swarm's atmosphere kept alive without the swarm. The grooves need no anchor at
// all, being axisymmetric; the grain's anchor is the ONE place orbital motion enters, and
// it is the only thing left here that is not a comment.
// ============================================================================

namespace {

	const double RING_TWOPI = 6.283185307179586;
	const double RING_PI    = 3.14159265358979;

	// ---- the detail's constants -------------------------------------------
	// The grain lattice: a base cell of ~2 km, sub-octaves down to 4 m in the shader, and a
	// hash period of 4096 base cells. THE CELL COUNT ROUND THE RING IS A MULTIPLE OF THE
	// HASH PERIOD, so when the anchor phase wraps by 2 pi the pattern offset moves by a whole
	// number of periods and nothing on screen changes - the integer-cycle lattice law.
	// !! THE OFFSETS TRAVEL AS INTEGER + FRACTION (flight 6, "as good resolution as we can"):
	// a fine octave multiplies the pattern coordinate by up to 512, and 4096 x 512 leaves a
	// float32 a quarter of a cell. Split, the integer product stays exact below 2^24 and the
	// fraction keeps all its bits, so a 4 m cell is placed to millimetres.
	const double DET_L0 = 2048.0;           // nominal cell, m (along track AND radial)
	const double DET_NP = 4096.0;           // the shader's RING_NP, verbatim

	// ---- what the sheet's detail needs, per frame - the look's lanes 4..11 ---
	// [0] contrast (amplitude)  [1] psi0 integer  [2] rho0 integer  [3] psi0 fraction
	// [4] cells/m along track  [5] cells/m radial  [6] rho0 fraction  [7] detail (fade reach)
	// [8] relief (the density field read as height)  [9..11] reserved (0)
	struct DetailAnchor {
		bool   valid;
		float  lane[12];
	};
	DetailAnchor s_anchor;

	// ---- the orbital rate in ORBITER'S field --------------------------------
	// Psys.cpp's SingleGacc_perturbation at latitude 0, mirrored exactly: the 1e-10 cutoff,
	// the Size() reference radius, and its nesting (J4 only if the J3 term passes). Saturn's
	// J2 alone moves the mean motion at the Cassini Division by 0.29% - 52 m/s along track -
	// which is the "stream of boulders" of the swarm's second flight. KNOWN LIMIT: bodies
	// that ship a spherical-harmonics gravity file (Earth, Moon, Mars, Mercury, Venus, Vesta
	// - none ringed) take Orbiter's other branch and are not mirrored.
	struct Field { double GM, Rpl, J2, J3, J4; bool perturb; };
	inline double FieldOmega(const Field& F, double a)
	{
		double q = 0.0;
		if (F.perturb) {
			const double Rr = F.Rpl / a, Rr2 = Rr * Rr;
			const double j2t = F.J2 * Rr2;
			if (fabs(j2t) > 1e-10) {
				q = 1.5 * j2t;
				const double j3t = F.J3 * Rr2 * Rr;
				if (fabs(j3t) > 1e-10) {
					const double j4t = F.J4 * Rr2 * Rr2;
					if (fabs(j4t) > 1e-10) q -= 1.875 * j4t;
				}
			}
		}
		return sqrt(F.GM / (a * a * a) * (1.0 + q));
	}
}

// Session start (23m): the anchor's spin-sense measurement and its integrated phase start
// over.
static double s_spinPrev  = 0.0;
static bool   s_spinHave  = false;
static double s_spinSense = -1.0;   // measured below; the analytic value for Orbiter's frame
static double s_anchorPhi = 0.0;    // the grain lattice's phase, INTEGRATED at the camera's own
static double s_anchorT   = -1.0;   //   orbital rate (flight 3's finding, see the anchor); -1 = fresh
static void NearFieldReset()
{
	memset(&s_anchor, 0, sizeof(s_anchor));
	s_spinHave = false;
	s_anchorPhi = 0.0; s_anchorT = -1.0;
}

// ----------------------------------------------------------------------------
// The main-thread half: the camera in ring coordinates, the grain's anchor, the readout.
// Called at the end of SenseRings, so it runs EVERY frame incl. paused (invariant 1's
// law) - slotIdx -1 when no ringed planet is the target.
// ----------------------------------------------------------------------------
static void SenseNearField(int slotIdx, double rPlaneKm, double hKm)
{
	s_anchor.valid = false;
	g_fx.ringPlaneAlt = 0.0f;
	if (slotIdx < 0) return;
	// THE PILL IS THE A/B here as much as in round 1: off (or Ctrl+G) means the client's
	// own ring shader and nothing else, so the close-up goes with it.
	if (!g_fx.masterArmed || !g_fx.ringsEnabled) return;

	RingSlot& s = s_slot[slotIdx];
	if (s.prof.n < 2) return;
	(void)rPlaneKm;

	// THIS planet's own knobs - the live sliders when it is the world whose per-body file is
	// loaded, its own saved numbers otherwise; the trims' rule, for the trims' reason.
	const bool   live     = (_stricmp(s.name, OroSettings_Body()) == 0);
	const double detail   = live ? (double)g_fx.ringDetail   : s.trim[3];   // how fine (the fade's reach)
	const double contrast = live ? (double)g_fx.ringContrast : s.trim[4];   // how strong (the amplitude)
	const double relief   = live ? (double)g_fx.ringRelief   : s.trim[5];   // how deep (the density read as height)
	g_fx.ringPlaneAlt = (float)(hKm * 1000.0);
	const double ir = s.irad * s.Rkm * 1000.0;

	// the planet and the ring plane, at the sensed epoch
	VECTOR3 O; oapiGetGlobalPos(s.hObj, &O);
	MATRIX3 R; oapiGetRotationMatrix(s.hObj, &R);
	VECTOR3 cam; oapiCameraGlobalPos(&cam);
	const VECTOR3 rel = cam - O;

	// !! THE IN-PLANE BASIS IS BUILT FROM THE SPIN AXIS ALONE, never from the rotation
	// matrix's own X and Z rows: those carry the planet's SPIN PHASE, and ring material
	// orbits, it does not co-rotate. The axis is inertial (to precession), so this frame is.
	const VECTOR3 axN0 = mul(R, _V(0, 1, 0));
	const double  ln = length(axN0);
	if (ln < 1e-9) return;
	const VECTOR3 axN = axN0 / ln;
	VECTOR3 axU = crossp(axN, _V(0, 0, 1));
	double  lu = length(axU);
	if (lu < 1e-6) { axU = crossp(axN, _V(1, 0, 0)); lu = length(axU); if (lu < 1e-6) return; }
	axU = axU / lu;
	const VECTOR3 axV = crossp(axN, axU);

	const double cu = dotp(rel, axU), cv = dotp(rel, axV);
	const double cr = sqrt(cu * cu + cv * cv);
	if (cr < 1.0) return;
	const double cth = atan2(cv, cu);

	// ========================================================================
	// THE GRAIN'S ANCHOR. The shader's along-track coordinate is (fragment - eye) . axTan,
	// so what it needs from here is where the CAMERA sits in a pattern that rotates rigidly
	// at the orbital rate of its own radius: psi0 = M (phi_cam - s Phi) / 2pi, reduced mod
	// the hash period, with Phi the pattern's phase (integrated at Omega(r_cam) - see the
	// !! below), M the number of cells round the ring (a multiple of the period) and s the
	// SPIN SENSE in this basis.
	// !! THE SENSE IS MEASURED, NOT DERIVED - invariant 22(b)'s rule: the azimuth of the
	// planet's own local X axis is watched from frame to frame, and ring material orbits in
	// the direction the planet spins. Derived from the matrix conventions it came out -1;
	// a sign error here is a grain field counter-rotating at twice orbital speed, which is
	// exactly the failure a flight cannot tell from a bug in the shader.
	{
		const VECTOR3 Xw = mul(R, _V(1, 0, 0));
		const double  th = atan2(dotp(Xw, axV), dotp(Xw, axU));
		if (s_spinHave) {
			double d = th - s_spinPrev;
			while (d >  RING_PI) d -= RING_TWOPI;
			while (d < -RING_PI) d += RING_TWOPI;
			if (fabs(d) > 1e-12) s_spinSense = (d > 0.0) ? 1.0 : -1.0;   // paused: keep the last
		}
		s_spinPrev = th; s_spinHave = true;

		Field F;
		F.GM  = GGRAV * oapiGetMass(s.hObj);
		F.Rpl = oapiGetSize(s.hObj);
		const DWORD nJ = oapiGetPlanetJCoeffCount(s.hObj);
		F.J2 = (nJ > 0) ? oapiGetPlanetJCoeff(s.hObj, 0) : 0.0;
		F.J3 = (nJ > 1) ? oapiGetPlanetJCoeff(s.hObj, 1) : 0.0;
		F.J4 = (nJ > 2) ? oapiGetPlanetJCoeff(s.hObj, 2) : 0.0;
		// the Launchpad flag has no oapi getter, but every vessel answers for it (one static);
		// the focus vessel is asked and the answer kept when there is none
		static bool s_gPerturb = false;
		VESSEL* vf = oapiGetFocusInterface();
		if (vf) s_gPerturb = vf->NonsphericalGravityEnabled();
		F.perturb = s_gPerturb;

		const double om = FieldOmega(F, cr);
		// !! THE PHASE IS INTEGRATED, NOT EVALUATED - FLIGHT 3 (2026-09-12) IS WHY. The first
		// build took Phi = om * t with t = J2000 seconds (~5.5e8 s), "so the pattern is a
		// property of the ring, not of the scenario's clock". But om is the rate at the
		// CAMERA'S radius, re-evaluated every frame, so dPhi/dcr = t * dom/dcr = 1.5 om t / cr
		// = 1.1e-3 rad per METRE of camera radius - SIXTY-THREE CELLS PER METRE. An external
		// camera swinging round the ship moves tens of metres in radius, and the instrument
		// read the whole pattern streaming at +395 cells/s under a held rotate key and
		// +3323/s under a zoom, while the along-track term sat at 0.02 cells/s throughout
		// (s -1, cam/om -1.000, J2 on: every other reading was as predicted). The offline
		// check had held the camera radius constant - the one thing an external camera never
		// does. So the lattice's phase ADVANCES at the rate of whatever radius the eye is at
		// right now, and nothing ever multiplies a rate by an epoch: the pattern co-rotates
		// with the material beside the eye, which is the whole of what the fake promises.
		// The rain runners' host-integrated phase (2026-09-06), the same law: a phase whose
		// RATE varies is integrated, never rebuilt from clock x current rate.
		const double tSim = oapiGetSimTime();
		if (s_anchorT >= 0.0 && tSim > s_anchorT)
			s_anchorPhi = fmod(s_anchorPhi + om * (tSim - s_anchorT), RING_TWOPI);
		s_anchorT = tSim;
		const double Phi = s_anchorPhi;
		double dl = cth - s_spinSense * Phi;
		dl = fmod(dl, RING_TWOPI); if (dl < 0.0) dl += RING_TWOPI;
		const double Mk  = floor(RING_TWOPI * cr / (DET_L0 * DET_NP) + 0.5);     // periods round the ring
		const double M   = (Mk < 1.0 ? 1.0 : Mk) * DET_NP;                         // cells - a multiple of NP
		double psi0 = fmod(M * dl / RING_TWOPI, DET_NP); if (psi0 < 0.0) psi0 += DET_NP;
		double rho0 = fmod((cr - ir) / DET_L0, DET_NP);  if (rho0 < 0.0) rho0 += DET_NP;
		const double Lt  = RING_TWOPI * cr / M;                                    // the actual cell, ~L0

		const double psiI = floor(psi0), rhoI = floor(rho0);
		s_anchor.valid   = true;
		s_anchor.lane[0] = (float)contrast;
		s_anchor.lane[1] = (float)psiI;
		s_anchor.lane[2] = (float)rhoI;
		s_anchor.lane[3] = (float)(psi0 - psiI);
		s_anchor.lane[4] = (float)(1.0 / Lt);
		s_anchor.lane[5] = (float)(1.0 / DET_L0);
		s_anchor.lane[6] = (float)(rho0 - rhoI);
		s_anchor.lane[7] = (float)detail;
		s_anchor.lane[8] = (float)relief;
		s_anchor.lane[9] = s_anchor.lane[10] = s_anchor.lane[11] = 0.0f;
	}
}

// The look's lanes 4..11 for PushRings - the amplitude and the grain's anchor. False = push
// zeros (no detail), which is also what an older client ignores.
static bool NearFieldLanes(float* out12)
{
	if (!s_anchor.valid) { for (int i = 0; i < 12; i++) out12[i] = 0.0f; return false; }
	for (int i = 0; i < 12; i++) out12[i] = s_anchor.lane[i];
	return true;
}
