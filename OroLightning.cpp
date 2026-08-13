// ==============================================================
// OroLightning.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - LIGHTNING: flashes in the cloud deck, seen from above
// ----------------------------------------------------------------------------
// DOMAIN: above the cloud layer only (v1 scope, user's call 2026-08-08) - external
// always, internal through the VC window when patch (g) depth-clipping is live,
// exactly the aurora's gating. No thunder, no cabin illumination in v1.
//
// THE REFERENCE (ISS photography, 2026-08-08 session): a flash from orbit is a LIT
// PATCH OF CLOUD, not a bolt - ~10 km across with a soft halo spreading further,
// BLUE-WHITE to VIOLET (the opposite palette pole from the reentry plasma), interior
// mottled by dark cloud lobes, and the motion signature is the FLICKER: 40-200 ms
// flashes averaging ~4 strokes 40-80 ms apart, cells tens of km apart pulsing
// independently.
//
// THE ONE HARD PROBLEM, and how it is solved: flashes must sit inside the ACTUAL
// visible cloud masses ("flashes over clear ocean are no good"). The framebuffer
// cannot answer "is there cloud here" - unlit night cloud renders as black as the
// sea beside it, and an additive draw cannot be masked by what is underneath. But
// the DATA can: the client renders the cloud layer from DXT5 tiles whose ALPHA IS
// THE COVERAGE (the ground shader's own cloud-shadow pass samples exactly that), so
// this file reads the same tiles - Textures\<body>\Archive\Cloud.tree, the format
// verified byte-for-byte against the client's ZTreeMgr on 2026-08-08 (48-byte
// header, 32-byte TOC nodes, per-node zlib blocks that oapiInflate - A PUBLIC CORE
// API - decompresses). Storm cells spawn only where coverage passes a floor, and
// every disc VERTEX multiplies its alpha by the coverage under it, so a flash
// straddling a deck edge dies exactly where the cloud does and the interior mottle
// comes from real data instead of hash noise.
//
// FRAMES: cells live in CLOUD-TEXTURE coordinates. The layer rotates relative to
// the surface (OBJPRM_PLANET_CLOUDROTATION, the client applies it as a rotation
// about the spin axis such that texture longitude lonT renders at planet longitude
// lonT - crot), so keeping cells in texture coordinates makes a storm RIDE ITS
// CLOUD MASS as the layer drifts - free correctness. The first successful map open
// logs a landmark diag that prints coverage under BOTH sign conventions; the sign
// below was settled by that diag, not by trusting the derivation.
//
// STATE DISCIPLINE (G10, amended by invariant 21d): the cell field is a pure
// deterministic function of (district, sim time) - nothing accumulates. The only
// cross-frame state is the flash-event ring, sub-second transients that self-expire
// in ~1.35 s REAL time (invariant 4: a flash is animation, so envelopes AND
// scheduling run on the real-time clock - which also means time acceleration does
// not strobe the sky, and a paused sim keeps flickering for beauty shots; only the
// slow FIELD evolution - which districts host storms - follows sim time, because
// that part is weather, not animation).
//
// MAIN THREAD ONLY (clbkPreStep -> UpdateLightning): oapi queries, file reads and
// the projection all happen here (invariant 1); the render callback only pushes
// ltgVtx and draws (DrawLightningPoly in OroModule.cpp). File I/O is budgeted to
// ONE tile decode per frame - a cold cap warms in under a second, and until a
// tile is decoded its cells simply do not spawn (fail closed, invisible).
// ============================================================================

#include "OroModule.h"
#include "OroState.h"
#include "gcCoreAPI.h"       // patch (l): CreateTrianglesTex / UpdateTexture2D (the
                             //   OroReentry.cpp precedent for a second gc consumer)
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

	// --- the storm field ----------------------------------------------------
	const double LTG_LIFE     = 1800.0;      // storm cell lifetime [s, SIM] (fade 15% ends)
	const double LTG_DGRID    = 2.5 * RAD;   // district grid pitch (72 x 144 districts)
	const int    LTG_MAX_CELL = 28;          // active cells in the visible cap (nearest kept)
	const float  LTG_CELL_MIN = 4.0f;        // Cell size slider 0..1 spans this radius [km]
	const float  LTG_CELL_MAX = 30.0f;
	const float  LTG_COV_SPAWN = 0.45f;      // coverage floor for a cell to exist at all
	const double LTG_TEST_COLAT = 2.0 * RAD; // TEST cell: this far north of the sub-camera
	                                         //   point (~220 km on Earth - fills the view
	                                         //   from a 300 km orbit without hunting)
	const float  LTG_ALPHA    = 250.0f;      // peak core alpha at Brightness 1 (same additive/
	                                         //   alpha-blend duality note as AUR_ALPHA)
	const int    LTG_SEC      = 18;          // disc sectors
	// ring radii (x cell radius) and their base alpha profile: a hot compact core, a
	// bright inner halo, a fading skirt - the Izmir/McClain falloff.
	const float  LTG_RING_R[4] = { 0.0f, 0.35f, 0.70f, 1.15f };
	const float  LTG_RING_A[4] = { 1.0f, 0.72f, 0.30f, 0.0f };

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
		r = (int)(c & 0xFF); g = (int)((c >> 8) & 0xFF); b = (int)((c >> 16) & 0xFF);
	}

	// Is the segment camera->P blocked by the planet? (the aurora's ray-sphere, verbatim)
	bool Occluded(const VECTOR3& cam, const VECTOR3& P, const VECTOR3& O, double R)
	{
		const VECTOR3 d = P - cam;
		const double L = length(d);
		if (L < 1.0) return false;
		const VECTOR3 u = d / L;
		const VECTOR3 m = O - cam;
		const double tca = dotp(m, u);
		const double d2  = dotp(m, m) - tca * tca;
		const double R2  = R * R;
		if (d2 > R2) return false;
		const double thc = sqrt(R2 - d2);
		const double t0  = tca - thc;
		return (t0 > 1.0 && t0 < L - 1.0);
	}

	OBJHANDLE FindStar()
	{
		const DWORD n = oapiGetGbodyCount();
		for (DWORD i = 0; i < n; i++) {
			OBJHANDLE h = oapiGetGbodyByIndex((int)i);
			if (h && oapiGetObjectType(h) == OBJTP_STAR) return h;
		}
		return oapiGetGbodyByIndex(0);
	}

	// ========================================================================
	// THE CLOUD MAP - a compact reader for the client's own tile archive.
	// Format verified against D3D9Client's ZTreeMgr (2026-08-08, offline probe of
	// the user's Cloud.tree: magic/48-byte header/32-byte node stride all
	// confirmed, every tile 512x512 DXT5). Own code against the struct layout
	// rather than a lift of the LGPL implementation.
	// ========================================================================
	const int   CM_LVL    = 7;     // sampling file level: 8x16 tiles, ~5 km/texel-block
	const int   CM_TILE_N = 24;    // decoded-tile cache (16.4 KB each)
	const int   CM_GRID   = 128;   // decoded alpha grid: one value per 4x4 DXT block

	struct CmNode { __int64 pos; DWORD size; DWORD child[4]; };

	struct DTile { int lvl, ilat, ilng; int stamp; BYTE a[CM_GRID * CM_GRID]; };

	char    g_cmBody[64] = "";     // planet the open (or failed) tree belongs to
	bool    g_cmOpenOK   = false;
	FILE*   g_cmFile     = NULL;
	__int64 g_cmDataOfs  = 0;
	__int64 g_cmDataLen  = 0;
	DWORD   g_cmNodeN    = 0;
	DWORD   g_cmRoot4[2] = { (DWORD)-1, (DWORD)-1 };
	CmNode* g_cmToc      = NULL;
	DTile   g_cmTile[CM_TILE_N];   // lvl < 0 = empty slot
	int     g_cmStamp    = 0;
	bool    g_cmDecoded  = false;  // the one-decode-per-frame budget flag
	bool    g_cmFmtWarned = false;

	void CmCloseFull();         // fwd: defined with the full-res cache below

	void CmCloseInternal()
	{
		if (g_cmFile) { fclose(g_cmFile); g_cmFile = NULL; }
		if (g_cmToc)  { free(g_cmToc);   g_cmToc  = NULL; }
		for (int i = 0; i < CM_TILE_N; i++) g_cmTile[i].lvl = -1;
		CmCloseFull();          // the full-res cache is per-body too
		g_cmOpenOK = false;
		g_cmBody[0] = 0;
	}

	// One-shot landmark diagnostic, printed at first successful Earth-family open.
	// It samples known geography under BOTH cloud-rotation sign conventions - the
	// convention that makes the Sahara read low and the Southern Ocean read high is
	// the right one, so the log SETTLES the sign question instead of trusting the
	// matrix derivation. (Declared below; defined after the sampler it uses.)
	void CmLandmarkDiag(double crot);

	bool CmOpen(const char* body, double crotForDiag)
	{
		if (_stricmp(g_cmBody, body) == 0) return g_cmOpenOK;   // cached answer (incl. failure)
		CmCloseInternal();
		strcpy_s(g_cmBody, sizeof(g_cmBody), body);

		// Texture root: Orbiter.cfg's TextureDir if present (the same key the client
		// reads), else the stock "Textures". Relative paths are fine - Orbiter runs
		// modules with the CWD at its root.
		char texDir[MAX_PATH] = "Textures";
		FILEHANDLE fh = oapiOpenFile("Orbiter.cfg", FILE_IN, ROOT);
		if (fh) {
			char buf[MAX_PATH];
			if (oapiReadItem_string(fh, (char*)"TextureDir", buf)) {
				char* p = buf;
				if (p[0] == '.' && (p[1] == '\\' || p[1] == '/')) p += 2;
				size_t n = strlen(p);
				while (n && (p[n-1] == '\\' || p[n-1] == '/')) p[--n] = 0;
				if (n) strcpy_s(texDir, sizeof(texDir), p);
			}
			oapiCloseFile(fh, FILE_IN);
		}

		char path[MAX_PATH];
		sprintf_s(path, sizeof(path), "%s\\%s\\Archive\\Cloud.tree", texDir, body);
		if (fopen_s(&g_cmFile, path, "rb") != 0 || !g_cmFile) {
			oapiWriteLogV("ORO: lightning - no cloud archive at %s (flashes limited to TEST).", path);
			return false;
		}

		// Header: fields in write order (verified stride; struct size / magic checked).
		DWORD magic = 0, hsize = 0, flags = 0, dataOfs32 = 0, nodeCount = 0;
		__int64 dataLen = 0;
		DWORD r1, r2, r3;
		bool ok = fread(&magic,     4, 1, g_cmFile) == 1 && magic == 0x00015854u   // 'T','X',1,0
		       && fread(&hsize,     4, 1, g_cmFile) == 1 && hsize == 48
		       && fread(&flags,     4, 1, g_cmFile) == 1
		       && fread(&dataOfs32, 4, 1, g_cmFile) == 1
		       && fread(&dataLen,   8, 1, g_cmFile) == 1
		       && fread(&nodeCount, 4, 1, g_cmFile) == 1
		       && fread(&r1,        4, 1, g_cmFile) == 1
		       && fread(&r2,        4, 1, g_cmFile) == 1
		       && fread(&r3,        4, 1, g_cmFile) == 1
		       && fread(g_cmRoot4,  4, 2, g_cmFile) == 2
		       && nodeCount > 0 && nodeCount < 4000000;
		if (!ok) {
			oapiWriteLogV("ORO: lightning - %s is not a tile tree (bad header).", path);
			CmCloseInternal();
			strcpy_s(g_cmBody, sizeof(g_cmBody), body);   // remember the failure per body
			return false;
		}

		// TOC: nodeCount records at a 32-byte stride (8 pos + 4 size + 16 child + 4 pad,
		// the C struct's own padding, confirmed empirically: (dataOfs-48)/nodeCount == 32).
		g_cmToc = (CmNode*)malloc(sizeof(CmNode) * nodeCount);
		BYTE* raw = (BYTE*)malloc((size_t)nodeCount * 32);
		ok = g_cmToc && raw && fread(raw, 32, nodeCount, g_cmFile) == nodeCount;
		if (ok) {
			for (DWORD i = 0; i < nodeCount; i++) {
				const BYTE* p = raw + (size_t)i * 32;
				memcpy(&g_cmToc[i].pos,   p,      8);
				memcpy(&g_cmToc[i].size,  p + 8,  4);
				memcpy(g_cmToc[i].child,  p + 12, 16);
			}
		}
		if (raw) free(raw);
		if (!ok) {
			oapiWriteLogV("ORO: lightning - failed reading %s TOC.", path);
			CmCloseInternal();
			strcpy_s(g_cmBody, sizeof(g_cmBody), body);
			return false;
		}
		g_cmDataOfs = (__int64)dataOfs32;
		g_cmDataLen = dataLen;
		g_cmNodeN   = nodeCount;
		g_cmOpenOK  = true;
		oapiWriteLogV("ORO: lightning cloud map open - %s (%u nodes).", path, nodeCount);
		CmLandmarkDiag(crotForDiag);
		return true;
	}

	DWORD CmIdx(int lvl, int ilat, int ilng)
	{
		if (!g_cmToc || lvl < 4) return (DWORD)-1;
		DWORD idx = g_cmRoot4[(ilng >> (lvl - 4)) & 1];
		for (int l = 5; l <= lvl; l++) {
			if (idx == (DWORD)-1) return idx;
			const int bit = lvl - l;
			const int c = (((ilat >> bit) & 1) << 1) + ((ilng >> bit) & 1);
			idx = g_cmToc[idx].child[c];
		}
		return idx;
	}

	// Decode one tile's coverage into a CM_GRID^2 byte grid: DXT5 alpha averaged per
	// 4x4 block (the honest average of all 16 decoded texels, not the endpoints).
	// Returns the cache slot's grid, or NULL: *absent tells a missing tile (walk up a
	// level) from a spent decode budget (unknown this frame - try again next frame).
	const BYTE* CmTile(int lvl, int ilat, int ilng, bool& absent, bool force)
	{
		absent = false;
		for (int i = 0; i < CM_TILE_N; i++) {
			if (g_cmTile[i].lvl == lvl && g_cmTile[i].ilat == ilat && g_cmTile[i].ilng == ilng) {
				g_cmTile[i].stamp = ++g_cmStamp;
				return g_cmTile[i].a;
			}
		}
		if (!g_cmOpenOK) { absent = true; return NULL; }
		if (g_cmDecoded && !force) return NULL;             // budget spent this frame

		const DWORD idx = CmIdx(lvl, ilat, ilng);
		if (idx == (DWORD)-1 || idx >= g_cmNodeN || g_cmToc[idx].size == 0) { absent = true; return NULL; }

		const __int64 zpos  = g_cmToc[idx].pos;
		const __int64 znext = (idx + 1 < g_cmNodeN) ? g_cmToc[idx + 1].pos : g_cmDataLen;
		const DWORD   zsize = (DWORD)(znext - zpos);
		const DWORD   esize = g_cmToc[idx].size;
		if (zsize == 0 || zsize > 8u * 1024 * 1024 || esize > 8u * 1024 * 1024) { absent = true; return NULL; }

		BYTE* zbuf = (BYTE*)malloc(zsize);
		BYTE* ebuf = (BYTE*)malloc(esize);
		bool ok = zbuf && ebuf
		       && _fseeki64(g_cmFile, g_cmDataOfs + zpos, SEEK_SET) == 0
		       && fread(zbuf, 1, zsize, g_cmFile) == zsize
		       && oapiInflate(zbuf, zsize, ebuf, esize) == esize;
		if (zbuf) free(zbuf);
		g_cmDecoded = true;                                  // a miss costs the budget too

		int slot = -1;
		if (ok) {
			// DDS sanity + format. Every tile in the verified archive is 512x512 DXT5;
			// anything else is logged once and treated as absent (fail soft, not wrong).
			const DWORD ddsMagic = *(DWORD*)ebuf;                    // 'DDS '
			const DWORD h = *(DWORD*)(ebuf + 12), w = *(DWORD*)(ebuf + 16);
			const DWORD fourcc = *(DWORD*)(ebuf + 84);
			if (ddsMagic != 0x20534444u || w != 512 || h != 512 || fourcc != 0x35545844u /*DXT5*/
			    || esize < 128 + 512 * 512) {
				if (!g_cmFmtWarned) {
					g_cmFmtWarned = true;
					oapiWriteLogV("ORO: lightning - unexpected cloud tile format (%ux%u fourcc 0x%08X) - skipping.",
					              w, h, fourcc);
				}
				ok = false;
				absent = true;
			} else {
				// LRU victim
				slot = 0;
				for (int i = 1; i < CM_TILE_N; i++)
					if (g_cmTile[i].lvl < 0 || (g_cmTile[slot].lvl >= 0 && g_cmTile[i].stamp < g_cmTile[slot].stamp))
						slot = i;
				DTile& T = g_cmTile[slot];
				T.lvl = lvl; T.ilat = ilat; T.ilng = ilng; T.stamp = ++g_cmStamp;
				const BYTE* blk = ebuf + 128;
				for (int by = 0; by < CM_GRID; by++) {
					for (int bx = 0; bx < CM_GRID; bx++) {
						const BYTE* B = blk + ((size_t)by * CM_GRID + bx) * 16;
						const int a0 = B[0], a1 = B[1];
						int av[8];
						av[0] = a0; av[1] = a1;
						if (a0 > a1) { for (int k = 1; k <= 6; k++) av[k + 1] = ((7 - k) * a0 + k * a1) / 7; }
						else         { for (int k = 1; k <= 4; k++) av[k + 1] = ((5 - k) * a0 + k * a1) / 5; av[6] = 0; av[7] = 255; }
						unsigned __int64 bits = 0;
						memcpy(&bits, B + 2, 6);
						int sum = 0;
						for (int t = 0; t < 16; t++) sum += av[(int)((bits >> (3 * t)) & 7)];
						T.a[by * CM_GRID + bx] = (BYTE)(sum >> 4);
					}
				}
			}
		} else absent = absent || !ok;
		if (ebuf) free(ebuf);
		return (ok && slot >= 0) ? g_cmTile[slot].a : NULL;
	}

	// Coverage 0..1 at (lat, lonT) - CLOUD-TEXTURE frame - or -1 when unknown this
	// frame (decode budget). Walks up from CM_LVL when a level is genuinely absent.
	// Tile addressing per the client's Tile::Extents: ilat 0 at the NORTH edge,
	// ilng 0 at longitude -180 running east (both verified against the real archive
	// by the ASCII coverage-map probe - the Southern Ocean storm ring came out at
	// the bottom, where it belongs).
	float CmCoverage(double lat, double lonT, bool force)
	{
		if (!g_cmOpenOK) return -1.0f;
		while (lonT >  PI) lonT -= 2.0 * PI;
		while (lonT < -PI) lonT += 2.0 * PI;
		for (int lvl = CM_LVL; lvl >= 4; lvl--) {
			const int nlat = 1 << (lvl - 4);
			const int nlng = 2 << (lvl - 4);
			double v = (0.5 - lat / PI) * nlat;
			double u = (lonT + PI) / (2.0 * PI) * nlng;
			int ilat = (int)floor(v); if (ilat < 0) ilat = 0; if (ilat >= nlat) ilat = nlat - 1;
			int ilng = (int)floor(u); ilng = ((ilng % nlng) + nlng) % nlng;
			bool absent = false;
			const BYTE* g = CmTile(lvl, ilat, ilng, absent, force);
			if (g) {
				double fx = (u - ilng) * CM_GRID - 0.5;
				double fy = (v - ilat) * CM_GRID - 0.5;
				int x0 = (int)floor(fx), y0 = (int)floor(fy);
				const double tx = fx - x0, ty = fy - y0;
				int x1 = x0 + 1, y1 = y0 + 1;
				if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
				if (x1 > CM_GRID - 1) x1 = CM_GRID - 1;
				if (y1 > CM_GRID - 1) y1 = CM_GRID - 1;
				if (x0 > CM_GRID - 1) x0 = CM_GRID - 1;
				if (y0 > CM_GRID - 1) y0 = CM_GRID - 1;
				const double a00 = g[y0 * CM_GRID + x0], a10 = g[y0 * CM_GRID + x1];
				const double a01 = g[y1 * CM_GRID + x0], a11 = g[y1 * CM_GRID + x1];
				const double a = (a00 * (1 - tx) + a10 * tx) * (1 - ty)
				               + (a01 * (1 - tx) + a11 * tx) * ty;
				return (float)(a / 255.0);
			}
			if (!absent) return -1.0f;                       // budget-starved: unknown
		}
		return -1.0f;
	}

	// ------------------------------------------------------------------------
	// FULL-RESOLUTION tile cache (patch l, the texture bake's source). The block
	// grids above average 4x4 texels (~20 km on Earth at level 7) - fine for cell
	// SPAWNING, far too coarse for a texture whose whole point is pixel shape. This
	// cache keeps complete 512^2 per-texel alpha for a few tiles at CM_FULL_LVL
	// (level 9: ~1.2 km/texel on Earth - the Izmir-frame mottle scale), decoded on
	// demand at flash trigger (event-driven, so no per-frame budget needed; a bake
	// touches 1-2 tiles and storms cluster, so the 4-slot LRU covers a region).
	// ------------------------------------------------------------------------
	const int CM_FULL_LVL    = 9;
	const int CM_FULL_TILE_N = 4;
	struct FTile { int lvl, ilat, ilng; int stamp; BYTE a[512 * 512]; };
	FTile g_cmFull[CM_FULL_TILE_N];       // 1 MB BSS; lvl 0 = empty (never queried at 0)
	int   g_cmFullStamp = 0;

	void CmCloseFull()
	{
		for (int i = 0; i < CM_FULL_TILE_N; i++) g_cmFull[i].lvl = 0;
	}

	const BYTE* CmTileFull(int lvl, int ilat, int ilng, bool& absent)
	{
		absent = false;
		for (int i = 0; i < CM_FULL_TILE_N; i++) {
			if (g_cmFull[i].lvl == lvl && g_cmFull[i].ilat == ilat && g_cmFull[i].ilng == ilng) {
				g_cmFull[i].stamp = ++g_cmFullStamp;
				return g_cmFull[i].a;
			}
		}
		if (!g_cmOpenOK) { absent = true; return NULL; }

		const DWORD idx = CmIdx(lvl, ilat, ilng);
		if (idx == (DWORD)-1 || idx >= g_cmNodeN || g_cmToc[idx].size == 0) { absent = true; return NULL; }
		const __int64 zpos  = g_cmToc[idx].pos;
		const __int64 znext = (idx + 1 < g_cmNodeN) ? g_cmToc[idx + 1].pos : g_cmDataLen;
		const DWORD   zsize = (DWORD)(znext - zpos);
		const DWORD   esize = g_cmToc[idx].size;
		if (zsize == 0 || zsize > 8u * 1024 * 1024 || esize > 8u * 1024 * 1024) { absent = true; return NULL; }

		BYTE* zbuf = (BYTE*)malloc(zsize);
		BYTE* ebuf = (BYTE*)malloc(esize);
		bool ok = zbuf && ebuf
		       && _fseeki64(g_cmFile, g_cmDataOfs + zpos, SEEK_SET) == 0
		       && fread(zbuf, 1, zsize, g_cmFile) == zsize
		       && oapiInflate(zbuf, zsize, ebuf, esize) == esize;
		if (zbuf) free(zbuf);

		int slot = -1;
		if (ok) {
			const DWORD ddsMagic = *(DWORD*)ebuf;
			const DWORD h = *(DWORD*)(ebuf + 12), w = *(DWORD*)(ebuf + 16);
			const DWORD fourcc = *(DWORD*)(ebuf + 84);
			if (ddsMagic != 0x20534444u || w != 512 || h != 512 || fourcc != 0x35545844u
			    || esize < 128 + 512 * 512) {
				ok = false; absent = true;
			} else {
				slot = 0;
				for (int i = 1; i < CM_FULL_TILE_N; i++)
					if (g_cmFull[i].lvl <= 0 || (g_cmFull[slot].lvl > 0 && g_cmFull[i].stamp < g_cmFull[slot].stamp))
						slot = i;
				FTile& T = g_cmFull[slot];
				T.lvl = lvl; T.ilat = ilat; T.ilng = ilng; T.stamp = ++g_cmFullStamp;
				const BYTE* blk = ebuf + 128;
				// full per-texel DXT5 alpha decode - all 16 values per block this time
				for (int by = 0; by < 128; by++) {
					for (int bx = 0; bx < 128; bx++) {
						const BYTE* B = blk + ((size_t)by * 128 + bx) * 16;
						const int a0 = B[0], a1 = B[1];
						int av[8];
						av[0] = a0; av[1] = a1;
						if (a0 > a1) { for (int k = 1; k <= 6; k++) av[k + 1] = ((7 - k) * a0 + k * a1) / 7; }
						else         { for (int k = 1; k <= 4; k++) av[k + 1] = ((5 - k) * a0 + k * a1) / 5; av[6] = 0; av[7] = 255; }
						unsigned __int64 bits = 0;
						memcpy(&bits, B + 2, 6);
						for (int t = 0; t < 16; t++) {
							const int px = bx * 4 + (t & 3), py = by * 4 + (t >> 2);
							T.a[py * 512 + px] = (BYTE)av[(int)((bits >> (3 * t)) & 7)];
						}
					}
				}
			}
		} else absent = true;
		if (ebuf) free(ebuf);
		return (ok && slot >= 0) ? g_cmFull[slot].a : NULL;
	}

	// Coverage at full texel resolution (the bake's sampler). Tries CM_FULL_LVL; a
	// genuinely missing tile falls back to the block-grid walk (always available).
	float CmCoverageFine(double lat, double lonT)
	{
		if (!g_cmOpenOK) return -1.0f;
		while (lonT >  PI) lonT -= 2.0 * PI;
		while (lonT < -PI) lonT += 2.0 * PI;
		const int lvl  = CM_FULL_LVL;
		const int nlat = 1 << (lvl - 4);
		const int nlng = 2 << (lvl - 4);
		double v = (0.5 - lat / PI) * nlat;
		double u = (lonT + PI) / (2.0 * PI) * nlng;
		int ilat = (int)floor(v); if (ilat < 0) ilat = 0; if (ilat >= nlat) ilat = nlat - 1;
		int ilng = (int)floor(u); ilng = ((ilng % nlng) + nlng) % nlng;
		bool absent = false;
		const BYTE* g = CmTileFull(lvl, ilat, ilng, absent);
		if (!g) return CmCoverage(lat, lonT, true);      // fall back to the block grids
		double fx = (u - ilng) * 512.0 - 0.5;
		double fy = (v - ilat) * 512.0 - 0.5;
		int x0 = (int)floor(fx), y0 = (int)floor(fy);
		const double tx = fx - x0, ty = fy - y0;
		int x1 = x0 + 1, y1 = y0 + 1;
		if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
		if (x1 > 511) x1 = 511; if (y1 > 511) y1 = 511;
		if (x0 > 511) x0 = 511; if (y0 > 511) y0 = 511;
		const double a00 = g[y0 * 512 + x0], a10 = g[y0 * 512 + x1];
		const double a01 = g[y1 * 512 + x0], a11 = g[y1 * 512 + x1];
		return (float)(((a00 * (1 - tx) + a10 * tx) * (1 - ty)
		              + (a01 * (1 - tx) + a11 * tx) * ty) / 255.0);
	}

	// The tangent basis at a cell, PLANET-LOCAL - shared by the geometry AND the bake
	// so texture texels and screen vertices agree about where a metre offset lies.
	void CellBasis(double latT, double lonT, double crot, VECTOR3& dLoc, VECTOR3& e1, VECTOR3& e2)
	{
		const double lonP = lonT - crot;              // texture -> planet longitude
		const double cl = cos(latT), sl = sin(latT);
		dLoc = _V(cl * cos(lonP), sl, cl * sin(lonP));
		e1 = crossp(_V(0, 1, 0), dLoc);
		const double e1l = length(e1);
		if (e1l < 1e-6) e1 = _V(1, 0, 0); else e1 = e1 / e1l;
		e2 = crossp(dLoc, e1);
	}

	// Bake one 128^2 atlas slot: the disc footprint (+-1.15 R around the cell) sampled
	// from the full-res cloud alpha into greyscale X8R8G8B8. The Gouraud vertices keep
	// radial falloff / envelope / tint; the texture is PURE cloud, so it never rebakes
	// during the flash - restrikes re-light the same lobes, which is what real ones do.
	// Texel<->offset convention mirrors the draw path's UV mapping exactly: texel
	// centre (tx+0.5) at 64 +- 62 spans dx = +-1.15 R, with a 1-texel zero border so
	// LINEAR filtering cannot bleed a neighbouring slot in.
	void BakeFlashSlot(DWORD* img, int slot, double latT, double lonT, double radM,
	                   double crot, double Rdeck, bool test, float cov0)
	{
		VECTOR3 dLoc, e1, e2;
		CellBasis(latT, lonT, crot, dLoc, e1, e2);
		const int ox = (slot & 3) * 128, oy = (slot >> 2) * 128;
		const double span = radM * 1.15;
		for (int ty = 0; ty < 128; ty++) {
			for (int tx = 0; tx < 128; tx++) {
				BYTE val = 0;
				if (tx >= 1 && tx <= 126 && ty >= 1 && ty <= 126) {
					if (test) val = 255;
					else {
						const double dx = ((tx + 0.5) - 64.0) / 62.0 * span;
						const double dy = ((ty + 0.5) - 64.0) / 62.0 * span;
						const VECTOR3 pl = dLoc * Rdeck + e1 * dx + e2 * dy;
						const double pll = length(pl);
						const double laV = asin(clampd(pl.y / pll, -1.0, 1.0));
						const double loV = atan2(pl.z, pl.x) + crot;
						float cov = CmCoverageFine(laV, loV);
						if (cov < 0.0f) cov = cov0;
						val = (BYTE)(clampf(cov, 0.0f, 1.0f) * 255.0f + 0.5f);
					}
				}
				img[(size_t)(oy + ty) * 512 + (ox + tx)] =
					0xFF000000u | ((DWORD)val << 16) | ((DWORD)val << 8) | (DWORD)val;
			}
		}
	}

	void CmLandmarkDiag(double crot)
	{
		// Geographic landmarks with unmistakable cloudmap signatures. Sampled at
		// texture lon = geo + crot AND geo - crot: the convention that keeps the
		// Sahara low and the Southern Ocean high is the one the draw path must use.
		static const struct { const char* name; double latDeg, lonDeg; } LM[] = {
			{ "Sahara",    23.0,   10.0 },   // expect LOW
			{ "Congo",      0.0,   23.0 },   // expect high-ish (ITCZ convection)
			{ "Amazon",    -4.0,  -63.0 },   // expect high-ish
			{ "SPacGyre", -25.0, -110.0 },   // expect low-moderate
			{ "SouthOcn",  -57.0,   90.0 },  // expect HIGH (the unbroken storm ring)
			{ "NAtlantic",  52.0,  -25.0 },  // expect moderate-high (storm track)
		};
		char line[512];
		int n = sprintf_s(line, sizeof(line), "ORO: ltg coverage diag (crot %.4f rad):", crot);
		for (int i = 0; i < 6; i++) {
			const float cp = CmCoverage(LM[i].latDeg * RAD, LM[i].lonDeg * RAD + crot, true);
			const float cm = CmCoverage(LM[i].latDeg * RAD, LM[i].lonDeg * RAD - crot, true);
			n += sprintf_s(line + n, sizeof(line) - n, " %s +%.2f/-%.2f", LM[i].name, cp, cm);
		}
		oapiWriteLog(line);
	}

	// The flash ENVELOPE: 0..1 as a function of REAL seconds since trigger. 1-4
	// strokes 45-80 ms apart (each a ~14 ms rise + 55-105 ms exponential decay),
	// first stroke brightest; ~1 in 7 flashes is a long "spider" that flickers 6-8
	// strokes across ~0.8 s - the Izmir-frame signature. Strokes combine by MAX,
	// not sum: it is the same cloud lit again, not two light sources.
	float FlashEnv(float tau, float seed)
	{
		if (tau < 0.0f) return 0.0f;
		int n; float gap;
		if (hashf(seed * 17.3f + 3.0f) < 0.14f) { n = 6 + (int)(hashf(seed + 9.0f) * 2.9f); gap = 0.11f; }
		else { n = 1 + (int)(hashf(seed + 5.0f) * 3.9f); gap = 0.045f + 0.035f * hashf(seed + 7.0f); }
		float e = 0.0f;
		for (int i = 0; i < n; i++) {
			const float dt = tau - (float)i * gap;
			if (dt < 0.0f) break;
			const float amp = (i == 0) ? 1.0f : (0.55f + 0.45f * hashf(seed + (float)i * 3.3f));
			const float tc  = 0.055f + 0.05f * hashf(seed + (float)i * 2.1f);
			const float s   = (dt < 0.014f) ? dt / 0.014f : expf(-(dt - 0.014f) / tc);
			if (amp * s > e) e = amp * s;
		}
		return e;
	}

}  // namespace

// Dialog readout: the km the Cell size slider currently means (same mapping the build uses).
float OroLightning_CellKm()
{
	return LTG_CELL_MIN + (LTG_CELL_MAX - LTG_CELL_MIN) * clampf(g_fx.ltgCellKm, 0.0f, 1.0f);
}

// Module-lifetime teardown (simulation end + destructor): the FILE handle, the
// malloc'd TOC and the decoded-tile cache. Cheap to reopen lazily next session.
void OroLightning_Close()
{
	CmCloseInternal();
}

// ----------------------------------------------------------------------------
// Per frame, MAIN thread. Builds the flash discs into ltgVtx and sets ltgActive;
// the render callback only pushes and draws (DrawLightningPoly).
// ----------------------------------------------------------------------------
void OroModule::UpdateLightning(double simt)
{
	const float animNow  = animT;
	const float animPrev = ltgPrevAnim;
	ltgPrevAnim = animNow;

	// Expire stale flash events UNCONDITIONALLY - transients must age out even while
	// the section is disabled or the camera is elsewhere (the blink-envelope rule).
	for (int i = 0; i < LTG_MAX_FLASH; i++)
		if (ltgFlash[i].startAnim >= 0.0f && animNow - ltgFlash[i].startAnim > 1.35f)
			ltgFlash[i].startAnim = -1.0f;

	ltgVtxN   = 0;
	ltgActive = false;
	g_fx.ltgCells   = 0;
	g_fx.ltgBody[0] = 0;

	if (!g_fx.masterArmed || !g_fx.ltgEnabled) return;
	if (viewW == 0 || viewH == 0) return;

	// TEXTURED MODE (client patch l): probe by BINDING once per session (invariant 18a)
	// and create the atlas - a main-thread resource op, never the render path. Either
	// failing leaves the Gouraud fallback, and the log says which world we are in,
	// because "the flashes look flat" must be diagnosable from the file.
	if (!ltgTexTried && pCore) {
		ltgTexTried = true;
		static_assert(sizeof(TexVtxP) == sizeof(gcCore::texVtx),
		              "TexVtxP layout must match gcCore::texVtx (DrawLightningPoly casts)");
		if (pCore->CanDrawTexPoly())
			hLtgAtlas = oapiCreateSurfaceEx(512, 512, OAPISURFACE_TEXTURE | OAPISURFACE_NOMIPMAPS);
		ltgTexMode = (hLtgAtlas != NULL);
		oapiWriteLogV("ORO: textured flash path (patch l) %s.",
		              ltgTexMode ? "available - flashes take their shape from the cloud map itself"
		                         : "NOT available - Gouraud disc fallback");
	}

	// ---- IDENTIFY THE WORLD FIRST (invariant 17's law: before any draw gate) -----
	// The PROXIMATE body only - deliberately NO nearest-cloudy-world fallback search.
	// Lightning is a property of the world you are AT; and because this finder can
	// never name a DIFFERENT body than the aurora's, the two per-frame
	// OroSettings_LoadBody calls can never thrash the loaded state between worlds.
	OBJHANDLE hP = NULL;
	{
		OBJHANDLE hT = oapiCameraTarget();
		if (hT) {
			if (oapiGetObjectType(hT) == OBJTP_VESSEL) {
				VESSEL* v = oapiGetVesselInterface(hT);
				if (v) hP = v->GetSurfaceRef();
			} else hP = hT;
		}
	}
	if (!hP || oapiGetObjectType(hP) == OBJTP_STAR) return;
	const bool* hasCld = (const bool*)oapiGetObjectParam(hP, OBJPRM_PLANET_HASCLOUDS);
	if (!hasCld || !*hasCld) return;                     // no cloud layer, no lightning
	const double* pCldAlt = (const double*)oapiGetObjectParam(hP, OBJPRM_PLANET_CLOUDALT);
	const double cloudAlt = (pCldAlt && *pCldAlt > 0.0) ? *pCldAlt : 7000.0;

	VECTOR3 O; oapiGetGlobalPos(hP, &O);
	const double R = oapiGetSize(hP);
	if (R < 1.0) return;

	CamCtx cc; GetCam(cc);
	const double camDist = length(cc.pos - O);
	if (camDist > 12.0 * R) return;                      // a 10 km flash is sub-pixel long before this

	oapiGetObjectName(hP, g_fx.ltgBody, sizeof(g_fx.ltgBody));
	OroSettings_LoadBody(g_fx.ltgBody);                // no-op when already loaded (aurora's call)

	// ---- from here on it is only about DRAWING -----------------------------------
	if (!extGate && !(viewGate && depthClipOK)) return;

	// ACTIVITY IS THE OPT-IN (invariant 17b): default 0, so an unconfigured world is
	// silent; TEST works regardless, because it is how you find out the section works.
	const float act = clampf(g_fx.ltgActivity, 0.0f, 1.0f);
	if (act <= 0.001f && !g_fx.ltgTest) return;

	// ABOVE THE DECK ONLY (the v1 scope): fade in across a band above the cloud tops
	// so climbing through them cannot pop the sky on. TEST bypasses - you must be able
	// to see it work from a runway, and the disc stays at deck altitude anyway.
	const double camAlt = camDist - R;
	float altFade = sstepf((float)((camAlt - (cloudAlt + 1500.0)) / 6000.0));
	if (g_fx.ltgTest) altFade = 1.0f;
	if (altFade <= 0.003f) return;

	// The cloud map (lazy per body; a failed open is remembered) + the layer's
	// current rotation. Frame budget: at most one tile decode below.
	double crot = 0.0;
	{
		const double* p = (const double*)oapiGetObjectParam(hP, OBJPRM_PLANET_CLOUDROTATION);
		if (p) crot = *p;
	}
	const bool mapOK = CmOpen(g_fx.ltgBody, crot);
	g_cmDecoded = false;

	MATRIX3 Rp; oapiGetRotationMatrix(hP, &Rp);
	OBJHANDLE hSun = FindStar();
	VECTOR3 Sg = { 0, 0, 0 };
	if (hSun) oapiGetGlobalPos(hSun, &Sg);

	// Sub-camera point, planet frame -> cloud-texture frame. Texture longitude lonT
	// renders at planet longitude lonT - crot (the client's cloud world matrix), so
	// planet -> texture adds crot. THE SIGN IS THE LANDMARK DIAG'S VERDICT.
	const VECTOR3 lc = tmul(Rp, cc.pos - O);
	const double lcl = length(lc);
	if (lcl < 1.0) return;
	const double subLat  = asin(clampd(lc.y / lcl, -1.0, 1.0));
	const double subLngP = atan2(lc.z, lc.x);
	const double subLngT = subLngP + crot;

	// The visible cap, clamped: past ~35 deg of arc a cell is a couple of pixels.
	double capRad = acos(clampd(R / camDist, 0.0, 1.0)) + 0.02;
	if (capRad > 0.61) capRad = 0.61;

	// ---- ACTIVE STORM CELLS: deterministic districts, coverage-gated -------------
	// Districts are a fixed 2.5 deg grid in TEXTURE coordinates; each district hashes
	// (per ~30 min sim epoch, phase-staggered so they never die together) into "hosts
	// a storm or not" plus the storm's offset, size, cadence and intensity. Nothing
	// is stored: the same district at the same sim time is the same storm from any
	// camera - a planetary phenomenon evaluated only where it can be seen.
	struct Cell {
		double latT, lonT, radM, ang;
		float  inten, period, phaseR, cov;
		bool   test;
	};
	Cell cells[LTG_MAX_CELL];
	int  nCell = 0;

	const float cellKm = LTG_CELL_MIN + (LTG_CELL_MAX - LTG_CELL_MIN) * clampf(g_fx.ltgCellKm, 0.0f, 1.0f);

	if (g_fx.ltgTest) {
		Cell& c = cells[nCell++];
		c.latT  = clampd(subLat + LTG_TEST_COLAT, -PI05 + 0.05, PI05 - 0.05);
		c.lonT  = subLngT;
		c.radM  = (double)cellKm * 1250.0;      // a touch larger than the slider says: the
		                                        //   test cell is for judging the look
		c.ang   = LTG_TEST_COLAT;
		c.inten = 1.0f; c.period = 1.4f; c.phaseR = 0.0f; c.cov = 1.0f; c.test = true;
	}

	if (act > 0.001f) {
		const int NJ = (int)(2.0 * PI / LTG_DGRID + 0.5);    // 144 columns
		const int NI = (int)(PI / LTG_DGRID + 0.5);          // 72 rows
		int iLat0 = (int)floor((PI05 - (subLat + capRad)) / LTG_DGRID);
		int iLat1 = (int)floor((PI05 - (subLat - capRad)) / LTG_DGRID);
		if (iLat0 < 0) iLat0 = 0;
		if (iLat1 > NI - 1) iLat1 = NI - 1;
		for (int di = iLat0; di <= iLat1; di++) {
			const double latC = PI05 - (di + 0.5) * LTG_DGRID;
			const double cosl = cos(latC);
			if (cosl < 0.06) continue;
			double dLon = capRad / cosl;
			if (dLon > PI) dLon = PI;
			const int jSpan = (int)ceil(dLon / LTG_DGRID);
			const int j0 = (int)floor((subLngT + PI) / LTG_DGRID);
			for (int jj = -jSpan; jj <= jSpan; jj++) {
				const int dj = ((j0 + jj) % NJ + NJ) % NJ;

				// Does this district host a storm this epoch? Per-district phase stagger,
				// then one hash against activity (cos-lat weighted so the fixed angular
				// grid does not over-populate high latitudes).
				const double phase = (double)hashf((float)(di * 7 + dj) * 1.317f + 0.71f) * LTG_LIFE;
				const double e     = floor((simt + phase) / LTG_LIFE);
				const float  s0    = hashf((float)di * 3.17f + (float)dj * 7.73f + (float)e * 13.31f + 0.5f);
				if (s0 > act * 0.55f * (float)cosl) continue;

				const float h3 = hashf(s0 * 29.3f + 3.0f);
				const float h4 = hashf(s0 * 91.1f + 4.0f);

				// storm life fade (sim time - this is weather, not animation)
				const double age01 = fmod(simt + phase, LTG_LIFE) / LTG_LIFE;
				const float lifeAmp = sstepf((float)(age01 / 0.15)) * sstepf((float)((1.0 - age01) / 0.15));
				if (lifeAmp < 0.05f) continue;

				// THE COVERAGE GATE - the point of the whole reader - with the v1.1 fix
				// (user's own diagnosis): the storm forms where the CLOUD is, not where
				// the dice landed. Up to three deterministic candidate positions per
				// district, keeping the best-covered one; a single unlucky hole in a
				// broken deck no longer silences a stormy district. Unknown coverage
				// (-1: budget warming, or no map) still fails CLOSED.
				double latT = 0, lonT = 0, ang = 0;
				float cov = -1.0f;
				for (int k = 0; k < 3; k++) {
					const float hk1 = hashf(s0 * 61.7f + 1.0f + (float)k * 17.9f);
					const float hk2 = hashf(s0 * 43.9f + 2.0f + (float)k * 23.3f);
					const double la = PI05 - ((double)di + 0.15 + 0.70 * hk1) * LTG_DGRID;
					const double lo = -PI  + ((double)dj + 0.15 + 0.70 * hk2) * LTG_DGRID;
					// inside the cap? (texture and planet frames share angular distances)
					const double an = acos(clampd(sin(la) * sin(subLat)
					                            + cos(la) * cos(subLat) * cos(lo - subLngT), -1.0, 1.0));
					if (an > capRad) continue;
					const float cv = mapOK ? CmCoverage(la, lo, false) : -1.0f;
					if (cv > cov) { cov = cv; latT = la; lonT = lo; ang = an; }
				}
				if (cov < LTG_COV_SPAWN) continue;

				Cell c;
				c.latT   = latT;
				c.lonT   = lonT;
				c.radM   = (double)cellKm * 1000.0 * (0.70 + 0.60 * h3);
				c.ang    = ang;
				c.inten  = lifeAmp * (0.55f + 0.45f * h4) * clampf((cov - 0.30f) / 0.55f, 0.35f, 1.0f);
				c.period = (14.0f - 11.0f * clampf(g_fx.ltgRate, 0.0f, 1.0f)) * (0.65f + 0.90f * hashf(s0 * 11.7f + 5.0f));
				c.phaseR = hashf(s0 * 5.13f + 6.0f);
				c.cov    = cov;
				c.test   = false;

				// keep the NEAREST cells when the cap saturates (a row-major scan would
				// otherwise bias every full sky toward its northern edge)
				if (nCell < LTG_MAX_CELL) cells[nCell++] = c;
				else {
					int worst = 0;
					for (int k = 1; k < LTG_MAX_CELL; k++)
						if (cells[k].ang > cells[worst].ang) worst = k;
					if (!cells[worst].test && c.ang < cells[worst].ang) cells[worst] = c;
				}
			}
		}
	}
	g_fx.ltgCells = nCell;

	// ---- FLASH TRIGGERING: real-time Poisson-ish per cell ------------------------
	// Scheduling runs on the REAL clock deliberately (see the header): warp cannot
	// strobe the sky, and a paused sim keeps flickering. Trigger = the cell's flash
	// ordinal k crossing between last frame's animT and this one's; at most one new
	// flash per cell per frame, and a few per frame globally.
	const double Rdeck = R + cloudAlt + 350.0;   // a hair above the deck: never coincident
	int fired = 0;
	for (int ci = 0; ci < nCell && fired < 3; ci++) {
		const Cell& c = cells[ci];
		const float j = c.phaseR * c.period;
		const int kPrev = (int)floorf((animPrev - j) / c.period);
		const int kNow  = (int)floorf((animNow  - j) / c.period);
		if (kNow <= kPrev) continue;
		// a slot: free first, else the OLDEST (a very old event is nearly dark anyway)
		int slot = -1, oldest = 0;
		for (int i = 0; i < LTG_MAX_FLASH; i++) {
			if (ltgFlash[i].startAnim < 0.0f) { slot = i; break; }
			if (ltgFlash[i].startAnim < ltgFlash[oldest].startAnim) oldest = i;
		}
		if (slot < 0) slot = oldest;
		LtgFlash& fl = ltgFlash[slot];
		fl.latT = c.latT; fl.lonT = c.lonT; fl.radM = c.radM;
		fl.startAnim = animNow;
		fl.seed = hashf((float)(c.latT * 57.29578) * 3.7f + (float)(c.lonT * 57.29578) * 1.9f + (float)kNow * 0.73f);
		fl.amp  = c.inten;
		fl.cov0 = c.cov;
		fl.test = c.test;
		// TEXTURED MODE: bake this flash's atlas slot from the full-res cloud alpha
		// (event-driven, only at trigger - restrikes re-light the same baked lobes,
		// which is what real ones do) and upload the atlas. An upload failure drops
		// the session to the Gouraud fallback once and LOUDLY - a silently black
		// flash is a bug report waiting to happen.
		if (ltgTexMode) {
			BakeFlashSlot(ltgAtlasImg, slot, fl.latT, fl.lonT, fl.radM, crot, Rdeck, fl.test, fl.cov0);
			if (!pCore->UpdateTexture2D(hLtgAtlas, ltgAtlasImg, 512, 512)) {
				ltgTexMode = false;
				oapiWriteLog("ORO: lightning atlas upload FAILED - Gouraud disc fallback from here on.");
			}
		}
		fired++;
	}

	// ---- GEOMETRY: one soft disc per live flash ----------------------------------
	// Rings at 0 / 0.35 / 0.70 / 1.15 x radius, LTG_SEC sectors, Gouraud alpha
	// falling outward. TEXTURED mode (patch l): the per-pixel cloud shape comes from
	// the flash's baked atlas slot (texture x Gouraud in the pad's modulate band), so
	// per-vertex coverage shaping is OFF - it would square the mask - and the sector
	// mottle drops to a mild restrike shimmer. Gouraud fallback: per-vertex coverage
	// and full mottle carry the shape, exactly the v1 look.
	const bool texMode = ltgTexMode;

	int rT, gT, bT;
	UnpackCR(g_fx.ltgColour, rT, gT, bT);
	// the hot core leans toward white - a blown-out centre is the McClain-frame look
	const int rC = rT + (int)((255 - rT) * 0.60f);
	const int gC = gT + (int)((255 - gT) * 0.60f);
	const int bC = bT + (int)((255 - bT) * 0.60f);

	const float bright = clampf(g_fx.ltgBright, 0.0f, 1.0f);

	for (int fi = 0; fi < LTG_MAX_FLASH; fi++) {
		LtgFlash& fl = ltgFlash[fi];
		if (fl.startAnim < 0.0f) continue;
		const float env = FlashEnv(animNow - fl.startAnim, fl.seed);
		if (env < 0.012f) continue;

		// cell basis, PLANET-LOCAL - the SAME helper the bake used, so texture texels
		// and screen vertices agree about where a metre offset lies.
		VECTOR3 dLoc, e1, e2;
		CellBasis(fl.latT, fl.lonT, crot, dLoc, e1, e2);

		// this flash's atlas slot centre, in atlas TEXELS (slot i is flash i's, 4x4 grid)
		const double axc = (double)((fi & 3) * 128 + 64), ayc = (double)((fi >> 2) * 128 + 64);
		const double uvK = 62.0 / (fl.radM * 1.15);      // metres -> texels within the slot

		// night at the centre: flashes are a night effect from orbit; a little
		// visibility past the terminator, then daylight drowns them.
		const VECTOR3 Pc = O + mul(Rp, dLoc * Rdeck);
		float night = 1.0f;
		if (!fl.test) {
			const double elev = dotp(mul(Rp, dLoc), unit(Sg - Pc));
			night = 1.0f - sstepf((float)((elev + 0.10) / 0.20));
		}
		if (night <= 0.004f) continue;

		const float aPeak = LTG_ALPHA * bright * env * fl.amp * night * altFade;
		if (aPeak < 1.5f) continue;

		// project the 1 + 3*LTG_SEC vertices
		float  vx[1 + 3 * LTG_SEC], vy[1 + 3 * LTG_SEC], vz[1 + 3 * LTG_SEC], va[1 + 3 * LTG_SEC];
		float  vu[1 + 3 * LTG_SEC], vv[1 + 3 * LTG_SEC];
		DWORD  vc[1 + 3 * LTG_SEC];
		bool   vok[1 + 3 * LTG_SEC];

		auto vertex = [&](int vi, double rr, int ring, int sec) {
			const double th = 2.0 * PI * (double)sec / (double)LTG_SEC;
			const double dx = cos(th) * rr, dy = sin(th) * rr;
			const VECTOR3 pl = dLoc * Rdeck + e1 * dx + e2 * dy;
			const VECTOR3 P  = O + mul(Rp, pl);
			double pz = 1.0;
			vok[vi] = ProjPx(cc, P, viewW, viewH, vx[vi], vy[vi], pz);
			vz[vi] = (float)length(P - cc.pos);          // Euclidean, the patch-(g) convention
			vu[vi] = (float)(axc + dx * uvK);            // atlas texels (the pad's convention)
			vv[vi] = (float)(ayc + dy * uvK);
			float a = 0.0f;
			if (vok[vi]) {
				const float occ = Occluded(cc.pos, P, O, R) ? 0.0f : 1.0f;
				// GOURAUD FALLBACK ONLY: coverage under THIS vertex (texture mode
				// carries the cloud shape per PIXEL in the atlas instead)
				float covMul = 1.0f;
				if (!texMode && !fl.test && ring > 0) {
					const double pll = length(pl);
					const double laV = asin(clampd(pl.y / pll, -1.0, 1.0));
					const double loV = atan2(pl.z, pl.x) + crot;
					float cov = CmCoverage(laV, loV, false);
					if (cov < 0.0f) cov = fl.cov0;
					covMul = 0.15f + 0.85f * sstepf((cov - 0.15f) / 0.40f);
				}
				// per-sector mottle, stable for this flash (restrikes re-light the same
				// lobes). Mild in texture mode - the real mottle is the cloud's own.
				const float mot = (ring == 0) ? 1.0f
				                : texMode ? 0.92f + 0.16f * hashf(fl.seed * 31.7f + (float)sec * 2.71f + (float)ring * 0.9f)
				                          : 0.78f + 0.35f * hashf(fl.seed * 31.7f + (float)sec * 2.71f + (float)ring * 0.9f);
				a = aPeak * LTG_RING_A[ring] * covMul * mot * occ;
			}
			va[vi] = a;
			const bool core = (ring == 0);
			vc[vi] = ACol(core ? rC : rT, core ? gC : gT, core ? bC : bT, (int)(a + 0.5f));
		};

		vertex(0, 0.0, 0, 0);
		for (int ring = 1; ring <= 3; ring++)
			for (int s = 0; s < LTG_SEC; s++)
				vertex(1 + (ring - 1) * LTG_SEC + s, fl.radM * (double)LTG_RING_R[ring], ring, s);

		if (!vok[0]) continue;
		auto RIX = [&](int ring, int s) { return 1 + (ring - 1) * LTG_SEC + ((s % LTG_SEC + LTG_SEC) % LTG_SEC); };

		// one emitter, both modes: indices into the staging arrays above. The two
		// vertex arrays share ltgVtxN and ltgDepth (only one is active per session).
		auto EMIT = [&](int a, int b, int c3) {
			if (ltgVtxN + 3 > LTG_MAX_TRI * 3) return;
			const int idx[3] = { a, b, c3 };
			for (int k = 0; k < 3; k++) {
				const int i = idx[k];
				if (texMode) {
					ltgTexVtx[ltgVtxN].x = vx[i]; ltgTexVtx[ltgVtxN].y = vy[i];
					ltgTexVtx[ltgVtxN].u = vu[i]; ltgTexVtx[ltgVtxN].v = vv[i];
					ltgTexVtx[ltgVtxN].c = vc[i];
				} else {
					ltgVtx[ltgVtxN].x = vx[i]; ltgVtx[ltgVtxN].y = vy[i];
					ltgVtx[ltgVtxN].c = vc[i];
				}
				ltgDepth[ltgVtxN] = vz[i];
				ltgVtxN++;
			}
		};

		// centre fan + two ring bands; the aurora's skip rule (all corners dark = skip)
		for (int s = 0; s < LTG_SEC; s++) {
			const int i1 = RIX(1, s), i2 = RIX(1, s + 1);
			if (vok[i1] && vok[i2] && (va[0] >= 2.0f || va[i1] >= 2.0f || va[i2] >= 2.0f))
				EMIT(0, i1, i2);
		}
		for (int ring = 1; ring <= 2; ring++) {
			for (int s = 0; s < LTG_SEC; s++) {
				const int a0 = RIX(ring, s), a1 = RIX(ring, s + 1);
				const int b0 = RIX(ring + 1, s), b1 = RIX(ring + 1, s + 1);
				if (!(vok[a0] && vok[a1] && vok[b0] && vok[b1])) continue;
				float amax = va[a0];
				if (va[a1] > amax) amax = va[a1];
				if (va[b0] > amax) amax = va[b0];
				if (va[b1] > amax) amax = va[b1];
				if (amax < 2.0f) continue;
				EMIT(a0, b0, b1);
				EMIT(a0, b1, a1);
			}
		}
	}

	// Pool-full warning, once (the aurora's rule: a silently clipped effect looks
	// like a bug report, a logged one is a budget).
	{
		static bool warned = false;
		if (!warned && ltgVtxN >= LTG_MAX_TRI * 3) {
			warned = true;
			oapiWriteLogV("ORO: lightning triangle pool FULL (%d tri) - flashes are being clipped.", LTG_MAX_TRI);
		}
	}

	// Zero-pad the unused tail of the ACTIVE vertex array (invariant 3):
	// D3DLOCK_DISCARD + full creation-count draw means an unwritten tail is random
	// triangles. Zeroed TexVtxP verts are alpha-0 degenerates like zeroed PlasVtx.
	if (ltgVtxN > 0 && ltgVtxN < LTG_MAX_TRI * 3) {
		if (texMode) memset(&ltgTexVtx[ltgVtxN], 0, sizeof(TexVtxP) * (LTG_MAX_TRI * 3 - ltgVtxN));
		else         memset(&ltgVtx[ltgVtxN],    0, sizeof(PlasVtx) * (LTG_MAX_TRI * 3 - ltgVtxN));
		memset(&ltgDepth[ltgVtxN], 0, sizeof(float) * (LTG_MAX_TRI * 3 - ltgVtxN));
	}

	ltgActive = (ltgVtxN > 0);

	// One-shot breadcrumb, the DrawOverlay discipline: "the effect emitted at least
	// once this run" - tells a dark night sky from a pipeline that never fired.
	{
		static bool logged = false;
		if (!logged && ltgVtxN > 0) {
			logged = true;
			oapiWriteLogV("ORO: lightning first flash on screen - %d verts, %d cells in cap, %s mode.",
			              ltgVtxN, nCell, texMode ? "TEXTURED (cloud-lit)" : "Gouraud");
		}
	}
}
