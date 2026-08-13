// ==============================================================
// bellgen.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO bell-glow texture synthesizer (2026-08-09)
// ----------------------------------------------------------------------------
// Generates the alpha-masked band texture for the ORO bell-glow shells,
// modelled on the Merlin Vacuum reference frames: dark base, dense vertical
// soot/cooling streaks, pronounced horizontal stiffener rings in the upper
// half, fine grain everywhere. The ALPHA channel is the load-bearing mask
// (opaque = glows, transparent = stays dark metal); the RGB carries the same
// structure as a warm-white modulation, belt and braces for whichever shader
// path multiplies what.
//
// UV CONVENTION (the mesh unwrap must match, or say the word and this flips):
//   U (x, 0..1)  = around the circumference, SEAMLESS (all u-terms periodic)
//   V (y, 0..1)  = along the bell axis: V=0 at the THROAT/BASE (dark),
//                  V=1 at the EXIT LIP
//
// Outputs (working dir):
//   bell_glow.tga    - 1024x1024 32-bit BGRA, top-left origin (the deliverable)
//   preview_hot.bmp  - what the mask looks like GLOWING (premultiplied, warm)
//   preview_mask.bmp - the raw alpha mask, grayscale
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

static const int W = 1024, H = 1024;
static const float PI2 = 6.28318530718f;

// Deterministic LCG so every run is identical (tweak SEED for a new pattern).
static unsigned s_rng = 20260809u;
static float frnd() { s_rng = s_rng * 1664525u + 1013904223u; return (s_rng >> 8) * (1.0f / 16777216.0f); }

static float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
static float sstep(float a, float b, float x)
{
	float t = clampf((x - a) / (b - a), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}
// Periodic distance on the circumference.
static float udist(float u, float c)
{
	float d = fabsf(u - c);
	return d > 0.5f ? 1.0f - d : d;
}

// Periodic-in-u value noise: integer harmonics in u keep the seam invisible.
static float pnoise(float u, float v, int fu, float fv, float p1, float p2)
{
	return 0.5f + 0.5f * sinf(PI2 * (fu * u + fv * v) + p1 + 1.7f * sinf(PI2 * fv * 0.37f * v + p2));
}

// --- streak / ring tables ---------------------------------------------------
static const int NSTK = 64;      // fine vertical streaks
static float stkU[NSTK], stkW[NSTK], stkD[NSTK], stkV0[NSTK], stkV1[NSTK];
static const int NSEC = 3;       // broad dark sectors (soot-choked patches)
static float secU[NSEC], secW[NSEC], secD[NSEC];
static const int NRING = 8;      // horizontal stiffener rings, upper-half heavy.
                                 // WIDE and SOFT (the first cut's thin constant
                                 // rings read as window blinds), and their depth
                                 // is modulated AROUND the bell in-loop.
static float ringV[NRING] = { 0.15f, 0.23f, 0.31f, 0.40f, 0.50f, 0.61f, 0.73f, 0.86f };
static float ringD[NRING] = { 0.34f, 0.30f, 0.32f, 0.26f, 0.22f, 0.19f, 0.16f, 0.13f };
static float ringW[NRING] = { 0.026f, 0.020f, 0.030f, 0.022f, 0.026f, 0.028f, 0.030f, 0.026f };

int main()
{
	for (int k = 0; k < NSTK; k++) {
		stkU[k]  = frnd();
		stkW[k]  = 0.0035f + 0.016f * frnd() * frnd();     // mostly fine, a few wide
		stkD[k]  = 0.30f + 0.65f * frnd();                 // depth (how dark)
		stkV0[k] = (frnd() < 0.55f) ? 0.0f : 0.55f * frnd(); // most run the full bell
		stkV1[k] = (frnd() < 0.60f) ? 1.2f : 0.45f + 0.55f * frnd(); // ~40% end early
	}
	for (int k = 0; k < NSEC; k++) {
		secU[k] = frnd();
		secW[k] = 0.035f + 0.05f * frnd();
		secD[k] = 0.28f + 0.22f * frnd();
	}

	unsigned char* tga = (unsigned char*)malloc(W * H * 4);
	unsigned char* bmpH = (unsigned char*)malloc(W * H * 3);
	unsigned char* bmpM = (unsigned char*)malloc(W * H * 3);

	for (int y = 0; y < H; y++) {
		const float v = (float)y / (H - 1);                // 0 = base, 1 = lip
		for (int x = 0; x < W; x++) {
			const float u = (float)x / W;                  // wraps

			// --- the axial envelope: dark base, full glow by ~a third down,
			//     mild dimming into the lip ------------------------------------
			float env = sstep(0.06f, 0.32f, v) * (1.0f - 0.22f * sstep(0.80f, 1.0f, v));

			// --- vertical streaks (multiplicative dark lines), CLUSTERED ------
			// A low-frequency circumferential field deepens streaks in some
			// sectors and nearly clears them in others - the reference has dense
			// sooty runs next to clean bright panels, not an even paling.
			const float cluster = 0.35f + 0.65f * pnoise(u, 0.0f, 2, 0.0f, 2.3f, 0.6f);
			float streaks = 1.0f;
			for (int k = 0; k < NSTK; k++) {
				const float d = udist(u, stkU[k]);
				if (d > stkW[k] * 3.0f) continue;
				const float g  = expf(-(d * d) / (stkW[k] * stkW[k]));
				const float on = sstep(stkV0[k], stkV0[k] + 0.10f, v)
				               * (1.0f - sstep(stkV1[k] - 0.12f, stkV1[k], v));
				streaks *= 1.0f - stkD[k] * cluster * g * on;
			}
			for (int k = 0; k < NSEC; k++) {
				const float d = udist(u, secU[k]);
				if (d > secW[k] * 2.5f) continue;
				const float g = expf(-(d * d) / (secW[k] * secW[k]));
				streaks *= 1.0f - secD[k] * g;
			}

			// --- horizontal stiffener rings, depth varying AROUND the bell ---
			float rings = 1.0f;
			for (int k = 0; k < NRING; k++) {
				const float dv = v - ringV[k];
				if (fabsf(dv) > ringW[k] * 3.0f) continue;
				const float g   = expf(-(dv * dv) / (ringW[k] * ringW[k]));
				const float mod = 0.45f + 0.55f * pnoise(u, 0.0f, 3, 0.0f, 1.3f * k, 2.9f);
				rings *= 1.0f - ringD[k] * mod * g;
			}

			// --- mottling + fine grain (all periodic in u) --------------------
			const float mot  = 0.78f + 0.22f * pnoise(u, v, 3, 2.2f, 1.1f, 4.2f)
			                        * pnoise(u, v, 5, 1.3f, 3.7f, 0.9f);
			const float grain = 0.92f + 0.08f * pnoise(u, v, 41, 57.0f, 0.3f, 2.6f)
			                          * pnoise(u, v, 67, 33.0f, 5.1f, 1.4f);

			float mask = clampf(env * streaks * rings * mot * grain, 0.0f, 1.0f);
			// Streak floors: even the darkest lines glow faintly on a hot bell.
			mask = 0.05f * env + 0.95f * mask;

			// --- channels -----------------------------------------------------
			// Alpha = the mask. RGB = warm white carrying the same structure at
			// reduced contrast (belt and braces across shader paths).
			const float rgbmod = 0.55f + 0.45f * mask;
			const unsigned char R = (unsigned char)(255.0f * clampf(1.00f * rgbmod, 0.0f, 1.0f));
			const unsigned char G = (unsigned char)(255.0f * clampf(0.905f * rgbmod, 0.0f, 1.0f));
			const unsigned char B = (unsigned char)(255.0f * clampf(0.80f * rgbmod, 0.0f, 1.0f));
			const unsigned char A = (unsigned char)(255.0f * mask);

			unsigned char* t = tga + (y * W + x) * 4;      // BGRA, top-left origin
			t[0] = B; t[1] = G; t[2] = R; t[3] = A;

			// preview_hot: the texture over black, lit by a blackbody-ish orange
			// (what the shell reads like at high heat).
			const float hb = mask;
			unsigned char* ph = bmpH + (y * W + x) * 3;    // BGR
			ph[0] = (unsigned char)(255.0f * clampf(0.28f * hb * (B / 255.0f) + 0.02f, 0.0f, 1.0f));
			ph[1] = (unsigned char)(255.0f * clampf(0.62f * hb * (G / 255.0f), 0.0f, 1.0f));
			ph[2] = (unsigned char)(255.0f * clampf(1.00f * hb * (R / 255.0f), 0.0f, 1.0f));

			unsigned char* pm = bmpM + (y * W + x) * 3;
			pm[0] = pm[1] = pm[2] = A;
		}
	}

	// --- TGA (type 2, 32-bit, top-left origin) ------------------------------
	{
		FILE* f = fopen("bell_glow.tga", "wb");
		if (!f) { printf("cannot open bell_glow.tga\n"); return 1; }
		unsigned char hdr[18]; memset(hdr, 0, 18);
		hdr[2] = 2;                                        // uncompressed truecolor
		hdr[12] = W & 0xFF; hdr[13] = (W >> 8) & 0xFF;
		hdr[14] = H & 0xFF; hdr[15] = (H >> 8) & 0xFF;
		hdr[16] = 32;                                      // bpp
		hdr[17] = 0x28;                                    // top-left + 8 alpha bits
		fwrite(hdr, 1, 18, f);
		fwrite(tga, 1, W * H * 4, f);
		fclose(f);
	}

	// --- BMP previews (24-bit, bottom-up) -----------------------------------
	const char* names[2] = { "preview_hot.bmp", "preview_mask.bmp" };
	unsigned char* bufs[2] = { bmpH, bmpM };
	for (int i = 0; i < 2; i++) {
		FILE* f = fopen(names[i], "wb");
		if (!f) { printf("cannot open %s\n", names[i]); return 1; }
		const int rowB = W * 3;                            // 1024*3 is 4-aligned
		const int dataB = rowB * H;
		unsigned char bh[54]; memset(bh, 0, 54);
		bh[0] = 'B'; bh[1] = 'M';
		*(int*)(bh + 2) = 54 + dataB;
		*(int*)(bh + 10) = 54;
		*(int*)(bh + 14) = 40;
		*(int*)(bh + 18) = W;
		*(int*)(bh + 22) = H;
		*(short*)(bh + 26) = 1;
		*(short*)(bh + 28) = 24;
		*(int*)(bh + 34) = dataB;
		fwrite(bh, 1, 54, f);
		for (int y = H - 1; y >= 0; y--) fwrite(bufs[i] + y * rowB, 1, rowB, f);
		fclose(f);
	}

	printf("bell_glow.tga + previews written (%dx%d)\n", W, H);
	return 0;
}
