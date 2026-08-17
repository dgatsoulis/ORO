// ==============================================================
// OroPlume.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - PLUME EXPANSION: pressure-dependent exhaust (2026-08-09)
// ----------------------------------------------------------------------------
// A rocket nozzle is expanded for ONE ambient pressure; the atmosphere decides
// what the jet does about everywhere else. At sea level the jet is OVEREXPANDED:
// ambient pressure pinches it narrow and the repeating compression/expansion
// cells put a train of bright Mach-disc DIAMONDS down the core - first one
// brightest, spacing on the order of a nozzle width, fading downstream as
// turbulent mixing destroys the cell structure. High up the jet is UNDEREXPANDED
// and relaxes into the wide faint EXPANSION BLOOM of high-altitude launch
// footage, nearly invisible in true vacuum (no entrained air to afterburn in).
// Stock Orbiter draws one fixed billboard at every altitude; this is the fix.
//
// ARCHITECTURE (the roadmap's assessed plan, 2026-08-02): our own ADDITIVE
// OVERLAY riding the stock exhaust's own spec. Stock plumes are NEVER touched -
// GetExhaustSpec copies out, and a per-frame Del/AddExhaust cycle churns indices
// that vessels' own code holds (retractable engines, damage models), which
// invariant 14's borrow-and-return cannot survive. The overlay simply draws ON
// TOP: at sea level the diamond train inside the stock flame, in vacuum the wide
// halo the stock billboard never had (stock stays as the bright core).
//
// THE REGIME IS AUTOMATIC - static pressure in, shape out, no knob to ride
// during an ascent (the standing preference: never ask the user to adjust what
// the sim already knows). The sliders shape the LOOK (diamond brightness/
// spacing, bloom width/brightness, two colour picks), all per vessel class.
//
// DRAW SUBSTRATE: connected screen-space ribbon layers per plume - a feathered
// SHEATH + a cell-modulated CORE + the DIAMOND LOZENGES (triangular-width strips
// with tips at each Mach disc, the "diamond-y" rework) - along the projected
// axis. Connected geometry,
// never disconnected sprites (G13); per-station sizing at per-station depth
// (pxAt - G8); the plume axis is a straight WORLD line so its projection is a
// legitimately straight screen line (constant perpendicular is NOT G7's shared-
// direction bug - that was about differing world directions sharing one screen
// direction; the degenerate end-on case fades out instead). Drawn in the
// patch-(i) pre-resolve slot so the diamond cores accumulate in fp16 and BLOOM
// to white - the Firefly law: white is never in the palette, it emerges. Per-
// vertex depth feeds the patch-(g) clip, which cuts the jet at the hull AND at
// the runway surface under a hover plume (scene depth includes terrain); when
// the clip is dark the shimmer's geometric facing fade steps back in.
//
// MAIN THREAD ONLY (clbkPreStep -> UpdatePlumeFx, invariant 1): all oapi
// queries and the projection happen here. The render callback only pushes
// plmVtx into the poly and draws it (DrawPlumePoly - OroModule.cpp).
// Nothing accumulates across frames (G10): the whole list is rebuilt each step.
// ============================================================================

#include "OroModule.h"
#include "OroState.h"
#include "gcCoreAPI.h"       // gcCore::SuppressExhaust (client patch n)
#include <math.h>
#include <string.h>

namespace {

	// --- the model's constants ---------------------------------------------
	// Regime blend, on log10(static pressure [Pa]) - FRAMED BY THE EXPANSION BAND
	// double slider since 2026-08-09 (g_fx.plumeExpHi/Lo, two log-Pa handles, per
	// class; the HIGH handle doubles as the OD reference). The ramps sit inside
	// the handle window as FIXED FRACTIONS of it - the original hard-coded bands'
	// proportions (diamond ramp 1.3 of 4.0 decades, bloom ramp 2.0) - so the two
	// weights stay DISJOINT wherever the handles sit, and the default handles
	// (101.325 kPa / 10 Pa) reproduce the original constants exactly.
	const double PLM_EXP_LPMIN = 0.0;    // handle track floor:   1 Pa
	const double PLM_EXP_LPMAX = 5.5;    // handle track ceiling: ~316 kPa
	const double PLM_FR_DIA    = 0.325;  // diamond ramp, fraction of the window (top)
	const double PLM_FR_BLO    = 0.50;   // bloom ramp, fraction of the window (bottom)

	const int    PLM_NCELL_DEF = 7;      // default shock-cell count (the Diamonds
	                                     //   slider owns it now, 1..12)
	const double PLM_NODE0     = 0.75;   // first Mach disc, in cell-spacings from the exit
	const double PLM_CELL_DECAY= 0.80;   // per-cell brightness/modulation decay
	const double PLM_SPACE_W   = 2.0;    // cell spacing = this x wsize x the Spacing slider

	// ---- THE SYNTHESISED NOZZLE (2026-08-17) -------------------------------
	// A thruster with NO exhaust definition still deserves a jet. The DG-S's two
	// scramjets are the case that found this: ScramSubsys.cpp:160 creates them with
	// CreateThruster and never calls AddExhaust, so they are thrusters but not
	// exhausts - and the plume scan walks the EXHAUST list (it needs lsize/wsize,
	// which a THRUSTER_HANDLE simply does not carry). Particles and the bell glow
	// walk GetThrusterCount and saw them all along; only the jet was blind, which is
	// why the user saw scram particles with no exhaust behind them.
	//
	// ⚠️ THE SIZE MUST NOT COME FROM THRUST, and this hull is precisely why.
	// ScramSubsys::clbkPostStep calls SetThrusterMax0(hScram[i], Fscram[i]/(level+eps))
	// EVERY STEP, so a scramjet's "max thrust" is rewritten continuously as a function
	// of air conditions and collapses toward zero as the throttle closes. Sizing a
	// nozzle from it would make the nozzle BREATHE with flight condition and vanish at
	// ignition. A nozzle is a piece of metal: its size is a fact about the airframe.
	// GetSize() is the only stable geometric quantity Orbiter offers here, so the
	// synthesis is a fraction of the hull radius - frame-invariant by construction.
	//
	// Calibrated against the DG's OWN specs (SetSize(10), so wsize/hull and lsize/wsize
	// are directly readable): main 12/1.0 = 0.100 hull, hover 6/0.5 = 0.050,
	// retro 3/0.4 = 0.040, and lsize/wsize runs 7.5..12. The synthesised engine lands
	// between hover and retro in girth with a mid-range slenderness, which is the right
	// scale for an auxiliary/airbreathing duct and is never the biggest thing on a hull.
	// The per-group Width and Length knobs then correct it per class, which is exactly
	// where a guess belongs - the user's own escape hatch, no new slider.
	const double PLM_SYN_W_HULL = 0.045;  // synthesised wsize, as a fraction of GetSize()
	const double PLM_SYN_LW     = 10.0;   // synthesised lsize / wsize (mid of the DG's range)
	const double PLM_SYN_W_MIN  = 0.05;   // absolute girth clamps [m]: a probe stays visible,
	const double PLM_SYN_W_MAX  = 2.50;   //   a station does not grow a jet the size of a house
	const double PLM_SYN_ACC_MIN= 0.005;  // admission test [m/s^2]: is this a PROPULSIVE engine
	                                      //   at all, or a cosmetic vent someone modelled as a
	                                      //   thruster? Framed as acceleration so it needs no
	                                      //   magic Newton constant and works on any mass. The
	                                      //   DG's own hatch vent is a particle stream and not a
	                                      //   thruster, so nothing stock is near this line.

	const double PLM_W0_FRAC   = 0.55;   // core radius at the exit, as a fraction of wsize
	const double PLM_SHEATH_X  = 1.7;    // sheath radius = this x the exit core radius
	const double PLM_SPREAD_LO = 2.0;    // natural divergence half-angle [deg] (always)
	const double PLM_SPREAD_HI = 26.0;   // added at full vacuum bloom [deg], x the Width slider

	const float  PLM_A_CORE    = 120.0f; // base core alpha (before every multiplier)
	const float  PLM_A_DIA     = 205.0f; // diamond peak alpha, x the Diamond slider
	const float  PLM_A_SHEATH  =  26.0f; // sheath alpha at sea level
	const float  PLM_A_BLOOM   =  46.0f; // sheath alpha added at full vacuum, x Bloom bright

	const int    PLM_NS        = 96;     // stations along the axis (~7 per cell even at
	                                     //   the 12-cell slider max - Gouraud only
	                                     //   interpolates what the geometry samples,
	                                     //   the plasma's lesson)

	inline float  clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
	inline double clampd(double x, double a, double b) { return x < a ? a : (x > b ? b : x); }
	inline float  sstepf(float t) { t = clampf(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
	inline float  hashf(float s) { float t = sinf(s * 12.9898f) * 43758.547f; return t - floorf(t); }

	// Projection: the reentry/shimmer/aurora projector, verbatim (the established
	// per-file copy - each effect file carries its own in its anonymous namespace).
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
		r = (int)( c        & 0xFF);
		g = (int)((c >>  8) & 0xFF);
		b = (int)((c >> 16) & 0xFF);
	}
}

// ----------------------------------------------------------------------------
// BUILD THE PLUME MODEL - once per step, BEFORE every consumer. The regime, the
// strongest-6 selection and the FOUR PHYSICS CURVES all live here, so the jet
// geometry, the shimmer's heat haze and (planned) the particle system can never
// disagree about what the exhaust is doing. The LAB | PHYSICS switch acts HERE
// and only here: in LAB the four factors pin to their reference values and the
// sliders rule alone; in PHYSICS they follow pressure and throttle and the
// sliders TRIM the result. Both modes are ANCHORED identical at (sea level,
// full throttle), which is why PHYSICS can default on without moving any look
// tuned on the test stand.
//
// THE PHYSICS, in one paragraph. Nozzle exit pressure scales with chamber
// pressure, i.e. with throttle (pc = 0.25 + 0.75*level, a deep-throttle floor).
// The OVEREXPANSION DEGREE relative to the sea-level-full-throttle reference is
// OD = (Pa / 101.325 kPa) / pc: climbing lowers it, throttling down raises it.
// From OD, each factor anchored to 1 at OD = 1:
//   cell spacing x 1/sqrt(OD) - shock cells STRETCH as you climb and TIGHTEN as
//     you throttle down (the classic L ~ D*sqrt(NPR) scaling);
//   diamond contrast x sqrt(OD) - deeper overexpansion = stronger discs;
//   core width x (1 - 0.13*(OD-1)) - the ambient pinch;
//   separation flicker past OD ~1.6 - deeply overexpanded nozzles separate and
//     the plume visibly clings and pulses (low throttle at low altitude).
// WASHOUT is regime-driven, not OD-driven: the per-cell decay steepens as the
// diamond band fades with altitude, so the train loses discs one by one on the
// way up - the Diamonds slider sets the SEA-LEVEL maximum.
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// WHICH GROUP A THRUSTER IS IN - the one definition, shared by all four thruster
// subsystems (declared in OroModule.h). Lives here because the plume is the busiest
// caller; the bell, the shimmer and the particles all route through it so the four
// can never disagree about what "HOVER" means.
//
// ⚠️ "USER" IS DEFINED BY EXCLUSION: a thruster in NO standard group. That is the
// definition the bell glow has used since 2026-08-09, adopted rather than reinvented.
// It is also what keeps RCS out of ORO entirely without a special case - every
// THGROUP_ATT_* thruster IS in a standard group, so it classifies as "not ours" and
// every caller skips it.
// ----------------------------------------------------------------------------
namespace {
	// Every group Orbiter names. The first three are ours; the twelve attitude groups
	// exist here only so a thruster in one of them is recognised as NOT user-defined.
	const THGROUP_TYPE THR_STD[] = {
		THGROUP_MAIN, THGROUP_RETRO, THGROUP_HOVER,
		THGROUP_ATT_PITCHUP, THGROUP_ATT_PITCHDOWN, THGROUP_ATT_YAWLEFT,
		THGROUP_ATT_YAWRIGHT, THGROUP_ATT_BANKLEFT, THGROUP_ATT_BANKRIGHT,
		THGROUP_ATT_RIGHT, THGROUP_ATT_LEFT, THGROUP_ATT_UP, THGROUP_ATT_DOWN,
		THGROUP_ATT_FORWARD, THGROUP_ATT_BACK
	};
	const int N_THR_STD = (int)(sizeof(THR_STD) / sizeof(THR_STD[0]));
}

int OroThrusterGroupOf(VESSEL* v, THRUSTER_HANDLE th)
{
	if (!v || !th) return -1;
	for (int g = 0; g < N_THR_STD; g++) {
		const DWORD n = v->GetGroupThrusterCount(THR_STD[g]);
		for (DWORD i = 0; i < n; i++) {
			if (v->GetGroupThruster(THR_STD[g], i) != th) continue;
			if (THR_STD[g] == THGROUP_MAIN)  return ORO_THR_MAIN;
			if (THR_STD[g] == THGROUP_HOVER) return ORO_THR_HOVER;
			if (THR_STD[g] == THGROUP_RETRO) return ORO_THR_RETRO;
			return -1;                       // an attitude group: RCS, never ours
		}
	}
	return ORO_THR_USER;                     // in no standard group at all
}

bool OroThrusterHasUser(VESSEL* v)
{
	if (!v) return false;
	const DWORD n = v->GetThrusterCount();
	for (DWORD i = 0; i < n; i++)
		if (OroThrusterGroupOf(v, v->GetThrusterHandleByIndex(i)) == ORO_THR_USER) return true;
	return false;
}

void OroModule::BuildPlumeModel()
{
	plmModelN = 0;
	plmRho    = 0.0;

	// The vessel we're LOOKING at (the shimmer's resolution rule).
	OBJHANDLE hObj = oapiCameraTarget();
	if (!hObj || oapiGetObjectType(hObj) != OBJTP_VESSEL) hObj = oapiGetFocusObject();
	if (!hObj || oapiGetObjectType(hObj) != OBJTP_VESSEL) return;
	VESSEL* v = oapiGetVesselInterface(hObj);
	if (!v) return;

	// THE REGIME, framed by the EXPANSION BAND handles. The window between them
	// carries the ramps as fixed fractions (top = diamonds, bottom = bloom, dead
	// zone between), disjoint by construction at any handle positions. Conditions
	// beyond the handles simply saturate - Venus at 9 MPa is just "very
	// overexpanded" without the track needing to reach it.
	// ⚠️ THE REGIME IS PER GROUP NOW (2026-08-16), and that is not a mechanical change.
	// AMBIENT PRESSURE is a fact about where the ship is, so it is computed once. THE
	// BAND IS NOT: its high handle is the pressure an ENGINE is rated for, so a vacuum
	// main and a sea-level hover genuinely sit in different regimes at the same instant.
	// Hence one blend per group, evaluated up front rather than inside the plume loop -
	// four cheap evaluations beats recomputing per plume, and it lets the readout below
	// answer for the SELECTED group without re-deriving anything.
	const double P  = v->GetAtmPressure();                       // [Pa]
	const double lp = log10(P > 0.01 ? P : 0.01);
	plmRho = v->GetAtmDensity();                                 // the shimmer's air gate

	float dWg[ORO_THR_N], bWg[ORO_THR_N];
	for (int gi = 0; gi < ORO_THR_N; gi++) {
		const OroThrusterFx& T = g_fx.thr[gi];
		const double lpHi = clampd(T.plumeExpHi, PLM_EXP_LPMIN + 0.5, PLM_EXP_LPMAX);
		const double lpLo = clampd(T.plumeExpLo, PLM_EXP_LPMIN, lpHi - 0.5);
		const double win  = lpHi - lpLo;
		const double diaRamp = PLM_FR_DIA * win, bloRamp = PLM_FR_BLO * win;
		dWg[gi] = (float)clampd((lp - (lpHi - diaRamp)) / diaRamp, 0.0, 1.0);
		bWg[gi] = (float)clampd(((lpLo + bloRamp) - lp) / bloRamp, 0.0, 1.0);
	}

	// Readouts publish UNCONDITIONALLY once the vessel resolves (the reentryHeat
	// discipline): the dialog must show what the model would do even from the
	// cockpit, disarmed, or with the pill off - a regime you cannot see is a
	// pressure blend you cannot argue with. It answers for the group the panel is
	// EDITING, which is the one whose band you are dragging.
	{
		const int gs = (g_fx.thrSel >= 0 && g_fx.thrSel < ORO_THR_N) ? g_fx.thrSel : 0;
		const float dW = dWg[gs], bW = bWg[gs];
		g_fx.plumeAtmKPa = (float)(P * 1e-3);
		if      (dW >= 0.35f) strcpy_s(g_fx.plumeRegime, "overexpanded - diamonds");
		else if (dW >  0.02f) strcpy_s(g_fx.plumeRegime, "overexpanded");
		else if (bW >= 0.60f) strcpy_s(g_fx.plumeRegime, "vacuum - expansion bloom");
		else if (bW >  0.02f) strcpy_s(g_fx.plumeRegime, "underexpanded - widening");
		else                  strcpy_s(g_fx.plumeRegime, "near-ideal expansion");
	}

	// ---- THE STACK, not one vessel (2026-08-09) ---------------------------
	// Orbiter's Shuttle is FOUR vessels - Atlantis, Atlantis_Tank and two
	// Atlantis_SRB, joined by docking ports (Atlantis_SRB::clbkPostCreation walks
	// oapiGetDockStatus to find its parent). Scanning only the camera target found
	// the three SSMEs and missed BOTH SRBs, i.e. missed essentially the entire
	// visible plume of a Shuttle launch. Any multi-vessel stack has the same shape,
	// so the scan is a breadth-first walk of everything RIGIDLY connected to the
	// camera target: docking ports both ways, plus attachments in both directions.
	// The strongest-MAX_PLUMES rule then spans the stack (Shuttle: 3 SSME + 2 SRB =
	// 5, which fits exactly), and every consumer inherits the fix for free.
	OBJHANDLE stack[STACK_MAX];
	int nStack = 0;
	stack[nStack++] = hObj;
	for (int s = 0; s < nStack; s++) {
		VESSEL* sv = oapiGetVesselInterface(stack[s]);
		if (!sv) continue;
		auto addV = [&](OBJHANDLE h) {
			if (!h || nStack >= STACK_MAX) return;
			if (oapiGetObjectType(h) != OBJTP_VESSEL) return;
			for (int q = 0; q < nStack; q++) if (stack[q] == h) return;
			stack[nStack++] = h;
		};
		const UINT nd = sv->DockCount();
		for (UINT d = 0; d < nd; d++) addV(sv->GetDockStatus(sv->GetDockHandle(d)));
		for (int par = 0; par < 2; par++) {
			const DWORD na = sv->AttachmentCount(par != 0);
			for (DWORD a = 0; a < na; a++)
				addV(sv->GetAttachmentStatus(sv->GetAttachmentHandle(par != 0, a)));
		}
	}

	// Gather lit, qualifying exhausts across the stack; keep the strongest
	// MAX_PLUMES (biggest + hottest wins) - ONE selection every consumer inherits.
	// Qualifying = MAIN / RETRO / HOVER / USER, per vessel. RCS is excluded by
	// OroThrusterGroupOf returning -1 for every THGROUP_ATT_* thruster (the shimmer's
	// original user call - RCS puffs are tiny, numerous, and would eat the budget for
	// no visible gain).
	// ⚠️ THE GROUP IS CARRIED THROUGH NOW (2026-08-16). It used to be discarded right
	// here: the three groups were flattened into one handle set and all that survived
	// was the exhaust index, which is why one set of sliders had to serve every engine
	// on the ship. `grp` rides each candidate into the PlumeModel so the shape, colour,
	// soot and expansion band can be that group's own.
	// The POOL STAYS SHARED (his call): the six brightest plumes win whatever group
	// they belong to. A group whose plume pill is off is skipped below, so it now
	// frees its slots instead of holding them - a small win that falls out for free.
	struct Cand { float w; DWORD idx; int vs; int grp; bool syn; THRUSTER_HANDLE th; };
	Cand cand[32];
	int nc = 0;
	EXHAUSTSPEC es;
	for (int s = 0; s < nStack && nc < 32; s++) {
		VESSEL* sv = oapiGetVesselInterface(stack[s]);
		if (!sv) continue;
		const DWORD nex = sv->GetExhaustCount();
		for (DWORD i = 0; i < nex && nc < 32; i++) {
			const double lvl = sv->GetExhaustLevel(i);
			if (lvl < 0.02) continue;
			sv->GetExhaustSpec(i, &es);
			if (!es.lpos || !es.ldir) continue;
			const int grp = OroThrusterGroupOf(sv, es.th);
			if (grp < 0) continue;                          // RCS / not ours
			if (!g_fx.thr[grp].plumeEnabled) continue;      // this group draws no jet
			cand[nc].w   = (float)(es.lsize * lvl);
			cand[nc].idx = i;
			cand[nc].vs  = s;
			cand[nc].grp = grp;
			cand[nc].syn = false;
			cand[nc].th  = es.th;
			nc++;
		}
	}

	// SECOND PASS - the engines the first one structurally cannot see: lit thrusters
	// that NO exhaust definition references. See the PLM_SYN_* block for why the size
	// is derived from the hull and never from thrust. A thruster the author DID give an
	// exhaust is already above, so the two passes are disjoint by construction and no
	// engine can be drawn twice; an author who defines several exhausts for one
	// thruster keeps every one of them and gets no synthesised extra.
	for (int s = 0; s < nStack && nc < 32; s++) {
		VESSEL* sv = oapiGetVesselInterface(stack[s]);
		if (!sv) continue;
		const DWORD nex = sv->GetExhaustCount();
		const DWORD nth = sv->GetThrusterCount();
		for (DWORD t = 0; t < nth && nc < 32; t++) {
			THRUSTER_HANDLE th = sv->GetThrusterHandleByIndex(t);
			if (!th) continue;
			const double lvl = sv->GetThrusterLevel(th);
			if (lvl < 0.02) continue;
			const int grp = OroThrusterGroupOf(sv, th);
			if (grp < 0) continue;                          // RCS / not ours
			if (!g_fx.thr[grp].plumeEnabled) continue;
			// Already drawn by the exhaust pass? Then it is not our business.
			bool hasEx = false;
			for (DWORD i = 0; i < nex && !hasEx; i++) {
				sv->GetExhaustSpec(i, &es);
				if (es.th == th) hasEx = true;
			}
			if (hasEx) continue;
			// Is it an ENGINE? A vent modelled as a thruster is not, and should not
			// grow a jet. Acceleration, so the test carries no mass assumption.
			const double m = sv->GetMass();
			if (m > 1.0 && (sv->GetThrusterMax0(th) * lvl) / m < PLM_SYN_ACC_MIN) continue;
			const double wsyn = clampd(sv->GetSize() * PLM_SYN_W_HULL,
			                           PLM_SYN_W_MIN, PLM_SYN_W_MAX);
			cand[nc].w   = (float)(wsyn * PLM_SYN_LW * lvl);   // same weight metric as
			                                                   // above: lsize x level
			cand[nc].idx = t;                                  // THRUSTER index now
			cand[nc].vs  = s;
			cand[nc].grp = grp;
			cand[nc].syn = true;
			cand[nc].th  = th;
			nc++;
		}
	}
	// Engines all off: clear the transient tracker (so a later relight reads
	// "previous level 0" and the ignition puff fires) and stand down.
	if (!nc) { plmPrevN = 0; plmVesPrev = hObj; return; }
	for (int slot = 0; slot < MAX_PLUMES && slot < nc; slot++) {
		int best = slot;
		for (int j = slot + 1; j < nc; j++) if (cand[j].w > cand[best].w) best = j;
		if (best != slot) { const Cand t = cand[slot]; cand[slot] = cand[best]; cand[best] = t; }
	}
	const int nkeep = nc < MAX_PLUMES ? nc : MAX_PLUMES;

	for (int p = 0; p < nkeep; p++) {
		VESSEL* sv = oapiGetVesselInterface(stack[cand[p].vs]);
		if (!sv) continue;
		// Sliders the MODEL owns (the silhouette + the train's spacing/count) - the
		// consumers read the RESULTS and never re-apply them. PER GROUP since
		// 2026-08-16: these used to be hoisted out of the loop because there was only
		// one set of them. Now each plume answers to its own engine group, so they are
		// resolved per plume - and the regime blend comes from that group's own
		// expansion band, which is what lets a vacuum main and a sea-level hover be in
		// different regimes in the same frame.
		const int    grp   = cand[p].grp;
		const OroThrusterFx& T = g_fx.thr[grp];
		const float  dW    = dWg[grp], bW = bWg[grp];
		const float  kWidth = clampf(T.plumeWidth, 0.05f, 3.0f);
		const float  kLen   = clampf(T.plumeLen,   0.05f, 3.0f);
		const float  kSpace = clampf(T.plumeSpacing, 0.15f, 3.0f);
		const int    nCell  = (int)(clampf(T.plumeCells, 1.0f, 12.0f) + 0.5f);
		const bool   phys   = T.plumePhysics;
		// THE NOZZLE, from whichever source this candidate came from. Everything below
		// this block is source-agnostic on purpose: a synthesised nozzle is a normal
		// plume in every other respect, so the regime, the physics factors, the puff
		// tracker and all four consumers inherit it with no special case anywhere.
		VECTOR3 lposL, ldirL;
		double  sLofs, sLsize, sWsize, lvl;
		if (cand[p].syn) {
			if (!cand[p].th) continue;
			sv->GetThrusterRef(cand[p].th, lposL);
			sv->GetThrusterDir(cand[p].th, ldirL);
			lvl    = sv->GetThrusterLevel(cand[p].th);
			sWsize = clampd(sv->GetSize() * PLM_SYN_W_HULL, PLM_SYN_W_MIN, PLM_SYN_W_MAX);
			sLsize = sWsize * PLM_SYN_LW;
			sLofs  = 0.0;            // no author offset to honour: the reference point IS
			                         // the nozzle as far as anything can tell
		} else {
			lvl = sv->GetExhaustLevel(cand[p].idx);
			sv->GetExhaustSpec(cand[p].idx, &es);
			if (!es.lpos || !es.ldir) continue;
			lposL  = *es.lpos;
			ldirL  = *es.ldir;
			sLofs  = es.lofs;
			sLsize = es.lsize;
			sWsize = es.wsize;
		}
		if (sWsize < 1e-3 || sLsize < 1e-3) continue;       // degenerate spec: spacing
		                                                    // divides by wsize below

		PlumeModel& e = plmModel[plmModelN];
		e.grp = grp;                 // every consumer reads g_fx.thr[e.grp] from here on

		// Axis, world space. ldir is the THRUST direction: exhaust streams along
		// -ldir from the nozzle at lpos - ldir*lofs (the shimmer's construction).
		const VECTOR3 rootL = lposL - ldirL * sLofs;
		sv->Local2Global(rootL, e.rootG);
		e.hOwn = sv->GetHandle();   // whose frame rootG is in - see RenderEpochShift
		sv->GetGlobalPos(e.ownCg);  // and that vessel's CENTRE, which is what the shift
		                            // is measured from (NOT rootG - see the header)
		VECTOR3 dl = -ldirL;
		const double dll = length(dl);
		if (dll < 1e-6) continue;
		dl = dl * (1.0 / dll);
		sv->GlobalRot(dl, e.dirG);

		// The four physics factors from the overexpansion degree (LAB pins OD = 1).
		// The OD reference is the HIGH handle - the pressure this engine is RATED
		// for. Default = Earth sea level; drag it low and the hull behaves like a
		// vacuum engine (shuddering pinched diamonds at the pad, rated in space).
		// PER GROUP: the high handle is THIS engine's rated pressure, not the ship's.
		const double Pd = pow(10.0, clampd(T.plumeExpHi, PLM_EXP_LPMIN + 0.5, PLM_EXP_LPMAX));
		const double pc = 0.25 + 0.75 * lvl;
		const double OD = phys ? clampd((P / Pd) / pc, 1e-4, 8.0) : 1.0;
		const double spacingF = phys ? clampd(1.0 / sqrt(OD), 0.60, 3.0) : 1.0;
		const double widthF   = phys ? clampd(1.0 - 0.13 * (OD - 1.0), 0.72, 1.15) : 1.0;
		e.diaF  = phys ? (float)clampd(sqrt(OD), 0.55, 1.5) : 1.0f;
		e.sepW  = phys ? sstepf((float)((OD - 1.6) / 1.2)) : 0.0f;
		e.decay = phys ? (0.45f + ((float)PLM_CELL_DECAY - 0.45f) * dW) : (float)PLM_CELL_DECAY;

		e.level = lvl;
		e.dW    = dW;
		e.bW    = bW;
		// Width scales every radial size through w0; Spacing stays on the RAW
		// wsize so the two knobs are orthogonal (widen the jet without moving
		// the discs, and vice versa). The physics factors multiply both.
		e.wRef    = sWsize * kWidth;
		e.w0      = e.wRef * PLM_W0_FRAC * widthF;
		e.spacing = sWsize * PLM_SPACE_W * kSpace * spacingF;

		// Regime-blended length (weights disjoint): mid-regime jet, diamond train
		// (which STRETCHES with the physics spacing), vacuum bloom - then the
		// Length knob and the throttle scale.
		const double L_sea = e.spacing * (nCell + 1.5);
		const double L_mid = sLsize * 1.15;
		const double L_vac = sLsize * 2.3;
		e.L = (L_mid * (1.0 - dW - bW) + L_sea * dW + L_vac * bW) * kLen * (0.30 + 0.70 * lvl);
		if (e.L < 1e-3) continue;

		// IDENTITY across frames (puff tracking, soot seeds, particle spawn
		// accumulators). Exhaust index 0 exists on EVERY vessel in the stack, so the
		// stack slot is folded in - deterministic, because the BFS above visits a
		// given topology in the same order every frame.
		// ⚠️ TWO INDEX NAMESPACES SINCE 2026-08-17: cand.idx is an EXHAUST index for an
		// authored plume and a THRUSTER index for a synthesised one, and both start at 0
		// on the same vessel. Bit 15 separates them, so exhaust 0 and thruster 0 cannot
		// alias - which would have crossed their puff transients and soot seeds.
		e.exIdx = ((DWORD)cand[p].vs << 16) | (cand[p].syn ? 0x8000u : 0u) | (cand[p].idx & 0x7FFFu);

		// THROTTLE-TRANSIENT PUFF (physics mode): on ignition, shutdown and
		// throttle slams the plume blooms briefly - chamber pressure ramping
		// through off-design. Event-driven, never random (a steady vacuum plume
		// is the STEADIEST regime there is - the day-side-lightning realism
		// ruling, applied here). Matched to LAST step by EXHAUST INDEX so slot
		// reshuffles in the strongest-6 sort can never fake a transient; an
		// unmatched engine on the SAME vessel was off (level < cull), so its
		// previous level counts as 0 and ignition puffs correctly; a vessel
		// change seeds fresh (no false puff on a camera switch). Decays in REAL
		// time (the flash-event class of transient - invariant 22d); one float,
		// always falling toward zero: G10-clean by construction.
		float puff = 0.0f;
		if (phys) {
			double lvlPrev = lvl;                       // vessel change: no transient
			float  puffPrev = 0.0f;
			if (hObj == plmVesPrev) {
				lvlPrev = 0.0;                          // same vessel, engine was off
				for (int q = 0; q < plmPrevN; q++)
					if (plmIdxPrev[q] == e.exIdx) { lvlPrev = plmLvlPrev[q]; puffPrev = plmPuffPrev[q]; break; }
			}
			double dt = oapiGetSysStep();
			if (dt < 1e-3) dt = 1e-3;
			const float spike = clampf((float)(fabs(lvl - lvlPrev) / dt) * 0.5f, 0.0f, 1.0f);
			puff = puffPrev * expf((float)(-dt / 0.45));
			if (spike > puff) puff = spike;
		}
		e.puff = puff;

		plmModelN++;
	}

	// Remember this step's (exhaust idx -> level, puff) for the transient detector.
	plmVesPrev = hObj;
	plmPrevN   = plmModelN;
	for (int q = 0; q < plmModelN; q++) {
		plmIdxPrev[q]  = plmModel[q].exIdx;
		plmLvlPrev[q]  = plmModel[q].level;
		plmPuffPrev[q] = plmModel[q].puff;
	}
}

// ----------------------------------------------------------------------------
// CONSUMER 1 - the jet geometry: three ribbon layers per model entry (feathered
// sheath / cell-modulated core / diamond lozenges), projected per station at
// per-station depth, additive, depth-clipped. Only DRAWING decisions live here
// (Diamond bright, the bloom pair, colours, master strength); everything about
// the jet's physical SHAPE arrived in the model.
// ----------------------------------------------------------------------------
void OroModule::UpdatePlumeFx()
{
	plmVtxN   = 0;
	plmDkVtxN = 0;                                               // the soot layer too
	if (viewW == 0 || viewH == 0) return;
	if (!extGate || !g_fx.masterArmed) return;                   // EXTERNAL view only
	// ⚠️ NO GLOBAL PILL TEST HERE ANY MORE (2026-08-16). The pill and the master
	// strength are PER GROUP, and BuildPlumeModel has already dropped every candidate
	// whose group is switched off - so a model entry existing at all means its group
	// wants a jet. Testing the edit buffer here would have made whichever group the
	// panel happened to be showing decide whether the OTHER groups draw.
	if (plmModelN <= 0) return;                                  // nothing burning

	// THE RENDER CAMERA (2026-08-15). CONSUMER 1 of the plume model runs in the render
	// path now: the jet is screen-space geometry, and clbkPreStep does not run while
	// PAUSED, so the diamonds used to sit wherever the nozzle was when the sim stopped.
	// The MODEL (BuildPlumeModel) stays on the main thread - it is physics, not a view.
	CamCtx cc;
	if (!FillProjCam(cc.pos, cc.rot, cc.tanAp)) return;
	// Pixels of a world length w at camera depth z (the round-3.5 law: per element,
	// at per-vertex depth - never a vessel-wide anchor).
	auto pxAt = [&](double z, double w) -> float {
		return (float)(w / (z * cc.tanAp) * (viewH * 0.5));
	};
	auto emitTri = [&](float x0, float y0, DWORD c0, float d0,
	                   float x1, float y1, DWORD c1, float d1,
	                   float x2, float y2, DWORD c2, float d2) {
		if (plmVtxN + 3 > PLM_MAX_TRI * 3) return;               // full: drop silently
		plmVtx[plmVtxN].x = x0; plmVtx[plmVtxN].y = y0; plmVtx[plmVtxN].c = c0; plmDepth[plmVtxN] = d0; plmVtxN++;
		plmVtx[plmVtxN].x = x1; plmVtx[plmVtxN].y = y1; plmVtx[plmVtxN].c = c1; plmDepth[plmVtxN] = d1; plmVtxN++;
		plmVtx[plmVtxN].x = x2; plmVtx[plmVtxN].y = y2; plmVtx[plmVtxN].c = c2; plmDepth[plmVtxN] = d2; plmVtxN++;
	};

	// Colour picks (COLORREF 0x00BBGGRR). Jet = the core + diamond body; Bloom = the
	// vacuum halo. Diamond WHITE is not in either palette - it emerges from the fp16
	// accumulation + the client's threshold bloom (patch i), with a per-vertex push
	// toward white at the nodes so the discs saturate first.
	// (the colour picks and every draw knob are resolved PER PLUME inside the loop
	//  since 2026-08-16 - see the block at the top of it.)
	const float t_anim  = (float)animT;                          // REAL time (invariant 4)

	auto emitDk = [&](float x0, float y0, DWORD c0, float d0,
	                  float x1, float y1, DWORD c1, float d1,
	                  float x2, float y2, DWORD c2, float d2) {
		if (plmDkVtxN + 3 > PLM_DK_MAX_TRI * 3) return;          // full: drop silently
		plmDkVtx[plmDkVtxN].x = x0; plmDkVtx[plmDkVtxN].y = y0; plmDkVtx[plmDkVtxN].c = c0; plmDkDepth[plmDkVtxN] = d0; plmDkVtxN++;
		plmDkVtx[plmDkVtxN].x = x1; plmDkVtx[plmDkVtxN].y = y1; plmDkVtx[plmDkVtxN].c = c1; plmDkDepth[plmDkVtxN] = d1; plmDkVtxN++;
		plmDkVtx[plmDkVtxN].x = x2; plmDkVtx[plmDkVtxN].y = y2; plmDkVtx[plmDkVtxN].c = c2; plmDkDepth[plmDkVtxN] = d2; plmDkVtxN++;
	};

	for (int p = 0; p < plmModelN; p++) {
		const PlumeModel& e = plmModel[p];
		// THIS PLUME'S OWN GROUP decides every draw knob (2026-08-16). These were all
		// hoisted out of the loop when one set of numbers served the whole ship; they
		// are per plume now, which is what lets a hydrolox main and a hypergolic retro
		// be different colours, with different soot, in the same frame.
		const OroThrusterFx& T = g_fx.thr[(e.grp >= 0 && e.grp < ORO_THR_N) ? e.grp : 0];
		int jR, jG, jB, bR, bG, bB;
		UnpackCR(T.plumeColJet,   jR, jG, jB);   // core + diamond body
		UnpackCR(T.plumeColBloom, bR, bG, bB);   // the vacuum halo
		const float master  = clampf(T.plume, 0.0f, 1.0f);
		const int   nCell   = (int)(clampf(T.plumeCells, 1.0f, 12.0f) + 0.5f);
		const float kDia    = clampf(T.plumeDiamond,  0.0f, 2.0f);
		const float kBloomW = clampf(T.plumeBloomWid, 0.0f, 2.0f);
		const float kBloomB = clampf(T.plumeBloomBri, 0.0f, 2.0f);
		const float kThroat = clampf(T.plumeThroat,   0.0f, 4.0f);
		const float kThrOfs = clampf(T.plumeThroatOfs, 0.0f, 1.0f);   // [m] downstream slide
		const float kSoot   = clampf(T.plumeSoot,     0.0f, 2.0f);
		const float kChurn  = clampf(T.plumeSootRate, 0.0f, 3.0f);
		if (master <= 0.001f) continue;          // this group's master is down
		const float   dW = e.dW, bW = e.bW;
		const double  w0 = e.w0, spacing = e.spacing, L = e.L;
		const double  lvl = e.level;
		// ⚠️ RENDER-EPOCH ANCHOR (invariant 21a). rootG was sampled at pre-step in the
		// BARYCENTRIC frame; this build runs a step later, by which time Earth has moved
		// ~500 m. Without this the whole jet sits off the ship and jitters with frame
		// pacing. dirG needs no correction - a translation does not rotate anything.
		const VECTOR3 rootG = e.rootG + RenderEpochShift(e.hOwn, e.ownCg);
		const VECTOR3 dirG  = e.dirG;

		// The bloom's opening half-angle: a small natural divergence always, plus
		// the vacuum expansion as ambient pressure stops confining the jet - and
		// the throttle-transient puff briefly flares it open.
		const double spreadTan = tan(RAD * (PLM_SPREAD_LO + PLM_SPREAD_HI * pow((double)bW, 1.3))) * kBloomW
		                       * (1.0 + 0.5 * e.puff);

		// SEPARATION UNSTEADINESS (physics factor 4): the whole train breathes back
		// and forth and its brightness pulses, coherently per plume per frame - a
		// per-station wander would be G12(c)'s chord-polyline bug all over again.
		const double sepShift = e.sepW * (0.10 * sin(t_anim * 12.7f + p * 1.7f)
		                                + 0.05 * sin(t_anim * 29.3f + p * 3.1f));
		const float  sepPulse = 1.0f - e.sepW * 0.22f * (0.5f + 0.5f * sinf(t_anim * 11.3f + p * 2.3f));
		const float  sepWob   = 1.0f + e.sepW * 0.05f * sinf(t_anim * 17.1f + p * 1.3f);

		// THE THROAT FIRE (his report 2026-08-09: stock's billboard filled the
		// bell cup; ours left it hollow). The axis extends UPSTREAM into the bell
		// by ~one nozzle width; the cup section draws bright, whitened, tapering
		// with the bell's interior, and the patch-(g) depth clip does the real
		// work - visible through the mouth, hidden by the bell walls per pixel.
		const double throatL = (kThroat > 0.01f) ? e.wRef * 1.15 : 0.0;
		const double tMin = -throatL;

		// NEAR-PLANE CLAMP (his report 2026-08-09: "clipping when the camera gets
		// close - we don't want that"). The first build projected root and tip and
		// SKIPPED the plume when either fell behind the camera - so a close pass
		// popped the whole jet off exactly when it filled the view. Camera-space z
		// is LINEAR along the axis, so instead: clamp the DRAWN SUB-RANGE [tA, tB]
		// to the part in front of the near plane and keep the shock-cell math in
		// absolute t - the structure stays anchored to the nozzle and only the
		// visible window slides. The per-station NEAR FADE below dissolves the last
		// few metres before the camera, so the clamp line itself can never show.
		const double ZNEAR = 1.05;                  // just past ProjPx's z >= 1 floor
		const double z0 = tmul(cc.rot, (rootG + dirG * tMin) - cc.pos).z;
		const double z1 = tmul(cc.rot, (rootG + dirG * L) - cc.pos).z;
		if (z0 < ZNEAR && z1 < ZNEAR) continue;     // entirely behind the camera
		double tA = tMin, tB = L;
		if      (z0 < ZNEAR) tA = tMin + (L - tMin) * (ZNEAR - z0) / (z1 - z0);
		else if (z1 < ZNEAR) tB = tMin + (L - tMin) * (ZNEAR - z0) / (z1 - z0);
		if (tB - tA < 1e-3) continue;

		float rpx, rpy, tpx, tpy;
		double rz, tz;
		if (!ProjPx(cc, rootG + dirG * tA, viewW, viewH, rpx, rpy, rz)) continue;
		if (!ProjPx(cc, rootG + dirG * tB, viewW, viewH, tpx, tpy, tz)) continue;

		// Off-screen cull, with margin for the sheath's width.
		const float mx = 0.4f * viewW, my = 0.4f * viewH;
		if ((rpx < -mx && tpx < -mx) || (rpx > viewW + mx && tpx > viewW + mx)) continue;
		if ((rpy < -my && tpy < -my) || (rpy > viewH + my && tpy > viewH + my)) continue;

		// End-on: the projected axis collapses and a ribbon has no direction to
		// span (G7's degenerate case). Fade out - the stock billboard carries the
		// straight-down-the-nozzle view; our structure has nothing to add there.
		const float axdx = tpx - rpx, axdy = tpy - rpy;
		const float Lpx = sqrtf(axdx * axdx + axdy * axdy);
		const float endFade = sstepf((Lpx - 6.0f) / 12.0f);
		if (endFade <= 0.01f) continue;
		const float pdx = -axdy / Lpx, pdy = axdx / Lpx;         // screen perpendicular

		// Hull occlusion FALLBACK (the shimmer's facing fade, invariant 11): only
		// when the per-pixel clip is dark - with the clip live it would fade plumes
		// the depth buffer can cut exactly.
		float vis = 1.0f;
		if (!depthClipOK) {
			const VECTOR3 c2p = rootG - cc.pos;
			const double lc = length(c2p);
			if (lc > 1e-6) {
				const double facing = dotp(dirG, c2p * (1.0 / lc));
				const double OCC0 = 0.20, OCC1 = 0.60;
				if (facing >= OCC1)      vis = 0.0f;
				else if (facing > OCC0)  { const double q = (facing - OCC0) / (OCC1 - OCC0); vis = (float)(1.0 - q * q * (3.0 - 2.0 * q)); }
			}
			if (vis < 0.01f) continue;
		}

		const float thrust = (float)pow(lvl, 0.45);              // saturating (the
		                                                         //   shimmer's law: idle
		                                                         //   is hot too)
		const float gain = master * thrust * endFade * vis;

		// Sub-pixel cull: if even the sheath is a hairline, skip the slot.
		if (pxAt(rz, w0 * PLM_SHEATH_X + L * spreadTan) < 1.0f && Lpx < 8.0f) continue;

		// Diamond-layer colour is constant per plume: the jet pick pushed hard toward
		// white (0.85) - the lozenges are the white-hot part of the jet, and the fp16
		// bloom finishes the job.
		const int diaR = jR + (int)((255 - jR) * 0.85f);
		const int diaG = jG + (int)((255 - jG) * 0.85f);
		const int diaB = jB + (int)((255 - jB) * 0.85f);

		// --- stations ---------------------------------------------------------
		float  sx[PLM_NS], sy[PLM_NS], sed[PLM_NS], nfv[PLM_NS];
		float  wCor[PLM_NS], wShe[PLM_NS], wDia[PLM_NS];
		DWORD  cCor[PLM_NS], cCorE[PLM_NS], cShe[PLM_NS], cSheE[PLM_NS], cDia[PLM_NS], cDiaE[PLM_NS];
		for (int i = 0; i < PLM_NS; i++) {
			const double t  = tA + (tB - tA) * (double)i / (double)(PLM_NS - 1);
			const double tt = t / L;                             // fades stay in FULL-jet
			                                                     //   coords - the window
			                                                     //   slides, the jet doesn't
			const double ttc = tt < 0.0 ? 0.0 : tt;              // cup stations fade like
			                                                     //   the root (never boosted
			                                                     //   by a negative tt)
			// THE CUP (t < 0): taper with the bell interior, boost and whiten the
			// fire, flicker gently. Everything else - cells, lozenges, soot - is
			// naturally absent upstream (their coordinates go negative and skip).
			const bool inCup = (t < 0.0);
			double cupW = 1.0, cupWS = 1.0;
			float  cupBoost = 1.0f, cupSheath = 1.0f, cupWhite = 0.0f;
			if (inCup) {
				const double d = t / throatL;                    // 0 at the lip -> -1 deep
				cupW  = 1.0 + 0.45 * d;
				cupWS = 1.0 + 0.60 * d;
				cupBoost  = kThroat * (1.45f + 0.15f * sinf(t_anim * 29.0f + (float)p * 2.3f));
				cupSheath = kThroat * 0.8f;
				cupWhite  = 0.55f;
			}
			const VECTOR3 pg = rootG + dirG * t;
			// On-segment points project iff the clamped endpoints did (z linear in
			// t), so this cannot fail - but the guard costs nothing.
			double z; float px, py;
			if (!ProjPx(cc, pg, viewW, viewH, px, py, z)) { px = rpx; py = rpy; z = rz; }
			sx[i] = px; sy[i] = py;
			sed[i] = (float)length(pg - cc.pos);                 // patch-(g) depth:
			                                                     //   EUCLIDEAN, like
			                                                     //   GBUF_DEPTH.a
			// The near fade: dissolve over the last ~4 m before the camera plane, so
			// neither the clamp line nor a hard cross-section shows inside the jet.
			const float nearF = sstepf((float)((z - ZNEAR) / 4.0));
			nfv[i] = nearF;                                      // the soot pass reuses it

			// Shock-cell coordinate: node k sits at (PLM_NODE0 + k) spacings.
			// sepShift breathes the whole train coherently when separation is live.
			// The decay base comes from the MODEL (washout: 0.80 at sea level,
			// steeper as the diamond band fades with altitude).
			const double s  = t / spacing - PLM_NODE0 + sepShift;
			const double kf = floor(s + 0.5);
			const int    k  = (int)kf;
			const double ds = s - kf;                            // -0.5..0.5, 0 at a node
			float cellDecay = 0.0f, nodeBump = 0.0f;
			if (k >= 0 && k <= nCell) {
				cellDecay = (float)pow((double)e.decay, (double)k);
				const float bb = (float)(1.0 - fabs(ds) / 0.30);
				if (bb > 0.0f) nodeBump = sstepf(bb);
			}

			// THE LOZENGE ("more diamond-y", user report 2026-08-09). The first build
			// put the brightness AT the waists - bright LUMPS, not diamonds. The
			// classic shape is the bright cell BETWEEN the discs: a lozenge with its
			// TIPS at node k and k+1 (width zero exactly where the core pinches) and
			// its widest point mid-cell - a TRIANGULAR width profile, deliberately not
			// smoothed, because the straight Gouraud edges between stations are what
			// draw the pointed ends. Brightness is front-loaded (the gas glows
			// hottest just after the disc reheats it) and decays per cell.
			const int    kL = (int)floor(s);                     // lozenge index: node k -> k+1
			const double u  = s - floor(s);                      // 0..1 across the lozenge
			float lozW = 0.0f, lozA = 0.0f;
			if (dW > 0.02f && kL >= 0 && kL < nCell) {
				const float dec    = (float)pow((double)e.decay, (double)kL);
				const float flickL = 0.88f + 0.12f * sinf(t_anim * 23.0f + (float)kL * 2.63f + (float)p * 1.91f);
				lozW = (float)(1.0 - fabs(2.0 * u - 1.0));       // triangular: tips at the discs
				lozA = PLM_A_DIA * dW * dec * kDia * flickL * (float)(1.0 - 0.55 * u)
				     * e.diaF * sepPulse;                        // throttle coupling + the
				                                                 //   separation pulse
			}

			// Widths [m]: the sheath opens with the bloom; the core keeps a third
			// of the spread, converges slightly when overexpanded, and carries the
			// cell structure - a waist AT each disc, a slight bulge between.
			double wc = (w0 * 0.95 * (1.0 - 0.20 * dW * ttc) + t * spreadTan * 0.35);
			wc *= 1.0 + dW * cellDecay * (0.10 - 0.42 * nodeBump);
			wc *= sepWob;                                        // separation: the jet
			                                                     //   itself trembles
			wc *= cupW;                                          // the bell interior taper
			double ws = (w0 * PLM_SHEATH_X) * (1.0 - 0.15 * dW * ttc) + t * spreadTan;
			ws *= cupWS;
			if (wc < 0.0) wc = 0.0;
			if (ws < 0.0) ws = 0.0;
			wCor[i] = pxAt(z, wc);
			wShe[i] = pxAt(z, ws);
			wDia[i] = pxAt(z, w0 * 0.85 * lozW);                 // the lozenge: tips at
			                                                     //   the discs, widest
			                                                     //   mid-cell

			// Brightness. Body fades downstream; at each disc the core keeps only a
			// small GLINT now (the Mach disc itself) - the lozenge layer carries the
			// diamond brightness since the rework. Flicker is per-node/per-lozenge
			// phased, zero where its bump is, so it can never tear a segment.
			const float bodyFade = powf((float)(1.0 - ttc), 1.35f);
			const float flick = 0.90f + 0.10f * sinf(t_anim * 23.0f + (float)k * 2.63f + (float)p * 1.91f);
			// The transient puff flares the sheath hard and the core gently - in
			// vacuum the sheath IS the visible plume, so ignitions read there.
			const float coreA = PLM_A_CORE * (0.55f + 0.45f * dW) * (1.0f - 0.55f * bW) * bodyFade
			                  * (1.0f + 0.3f * e.puff) * cupBoost;
			const float diaA  = PLM_A_DIA * 0.30f * dW * cellDecay * nodeBump * kDia * flick
			                  * e.diaF * sepPulse;               // the disc glint follows
			                                                     //   the lozenges' factors
			const float sheA  = (PLM_A_SHEATH * (1.0f - 0.35f * dW) + PLM_A_BLOOM * bW * kBloomB) * bodyFade
			                  * (1.0f + 1.6f * e.puff) * cupSheath;

			// Colours. Core = jet pick, nudged toward white at the disc glints (the
			// lozenges carry the real white push); sheath = jet -> bloom pick as the
			// vacuum takes over. Edge columns share the rgb at ALPHA 0 - the feather.
			const float wf = clampf(0.40f * nodeBump * cellDecay * dW + cupWhite, 0.0f, 1.0f);
			const int cr = (int)(jR + (255 - jR) * wf);
			const int cg = (int)(jG + (255 - jG) * wf);
			const int cb = (int)(jB + (255 - jB) * wf);
			const int sr = (int)(jR + (bR - jR) * bW);
			const int sg = (int)(jG + (bG - jG) * bW);
			const int sb = (int)(jB + (bB - jB) * bW);
			cCor[i]  = ACol(cr, cg, cb, (int)((coreA + diaA) * gain * nearF));
			cCorE[i] = ACol(cr, cg, cb, 0);
			cShe[i]  = ACol(sr, sg, sb, (int)(sheA * gain * nearF));
			cSheE[i] = ACol(sr, sg, sb, 0);
			cDia[i]  = ACol(diaR, diaG, diaB, (int)(lozA * gain * nearF));
			cDiaE[i] = ACol(diaR, diaG, diaB, 0);
		}

		// --- ribbons: sheath under, core, then the diamond lozenges on top
		// (additive - order is cosmetic). Each layer is a 3-column strip (edge-0 /
		// centre / edge-0): two quads = four triangles per segment, Gouraud
		// feathering the width for free. The lozenge layer only exists in the
		// diamond regime - skipping it entirely keeps the vacuum bloom at two
		// layers and the pool honest.
		const int nLay = (dW > 0.02f) ? 3 : 2;
		for (int layer = 0; layer < nLay; layer++) {
			const float* w   = (layer == 0) ? wShe  : (layer == 1) ? wCor  : wDia;
			const DWORD* cc_ = (layer == 0) ? cShe  : (layer == 1) ? cCor  : cDia;
			const DWORD* ce  = (layer == 0) ? cSheE : (layer == 1) ? cCorE : cDiaE;
			for (int i = 0; i + 1 < PLM_NS; i++) {
				const float l0x = sx[i]   - pdx * w[i],   l0y = sy[i]   - pdy * w[i];
				const float r0x = sx[i]   + pdx * w[i],   r0y = sy[i]   + pdy * w[i];
				const float l1x = sx[i+1] - pdx * w[i+1], l1y = sy[i+1] - pdy * w[i+1];
				const float r1x = sx[i+1] + pdx * w[i+1], r1y = sy[i+1] + pdy * w[i+1];
				emitTri(l0x, l0y, ce[i],  sed[i], sx[i],  sy[i],  cc_[i],  sed[i], sx[i+1], sy[i+1], cc_[i+1], sed[i+1]);
				emitTri(l0x, l0y, ce[i],  sed[i], sx[i+1], sy[i+1], cc_[i+1], sed[i+1], l1x, l1y, ce[i+1], sed[i+1]);
				emitTri(sx[i], sy[i], cc_[i], sed[i], r0x, r0y, ce[i],  sed[i], r1x, r1y, ce[i+1], sed[i+1]);
				emitTri(sx[i], sy[i], cc_[i], sed[i], r1x, r1y, ce[i+1], sed[i+1], sx[i+1], sy[i+1], cc_[i+1], sed[i+1]);
			}
		}

		// --- THE THROAT DISCS (cup fire, round 2) -----------------------------
		// His report on round 1: "not very good even at full slider" - because
		// the ribbon extension is a FLAT STRIP along the axis, and it foreshortens
		// to nothing exactly at the angles where the cup interior shows most
		// (which is why stock's CAMERA-FACING billboard filled the cup and ours
		// did not). The cup's body is therefore CAMERA-FACING: two soft discs at
		// the throat - the plasma origin-glow pattern, a fan around one projected
		// point with a bright whitened centre, an alpha-0 rim, and SMOOTH angular
		// jitter (hash radii were round 5.3's irregular-polygon bug). The patch-(g)
		// clip carves them against the bell walls per pixel: full fire through the
		// mouth, nothing from the side. Deliberately NOT endFade-gated - the
		// end-on view is this element's whole job; nearF still owns the close-in
		// dissolve, and the ribbon cup section stays for the profile views.
		if (kThroat > 0.01f) {
			const int NSEG = 20;
			const int wr = jR + (int)((255 - jR) * 0.75f);
			const int wg = jG + (int)((255 - jG) * 0.75f);
			const int wb = jB + (int)((255 - jB) * 0.75f);
			const float discGain = master * thrust * vis;        // no endFade, by design
			for (int disc = 0; disc < 2; disc++) {
				const double dIn = disc ? 0.55 : 0.15;           // depth into the cup, x wRef
				const double rad = (disc ? 0.44 : 0.62) * e.wRef;
				// The Throat offset slider slides the discs DOWNSTREAM, out of the
				// bell - per hull, the visual nozzle and the exhaust spec disagree.
				const VECTOR3 cg = rootG + dirG * ((double)kThrOfs - e.wRef * dIn);
				float cx, cy; double cz;
				if (!ProjPx(cc, cg, viewW, viewH, cx, cy, cz)) continue;
				const float ced = (float)length(cg - cc.pos);
				const float nf  = sstepf((float)((cz - ZNEAR) / 4.0));
				const float rp  = pxAt(cz, rad);
				if (rp < 1.0f || nf <= 0.01f) continue;
				const float aC = (disc ? 128.0f : 102.0f) * kThroat
				               * (0.92f + 0.08f * sinf(t_anim * 27.0f + (float)p * 2.1f + (float)disc * 1.3f))
				               * discGain * nf;
				const DWORD cCen = ACol(wr, wg, wb, (int)aC);
				const DWORD cRim = ACol(wr, wg, wb, 0);
				for (int sgm = 0; sgm < NSEG; sgm++) {
					const float a0 = PI2 * (float)sgm / NSEG;
					const float a1 = PI2 * (float)(sgm + 1) / NSEG;
					auto rimAt = [&](float a) -> float {
						return rp * (1.0f + 0.10f * sinf(a * 3.0f + t_anim * 2.3f + (float)p)
						                  + 0.06f * sinf(a * 5.0f - t_anim * 1.7f + (float)disc));
					};
					const float r0 = rimAt(a0), r1 = rimAt(a1);
					emitTri(cx, cy, cCen, ced,
					        cx + cosf(a0) * r0, cy + sinf(a0) * r0, cRim, ced,
					        cx + cosf(a1) * r1, cy + sinf(a1) * r1, cRim, ced);
				}
			}
		}

		// --- ABLATIVE SOOT STREAKS (the dark layer) ---------------------------
		// Thin alpha-blended wisps hugging the core from the nozzle lip - the
		// Merlin ablative look. They ride the SAME stations (strided: streaks
		// need no per-cell resolution), offset laterally by per-streak hashes
		// seeded on the EXHAUST INDEX (stable however the strongest-6 sort
		// reshuffles slots), and they are STRAIGHT by law - a wavy path is
		// G12(c)'s chord polyline. Emitted into the separate dark buffer;
		// DrawPlumePoly draws it AFTER the additive glow with plain alpha
		// blending, so the wisps genuinely dim the glow they sit in (soot is IN
		// the jet - the G11 draw-dark-first recipe is for smoke BEHIND content,
		// which this deliberately is not). Alpha = the Soot slider (0 = off: the
		// slider IS the toggle, the aurora's opt-in law), dense at the lip and
		// dissolving downstream.
		if (kSoot > 0.01f) {
			// SIXTEEN LIFECYCLED STREAKS (the dynamic rework - his report: ablation
			// is "a lot more streaks, changing position and length all the time").
			// Each slot runs its own hashed PERIOD on real time: born at a freshly
			// hashed rim position, SHOOTS OUT (length grows from the lip over the
			// first third of its life), flickers, fades, then reseeds elsewhere -
			// the incarnation's seeds change with the CYCLE NUMBER, so every
			// rebirth is a new streak. Pure functions of animT (the lightning-
			// cadence trick): no stored state, nothing accumulates (G10), and the
			// envelopes reach zero BEFORE the cycle boundary so a rebirth can
			// never pop. Alternating sides + rim-biased offsets as before; an
			// occasional HEAVY SHED (hash-gated) doubles a streak's width and
			// density - the chunk letting go. Soot churn scales the clock; 0
			// freezes the pattern.
			// Soot starts AT the lip: with the throat extension the station walk
			// begins inside the cup, and skipping whole straddling strided
			// segments quantized to ~a metre of bare jet ("disconnected from the
			// bell"). Stride from the first at-or-past-the-lip station instead.
			int i0 = 0;
			if (tA < 0.0) i0 = (int)ceil((0.0 - tA) / (tB - tA) * (double)(PLM_NS - 1));
			if (i0 < 0) i0 = 0;
			const int NSTK = 16, STRIDE = 5;
			for (int j = 0; j < NSTK; j++) {
				// The lifecycle clock.
				const float hp     = hashf((float)e.exIdx * 2.11f + (float)j * 5.03f);
				const float ho     = hashf((float)e.exIdx * 6.37f + (float)j * 1.27f + 0.4f);
				const float period = 0.55f + 1.45f * hp;             // seconds per life
				const float ph     = (kChurn > 0.001f ? t_anim * kChurn / period : 0.0f) + ho * 7.0f;
				const float cyc    = floorf(ph);
				const float u      = ph - cyc;                       // 0..1 through this life
				// This incarnation's seeds - cyc in the hash = a NEW streak each rebirth.
				const float h1 = hashf((float)e.exIdx * 3.17f + (float)j * 7.13f + cyc * 3.77f);
				const float h2 = hashf((float)e.exIdx * 5.71f + (float)j * 2.39f + cyc * 9.31f + 0.7f);
				const float h3 = hashf((float)e.exIdx * 1.93f + (float)j * 4.81f + cyc * 1.61f + 1.9f);
				const float h4 = hashf(h1 * 9.17f + cyc * 0.71f);
				const bool  heavy = (h4 > 0.87f);                    // the chunk letting go
				// Envelope: fast ramp-in, fade-out completing AT the boundary; the
				// length GROWS over the first ~third (the "shooting out").
				const float grow = sstepf(u / 0.32f);
				const float env  = sstepf(u / 0.10f)
				                 * (1.0f - sstepf((u - 0.72f) / 0.28f))
				                 * (0.82f + 0.18f * sinf(t_anim * (11.0f + 9.0f * hp) + (float)j * 2.1f));
				if (env <= 0.02f || grow <= 0.02f) continue;
				const float  sgn   = (j & 1) ? 1.0f : -1.0f;
				const float  off   = sgn * (0.35f + 0.62f * powf(h1, 0.55f));  // rim-heavy
				const double tEndS = (0.22 + 0.50 * h2) * L * (double)grow;    // live length
				if (tEndS < 0.05) continue;
				const float  wdF   = (0.07f + 0.08f * h3) * (heavy ? 1.9f : 1.0f);
				const float  aStk  = (0.60f + 0.40f * h4) * 105.0f * kSoot * env
				                   * (heavy ? 1.5f : 1.0f);
				for (int i = i0; i + STRIDE < PLM_NS; i += STRIDE) {
					const int i2 = i + STRIDE;
					const double tI0 = tA + (tB - tA) * (double)i  / (double)(PLM_NS - 1);
					const double tI1 = tA + (tB - tA) * (double)i2 / (double)(PLM_NS - 1);
					if (tI0 >= tEndS) break;
					auto stkA = [&](double t, int idx) -> int {
						const double f = 1.0 - t / tEndS;          // dense at the lip
						if (f <= 0.0) return 0;
						return (int)(aStk * pow(f, 1.15) * gain * nfv[idx]);
					};
					const float c0x = sx[i]  + pdx * (off * wCor[i]),  c0y = sy[i]  + pdy * (off * wCor[i]);
					const float c1x = sx[i2] + pdx * (off * wCor[i2]), c1y = sy[i2] + pdy * (off * wCor[i2]);
					const float w0p = wdF * wCor[i], w1p = wdF * wCor[i2];
					const DWORD k0 = ACol(26, 21, 17, stkA(tI0, i));    // fixed warm soot
					const DWORD k1 = ACol(26, 21, 17, stkA(tI1, i2));
					const DWORD e0 = ACol(26, 21, 17, 0);
					const float l0x = c0x - pdx * w0p, l0y = c0y - pdy * w0p;
					const float r0x = c0x + pdx * w0p, r0y = c0y + pdy * w0p;
					const float l1x = c1x - pdx * w1p, l1y = c1y - pdy * w1p;
					const float r1x = c1x + pdx * w1p, r1y = c1y + pdy * w1p;
					emitDk(l0x, l0y, e0, sed[i], c0x, c0y, k0, sed[i], c1x, c1y, k1, sed[i2]);
					emitDk(l0x, l0y, e0, sed[i], c1x, c1y, k1, sed[i2], l1x, l1y, e0, sed[i2]);
					emitDk(c0x, c0y, k0, sed[i], r0x, r0y, e0, sed[i], r1x, r1y, e0, sed[i2]);
					emitDk(c0x, c0y, k0, sed[i], r1x, r1y, e0, sed[i2], c1x, c1y, k1, sed[i2]);
				}
			}
		}
	}

	// Invariant 3: the render proc hands the client the FULL creation count -
	// zero-pad the unused tail (alpha-0 degenerate) or discarded VRAM strobes.
	if (plmVtxN > 0 && plmVtxN < PLM_MAX_TRI * 3) {
		memset(&plmVtx[plmVtxN],   0, sizeof(PlasVtx) * (PLM_MAX_TRI * 3 - plmVtxN));
		memset(&plmDepth[plmVtxN], 0, sizeof(float)   * (PLM_MAX_TRI * 3 - plmVtxN));
	}
	if (plmDkVtxN > 0 && plmDkVtxN < PLM_DK_MAX_TRI * 3) {
		memset(&plmDkVtx[plmDkVtxN],   0, sizeof(PlasVtx) * (PLM_DK_MAX_TRI * 3 - plmDkVtxN));
		memset(&plmDkDepth[plmDkVtxN], 0, sizeof(float)   * (PLM_DK_MAX_TRI * 3 - plmDkVtxN));
	}
}

// ----------------------------------------------------------------------------
// STOCK EXHAUST suppression (client patch n) - the judging pill. While the
// dialog's STOCK EXHAUST pill is OFF (and ORO is armed), the camera-target
// vessel's stock exhaust billboards and exhaust-stream emission are suppressed
// client-side, so the overlay above can be judged alone. Invariant 18's rules:
// probed by binding, pushed on CHANGE only, and Ctrl+G / disarm hands stock
// back instantly. Invariant 14's rule: the suppression is a BORROW, returned
// on every exit path - hStockExSupp is the one vessel currently borrowed from.
// ----------------------------------------------------------------------------
void OroModule::UpdateStockExhaust()
{
	if (!pCore || !pCore->CanSuppressExhaust()) return;

	// Who should be suppressed right now? The camera-target vessel AND EVERYTHING
	// DOCKED OR ATTACHED TO IT (the same stack BuildPlumeModel draws - suppressing
	// only the camera target would leave the Shuttle's two SRBs burning their stock
	// billboards under our overlay), while armed with the pill off - else nobody.
	// NOT gated on extGate: suppression is a scene fact, and flipping it per view
	// would strobe the particle streams' emission on every F1.
	// The two halves suppress independently now (the patch-(n) split): the EXHAUST
	// tab's pill owns the billboards, the PARTICLES tab's owns the stock streams.
	DWORD flags = 0;
	if (!g_fx.stockExhaust)   flags |= GCEXH_BILLBOARD;
	if (!g_fx.stockParticles) flags |= GCEXH_STREAM;

	OBJHANDLE want[STACK_MAX];
	int nWant = 0;
	if (g_fx.masterArmed && flags) {
		OBJHANDLE h = oapiCameraTarget();
		if (!h || oapiGetObjectType(h) != OBJTP_VESSEL) h = oapiGetFocusObject();
		if (h && oapiGetObjectType(h) == OBJTP_VESSEL) {
			want[nWant++] = h;
			for (int s = 0; s < nWant; s++) {
				VESSEL* sv = oapiGetVesselInterface(want[s]);
				if (!sv) continue;
				auto addV = [&](OBJHANDLE hn) {
					if (!hn || nWant >= STACK_MAX) return;
					if (oapiGetObjectType(hn) != OBJTP_VESSEL) return;
					for (int q = 0; q < nWant; q++) if (want[q] == hn) return;
					want[nWant++] = hn;
				};
				const UINT nd = sv->DockCount();
				for (UINT d = 0; d < nd; d++) addV(sv->GetDockStatus(sv->GetDockHandle(d)));
				for (int par = 0; par < 2; par++) {
					const DWORD na = sv->AttachmentCount(par != 0);
					for (DWORD a = 0; a < na; a++)
						addV(sv->GetAttachmentStatus(sv->GetAttachmentHandle(par != 0, a)));
				}
			}
		}
	}

	// Push on CHANGE (invariant 18) - compare the SETS, not just the head, or a
	// staging separation would leave a jettisoned booster suppressed forever.
	bool same = (nWant == nStockExSupp) && (flags == stockExFlags);
	if (same) for (int i = 0; i < nWant; i++) if (want[i] != hStockExSupp[i]) { same = false; break; }
	if (same) return;

	ReleaseStockExhaust();                               // hand the old set back
	for (int i = 0; i < nWant; i++) {
		hStockExSupp[i] = want[i];
		pCore->SuppressExhaust(want[i], flags);
	}
	nStockExSupp = nWant;
	stockExFlags = flags;
}

// The hand-it-back path - clbkDeleteVessel (handle still valid during the callback),
// simulation end, destructor. Safe to call twice; oapiIsVessel guards teardown-order
// races exactly like the reentry table's returns.
void OroModule::ReleaseStockExhaust()
{
	if (pCore && pCore->CanSuppressExhaust()) {
		for (int i = 0; i < nStockExSupp; i++)
			if (hStockExSupp[i] && oapiIsVessel(hStockExSupp[i]))
				pCore->SuppressExhaust(hStockExSupp[i], 0);
	}
	for (int i = 0; i < STACK_MAX; i++) hStockExSupp[i] = NULL;
	nStockExSupp = 0;
	stockExFlags = 0;
}
