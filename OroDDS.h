// ==============================================================
// OroDDS.h
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - THE DDS READER, shared (2026-09-12).
// ----------------------------------------------------------------------------
// Born in OroParticles.cpp on 2026-08-30 for the texture picker (own decoder on
// purpose: deterministic, no dependence on the lock semantics of loaded D3D
// surfaces). The RINGS needed the same decoder for two more things - the 8192 x 1
// high-res ring profile and the legacy `.tex` ring texture - so it moved here
// rather than being copied (the 26(a) shared-definition rule). Two things grew
// with the move, both needed by the rings and both harmless to the picker:
//   - the size guard: the picker capped at 4096 square and rejected h < 4; a ring
//     profile is 8192 x 1.
//   - a walker for Orbiter's `.tex` format, which is a CONCATENATION of DDS
//     surfaces (the client's LoadPlanetTextures, D3D9Util.cpp:1565 - one mip level
//     per surface, the top one). The legacy ring textures ship as three surfaces,
//     64/128/256 square, and the caller wants the largest.
//
// !! PIXEL CONVENTION: 0xAARRGGBB as a DWORD = B,G,R,A bytes in memory, i.e. the
// A8R8G8B8 TEXTURE order that UpdateTexture2D wants. That is NOT invariant 5's
// 0xAABBGGRR Sketchpad colour order. Do not pass one where the other is expected.
//
// Invariant 1: file I/O - MAIN THREAD ONLY. No oapi calls in here at all, which
// is what lets tools/ringprofile_test build this unit outside Orbiter.
// ============================================================================

#pragma once
#include <windows.h>

// Decode ONE DDS surface (top mip) from memory into malloc'd ARGB, top row first.
// The caller frees *outPix. *consumed (may be NULL) = bytes the surface occupies,
// header + top-level data, so a caller can walk a concatenation. Handles DXT1/3/5
// and uncompressed 32-bit A8R8G8B8 / X8R8G8B8 (X -> alpha forced to 255).
// False = unsupported or truncated; nothing is allocated.
bool OroDDS_DecodeMem(const BYTE* buf, size_t len, DWORD** outPix, int* outW, int* outH,
                      size_t* consumed);

// Top mip of a .dds FILE. (Byte-identical to the picker's old LoadDDSRGBA for every
// file it accepted; it accepts more now - see the guard note above.)
bool OroDDS_LoadFile(const char* path, DWORD** outPix, int* outW, int* outH);

// The LARGEST surface of an Orbiter .tex (a concatenation of DDS surfaces).
bool OroDDS_LoadTexLargest(const char* path, DWORD** outPix, int* outW, int* outH);
