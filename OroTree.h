// ==============================================================
// OroTree.h
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - THE TILE-TREE READER (2026-09-07), the shared service.
// ----------------------------------------------------------------------------
// Orbiter keeps a planet's surface layers in Textures\<body>\Archive\<layer>.tree:
// Surf, Mask, Elev, Cloud, Label - one quadtree archive each, the client's ZTreeMgr
// format (a 48-byte header, 32-byte TOC nodes, per-node zlib blocks holding a DDS).
// ORO first read one of them for the LIGHTNING (2026-08-09, OroLightning.cpp: the
// storms form where the planet's OWN cloud map has cloud), and the weather chapter
// needs the same reader pointed at other layers: the VAPOUR CONE reads the WATER MASK
// (the Mask layer's alpha - the terrain shader's own specular/sea term, 0 = water) to
// know whether a hull is over the sea, and the weather model will read the cloud
// layer for everyone. So the reader lives here as a CLASS, one instance per consumer
// and layer. The lightning keeps its own copy of the code this round - flown and
// approved, with the empirically-settled rotation sign and the landmark diagnostic
// riding on it - and migrates onto this class in the weather-model round.
//
// Format facts, all verified offline against the shipped archives before any C++
// (the 2026-08-09 method, repeated 2026-09-07 for Mask.tree): every tile is 512x512;
// Cloud tiles are DXT5, Mask tiles are DXT1 (one-bit alpha), both decoded here to a
// 128x128 grid of per-4x4-block alpha averages; Mask level 7 is fully populated at
// ~5 km per texel while level 8 has holes over open ocean (the walk-up handles it).
//
// Invariant 1: file I/O and oapi calls - MAIN THREAD ONLY.
// ============================================================================

#pragma once
#include <windows.h>
#include <stdio.h>

class OroTileTree {
public:
	OroTileTree();
	~OroTileTree();

	// Opens Textures\<body>\Archive\<layer>.tree (TextureDir from Orbiter.cfg honoured).
	// Cached per body, failure included - calling it every frame is free. `who` names
	// the consumer in the log lines.
	bool  Open(const char* body, const char* layer, const char* who);
	void  Close();
	bool  IsOpen() const { return m_openOK; }
	const char* Body() const { return m_body; }
	int   Format() const { return m_fmt; }        // 0 unknown yet, 1 = DXT1, 5 = DXT5

	// One tile decode per frame unless forced - call once per main-thread frame.
	void  NewFrame() { m_decoded = false; }

	// The layer's ALPHA (0..1) at (lat, lon) [rad], from tree level `lvl` walking up a
	// level wherever a tile is absent. Returns -1 when unknown this frame (decode
	// budget spent and !force). Bilinear on the block grid.
	float Sample(double lat, double lon, int lvl, bool force);

private:
	enum { TILE_N = 24, GRID = 128, TEX = 512 };
	struct Node  { __int64 pos; DWORD size; DWORD child[4]; };
	struct DTile { int lvl, ilat, ilng; int stamp; BYTE a[GRID * GRID]; };

	char    m_body[64];
	char    m_layer[32];
	char    m_who[32];
	bool    m_openOK;
	FILE*   m_file;
	__int64 m_dataOfs;
	__int64 m_dataLen;
	DWORD   m_nodeN;
	DWORD   m_root4[2];
	Node*   m_toc;
	DTile   m_tile[TILE_N];
	int     m_stamp;
	bool    m_decoded;
	bool    m_fmtWarned;
	int     m_fmt;

	DWORD       Idx(int lvl, int ilat, int ilng) const;
	const BYTE* Tile(int lvl, int ilat, int ilng, bool& absent, bool force);
};
