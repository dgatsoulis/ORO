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
// scenario gets SND_SCEN_BASE + its index (see INDUCE_SEQ[]). The RAIN loops own
// 30..33 (SND_RAIN_BASE in OroModule.h - declared there because UpdateRainSound
// lives in OroRain.cpp); the THUNDER set owns 40..48 (SND_THUNDER_BASE, same place).
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
// THE RAIN poly. Alpha-blended for the same reason the vapour cone is (invariant 25a):
// water SCATTERS and occludes, and an additive layer can only ever brighten what is
// behind it. Depth-clipped, so a drop behind the hull disappears per pixel instead of
// painting the ship out - but note the client's depth buffer contains vessels and the
// cockpit ONLY, so the GROUND cannot clip rain and the build does that by hand.
void OroModule::DrawRainPoly(oapi::Sketchpad* pSkp)
{
	if ((rainVtxN <= 0 && gndVtxN <= 0 && ringCN <= 0 && ringVN <= 0 && deckN <= 0) || !pCore) return;

	// TWO POLYS, ONE CEILING EACH (see RAIN_GND_TRI in OroModule.h): the 65535-vertex cap
	// is per HPOLY, so the deck + ring fields and the streak sheet each get their own
	// buffer - and drawing GND first is also the correct painter's order, since the
	// streaks are the layer nearest the eye.
	DWORD blend = (DWORD)oapi::Sketchpad::BlendState::ALPHABLEND;
	if (depthClipOK) blend |= 0x100;
	pSkp->SetBlendState((oapi::Sketchpad::BlendState)blend);

	// the TEXTURED deck (patch l - the synthesized cloud cover); the Gouraud gndVtx
	// block below is its no-(l) fallback and is empty whenever this one draws
	if (deckN > 0 && hRainCloudTex) {
		if (!hRainDeckPoly) {
			static gcCore::texVtx zeroD[RAIN_GND_TRI * 3];   // zero-init: degenerate, alpha 0
			hRainDeckPoly = pCore->CreateTrianglesTex(NULL, zeroD, NULL, RAIN_GND_TRI * 3, PF_TRIANGLES, hRainCloudTex);
		}
		if (hRainDeckPoly) {
			pCore->CreateTrianglesTex(hRainDeckPoly, (const gcCore::texVtx*)deckVtx,
			                          depthClipOK ? deckDepth : NULL, RAIN_GND_TRI * 3, PF_TRIANGLES, hRainCloudTex);
			pSkp->DrawPoly(hRainDeckPoly);
		}
	}
	if (gndVtxN > 0) {
		if (!hRainGndPoly) {
			static gcCore::clrVtx zeroG[RAIN_GND_TRI * 3];   // zero-init: degenerate, alpha 0
			hRainGndPoly = pCore->CreateTriangles(NULL, zeroG, RAIN_GND_TRI * 3, PF_TRIANGLES);
		}
		if (hRainGndPoly) {
			if (depthClipOK) pCore->CreateTrianglesDepth(hRainGndPoly, (const gcCore::clrVtx*)gndVtx, gndDepth, RAIN_GND_TRI * 3, PF_TRIANGLES);
			else             pCore->CreateTriangles(hRainGndPoly, (const gcCore::clrVtx*)gndVtx, RAIN_GND_TRI * 3, PF_TRIANGLES);
			pSkp->DrawPoly(hRainGndPoly);
		}
	}
	// THE BOLTS (rain lightning part 2): additive textured quads - a bolt is light,
	// so it takes the additive state the alpha polys around it do not
	if (boltN > 0 && hBoltTex) {
		if (!hRainBoltPoly) {
			static gcCore::texVtx zeroB[RAIN_BOLT_TRI * 3];
			hRainBoltPoly = pCore->CreateTrianglesTex(NULL, zeroB, NULL, RAIN_BOLT_TRI * 3, PF_TRIANGLES, hBoltTex);
		}
		if (hRainBoltPoly) {
			pCore->CreateTrianglesTex(hRainBoltPoly, (const gcCore::texVtx*)boltVtx,
			                          depthClipOK ? boltDepth : NULL, RAIN_BOLT_TRI * 3, PF_TRIANGLES, hBoltTex);
			DWORD bb = padAdditive ? 0x5 : (DWORD)oapi::Sketchpad::BlendState::ALPHABLEND;
			if (depthClipOK) bb |= 0x100;
			pSkp->SetBlendState((oapi::Sketchpad::BlendState)bb);
			pSkp->DrawPoly(hRainBoltPoly);
			pSkp->SetBlendState((oapi::Sketchpad::BlendState)blend);   // back to the alpha state
		}
	}
	// the two splash-ring polys (one per field - the more-groups trick): between the
	// deck and the streak sheet in painter's order
	if (ringCN > 0) {
		if (!hRainRingCPoly) {
			static gcCore::clrVtx zeroRC[RAIN_RINGP_TRI * 3];
			hRainRingCPoly = pCore->CreateTriangles(NULL, zeroRC, RAIN_RINGP_TRI * 3, PF_TRIANGLES);
		}
		if (hRainRingCPoly) {
			if (depthClipOK) pCore->CreateTrianglesDepth(hRainRingCPoly, (const gcCore::clrVtx*)ringVtxC, ringDepC, RAIN_RINGP_TRI * 3, PF_TRIANGLES);
			else             pCore->CreateTriangles(hRainRingCPoly, (const gcCore::clrVtx*)ringVtxC, RAIN_RINGP_TRI * 3, PF_TRIANGLES);
			pSkp->DrawPoly(hRainRingCPoly);
		}
	}
	if (ringVN > 0) {
		if (!hRainRingVPoly) {
			static gcCore::clrVtx zeroRV[RAIN_RINGP_TRI * 3];
			hRainRingVPoly = pCore->CreateTriangles(NULL, zeroRV, RAIN_RINGP_TRI * 3, PF_TRIANGLES);
		}
		if (hRainRingVPoly) {
			if (depthClipOK) pCore->CreateTrianglesDepth(hRainRingVPoly, (const gcCore::clrVtx*)ringVtxV, ringDepV, RAIN_RINGP_TRI * 3, PF_TRIANGLES);
			else             pCore->CreateTriangles(hRainRingVPoly, (const gcCore::clrVtx*)ringVtxV, RAIN_RINGP_TRI * 3, PF_TRIANGLES);
			pSkp->DrawPoly(hRainRingVPoly);
		}
	}
	if (rainVtxN > 0) {
		if (!hRainPoly) {
			static gcCore::clrVtx zero[RAIN_MAX_TRI * 3];    // zero-init: degenerate, alpha 0
			hRainPoly = pCore->CreateTriangles(NULL, zero, RAIN_MAX_TRI * 3, PF_TRIANGLES);
		}
		if (hRainPoly) {
			if (depthClipOK) pCore->CreateTrianglesDepth(hRainPoly, (const gcCore::clrVtx*)rainVtx, rainDepth, RAIN_MAX_TRI * 3, PF_TRIANGLES);
			else             pCore->CreateTriangles(hRainPoly, (const gcCore::clrVtx*)rainVtx, RAIN_MAX_TRI * 3, PF_TRIANGLES);
			pSkp->DrawPoly(hRainPoly);
		}
	}
	pSkp->SetBlendState(oapi::Sketchpad::BlendState::ALPHABLEND);        // leave the pad as found
}

// THE GLOOM resample - the overcast. Self-gating one-liner at its call site, exactly like
// DrawEclipsePass, and it runs immediately after it for the same reason: both decide what
// the LIGHT is doing before anything is drawn into it. Was EXTERNAL only until 2026-08-23;
// with the rain visible from the VC (rainVC) the internal call site activates too, and the
// mild frame-wide desaturation deliberately covers the panel as well - the eclipse's eye
// already treats the whole frame, and a cockpit under a storm ceiling IS greyer. The heavy
// lifting stays with the client-side storm light (patch s part 2), which dims only the SUN.
void OroModule::DrawGloomPass()
{
	// TEMPORARY DIAGNOSTIC, ROUND 2 (2026-08-26). Round 1 sat BELOW this early return, so
	// "all bits clear" could not distinguish "bailed here" from "never called" - the
	// instrument was downstream of the thing it was measuring. These five are recorded
	// BEFORE the return, and glassDiagI carries the intensity as the RENDER PATH sees it,
	// which is the one value the clbkPreStep line cannot report.
	glassDiag = 0x80;
	if (rainIntensityLive > 0.002f) glassDiag |= 0x0100;
	if (ipiReady)                   glassDiag |= 0x0200;
	if (pCore)                      glassDiag |= 0x0400;
	if (hFrameTex)                  glassDiag |= 0x0800;
	if (pIPIGloom)                  glassDiag |= 0x1000;
	if (viewGate)                   glassDiag |= 0x2000;
	glassDiagI = rainIntensityLive;

	if (rainIntensityLive <= 0.002f || !ipiReady || !pCore || !hFrameTex || !pIPIGloom) return;
	// The SLIDER is read HERE, in the render path, not in clbkPreStep - which does not run
	// while paused, so folding it in on the main thread made the control dead in exactly
	// the state the look gets judged in (invariant 1).
	float gs = g_fx.rainGloom; if (gs < 0.0f) gs = 0.0f; if (gs > 2.0f) gs = 2.0f;
	const float gl = rainIntensityLive * gs * 0.5f;

	// --- RAINDROPS ON THE GLASS (2026-08-26, client patch h) --------------------
	// VC ONLY, and that is a physical statement rather than a budget one: these drops
	// are ON the canopy, so a view with no canopy in it has nowhere to put them. Note
	// the flat internal modes (rainPanel) are deliberately NOT included - a 2D panel is
	// an overlay painted by Pane::Render, not glass, and there is no window to wet.
	float dr = 0.0f;
	VECTOR3 cpos; MATRIX3 Rcam; double tanAp = 0.5;
	// TEMPORARY DIAGNOSTIC (2026-08-26). The drops draw nothing and every link in the
	// chain reads correct, which is the standing signal to stop reading and instrument.
	// The render path may not log (invariant 1), so the bits are DEFERRED to clbkPreStep.
	if (rainVC)                  glassDiag |= 0x01;
	if (rainGlassOK)             glassDiag |= 0x02;
	if (ipiDepthOK)              glassDiag |= 0x04;
	if (g_fx.rainGlass > 0.001f) glassDiag |= 0x08;
	if (viewH > 0)               glassDiag |= 0x10;
	if (rainVC && rainGlassOK && ipiDepthOK && g_fx.rainGlass > 0.001f &&
	    g_fx.rainGlassSize > 0.05f && viewH > 0 &&
	    FillProjCam(cpos, Rcam, tanAp)) {
		glassDiag |= 0x20;
		// COVERAGE = the user's target x the glass fill (2026-08-26, his build-up ask).
		// rainGlassWet is the canopy's own soak scalar (UpdateRain, GLASS_RISE), so the
		// window FILLS toward wherever the Glass drops slider points - each cell's hash
		// is its birth order, so drops pop in one at a time. The 0.9 keeps the approved
		// full-storm look: slider 2.0 lands on the same ~90% cell occupancy he signed
		// off, not a wall-to-wall 100%.
		// ⚠️ rainIntensityLive is deliberately NOT a factor here: it carries the altitude
		// gate through s_gateF semantics (climb out of the storm and the drops on your
		// glass would thin out mid-climb, which is backwards - they were deposited). The
		// glass scalar already snaps to zero with the pill, which is the A/B rule.
		// ⚠️ rainGateLive, NOT rainIntensityLive: the gate says "is this a place drops
		// can exist" (right world, below the weather - fading over the same 5-9 km band,
		// which reads as the airflow stripping them on the climb), while the envelope
		// says "is it raining NOW" - and deposited drops must survive the storm easing.
		// Without the gate the fill scalar - which deliberately keeps running everywhere,
		// like every envelope - would put drops on the glass in ORBIT.
		float ds = g_fx.rainGlass; if (ds > 2.0f) ds = 2.0f;
		float gw = g_fx.rainGlassWet; if (gw < 0.0f) gw = 0.0f; if (gw > 1.0f) gw = 1.0f;
		dr = 0.9f * ds * 0.5f * gw * rainGateLive;
	}

	// Nothing to do at all - and BOTH terms have to be idle, because the drops must
	// survive a Gloom slider at zero (they are not the overcast, they are on the window).
	if (gl <= 0.002f && dr <= 0.002f) return;

	SURFHANDLE hBB = pCore->GetBackBufferHandle();
	if (!hBB || !pCore->CopyResource(hFrameTex, hBB)) return;
	pIPIGloom->SetTexture("tSrc", hFrameTex, IPF_CLAMP_U | IPF_CLAMP_V | IPF_LINEAR);
	pIPIGloom->SetOutput(0, hBB);
	pIPIGloom->SetFloat("fGloom", gl > 1.0f ? 1.0f : gl);

	if (dr > 0.002f) {
		// ⚠️ PATCH (h). If the bind FAILS the buffer does not exist this session
		// (SunGlare off) and the mask would be a constant - which would paper drops
		// across the instrument panel. Degrade to no drops rather than assume, exactly
		// as the Sketchpad clip does, and let the RAIN caption carry the reason.
		if (!pCore->SetIPISceneDepth(pIPIGloom, "tDepth", IPF_POINT | IPF_CLAMP)) dr = 0.0f;
		else glassDiag |= 0x40;
	}
	glassDiagDr = dr;

	if (dr > 0.002f) {
		// THE CAMERA AXES, EXPRESSED IN THE VESSEL FRAME. This is what nails the drops
		// to the glass while the pilot looks around: the shader turns each pixel into a
		// view ray, rotates it here, and hashes the lattice on the result - so the field
		// is a property of the airframe, not of where the eye happens to point.
		// Camera axes in WORLD are the COLUMNS of Rcam (ProjectToUV uses tmul, i.e.
		// Rcam^T, to go world -> camera). tmul against the vessel rotation then takes
		// them the rest of the way into the vessel frame.
		const VECTOR3 cR = tmul(rainGlassRot, _V(Rcam.m11, Rcam.m21, Rcam.m31));
		const VECTOR3 cU = tmul(rainGlassRot, _V(Rcam.m12, Rcam.m22, Rcam.m32));
		const VECTOR3 cF = tmul(rainGlassRot, _V(Rcam.m13, Rcam.m23, Rcam.m33));
		const float axR[3] = { (float)cR.x, (float)cR.y, (float)cR.z };
		const float axU[3] = { (float)cU.x, (float)cU.y, (float)cU.z };
		const float axF[3] = { (float)cF.x, (float)cF.y, (float)cF.z };
		const float run[3] = { (float)rainGlassRun.x, (float)rainGlassRun.y, (float)rainGlassRun.z };

		// SIZE, as lattice cells per radian - so it is an ANGULAR size and a drop does
		// not change when the viewport does. 100 cells/rad puts a mid-size drop at
		// ~13 px across on a 1920-wide frame at a typical VC aperture, which is about
		// what a 5 mm drop on a canopy 0.8 m from the eye actually subtends.
		// ⚠️ That is a good deal smaller than a windscreen PHOTOGRAPH suggests, and the
		// difference is the camera: a phone sits ~30 cm from the glass, an eye sits
		// 60-80 cm from a canopy, so the same drop covers about half the angle.
		// 0..3 since 2026-08-26 (his spec); at/below 0.05 the entry condition above has
		// already switched the drops off entirely, so this floor only guards the divide.
		float dsz = g_fx.rainGlassSize; if (dsz < 0.08f) dsz = 0.08f; if (dsz > 3.0f) dsz = 3.0f;
		float dln = g_fx.rainGlassLens; if (dln < 0.0f) dln = 0.0f; if (dln > 2.0f) dln = 2.0f;

		pIPIGloom->SetFloat("fDrop",     dr);
		pIPIGloom->SetFloat("fDropCell", 100.0f / dsz);
		pIPIGloom->SetFloat("fDropLens", 4.0f * dln);
		// (The 1.6 m glass-plane guess that used to sit here is GONE - the mask is the
		// SIGN of the depth now: mesh groups the author flags with FLAG 1000 write their
		// distance negated in the client's depth pass, patch (h) part 2. His design,
		// 2026-08-26: "the burden is on the user to apply it to the correct groups" -
		// exact per-pixel windows on any hull, no per-class number to tune, and the deep
		// cabin that broke the plane heuristic cannot break an authored mask.)
		pIPIGloom->SetFloat("fDropDbg",  g_fx.rainGlassDbg);   // TEMPORARY scaffold
		// THE RUNNERS (2026-08-27). Amount is his knob; SPEED is not - it derives from
		// the sensed |gravity + airflow| (invariant 25e), mapped to column-cells/s.
		// The square term is what separates the regimes: parked (mag ~9.8) a runner
		// crawls the pane in ~20 s; at approach speeds it sweeps aft in under a second.
		{
			float ra = g_fx.rainGlassRunners; if (ra < 0.0f) ra = 0.0f; if (ra > 3.0f) ra = 3.0f;
			const float mag  = rainGlassRunMag;
			float radS = 0.045f + mag * mag * 4.0e-5f;         // rad/s along the meridian
			if (radS > 0.9f) radS = 0.9f;
			// ... INTEGRATED here, never multiplied by the clock in the shader (2026-09-06,
			// his runway report: engine cut, still rolling, "the drops sliding on the
			// window reverse their direction"). fTime x radS tracks the product's
			// DERIVATIVE: at 300 s of session a drop of radS from 0.15 to 0.11 moves the
			// heads 12 rad backward while the true motion is 0.11 rad/s forward. The
			// sheet had this exact bug on 08-27 (rainSheetPh); the runners, built the
			// same day, were not swept. Real time, so it freezes under pause with animT.
			{
				const float dtp = (rainGlassRunPhT >= 0.0f && animT > rainGlassRunPhT) ? (animT - rainGlassRunPhT) : 0.0f;
				rainGlassRunPhT = animT;
				rainGlassRunPh += dtp * radS;
				if (rainGlassRunPh > 1.0e4f) rainGlassRunPh -= 1.0e4f;   // bounded; the shader wraps mod 0.45
			}
			pIPIGloom->SetFloat("fRunAmt", ra);
			pIPIGloom->SetFloat("fRunPh",  rainGlassRunPh);
			// Drop size scales BOTH families; Runner size sets only their RATIO -
			// one uniform carries the product, and the shader's column-overflow cap
			// guards the combined extreme.
			float rsz = g_fx.rainGlassRunSize; if (rsz < 0.4f) rsz = 0.4f; if (rsz > 2.0f) rsz = 2.0f;
			pIPIGloom->SetFloat("fRunSize", dsz * rsz);
			pIPIGloom->SetFloat("fTime",   animT);             // real time (invariant 4)
			// The streaks' polar chart: an orthonormal basis AROUND the run axis, built
			// from whichever vessel axis is least parallel to it so it can never
			// degenerate. The chart is anchored to the AXIS, not to any per-pixel
			// projection - the projection is exactly what rang the forward window.
			// ref = vessel +X, always: the run axis lives in the vessel's Y/Z plane
			// (gravity down, airflow aft) short of extreme sideslip, so X stays
			// clear of it through the WHOLE down->aft tilt of an acceleration - a
			// conditional ref would snap the chart once mid-takeoff.
			const VECTOR3 R1  = rainGlassRun;
			const VECTOR3 ref = (fabs(R1.x) < 0.9) ? _V(1, 0, 0) : _V(0, 1, 0);
			VECTOR3 R2 = crossp(ref, R1); R2 = R2 / length(R2);
			const VECTOR3 R3 = crossp(R1, R2);
			const float rA[3] = { (float)R2.x, (float)R2.y, (float)R2.z };
			const float rB[3] = { (float)R3.x, (float)R3.y, (float)R3.z };
			pIPIGloom->SetFloat("vGlRunA", rA, sizeof(rA));
			pIPIGloom->SetFloat("vGlRunB", rB, sizeof(rB));
		}
		pIPIGloom->SetFloat("fTanAp",    (float)tanAp);
		pIPIGloom->SetFloat("fAspect",   (float)viewW / (float)viewH);
		pIPIGloom->SetFloat("vGlR",   axR, sizeof(axR));
		pIPIGloom->SetFloat("vGlU",   axU, sizeof(axU));
		pIPIGloom->SetFloat("vGlF",   axF, sizeof(axF));
		pIPIGloom->SetFloat("vGlRun", run, sizeof(run));
	}
	else {
		pIPIGloom->SetFloat("fDrop", 0.0f);
		// UNBIND, don't just branch past it. The shader's tDepth fetch is inside a
		// uniform branch and is not taken here, but leaving a texture bound across
		// frames is how a pointer outlives the thing it points at - the 23(m) shape.
		// The interface is per session (ReleaseDeviceResources) and the bind is redone
		// every frame, so this is belt and braces rather than a live bug; it costs one
		// call in a path that is already doing nothing.
		pIPIGloom->SetTexture("tDepth", NULL, 0);
	}

	pIPIGloom->Execute((DWORD)0, true, gcIPInterface::Rect);
}

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
void OroModule::DrawPlumePoly(oapi::Sketchpad* pSkp, bool depthClip, bool writeAlpha)
{
	if (plmVtxN <= 0 || !pCore) return;
	if (!hPlumePoly) {
		static gcCore::clrVtx zero[PLM_MAX_TRI * 3];    // zero-init: degenerate, alpha 0
		hPlumePoly = pCore->CreateTriangles(NULL, zero, PLM_MAX_TRI * 3, PF_TRIANGLES);
	}
	if (!hPlumePoly) return;

	DWORD blend = padAdditive ? 0x5 : (DWORD)oapi::Sketchpad::BlendState::ALPHABLEND;
	if (writeAlpha) blend |= 0x200;                 // patch (u): lay down coverage alpha
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
			if (writeAlpha) dkBlend |= 0x200;       // the soot is part of the same image
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
		// ⚠️ THE ELEVEN EFFECT PILLS AND THE WHOLE PILOT MODEL LEFT THIS TABLE ON
		// 2026-08-25 - they are in PILOTSET below, because they can now be written to
		// EITHER this file or a vessel class's, and a key can only have one home.
		// MasterArmed above and ScenarioSound further down deliberately did NOT go with
		// them: see the Save target note on PILOTSET for why those two must stay global.
		// ⚠️ THE SIX CAM-SHAKE KNOBS MOVED TO VCSET on 2026-08-25, for the same reason the
		// pilot block moved and a better one: what a hull transmits to the seat is a fact
		// about its mass and size, not about the person in it.
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
		// ⚠️ VCShadowsOn MOVED TO VCSET TOO (2026-08-25). The cabin box and the shadow
		// depth beside it were already per class; the on/off was the odd one out.
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
		// RAIN - the runway slice. GLOBAL for v1: one world, one storm. The pill only;
		// rainTest is deliberately NOT saved, for the same reason CANCEL THRUST is not
		// (23i) - a test rig that comes back on at load reads as a bug in the weather.
		// FOG (2026-09-05, client patch aa) - the FOG leaf. GLOBAL like the rain, and for
		// the same reason; fogTest is transient and never written (23i's rule again).
		{ "FogOn",            &g_fx.fogEnabled,       ST_B },
		{ "FogVisibility",    &g_fx.fogVis,           ST_F },
		{ "FogTop",           &g_fx.fogTop,           ST_F },
		{ "FogFade",          &g_fx.fogFade,          ST_F },
		{ "FogBrightness",    &g_fx.fogBright,        ST_F },
		{ "FogSunGlow",       &g_fx.fogGlow,          ST_F },
		// BASE LIGHTS (patch ac) - one setting, two pages (RAIN + FOG), GLOBAL.
		{ "BaseLightsOn",     &g_fx.baseLightsOn,     ST_B },
		{ "BaseLightsGlow",   &g_fx.baseLightsGlow,   ST_F },
		{ "BaseLightsHalo",   &g_fx.baseLightsHalo,   ST_F },
		{ "RainOn",           &g_fx.rainEnabled,      ST_B },
		{ "RainGloom",        &g_fx.rainGloom,        ST_F },
		{ "RainGlass",        &g_fx.rainGlass,        ST_F },
		{ "RainGlassSize",    &g_fx.rainGlassSize,    ST_F },
		{ "RainGlassLens",    &g_fx.rainGlassLens,    ST_F },
		{ "RainGlassRise",    &g_fx.rainGlassRise,    ST_F },
		{ "RainGlassRunners", &g_fx.rainGlassRunners, ST_F },
		{ "RainGlassRunSize", &g_fx.rainGlassRunSize, ST_F },
		{ "RainDensity",      &g_fx.rainDensity,      ST_F },
		{ "RainStreak",       &g_fx.rainStreak,       ST_F },
		{ "RainStreakGlow",   &g_fx.rainStreakA,      ST_F },
		{ "RainSpeed",        &g_fx.rainSpeed,        ST_F },
		{ "RainAngle",        &g_fx.rainAngle,        ST_F },
		{ "RainPuddle",       &g_fx.rainPuddle,       ST_F },
		{ "RainWetDark",      &g_fx.rainWetDark,      ST_F },
		{ "RainGlint",        &g_fx.rainGlint,        ST_F },
		{ "RainRefl",         &g_fx.rainRefl,         ST_F },
		{ "RainSwimAmp",      &g_fx.rainSwimAmp,      ST_F },
		{ "RainSwimRate",     &g_fx.rainSwimRate,     ST_F },
		{ "RainPoolSize",     &g_fx.rainPoolSize,     ST_F },
		{ "RainPoolReach",    &g_fx.rainPoolReach,    ST_F },
		{ "RainCloudLvl",     &g_fx.rainCloudLvl,     ST_F },
		{ "RainGrainOp",      &g_fx.rainGrainOp,      ST_F },
		{ "RainGrainSize",    &g_fx.rainGrainSize,    ST_F },
		{ "RainLtg",          &g_fx.rainLtg,          ST_F },
		{ "RainBoltBloom",    &g_fx.rainBoltBloom,    ST_F },
		{ "RainReflBlur",     &g_fx.rainReflBlur,     ST_F },
		{ "RainSound",        &g_fx.rainSoundVol,     ST_F },
		{ "RainThunder",      &g_fx.rainThunder,      ST_F },
		{ "RainViewMode",     &g_fx.rainViewMode,     ST_I },
		{ "VapourOn",         &g_fx.vapEnabled,       ST_B },
	};
	const int NSETTINGS = (int)(sizeof(SETTINGS) / sizeof(SETTINGS[0]));

	// --- THE PILOT BLOCK: the one table that can live in EITHER scope --------
	// Everything the G-FORCE tab owns except the two housekeeping keys. It is a SEPARATE
	// table rather than a marked subset of SETTINGS so that there is exactly ONE list of
	// these keys in the project: the global writer appends this table, the class writer
	// appends the same table, and neither can drift from the other.
	//
	// WHY IT MOVES AT ALL (his ask, 2026-08-25): "different vessels might have different
	// seating positions for the crew". Posture and the camera-vs-CoM reference really are
	// facts about an airframe, not about the person flying it, and there was no way to say
	// so. `g_fx.pilotPerClass` - the panel's Save target button - decides which file this
	// block is written to, and it is stored per class so the button always describes the
	// hull you are in.
	//
	// ⚠️ THE READ RULE IS A THIRD ONE, DIFFERENT FROM BOTH THAT ALREADY EXIST (invariant
	// 17a). A class with no LOOK settings KEEPS the current numbers, and a world with no
	// aurora file gets the BUILT-IN DEFAULTS; a hull with no pilot block gets neither. It
	// falls back to the GLOBAL values, held in g_pilotGlobal below. Anything else is wrong
	// here: "keep current" would silently fly a stock DeltaGlider with the seating position
	// you tuned for a DG-S, and "defaults" would throw your pilot away for visiting an
	// untuned hull. Falling back to global is the only rule under which an unconfigured
	// vessel flies as the pilot you actually configured.
	const SetItem PILOTSET[] = {
		// the eleven effect pills - enables only, never the scripted channels themselves
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
		// pilot / felt-G model - the seating position is what started this
		{ "PhysicsMode",      &g_fx.physicsMode,      ST_B },
		{ "GTolerance",       &g_fx.gTolerance,       ST_F },
		{ "GSuit",            &g_fx.gsuitOn,          ST_B },
		{ "PilotPose",        &g_fx.pilotPose,        ST_I },
		{ "GRefCamera",       &g_fx.gRefCamera,       ST_B },
		{ "FxVCOnly",         &g_fx.fxVCOnly,         ST_B },
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
	const int NPILOTSET = (int)(sizeof(PILOTSET) / sizeof(PILOTSET[0]));

	// The GLOBAL pilot values, kept in memory so an unconfigured hull has something to fall
	// back TO. Refreshed whenever Config\ORO.cfg is read, and whenever it is written with
	// the block in it - i.e. exactly when the global copy changes. Every SetType is a
	// 4-byte scalar, so this is driven off the table itself and cannot drift from it the
	// way a hand-written mirror struct would.
	// ⚠️ ONE SetVal FOR THE WHOLE FILE. The per-BODY defaults further down needed exactly
	// this and carried their own copy of the type plus their own pair of switch loops; the
	// declaration lives here now, ahead of its first user, and both consumers share the
	// generic pair below instead of open-coding the same three cases twice.
	struct SetVal { float f; int i; bool b; };
	SetVal g_pilotGlobal[NPILOTSET];
	bool   g_pilotGlobalOk = false;          // false until the first read/write
	// ⚠️ IS THE LIVE PILOT BLOCK A HULL'S, OR THE GLOBAL ONE? This exists to keep the
	// feature from costing anything to people who never use it. Restoring the global values
	// on EVERY class change would be simpler and would quietly throw away unsaved pilot
	// tweaks whenever the focus vessel's class changed - a regression for everybody, in
	// service of a case that only arises once some hull actually owns a block. So the undo
	// is conditional: with this false, a class change does not touch the pilot at all, and
	// the behaviour is bit-for-bit what it was before the Save target existed.
	bool   g_pilotFromClass = false;

	void SnapTable(const SetItem* t, int n, SetVal* out)
	{
		for (int i = 0; i < n; i++) {
			switch (t[i].t) {
			case ST_F: out[i].f = *(float*)t[i].p; break;
			case ST_B: out[i].b = *(bool*) t[i].p; break;
			case ST_I: out[i].i = *(int*)  t[i].p; break;
			}
		}
	}
	void RestoreTable(const SetItem* t, int n, const SetVal* in)
	{
		for (int i = 0; i < n; i++) {
			switch (t[i].t) {
			case ST_F: *(float*)t[i].p = in[i].f; break;
			case ST_B: *(bool*) t[i].p = in[i].b; break;
			case ST_I: *(int*)  t[i].p = in[i].i; break;
			}
		}
	}

	// --- THE SECOND MOVABLE BLOCK: the VC tab (2026-08-25) -------------------
	// The VC tab's GLOBAL keys. The cabin box and the shadow depth are NOT here - they
	// have been per class since patch (p) and stay that way unconditionally, because how
	// a virtual cockpit was authored is never a global fact.
	// ⚠️ CAM-SHAKE IS WHY THIS EXISTS, and it is a better case than the pilot's: amplitude
	// and frequency describe what a HULL passes to the seat. His words - "a slow turning
	// behemoth like the XR5 Vanguard cannot and should not produce the same shake/rattle
	// like the tiny shuttle-PB". Invariant 9 already says the STRENGTH is physics-driven
	// from thrust, dynamic pressure and ground contact; these six shape what that strength
	// looks like, and that shape belongs to the airframe.
	const SetItem VCSET[] = {
		{ "VCShadowsOn",      &g_fx.vcShadows,        ST_B },
		{ "VCNightOn",        &g_fx.vcNight,          ST_B },   // patch (ad): the cabin at night
		{ "VCNightFloor",     &g_fx.vcNightFloor,     ST_F },
		{ "VCWeatherDimOn",   &g_fx.vcWxDimOn,        ST_B },   // ... and under the weather
		{ "VCWeatherDim",     &g_fx.vcWxDim,          ST_F },
		{ "VCRainSound",      &g_fx.vcRainSound,      ST_F },   // the cabin's own rain loops (2026-09-06)
		{ "RainHullVol",      &g_fx.rainHullVol,      ST_F },   // moved here from the RAIN table the same
		                                                        //   day - the drum is about the HULL (28q)
		{ "ShakeOn",          &g_fx.shakeEnabled,     ST_B },
		{ "ShakeAmpX",        &g_fx.shakeAmpX,        ST_F },
		{ "ShakeAmpY",        &g_fx.shakeAmpY,        ST_F },
		{ "ShakeAmpZ",        &g_fx.shakeAmpZ,        ST_F },
		{ "ShakeFreq",        &g_fx.shakeFreq,        ST_F },
		{ "ShakePush",        &g_fx.shakePush,        ST_F },   // the lean, split from the buffet
	};
	const int NVCSET = (int)(sizeof(VCSET) / sizeof(VCSET[0]));
	SetVal g_vcGlobal[NVCSET];
	bool   g_vcGlobalOk  = false;
	bool   g_vcFromClass = false;

	// --- THE MOVABLE-BLOCK REGISTER -----------------------------------------
	// Two blocks now, and the four places that care (global read, global write, class read,
	// class write) all just LOOP over this. Adding a third tab's block later is one table
	// and one row here, with no fifth copy of the fallback rule to get subtly wrong -
	// which is the whole reason this is a register rather than a second hand-written set
	// of g_pilot* / g_vc* branches.
	struct MovBlock {
		const char*    name;        // for the log line, so a load says WHICH block moved
		const SetItem* tbl;
		int            n;
		bool*          perClass;    // the Save target flag: a g_fx field, saved in CLASSSET
		SetVal*        globalVal;   // the global copy - what an unconfigured hull falls back to
		bool*          globalOk;
		bool*          fromClass;   // is the LIVE block a hull's, rather than the global one?
	};
	const MovBlock MOVBLK[] = {
		{ "pilot", PILOTSET, NPILOTSET, &g_fx.pilotPerClass, g_pilotGlobal, &g_pilotGlobalOk, &g_pilotFromClass },
		{ "VC",    VCSET,    NVCSET,    &g_fx.vcPerClass,    g_vcGlobal,    &g_vcGlobalOk,    &g_vcFromClass    },
	};
	const int NMOVBLK = (int)(sizeof(MOVBLK) / sizeof(MOVBLK[0]));

	// --- PER VESSEL CLASS: everything whose right value depends on the hull ---
	// Size, shape and engine layout decide all of these, so they are remembered
	// against the class name and swapped in when the focus vessel changes.
	const SetItem CLASSSET[] = {
		// Does this hull keep its OWN pilot settings? The Save target button. It is read
		// with the rest of the class file, and it is what tells the loader whether to look
		// for a PILOTSET block in here at all. ⚠️ It must be CLEARED before the file is
		// read, not merely left to a missing key: the class loader's rule for an absent key
		// is "keep the current value", which would let one hull's Save target leak into the
		// next hull that has no file. LoadClass clears it explicitly.
		{ "PilotScope",       &g_fx.pilotPerClass,    ST_B },
		{ "VCScope",          &g_fx.vcPerClass,       ST_B },   // ... and the VC tab's
		{ "Shimmer",          &g_fx.shimmer,          ST_F },   // engine haze: amplitude,
		{ "ShimmerOfs",       &g_fx.shimmerOfs,       ST_F },   //   axial slide [m], plus
		{ "ShimmerWave",      &g_fx.shimmerWave,      ST_F },   //   the wave texture's
		{ "ShimmerFreq",      &g_fx.shimmerFreq,      ST_F },   //   scale + churn (26-09-04)
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
		{ "BellTint",         &g_fx.bellTint,         ST_I },   //   and its hue pick
		{ "PlumeCells",       &g_fx.plumeCells,       ST_F },   //   the disc count,
		{ "PlumeDiamond",     &g_fx.plumeDiamond,     ST_F },   //   strength + the shape
		{ "PlumeDiaShape",    &g_fx.plumeDiaShape,    ST_F },   //   knobs (lozenge profile,
		{ "PlumeDiaSize",     &g_fx.plumeDiaSize,     ST_F },   //   width - the key predates
		{ "PlumeDiaLength",   &g_fx.plumeDiaLen,      ST_F },   //   the length/width split -
		{ "PlumeDiaOfs",      &g_fx.plumeDiaOfs,      ST_F },   //   + train slide [m])
		{ "PlumeSpacing",     &g_fx.plumeSpacing,     ST_F },   //   + the two colour
		{ "PlumeBloomWid",    &g_fx.plumeBloomWid,    ST_F },   //   picks, per class like
		{ "PlumeBloomBri",    &g_fx.plumeBloomBri,    ST_F },   //   the shimmer (nozzle
		{ "PlumeThroat",      &g_fx.plumeThroat,      ST_F },   //   + the throat fire
		{ "PlumeThroatOfs",   &g_fx.plumeThroatOfs,   ST_F },   //   and its axial slide,
		{ "PlumeColJet",      &g_fx.plumeColJet,      ST_I },   //   layout is a property
		{ "PlumeColBloom",    &g_fx.plumeColBloom,    ST_I },   //   of the hull)
		{ "PlumeColDia",      &g_fx.plumeColDia,      ST_I },   //   + the diamond tint
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
	{ "PrtColour2",       &g_fx.prtColour2,       ST_I },
	{ "PrtTexStock",      &g_fx.prtTexStock,      ST_B },
		{ "ReentryTrim",      &g_fx.reentry,          ST_F },
		{ "PlasSaturation",   &g_fx.plasSat,          ST_F },
		{ "PlasHullLight",    &g_fx.plasLight,        ST_F },
		{ "PlasStreakLen",    &g_fx.plasStreakLen,    ST_F },
		{ "PlasStreakWid",    &g_fx.plasStreakWid,    ST_F },
		{ "PlasWander",       &g_fx.plasWander,       ST_F },
		{ "PlasChurn",        &g_fx.plasChurn,        ST_F },   // how fast the wake lives
		{ "PlasFinRake",      &g_fx.plasFinRake,      ST_F },   // ... and how far it splays
		{ "PlasVCGlow",       &g_fx.plasVCGlow,       ST_F },   // the cockpit sheath
		{ "PlasCabinWash",    &g_fx.plasCabin,        ST_F },   // where the cockpit glow lands
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
		{ "VapourStrength",   &g_fx.vapStrength,      ST_F },   // the dialog says OPACITY
		                                                        // since 2026-08-29; the key
		                                                        // stays so tuned cfgs load
		{ "VapourSize",       &g_fx.vapSize,          ST_F },   // = Size X, the master. Y/Z
		{ "VapourSizeY",      &g_fx.vapSizeY,         ST_F },   //   are RATIOS of it, so a
		{ "VapourSizeZ",      &g_fx.vapSizeZ,         ST_F },   //   pre-split cfg (no Y/Z
		                                                        //   keys -> defaults 1.0)
		                                                        //   loads as the old cone
		                                                        //   bit for bit
		{ "VapourStreaks",    &g_fx.vapStreaks,       ST_F },   // 0 = clean sheet (default)
		{ "VapourStreakChurn",&g_fx.vapStreakChurn,   ST_F },
		{ "VapourColour",     &g_fx.vapColour,        ST_I },   // COLORREF bits via int,
		{ "VapourStreakCol",  &g_fx.vapStreakCol,     ST_I },   //   the PlasmaTint pattern
		{ "VapourBaseFill",   &g_fx.vapBaseOn,        ST_B },   // the base disc pill
		{ "VapourBaseOfs",    &g_fx.vapBaseOfs,       ST_F },   //   + its axial offset (26-09-04)
		{ "VapourPos",        &g_fx.vapPos,           ST_F },   // "Position z" on the panel;
	                                                        //   key unchanged (old cfgs)
	{ "VapourPosX",       &g_fx.vapPosX,          ST_F },   // full placement, 2026-08-30
	{ "VapourPosY",       &g_fx.vapPosY,          ST_F },   //   (missing keys = 0 = the
	{ "VapourPitch",      &g_fx.vapPitch,         ST_F },   //   pre-fix cone exactly)
	{ "VapourYaw",        &g_fx.vapYaw,           ST_F },
		{ "VapourMachMin",    &g_fx.vapMachMin,       ST_F },   // the band handles: where
		{ "VapourMachMax",    &g_fx.vapMachMax,       ST_F },   //   the shroud lives
		{ "VapourFlickHz",    &g_fx.vapFlickHz,       ST_F },
		// THE SECOND CONE (2026-08-29): the identical set with a 2. A cfg without
		// these keys loads the defaults, whose opacity is 0 - no second cone until
		// a hull is given one, so nothing older changes look.
		{ "Vapour2Strength",  &g_fx.vapStrength2,     ST_F },
		{ "Vapour2Size",      &g_fx.vapSize2,         ST_F },
		{ "Vapour2SizeY",     &g_fx.vapSizeY2,        ST_F },
		{ "Vapour2SizeZ",     &g_fx.vapSizeZ2,        ST_F },
		{ "Vapour2Streaks",   &g_fx.vapStreaks2,      ST_F },
		{ "Vapour2StreakChurn",&g_fx.vapStreakChurn2, ST_F },
		{ "Vapour2Colour",    &g_fx.vapColour2,       ST_I },
		{ "Vapour2StreakCol", &g_fx.vapStreakCol2,    ST_I },
		{ "Vapour2BaseFill",  &g_fx.vapBaseOn2,       ST_B },
		{ "Vapour2BaseOfs",   &g_fx.vapBaseOfs2,      ST_F },
		{ "Vapour2Pos",       &g_fx.vapPos2,          ST_F },
	{ "Vapour2PosX",      &g_fx.vapPosX2,         ST_F },
	{ "Vapour2PosY",      &g_fx.vapPosY2,         ST_F },
	{ "Vapour2Pitch",     &g_fx.vapPitch2,        ST_F },
	{ "Vapour2Yaw",       &g_fx.vapYaw2,          ST_F },
		{ "Vapour2MachMin",   &g_fx.vapMachMin2,      ST_F },
		{ "Vapour2MachMax",   &g_fx.vapMachMax2,      ST_F },
		{ "Vapour2FlickHz",   &g_fx.vapFlickHz2,      ST_F },

		{ "CopShift",         &g_fx.copShift,         ST_F },   // the most per-vessel
		                                                        // number in the addon
		{ "CopReentryOnly",   &g_fx.copReentryOnly,   ST_B },   // ... and WHEN it may act.
		                                                        // ⚠️ A cfg written before
		                                                        // 2026-08-15 has no such key,
		                                                        // so it loads the DEFAULT -
		                                                        // which is the gate. That is
		                                                        // deliberate: the shipped
		                                                        // cfgs are exactly the ones
		                                                        // that caused the PIO report.
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
	// SetVal, SnapTable and RestoreTable are declared up beside PILOTSET, which is their
	// first user; this pair was the SECOND and used to carry its own copy of all three.
	SetVal g_bodyDefault[NBODYSET];
	bool   g_bodyDefaultsCaptured = false;

	void CaptureBodyDefaults()
	{
		if (g_bodyDefaultsCaptured) return;
		SnapTable(BODYSET, NBODYSET, g_bodyDefault);
		g_bodyDefaultsCaptured = true;
	}

	void RestoreBodyDefaults()
	{
		CaptureBodyDefaults();
		RestoreTable(BODYSET, NBODYSET, g_bodyDefault);
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

	// The same, but from a SNAPSHOT rather than from the live fields. This is how the
	// global file keeps its own copy of a movable block while a hull is flying different
	// values: write what global HAD, not what is on the sliders. Doing it this way rather
	// than swapping the snapshot in, writing, and swapping back means the live state is
	// never disturbed even for an instant - no temporary, and nothing to restore if the
	// write fails halfway.
	void WriteTableFrom(FILEHANDLE f, const SetItem* t, int n, const SetVal* v)
	{
		for (int i = 0; i < n; i++) {
			switch (t[i].t) {
			case ST_F: oapiWriteItem_float(f, K(t[i].key), (double)v[i].f); break;
			case ST_B: oapiWriteItem_bool (f, K(t[i].key), v[i].b);         break;
			case ST_I: oapiWriteItem_int  (f, K(t[i].key), v[i].i);         break;
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
// Defined further down, beside the per-group table they walk.
static void ThrWriteGroups(FILEHANDLE f);
static void ThrReadGroups(FILEHANDLE f);

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
			// ⚠️ THE GLOBAL FILE ALWAYS CARRIES A PILOT BLOCK - the question is WHOSE.
			// The obvious implementation is "skip it when a hull owns the pilot", and that
			// is wrong, because oapiOpenFile(FILE_OUT) TRUNCATES: skipping would DELETE the
			// global copy and every unconfigured hull would silently fall back to factory
			// defaults from then on. So when the Save target is THIS VESSEL CLASS the live
			// values belong to the hull and the SNAPSHOT is written instead, preserving the
			// global copy untouched. Only an ALL VESSELS save may overwrite it.
			for (int b = 0; b < NMOVBLK; b++) {
				const MovBlock& mb = MOVBLK[b];
				if (*mb.perClass && *mb.globalOk) {
					WriteTableFrom(f, mb.tbl, mb.n, mb.globalVal);
				} else {
					WriteTable(f, mb.tbl, mb.n);
					SnapTable(mb.tbl, mb.n, mb.globalVal);       // the global copy just moved
					*mb.globalOk = true;
				}
			}
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
			ThrWriteGroups(fc);      // ... and every thruster group's own set
			// ... and this hull's own pilot, when the Save target says it has one. The
			// PilotScope key written by CLASSSET above is what sends the loader looking.
			for (int b = 0; b < NMOVBLK; b++) {
				const MovBlock& mb = MOVBLK[b];
				if (*mb.perClass) {
					WriteTable(fc, mb.tbl, mb.n);
					// The live values are now THIS hull's, so flying away from it has to
					// put the global block back. Without this, saving a hull-specific block
					// and then switching vessel would carry those numbers onto the next ship.
					*mb.fromClass = true;
				} else {
					// Nothing written, and the file was rewritten whole - so any block this
					// hull used to own is GONE, along with the scope flag that pointed at it
					// (CLASSSET wrote that false a few lines up). This is what makes
					// "switch back to ALL VESSELS and save" actually stick.
					*mb.fromClass = false;
				}
			}
			oapiCloseFile(fc, FILE_OUT);
			// The class cache: this file just changed on disk, so any cached copy of
			// it is stale. The focus class resolves live anyway; this covers the
			// focus-it, tune-it, save-it, focus-away round trip.
			OroThr_CacheDrop(g_setClass);
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
	// The pilot fallback must exist even with NO global file, or the very first hull that
	// declares its own block would leave nothing to fall back to when you fly away from it.
	// Snapshot the built-in values now; the read below replaces them if the file has any.
	for (int b = 0; b < NMOVBLK; b++) {
		SnapTable(MOVBLK[b].tbl, MOVBLK[b].n, MOVBLK[b].globalVal);
		*MOVBLK[b].globalOk = true;
	}
	FILEHANDLE f = oapiOpenFile(SETTINGS_FILE, FILE_IN, CONFIG);
	if (!f) return;                         // no file yet - built-in defaults stand
	const int n = ReadTable(f, SETTINGS, NSETTINGS);
	// The movable blocks are read UNCONDITIONALLY, whatever any hull says. This file is the
	// fallback every unconfigured vessel lands on, so its copy is always wanted in memory.
	int nm = 0, nmTot = 0;
	for (int b = 0; b < NMOVBLK; b++) {
		nm    += ReadTable(f, MOVBLK[b].tbl, MOVBLK[b].n);
		nmTot += MOVBLK[b].n;
		SnapTable(MOVBLK[b].tbl, MOVBLK[b].n, MOVBLK[b].globalVal);
	}
	oapiCloseFile(f, FILE_IN);
	oapiWriteLogV("ORO: global settings loaded (%d of %d items, movable %d of %d).",
	              n, NSETTINGS, nm, nmTot);
}

// ----------------------------------------------------------------------------
// THE PANEL'S OWN HEIGHT (2026-08-16). The dialog is vertically resizable now, and it has
// to come back the size you left it or you resize it every single session.
//
// ⚠️ ITS OWN FILE, AND THAT IS THE WHOLE POINT. The obvious home is a key in the global
// table, which would be one line - and it would be wrong. Orbiter's config writer has no
// notion of updating a single key: oapiOpenFile(FILE_OUT) TRUNCATES, so persisting the
// height through the global table means rewriting Config\ORO.cfg in full. Dragging the
// window would then silently commit every slider the user had moved and not saved, which
// breaks the one promise the panel makes out loud - the readme says in as many words that
// nothing is permanent until you press SAVE. A window size is chrome, not tuning; it does
// not belong inside that contract and it does not get to write that file.
// So: Config\ORO\window.cfg, written on the resize itself, read when the dialog opens.
// It is also why this pair does NOT live in the SETTINGS table and has no SAVE button.
// It holds THREE numbers now (the help window joined it 2026-08-16), and because the
// writer truncates, every save writes all three - so they are kept in file-scope statics
// that are primed by the first load and updated by whichever window moved.
static const char* WINDOW_FILE = "ORO\\window.cfg";
static int s_dlgH = 0, s_helpW = 0, s_helpH = 0;   // 0 = "no saved value, use the default"

static void WindowCfgRead()
{
	static bool tried = false;
	if (tried) return;
	tried = true;
	FILEHANDLE f = oapiOpenFile(WINDOW_FILE, FILE_IN_ZEROONFAIL, CONFIG);
	if (!f) return;                         // never saved: every caller keeps its default
	int v = 0;
	if (oapiReadItem_int(f, K("DialogHeight"), v)) s_dlgH  = v;
	if (oapiReadItem_int(f, K("HelpWidth"),    v)) s_helpW = v;
	if (oapiReadItem_int(f, K("HelpHeight"),   v)) s_helpH = v;
	oapiCloseFile(f, FILE_IN_ZEROONFAIL);
}

static void WindowCfgWrite()
{
	CreateDirectoryA("Config\\ORO", NULL);  // harmless if it already exists
	FILEHANDLE f = oapiOpenFile(WINDOW_FILE, FILE_OUT, CONFIG);
	if (!f) { oapiWriteLogV("ORO: could not write Config\\%s", WINDOW_FILE); return; }
	oapiWriteLine(f, K("; ORO - window geometry. Written whenever you finish resizing the"));
	oapiWriteLine(f, K("; panel or the help window, so each reopens the size you left it."));
	oapiWriteLine(f, K("; Deliberately NOT in Config\\ORO.cfg: writing that file rewrites every"));
	oapiWriteLine(f, K("; setting in it, and resizing a window must never commit tuning you"));
	oapiWriteLine(f, K("; have not saved. Delete this file to get the default sizes back."));
	oapiWriteLine(f, K("; (Note there is no 'help window was open' flag, on purpose - the help"));
	oapiWriteLine(f, K(";  window never reopens by itself, only its SIZE is remembered.)"));
	oapiWriteLine(f, K(""));
	if (s_dlgH  > 0) oapiWriteItem_int(f, K("DialogHeight"), s_dlgH);
	if (s_helpW > 0) oapiWriteItem_int(f, K("HelpWidth"),    s_helpW);
	if (s_helpH > 0) oapiWriteItem_int(f, K("HelpHeight"),   s_helpH);
	oapiCloseFile(f, FILE_OUT);
}

int  OroSettings_LoadDlgHeight()       { WindowCfgRead(); return s_dlgH; }
void OroSettings_SaveDlgHeight(int h)  { if (h > 0) { WindowCfgRead(); s_dlgH = h; WindowCfgWrite(); } }

void OroSettings_LoadHelpSize(int& w, int& h) { WindowCfgRead(); w = s_helpW; h = s_helpH; }
void OroSettings_SaveHelpSize(int w, int h)
{
	if (w <= 0 || h <= 0) return;
	WindowCfgRead();                        // never write a file we have not read: it would
	s_helpW = w; s_helpH = h;               // drop the panel height saved in a past session
	WindowCfgWrite();
}

// ----------------------------------------------------------------------------
// PER-THRUSTER-GROUP plumbing (2026-08-16). The rules live above OroThrusterFx in
// OroState.h; this is the mechanism. The two Sync functions are deliberately dumb
// field-for-field copies rather than a memcpy of the struct, because the flat fields
// and the struct are two DIFFERENT types that merely happen to share names - a memcpy
// would compile today and corrupt state the moment either gains a field.
// ----------------------------------------------------------------------------
#define ORO_THR_FIELDS(A, B) \
	A.shimmerEnabled = B.shimmerEnabled;   A.shimmer = B.shimmer;                 \
	A.shimmerOfs = B.shimmerOfs;           A.shimmerWave = B.shimmerWave;         \
	A.shimmerFreq = B.shimmerFreq;         A.plumeEnabled = B.plumeEnabled;       \
	A.plume = B.plume;                     A.plumePhysics = B.plumePhysics;       \
	A.plumeExpHi = B.plumeExpHi;           A.plumeExpLo = B.plumeExpLo;           \
	A.plumeWidth = B.plumeWidth;           A.plumeLen = B.plumeLen;               \
	A.plumeCells = B.plumeCells;           A.plumeDiamond = B.plumeDiamond;       \
	A.plumeDiaShape = B.plumeDiaShape;     A.plumeDiaSize = B.plumeDiaSize;       \
	A.plumeDiaLen = B.plumeDiaLen;         A.plumeDiaOfs = B.plumeDiaOfs;         \
	A.plumeSpacing = B.plumeSpacing;       A.plumeBloomWid = B.plumeBloomWid;     \
	A.plumeBloomBri = B.plumeBloomBri;     A.plumeThroatOfs = B.plumeThroatOfs;   \
	A.plumeThroat = B.plumeThroat;         A.plumeSootRate = B.plumeSootRate;     \
	A.plumeSoot = B.plumeSoot;             A.plumeColJet = B.plumeColJet;         \
	A.plumeColBloom = B.plumeColBloom;     A.plumeColDia = B.plumeColDia;         \
	A.plumeBellOn = B.plumeBellOn;                                                \
	A.plumeBellGlow = B.plumeBellGlow;     A.plumeBellHeatT = B.plumeBellHeatT;   \
	A.plumeBellCoolT = B.plumeBellCoolT;   A.bellTint = B.bellTint;               \
	A.prtEnabled = B.prtEnabled;           A.prtOffset = B.prtOffset;             \
	A.prtSize = B.prtSize;                 A.prtLifetime = B.prtLifetime;         \
	A.prtRate = B.prtRate;                 A.prtSpeed = B.prtSpeed;               \
	A.prtSpread = B.prtSpread;             A.prtGrowth = B.prtGrowth;             \
	A.prtSlowdown = B.prtSlowdown;         A.prtDiffuse = B.prtDiffuse;           \
	A.prtAirFade = B.prtAirFade;           A.prtColour = B.prtColour;             \
	A.prtColour2 = B.prtColour2;           A.prtTexStock = B.prtTexStock;         \
	strcpy_s(A.prtTexName, B.prtTexName);

// ----------------------------------------------------------------------------
// THE PER-GROUP CONFIG KEYS. One table, walked four times with a prefix, rather than
// 152 hand-written entries. Offsets into OroThrusterFx so a field can never be paired
// with the wrong key by a copy-paste slip.
//
// ⚠️ MIGRATION, AND IT IS THE WHOLE REASON THIS IS NOT JUST A RENAME. Every class cfg
// written before 2026-08-16 carries UNPREFIXED keys (PlumeWidth, PrtRate, ...) from
// when one set of numbers served the whole ship. Those are read FIRST, by the ordinary
// flat table, into the edit buffer - and then copied into ALL FOUR GROUPS here before
// any prefixed key is looked at. So an old file lands every group exactly where the
// user's existing tuning was, nothing is lost, and nothing looks different until they
// cycle and change something. A prefixed key, when present, then overrides its group.
// ----------------------------------------------------------------------------
// fam (Phase B): which override FAMILY the key belongs to. EXH and PRT can be
// owned per thruster; BELL is deliberately group-only in v1 - a bell edit with a
// thruster selected edits the GROUP, and bell keys are never written per thruster.
struct ThrKey { const char* key; size_t off; int type; int fam; };
static const ThrKey THRSET[] = {
	{ "ShimmerOn",      offsetof(OroThrusterFx, shimmerEnabled), ST_B, ORO_FAM_EXH },
	{ "Shimmer",        offsetof(OroThrusterFx, shimmer),        ST_F, ORO_FAM_EXH },
	{ "ShimmerOfs",     offsetof(OroThrusterFx, shimmerOfs),     ST_F, ORO_FAM_EXH },
	{ "ShimmerWave",    offsetof(OroThrusterFx, shimmerWave),    ST_F, ORO_FAM_EXH },
	{ "ShimmerFreq",    offsetof(OroThrusterFx, shimmerFreq),    ST_F, ORO_FAM_EXH },
	{ "PlumeOn",        offsetof(OroThrusterFx, plumeEnabled),   ST_B, ORO_FAM_EXH },
	{ "Plume",          offsetof(OroThrusterFx, plume),          ST_F, ORO_FAM_EXH },
	{ "PlumePhysics",   offsetof(OroThrusterFx, plumePhysics),   ST_B, ORO_FAM_EXH },
	{ "PlumeExpHi",     offsetof(OroThrusterFx, plumeExpHi),     ST_F, ORO_FAM_EXH },
	{ "PlumeExpLo",     offsetof(OroThrusterFx, plumeExpLo),     ST_F, ORO_FAM_EXH },
	{ "PlumeWidth",     offsetof(OroThrusterFx, plumeWidth),     ST_F, ORO_FAM_EXH },
	{ "PlumeLen",       offsetof(OroThrusterFx, plumeLen),       ST_F, ORO_FAM_EXH },
	{ "PlumeCells",     offsetof(OroThrusterFx, plumeCells),     ST_F, ORO_FAM_EXH },
	{ "PlumeDiamond",   offsetof(OroThrusterFx, plumeDiamond),   ST_F, ORO_FAM_EXH },
	{ "PlumeDiaShape",  offsetof(OroThrusterFx, plumeDiaShape),  ST_F, ORO_FAM_EXH },
	{ "PlumeDiaSize",   offsetof(OroThrusterFx, plumeDiaSize),   ST_F, ORO_FAM_EXH },
	{ "PlumeDiaLength", offsetof(OroThrusterFx, plumeDiaLen),    ST_F, ORO_FAM_EXH },
	{ "PlumeDiaOfs",    offsetof(OroThrusterFx, plumeDiaOfs),    ST_F, ORO_FAM_EXH },
	{ "PlumeSpacing",   offsetof(OroThrusterFx, plumeSpacing),   ST_F, ORO_FAM_EXH },
	{ "PlumeBloomWid",  offsetof(OroThrusterFx, plumeBloomWid),  ST_F, ORO_FAM_EXH },
	{ "PlumeBloomBri",  offsetof(OroThrusterFx, plumeBloomBri),  ST_F, ORO_FAM_EXH },
	{ "PlumeThroatOfs", offsetof(OroThrusterFx, plumeThroatOfs), ST_F, ORO_FAM_EXH },
	{ "PlumeThroat",    offsetof(OroThrusterFx, plumeThroat),    ST_F, ORO_FAM_EXH },
	{ "PlumeSootRate",  offsetof(OroThrusterFx, plumeSootRate),  ST_F, ORO_FAM_EXH },
	{ "PlumeSoot",      offsetof(OroThrusterFx, plumeSoot),      ST_F, ORO_FAM_EXH },
	{ "PlumeColJet",    offsetof(OroThrusterFx, plumeColJet),    ST_I, ORO_FAM_EXH },
	{ "PlumeColBloom",  offsetof(OroThrusterFx, plumeColBloom),  ST_I, ORO_FAM_EXH },
	{ "PlumeColDia",    offsetof(OroThrusterFx, plumeColDia),    ST_I, ORO_FAM_EXH },
	{ "PlumeBellOn",    offsetof(OroThrusterFx, plumeBellOn),    ST_B, ORO_FAM_BELL },
	{ "PlumeBellGlow",  offsetof(OroThrusterFx, plumeBellGlow),  ST_F, ORO_FAM_BELL },
	{ "PlumeBellHeatT", offsetof(OroThrusterFx, plumeBellHeatT), ST_F, ORO_FAM_BELL },
	{ "PlumeBellCoolT", offsetof(OroThrusterFx, plumeBellCoolT), ST_F, ORO_FAM_BELL },
	{ "BellTint",       offsetof(OroThrusterFx, bellTint),       ST_I, ORO_FAM_BELL },
	{ "PrtOn",          offsetof(OroThrusterFx, prtEnabled),     ST_B, ORO_FAM_PRT },
	{ "PrtOffset",      offsetof(OroThrusterFx, prtOffset),      ST_F, ORO_FAM_PRT },
	{ "PrtSize",        offsetof(OroThrusterFx, prtSize),        ST_F, ORO_FAM_PRT },
	{ "PrtLifetime",    offsetof(OroThrusterFx, prtLifetime),    ST_F, ORO_FAM_PRT },
	{ "PrtRate",        offsetof(OroThrusterFx, prtRate),        ST_F, ORO_FAM_PRT },
	{ "PrtSpeed",       offsetof(OroThrusterFx, prtSpeed),       ST_F, ORO_FAM_PRT },
	{ "PrtSpread",      offsetof(OroThrusterFx, prtSpread),      ST_F, ORO_FAM_PRT },
	{ "PrtGrowth",      offsetof(OroThrusterFx, prtGrowth),      ST_F, ORO_FAM_PRT },
	{ "PrtSlowdown",    offsetof(OroThrusterFx, prtSlowdown),    ST_F, ORO_FAM_PRT },
	{ "PrtDiffuse",     offsetof(OroThrusterFx, prtDiffuse),     ST_B, ORO_FAM_PRT },
	{ "PrtAirFade",     offsetof(OroThrusterFx, prtAirFade),     ST_B, ORO_FAM_PRT },
	{ "PrtColour",      offsetof(OroThrusterFx, prtColour),      ST_I, ORO_FAM_PRT },
	{ "PrtColour2",     offsetof(OroThrusterFx, prtColour2),     ST_I, ORO_FAM_PRT },
	{ "PrtTexStock",    offsetof(OroThrusterFx, prtTexStock),    ST_B, ORO_FAM_PRT },
};
static const int NTHRSET = (int)(sizeof(THRSET) / sizeof(THRSET[0]));

// Copy the fields of the given FAMILIES between two blocks. prtTexName is not in
// THRSET (it is the one string key, written separately) but it IS particle-family
// state, so it rides the PRT mask here.
static void ThrCopyFam(OroThrusterFx& dst, const OroThrusterFx& src, int famMask)
{
	char* d = (char*)&dst;
	const char* s = (const char*)&src;
	for (int i = 0; i < NTHRSET; i++) {
		if (!(THRSET[i].fam & famMask)) continue;
		switch (THRSET[i].type) {
		case ST_F: *(float*)(d + THRSET[i].off) = *(const float*)(s + THRSET[i].off); break;
		case ST_B: *(bool*) (d + THRSET[i].off) = *(const bool*) (s + THRSET[i].off); break;
		case ST_I: *(DWORD*)(d + THRSET[i].off) = *(const DWORD*)(s + THRSET[i].off); break;
		}
	}
	if (famMask & ORO_FAM_PRT) strcpy_s(dst.prtTexName, src.prtTexName);
}

static void ThrWriteGroups(FILEHANDLE f)
{
	// Bank the sliders FIRST: a save clicked right after an edit - or while PAUSED,
	// when clbkPreStep's periodic SyncOut is not running - must write what the panel
	// shows, not last frame's storage.
	OroThr_SyncOut();
	char key[96];
	for (int gi = 0; gi < ORO_THR_N; gi++) {
		char* base = (char*)&g_fx.thr[gi];
		oapiWriteLine(f, K(""));
		sprintf_s(key, "; --- %s engines ---", OroThr_Name(gi));
		oapiWriteLine(f, key);
		for (int i = 0; i < NTHRSET; i++) {
			sprintf_s(key, "%s%s", OroThr_Name(gi), THRSET[i].key);
			void* p = base + THRSET[i].off;
			switch (THRSET[i].type) {
			case ST_F: oapiWriteItem_float(f, K(key), (double)*(float*)p); break;
			case ST_B: oapiWriteItem_bool (f, K(key), *(bool*)p);          break;
			case ST_I: oapiWriteItem_int  (f, K(key), (int)*(DWORD*)p);    break;
			}
		}
		// The ONE string key, special-cased beside the typed table rather than
		// threading an ST_S through the whole machinery. Written only when set -
		// Orbiter's writer TRUNCATES, so clearing back to the synthesized atlas
		// erases the old key on the next save for free (no stale-block trap).
		OroThrusterFx& T = g_fx.thr[gi];
		if (T.prtTexName[0]) {
			sprintf_s(key, "%sPrtTex", OroThr_Name(gi));
			oapiWriteItem_string(f, K(key), T.prtTexName);
		}
	}
	// ---- PER-THRUSTER OVERRIDE BLOCKS (Phase B) ----------------------------
	// Sparse: only live blocks write, only the FAMILIES they own, each behind an
	// explicit flag key (THR<idx>_OvrExh / _OvrPrt). The flag is load-bearing:
	// block-exists is the flag, never "some key happened to be present", so a
	// hand-edited partial block still loads sanely with group values as fallback
	// (the 17c class of read rule). The truncating writer erases CLEARED blocks
	// on the next save for free.
	for (int s = 0; s < ORO_THR_OVR_MAX; s++) {
		const OroThrOvr& o = g_fx.thrOvr[s];
		if (o.thrIdx < 0 || (!o.ovrExh && !o.ovrPrt)) continue;
		char* base = (char*)&o.fx;
		oapiWriteLine(f, K(""));
		sprintf_s(key, "; --- thruster %d override ---", o.thrIdx);
		oapiWriteLine(f, key);
		if (o.ovrExh) { sprintf_s(key, "THR%d_OvrExh", o.thrIdx); oapiWriteItem_bool(f, K(key), true); }
		if (o.ovrPrt) { sprintf_s(key, "THR%d_OvrPrt", o.thrIdx); oapiWriteItem_bool(f, K(key), true); }
		const int famMask = (o.ovrExh ? ORO_FAM_EXH : 0) | (o.ovrPrt ? ORO_FAM_PRT : 0);
		for (int i = 0; i < NTHRSET; i++) {
			if (!(THRSET[i].fam & famMask)) continue;
			sprintf_s(key, "THR%d_%s", o.thrIdx, THRSET[i].key);
			void* p = base + THRSET[i].off;
			switch (THRSET[i].type) {
			case ST_F: oapiWriteItem_float(f, K(key), (double)*(float*)p); break;
			case ST_B: oapiWriteItem_bool (f, K(key), *(bool*)p);          break;
			case ST_I: oapiWriteItem_int  (f, K(key), (int)*(DWORD*)p);    break;
			}
		}
		if (o.ovrPrt && o.fx.prtTexName[0]) {
			sprintf_s(key, "THR%d_PrtTex", o.thrIdx);
			oapiWriteItem_string(f, K(key), (char*)o.fx.prtTexName);
		}
	}
}

static void ThrReadGroups(FILEHANDLE f)
{
	// STEP 1 - the migration: whatever the flat table just loaded (an old file's
	// unprefixed keys, or the built-in defaults for a new one) becomes every group's
	// starting point. See the ⚠️ above.
	for (int gi = 0; gi < ORO_THR_N; gi++) {
		const int save = g_fx.thrSel;
		g_fx.thrSel = gi;
		OroThr_SyncOut();
		g_fx.thrSel = save;
	}
	// STEP 2 - then a prefixed key, where one exists, overrides its own group.
	char key[96];
	for (int gi = 0; gi < ORO_THR_N; gi++) {
		char* base = (char*)&g_fx.thr[gi];
		for (int i = 0; i < NTHRSET; i++) {
			sprintf_s(key, "%s%s", OroThr_Name(gi), THRSET[i].key);
			void* p = base + THRSET[i].off;
			switch (THRSET[i].type) {
			case ST_F: { double d; if (oapiReadItem_float(f, K(key), d)) *(float*)p = (float)d; break; }
			case ST_B: { bool   b; if (oapiReadItem_bool (f, K(key), b)) *(bool*)p  = b;        break; }
			case ST_I: { int    v; if (oapiReadItem_int  (f, K(key), v)) *(DWORD*)p = (DWORD)v; break; }
			}
		}
		// The string key (see ThrWriteGroups): present = override the group; absent =
		// keep current, the same rule as every other per-class look setting.
		char buf[256];
		sprintf_s(key, "%sPrtTex", OroThr_Name(gi));
		if (oapiReadItem_string(f, K(key), buf)) {
			buf[sizeof(g_fx.thr[gi].prtTexName) - 1] = 0;
			strcpy_s(g_fx.thr[gi].prtTexName, buf);
		}
	}
	// STEP 3 - PER-THRUSTER OVERRIDE BLOCKS (Phase B). The pool was wiped by the
	// class load before this file was opened, so everything here is a fresh read.
	// GROUPS FIRST is load-bearing: each block's base is a snapshot of its own
	// thruster's GROUP as just loaded, and any key the file is missing keeps that
	// fallback (the 17c class of read rule). Probing is bounded by the FOCUS
	// vessel's real thruster count - the class these settings belong to - so a
	// stale key from a reworked addon version is simply never asked about.
	{
		VESSEL* v = oapiGetFocusInterface();
		const int nthr = v ? (int)v->GetThrusterCount() : 0;
		const int nprobe = nthr < 96 ? nthr : 96;
		int nloaded = 0;
		char buf[256];
		for (int ti = 0; ti < nprobe; ti++) {
			bool fe = false, fp = false; bool b;
			sprintf_s(key, "THR%d_OvrExh", ti);
			if (oapiReadItem_bool(f, K(key), b)) fe = b;
			sprintf_s(key, "THR%d_OvrPrt", ti);
			if (oapiReadItem_bool(f, K(key), b)) fp = b;
			if (!fe && !fp) continue;
			THRUSTER_HANDLE th = v->GetThrusterHandleByIndex((DWORD)ti);
			if (!th) continue;
			const int grp = OroThrusterGroupOf(v, th);
			if (grp < 0) continue;
			OroThrOvr* o = NULL;
			for (int s = 0; s < ORO_THR_OVR_MAX && !o; s++)
				if (g_fx.thrOvr[s].thrIdx < 0) o = &g_fx.thrOvr[s];
			if (!o) break;                              // pool full: the rest stay group
			o->thrIdx = ti;
			o->ovrExh = fe;
			o->ovrPrt = fp;
			o->fx = g_fx.thr[grp];                      // the fallback base
			const int famMask = (fe ? ORO_FAM_EXH : 0) | (fp ? ORO_FAM_PRT : 0);
			char* base = (char*)&o->fx;
			for (int i = 0; i < NTHRSET; i++) {
				if (!(THRSET[i].fam & famMask)) continue;
				sprintf_s(key, "THR%d_%s", ti, THRSET[i].key);
				void* p = base + THRSET[i].off;
				switch (THRSET[i].type) {
				case ST_F: { double d; if (oapiReadItem_float(f, K(key), d)) *(float*)p = (float)d; break; }
				case ST_B: { bool  bb; if (oapiReadItem_bool (f, K(key), bb)) *(bool*)p  = bb;      break; }
				case ST_I: { int   iv; if (oapiReadItem_int  (f, K(key), iv)) *(DWORD*)p = (DWORD)iv; break; }
				}
			}
			if (fp) {
				sprintf_s(key, "THR%d_PrtTex", ti);
				if (oapiReadItem_string(f, K(key), buf)) {
					buf[sizeof(o->fx.prtTexName) - 1] = 0;
					strcpy_s(o->fx.prtTexName, buf);
				}
			}
			nloaded++;
		}
		if (nloaded)
			oapiWriteLogV("ORO: loaded %d per-thruster override block%s.",
			              nloaded, nloaded == 1 ? "" : "s");
	}
	OroThr_SyncIn();      // and show the selection on the sliders
}

static int ThrClamp(int g) { return (g < 0 || g >= ORO_THR_N) ? ORO_THR_MAIN : g; }

static OBJHANDLE PrtSpecVessel();          // the PANEL vessel (defined below, COPY STOCK's)

// The selection's owning override block, if any (Phase B).
static OroThrOvr* ThrSelOvr()
{
	return (g_fx.thrThrSel >= 0) ? OroThr_FindOvr(g_fx.thrThrSel) : NULL;
}

void OroThr_SyncOut()
{
	// Gather the flat buffer once, then route each FAMILY to its owner (the law in
	// OroState.h: an owned family to its override block, bell ALWAYS to the group,
	// everything un-owned to the group). With no thruster selected this degenerates
	// to the original whole-struct copy into thr[thrSel], bit for bit.
	OroThrusterFx cur;
	ORO_THR_FIELDS(cur, g_fx)
	OroThrusterFx& grp = g_fx.thr[ThrClamp(g_fx.thrSel)];
	OroThrOvr* o = ThrSelOvr();
	int grpMask = ORO_FAM_BELL | ORO_FAM_EXH | ORO_FAM_PRT;
	if (o && o->ovrExh) { ThrCopyFam(o->fx, cur, ORO_FAM_EXH); grpMask &= ~ORO_FAM_EXH; }
	if (o && o->ovrPrt) { ThrCopyFam(o->fx, cur, ORO_FAM_PRT); grpMask &= ~ORO_FAM_PRT; }
	ThrCopyFam(grp, cur, grpMask);
}

void OroThr_SyncIn()
{
	// Compose the selection's EFFECTIVE values: group base, owned families overlaid.
	OroThrusterFx eff = g_fx.thr[ThrClamp(g_fx.thrSel)];
	OroThrOvr* o = ThrSelOvr();
	if (o && o->ovrExh) ThrCopyFam(eff, o->fx, ORO_FAM_EXH);
	if (o && o->ovrPrt) ThrCopyFam(eff, o->fx, ORO_FAM_PRT);
	ORO_THR_FIELDS(g_fx, eff)
}

// ---- the override pool (Phase B, 2026-08-30) --------------------------------
// Fixed array, never compacted (slot-address stability); memory-only lookups so
// the render path may resolve through them. The laws live above OroThrOvr.

OroThrOvr* OroThr_FindOvr(int thrIdx)
{
	if (thrIdx < 0) return NULL;
	for (int i = 0; i < ORO_THR_OVR_MAX; i++)
		if (g_fx.thrOvr[i].thrIdx == thrIdx) return &g_fx.thrOvr[i];
	return NULL;
}

const OroThrusterFx& OroThr_Eff(int grp, int thrIdx, int fam)
{
	if (thrIdx >= 0) {
		for (int i = 0; i < ORO_THR_OVR_MAX; i++) {
			const OroThrOvr& o = g_fx.thrOvr[i];
			if (o.thrIdx != thrIdx) continue;
			// re-checking the flag here is what lets a CLEAR landing mid-frame
			// self-heal to group values instead of dangling
			if ((fam == ORO_FAM_EXH && o.ovrExh) || (fam == ORO_FAM_PRT && o.ovrPrt))
				return o.fx;
			break;
		}
	}
	return g_fx.thr[ThrClamp(grp)];
}

OroThrOvr* OroThr_EnsureOvr(int fam)
{
	const int ti = g_fx.thrThrSel;
	if (ti < 0 || !(fam & (ORO_FAM_EXH | ORO_FAM_PRT))) return NULL;
	OroThrOvr* o = OroThr_FindOvr(ti);
	if (!o) {
		for (int i = 0; i < ORO_THR_OVR_MAX && !o; i++)
			if (g_fx.thrOvr[i].thrIdx < 0) o = &g_fx.thrOvr[i];
		if (!o) return NULL;                 // pool full (24 blocks): the edit still lands
		                                     //   on the group - sparse is the design
		o->thrIdx = ti;
		o->ovrExh = o->ovrPrt = false;
		o->fx = g_fx.thr[ThrClamp(g_fx.thrSel)];
	}
	// Adopting a family snapshots it from the GROUP at that moment; the edit that
	// caused this lands on the block at the next SyncOut, so the jet keeps looking
	// as it did except the one thing that moved.
	if ((fam & ORO_FAM_EXH) && !o->ovrExh) { ThrCopyFam(o->fx, g_fx.thr[ThrClamp(g_fx.thrSel)], ORO_FAM_EXH); o->ovrExh = true; }
	if ((fam & ORO_FAM_PRT) && !o->ovrPrt) { ThrCopyFam(o->fx, g_fx.thr[ThrClamp(g_fx.thrSel)], ORO_FAM_PRT); o->ovrPrt = true; }
	return o;
}

void OroThr_ClearOvr(int fam)
{
	OroThrOvr* o = ThrSelOvr();
	if (!o) return;
	if (fam & ORO_FAM_EXH) o->ovrExh = false;
	if (fam & ORO_FAM_PRT) o->ovrPrt = false;
	if (!o->ovrExh && !o->ovrPrt) o->thrIdx = -1;    // free the slot (never compacts)
	OroThr_SyncIn();                                 // the sliders snap back to inheriting
}

void OroThr_ResetOvr()
{
	for (int i = 0; i < ORO_THR_OVR_MAX; i++) {
		g_fx.thrOvr[i].thrIdx = -1;
		g_fx.thrOvr[i].ovrExh = g_fx.thrOvr[i].ovrPrt = false;
	}
	g_fx.thrThrSel = -1;
	g_fx.thrOrd = 0;
	g_fx.thrSelInfo[0] = 0;
}

bool OroThr_ClassMatch(VESSEL* v)
{
	if (!v || !g_fx.loadedClass[0]) return false;
	const char* c = v->GetClassNameA();
	return c && _stricmp(c, g_fx.loadedClass) == 0;
}

// The header-row thruster cycler: ALL <-> 1/m <-> ... within the SELECTED group on
// the panel vessel, wrapping both ways. Main thread (a click handler) - oapi is fine
// here, exactly as the COPY STOCK machinery below.
void OroThr_CycleThr(int dir)
{
	OroThr_SyncOut();                        // bank what is on the sliders NOW
	VESSEL* v = NULL;
	{
		OBJHANDLE h = PrtSpecVessel();
		if (h) v = oapiGetVesselInterface(h);
	}
	static const int SEQ_MAX = 256;
	int seq[SEQ_MAX]; int nseq = 0;
	const int grp = ThrClamp(g_fx.thrSel);
	if (v) {
		const DWORD n = v->GetThrusterCount();
		for (DWORD i = 0; i < n && nseq < SEQ_MAX; i++) {
			THRUSTER_HANDLE th = v->GetThrusterHandleByIndex(i);
			if (th && OroThrusterGroupOf(v, th) == grp) seq[nseq++] = (int)i;
		}
	}
	if (!nseq) { g_fx.thrThrSel = -1; g_fx.thrOrd = 0; g_fx.thrCnt = 0; OroThr_SyncIn(); return; }
	int pos = -1;                            // position in the ring: -1 = ALL
	for (int i = 0; i < nseq; i++) if (seq[i] == g_fx.thrThrSel) { pos = i; break; }
	const int ring = nseq + 1;               // 0 = ALL, 1..nseq = the thrusters
	const int cur  = (pos < 0) ? 0 : pos + 1;
	const int nxt  = ((cur + dir) % ring + ring) % ring;
	g_fx.thrThrSel = (nxt == 0) ? -1 : seq[nxt - 1];
	g_fx.thrCnt = nseq;
	g_fx.thrOrd = nxt;                       // 0 = ALL, else 1-based ordinal
	OroThr_SyncIn();                         // and show the selection's effective values
}

// ---- THE PER-VESSEL CLASS CACHE (2026-08-30) --------------------------------
// The laws live above the declarations in OroState.h. Storage is file-static and
// opaque; slots NEVER move (the invariant-14 address family), so EffC pointers
// taken within a frame stay valid - eviction does not exist, only drop-by-name,
// which happens on the main thread before any consumer runs.

namespace {
	struct ThrClassCache {
		char cls[64];                 // class name; "" = free slot
		bool hasFile;                 // false = KNOWN no cfg (negative cache: the
		                              //   file is probed once per class per session)
		OroThrusterFx thr[ORO_THR_N];
		OroThrOvr     ovr[ORO_THR_OVR_MAX];
	};
	ThrClassCache g_thrCache[ORO_THR_CACHE_MAX];
	DWORD g_thrCacheGen = 1;
	bool  g_thrCacheFullWarned = false;
}

DWORD OroThr_CacheGen() { return g_thrCacheGen; }

const OroThrusterFx* OroThr_CacheGrp(int cacheIdx, int grp)
{
	if (cacheIdx < 0 || cacheIdx >= ORO_THR_CACHE_MAX) return NULL;
	if (!g_thrCache[cacheIdx].cls[0]) return NULL;
	return &g_thrCache[cacheIdx].thr[ThrClamp(grp)];
}

// The cached-override lookups the particle texture path needs: WHICH override slot
// owns a thruster's particle family, and that slot's block. His first real use of
// the cache hit exactly this - the SRB motor's Contrail4 lives in a THR0 override,
// and baking it with the group's texture rendered the wrong contrail (2026-08-30).
int OroThr_CacheOvrSlot(int cacheIdx, int thrIdx)
{
	if (cacheIdx < 0 || cacheIdx >= ORO_THR_CACHE_MAX || thrIdx < 0) return -1;
	if (!g_thrCache[cacheIdx].cls[0]) return -1;
	for (int i = 0; i < ORO_THR_OVR_MAX; i++) {
		const OroThrOvr& o = g_thrCache[cacheIdx].ovr[i];
		if (o.thrIdx == thrIdx) return o.ovrPrt ? i : -1;
	}
	return -1;
}

const OroThrusterFx* OroThr_CacheOvrFx(int cacheIdx, int slot)
{
	if (cacheIdx < 0 || cacheIdx >= ORO_THR_CACHE_MAX) return NULL;
	if (slot < 0 || slot >= ORO_THR_OVR_MAX) return NULL;
	if (!g_thrCache[cacheIdx].cls[0] || g_thrCache[cacheIdx].ovr[slot].thrIdx < 0) return NULL;
	return &g_thrCache[cacheIdx].ovr[slot].fx;
}

const OroThrusterFx& OroThr_EffC(int cacheIdx, int grp, int thrIdx, int fam)
{
	if (cacheIdx >= 0 && cacheIdx < ORO_THR_CACHE_MAX && g_thrCache[cacheIdx].cls[0]) {
		const ThrClassCache& c = g_thrCache[cacheIdx];
		if (thrIdx >= 0) {
			for (int i = 0; i < ORO_THR_OVR_MAX; i++) {
				const OroThrOvr& o = c.ovr[i];
				if (o.thrIdx != thrIdx) continue;
				if ((fam == ORO_FAM_EXH && o.ovrExh) || (fam == ORO_FAM_PRT && o.ovrPrt))
					return o.fx;
				break;
			}
		}
		return c.thr[ThrClamp(grp)];
	}
	return OroThr_Eff(grp, thrIdx, fam);
}

void OroThr_CacheDrop(const char* cls)
{
	if (!cls || !cls[0]) return;
	for (int i = 0; i < ORO_THR_CACHE_MAX; i++)
		if (g_thrCache[i].cls[0] && _stricmp(g_thrCache[i].cls, cls) == 0) {
			g_thrCache[i].cls[0] = 0;
			g_thrCacheGen++;
		}
}

void OroThr_CacheReset()
{
	for (int i = 0; i < ORO_THR_CACHE_MAX; i++) g_thrCache[i].cls[0] = 0;
	g_thrCacheFullWarned = false;
	g_thrCacheGen++;
}

// Fill one slot from Config\ORO\<class>.cfg - the CACHE mirror of ThrReadGroups,
// kept as its own function so the flown live path stays untouched. ⚠️ KEEP THE TWO
// IN STEP: same migration (unprefixed keys seed every group), same prefixed
// per-group read, same THR<idx> override blocks with the 17c flag-key rule. The
// one deliberate difference: the migration BASE is the struct DEFAULTS, not the
// live sliders - a foreign class must never inherit the focus class's look
// through a loader side door.
static bool ThrCacheLoad(ThrClassCache& c, VESSEL* v, const char* cls)
{
	strcpy_s(c.cls, cls);
	c.hasFile = false;
	for (int i = 0; i < ORO_THR_OVR_MAX; i++) {
		c.ovr[i].thrIdx = -1;
		c.ovr[i].ovrExh = c.ovr[i].ovrPrt = false;
	}
	char fn[64], rel[128], key[96], buf[256];
	ClassFileName(cls, fn, sizeof(fn));
	sprintf_s(rel, "ORO\\%s.cfg", fn);
	FILEHANDLE f = oapiOpenFile(rel, FILE_IN, CONFIG);
	OroThrusterFx base;                       // struct defaults
	if (!f) {
		for (int gi = 0; gi < ORO_THR_N; gi++) c.thr[gi] = base;
		return false;                         // negative-cached: probed once
	}
	// STEP 1 - the migration: unprefixed keys over the defaults, seeding every group.
	{
		char* p0 = (char*)&base;
		for (int i = 0; i < NTHRSET; i++) {
			void* p = p0 + THRSET[i].off;
			switch (THRSET[i].type) {
			case ST_F: { double d; if (oapiReadItem_float(f, K(THRSET[i].key), d)) *(float*)p = (float)d; break; }
			case ST_B: { bool   b; if (oapiReadItem_bool (f, K(THRSET[i].key), b)) *(bool*)p  = b;        break; }
			case ST_I: { int    n; if (oapiReadItem_int  (f, K(THRSET[i].key), n)) *(DWORD*)p = (DWORD)n; break; }
			}
		}
	}
	for (int gi = 0; gi < ORO_THR_N; gi++) c.thr[gi] = base;
	// STEP 2 - prefixed keys override their own group.
	for (int gi = 0; gi < ORO_THR_N; gi++) {
		char* p0 = (char*)&c.thr[gi];
		for (int i = 0; i < NTHRSET; i++) {
			sprintf_s(key, "%s%s", OroThr_Name(gi), THRSET[i].key);
			void* p = p0 + THRSET[i].off;
			switch (THRSET[i].type) {
			case ST_F: { double d; if (oapiReadItem_float(f, K(key), d)) *(float*)p = (float)d; break; }
			case ST_B: { bool   b; if (oapiReadItem_bool (f, K(key), b)) *(bool*)p  = b;        break; }
			case ST_I: { int    n; if (oapiReadItem_int  (f, K(key), n)) *(DWORD*)p = (DWORD)n; break; }
			}
		}
		sprintf_s(key, "%sPrtTex", OroThr_Name(gi));
		if (oapiReadItem_string(f, K(key), buf)) {
			buf[sizeof(c.thr[gi].prtTexName) - 1] = 0;
			strcpy_s(c.thr[gi].prtTexName, buf);
		}
	}
	// STEP 3 - the THR<idx> override blocks, probed against THIS vessel's thrusters.
	{
		const int nthr = (int)v->GetThrusterCount();
		const int nprobe = nthr < 96 ? nthr : 96;
		int novr = 0, nloaded = 0;
		for (int ti = 0; ti < nprobe && novr < ORO_THR_OVR_MAX; ti++) {
			bool fe = false, fp = false; bool b;
			sprintf_s(key, "THR%d_OvrExh", ti);
			if (oapiReadItem_bool(f, K(key), b)) fe = b;
			sprintf_s(key, "THR%d_OvrPrt", ti);
			if (oapiReadItem_bool(f, K(key), b)) fp = b;
			if (!fe && !fp) continue;
			THRUSTER_HANDLE th = v->GetThrusterHandleByIndex((DWORD)ti);
			if (!th) continue;
			const int grp = OroThrusterGroupOf(v, th);
			if (grp < 0) continue;
			OroThrOvr& o = c.ovr[novr++];
			o.thrIdx = ti;
			o.ovrExh = fe;
			o.ovrPrt = fp;
			o.fx = c.thr[grp];                // group fallback base (the 17c read rule)
			const int famMask = (fe ? ORO_FAM_EXH : 0) | (fp ? ORO_FAM_PRT : 0);
			char* p0 = (char*)&o.fx;
			for (int i = 0; i < NTHRSET; i++) {
				if (!(THRSET[i].fam & famMask)) continue;
				sprintf_s(key, "THR%d_%s", ti, THRSET[i].key);
				void* p = p0 + THRSET[i].off;
				switch (THRSET[i].type) {
				case ST_F: { double d; if (oapiReadItem_float(f, K(key), d)) *(float*)p = (float)d; break; }
				case ST_B: { bool  bb; if (oapiReadItem_bool (f, K(key), bb)) *(bool*)p  = bb;      break; }
				case ST_I: { int   iv; if (oapiReadItem_int  (f, K(key), iv)) *(DWORD*)p = (DWORD)iv; break; }
				}
			}
			if (fp) {
				sprintf_s(key, "THR%d_PrtTex", ti);
				if (oapiReadItem_string(f, K(key), buf)) {
					buf[sizeof(o.fx.prtTexName) - 1] = 0;
					strcpy_s(o.fx.prtTexName, buf);
				}
			}
			nloaded++;
		}
		oapiCloseFile(f, FILE_IN);
		oapiWriteLogV("ORO: cached thruster settings for class %s (%d override block%s).",
		              cls, nloaded, nloaded == 1 ? "" : "s");
	}
	c.hasFile = true;
	return true;
}

int OroThr_CacheFor(VESSEL* v)
{
	if (!v) return -1;
	const char* cls = v->GetClassNameA();
	if (!cls || !cls[0]) return -1;
	if (_stricmp(cls, g_setClass) == 0) return -1;     // the focus class IS the live tables
	int freeSlot = -1;
	for (int i = 0; i < ORO_THR_CACHE_MAX; i++) {
		if (!g_thrCache[i].cls[0]) { if (freeSlot < 0) freeSlot = i; continue; }
		if (_stricmp(g_thrCache[i].cls, cls) == 0)
			return g_thrCache[i].hasFile ? i : -1;     // negative-cached: no file = live
	}
	if (freeSlot < 0) {
		if (!g_thrCacheFullWarned) {
			g_thrCacheFullWarned = true;
			oapiWriteLogV("ORO: thruster class cache full (%d classes) - %s takes the "
			              "loaded class's settings.", ORO_THR_CACHE_MAX, cls);
		}
		return -1;                                     // degrade to today's behaviour
	}
	g_thrCacheGen++;                                   // the particle sig watches this
	return ThrCacheLoad(g_thrCache[freeSlot], v, cls) ? freeSlot : -1;
}

const char* OroThr_Name(int grp)
{
	static const char* n[ORO_THR_N] = { "MAIN", "HOVER", "RETRO", "USER", "RCS" };
	return n[ThrClamp(grp)];
}

int OroThr_Count()
{
	int n = 0;
	for (int i = 0; i < ORO_THR_N; i++) if (g_fx.thrAvail & (1 << i)) n++;
	return n ? n : 1;
}

void OroThr_Cycle()
{
	OroThr_SyncOut();                       // bank what is on the sliders NOW (the old
	                                        //   selection's targets - order matters)
	g_fx.thrThrSel = -1;                    // a new group starts at ALL (Phase B): the
	g_fx.thrOrd = 0;                        //   thruster selection is group-scoped
	for (int step = 1; step <= ORO_THR_N; step++) {
		const int cand = (g_fx.thrSel + step) % ORO_THR_N;
		if (g_fx.thrAvail & (1 << cand)) { g_fx.thrSel = cand; break; }
	}
	OroThr_SyncIn();                        // and bring the new group's up
}

// ⚠️ THE STOCK-PARTICLES PILL IS A VESSEL-WIDE SWITCH, SO ITS COUNTERPART MUST BE TOO
// (2026-08-25 - the bug where the pill lit for one frame and stock never appeared).
// gcCore::SuppressExhaust takes an OBJHANDLE and a per-vessel flag map (invariant 26d), so
// "stock particles" is ONE answer for the whole ship. The dialog was turning ORO's streams
// off by writing the FLAT edit buffer, which SyncOut copies into thr[thrSel] and nowhere
// else - so on a DG-S with HOVER and USER still enabled, UpdateParticles' mutual-exclusion
// loop found a group still streaming and switched stock straight back off on the next
// pre-step. The pill was doing exactly what it was told; it was told about one group.
// ⚠️ THE FLAT FIELD MUST BE SET TOO, not just the array: SyncOut runs at the top of the
// next clbkPreStep and would otherwise copy the stale edit buffer back over thr[thrSel].
void OroThr_SetPrtAll(bool on)
{
	for (int i = 0; i < ORO_THR_N; i++) g_fx.thr[i].prtEnabled = on;
	// Phase B: a vessel-wide switch means the OVERRIDE blocks too, or an overridden
	// jet would go on streaming after "off - no exhaust particles at all".
	for (int i = 0; i < ORO_THR_OVR_MAX; i++)
		if (g_fx.thrOvr[i].thrIdx >= 0) g_fx.thrOvr[i].fx.prtEnabled = on;
	g_fx.prtEnabled = on;
}

// Is ANY group streaming? The panel needs this wherever it makes a claim about the VESSEL
// rather than about the group being edited - "no exhaust particles at all" was being said
// while two other groups were happily emitting.
// ⚠️ THE SELECTED GROUP IS READ FROM THE EDIT BUFFER, NOT FROM thr[]. The two agree only
// after the next SyncOut, and the panel repaints long before that, so reading the array
// alone would leave the caption one frame behind the pill the user just clicked.
bool OroThr_AnyPrtOn()
{
	const int sel = ThrClamp(g_fx.thrSel);
	// With a THRUSTER selected the edit buffer describes that thruster, not the
	// group - so the group array answers for the group and the buffer answers only
	// for the block it is actually editing (Phase B).
	const bool bufIsGroup = (g_fx.thrThrSel < 0);
	for (int i = 0; i < ORO_THR_N; i++)
		if ((i == sel && bufIsGroup) ? g_fx.prtEnabled : g_fx.thr[i].prtEnabled) return true;
	for (int i = 0; i < ORO_THR_OVR_MAX; i++) {
		const OroThrOvr& o = g_fx.thrOvr[i];
		if (o.thrIdx < 0 || !o.ovrPrt) continue;
		if (o.thrIdx == g_fx.thrThrSel ? g_fx.prtEnabled : o.fx.prtEnabled) return true;
	}
	return false;
}

// REVERT - re-read the given scopes from disk, discarding everything moved since the last
// save (2026-08-15, a beta ask: there was no way back from a bad tuning session but memory
// or a restart, and the load path had existed all along without a control).
//
// ⚠️ IT CANNOT JUST CALL THE LOADERS. Both of them early-return on "already current" - that
// compare is what makes the per-frame focus/world check free - so from outside they are
// no-ops for the class and body already loaded, which is precisely the case a revert means.
// Clearing the cached names first is the whole trick, and it belongs HERE rather than in the
// dialog because these statics live in this file.
//
// The two loaders then disagree about a scope with NO file, and BOTH are right (invariant
// 17a): an unconfigured hull KEEPS the current numbers, an unconfigured world gets the
// built-in defaults back. Either way the user lands on what they would have had if they had
// never touched anything, which is what "revert" means.
bool OroSettings_PilotFromClass() { return g_pilotFromClass; }
bool OroSettings_VcFromClass()    { return g_vcFromClass; }

void OroSettings_Revert(int mask)
{
	if (mask & ORO_SCOPE_GLOBAL) OroSettings_Load();
	if (mask & ORO_SCOPE_CLASS) {
		char cls[64]; strcpy_s(cls, g_setClass);
		g_setClass[0] = 0;
		OroSettings_LoadClass(cls);
	}
	if (mask & ORO_SCOPE_BODY) {
		char body[64]; strcpy_s(body, g_setBody);
		g_setBody[0] = 0;
		OroSettings_LoadBody(body);
	}
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
	// Phase B: overrides belong to a CLASS FILE, so a class change wipes the pool and
	// the thruster selection BEFORE anything is read - including the no-file early
	// return below (a hull with no cfg keeps the current sliders but must never keep
	// another hull's thruster overrides; index 3 means a different jet here). This is
	// also what makes ThrReadGroups' migration SyncOut juggling mean pure groups.
	strcpy_s(g_fx.loadedClass, cls);                   // the class-faithful reference
	OroThr_ResetOvr();
	OroThr_CacheDrop(cls);                             // this class is LIVE now - a stale
	                                                   //   cache slot would shadow it the
	                                                   //   moment focus moves away again

	// ⚠️ THE PILOT BLOCK RESETS FIRST - before the file is read AND before the no-file
	// early return below. Both orderings matter. Clearing the flag first means a class file
	// that never mentions PilotScope reads as "global", which is what its absence means;
	// restoring before the early return means a hull with no file at all flies the GLOBAL
	// pilot rather than inheriting the previous hull's seating position, which is the whole
	// point of the third read rule (see PILOTSET). Note this is the one part of the class
	// load that does NOT follow "an unconfigured hull keeps the current numbers".
	// Undo the OUTGOING hull's blocks - but only those it actually owned, see fromClass.
	for (int b = 0; b < NMOVBLK; b++) {
		const MovBlock& mb = MOVBLK[b];
		if (*mb.fromClass && *mb.globalOk) RestoreTable(mb.tbl, mb.n, mb.globalVal);
		*mb.fromClass = false;
		*mb.perClass  = false;
	}

	char fn[64], rel[128];
	ClassFileName(cls, fn, sizeof(fn));
	sprintf_s(rel, "ORO\\%s.cfg", fn);
	FILEHANDLE f = oapiOpenFile(rel, FILE_IN, CONFIG);
	if (!f) {
		oapiWriteLogV("ORO: vessel class %s has no saved settings - keeping the current ones"
		              " (pilot: global).", cls);
		return;
	}
	// ⚠️ THE SIZE-Y SENTINEL (2026-08-29). Size y became ABSOLUTE (same units as Size x -
	// his rule: equal sliders = a circular cone), which breaks the usual missing-key
	// story: a cfg from the one-Size era would load X from its VapourSize key and leave
	// Y at whatever was current, giving a tuned hull a silently elliptical cone. So Y is
	// parked at an impossible value before the read; if the file carried no VapourSizeY
	// it is still negative afterwards and becomes X - a pre-split file loads the exact
	// circular cone it always meant. Deliberately placed AFTER the no-file early return:
	// a hull with no cfg keeps the current sliders, exactly as before.
	g_fx.vapSizeY = -1.0f;
	const int n = ReadTable(f, CLASSSET, NCLASSSET);
	if (g_fx.vapSizeY < 0.0f) g_fx.vapSizeY = g_fx.vapSize;
	ThrReadGroups(f);            // per group, with the unprefixed-key migration
	// ... and this hull's own pilot settings, if it declared any. PilotScope came in with
	// CLASSSET a line ago, so by here we already know whether to look.
	for (int b = 0; b < NMOVBLK; b++) {
		const MovBlock& mb = MOVBLK[b];
		if (!*mb.perClass) continue;
		const int nb = ReadTable(f, mb.tbl, mb.n);
		*mb.fromClass = true;            // so leaving this hull puts the global block back
		oapiWriteLogV("ORO: vessel class %s carries its OWN %s settings (%d of %d).",
		              cls, mb.name, nb, mb.n);
	}
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

// Third thunk, for the patch-(u) wet-mirror slot. Same reason as the second: a render
// proc receives no id, so one entry point per slot.
static void __cdecl OroWetMirrorProc(oapi::Sketchpad* pSkp, void* pParam)
{
	static_cast<OroModule*>(pParam)->DrawWetMirror(pSkp);
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

// ----------------------------------------------------------------------------
// RAINSURFACES (2026-09-01) - the click-to-declare rain-glass pick.
// GENERICPROC_PICK_VESSEL is a STOCK gcCore slot: while a proc is registered the
// client's own WM_LBUTTONDOWN handler runs Scene::PickScene - the same pick the
// D3D9 Debug dialog uses, VC meshes included - and calls back with a
// gcCore::PickData. Same thunk shape as OroShutdownProc above: the instance is
// the THIRD argument; here iUser = sizeof(PickData) and pUser = the data (our
// patch (h) part 3 also null-guards the client side, so a sky click makes no
// call at all instead of crashing stock's unchecked dereference).
// ----------------------------------------------------------------------------
static void __cdecl OroRsPickProc(int iUser, void* pUser, void* pParam)
{
	if (!pParam || !pUser || iUser != (int)sizeof(gcCore::PickData)) return;
	static_cast<OroModule*>(pParam)->RsPickDeliver(pUser);
}

// ⚠️ Registered PER ARM, not per session, unlike the procs above: a registered
// pick proc makes the client run a full scene pick on EVERY left click
// (IsGenericProcEnabled gates it), so the cost must exist only while the popup's
// ADD button is amber. RegisterGenericProc with a non-zero id APPENDS - a second
// register would duplicate the entry (read in the client source) - so the flag
// guards it; id 0 unregisters by proc pointer, which the client also supports.
static bool s_rsPickOn = false;

bool OroModule::RsPickAvail() const
{
	// Guard #12: the name getter is the half the cfg cannot live without -
	// PickData's mesh is a DEVICE handle, and only the client can name it.
	return pCore && pCore->CanGetDevMeshName();
}

bool OroModule::RsPickArm(bool on)
{
	if (!RsPickAvail()) return false;
	if (on == s_rsPickOn) return true;
	if (on) {
		if (!pCore->RegisterGenericProc(OroRsPickProc, GENERICPROC_PICK_VESSEL, this))
			return false;
	} else {
		pCore->RegisterGenericProc(OroRsPickProc, 0, this);   // id 0 = unregister by proc
	}
	s_rsPickOn = on;
	return true;
}

void OroModule::RsPickDeliver(const void* pickData)
{
	// Main thread (the client's message pump), so oapi queries are safe here -
	// this is the same phase the Debug dialog's own pick handling runs in.
	if (!s_rsPickOn || !pCore) return;
	const gcCore::PickData* pd = (const gcCore::PickData*)pickData;
	// Only works in VC view (his spec) - and only on the ship you are sitting in:
	// PickScene can hit another vessel through the glass. A non-qualifying click
	// keeps the pick armed, so a stray click on the scenery is not a cancel.
	if (!oapiCameraInternal() || oapiCockpitMode() != COCKPIT_VIRTUAL) return;
	if (pd->hVessel != oapiGetFocusObject()) return;
	char mesh[192] = "";
	pCore->GetDevMeshName(pd->mesh, mesh, sizeof(mesh));
	if (!mesh[0]) return;              // a nameless mesh cannot be declared in a cfg
	// The confirmation highlight (his spec, 2026-09-02): the picked group lights the
	// Debug dialog's own green and STAYS lit while the mouse button is held (msec 0 =
	// the held mode), so the user SEES what they just declared. On every ACCEPTED
	// pick - a duplicate answer lights too, since either way it is the honest answer
	// to "which group did I click".
	if (pCore->CanFlashMeshGroup()) pCore->FlashMeshGroup(pd->mesh, pd->group, 0);
	OroDlg_RsPickResult(mesh, pd->group);
}

bool OroModule::RsApplyAvail() const
{
	return pCore && pCore->CanReloadRainSurfaces();   // guard #13 - patch (h) part 4
}

bool OroModule::RsApplyNow()
{
	// The client re-reads the cfg and hands every LIVE mesh its fresh rain-glass list,
	// so the picker's SAVE (adds AND removals) takes effect on the next frame. On an
	// older client this answers false and the picker's message says "reload" instead.
	if (!RsApplyAvail()) return false;
	pCore->ReloadRainSurfaces();
	return true;
}

// File-scope mirror of the module's patch-(f) capability flag, so the dialog can grey the
// VC SHADOWS section out without reaching into the module instance (the OroSettings_*
// pattern). Set once in clbkSimulationStart.
static bool g_vcShadowSupported = false;
bool OroVCShadowsSupported() { return g_vcShadowSupported; }
static bool g_vcNightSupported = false;
bool OroVCNightSupported() { return g_vcNightSupported; }

// Same pattern for patch (g)'s depth clip: the REENTRY tab warns when it is dark,
// because on screen the degradation is silent (plasma paints through the hull).
static bool g_depthClipMirror = false;
bool OroDepthClipOK() { return g_depthClipMirror; }

// Same pattern for patch (n): the THRUSTER tab's STOCK EXHAUST pill greys out when
// the client cannot suppress (a switch that cannot do anything is worse than none).
static bool g_stockExSupported = false;
bool OroStockExhaustSupported() { return g_stockExSupported; }
// Patch (ac): the BASE LIGHTS pill greys out wholesale without the patch (18b's rule).
bool g_baseLightsSupported = false;
bool OroBaseLightsSupported() { return g_baseLightsSupported; }

// ---------------------------------------------------------------------------
// Patch (y) bridge: the PARTICLES page's COPY STOCK button. The core pointer
// doubles as the capability flag (probe-by-binding, set at session start).
// Read-only queries - nothing is lent, so 23(k)'s load-window rule does not
// apply. The vessel is the same one the particle system edits (camera target,
// focus fallback - the OroParticles.cpp convention).
// ---------------------------------------------------------------------------
static gcCore2* g_prtSpecCore = nullptr;

static OBJHANDLE PrtSpecVessel()
{
	OBJHANDLE h = oapiCameraTarget();
	if (!h || oapiGetObjectType(h) != OBJTP_VESSEL) h = oapiGetFocusObject();
	return h;
}

// The GROUP FILTER + DEDUPE (his ask, 2026-08-30: "is there no way to pre-detect
// which one was used for the stock main engines?"). Each stream's reported THRUST
// DIRECTION is a pointer into the vessel's own thruster storage, so matching it
// against the selected group's thruster directions classifies the stream - DG mains
// thrust +Z, hovers +Y, and the ~10 raw streams collapse further because stock code
// attaches ONE stream PER THRUSTER: byte-identical specs fold into one entry. On a
// DeltaGlider MAIN therefore offers exactly TWO candidates - the contrail and the
// flame puffs.
static int PrtStockForGroup(int gi, int wantK, PARTICLESTREAMSPEC* outSpec)
{
	if (!g_prtSpecCore) return -1;
	OBJHANDLE h = PrtSpecVessel();
	if (!h) return 0;
	VESSEL* v = oapiGetVesselInterface(h);
	if (!v) return 0;
	const DWORD nth = v->GetThrusterCount();

	// Phase B (round 2): with a THRUSTER selected the candidates narrow to exactly
	// the streams attached to THAT thruster. Patch (y)'s pos/dir are dereferenced
	// from pointers INTO the vessel's own thruster storage, so a thruster-attached
	// stream's position is bit-identical to GetThrusterRef - an epsilon equality on
	// position AND direction names the thruster exactly, sharper than the group
	// path's folding. The one honest fallback: a stream the author attached at an
	// EXPLICIT offset (the AddExhaustStream(th, pos, spec) overload) points at a
	// stored copy, sits at NO thruster's ref, and degrades to the direction-only
	// test - offered rather than lost, exactly today's group behaviour for it.
	const bool perThr = (g_fx.thrThrSel >= 0);
	VECTOR3 selRef = _V(0, 0, 0), selDir = _V(0, 0, 0);
	if (perThr) {
		THRUSTER_HANDLE th = ((DWORD)g_fx.thrThrSel < nth)
		                   ? v->GetThrusterHandleByIndex((DWORD)g_fx.thrThrSel) : NULL;
		if (!th) return 0;
		v->GetThrusterRef(th, selRef);
		v->GetThrusterDir(th, selDir);
		const double L = length(selDir);
		if (L < 1e-6) return 0;
		selDir = selDir * (1.0 / L);
	}
	// The selected group's thrust directions (the group path), and EVERY thruster's
	// ref position - the offset-stream fallback needs "sits at no thruster at all".
	VECTOR3 gdir[64]; int ng = 0;
	VECTOR3 tref[128]; int nr = 0;
	for (DWORD i = 0; i < nth; i++) {
		THRUSTER_HANDLE th = v->GetThrusterHandleByIndex(i);
		if (!th) continue;
		if (nr < 128) { v->GetThrusterRef(th, tref[nr]); nr++; }
		if (OroThrusterGroupOf(v, th) != gi) continue;
		VECTOR3 d; v->GetThrusterDir(th, d);
		const double L = length(d);
		if (L > 1e-6 && ng < 64) gdir[ng++] = d * (1.0 / L);
	}
	if (!perThr && !ng) return 0;
	const int total = g_prtSpecCore->GetExhaustStreamSpec(h, -1, NULL, NULL, NULL);
	PARTICLESTREAMSPEC uniq[16]; int nu = 0;
	for (int i = 0; i < total; i++) {
		PARTICLESTREAMSPEC ps; VECTOR3 sp, sd;
		if (i >= g_prtSpecCore->GetExhaustStreamSpec(h, i, &ps, &sp, &sd)) break;
		const double L = length(sd);
		if (L < 1e-6) continue;
		sd = sd * (1.0 / L);
		if (perThr) {
			if (dotp(sd, selDir) <= 0.999) continue;         // not this thruster's way
			if (length(sp - selRef) > 1e-3) {                // not AT the thruster...
				bool atAny = false;                          // ...an offset stream, or
				for (int t = 0; t < nr && !atAny; t++)       //    another thruster's?
					atAny = (length(sp - tref[t]) < 1e-3);
				if (atAny) continue;                         // someone else's - skip
			}
		} else {
			bool inGrp = false;
			for (int t = 0; t < ng && !inGrp; t++) inGrp = (dotp(sd, gdir[t]) > 0.98);
			if (!inGrp) continue;
		}
		// fold per-thruster duplicates: OroGetSpec zero-fills, so memcmp is honest
		bool dup = false;
		for (int u = 0; u < nu && !dup; u++) dup = !memcmp(&uniq[u], &ps, sizeof(ps));
		if (dup || nu >= 16) continue;
		if (wantK == nu && outSpec) *outSpec = ps;
		uniq[nu++] = ps;
	}
	return nu;
}

int OroPrt_StockSpecCount()
{
	return PrtStockForGroup(ThrClamp(g_fx.thrSel), -1, NULL);
}

bool OroPrt_StockSpecGet(int idx, float* size, float* life, float* rate, float* speed,
                         float* spread, float* growth, float* slowdown,
                         bool* diffuse, bool* airfade)
{
	if (!g_prtSpecCore || idx < 0) return false;
	PARTICLESTREAMSPEC ps;
	if (idx >= PrtStockForGroup(ThrClamp(g_fx.thrSel), idx, &ps)) return false;
	*size     = (float)ps.srcsize;
	*life     = (float)ps.lifetime;
	*rate     = (float)ps.srcrate;
	*speed    = (float)ps.v0;
	*spread   = (float)ps.srcspread;
	*growth   = (float)ps.growthrate;
	*slowdown = (float)ps.atmslowdown;
	*diffuse  = (ps.ltype == PARTICLESTREAMSPEC::DIFFUSE);
	*airfade  = (ps.atmsmap != PARTICLESTREAMSPEC::ATM_FLAT);
	return true;
}

// Same pattern for patch (l)'s textured sprites: the EXHAUST PARTICLES section greys
// out wholesale without them. Mirrored from prtTexMode each pre-step rather than once
// at start, because the probe is lazy (the atlas is created on the first update).
static bool g_prtSupported = false;
bool OroParticleTintOK() { return g_prtSupported; }

// And for the client's POST-PROCESSING (bloom). Not a patch and not a capability - it is
// a user SETTING, read out of D3D9Client.cfg at session start (see the block in
// clbkSimulationStart for why it earns its place). Two flags, because "we could not read
// the file" must not be reported as "your bloom is off".
static bool g_bloomOn = true, g_bloomKnown = false;
bool OroBloomOn()    { return g_bloomOn; }
bool OroBloomKnown() { return g_bloomKnown; }

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
	if (pCore && wetMirrorRegistered)
		pCore->RegisterRenderProc(OroWetMirrorProc, RENDERPROC_DELETE, nullptr);
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
	if (pCore && hRainPoly) {
		pCore->DeletePoly(hRainPoly);
		hRainPoly = NULL;
	}
	if (pCore && hRainGndPoly) {
		pCore->DeletePoly(hRainGndPoly);
		hRainGndPoly = NULL;
	}
	if (pCore && hRainRingCPoly) {
		pCore->DeletePoly(hRainRingCPoly);
		hRainRingCPoly = NULL;
	}
	if (pCore && hRainRingVPoly) {
		pCore->DeletePoly(hRainRingVPoly);
		hRainRingVPoly = NULL;
	}
	if (pCore && hRainDeckPoly) {
		pCore->DeletePoly(hRainDeckPoly);
		hRainDeckPoly = NULL;
	}
	if (pCore && hRainBoltPoly) {
		pCore->DeletePoly(hRainBoltPoly);
		hRainBoltPoly = NULL;
	}
	// patch (s): give the client its dry ground back. A world left wet by an addon that
	// is no longer running is a bug the user cannot even attribute to us.
	if (pCore && pCore->CanSetSurfaceWetness()) { pCore->SetSurfaceWetness(0.0f); wetPushed = -1.0f; }
	if (pCore && pCore->CanSetStormLight())     { pCore->SetStormLight(0.0f);     stormPushed = -1.0f; }
	if (pCore && pCore->CanSetWetDarkness())    { pCore->SetWetDarkness(1.0f);    wetDarkPushed = -1.0f; }
	if (pCore && pCore->CanSetWetGlint())       { pCore->SetWetGlint(1.0f);       glintPushed = -1.0f; }
	if (pCore && pCore->CanSetWetReflection())  { pCore->SetWetReflection(1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f);  reflPushed = -1.0f; swimAmpPushed = -1.0f; swimRatePushed = -1.0f; poolSizePushed = -1.0f; poolReachPushed = -1.0f; reflBlurPushed = -1.0f; }
	if (pCore && pCore->CanSetWetGrain())       { pCore->SetWetGrain(1.0f, 1.0f);  grainOpPushed = -1.0f; grainSizePushed = -1.0f; }
	// patch (aa): and its clear air - both fog layers to zero, the anchor forgotten
	if (pCore && pCore->CanSetFogLayer())       { pCore->SetFogLayer(0, 0.0, 0.0f, 1.0f, 0.0f); pCore->SetFogLayer(1, 0.0, 0.0f, 1.0f, 0.0f); }
	{ extern void OroFog_Reset(); OroFog_Reset(); fogNearDens = 0.0f; }
	if (pCore && pCore->CanSetBaseLights())     { pCore->SetBaseLights(false, 1.0f, 1.0f); blPushedOn = -1; blPushedGlow = -1.0f; blPushedHalo = -1.0f; }   // patch (ac): stock lights back
	if (pCore && pCore->CanSetVCNightLight())   { pCore->SetVCNightLight(1.0f); vcNightPushed = -1.0f; g_fx.vcNightLive = 1.0f; }   // patch (ad): the cabin lit as stock
	if (rainLtgLight && rainLtgLightV && oapiIsVessel(rainLtgLightV)) {
		VESSEL* lv = oapiGetVesselInterface(rainLtgLightV);
		if (lv) lv->DelLightEmitter(rainLtgLight);      // invariant 14: hand it back
	}
	rainLtgLight = NULL; rainLtgLightV = NULL; rainLtgLI = 0.0;
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
	if (pCore && pIPIGloom)  { pCore->ReleaseIPInterface(pIPIGloom);  pIPIGloom  = nullptr; }
	if (hFrameTex)         { oapiDestroySurface(hFrameTex); hFrameTex = NULL; }
	if (hBlurTex)          { oapiDestroySurface(hBlurTex);  hBlurTex  = NULL; }
	if (hLtgAtlas)         { oapiDestroySurface(hLtgAtlas); hLtgAtlas = NULL; }
	ltgTexMode = false; ltgTexTried = false;
	if (hRainCloudTex)     { oapiDestroySurface(hRainCloudTex); hRainCloudTex = NULL; }
	ringRefBody = NULL;                        // splash lattice re-anchors next frame
	rainCloudBuilt = -1; rainCloudN = 0;
	if (hBoltTex)          { oapiReleaseTexture(hBoltTex); hBoltTex = NULL; }
	boltTexTried = false;

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
	OroThr_CacheReset();     // the class cache is per session too (23m's rule: a
	                         //   file-static must not outlive what it describes -
	                         //   cfgs may have been edited between scenarios)
	OroRain_ShieldReset();   // same rule: re-probe the shield mesh next storm (he
	                         // iterates on the file between runs)
	{ extern void OroFog_Reset(); OroFog_Reset(); fogNearDens = fogNearDens0 = 0.0f; }
	blPushedOn = -1; blPushedGlow = -1.0f; blPushedHalo = -1.0f;   // patch (ac): the first push of the session is unconditional
	                         // and the fog's anchor + envelope (patch aa): a crash must
	                         // not leave last session's world anchored under this one
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
		// Patch (ad): the cabin at night - the same binding probe.
		vcNightSupported = pCore->CanSetVCNightLight();
		g_vcNightSupported = vcNightSupported;
		vcNightPushed = -1.0f;
		// Patch (n): per-vessel stock-exhaust suppression, probed by binding like the
		// rest. The dialog's STOCK EXHAUST pill greys out without it (invariant 18b).
		g_stockExSupported = pCore->CanSuppressExhaust();
		oapiWriteLogV("ORO: client VC shadows (patch f) %s.",
		              vcShadowSupported ? "available" : "NOT available");
		oapiWriteLogV("ORO: VC night light (patch ad) %s.",
		              vcNightSupported ? "available - the CABIN AT NIGHT section is live" : "NOT available - section greyed");
		oapiWriteLogV("ORO: stock exhaust suppression (patch n) %s.",
		              g_stockExSupported ? "available" : "NOT available");
		// Patch (y): stock stream-spec readback for the COPY STOCK buttons.
		g_prtSpecCore = pCore->CanGetExhaustStreamSpec() ? pCore : nullptr;
		oapiWriteLogV("ORO: stream-spec readback (patch y) %s.",
		              g_prtSpecCore ? "available - COPY STOCK live"
		                            : "NOT available - COPY STOCK greyed");
		oapiWriteLogV("ORO: surface wetness (patch s) %s.",
		              pCore->CanSetSurfaceWetness() ? "available - rain can wet the ground"
		                                            : "NOT available - rain falls on dry ground");
		oapiWriteLogV("ORO: storm light (patch s part 2) %s.",
		              pCore->CanSetStormLight() ? "available - overcast collapses the sun"
		                                        : "NOT available - storms stay sunlit");
		oapiWriteLogV("ORO: fog layers (patch aa) %s; snow cover %s.",
		              pCore->CanSetFogLayer() ? "available - the FOG page is live"
		                                      : "NOT available - FOG page inert",
		              pCore->CanSetSnowCover() ? "plumbed (dormant)" : "NOT available");
		g_baseLightsSupported = pCore->CanSetBaseLights();
		oapiWriteLogV("ORO: base lights (patch ac) %s.",
		              g_baseLightsSupported ? "available - the BASE LIGHTS pill is live" : "NOT available - pill greyed");
		oapiWriteLogV("ORO: additive sketchpad blend (patch d, probed by binding) %s; gcAPIVer reads %u (diagnostic only, known-broken).",
		              padAdditive ? "available" : "NOT available (plasma will alpha-blend)", specs.gcAPIVer);
	}

	// --- IS THE CLIENT'S BLOOM ON? ----------------------------------------
	// ⚠️ THIS IS A DIAGNOSTIC FOR A WHOLE CLASS OF LOOK COMPLAINT, and it exists because
	// two beta testers produced four of them on 2026-08-15 that we could not tell from a
	// settings line: reentry "looks pretty sharp", "a little pointy/sharp", and "max. eng
	// bell colour is yellow? Was expecting a white/red". Both subsystems DELEGATE WHITE TO
	// THE BLOOM by design - the plasma composites pre-resolve into the fp16 chain
	// specifically so white EMERGES from HDR accumulation (patch i, invariant 20), and the
	// bell overdrives its emissive x2.2 past the same threshold (invariant 23g). With
	// PostProcess=0 both are exactly the colours they were authored, and both read wrong
	// in exactly the ways reported. Neither log nor screenshot could distinguish that from
	// a real tuning fault, and the answer cost a round trip through a stranger's video
	// settings. Now it is one line in every log, and the panel says it.
	//
	// Read from the CLIENT's own cfg at the Orbiter ROOT (D3D9Config.cpp does the same),
	// because there is no gcCore query for it. ZEROONFAIL: a missing file reads 0, which
	// we treat as UNKNOWN rather than as "off" - a false alarm about someone's settings is
	// worse than no alarm. Main thread, session start, once.
	{
		bloomKnown = false;
		bloomOn    = true;                          // assume the good case until told otherwise
		FILEHANDLE fc = oapiOpenFile("D3D9Client.cfg", FILE_IN_ZEROONFAIL, ROOT);
		if (fc) {
			int pp = -1;
			if (oapiReadItem_int(fc, (char*)"PostProcess", pp) && pp >= 0) {
				bloomKnown = true;
				bloomOn    = (pp != 0);
			}
			oapiCloseFile(fc, FILE_IN_ZEROONFAIL);
		}
		g_bloomOn    = bloomOn;                     // dialog-visible mirrors
		g_bloomKnown = bloomKnown;
		oapiWriteLogV("ORO: client post-processing (Light glow) %s.",
		              !bloomKnown ? "UNKNOWN - D3D9Client.cfg unreadable; assuming ON"
		                          : (bloomOn ? "ON - plasma and bell reach white through the bloom"
		                                     : "OFF - plasma will read hard-edged and the bell will read amber; "
		                                       "turn PostProcess on in Video / Advanced for the intended look"));
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

	// Patch (u): the wet-mirror slot, so ORO's own plume appears in the standing water.
	// Same registration story as (i) - any id is accepted, a pre-(u) client just never
	// calls it - with one extra wrinkle worth knowing before reading a log: this one
	// ALSO does not fire on a patched client until the ground is actually wet and the
	// camera is under 250 m AGL, because that is when the reflection pass itself runs.
	// So "wetMirrorLive still false" means "not proven yet", never "not supported".
	if (!wetMirrorRegistered) {
		if (pCore->RegisterRenderProc(OroWetMirrorProc, RENDERPROC_WET_MIRROR, this)) {
			wetMirrorRegistered = true;
			oapiWriteLogV("ORO: wet-mirror proc registered (RENDERPROC_WET_MIRROR, patch u). Fires only on a Build >= 260825 client, and only in the rain near the ground.");
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
	// SOUNDS LIVE IN XRSound\ORO\ (his call, 2026-08-23): the Orbiter convention -
	// textures in Textures\, meshes in Meshes\, sounds under XRSound\<addon>\, exactly
	// where ChessMFD and CrewMFD put theirs. (They lived in Modules\ORO\sounds\ before.)
	if (!pXRSound) pXRSound = XRSound::CreateInstance("ORO");
	rainSndLoaded = false;   // re-proven every session, never inherited
	if (pXRSound && pXRSound->IsPresent()) {
		oapiWriteLogV("ORO: XRSound %.2f connected.", pXRSound->GetVersion());
		if (!pXRSound->LoadWav(SND_HEARTBEAT, "XRSound\\ORO\\heartbeat.wav", XRSound::PlaybackType::Global))
			oapiWriteLogV("ORO: heartbeat.wav not found - drop a WAV at XRSound\\ORO\\heartbeat.wav (heartbeat sound stays off until then).");
		// Scenario clips (Induce_*/Recover_*): one per INDUCE_SEQ entry, id SND_SCEN_BASE + i.
		// Missing files are fine - that button just runs silent; the user adds them over time.
		for (int i = 0; i < NSCEN; i++) {
			char path[MAX_PATH];
			sprintf_s(path, "XRSound\\ORO\\%s", INDUCE_SEQ[i].wav);
			if (!pXRSound->LoadWav(SND_SCEN_BASE + i, path, XRSound::PlaybackType::Global))
				oapiWriteLogV("ORO: scenario clip %s not found - that button runs silent.", INDUCE_SEQ[i].wav);
		}
		// The RAIN loops (tools/raingen.py). All-or-nothing: a crossfade missing one
		// tier would leave a silent hole in the middle of the envelope, so one missing
		// file disables the rain sound rather than degrading it confusingly. The
		// fourth is the interior hull-tap loop (2026-08-23) - generated by the same
		// tool, shipped with its siblings.
		static const char* RAIN_WAVS[] = { "Rain_light.wav", "Rain_medium.wav", "Rain_heavy.wav", "Rain_hull.wav" };
		rainSndLoaded = true;
		for (int i = 0; i < SND_RAIN_N; i++) {
			char path[MAX_PATH];
			sprintf_s(path, "XRSound\\ORO\\%s", RAIN_WAVS[i]);
			if (!pXRSound->LoadWav(SND_RAIN_BASE + i, path, XRSound::PlaybackType::Global)) {
				oapiWriteLogV("ORO: %s not found - rain sound disabled (visuals unaffected).", RAIN_WAVS[i]);
				rainSndLoaded = false;
			}
		}
		if (rainSndLoaded) oapiWriteLogV("ORO: rain sound loops loaded (3 tiers).");
		// ... and their MUFFLED interior twins (tools/rainmuffle.py, 2026-08-27).
		// All-or-nothing like the exterior set; absent, the mixer falls back to the
		// old inside-the-hull volume duck rather than half a crossfade.
		static const char* RAIN_IN_WAVS[] = { "Rain_light_in.wav", "Rain_medium_in.wav", "Rain_heavy_in.wav" };
		rainSndInLoaded = true;
		for (int i = 0; i < 3; i++) {
			char path[MAX_PATH];
			sprintf_s(path, "XRSound\\ORO\\%s", RAIN_IN_WAVS[i]);
			if (!pXRSound->LoadWav(SND_RAIN_IN_BASE + i, path, XRSound::PlaybackType::Global))
				rainSndInLoaded = false;
		}
		oapiWriteLogV("ORO: interior (muffled) rain tiers %s.",
		              rainSndInLoaded ? "loaded" : "missing - volume duck fallback");
		// The THUNDER set (sourced from freesound - the credit ledger is
		// XRSound\ORO\README.txt; leveled by tools/thunderprep.py). Per-file
		// tolerant: a missing variant narrows the pick, an empty class skips.
		{
			static const char* THUN_CLS[] = { "close", "mid", "far" };
			int nThun = 0;
			for (int i = 0; i < THUN_FILES; i++) {
				char path[MAX_PATH];
				sprintf_s(path, "XRSound\\ORO\\Thunder_%s_%d.wav", THUN_CLS[i / 3], (i % 3) + 1);
				thunLoaded[i] = pXRSound->LoadWav(SND_THUNDER_BASE + i, path, XRSound::PlaybackType::Global);
				if (thunLoaded[i]) nThun++;
				// ... and the clap's muffled twin (rainmuffle.py) - same per-file
				// tolerance; the fire site picks the family by the CURRENT view.
				sprintf_s(path, "XRSound\\ORO\\Thunder_%s_%d_in.wav", THUN_CLS[i / 3], (i % 3) + 1);
				thunInLoaded[i] = pXRSound->LoadWav(SND_THUNDER_IN_BASE + i, path, XRSound::PlaybackType::Global);
			}
			oapiWriteLogV("ORO: thunder set - %d of %d files loaded.", nThun, (int)THUN_FILES);
		}
	} else {
		oapiWriteLogV("ORO: XRSound not present - sounds disabled (visuals unaffected).");
	}
	// Fresh session, fresh mixer: no loop is playing yet, whatever a previous
	// session's state said (the 23(m) sweep - state reset belongs at START).
	for (int i = 0; i < SND_RAIN_CH; i++) {
		rainSndLvl[i] = 0.0f; rainSndOn[i] = false; rainSndPushed[i] = 0.0f;
	}
	for (int q = 0; q < THUN_Q; q++) thunQ[q].vol = 0.0f;
	thunPrimed = false;
	thunLastStrikeT0 = boltTestT0;   // whatever the stamp holds, it is not a NEW press
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
		for (int i = 0; i < NSCEN; i++) {
			pXRSound->SetPaused(SND_SCEN_BASE + i, false);   // never leave a voice parked paused
			pXRSound->StopWav(SND_SCEN_BASE + i);
		}
		for (int i = 0; i < SND_RAIN_N; i++) pXRSound->StopWav(SND_RAIN_BASE + i);
		for (int i = 0; i < 3; i++)          pXRSound->StopWav(SND_RAIN_IN_BASE + i);
		for (int i = 0; i < THUN_FILES; i++) pXRSound->StopWav(SND_THUNDER_BASE + i);
		for (int i = 0; i < THUN_FILES; i++) pXRSound->StopWav(SND_THUNDER_IN_BASE + i);
		delete pXRSound; pXRSound = nullptr;
	}
	seqSoundPlaying = false;

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
	// the rain's cloud-deck texture: same shape exactly - its POLY holds it bound,
	// so the poly goes first (23l's dependency ordering, third instance)
	if (pCore && hRainDeckPoly) {
		pCore->DeletePoly(hRainDeckPoly);
		hRainDeckPoly = NULL;
	}
	if (pCore && hRainBoltPoly) {
		pCore->DeletePoly(hRainBoltPoly);
		hRainBoltPoly = NULL;
	}
	if (hRainCloudTex) { oapiDestroySurface(hRainCloudTex); hRainCloudTex = NULL; }
	ringRefBody = NULL;                        // splash lattice re-anchors next frame
	rainCloudBuilt = -1; rainCloudN = 0;       // recreated next session
	if (hBoltTex) { oapiReleaseTexture(hBoltTex); hBoltTex = NULL; }
	boltTexTried = false;
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
	if (pCore && hRainPoly) {
		pCore->DeletePoly(hRainPoly);
		hRainPoly = NULL;
	}
	if (pCore && hRainGndPoly) {
		pCore->DeletePoly(hRainGndPoly);
		hRainGndPoly = NULL;
	}
	if (pCore && hRainRingCPoly) {
		pCore->DeletePoly(hRainRingCPoly);
		hRainRingCPoly = NULL;
	}
	if (pCore && hRainRingVPoly) {
		pCore->DeletePoly(hRainRingVPoly);
		hRainRingVPoly = NULL;
	}
	if (pCore && hRainDeckPoly) {
		pCore->DeletePoly(hRainDeckPoly);
		hRainDeckPoly = NULL;
	}
	if (pCore && hRainBoltPoly) {
		pCore->DeletePoly(hRainBoltPoly);
		hRainBoltPoly = NULL;
	}
	// patch (s): give the client its dry ground back. A world left wet by an addon that
	// is no longer running is a bug the user cannot even attribute to us.
	if (pCore && pCore->CanSetSurfaceWetness()) { pCore->SetSurfaceWetness(0.0f); wetPushed = -1.0f; }
	if (pCore && pCore->CanSetStormLight())     { pCore->SetStormLight(0.0f);     stormPushed = -1.0f; }
	if (pCore && pCore->CanSetWetDarkness())    { pCore->SetWetDarkness(1.0f);    wetDarkPushed = -1.0f; }
	if (pCore && pCore->CanSetWetGlint())       { pCore->SetWetGlint(1.0f);       glintPushed = -1.0f; }
	if (pCore && pCore->CanSetWetReflection())  { pCore->SetWetReflection(1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f);  reflPushed = -1.0f; swimAmpPushed = -1.0f; swimRatePushed = -1.0f; poolSizePushed = -1.0f; poolReachPushed = -1.0f; reflBlurPushed = -1.0f; }
	if (pCore && pCore->CanSetWetGrain())       { pCore->SetWetGrain(1.0f, 1.0f);  grainOpPushed = -1.0f; grainSizePushed = -1.0f; }
	// patch (aa): and its clear air - both fog layers to zero, the anchor forgotten
	if (pCore && pCore->CanSetFogLayer())       { pCore->SetFogLayer(0, 0.0, 0.0f, 1.0f, 0.0f); pCore->SetFogLayer(1, 0.0, 0.0f, 1.0f, 0.0f); }
	{ extern void OroFog_Reset(); OroFog_Reset(); fogNearDens = 0.0f; }
	if (pCore && pCore->CanSetBaseLights())     { pCore->SetBaseLights(false, 1.0f, 1.0f); blPushedOn = -1; blPushedGlow = -1.0f; blPushedHalo = -1.0f; }   // patch (ac): stock lights back
	if (pCore && pCore->CanSetVCNightLight())   { pCore->SetVCNightLight(1.0f); vcNightPushed = -1.0f; g_fx.vcNightLive = 1.0f; }   // patch (ad): the cabin lit as stock
	if (rainLtgLight && rainLtgLightV && oapiIsVessel(rainLtgLightV)) {
		VESSEL* lv = oapiGetVesselInterface(rainLtgLightV);
		if (lv) lv->DelLightEmitter(rainLtgLight);      // invariant 14: hand it back
	}
	rainLtgLight = NULL; rainLtgLightV = NULL; rainLtgLI = 0.0;
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
	if (pCore && pIPIGloom)  { pCore->ReleaseIPInterface(pIPIGloom);  pIPIGloom  = nullptr; }
	if (hFrameTex)         { oapiDestroySurface(hFrameTex); hFrameTex = NULL; }
	if (hBlurTex)          { oapiDestroySurface(hBlurTex);  hBlurTex  = NULL; }
	if (hLtgAtlas)         { oapiDestroySurface(hLtgAtlas); hLtgAtlas = NULL; }
	ltgTexMode = false; ltgTexTried = false;   // re-probed + recreated next session
	if (hRainCloudTex)     { oapiDestroySurface(hRainCloudTex); hRainCloudTex = NULL; }
	ringRefBody = NULL;                        // splash lattice re-anchors next frame
	rainCloudBuilt = -1; rainCloudN = 0;
	if (hBoltTex)          { oapiReleaseTexture(hBoltTex); hBoltTex = NULL; }
	boltTexTried = false;
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

// ============================================================================
// SENSE THE VIEW - which domain the camera is in, and how big the frame is.
//
// EXTRACTED FROM clbkPreStep 2026-08-24 SO IT CAN RUN WHILE PAUSED. The note below
// already said what was wrong - "clbkPreStep is not called while paused, so a view
// change made while paused will not update the gate until the sim resumes" - and a
// public-beta tester duly found it from outside the code: pause in an external view,
// switch to the VC, and the rain draws with the external gate still set, which puts
// raindrops inside the cabin. Confirmed in the core rather than inferred - Orbiter.cpp
// UpdateWorld(): "if (bRunning) ModulePreStep();".
//
// Called from clbkPreStep AND from clbkProcessKeyboardImmediate, which the core calls
// every frame whether or not the sim is running. Pure sensing - nothing here
// accumulates - so running it twice in one frame is a no-op, and that is exactly what
// makes it safe to call from two places instead of needing a "already ran" flag.
// ============================================================================
void OroModule::SenseView()
{
	// Compute the view gate on the MAIN thread (here), where oapi camera/cockpit queries
	// are safe, and cache it for the render callback to read. Effects apply only in an
	// internal panel/VC view - NOT the generic glass cockpit (which stays the natural
	// kill: F8 to it clears the effect). NOTE: clbkPreStep is not called while paused, so
	// a view change made while paused won't update the gate until the sim resumes.
	// The VC-ONLY option narrows it further, to the virtual cockpit alone (see g_fx.fxVCOnly).
	// Note the two toggles pull opposite ways on purpose: reentryVC WIDENS the plasma's domain
	// into the VC, fxVCOnly NARROWS the physiology's out of the 2D panel. Both exist because
	// the VC is the one internal view with real geometry to be part of.
	viewGate = oapiCameraInternal()
	        && (oapiCockpitMode() != COCKPIT_GENERIC)
	        && (!g_fx.fxVCOnly || oapiCockpitMode() == COCKPIT_VIRTUAL);

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

	// THE RAIN'S VC GATE (2026-08-23, his ask: "just to be able to see the rain from
	// the VC"). Same shape as the plasma's crack in the wall, VIRTUAL cockpit only
	// (2D panels and the glass cockpit are flat overlays - the plasma's reasoning
	// holds unchanged) - but no dialog toggle: like the aurora and the lightning
	// through the windows, it simply follows the view. ⚠️ depthClipOK is EMBEDDED:
	// the whole trick is patch (g) cutting every streak at the window frame per
	// pixel (the scene depth includes the cockpit - the aurora's mechanism), and
	// without real depth the VC must have NO rain rather than rain painted over
	// the cabin. Opening this gate also opens s_gateF in the VC, so the client
	// pushes (wet ground, storm light) and the sounds come inside with it.
	rainVC = oapiCameraInternal() && (oapiCockpitMode() == COCKPIT_VIRTUAL)
	      && depthClipOK;
	// The PLUME follows the same recipe (2026-08-29): RCS jets are visible from the
	// flight deck, so the jet draws in the VC too, cut at the window frame per pixel.
	plumeVC = rainVC;

	// ⚠️ THE FLAT INTERNAL VIEWS, ON DIFFERENT TERMS (2026-08-25). A tester asked for the
	// rain in the 2D panel and the glass cockpit, and the reason it was VC-only turns out
	// not to apply there at all. The VC needs patch (g) because the cockpit is real 3D
	// geometry drawn in the main scene, so without a per-pixel cut the streaks paint over
	// the cabin. A 2D panel is not geometry - it is painted by the core's Pane::Render,
	// which runs AFTER the pre-resolve slot ORO draws the rain in. So the panel covers the
	// rain by DRAW ORDER, for free, and no depth buffer is involved.
	// What it does require is patch (i): without the pre-resolve slot the rain falls back
	// to RENDERPROC_HUD_2ND, which is after the pane, and it would paint over the panel -
	// exactly the failure the VC gate exists to prevent. Hence preResolveLive here and
	// depthClipOK there; each mode is gated on the thing that actually makes it correct.
	// Kept as its OWN flag rather than widening rainVC, because rainVC still selects the
	// deeper sheet plane and the rain-shield test, and neither belongs to a flat panel.
	rainPanel = false;
	if (g_fx.rainViewMode >= 1 && oapiCameraInternal() && preResolveLive) {
		const int cm = oapiCockpitMode();
		if      (cm == COCKPIT_PANELS)  rainPanel = true;
		else if (cm == COCKPIT_GENERIC) rainPanel = (g_fx.rainViewMode >= 2);
	}

	// Viewport size for the render pass (tunnel geometry) - cached HERE because the
	// render callback makes no oapi calls by policy.
	oapiGetViewportSize(&viewW, &viewH);

	SenseMarker();     // Phase B: selection validity, its readouts, the nozzle marker
}

// ----------------------------------------------------------------------------
// Phase B's sensing half (2026-08-30): validate the header-row thruster selection
// against the PANEL vessel, publish the readouts the dialog paints from (paint may
// not call oapi - the plumeRegime discipline), and snapshot the nozzle marker(s)
// for the render path. Rides SenseView, so it runs from clbkPreStep AND the
// keyboard-immediate tick: every frame, paused included, idempotent throughout.
// ----------------------------------------------------------------------------
void OroModule::SenseMarker()
{
	mkN = 0;
	g_fx.thrPageLive = OroDlg_ThrPageLive();   // single writer; close-safe (IsWindow)
	// The PANEL vessel - the ship the thruster pages describe (camera target,
	// focus fallback: the same resolution as thrAvail and COPY STOCK).
	OBJHANDLE h = oapiCameraTarget();
	if (!h || oapiGetObjectType(h) != OBJTP_VESSEL) h = oapiGetFocusObject();
	VESSEL* v = (h && oapiGetObjectType(h) == OBJTP_VESSEL) ? oapiGetVesselInterface(h) : NULL;
	if (!v) {
		// ⚠️ A dying selection must RESYNC the flat buffer (memory-only, safe here):
		// left alone, the buffer keeps the dead thruster's values with the selection
		// reading ALL, and the next SyncOut would copy them onto the GROUP.
		if (g_fx.thrThrSel >= 0) { g_fx.thrThrSel = -1; OroThr_SyncIn(); }
		g_fx.thrCnt = 0; g_fx.thrOrd = 0;
		g_fx.thrSelInfo[0] = 0; g_fx.thrSelBelowFloor = false; g_fx.thrSelHasExh = false;
		return;
	}

	const int   grp  = (g_fx.thrSel >= 0 && g_fx.thrSel < ORO_THR_N) ? g_fx.thrSel : 0;
	const DWORD nthr = v->GetThrusterCount();

	// Membership + validation: a selection that no longer classifies into the
	// selected group (vessel switch, group cycle from the keyboard path, staging)
	// falls back to ALL rather than pointing at a stranger's jet.
	int cnt = 0, ord = 0;
	int selIdx = g_fx.thrThrSel;
	bool selValid = false;
	for (DWORD i = 0; i < nthr; i++) {
		THRUSTER_HANDLE th = v->GetThrusterHandleByIndex(i);
		if (!th || OroThrusterGroupOf(v, th) != grp) continue;
		cnt++;
		if ((int)i == selIdx) { selValid = true; ord = cnt; }
	}
	if (selIdx >= 0 && !selValid) {
		// Same resync rule as the no-vessel path above: a force-reset without a
		// SyncIn leaves the dead thruster's values in the flat buffer addressed to
		// the GROUP - the one way this design could silently rewrite group settings.
		g_fx.thrThrSel = -1; selIdx = -1; ord = 0;
		OroThr_SyncIn();
	}
	g_fx.thrCnt = cnt;
	g_fx.thrOrd = ord;

	g_fx.thrSelInfo[0]    = 0;
	g_fx.thrSelBelowFloor = false;
	g_fx.thrSelHasExh     = false;

	const bool wantMarkers = g_fx.thrMarkOn && g_fx.thrPageLive && g_fx.masterArmed;
	mkOwn = h;
	v->GetGlobalPos(mkCg);
	mkScale = v->GetSize();

	if (selIdx < 0 && !wantMarkers) return;    // nothing below is needed

	// ⚠️ MARKERS ARE PER NOZZLE, NOT PER THRUSTER (his DG report, 2026-08-30). The
	// DG feeds one attitude thruster from SEVERAL AddExhaust nozzles (each pitch
	// thruster owns two, port + starboard), so a per-thruster marker ringed the
	// first nozzle and left its twins bare - "some marks are missing". The JET
	// draws per exhaust; the marker now walks the same list and agrees with it.
	// The selected thruster rings ALL of its nozzles bright - the honest picture,
	// since one thruster's override covers all its exhausts. A thruster with NO
	// authored exhaust (the scramjet family, 26j) falls back to GetThrusterRef/Dir
	// with one marker, exactly as before.
	static const int TH_MAX = 256;
	THRUSTER_HANDLE hh[TH_MAX];
	bool exSeen[TH_MAX] = {};
	const int nh = (int)(nthr < (DWORD)TH_MAX ? nthr : (DWORD)TH_MAX);
	for (int t = 0; t < nh; t++) hh[t] = v->GetThrusterHandleByIndex((DWORD)t);

	auto wantThr = [&](int ti) -> bool {
		if (!hh[ti]) return false;
		if (selIdx >= 0) return ti == selIdx;
		return OroThrusterGroupOf(v, hh[ti]) == grp;
	};
	const bool bright = (selIdx >= 0);

	EXHAUSTSPEC es;
	const DWORD nex = v->GetExhaustCount();
	for (DWORD e = 0; e < nex; e++) {
		v->GetExhaustSpec(e, &es);
		if (!es.th || !es.lpos || !es.ldir) continue;
		int ti = -1;
		for (int t = 0; t < nh; t++) if (hh[t] == es.th) { ti = t; break; }
		if (ti < 0) continue;
		exSeen[ti] = true;                     // the caption's "has authored exhaust"
		if (!wantMarkers || mkN >= MK_MAX || !wantThr(ti)) continue;
		VECTOR3 ldir = *es.ldir;
		const double L = length(ldir);
		if (L < 1e-9) continue;
		ldir = ldir * (1.0 / L);
		v->Local2Global(*es.lpos - ldir * es.lofs, mkPos[mkN]);
		VECTOR3 fl = -ldir;                    // exhaust flows OPPOSITE the thrust
		v->GlobalRot(fl, mkDir[mkN]);
		mkBri[mkN] = bright;
		mkN++;
	}
	// Exhaust-less thrusters: one marker at the thrust reference (26j's family).
	if (wantMarkers) {
		for (int t = 0; t < nh && mkN < MK_MAX; t++) {
			if (exSeen[t] || !wantThr(t)) continue;
			VECTOR3 lpos, ldir;
			v->GetThrusterRef(hh[t], lpos);
			v->GetThrusterDir(hh[t], ldir);
			const double L = length(ldir);
			if (L < 1e-9) continue;
			ldir = ldir * (1.0 / L);
			v->Local2Global(lpos, mkPos[mkN]);
			VECTOR3 fl = -ldir;
			v->GlobalRot(fl, mkDir[mkN]);
			mkBri[mkN] = bright;
			mkN++;
		}
	}

	if (selIdx >= 0 && selIdx < nh && hh[selIdx]) {
		const double m  = v->GetMass();
		const double f0 = v->GetThrusterMax0(hh[selIdx]);
		g_fx.thrSelHasExh = exSeen[selIdx];
		// 26(j)'s vent reading in static form: no authored exhaust AND too weak to
		// push the ship even at FULL throttle = plumbing, not propulsion. The plume
		// admission gate holds for it, and the caption must say so (H5's law).
		g_fx.thrSelBelowFloor = !exSeen[selIdx] && m > 1.0 && (f0 / m) < OroPlumeSynAccMin();
		if      (f0 >= 9.5e5) sprintf_s(g_fx.thrSelInfo, "thr %d - %.2f MN", selIdx, f0 * 1e-6);
		else if (f0 >= 950.0) sprintf_s(g_fx.thrSelInfo, "thr %d - %.1f kN", selIdx, f0 * 1e-3);
		else                  sprintf_s(g_fx.thrSelInfo, "thr %d - %.0f N",  selIdx, f0);
	}
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

	// The FALLBACK projection camera, for a client without patch (k). The aurora, the
	// lightning and the plasma are all projected in the render path now (2026-08-15) and
	// prefer the render camera; this is what they fall back to, and it is exactly the
	// pre-2026-08-15 behaviour - one step stale, and frozen while paused.
	SnapPreStepCam();

	// View domain + frame size. Re-asked every frame, paused or not - see SenseView.
	SenseView();

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
			if (prev >= 0 && prev < NSCEN) {
				// Clear any PAUSE we left on it (see the toggle edge below) before stopping,
				// so the next PlayWav on this id does not start into a paused mixer voice.
				pXRSound->SetPaused(SND_SCEN_BASE + prev, false);
				pXRSound->StopWav(SND_SCEN_BASE + prev);
			}
			seqSoundPlaying = false;
			if (g_fx.seqActive >= 0 && g_fx.seqSoundEnabled)
				seqSoundPlaying = pXRSound->PlayWav(SND_SCEN_BASE + g_fx.seqActive, false, 1.0f);
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
				if (pXRSound && pXRSound->IsPresent()) {
					pXRSound->SetPaused(SND_SCEN_BASE + g_fx.seqActive, false);
					pXRSound->StopWav(SND_SCEN_BASE + g_fx.seqActive);
				}
				seqSoundPlaying = false;
				g_fx.seqActive = -1;
				g_fx.blackout = g_fx.redout = g_fx.tunnel = g_fx.spots = g_fx.greyout =
				g_fx.blur = g_fx.heartbeat = g_fx.aberration = g_fx.sparkles = g_fx.swim = g_fx.tilt = 0.0f;
			}
		}
	}

	// Sound toggle edge, mid-scenario. It USED to be a one-way street: the mute stopped the
	// clip and turning the switch back on did nothing until the next scenario, which a beta
	// tester reported and which invariant 8's "also mutes mid-run" quietly over-claimed.
	// The fix is to stop STOPPING it. XRSound has SetPaused/SetPlayPosition, so a mute can
	// PAUSE the voice and un-muting resumes exactly where the visuals are - a Stop+Play would
	// restart the narration from the top, describing a G-event that is halfway over.
	// The one case with nothing to resume is a scenario that STARTED silent; there the clip
	// is begun now and SEEKED to the timeline, which lands in the same place.
	if (pXRSound && pXRSound->IsPresent() && g_fx.seqSoundEnabled != seqSoundWasOn
	    && g_fx.seqActive >= 0 && g_fx.seqActive < NSCEN) {
		const int id = SND_SCEN_BASE + g_fx.seqActive;
		if (!g_fx.seqSoundEnabled) {
			if (seqSoundPlaying) pXRSound->SetPaused(id, true);
		} else if (seqSoundPlaying) {
			pXRSound->SetPaused(id, false);
		} else if (pXRSound->PlayWav(id, false, 1.0f)) {
			// seqT is REAL seconds into the scenario and each wav is authored to the
			// scenario's own duration (invariant 8), so the timeline IS the clip position.
			pXRSound->SetPlayPosition(id, (unsigned int)(seqT * 1000.0));
			seqSoundPlaying = true;
		}
	}
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
	// --- PER-THRUSTER-GROUP: which groups exist, and bank the sliders -------
	// THE ONLY SYNC POINT (see the note above OroThrusterFx). Everything the dialog has
	// been editing lands in thr[thrSel] here, and from this line on every consumer reads
	// thr[] by the group it is drawing. It MUST precede BuildPlumeModel.
	//
	// thrAvail is recomputed every step because it is a fact about the CURRENT vessel -
	// docking, undocking or a focus change can add or remove groups under us. If the
	// selected group vanishes (you tuned HOVER, then switched to a ship without hovers)
	// the selection falls back to the first group that does exist rather than editing a
	// group nothing can show.
	{
		VESSEL* tv = NULL;
		OBJHANDLE th = oapiCameraTarget();
		if (!th || oapiGetObjectType(th) != OBJTP_VESSEL) th = oapiGetFocusObject();
		if (th && oapiGetObjectType(th) == OBJTP_VESSEL) tv = oapiGetVesselInterface(th);
		int avail = 0;
		if (tv) {
			if (tv->GetGroupThrusterCount(THGROUP_MAIN)  > 0) avail |= 1 << ORO_THR_MAIN;
			if (tv->GetGroupThrusterCount(THGROUP_HOVER) > 0) avail |= 1 << ORO_THR_HOVER;
			if (tv->GetGroupThrusterCount(THGROUP_RETRO) > 0) avail |= 1 << ORO_THR_RETRO;
			if (OroThrusterHasUser(tv))                       avail |= 1 << ORO_THR_USER;
			if (OroThrusterHasRcs(tv))                        avail |= 1 << ORO_THR_RCS;
		}
		if (!avail) avail = 1 << ORO_THR_MAIN;   // never leave the cycler with nothing
		g_fx.thrAvail = avail;
		OroThr_SyncOut();
		if (!(avail & (1 << g_fx.thrSel))) {
			for (int i = 0; i < ORO_THR_N; i++)
				if (avail & (1 << i)) { g_fx.thrSel = i; break; }
			OroThr_SyncIn();
		}
	}

	// THE PLUME MODEL: regime, strongest-6 selection and the four physics curves,
	// built ONCE for every consumer below (OroPlume.cpp). The LAB|PHYSICS switch
	// acts inside it; both modes are anchored identical at sea level, full throttle.
	// The strongest-6 pool stays SHARED across groups (his call): the six brightest
	// plumes on the ship get drawn whichever group they came from, and a group whose
	// plume pill is off now frees its slots instead of holding them.
	BuildPlumeModel();

	// (CONSUMER 2, the exhaust shimmer's screen-space capsules, and CONSUMER 1, the plume
	//  jet geometry, both MOVED TO THE RENDER PATH on 2026-08-15 - see ProjCam. Both are
	//  screen-space and clbkPreStep does not run while paused. The MODEL above stays here:
	//  it is physics, it needs oapi, and it is the same for any camera.)

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
	// PATCH (h), the same probe-by-binding discipline and the same reason for logging it:
	// without the depth buffer in the IPI shader the windscreen drops have no way to tell
	// glass from instrument panel, so they do not draw at all - and a silently absent
	// effect is exactly the ambiguity the (g) line above exists to remove.
	ipiDepthOK = pCore && pCore->CanSetIPISceneDepth() && pCore->HasDepthBuffer();
	if (ipiDepthLogged != (ipiDepthOK ? 1 : 0)) {
		ipiDepthLogged = ipiDepthOK ? 1 : 0;
		oapiWriteLogV("ORO: client scene depth in IPI (patch h) %s.",
		              ipiDepthOK ? "available - raindrops on the VC glass"
		                         : "NOT available - no drops on the glass");
	}
	// TEMPORARY DIAGNOSTIC (2026-08-26) - see glassDiag. One line a second while the rain
	// is live, so a short VC flight says which link is open. Removed on sign-off.
	if (rainIntensityLive > 0.002f && simt - glassDiagT > 1.0) {
		glassDiagT = simt;
		oapiWriteLogV("ORO GLASS DIAG: 0x%04X entry[call%d I%d ipi%d core%d tex%d gloom%d vgate%d]"
		              " gate[vc%d ok%d ipiD%d sld%d vh%d cam%d bind%d]"
		              " Irender %.3f Ipre %.3f dr %.3f dbg %.0f",
		              glassDiag,
		              (glassDiag & 0x0080) ? 1 : 0, (glassDiag & 0x0100) ? 1 : 0,
		              (glassDiag & 0x0200) ? 1 : 0, (glassDiag & 0x0400) ? 1 : 0,
		              (glassDiag & 0x0800) ? 1 : 0, (glassDiag & 0x1000) ? 1 : 0,
		              (glassDiag & 0x2000) ? 1 : 0,
		              (glassDiag & 0x01) ? 1 : 0, (glassDiag & 0x02) ? 1 : 0,
		              (glassDiag & 0x04) ? 1 : 0, (glassDiag & 0x08) ? 1 : 0,
		              (glassDiag & 0x10) ? 1 : 0, (glassDiag & 0x20) ? 1 : 0,
		              (glassDiag & 0x40) ? 1 : 0,
		              glassDiagI, rainIntensityLive, glassDiagDr, g_fx.rainGlassDbg);
		glassDiag = 0;   // so a stale value can never be read as a fresh one
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
	UpdateRain();              // EVOLVES the storm: the envelope ramp and the wetness
	                           //   soak. Correctly frozen under pause - no sim time
	                           //   passes, so nothing should get wetter (his rule).
	SenseRain();               // SENSES it: which view, which world, how high the camera
	                           //   is. Also called every frame from
	                           //   clbkProcessKeyboardImmediate so it stays true while
	                           //   paused; idempotent, so the double call is a no-op.
	                           //   ⚠️ MUST FOLLOW UpdateRain here - it reads the envelope
	                           //   that call just advanced.
	PushSurfaceWet();          // patch (s) - client state, pushed on change (invariant 18)
	UpdateFog();               // THE FOG (patch aa): evolves the envelope + the anchor slew
	SenseFog();                //   senses world/air/ground/altitude (also every frame from
	                           //   the keyboard tick, so it stays true while paused)
	PushFog();                 //   the two layers to the client, on change. AFTER
	                           //   PushSurfaceWet: the storm mist reads rainIntensityLive.
	PushBaseLights();          // the BASE LIGHTS pill + glow (patch ac), on change
	SenseVCNight();            // the cabin at night (patch ad): the sun at the camera, every frame
	PushVCNight();             //   ... and the scale to the client, on change
	UpdateRainSound();         // the loop crossfade rides the envelope just published
	UpdateThunder();           // flash events -> delayed one-shots (dist/340 s)
	UpdateRainFlashLight();    // rain lightning's borrowed scene light - unconditional,
	                           //   so every gate failure RETURNS the borrow (inv. 14)

	// ⚠️ THIS LIST IS THE SET OF hFrameTex CONSUMERS, AND IT MUST TRACK THEM (2026-08-26).
	// Every Draw*Pass that resamples the frame early-returns on !hFrameTex, so a consumer
	// missing from this list does not error - it silently draws nothing, UNLESS some other
	// effect happened to create the texture first (it persists for the session, so one
	// moment of lit engines or one frame of greyout arms it forever - which is exactly why
	// the gap hid). Found because the GLASS DROPS were the first consumer ever tested from
	// a cold start with nothing else live: parked VC storm, PHYSICS mode at rest, engines
	// idle - list all false, texture never allocated, whole premium stack skipped. The
	// GOD RAYS and the rain GLOOM had the same latent gap since the day each shipped.
	// New resample consumer => new line here, in the same commit.
	if (ipiReady && (eclActive ||
	                 (g_fx.greyoutEnabled    && g_fx.greyout    > 0.001f) ||
	                 (g_fx.blurEnabled       && g_fx.blur       > 0.001f) ||
	                 (g_fx.aberrationEnabled && g_fx.aberration > 0.001f) ||
	                 (g_fx.swimEnabled       && g_fx.swim       > 0.001f) ||
	                 (g_fx.tiltEnabled       && (g_fx.tilt > 0.001f || fabs(g_fx.tiltLean) > 0.001f)) ||
	                 (g_fx.reentryEnabled    && plasmaGlow > 0.001f) ||
	                 plumeCount > 0 ||
	                 grActive ||                       // god rays - missing since 2026-08-11
	                 rainIntensityLive > 0.002f))      // rain gloom + glass drops - since 2026-08-22
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
	// the rain-lightning light rides the focus vessel; if that vessel dies, return
	// the borrow while the handle is still valid (the callback precedes destruction)
	if (hVessel == rainLtgLightV && rainLtgLight) {
		VESSEL* lv = oapiGetVesselInterface(hVessel);
		if (lv) lv->DelLightEmitter(rainLtgLight);
		rainLtgLight = NULL; rainLtgLightV = NULL; rainLtgLI = 0.0;
	}
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

// ============================================================================
// THE PAUSE-PROOF PER-FRAME TICK - and it is a keyboard callback only by address.
//
// ⚠️ ORO'S GATES USED TO FREEZE UNDER PAUSE, AND A PUBLIC-BETA TESTER FOUND IT THREE
// WAYS IN ONE REPORT: pause near the ground and fly the camera to orbit and the grey
// sky and the raindrops come with you; pause in an external view, switch to the VC, and
// the drops are inside the cabin; and the same staleness in the plasma's own VC gate.
// One cause - Orbiter.cpp's UpdateWorld() runs "if (bRunning) ModulePreStep();", so
// everything ORO senses in clbkPreStep holds its last value for as long as you stay
// paused, while the render callback happily keeps drawing from it.
//
// The core calls THIS from UserInput(), gated on the window being visible and active but
// NOT on bRunning, and before the frame is rendered. Main thread, outside the render
// pass, the same phase in which Orbiter updates its own dialogs - so oapi queries are as
// safe here as in clbkPreStep, and invariant 1 is not bent: the render callback still
// makes none.
//
// WE CONSUME NOTHING. Returning false always is deliberate and load-bearing: returning
// true would block the key from Orbiter's own processing, and ORO has no business
// swallowing input from a function it is using as a clock.
//
// KNOWN AND ACCEPTED: with the render window INACTIVE the core skips UserInput, so the
// gates hold. Nothing is moving the camera then either, and the first frame after focus
// returns puts it right.
// ============================================================================
bool OroModule::clbkProcessKeyboardImmediate(char kstate[256], bool simRunning)
{
	// The gates are cheap; when the sim IS running clbkPreStep has already called these
	// in this same frame and they are idempotent, so this is a harmless second look
	// rather than a branch that could disagree.
	SenseView();
	SenseRain();
	// ⚠️ AND THE CLIENT PUSHES HAVE TO FOLLOW THE SENSING (2026-08-24, round 2). Fixing
	// the gates alone left the raindrops correctly gone in orbit but the whole EARTH
	// grey: PushSurfaceWet multiplies every value by s_gateF, so the gate was right, but
	// the push that carries it to the client still only ran in clbkPreStep. The client
	// kept the storm light from wherever you paused - the sun collapsed at the source,
	// which from orbit is a flat grey planet. OUTPUT DERIVED FROM SENSING MUST RUN
	// WHEREVER THE SENSING RUNS; it is change-gated internally, so calling it here costs
	// nothing when nothing moved.
	PushSurfaceWet();
	SenseFog();                             // the fog's gates, same law (patch aa)
	PushFog();
	PushBaseLights();
	SenseVCNight();                         // the cabin at night, same law (patch ad)
	PushVCNight();
	return false;                           // never consume - see the note above
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
	pIPIGloom   = pCore->CreateIPInterface("Modules/ORO/orofx.hlsl", "PSGloom",  NULL, NULL);

	oapiWriteLogV("ORO: premium IPI pipeline ready - grey-out %s, blur %s, aberration %s, swim %s, tilt %s, shimmer %s, plasma %s, eclipse %s, god rays %s, gloom %s.",
	              pIPIGrey   ? "live" : "FAILED (shader compile?)",
	              pIPIBlur   ? "live" : "FAILED (shader compile?)",
	              pIPIChroma ? "live" : "FAILED (shader compile?)",
	              pIPISwim   ? "live" : "FAILED (shader compile?)",
	              pIPITilt   ? "live" : "FAILED (shader compile?)",
	              pIPIShimmer ? "live" : "FAILED (shader compile?)",
	              pIPIPlasma ? "live" : "FAILED (shader compile?)",
	              pIPIEclipse ? "live" : "FAILED (shader compile?)",
	              pIPIGodRay ? "live" : "FAILED (shader compile?)",
	              pIPIGloom  ? "live" : "FAILED (shader compile?)");
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

// ----------------------------------------------------------------------------
// THE CABIN AT NIGHT (client patch ad, 2026-09-05). MAIN thread, EVERY frame - it senses
// the world (the sun at the camera), so it runs from the keyboard tick too and stays
// true while paused (invariant 1's law). ORO renders nothing: the client scales the VC's
// ambient + emissive fill in its cockpit pass; this decides by how much.
//
// THE RAMP IS THE SUN AT THE CAMERA against the LOCAL horizon, which dips with altitude
// (sin h = -sqrt(1 - (R/r)^2)): on the ground that is the real horizon, in orbit it is
// the planet's shadow - one formula, no orbit/ground special case. With an atmosphere
// the light fades across TWILIGHT (full by +3 deg, gone by -8 deg on the ground -
// nautical twilight, when a real cockpit is dark; the band narrows with altitude because
// the air thins); without one it is a sharp flip. A solar eclipse the ECLIPSE effect is
// tracking darkens it too (the client only knows the primary's own shadow).
// ----------------------------------------------------------------------------
void OroModule::SenseVCNight()
{
	vcNightK = 1.0f;
	g_fx.vcNightLive = 1.0f;
	if (!vcNightSupported || !g_fx.vcNight || !g_fx.masterArmed) return;
	OBJHANDLE hS   = OroFindStar();
	OBJHANDLE hRef = oapiCameraProxyGbody();
	if (!hS || !hRef) return;
	VECTOR3 cam; oapiCameraGlobalPos(&cam);
	VECTOR3 pC;  oapiGetGlobalPos(hRef, &pC);
	VECTOR3 sp;  oapiGetGlobalPos(hS, &sp);
	const VECTOR3 rel = cam - pC;
	const double r = length(rel);
	const double R = oapiGetSize(hRef);
	if (r < 1.0 || R < 1.0) return;
	const VECTOR3 up = rel / r;
	const double sinE  = dotp(unit(sp - cam), up);               // sun elevation, sine
	const double ratio = (R < r) ? (R / r) : 1.0;
	double dip = 1.0 - ratio * ratio; if (dip < 0.0) dip = 0.0;
	const double sinH  = -sqrt(dip);                             // the dipped horizon
	const double e     = sinE - sinH;                            // 0 at that horizon
	double hi = 0.012, lo = -0.012;                              // vacuum: a flip
	if (oapiPlanetHasAtmosphere(hRef)) {
		const ATMCONST* ac = oapiGetPlanetAtmConstants(hRef);
		const double altLim = (ac && ac->altlimit > 1000.0) ? ac->altlimit : 100000.0;
		double f = (r - R) / altLim; f = (f < 0.0) ? 0.0 : (f > 1.0 ? 1.0 : f);   // 0 ground .. 1 above the air
		hi =  0.052 + ( 0.012 - 0.052) * f;                      // +3.0 deg -> +0.7 deg
		lo = -0.139 + (-0.017 + 0.139) * f;                      // -8.0 deg -> -1.0 deg
	}
	double x = (e - lo) / (hi - lo);
	x = (x < 0.0) ? 0.0 : (x > 1.0 ? 1.0 : x);
	double ramp = x * x * (3.0 - 2.0 * x);
	ramp *= (1.0 - 0.92 * (double)g_fx.eclipseObsc * (double)g_fx.eclipseObsc);   // totality is night
	const double fl = (double)((g_fx.vcNightFloor < 0.0f) ? 0.0f : (g_fx.vcNightFloor > 1.0f ? 1.0f : g_fx.vcNightFloor));
	// THE SKY OVERHEAD (his rain report, 2026-09-05: "the whole world has dimmed ... but the
	// VC is bright as day"). The fill stands in for the light coming in through the windows,
	// so it follows what the weather leaves of it: the storm light collapses the sun (the
	// very factor the client is running), and the fog's sun column above the camera thins
	// it - mildly, because thick fog is a BRIGHT white sky, not a dark one. Full storm at
	// noon leaves about a quarter of the fill: dim, shadowless, matching the world outside.
	// WEATHER DIM (his second test: gloom 1 was "a bit more bright than I'd like") scales
	// the darkening, not the weather: gloom and visibility still decide how much sun is
	// left, this decides how much of THAT loss the cabin shows. At 1, gloom 1 leaves ~37%
	// and a full storm ~10%; at 2 the storm is near-black; the pill off = the weather
	// leaves the cabin alone.
	const double wx     = g_fx.vcWxDimOn ? (double)((g_fx.vcWxDim < 0.0f) ? 0.0f : (g_fx.vcWxDim > 2.0f ? 2.0f : g_fx.vcWxDim)) : 0.0;
	const double storm  = (stormPushed > 0.0f) ? (double)stormPushed : 0.0;
	const double sunSin = (sinE > 0.08) ? sinE : 0.08;
	const double fogSun = exp(-(double)fogSunColumn / sunSin);
	double stormDim = storm * wx * 1.4; if (stormDim > 1.0) stormDim = 1.0;
	double fogDim   = (1.0 - fogSun) * wx * 0.25; if (fogDim > 0.6) fogDim = 0.6;
	const double sky    = (1.0 - 0.9 * stormDim) * (1.0 - fogDim);
	vcNightK = (float)(fl + (1.0 - fl) * ramp * sky);
	g_fx.vcNightLive = vcNightK;
}

// PushVCNight - wherever the sensing ran; change-gated, so it costs nothing when the sun
// has not moved. Off, disarmed or unsupported all sense as 1.0 = stock.
void OroModule::PushVCNight()
{
	if (!pCore || !vcNightSupported) return;
	if (vcNightPushed >= 0.0f && fabsf(vcNightK - vcNightPushed) < 0.004f) return;
	pCore->SetVCNightLight(vcNightK);
	vcNightPushed = vcNightK;
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
	g_fx.copMach   = 0.0f;
	g_fx.copGated  = false;
	const double d = (double)g_fx.copShift;
	if (!g_fx.masterArmed || fabs(d) < 0.001) return;   // 0 = the vessel exactly as coded
	VESSEL* v = oapiGetFocusInterface();
	if (!v) return;

	g_fx.copMach = (float)v->GetMachNumber();

	// The vertical aero force in VESSEL coordinates. GetLiftVector is documented as
	// perpendicular to the relative wind with zero x-component (side force is a
	// separate accessor), so this is the pitch-relevant force and nothing else; the
	// drag term matters because at high AoA its y-component is a large part of it -
	// and a real CoP is where lift AND drag are applied.
	VECTOR3 L = _V(0, 0, 0), D = _V(0, 0, 0);
	v->GetLiftVector(L);
	v->GetDragVector(D);
	double Fy = L.y + D.y;                              // [N]
	if (fabs(Fy) < 1.0) return;                         // vacuum / no lift - nothing to shift

	// --- THE REGIME GATE (2026-08-15) -------------------------------------
	// See g_fx.copReentryOnly. Deliberately AFTER the no-air return, so a parked or
	// orbiting ship still reads "+0.0 kNm" rather than "gated" - "gated" must mean the
	// gate is holding back a couple that would otherwise be applied, or it is just noise.
	// The couple ramps in across a band rather than switching, so decelerating out of the
	// regime hands the airframe's own stability back gradually instead of dropping the
	// nose at a threshold - the complaint Ctrl+G mid-entry already earns, and there is no
	// reason to build a second one in.
	if (g_fx.copReentryOnly) {
		const double lo = 3.0, hi = 5.0;                // fully off below lo, full above hi
		double authority = ((double)g_fx.copMach - lo) / (hi - lo);
		if (authority <= 0.0) { g_fx.copGated = true; return; }
		if (authority > 1.0) authority = 1.0;
		else                 g_fx.copGated = true;      // partial: say so in the readout
		authority = authority * authority * (3.0 - 2.0 * authority);   // smoothstep
		Fy *= authority;
		if (fabs(Fy) < 1.0) return;
	}

	// (0,0,d) x (0,Fy,0) = (-d*Fy, 0, 0). Mx>0 is nose-DOWN, so d>0 (CoP forward)
	// with positive lift is nose-UP: the knob reads the way it flies.
	v->AddForce(_V(0,  Fy, 0), _V(0, 0, d));
	v->AddForce(_V(0, -Fy, 0), _V(0, 0, 0));
	g_fx.copMoment = (float)(d * Fy * 1e-3);            // kN m, + = nose-up
}

// ----------------------------------------------------------------------------
// CANCEL THRUST - the test-stand rig (user request 2026-08-09, for plume tuning:
// the DG at full throttle rolled off the runway before the sliders got a fair
// try). Invariant 9's mechanism exactly: AddForce lives one timestep, so there
// is nothing to hand back, and disarm/Ctrl+G or the pill releases the ship on
// the next step. SESSION-ONLY by design - see the note on g_fx.cancelThrust.
// ⚠️ PHASE B RESCOPED IT (2026-08-30, his requirement): the hold nulls the
// SELECTED scope - one thruster, or the selected group at ALL - each thruster
// cancelled at its OWN position, so its force and torque die together while
// everything outside the scope (attitude control included) stays fully honest,
// the old "keeps the controls honest" rationale in stronger form. The pre-B
// whole-ship CoM null is expressible as "select the group you are firing"; what
// is no longer expressible is nulling two groups at once.
// ----------------------------------------------------------------------------
void OroModule::UpdateCancelThrust()
{
	if (!g_fx.masterArmed || !g_fx.cancelThrust) return;
	// ⚠️ SCOPED TO THE SELECTION SINCE PHASE B (2026-08-30, his requirement): the pill
	// nulls the SELECTED thruster, or the selected GROUP at ALL - not the whole ship.
	// And it cancels each thruster AT ITS OWN POSITION: AddForce(-F, ref) kills that
	// thruster's force AND its torque exactly, so a lone RCS jet under the hold moves
	// nothing at all - while every OUT-OF-SCOPE group (attitude control included)
	// stays fully honest, which is the old comment's rationale in stronger form.
	// The scope follows the selection LIVE while held; the caption names it.
	// Acts on the PANEL vessel (camera target, focus fallback) - the ship the
	// thruster pages describe and the selection indexes into.
	OBJHANDLE h = oapiCameraTarget();
	if (!h || oapiGetObjectType(h) != OBJTP_VESSEL) h = oapiGetFocusObject();
	VESSEL* v = (h && oapiGetObjectType(h) == OBJTP_VESSEL) ? oapiGetVesselInterface(h) : NULL;
	if (!v) return;
	const int grp = (g_fx.thrSel >= 0 && g_fx.thrSel < ORO_THR_N) ? g_fx.thrSel : 0;
	const DWORD n = v->GetThrusterCount();
	for (DWORD i = 0; i < n; i++) {
		THRUSTER_HANDLE th = v->GetThrusterHandleByIndex(i);
		if (!th) continue;
		if (g_fx.thrThrSel >= 0) { if ((int)i != g_fx.thrThrSel) continue; }
		else if (OroThrusterGroupOf(v, th) != grp) continue;
		const double lvl = v->GetThrusterLevel(th);
		if (lvl <= 1e-4) continue;
		VECTOR3 d; v->GetThrusterDir(th, d);
		const double L = length(d);
		if (L < 1e-9) continue;
		const double F = v->GetThrusterMax(th) * lvl;   // pressure-adjusted current max
		if (F < 0.5) continue;
		VECTOR3 ref; v->GetThrusterRef(th, ref);
		v->AddForce(d * (-(F / L)), ref);
	}
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
	// The gain scales the CLAMP as well as the slope, so turning it up genuinely raises the
	// ceiling rather than just reaching it sooner - the clamp is there to bound a monster
	// thruster, not to bound the user's taste. 1.0 is the pre-2026-08-15 behaviour exactly.
	const double pushK = (double)(g_fx.shakePush < 0.0f ? 0.0f : (g_fx.shakePush > 2.0f ? 2.0f : g_fx.shakePush));
	VECTOR3 push = feltAcc * (-SHAKE_PUSH_K * pushK);
	const double pl   = length(push);
	const double pmax = SHAKE_PUSH_MAX * pushK;
	if (pmax > 0.0 && pl > pmax) push = push * (pmax / pl);

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
	plmShimStr = 0.0f;
	plmShimWave = plmShimFreq = 1.0f;      // sane defaults when nothing contributes
	                                       //   (fShimmer 0 makes the pass a no-op anyway)
	if (!extGate || !g_fx.masterArmed) return;                    // EXTERNAL view only
	if (viewW == 0 || viewH == 0) return;
	if (plmModelN <= 0) return;                                   // nothing burning
	// ⚠️ THE STRENGTH IS ONE SHADER UNIFORM, so it cannot be per plume however much the
	// rest of this is (2026-08-16). PSShimmer warps the WHOLE FRAME by fShimmer and the
	// capsules only say WHERE - so the honest resolution is the strongest CONTRIBUTING
	// group, accumulated in the loop below. The enable and the offset ARE per group,
	// because those act per capsule and cost nothing.
	{
		bool anyOn = false;
		for (int gi = 0; gi < ORO_THR_N; gi++)
			if (g_fx.thr[gi].shimmerEnabled && g_fx.thr[gi].shimmer > 0.001f) anyOn = true;
		// Phase B: an override can enable the haze on one jet while its group is off
		// (memory-only scan - this runs in the render path).
		for (int s = 0; s < ORO_THR_OVR_MAX && !anyOn; s++) {
			const OroThrOvr& o = g_fx.thrOvr[s];
			if (o.thrIdx >= 0 && o.ovrExh && o.fx.shimmerEnabled && o.fx.shimmer > 0.001f)
				anyOn = true;
		}
		// ... and so can a CACHED foreign class in the stack (also memory-only).
		for (int ci = 0; ci < ORO_THR_CACHE_MAX && !anyOn; ci++) {
			for (int gi = 0; gi < ORO_THR_N && !anyOn; gi++) {
				const OroThrusterFx* T = OroThr_CacheGrp(ci, gi);
				if (T && T->shimmerEnabled && T->shimmer > 0.001f) anyOn = true;
			}
		}
		if (!anyOn) return;
	}

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
	// DENSITY RESPONSE (2026-09-04, his call - DENSITY, not pressure: refractivity
	// is proportional to rho (Gladstone-Dale) and the waver IS the turbulent mixing
	// layer, which needs ambient air to entrain). Two halves:
	//  - the RAMP: linear in rho to full at 0.3 kg/m3 (~9 km on Earth). The old
	//    saturation at 0.02 (~35 km) meant the last 60x of an ascent's density
	//    change did nothing - the haze never visibly thinned until it abruptly
	//    started dying in the stratosphere.
	//  - the OVERDRIVE: above EARTH-SEA-LEVEL density the response keeps growing,
	//    logarithmically, capped at 2.5x (his Venus question: the jet-vs-ambient
	//    density CONTRAST there is ~45x Earth's, but 45x a full-frame UV warp is
	//    not a look, it is a broken frame). Venus surface ~1.95x, Titan ~1.35x,
	//    Earth sea level EXACTLY 1.0 - the tuned look is the identity case.
	const float atmW = (float)(min(1.0, rho / 0.3)
	                 * min(2.5, 1.0 + 0.55 * log10(max(1.0, rho / 1.225))));

	// THE RENDER CAMERA (2026-08-15). This runs in the render path now: the capsules are
	// SCREEN-SPACE, so under pause they used to keep warping wherever the plumes were when
	// the sim stopped. Everything else it needs comes from the plume model (consumer 2).
	VECTOR3 cpos; MATRIX3 Rcam; double tanAp;
	if (!FillProjCam(cpos, Rcam, tanAp)) return;
	const double aspect = (double)viewW / (double)viewH;

	for (int p = 0; p < plmModelN && plumeCount < SHIM_PLUMES; p++) {
		const PlumeModel& e = plmModel[p];

		// Capsule from the model: root at the nozzle plus the Offset knob along
		// the flow axis; tip at the model's jet length.
		// ⚠️ RENDER-EPOCH ANCHOR (invariant 21a) - the same correction the jet applies.
		// The haze is diffuse enough to hide a 500 m offset far better than the jet does,
		// which is precisely why it would have gone unnoticed.
		// Phase B: resolve through the override pool AND the class cache (memory-only,
		// render-path legal). The strength stays one uniform; "strongest contributor"
		// now reads effective per-thruster, per-class values - the same rule, finer.
		const OroThrusterFx& T = OroThr_EffC(e.cls, e.grp, e.thrIdx, ORO_FAM_EXH);
		if (!T.shimmerEnabled || T.shimmer <= 0.001f) continue;   // this plume hazes nothing
		// THE STRONGEST CONTRIBUTOR now selects only the frame's WAVE TEXTURE
		// (wavelength + churn live inside the shared phase math and stay frame
		// uniforms). STRENGTH stopped being a frame uniform on 2026-09-04, his
		// call: each capsule folds its own block's strength below, so a weak
		// hover haze and a strong main haze coexist honestly in one frame.
		if (T.shimmer > plmShimStr) {
			plmShimStr  = T.shimmer;
			plmShimWave = T.shimmerWave;
			plmShimFreq = T.shimmerFreq;
		}

		const VECTOR3 rootR = e.rootG + RenderEpochShift(e.hOwn, e.ownCg);
		VECTOR3 groot = rootR + e.dirG * T.shimmerOfs;
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
		// PER-PLUME STRENGTH FOLD (2026-09-04, his call): the dialog strength rides
		// each capsule's own vPlumeP slot instead of a frame master - the shader's
		// contribution is linear in s, so with ONE group active this is arithmetic-
		// identical to the old fShimmer multiply, and with several it is the fix
		// (a weak group's capsule used to warp at the strongest group's strength).
		// Clamped here because the old path saturate()d the master in the shader
		// and a hand-edited cfg above 1.0 must not overdrive the warp now.
		s.str = atmW * (float)(vis * pow(e.level, 0.40))
		      * min(1.0f, max(0.0f, T.shimmer));
		s.hpk = (float)(0.15 + 0.45 * e.level);
	}
	for (int i = plumeCount; i < SHIM_PLUMES; i++) plumes[i] = PlumeScr{};   // unused: str = 0
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
// THE PROJECTION CAMERA (2026-08-15, the pause fix). See the comment block on
// ProjCam in OroModule.h for why this exists at all.
//
// SnapPreStepCam runs on the main thread and is the FALLBACK only. FillProjCam runs
// in the RENDER PATH and prefers patch (k)'s render camera, which is the camera the
// frame is actually being drawn with - and, while the sim is PAUSED, the only camera
// that is still changing at all.
//
// INVARIANT-1 AUDIT for FillProjCam: no oapi calls. gcCore::GetRenderCam is a client
// call, the same class as CopyResource / GetBackBufferHandle, both of which have run
// mid-render since patch (b).
// ----------------------------------------------------------------------------
void OroModule::SnapPreStepCam()
{
	oapiCameraGlobalPos(&preStepCam.pos);
	oapiCameraRotationMatrix(&preStepCam.rot);
	preStepCam.tanAp = tan(oapiCameraAperture());
	preStepCamValid  = true;
}

bool OroModule::FillProjCam(VECTOR3& pos, MATRIX3& rot, double& tanAp)
{
	if (pCore && pCore->CanGetRenderCam() && pCore->GetRenderCam(&pos, &rot, &tanAp)) {
		// THE WET MIRROR'S ONE RECONCILIATION (patch u). The client reports a PURE planar
		// mirror, so its basis is left-handed; the reflection RT, meanwhile, holds a
		// horizontally flipped image, because the pass draws its meshes through an extra
		// clip-space X flip to keep their winding legal and the ground shaders undo that
		// when they sample. Negating the camera's RIGHT column does both jobs at once -
		// it restores a proper right-handed rotation AND puts our screen X in the RT's
		// convention - which is why the whole reconciliation is three lines here rather
		// than a second projection path. VERIFIED IN THE SIM: with the panel reporting
		// both builds' projected root, the mirror's landed within 3 px of
		// (RT width - direct root / 2) - so this is the whole reconciliation, and the
		// two rounds spent suspecting it were spent in the wrong place.
		if (wetMirrorPass) {
			rot.m11 = -rot.m11; rot.m21 = -rot.m21; rot.m31 = -rot.m31;
		}
		return true;
	}
	if (preStepCamValid) {
		pos = preStepCam.pos; rot = preStepCam.rot; tanAp = preStepCam.tanAp;
		return true;
	}
	return false;   // before the first pre-step: nothing to project against
}

// The anchor half. See the comment on the declaration - a render camera paired with a
// pre-step anchor is off by one step of the BODY's barycentric motion, ~500 m for Earth
// at 60 fps, and it jitters with frame pacing rather than sitting still.
// ----------------------------------------------------------------------------
// THE CABIN WASH'S PROJECTION (2026-08-25) - the last member of H1's family.
//
// PSPlasma paints a directional glare lobe centred on where the fire actually is, plus a
// flat term for the light bouncing round the cabin (invariant 27i). The lobe's centre used
// to be computed in UpdateReentry, on the main thread - which does not run while PAUSED,
// so pausing and looking around left the glare nailed to the screen where the plasma had
// been. Nobody reported it in ten days of public beta because a soft broad bloom reads far
// less obviously out of place than the sheath did, but it is the same defect.
//
// Two things it must get right, both already law elsewhere in this file:
//  - THE CAMERA is the RENDER camera (patch k), not the pre-step one, or the fix would
//    only be half a fix: under pause the pre-step camera is the frozen one.
//  - THE ANCHOR is the RENDER-EPOCH position (patch k2). Pairing a render camera with a
//    pre-step vessel position is invariant 21(a)'s first trap, and at cockpit range it
//    would be catastrophic rather than subtle - one frame of Earth's barycentric motion
//    is ~500 m against a glow point a few metres from the eye.
// Makes no oapi call: FillProjCam and RenderEpochShift are both client calls (invariant 1).
// ----------------------------------------------------------------------------
void OroModule::UpdatePlasmaWashUV()
{
	if (!plasmaGlowValid || viewW == 0 || viewH == 0) return;
	VECTOR3 cpos; MATRIX3 Rcam; double tanAp;
	if (!FillProjCam(cpos, Rcam, tanAp)) return;      // keep the previous UV rather than lie
	const VECTOR3 G = plasmaGlowG + RenderEpochShift(plasmaGlowV, plasmaGlowCg);
	const VECTOR3 c = tmul(Rcam, G - cpos);
	if (c.z < 0.1) {
		// Behind the camera: keep the ambient lift, park the lobe well off-screen so only
		// the flat wash remains. Same behaviour the old main-thread version had.
		plasmaUV[0] = 0.5f; plasmaUV[1] = 2.5f;
		return;
	}
	const double aspect = (double)viewW / (double)viewH;
	plasmaUV[0] = (float)(0.5 + 0.5 * ((c.x / c.z) / (tanAp * aspect)));
	plasmaUV[1] = (float)(0.5 - 0.5 * ((c.y / c.z) /  tanAp));   // UV y grows downward
}

VECTOR3 OroModule::RenderEpochShift(OBJHANDLE h, const VECTOR3& bodyCentrePreStep)
{
	if (h && pCore && pCore->CanGetRenderObjPos()) {
		VECTOR3 rp;
		if (pCore->GetRenderObjPos(h, &rp)) return rp - bodyCentrePreStep;
	}
	return _V(0, 0, 0);
}

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
	if (extGate) {
		BuildVapourGeometry();     // render-path since 2026-08-15 (the pause fix)
		if (vapActive) DrawVapourPoly(pSkp);
	}
	// RAIN - alpha-blended, so it goes with the cone and before every additive layer
	// below. EXTERNAL, and since 2026-08-23 the VIRTUAL COCKPIT too (his ask): in the
	// VC the patch-(g) clip cuts every streak, splash and bolt at the window frame
	// per pixel - the aurora's mechanism, the scene depth includes the cockpit.
	// rainVC embeds depthClipOK, so a depthless client keeps the VC dry rather than
	// painting drops over the cabin.
	if (extGate || rainVC || rainPanel) {
		BuildRainGeometry();
		if (rainActive) DrawRainPoly(pSkp);
	}
	if (extGate || vcGate) {
		ProjectTrail();
		// TWO VIEWPOINTS, TWO TECHNIQUES (2026-08-20). The geometric draw list is built
		// from a point field ON THE SKIN and starves once the camera is inside the hull
		// (measured: 49/417 points, 56 triangles), which is what made the cockpit look
		// faceted. Inside, the honest thing to draw is the luminous sheath a pilot
		// actually sees - see BuildVCGlow. External is untouched.
		if (vcGate) BuildVCGlow(); else BuildPlasmaGeometry();
		DrawTrailPoly(pSkp, /*depthClip=*/true);
		DrawPlasmaPoly(pSkp, /*depthClip=*/true);
	}
	// PLUME EXPANSION - external, and since 2026-08-29 the VIRTUAL COCKPIT too: RCS
	// made "your own engines are behind the cockpit" false (the OMS pods sit outside
	// the aft windows), and plumeVC embeds the patch-(g) clip so the jet cuts at the
	// window frame instead of painting over the cabin.
	// Pre-bloom is exactly where the diamond cores want to composite: the
	// fp16 chain accumulates them past 1.0 and the client's threshold bloom whitens
	// them (the Firefly law) - and the shimmer's resample runs later in DrawOverlay,
	// so the diamonds ripple through their own heat haze, which is physically right.
	if (extGate || plumeVC) {
		UpdatePlumeFx();     // render-path since 2026-08-15 (the pause fix)
		DrawPlumePoly(pSkp, /*depthClip=*/true);
	}
}

// ----------------------------------------------------------------------------
// THE WET-MIRROR SLOT (client patch u, 2026-08-25) - ORO's own plume, in the standing
// water. It fires INSIDE the client's planar-reflection pass, after the mirrored hulls,
// exhaust billboards, beacons and particle streams and before the target is popped.
//
// WHY IT NEEDS A SLOT AT ALL, when all of those needed nothing: every one of them is
// geometry the CLIENT draws, oriented from a camera-relative position and pushed through
// the view-projection the pass already overrides - so a matrix-only mirror carried them
// for free. Ours is screen-space Sketchpad triangles projected on the CPU and drawn after
// the whole scene, in the pre-resolve slot; by then this pass is long over. What it
// reflects in practice for anyone running ORO is the JET - patch (n) has already
// suppressed the stock billboards for them, so the client's own exhaust loop in there
// draws nothing and only the contrail survives.
//
// IT IS A PARAMETERISATION, NOT A SECOND RENDERER, and that is the whole design:
//  - THE CAMERA needs no argument. For the duration of this call the client reports the
//    MIRRORED camera through GetRenderCam, so FillProjCam - which UpdatePlumeFx already
//    calls, at exactly one place - returns it and every screen-space offset in ~1000
//    lines recomputes itself. One camera path, not two to keep in sync.
//  - THE VIEWPORT comes out of the PAD, via the stock Sketchpad virtual
//    GetRenderSurfaceSize (D3D9Pad answers from its bound target's descriptor). The
//    reflection target is HALF RESOLUTION and ORO must never assume that: hardcoding
//    "half" would couple us to a client implementation detail that could be retuned for
//    performance at any time, and the pad already knows the answer. Not an oapi call
//    (invariant 1), just a member read.
//
// THE BUFFER IS REUSED, NOT DOUBLED. This pass runs BEFORE the main scene, so the
// mirrored build is drawn and finished by the time DrawPreResolve rebuilds the same
// vertex buffer for the real view later in the same frame. Both are full-buffer updates,
// so invariant 3 holds unchanged; the cost is one extra ~1000-triangle CPU build and one
// half-res draw, and only while the ground is wet and the camera is under 250 m AGL.
//
// ⚠️ NO DEPTH CLIP HERE, and it is not an oversight. Patch (g) clips against
// ptgBuffer[GBUF_DEPTH], which was filled from the MAIN camera in
// RENDERPASS_NORMAL_DEPTH - inside a mirrored pass those depths describe different
// geometry at every pixel, so the clip would cut the reflection against a scene that is
// not the one being drawn. Without it the jet paints over the mirrored hulls, which is
// the pre-patch-(g) trade and entirely invisible: this image only ever reaches the eye
// through the puddle lattice, rippled, Fresnel-masked and blurred.
//
// ⚠️ EXTERNAL ONLY, deliberately, and it is the one thing here worth a second opinion.
// invariant 10 makes the plume external-only because your own engines are behind the
// cockpit - reasoning that does NOT transfer to a reflection, since a puddle ahead of the
// nose is in plain view from the VC. Left matching every other plume path for now rather
// than widening the invariant unasked; it is this one line if he wants it.
// ----------------------------------------------------------------------------
void OroModule::DrawWetMirror(oapi::Sketchpad* pSkp)
{
	// Latch FIRST and unconditionally - the preResolveLive rule. This firing at all is
	// the only proof the running client HAS the slot; a pre-(u) client accepts the
	// registration and silently never calls it.
	wetMirrorLive = true;

	// extGate OR plumeVC (2026-08-29): the reflection is visible through the VC
	// windows too, and since the jet now draws in the VC its mirror image must
	// keep up - a mirror missing only OUR plume would be the (w) lesson inverted.
	if (!pSkp || !g_fx.masterArmed || !(extGate || plumeVC)) return;

	SIZE sz = { 0, 0 };
	pSkp->GetRenderSurfaceSize(&sz);          // the reflection RT, not the frame
	if (sz.cx <= 0 || sz.cy <= 0) return;

	wetMirrorPass = true;                     // FillProjCam reconciles the mirror's X
	UpdatePlumeFx((DWORD)sz.cx, (DWORD)sz.cy);
	wetMirrorPass = false;                    // down before anything else projects
	// ⚠️ writeAlpha, and it is the difference between the effect working and looking
	// half-broken. This target's ALPHA is the ground shaders' reflection mask; patch
	// (d)'s additive path masks alpha off, which is correct on the backbuffer and
	// wrong here. Without it the jet only survives where it overlaps the hull's own
	// alpha - a stub at the tail rather than a plume.
	DrawPlumePoly(pSkp, /*depthClip=*/false, /*writeAlpha=*/true);
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
		DrawGloomPass();   // the overcast, before anything is drawn into the frame

		// GOD RAYS second: still world illumination, but they ADD light to the frame
		// the eye has just decided the brightness of, so they must follow the eclipse
		// rather than precede it (a shaft does not dim because you are adapting - it is
		// part of what you are adapting TO).
		BuildGodRayScreen();   // render-path since 2026-08-15 (the pause fix)
		DrawGodRayPass();

		// CONSUMER 2 of the plume model - the heat-haze capsules, projected HERE since
		// 2026-08-15 so a paused pan does not leave the warp behind (see ProjCam).
		UpdateShimmerPlumes();
		if (ipiReady && pCore && hFrameTex && pIPIShimmer && plumeCount > 0) {
			SURFHANDLE hBB = pCore->GetBackBufferHandle();
			if (hBB && pCore->CopyResource(hFrameTex, hBB)) {
				// Plume table -> shader arrays (SHIM_PLUMES entries - the shimmer keeps its
				// strongest-6 contract while the jet pool is 16; see MAX_PLUMES).
				float axes[SHIM_PLUMES * 4], prm[SHIM_PLUMES * 4];
				for (int i = 0; i < SHIM_PLUMES; i++) {
					axes[i * 4 + 0] = plumes[i].ax; axes[i * 4 + 1] = plumes[i].ay;
					axes[i * 4 + 2] = plumes[i].bx; axes[i * 4 + 3] = plumes[i].by;
					prm[i * 4 + 0]  = plumes[i].rad; prm[i * 4 + 1] = plumes[i].str;
					prm[i * 4 + 2]  = plumes[i].hpk; prm[i * 4 + 3] = 0.0f;
				}
				pIPIShimmer->SetTexture("tSrc", hFrameTex, IPF_CLAMP_U | IPF_CLAMP_V | IPF_LINEAR);
				pIPIShimmer->SetOutput(0, hBB);
				// fShimmer is GONE (2026-09-04): strength rides each capsule's own
				// vPlumeP slot now - only the wave texture is still frame-wide,
				// from the strongest contributor's block.
				pIPIShimmer->SetFloat("fShimWave", plmShimWave);
				pIPIShimmer->SetFloat("fShimFreq", plmShimFreq);
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
			BuildPlasmaGeometry();     // render-path since 2026-08-15 (the pause fix)
			UpdatePlumeFx();           //   "
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
		// does not matter. BUILT HERE since 2026-08-15 (was clbkPreStep): that callback
		// does not run while PAUSED, so the curtains used to stay projected for whatever
		// camera existed when the sim stopped and appeared to follow the ship.
		BuildAuroraGeometry();
		if (aurActive) DrawAuroraPoly(pSkp);

		// LIGHTNING - flash discs in the cloud deck, additive like the aurora and
		// drawn beside it for the same reasons (order between additive layers is
		// cosmetic; both are light sources the eclipse's eye need not protect).
		BuildLightningGeometry();
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
	// The RAIN's fallback rides the same condition, alpha FIRST (G11's order):
	// on a patch-(i) client both drew in DrawPreResolve already.
	if (rainVC && !preResolveLive) {
		BuildRainGeometry();
		if (rainActive) DrawRainPoly(pSkp);
	}
	if (vcGate && !preResolveLive) {
		ProjectTrail();
		BuildVCGlow();             // the cockpit's own technique - see the pre-resolve slot
		DrawTrailPoly(pSkp, /*depthClip=*/true);
		DrawPlasmaPoly(pSkp, /*depthClip=*/true);
	}
	// PLUME in the VC - the fallback slot, same rule as the plasma above (the
	// pre-resolve slot draws it on a patch-(i) client). See plumeVC.
	if (plumeVC && !preResolveLive) {
		UpdatePlumeFx();
		DrawPlumePoly(pSkp, /*depthClip=*/true);
	}

	// --- AURORA through the cockpit windows (patch g) --------------------
	// Now that the curtains are depth-clipped against the scene (depthClipOK), they draw in
	// the VC too - occluded by the cockpit frame per pixel, visible through the glass. Drawn
	// BEFORE the resample stack like the VC plasma, so blur/grey-out/eclipse treat them as
	// sky. Gated on depthClipOK: without real depth this would paint the cabin, so it stays
	// external (UpdateAurora built nothing for an internal view in that case anyway).
	BuildAuroraGeometry();
	if (aurActive && depthClipOK) DrawAuroraPoly(pSkp);

	// --- LIGHTNING through the cockpit windows (patch g) ------------------
	// Same rule as the aurora directly above: with per-pixel depth the flash discs
	// sit behind the frame and glass; without it UpdateLightning built nothing for
	// an internal view. No cabin illumination in v1 - this is only the world,
	// visible out the window.
	BuildLightningGeometry();
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
		DrawGloomPass();   // the overcast, before anything is drawn into the frame

		// GOD RAYS - world light too, and they come through the window like anything
		// else out there, so the physiological stack below treats them as scenery. After
		// the eclipse for the reason given at the external call site.
		BuildGodRayScreen();   // render-path since 2026-08-15 (the pause fix)
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
				pIPIPlasma->SetFloat("fCabin", g_fx.plasCabin);
				// THE FLASH LIGHTS THE CABIN (2026-08-20 round 3). Same envelope, same
				// frame, as the sheath outside the glass - see PlasmaFlashNow. It is
				// passed SEPARATELY rather than folded into fPlasma because the shader
				// saturate()s that one: with heat x trim already several times over 1.0
				// a multiplier there would be swallowed whole and the cabin would never
				// flicker. Gated on the VC glow being on, so switching the sheath off
				// does not leave the cockpit strobing with nothing to explain it.
				pIPIPlasma->SetFloat("fFlash",
					(g_fx.plasVCGlow > 0.001f) ? PlasmaFlashNow() : 1.0f);
				UpdatePlasmaWashUV();   // render-path projection (2026-08-25) - see below
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

// RAINSURFACES free wrappers - the dialog speaks through these (declared in
// OroState.h); the members live beside the pick thunk above.
bool OroRs_PickAvail()      { return g_oro && g_oro->RsPickAvail(); }
bool OroRs_PickArm(bool on) { return g_oro && g_oro->RsPickArm(on); }
bool OroRs_ApplyAvail()     { return g_oro && g_oro->RsApplyAvail(); }
bool OroRs_ApplyNow()       { return g_oro && g_oro->RsApplyNow(); }
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
