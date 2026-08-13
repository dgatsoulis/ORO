// ==============================================================
// OroModule.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

#include "OroModule.h"
#include "OroState.h"
#include "OroDialog.h"
#include <math.h>   // sinf/cosf/sqrtf for the tunnel ring geometry
#include <stdio.h>  // sprintf_s for the per-scenario sound paths
#include <stdlib.h>    // _set_invalid_parameter_handler (the crash forensics below)
#include <exception>   // std::set_terminate

// D3D9Client image-processing interface. Included AFTER Orbitersdk.h so the Windows
// min/max macros it relies on are in scope. gcGetCoreInterface() binds to
// D3D9Client.dll at runtime and returns NULL if D3D9Client is not the active client.
#include "gcCoreAPI.h"

// Doug Beachy's XRSound 2.0 module sound API (ships with Orbiter 2024). Self-contained
// header; the import lib is XRSound.lib. All calls no-op if XRSound.dll is absent.
#include "XRSound.h"

// The shared effect state (see OroState.h). Written by the dialog, read by the
// render callback - same thread by construction, no locking.
OroEffectState g_fx;

// Defined with the crash forensics near the bottom of this file, but called from the
// session callbacks well above them - hence the forward declaration.
namespace { void OroLogMemory(const char* when); }

// XRSound sound IDs for this module - must be unique and < 10000 (XRSound reserves
// 10000+ for its own default sounds). Heartbeat is one id; each INDUCE/RECOVER
// scenario gets SND_SCEN_BASE + its index (see INDUCE_SEQ[]).
enum { SND_HEARTBEAT = 1, SND_SCEN_BASE = 10 };

// ----------------------------------------------------------------------------
// INDUCE / RECOVER scenarios - scripted effect timelines. Each key is a time
// (seconds) + a full snapshot of the 11 driven effect values; the player (in
// clbkPreStep) lerps between keys and writes the result into g_fx every frame.
//   INDUCE scenarios (hold=true) RAMP UP ONLY, then HOLD their final state - you
//     stay blacked-out / greyed / red-out until you recover (no auto-return).
//   RECOVER scenarios (hold=false) start AT the matching induce peak and ramp
//     back to all-zero, then release.
// Each scenario carries a sound clip (played for its duration when the section's
// Sound toggle is on) + involuntary blink times. INDUCE_SEQ[] order MUST match
// the dialog buttons: induce 0=G-LOC,1=Grey,2=Red; recover 3=G-LOC,4=Grey,5=Red
// (recover buttons map to NIND + i).
// ----------------------------------------------------------------------------
namespace {
	enum { FX_BLK, FX_RED, FX_TUN, FX_SPT, FX_GRY, FX_BLR, FX_HB, FX_AB, FX_SPK, FX_SWM, FX_TLT, FX_N };
	struct SeqKey { float t; float v[FX_N]; };

	//                                          blk    red    tun    spt    grey   blur   hb     ab     spk    swim   tilt
	const SeqKey SEQ_GLOC[] = {   // +Gz to lights-out and back
		{  0.0f, { 0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0     } },
		{  2.5f, { 0,     0,     0,     0,     0.12f, 0.18f, 0.45f, 0.10f, 0,     0,     0     } },
		{  5.5f, { 0,     0,     0.25f, 0.10f, 0.45f, 0.32f, 0.70f, 0.25f, 0.20f, 0.18f, 0.15f } },
		{  8.0f, { 0,     0,     0.55f, 0.30f, 0.72f, 0.50f, 1.00f, 0.45f, 0.45f, 0.40f, 0.30f } },
		{ 10.5f, { 0.45f, 0,     0.85f, 0.40f, 0.90f, 0.68f, 1.00f, 0.55f, 0.65f, 0.60f, 0.45f } },
		{ 12.0f, { 1.00f, 0,     1.00f, 0.30f, 0.95f, 0.75f, 0.80f, 0.55f, 0.30f, 0.55f, 0.40f } },  // full G-LOC - HELD (no auto-return)
	};
	const SeqKey SEQ_GREY[] = {   // +Gz near-miss: RAMP UP to grey-out and HOLD (never blacks out)
		{  0.0f, { 0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0     } },
		{  2.5f, { 0,     0,     0,     0,     0.18f, 0.15f, 0.45f, 0.10f, 0,     0,     0     } },
		{  5.0f, { 0,     0,     0.30f, 0.10f, 0.50f, 0.30f, 0.70f, 0.22f, 0.20f, 0.15f, 0.15f } },
		{  7.0f, { 0,     0,     0.50f, 0.20f, 0.70f, 0.40f, 0.90f, 0.30f, 0.35f, 0.28f, 0.25f } },  // grey-out - HELD
	};
	const SeqKey SEQ_RED[] = {    // -Gz red-out: RAMP UP to red-out and HOLD
		{  0.0f, { 0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0     } },
		{  2.0f, { 0,     0.30f, 0,     0,     0,     0.10f, 0.50f, 0,     0,     0,     0     } },
		{  4.0f, { 0,     0.60f, 0,     0.10f, 0,     0.20f, 0.80f, 0.15f, 0,     0.10f, 0.06f } },
		{  6.0f, { 0,     0.85f, 0,     0.20f, 0,     0.35f, 1.00f, 0.20f, 0,     0.20f, 0.10f } },  // red-out - HELD
	};

	//                                          blk    red    tun    spt    grey   blur   hb     ab     spk    swim   tilt
	const SeqKey SEQ_RGLOC[] = {  // RECOVER from G-LOC: start blacked-out, come to (woozy)
		{  0.0f, { 1.00f, 0,     1.00f, 0.30f, 0.95f, 0.75f, 0.80f, 0.55f, 0.30f, 0.55f, 0.40f } },  // = induce G-LOC peak
		{  1.5f, { 0.85f, 0,     0.90f, 0.10f, 0.80f, 0.55f, 0.50f, 0.40f, 0.30f, 0.45f, 0.55f } },
		{  3.5f, { 0.45f, 0,     0.60f, 0.20f, 0.60f, 0.45f, 0.75f, 0.35f, 0.45f, 0.50f, 0.55f } },
		{  5.5f, { 0.20f, 0,     0.35f, 0.10f, 0.35f, 0.30f, 0.70f, 0.20f, 0.25f, 0.35f, 0.35f } },
		{  7.5f, { 0.05f, 0,     0.12f, 0,     0.15f, 0.15f, 0.55f, 0.08f, 0.10f, 0.15f, 0.15f } },
		{  9.0f, { 0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0     } },
	};
	const SeqKey SEQ_RGREY[] = { // RECOVER from grey-out: start greyed, clear back (never was black)
		{  0.0f, { 0,     0,     0.50f, 0.20f, 0.70f, 0.40f, 0.90f, 0.30f, 0.35f, 0.28f, 0.25f } },  // = induce grey-out peak
		{  2.0f, { 0,     0,     0.35f, 0.12f, 0.50f, 0.30f, 0.80f, 0.20f, 0.20f, 0.20f, 0.18f } },
		{  4.0f, { 0,     0,     0.18f, 0.05f, 0.28f, 0.16f, 0.62f, 0.10f, 0.10f, 0.10f, 0.10f } },
		{  6.0f, { 0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0     } },
	};
	const SeqKey SEQ_RRED[] = {   // RECOVER from red-out: start deep red, fade out
		{  0.0f, { 0,     0.85f, 0,     0.20f, 0,     0.35f, 1.00f, 0.20f, 0,     0.20f, 0.10f } },
		{  2.0f, { 0,     0.55f, 0,     0.10f, 0,     0.22f, 0.80f, 0.12f, 0,     0.12f, 0.08f } },
		{  4.0f, { 0,     0.25f, 0,     0,     0,     0.12f, 0.55f, 0,     0,     0,     0.04f } },
		{  6.0f, { 0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0     } },
	};

	// Blink trigger times (seconds into the scenario): involuntary stress blinks and the
	// eyes fluttering open on recovery. Fired as blinkRequest when seqT crosses each.
	const float BLINK_GLOC[]  = { 6.0f, 9.0f, 11.0f };
	const float BLINK_GREY[]  = { 4.0f, 6.5f };
	const float BLINK_RED[]   = { 3.0f, 5.5f };
	const float BLINK_RGLOC[] = { 1.2f, 2.8f, 4.2f, 5.6f };
	const float BLINK_RGREY[] = { 1.0f, 3.0f };
	const float BLINK_RRED[]  = { 1.0f, 3.0f };

	struct Scenario { const SeqKey* keys; int n; float dur; const float* blinks; int nblinks; bool hold; const char* wav; };
	#define SEQ(a, d, b, h, w) { a, (int)(sizeof(a)/sizeof(SeqKey)), d, b, (int)(sizeof(b)/sizeof(float)), h, w }
	const Scenario INDUCE_SEQ[] = {
		SEQ(SEQ_GLOC,  12.0f, BLINK_GLOC,  true,  "Induce_gloc.wav"),   // 0  induce  G-LOC       (hold)
		SEQ(SEQ_GREY,   7.0f, BLINK_GREY,  true,  "Induce_grey.wav"),   // 1  induce  Grey-out    (hold)
		SEQ(SEQ_RED,    6.0f, BLINK_RED,   true,  "Induce_red.wav"),    // 2  induce  Red-out     (hold)
		SEQ(SEQ_RGLOC,  9.0f, BLINK_RGLOC, false, "Recover_gloc.wav"),  // 3  recover from G-LOC
		SEQ(SEQ_RGREY,  6.0f, BLINK_RGREY, false, "Recover_grey.wav"),  // 4  recover from Grey-out
		SEQ(SEQ_RRED,   6.0f, BLINK_RRED,  false, "Recover_red.wav"),   // 5  recover from Red-out
	};
	#undef SEQ
	const int NSCEN = (int)(sizeof(INDUCE_SEQ) / sizeof(Scenario));

	// Sample a timeline at time t into out[FX_N] (piecewise-linear between keys).
	void SeqSample(const SeqKey* k, int n, float t, float out[FX_N])
	{
		if (t <= k[0].t)     { for (int j = 0; j < FX_N; j++) out[j] = k[0].v[j];     return; }
		if (t >= k[n-1].t)   { for (int j = 0; j < FX_N; j++) out[j] = k[n-1].v[j];   return; }
		int i = 1; while (i < n && t > k[i].t) i++;
		const float u = (t - k[i-1].t) / (k[i].t - k[i-1].t);
		for (int j = 0; j < FX_N; j++) out[j] = k[i-1].v[j] + (k[i].v[j] - k[i-1].v[j]) * u;
	}
}

// The plasma poly update + draw, shared by the render proc's EXTERNAL branch and
// the VC path (round 3.5). Always updates the FULL buffer: the client's
// D3D9Triangle draws its CREATION count unconditionally and its Update() Locks
// with D3DLOCK_DISCARD - fresh UNINITIALIZED VRAM on every lock - so an unwritten
// tail is random flashing triangles (the green-flash hunt, 2026-08-01). Invariant
// 3's dark-spots rule; UpdateReentry zero-pads plasVtx past plasVtxN to make the
// full-count update safe.
void OroModule::DrawPlasmaPoly(oapi::Sketchpad* pSkp, bool depthClip)
{
	if (plasVtxN <= 0 || !pCore) return;
	if (!hPlasmaPoly) {
		static gcCore::clrVtx zero[PLAS_MAX_TRI * 3];   // zero-init: degenerate, alpha 0
		hPlasmaPoly = pCore->CreateTriangles(NULL, zero, PLAS_MAX_TRI * 3, PF_TRIANGLES);
	}
	if (!hPlasmaPoly) return;

	// Depth path (patch g), the aurora's route exactly: CreateTrianglesDepth threads the
	// per-vertex camera distance into the poly and the 0x100 blend bit asks the client's
	// Sketchpad to drop fragments the scene occludes. PLAS_MAX_TRI * 3 = 49152 vertices,
	// under the 65535 stream ceiling that CTD'd the aurora (invariant 19d) - but the
	// NEXT doubling is not; split across HPOLYs before growing the pool again.
	DWORD blend = padAdditive ? 0x5 : (DWORD)oapi::Sketchpad::BlendState::ALPHABLEND;
	if (depthClip && depthClipOK) {
		pCore->CreateTrianglesDepth(hPlasmaPoly, (const gcCore::clrVtx*)plasVtx, plasDepth, PLAS_MAX_TRI * 3, PF_TRIANGLES);
		blend |= 0x100;
	} else {
		pCore->CreateTriangles(hPlasmaPoly, (const gcCore::clrVtx*)plasVtx, PLAS_MAX_TRI * 3, PF_TRIANGLES);
	}
	pSkp->SetBlendState((oapi::Sketchpad::BlendState)blend);
	pSkp->DrawPoly(hPlasmaPoly);
	pSkp->SetBlendState(oapi::Sketchpad::BlendState::ALPHABLEND);        // leave the pad as found
}

// The aurora poly update + draw - a near-exact copy of DrawPlasmaPoly (the curtains ARE
// a ribbon, and this is the same additive Sketchpad triangle list). Same full-buffer
// rule: the client draws its CREATION count and Locks with D3DLOCK_DISCARD, so
// UpdateAurora zero-pads aurVtx past aurVtxN and we hand over the whole buffer. Additive
// where patch (d) is live (padAdditive, shared with the plasma), alpha-blend otherwise.
void OroModule::DrawAuroraPoly(oapi::Sketchpad* pSkp)
{
	if (aurVtxN <= 0 || !pCore) return;
	if (!hAuroraPoly) {
		static gcCore::clrVtx zero[AUR_MAX_TRI * 3];    // zero-init: degenerate, alpha 0
		hAuroraPoly = pCore->CreateTriangles(NULL, zero, AUR_MAX_TRI * 3, PF_TRIANGLES);
	}
	if (!hAuroraPoly) return;

	// Depth path (patch g): CreateTrianglesDepth threads the per-vertex camera depth into
	// the poly, and the 0x100 blend bit tells the client's Sketchpad to clip fragments the
	// scene occludes (the no-SDK-header trick, same as patch d's 0x5). Falls back to the
	// plain additive poly when depth-clipping is not live this session.
	DWORD blend = padAdditive ? 0x5 : (DWORD)oapi::Sketchpad::BlendState::ALPHABLEND;
	if (depthClipOK) {
		pCore->CreateTrianglesDepth(hAuroraPoly, (const gcCore::clrVtx*)aurVtx, aurDepth, AUR_MAX_TRI * 3, PF_TRIANGLES);
		blend |= 0x100;
	} else {
		pCore->CreateTriangles(hAuroraPoly, (const gcCore::clrVtx*)aurVtx, AUR_MAX_TRI * 3, PF_TRIANGLES);
	}
	pSkp->SetBlendState((oapi::Sketchpad::BlendState)blend);
	pSkp->DrawPoly(hAuroraPoly);
	pSkp->SetBlendState(oapi::Sketchpad::BlendState::ALPHABLEND);        // leave the pad as found
}

// The LIGHTNING poly - the flash discs in the cloud deck, the aurora's draw path
// verbatim (same full-buffer + zero-pad discipline, same additive / depth-clip /
// ALPHABLEND-fallback chain). Its own small HPOLY (LTG_MAX_TRI 2048 = 6144 verts,
// far under invariant 19d's ceiling).
void OroModule::DrawLightningPoly(oapi::Sketchpad* pSkp)
{
	if (ltgVtxN <= 0 || !pCore) return;
	if (!hLightningPoly) {
		static gcCore::clrVtx zero[LTG_MAX_TRI * 3];    // zero-init: degenerate, alpha 0
		hLightningPoly = pCore->CreateTriangles(NULL, zero, LTG_MAX_TRI * 3, PF_TRIANGLES);
	}
	if (!hLightningPoly) return;

	DWORD blend = padAdditive ? 0x5 : (DWORD)oapi::Sketchpad::BlendState::ALPHABLEND;
	if (ltgTexMode && hLtgAtlas) {
		// TEXTURED path (patch l): the flash IS the cloud image lighting up - the pad's
		// modulate band multiplies the baked atlas by the Gouraud vertices, and the
		// per-vertex depth threads through exactly as in the depth path below.
		pCore->CreateTrianglesTex(hLightningPoly, (const gcCore::texVtx*)ltgTexVtx,
		                          depthClipOK ? ltgDepth : NULL, LTG_MAX_TRI * 3, PF_TRIANGLES, hLtgAtlas);
		if (depthClipOK) blend |= 0x100;
	} else if (depthClipOK) {
		pCore->CreateTrianglesDepth(hLightningPoly, (const gcCore::clrVtx*)ltgVtx, ltgDepth, LTG_MAX_TRI * 3, PF_TRIANGLES);
		blend |= 0x100;
	} else {
		pCore->CreateTriangles(hLightningPoly, (const gcCore::clrVtx*)ltgVtx, LTG_MAX_TRI * 3, PF_TRIANGLES);
	}
	pSkp->SetBlendState((oapi::Sketchpad::BlendState)blend);
	pSkp->DrawPoly(hLightningPoly);
	pSkp->SetBlendState(oapi::Sketchpad::BlendState::ALPHABLEND);        // leave the pad as found
}

// THE VAPOUR CONE poly - and it is the ODD ONE OUT, deliberately. Every other poly in
// ORO draws ADDITIVE (patch d's 0x5) because every other poly is light. Condensed
// water is not: it scatters and it OCCLUDES, so this one draws with the pad's DEFAULT
// ALPHABLEND state and never asks for the additive bit at all. That makes it the first
// live use of the recipe graveyard G11 left behind when the trail's smoke layer died
// with it - "a second poly drawn with the pad's default blend state BEFORE the additive
// one, no client patch involved" - and the BEFORE is why the call site puts it ahead of
// the trail, the plasma and the plume: a cloud has to be laid down before light is added
// over it, or the light it should have hidden shines through.
//
// padAdditive is deliberately NOT consulted here. Same full-buffer + zero-pad discipline
// as its siblings (invariant 3; UpdateVapour pads vapVtx), and the same patch-(g) depth
// clip - which matters more here than anywhere else, because the cone WRAPS the hull and
// its far half is genuinely behind the ship.
void OroModule::DrawVapourPoly(oapi::Sketchpad* pSkp)
{
	if (vapVtxN <= 0 || !pCore) return;
	if (!hVapourPoly) {
		static gcCore::clrVtx zero[VAP_MAX_TRI * 3];    // zero-init: degenerate, alpha 0
		hVapourPoly = pCore->CreateTriangles(NULL, zero, VAP_MAX_TRI * 3, PF_TRIANGLES);
	}
	if (!hVapourPoly) return;

	DWORD blend = (DWORD)oapi::Sketchpad::BlendState::ALPHABLEND;
	if (depthClipOK) {
		pCore->CreateTrianglesDepth(hVapourPoly, (const gcCore::clrVtx*)vapVtx, vapDepth, VAP_MAX_TRI * 3, PF_TRIANGLES);
		blend |= 0x100;
	} else {
		pCore->CreateTriangles(hVapourPoly, (const gcCore::clrVtx*)vapVtx, VAP_MAX_TRI * 3, PF_TRIANGLES);
	}
	pSkp->SetBlendState((oapi::Sketchpad::BlendState)blend);
	pSkp->DrawPoly(hVapourPoly);
	pSkp->SetBlendState(oapi::Sketchpad::BlendState::ALPHABLEND);        // leave the pad as found
}

// The TRAIL poly - the third instance of the same pattern (plasma, aurora, now the
// particle trail's sprites). Its OWN HPOLY, deliberately: the trail and the attached
// plasma never starve each other's triangle budgets, and the phase-2 ablation-smoke
// layer (G11's alpha-blend recipe) gets a sibling slot to land in. Same full-buffer +
// zero-pad discipline (invariant 3; UpdateReentry pads trailVtx), same additive /
// depth-clip / ALPHABLEND-fallback chain as the other two.
void OroModule::DrawTrailPoly(oapi::Sketchpad* pSkp, bool depthClip)
{
	if (trailVtxN <= 0 || !pCore) return;
	if (!hTrailPoly) {
		static gcCore::clrVtx zero[TRAIL_MAX_TRI * 3];  // zero-init: degenerate, alpha 0
		hTrailPoly = pCore->CreateTriangles(NULL, zero, TRAIL_MAX_TRI * 3, PF_TRIANGLES);
	}
	if (!hTrailPoly) return;

	DWORD blend = padAdditive ? 0x5 : (DWORD)oapi::Sketchpad::BlendState::ALPHABLEND;
	if (depthClip && depthClipOK) {
		pCore->CreateTrianglesDepth(hTrailPoly, (const gcCore::clrVtx*)trailVtx, trailDepth, TRAIL_MAX_TRI * 3, PF_TRIANGLES);
		blend |= 0x100;
	} else {
		pCore->CreateTriangles(hTrailPoly, (const gcCore::clrVtx*)trailVtx, TRAIL_MAX_TRI * 3, PF_TRIANGLES);
	}
	pSkp->SetBlendState((oapi::Sketchpad::BlendState)blend);
	pSkp->DrawPoly(hTrailPoly);
	pSkp->SetBlendState(oapi::Sketchpad::BlendState::ALPHABLEND);        // leave the pad as found
}

// The plume-expansion poly (OroPlume.cpp) - the same additive Sketchpad triangle
// list as the plasma/aurora/trail, same full-buffer rule (invariant 3), same
// patch-(g) per-vertex depth. The clip is what cuts a hover plume at the runway
// surface (scene depth includes terrain); without it OroPlume falls back to the
// shimmer's geometric facing fade at build time.
void OroModule::DrawPlumePoly(oapi::Sketchpad* pSkp, bool depthClip)
{
	if (plmVtxN <= 0 || !pCore) return;
	if (!hPlumePoly) {
		static gcCore::clrVtx zero[PLM_MAX_TRI * 3];    // zero-init: degenerate, alpha 0
		hPlumePoly = pCore->CreateTriangles(NULL, zero, PLM_MAX_TRI * 3, PF_TRIANGLES);
	}
	if (!hPlumePoly) return;

	DWORD blend = padAdditive ? 0x5 : (DWORD)oapi::Sketchpad::BlendState::ALPHABLEND;
	if (depthClip && depthClipOK) {
		pCore->CreateTrianglesDepth(hPlumePoly, (const gcCore::clrVtx*)plmVtx, plmDepth, PLM_MAX_TRI * 3, PF_TRIANGLES);
		blend |= 0x100;
	} else {
		pCore->CreateTriangles(hPlumePoly, (const gcCore::clrVtx*)plmVtx, PLM_MAX_TRI * 3, PF_TRIANGLES);
	}
	pSkp->SetBlendState((oapi::Sketchpad::BlendState)blend);
	pSkp->DrawPoly(hPlumePoly);

	// The SOOT layer - plain ALPHA-BLENDED, drawn AFTER the additive glow so the
	// dark wisps genuinely dim it (soot is IN the jet; drawing dark first would
	// let the glow wash straight over it). Same per-vertex depth path; in the
	// patch-(i) fp16 slot the alpha blend scales the accumulated HDR down, which
	// dims what would have bloomed - exactly what soot does to a real plume.
	if (plmDkVtxN > 0) {
		if (!hPlumeDkPoly) {
			static gcCore::clrVtx zero2[PLM_DK_MAX_TRI * 3];   // zero-init: degenerate
			hPlumeDkPoly = pCore->CreateTriangles(NULL, zero2, PLM_DK_MAX_TRI * 3, PF_TRIANGLES);
		}
		if (hPlumeDkPoly) {
			DWORD dkBlend = (DWORD)oapi::Sketchpad::BlendState::ALPHABLEND;
			if (depthClip && depthClipOK) {
				pCore->CreateTrianglesDepth(hPlumeDkPoly, (const gcCore::clrVtx*)plmDkVtx, plmDkDepth, PLM_DK_MAX_TRI * 3, PF_TRIANGLES);
				dkBlend |= 0x100;
			} else {
				pCore->CreateTriangles(hPlumeDkPoly, (const gcCore::clrVtx*)plmDkVtx, PLM_DK_MAX_TRI * 3, PF_TRIANGLES);
			}
			pSkp->SetBlendState((oapi::Sketchpad::BlendState)dkBlend);
			pSkp->DrawPoly(hPlumeDkPoly);
		}
	}
	pSkp->SetBlendState(oapi::Sketchpad::BlendState::ALPHABLEND);        // leave the pad as found
}

// The ECLIPSE resample, shared by the render proc's EXTERNAL branch and its INTERNAL
// stack - the first effect ORO runs in both, and deliberately so: an eclipse is a
// property of the LIGHT, not of who is looking. It self-gates entirely on eclActive
// (set in UpdateEclipse, main thread) so both call sites are one line, and it runs
// FIRST in each: everything after it - the shimmer, the plasma, the whole
// physiological stack - happens to a world whose illumination is already decided.
void OroModule::DrawEclipsePass()
{
	if (!eclActive || !ipiReady || !pCore || !hFrameTex || !pIPIEclipse) return;
	SURFHANDLE hBB = pCore->GetBackBufferHandle();
	if (!hBB || !pCore->CopyResource(hFrameTex, hBB)) return;
	pIPIEclipse->SetTexture("tSrc", hFrameTex, IPF_CLAMP_U | IPF_CLAMP_V | IPF_LINEAR);
	pIPIEclipse->SetOutput(0, hBB);
	pIPIEclipse->SetFloat("fEclGain",  g_fx.eclipseGain);
	pIPIEclipse->SetFloat("fEclDesat", eclDesat);
	// Highlight protection is for INSTRUMENTS, so it only makes sense where there are
	// any. Outside the ship every bright pixel is world content - the sun, a hot plume,
	// a city at night - and all of those SHOULD dim with everything else.
	pIPIEclipse->SetFloat("fEclProt", extGate ? 0.0f : 0.85f);
	pIPIEclipse->Execute((DWORD)0, true, gcIPInterface::Rect);
}

// ----------------------------------------------------------------------------
// GOD RAYS - the same self-gating shape as the eclipse pass above, and called from
// the same two places for the same reason: shafts are light in the WORLD, and the
// world is there whether you look at it from a seat or from outside (invariant 10's
// "both domains" case).
//
// It captures its own copy of the backbuffer rather than reusing the eclipse's,
// because it must run AFTER the eclipse: the eye decides how bright the frame is,
// and the shafts are then added to that frame. Sharing one capture would feed the
// rays the pre-eclipse image and they would not dim with everything else.
// ----------------------------------------------------------------------------
static inline float clampf01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

void OroModule::DrawGodRayPass()
{
	if (!grActive || !ipiReady || !pCore || !hFrameTex || !pIPIGodRay) return;
	SURFHANDLE hBB = pCore->GetBackBufferHandle();
	if (!hBB || !pCore->CopyResource(hFrameTex, hBB)) return;
	pIPIGodRay->SetTexture("tSrc", hFrameTex, IPF_CLAMP_U | IPF_CLAMP_V | IPF_LINEAR);
	pIPIGodRay->SetOutput(0, hBB);

	const float sun[2] = { grSunU, grSunV };
	pIPIGodRay->SetFloat("vGRSun", sun, sizeof(sun));
	pIPIGodRay->SetFloat("fGRStr",    grStr);
	pIPIGodRay->SetFloat("fGRFade",   grFade);
	// REACH -> the fraction of each pixel's distance-to-sun that the march covers. It is
	// what decides whether a pixel far from the disc ever reaches the bright source at
	// all, so it is the real "how long are the shafts" control. Mapped into a USEFUL band
	// rather than passed raw: at a literal 0 the march has no length and the effect
	// vanishes, which is a dead sixth of the slider. The top of the band is just under 1
	// for the same reason the classic algorithm uses ~0.93 - the samples of a very
	// distant pixel are spread thin, and stopping fractionally short keeps them denser.
	pIPIGodRay->SetFloat("fGRLen",    0.25f + 0.70f * clampf01(g_fx.grayLength));
	// The dial is "how long are the rays"; the shader wants a per-sample survival
	// factor, where the interesting range is a narrow band just under 1. Mapping it
	// here keeps the slider linear in the thing the user can see.
	pIPIGodRay->SetFloat("fGRDecay",  0.86f + 0.13f * clampf01(g_fx.grayDecay));
	// SENSITIVITY -> threshold, and note the sign: the dial goes UP as the threshold
	// comes DOWN, so "more" admits more of the frame. 1.0 on the frame's own scale is a
	// fully blown-out pixel, so the usable band is the top half and the dial spans it.
	pIPIGodRay->SetFloat("fGRThresh", 0.90f - 0.45f * clampf01(g_fx.graySens));
	const float tint[3] = { grTintR, grTintG, grTintB };
	pIPIGodRay->SetFloat("vGRTint", tint, sizeof(tint));
	pIPIGodRay->SetFloat("fAspect", (viewH > 0) ? ((float)viewW / (float)viewH) : 1.3333f);
	pIPIGodRay->Execute((DWORD)0, true, gcIPInterface::Rect);
}

// ----------------------------------------------------------------------------
// SETTINGS PERSISTENCE (2026-08-02) - Config\ORO.cfg + Config\ORO\<class>.cfg
//
// Tables drive both directions, so a new knob is one line and can never be
// saved-but-not-loaded. Orbiter's own oapiWriteItem_*/oapiReadItem_* format, in
// Config\ where every other addon keeps its settings.
//
// TWO SCOPES, because settings divide cleanly into two kinds. What the PILOT is
// (G tolerance, posture, cam-shake shape, which effects are enabled) travels with
// the user and belongs in one GLOBAL file. What a VESSEL needs (the plasma tuning,
// the reentry trim, the exhaust shimmer, the CoP shift) is a property of that
// hull's size, shape and engines - tuning the DG and then flying Atlantis with the
// DG's numbers is meaningless. Those live per CLASS, one file each, and are loaded
// automatically when the focus vessel's class changes: tune once per vessel type,
// never again (user's request, 2026-08-02).
//
// One file per class rather than sections in one file, because Orbiter's config
// reader is a flat key=value scanner with no notion of a section - a single file
// would mean hand-rolling a parser to gain nothing.
//
// WHAT IS NOT SAVED AT ALL, and why: the eleven scripted effect CHANNELS
// (blackout, red-out, tunnel, spots, grey-out, blur, heartbeat, aberration,
// sparkles, swim, tilt). Those are not settings, they are the current STATE of the
// eye - the INDUCE/RECOVER player overwrites them every frame (invariant 8), so
// saving mid scenario would persist a demo pose and the next session would start
// blacked out with no obvious cause. Their enable pills ARE saved.
// ----------------------------------------------------------------------------
namespace {
	enum SetType { ST_F, ST_B, ST_I };
	struct SetItem { const char* key; void* p; SetType t; };

	// char* rather than const char* on the keys: oapiReadItem_*/oapiWriteItem_*
	// take a non-const char*, and a string literal cannot bind to that.
	static char* K(const char* s) { return const_cast<char*>(s); }

	// --- GLOBAL: the pilot and the session, not the ship ---------------------
	const SetItem SETTINGS[] = {
		{ "MasterArmed",      &g_fx.masterArmed,      ST_B },
		// enables only - see the note above about the scripted channels
		{ "BlackoutOn",       &g_fx.blackoutEnabled,  ST_B },
		{ "RedoutOn",         &g_fx.redoutEnabled,    ST_B },
		{ "TunnelOn",         &g_fx.tunnelEnabled,    ST_B },
		{ "SpotsOn",          &g_fx.spotsEnabled,     ST_B },
		{ "GreyoutOn",        &g_fx.greyoutEnabled,   ST_B },
		{ "BlurOn",           &g_fx.blurEnabled,      ST_B },
		{ "AberrationOn",     &g_fx.aberrationEnabled,ST_B },
		{ "SparklesOn",       &g_fx.sparklesEnabled,  ST_B },
		{ "SwimOn",           &g_fx.swimEnabled,      ST_B },
		{ "TiltOn",           &g_fx.tiltEnabled,      ST_B },
		{ "HeartbeatOn",      &g_fx.heartbeatEnabled, ST_B },
		// cam-shake: the LOOK knobs (intensity is physics-driven, nothing to save)
		{ "ShakeOn",          &g_fx.shakeEnabled,     ST_B },
		{ "ShakeAmpX",        &g_fx.shakeAmpX,        ST_F },
		{ "ShakeAmpY",        &g_fx.shakeAmpY,        ST_F },
		{ "ShakeAmpZ",        &g_fx.shakeAmpZ,        ST_F },
		{ "ShakeFreq",        &g_fx.shakeFreq,        ST_F },
		// world - the ENABLES and the VC preference are the user's, not the ship's
		{ "ShimmerOn",        &g_fx.shimmerEnabled,   ST_B },
		{ "PlumeOn",          &g_fx.plumeEnabled,     ST_B },
		{ "PlumePhysicsOn",   &g_fx.plumePhysics,     ST_B },   // the thruster family's
		                                                        //   LAB | PHYSICS switch
		{ "PlumeBellOn",      &g_fx.plumeBellOn,      ST_B },   // bell glow pill
		{ "PrtOn",            &g_fx.prtEnabled,       ST_B },   // exhaust particles pill
		{ "StockExhaustOn",   &g_fx.stockExhaust,     ST_B },   // patch (n): off = judge
		{ "StockParticlesOn", &g_fx.stockParticles,   ST_B },   //   the halves split
		                                                        //   the overlay alone
		{ "ReentryOn",        &g_fx.reentryEnabled,   ST_B },
		{ "ReentryVC",        &g_fx.reentryVC,        ST_B },
		{ "ScenarioSound",    &g_fx.seqSoundEnabled,  ST_B },
		// eclipse: an EYE, not a hull - the same pilot behind every canopy, so global
		// (invariant 17). Test is transient, like shakeTest, and is never written.
		{ "VCShadowsOn",      &g_fx.vcShadows,        ST_B },   // the pilot's preference;
		                                                        // the RADIUS is per class
		{ "EclipseOn",        &g_fx.eclipseEnabled,   ST_B },
		{ "EclipseDim",       &g_fx.eclipseDim,       ST_F },
		{ "EclipseAdapt",     &g_fx.eclipseAdapt,     ST_F },
		{ "EclipseColour",    &g_fx.eclipseColour,    ST_F },
		// Aurora: only the master on/off pill is global - "do I want curtains at all" is the
		// pilot's preference. EVERY look knob is PER BODY (below), because an aurora is a
		// property of the WORLD you are at: switching from Earth to Jupiter must bring
		// Jupiter's saved look with it, not leave Earth's numbers on the sliders.
		// Test is transient (like eclipseTest / shakeTest) and never written.
		{ "AuroraOn",         &g_fx.auroraEnabled,    ST_B },
		// Lightning: same split as the aurora - the pill is the pilot's global
		// preference, every storm property is per body (below).
		{ "LightningOn",      &g_fx.ltgEnabled,       ST_B },
		// God rays: GLOBAL in full, unlike the aurora and the lightning. There is no
		// per-body split to make, because the thing that differs between worlds - how
		// much air there is to scatter in - is read from the sim every frame, not set by
		// the user. These five are taste. Test is transient and never written.
		{ "GodRaysOn",        &g_fx.grayEnabled,      ST_B },
		{ "GodRayStrength",   &g_fx.grayStrength,     ST_F },
		{ "GodRayLength",     &g_fx.grayLength,       ST_F },
		{ "GodRayDecay",      &g_fx.grayDecay,        ST_F },
		{ "GodRaySens",       &g_fx.graySens,         ST_F },   // was GodRayThresh, inverted
		{ "GodRayWarm",       &g_fx.grayWarm,         ST_F },
		// Vapour cone: the PILL only. Everything about its shape is a fact about a hull,
		// so it lives in the class file (below) - the same split the shimmer uses. Test is
		// transient and never written: a saved TEST would hang a permanent shroud on a
		// parked ship at the next session start.
		{ "VapourOn",         &g_fx.vapEnabled,       ST_B },
		// pilot / felt-G model
		{ "PhysicsMode",      &g_fx.physicsMode,      ST_B },
		{ "GTolerance",       &g_fx.gTolerance,       ST_F },
		{ "GSuit",            &g_fx.gsuitOn,          ST_B },
		{ "PilotPose",        &g_fx.pilotPose,        ST_I },
		{ "GRefCamera",       &g_fx.gRefCamera,       ST_B },
		{ "GainBlackout",     &g_fx.gainBlackout,     ST_F },
		{ "GainRedout",       &g_fx.gainRedout,       ST_F },
		{ "GainTunnel",       &g_fx.gainTunnel,       ST_F },
		{ "GainSpots",        &g_fx.gainSpots,        ST_F },
		{ "GainGreyout",      &g_fx.gainGreyout,      ST_F },
		{ "GainBlur",         &g_fx.gainBlur,         ST_F },
		{ "GainHeartbeat",    &g_fx.gainHeartbeat,    ST_F },
		{ "GainAberration",   &g_fx.gainAberration,   ST_F },
		{ "GainSparkles",     &g_fx.gainSparkles,     ST_F },
		{ "GainSwim",         &g_fx.gainSwim,         ST_F },
		{ "GainTilt",         &g_fx.gainTilt,         ST_F },
	};
	const int NSETTINGS = (int)(sizeof(SETTINGS) / sizeof(SETTINGS[0]));

	// --- PER VESSEL CLASS: everything whose right value depends on the hull ---
	// Size, shape and engine layout decide all of these, so they are remembered
	// against the class name and swapped in when the focus vessel changes.
	const SetItem CLASSSET[] = {
		{ "Shimmer",          &g_fx.shimmer,          ST_F },   // engine haze: per engine
		{ "ShimmerOfs",       &g_fx.shimmerOfs,       ST_F },   //   layout
		{ "Plume",            &g_fx.plume,            ST_F },   // plume expansion overlay:
		{ "PlumeWidth",       &g_fx.plumeWidth,       ST_F },   //   the silhouette axes
		{ "PlumeLen",         &g_fx.plumeLen,         ST_F },   //   (ours replaces stock),
		{ "PlumeExpHi",       &g_fx.plumeExpHi,       ST_F },   //   the expansion band
		{ "PlumeExpLo",       &g_fx.plumeExpLo,       ST_F },   //   handles (log10 Pa),
		{ "PlumeSoot",        &g_fx.plumeSoot,        ST_F },   //   the ablative soot
		{ "PlumeSootRate",    &g_fx.plumeSootRate,    ST_F },   //   opacity + churn,
		{ "PlumeBellGlow",    &g_fx.plumeBellGlow,    ST_F },   //   the bell-glow trim
		{ "PlumeBellHeatT",   &g_fx.plumeBellHeatT,   ST_F },   //   + its two thermal
		{ "PlumeBellCoolT",   &g_fx.plumeBellCoolT,   ST_F },   //   timescales [s],
		{ "PlumeCells",       &g_fx.plumeCells,       ST_F },   //   the disc count,
		{ "PlumeDiamond",     &g_fx.plumeDiamond,     ST_F },   //   strength + the shape
		{ "PlumeSpacing",     &g_fx.plumeSpacing,     ST_F },   //   knobs + the two colour
		{ "PlumeBloomWid",    &g_fx.plumeBloomWid,    ST_F },   //   picks, per class like
		{ "PlumeBloomBri",    &g_fx.plumeBloomBri,    ST_F },   //   the shimmer (nozzle
		{ "PlumeThroat",      &g_fx.plumeThroat,      ST_F },   //   + the throat fire
		{ "PlumeThroatOfs",   &g_fx.plumeThroatOfs,   ST_F },   //   and its axial slide,
		{ "PlumeColJet",      &g_fx.plumeColJet,      ST_I },   //   layout is a property
		{ "PlumeColBloom",    &g_fx.plumeColBloom,    ST_I },   //   of the hull)
		{ "PrtOffset",        &g_fx.prtOffset,        ST_F },   // exhaust particles: the
		{ "PrtSize",          &g_fx.prtSize,          ST_F },   //   author's own stream
		{ "PrtLifetime",      &g_fx.prtLifetime,      ST_F },   //   fields, per class -
		{ "PrtRate",          &g_fx.prtRate,          ST_F },   //   nozzle scale decides
		{ "PrtSpeed",         &g_fx.prtSpeed,         ST_F },   //   every one of them, so
		{ "PrtSpread",        &g_fx.prtSpread,        ST_F },   //   a DG's numbers mean
		{ "PrtGrowth",        &g_fx.prtGrowth,        ST_F },   //   nothing on an SRB
		{ "PrtSlowdown",      &g_fx.prtSlowdown,      ST_F },
		{ "PrtDiffuse",       &g_fx.prtDiffuse,       ST_B },
		{ "PrtAirFade",       &g_fx.prtAirFade,       ST_B },
		{ "PrtColour",        &g_fx.prtColour,        ST_I },
		{ "ReentryTrim",      &g_fx.reentry,          ST_F },
		{ "PlasSaturation",   &g_fx.plasSat,          ST_F },
		{ "PlasHullLight",    &g_fx.plasLight,        ST_F },
		{ "PlasStreakLen",    &g_fx.plasStreakLen,    ST_F },
		{ "PlasStreakWid",    &g_fx.plasStreakWid,    ST_F },
		{ "PlasWander",       &g_fx.plasWander,       ST_F },
		{ "PlasEdgeLight",    &g_fx.plasComa,         ST_F },
		{ "PlasSpark",        &g_fx.plasSpark,        ST_F },
		{ "PlasSparkLife",    &g_fx.plasSparkLife,    ST_F },
		{ "PlasSparkSize",    &g_fx.plasSparkSize,    ST_F },
		{ "PlasShockBright",  &g_fx.plasShockBright,  ST_F },
		{ "PlasShockDist",    &g_fx.plasShockDist,    ST_F },
		{ "PlasShellDist",    &g_fx.plasShellDist,    ST_F },
		{ "PlasBowlSizeX",    &g_fx.plasBowlSX,       ST_F },
		{ "PlasBowlSizeY",    &g_fx.plasBowlSY,       ST_F },
		{ "PlasBowlSizeZ",    &g_fx.plasBowlSZ,       ST_F },
		{ "PlasTrail",        &g_fx.plasTrail,        ST_F },   // the trail: density / life /
		{ "PlasTrailLife",    &g_fx.plasTrailLife,    ST_F },   //   width / start standoff /
		{ "PlasTrailWid",     &g_fx.plasTrailWid,     ST_F },   //   head+tail hues,
		{ "PlasTrailStart",   &g_fx.plasTrailStart,   ST_F },   //   per class like the rest
		{ "PlasTrailTint",    &g_fx.plasTrailTint,    ST_I },
		{ "PlasTrailTint2",   &g_fx.plasTrailTint2,   ST_I },
		{ "PlasmaTint",       &g_fx.plasmaTint,       ST_I },   // COLORREF bits via int
		{ "PlasmaTint2",      &g_fx.plasmaTint2,      ST_I },   // ... and the magenta cast

		// THE VAPOUR CONE's shape. Per class because both numbers are answers to
		// questions about the airframe: how wide the shroud stands off, and where along
		// the flow axis the flow first goes supersonic (which is a question about the
		// nose). Its LENGTH is deliberately absent - that comes from the Mach angle.
		{ "VapourStrength",   &g_fx.vapStrength,      ST_F },
		{ "VapourSize",       &g_fx.vapSize,          ST_F },
		{ "VapourPos",        &g_fx.vapPos,           ST_F },
		{ "VapourMachMin",    &g_fx.vapMachMin,       ST_F },   // the band handles: where
		{ "VapourMachMax",    &g_fx.vapMachMax,       ST_F },   //   the shroud lives
		{ "VapourFlickHz",    &g_fx.vapFlickHz,       ST_F },

		{ "CopShift",         &g_fx.copShift,         ST_F },   // the most per-vessel
		                                                        // number in the addon
		// VC shadow box: how big the cabin is, which is a property of the HULL
		{ "VCShadowRadius",   &g_fx.vcShadowRadius,   ST_F },
		{ "VCShadowDepth",    &g_fx.vcShadowDepth,    ST_F },   // patch (p): the ambient bite
	};
	const int NCLASSSET = (int)(sizeof(CLASSSET) / sizeof(CLASSSET[0]));

	// --- PER BODY: what a WORLD's aurora is ------------------------------------
	// Config\ORO\bodies\<name>.cfg - files ORO ships and owns, deliberately NOT the
	// body's own Orbiter cfg (user's call 2026-08-07: editing stock configs risks trouble
	// later and confuses users; owning the tree leaves room for weather data too).
	// The RANGES the sliders span live here, so the same 0..1 Base knob means 40-160 km at
	// Earth and thousands of km at Jupiter, plus the colours and ribbon count that give the
	// world its identity.
	// There is deliberately no AuroraEnable key: ACTIVITY is the opt-in. Its built-in
	// default is zero, so a world with no file is silent, and turning the slider up at any
	// world is what gives it an aurora. One control, no way for two flags to disagree.
	const SetItem BODYSET[] = {
		// The RANGES the sliders span - what makes one 0..1 knob mean 40-160 km at Earth
		// and 200-700 km at Jupiter.
		{ "AuroraBaseMinKm",  &g_fx.aurBaseMinKm,     ST_F },
		{ "AuroraBaseMaxKm",  &g_fx.aurBaseMaxKm,     ST_F },
		{ "AuroraTopMinKm",   &g_fx.aurTopMinKm,      ST_F },
		{ "AuroraTopMaxKm",   &g_fx.aurTopMaxKm,      ST_F },
		{ "AuroraColatMin",   &g_fx.aurColatMinDeg,   ST_F },   // COLATITUDE range [deg]
		{ "AuroraColatMax",   &g_fx.aurColatMaxDeg,   ST_F },
		// The LOOK itself. These are the dialog's sliders, and they are per body for the
		// same reason the ranges are: "the aurora at Jupiter" is a whole configuration, and
		// arriving there must restore it. Ranges alone were not enough - with the positions
		// left global, only Ribbons appeared to change when the target body switched.
		{ "AuroraActivity",   &g_fx.auroraActivity,   ST_F },
		{ "AuroraReach",      &g_fx.auroraReach,      ST_F },
		{ "AuroraFold",       &g_fx.auroraFold,       ST_F },
		{ "AuroraRays",       &g_fx.auroraRays,       ST_F },
		{ "AuroraBreakup",    &g_fx.auroraBreakup,    ST_F },
		{ "AuroraBase",       &g_fx.auroraBase,       ST_F },
		{ "AuroraHeight",     &g_fx.auroraHeight,     ST_F },
		{ "AuroraThick",      &g_fx.auroraThick,      ST_F },
		// The MAGNETIC pole offset [deg] - a property of the world's field, so per body like
		// everything else here. Earth's is ~11 deg.
		{ "AuroraTiltX",      &g_fx.auroraTiltX,      ST_F },
		{ "AuroraTiltY",      &g_fx.auroraTiltY,      ST_F },
		// Three colours by ALTITUDE - the world's own chemistry. Named rather than numbered
		// because "Col1" said nothing about which band it painted.
		{ "AuroraColBase",    &g_fx.auroraColBase,    ST_I },
		{ "AuroraColBody",    &g_fx.auroraColBody,    ST_I },
		{ "AuroraColTop",     &g_fx.auroraColTop,     ST_I },
		{ "AuroraRibbons",    &g_fx.auroraRibbons,    ST_I },
		// LIGHTNING (2026-08-08): what a world's storms ARE. Same law as the aurora
		// block - LtgActivity is the opt-in (built-in default 0, no enable key), so a
		// world with no file stays silent and turning the slider up + SAVE gives it
		// storms. Cells themselves come from the world's own cloud tiles at runtime.
		{ "LtgActivity",      &g_fx.ltgActivity,      ST_F },
		{ "LtgBright",        &g_fx.ltgBright,        ST_F },
		{ "LtgRate",          &g_fx.ltgRate,          ST_F },
		{ "LtgCellKm",        &g_fx.ltgCellKm,        ST_F },
		{ "LtgColour",        &g_fx.ltgColour,        ST_I },
	};
	const int NBODYSET = (int)(sizeof(BODYSET) / sizeof(BODYSET[0]));

	// The BUILT-IN defaults for every per-body field, captured from g_fx's member
	// initialisers before anything is ever loaded. A world with no file must snap the
	// sliders back to these rather than inherit the last world's numbers - otherwise
	// arriving somewhere unconfigured silently shows you Jupiter's 3000 km curtain tops.
	// (This is the opposite of the per-CLASS rule, which deliberately CARRIES the current
	// look over to an untuned vessel: there, one hull's look is a reasonable starting point
	// for another; here, a world's aurora is not.)
	struct SetVal { float f; int i; bool b; };
	SetVal g_bodyDefault[NBODYSET];
	bool   g_bodyDefaultsCaptured = false;

	void CaptureBodyDefaults()
	{
		if (g_bodyDefaultsCaptured) return;
		for (int i = 0; i < NBODYSET; i++) {
			switch (BODYSET[i].t) {
			case ST_F: g_bodyDefault[i].f = *(float*)BODYSET[i].p; break;
			case ST_B: g_bodyDefault[i].b = *(bool*) BODYSET[i].p; break;
			case ST_I: g_bodyDefault[i].i = *(int*)  BODYSET[i].p; break;
			}
		}
		g_bodyDefaultsCaptured = true;
	}

	void RestoreBodyDefaults()
	{
		CaptureBodyDefaults();
		for (int i = 0; i < NBODYSET; i++) {
			switch (BODYSET[i].t) {
			case ST_F: *(float*)BODYSET[i].p = g_bodyDefault[i].f; break;
			case ST_B: *(bool*) BODYSET[i].p = g_bodyDefault[i].b; break;
			case ST_I: *(int*)  BODYSET[i].p = g_bodyDefault[i].i; break;
			}
		}
	}

	const char* SETTINGS_FILE = "ORO.cfg";
	char        g_setClass[64]  = "";        // class whose file is currently loaded
	char        g_setBody[64]   = "";        // body whose aurora file is currently loaded
	char        g_setPathBuf[128] = "Config\\ORO.cfg";

	void WriteTable(FILEHANDLE f, const SetItem* t, int n)
	{
		for (int i = 0; i < n; i++) {
			switch (t[i].t) {
			case ST_F: oapiWriteItem_float(f, K(t[i].key), (double)(*(float*)t[i].p)); break;
			case ST_B: oapiWriteItem_bool (f, K(t[i].key), *(bool*)t[i].p);            break;
			case ST_I: oapiWriteItem_int  (f, K(t[i].key), *(int*)t[i].p);             break;
			}
		}
	}

	int ReadTable(FILEHANDLE f, const SetItem* t, int n)
	{
		int got = 0;
		for (int i = 0; i < n; i++) {
			switch (t[i].t) {
			case ST_F: { double d; if (oapiReadItem_float(f, K(t[i].key), d)) { *(float*)t[i].p = (float)d; got++; } break; }
			case ST_B: { bool   b; if (oapiReadItem_bool (f, K(t[i].key), b)) { *(bool*)t[i].p  = b;        got++; } break; }
			case ST_I: { int    v; if (oapiReadItem_int  (f, K(t[i].key), v)) { *(int*)t[i].p   = v;        got++; } break; }
			}
		}
		return got;
	}

	// Class name -> file name. Orbiter class names can carry path separators and
	// spaces (they are config paths), none of which belong in a leaf file name.
	void ClassFileName(const char* cls, char* out, int cap)
	{
		int j = 0;
		out[0] = 0;
		if (!cls) return;
		for (int i = 0; cls[i] && j < cap - 6; i++) {
			const char c = cls[i];
			const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
			             || (c >= '0' && c <= '9') || c == '-' || c == '_';
			out[j++] = ok ? c : '_';
		}
		out[j] = 0;
		if (j == 0) { strcpy_s(out, cap, "unnamed"); }
	}
}

const char* OroSettings_Class() { return g_setClass; }

// Exposed for OroReentry.cpp: the heatshield-override mesh shares the class
// file-name sanitiser so the cfg and the mesh can never disagree about a name.
void OroClassFileName(const char* cls, char* out, int cap) { ClassFileName(cls, out, cap); }

// What the dialog's confirmation line names: the global file, or the class file
// once a class is in play (that is the one the user just tuned).
const char* OroSettings_Path()
{
	if (g_setClass[0]) {
		char fn[64];
		ClassFileName(g_setClass, fn, sizeof(fn));
		sprintf_s(g_setPathBuf, "Config\\ORO\\%s.cfg", fn);
	} else {
		strcpy_s(g_setPathBuf, "Config\\ORO.cfg");
	}
	return g_setPathBuf;
}

const char* OroSettings_Body() { return g_setBody; }

namespace {
	// Which worlds we ship (or the user has authored) an aurora for. Scanned ONCE from
	// Config\ORO\bodies\ and cached: it is the answer to "can this body glow at all",
	// and it has to be cheap because FindAuroraBody asks it about every gbody each frame.
	//
	// It exists because Orbiter's atmosphere flag is the WRONG test for the moons. GANYMEDE
	// is the only moon in the solar system with its own magnetic field - it has genuine
	// polar ovals - and EUROPA's oxygen glow is part of how we know it has a subsurface
	// ocean, yet neither carries an atmosphere in Orbiter's configs, so both were
	// unreachable. A shipped config file IS the statement that a world glows, so it
	// overrides the flag. (Io, Titan and Triton do have atmospheres and never needed this.)
	char g_bodyFiles[48][32];
	int  g_nBodyFiles  = 0;
	bool g_bodyScanned = false;

	void ScanBodyFiles()
	{
		if (g_bodyScanned) return;
		g_bodyScanned = true;
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA("Config\\ORO\\bodies\\*.cfg", &fd);
		if (h == INVALID_HANDLE_VALUE) return;
		do {
			if (g_nBodyFiles >= 48) break;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			char nm[32];
			strncpy_s(nm, fd.cFileName, _TRUNCATE);
			char* dot = strrchr(nm, '.');
			if (dot) *dot = 0;
			strcpy_s(g_bodyFiles[g_nBodyFiles++], nm);
		} while (FindNextFileA(h, &fd));
		FindClose(h);
		oapiWriteLogV("ORO: %d aurora body file(s) in Config\\ORO\\bodies.", g_nBodyFiles);
	}
}

// True if this world has an aurora file - checked by the aurora's body selection so a
// configured moon qualifies even with no atmosphere. A file APPEARS the moment one is
// saved, so re-scan on save rather than making the user restart.
bool OroSettings_BodyHasFile(const char* name)
{
	if (!name || !name[0]) return false;
	ScanBodyFiles();
	for (int i = 0; i < g_nBodyFiles; i++)
		if (_stricmp(g_bodyFiles[i], name) == 0) return true;
	return false;
}

// Targeted save. `mask` is ORO_SCOPE_GLOBAL | _CLASS | _BODY - the per-tab SAVE buttons
// each write only the files their tab can have changed, so pressing SAVE on one tab never
// rewrites another's numbers. A scope with nothing to write yet (no focus class, no body
// in range) is skipped silently rather than failing: there is nothing wrong with saving
// the pilot's settings before you have flown anything.
bool OroSettings_SaveScope(int mask)
{
	bool ok = true;

	if (mask & ORO_SCOPE_GLOBAL) {
		FILEHANDLE f = oapiOpenFile(SETTINGS_FILE, FILE_OUT, CONFIG);
		if (!f) {
			oapiWriteLogV("ORO: could not write Config\\ORO.cfg");
			ok = false;
		} else {
			oapiWriteLine(f, K("; ORO - global settings (the pilot and the session). Written by"));
			oapiWriteLine(f, K("; the dialog's SAVE buttons, loaded when the module starts. Per-VESSEL"));
			oapiWriteLine(f, K("; settings - plasma tuning, reentry trim, shimmer, CoP shift - live in"));
			oapiWriteLine(f, K("; Config\\ORO\\<class>.cfg, and each world's aurora lives in"));
			oapiWriteLine(f, K("; Config\\ORO\\bodies\\<name>.cfg. Delete to restore the defaults."));
			oapiWriteLine(f, K(""));
			WriteTable(f, SETTINGS, NSETTINGS);
			oapiCloseFile(f, FILE_OUT);
		}
	}

	// PER CLASS. Nothing to write until we know which vessel these numbers were
	// tuned against, which is the point of the whole exercise.
	if ((mask & ORO_SCOPE_CLASS) && g_setClass[0]) {
		CreateDirectoryA("Config\\ORO", NULL);      // harmless if it already exists
		char fn[64], rel[128];
		ClassFileName(g_setClass, fn, sizeof(fn));
		sprintf_s(rel, "ORO\\%s.cfg", fn);
		FILEHANDLE fc = oapiOpenFile(rel, FILE_OUT, CONFIG);
		if (!fc) {
			oapiWriteLogV("ORO: could not write Config\\%s", rel);
			ok = false;
		} else {
			char hdr[160];
			sprintf_s(hdr, "; ORO - settings for vessel class %s. Loaded automatically", g_setClass);
			oapiWriteLine(fc, hdr);
			oapiWriteLine(fc, K("; whenever a vessel of this class becomes the focus vessel."));
			oapiWriteLine(fc, K(""));
			WriteTable(fc, CLASSSET, NCLASSSET);
			oapiCloseFile(fc, FILE_OUT);
		}
	}

	// PER BODY - the world's own aurora. Same shape, one directory deeper.
	if ((mask & ORO_SCOPE_BODY) && g_setBody[0]) {
		CreateDirectoryA("Config\\ORO", NULL);
		CreateDirectoryA("Config\\ORO\\bodies", NULL);
		char fn[64], rel[160];
		ClassFileName(g_setBody, fn, sizeof(fn));   // same sanitiser: it is a leaf file name
		sprintf_s(rel, "ORO\\bodies\\%s.cfg", fn);
		FILEHANDLE fb = oapiOpenFile(rel, FILE_OUT, CONFIG);
		if (!fb) {
			oapiWriteLogV("ORO: could not write Config\\%s", rel);
			ok = false;
		} else {
			char hdr[160];
			sprintf_s(hdr, "; ORO - aurora settings for %s. Loaded automatically whenever", g_setBody);
			oapiWriteLine(fb, hdr);
			oapiWriteLine(fb, K("; the curtains are drawn at this world. AuroraActivity 0 (or no file"));
			oapiWriteLine(fb, K("; at all) means this world simply has no aurora."));
			oapiWriteLine(fb, K("; The Min/Max pairs are the RANGES the dialog sliders span."));
			oapiWriteLine(fb, K(""));
			WriteTable(fb, BODYSET, NBODYSET);
			oapiCloseFile(fb, FILE_OUT);
			// A world just became configured - re-scan so it qualifies immediately rather
			// than after a restart (authoring a new world is exactly when this matters).
			g_bodyScanned = false;
			g_nBodyFiles  = 0;
		}
	}
	return ok;
}

// The SAVE-EVERYTHING button in the fixed strip: all three scopes at once.
bool OroSettings_Save()
{
	return OroSettings_SaveScope(ORO_SCOPE_GLOBAL | ORO_SCOPE_CLASS | ORO_SCOPE_BODY);
}

void OroSettings_Load()
{
	// Snapshot the per-body defaults BEFORE anything can overwrite them. This runs from the
	// module constructor, so the fields still hold their member initialisers - which is what
	// an unconfigured world must snap back to.
	CaptureBodyDefaults();
	FILEHANDLE f = oapiOpenFile(SETTINGS_FILE, FILE_IN, CONFIG);
	if (!f) return;                         // no file yet - built-in defaults stand
	const int n = ReadTable(f, SETTINGS, NSETTINGS);
	oapiCloseFile(f, FILE_IN);
	oapiWriteLogV("ORO: global settings loaded (%d of %d items).", n, NSETTINGS);
}

// Swap in a vessel class's numbers. Called when the focus vessel's class changes.
// A class with NO file keeps whatever is on the sliders rather than snapping to
// the built-in defaults: carrying the last look over to an untuned vessel is a
// better starting point than resetting, and it is what makes "tune it, save it"
// the only step the user ever has to take.
void OroSettings_LoadClass(const char* cls)
{
	if (!cls || !cls[0]) return;
	if (_stricmp(cls, g_setClass) == 0) return;        // already current
	strcpy_s(g_setClass, cls);

	char fn[64], rel[128];
	ClassFileName(cls, fn, sizeof(fn));
	sprintf_s(rel, "ORO\\%s.cfg", fn);
	FILEHANDLE f = oapiOpenFile(rel, FILE_IN, CONFIG);
	if (!f) {
		oapiWriteLogV("ORO: vessel class %s has no saved settings - keeping the current ones.", cls);
		return;
	}
	const int n = ReadTable(f, CLASSSET, NCLASSSET);
	oapiCloseFile(f, FILE_IN);
	oapiWriteLogV("ORO: vessel class %s - loaded %d of %d settings.", cls, n, NCLASSSET);
}

// Swap in a WORLD's aurora numbers. Called when the aurora's target body changes.
//
// The fallback policy is FORGIVING PER FIELD, then CLAMP (the user's choice): a missing or
// unreadable key keeps the built-in default for that field alone rather than rejecting the
// whole file, and every value is then clamped into a sane band. A hand-edited file with one
// typo therefore still works, and no value in it can produce a degenerate curtain.
//
// A body with NO file therefore gets a ZERO ACTIVITY back, which is the opt-in: an unlisted
// world is silent rather than wearing Earth's aurora, and turning Activity up is all it
// takes to give it one.
void OroSettings_LoadBody(const char* body)
{
	if (!body || !body[0]) return;
	if (_stricmp(body, g_setBody) == 0) return;        // already current
	strcpy_s(g_setBody, body);

	// DEFAULTS FIRST, ALWAYS - then read the file over the top. That single ordering gives
	// both halves of the rule the user asked for: a world with a file gets exactly what was
	// saved for it, and a world WITHOUT one snaps to the built-in defaults instead of
	// inheriting the last world's look. It also makes the forgiving per-field fallback mean
	// "missing key = the default" rather than "missing key = whatever was on screen".
	RestoreBodyDefaults();

	char fn[64], rel[160];
	ClassFileName(body, fn, sizeof(fn));
	sprintf_s(rel, "ORO\\bodies\\%s.cfg", fn);
	FILEHANDLE f = oapiOpenFile(rel, FILE_IN, CONFIG);
	if (!f) {
		oapiWriteLogV("ORO: %s has no aurora file (Config\\%s) - defaults restored, no curtains there.",
		              body, rel);
		return;
	}
	const int n = ReadTable(f, BODYSET, NBODYSET);
	oapiCloseFile(f, FILE_IN);

	// CLAMP. Ranges must be ordered and positive, or the slider mapping degenerates.
	if (g_fx.aurBaseMinKm   <    0.0f) g_fx.aurBaseMinKm   = 0.0f;
	if (g_fx.aurBaseMaxKm   < g_fx.aurBaseMinKm) g_fx.aurBaseMaxKm = g_fx.aurBaseMinKm + 1.0f;
	if (g_fx.aurTopMinKm    < g_fx.aurBaseMaxKm) g_fx.aurTopMinKm  = g_fx.aurBaseMaxKm;
	if (g_fx.aurTopMaxKm    < g_fx.aurTopMinKm)  g_fx.aurTopMaxKm  = g_fx.aurTopMinKm + 1.0f;
	if (g_fx.aurColatMinDeg <  0.5f) g_fx.aurColatMinDeg =  0.5f;
	if (g_fx.aurColatMinDeg > 89.0f) g_fx.aurColatMinDeg = 89.0f;
	if (g_fx.aurColatMaxDeg < g_fx.aurColatMinDeg) g_fx.aurColatMaxDeg = g_fx.aurColatMinDeg;
	if (g_fx.aurColatMaxDeg > 89.0f) g_fx.aurColatMaxDeg = 89.0f;
	if (g_fx.auroraRibbons  < 1) g_fx.auroraRibbons = 1;
	if (g_fx.auroraRibbons  > 6) g_fx.auroraRibbons = 6;
	g_fx.auroraColBase &= 0x00FFFFFFu;
	g_fx.auroraColBody &= 0x00FFFFFFu;
	g_fx.auroraColTop  &= 0x00FFFFFFu;
	// The look sliders are all unit knobs - a hand-edited 5 must not drive the geometry.
	float* unit[] = { &g_fx.auroraActivity, &g_fx.auroraReach, &g_fx.auroraFold,
	                  &g_fx.auroraRays, &g_fx.auroraBreakup, &g_fx.auroraBase,
	                  &g_fx.auroraHeight, &g_fx.auroraThick };
	for (int u = 0; u < (int)(sizeof(unit) / sizeof(unit[0])); u++) {
		if (*unit[u] < 0.0f) *unit[u] = 0.0f; else if (*unit[u] > 1.0f) *unit[u] = 1.0f;
	}
	// Tilts are angles, not unit knobs: +-90 deg swings the magnetic pole all the way to
	// the equator, which is as far as the idea means anything.
	if (g_fx.auroraTiltX < -90.0f) g_fx.auroraTiltX = -90.0f; else if (g_fx.auroraTiltX > 90.0f) g_fx.auroraTiltX = 90.0f;
	if (g_fx.auroraTiltY < -90.0f) g_fx.auroraTiltY = -90.0f; else if (g_fx.auroraTiltY > 90.0f) g_fx.auroraTiltY = 90.0f;

	oapiWriteLogV("ORO: %s aurora - loaded %d of %d settings, activity %.2f.",
	              body, n, NBODYSET, g_fx.auroraActivity);
}

// File-scope render callback matching __gcRenderProc (void(__cdecl*)(Sketchpad*, void*)).
// Mirrors the canonical DrawOrbits sample: a plain __cdecl thunk that forwards to the
// module instance (arriving via pParam). Declared here so clbkSimulationStart and the
// destructor can (un)register it.
static void __cdecl OroRenderProc(oapi::Sketchpad* pSkp, void* pParam)
{
	static_cast<OroModule*>(pParam)->DrawOverlay(pSkp);
}

// Second thunk for the patch-(i) pre-resolve slot. A render proc receives no id, so
// each slot needs its own entry point; this one forwards to DrawPreResolve.
static void __cdecl OroPreResolveProc(oapi::Sketchpad* pSkp, void* pParam)
{
	static_cast<OroModule*>(pParam)->DrawPreResolve(pSkp);
}

// GENERICPROC_SHUTDOWN thunk. Signature differs from the render procs
// (__gcGenericProc is int/void*/void*, no Sketchpad).
// !! THE INSTANCE ARRIVES AS THE THIRD ARGUMENT !! The client dispatches
// `it->proc(iUser, pUser, it->pParam)` and calls the shutdown id with iUser=0,
// pUser=NULL, so reading pUser would dereference NULL on every session close.
static void __cdecl OroShutdownProc(int iUser, void* pUser, void* pParam)
{
	if (pParam) static_cast<OroModule*>(pParam)->ReleaseSceneOwnedBorrows(true);
}

// File-scope mirror of the module's patch-(f) capability flag, so the dialog can grey the
// VC SHADOWS section out without reaching into the module instance (the OroSettings_*
// pattern). Set once in clbkSimulationStart.
static bool g_vcShadowSupported = false;
bool OroVCShadowsSupported() { return g_vcShadowSupported; }

// Same pattern for patch (g)'s depth clip: the REENTRY tab warns when it is dark,
// because on screen the degradation is silent (plasma paints through the hull).
static bool g_depthClipMirror = false;
bool OroDepthClipOK() { return g_depthClipMirror; }

// Same pattern for patch (n): the THRUSTER tab's STOCK EXHAUST pill greys out when
// the client cannot suppress (a switch that cannot do anything is worse than none).
static bool g_stockExSupported = false;
bool OroStockExhaustSupported() { return g_stockExSupported; }

// Same pattern for patch (l)'s textured sprites: the EXHAUST PARTICLES section greys
// out wholesale without them. Mirrored from prtTexMode each pre-step rather than once
// at start, because the probe is lazy (the atlas is created on the first update).
static bool g_prtSupported = false;
bool OroParticleTintOK() { return g_prtSupported; }

// ----------------------------------------------------------------------------
// Module lifecycle
// ----------------------------------------------------------------------------

OroModule::OroModule(HINSTANCE hDLL) : oapi::Module(hDLL)
{
	// Settings come back ONCE, here - not per simulation start, which would throw
	// away any tuning done earlier in the same Orbiter run.
	OroSettings_Load();
	oapiWriteLogV("ORO: module constructed.");
}

OroModule::~OroModule()
{
	// Remove our render callback (RENDERPROC_DELETE = 0: the client finds it by proc
	// pointer and nulls the slot). The RenderProcs list lives on the persistent
	// D3D9Client, so a leftover entry would outlive us - unregister on unload.
	if (pCore && renderProcRegistered)
		pCore->RegisterRenderProc(OroRenderProc, RENDERPROC_DELETE, nullptr);
	if (pCore && preResolveRegistered)
		pCore->RegisterRenderProc(OroPreResolveProc, RENDERPROC_DELETE, nullptr);
	// Same rule, and MORE important here: this thunk captures `this`, so a leftover entry
	// would hand a freed OroModule to the next session's close.
	if (pCore && shutdownProcRegistered)
		pCore->RegisterGenericProc(OroShutdownProc, GENERICPROC_DELETE, nullptr);

	// Normally already released in clbkSimulationEnd; guard for teardown paths
	// that skip it.
	if (pCore && hTunnelPoly) {
		pCore->DeletePoly(hTunnelPoly);
		hTunnelPoly = NULL;
	}
	if (pCore && hSpotsPoly) {
		pCore->DeletePoly(hSpotsPoly);
		hSpotsPoly = NULL;
	}
	if (pCore && hHeartPoly) {
		pCore->DeletePoly(hHeartPoly);
		hHeartPoly = NULL;
	}
	if (pCore && hSparkPoly) {
		pCore->DeletePoly(hSparkPoly);
		hSparkPoly = NULL;
	}
	if (pCore && hPlasmaPoly) {
		pCore->DeletePoly(hPlasmaPoly);
		hPlasmaPoly = NULL;
	}
	if (pCore && hAuroraPoly) {
		pCore->DeletePoly(hAuroraPoly);
		hAuroraPoly = NULL;
	}
	if (pCore && hTrailPoly) {
		pCore->DeletePoly(hTrailPoly);
		hTrailPoly = NULL;
	}
	if (pCore && hPlumePoly) {
		pCore->DeletePoly(hPlumePoly);
		hPlumePoly = NULL;
	}
	if (pCore && hPlumeDkPoly) {
		pCore->DeletePoly(hPlumeDkPoly);
		hPlumeDkPoly = NULL;
	}
	if (pCore && hVapourPoly) {
		pCore->DeletePoly(hVapourPoly);
		hVapourPoly = NULL;
	}
	ReleaseParticles("destructor");
	ReleaseParticleTex();     // the synthesized particle texture (device resource)
	if (pCore && hLightningPoly) {
		pCore->DeletePoly(hLightningPoly);
		hLightningPoly = NULL;
	}
	OroLightning_Close();   // the cloud-map FILE handle + TOC + tile cache
	if (pCore && pIPIGrey) { pCore->ReleaseIPInterface(pIPIGrey); pIPIGrey = nullptr; }
	if (pCore && pIPIBlur) { pCore->ReleaseIPInterface(pIPIBlur); pIPIBlur = nullptr; }
	if (pCore && pIPIChroma) { pCore->ReleaseIPInterface(pIPIChroma); pIPIChroma = nullptr; }
	if (pCore && pIPISwim) { pCore->ReleaseIPInterface(pIPISwim); pIPISwim = nullptr; }
	if (pCore && pIPITilt) { pCore->ReleaseIPInterface(pIPITilt); pIPITilt = nullptr; }
	if (pCore && pIPIShimmer) { pCore->ReleaseIPInterface(pIPIShimmer); pIPIShimmer = nullptr; }
	if (pCore && pIPIPlasma) { pCore->ReleaseIPInterface(pIPIPlasma); pIPIPlasma = nullptr; }
	if (pCore && pIPIEclipse) { pCore->ReleaseIPInterface(pIPIEclipse); pIPIEclipse = nullptr; }
	if (pCore && pIPIGodRay) { pCore->ReleaseIPInterface(pIPIGodRay); pIPIGodRay = nullptr; }
	if (hFrameTex)         { oapiDestroySurface(hFrameTex); hFrameTex = NULL; }
	if (hBlurTex)          { oapiDestroySurface(hBlurTex);  hBlurTex  = NULL; }
	if (hLtgAtlas)         { oapiDestroySurface(hLtgAtlas); hLtgAtlas = NULL; }
	ltgTexMode = false; ltgTexTried = false;

	// Defensive: normally released in clbkSimulationEnd; guard teardown paths that skip it.
	ReleaseStockExhaust();
	ReleaseBellGlow();
	if (pXRSound) { delete pXRSound; pXRSound = nullptr; }

	// Reentry plasma lights are BORROWED from other vessels - hand back anything still held.
	ReleaseReentry();

	oapiWriteLogV("ORO: module destroyed.");
}

void OroModule::clbkSimulationStart(RenderMode mode)
{
	// Probe the D3D9Client graphics interface. NULL => not the active client (e.g. the
	// stock inline client); ORO then stays dormant - every effect needs the client's
	// image-processing pipeline.
	OroLogMemory("session start");
	sceneRendered = false;        // no frame drawn in THIS session yet
	lendDeferLogged = false;      // ... and the deferral note is per session too
	// ⚠️ The bell template cache holds an oapiLoadMeshGlobal handle, which is valid for
	// exactly ONE session - both owners (the core's global mesh manager and the client's
	// device-side copy) drop it at session end. Cleared HERE rather than only at session
	// end, so a previous crash or a forced exit cannot leave a stale handle armed for this
	// run. Reusing one was the reload CTD's access-violation face; see OroBell_Reset.
	OroBell_Reset();
	pCore = gcGetCoreInterface();
	if (!pCore) {
		oapiWriteLogV("ORO: D3D9Client interface NOT found - effects disabled (is D3D9Client the active graphics client?).");
		return;
	}

	oapiWriteLogV("ORO: D3D9Client interface connected (gcGetCoreInterface OK). Render mode %d.", (int)mode);

	// Client capability probe: patch (d), the additive Sketchpad blend the plasma
	// geometry draws with. PROBED BY BINDING since 2026-08-08 (invariant 18a):
	// CanSuppressReentry() null-checks patch (c)'s bound pointer, and (c) and (d)
	// landed in the same 260801 rebuild, so one binding covers both. The old
	// `gcAPIVer >= 260801` date compare had been FAILING SILENTLY the whole time -
	// gcAPIVer reads 0 - so the plasma alpha-blended through the fallback for a week
	// while the code claimed additive. ROOT CAUSE (established 2026-08-13, and it is
	// in the client, not here): D3D9Util.cpp's BuildDate() does its only work inside
	// an assert(sscanf_s(__DATE__, ...) == 3), and NDEBUG deletes an assert ALONG
	// WITH ITS ARGUMENT - so in a Release build the parse never runs, day/year stay
	// at their 0 initialisers, and the function returns exactly 0. The value is not
	// lost crossing the call; it was never computed.
	// The binding cannot be fooled that way. NOTE this flips the
	// plasma to REAL additive for the first time: a deliberate step,
	// taken with the Firefly-rework compositing change so both are judged on
	// the new baseline together - additive accumulation past 1.0 is also what the
	// fp16 bloom path NEEDS (alpha-blend can never exceed 1.0, so it can never bloom).
	{
		gcCore::SystemSpecs specs;
		pCore->GetSystemSpecs(&specs, sizeof(specs));
		padAdditive = pCore->CanSuppressReentry();
		// Patch (f): probed by BINDING, not by build date - the guard null-checks the
		// bound pointer, so it cannot be fooled by a stale stamp the way a date compare
		// can. (gcAPIVer itself is reading 0 here; see the BuildDate note above.)
		vcShadowSupported = pCore->CanSetVCShadows();
		g_vcShadowSupported = vcShadowSupported;
		vcShadowLastRad = -1.0f;      // force the first push
		// Patch (n): per-vessel stock-exhaust suppression, probed by binding like the
		// rest. The dialog's STOCK EXHAUST pill greys out without it (invariant 18b).
		g_stockExSupported = pCore->CanSuppressExhaust();
		oapiWriteLogV("ORO: client VC shadows (patch f) %s.",
		              vcShadowSupported ? "available" : "NOT available");
		oapiWriteLogV("ORO: stock exhaust suppression (patch n) %s.",
		              g_stockExSupported ? "available" : "NOT available");
		oapiWriteLogV("ORO: additive sketchpad blend (patch d, probed by binding) %s; gcAPIVer reads %u (diagnostic only, known-broken).",
		              padAdditive ? "available" : "NOT available (plasma will alpha-blend)", specs.gcAPIVer);
	}

	// Register our full-frame draw callback ONCE. RENDERPROC_HUD_2ND fires after the
	// HUD every frame with a Sketchpad bound to the backbuffer. The RenderProcs list
	// persists across sessions (clbkCloseSession does not clear it), so registering
	// every session would stack duplicates - hence the one-shot guard.
	if (!renderProcRegistered) {
		if (pCore->RegisterRenderProc(OroRenderProc, RENDERPROC_HUD_2ND, this)) {
			renderProcRegistered = true;
			oapiWriteLogV("ORO: render proc registered (RENDERPROC_HUD_2ND). Ctrl+G toggles the test tint.");
		} else {
			oapiWriteLogV("ORO: WARNING - RegisterRenderProc failed; no frame draw.");
		}
	}

	// Patch (i): the pre-resolve slot for the reentry plasma. RegisterRenderProc accepts
	// ANY non-zero id (it is a plain list append), so this succeeds even on a pre-(i)
	// client - which then simply never calls it. preResolveLive (latched inside
	// DrawPreResolve on the first real invocation) is what tells DrawOverlay the slot
	// exists; until then the plasma keeps drawing in the old HUD_2ND position.
	if (!preResolveRegistered) {
		if (pCore->RegisterRenderProc(OroPreResolveProc, RENDERPROC_PRE_RESOLVE, this)) {
			preResolveRegistered = true;
			oapiWriteLogV("ORO: pre-resolve proc registered (RENDERPROC_PRE_RESOLVE, patch i). Fires only on a Build >= 260808 client.");
		}
	}

	// ⚠⚠ GENERICPROC_SHUTDOWN - AND IT FIXES A USE-AFTER-FREE. Read this before touching
	// the teardown; it cost a long night and three wrong theories to find.
	//
	// clbkSimulationEnd IS TOO LATE TO HAND BACK ANYTHING THE SCENE OWNS. Measured in the
	// user's own log:
	//     186.575  D3D9: [Session Closed. Scene deleted.]
	//     186.849  ORO: simulation end.            <- 274 ms later
	// Our exhaust particle streams are created by clbkCreateExhaustStream, which does
	// `scene->AddParticleStream(es)` - THE SCENE OWNS THE OBJECT. So by the time
	// clbkSimulationEnd ran, every stream we held had already been freed, and
	// Vessel::DelExhaustStream opens with `ep->Detach()` on that dead pointer, then
	// rebuilds the vessel's contrail[] array around it. A textbook use-after-free in
	// Orbiter's own heap.
	//
	// It never crashed where it happened. The corrupted heap took down the NEXT scenario
	// load instead - std::bad_array_new_length from a garbage `new[]` count, or a NULL
	// allocation the client dereferenced (D3D9Client +0x1ecb). Three unrelated-looking
	// crashes, one cause. Bisection proved it: PrtOn=FALSE, no CTD; PrtOn=TRUE, CTD every
	// time.
	//
	// GENERICPROC_SHUTDOWN is a STOCK gcCore slot (no patch needed) fired at the TOP of
	// clbkCloseSession - before the render-stack check, before bRunning=false, and
	// crucially BEFORE the scene is deleted. That is the only correct moment to give back
	// a scene-owned borrow.
	//
	// ⚠ IT RELEASES ONLY THE SCENE-OWNED BORROWS - see ReleaseSceneOwnedBorrows. An
	// earlier attempt moved the DEVICE resources here instead and left the streams in
	// clbkSimulationEnd, which inverted a dependency (our synthesized particle texture is
	// referenced BY those streams) and fixed nothing. Resources are not a flat set: order
	// them by what points at what, not by category.
	//
	// One-shot guard like the render procs: GenericProcs is a plain member vector of the
	// persistent D3D9Client and clbkCloseSession never clears it.
	if (!shutdownProcRegistered) {
		if (pCore->RegisterGenericProc(OroShutdownProc, GENERICPROC_SHUTDOWN, this)) {
			shutdownProcRegistered = true;
			oapiWriteLogV("ORO: shutdown proc registered (GENERICPROC_SHUTDOWN) - scene-owned borrows returned before the scene is deleted.");
		} else {
			oapiWriteLogV("ORO: WARNING - GENERICPROC_SHUTDOWN registration failed; exhaust streams will be handed back too late.");
		}
	}

	// Build the premium (frame-resampling) pipeline for this session. Compiles
	// orofx.hlsl and confirms the client can hand us the live backbuffer. The
	// capture texture itself is sized lazily in clbkPreStep (needs the viewport).
	EnsureIPI();

	// Eclipse: force the eye to re-snap to whatever light this scenario starts in. A
	// scenario that opens on the night side of a planet must not open BLIND - the pilot
	// has been sitting there, adapted, since before the sim existed.
	eclPrimed = false;

	// Audio (XRSound). Create our module proxy and register the heartbeat wav. XRSound
	// ships with Orbiter 2024; if its DLL is somehow absent, IsPresent() is false and every
	// call no-ops (ORO stays silent, visuals unaffected). LoadWav is lightweight - it
	// defers the actual file read to the first PlayWav - and returns false if the file is
	// missing, so a not-yet-sourced wav is a soft "no sound", not an error. Module sounds
	// must use PlaybackType::Global (XRSound.h); we do our own cockpit/arm gating.
	if (!pXRSound) pXRSound = XRSound::CreateInstance("ORO");
	if (pXRSound && pXRSound->IsPresent()) {
		oapiWriteLogV("ORO: XRSound %.2f connected.", pXRSound->GetVersion());
		if (!pXRSound->LoadWav(SND_HEARTBEAT, "Modules\\ORO\\sounds\\heartbeat.wav", XRSound::PlaybackType::Global))
			oapiWriteLogV("ORO: heartbeat.wav not found - drop a WAV at Modules\\ORO\\sounds\\heartbeat.wav (heartbeat sound stays off until then).");
		// Scenario clips (Induce_*/Recover_*): one per INDUCE_SEQ entry, id SND_SCEN_BASE + i.
		// Missing files are fine - that button just runs silent; the user adds them over time.
		for (int i = 0; i < NSCEN; i++) {
			char path[MAX_PATH];
			sprintf_s(path, "Modules\\ORO\\sounds\\%s", INDUCE_SEQ[i].wav);
			if (!pXRSound->LoadWav(SND_SCEN_BASE + i, path, XRSound::PlaybackType::Global))
				oapiWriteLogV("ORO: scenario clip %s not found - that button runs silent.", INDUCE_SEQ[i].wav);
		}
	} else {
		oapiWriteLogV("ORO: XRSound not present - sounds disabled (visuals unaffected).");
	}
}

void OroModule::clbkSimulationEnd()
{
	// The dialog is a child of the render window, which is about to go away.
	OroDlg_Close();

	// Hand back patch (n)'s stock-exhaust suppression (invariant 14: every borrow
	// returned on every exit path). Before the device teardown - pCore is still live.
	ReleaseStockExhaust();

	// ... and the bell shell (an AddMesh on the camera target - same law).
	ReleaseBellGlow();

	// ... and every borrowed particle stream. A VESSEL borrow, so it stays here rather
	// than in ReleaseDeviceResources: this callback is the point where the vessels are
	// unambiguously still alive.
	// STREAMS BEFORE TEXTURE, AND THAT ORDER IS LOAD-BEARING: the streams reference our
	// synthesized particle texture, which ReleaseDeviceResources destroys. Freeing the
	// texture first would leave live streams pointing at released video memory.
	// On a client that fired GENERICPROC_SHUTDOWN the streams are already gone and this
	// is a no-op; it stays as the fallback for one that did not, and to keep the order
	// visible in one place.
	ReleaseSceneOwnedBorrows(false);

	ReleaseDeviceResources();

	OroLightning_Close();   // reopened lazily next session (per-body, cheap)
	OroBell_Reset();        // the template handle dies with the session - never let a
	                          // stale one simply sit in memory (clbkSimulationStart clears
	                          // it too; that one is the authoritative call)

	// The focus vessel is tearing down - just drop our shake hold (don't touch its offset).
	camActive = false;
	camDelta  = _V(0, 0, 0);

	// Audio: stop our sounds and drop the XRSound proxy (recreated next session start).
	if (pXRSound) {
		pXRSound->StopWav(SND_HEARTBEAT);
		for (int i = 0; i < NSCEN; i++) pXRSound->StopWav(SND_SCEN_BASE + i);
		delete pXRSound; pXRSound = nullptr;
	}

	// Reentry plasma: give every borrowed LightEmitter back before the session tears down.
	// ReentryFreeSlot validates each handle with oapiIsVessel first, so this is safe even
	// as vessels are being destroyed around us.
	ReleaseReentry();
	reentryScanT      = 0.0;
	reentryFullWarned = false;

	oapiWriteLogV("ORO: simulation end.");
	// AFTER every release path above, so the delta between this line and the next
	// session's "session start" is what ORO failed to give back.
	OroLogMemory("session end, after release");
}

// Everything we borrowed that the SCENE owns, handed back at the one moment it is still
// safe to: gcCore's GENERICPROC_SHUTDOWN, at the top of clbkCloseSession. See the long
// note at the registration site.
//
// The exhaust particle streams live in the scene (clbkCreateExhaustStream ->
// scene->AddParticleStream), and since 2026-08-12 so do TWO SURFACES - see below.
// Anything added here later must meet the same test: IS IT DESTROYED WITH THE SCENE?
// Light emitters, meshes and the suppression flags are not - they belong to VESSELS,
// which outlive the scene and are still alive in clbkSimulationEnd, so they stay there.
//
// Safe to call twice: every release below NULLs its handle, so the clbkSimulationEnd
// call that follows is a no-op on a client that fired the proc, and the real release on
// one that did not.
void OroModule::ReleaseSceneOwnedBorrows(bool fromShutdownProc)
{
	ReleaseParticles(fromShutdownProc ? "shutdown proc - scene still alive"
	                                  : "simulation end - SCENE ALREADY GONE");

	// ⚠️ THE TWO SURFACES THAT WERE STILL ALIVE WHEN THE DEVICE DIED (2026-08-12, from
	// his log): `UnDeleted Surfaces(s) Detected 2` naming a 512x512 and a 256x256, then
	// two `clbkReleaseSurface ... D3D9 Graphics services off-line` when clbkSimulationEnd
	// finally got to them ~270 ms too late. They are the LIGHTNING ATLAS and the
	// synthesized PARTICLE TEXTURE, and they belong here for invariant 23(l)'s reason:
	// they are device resources, and the device goes with the scene.
	//
	// ⚠️ THE ORDER IS THE WHOLE POINT, and 23(l) records the earlier attempt that got it
	// wrong: "resources are not a flat set - order them by what points at what, never by
	// category." Two dependencies to honour, and they run in opposite directions:
	//   * the exhaust STREAMS hold the particle texture in their PARTICLESTREAMSPEC, so
	//     the streams go first (ReleaseParticles, above) and the texture last;
	//   * the lightning POLY holds the atlas as its bound texture (CreateTrianglesTex),
	//     so the poly goes before the atlas.
	// Moving the poly early is otherwise harmless - nothing draws after the scene is
	// gone - and it removes the question rather than reasoning about whether the client's
	// D3D9Triangle destructor dereferences its texture handle.
	if (pCore && hLightningPoly) {
		pCore->DeletePoly(hLightningPoly);
		hLightningPoly = NULL;
	}
	if (hLtgAtlas) { oapiDestroySurface(hLtgAtlas); hLtgAtlas = NULL; }
	ltgTexMode = false; ltgTexTried = false;   // re-probed + recreated next session
	ReleaseParticleTex();                      // ... and it points at nothing now

	// THE CAPTURE PAIR, moved here 2026-08-12 for the same reason. Fixing the first two
	// surfaces did not empty the list - it just changed which two were named: his next log
	// reported `UnDeleted Surfaces Detected 2` as (1920,1080) twice, i.e. these. They are
	// the IPI capture and blur targets, they are device resources like the rest, and
	// clbkSimulationEnd is ~400 ms too late for all of them. Nothing points at these two,
	// so they need no ordering; the IPI interfaces that SAMPLE them are released in
	// ReleaseDeviceResources afterwards, which is safe because nothing draws after the
	// scene is gone. (EnsureFrameTex recreates them next session; texW/texH are the
	// size-change guard and must go with them or it will think they are still valid.)
	if (hFrameTex) { oapiDestroySurface(hFrameTex); hFrameTex = NULL; }
	if (hBlurTex)  { oapiDestroySurface(hBlurTex);  hBlurTex  = NULL; }
	texW = texH = 0;
}

// See the header for WHY this is split out. Called from clbkSimulationEnd only - these
// are DEVICE resources, and unlike the scene-owned borrows above they survive long
// enough to be released there.
void OroModule::ReleaseDeviceResources()
{
	// The effect polys live on the client's device resources - release them with
	// the session and let them lazily recreate next session.
	if (pCore && hTunnelPoly) {
		pCore->DeletePoly(hTunnelPoly);
		hTunnelPoly = NULL;
	}
	if (pCore && hSpotsPoly) {
		pCore->DeletePoly(hSpotsPoly);
		hSpotsPoly = NULL;
	}
	if (pCore && hHeartPoly) {
		pCore->DeletePoly(hHeartPoly);
		hHeartPoly = NULL;
	}
	if (pCore && hSparkPoly) {
		pCore->DeletePoly(hSparkPoly);
		hSparkPoly = NULL;
	}
	if (pCore && hPlasmaPoly) {
		pCore->DeletePoly(hPlasmaPoly);
		hPlasmaPoly = NULL;
	}
	if (pCore && hAuroraPoly) {
		pCore->DeletePoly(hAuroraPoly);
		hAuroraPoly = NULL;
	}
	if (pCore && hTrailPoly) {
		pCore->DeletePoly(hTrailPoly);
		hTrailPoly = NULL;
	}
	if (pCore && hPlumePoly) {
		pCore->DeletePoly(hPlumePoly);
		hPlumePoly = NULL;
	}
	if (pCore && hPlumeDkPoly) {
		pCore->DeletePoly(hPlumeDkPoly);
		hPlumeDkPoly = NULL;
	}
	if (pCore && hVapourPoly) {
		pCore->DeletePoly(hVapourPoly);
		hVapourPoly = NULL;
	}
	ReleaseParticleTex();     // the synthesized particle texture (device resource)
	if (pCore && hLightningPoly) {
		pCore->DeletePoly(hLightningPoly);
		hLightningPoly = NULL;
	}
	lastTunnel = -1.0f;       // the tunnel poly's rebuild cache - dies with the poly

	// Premium pipeline is device-bound too: drop the shader interfaces and the
	// capture/blur textures, and re-arm creation for the next session.
	if (pCore && pIPIGrey) { pCore->ReleaseIPInterface(pIPIGrey); pIPIGrey = nullptr; }
	if (pCore && pIPIBlur) { pCore->ReleaseIPInterface(pIPIBlur); pIPIBlur = nullptr; }
	if (pCore && pIPIChroma) { pCore->ReleaseIPInterface(pIPIChroma); pIPIChroma = nullptr; }
	if (pCore && pIPISwim) { pCore->ReleaseIPInterface(pIPISwim); pIPISwim = nullptr; }
	if (pCore && pIPITilt) { pCore->ReleaseIPInterface(pIPITilt); pIPITilt = nullptr; }
	if (pCore && pIPIShimmer) { pCore->ReleaseIPInterface(pIPIShimmer); pIPIShimmer = nullptr; }
	if (pCore && pIPIPlasma) { pCore->ReleaseIPInterface(pIPIPlasma); pIPIPlasma = nullptr; }
	if (pCore && pIPIEclipse) { pCore->ReleaseIPInterface(pIPIEclipse); pIPIEclipse = nullptr; }
	if (pCore && pIPIGodRay) { pCore->ReleaseIPInterface(pIPIGodRay); pIPIGodRay = nullptr; }
	if (hFrameTex)         { oapiDestroySurface(hFrameTex); hFrameTex = NULL; }
	if (hBlurTex)          { oapiDestroySurface(hBlurTex);  hBlurTex  = NULL; }
	if (hLtgAtlas)         { oapiDestroySurface(hLtgAtlas); hLtgAtlas = NULL; }
	ltgTexMode = false; ltgTexTried = false;   // re-probed + recreated next session
	texW = texH = 0;
	ipiTried = false;
	ipiReady = false;
}

// POST-step hook (round 3, 2026-08-08) - the TRAIL's epoch fix and nothing else.
// Runs after Orbiter has advanced the vessel states and (if the core's ordering
// holds - UpdateTrailPost logs a one-shot verdict) the camera, so the trail's
// world-anchored particles project with the same camera the frame is rendered
// with. Everything vessel-anchored stays in clbkPreStep, where it has always
// been correct. See clbkPostStep's declaration comment for the full mechanism.
void OroModule::clbkPostStep(double simt, double simdt, double mjd)
{
	UpdateTrailPost(simdt);
}

void OroModule::clbkPreStep(double simt, double simdt, double mjd)
{
	// KEYBOARD FOCUS PRIME (once, first frame). Some setups launch a scenario with the
	// render window NOT holding keyboard focus, so the first keypresses fall through and
	// Windows dings - and opening/closing ANY dialog cures it by handing focus back. This
	// is a focus quirk, not an ORO effect (nothing here touches keyboard), but since ORO
	// is loaded we can do the same hand-off automatically. Main thread owns the render
	// window, so SetFocus is safe here; guarded to run exactly once.
	if (!focusPrimed && pCore) {
		HWND hRender = pCore->GetRenderWindow();
		if (hRender) SetFocus(hRender);
		focusPrimed = true;
	}

	// Compute the view gate on the MAIN thread (here), where oapi camera/cockpit queries
	// are safe, and cache it for the render callback to read. Effects apply only in an
	// internal panel/VC view - NOT the generic glass cockpit (which stays the natural
	// kill: F8 to it clears the effect). NOTE: clbkPreStep is not called while paused, so
	// a view change made while paused won't update the gate until the sim resumes.
	viewGate = oapiCameraInternal()
	        && (oapiCockpitMode() != COCKPIT_GENERIC);

	// The EXTERNAL-view gate - the exhaust shimmer's domain, and the exact inverse of
	// viewGate. The shimmer is a WORLD effect (hot air bending light), not a physiological
	// one: from the cockpit the engines are behind you, and with no depth buffer a
	// screen-space warp in an internal view would smear the panel and window frame along
	// with the plume. So: internal = physiology, external = world.
	extGate = !oapiCameraInternal();

	// Round 3.5: the VIRTUAL-COCKPIT plasma gate (dialog VC toggle). The one deliberate
	// crack in the internal/external wall: the reentry GEOMETRY may also draw looking out
	// of the VC - and ONLY the VC. 2D panels and the glass cockpit stay clean (their flat
	// overlays would sit fully inside the glow with nothing reading as "outside").
	vcGate = g_fx.reentryVC && oapiCameraInternal()
	      && (oapiCockpitMode() == COCKPIT_VIRTUAL);

	// Viewport size for the render pass (tunnel geometry) - cached HERE because the
	// render callback makes no oapi calls by policy.
	oapiGetViewportSize(&viewW, &viewH);

	// Real-time step, shared by the scenario player and the animation clocks below.
	const double sysdt = oapiGetSysStep();

	// --- INDUCE scenario player -------------------------------------------
	// A one-click scripted sequence overwrites the effect sliders each frame. Consume a
	// button request (clicking the ACTIVE scenario toggles it off), then advance the
	// timeline and write the interpolated snapshot into g_fx. Runs BEFORE the capture-
	// texture gate so a scenario ramping grey-out/blur allocates the texture in time.
	if (g_fx.seqRequest >= 0) {
		const int  prev = g_fx.seqActive;
		const bool stop = (g_fx.seqRequest == g_fx.seqActive);
		g_fx.seqActive  = stop ? -1 : g_fx.seqRequest;
		g_fx.seqRequest = -1;
		seqT = 0.0;
		// Scenario sound: stop the previous clip, start the new one's (if the section's
		// Sound toggle is on). Each scenario's wav loads under SND_SCEN_BASE + its index.
		if (pXRSound && pXRSound->IsPresent()) {
			if (prev >= 0 && prev < NSCEN) pXRSound->StopWav(SND_SCEN_BASE + prev);
			if (g_fx.seqActive >= 0 && g_fx.seqSoundEnabled)
				pXRSound->PlayWav(SND_SCEN_BASE + g_fx.seqActive, false, 1.0f);
		}
		if (g_fx.seqActive >= 0) {
			// Force the driven effects enabled so the scenario always shows in full.
			g_fx.blackoutEnabled = g_fx.redoutEnabled = g_fx.tunnelEnabled = g_fx.spotsEnabled =
			g_fx.greyoutEnabled = g_fx.blurEnabled = g_fx.heartbeatEnabled = g_fx.aberrationEnabled =
			g_fx.sparklesEnabled = g_fx.swimEnabled = g_fx.tiltEnabled = true;
		} else {
			// Stopped early: clear the driven effects to zero at once.
			g_fx.blackout = g_fx.redout = g_fx.tunnel = g_fx.spots = g_fx.greyout =
			g_fx.blur = g_fx.heartbeat = g_fx.aberration = g_fx.sparkles = g_fx.swim = g_fx.tilt = 0.0f;
		}
	}
	if (g_fx.seqActive >= 0 && g_fx.seqActive < NSCEN) {
		const Scenario& sc = INDUCE_SEQ[g_fx.seqActive];
		const float t0 = (float)seqT;
		float out[FX_N];
		SeqSample(sc.keys, sc.n, t0, out);
		g_fx.blackout  = out[FX_BLK]; g_fx.redout     = out[FX_RED]; g_fx.tunnel   = out[FX_TUN];
		g_fx.spots     = out[FX_SPT]; g_fx.greyout    = out[FX_GRY]; g_fx.blur     = out[FX_BLR];
		g_fx.heartbeat = out[FX_HB];  g_fx.aberration = out[FX_AB];  g_fx.sparkles = out[FX_SPK];
		g_fx.swim      = out[FX_SWM]; g_fx.tilt       = out[FX_TLT];
		seqT += sysdt;
		const float t1 = (float)seqT;
		// Fire scripted blinks (stress blinks + recovery eye-flutter) crossed this step;
		// blinkRequest is consumed by the blink envelope below, same frame.
		for (int b = 0; b < sc.nblinks; b++)
			if (sc.blinks[b] > t0 && sc.blinks[b] <= t1) g_fx.blinkRequest = true;
		if (t1 > sc.dur) {
			if (sc.hold) {
				// INDUCE: hold the final state - clamp time so SeqSample keeps returning the
				// last key. The effects (and the induced state) persist until you RECOVER.
				seqT = sc.dur;
			} else {
				// RECOVER: arc complete - back to normal. Release the scenario + its sound.
				if (pXRSound && pXRSound->IsPresent()) pXRSound->StopWav(SND_SCEN_BASE + g_fx.seqActive);
				g_fx.seqActive = -1;
				g_fx.blackout = g_fx.redout = g_fx.tunnel = g_fx.spots = g_fx.greyout =
				g_fx.blur = g_fx.heartbeat = g_fx.aberration = g_fx.sparkles = g_fx.swim = g_fx.tilt = 0.0f;
			}
		}
	}

	// Sound toggle edge: turning the section's Sound OFF mid-scenario silences the clip now
	// (turning it back on won't restart a clip mid-way - it applies to the next scenario).
	if (pXRSound && pXRSound->IsPresent() && g_fx.seqActive >= 0
	    && seqSoundWasOn && !g_fx.seqSoundEnabled)
		pXRSound->StopWav(SND_SCEN_BASE + g_fx.seqActive);
	seqSoundWasOn = g_fx.seqSoundEnabled;

	// --- FELT-G PHYSICS ---------------------------------------------------
	// In PHYSICS mode this OVERWRITES the effect values from the vessel's real motion
	// (the sliders become gains); in LAB mode it returns immediately and the sliders
	// rule, exactly as the whole lab phase worked. Placed AFTER the scenario player
	// (which owns the values while it runs, and which the model refuses to fight) and
	// BEFORE the capture-texture gate below, so a model-driven grey-out or blur gets
	// its texture allocated on the same frame it appears.
	UpdatePhysics();

	// Reentry plasma: per-vessel, and the only effect family that needs NO client patch
	// (core Orbiter API - lights and particle streams, not gcCore). Like the camera shake
	// it BORROWS things owned by other vessels - light emitters, particle streams, and
	// stock's own reentry texture - so it must hand them all back when disarmed, which it
	// does itself on the way in. Runs BEFORE the capture-texture gate below so the cockpit
	// glow gets its texture on the same frame it appears.
	UpdateReentry();

	// Premium capture textures: created/resized here (an oapi resource op, main-thread
	// only) so the render callback can just copy the backbuffer into them. Maintained
	// only while a frame-resample effect (grey-out or blur) is actually calling for one.
	// THE PLUME MODEL: regime, strongest-6 selection and the four physics curves,
	// built ONCE for every consumer below (OroPlume.cpp). The LAB|PHYSICS switch
	// acts inside it; both modes are anchored identical at sea level, full throttle.
	BuildPlumeModel();

	// Exhaust shimmer: CONSUMER 2 - shapes its heat-haze capsules from the model
	// (merged 2026-08-09, his call), projects them to screen space here (main
	// thread - the render callback makes no oapi calls). Self-gates on extGate/
	// armed/enabled/strength/air and leaves plumeCount = 0 when there is nothing.
	UpdateShimmerPlumes();

	// PLUME EXPANSION: CONSUMER 1 - the jet geometry from the same model.
	UpdatePlumeFx();

	// EXHAUST PARTICLES: CONSUMER 3 - the DETACHED half (OroParticles.cpp). Ages
	// and spawns here on SIM time (smoke is a physical object); the projection runs
	// in the render path, because the pool is world-anchored and invariant 21b's
	// epoch law applies. Not view-gated: the cloud must exist whether or not you are
	// looking at it, or every camera change would show empty air.
	UpdateParticles(simdt);

	// STOCK EXHAUST pill (client patch n): keep the client's suppression pointed at
	// the right vessel - or at nobody. Pushes on CHANGE only; disarm hands stock back.
	UpdateStockExhaust();

	// BELL GLOW: the incandescent nozzle shells (OroBell.cpp) - config follows
	// the camera target's class, the shell is a borrowed AddMesh on the target,
	// the thermal model runs on SIM time. Self-releases on disarm/trim-0/no-mesh.
	UpdateBellGlow(simdt);

	// VC SHADOWS: hand the patched client its two knobs. Not an effect - ORO draws
	// nothing here - but it is part of the same immersion panel, so the dialog owns it.
	UpdateVCShadows();

	// ECLIPSE: solar-disc obscuration at the CAMERA plus the eye's response to it.
	// Cheap (a dot product per celestial body) and unconditional - it has to keep the
	// adaptation state tracking even while the effect is off, or switching it on inside
	// a shadow would invent a transient that never happened. Runs before the texture
	// gate below like everything else that can ask for a resample.
	UpdateEclipse();

	// GOD RAYS: where the sun is on screen, and whether there is any air to scatter in.
	// MUST follow UpdateEclipse - it reads g_fx.eclipseObsc so a transit or an eclipse
	// takes the shafts with it, and reading last frame's value would lag the sky.
	UpdateGodRays();

	// AURORA: build the curtain triangles for this frame (main thread - invariant 1).
	// Self-gates on enable/armed/atmospheric-planet-in-range. Additive geometry, no capture
	// texture - so it is NOT part of the EnsureFrameTex gate below.
	// depthClipOK is the CLIENT capability behind patch (g), probed by BINDING rather than
	// by build stamp (invariant 18a): the entry point is bound AND the depth buffer exists
	// this session (SunGlare on). TWO consumers now - it unlocks the VC and retires the
	// bounding-sphere fallback for the aurora, and cuts the plasma at the window frame in
	// the VC. Probed here, once per step, before either builder runs.
	depthClipOK = pCore && pCore->CanDrawDepth() && pCore->HasDepthBuffer();
	g_depthClipMirror = depthClipOK;   // dialog-visible (OroDepthClipOK)
	g_prtSupported    = prtTexMode;    // dialog-visible (OroParticleTintOK)
	// Announce it ONCE, on the first settled answer. Patches (b), (d) and (f) all log their
	// capability and (g) did not, which would leave a "nothing changed in the VC" report
	// ambiguous between "the clip never went live" and "it did, and did nothing visible" -
	// a whole fly-and-report round to disambiguate. Probed per step rather than at init
	// because the depth buffer belongs to the Scene, which does not exist until the render
	// window does; logged on CHANGE so that costs one line, not one per frame.
	if (depthClipLogged != (depthClipOK ? 1 : 0)) {
		depthClipLogged = depthClipOK ? 1 : 0;
		oapiWriteLogV("ORO: client depth clip (patch g) %s.",
		              depthClipOK ? "available - aurora + VC plasma clip per pixel"
		                          : "NOT available - screen-space overlay fallback");
	}
	UpdateAurora();

	// LIGHTNING: storm cells from the world's own cloud tiles, flash discs on the deck
	// (main thread - the file reads, oapi queries and projection all live here,
	// invariant 1). Self-gates on enable/armed/cloudy-world/above-the-deck; shares
	// depthClipOK with the aurora for the VC-window path.
	UpdateLightning(simt);

	// THE VAPOUR CONE: transonic condensation around the camera-target hull. Last of the
	// geometry builders because it is the only ALPHA-BLENDED one, and its draw has to go
	// FIRST - a cloud occludes what is behind it, so it must be laid down before the
	// additive layers add light on top (graveyard G11's shelved recipe, invariant 25).
	UpdateVapour();

	if (ipiReady && (eclActive ||
	                 (g_fx.greyoutEnabled    && g_fx.greyout    > 0.001f) ||
	                 (g_fx.blurEnabled       && g_fx.blur       > 0.001f) ||
	                 (g_fx.aberrationEnabled && g_fx.aberration > 0.001f) ||
	                 (g_fx.swimEnabled       && g_fx.swim       > 0.001f) ||
	                 (g_fx.tiltEnabled       && (g_fx.tilt > 0.001f || fabs(g_fx.tiltLean) > 0.001f)) ||
	                 (g_fx.reentryEnabled    && plasmaGlow > 0.001f) ||
	                 plumeCount > 0))
		EnsureFrameTex();

	// Animation clocks: REAL time, not sim time - the spot shimmer and the blink are
	// physiological, they must not warp with time acc. (sysdt computed above.)
	animT += (float)sysdt;

	// Blink envelope: close 0.10 s -> hold 0.06 s -> open 0.16 s. The dialog
	// button only REQUESTS; the envelope runs here and the renderer just reads
	// blinkAmount. A request during a running blink restarts it.
	if (g_fx.blinkRequest) {
		g_fx.blinkRequest = false;
		blinkT = 0.0;
	}
	if (blinkT >= 0.0) {
		blinkT += sysdt;
		const double tClose = 0.10, tHold = 0.06, tOpen = 0.16;
		if      (blinkT < tClose)                 g_fx.blinkAmount = (float)(blinkT / tClose);
		else if (blinkT < tClose + tHold)         g_fx.blinkAmount = 1.0f;
		else if (blinkT < tClose + tHold + tOpen) g_fx.blinkAmount = (float)(1.0 - (blinkT - tClose - tHold) / tOpen);
		else                                    { g_fx.blinkAmount = 0.0f; blinkT = -1.0; }
	}

	// Heartbeat cardiac envelope (REAL time), VARIABLE rate. The slider is a lab proxy
	// for exertion: rate climbs from a gentle ~55 bpm at low settings to a ~150 bpm
	// pound at max (physics phase: driven by felt-G instead). Phase accumulates at the
	// current rate each frame, so dragging the slider changes tempo smoothly without
	// stuttering. heartEnv peaks at systole and falls to ~0 between beats.
	{
		const double bpm = 55.0 + g_fx.heartbeat * (150.0 - 55.0);
		heartPhase += sysdt * (bpm / 60.0);             // advance by beats-elapsed
		heartPhase -= floor(heartPhase);                // wrap to 0..1
		const double p   = heartPhase;
		// Two Gaussians (systole + softer diastole), both narrow enough to sit at ~0
		// at the beat boundary so the throb starts cleanly from black, no wrap pop.
		const double lub = exp(-((p - 0.20) * (p - 0.20)) / (2.0 * 0.060 * 0.060));  // systole
		const double dub = 0.45 * exp(-((p - 0.40) * (p - 0.40)) / (2.0 * 0.070 * 0.070)); // diastole
		const double e   = lub + dub;
		heartEnv = (float)(e > 1.0 ? 1.0 : e);
	}

	// Heartbeat SOUND: fire a one-shot "lub-dub" per beat, re-triggered at the current
	// rate so the tempo tracks the visual throb (a fixed-BPM looped file would drift out
	// of sync as the rate changes). A "beat" = heartPhase crossing HB_TRIGGER, set just
	// before the visual lub peak (0.20) so the wav's attack lands with the throb. Main
	// thread here => PlayWav is safe (NEVER call it from the render proc). Module sounds
	// are Global (XRSound.h), so gate audibility exactly like the render callback gates the
	// visuals: cockpit view + master armed + heartbeat active. Stop-then-Play guarantees a
	// clean re-attack even when beats come faster than the file is long (XRSound otherwise
	// only adjusts volume on a re-Play of an already-playing sound).
	{
		const double HB_TRIGGER = 0.10;   // beat-wav onset phase (tunable; lower = sound leads the throb)
		const bool beat = (heartPhasePrev < HB_TRIGGER && heartPhase >= HB_TRIGGER);
		heartPhasePrev  = heartPhase;
		if (beat && pXRSound && pXRSound->IsPresent()
		         && viewGate && g_fx.masterArmed
		         && g_fx.heartbeatEnabled && g_fx.heartbeat > 0.001f) {
			const float in  = g_fx.heartbeat > 1.0f ? 1.0f : g_fx.heartbeat;
			const float vol = 0.25f + 0.75f * in;   // audible floor, then scales with intensity
			pXRSound->StopWav(SND_HEARTBEAT);
			pXRSound->PlayWav(SND_HEARTBEAT, false, vol);   // one-shot; tempo carried by the re-trigger
		}
	}

	// Camera shake (felt-G): reads the focus vessel's physics and perturbs its camera
	// offset. Main thread, no render path, no D3D9. Uses animT (advanced above).
	UpdateCameraShake();

	// FLIGHT AID: the CoP shift, the other place ORO reaches into the focus vessel.
	// Must run in a PreStep - AddForce applies to the NEXT step only.
	UpdateCopShift();

	// CANCEL THRUST: the test-stand rig - same AddForce mechanism, same PreStep rule.
	UpdateCancelThrust();

	// PER-CLASS SETTINGS: the plasma tuning belongs to the HULL, so it follows the
	// focus vessel's class. Cheap enough to test every frame - the load only fires
	// on an actual change of class, and the compare is a string that is already in
	// memory. (Focus, not camera target: "the vessel I am flying" is what the user
	// means by "when I use that vessel".)
	{
		VESSEL* fv = oapiGetFocusInterface();
		if (fv) {
			const char* cls = fv->GetClassName();
			if (cls && cls[0]) OroSettings_LoadClass(cls);
		}
	}
}

void OroModule::clbkDeleteVessel(OBJHANDLE hVessel)
{
	// Mandatory: we may be holding a LightEmitter* belonging to this vessel and the handle
	// dies the moment we return. See OroReentry.cpp.
	ReentryForget(hVessel);

	// Patch (n)'s borrow too: if this was the suppressed vessel, clear the client-side
	// entry while the handle is still valid (a stale entry is harmless - the client
	// only compares handles - but the discipline is return-on-every-exit-path).
	for (int i = 0; i < nStockExSupp; i++)
		if (hVessel == hStockExSupp[i]) { ReleaseStockExhaust(); break; }

	// The bell shell dies WITH the vessel - just forget it (no DelMesh on a
	// dying handle; the mesh instance is part of what is being destroyed).
	if (hVessel == bellVessel) { bellVessel = NULL; bellMeshIdx = (UINT)-1; }
}

bool OroModule::clbkProcessKeyboardBuffered(DWORD key, char kstate[256], bool simRunning)
{
	// Ctrl+G toggles the master arm - the keyboard panic/quick kill, mirroring the
	// dialog's ARMED switch. We consume the event ONLY when we act on it, so plain
	// 'G' (landing gear on most vessels) still passes through untouched.
	if (key == OAPI_KEY_G && KEYMOD_CONTROL(kstate)) {
		g_fx.masterArmed = !g_fx.masterArmed;
		oapiWriteLogV("ORO: master %s.", g_fx.masterArmed ? "ARMED" : "SAFE");
		return true;
	}
	return false;
}

// ----------------------------------------------------------------------------
// Premium pipeline setup (MAIN thread only). EnsureIPI compiles the shader once
// per session; EnsureFrameTex (re)allocates the capture texture on viewport change.
// ----------------------------------------------------------------------------

void OroModule::EnsureIPI()
{
	if (ipiTried) return;         // one shot: don't recompile a broken shader every frame
	ipiTried = true;
	if (!pCore) return;

	// On a stock/unpatched client the backbuffer-capture methods never bound -
	// stay dormant (the additive VISION effects still work; only the premium
	// resample effects need the patch).
	if (!pCore->CanCaptureBackBuffer()) {
		oapiWriteLogV("ORO: client exposes no backbuffer capture (unpatched D3D9Client?) - premium effects (grey-out, blur) OFF.");
		return;
	}
	ipiReady = true;   // the client can hand us the live frame

	// One gcIPInterface per pixel shader, kept SEPARATE on purpose: a compile
	// failure in one shader must not disable the other (ImageProcessing::IsOK()
	// fails an interface if ANY shader it holds failed to compile). VS is the
	// stock IPI.hlsl:VSMain, auto-selected; paths are relative to the Orbiter root.
	pIPIGrey   = pCore->CreateIPInterface("Modules/ORO/orofx.hlsl", "PSGrey", NULL, NULL);
	pIPIBlur   = pCore->CreateIPInterface("Modules/ORO/orofx.hlsl", "PSBlur", NULL, NULL);
	pIPIChroma = pCore->CreateIPInterface("Modules/ORO/orofx.hlsl", "PSChroma", NULL, NULL);
	pIPISwim   = pCore->CreateIPInterface("Modules/ORO/orofx.hlsl", "PSSwim", NULL, NULL);
	pIPITilt   = pCore->CreateIPInterface("Modules/ORO/orofx.hlsl", "PSTilt", NULL, NULL);
	pIPIShimmer = pCore->CreateIPInterface("Modules/ORO/orofx.hlsl", "PSShimmer", NULL, NULL);
	pIPIPlasma  = pCore->CreateIPInterface("Modules/ORO/orofx.hlsl", "PSPlasma", NULL, NULL);
	pIPIEclipse = pCore->CreateIPInterface("Modules/ORO/orofx.hlsl", "PSEclipse", NULL, NULL);
	pIPIGodRay  = pCore->CreateIPInterface("Modules/ORO/orofx.hlsl", "PSGodRay", NULL, NULL);

	oapiWriteLogV("ORO: premium IPI pipeline ready - grey-out %s, blur %s, aberration %s, swim %s, tilt %s, shimmer %s, plasma %s, eclipse %s, god rays %s.",
	              pIPIGrey   ? "live" : "FAILED (shader compile?)",
	              pIPIBlur   ? "live" : "FAILED (shader compile?)",
	              pIPIChroma ? "live" : "FAILED (shader compile?)",
	              pIPISwim   ? "live" : "FAILED (shader compile?)",
	              pIPITilt   ? "live" : "FAILED (shader compile?)",
	              pIPIShimmer ? "live" : "FAILED (shader compile?)",
	              pIPIPlasma ? "live" : "FAILED (shader compile?)",
	              pIPIEclipse ? "live" : "FAILED (shader compile?)",
	              pIPIGodRay ? "live" : "FAILED (shader compile?)");
}

// ----------------------------------------------------------------------------
// VC SHADOWS (client patch f). MAIN thread. ORO renders nothing here - the
// client's internal-pass shadow map does the work - so this is purely a setter,
// pushed on CHANGE rather than per frame.
//
// Ctrl+G is honoured: the master kill means "stop everything ORO is doing to
// this session", and a shadow pass we switched on is something ORO is doing.
// Disarming hands the client back its stock behaviour (fully-lit VC).
// ----------------------------------------------------------------------------
void OroModule::UpdateVCShadows()
{
	if (!pCore || !vcShadowSupported) return;
	const bool  want = g_fx.vcShadows && g_fx.masterArmed;
	const float rad  = g_fx.vcShadowRadius;
	const float dep  = g_fx.vcShadowDepth;
	if (want == vcShadowLastOn && fabs(rad - vcShadowLastRad) < 0.001f
	                           && fabs(dep - vcShadowLastDep) < 0.001f) return;
	pCore->SetVCShadows(want, rad, dep);
	vcShadowLastOn  = want;
	vcShadowLastRad = rad;
	vcShadowLastDep = dep;
}

void OroModule::EnsureFrameTex()
{
	if (!ipiReady || viewW == 0 || viewH == 0) return;
	if (hFrameTex && hBlurTex && texW == viewW && texH == viewH) return;   // already the right size

	if (hFrameTex) { oapiDestroySurface(hFrameTex); hFrameTex = NULL; }
	if (hBlurTex)  { oapiDestroySurface(hBlurTex);  hBlurTex  = NULL; }

	// Render-target TEXTURES: each must be a valid StretchRect/render destination
	// AND sampleable by a shader (SetTexture needs GetTexture()). No mipmaps -
	// sampled 1:1 at full-frame resolution. hFrameTex = backbuffer copy (grey
	// input / blur H-pass input); hBlurTex = blur intermediate (H out -> V in).
	const DWORD attr = OAPISURFACE_TEXTURE | OAPISURFACE_RENDERTARGET | OAPISURFACE_NOMIPMAPS;
	hFrameTex = oapiCreateSurfaceEx((int)viewW, (int)viewH, attr);
	hBlurTex  = oapiCreateSurfaceEx((int)viewW, (int)viewH, attr);
	if (hFrameTex && hBlurTex) {
		texW = viewW; texH = viewH;
	} else {
		if (hFrameTex) { oapiDestroySurface(hFrameTex); hFrameTex = NULL; }
		if (hBlurTex)  { oapiDestroySurface(hBlurTex);  hBlurTex  = NULL; }
		texW = texH = 0;
		oapiWriteLogV("ORO: oapiCreateSurfaceEx(%ux%u) for premium capture FAILED - grey-out/blur OFF this size.", viewW, viewH);
	}
}

// ----------------------------------------------------------------------------
// Camera shake (felt-G) - tunables + buffet noise. MAIN-thread only. These are the
// knobs the cfg file will expose; good defaults for now.
// ----------------------------------------------------------------------------
namespace {
	const double SHAKE_PUSH_K     = 0.002;    // eyepoint shift [m] per m/s^2 of felt accel (seat-push)
	const double SHAKE_PUSH_MAX   = 0.05;     // max seat-push displacement [m]
	const double SHAKE_THRUST_REF = 15.0;     // thrust accel [m/s^2] for full engine buffet
	const double SHAKE_DYNP_REF   = 30000.0;  // dynamic pressure [Pa] for full aero buffet
	const double SHAKE_GS_REF     = 80.0;     // groundspeed [m/s] for full runway rumble
	const double SHAKE_GROUND_AMP = 1.0;      // extra engine buffet while rolling on the ground
	const double SHAKE_MIN        = 1.0e-4;   // total delta below this -> hold nothing (release)
	// SHAKE_RESET (the "external change" offset jump) lives in OroModule.h - the felt-G
	// model needs the same value to recover the same clean camera offset.
	// Per-axis amplitude + base frequency are LIVE knobs now (g_fx.shakeAmp*/shakeFreq).

	inline double sat01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

	// Layered sines per axis -> a rattly buffet in ~[-1,1] (not a pure hum). freq in Hz.
	inline double shakeNoise(double t, int axis, double freq) {
		const double p = axis * 2.1;
		const double w = freq * 6.2831853;
		return 0.60 * sin(t * w       + p)
		     + 0.30 * sin(t * w * 2.7 + p * 1.7 + 1.3)
		     + 0.10 * sin(t * w * 5.3 + p * 0.5 + 2.9);
	}
}

void OroModule::ReleaseCameraShake()
{
	if (!camActive) return;
	VESSEL* v = oapiGetFocusInterface();
	if (v) {
		VECTOR3 cur; v->GetCameraOffset(cur);
		// Recover the clean base (remove our delta), unless it jumped externally.
		VECTOR3 base = (length(cur - camApplied) > SHAKE_RESET) ? cur : (cur - camDelta);
		v->SetCameraOffset(base);   // hand the clean offset back to the vessel
	}
	camActive = false;
	camDelta  = _V(0, 0, 0);
}

// ----------------------------------------------------------------------------
// FLIGHT AID - a live CENTRE-OF-PRESSURE shift on the FOCUS vessel (2026-08-02).
//
// The problem it solves: stock vessels are trimmed to weathervane. The DG's wing
// CoP sits 0.3 m aft of the CG, which at 25 deg AoA is ~18.8*q of nose-down moment
// against ~20.8*q of full elevator + trim - so the nose drops into the flight
// direction and the entry makes no plasma. Atlantis behaves the same way. Editing
// each vessel's CreateAirfoil3 call and recompiling does not scale.
//
// Why not the airfoil API: EditAirfoil() can move a CoP, but it needs an
// AIRFOILHANDLE, and a handle only ever goes to the vessel that called
// CreateAirfoil2/3/4 - nothing enumerates another vessel's airfoils. From a global
// module they are unreachable.
//
// What this does instead: moving a CoP by d changes ONE thing - the moment gains
// (0,0,d) x F, with the net force unchanged. That is reproducible from outside as a
// pure COUPLE: +F at (0,0,d) and -F at the origin. Net force zero (AddForce applies
// both), net torque identical to the real shift. Taking F as the vessel-frame
// VERTICAL aero force gives exactly the pitch term and touches nothing else - yaw
// stability is left alone deliberately, so the knob does one thing.
//
// It self-scales like the real thing: the couple rides the vessel's OWN lift+drag,
// so it grows with dynamic pressure at the same rate as the moment it cancels, at
// every altitude, on any vessel, with no per-vessel tuning.
//
// Two honest limits. (1) GetLift/DragVector report the LAST step's forces, so the
// couple lags one frame - irrelevant at 1x-10x, sloppier under heavy time
// acceleration. (2) It is gated on masterArmed like everything else, so Ctrl+G in
// the middle of an entry hands the airframe's full stability back at once and the
// nose WILL drop. That is the panic button doing its job, but it is worth knowing
// before pressing it at Mach 20.
// ----------------------------------------------------------------------------
void OroModule::UpdateCopShift()
{
	g_fx.copMoment = 0.0f;
	const double d = (double)g_fx.copShift;
	if (!g_fx.masterArmed || fabs(d) < 0.001) return;   // 0 = the vessel exactly as coded
	VESSEL* v = oapiGetFocusInterface();
	if (!v) return;

	// The vertical aero force in VESSEL coordinates. GetLiftVector is documented as
	// perpendicular to the relative wind with zero x-component (side force is a
	// separate accessor), so this is the pitch-relevant force and nothing else; the
	// drag term matters because at high AoA its y-component is a large part of it -
	// and a real CoP is where lift AND drag are applied.
	VECTOR3 L = _V(0, 0, 0), D = _V(0, 0, 0);
	v->GetLiftVector(L);
	v->GetDragVector(D);
	const double Fy = L.y + D.y;                        // [N]
	if (fabs(Fy) < 1.0) return;                         // vacuum / no lift - nothing to shift

	// (0,0,d) x (0,Fy,0) = (-d*Fy, 0, 0). Mx>0 is nose-DOWN, so d>0 (CoP forward)
	// with positive lift is nose-UP: the knob reads the way it flies.
	v->AddForce(_V(0,  Fy, 0), _V(0, 0, d));
	v->AddForce(_V(0, -Fy, 0), _V(0, 0, 0));
	g_fx.copMoment = (float)(d * Fy * 1e-3);            // kN m, + = nose-up
}

// ----------------------------------------------------------------------------
// CANCEL THRUST - the test-stand rig (user request 2026-08-09, for plume tuning:
// the DG at full throttle rolled off the runway before the sliders got a fair
// try). The vessel's own TOTAL thrust vector, negated, applied at the CoM every
// step - invariant 9's mechanism exactly: AddForce lives one timestep, so there
// is nothing to hand back, and disarm/Ctrl+G or the pill releases the ship on
// the next step. Thrust FORCE only: engine/RCS torque survives, which on a
// balanced vessel is ~zero and keeps the attitude controls honest while held.
// SESSION-ONLY by design - see the note on g_fx.cancelThrust.
// ----------------------------------------------------------------------------
void OroModule::UpdateCancelThrust()
{
	if (!g_fx.masterArmed || !g_fx.cancelThrust) return;
	VESSEL* v = oapiGetFocusInterface();
	if (!v) return;
	VECTOR3 T = _V(0, 0, 0);
	v->GetThrustVector(T);                              // total, pressure-adjusted [N]
	if (length(T) < 1.0) return;                        // engines idle - nothing to null
	v->AddForce(-T, _V(0, 0, 0));
}

void OroModule::UpdateCameraShake()
{
	// Only in an internal cockpit view, armed, and enabled - else release any hold.
	if (!(viewGate && g_fx.masterArmed && g_fx.shakeEnabled)) { ReleaseCameraShake(); return; }
	VESSEL* v = oapiGetFocusInterface();
	if (!v)                                                    { ReleaseCameraShake(); return; }
	const double m = v->GetMass();
	if (m < 1.0)                                               { ReleaseCameraShake(); return; }

	// Felt (non-gravitational) acceleration in vessel-LOCAL coords: thrust + lift + drag,
	// all local [N], over mass. Gravity is excluded, so parked on the ground = 0.
	VECTOR3 T = _V(0,0,0), L = _V(0,0,0), D = _V(0,0,0);
	v->GetThrustVector(T);
	v->GetLiftVector(L);
	v->GetDragVector(D);
	const VECTOR3 feltAcc = (T + L + D) / m;   // m/s^2

	// (1) Seat-push: the eyepoint shifts OPPOSITE the felt accel (main -> back into the
	// seat, hover -> down, retro/reentry-decel -> forward). Clamped, so even monster
	// thrusters give a firm-but-bounded shove.
	VECTOR3 push = feltAcc * (-SHAKE_PUSH_K);
	const double pl = length(push);
	if (pl > SHAKE_PUSH_MAX) push = push * (SHAKE_PUSH_MAX / pl);

	// (2) Buffet intensity: engine roughness (amplified rolling on the ground) + aero
	// buffet (dynamic pressure) + runway rumble; max'd with the manual test slider.
	const double thrustAcc = length(T) / m;
	const bool   onGround  = v->GroundContact();
	const double eng  = sat01(thrustAcc / SHAKE_THRUST_REF);
	const double aero = sat01(v->GetDynPressure() / SHAKE_DYNP_REF);
	const double roll = onGround ? sat01(v->GetGroundspeed() / SHAKE_GS_REF) : 0.0;
	double intensity = eng * (1.0 + (onGround ? SHAKE_GROUND_AMP : 0.0)) + aero + 0.3 * roll;
	intensity = sat01(intensity);
	if (g_fx.shakeTest) intensity = 1.0;   // dialog Test toggle: full-power preview at the tuned settings

	// Per-axis amplitude [m] and base frequency [Hz] are the live CAM-SHAKE dialog knobs.
	const double fr = g_fx.shakeFreq;
	VECTOR3 jit;
	jit.x = shakeNoise(animT, 0, fr) * g_fx.shakeAmpX * intensity;
	jit.y = shakeNoise(animT, 1, fr) * g_fx.shakeAmpY * intensity;
	jit.z = shakeNoise(animT, 2, fr) * g_fx.shakeAmpZ * intensity;

	const VECTOR3 delta = push + jit;

	// Nothing meaningful -> release and let the vessel own its offset.
	if (length(delta) < SHAKE_MIN) { ReleaseCameraShake(); return; }

	// Apply on top of the vessel's clean base offset. Recover base by removing our last
	// delta; if the offset JUMPED (a pilot/copilot switch reset it), adopt it as the base.
	VECTOR3 cur; v->GetCameraOffset(cur);
	VECTOR3 base = (!camActive || length(cur - camApplied) > SHAKE_RESET) ? cur : (cur - camDelta);
	const VECTOR3 applied = base + delta;
	v->SetCameraOffset(applied);
	camApplied = applied;
	camDelta   = delta;
	camActive  = true;
}

// ----------------------------------------------------------------------------
// Exhaust shimmer: build this frame's screen-space plume table (main thread).
// ----------------------------------------------------------------------------
namespace {
	// Project a GLOBAL position to viewport UV. oapiCameraRotationMatrix gives
	// camera->global, so tmul() applies the inverse (global->camera). Orbiter's camera
	// frame looks along +z, and oapiCameraAperture() is the VERTICAL SEMI-aperture, so
	// the frustum half-height at depth z is z*tan(ap) and the half-width is that x aspect.
	// Returns false behind/too near the camera. zOut = camera-frame depth [m].
	bool ProjectToUV(const VECTOR3& gpos, const VECTOR3& cpos, const MATRIX3& Rcam,
	                 double tanAp, double aspect, float& u, float& v, double& zOut)
	{
		const VECTOR3 c = tmul(Rcam, gpos - cpos);     // global -> camera frame
		if (c.z < 0.5) return false;                    // behind the camera (or on top of it)
		zOut = c.z;
		u = (float)(0.5 + 0.5 * ((c.x / c.z) / (tanAp * aspect)));
		v = (float)(0.5 - 0.5 * ((c.y / c.z) /  tanAp));          // UV y grows downward
		return true;
	}
}

void OroModule::UpdateShimmerPlumes()
{
	plumeCount = 0;
	if (!extGate || !g_fx.masterArmed) return;                    // EXTERNAL view only
	if (!g_fx.shimmerEnabled || g_fx.shimmer <= 0.001f) return;
	if (viewW == 0 || viewH == 0) return;
	if (plmModelN <= 0) return;                                   // nothing burning

	// CONSUMER 2 OF THE PLUME MODEL (2026-08-09, his call: "bring in the shimmer
	// into the physics"). The exhaust scan, the strongest-6 selection and the
	// capsule's SHAPE all come from BuildPlumeModel now, so the haze follows
	// whatever the physics (and the Width/Length knobs) decided the jet IS this
	// frame - haze and jet can never disagree. What stays the shimmer's own: the
	// atmosphere gate (haze needs AIR, whatever the jet does), the geometric
	// facing occlusion (IPI has no depth buffer - invariant 11), the saturating
	// thrust response and the aft-migrating turbulence peak (both lab-tuned
	// 2026-07-30 and untouched).
	const double rho = plmRho;                                    // model-published
	if (rho < 1.0e-4) return;
	const float atmW = (float)(rho > 0.02 ? 1.0 : rho / 0.02);

	VECTOR3 cpos; oapiCameraGlobalPos(&cpos);
	MATRIX3 Rcam; oapiCameraRotationMatrix(&Rcam);
	const double tanAp  = tan(oapiCameraAperture());
	const double aspect = (double)viewW / (double)viewH;

	for (int p = 0; p < plmModelN && plumeCount < MAX_PLUMES; p++) {
		const PlumeModel& e = plmModel[p];

		// Capsule from the model: root at the nozzle plus the Offset knob along
		// the flow axis; tip at the model's jet length.
		VECTOR3 groot = e.rootG + e.dirG * g_fx.shimmerOfs;
		VECTOR3 gtip  = groot + e.dirG * e.L;

		// HULL OCCLUSION (geometric - see the old scan's comment, preserved in
		// spirit): behind ~ -1 clear, side ~ 0 clear, front ~ +1 means the nozzle
		// sits past the hull and warping would ripple the NOSE [lab 2026-07-30].
		// Judged on the UNCLAMPED nozzle position - the clamp below moves points
		// for projection only, and hull facing is a fact about the real nozzle.
		const VECTOR3 c2p = groot - cpos;
		const double  lc  = length(c2p);
		if (lc < 1e-6) continue;
		const double facing = dotp(e.dirG, c2p / lc);
		const double OCC0 = 0.20, OCC1 = 0.60;                    // fade band (tune here)
		double vis = 1.0;
		if (facing >= OCC1)      vis = 0.0;
		else if (facing > OCC0)  { const double t = (facing - OCC0) / (OCC1 - OCC0); vis = 1.0 - t * t * (3.0 - 2.0 * t); }
		if (vis < 0.01) continue;                                 // fully behind the hull

		// NEAR-PLANE CLAMP (the plume's close-camera lesson, 2026-08-09): pull the
		// endpoint that fell behind the camera back to the near plane instead of
		// skipping the whole capsule - a close pass used to pop the haze off with
		// the jet. z is linear along the axis; ProjectToUV's floor is z >= 0.5.
		{
			const double ZN = 0.6;
			const double zr = tmul(Rcam, groot - cpos).z;
			const double zt = tmul(Rcam, gtip  - cpos).z;
			if (zr < ZN && zt < ZN) continue;               // entirely behind the camera
			if (zr < ZN)      groot = groot + (gtip - groot) * ((ZN - zr) / (zt - zr));
			else if (zt < ZN) gtip  = groot + (gtip - groot) * ((ZN - zr) / (zt - zr));
		}

		float ax, ay, bx, by;
		double za, zb;
		if (!ProjectToUV(groot, cpos, Rcam, tanAp, aspect, ax, ay, za)) continue;
		if (!ProjectToUV(gtip,  cpos, Rcam, tanAp, aspect, bx, by, zb)) continue;

		// Cull plumes entirely off-screen (with margin - the haze spreads past the axis).
		const float lo = -0.35f, hi = 1.35f;
		if ((ax < lo && bx < lo) || (ax > hi && bx > hi)) continue;
		if ((ay < lo && by < lo) || (ay > hi && by > hi)) continue;

		// Haze radius: the frustum spans 2*z*tan(ap) metres vertically at depth z.
		// wRef is the model's EFFECTIVE nozzle width (stock wsize x the Width
		// knob), envelope 1.8x wider than the visible jet as ever.
		const double zavg = 0.5 * (za + zb);
		const float  rad  = (float)((e.wRef * 1.8) / (2.0 * zavg * tanAp));
		if (rad < 0.002f) continue;                               // sub-pixel

		PlumeScr& s = plumes[plumeCount++];
		s.ax = ax; s.ay = ay; s.bx = bx; s.by = by;
		s.rad = rad;
		// Strength SATURATES with thrust (even an idling engine bends light hard);
		// the turbulence peak migrates aft as thrust rises. Both lab-tuned laws.
		s.str = atmW * (float)(vis * pow(e.level, 0.40));
		s.hpk = (float)(0.15 + 0.45 * e.level);
	}
	for (int i = plumeCount; i < MAX_PLUMES; i++) plumes[i] = PlumeScr{};   // unused: str = 0
}

// ----------------------------------------------------------------------------
// Render callback body - runs mid-frame on the render (main) thread, with the
// Sketchpad bound to the backbuffer. Draws the whole effect stack in physiological
// layering order: grey-out FIRST (a true frame RESAMPLE through the IPI/HLSL
// pipeline - it rewrites the frame's pixels), then the additive Sketchpad washes
// (red-out, blackout, dark spots, tunnel) and finally the eyelids over everything.
//
// Makes NO oapi calls (the view gate + viewport size are precomputed in clbkPreStep
// and the capture texture is (re)allocated there too). The Sketchpad batch calls
// (SetBlendState/ColorFill/DrawPoly) only set state and enqueue vertices; the IPI
// Execute manages and restores its own device render targets (bInScene=true).
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// Pre-resolve pass (client patch i) - the reentry plasma's compositing point since
// the Firefly rework (2026-08-08). Fires after the COMPLETE scene (terrain, vessels,
// transparency, VC) and before the client's light-blur resolve + tonemap + HUD:
//  - with PostProcess=1 the target is the fp16 offscreen buffer, so the additive
//    plasma ACCUMULATES past 1.0 where its layers stack and the client's own
//    threshold bloom (GFXThreshold, default 1.1) blooms exactly the hottest zones,
//    then the soft tonemap rolls the result toward white. This is the mechanism
//    Firefly's whole look depends on (its colors are authored up to 8x and white is
//    never in any palette - it EMERGES from HDR accumulation), and the round-5.5
//    "red pins at 255" law was a symptom of drawing post-tonemap, not a palette fact.
//  - with PostProcess=0 the target is the plain backbuffer: same look as before,
//    now under the HUD instead of over it.
// Same rules as DrawOverlay: no oapi calls, gates precomputed in clbkPreStep.
// ----------------------------------------------------------------------------

void OroModule::DrawPreResolve(oapi::Sketchpad* pSkp)
{
	// Latch FIRST, unconditionally: this firing at all proves the patch-(i) slot exists
	// in the running client, and DrawOverlay must stop drawing the plasma in the old
	// HUD_2ND position. PRE_RESOLVE fires before HUD_2ND within the same frame, so the
	// hand-off has no one-frame double-draw or gap.
	preResolveLive = true;

	if (!pSkp || !g_fx.masterArmed) return;

	// The same geometry, the same two view domains as before (invariant 10): external,
	// or looking out of the virtual cockpit. Only the plasma moved here - the eclipse,
	// shimmer and aurora stay in DrawOverlay (they are frame resamples / settled looks;
	// one variable per round). Note two consequences of the move, both acceptable and
	// both physically right: the eclipse's eye-gain at HUD_2ND now dims a frame that
	// already CONTAINS the plasma (the eye responds to the plasma too), and the exhaust
	// shimmer's heat-haze warp now includes it (plasma seen through exhaust shimmers).
	// DEPTH CLIP IS ON EXTERNALLY TOO since 2026-08-08 - the "one-argument experiment"
	// the round-5.1 comment reserved, finally run for the fin system: far-side fins
	// painted straight through the hull ("streaks visible through the hull", user's
	// Atlantis/DG report), because fins keep a 0.40 facing floor where the old ribbon
	// roots had camera-facing selection. Per-pixel scene depth cuts them at the hull
	// exactly like the aurora already does externally. Degrades to the old paint-over
	// when depthClipOK is false (SunGlare off / unpatched client).
	// The TRAIL draws first (both additive, so order between them is cosmetic - but the
	// phase-2 smoke layer will be alpha-blended and MUST precede the additive draws, so
	// the trail family's slot is established ahead of the plasma's now). ProjectTrail
	// runs HERE, in the render path, because only the renderer knows the true camera
	// (round 4 / patch k - see its comment block in OroReentry.cpp).
	// THE VAPOUR CONE GOES FIRST OF EVERYTHING HERE, and the order is load-bearing rather
	// than cosmetic. It is the only ALPHA-BLENDED layer ORO draws: it REPLACES what is
	// behind it in proportion to its opacity. Draw it after the additive layers and it
	// would dim the plume and the plasma it should have been sitting in front of; draw it
	// first and the additive layers correctly add their light over the cloud. This is
	// G11's shelved recipe as written - "before the additive one".
	// External only (invariant 10): UpdateVapour self-gates, so vapActive is false in any
	// internal view and this costs a branch.
	if (extGate && vapActive) DrawVapourPoly(pSkp);
	if (extGate || vcGate) {
		ProjectTrail();
		DrawTrailPoly(pSkp, /*depthClip=*/true);
		DrawPlasmaPoly(pSkp, /*depthClip=*/true);
	}
	// PLUME EXPANSION - external only (invariant 10: your own engines are behind the
	// cockpit). Pre-bloom is exactly where the diamond cores want to composite: the
	// fp16 chain accumulates them past 1.0 and the client's threshold bloom whitens
	// them (the Firefly law) - and the shimmer's resample runs later in DrawOverlay,
	// so the diamonds ripple through their own heat haze, which is physically right.
	if (extGate) DrawPlumePoly(pSkp, /*depthClip=*/true);
}

void OroModule::DrawOverlay(oapi::Sketchpad* pSkp)
{
	// One-shot diagnostic, kept: proves the callback fired at least once this run.
	// (This line is how we confirmed the D3D9Client SetViewProj(NULL,NULL) CTD: with the
	// stock client it never printed - the crash was in the invocation preamble, before
	// any ORO code. Requires the patched client, Build >= 260725.)
	static bool loggedOnce = false;
	if (!loggedOnce) {
		oapiWriteLogV("ORO: DrawOverlay first invocation - render callback is live.");
		loggedOnce = true;
	}

	// THE SCENE HAS RENDERED A FRAME. Reset per session, and it gates anything that
	// hands the CORE or the CLIENT a long-lived object - see UpdateParticles.
	// RenderMainScene returns early (before ever reaching the HUD stages this callback
	// runs from) while the reloaded scene has no focus visual, which on a scenario
	// RELOAD is a window of well over a second. Getting here proves that window is over.
	sceneRendered = true;

	if (!pSkp || !g_fx.masterArmed) return;

	// --- EXTERNAL view: the ENVIRONMENT effects ---------------------------
	// The exhaust shimmer is a WORLD effect, so it is the one thing ORO draws OUTSIDE
	// the cockpit - and the only thing it draws there (nothing physiological applies when
	// you are not looking through the pilot's eyes). It also runs FIRST in the overall
	// resample order by construction: the world shimmers, then - in an internal view -
	// the pilot's physiology layers on top. plumeCount > 0 already implies extGate,
	// armed, enabled, in-atmosphere and at least one lit engine on screen (clbkPreStep).
	if (extGate) {
		// ECLIPSE first of all - it sets the light the rest of the frame is seen by.
		DrawEclipsePass();

		// GOD RAYS second: still world illumination, but they ADD light to the frame
		// the eye has just decided the brightness of, so they must follow the eclipse
		// rather than precede it (a shaft does not dim because you are adapting - it is
		// part of what you are adapting TO).
		DrawGodRayPass();

		if (ipiReady && pCore && hFrameTex && pIPIShimmer && plumeCount > 0) {
			SURFHANDLE hBB = pCore->GetBackBufferHandle();
			if (hBB && pCore->CopyResource(hFrameTex, hBB)) {
				// Plume table -> shader arrays (MAX_PLUMES entries; unused slots carry str 0).
				float axes[MAX_PLUMES * 4], prm[MAX_PLUMES * 4];
				for (int i = 0; i < MAX_PLUMES; i++) {
					axes[i * 4 + 0] = plumes[i].ax; axes[i * 4 + 1] = plumes[i].ay;
					axes[i * 4 + 2] = plumes[i].bx; axes[i * 4 + 3] = plumes[i].by;
					prm[i * 4 + 0]  = plumes[i].rad; prm[i * 4 + 1] = plumes[i].str;
					prm[i * 4 + 2]  = plumes[i].hpk; prm[i * 4 + 3] = 0.0f;
				}
				pIPIShimmer->SetTexture("tSrc", hFrameTex, IPF_CLAMP_U | IPF_CLAMP_V | IPF_LINEAR);
				pIPIShimmer->SetOutput(0, hBB);
				pIPIShimmer->SetFloat("fShimmer", g_fx.shimmer);
				pIPIShimmer->SetFloat("fAspect", (float)viewW / (float)viewH);
				pIPIShimmer->SetFloat("fTime", animT);       // real-time clock, streams the ripple
				pIPIShimmer->SetFloat("vPlume",  axes, sizeof(axes));
				pIPIShimmer->SetFloat("vPlumeP", prm,  sizeof(prm));
				pIPIShimmer->Execute((DWORD)0, true, gcIPInterface::Rect);
			}
		}

		// --- REENTRY PLASMA geometry (envelope + filaments + wake) ------------
		// Built and PROJECTED in clbkPreStep (UpdateReentry - invariant 1: this callback
		// makes no oapi calls); here we only push the vertices and draw. Drawn AFTER the
		// shimmer resample so the plasma stays crisp on top of any heat haze. ADDITIVE
		// (client patch d) because plasma is light - it adds to the frame; on a client
		// without the patch it falls back to alpha blending (tints, degraded not broken).
		// The full-buffer-update rule (the D3DLOCK_DISCARD lesson) lives with the
		// shared helper - see DrawPlasmaPoly in OroModule.h.
		// Depth clip ON externally since 2026-08-08 (see DrawPreResolve) - the round-5.1
		// "one-argument experiment" run for the fin system's through-hull streaks. If it
		// holds, invariant 16's camera-space map can retire.
		// FALLBACK SLOT since the Firefly rework: on a patch-(i) client the plasma draws
		// in DrawPreResolve (pre-bloom, pre-HUD) and preResolveLive skips this call.
		if (!preResolveLive) {
			ProjectTrail();
			DrawTrailPoly(pSkp, /*depthClip=*/true);
			DrawPlasmaPoly(pSkp, /*depthClip=*/true);
			DrawPlumePoly(pSkp, /*depthClip=*/true);   // plume expansion rides the same
			                                           // fallback slot (post-shimmer here,
			                                           // so no haze on it - degraded, not
			                                           // broken, like the plasma)
		}

		// AURORA - the curtains in the world, additive geometry like the plasma. Drawn
		// LAST here (the proven external Sketchpad slot, where the plasma sits): after the
		// eclipse/shimmer resamples, so it is not eye-adapted externally, which is fine -
		// it is a light source. Both are additive, so their order relative to each other
		// does not matter. Built + projected in clbkPreStep (UpdateAurora).
		if (aurActive) DrawAuroraPoly(pSkp);

		// LIGHTNING - flash discs in the cloud deck, additive like the aurora and
		// drawn beside it for the same reasons (order between additive layers is
		// cosmetic; both are light sources the eclipse's eye need not protect).
		if (ltgActive) DrawLightningPoly(pSkp);
		return;   // nothing physiological outside the cockpit
	}

	if (!viewGate) return;

	// --- REENTRY PLASMA in the VIRTUAL COCKPIT (round 3.5, the dialog VC toggle).
	// The same world geometry, drawn looking OUT. vcGate is true only in the VC
	// (never 2D panel / glass cockpit) and only while the toggle is on - computed
	// in clbkPreStep (invariant 1). Drawn FIRST in the internal stack so the IPI
	// resamples (blur, grey-out, swim...) treat it as part of the world and the
	// physiological washes darken it like everything else.
	// DEPTH-CLIPPED HERE (patch g, 2026-08-07), which closes the round-3.5 caveat that
	// stood on this line for five days: "no depth buffer - the glow crosses window frames
	// and reads as bloom on the glass". It no longer does. The streaks the user reported
	// "rendered inside the cockpit instead of around it" are now cut per PIXEL at the
	// window frame, because the client's GBUF_DEPTH pass includes the cockpit. Without
	// patch (g) depthClipOK is false and this degrades to exactly the old overlay.
	// FALLBACK SLOT since the Firefly rework, same rule as the external branch: a
	// patch-(i) client draws the VC plasma in DrawPreResolve instead.
	if (vcGate && !preResolveLive) {
		ProjectTrail();
		DrawTrailPoly(pSkp, /*depthClip=*/true);
		DrawPlasmaPoly(pSkp, /*depthClip=*/true);
	}

	// --- AURORA through the cockpit windows (patch g) --------------------
	// Now that the curtains are depth-clipped against the scene (depthClipOK), they draw in
	// the VC too - occluded by the cockpit frame per pixel, visible through the glass. Drawn
	// BEFORE the resample stack like the VC plasma, so blur/grey-out/eclipse treat them as
	// sky. Gated on depthClipOK: without real depth this would paint the cabin, so it stays
	// external (UpdateAurora built nothing for an internal view in that case anyway).
	if (aurActive && depthClipOK) DrawAuroraPoly(pSkp);

	// --- LIGHTNING through the cockpit windows (patch g) ------------------
	// Same rule as the aurora directly above: with per-pixel depth the flash discs
	// sit behind the frame and glass; without it UpdateLightning built nothing for
	// an internal view. No cabin illumination in v1 - this is only the world,
	// visible out the window.
	if (ltgActive && depthClipOK) DrawLightningPoly(pSkp);

	// --- Premium frame RESAMPLE stack (IPI/HLSL) --------------------------
	// Runs BEFORE any additive wash: these rewrite the frame's own pixels, and
	// blackout/red-out/spots/tunnel/blink then layer ON TOP of the resampled
	// frame. Each stage captures the CURRENT backbuffer (a StretchRect copy -
	// can't sample the surface we render to) and writes it back, so they
	// compose. Execute is told bInScene=true (we're already mid-frame) and it
	// saves/restores the render target. Skipped wholesale on an unpatched
	// client (ipiReady == false).
	if (ipiReady && pCore && hFrameTex && hBlurTex) {

		// ECLIPSE - the world's illumination, so it goes ahead of everything the pilot
		// does to it. Drawn after the VC plasma poly above deliberately: they are the
		// same kind of thing (world content), and a reentry inside a shadow is not a
		// coincidence worth contorting the order for.
		DrawEclipsePass();

		// GOD RAYS - world light too, and they come through the window like anything
		// else out there, so the physiological stack below treats them as scenery. After
		// the eclipse for the reason given at the external call site.
		DrawGodRayPass();

		// PERIPHERAL SWIM - a woozy periphery-weighted UV warp. First in the stack, so
		// the geometric distortion happens before the other resamples process the frame.
		//   backbuffer -> hFrameTex (copy) -> PSSwim -> backbuffer
		if (pIPISwim && g_fx.swimEnabled && g_fx.swim > 0.001f) {
			SURFHANDLE hBB = pCore->GetBackBufferHandle();
			if (hBB && pCore->CopyResource(hFrameTex, hBB)) {
				pIPISwim->SetTexture("tSrc", hFrameTex, IPF_CLAMP_U | IPF_CLAMP_V | IPF_LINEAR);
				pIPISwim->SetOutput(0, hBB);
				pIPISwim->SetFloat("fSwim", g_fx.swim);
				pIPISwim->SetFloat("fTime", animT);      // real-time clock, drives the wobble
				pIPISwim->Execute((DWORD)0, true, gcIPInterface::Rect);
			}
		}

		// TILT (roll) - a geometric warp too, after swim, before the optical resamples.
		// TWO inputs: fTilt is the unipolar woozy SWAY (lab slider / scenarios) and fLean
		// is a SIGNED steady head lean written only by the felt-G model from lateral G.
		// The shader adds them, so either can be zero. Gate on both or a pure lean would
		// never render.
		//   backbuffer -> hFrameTex (copy) -> PSTilt -> backbuffer
		if (pIPITilt && g_fx.tiltEnabled && viewH > 0
		    && (g_fx.tilt > 0.001f || fabs(g_fx.tiltLean) > 0.001f)) {
			SURFHANDLE hBB = pCore->GetBackBufferHandle();
			if (hBB && pCore->CopyResource(hFrameTex, hBB)) {
				pIPITilt->SetTexture("tSrc", hFrameTex, IPF_CLAMP_U | IPF_CLAMP_V | IPF_LINEAR);
				pIPITilt->SetOutput(0, hBB);
				pIPITilt->SetFloat("fTilt", g_fx.tilt);
				pIPITilt->SetFloat("fLean", g_fx.tiltLean);
				pIPITilt->SetFloat("fAspect", (float)viewW / (float)viewH);
				pIPITilt->SetFloat("fTime", animT);      // real-time clock, drives the sway
				pIPITilt->Execute((DWORD)0, true, gcIPInterface::Rect);
			}
		}

		// BLUR (separable Gaussian, TWO passes) - optical softening.
		//   H: backbuffer -> hFrameTex (copy) -> PSBlur -> hBlurTex
		//   V: hBlurTex -> PSBlur -> backbuffer
		if (pIPIBlur && g_fx.blurEnabled && g_fx.blur > 0.001f) {
			SURFHANDLE hBB = pCore->GetBackBufferHandle();
			if (hBB && pCore->CopyResource(hFrameTex, hBB)) {
				const DWORD f = IPF_CLAMP_U | IPF_CLAMP_V | IPF_LINEAR;
				const float stepH[2] = { 1.0f / (float)viewW, 0.0f };  // one-texel step, H axis
				const float stepV[2] = { 0.0f, 1.0f / (float)viewH };  // one-texel step, V axis
				pIPIBlur->SetFloat("fBlur", g_fx.blur);                 // persists across both passes
				// Horizontal pass: hFrameTex -> hBlurTex
				pIPIBlur->SetFloat("vBlurStep", stepH, sizeof(stepH));
				pIPIBlur->SetTexture("tSrc", hFrameTex, f);
				pIPIBlur->SetOutput(0, hBlurTex);
				pIPIBlur->Execute((DWORD)0, true, gcIPInterface::Rect);
				// Vertical pass: hBlurTex -> backbuffer
				pIPIBlur->SetFloat("vBlurStep", stepV, sizeof(stepV));
				pIPIBlur->SetTexture("tSrc", hBlurTex, f);
				pIPIBlur->SetOutput(0, hBB);
				pIPIBlur->Execute((DWORD)0, true, gcIPInterface::Rect);
			}
		}

		// CHROMATIC ABERRATION - RGB split radially. Another optical stage, after blur,
		// before grey-out (a fringe on a desaturated frame would have no colour to show).
		//   backbuffer -> hFrameTex (copy) -> PSChroma -> backbuffer
		if (pIPIChroma && g_fx.aberrationEnabled && g_fx.aberration > 0.001f) {
			SURFHANDLE hBB = pCore->GetBackBufferHandle();
			if (hBB && pCore->CopyResource(hFrameTex, hBB)) {
				pIPIChroma->SetTexture("tSrc", hFrameTex, IPF_CLAMP_U | IPF_CLAMP_V | IPF_LINEAR);
				pIPIChroma->SetOutput(0, hBB);
				pIPIChroma->SetFloat("fChroma", g_fx.aberration);
				pIPIChroma->Execute((DWORD)0, true, gcIPInterface::Rect);
			}
		}

		// GREY-OUT (pure desaturation) - after blur, so it greys the softened frame.
		//   backbuffer -> hFrameTex (copy) -> PSGrey -> backbuffer
		if (pIPIGrey && g_fx.greyoutEnabled && g_fx.greyout > 0.001f) {
			SURFHANDLE hBB = pCore->GetBackBufferHandle();
			if (hBB && pCore->CopyResource(hFrameTex, hBB)) {
				pIPIGrey->SetTexture("tSrc", hFrameTex, IPF_CLAMP_U | IPF_CLAMP_V | IPF_LINEAR);
				pIPIGrey->SetOutput(0, hBB);
				pIPIGrey->SetFloat("fGrey", g_fx.greyout);
				pIPIGrey->Execute((DWORD)0, true, gcIPInterface::Rect);
			}
		}

		// COCKPIT PLASMA GLOW - the reentry effect's internal half, and the LAST resample.
		// It is external light entering the cabin, so it goes on TOP of every optical stage
		// above (a plasma glow should not be desaturated by the pilot's grey-out) but UNDER
		// the physiological washes below (blackout must still be able to black it out - the
		// failure is in the eye, and a closing eye does not care how bright the cabin is).
		//   backbuffer -> hFrameTex (copy) -> PSPlasma -> backbuffer
		if (pIPIPlasma && g_fx.reentryEnabled && plasmaGlow > 0.001f) {
			SURFHANDLE hBB = pCore->GetBackBufferHandle();
			if (hBB && pCore->CopyResource(hFrameTex, hBB)) {
				pIPIPlasma->SetTexture("tSrc", hFrameTex, IPF_CLAMP_U | IPF_CLAMP_V | IPF_LINEAR);
				pIPIPlasma->SetOutput(0, hBB);
				pIPIPlasma->SetFloat("fPlasma", plasmaGlow);
				pIPIPlasma->SetFloat("vPlasmaUV", plasmaUV, sizeof(plasmaUV));
				pIPIPlasma->SetFloat("vPlasmaCol", plasmaCol, sizeof(plasmaCol));
				pIPIPlasma->SetFloat("fAspect", (float)viewW / (float)viewH);
				pIPIPlasma->Execute((DWORD)0, true, gcIPInterface::Rect);
			}
		}
	}

	// One blend setup for all effects (LoadDefaults already set ALPHABLEND;
	// stated explicitly to keep the dependency visible). Draw order is the
	// physiological layering: washes first, then the tunnel closes over them.
	pSkp->SetBlendState(oapi::Sketchpad::ALPHABLEND);

	// --- Red-out -----------------------------------------------------------
	// Full-frame red wash, alpha capped at 80% (0xCC) [lab tuning 2026-07-25]:
	// at slider max the MFDs sit at the edge of readability. Colour 0xAABBGGRR;
	// ColorFill(colour, NULL) = whole render target, no viewport query.
	if (g_fx.redoutEnabled && g_fx.redout > 0.0f) {
		const DWORD alpha = (DWORD)(g_fx.redout * 0xCC) & 0xFF;
		pSkp->ColorFill((alpha << 24) | 0x0000FF, NULL);
	}

	// --- Blackout ----------------------------------------------------------
	// Full range 0..0xFF: at slider max the frame is genuinely gone - that IS a
	// blackout, and the dialog floats above the frame so recovery is always a
	// drag away. Black is byte-order-proof (R=G=B=0), only the alpha byte acts.
	if (g_fx.blackoutEnabled && g_fx.blackout > 0.0f) {
		const DWORD alpha = (DWORD)(g_fx.blackout * 0xFF) & 0xFF;
		pSkp->ColorFill(alpha << 24, NULL);
	}

	// --- Dark spots (scotomas) ---------------------------------------------
	// Shimmering soft blobs in the mid-periphery: one triangle-list HPOLY with
	// CONSTANT vertex count (12 fans x 14 tris x 3) - inactive spots collapse
	// to alpha 0, so in-place updates never exceed the creation count. Rebuilt
	// per frame while active: the shimmer IS the animation (sinusoids on the
	// real-time clock). The slider drives spot COUNT and opacity together.
	// Drawn UNDER the tunnel: peripheral darkness swallows peripheral spots
	// first, which is physiologically right.
	static const int SPOT_N = 12, SPOT_SEGS = 14;
	if (g_fx.spotsEnabled && g_fx.spots > 0.001f && viewW > 0 && pCore) {
		static const float tab[SPOT_N][3] = { // {angleFrac, radiusFrac(Rmax), sizeFrac(Rmax)}
			{0.03f,0.34f,0.052f},{0.11f,0.18f,0.038f},{0.22f,0.29f,0.061f},{0.31f,0.12f,0.033f},
			{0.40f,0.38f,0.047f},{0.49f,0.22f,0.055f},{0.58f,0.31f,0.036f},{0.66f,0.15f,0.049f},
			{0.74f,0.36f,0.058f},{0.82f,0.24f,0.041f},{0.90f,0.33f,0.045f},{0.97f,0.19f,0.053f}
		};
		const float s    = g_fx.spots;
		const float cx   = viewW * 0.5f, cy = viewH * 0.5f;
		const float Rmax = 0.5f * sqrtf((float)(viewW * viewW + viewH * viewH));
		const int   nAct = (int)(s * SPOT_N + 0.999f);

		static gcCore::clrVtx vtx[SPOT_N * SPOT_SEGS * 3];
		int n = 0;
		for (int i = 0; i < SPOT_N; i++) {
			float a = 0.0f;
			if (i < nAct) {
				const float shimmer = 0.55f + 0.45f * sinf(animT * (0.9f + 0.13f * i) + i * 2.3f);
				a = s * 230.0f * shimmer;
				if (a < 0.0f) a = 0.0f;
				if (a > 255.0f) a = 255.0f;
			}
			const DWORD cCen = ((DWORD)a) << 24;      // black: only the alpha byte acts
			const float ang  = tab[i][0] * 6.2831853f;
			const float scx  = cx + tab[i][1] * Rmax * cosf(ang);
			const float scy  = cy + tab[i][1] * Rmax * sinf(ang);
			const float rad  = tab[i][2] * Rmax;
			for (int k = 0; k < SPOT_SEGS; k++) {
				const float b0 = (float)k       * (6.2831853f / SPOT_SEGS);
				const float b1 = (float)(k + 1) * (6.2831853f / SPOT_SEGS);
				vtx[n].pos = oapi::FVECTOR2(scx, scy);                                   vtx[n++].color = cCen;
				vtx[n].pos = oapi::FVECTOR2(scx + rad * cosf(b0), scy + rad * sinf(b0)); vtx[n++].color = 0x00000000;
				vtx[n].pos = oapi::FVECTOR2(scx + rad * cosf(b1), scy + rad * sinf(b1)); vtx[n++].color = 0x00000000;
			}
		}
		hSpotsPoly = pCore->CreateTriangles(hSpotsPoly, vtx, n, PF_TRIANGLES);
		if (hSpotsPoly) pSkp->DrawPoly(hSpotsPoly);
	}

	// --- Heartbeat pulse (cardiac vignette throb) --------------------------
	// A soft peripheral darkening that THROBS on the real-time cardiac clock: near
	// G-LOC the field dims with every heartbeat. heartEnv (0..1, computed in
	// clbkPreStep) is the beat envelope; the slider scales the depth. One HPOLY,
	// CONSTANT vertex count, rebuilt per frame (the alpha pulses): a feather from a
	// fixed central aperture out to the screen edge, plus a solid band past the
	// corners (same corner-cover trick as the tunnel). Drawn under the tunnel.
	static const int HEART_SEGS = 48;
	if (g_fx.heartbeatEnabled && g_fx.heartbeat > 0.001f && heartEnv > 0.003f && viewW > 0 && pCore) {
		const float cx   = viewW * 0.5f, cy = viewH * 0.5f;
		const float Rmax = 0.5f * sqrtf((float)(viewW * viewW + viewH * viewH));
		const float rIn  = Rmax * 0.45f;                    // central clear aperture (fixed)
		const float Rfar = Rmax + 8.0f;                     // past every screen corner
		float a = g_fx.heartbeat * heartEnv * 205.0f;       // peak alpha at systole
		if (a > 255.0f) a = 255.0f;
		const DWORD cEdge = ((DWORD)a) << 24;               // black: only the alpha byte acts

		const float rad[3] = { rIn, Rmax, Rfar };
		const DWORD alp[3] = { 0x00000000, cEdge, cEdge };  // feather 0->a, then solid a
		static gcCore::clrVtx vtx[2 * HEART_SEGS * 6];
		int n = 0;
		for (int b = 0; b < 2; b++) {
			for (int k = 0; k < HEART_SEGS; k++) {
				const float a0 = (float)k       * (6.2831853f / HEART_SEGS);
				const float a1 = (float)(k + 1) * (6.2831853f / HEART_SEGS);
				const oapi::FVECTOR2 i0(cx + rad[b]     * cosf(a0), cy + rad[b]     * sinf(a0));
				const oapi::FVECTOR2 i1(cx + rad[b]     * cosf(a1), cy + rad[b]     * sinf(a1));
				const oapi::FVECTOR2 o0(cx + rad[b + 1] * cosf(a0), cy + rad[b + 1] * sinf(a0));
				const oapi::FVECTOR2 o1(cx + rad[b + 1] * cosf(a1), cy + rad[b + 1] * sinf(a1));
				vtx[n].pos = i0; vtx[n++].color = alp[b];
				vtx[n].pos = o0; vtx[n++].color = alp[b + 1];
				vtx[n].pos = o1; vtx[n++].color = alp[b + 1];
				vtx[n].pos = i0; vtx[n++].color = alp[b];
				vtx[n].pos = o1; vtx[n++].color = alp[b + 1];
				vtx[n].pos = i1; vtx[n++].color = alp[b];
			}
		}
		hHeartPoly = pCore->CreateTriangles(hHeartPoly, vtx, n, PF_TRIANGLES);
		if (hHeartPoly) pSkp->DrawPoly(hHeartPoly);
	}

	// --- Tunnel vision -----------------------------------------------------
	// Concentric per-vertex-alpha bands (triangle LIST), circle-only geometry:
	//   rClear .. rBlack : the feather - SIX bands whose boundary alphas follow
	//                      a QUADRATIC ease (u^2), so dimming creeps in slowly
	//                      from 30% of the aperture radius and steepens toward
	//                      the closure front [lab feedback 2026-07-25: the old
	//                      single narrow band read as a hard rim];
	//   rBlack .. Rfar   : one solid black band reaching past the farthest
	//                      screen corner - which is what fixes the "clear
	//                      square" bug: the old code filled from the ring's
	//                      BOUNDING BOX outward with 4 rects and left the four
	//                      corner regions between circle and box uncovered.
	// One HPOLY, created once and UPDATED in place (constant vertex count),
	// only when the slider or viewport changes. t=1 closes to a ~14 px glimmer.
	static const int TUNNEL_SEGS  = 48;   // circle segments
	static const int TUNNEL_BANDS = 6;    // feather bands (+1 solid band appended)
	if (g_fx.tunnelEnabled && g_fx.tunnel > 0.001f && viewW > 0 && pCore) {
		float t = g_fx.tunnel;
		// Heartbeat coupling: each beat transiently tightens the aperture, so the throb
		// stays visible once the tunnel has crushed the periphery to black (out there a
		// plain peripheral vignette would just be painting black over black). This is
		// what keeps the heartbeat legible deep into tunnel vision.
		if (g_fx.heartbeatEnabled && g_fx.heartbeat > 0.001f)
			t = min(1.0f, t + g_fx.heartbeat * heartEnv * 0.14f);
		const float cx     = viewW * 0.5f, cy = viewH * 0.5f;
		const float Rmax   = 0.5f * sqrtf((float)(viewW * viewW + viewH * viewH));
		const float rBlack = max(Rmax * (1.0f - t), 14.0f);  // closure front (alpha 255)
		const float rClear = rBlack * 0.30f;                 // feather begins (alpha 0)
		const float Rfar   = Rmax + 8.0f;                    // beyond every screen corner

		if (t != lastTunnel || viewW != lastViewW || viewH != lastViewH || !hTunnelPoly) {
			// Ring boundaries: 0..TUNNEL_BANDS = feather (alpha 255*u^2), +1 = Rfar (solid).
			float rad[TUNNEL_BANDS + 2];
			DWORD alp[TUNNEL_BANDS + 2];
			for (int i = 0; i <= TUNNEL_BANDS; i++) {
				const float u = (float)i / TUNNEL_BANDS;
				rad[i] = rClear + (rBlack - rClear) * u;
				alp[i] = (DWORD)(255.0f * u * u + 0.5f);
			}
			rad[TUNNEL_BANDS + 1] = Rfar;
			alp[TUNNEL_BANDS + 1] = 255;

			// Triangle list: (BANDS+1) bands x SEGS quads x 2 tris x 3 vtx.
			static gcCore::clrVtx vtx[(TUNNEL_BANDS + 1) * TUNNEL_SEGS * 6];
			int n = 0;
			for (int b = 0; b <= TUNNEL_BANDS; b++) {
				const DWORD c0 = alp[b] << 24, c1 = alp[b + 1] << 24;  // black: only alpha acts
				for (int k = 0; k < TUNNEL_SEGS; k++) {
					const float a0 = (float)k       * (6.2831853f / TUNNEL_SEGS);
					const float a1 = (float)(k + 1) * (6.2831853f / TUNNEL_SEGS);
					const oapi::FVECTOR2 i0(cx + rad[b]     * cosf(a0), cy + rad[b]     * sinf(a0));
					const oapi::FVECTOR2 i1(cx + rad[b]     * cosf(a1), cy + rad[b]     * sinf(a1));
					const oapi::FVECTOR2 o0(cx + rad[b + 1] * cosf(a0), cy + rad[b + 1] * sinf(a0));
					const oapi::FVECTOR2 o1(cx + rad[b + 1] * cosf(a1), cy + rad[b + 1] * sinf(a1));
					vtx[n].pos = i0; vtx[n++].color = c0;
					vtx[n].pos = o0; vtx[n++].color = c1;
					vtx[n].pos = o1; vtx[n++].color = c1;
					vtx[n].pos = i0; vtx[n++].color = c0;
					vtx[n].pos = o1; vtx[n++].color = c1;
					vtx[n].pos = i1; vtx[n++].color = c0;
				}
			}
			hTunnelPoly = pCore->CreateTriangles(hTunnelPoly, vtx, n, PF_TRIANGLES);
			lastTunnel = t; lastViewW = viewW; lastViewH = viewH;
		}
		if (hTunnelPoly) pSkp->DrawPoly(hTunnelPoly);
	}

	// --- Sparkles / phosphenes ---------------------------------------------
	// "Seeing stars" under G/impact: fine bright scintillations scattered across the
	// field. Kept deliberately SUBTLE (small, soft, cool-white, alpha-blended - never
	// additive, so they can't blow out to a cartoony glint) and SCINTILLATING (each
	// flashes briefly on its own desync'd clock, not a steady twinkle). Placed on a
	// golden-angle spiral (even natural scatter) - intentionally DISTINCT from the
	// dark-spots mid-periphery ring. Drawn LATE (over the tunnel) so you still see stars
	// as the view darkens. Constant vertex count; rebuilt per frame (the flicker IS the
	// animation), slider drives count AND flash brightness together.
	static const int SPARK_N = 28, SPARK_SEGS = 8;
	// Black-out WINS over stars: full black-out is total vision loss, so nobody sees
	// phosphenes through it. Fade the sparkles out (count AND brightness) as black-out
	// deepens - gone at full black. Tunnel does NOT suppress them (it only narrows the
	// field; central vision persists, so stars still flicker in the closing dark).
	float sparkStr = g_fx.sparkles;
	if (g_fx.blackoutEnabled) sparkStr *= (1.0f - g_fx.blackout);
	if (g_fx.sparklesEnabled && sparkStr > 0.001f && viewW > 0 && pCore) {
		const float s    = sparkStr;
		const float cx   = viewW * 0.5f, cy = viewH * 0.5f;
		const float Rmax = 0.5f * sqrtf((float)(viewW * viewW + viewH * viewH));
		const int   nAct = (int)(s * SPARK_N + 0.999f);

		static gcCore::clrVtx vtx[SPARK_N * SPARK_SEGS * 3];
		int n = 0;
		for (int i = 0; i < SPARK_N; i++) {
			float a = 0.0f;
			if (i < nAct) {
				// Sharp per-sparkle scintillation: mostly dark, brief bright flashes,
				// desync'd frequencies so they never pulse in unison.
				const float ph   = i * 1.7f;
				const float freq = 7.0f + 2.3f * (float)((i * 13) % 7);
				float tw = sinf(animT * freq + ph);
				tw = tw > 0.0f ? tw * tw * tw : 0.0f;               // sharpen to flashes
				a = s * 150.0f * tw;                                // subtle peak (NOT 255)
				if (a > 160.0f) a = 160.0f;
			}
			// Golden-angle (sunflower) spiral - even scatter over the whole field.
			const float ang  = (float)i * 2.39996323f;
			const float rr   = sqrtf(((float)i + 0.5f) / SPARK_N) * Rmax * 0.78f;
			const float scx  = cx + rr * cosf(ang);
			const float scy  = cy + rr * sinf(ang);
			const float rad  = (0.006f + 0.004f * (float)((i * 7) % 5) / 4.0f) * Rmax;  // small, varied
			const DWORD cCen = ((DWORD)a << 24) | 0x00FFF0E8;       // cool-white (0xAABBGGRR)
			for (int k = 0; k < SPARK_SEGS; k++) {
				const float b0 = (float)k       * (6.2831853f / SPARK_SEGS);
				const float b1 = (float)(k + 1) * (6.2831853f / SPARK_SEGS);
				vtx[n].pos = oapi::FVECTOR2(scx, scy);                                   vtx[n++].color = cCen;
				vtx[n].pos = oapi::FVECTOR2(scx + rad * cosf(b0), scy + rad * sinf(b0)); vtx[n++].color = 0x00FFF0E8;
				vtx[n].pos = oapi::FVECTOR2(scx + rad * cosf(b1), scy + rad * sinf(b1)); vtx[n++].color = 0x00FFF0E8;
			}
		}
		hSparkPoly = pCore->CreateTriangles(hSparkPoly, vtx, n, PF_TRIANGLES);
		if (hSparkPoly) pSkp->DrawPoly(hSparkPoly);
	}

	// --- Blink (eyelids) - over EVERYTHING, including the tunnel ------------
	// Two lids closing towards the horizontal midline, each a solid black rect
	// with a soft gradient edge (GradientFillRect). blinkAmount is the envelope
	// computed in clbkPreStep on the real-time clock.
	if (g_fx.blinkAmount > 0.001f && viewH > 0) {
		const LONG  W = (LONG)viewW, H = (LONG)viewH;
		const float F = 26.0f;                            // soft lid edge height
		const float cov = g_fx.blinkAmount * (H * 0.5f + F);
		const LONG  solid = (LONG)max(0.0f, cov - F);     // fully covered depth
		const LONG  soft  = (LONG)min(H * 0.5f + F, cov); // gradient reaches here
		RECT r;
		if (solid > 0)     { r = { 0, 0, W, solid };            pSkp->ColorFill(0xFF000000, &r); }
		if (soft > solid)  { r = { 0, solid, W, soft };         pSkp->GradientFillRect(&r, 0xFF000000, 0x00000000, true); }
		if (solid > 0)     { r = { 0, H - solid, W, H };        pSkp->ColorFill(0xFF000000, &r); }
		if (soft > solid)  { r = { 0, H - soft, W, H - solid }; pSkp->GradientFillRect(&r, 0x00000000, 0xFF000000, true); }
	}
}

// ----------------------------------------------------------------------------
// DLL entry points. Orbiter calls InitModule when the plugin is activated in
// Launchpad -> Modules (or at startup if already enabled), and ExitModule on unload.
// ----------------------------------------------------------------------------

static OroModule* g_oro = nullptr;
static DWORD g_customCmd = 0;   // Custom Functions (Ctrl+F4) entry id
static HINSTANCE g_hInstDLL = NULL;

// Ctrl+F4 "Custom Functions" callback: open the ORO control dialog.
static void OpenOroDlgClbk(void* context)
{
	OroDlg_Open(g_hInstDLL);
}

// ----------------------------------------------------------------------------
// CRASH FORENSICS (2026-08-09, after an unexplained CTD on the BELL GLOW pill).
// Windows recorded it as ucrtbase.dll, exception 0xc0000409, data 0x7 - that is
// FAST_FAIL_FATAL_APP_EXIT, i.e. abort(). In a release build the two things that
// reach abort() are the secure-CRT INVALID PARAMETER handler (any *_s function
// given a destination too small, or a bad argument) and an UNCAUGHT C++
// EXCEPTION via std::terminate. Neither leaves a single line in Orbiter.log, and
// neither is reproducible on demand, so a crash like it could otherwise cost
// several fly-and-report rounds to corner.
//
// Both are hookable. ORO installs a handler for each that LOGS - with a stack
// walk resolved to module + offset, which is enough to say whose code it was -
// and then, for the CRT case, RETURNS. Returning turns a fatal abort into a
// failed call: sprintf_s writes nothing and reports an error, and the sim keeps
// flying. That is the right trade for a beta: a truncated caption beats a CTD.
//
// ⚠ These are PROCESS-WIDE CRT settings, so they are restored in ExitModule -
// a handler left pointing into an unloaded DLL would be a far worse bug than the
// one it was installed to find.
namespace {
	_invalid_parameter_handler g_prevIPH = NULL;
	std::terminate_handler     g_prevTH  = NULL;
	PVOID                      g_veh     = NULL;

	// ------------------------------------------------------------------------
	// THROW-TIME CAPTURE (2026-08-11), and the reason it exists is a lesson worth
	// keeping: THE STACK AT std::terminate IS NOT THE STACK THAT THREW. The first
	// catch of this crash logged four frames of KERNELBASE/ntdll exception-dispatch
	// machinery and nothing else, because by the time terminate() runs the throwing
	// frames are gone. The only place the thrower is still on the stack is the moment
	// of the throw itself, which is what a vectored exception handler sees.
	//
	// 0xE06D7363 ('msc') is MSVC's C++ throw. The handler must be CHEAP and must NEVER
	// interfere: most C++ exceptions in a process this size are thrown and caught
	// normally, so it records into a fixed ring in static memory - no allocation, no
	// file I/O - and always returns CONTINUE_SEARCH. OroTerminate then dumps the ring,
	// so we learn what threw LAST before nothing caught it.
	// ------------------------------------------------------------------------
	const DWORD ORO_CPP_EXC = 0xE06D7363;

	struct ThrowRec {
		void* fr[20];
		USHORT n;
		char   type[128];
	};
	ThrowRec g_throws[4];
	volatile LONG g_throwSeq = 0;      // total throws seen; & 3 indexes the ring

	// Dig the thrown object's C++ type name out of MSVC's ThrowInfo. 32-BIT ONLY, and
	// that is what makes it simple: every pointer in these structures is a direct
	// address, with none of the image-base-relative rebasing the 64-bit layout needs.
	// Wrapped in SEH because it is pointer-walking inside a handler - if any of it is
	// not what we expect, we would rather lose the name than the whole diagnostic.
	void OroThrowTypeName(const EXCEPTION_RECORD* er, char* out, size_t cb)
	{
		out[0] = '\0';
		__try {
			if (er->NumberParameters < 3) return;
			const DWORD* ti = (const DWORD*)er->ExceptionInformation[2];   // ThrowInfo*
			if (!ti) return;
			const DWORD* cta = (const DWORD*)ti[3];                        // CatchableTypeArray*
			if (!cta || cta[0] < 1) return;
			const DWORD* ct = (const DWORD*)cta[1];                        // CatchableType*
			if (!ct) return;
			const char* name = (const char*)((const DWORD*)ct[1] + 2);     // TypeDescriptor::name
			if (name) strncpy_s(out, cb, name, _TRUNCATE);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
	}

	LONG CALLBACK OroVeh(EXCEPTION_POINTERS* ep)
	{
		if (ep && ep->ExceptionRecord &&
		    ep->ExceptionRecord->ExceptionCode == ORO_CPP_EXC) {
			ThrowRec& r = g_throws[InterlockedIncrement(&g_throwSeq) & 3];
			r.n = CaptureStackBackTrace(1, 20, r.fr, NULL);
			OroThrowTypeName(ep->ExceptionRecord, r.type, sizeof(r.type));
		}
		return EXCEPTION_CONTINUE_SEARCH;   // never interfere - only observe
	}

	// A SECOND destination for the forensics, and the reason for it is the whole point:
	// ORBITER.LOG IS TRUNCATED AT EVERY LAUNCH. The abort this instrumentation hunts has
	// now fired twice (2026-08-09 20:59 and 2026-08-11 02:13, identical WER signature
	// ucrtbase 0xc0000409 +0x0009eddb) and BOTH stack walks were destroyed by the next
	// Orbiter start before anyone could read them. A crash you only learn about after
	// restarting needs a log that survives restarting, so this one APPENDS and is never
	// truncated. Plain CRT file I/O deliberately - no oapi call, because a handler can
	// run at points in teardown where Orbiter's own logging is not safe to re-enter.
	// ------------------------------------------------------------------------
	// MEMORY WATCH (2026-08-11). Orbiter is a 32-BIT process, so it dies of address
	// space long before it dies of RAM, and the two exceptions the throw-capture caught
	// (std::bad_alloc and std::bad_array_new_length, both thrown from Orbiter.exe on the
	// second scenario load) are what running out of it looks like. The earlier
	// D3D9Client +0x1ecb access violation is almost certainly the same thing wearing a
	// different hat: an allocation that returned NULL and was not checked.
	//
	// So: print what ORO is holding at each session boundary. If the number climbs by
	// a big constant every load, the leak is ours and the size names the culprit.
	// K32GetProcessMemoryInfo is bound by NAME from kernel32 so this needs no psapi.lib
	// and cannot break the link on any machine.
	// ------------------------------------------------------------------------
	struct ORO_PMC {           // PROCESS_MEMORY_COUNTERS_EX, declared locally so the
		DWORD  cb;               // build does not depend on psapi.h being in the chain
		DWORD  PageFaultCount;
		SIZE_T PeakWorkingSetSize, WorkingSetSize;
		SIZE_T QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage;
		SIZE_T QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage;
		SIZE_T PagefileUsage, PeakPagefileUsage;
		SIZE_T PrivateUsage;
	};

	void OroLogMemory(const char* when)
	{
		typedef BOOL(WINAPI* PFN)(HANDLE, ORO_PMC*, DWORD);
		static PFN pfn = (PFN)GetProcAddress(GetModuleHandleA("kernel32.dll"),
		                                     "K32GetProcessMemoryInfo");
		if (!pfn) return;
		ORO_PMC pmc; ZeroMemory(&pmc, sizeof(pmc)); pmc.cb = sizeof(pmc);
		if (!pfn(GetCurrentProcess(), &pmc, sizeof(pmc))) return;

		// Address space is the resource that actually runs out here, so report the
		// committed private bytes rather than the working set (which the OS trims and
		// which would therefore hide a leak completely).
		MEMORYSTATUSEX ms; ZeroMemory(&ms, sizeof(ms)); ms.dwLength = sizeof(ms);
		GlobalMemoryStatusEx(&ms);
		oapiWriteLogV("ORO MEM [%s]: private %u MB, working set %u MB, "
		              "process address space free %u MB of %u MB.",
		              when,
		              (unsigned)(pmc.PrivateUsage    / (1024 * 1024)),
		              (unsigned)(pmc.WorkingSetSize  / (1024 * 1024)),
		              (unsigned)(ms.ullAvailVirtual  / (1024 * 1024)),
		              (unsigned)(ms.ullTotalVirtual  / (1024 * 1024)));
	}

	void OroCrashFile(const char* line)
	{
		FILE* f = nullptr;
		if (fopen_s(&f, "Modules\\ORO\\ORO_crash.log", "a") != 0 || !f) return;
		fprintf(f, "%s\n", line);
		fclose(f);
	}

	void OroLogStack(const char* what)
	{
		void* fr[24];
		const USHORT n = CaptureStackBackTrace(2, 24, fr, NULL);
		char line[512];

		SYSTEMTIME st; GetLocalTime(&st);
		sprintf_s(line, "=== %04u-%02u-%02u %02u:%02u:%02u  ORO build %s ===",
		          st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, __DATE__);
		OroCrashFile(line);

		sprintf_s(line, "ORO: *** %s *** stack follows (module+offset):", what);
		oapiWriteLog(line);
		OroCrashFile(line);
		for (USHORT i = 0; i < n; i++) {
			HMODULE hm = NULL;
			char mod[MAX_PATH] = "?";
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                       (LPCSTR)fr[i], &hm) && hm) {
				char full[MAX_PATH] = "";
				GetModuleFileNameA(hm, full, MAX_PATH);
				const char* leaf = strrchr(full, '\\');
				strcpy_s(mod, leaf ? leaf + 1 : full);
			}
			sprintf_s(line, "ORO:   [%02u] %s + 0x%08X", (unsigned)i, mod,
			          (unsigned)((BYTE*)fr[i] - (BYTE*)hm));
			oapiWriteLog(line);
			OroCrashFile(line);
		}
	}

	void __cdecl OroInvalidParam(const wchar_t*, const wchar_t*, const wchar_t*,
	                               unsigned int, uintptr_t)
	{
		// Release CRTs pass NULL for expression/file/function, so the stack is the
		// only evidence there is - which is exactly why we walk it.
		OroLogStack("CRT INVALID PARAMETER (a *_s call with a bad argument)");
		// Return, do not abort: the offending call fails and the sim survives.
	}

	// Resolve one captured frame to "module + offset", the same form OroLogStack uses.
	void OroFrameStr(void* addr, char* out, size_t cb)
	{
		HMODULE hm = NULL;
		char mod[MAX_PATH] = "?";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                       (LPCSTR)addr, &hm) && hm) {
			char full[MAX_PATH] = "";
			GetModuleFileNameA(hm, full, MAX_PATH);
			const char* leaf = strrchr(full, '\\');
			strcpy_s(mod, leaf ? leaf + 1 : full);
		}
		sprintf_s(out, cb, "%s + 0x%08X", mod, (unsigned)((BYTE*)addr - (BYTE*)hm));
	}

	void OroTerminate()
	{
		OroLogStack("UNCAUGHT C++ EXCEPTION (std::terminate)");

		// THE USEFUL HALF: the terminate stack above is only exception-dispatch
		// machinery. These are the last throws the VEH saw, newest first - the top
		// entry is almost certainly the one nothing caught.
		char line[512], frame[MAX_PATH + 32];
		const LONG seen = g_throwSeq;
		sprintf_s(line, "ORO: last C++ throws seen (%d total this run), newest first:", (int)seen);
		oapiWriteLog(line); OroCrashFile(line);

		const int lim = (seen < 4) ? (int)seen : 4;
		for (int k = 0; k < lim; k++) {
			const ThrowRec& r = g_throws[(seen - k) & 3];
			sprintf_s(line, "ORO:  throw -%d  type: %s", k, r.type[0] ? r.type : "(unknown)");
			oapiWriteLog(line); OroCrashFile(line);
			for (USHORT i = 0; i < r.n; i++) {
				OroFrameStr(r.fr[i], frame, sizeof(frame));
				sprintf_s(line, "ORO:    [%02u] %s", (unsigned)i, frame);
				oapiWriteLog(line); OroCrashFile(line);
			}
		}

		if (g_prevTH) g_prevTH();          // let the old handler do the dying
		abort();
	}
}

// Orbiter reads this export to fill the `[Build ......]` field it prints for every module
// in the log; without it ORO has always logged `[Build ******]`. Normally it comes free
// from OrbiterAPI.h's ORBITER_MODULE block, but ORO deliberately does NOT define that
// symbol - the block also emits a `calldummy()` referencing DllMain glue we bypass with
// the vcxproj's ForceSymbolReferences arrangement (see the long comment there). Exporting
// the one function by hand is the whole fix and touches nothing else.
DLLCLBK char* ModuleDate() { return (char*)__DATE__; }

DLLCLBK void InitModule(HINSTANCE hDLL)
{
	g_prevIPH = _set_invalid_parameter_handler(OroInvalidParam);
	g_prevTH  = std::set_terminate(OroTerminate);
	// FIRST in the chain (1), so we see a throw before anything else can swallow it.
	// Observe-only: it always returns CONTINUE_SEARCH.
	g_veh     = AddVectoredExceptionHandler(1, OroVeh);
	g_hInstDLL = hDLL;
	g_oro = new OroModule(hDLL);
	oapiRegisterModule(g_oro);
	// The dialog's entry point for the user (same pattern as the DialogTemplate sample).
	g_customCmd = oapiRegisterCustomCmd(
		(char*)"ORO control",
		(char*)"Open the ORO immersion control panel.",
		OpenOroDlgClbk, NULL);
	oapiWriteLogV("ORO: InitModule - registered global module + custom command.");
}

DLLCLBK void ExitModule(HINSTANCE hDLL)
{
	// Per the oapiRegisterModule contract, the DLL owns the instance and deletes it here.
	oapiWriteLogV("ORO: ExitModule.");
	OroDlg_Close();
	oapiUnregisterCustomCmd(g_customCmd);
	delete g_oro;
	g_oro = nullptr;
	// MANDATORY: these are process-wide and point into THIS DLL, which is about to
	// be unloaded. Leaving them installed would turn the next CRT violation
	// anywhere in Orbiter into a jump to freed code.
	_set_invalid_parameter_handler(g_prevIPH);
	std::set_terminate(g_prevTH);
	// Same rule, and it matters MORE for this one: a vectored handler left registered
	// after the DLL unloads is called on every exception in the process, at an address
	// that no longer exists.
	if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = NULL; }
}
