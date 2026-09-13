// ==============================================================
// OroDDS.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================
// The shared DDS reader. See OroDDS.h for what it is and why it lives here.
// The decode loops are the picker's (OroParticles.cpp, 2026-08-30) verbatim; only
// the I/O changed from streaming fread to decode-from-memory, which is what the
// .tex concatenation walker needs.
// ============================================================================

#include "OroDDS.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

	// DXT1 colour block -> the four palette entries, ARGB. dxt1=true honours the
	// 3-colour + transparent mode; DXT3/5 always use the 4-colour mode.
	void Dxt1Colours(const BYTE* b, DWORD c[4], bool dxt1)
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

	// Whole file into malloc'd memory. Caller frees. NULL = cannot open / empty.
	BYTE* ReadWhole(const char* path, size_t* len)
	{
		FILE* fp = NULL;
		if (fopen_s(&fp, path, "rb") || !fp) return NULL;
		fseek(fp, 0, SEEK_END);
		const long sz = ftell(fp);
		if (sz <= 0) { fclose(fp); return NULL; }
		rewind(fp);
		BYTE* buf = (BYTE*)malloc((size_t)sz);
		if (!buf) { fclose(fp); return NULL; }
		const bool ok = fread(buf, 1, (size_t)sz, fp) == (size_t)sz;
		fclose(fp);
		if (!ok) { free(buf); return NULL; }
		*len = (size_t)sz;
		return buf;
	}
}

bool OroDDS_DecodeMem(const BYTE* buf, size_t len, DWORD** outPix, int* outW, int* outH,
                      size_t* consumed)
{
	if (!buf || len < 128 || *(const DWORD*)buf != 0x20534444u) return false;   // 'DDS '
	const BYTE* hdr = buf;
	const int   h       = *(const int*)(hdr + 12), w = *(const int*)(hdr + 16);
	const DWORD pfFlags = *(const DWORD*)(hdr + 80);
	const DWORD fourCC  = *(const DWORD*)(hdr + 84);
	const DWORD bits    = *(const DWORD*)(hdr + 88);
	// Was `w < 4 || h < 4 || w > 4096 || h > 4096` in the picker: a ring profile is
	// 8192 x 1. The pixel-count cap keeps a corrupt header from asking for the moon.
	if (w < 1 || h < 1 || w > 16384 || h > 16384 || (size_t)w * h > (size_t)64 * 1024 * 1024) return false;
	DWORD* pix = (DWORD*)malloc((size_t)w * h * 4);
	if (!pix) return false;
	bool   ok   = false;
	size_t used = 0;
	if (pfFlags & 0x4) {                               // DDPF_FOURCC - compressed
		const bool dxt1 = (fourCC == 0x31545844u);     // 'DXT1'
		const bool dxt3 = (fourCC == 0x33545844u);     // 'DXT3'
		const bool dxt5 = (fourCC == 0x35545844u);     // 'DXT5'
		if (dxt1 || dxt3 || dxt5) {
			const int    bw = (w + 3) / 4, bh = (h + 3) / 4, bsz = dxt1 ? 8 : 16;
			const size_t need = (size_t)bw * bh * bsz;
			if (len - 128 >= need) {
				const BYTE* blocks = buf + 128;
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
				ok = true; used = 128 + need;
			}
		}
	} else if ((pfFlags & 0x40) && bits == 32) {       // DDPF_RGB, 32-bit A8R8G8B8/X8R8G8B8
		const DWORD  aMask = *(const DWORD*)(hdr + 104);
		const size_t need  = (size_t)w * h * 4;
		if (len - 128 >= need) {
			memcpy(pix, buf + 128, need);
			if (!aMask) for (size_t i = 0; i < (size_t)w * h; i++) pix[i] |= 0xFF000000u;
			ok = true; used = 128 + need;
		}
	}
	if (!ok) { free(pix); return false; }
	*outPix = pix; *outW = w; *outH = h;
	if (consumed) *consumed = used;
	return true;
}

bool OroDDS_LoadFile(const char* path, DWORD** outPix, int* outW, int* outH)
{
	size_t len = 0;
	BYTE* buf = ReadWhole(path, &len);
	if (!buf) return false;
	const bool ok = OroDDS_DecodeMem(buf, len, outPix, outW, outH, NULL);
	free(buf);
	return ok;
}

bool OroDDS_LoadTexLargest(const char* path, DWORD** outPix, int* outW, int* outH)
{
	size_t len = 0;
	BYTE* buf = ReadWhole(path, &len);
	if (!buf) return false;
	DWORD* best = NULL; int bw = 0, bh = 0;
	size_t off = 0;
	// The client's own walker stops at the first non-'DDS ' magic; so do we.
	while (off + 128 <= len && *(const DWORD*)(buf + off) == 0x20534444u) {
		DWORD* pix = NULL; int w = 0, h = 0; size_t used = 0;
		if (!OroDDS_DecodeMem(buf + off, len - off, &pix, &w, &h, &used)) break;
		if (!best || (size_t)w * h > (size_t)bw * bh) { free(best); best = pix; bw = w; bh = h; }
		else free(pix);
		off += used;
	}
	free(buf);
	if (!best) return false;
	*outPix = best; *outW = bw; *outH = bh;
	return true;
}
