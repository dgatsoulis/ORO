// ==============================================================
// OroRingProfile.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================
// The ring profile derivation. See OroRingProfile.h for what it is, and
// tools/ringprofile.py for the SPEC this is a port of - keep the two in step and
// run tools/ringprofile_test after any change. Every numeric step below names its
// Python twin in a comment.
// ============================================================================

#include "OroRingProfile.h"
#include "OroDDS.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

	const double PI_ = 3.14159265358979323846;

	// !! The .tex radial scan must use the mesh the texture was authored against:
	// RingMgr::CreateRing's top LOD, nsect = 8 + res*4 = 16 - NOT ORO's raised section
	// count. cos(pi/nsect) is a ~2% radial term; deriving with the new mesh would move
	// every band. (ringprofile.py: STOCK_NSECT)
	const int STOCK_NSECT = 16;

	double Grey(DWORD c) { return (((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255)) / 3.0; }

	// ringprofile.py: resample() - linear, endpoints preserved.
	void Resample(const double* src, int m, double* dst, int n)
	{
		if (m < 2) { for (int i = 0; i < n; i++) dst[i] = m ? src[0] : 0.0; return; }
		if (m == n) { memcpy(dst, src, sizeof(double) * n); return; }
		for (int i = 0; i < n; i++) {
			const double t = i * (m - 1) / (n - 1.0);
			int j = (int)t; if (j > m - 2) j = m - 2;
			const double f = t - j;
			dst[i] = src[j] * (1.0 - f) + src[j + 1] * f;
		}
	}

	// ringprofile.py: scan_tex(). RingMgr::CreateRing maps the OUTER node (radius nrad)
	// to tv=0 and the INNER node (radius ir) to tv=1, tu running fo..1-fo along the arc.
	// At u = 0.5 the quad's bilinear interpolation sits at the ARC MIDPOINT, whose radius
	// is nrad*cos(alpha) = orad at v=0 and ir*cos(alpha) at v=1 - so the centre column is
	// a clean radial scan, r(v) = orad - v*(orad - irad*cos(alpha)). Validated on both
	// stock bodies. Output: h samples, UNIFORM in radius over [irad, orad], inner first.
	void ScanTex(const DWORD* px, int w, int h, double irad, double orad,
	             double* outB, double* outA)
	{
		const double alpha = PI_ / STOCK_NSECT;
		const double rAtV1 = irad * cos(alpha);
		const int    mid   = w / 2;
		double* rs  = (double*)malloc(sizeof(double) * h);
		double* bs  = (double*)malloc(sizeof(double) * h);
		double* als = (double*)malloc(sizeof(double) * h);
		for (int row = 0; row < h; row++) {
			const double v = row / (h - 1.0);
			const int    k = h - 1 - row;                       // reversed: index 0 = INNER edge
			const DWORD  c = px[row * w + mid];
			rs[k]  = orad - v * (orad - rAtV1);
			bs[k]  = Grey(c);
			als[k] = (double)((c >> 24) & 255);
		}
		for (int i = 0; i < h; i++) {
			const double rad = irad + (orad - irad) * i / (h - 1.0);
			double b, a;
			if (rad <= rs[0])          { b = bs[0];     a = als[0]; }
			else if (rad >= rs[h - 1]) { b = bs[h - 1]; a = als[h - 1]; }
			else {
				int lo = 0;
				while (lo + 1 < h && rs[lo + 1] < rad) lo++;
				const double d = rs[lo + 1] - rs[lo];
				const double f = (rad - rs[lo]) / (d > 1e-9 ? d : 1e-9);
				b = bs[lo]  * (1.0 - f) + bs[lo + 1]  * f;
				a = als[lo] * (1.0 - f) + als[lo + 1] * f;
			}
			outB[i] = b; outA[i] = a;
		}
		free(rs); free(bs); free(als);
	}

	// ringprofile.py: tau_from_alpha() - the legacy alpha IS opacity: tau = -ln(1 - a).
	double TauFromAlpha(double a)
	{
		double f = a / 255.0; if (f < 0.0) f = 0.0; if (f > 0.999) f = 0.999;
		double t = -log(1.0 - f);
		if (t > ORO_RING_TAU_MAX) t = ORO_RING_TAU_MAX;
		return t > 0.0 ? t : 0.0;                             // max() kills -0.0 at a=0
	}

	// ringprofile.py: tau_from_bright() - LAST RESORT, no alpha source anywhere.
	double TauFromBright(double b, double bmax)
	{
		const double f = 0.98 * (bmax > 0.0 ? b / bmax : 0.0);
		double x = 1.0 - f; if (x < 1e-3) x = 1e-3;
		double t = -log(x);
		return t > ORO_RING_TAU_MAX ? ORO_RING_TAU_MAX : t;
	}

	// ringprofile.py: gate_tau(). THE TWO SOURCES DISAGREE ABOUT WHERE THE EDGES ARE:
	// brightness from an 8192-texel source and tau from a 256-texel one, so Saturn's
	// Encke gap arrives BLACK while still carrying tau 0.42 - a gap you can see through
	// that nonetheless shadows the planet. The coarse source sets the LEVEL, the fine
	// source sets the EDGES: no material, no optical depth.
	// !! Normalised against the profile's OWN maximum, never an absolute, so a dark ring
	// that is optically thick (Uranus, albedo 0.03) is never gated away. Measured: inert
	// on Uranus, clears Saturn's gaps, leaves the ring body untouched.
	void GateTau(const double* bright, double* tau, int n)
	{
		double bmax = 0.0;
		for (int i = 0; i < n; i++) if (bright[i] > bmax) bmax = bright[i];
		if (bmax <= 0.0) bmax = 1.0;
		const double bref = 0.20 * bmax;
		for (int i = 0; i < n; i++) {
			double g = bref > 0.0 ? bright[i] / bref : 1.0;
			if (g > 1.0) g = 1.0;
			tau[i] *= g;
		}
	}

	bool Exists(const char* path) { FILE* f = NULL; if (fopen_s(&f, path, "rb") || !f) return false; fclose(f); return true; }

	void Encode(OroRingProfile* p)
	{
		for (int i = 0; i < p->n; i++) {
			int b = (int)(p->bright[i] + 0.5); if (b < 0) b = 0; if (b > 255) b = 255;
			double t = p->tau[i]; if (t < 0.0) t = 0.0;
			int a = (int)(255.0 * sqrt(t / ORO_RING_TAU_MAX) + 0.5); if (a < 0) a = 0; if (a > 255) a = 255;
			p->rgba[i] = ((DWORD)a << 24) | ((DWORD)b << 16) | ((DWORD)b << 8) | (DWORD)b;
		}
	}

	void Alloc(OroRingProfile* p, int n)
	{
		p->n      = n;
		p->bright = (double*)calloc(n, sizeof(double));
		p->tau    = (double*)calloc(n, sizeof(double));
		p->rgba   = (DWORD*) calloc(n, sizeof(DWORD));
	}
}

void OroRingProfile_Free(OroRingProfile* p)
{
	if (!p) return;
	free(p->bright); free(p->tau); free(p->rgba);
	memset(p, 0, sizeof(*p));
}

bool OroRingProfile_Derive(const char* texRoot, const char* body, double Rkm,
                           double irad, double orad, int nRequest, bool honourOverride,
                           OroRingProfile* out)
{
	memset(out, 0, sizeof(*out));
	out->Rkm = Rkm; out->irad = irad; out->orad = orad;
	if (!(orad > irad) || !(Rkm > 0.0)) return false;

	char path[MAX_PATH];

	// 1. THE OVERRIDE - ORO's own format; the file IS the profile.
	if (honourOverride) {
		sprintf_s(path, "%s\\%s_ring_oro.dds", texRoot, body);
		DWORD* px = NULL; int w = 0, h = 0;
		if (OroDDS_LoadFile(path, &px, &w, &h)) {
			double* b = (double*)malloc(sizeof(double) * w);
			double* t = (double*)malloc(sizeof(double) * w);
			for (int i = 0; i < w; i++) {                        // decode tau = 5*a*a
				const double a = ((px[i] >> 24) & 255) / 255.0;
				b[i] = Grey(px[i]);
				t[i] = ORO_RING_TAU_MAX * a * a;
			}
			free(px);
			const int n = nRequest > 1 ? nRequest : w;
			Alloc(out, n);
			Resample(b, w, out->bright, n);
			Resample(t, w, out->tau, n);
			free(b); free(t);
			Encode(out);
			out->fromOverride = true;
			sprintf_s(out->srcBright, "%s_ring_oro.dds", body);
			strcpy_s(out->srcTau, out->srcBright);
			return true;
		}
	}

	// 2. DERIVED. Brightness from the best source; tau from the .tex alpha.
	//    (ringprofile.py: derive())
	DWORD* hiPx = NULL; int hiW = 0, hiH = 0;
	static const int sizes[3] = { 8192, 4096, 2048 };
	for (int k = 0; k < 3 && !hiPx; k++) {
		sprintf_s(path, "%s\\%s_ring_%d.dds", texRoot, body, sizes[k]);
		if (Exists(path) && OroDDS_LoadFile(path, &hiPx, &hiW, &hiH))
			sprintf_s(out->srcBright, "%s_ring_%d.dds", body, sizes[k]);
	}

	double* legB = NULL; double* legA = NULL; int legN = 0;
	sprintf_s(path, "%s\\%s_ring.tex", texRoot, body);
	{
		DWORD* px = NULL; int w = 0, h = 0;
		if (Exists(path) && OroDDS_LoadTexLargest(path, &px, &w, &h) && w >= 2 && h >= 2) {
			legN = h;
			legB = (double*)malloc(sizeof(double) * h);
			legA = (double*)malloc(sizeof(double) * h);
			ScanTex(px, w, h, irad, orad, legB, legA);
			free(px);
			sprintf_s(out->srcTau, "%s_ring.tex", body);
		}
	}

	const int n = nRequest > 1 ? nRequest : (hiPx ? 8192 : 1024);
	Alloc(out, n);

	if (hiPx) {
		double* b1 = (double*)malloc(sizeof(double) * hiW);
		for (int i = 0; i < hiW; i++) b1[i] = Grey(hiPx[i]);       // bright_from_1d: row 0
		Resample(b1, hiW, out->bright, n);
		free(b1); free(hiPx);
	} else if (legB) {
		Resample(legB, legN, out->bright, n);
		strcpy_s(out->srcBright, out->srcTau);
	} else {
		for (int i = 0; i < n; i++) out->bright[i] = 180.0;       // no texture at all
		strcpy_s(out->srcBright, "(synthesised)");
	}

	if (legA) {
		double* aR = (double*)malloc(sizeof(double) * n);
		Resample(legA, legN, aR, n);                                // resample ALPHA, then tau
		for (int i = 0; i < n; i++) out->tau[i] = TauFromAlpha(aR[i]);
		free(aR);
		GateTau(out->bright, out->tau, n);
	} else {
		double bmax = 0.0;
		for (int i = 0; i < n; i++) if (out->bright[i] > bmax) bmax = out->bright[i];
		if (bmax <= 0.0) bmax = 1.0;
		for (int i = 0; i < n; i++) out->tau[i] = TauFromBright(out->bright[i], bmax);
		strcpy_s(out->srcTau, "(inverted from brightness - no alpha source)");
	}
	free(legB); free(legA);

	Encode(out);
	return true;
}

double OroRingProfile_RadiusKm(const OroRingProfile* p, int i)
{
	return p->Rkm * (p->irad + (p->orad - p->irad) * i / (p->n - 1.0));
}

int OroRingProfile_Texel(const OroRingProfile* p, double rkm)
{
	const double t = (rkm / p->Rkm - p->irad) / (p->orad - p->irad);
	int i = (int)(t * (p->n - 1) + 0.5);
	if (i < 0) i = 0; if (i > p->n - 1) i = p->n - 1;
	return i;
}
