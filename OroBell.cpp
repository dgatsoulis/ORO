// ==============================================================
// OroBell.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - BELL GLOW: incandescent nozzle shells (2026-08-09)
// ----------------------------------------------------------------------------
// The Merlin Vacuum reference: a radiatively-cooled nozzle skirt running
// visibly hot - dark base, banded orange glow, dark soot streaks - heating
// over seconds and cooling through a long dull-red ember tail after cutoff.
//
// ARCHITECTURE (the discussion's plan, verbatim):
// - The AUTHOR ships the shell: Meshes\ORO\<class>_bell.msh (the heatshield
//   override pattern - file presence IS the opt-in). Groups are LABELed
//   MAIN / HOVER / RETRO / USER, each with its own material; the banding
//   structure lives in the texture's ALPHA channel (Textures\ORO\
//   bell_glow.tga, the synthesized mask - transparent texels stay dark metal).
// - The Orbiter API exposes no group labels or material names at runtime, so
//   ORO PARSES THE .MSH TEXT ITSELF (the lightning's Cloud.tree move): LABEL
//   lines and the MATERIALS name list map families to group + material indices.
// - The mesh is ADDED to the camera-target vessel (VESSEL::AddMesh) so the
//   client renders it as normal geometry - correct depth, lighting, bloom.
//   It is a BORROW in invariant 14's sense: tracked and removed on every exit
//   path (target switch, disarm/Ctrl+G, trim 0, vessel delete, sim end).
// - Each family runs a THERMAL MODEL on SIM time (a cooling nozzle is a
//   physical object - warp speeds it): heating toward the radiative
//   equilibrium T ~ level^(1/4) with tau ~3 s; cooling by real radiation,
//   dT/dt = -K*T^4, integrated in closed form (unconditionally stable at any
//   warp) - which gives the characteristic fade for free: white heat drops in
//   seconds, the dull-red tail lingers for the better part of a minute.
// - The DRIVER: per family, oapiSetMaterial on OUR mesh instance
//   (oapiObjectVisualPtr -> VESSEL::GetDevMesh, re-acquired per use - visuals
//   die and rebuild with camera range). Emissive = blackbody(T) x the Bell
//   glow trim; diffuse alpha = the thermal fade, so a cold shell renders
//   NOTHING (no z-fighting against the stock bell, no cold geometry).
//   Pushed EVERY FRAME while attached - deliberately not push-on-change: the
//   client re-instantiates the visual's mesh list after an AddMesh, which
//   swallowed change-tracked pushes (the first build's dark-bell bug). A fresh
//   visual reinstantiates template materials, which is also why a lost borrow
//   self-heals.
//
// MISSING PIECES ARE INERT, NEVER BROKEN: a family glows iff its group exists
// in the mesh AND that group has a material AND the vessel has thrusters of
// that family. The dialog caption is the readout ("bell: MAIN+HOVER" / "no
// bell mesh ..."), so an unconfigured hull can never look broken.
// ============================================================================

#include "OroModule.h"
#include "OroState.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

namespace {

	enum { BELL_MAIN = 0, BELL_HOVER, BELL_RETRO, BELL_USER, BELL_NFAM };
	const char* FAM_NAME[BELL_NFAM] = { "MAIN", "HOVER", "RETRO", "USER" };

	// Thermal timescales are USER SLIDERS now (2026-08-09, his ask):
	//   Heat time = seconds to full glow at 100% thrust -> tau = t/3 (95% at 3 tau),
	//     with the RATE also scaled by throttle (fewer watts into the same metal).
	//   Cool time = seconds from full glow to sub-visible (T ~0.3) -> the -T^4
	//     constant K = ((1/0.3)^3 - 1) / (3 t) ~= 12/t, so the fade SHAPE (fast
	//     off white heat, long dull-red ember tail) is preserved at any length.
	// The defaults reproduce the confirmed first-build feel (tau 3 / K 0.29).

	// Emissive overdrive: the blackbody ramp tops out ~1.45, and at 1.0-ish the
	// shell reads as PAINT. Pushed past the fp16 chain's bloom threshold the
	// client halos it, and the halo is what reads as incandescence. Trim 2 on
	// the slider doubles this again for the truly nuclear look.
	const float BELL_EMIS_GAIN = 2.2f;

	// The per-class bell configuration (one at a time - the camera target's class).
	// ⚠️⚠️ THIS CACHE IS SESSION-SCOPED AND IT DID NOT USED TO KNOW THAT (fixed 2026-08-12,
	// and it was the reload CTD's THIRD face - an access violation inside D3D9Client.dll
	// at +0x1ecb, with no ORO_crash.log entry because an AV is not a C++ throw and our
	// terminate handler never sees it).
	//
	// `hTmpl` comes from oapiLoadMeshGlobal, and the SDK is explicit about what that means
	// under an external client: "the Orbiter core forwards the mesh data to the client for
	// conversion to a device-specific format", and Orbiter "takes care of deleting globally
	// managed meshes". BOTH owners drop it at the end of a session - the core's global mesh
	// manager and the client's device-side copy. The handle is therefore VALID FOR EXACTLY
	// ONE SESSION.
	//
	// The reload guard below keys on the CLASS NAME alone, so flying the same vessel twice
	// in a row - the single most ordinary thing a user does - skipped the reload and handed
	// AddMesh a dangling handle, which the client then dereferenced while building the
	// visual. It is a textbook use-after-free, which is exactly why it was intermittent:
	// whether the freed memory had been reused yet decided whether the frame survived. His
	// reproduction (add thrust so the bell attaches, let it cool, exit, reload) crashes
	// reliably because the intervening allocation churn makes reuse near-certain.
	//
	// OroBell_Reset() clears it at every session boundary. THE GENERAL RULE, and it
	// generalises past meshes: a file-static that caches a handle OUTLIVES THE THING THE
	// HANDLE REFERS TO unless something resets it - so any static holding a core or client
	// resource needs a session-boundary reset, the way OroLightning_Close() already does
	// for the cloud-map file handle and its tile cache.
	struct BellCfg {
		char       cls[64];          // sanitised class leaf this holds ("" = none)
		bool       tried;            // true once we looked (negative cache)
		MESHHANDLE hTmpl;            // global template - SESSION-SCOPED, see above
		int        grp[BELL_NFAM];   // group index per family, -1 = absent
		int        mat[BELL_NFAM];   // material index per family (0-based), -1 = absent
		MATERIAL   base[BELL_NFAM];  // the template's original materials (the borrow base)
		char       info[64];         // dialog readout
	};
	BellCfg  s_cfg = {};
	float    s_T[BELL_NFAM] = {};        // thermal state per family (0..1). Survives
	                                     // pill/trim toggles and re-attaches BY DESIGN
	                                     // (the eclipse precedent: state tracks even
	                                     // while the effect is off); reset only on a
	                                     // class change - different hull, different metal.

	inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
	inline float sstepf(float a, float b, float x)
	{
		float t = clampf((x - a) / (b - a), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	// Blackbody-ish emissive ramp over the normalized glow temperature: nothing
	// visible below ~0.30, dull red, orange, and past 1.0-ish values the fp16
	// chain blooms toward white on its own.
	void BellColour(float T, float& r, float& g, float& b)
	{
		static const float key[6][4] = {
			{ 0.30f, 0.28f, 0.010f, 0.000f },
			{ 0.45f, 0.55f, 0.060f, 0.010f },
			{ 0.60f, 0.85f, 0.180f, 0.030f },
			{ 0.75f, 1.05f, 0.420f, 0.100f },
			{ 0.90f, 1.25f, 0.750f, 0.300f },
			{ 1.00f, 1.45f, 1.050f, 0.550f },
		};
		if (T <= key[0][0]) { float k = clampf(T / key[0][0], 0.0f, 1.0f); r = key[0][1] * k; g = key[0][2] * k; b = key[0][3] * k; return; }
		for (int i = 0; i < 5; i++) {
			if (T <= key[i + 1][0]) {
				const float t = (T - key[i][0]) / (key[i + 1][0] - key[i][0]);
				r = key[i][1] + (key[i + 1][1] - key[i][1]) * t;
				g = key[i][2] + (key[i + 1][2] - key[i][2]) * t;
				b = key[i][3] + (key[i + 1][3] - key[i][3]) * t;
				return;
			}
		}
		r = key[5][1]; g = key[5][2]; b = key[5][3];
	}

	// --- the .msh text parser ----------------------------------------------
	// Only three keywords matter: LABEL/MATERIAL between group headers, GEOM as
	// the group commit, and the MATERIALS name list for validation. Vertex and
	// triangle data lines start with numbers, so keyword scanning cannot desync.
	bool ParseBellMsh(const char* path, BellCfg& c)
	{
		FILE* f = fopen(path, "rb");
		if (!f) return false;
		fseek(f, 0, SEEK_END);
		const long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (sz <= 0 || sz > 32 * 1024 * 1024) { fclose(f); return false; }
		char* buf = new char[sz + 1];
		fread(buf, 1, sz, f);
		buf[sz] = 0;
		fclose(f);

		for (int k = 0; k < BELL_NFAM; k++) { c.grp[k] = -1; c.mat[k] = -1; }

		char matNames[64][32]; int nMatNames = 0;
		int  pendMat = -1;                   // 1-based, from the group's MATERIAL line
		char pendLabel[32] = "";
		int  grpIdx = 0;
		int  readNames = 0;                  // >0: consuming the MATERIALS name list

		char* ctx = NULL;
		for (char* line = strtok_s(buf, "\r\n", &ctx); line; line = strtok_s(NULL, "\r\n", &ctx)) {
			while (*line == ' ' || *line == '\t') line++;
			if (!*line || *line == ';') continue;

			if (readNames > 0) {             // the MATERIALS name list, in order
				if (nMatNames < 64) {
					strncpy_s(matNames[nMatNames], line, _TRUNCATE);
					char* e = matNames[nMatNames] + strlen(matNames[nMatNames]);
					while (e > matNames[nMatNames] && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
					nMatNames++;
				}
				readNames--;
				continue;
			}

			if (_strnicmp(line, "LABEL", 5) == 0 && (line[5] == ' ' || line[5] == '\t')) {
				const char* p = line + 6;
				while (*p == ' ' || *p == '\t') p++;
				strncpy_s(pendLabel, p, _TRUNCATE);
				char* e = pendLabel + strlen(pendLabel);
				while (e > pendLabel && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
			}
			else if (_strnicmp(line, "MATERIALS", 9) == 0) {
				readNames = atoi(line + 9);  // the name list follows
			}
			else if (_strnicmp(line, "MATERIAL", 8) == 0 && (line[8] == ' ' || line[8] == '\t')) {
				pendMat = atoi(line + 9);    // 1-based; 0 = none
			}
			else if (_strnicmp(line, "GEOM", 4) == 0) {
				// Commit this group: match the pending label to a family.
				for (int k = 0; k < BELL_NFAM; k++) {
					if (_stricmp(pendLabel, FAM_NAME[k]) == 0) {
						c.grp[k] = grpIdx;
						c.mat[k] = pendMat - 1;          // -1 when the line was absent/0
					}
				}
				grpIdx++;
				pendLabel[0] = 0;
				pendMat = -1;
			}
		}
		delete[] buf;

		// Validation warnings (log once - the caption carries the summary).
		for (int k = 0; k < BELL_NFAM; k++) {
			if (c.grp[k] < 0) continue;
			if (c.mat[k] < 0) {
				oapiWriteLogV("ORO bell: group %s has no MATERIAL line - family disabled.", FAM_NAME[k]);
				c.grp[k] = -1;
				continue;
			}
			if (c.mat[k] < nMatNames && _stricmp(matNames[c.mat[k]], FAM_NAME[k]) != 0)
				oapiWriteLogV("ORO bell: group %s uses material '%s' (expected '%s') - driving it anyway.",
				              FAM_NAME[k], matNames[c.mat[k]], FAM_NAME[k]);
			for (int j = 0; j < k; j++)
				if (c.grp[j] >= 0 && c.mat[j] == c.mat[k])
					oapiWriteLogV("ORO bell: %s and %s share material %d - it will follow the hotter of the two.",
					              FAM_NAME[j], FAM_NAME[k], c.mat[k] + 1);
		}
		return true;
	}

	// USER family: the strongest thruster in NO standard group - covers custom
	// engine groups without lighting on RCS pulses. Provisional wiring, on
	// record as such; only runs when a USER group exists in the mesh.
	double BellUserLevel(VESSEL* v)
	{
		static const THGROUP_TYPE stdGrp[] = {
			THGROUP_MAIN, THGROUP_RETRO, THGROUP_HOVER,
			THGROUP_ATT_PITCHUP, THGROUP_ATT_PITCHDOWN, THGROUP_ATT_YAWLEFT,
			THGROUP_ATT_YAWRIGHT, THGROUP_ATT_BANKLEFT, THGROUP_ATT_BANKRIGHT,
			THGROUP_ATT_RIGHT, THGROUP_ATT_LEFT, THGROUP_ATT_UP, THGROUP_ATT_DOWN,
			THGROUP_ATT_FORWARD, THGROUP_ATT_BACK
		};
		THRUSTER_HANDLE inStd[256];
		int nStd = 0;
		for (int g = 0; g < (int)(sizeof(stdGrp) / sizeof(stdGrp[0])); g++) {
			const DWORD n = v->GetGroupThrusterCount(stdGrp[g]);
			for (DWORD i = 0; i < n && nStd < 256; i++) inStd[nStd++] = v->GetGroupThruster(stdGrp[g], i);
		}
		double best = 0.0;
		const DWORD n = v->GetThrusterCount();
		for (DWORD i = 0; i < n; i++) {
			THRUSTER_HANDLE th = v->GetThrusterHandleByIndex(i);
			bool std = false;
			for (int k = 0; k < nStd; k++) if (inStd[k] == th) { std = true; break; }
			if (std) continue;
			const double l = v->GetThrusterLevel(th);
			if (l > best) best = l;
		}
		return best;
	}
}

// ----------------------------------------------------------------------------
// The per-step update: config follows the CAMERA TARGET's class, the mesh
// follows the camera target itself, the thermal model follows its throttles.
// ----------------------------------------------------------------------------
void OroModule::UpdateBellGlow(double simdt)
{
	const float trim = clampf(g_fx.plumeBellGlow, 0.0f, 2.0f);

	// The vessel we're LOOKING at (the plume model's resolution rule).
	OBJHANDLE hObj = oapiCameraTarget();
	if (!hObj || oapiGetObjectType(hObj) != OBJTP_VESSEL) hObj = oapiGetFocusObject();
	if (!hObj || oapiGetObjectType(hObj) != OBJTP_VESSEL) { ReleaseBellGlow(); return; }
	VESSEL* v = oapiGetVesselInterface(hObj);
	if (!v) { ReleaseBellGlow(); return; }

	// The bell config follows this vessel's CLASS (sanitised leaf, the same
	// name the class cfg and the heatshield override use).
	char leaf[64] = "";
	const char* cls = v->GetClassName();
	if (cls && cls[0]) OroClassFileName(cls, leaf, sizeof(leaf));
	if (!leaf[0]) { ReleaseBellGlow(); strcpy_s(g_fx.plumeBellInfo, "no vessel class"); return; }

	if (_stricmp(leaf, s_cfg.cls) != 0) {
		// Class change: hand the old vessel back, look for the new class's bell.
		ReleaseBellGlow();
		memset(&s_cfg, 0, sizeof(s_cfg));
		strcpy_s(s_cfg.cls, leaf);
		s_cfg.tried = true;
		char path[MAX_PATH], name[96];
		sprintf_s(path, "Meshes\\ORO\\%s_bell.msh", leaf);
		sprintf_s(name, "ORO\\%s_bell", leaf);
		if (ParseBellMsh(path, s_cfg)) {
			bool any = false;
			for (int k = 0; k < BELL_NFAM; k++) if (s_cfg.grp[k] >= 0) any = true;
			if (any) {
				s_cfg.hTmpl = oapiLoadMeshGlobal(name);
				if (s_cfg.hTmpl) {
					// THE ALPHA GATE (found 2026-08-09, his stuck-opaque report + the
					// diag run: setmat ret 0 every frame, bell opaque regardless).
					// For TEXTURED groups the client substitutes 1.0 for the material
					// alpha unless the mesh's bModulateMatAlpha flag is set
					// (Mesh.cpp: `if (bModulateMatAlpha || bTextured==false)
					// SetFloat(eMtrlAlpha, mat->Diffuse.w) else SetFloat(..., 1.0f)`),
					// and it initialises FALSE. One documented call flips it.
					oapiSetMeshProperty(s_cfg.hTmpl, MESHPROPERTY_MODULATEMATALPHA, 1);
					// The borrow base: the template's original materials.
					char fam[40] = "";
					for (int k = 0; k < BELL_NFAM; k++) {
						if (s_cfg.grp[k] < 0) continue;
						MATERIAL* m = oapiMeshMaterial(s_cfg.hTmpl, (DWORD)s_cfg.mat[k]);
						if (m) s_cfg.base[k] = *m;
						else   { s_cfg.grp[k] = -1; continue; }
						if (fam[0]) strcat_s(fam, "+");
						strcat_s(fam, FAM_NAME[k]);
					}
					sprintf_s(s_cfg.info, "bell: %s", fam[0] ? fam : "no usable groups");
					oapiWriteLogV("ORO bell: %s -> %s.", name, s_cfg.info);
				} else {
					sprintf_s(s_cfg.info, "bell mesh failed to load");
					oapiWriteLogV("ORO bell: oapiLoadMeshGlobal failed for %s.", name);
				}
			} else {
				sprintf_s(s_cfg.info, "bell mesh: no MAIN/HOVER/RETRO/USER labels");
				oapiWriteLogV("ORO bell: %s has no labelled groups - inert.", path);
			}
		} else {
			sprintf_s(s_cfg.info, "no bell mesh (Meshes\\ORO\\%s_bell.msh)", leaf);
		}
		for (int k = 0; k < BELL_NFAM; k++) s_T[k] = 0.0f;
	}
	strcpy_s(g_fx.plumeBellInfo, s_cfg.info);
	if (!s_cfg.hTmpl) { ReleaseBellGlow(); return; }   // no bell for this class

	// --- the thermal model, per family (SIM time - physical object) --------
	// RUNS UNCONDITIONALLY - before every effect gate (the eclipse precedent:
	// the state must keep tracking while the effect is off, or enabling the
	// glow mid-burn would show a cold bell that never physically existed).
	// The slider-derived constants (see the note at the top of the file);
	// coolK is calibrated so the glow is ENTIRELY gone - past the alpha ramp's
	// foot at T = 0.26, not merely past a "visibility threshold" - in Cool
	// time seconds ("time to lose ALL the glow", his spec verbatim).
	const double tauUp = clampf(g_fx.plumeBellHeatT, 1.0f, 20.0f) / 3.0;
	const double coolK = 18.56 / clampf(g_fx.plumeBellCoolT, 5.0f, 120.0f);
	for (int k = 0; k < BELL_NFAM; k++) {
		if (s_cfg.grp[k] < 0) continue;
		double lvl = 0.0;
		switch (k) {
		case BELL_MAIN:  lvl = v->GetThrusterGroupLevel(THGROUP_MAIN);  break;
		case BELL_HOVER: lvl = v->GetThrusterGroupLevel(THGROUP_HOVER); break;
		case BELL_RETRO: lvl = v->GetThrusterGroupLevel(THGROUP_RETRO); break;
		case BELL_USER:  lvl = BellUserLevel(v);                        break;
		}
		double T = (double)s_T[k];
		const double Teq = pow(lvl, 0.25);       // radiative equilibrium: throttle^(1/4) -
		                                         // 10% thrust glows at ~56%, never "full"
		if (Teq > T) {
			// Heating RATE scales with throttle too - fewer watts, same metal.
			const double rate = 0.25 + 0.75 * lvl;
			T += (Teq - T) * (1.0 - exp(-simdt * rate / tauUp));
		} else {
			// dT/dt = -K T^4, closed form: stable at any warp, and the ember
			// tail (the long dull-red fade) falls out of the physics.
			T = T * pow(1.0 + 3.0 * coolK * T * T * T * simdt, -1.0 / 3.0);
			if (T < 0.03) T = 0.0;               // fully cold - stop the asymptote
		}
		s_T[k] = (float)T;
	}

	const bool want = g_fx.masterArmed && g_fx.plumeBellOn && trim > 0.001f;
	if (!want) { ReleaseBellGlow(); return; }

	// ⚠️ INVARIANT 23(k), AND THIS SITE WAS MISSED TOO (2026-08-12). Vessel::AddMesh
	// grows the vessel's mesh list with a new[] and makes the client re-instantiate the
	// visual - a long-lived object handed to a half-built world on a scenario reload,
	// the same class of mistake as the exhaust streams. Note this gate sits AFTER the
	// thermal model above, which is correct and deliberate: invariant 23(f) says the
	// metal is hot whether or not we are drawing it, so the temperature must keep
	// integrating through the load window. What waits is the LENDING, nothing else.
	if (!sceneRendered) {
		if (!lendDeferLogged) {
			lendDeferLogged = true;
			oapiWriteLogV("ORO: deferring AddMesh (bell glow shell) - scene has not rendered "
			              "a frame yet (invariant 23k). Will attach once it has.");
		}
		return;
	}

	// Attach our shell to the target (a borrow - ReleaseBellGlow returns it).
	// The thermal state deliberately survives the attach: the metal was hot
	// whether or not we were drawing it.
	if (bellVessel != hObj) {
		ReleaseBellGlow();
		bellVessel  = hObj;
		bellMeshIdx = v->AddMesh(s_cfg.hTmpl);
		v->SetMeshVisibilityMode(bellMeshIdx, MESHVIS_EXTERNAL);
	}

	// --- the material driver: EVERY FRAME while attached -------------------
	// Deliberately NOT push-on-change (the first build's bug): AddMesh makes the
	// client re-instantiate the visual's mesh list a little later, so early
	// change-tracked pushes landed on an instance that was then REPLACED - and
	// once the temperature pinned at equilibrium nothing changed, no push ever
	// reached the live mesh, and the bell sat dark until a slider drag forced
	// one. Four material writes per frame are trivially cheap and immune to
	// every instancing/timing case by construction.
	VISHANDLE* pvis = oapiObjectVisualPtr(hObj);
	VISHANDLE  vis  = pvis ? *pvis : NULL;
	if (!vis) return;                              // out of visual range: nothing rendered

	DEVMESHHANDLE hDM = v->GetDevMesh(vis, bellMeshIdx);
	if (!hDM) return;
	// Per-INSTANCE alpha gate too, every frame like the material pushes (the
	// instance is rebuilt behind our back; one bool set is free and cannot miss).
	oapiSetMeshProperty(hDM, MESHPROPERTY_MODULATEMATALPHA, 1);

	for (int k = 0; k < BELL_NFAM; k++) {
		if (s_cfg.grp[k] < 0) continue;
		// Shared-material rule: it follows the hotter family.
		float T = s_T[k];
		for (int j = 0; j < BELL_NFAM; j++)
			if (j != k && s_cfg.grp[j] >= 0 && s_cfg.mat[j] == s_cfg.mat[k] && s_T[j] > T) T = s_T[j];

		// INCANDESCENT, NOT PAINTED (his "looks fake" report, 2026-08-09): the
		// first driver kept the author's white diffuse, so the SUN lit the shell
		// like painted metal and the emissive was a tint on daylight. Hot metal
		// is its own light source: kill every response to external light -
		// diffuse, ambient, specular all black - so the emissive term is the
		// ONLY content (the texture still multiplies it: bands glow, streaks
		// stay dark), and DRIVE IT PAST 1.0 so the fp16 chain's threshold bloom
		// halos it. The bloom IS the glow.
		MATERIAL m = s_cfg.base[k];
		m.diffuse.r  = m.diffuse.g  = m.diffuse.b  = 0.0f;
		m.ambient.r  = m.ambient.g  = m.ambient.b  = 0.0f;
		m.specular.r = m.specular.g = m.specular.b = 0.0f;
		float r, g, b;
		BellColour(T, r, g, b);
		m.emissive.r = r * trim * BELL_EMIS_GAIN;
		m.emissive.g = g * trim * BELL_EMIS_GAIN;
		m.emissive.b = b * trim * BELL_EMIS_GAIN;
		m.emissive.a = 1.0f;
		// The thermal fade owns visibility: a cold shell renders NOTHING.
		m.diffuse.a  = s_cfg.base[k].diffuse.a * sstepf(0.26f, 0.45f, T);
		oapiSetMaterial(hDM, (DWORD)s_cfg.mat[k], &m);
	}
}

// The hand-it-back path: remove our shell from whoever carries it. Safe to call
// twice; oapiIsVessel guards teardown-order races (the reentry table's rule).
// A visual that died meanwhile needs nothing - it rebuilds from the template.
void OroModule::ReleaseBellGlow()
{
	if (bellVessel && oapiIsVessel(bellVessel) && bellMeshIdx != (UINT)-1) {
		VESSEL* v = oapiGetVesselInterface(bellVessel);
		if (v) v->DelMesh(bellMeshIdx);
	}
	bellVessel  = NULL;
	bellMeshIdx = (UINT)-1;
}

// ⚠️ SESSION-BOUNDARY RESET FOR THE TEMPLATE CACHE - the fix for the reload CTD's
// access-violation face (2026-08-12). See the long note on BellCfg: `hTmpl` is an
// oapiLoadMeshGlobal handle, both of whose owners (the core's global mesh manager and the
// client's device-side copy) drop it when a session ends, while `s_cfg` is a file-static
// that does not. Zeroing the whole struct is deliberate rather than just NULLing hTmpl: it
// also clears `cls`, which is what the reload guard compares, so the next session re-parses
// the .msh and re-fetches a FRESH handle even when the very same vessel class is flown
// again - which is the ordinary case that crashed.
//
// Called from clbkSimulationStart, which is the authoritative place: it does not depend on
// the previous session having shut down cleanly, so a crash or a forced exit cannot leave a
// stale handle armed for the next run. Also called at session end so a dangling handle never
// simply sits in memory waiting for someone to use it.
//
// The THERMAL state deliberately survives (invariant 23f: the metal is hot whether or not
// we are drawing it) - but only within a session; a new session is new metal.
void OroBell_Reset()
{
	memset(&s_cfg, 0, sizeof(s_cfg));
	for (int k = 0; k < BELL_NFAM; k++) s_T[k] = 0.0f;
}
