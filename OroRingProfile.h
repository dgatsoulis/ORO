// ==============================================================
// OroRingProfile.h
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - THE RING PROFILE (2026-09-12): one normalised radial profile per ringed
// body, derived at runtime from whatever the planet ships. This is the generic
// mechanism behind the RINGS effect - any ringed planet, stock or addon, with no
// per-planet files required (his rule, 2026-09-11: "we cannot rely on a custom
// way to do this for just these two planets").
// ----------------------------------------------------------------------------
// THE PROFILE: N x 1 texels, LINEAR IN RADIUS from irad to orad (planet radii),
// RGB = brightness, ALPHA = encoded optical depth, a = 255*sqrt(tau/TAU_MAX).
//
// THE SOURCES, in priority order (beta/reports/260911/RINGS_PLAN.md section 3.1):
//   1. Textures\<Body>_ring_oro.dds - ORO's own format, hand-authored, the
//      reference-quality override. Absent for almost every planet; fine.
//   2. DERIVED, each channel from the best source available:
//      brightness  <Body>_ring_<size>.dds (8192 texels on Saturn) if present,
//                  else the legacy <Body>_ring.tex scanned radially.
//      tau         the .tex ALPHA. It is DXT3 with a LIVE alpha that RingTechPS
//                  feeds to SrcAlpha - real, author-intended OPACITY, and for a
//                  fictional addon ring the author's own alpha is ground truth by
//                  definition. (The hi-res .dds alpha is DEAD, all 255; measured.)
//                  Last resort with no alpha source anywhere: inverted from
//                  brightness - cannot tell a dark thick ring from a bright thin
//                  one, which is why it is the fallback and not the method.
//   3. no ring texture at all: a neutral synthesised ring.
//
// !! LINEAR IN RADIUS, and this corrects the stock client. RingTech2PS samples the
// profile at smoothstep(irad, orad, r) - the cubic - but the shipped texture is
// authored linear: the B ring's outer edge sits at the linear prediction (texel
// 5368, local gradient 18.0 vs a profile mean of 2.4) and NOT at the smoothstep
// one (5943, gradient 1.6). Stock therefore displaces ring structure by up to
// 6,300 km, more than the Cassini Division is wide. Everything here is linear and
// ORO's shader samples linearly.
//
// !! THE REFERENCE IMPLEMENTATION IS tools/ringprofile.py, and this file is its
// line-for-line port. tools/ringprofile_test builds this unit standalone and diffs
// the two; run it after ANY change here. The Python is the spec.
//
// No oapi calls in this unit - that is what makes it testable outside Orbiter.
// The Orbiter-facing half (OroRings.cpp) resolves the texture root, reads the
// planet's parameters, picks N for the device, and uploads.
// ============================================================================

#pragma once
#include <windows.h>

const double ORO_RING_TAU_MAX = 5.0;   // encode a = 255*sqrt(tau/5); decode tau = 5*a*a

struct OroRingProfile {
	int     n;              // texels
	double* bright;         // [n] 0..255 grey brightness
	double* tau;            // [n] optical depth 0..ORO_RING_TAU_MAX
	DWORD*  rgba;           // [n] encoded 0xAARRGGBB (B,G,R,A in memory), ready for UpdateTexture2D
	double  Rkm, irad, orad;// what it was built for: planet radius km, ring radii in planet radii
	char    srcBright[96];  // which file each channel came from - for the log line
	char    srcTau[96];
	bool    fromOverride;   // an _ring_oro.dds was used
};

// Build from the files under texRoot (e.g. "Textures", or Orbiter.cfg's TextureDir).
// nRequest 0 = source-driven (8192 with a hi-res profile present, else 1024); otherwise
// exactly nRequest texels, which is how the Orbiter side honours the device's texture
// width cap. honourOverride=false ignores an _ring_oro.dds (the authoring/diff path).
// The struct is zeroed on failure; free with OroRingProfile_Free either way.
bool OroRingProfile_Derive(const char* texRoot, const char* body, double Rkm,
                           double irad, double orad, int nRequest, bool honourOverride,
                           OroRingProfile* out);
void OroRingProfile_Free(OroRingProfile* p);

// Radius (km) at texel i; the texel (clamped) at a radius; a region name for the readout.
double OroRingProfile_RadiusKm(const OroRingProfile* p, int i);
int    OroRingProfile_Texel(const OroRingProfile* p, double rkm);
