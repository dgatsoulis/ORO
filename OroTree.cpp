// ==============================================================
// OroTree.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - THE TILE-TREE READER (2026-09-07). See OroTree.h for what it is and why.
// Own code against the archive's struct layout rather than a lift of the client's
// LGPL ZTreeMgr; the zlib blocks go through oapiInflate, a PUBLIC core call.
// ============================================================================

#include "OroTree.h"
#include "OroLog.h"
#include "Orbitersdk.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

OroTileTree::OroTileTree()
{
	m_body[0] = 0; m_layer[0] = 0; m_who[0] = 0;
	m_openOK = false; m_file = NULL; m_dataOfs = 0; m_dataLen = 0; m_nodeN = 0;
	m_root4[0] = m_root4[1] = (DWORD)-1;
	m_toc = NULL;
	for (int i = 0; i < TILE_N; i++) m_tile[i].lvl = -1;
	m_stamp = 0; m_decoded = false; m_fmtWarned = false; m_fmt = 0;
}

OroTileTree::~OroTileTree()
{
	Close();
}

void OroTileTree::Close()
{
	if (m_file) { fclose(m_file); m_file = NULL; }
	if (m_toc)  { free(m_toc);   m_toc  = NULL; }
	for (int i = 0; i < TILE_N; i++) m_tile[i].lvl = -1;
	m_openOK = false; m_fmt = 0; m_fmtWarned = false;
	m_body[0] = 0;
}

bool OroTileTree::Open(const char* body, const char* layer, const char* who)
{
	if (_stricmp(m_body, body) == 0 && _stricmp(m_layer, layer) == 0) return m_openOK;   // cached, failure included
	Close();
	strcpy_s(m_body,  sizeof(m_body),  body);
	strcpy_s(m_layer, sizeof(m_layer), layer);
	strcpy_s(m_who,   sizeof(m_who),   who);

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
	sprintf_s(path, sizeof(path), "%s\\%s\\Archive\\%s.tree", texDir, body, layer);
	if (fopen_s(&m_file, path, "rb") != 0 || !m_file) {
		// LEVEL 1, NOT 0: most worlds simply HAVE no water mask or cloud map, so this is
		// a fact about the body rather than a failure - it fired twice at Saturn on its
		// first flight. It stays worth saying at the default level, because it is the
		// answer to "why does the vapour cone never form here", but a user who has asked
		// for quiet should not be told about every archive the solar system lacks.
		OroLog(1, "ORO: %s - no %s archive at %s.", who, layer, path);
		m_file = NULL;
		return false;
	}

	// Header: fields in write order (verified stride; struct size / magic checked).
	DWORD magic = 0, hsize = 0, flags = 0, dataOfs32 = 0, nodeCount = 0;
	__int64 dataLen = 0;
	DWORD r1, r2, r3;
	bool ok = fread(&magic,     4, 1, m_file) == 1 && magic == 0x00015854u   // 'T','X',1,0
	       && fread(&hsize,     4, 1, m_file) == 1 && hsize == 48
	       && fread(&flags,     4, 1, m_file) == 1
	       && fread(&dataOfs32, 4, 1, m_file) == 1
	       && fread(&dataLen,   8, 1, m_file) == 1
	       && fread(&nodeCount, 4, 1, m_file) == 1
	       && fread(&r1,        4, 1, m_file) == 1
	       && fread(&r2,        4, 1, m_file) == 1
	       && fread(&r3,        4, 1, m_file) == 1
	       && fread(m_root4,    4, 2, m_file) == 2
	       && nodeCount > 0 && nodeCount < 4000000;
	if (!ok) {
		OroLog(0, "ORO: %s - %s is not a tile tree (bad header).", who, path);
		fclose(m_file); m_file = NULL;
		return false;                          // m_body stays: the failure is remembered
	}

	// TOC: nodeCount records at a 32-byte stride (8 pos + 4 size + 16 child + 4 pad,
	// the C struct's own padding, confirmed empirically: (dataOfs-48)/nodeCount == 32).
	m_toc = (Node*)malloc(sizeof(Node) * nodeCount);
	BYTE* raw = (BYTE*)malloc((size_t)nodeCount * 32);
	ok = m_toc && raw && fread(raw, 32, nodeCount, m_file) == nodeCount;
	if (ok) {
		for (DWORD i = 0; i < nodeCount; i++) {
			const BYTE* p = raw + (size_t)i * 32;
			memcpy(&m_toc[i].pos,   p,      8);
			memcpy(&m_toc[i].size,  p + 8,  4);
			memcpy(m_toc[i].child,  p + 12, 16);
		}
	}
	if (raw) free(raw);
	if (!ok) {
		OroLog(0, "ORO: %s - failed reading %s TOC.", who, path);
		if (m_toc) { free(m_toc); m_toc = NULL; }
		fclose(m_file); m_file = NULL;
		return false;
	}
	m_dataOfs = (__int64)dataOfs32;
	m_dataLen = dataLen;
	m_nodeN   = nodeCount;
	m_openOK  = true;
	OroLog(1, "ORO: %s - %s map open - %s (%u nodes).", who, layer, path, nodeCount);
	return true;
}

DWORD OroTileTree::Idx(int lvl, int ilat, int ilng) const
{
	if (!m_toc || lvl < 4) return (DWORD)-1;
	DWORD idx = m_root4[(ilng >> (lvl - 4)) & 1];
	for (int l = 5; l <= lvl; l++) {
		if (idx == (DWORD)-1) return idx;
		const int bit = lvl - l;
		const int c = (((ilat >> bit) & 1) << 1) + ((ilng >> bit) & 1);
		idx = m_toc[idx].child[c];
	}
	return idx;
}

// Decode one tile's alpha into a GRID^2 byte grid: the average of all 16 texels of
// each 4x4 block (DXT5: the interpolated alpha ramp; DXT1: the one-bit alpha - a
// block's transparent code exists only in its c0 <= c1 mode). Returns the cache
// slot's grid, or NULL: *absent tells a missing tile (walk up a level) from a spent
// decode budget (unknown this frame - try again next frame).
const BYTE* OroTileTree::Tile(int lvl, int ilat, int ilng, bool& absent, bool force)
{
	absent = false;
	for (int i = 0; i < TILE_N; i++) {
		if (m_tile[i].lvl == lvl && m_tile[i].ilat == ilat && m_tile[i].ilng == ilng) {
			m_tile[i].stamp = ++m_stamp;
			return m_tile[i].a;
		}
	}
	if (!m_openOK) { absent = true; return NULL; }
	if (m_decoded && !force) return NULL;                // budget spent this frame

	const DWORD idx = Idx(lvl, ilat, ilng);
	if (idx == (DWORD)-1 || idx >= m_nodeN || m_toc[idx].size == 0) { absent = true; return NULL; }

	const __int64 zpos  = m_toc[idx].pos;
	const __int64 znext = (idx + 1 < m_nodeN) ? m_toc[idx + 1].pos : m_dataLen;
	const DWORD   zsize = (DWORD)(znext - zpos);
	const DWORD   esize = m_toc[idx].size;
	if (zsize == 0 || zsize > 8u * 1024 * 1024 || esize > 8u * 1024 * 1024) { absent = true; return NULL; }

	BYTE* zbuf = (BYTE*)malloc(zsize);
	BYTE* ebuf = (BYTE*)malloc(esize);
	bool ok = zbuf && ebuf
	       && _fseeki64(m_file, m_dataOfs + zpos, SEEK_SET) == 0
	       && fread(zbuf, 1, zsize, m_file) == zsize
	       && oapiInflate(zbuf, zsize, ebuf, esize) == esize;
	if (zbuf) free(zbuf);
	m_decoded = true;                                    // a miss costs the budget too

	int slot = -1;
	if (ok) {
		const DWORD ddsMagic = *(DWORD*)ebuf;                    // 'DDS '
		const DWORD h = *(DWORD*)(ebuf + 12), w = *(DWORD*)(ebuf + 16);
		const DWORD fourcc = *(DWORD*)(ebuf + 84);
		const bool  dxt5 = (fourcc == 0x35545844u);              // 'DXT5'
		const bool  dxt1 = (fourcc == 0x31545844u);              // 'DXT1'
		const DWORD need = 128 + (dxt5 ? (DWORD)GRID * GRID * 16 : (DWORD)GRID * GRID * 8);
		if (ddsMagic != 0x20534444u || w != TEX || h != TEX || !(dxt5 || dxt1) || esize < need) {
			if (!m_fmtWarned) {
				m_fmtWarned = true;
				OroLog(1, "ORO: %s - unexpected %s tile format (%ux%u fourcc 0x%08X) - skipping.",
				              m_who, m_layer, w, h, fourcc);
			}
			ok = false;
			absent = true;
		} else {
			if (m_fmt == 0) {
				m_fmt = dxt5 ? 5 : 1;
				OroLog(1, "ORO: %s - %s tiles are 512x512 DXT%d.", m_who, m_layer, m_fmt);
			}
			// LRU victim
			slot = 0;
			for (int i = 1; i < TILE_N; i++)
				if (m_tile[i].lvl < 0 || (m_tile[slot].lvl >= 0 && m_tile[i].stamp < m_tile[slot].stamp))
					slot = i;
			DTile& T = m_tile[slot];
			T.lvl = lvl; T.ilat = ilat; T.ilng = ilng; T.stamp = ++m_stamp;
			const BYTE* blk = ebuf + 128;
			if (dxt5) {
				for (int by = 0; by < GRID; by++) {
					for (int bx = 0; bx < GRID; bx++) {
						const BYTE* B = blk + ((size_t)by * GRID + bx) * 16;
						const int a0 = B[0], a1 = B[1];
						int av[8];
						av[0] = a0; av[1] = a1;
						if (a0 > a1) { for (int k = 1; k <= 6; k++) av[k + 1] = ((7 - k) * a0 + k * a1) / 7; }
						else         { for (int k = 1; k <= 4; k++) av[k + 1] = ((5 - k) * a0 + k * a1) / 5; av[6] = 0; av[7] = 255; }
						unsigned __int64 bits = 0;
						memcpy(&bits, B + 2, 6);
						int sum = 0;
						for (int t = 0; t < 16; t++) sum += av[(int)((bits >> (3 * t)) & 7)];
						T.a[by * GRID + bx] = (BYTE)(sum >> 4);
					}
				}
			} else {
				for (int by = 0; by < GRID; by++) {
					for (int bx = 0; bx < GRID; bx++) {
						const BYTE* B = blk + ((size_t)by * GRID + bx) * 8;
						const unsigned c0 = B[0] | (B[1] << 8), c1 = B[2] | (B[3] << 8);
						int opaque = 16;
						if (c0 <= c1) {                              // the mode with a transparent code
							DWORD bits = 0;
							memcpy(&bits, B + 4, 4);
							for (int t = 0; t < 16; t++) if (((bits >> (2 * t)) & 3) == 3) opaque--;
						}
						T.a[by * GRID + bx] = (BYTE)((opaque * 255) / 16);
					}
				}
			}
		}
	} else absent = absent || !ok;
	if (ebuf) free(ebuf);
	return (ok && slot >= 0) ? m_tile[slot].a : NULL;
}

// Alpha 0..1 at (lat, lon) - the layer's own frame - or -1 when unknown this frame
// (decode budget). Walks up from `lvl` when a level is genuinely absent. Tile
// addressing per the client's Tile::Extents: ilat 0 at the NORTH edge, ilng 0 at
// longitude -180 running east (verified against the real archives by probe).
float OroTileTree::Sample(double lat, double lon, int lvl, bool force)
{
	if (!m_openOK) return -1.0f;
	while (lon >  PI) lon -= 2.0 * PI;
	while (lon < -PI) lon += 2.0 * PI;
	for (; lvl >= 4; lvl--) {
		const int nlat = 1 << (lvl - 4);
		const int nlng = 2 << (lvl - 4);
		double v = (0.5 - lat / PI) * nlat;
		double u = (lon + PI) / (2.0 * PI) * nlng;
		int ilat = (int)floor(v); if (ilat < 0) ilat = 0; if (ilat >= nlat) ilat = nlat - 1;
		int ilng = (int)floor(u); ilng = ((ilng % nlng) + nlng) % nlng;
		bool absent = false;
		const BYTE* g = Tile(lvl, ilat, ilng, absent, force);
		if (g) {
			double fx = (u - ilng) * GRID - 0.5;
			double fy = (v - ilat) * GRID - 0.5;
			int x0 = (int)floor(fx), y0 = (int)floor(fy);
			const double tx = fx - x0, ty = fy - y0;
			int x1 = x0 + 1, y1 = y0 + 1;
			if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
			if (x1 > GRID - 1) x1 = GRID - 1;
			if (y1 > GRID - 1) y1 = GRID - 1;
			// ⚠️ AND ON THE LOW SIDE (2026-09-12). x1/y1 were clamped at the top only,
			// so a latitude outside [-90, +90] - which drives ilat into its own clamp and
			// leaves fy far negative - left y1 NEGATIVE and read g[] BEFORE the buffer.
			// An out-of-bounds read, not a wrong answer. No caller does that today (the
			// lightning derives lat from a sub-camera point; the vapour cone clamps to
			// +-(90 - 1e-4) before it samples), which is why it has never shown - it was
			// found by a differential probe that fed it 180 deg deliberately.
			// INERT FOR EVERY REACHABLE INPUT: with lat in range, fy >= -0.5, so y0 >= -1
			// and y1 >= 0 always, and these two lines never fire.
			if (x1 < 0) x1 = 0;
			if (y1 < 0) y1 = 0;
			if (x0 > GRID - 1) x0 = GRID - 1;
			if (y0 > GRID - 1) y0 = GRID - 1;
			const double a00 = g[y0 * GRID + x0], a10 = g[y0 * GRID + x1];
			const double a01 = g[y1 * GRID + x0], a11 = g[y1 * GRID + x1];
			const double a = (a00 * (1 - tx) + a10 * tx) * (1 - ty)
			               + (a01 * (1 - tx) + a11 * tx) * ty;
			return (float)(a / 255.0);
		}
		if (!absent) return -1.0f;                       // budget-starved: unknown
	}
	return -1.0f;
}
