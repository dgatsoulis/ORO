// ==============================================================
// OroState.h
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

#pragma once

// ============================================================================
// ORO - shared effect state
// ----------------------------------------------------------------------------
// Written by the control dialog (UI events, main thread) and read by the
// render callback (also main thread - Orbiter's dialogs are pumped by the sim
// loop, and D3D9Client render procs run inside the frame render on the same
// thread). Single-threaded by construction: NO mutex needed. If any effect
// ever gains an off-thread writer, that changes - add locking then.
//
// LAB PHASE: sliders DRIVE these values directly (no physics). In the physics
// phase the same fields become per-effect GAINS applied to the felt-G model,
// so the dialog survives unchanged.
// ============================================================================

struct OroEffectState {
	// Master arm: the whole experience on/off (the dialog's ARMED switch;
	// Ctrl+G toggles it from the keyboard as the panic/quick kill).
	bool  masterArmed    = true;

	// --- VISION ---
	bool  blackoutEnabled = true;
	float blackout        = 0.0f;  // 0..1; 1 = lights out (full black - the dialog
	                               // floats above the frame, so recovery is always possible)
	bool  redoutEnabled   = true;
	float redout          = 0.0f;  // 0..1; alpha capped at 80% (lab tuning, 2026-07-25)
	bool  tunnelEnabled   = true;
	float tunnel          = 0.0f;  // 0..1; 0 = open, 1 = closed to a last glimmer
	bool  spotsEnabled    = true;
	float spots           = 0.0f;  // 0..1; drives spot count AND opacity (shimmering scotomas)

	// Grey-out: the first PREMIUM effect - a true frame RESAMPLE through the
	// D3D9Client image-processing (HLSL) pipeline, not an additive overlay.
	// PURE desaturation of the whole frame toward monochrome (colour vision
	// fades before black-out under sustained +Gz); darkening is the black-out's
	// job, not this one's. Needs an ORO-patched client (gcCore backbuffer
	// capture); silently off without it.
	bool  greyoutEnabled  = true;
	float greyout         = 0.0f;  // 0..1; 0 = full colour, 1 = fully monochrome

	// Blur: the second PREMIUM effect - a separable Gaussian frame RESAMPLE (two
	// IPI passes, H then V). Vision softens/smears under G stress. Same client
	// dependency as grey-out; silently off without it.
	bool  blurEnabled     = true;
	float blur            = 0.0f;  // 0..1; 0 = sharp, 1 = full-frame Gaussian smear

	// Chromatic aberration: RGB channels split radially (worse toward the edges),
	// the ocular lens distorting under load. Premium IPI resample; off without patch.
	bool  aberrationEnabled = true;
	float aberration        = 0.0f; // 0..1; 0 = aligned, 1 = strong edge colour fringing

	// Sparkles / phosphenes: "seeing stars" under G/impact - fine bright scintillations
	// scattered across the field (the BRIGHT twin of dark spots). Deliberately subtle
	// (small, soft, cool-white) and placed differently from the dark-spots ring. Additive
	// Sketchpad overlay, drawn LATE so stars show even as the view darkens.
	bool  sparklesEnabled = true;
	float sparkles        = 0.0f;   // 0..1; drives count AND flash brightness

	// Peripheral swim: a slow woozy warp of the field, strongest in the periphery
	// (central vision holds longest) - the disorientation just before G-LOC. Premium
	// IPI UV-displacement resample; off without the client patch.
	bool  swimEnabled     = true;
	float swim            = 0.0f;   // 0..1; 0 = steady, 1 = strong peripheral wobble

	// --- MOTION ---
	// Tilt: the whole field rolls about centre - a head/vestibular tilt under lateral
	// G or disorientation. Premium IPI rotate-resample; off without the client patch.
	bool  tiltEnabled     = true;
	float tilt            = 0.0f;   // 0..1; 0 = level, 1 = full roll (~16 deg)

	// Camera shake: PHYSICS-DRIVEN eyepoint jitter + felt-G "push into your seat"
	// (via VESSEL::SetCameraOffset - no D3D9, keeps view direction, follows the pilot/
	// copilot position). The module computes the 0..1 intensity from thrust/drag/ground;
	// these knobs (live-tunable in the dialog's CAM-SHAKE section) shape the LOOK: the
	// per-axis buffet AMPLITUDE (metres, at full intensity) and the base FREQUENCY (Hz).
	bool  shakeEnabled    = true;
	float shakeAmpX       = 0.004f; // buffet amplitude, lateral   [m] (dialog shows mm, 0..10)
	float shakeAmpY       = 0.004f; // buffet amplitude, vertical  [m]
	float shakeAmpZ       = 0.003f; // buffet amplitude, fore/aft  [m]
	float shakeFreq       = 8.0f;   // base buffet frequency       [Hz] (dialog 0..10)
	bool  shakeTest       = false;  // dialog "Test" toggle: force full-intensity shake at the tuned settings

	// Heartbeat: the signature ORO effect - the visual field THROBS darker with
	// each heartbeat as you approach G-LOC (systolic pressure waves at the retina).
	// A soft peripheral vignette pulsing on a real-time cardiac clock; the slider
	// sets throb depth. (Physics phase: rate + depth track felt-G / exertion; the
	// same beat will drive the heartbeat SOUND.)
	bool  heartbeatEnabled = true;
	float heartbeat        = 0.0f; // 0..1; 0 = steady, 1 = deep dim on every beat

	// --- ENVIRONMENT (world effects) ---
	// Exhaust shimmer: heat-haze REFRACTION around the engine plumes in atmospheric
	// flight - the air above a hot exhaust bends the view behind it. This is the first
	// ORO effect that is NOT physiological: it happens in the WORLD, not the eye, so
	// it is the ONLY effect that renders in EXTERNAL view (in the cockpit the engines are
	// behind you) and it runs FIRST in the resample chain - the pilot's physiology layers
	// on top of an already-shimmering world. Premium IPI resample; off without the client
	// patch. Driven by the camera-target vessel's main/hover/retro plumes x air density.
	bool  shimmerEnabled  = true;
	float shimmer         = 0.0f;  // 0..1; 0 = no distortion, 1 = strong heat haze
	float shimmerOfs      = 0.0f;  // -1..+1 m: slides the haze along the plume axis (0 = at the
	                               // nozzle as the exhaust spec defines it; +ve = further aft)

	// PLUME EXPANSION (2026-08-09) - pressure-dependent exhaust: the nozzle is expanded
	// for ONE ambient pressure and the atmosphere decides what the jet does everywhere
	// else. Overexpanded at sea level = pinched narrow with the SHOCK-DIAMOND train;
	// underexpanded high up = the wide faint EXPANSION BLOOM of high-altitude launch
	// footage. Stock draws one fixed billboard at every altitude - this is the fix, as
	// our own ADDITIVE OVERLAY (the roadmap's assessed plan: rewriting the stock exhaust
	// list per frame churns indices vessels' own code holds, so stock is never touched).
	// Geometry per frame in OroPlume.cpp; regime is automatic from static pressure -
	// no knob to ride during an ascent, the sim knows. Per CLASS like the shimmer.
	bool  plumeEnabled    = true;
	float plume           = 1.0f;  // 0..1 master strength. Defaults ON at full like the
	                               // reentry trim: the overlay IS the corrected look,
	                               // not an optional garnish.
	// THE THRUSTER FAMILY'S LAB | PHYSICS switch (2026-08-09, his call - the same
	// transition the vision suite made). PHYSICS: the plume model computes the four
	// pressure/throttle curves (cell spacing from the expansion ratio, train washout
	// with altitude, throttle->NPR coupling, deep-overexpansion separation flicker)
	// and the sliders TRIM on top of that base. LAB: the curves pin to their
	// reference values (sea level, full throttle) and the sliders rule alone -
	// today's hand-tuning environment, exactly. The curves are ANCHORED so that at
	// (sea level, full throttle) both modes are identical - which is why PHYSICS can
	// default ON without moving any look he has already tuned on the test stand.
	// The SHIMMER reads the same model in both modes (his call: "bring in the
	// shimmer into the physics"), so haze and jet can never disagree about shape.
	bool  plumePhysics    = true;
	// THE EXPANSION BAND (2026-08-09, his design: one track, two handles). The two
	// ends of the double slider are the pressures that frame the whole regime
	// machinery, stored as log10(Pa) - pressure-based so it works at every world:
	//   HIGH handle = full OVERexpansion: at/above this ambient the diamond train
	//     is fully developed - and it doubles as the OD REFERENCE (the "design
	//     pressure" the engine is rated for), so dragging it low makes the hull
	//     behave like a vacuum engine: shuddering pinched diamonds at the pad,
	//     clean and rated in space.
	//   LOW handle = full UNDERexpansion: at/below this ambient the vacuum bloom
	//     is fully open.
	// The ramps live INSIDE the window as fixed fractions (top 32.5% = diamond
	// ramp, bottom 50% = bloom ramp, dead zone between - today's proportions), so
	// the two weights stay disjoint at ANY handle positions and the defaults
	// reproduce the pre-slider behaviour exactly. Per CLASS: an engine fact.
	float plumeExpHi      = 5.0057f; // log10 Pa; default 101.325 kPa (Earth sea level)
	float plumeExpLo      = 1.0f;    // log10 Pa; default 10 Pa

	float plumeWidth      = 1.0f;  // x jet width  (0..3) - scales every radial size
	                               //   (core, sheath, lozenges). Added 2026-08-09 when
	                               //   "ours replaces stock" became the direction: the
	                               //   stock wsize is only a hint, and the overlay now
	                               //   owns the silhouette, so its width is a per-hull
	                               //   fact (the Shell-dist lesson - a knob, not a bake).
	float plumeLen        = 1.0f;  // x jet length (0..3) - the silhouette's other axis
	float plumeCells      = 7.0f;  // NUMBER of shock cells / diamonds (1..12, rounded to
	                               //   int at use). Spacing moves the same train closer/
	                               //   further (doubling as a length lever - his call:
	                               //   "that's ok"); this sets how many discs there ARE.
	float plumeDiamond    = 1.0f;  // x diamond (Mach disc) brightness  (0..2)
	float plumeSpacing    = 1.0f;  // x shock-cell spacing              (0..3; cells sit
	                               //   ~2 nozzle widths apart at 1.0)
	float plumeBloomWid   = 1.0f;  // x vacuum bloom opening angle      (0..2)
	float plumeBloomBri   = 1.0f;  // x vacuum bloom brightness         (0..2)
	float plumeThroatOfs  = 0.0f;  // THROAT OFFSET [m] (0..1): slides the throat-fire
	                               //   DISCS downstream along the flow axis, out of the
	                               //   bell - for hulls whose visual nozzle sits deeper
	                               //   or shallower than the exhaust spec says.
	float plumeThroat     = 1.0f;  // THROAT GLOW (0..4): the fire INSIDE the bell cup -
	                               //   the core/sheath extended upstream ~one nozzle
	                               //   width, bright and flickering, cut per pixel by
	                               //   the patch-(g) depth clip so it shows through the
	                               //   mouth and hides behind the bell walls. 0 = the
	                               //   hollow look (stock's billboard filled the cup;
	                               //   this is our own fill - his report 2026-08-09).
	float plumeSootRate   = 1.0f;  // SOOT CHURN (0..3): the lifecycle speed - streaks
	                               //   are born at a hashed rim position, SHOOT OUT
	                               //   (length grows from the lip), flicker, fade and
	                               //   reseed elsewhere, each slot on its own hashed
	                               //   period. Pure functions of REAL time (the
	                               //   lightning-cadence trick - no stored state).
	                               //   0 freezes the pattern (the static look), 1 is
	                               //   the natural cadence, 3 is frantic. Per class.
	float plumeSoot       = 0.0f;  // ABLATIVE SOOT STREAKS (0..2): dark alpha-blended
	                               //   wisps hugging the core from the nozzle lip - the
	                               //   Merlin ablative look, drawn OVER the additive
	                               //   glow so they genuinely dim it (the G11 recipe's
	                               //   inverse: soot is IN the plume, not behind it).
	                               //   0 = off - the slider IS the toggle (the aurora's
	                               //   "activity is the opt-in" law; an extra enable
	                               //   flag was deliberately not built). Opacity only;
	                               //   the colour is fixed near-black soot. Per class.
	DWORD plumeColJet     = 0x00A0D2FFu; // JET tint (COLORREF 0x00BBGGRR): the core +
	                               //   diamond body. Default warm orange-white (kerolox
	                               //   daylight); diamonds whiten via the fp16 bloom,
	                               //   never via the palette (the Firefly law).
	DWORD plumeColBloom   = 0x00FFC8AAu; // BLOOM tint: the vacuum halo. Default pale
	                               //   blue - vacuum plumes lose the afterburning
	                               //   orange (no entrained air to burn in).
	// Outputs - module-written, dialog-read (the reentryHeat discipline: the regime
	// the model picked must be VISIBLE, or a wrong pressure blend reads as a bug).
	float plumeAtmKPa     = 0.0f;  // ambient static pressure at the vessel [kPa]
	char  plumeRegime[28] = "";    // "overexpanded - shock diamonds", "vacuum bloom", ...

	// BELL GLOW (2026-08-09) - incandescent nozzle shells. The author ships
	// Meshes\ORO\<class>_bell.msh (the heatshield-override pattern: file
	// presence IS the opt-in): a shell mesh with groups LABELed MAIN / HOVER /
	// RETRO / USER, each with its own material; ORO adds it to the CAMERA
	// TARGET vessel and drives each family's material with a thermal model -
	// sim-time integration, T ~ throttle^1/4 radiative equilibrium heating,
	// -T^4 radiative cooling (fast off white heat, then the long dull-red
	// ember tail). Emissive = blackbody(T) x this trim; diffuse alpha = the
	// thermal fade (a cold shell renders NOTHING - no z-fighting, no cold
	// geometry). The banding structure lives in the shell texture's ALPHA
	// (Textures\ORO\bell_glow.tga, the synthesized mask). Per class.
	bool  plumeBellOn     = true;  // the pill (GLOBAL, like the other enables)
	float plumeBellGlow   = 1.0f;  // 0..2 glow strength trim
	float plumeBellHeatT  = 8.0f;  // [s] time to reach full glow at 100% thrust (1..20).
	                               //   Sets the heating time constant; at partial
	                               //   throttle the RATE also scales with throttle
	                               //   (fewer watts into the same metal) and the
	                               //   EQUILIBRIUM follows throttle^(1/4) - at 10%
	                               //   thrust the bell settles at ~56% temperature,
	                               //   it never reaches full glow at all.
	float plumeBellCoolT  = 40.0f; // [s] time from full glow to sub-visible (5..120).
	                               //   Sets the -T^4 radiative constant, so the SHAPE
	                               //   of the fade (fast off white heat, the long
	                               //   dull-red ember tail) is preserved at any length.
	// Readout - module-written, dialog-read (the discipline): which families are
	// wired, or why nothing glows ("no bell mesh ...").
	char  plumeBellInfo[64] = "";

	// EXHAUST PARTICLES (2026-08-09) - Orbiter's OWN particle streams, with the
	// controls a vessel author has in code exposed as live sliders. ORO draws
	// none of this: it adds a ParticleStream per main/hover/retro thruster on the
	// camera-target stack and hands the core these values. THE FIELDS ARE THE
	// API'S FIELDS, in the API's units, deliberately - that is the whole feature.
	//
	// Defaults reproduce the DeltaGlider's own main exhaust stream
	// ({2.0, 13, 150, 0.1, 0.2, 16, 1.0, EMISSIVE} - DeltaGlider.cpp:938), because
	// the least surprising starting point for "adjust the particles" is the look
	// Orbiter already ships. Per vessel class: nozzle scale decides all of it.
	//
	// ⚠ NO WIDTH/LENGTH EXISTS. A particle is a ROUND sprite: one srcsize at birth
	// plus a growth rate. And PARTICLESTREAMSPEC has NO COLOUR - colour lives in
	// the particle TEXTURE, so prtColour drives a synthesized one (patch l); the
	// swatch greys out without the patch and the stock texture is used.
	bool  prtEnabled      = false; // OFF by default: it is a replacement for stock's
	                               //   streams, so it must be asked for
	float prtOffset       = 0.0f;  // [m] -5..+15 emission point along the flow from
	                               //   the nozzle (the core cannot move a stream
	                               //   after creation, so this rebuilds them)
	float prtSize         = 2.0f;  // srcsize     [m]    0.1..20
	float prtLifetime     = 0.2f;  // lifetime    [s]    0.05..10
	float prtRate         = 13.0f; // srcrate     [Hz]   1..100
	float prtSpeed        = 150.0f;// v0          [m/s]  0..400
	float prtSpread       = 0.1f;  // srcspread          0..1
	float prtGrowth       = 16.0f; // growthrate  [m/s]  0..30
	float prtSlowdown     = 1.0f;  // atmslowdown        0..5
	bool  prtDiffuse      = false; // ltype: false = EMISSIVE (flame), true = DIFFUSE
	                               //   (lit smoke/vapour). The single biggest look
	                               //   switch in the whole spec.
	bool  prtAirFade      = false; // atmsmap. ⚠ THIS ONE IS A TRAP IF HIDDEN: the
	                               //   stock exhaust-stream mapping is ATM_PLOG over
	                               //   1e-5..0.1, and Atm2Alpha returns 0 below amin,
	                               //   so a stream authored that way emits NOTHING in
	                               //   vacuum. Enable the effect in orbit with that as
	                               //   an invisible default and the only symptom is
	                               //   "it doesn't work". So it is a button: false =
	                               //   ATM_FLAT with amin 1.0 (always emits, the
	                               //   predictable lab default), true = the stock
	                               //   atmospheric fade.
	DWORD prtColour       = 0x00FFFFFFu; // COLORREF 0x00BBGGRR; white = the neutral
	                               //   stock-ish particle
	// Readouts - module-written, dialog-read (the reentryHeat discipline).
	char  prtInfo[64]     = "";    // how many streams, or why there are none
	int   prtCount        = 0;     // live streams

	// STOCK EXHAUST (client patch n, 2026-08-09): render the camera-target vessel's
	// stock exhaust billboards + exhaust particle streams, or suppress them so the
	// ORO overlay can be judged alone. Suppression is the CLIENT's (the exhaust
	// list cannot be safely rewritten from outside - see the roadmap assessment);
	// ORO only pushes the toggle, per invariant 18: probe by binding, grey out
	// when absent, push on change, and Ctrl+G / disarm hands stock back at once.
	// GLOBAL scope (a judging/lab preference, like the other enable flags).
	// SPLIT 2026-08-09 (his call): the two halves of "stock exhaust" are separate
	// things and belong to separate tabs. The EXHAUST tab's pill kills the stock
	// BILLBOARDS (which our plume overlay replaces); the PARTICLES tab's kills the
	// stock exhaust PARTICLE STREAMS (which our streams replace). Killing both from
	// one switch meant turning off the flame to adjust the smoke.
	bool  stockExhaust    = true;  // billboards render as always (the default)
	bool  stockParticles  = true;  // stock exhaust particle streams likewise

	// Reentry plasma LIGHT (OroReentry.cpp) - step 1 of the reentry rework. Stock draws
	// a brilliant plasma ball and then leaves the hull inside it DARK, because the effect
	// is billboards only and lights nothing. This puts a real light source at the
	// stagnation point, so the plasma illuminates the ship that is making it.
	//
	// It is the first ORO effect that belongs to BOTH domains (cf. invariant 10): the
	// emitter is created VIS_ALWAYS, so the same source lights the hull in external view
	// AND the cockpit interior from outside the windows - a glow stock has never had,
	// since its reentry effect is skipped entirely in internal view.
	//
	// PHYSICS-DRIVEN like the camera shake: the slider is a strength trim, not the
	// intensity. Hence the default of 1.0 rather than 0.
	bool  reentryEnabled  = true;
	bool  reentryVC       = false; // round 3.5: draw the plasma GEOMETRY in the
	                               //   VIRTUAL COCKPIT too (dialog VC toggle).
	                               //   VC only - 2D panel and glass stay clean.
	float reentry         = 1.0f;  // 0..1 strength trim on the plasma light
	float reentryHeat     = 0.0f;  // 0..1 heat of the CAMERA-TARGET vessel; module-written,
	                               // read by the dialog so a wrong threshold is visible as a
	                               // number instead of as "nothing happened"

	// PLASMA TUNING (round 2.6.2) - LAB scaffolding. Live multipliers the dialog
	// writes and BuildPlasmaGeometry reads every frame, so the reentry look tunes
	// IN-SIM instead of per exit/rebuild/restart cycle. Values the user settles on
	// get BAKED into the OroReentry.cpp constants later, and these return to the
	// neutral 1.0.
	// Defaults = the user's tuned look (2026-08-01, round 3.1): long parallel
	// streams, no edge light.
	float plasSat         = 1.0f;  // PALETTE saturation         (0..2; 1 = the round-5.5
	                               //   reference palette). Low = the old creamy
	                               //   orange-white, high = deep red / magenta. Slides
	                               //   every emissive plasma colour at once.
	float plasLight       = 1.0f;  // x STAGNATION LIGHT         (0..2; 0 = no emitter at
	                               //   all). The only part of the effect that lights the
	                               //   vessel's REAL mesh - our geometry is emissive and
	                               //   ignores it - so it owns the sides, the leeward
	                               //   hull, and every vessel that is not the camera target.
	// (there is no origin-tilt field: the rake the streaks start on is computed from
	//  the ANGLE OF ATTACK in BuildPlasmaGeometry - round 5.11. It had a slider for
	//  one round to find the curve, and lost it once the curve was found.)
	float plasStreakLen   = 3.0f;  // x streak length            (0..20; raised from 9 when
	                               //   the TRAIL was abandoned - the streaks carry the
	                               //   whole downstream story now)
	float plasStreakWid   = 2.0f;  // x streak width             (0..6; range x2'd)
	float plasWander      = 0.27f; // x streak wander amplitude  (0..3)
	float plasComa        = 0.0f;  // x EDGE LIGHT gain (0..2) - round 3 repurposed
	                               //   the retired coma knob's storage slot
	float plasBlob        = 1.0f;  // RETIRED (round 3, blobs no longer draw);
	                               //   field kept so nothing else shifts
	float plasSpark       = 1.0f;  // x spark count per stream   (0..6 -> 0..12 sparks)
	float plasSparkLife   = 1.0f;  // [s] spark travel time root->tip - longer = slower
	                               //   march, visible for longer (0.1..3 in effect;
	                               //   range retuned to the user's working band)
	float plasSparkSize   = 1.0f;  // x spark radius             (0..4)
	float plasShockBright = 1.0f;  // x shock-sheet brightness   (0..3; round 3.6)
	float plasShockDist   = 0.06f; // REPURPOSED 2026-08-08: envelope ("bowl") standoff
	                               //   scale, 0.10 = the automatic law, range 0..0.30
	                               //   = 0..3x it. (The SHELL standoff it used to set
	                               //   is baked at 0.015 - the 5.11 pattern.)
	float plasShellDist   = 0.015f;// SHELL standoff, fraction of hull radius (0..0.08).
	                               //   Was baked at 0.015 for half a day (2026-08-08) -
	                               //   the DG's number - then the Atlantis SANK the shell
	                               //   into its hull, so the knob returned: the standoff
	                               //   is a fact about each hull after all, like the VC
	                               //   shadow box, not one bakeable constant.
	float plasBowlSX      = 1.0f;  // envelope scale, vessel X - spanwise   (0..2, 1 = neutral)
	float plasBowlSY      = 1.0f;  // envelope scale, vessel Y - vertical   (0..2, 1 = neutral)
	float plasBowlSZ      = 1.0f;  // envelope scale, vessel Z - fore/aft   (0..2, 1 = neutral)
	                               //   lab sculpting knobs for the shock envelope (user
	                               //   request 2026-08-08); scale the LOFT in vessel axes
	                               //   around the origin, per class like all plasma tuning
	DWORD plasmaTint2     = 0x00FFFFFFu; // the SECOND colour: the plasma's magenta/pink cast
	                               //   (COLORREF 0x00BBGGRR, white = neutral). It is not one
	                               //   hard-coded constant - the cast appears in the edge
	                               //   light's fringe, the origin glow's corona, the shell's
	                               //   windward shoulder and the streak roots. Rather than
	                               //   patch four sites, this rides round 5.5's OWN definition
	                               //   of the cast - a colour is magenta exactly when b > g -
	                               //   so PCol weights it by (b - g) and every one of those
	                               //   places follows automatically, with no seam where a band
	                               //   changes and nothing to keep in step when a new one is
	                               //   added. Orange and white-hot samples have b <= g and are
	                               //   untouched by construction.
	DWORD plasmaTint      = 0x00FFFFFFu; // per-channel TINT multiply on the WHOLE plasma
	                               //   palette (COLORREF 0x00BBGGRR). White = neutral, i.e.
	                               //   the reference look exactly - so it is a pure add with
	                               //   no regression. Applied once in PCol after the
	                               //   saturation shift; a cool tint filters the red-pinned
	                               //   palette toward blue, a warm one deepens the embers.
	// THE TRAIL, take 2 (2026-08-08) - the PARTICLE POOL, not the knot ring. The old
	// trail's four knobs lived here until 2026-08-02 (graveyard G10: connected geometry
	// through accumulated knots). These drive the new architecture - independent expiring
	// particles, no connectivity - which answers G10's exam the way Firefly's smoke
	// system does: per-particle expiry + no drawn primitive ever spans two samples.
	float plasTrail       = 1.0f;  // TRAIL density (0..2; 0 = no trail - the section's
	                               //   usual "zero is off" idiom, like edge/hull light).
	                               //   Scales 1/spacing of the shed cadence.
	float plasTrailLife   = 6.0f;  // TRAIL life [s, SIM time] - the LENGTH lever: at entry
	                               //   speed 6 s ~ 45 km ("tens of km" is the spec; G9's
	                               //   hundreds-of-km is unreachable by construction)
	float plasTrailWid    = 1.0f;  // x TRAIL width (0..8; range x2'd on request) - the
	                               //   ribbon's lateral size
	float plasTrailStart  = 0.0f;  // TRAIL START position, dial -5..+5, RECENTRED (A.7):
	                               //   effective standoff = (value - 2.5) x hull size, so
	                               //   dial 0 = snug against the vessel (the old -2.5)
	                               //   and the DG's found best fit sits at -2.5 on the
	                               //   dial. Negative = deeper upstream into the fireball
	                               //   (patch g's depth clip owns the hull overlap); the
	                               //   ignition fade is part of the ribbon's SHAPE (A.6)
	                               //   and translates with it. A per-hull fact like the
	                               //   shell standoff, so it stays a knob. ⚠️ Values saved
	                               //   before A.7 shift meaning by -2.5 - re-tune, re-save.
	DWORD plasTrailTint   = 0x00FFFFFFu; // TRAIL colour pickers (COLORREF 0x00BBGGRR,
	DWORD plasTrailTint2  = 0x00FFFFFFu; //   white = the reference ramp): HEAD hue and
	                               //   TAIL hue, hue-rotated per invariant 15b's law
	                               //   (pick a colour, get that colour; the rotation
	                               //   angle blends head->tail along the VISIBLE trail,
	                               //   so two picks author the colour JOURNEY)

	// --- THE VAPOUR CONE (2026-08-11) - transonic condensation -------------------
	// Prandtl-Glauert: crossing Mach 1 the flow over the hull expands, pressure and
	// temperature drop, and if the air is moist enough the water in it CONDENSES into a
	// visible shroud. It is the one famous aerodynamic visual nothing in Orbiter has, and
	// unlike everything else in this file it happens on flights he is already making -
	// every ascent and every descent crosses the band.
	//
	// IT IS A CLOUD, NOT A GLOW, and that decides the whole implementation. Every other
	// piece of ORO geometry is emissive and draws ADDITIVE; condensed water SCATTERS and
	// OCCLUDES, so this is the first consumer of the alpha-blended draw whose recipe has
	// sat on the shelf since the trail's smoke layer died with it (graveyard G11).
	//
	// SCOPE SPLIT per invariant 17: the pill is GLOBAL (an effect enable is what the PILOT
	// wants), the three shape knobs are PER CLASS (where the cone sits and how big it is
	// are facts about a hull - the shell-standoff lesson, learned when the DG's 0.015 sank
	// the Atlantis's shell within hours).
	bool  vapEnabled      = true;  // the pill. GLOBAL.
	bool  vapTest         = false; // bypass the Mach + air gates and draw the M~0.98 look
	                               //   at full strength. NOT persisted - like every other
	                               //   TEST toggle it is a look-judging tool, and a saved
	                               //   one would put a permanent cone on a parked ship.
	float vapStrength     = 1.0f;  // x opacity of the shroud (0..2; 0 = off, the section
	                               //   idiom shared with edge light and hull light)
	float vapSize         = 1.6f;  // cone OUTER RADIUS in hull sizes (0..3). The axial
	                               //   length is NOT a knob - it falls out of the Mach
	                               //   angle (see OroVapour.cpp), which is the whole
	                               //   reason the shape reads as speed rather than as a
	                               //   decal that happens to be there.
	// THE MACH BAND, as a double-handled slider (his design, 2026-08-11 round 3 - the
	// EXPANSION BAND's control kind reused). Track spans M 0.5 .. 1.5; the handles are
	// where the shroud starts and stops existing, and the RAMPS live inside that window
	// as fixed fractions, so "tighten it or loosen it" works at any spread without the
	// fade-in and fade-out ever overlapping (invariant 23b's law, transferred).
	// ⚠️ THIS IS NOT THE USER OVERRULING THE PHYSICS - it is the LAB→PHYSICS shape he
	// described for the reentry automatic mode arriving early: the sim decides everything
	// that happens inside the window (the Mach angle, the density gate, the shading), and
	// the user sets the BOUNDS. His words: "everything is physics based, just the user has
	// some control over the look of the effect."
	float vapMachMin      = 0.85f; // shroud begins
	float vapMachMax      = 1.15f; // shroud gone
	float vapFlickHz      = 4.0f;  // [Hz] base rate of the breathing (0 = frozen, 8 = max).
	                               //   Three octaves ride it at x1.0 / x1.9 / x2.8, so the
	                               //   top one stays clear of the frame rate at the top of
	                               //   the range. Raised from a baked ~0.7 Hz on his round-2
	                               //   note that the flicker should be faster.
	float vapPos          = 0.0f;  // BIPOLAR: where the apex sits along the flow axis, in
	                               //   hull sizes (-2..+2, snap to zero). Positive =
	                               //   upstream, ahead of the vessel centre. A per-hull
	                               //   fact like the trail start and the VC cabin box, so
	                               //   it is a knob forever rather than a constant waiting
	                               //   to be baked.
	// Readouts - module-written, dialog-read (the reentryHeat discipline: a threshold
	// nobody can see is indistinguishable from a bug).
	float vapMach         = 0.0f;  // the camera-target vessel's Mach number
	float vapVis          = 0.0f;  // 0..1 combined gate - what fraction of full the cone is
	char  vapWhy[24]      = "";    // why it is zero when it is: "off", "vacuum", "subsonic",
	                               //   "too fast", "thin air", "no target"

	// --- VC SHADOWS (2026-08-04, client patch f) ---------------------------------
	// NOT an ORO effect - ORO draws nothing here. These drive the patched client's
	// virtual-cockpit shadow pass through gcCore::SetVCShadows, which is why they carry
	// no enable pill of the usual kind and why the whole section greys out on a client
	// without patch (f) (CanSetVCShadows). Filed in the dialog anyway because it is the
	// immersion panel, and "sunlight moving across the cockpit as you rotate" is exactly
	// what this addon is for - it just happens to be rendered by the client.
	//
	// There is deliberately no ShadowMapFilter control: that value is a D3DXMACRO baked
	// into D3D9Client.fx when clbkCreateRenderWindow compiles the effect, so nothing can
	// change it mid-session. It lives in the Launchpad D3D9 setup and applies on the next
	// scenario launch.
	bool  vcShadows       = true;  // false = skip the internal-pass shadow map entirely
	float vcShadowRadius  = 2.2f;  // [m] half-width of the ortho box fitted around the eye.
	// ORO patch (p): how much of the material AMBIENT the shadow takes with it.
	// Stock self-shadowing scales the SUN term only, so a shadowed VC surface keeps
	// all its ambient and emissive - and a cockpit is authored with plenty of both,
	// which is exactly why VC shadows read as a faint grey smudge that no Launchpad
	// setting can deepen. 0 = stock, 1 = the shadow removes the ambient entirely.
	// EMISSIVE IS NEVER SCALED (his call): a lit MFD does not dim because a canopy
	// frame passes over it. PER VESSEL CLASS, like the cabin box - the right value
	// depends on how that particular VC was authored, not on who is flying it.
	float vcShadowDepth   = 0.0f;
	                               //   MEASURED on the stock DeltaGlider (2026-08-04), not
	                               //   guessed: the sharpest box that still contains its
	                               //   canopy structure. It stays a slider because unlike
	                               //   the origin rake there is no curve to fit - cabin
	                               //   size is a fact about the hull, so it is stored per
	                               //   vessel class and the DG's number is only a default.
	                               //   The map is fitted to the CABIN, not the hull: the
	                               //   exterior box is ~1 cm/texel on a DG, which stair-
	                               //   steps on a panel 40 cm from your face. Smaller is
	                               //   sharper but stops distant geometry casting into the
	                               //   cabin - per-vessel taste, hence per-vessel-class
	                               //   storage (invariant 17).

	// --- ECLIPSE (2026-08-02) - the first effect that is BOTH domains at once -----
	// The camera sits in another body's shadow: the sun's disc is partly or fully
	// covered, and the light goes. Pure geometry (OroEclipse.cpp), no client patch
	// beyond the IPI pipeline we already have.
	//
	// It is deliberately built as an EYE, not as a dimmer, and that is what keeps it
	// honest next to the renderer. D3D9Client already darkens the planet surface under
	// a moon's shadow (vPlanet::SetupEclipse) and already cuts a vessel's sunlight in
	// its primary's shadow (vVessel::ModLighting -> SunOcclusionByPlanet). What NO
	// renderer models is the observer: the lag of dark adaptation, the glare when the
	// sun comes back, and the colour draining out of a scene lit below cone threshold.
	// Those three are ours, they never double-count what the client already did, and
	// they are the reason an orbital sunset feels like anything at all.
	bool  eclipseEnabled  = true;
	float eclipseDim      = 0.55f; // 0..1 STEADY darkening while obscured. Driven by the
	                               //   obscuration from bodies OTHER than the one you are
	                               //   at - see OroEclipse.cpp for why the primary is
	                               //   excluded from this one term and nothing else.
	float eclipseAdapt    = 1.00f; // 0..1 how much of the eye's adaptation lag shows:
	                               //   blindness going in, glare coming out. Centred on
	                               //   1.0 gain, so at full adaptation it does NOTHING.
	float eclipseColour   = 0.70f; // 0..1 scotopic colour loss - rods carry no colour, and
	                               //   they peak blue-green, so the residue is a cool grey
	bool  eclipseTest     = false; // dialog TEST toggle: run a synthetic eclipse cycle so
	                               //   the effect can be seen without waiting for one
	// Outputs - module-written, dialog-read (the same rule as reentryHeat and the felt-G
	// readouts: a number you cannot see is a threshold you cannot argue with).
	float eclipseObsc     = 0.0f;  // 0..1 fraction of the SOLAR DISC AREA covered, at the camera
	float eclipseGain     = 1.0f;  // the eye's current brightness multiplier (1 = adapted)
	char  eclipseBody[32] = "";    // name of the body doing the occulting ("" = none)

	// --- GOD RAYS (2026-08-10) - crepuscular shafts from the sun ------------------
	// Light scattering out of the sunbeam on its way past an occluder. The classic
	// radial-occlusion post-process, and it lands in ORO's lap almost finished:
	// D3D9Client draws its sun glare into the backbuffer AFTER the bloom resolve and
	// BEFORE the HUD stages where we capture, so the frame we resample already holds a
	// bright, correctly-occluded sun. We never have to answer "is the sun behind the
	// hull / the terrain / the limb" - the client already did, and its answer is in the
	// pixels. See OroGodRays.cpp and PSGodRay in orofx.hlsl.
	//
	// SHARES THE ECLIPSE'S SUN. Both effects ask where the sun is and how blocked it
	// is; OroFindStar() is the one place that decides which body is the star.
	//
	// ATMOSPHERE IS A GATE, NOT A SLIDER. There is no medium in vacuum and therefore no
	// scattering, so in orbit this pass does not run at all - the same class of ruling
	// as day-side lightning (invariant 22f). That gate is also why the settings are
	// GLOBAL rather than per body: the physical difference between a thick atmosphere
	// and a thin one is already handled by the density the sim reports, and asking the
	// user to re-tune per world would be asking him to adjust something the sim knows.
	bool  grayEnabled  = true;
	float grayStrength = 0.55f;    // 0..1 master intensity of the shafts
	float grayLength   = 0.90f;    // 0..1 how far along the pixel->sun span the march runs;
	                               //   short = stubby shafts hugging the disc, long = reaching.
	                               //   HIGH by default: the reference photographs all show rays
	                               //   crossing the whole sky, and a pixel only gets one if its
	                               //   march actually reaches the bright source.
	float grayDecay    = 0.75f;    // 0..1 -> per-sample falloff; low = crisp short rays,
	                               //   high = long soft ones. Remapped before it reaches HLSL.
	                               //   The SECOND half of reach: it sets how much the far end of
	                               //   the march (the end nearest the sun) still counts for.
	float graySens     = 0.35f;    // 0..1 SENSITIVITY - how dim a thing is still allowed to
	                               //   cast a shaft. THE knob that decides "shafts" vs
	                               //   "radial blur over the whole sky": low = only the
	                               //   blown-out sun casts, high = bright cloud edges do too.
	                               //   ⚠ This was a THRESHOLD first (2026-08-10) and it was a
	                               //   mistake: raising it made the effect WEAKER, which is
	                               //   backwards from every other slider in the panel, and the
	                               //   user duly maxed everything and saw nothing. Inverted
	                               //   the same day. More is more, everywhere, no exceptions.
	                               //   Defaults LOW on purpose: a bright daylit sky is not far
	                               //   off the threshold, and letting it in turns shafts into
	                               //   a wash over the whole upper frame.
	float grayWarm     = 0.70f;    // 0..1 how much the shafts redden as the sun nears the
	                               //   horizon (long path through air = Rayleigh scattering
	                               //   takes the blue out). 0 = always neutral white.
	bool  grayTest     = false;    // dialog TEST toggle: bypass the atmosphere + elevation
	                               //   gates so the effect is judgeable from orbit or at noon
	// Outputs - module-written, dialog-read. "No rays" has several honest causes (vacuum,
	// sun behind you, sun too high) and the readout is what tells those from "broken".
	float grayVis      = 0.0f;     // 0..1 the combined gate actually applied this frame
	char  grayWhy[40]  = "";       // one-line reason when grayVis is 0

	// --- AURORA (2026-08-05) - the auroral curtains ------------------------------
	// Additive ribbon GEOMETRY - "a curtain IS a ribbon", so the whole round-5 plasma
	// machinery transfers (per-vertex projection, mitred/Gouraud ribbons, additive
	// draw). It needs NO client patch: a Sketchpad additive triangle poly, exactly like
	// the plasma. Drawn around the primary's magnetic poles - approximated by the
	// GEOGRAPHIC poles, since Orbiter models no magnetic field - whenever the body has
	// an atmosphere; occlusion against the planet AND the camera-target vessel is a
	// ray-sphere test (NOT invariant 16's depth map, which the curtains have no need of).
	// EXTERNAL VIEW ONLY: like the shimmer, screen-space geometry with no depth cannot
	// sit behind the cockpit, so it stays external until patch (g). (The roadmap wanted
	// domain BOTH; that awaits the depth buffer - which also brings the VC plasma through
	// the windows.)
	//
	// GLOBAL scope like the eclipse: the look is the pilot's taste and a property of
	// the sky, not of the hull, so these are NOT per-vessel-class and NOT baked away.
	bool  auroraEnabled  = true;
	float auroraActivity = 0.0f;   // 0..1 overall brightness/extent - a stand-in for the
	                               //   solar/geomagnetic activity we do not model.
	                               //   DEFAULT 0, AND THAT IS THE OPT-IN: a world with no
	                               //   cfg has no aurora because its activity is zero, not
	                               //   because of a separate enable flag. Raise the slider
	                               //   at any world and curtains appear; save, and that
	                               //   world has them from then on. One honest control
	                               //   instead of two that can contradict each other (an
	                               //   explicit AuroraEnable toggle was built 2026-08-07
	                               //   and removed the same day - it blocked exactly the
	                               //   "turn it up and see something" flow it was meant
	                               //   to guard).
	float auroraReach    = 0.26f;  // 0..1 how far EQUATORWARD the oval sits - the geomagnetic
	                               //   activity that pushes the auroral oval to lower latitudes
	                               //   in a storm. 0 = quiet (near the pole, ~78 deg); 1 = big
	                               //   storm (~40 deg, low enough for the ISS at 56 deg incl).
	                               //   Default 0.26 == the ~68 deg oval the first build shipped.
	float auroraFold     = 0.5f;   // 0..1 drapery: how far the curtain waves in and out
	float auroraRays     = 0.6f;   // 0..1 ray contrast: the vertical brightness striations
	float auroraBreakup  = 0.4f;   // 0..1 how broken the ring is into separate BANDS: 0 = one
	                               //   closed loop, up = irregular disconnected segments with
	                               //   soft ends (each ring seeded differently, gaps drift in time)
	float auroraBase     = 0.46f;  // 0..1 -> BASE altitude of the curtain (AUR_BASE_MIN..MAX);
	                               //   default 0.46 == the ~95 km the first builds hard-coded.
	                               //   Lower it to drop the aurora toward the atmosphere limb.
	float auroraHeight   = 0.5f;   // 0..1 -> TOP altitude of the curtain (AUR_H1_MIN..MAX)
	float auroraThick    = 0.0f;   // 0..1 curtain THICKNESS. A curtain is a sheet with no
	                               //   depth; this stacks 1..4 parallel sheets across a
	                               //   widening band of colatitude, which is what an emissive
	                               //   VOLUME looks like - and it brings limb brightening
	                               //   with it for free, since edge-on you look through more
	                               //   sheets than face-on. 0 = the single-sheet original
	                               //   look and costs exactly nothing.
	float auroraTiltX    = 0.0f;   // [deg] -90..+90 - offset of the auroral oval's centre
	float auroraTiltY    = 0.0f;   //   from the GEOGRAPHIC pole, toward the prime meridian
	                               //   (X) and 90 deg east of it (Y). This is the magnetic
	                               //   pole offset, and it is real physics before it is
	                               //   exotic: Earth's sits ~11 deg from the spin axis. Both
	                               //   ovals move together as a DIPOLE - tilting north by X
	                               //   tilts south by -X - because the axis is what tilts.
	int   auroraRibbons  = 2;      // 1..6 concentric curtains per pole. Default 2 = the main
	                               //   ring + one fainter poleward companion the build shipped.
	                               //   (A per-body DEFAULT lands in the body cfg in Stage B;
	                               //   this stays the user's live override.)
	// THREE colours, one per ALTITUDE BAND (COLORREF 0x00BBGGRR), because that is how the
	// physics works: which species emits, and which of its lines, is decided by how deep the
	// particles get. Earth is the proof that two are not enough - its lower border is
	// nitrogen VIOLET-PINK and its top is oxygen RED, so a single shared "edge" colour
	// cannot render it. Defaults below are Earth's.
	DWORD auroraColBase  = 0x00AF5AC8u; // the sharp LOWER BORDER - nitrogen, below ~96 km
	DWORD auroraColBody  = 0x0069E13Cu; // the MAIN BODY - 557.7 nm oxygen green, 96-193 km
	DWORD auroraColTop   = 0x00373CCDu; // the DIFFUSE TOP - 630 nm oxygen red, above ~240 km
	bool  auroraTest     = false;  // dialog TEST toggle: draw one oval centred on the
	                               //   SUB-CAMERA point (night fade off) so the curtains are
	                               //   visible wherever you are - a real display needs a
	                               //   polar night, which is not something you can wait for.
	// Output - module-written, dialog-read (the eclipse/plasma-heat readout discipline:
	// a name you can see tells "no auroral planet in range" from "the effect is broken").
	char  auroraBody[32] = "";     // planet the curtains are drawn at ("" = none in range)

	// --- PER BODY (Config\ORO\bodies\<name>.cfg) - the THIRD settings scope ------
	// A body's aurora is a fact about THAT WORLD: Jupiter's curtains are not Earth's at a
	// different brightness, they sit at different altitudes, in a tighter oval, in a
	// different colour. So the RANGES the sliders move within, and the default colours and
	// ribbon count, come from a per-body file. ORO ships and OWNS these files
	// (Config\ORO\bodies\), deliberately NOT the body's own Orbiter cfg - editing stock
	// config files risks breaking things the user did not ask us to touch, and this leaves
	// room for weather/cloud data later without bloating them (user's call, 2026-08-07).
	//
	// STRICT OPT-IN, carried by auroraActivity above: a world with no file loads the
	// defaults, and the default activity is ZERO, so it is silent until you turn it up.
	// No separate enable flag - see the note on auroraActivity.
	float aurBaseMinKm   =  40.0f; // Base slider 0..1 spans this altitude range [km]
	float aurBaseMaxKm   = 160.0f;
	float aurTopMinKm    = 180.0f; // Top slider 0..1 spans this range [km]
	float aurTopMaxKm    = 400.0f;
	float aurColatMinDeg =  12.0f; // Oval lat slider 0..1 spans this COLATITUDE range [deg]
	float aurColatMaxDeg =  50.0f; //   (from the pole; the dialog shows the latitude)

	// --- LIGHTNING (2026-08-08) - flashes in the cloud deck, seen from above -------
	// The first effect that READS THE WORLD'S OWN DATA: storm cells spawn from the
	// planet's cloud tile alpha (Textures\<body>\Archive\Cloud.tree - the same tiles the
	// client renders), so the flashes sit inside the actual visible cloud masses and a
	// flash straddling a deck edge dies exactly where the cloud does. v1 scope (user's
	// call, 2026-08-08): visible from ABOVE the deck only, Earth first, no thunder, no
	// cabin illumination - the discs are visible through the VC window like the aurora
	// (depth-clipped), but nothing lights the cockpit.
	//
	// Scope split mirrors the aurora exactly: the pill is GLOBAL ("do I want storms at
	// all"); everything that says what a WORLD'S storms ARE - activity, cadence, cell
	// size, flash colour - is PER BODY, and ACTIVITY IS THE OPT-IN (default 0, no
	// second enable flag - invariant 17b's law).
	bool  ltgEnabled   = true;     // section pill (GLOBAL)
	bool  ltgTest      = false;    // TEST: one guaranteed fast cell near the sub-camera
	                               //   point, coverage/night/altitude gates bypassed - a
	                               //   real storm needs the night side and live cloud,
	                               //   which is not something you can wait for
	float ltgActivity  = 0.0f;     // 0..1 how much of the planet is storming (cell count).
	                               //   PER BODY, default 0 = the opt-in.
	float ltgBright    = 0.65f;    // 0..1 flash brightness (per body)
	float ltgRate      = 0.55f;    // 0..1 per-cell flash cadence (per body)
	float ltgCellKm    = 0.45f;    // 0..1 -> cell glow radius (LTG_CELL_MIN..MAX km; the
	                               //   dialog readout shows the km). Per body.
	DWORD ltgColour    = 0x00FFD8B4u; // flash tint (COLORREF 0x00BBGGRR). Default is the
	                               //   ISS-photo blue-white (R180 G216 B255): lightning
	                               //   through cloud reads BLUE-VIOLET from orbit, the
	                               //   opposite palette pole from the reentry plasma.
	// Outputs - module-written, dialog-read (the reentryHeat/aurora discipline: a name
	// and a count you can see tell "no storms in range" from "the effect is broken").
	char  ltgBody[32]  = "";       // world the storm field is evaluated at ("" = none)
	int   ltgCells     = 0;        // active storm cells in the visible cap right now

	// FLIGHT AID (2026-08-02) - the TEST RIG, not an effect. A live centre-of-pressure
	// shift on the FOCUS vessel: stock vessels are trimmed to weathervane to low AoA,
	// so they drop the nose into the flight direction during an entry and there is no
	// plasma worth filming. Recompiling each vessel does not scale (the DG needed it,
	// Atlantis needs it, every test vessel will) - and it CANNOT be done through the
	// airfoil API from outside: EditAirfoil needs an AIRFOILHANDLE that only the
	// vessel's own code receives, and nothing enumerates them. So ORO reproduces the
	// shift with a force COUPLE instead (see UpdateCopShift).
	float copShift        = 0.0f;  // [m] +forward / -aft; 0 = the vessel exactly as coded
	float copMoment       = 0.0f;  // [kN m] pitch moment being added, +nose-up;
	                               //   module-written readout (the dialog shows it - an
	                               //   aid you cannot see working is an aid you cannot tune)

	// CANCEL THRUST (2026-08-09) - the TEST-STAND rig, the flight aid's sibling: a
	// per-timestep counter-force nulls the focus vessel's own total thrust at the CoM,
	// so the engines fire at any throttle while the ship stays put - built for plume
	// tuning (the DG rolled off the runway before the sliders got a fair try).
	// SESSION-ONLY, deliberately in NO settings table: a persisted thrust-cancel
	// silently loaded into a launch scenario reads as "my engines are dead".
	// Ctrl+G / disarm releases it instantly (invariant 9's discipline).
	bool  cancelThrust    = false;

	// Blink: the dialog button REQUESTS one; the module runs the eyelid envelope
	// on the real-time clock in clbkPreStep and publishes the closure amount for
	// the renderer. (Physics phase: triggered automatically on G-LOC recovery.)
	bool  blinkRequest    = false; // set by the dialog, consumed by clbkPreStep
	float blinkAmount     = 0.0f;  // 0..1 eyelid closure, written by the module ONLY

	// INDUCE scenarios: one-click scripted effect SEQUENCES. The dialog's Induce
	// buttons set seqRequest (>=0 = that scenario was clicked); the module consumes it
	// in clbkPreStep, plays the timeline (overwriting the effect values above each
	// frame), and publishes the currently-playing scenario in seqActive (-1 = none)
	// so the dialog can lock+animate the sliders and highlight the active button.
	// Clicking the active scenario again toggles it off (module handles the toggle).
	int   seqRequest = -1;         // dialog -> module: scenario clicked (>=0), else -1
	int   seqActive  = -1;         // module -> dialog: playing scenario, -1 = none

	// Scenario sound: ONE toggle for the whole INDUCE/RECOVER section. When on, each
	// scenario plays its matching clip (Induce_*/Recover_*.wav) for its duration.
	bool  seqSoundEnabled = true;

	// ========================================================================
	// PHYSICS (the felt-G model) - OroPhysics.cpp
	// ------------------------------------------------------------------------
	// LAB mode (default): every slider above IS the effect value, exactly as the
	// whole lab phase worked. PHYSICS mode: the model computes those values from the
	// vessel's real motion and the sliders become per-effect GAINS instead. The gains
	// live in their own fields below so a mode flip never destroys either set - the
	// dialog just points its sliders at the other pointer (and shows the DRIVEN value
	// in the readout column, so you can trim a gain while watching what it produces).
	// ========================================================================
	bool  physicsMode  = false;
	float gTolerance   = 0.5f;   // 0..1 -> +Gz symptom threshold, ~2.5 .. 5.0 G
	bool  gsuitOn      = false;  // anti-G suit: +1.5 G on the +Gz threshold (does NOT help -Gz)
	int   pilotPose    = 0;      // index into POSES[] (seated / reclined / prone / standing / couch)
	bool  gRefCamera   = true;   // true = G at the CAMERA position (pilot/copilot/passenger),
	                             // false = at the vessel CoM. In orbit the difference is the
	                             // WHOLE effect: free-falling, the CoM feels exactly zero.

	// Per-effect gains, used ONLY in physics mode (0 = effect suppressed, 1 = full model).
	float gainBlackout   = 1.0f;
	float gainRedout     = 1.0f;
	float gainTunnel     = 1.0f;
	float gainSpots      = 1.0f;
	float gainGreyout    = 1.0f;
	float gainBlur       = 1.0f;
	float gainHeartbeat  = 1.0f;
	float gainAberration = 1.0f;
	float gainSparkles   = 1.0f;
	float gainSwim       = 1.0f;
	float gainTilt       = 1.0f;

	// Model OUTPUTS - written by the module only, read by the dialog's readout.
	// Signed, in g, in PILOT BODY axes (not vessel axes):
	float feltGz   = 0.0f;       // +Gz blood footward (grey-out chain) / -Gz headward (red-out)
	float feltGx   = 0.0f;       // +Gx "eyeballs in" / -Gx "eyeballs out" (globe deforms -> blur)
	float feltGy   = 0.0f;       // +Gy toward the pilot's right (head lolls -> tilt lean)
	float gReserve = 1.0f;       // 1 = fully oxygenated, 0 = G-LOC. THE state variable: symptoms
	                             // track this, not instantaneous G, which is why duration and
	                             // onset rate matter and a 6 G snap barely registers.

	// Signed steady head lean from lateral G, -1..+1. Separate from `tilt` (which stays the
	// unipolar 0..1 SWAY amplitude) so the lab slider and the scenarios are untouched - the
	// shader adds the two. Physics writes this; nothing else does.
	float tiltLean = 0.0f;
};

extern OroEffectState g_fx;

// ----------------------------------------------------------------------------
// Felt-G model queries (OroPhysics.cpp), for the dialog's PILOT section. Kept
// here rather than in a header of their own because the dialog already includes
// this file and these are all about the state above.
// ----------------------------------------------------------------------------
int         OroPhys_PoseCount();       // number of pilot postures
const char* OroPhys_PoseName(int i);   // "Seated", "Reclined", ...
// Aurora oval latitude (OroAurora.cpp) for the dialog readout - the geographic
// latitude the current Reach knob puts the oval at, computed from the same MIN/MAX
// mapping the build uses, so the dialog shows degrees without duplicating the constants.
float       OroAurora_OvalLatDeg();
float       OroAurora_BaseAltKm();     // the base/top altitudes the sliders produce, in km,
float       OroAurora_TopAltKm();      //   for the dialog readouts (same mapping the build uses)
// Lightning (OroLightning.cpp): the cell radius the Cell size slider produces, in km,
// for the dialog readout (same mapping the build uses); and the module-lifetime cloud-map
// teardown (closes the .tree handle, frees the TOC + decoded tile cache).
float       OroLightning_CellKm();
void        OroLightning_Close();
// Bell glow (OroBell.cpp): drop the per-class template cache at a SESSION BOUNDARY.
// Its MESHHANDLE comes from oapiLoadMeshGlobal and is valid for exactly one session (both
// the core's global mesh manager and the client's device-side copy release it at session
// end), while the cache holding it is a file-static that outlives them. Reusing it across a
// reload is a use-after-free that lands as an access violation inside D3D9Client.dll - the
// reload CTD's third face. See the long note on BellCfg.
void        OroBell_Reset();

float       OroPhys_GzThreshold();     // the +Gz symptom threshold [G] the current
                                         // tolerance / G-suit / posture produce - the
                                         // dialog shows THIS, not a meaningless 0..1

// ----------------------------------------------------------------------------
// Settings persistence (OroModule.cpp) - Config\ORO.cfg, Orbiter's own
// key=value format via oapiWriteItem_*. Saved by the dialog's SAVE button,
// loaded ONCE when the module is constructed (never per scenario start - that
// would throw away tweaks made earlier in the same Orbiter run).
// ----------------------------------------------------------------------------
bool        OroSettings_Save();        // global + the current vessel class + body
void        OroSettings_Load();        // global only; missing key = keep the default
void        OroSettings_LoadClass(const char* cls);   // swap in a class's numbers
const char* OroSettings_Path();        // for the dialog's confirmation line
const char* OroSettings_Class();       // class currently in effect ("" = none yet)
void        OroClassFileName(const char* cls, char* out, int cap);
                                         // the class-name -> leaf-file sanitiser, shared so
                                         // Config\ORO\<class>.cfg and the heatshield mesh
                                         // Meshes\ORO\<class>.msh always pair up

// Per-BODY aurora settings (Config\ORO\bodies\<name>.cfg) - the third scope.
// SCOPE BITS for the per-tab SAVE buttons: each tab writes only the files it can
// actually have changed, and the status line names them.
const int   ORO_SCOPE_GLOBAL = 1;
const int   ORO_SCOPE_CLASS  = 2;
const int   ORO_SCOPE_BODY   = 4;
bool        OroSettings_SaveScope(int mask);          // targeted save; returns false on write error
void        OroSettings_LoadBody(const char* body);   // swap in a body's aurora numbers
const char* OroSettings_Body();        // body currently loaded ("" = none yet)
// True if this world has an aurora file. The aurora's body selection uses it so a
// CONFIGURED world qualifies even when Orbiter gives it no atmosphere - which is the only
// way Ganymede (the one moon with its own magnetic field) and Europa can glow at all.
bool        OroSettings_BodyHasFile(const char* name);

// True if the running client carries patch (f) - i.e. the VC SHADOWS section has
// something to control. Probed by BINDING (gcCore::CanSetVCShadows null-checks the
// bound pointer), so a stale build stamp cannot fool it. The dialog greys the whole
// section out when this is false: a switch that cannot do anything is worse than none.
bool        OroVCShadowsSupported();

// True while patch (g)'s per-pixel depth clip is LIVE this session - the client
// carries it AND the scene depth buffer exists (SunGlare on). The REENTRY tab
// swaps its caption to a warning when false, because the degradation is silent
// on screen (streaks paint through the hull) and only the log said why - a
// whole confused round on 2026-08-08 came from exactly that.
bool        OroDepthClipOK();

// True if the running client carries patch (n) - per-vessel stock-exhaust
// suppression. Probed by binding (CanSuppressExhaust); the THRUSTER tab's
// STOCK EXHAUST pill greys out when false.
bool        OroStockExhaustSupported();

// True if the running client carries patch (l), so ORO can synthesize a tinted
// particle texture. PARTICLESTREAMSPEC has no colour field - colour lives in the
// texture - so this gates the PARTICLES sub-tab's COLOUR swatch only. Every other
// control on that tab is stock core API and works on any client.
bool        OroParticleTintOK();
