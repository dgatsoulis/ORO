// ==============================================================
// OroParticles.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - EXHAUST PARTICLES: Orbiter's own streams, under live user control
// (2026-08-09, the second design - see the note at the bottom on the first)
// ----------------------------------------------------------------------------
// THE IDEA, in the user's words: "give the users the controls they'd have in
// the code, but with live sliders and buttons... stay within Orbiter's
// limitations for now." So this draws NOTHING. It is the invariant-18 category
// again - ORO controlling something it does not render - except the thing
// being driven is the CORE's particle system rather than the client's.
//
// WHAT THE API ACTUALLY GIVES US, since it decided the whole tab:
//   PARTICLESTREAMSPEC is { flags, srcsize, srcrate, v0, srcspread, lifetime,
//   growthrate, atmslowdown, ltype, levelmap+lmin/lmax, atmsmap+amin/amax, tex }.
//   - There is NO width/length. A particle is a ROUND sprite with ONE srcsize
//     [m] at birth plus a growthrate [m/s]. (The user asked; that is the answer.)
//   - There is NO COLOUR FIELD. Colour comes entirely from the particle TEXTURE
//     plus the EMISSIVE/DIFFUSE lighting flag. A colour picker is therefore a
//     TEXTURE SYNTHESIS problem, which patch (l)'s UpdateTexture2D solves - see
//     BakeParticleTex. Without patch (l) the swatch greys out and the stock
//     texture is used: degraded, not broken.
//
// THREE FINDINGS FROM THE SOURCE THAT SHAPE THE IMPLEMENTATION:
//
// (1) THE CORE COPIES THE SPEC AT CONSTRUCTION - `D3D9ParticleStream::SetSpecs`
//     unpacks every field into members (the inline D3D7 client does the same).
//     Mutating our PARTICLESTREAMSPEC afterwards does nothing at all. So a
//     slider change means DELETE AND RE-ADD the streams. That is cheap and it
//     does not flicker: "a deleted particle stream will no longer emit
//     particles, but existing particles persist until they expire", so the old
//     puffs drain away while the new spec starts emitting. Pushed ON CHANGE
//     (invariant 18), never per frame - the settings signature below is what
//     makes that test exact.
//
// (2) ⚠ `AddParticleStream` IS DEAD UNDER D3D9CLIENT. `clbkCreateParticleStream`
//     is unimplemented - it logs "UnImplemented Feature Used" and returns NULL
//     (D3D9Client.cpp:1383). Only the exhaust and reentry factories are real. The
//     first version of this file used AddParticleStream precisely because plain
//     ParticleStreams escape patch (n)'s gate, and it would have produced exactly
//     nothing, with a misleading "streams off in Launchpad?" caption to explain
//     it. So our streams are ExhaustStreams like everyone else's - which means
//     patch (n) suppresses OURS along with stock's, and patch (o) exists to say
//     "not this one": gcCore::ExemptStream marks a stream immune to the
//     suppression, so STOCK EXHAUST off leaves stock's billboards and streams
//     dead and ours alive. Without patch (o) the tab still works with stock
//     exhaust ON (ours simply adds to the vessel's own) and the caption says so.
//     Using AddExhaustStream also means the core drives the level from the
//     thruster itself - no level pointer of ours, one less thing to own.
//
// (3) ⚠ THE PARTICLE TEXTURE IS A 2x2 ATLAS OF FOUR VARIANTS. `Particle.cpp`'s
//     tu/tv tables address quadrants at 0.0/0.5/1.0 and each particle picks one
//     of eight quadrant-and-rotation combinations at random. A single centred
//     blob filling the texture would render as four CORNER WEDGES. BakeParticleTex
//     therefore lays down four independent puffs, one per quadrant.
//
// BORROW AND RETURN (invariant 14). Every stream we add to someone else's
// vessel is handed back: pill off, disarm/Ctrl+G, spec change, vessel change,
// clbkDeleteVessel, simulation end, destructor.
// ⚠ SLOT ADDRESSES ARE LOAD-BEARING, exactly as they are for the reentry table:
// AddParticleStream binds the core to `&s.lvl`, so the stream array is never
// moved, compacted or reordered while a stream is live.
//
// ----------------------------------------------------------------------------
// THE FIRST DESIGN, SHELVED THE SAME DAY (OroParticlesSprites.cpp.shelved).
// A custom analytic-motion sprite system on patch-(l) textured quads - ground
// impingement billows, a shed wake, and one ambient-density blend carrying a
// sea-level pad cloud, Mars dust and the ballistic Apollo ejecta sheet. It was
// built, it compiled and it was never flown: the user redirected to this,
// smaller, shippable design for a closed beta. The file is kept unbuilt because
// its header carries the whole design and the reasoning behind it (the analytic
// motion law that answers G10, and the co-rotating planet-relative frame that
// invariant 21a's inertial anchor gets wrong for anything near a surface).
// "At some point later we will revisit."
// ============================================================================

#include "OroModule.h"
#include "OroState.h"
#include "gcCoreAPI.h"       // patch (l): UpdateTexture2D, for the colour picker
#include <math.h>
#include <string.h>

namespace {

	// One 256^2 texture = a 2x2 grid of 128^2 particle variants (finding 3).
	const int PT_DIM = 256, PT_HALF = 128;
	DWORD s_ptex[PT_DIM * PT_DIM];

	inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }

	inline float phash(int x, int y, int s)
	{
		int n = x * 374761393 + y * 668265263 + s * 1274126177;
		n = (n ^ (n >> 13)) * 1274126177;
		return (float)((n ^ (n >> 16)) & 0xFFFF) / 65535.0f;
	}
	float pnoise(float x, float y, int s)
	{
		const int xi = (int)floorf(x), yi = (int)floorf(y);
		const float xf = x - xi, yf = y - yi;
		const float u = xf * xf * (3.0f - 2.0f * xf), v = yf * yf * (3.0f - 2.0f * yf);
		const float a = phash(xi, yi, s),     b = phash(xi + 1, yi, s);
		const float c = phash(xi, yi + 1, s), d = phash(xi + 1, yi + 1, s);
		const float ab = a + (b - a) * u, cd = c + (d - c) * u;
		return ab + (cd - ab) * v;
	}

	// FOUR soft puffs, one per quadrant (finding 3). RGB carries the user's tint,
	// ALPHA the soft ragged mask - which is the only channel the EMISSIVE path
	// really trades on, and the shape channel for DIFFUSE too.
	void BakeParticleTex(DWORD colourRef)
	{
		const int tr = (int)( colourRef        & 0xFF);
		const int tg = (int)((colourRef >>  8) & 0xFF);
		const int tb = (int)((colourRef >> 16) & 0xFF);
		for (int q = 0; q < 4; q++) {
			const int ox = (q & 1) * PT_HALF, oy = (q >> 1) * PT_HALF;
			const int sd = q * 613 + 7;
			for (int y = 0; y < PT_HALF; y++) {
				for (int x = 0; x < PT_HALF; x++) {
					const float nx = ((float)x + 0.5f) / (PT_HALF * 0.5f) - 1.0f;
					const float ny = ((float)y + 0.5f) / (PT_HALF * 0.5f) - 1.0f;
					float r = sqrtf(nx * nx + ny * ny);
					// Ragged rim: four variants that do not read as four copies.
					r *= 1.0f + 0.30f * (pnoise(nx * 1.8f + 3.0f, ny * 1.8f - 1.0f, sd) - 0.5f);
					float a = clampf((0.92f - r) / 0.66f, 0.0f, 1.0f);
					a = a * a * (3.0f - 2.0f * a);
					a *= 0.62f + 0.55f * pnoise(nx * 3.1f - 6.0f, ny * 3.1f + 4.0f, sd + 37);
					if (a > 1.0f) a = 1.0f;
					const float lum = 0.78f + 0.22f * pnoise(nx * 2.4f + 1.1f, ny * 2.4f - 3.3f, sd + 71);
					const int A = (int)(a * 255.0f);
					const int R = (int)clampf(tr * lum, 0.0f, 255.0f);
					const int G = (int)clampf(tg * lum, 0.0f, 255.0f);
					const int B = (int)clampf(tb * lum, 0.0f, 255.0f);
					s_ptex[(oy + y) * PT_DIM + (ox + x)] =
						((DWORD)A << 24) | ((DWORD)R << 16) | ((DWORD)G << 8) | (DWORD)B;
				}
			}
		}
	}

	// The applied-settings signature. Every field that would need the streams
	// rebuilt goes in; the LEVEL does not, because that is a live pointer.
	struct PrtSig {
		OBJHANDLE hV;
		int    nStream;
		float  ofs, size, life, rate, speed, spread, growth, slow;
		bool   diffuse, airfade;
		DWORD  colour;
		bool   on;
		bool operator!=(const PrtSig& o) const { return memcmp(this, &o, sizeof(PrtSig)) != 0; }
	};
	PrtSig s_applied = {};
	bool   s_haveApplied = false;

	// ⚠ REBUILD THROTTLE, and it is not politeness - it is required.
	// `Vessel::DelExhaustStream` calls Detach() and then deliberately "leave[s] it to
	// the scene to delete it once all particles have expired" (Vessel.cpp:2309). A
	// deleted stream therefore LINGERS, holding its particles, for up to `lifetime`
	// seconds. Rebuilding once per frame while a slider is dragged would strand ~24
	// orphan streams every frame - well over a thousand a second, each alive for as
	// long as ten seconds at the top of the Lifetime range.
	// So: rebuild once the settings have been STABLE for a beat (a drag produces no
	// rebuilds at all until the hand stops), with a slower ceiling so a long
	// continuous drag still previews instead of going dead.
	const double PRT_SETTLE  = 0.25;    // s of no change before applying
	const double PRT_MAX_GAP = 0.60;    // s: preview at least this often while dragging
	PrtSig s_pending = {};
	bool   s_havePending = false;
	double s_pendSince = 0.0, s_lastBuild = -1e9;
}

// ----------------------------------------------------------------------------
// MAIN THREAD (clbkPreStep). Decide the stream set, rebuild it when the spec
// changes, and drive every live stream's level from its thruster.
// ----------------------------------------------------------------------------
void OroModule::UpdateParticles(double simdt)
{
	// THE TWO PILLS ANSWER ONE QUESTION, so they can never both be on: stock's
	// streams or ours, not both stacked. The dialog enforces it on click, but it must
	// hold here too - settings arrive from three scopes, and a class cfg written
	// before the split carries no StockParticlesOn key at all, so it would load as
	// "stock on" beside a saved "ours on". Ours wins: it is the one the user just
	// asked for by enabling it. Runs before UpdateStockExhaust, which reads the flag.
	if (g_fx.prtEnabled) g_fx.stockParticles = false;

	// ⚠ NOT UNTIL THE SCENE HAS DRAWN A FRAME. AddExhaustStream is the one thing ORO
	// does that hands a LONG-LIVED OBJECT to Orbiter's core and the client's scene:
	// Vessel::AddExhaustStream grows the vessel's contrail[] with
	// `new oapi::ParticleStream*[ncontrail+1]` and D3D9Client::clbkCreateExhaustStream
	// does `scene->AddParticleStream(es)`. On a scenario RELOAD, clbkPreStep starts
	// running while Scene::RenderMainScene is still bailing out at "no focus visual" -
	// measured at well over a second in the user's log - so doing this there reaches
	// into a world that is only half built. The reload CTD is an allocation failure
	// (std::bad_array_new_length) out of exactly that `new[]`, which is what a garbage
	// ncontrail produces.
	// sceneRendered is set by DrawOverlay and cleared at every session start, so this
	// costs one bool and delays the streams by a few frames on a normal load.
	if (!sceneRendered) return;

	// ---- who should be streaming right now? --------------------------------
	// The camera-target vessel and everything docked or attached to it - the same
	// stack BuildPlumeModel walks, so a Shuttle's SRBs get streams too.
	OBJHANDLE stack[STACK_MAX];
	int nStack = 0;
	bool want = g_fx.masterArmed && g_fx.prtEnabled;
	if (want) {
		OBJHANDLE h = oapiCameraTarget();
		if (!h || oapiGetObjectType(h) != OBJTP_VESSEL) h = oapiGetFocusObject();
		if (h && oapiGetObjectType(h) == OBJTP_VESSEL) {
			stack[nStack++] = h;
			for (int s = 0; s < nStack; s++) {
				VESSEL* sv = oapiGetVesselInterface(stack[s]);
				if (!sv) continue;
				auto addV = [&](OBJHANDLE hn) {
					if (!hn || nStack >= STACK_MAX) return;
					if (oapiGetObjectType(hn) != OBJTP_VESSEL) return;
					for (int q = 0; q < nStack; q++) if (stack[q] == hn) return;
					stack[nStack++] = hn;
				};
				const UINT nd = sv->DockCount();
				for (UINT d = 0; d < nd; d++) addV(sv->GetDockStatus(sv->GetDockHandle(d)));
				for (int par = 0; par < 2; par++) {
					const DWORD na = sv->AttachmentCount(par != 0);
					for (DWORD a = 0; a < na; a++)
						addV(sv->GetAttachmentStatus(sv->GetAttachmentHandle(par != 0, a)));
				}
			}
		} else want = false;
	}

	// ---- the texture (patch l), rebaked only when the colour changes --------
	if (!prtTexTried && pCore) {
		prtTexTried = true;
		if (pCore->CanDrawTexPoly())        // same two bound pointers UpdateTexture2D needs
			// OAPISURFACE_ALPHA IS LOAD-BEARING. Without it D3D9Surface picks
			// D3DFMT_X8R8G8B8 (D3D9Surface.cpp:322) - NO ALPHA CHANNEL - and both
			// particle techniques shape the sprite with `color.a * gMix` over
			// SrcAlpha/InvSrcAlpha blending. An alpha-less texture therefore renders
			// every particle as a fully opaque QUAD: the "particles are squares" bug.
			// (The lightning atlas gets away without it only because it draws
			// additive, where black RGB contributes nothing and alpha is ignored.)
			hPrtTex = oapiCreateSurfaceEx(PT_DIM, PT_DIM,
			                              OAPISURFACE_TEXTURE | OAPISURFACE_NOMIPMAPS |
			                              OAPISURFACE_ALPHA);
		prtTexMode = (hPrtTex != NULL);
		oapiWriteLogV("ORO: particle tinting (patch l) %s.",
		              prtTexMode ? "available - synthesized 2x2 particle atlas"
		                         : "NOT available - stock particle texture, colour pick disabled");
		// Patch (o) decides whether our streams survive STOCK EXHAUST being turned
		// off. Without it the tab still works, but only alongside stock's streams -
		// a degradation that is invisible on screen, so it names itself here.
		oapiWriteLogV("ORO: stream exemption (patch o) %s.",
		              (pCore && pCore->CanExemptStream())
		                ? "bound - ORO streams survive stock-exhaust suppression"
		                : "NOT available - ORO streams die with stock's when suppressed");
	}

	// ---- rebuild only when something the core copied has changed ------------
	PrtSig sig = {};
	sig.hV      = nStack ? stack[0] : NULL;
	sig.ofs     = g_fx.prtOffset;
	sig.size    = g_fx.prtSize;
	sig.life    = g_fx.prtLifetime;
	sig.rate    = g_fx.prtRate;
	sig.speed   = g_fx.prtSpeed;
	sig.spread  = g_fx.prtSpread;
	sig.growth  = g_fx.prtGrowth;
	sig.slow    = g_fx.prtSlowdown;
	sig.diffuse = g_fx.prtDiffuse;
	sig.airfade = g_fx.prtAirFade;
	sig.colour  = g_fx.prtColour;
	sig.on      = want;

	// Count the qualifying thrusters too: a staging event changes the set without
	// changing any slider, and the streams must follow the hardware.
	int nWantStream = 0;
	if (want) {
		const THGROUP_TYPE grp[3] = { THGROUP_MAIN, THGROUP_RETRO, THGROUP_HOVER };
		for (int s = 0; s < nStack; s++) {
			VESSEL* sv = oapiGetVesselInterface(stack[s]);
			if (!sv) continue;
			for (int g = 0; g < 3; g++) nWantStream += (int)sv->GetGroupThrusterCount(grp[g]);
		}
		if (nWantStream > PRT_MAX_STREAM) nWantStream = PRT_MAX_STREAM;
	}
	sig.nStream = nWantStream;

	if (s_haveApplied && !(sig != s_applied)) {
		s_havePending = false;
		return;                                    // settled: the core drives the levels
	}

	// Dirty. Wait for the hand to settle (see the throttle note above) - but never
	// let a long drag go more than PRT_MAX_GAP without showing something.
	if (!s_havePending || (sig != s_pending)) {
		s_pending = sig;
		s_havePending = true;
		s_pendSince = animT;                       // REAL time: this is a UI cadence,
	}                                              //   not a physical one (invariant 4)
	// Two changes are never deferred: STOPPING (pill off, Ctrl+G - invariant 18c wants
	// the borrow handed back at once) and a VESSEL CHANGE (the streams belong to a
	// vessel we are no longer looking at). Neither can repeat frame after frame, so
	// neither can churn.
	const bool urgent = !sig.on || (s_haveApplied && sig.hV != s_applied.hV);
	if (!urgent && (animT - s_pendSince) < PRT_SETTLE && (animT - s_lastBuild) < PRT_MAX_GAP)
		return;
	s_lastBuild   = animT;
	s_havePending = false;

	ReleaseParticles("rebuild");                    // hand back the old set (invariant 14)
	s_applied = sig;
	s_haveApplied = true;
	if (!want || !nWantStream) {
		strcpy_s(g_fx.prtInfo, want ? "no main/hover/retro thrusters on this vessel" : "off");
		g_fx.prtCount = 0;
		return;
	}

	// The tint, baked into our own texture (finding 3's 2x2 layout). tex = NULL
	// falls through to the client's own Contrail1.dds - the stock look.
	SURFHANDLE hTex = NULL;
	if (prtTexMode && hPrtTex) {
		BakeParticleTex(g_fx.prtColour);
		if (pCore->UpdateTexture2D(hPrtTex, s_ptex, PT_DIM, PT_DIM)) hTex = hPrtTex;
		else {
			prtTexMode = false;
			oapiWriteLog("ORO: particle texture upload FAILED - stock texture from here on.");
		}
	}

	// THE SPEC. These are the author's own fields, straight through - that is the
	// entire point of the tab.
	PARTICLESTREAMSPEC pss = {};
	pss.flags       = 0;
	pss.srcsize     = (double)g_fx.prtSize;
	pss.srcrate     = (double)g_fx.prtRate;
	pss.v0          = (double)g_fx.prtSpeed;
	pss.srcspread   = (double)g_fx.prtSpread;
	pss.lifetime    = (double)g_fx.prtLifetime;
	pss.growthrate  = (double)g_fx.prtGrowth;
	pss.atmslowdown = (double)g_fx.prtSlowdown;
	pss.ltype       = g_fx.prtDiffuse ? PARTICLESTREAMSPEC::DIFFUSE
	                                  : PARTICLESTREAMSPEC::EMISSIVE;
	pss.levelmap    = PARTICLESTREAMSPEC::LVL_SQRT;   // alpha = sqrt(throttle), the
	pss.lmin        = 0; pss.lmax = 1;               //   stock exhaust-stream mapping
	// AIR FADE. ATM_FLAT returns amin as a CONSTANT factor (Particle.cpp:179), so
	// amin = 1.0 means "full strength everywhere, including vacuum" - the honest lab
	// default. The alternative is stock's own atmospheric ramp, which is correct for
	// a contrail and emits absolutely nothing in space. See prtAirFade's comment.
	if (g_fx.prtAirFade) {
		pss.atmsmap = PARTICLESTREAMSPEC::ATM_PLOG;
		pss.amin    = 1e-5; pss.amax = 0.1;
	} else {
		pss.atmsmap = PARTICLESTREAMSPEC::ATM_FLAT;
		pss.amin    = 1.0;  pss.amax = 1.0;
	}
	pss.tex         = hTex;

	// ---- create the streams -------------------------------------------------
	// Patch (o): raise the exemption latch around the whole creation loop. Every
	// ExhaustStream born while it is up stamps itself exempt from patch (n)'s
	// per-vessel stream suppression, so OUR streams keep emitting on a vessel whose
	// stock ones we just silenced. Lowered again below, unconditionally.
	const bool exempt = (pCore && pCore->CanExemptStream());
	if (exempt) pCore->ExemptNewStreams(true);

	const THGROUP_TYPE grp[3] = { THGROUP_MAIN, THGROUP_RETRO, THGROUP_HOVER };
	int made = 0;
	for (int s = 0; s < nStack && prtStrN < PRT_MAX_STREAM; s++) {
		VESSEL* sv = oapiGetVesselInterface(stack[s]);
		if (!sv) continue;
		for (int g = 0; g < 3 && prtStrN < PRT_MAX_STREAM; g++) {
			const DWORD n = sv->GetGroupThrusterCount(grp[g]);
			for (DWORD i = 0; i < n && prtStrN < PRT_MAX_STREAM; i++) {
				THRUSTER_HANDLE th = sv->GetGroupThruster(grp[g], i);
				if (!th) continue;
				VECTOR3 pos, dir;
				sv->GetThrusterRef(th, pos);
				sv->GetThrusterDir(th, dir);
				const double dl = length(dir);
				if (dl < 1e-6) continue;
				dir = dir / dl;
				// The exhaust leaves along -dir; OFFSET slides the emission point
				// along that flow (negative = back toward/into the bell). The core
				// cannot redefine a stream's position later, which is the other
				// reason a slider change rebuilds.
				const VECTOR3 flow = -dir;
				const VECTOR3 src  = pos + flow * (double)g_fx.prtOffset;

				PrtStream& e = prtStr[prtStrN];
				e.hV  = stack[s];
				e.th  = th;
				// AddExhaustStream, not AddParticleStream (finding 2): the core drives
				// the level from the thruster itself, so we own no level pointer.
				e.h   = sv->AddExhaustStream(th, src, &pss);
				if (!e.h) continue;                // streams disabled in the Launchpad
				prtStrN++;
				made++;
			}
		}
	}
	if (exempt) pCore->ExemptNewStreams(false);      // never leave the latch up
	g_fx.prtCount = made;

	if (!made) {
		// AddExhaustStream returns NULL when the user has turned particle streams
		// off in the Launchpad - a setting outside ORO that would otherwise look
		// exactly like a broken effect.
		strcpy_s(g_fx.prtInfo, "no streams created - particle streams off in Launchpad?");
	} else if (g_fx.stockParticles) {
		// The pill that matters here is the one on THIS tab (the patch-(n) split):
		// stock's streams are killed by STOCK PARTICLES, not by STOCK EXHAUST.
		sprintf_s(g_fx.prtInfo, "%d stream%s - stock's emit too (see STOCK PARTICLES)",
		          made, made == 1 ? "" : "s");
	} else if (pCore && pCore->CanExemptStream()) {
		sprintf_s(g_fx.prtInfo, "%d stream%s replacing stock's%s",
		          made, made == 1 ? "" : "s", hTex ? "" : " (stock texture)");
	} else {
		// Stock is suppressed and this client has no patch (o), so patch (n) is
		// gating OUR streams along with stock's. Silent otherwise - the tab would
		// simply do nothing and look broken.
		sprintf_s(g_fx.prtInfo, "%d stream%s SUPPRESSED with stock's - needs patch (o)",
		          made, made == 1 ? "" : "s");
	}
}

// ----------------------------------------------------------------------------
// Hand every borrowed stream back. Safe to call twice; oapiIsVessel guards the
// teardown-order races exactly as the reentry table's returns do.
// ----------------------------------------------------------------------------
void OroModule::ReleaseParticles(const char* why)
{
	for (int i = 0; i < prtStrN; i++) {
		PrtStream& e = prtStr[i];
		// Nothing to un-exempt: patch (o) is a LATCH that stamps a member at
		// construction, so a deleted stream takes its exemption with it. That is
		// the point of a latch - there is no client-side table to leak.
		if (e.h && e.hV && oapiIsVessel(e.hV)) {
			VESSEL* v = oapiGetVesselInterface(e.hV);
			if (v) v->DelExhaustStream(e.h);       // the universal stream deleter
		}
		e.h = NULL; e.hV = NULL; e.th = NULL;
	}
	// DIAGNOSTIC (2026-08-11): the reload CTD is a corrupted `ncontrail` in Orbiter's
	// Vessel - the array this feeds. Log every release with its count so the teardown
	// order is visible in the log instead of inferred. Cheap: a few lines per session.
	if (prtStrN) oapiWriteLogV("ORO PRT: released %d stream(s) [%s].", prtStrN, why ? why : "?");
	prtStrN = 0;
	g_fx.prtCount = 0;
	s_haveApplied = false;                      // next update rebuilds from scratch
	s_havePending = false;                      // ... and drop any pending rebuild with it
}

// A vessel is about to die: drop its streams while the handle is still valid,
// and force a rebuild so the survivors are re-enumerated.
void OroModule::ForgetParticleVessel(OBJHANDLE hVessel)
{
	for (int i = 0; i < prtStrN; i++)
		if (prtStr[i].hV == hVessel) { ReleaseParticles("vessel deleted"); return; }
}

// Session teardown: the texture is a device resource.
void OroModule::ReleaseParticleTex()
{
	if (hPrtTex) { oapiDestroySurface(hPrtTex); hPrtTex = NULL; }
	prtTexMode  = false;
	prtTexTried = false;
}
