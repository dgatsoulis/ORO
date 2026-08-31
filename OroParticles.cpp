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

	// ------------------------------------------------------------------------
	// FILE TEXTURES (phase 1 of the texture picker, 2026-08-29, his design): the
	// particle SHAPE can come from a .dds - Orbiter's own Contrail1/Contrail1a, or
	// anything the user drops in Textures\ORO\Particles. The file's pixels are
	// TINTED by the swatch colour and uploaded through the SAME patch-(l) path as
	// the synthesized atlas, so the stream's SURFHANDLE never changes (no lifetime
	// question, no rebuild-for-texture) and the Colour swatch keeps working: white
	// = the file exactly as authored. ⚠️ Files must be 2x2 ATLASES (23j) - a single
	// centred blob renders as corner wedges; the folder README says so.
	// Own DDS reader on purpose: deterministic, no dependence on lock semantics of
	// loaded D3D surfaces, and phase 2's thumbnails need exactly this decoder.
	// DXT1/3/5 + uncompressed 32-bit, top mip only, any size (bilinear to 256).
	// ------------------------------------------------------------------------
	static void Dxt1Colours(const BYTE* b, DWORD c[4], bool dxt1)
	{
		const WORD c0 = *(const WORD*)b, c1 = *(const WORD*)(b + 2);
		const int r0 = ((c0 >> 11) & 31) * 255 / 31, g0 = ((c0 >> 5) & 63) * 255 / 63, b0 = (c0 & 31) * 255 / 31;
		const int r1 = ((c1 >> 11) & 31) * 255 / 31, g1 = ((c1 >> 5) & 63) * 255 / 63, b1 = (c1 & 31) * 255 / 31;
		c[0] = 0xFF000000u | (r0 << 16) | (g0 << 8) | b0;
		c[1] = 0xFF000000u | (r1 << 16) | (g1 << 8) | b1;
		if (!dxt1 || c0 > c1) {
			c[2] = 0xFF000000u | (((2*r0 + r1) / 3) << 16) | (((2*g0 + g1) / 3) << 8) | ((2*b0 + b1) / 3);
			c[3] = 0xFF000000u | (((r0 + 2*r1) / 3) << 16) | (((g0 + 2*g1) / 3) << 8) | ((b0 + 2*b1) / 3);
		} else {
			c[2] = 0xFF000000u | (((r0 + r1) / 2) << 16) | (((g0 + g1) / 2) << 8) | ((b0 + b1) / 2);
			c[3] = 0x00000000u;                            // DXT1 3-colour mode: transparent black
		}
	}

	// Decode the top mip of a DDS into malloc'd ARGB. Caller frees. False = cannot use.
	static bool LoadDDSRGBA(const char* path, DWORD** outPix, int* outW, int* outH)
	{
		FILE* fp = NULL;
		if (fopen_s(&fp, path, "rb") || !fp) return false;
		BYTE hdr[128];
		if (fread(hdr, 1, 128, fp) != 128 || *(DWORD*)hdr != 0x20534444u) { fclose(fp); return false; }
		const int   h       = *(int*)(hdr + 12), w = *(int*)(hdr + 16);
		const DWORD pfFlags = *(DWORD*)(hdr + 80);
		const DWORD fourCC  = *(DWORD*)(hdr + 84);
		const DWORD bits    = *(DWORD*)(hdr + 88);
		if (w < 4 || h < 4 || w > 4096 || h > 4096) { fclose(fp); return false; }
		DWORD* pix = (DWORD*)malloc((size_t)w * h * 4);
		if (!pix) { fclose(fp); return false; }
		bool ok = false;
		if (pfFlags & 0x4) {                               // DDPF_FOURCC - compressed
			const bool dxt1 = (fourCC == 0x31545844u);     // 'DXT1'
			const bool dxt3 = (fourCC == 0x33545844u);     // 'DXT3'
			const bool dxt5 = (fourCC == 0x35545844u);     // 'DXT5'
			if (dxt1 || dxt3 || dxt5) {
				const int bw = (w + 3) / 4, bh = (h + 3) / 4, bsz = dxt1 ? 8 : 16;
				BYTE* blocks = (BYTE*)malloc((size_t)bw * bh * bsz);
				if (blocks && fread(blocks, 1, (size_t)bw * bh * bsz, fp) == (size_t)bw * bh * bsz) {
					for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
						const BYTE* blk = blocks + ((size_t)by * bw + bx) * bsz;
						const BYTE* cb  = dxt1 ? blk : blk + 8;      // colour half
						DWORD col[4]; Dxt1Colours(cb, col, dxt1);
						DWORD cidx = *(const DWORD*)(cb + 4);
						BYTE  a5[8] = {};                            // DXT5 alpha palette
						if (dxt5) {
							a5[0] = blk[0]; a5[1] = blk[1];
							if (a5[0] > a5[1]) for (int k = 0; k < 6; k++) a5[2+k] = (BYTE)(((6-k)*a5[0] + (k+1)*a5[1]) / 7);
							else { for (int k = 0; k < 4; k++) a5[2+k] = (BYTE)(((4-k)*a5[0] + (k+1)*a5[1]) / 5); a5[6] = 0; a5[7] = 255; }
						}
						for (int py = 0; py < 4; py++) for (int px = 0; px < 4; px++) {
							const int x = bx*4 + px, y = by*4 + py;
							if (x >= w || y >= h) continue;
							DWORD c = col[(cidx >> ((py*4 + px)*2)) & 3];
							if (dxt3) {
								const int an = py*4 + px;
								BYTE a = (blk[an >> 1] >> ((an & 1) * 4)) & 0xF; a = (BYTE)(a * 17);
								c = (c & 0x00FFFFFFu) | ((DWORD)a << 24);
							} else if (dxt5) {
								const UINT64 aidx = *(const UINT64*)blk >> 16;   // 48 bits of 3-bit indices
								BYTE a = a5[(aidx >> ((py*4 + px)*3)) & 7];
								c = (c & 0x00FFFFFFu) | ((DWORD)a << 24);
							}
							pix[(size_t)y * w + x] = c;
						}
					}
					ok = true;
				}
				free(blocks);
			}
		} else if ((pfFlags & 0x40) && bits == 32) {       // DDPF_RGB, 32-bit A8R8G8B8/X8R8G8B8
			const DWORD aMask = *(DWORD*)(hdr + 104);
			if (fread(pix, 4, (size_t)w * h, fp) == (size_t)w * h) {
				if (!aMask) for (size_t i = 0; i < (size_t)w * h; i++) pix[i] |= 0xFF000000u;
				ok = true;
			}
		}
		fclose(fp);
		if (!ok) { free(pix); return false; }
		*outPix = pix; *outW = w; *outH = h;
		return true;
	}

	// The A/B split by DEST quadrant: diagonal pairs (TL+BR = A, TR+BL = B), so both
	// tints appear in every one of the renderer's eight orientation variants and the
	// random per-particle quadrant pick delivers the 50/50 mix - the atlas IS the
	// mechanism, no renderer change.
	static inline bool QuadIsA(int x, int y)
	{
		const int q = (y >= PT_HALF ? 2 : 0) + (x >= PT_HALF ? 1 : 0);
		return q == 0 || q == 3;
	}

	// Resolve, decode, resample to 256 and colour into s_ptex. stockCol = the file's
	// own authored colours; otherwise the tint REPLACES the file's colour using its
	// LUMINANCE as shading (a multiply can only darken - the 15b lesson - and white
	// must mean white). False = fall back to the synthesized puffs (and name the
	// failure once, main thread, so it is loud exactly once rather than per rebuild).
	static bool BakeFileTex(const char* name, DWORD colA, DWORD colB, bool stockCol)
	{
		char path[MAX_PATH];
		DWORD* pix = NULL; int w = 0, h = 0;
		sprintf_s(path, "Textures\\ORO\\Particles\\%s.dds", name);
		if (!LoadDDSRGBA(path, &pix, &w, &h)) {
			sprintf_s(path, "Textures\\%s.dds", name);      // the stock names live here
			if (!LoadDDSRGBA(path, &pix, &w, &h)) {
				static char lastFail[48] = "";
				if (_stricmp(lastFail, name)) {
					strcpy_s(lastFail, name);
					oapiWriteLogV("ORO: particle texture '%s' missing or undecodable - synthesized fallback.", name);
				}
				return false;
			}
		}
		for (int y = 0; y < PT_DIM; y++) {
			const float fy = ((float)y + 0.5f) * h / PT_DIM - 0.5f;
			int y0 = (int)floorf(fy); float wy = fy - y0;
			if (y0 < 0) { y0 = 0; wy = 0; } if (y0 > h - 2) { y0 = h - 2; wy = 1; }
			for (int x = 0; x < PT_DIM; x++) {
				const float fx = ((float)x + 0.5f) * w / PT_DIM - 0.5f;
				int x0 = (int)floorf(fx); float wx = fx - x0;
				if (x0 < 0) { x0 = 0; wx = 0; } if (x0 > w - 2) { x0 = w - 2; wx = 1; }
				const DWORD p00 = pix[(size_t)y0*w + x0],     p10 = pix[(size_t)y0*w + x0 + 1];
				const DWORD p01 = pix[(size_t)(y0+1)*w + x0], p11 = pix[(size_t)(y0+1)*w + x0 + 1];
				float ch[4];
				for (int c = 0; c < 4; c++) {
					const int sh = c * 8;
					const float a = (float)((p00 >> sh) & 0xFF), b = (float)((p10 >> sh) & 0xFF);
					const float d = (float)((p01 >> sh) & 0xFF), e = (float)((p11 >> sh) & 0xFF);
					ch[c] = (a + (b - a)*wx) + ((d + (e - d)*wx) - (a + (b - a)*wx)) * wy;
				}
				int R, G, B;
				if (stockCol) {
					// the file's own authored colours, untouched
					B = (int)ch[0]; G = (int)ch[1]; R = (int)ch[2];
				} else {
					// luminance x this quadrant's tint - white really is white
					const DWORD col = QuadIsA(x, y) ? colA : colB;
					const float lum = (0.114f*ch[0] + 0.587f*ch[1] + 0.299f*ch[2]) / 255.0f;
					R = (int)clampf(lum * (float)( col        & 0xFF), 0.0f, 255.0f);
					G = (int)clampf(lum * (float)((col >>  8) & 0xFF), 0.0f, 255.0f);
					B = (int)clampf(lum * (float)((col >> 16) & 0xFF), 0.0f, 255.0f);
				}
				const int A = (int)ch[3];
				s_ptex[(size_t)y * PT_DIM + x] = ((DWORD)A << 24) | ((DWORD)R << 16) | ((DWORD)G << 8) | (DWORD)B;
			}
		}
		free(pix);
		return true;
	}

	// FOUR soft puffs, one per quadrant (finding 3). RGB carries the user's tint,
	// ALPHA the soft ragged mask - which is the only channel the EMISSIVE path
	// really trades on, and the shape channel for DIFFUSE too.
	// Phase 1 texture picker: a non-empty texName routes to the FILE bake above;
	// failure falls through to the puffs (missing-piece-is-inert). TWO tints since
	// 2026-08-30 (his design): diagonal quadrant pairs carry colour A and colour B,
	// and the renderer's random quadrant pick mixes them 50/50 per particle. STOCK
	// = the texture's own colours (for the puffs that means plain white shading).
	void BakeParticleTex(DWORD colA, DWORD colB, bool stockCol, const char* texName)
	{
		if (texName && texName[0] && BakeFileTex(texName, colA, colB, stockCol)) return;
		for (int q = 0; q < 4; q++) {
			const int ox = (q & 1) * PT_HALF, oy = (q >> 1) * PT_HALF;
			const int sd = q * 613 + 7;
			const DWORD col = (q == 0 || q == 3) ? colA : colB;   // the diagonal A/B split
			const int tr = stockCol ? 255 : (int)( col        & 0xFF);
			const int tg = stockCol ? 255 : (int)((col >>  8) & 0xFF);
			const int tb = stockCol ? 255 : (int)((col >> 16) & 0xFF);
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
	// PER GROUP since 2026-08-16. ⚠️ ONE SIGNATURE COVERING ALL FOUR GROUPS, deliberately,
	// rather than four independent lifecycles. A stream cannot be reconfigured in place -
	// the core COPIES the spec at construction - so any change means delete-and-re-add,
	// and four independent rebuild clocks would mean four settle timers, four throttle
	// windows and four sets of lingering orphan streams to reason about. Folding every
	// group into one signature costs exactly one thing: tuning HOVER also rebuilds MAIN's
	// streams, so MAIN's particles blink once. During an active tuning drag that is
	// invisible against the 0.25 s settle, and it buys a rebuild path with a single
	// state machine instead of four interleaved ones.
	struct PrtGrpSig {
		float  ofs, size, life, rate, speed, spread, growth, slow;
		bool   diffuse, airfade;
		DWORD  colour, colour2;
		bool   texstock;
		bool   on;
		char   tex[48];       // texture NAME - a change re-bakes at the next rebuild
	};
	struct PrtSig {
		OBJHANDLE hV;
		int    nStream;
		PrtGrpSig g[ORO_THR_N];
		bool   on;
		DWORD  ovrHash;       // Phase B: the override pool's PARTICLE families, folded
		                      //   in so an override edit rebuilds on the SAME single
		                      //   settle clock (26f preserved - one state machine)
		DWORD  cacheGen;      // ... and the CLASS CACHE's generation: a foreign class
		                      //   loading, dropping or reloading changes what the
		                      //   streams should be without touching any live table
		bool operator!=(const PrtSig& o) const { return memcmp(this, &o, sizeof(PrtSig)) != 0; }
	};

	// Phase B: the particle block a thruster answers to, plus its override slot when
	// one owns it (the slot indexes the per-override texture). faithful = the vessel
	// passed OroThr_ClassMatch; anything else resolves to the group, always.
	const OroThrusterFx* PrtEffOf(bool faithful, int thrIdx, int gi, int* ovrSlot)
	{
		if (ovrSlot) *ovrSlot = -1;
		if (faithful && thrIdx >= 0) {
			for (int s = 0; s < ORO_THR_OVR_MAX; s++) {
				const OroThrOvr& o = g_fx.thrOvr[s];
				if (o.thrIdx == thrIdx && o.ovrPrt) {
					if (ovrSlot) *ovrSlot = s;
					return &o.fx;
				}
			}
		}
		return &g_fx.thr[(gi >= 0 && gi < ORO_THR_N) ? gi : 0];
	}
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
	// ⛔ THE MUTUAL EXCLUSION IS GONE (2026-08-29, his ruling): "some users might
	// want both" - stock's streams and ORO's may fly stacked, and ORO's job is to
	// save the combination the user set and load it back faithfully. The old rule
	// here silently rewrote loaded settings on every pre-step, which is exactly how
	// a saved pill state gets overridden without anyone pressing anything.

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
	// ⚠️ THE WALK IS NO LONGER GATED ON THE LIVE PILLS (his SRB report, 2026-08-30):
	// a CACHED foreign class can want streams while every live pill is off - his
	// Atlantis_SRB.cfg said USERPrtOn TRUE while the orbiter's class said FALSE -
	// and gating the walk on the pills was exactly how the boosters fell in the
	// suppress/replace gap. The pills still decide the CAPTION's wording below.
	OBJHANDLE stack[STACK_MAX];
	int nStack = 0;
	bool pillsOn = false;
	for (int gi = 0; gi < ORO_THR_N && !pillsOn; gi++)
		if (g_fx.thr[gi].prtEnabled) pillsOn = true;
	for (int s = 0; s < ORO_THR_OVR_MAX && !pillsOn; s++)
		if (g_fx.thrOvr[s].thrIdx >= 0 && g_fx.thrOvr[s].ovrPrt &&
		    g_fx.thrOvr[s].fx.prtEnabled) pillsOn = true;
	if (g_fx.masterArmed) {
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
		}
	}

	// ---- is AIR FADE currently emitting nothing? ---------------------------
	// Published every frame so the panel can SAY SO. Stock's atmospheric ramp is
	// ATM_PLOG over 1e-5..0.1 kg/m3 and Atm2Alpha returns exactly zero below amin, so
	// with the fade on and the ship in vacuum the streams exist, are correctly
	// configured, and emit nothing whatsoever - which is indistinguishable from a
	// broken tab unless something names it. That failure mode is the entire reason
	// invariant 23j made ALWAYS ON the default; naming it is what let the default flip
	// to the physically honest one on 2026-08-15.
	{
		OBJHANDLE hv = oapiCameraTarget();
		if (!hv || oapiGetObjectType(hv) != OBJTP_VESSEL) hv = oapiGetFocusObject();
		VESSEL* vv = (hv && oapiGetObjectType(hv) == OBJTP_VESSEL) ? oapiGetVesselInterface(hv) : NULL;
		// The readout answers for whatever the panel is EDITING - group or thruster.
		// The FLAT field is exactly that (the edit buffer holds the selection's
		// effective values, and SyncOut ran at the top of this pre-step), where
		// thr[thrSel] would answer for the group even with a thruster selected.
		g_fx.prtVacuum = g_fx.prtAirFade && vv && (vv->GetAtmDensity() < 1e-5);
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
			for (int gi = 0; gi < ORO_THR_N; gi++)
				hPrtTex[gi] = oapiCreateSurfaceEx(PT_DIM, PT_DIM,
				                                  OAPISURFACE_TEXTURE | OAPISURFACE_NOMIPMAPS |
				                                  OAPISURFACE_ALPHA);
		prtTexMode = (hPrtTex[0] != NULL);
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
	for (int gi = 0; gi < ORO_THR_N; gi++) {
		const OroThrusterFx& T = g_fx.thr[gi];
		PrtGrpSig& G = sig.g[gi];
		G.ofs = T.prtOffset;   G.size    = T.prtSize;     G.life   = T.prtLifetime;
		G.rate = T.prtRate;    G.speed   = T.prtSpeed;    G.spread = T.prtSpread;
		G.growth = T.prtGrowth; G.slow   = T.prtSlowdown; G.diffuse = T.prtDiffuse;
		G.airfade = T.prtAirFade; G.colour = T.prtColour; G.on     = T.prtEnabled;
		G.colour2 = T.prtColour2; G.texstock = T.prtTexStock;
		strcpy_s(G.tex, T.prtTexName);     // sig is zero-inited, so memcmp stays honest
	}
	// Phase B: hash the override pool's particle families in. Field by field, never a
	// struct memcpy - the file's own warning about types that merely share names.
	{
		DWORD hsh = 2166136261u;
		auto mix = [&](const void* p, size_t n) {
			const unsigned char* b = (const unsigned char*)p;
			for (size_t k = 0; k < n; k++) { hsh ^= b[k]; hsh *= 16777619u; }
		};
		for (int s = 0; s < ORO_THR_OVR_MAX; s++) {
			const OroThrOvr& o = g_fx.thrOvr[s];
			if (o.thrIdx < 0 || !o.ovrPrt) continue;
			mix(&o.thrIdx, sizeof(int));
			mix(&o.fx.prtEnabled,  sizeof(bool));
			mix(&o.fx.prtOffset,   sizeof(float));
			mix(&o.fx.prtSize,     sizeof(float));
			mix(&o.fx.prtLifetime, sizeof(float));
			mix(&o.fx.prtRate,     sizeof(float));
			mix(&o.fx.prtSpeed,    sizeof(float));
			mix(&o.fx.prtSpread,   sizeof(float));
			mix(&o.fx.prtGrowth,   sizeof(float));
			mix(&o.fx.prtSlowdown, sizeof(float));
			mix(&o.fx.prtDiffuse,  sizeof(bool));
			mix(&o.fx.prtAirFade,  sizeof(bool));
			mix(&o.fx.prtColour,   sizeof(DWORD));
			mix(&o.fx.prtColour2,  sizeof(DWORD));
			mix(&o.fx.prtTexStock, sizeof(bool));
			mix(o.fx.prtTexName, strlen(o.fx.prtTexName));
		}
		sig.ovrHash = hsh;
	}

	// ONE predicate for the count here and the creation loop below - a mismatch
	// between them churns rebuilds forever. Per VESSEL: its class-cache slot wins
	// (the vessel's own cfg, groups and its own override blocks), the live tables
	// otherwise, with live per-thruster overrides only for the focus class.
	auto effPrt = [](VESSEL* sv, int cacheIdx, bool faithful, DWORD i, int gi,
	                 int* ovrSlot) -> const OroThrusterFx* {
		if (ovrSlot) *ovrSlot = -1;
		if (cacheIdx >= 0)
			return &OroThr_EffC(cacheIdx, gi, (int)i, ORO_FAM_PRT);
		return PrtEffOf(faithful, faithful ? (int)i : -1, gi, ovrSlot);
	};

	// Count the qualifying thrusters: a staging event changes the set without
	// changing any slider, and the streams must follow the hardware. This walk is
	// also where foreign classes LOAD lazily (OroThr_CacheFor reads the cfg on
	// first sight - main thread, once per class per session).
	int nWantStream = 0;
	for (int s = 0; s < nStack; s++) {
		VESSEL* sv = oapiGetVesselInterface(stack[s]);
		if (!sv) continue;
		const int  cIdx     = OroThr_CacheFor(sv);
		const bool faithful = (cIdx < 0) && OroThr_ClassMatch(sv);
		const DWORD nth = sv->GetThrusterCount();
		for (DWORD i = 0; i < nth; i++) {
			const int gi = OroThrusterGroupOf(sv, sv->GetThrusterHandleByIndex(i));
			if (gi < 0) continue;
			if (effPrt(sv, cIdx, faithful, i, gi, NULL)->prtEnabled) nWantStream++;
		}
	}
	if (nWantStream > PRT_MAX_STREAM) nWantStream = PRT_MAX_STREAM;
	const bool want = g_fx.masterArmed && nWantStream > 0;
	sig.nStream  = nWantStream;
	sig.on       = want;
	sig.cacheGen = OroThr_CacheGen();

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
		strcpy_s(g_fx.prtInfo, pillsOn && g_fx.masterArmed
		                     ? "no thrusters in the enabled group(s)" : "off");
		g_fx.prtCount = 0;
		return;
	}

	// The tint, baked into our own texture (finding 3's 2x2 layout). tex = NULL
	// falls through to the client's own Contrail1.dds - the stock look.
	// ONE PER GROUP (2026-08-16): the spec has no colour field, so a per-group colour
	// is a per-group texture. Baked here, at rebuild time, not per frame.
	SURFHANDLE hTexG[ORO_THR_N] = {};
	SURFHANDLE hTex = NULL;                 // (kept for the failure log below)
	if (prtTexMode && hPrtTex[0]) {
		for (int gi = 0; gi < ORO_THR_N; gi++) {
			if (!hPrtTex[gi]) continue;
			BakeParticleTex(g_fx.thr[gi].prtColour, g_fx.thr[gi].prtColour2,
			                g_fx.thr[gi].prtTexStock, g_fx.thr[gi].prtTexName);
			if (pCore->UpdateTexture2D(hPrtTex[gi], s_ptex, PT_DIM, PT_DIM)) hTexG[gi] = hPrtTex[gi];
			else {
				prtTexMode = false;
				oapiWriteLog("ORO: particle texture upload FAILED - stock texture from here on.");
				break;
			}
		}
		hTex = hTexG[0];
	}
	// Phase B: a texture per ACTIVE particle override - same reasoning as the group
	// set (the spec has no colour field, so a per-thruster colour IS a per-thruster
	// texture). Surfaces are lazily created here at rebuild time, bounded by the
	// pool, and released with the group set in ReleaseParticleTex.
	SURFHANDLE hTexOvrS[ORO_THR_OVR_MAX] = {};
	if (prtTexMode && hPrtTex[0]) {
		for (int s = 0; s < ORO_THR_OVR_MAX; s++) {
			const OroThrOvr& o = g_fx.thrOvr[s];
			if (o.thrIdx < 0 || !o.ovrPrt || !o.fx.prtEnabled) continue;
			if (!hPrtTexOvr[s])
				hPrtTexOvr[s] = oapiCreateSurfaceEx(PT_DIM, PT_DIM,
				                                    OAPISURFACE_TEXTURE | OAPISURFACE_NOMIPMAPS |
				                                    OAPISURFACE_ALPHA);
			if (!hPrtTexOvr[s]) continue;
			BakeParticleTex(o.fx.prtColour, o.fx.prtColour2, o.fx.prtTexStock, o.fx.prtTexName);
			if (pCore->UpdateTexture2D(hPrtTexOvr[s], s_ptex, PT_DIM, PT_DIM))
				hTexOvrS[s] = hPrtTexOvr[s];
		}
	}
	// ... and per CACHED CLASS per group, baked lazily for the pairs the creation
	// loop actually uses - and RE-baked on every rebuild, because the surface
	// outlives a cache reload and would otherwise keep the old class colours (the
	// sig's cacheGen forces the rebuild; this makes the rebuild repaint). Falls
	// back to the live group texture on any failure, never silently to stock.
	bool cbaked[ORO_THR_CACHE_MAX][ORO_THR_N] = {};
	auto cacheTex = [&](int ci, int gi) -> SURFHANDLE {
		if (!prtTexMode || ci < 0 || ci >= ORO_THR_CACHE_MAX) return hTexG[gi];
		const OroThrusterFx* T = OroThr_CacheGrp(ci, gi);
		if (!T) return hTexG[gi];
		if (!cbaked[ci][gi]) {
			cbaked[ci][gi] = true;
			if (!hPrtTexC[ci][gi])
				hPrtTexC[ci][gi] = oapiCreateSurfaceEx(PT_DIM, PT_DIM,
				                                       OAPISURFACE_TEXTURE | OAPISURFACE_NOMIPMAPS |
				                                       OAPISURFACE_ALPHA);
			if (hPrtTexC[ci][gi]) {
				BakeParticleTex(T->prtColour, T->prtColour2, T->prtTexStock, T->prtTexName);
				if (!pCore->UpdateTexture2D(hPrtTexC[ci][gi], s_ptex, PT_DIM, PT_DIM)) {
					oapiDestroySurface(hPrtTexC[ci][gi]);
					hPrtTexC[ci][gi] = NULL;
				}
			}
		}
		return hPrtTexC[ci][gi] ? hPrtTexC[ci][gi] : hTexG[gi];
	};
	// ... and per CACHED OVERRIDE (his SRB report, round 2: the motor's Contrail4
	// lives in a THR0 override while the group holds Contrail1a - a share-the-group-
	// texture bound flew the wrong contrail on its first real use). Same lifecycle,
	// falling back to the class-group texture on any failure.
	bool cbakedO[ORO_THR_CACHE_MAX][ORO_THR_OVR_MAX] = {};
	auto cacheTexOvr = [&](int ci, int cov, int gi) -> SURFHANDLE {
		if (!prtTexMode || ci < 0 || ci >= ORO_THR_CACHE_MAX ||
		    cov < 0 || cov >= ORO_THR_OVR_MAX) return cacheTex(ci, gi);
		const OroThrusterFx* T = OroThr_CacheOvrFx(ci, cov);
		if (!T) return cacheTex(ci, gi);
		if (!cbakedO[ci][cov]) {
			cbakedO[ci][cov] = true;
			if (!hPrtTexCO[ci][cov])
				hPrtTexCO[ci][cov] = oapiCreateSurfaceEx(PT_DIM, PT_DIM,
				                                         OAPISURFACE_TEXTURE | OAPISURFACE_NOMIPMAPS |
				                                         OAPISURFACE_ALPHA);
			if (hPrtTexCO[ci][cov]) {
				BakeParticleTex(T->prtColour, T->prtColour2, T->prtTexStock, T->prtTexName);
				if (!pCore->UpdateTexture2D(hPrtTexCO[ci][cov], s_ptex, PT_DIM, PT_DIM)) {
					oapiDestroySurface(hPrtTexCO[ci][cov]);
					hPrtTexCO[ci][cov] = NULL;
				}
			}
		}
		return hPrtTexCO[ci][cov] ? hPrtTexCO[ci][cov] : cacheTex(ci, gi);
	};

	// THE SPEC. These are the author's own fields, straight through - that is the
	// entire point of the tab.
	// ONE SPEC PER GROUP, built up front (2026-08-16). The core copies the spec at
	// construction, so each stream can be handed its own group's numbers and then never
	// needs to know about groups again. Phase B routes the OVERRIDE specs through the
	// SAME mapping (the lambda), so the two can never diverge field by field.
	auto buildPss = [](PARTICLESTREAMSPEC& pss, const OroThrusterFx& T, SURFHANDLE tex) {
		pss.flags       = 0;
		pss.srcsize     = (double)T.prtSize;
		pss.srcrate     = (double)T.prtRate;
		pss.v0          = (double)T.prtSpeed;
		pss.srcspread   = (double)T.prtSpread;
		pss.lifetime    = (double)T.prtLifetime;
		pss.growthrate  = (double)T.prtGrowth;
		pss.atmslowdown = (double)T.prtSlowdown;
		pss.ltype       = T.prtDiffuse ? PARTICLESTREAMSPEC::DIFFUSE
		                               : PARTICLESTREAMSPEC::EMISSIVE;
		pss.levelmap    = PARTICLESTREAMSPEC::LVL_SQRT;   // alpha = sqrt(throttle), the
		pss.lmin        = 0; pss.lmax = 1;                //   stock exhaust-stream mapping
		// AIR FADE. ATM_FLAT returns amin as a CONSTANT factor (Particle.cpp:179), so
		// amin = 1.0 means "full strength everywhere, including vacuum". The alternative
		// is stock's own atmospheric ramp, which is correct for a contrail and emits
		// absolutely nothing in space - the default since 2026-08-15, with the panel
		// saying so while it holds emission off. See prtAirFade's comment.
		if (T.prtAirFade) {
			pss.atmsmap = PARTICLESTREAMSPEC::ATM_PLOG;
			pss.amin    = 1e-5; pss.amax = 0.1;
		} else {
			pss.atmsmap = PARTICLESTREAMSPEC::ATM_FLAT;
			pss.amin    = 1.0;  pss.amax = 1.0;
		}
		pss.tex         = tex;
	};
	PARTICLESTREAMSPEC pssG[ORO_THR_N] = {};
	for (int gi = 0; gi < ORO_THR_N; gi++) buildPss(pssG[gi], g_fx.thr[gi], hTexG[gi]);

	// ---- create the streams -------------------------------------------------
	// Patch (o): raise the exemption latch around the whole creation loop. Every
	// ExhaustStream born while it is up stamps itself exempt from patch (n)'s
	// per-vessel stream suppression, so OUR streams keep emitting on a vessel whose
	// stock ones we just silenced. Lowered again below, unconditionally.
	const bool exempt = (pCore && pCore->CanExemptStream());
	if (exempt) pCore->ExemptNewStreams(true);

	int made = 0;
	for (int s = 0; s < nStack && prtStrN < PRT_MAX_STREAM; s++) {
		VESSEL* sv = oapiGetVesselInterface(stack[s]);
		if (!sv) continue;
		const int  cIdx     = OroThr_CacheFor(sv);     // its own class file, cached
		const bool faithful = (cIdx < 0) && OroThr_ClassMatch(sv);   // live overrides
		{
			// Walk the vessel's thrusters ONCE and classify each, instead of walking
			// three named groups: that is what admits USER-defined engines (which had
			// a bell but never a stream), from the one shared definition rather than
			// from a list repeated in four files.
			const DWORD n = sv->GetThrusterCount();
			for (DWORD i = 0; i < n && prtStrN < PRT_MAX_STREAM; i++) {
				THRUSTER_HANDLE th = sv->GetThrusterHandleByIndex(i);
				if (!th) continue;
				const int gi = OroThrusterGroupOf(sv, th);
				if (gi < 0) continue;
				// Phase B: the block THIS thruster answers to - the SAME predicate as
				// the count above, or the signature would churn.
				int ovrSlot = -1;
				const OroThrusterFx* pT = effPrt(sv, cIdx, faithful, i, gi, &ovrSlot);
				if (!pT->prtEnabled) continue;
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
				const VECTOR3 src  = pos + flow * (double)pT->prtOffset;

				// A non-live block gets its OWN spec through the same mapping AND its
				// own texture: a cached class bakes per group and per OVERRIDE (the
				// SRB motor's Contrail4 vs its group's Contrail1a - his report), a
				// live override uses its per-thruster surface. Group texture as the
				// honest fallback throughout, never NULL-for-stock by accident.
				PARTICLESTREAMSPEC pssOvr;
				PARTICLESTREAMSPEC* pss = &pssG[gi];
				if (cIdx >= 0) {
					const int cov = OroThr_CacheOvrSlot(cIdx, (int)i);
					buildPss(pssOvr, *pT, cov >= 0 ? cacheTexOvr(cIdx, cov, gi)
					                               : cacheTex(cIdx, gi));
					pss = &pssOvr;
				} else if (ovrSlot >= 0) {
					buildPss(pssOvr, *pT, hTexOvrS[ovrSlot] ? hTexOvrS[ovrSlot] : hTexG[gi]);
					pss = &pssOvr;
				}

				PrtStream& e = prtStr[prtStrN];
				e.hV  = stack[s];
				e.th  = th;
				// AddExhaustStream, not AddParticleStream (finding 2): the core drives
				// the level from the thruster itself, so we own no level pointer.
				e.h   = sv->AddExhaustStream(th, src, pss);
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
	for (int gi = 0; gi < ORO_THR_N; gi++)
		if (hPrtTex[gi]) { oapiDestroySurface(hPrtTex[gi]); hPrtTex[gi] = NULL; }
	for (int s = 0; s < ORO_THR_OVR_MAX; s++)
		if (hPrtTexOvr[s]) { oapiDestroySurface(hPrtTexOvr[s]); hPrtTexOvr[s] = NULL; }
	for (int ci = 0; ci < ORO_THR_CACHE_MAX; ci++)
		for (int gi = 0; gi < ORO_THR_N; gi++)
			if (hPrtTexC[ci][gi]) { oapiDestroySurface(hPrtTexC[ci][gi]); hPrtTexC[ci][gi] = NULL; }
	for (int ci = 0; ci < ORO_THR_CACHE_MAX; ci++)
		for (int s = 0; s < ORO_THR_OVR_MAX; s++)
			if (hPrtTexCO[ci][s]) { oapiDestroySurface(hPrtTexCO[ci][s]); hPrtTexCO[ci][s] = NULL; }
	prtTexMode  = false;
	prtTexTried = false;
}
