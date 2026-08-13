// ==============================================================
// OroPhysics.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - the felt-G model  (OroModule::UpdatePhysics, its own translation unit)
// ----------------------------------------------------------------------------
// Turns the effects lab into a simulation: the vessel's real motion drives the
// vision suite, and the dialog sliders become per-effect gains.
//
// MAIN THREAD ONLY (called from clbkPreStep). Makes oapi/VESSEL calls, so it must
// never be reached from the render callback (invariant 1).
//
// The chain, in order:
//   1. PROPER acceleration at the centre of mass   = (F_total - W_gravity) / m
//   2. carried to the PILOT'S HEAD                 + w x (w x r) + alpha x r
//   3. rotated into PILOT BODY axes                 (the posture triad)
//   4. through the physiology                       (cardiovascular lag, onset rate,
//                                                    cerebral oxygen reserve)
//   5. out to the effects                           (each axis owns different ones)
//
// WHY PROPER ACCELERATION AND NOT TOTAL. A body feels the forces that act on its
// SURFACE - seat, harness, floor - not gravity, which acts on every cell at once.
// That is why free fall feels like nothing while parked on a runway feels like 1 G.
// GetForceVector sums ALL forces (thrust, lift, drag, ground contact, user-defined);
// subtracting the weight vector leaves exactly what an accelerometer would read.
//
// AXIS OWNERSHIP - each is a different mechanism, so each drives different effects:
//   +/-Gz  PERFUSION. The hydrostatic blood column between heart and eye only exists
//          along the spine. +Gz drains the retina (grey-out -> tunnel -> blackout),
//          -Gz congests it (red-out). This axis owns the vision suite.
//   +/-Gx  MECHANICAL. No head-foot gradient, so none of the above fires. Instead the
//          eyeball is deformed in its orbit, shifting its refractive power: BLUR and
//          optical fringing. Much higher thresholds - eyeballs-out matters around 6 G,
//          eyeballs-in is tolerable far past that (which is why you launch on your back).
//   +/-Gy  POSTURAL. The head lolls toward the lateral load, so the visual field rolls
//          with it: a SIGNED lean (tiltLean), not the lab's unipolar sway.
//
// HANDEDNESS. Orbiter's vessel frame is LEFT-handed (+X right, +Y up, +Z forward) and
// VesselAPI.h:751 warns about cross-product operand order because of it. The centripetal
// term is therefore written as the BAC-CAB expansion w(w.r) - r(w.w) - pure dot products,
// which no handedness convention can flip. Only the tangential term alpha x r needs a
// real cross product, and it only appears while angular rates are CHANGING.
// ============================================================================

#include "OroModule.h"
#include "OroState.h"

#include <math.h>

namespace {

	const double G0 = 9.80665;          // standard gravity [m/s^2]

	// --- physiology constants (lab-tunable; the dialog knob moves the threshold) ---
	const double CV_LAG_TAU    = 0.80;  // [s] cardiovascular response lag - the eye follows a
	                                    //     smoothed G, not the instantaneous value
	const double ONSET_TAU     = 0.50;  // [s] smoothing on the measured onset RATE
	const double RESERVE_SEC   = 5.0;   // [s] to empty the cerebral oxygen reserve at 1 G over
	                                    //     threshold. THE number that makes duration matter.
	const double REFILL_RATE   = 0.16;  // [1/s] reserve recovery below threshold - deliberately
	                                    //     slower than the drain: you recover after the pull
	const double GLOC_HOLD     = 14.0;  // [s] absolute + relative incapacitation after G-LOC.
	                                    //     Effects stay pinned even if the G comes straight off.

	inline float sat01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

	inline float sstep(float e0, float e1, float x)
	{
		if (e1 <= e0) return x >= e1 ? 1.0f : 0.0f;
		const float t = sat01((x - e0) / (e1 - e0));
		return t * t * (3.0f - 2.0f * t);
	}

	// Pilot posture: the body triad expressed in VESSEL coordinates, plus what the
	// posture is worth in +Gz tolerance.
	//   head  = feet -> head (the spine).  +Gz when proper acceleration points along it.
	//   chest = back -> chest.             +Gx ("eyeballs in") when it points along it.
	//   right = left -> right shoulder.    +Gy when it points along it.
	// Sanity check on `head`: a seated pilot pulling up feels the seat push UP (+Y) and
	// that is textbook +Gz, blood to the feet. Sanity check on `chest`: the same pilot
	// accelerating forward (+Z) is pressed back into the seat - +Gx, eyeballs in.
	struct PilotPose {
		const char* name;
		VECTOR3     head, chest, right;
		float       gzBonus;            // [G] added to the +Gz symptom threshold
	};

	const PilotPose POSES[] = {
		// Upright in a seat - the default, and what almost every Orbiter vessel is.
		{ "Seated",   { 0, 1, 0 },              { 0, 0, 1 },              { 1, 0, 0 },  0.0f },
		// Reclined 30 deg (F-16 style): shortens the heart-to-eye column, worth about a G.
		{ "Reclined", { 0, 0.8660, -0.5000 },   { 0, 0.5000, 0.8660 },    { 1, 0, 0 },  1.0f },
		// Face-down, head toward the nose. The thrust axis leaves the spine entirely.
		{ "Prone",    { 0, 0, 1 },              { 0, -1, 0 },             { 1, 0, 0 },  2.0f },
		// Standing: no seat support, slightly worse than seated.
		{ "Standing", { 0, 1, 0 },              { 0, 0, 1 },              { 1, 0, 0 }, -0.5f },
		// On the back in a couch, head toward the nose, chest facing vessel-up.
		{ "Couch",    { 0, 0, 1 },              { 0, 1, 0 },              { 1, 0, 0 },  1.5f },
	};
	const int NPOSE = (int)(sizeof(POSES) / sizeof(POSES[0]));

}  // namespace

// Number of poses + their names, for the dialog's cycling button.
int         OroPhys_PoseCount()      { return NPOSE; }
const char* OroPhys_PoseName(int i)  { return POSES[(i < 0 || i >= NPOSE) ? 0 : i].name; }

// The +Gz threshold the current settings produce, in G. The dialog shows this instead
// of a meaningless 0..1, so the tolerance knob reads in the units it actually means.
// (Does not include the onset-rate penalty, which is transient by nature.)
float OroPhys_GzThreshold()
{
	const PilotPose& P = POSES[(g_fx.pilotPose < 0 || g_fx.pilotPose >= NPOSE) ? 0 : g_fx.pilotPose];
	return 2.5f + g_fx.gTolerance * 2.5f + (g_fx.gsuitOn ? 1.5f : 0.0f) + P.gzBonus;
}

// ----------------------------------------------------------------------------
// The vessel's camera offset WITHOUT our own camera shake in it.
//
// Invariant 9: UpdateCameraShake MUTATES SetCameraOffset every frame and tracks the
// delta so it can recover the clean base. The physics runs BEFORE it in clbkPreStep,
// so GetCameraOffset still carries LAST frame's shake - and feeding that back in would
// close a loop (shake -> G -> effects -> shake). Same jump test as the shake uses: if
// the offset moved by more than our delta could explain, the vessel reseated the camera
// (pilot/copilot switch) and the current value IS the clean base.
// ----------------------------------------------------------------------------
VECTOR3 OroModule::CleanCameraOffset(VESSEL* v) const
{
	VECTOR3 cur = _V(0, 0, 0);
	if (!v) return cur;
	v->GetCameraOffset(cur);
	if (!camActive) return cur;
	return (length(cur - camApplied) > SHAKE_RESET) ? cur : (cur - camDelta);
}

// ----------------------------------------------------------------------------
// The model. Runs every frame from clbkPreStep while PHYSICS mode is on.
// ----------------------------------------------------------------------------
void OroModule::UpdatePhysics()
{
	// --- mode edges -------------------------------------------------------
	if (!g_fx.physicsMode) {
		if (physWasOn) {          // leaving PHYSICS: clear what the model was driving,
			ZeroDrivenEffects();  // so the lab does not inherit a half-blacked-out frame
			g_fx.tiltLean = 0.0f;
			physWasOn = false;
		}
		return;                   // LAB mode: the sliders are the values, hands off
	}
	if (!physWasOn) {             // entering PHYSICS: start from a healthy pilot
		physWasOn  = true;
		physGzFilt = physGyFilt = physOnset = 0.0f;
		physGzPrev = 0.0f;
		physGlocT  = -1.0;
		g_fx.gReserve = 1.0f;
		ZeroDrivenEffects();
	}

	// A scenario OWNS the effect values while it plays (invariant 8). Two writers on one
	// set of fields is a fight nobody wins - the dialog also refuses to start one in
	// PHYSICS mode, this is the belt to that's braces.
	if (g_fx.seqActive >= 0) return;

	VESSEL* v = oapiGetFocusInterface();
	const double m = v ? v->GetMass() : 0.0;
	if (!v || m < 1.0) {
		g_fx.feltGz = g_fx.feltGx = g_fx.feltGy = 0.0f;
		return;
	}

	// --- 1. proper acceleration at the centre of mass, vessel-local [m/s^2] ---
	VECTOR3 F = _V(0, 0, 0), W = _V(0, 0, 0);
	v->GetForceVector(F);
	v->GetWeightVector(W);
	VECTOR3 a = (F - W) / m;

	// --- 2. carry it out to the pilot's head ------------------------------
	// For a point r from the CoM of a rigid body: a_P = a_CoM + w x (w x r) + alpha x r.
	// IN ORBIT THIS TERM IS THE ENTIRE EFFECT. Free-falling with the RCS idle, a_CoM is
	// exactly zero, so every G a spinning pilot feels comes from being off-axis - which
	// is also why the copilot and the passengers read differently from each other.
	if (g_fx.gRefCamera) {
		const VECTOR3 r = CleanCameraOffset(v);
		VECTOR3 w = _V(0, 0, 0), al = _V(0, 0, 0);
		v->GetAngularVel(w);
		v->GetAngularAcc(al);
		const VECTOR3 aCent = w * dotp(w, r) - r * dotp(w, w);   // handedness-immune (see header)
		const VECTOR3 aTan  = crossp(al, r);                     // only while rates CHANGE
		a = a + aCent + aTan;
	}

	// --- 3. into pilot body axes, in g ------------------------------------
	const PilotPose& P = POSES[(g_fx.pilotPose < 0 || g_fx.pilotPose >= NPOSE) ? 0 : g_fx.pilotPose];
	const VECTOR3 g = a / G0;
	g_fx.feltGz = (float)dotp(g, P.head);
	g_fx.feltGx = (float)dotp(g, P.chest);
	g_fx.feltGy = (float)dotp(g, P.right);

	// --- 4. physiology ----------------------------------------------------
	const double dt = oapiGetSysStep();      // REAL time (invariant 4): G-LOC must not
	                                         // arrive 100x sooner under time acceleration
	const float  lagK = (float)(1.0 - exp(-dt / CV_LAG_TAU));

	// Onset RATE, measured before smoothing the value. A violent pull outruns the
	// baroreflex, so the compensating blood-pressure rise never arrives - which is how
	// real G-LOC happens with NO grey-out warning at all. Model it as the fast onset
	// stealing back up to 1.5 G of the tolerance the reflex would otherwise have provided.
	const float onsetRaw = (dt > 1e-6) ? (float)((g_fx.feltGz - physGzPrev) / dt) : 0.0f;
	physGzPrev = g_fx.feltGz;
	physOnset += ((onsetRaw > 0.0f ? onsetRaw : 0.0f) - physOnset) * (float)(1.0 - exp(-dt / ONSET_TAU));

	physGzFilt += (g_fx.feltGz - physGzFilt) * lagK;
	physGyFilt += (g_fx.feltGy - physGyFilt) * lagK;

	const float thresh = OroPhys_GzThreshold() - 1.5f * sat01((physOnset - 0.5f) / 2.0f);

	// The cerebral oxygen RESERVE - the whole reason this is a model and not a threshold.
	// Symptoms track the reserve, so a 6 G snap for one second barely dents it while 6 G
	// held for six seconds empties it. Drains in proportion to the excess; refills slower.
	const float excess = physGzFilt - thresh;
	if (excess > 0.0f) g_fx.gReserve -= (float)(dt * excess / RESERVE_SEC);
	else               g_fx.gReserve += (float)(dt * REFILL_RATE);
	g_fx.gReserve = sat01(g_fx.gReserve);

	// G-LOC: once the reserve is gone you are out for a fixed spell whatever the G does
	// next (absolute incapacitation, then the confused relative phase). Coming round
	// fires the involuntary eye-flutter, which the blink envelope already knows how to run.
	if (g_fx.gReserve <= 0.0f && physGlocT < 0.0) physGlocT = 0.0;
	if (physGlocT >= 0.0) {
		physGlocT += dt;
		if (physGlocT >= GLOC_HOLD) { physGlocT = -1.0; g_fx.blinkRequest = true; }
		else                          g_fx.gReserve = 0.0f;
	}

	const float sev = 1.0f - g_fx.gReserve;   // 0 = fine, 1 = out cold

	// --- 5. out to the effects -------------------------------------------
	// +Gz chain, in the order a real pull produces them: the heart pounds first, colour
	// goes, the field starts swimming, phosphenes scatter, scotomas appear, the tunnel
	// closes and finally the lights go out.
	g_fx.heartbeat  = g_fx.gainHeartbeat  * sstep(0.02f, 0.45f, sev);
	g_fx.greyout    = g_fx.gainGreyout    * sstep(0.10f, 0.60f, sev);
	g_fx.swim       = g_fx.gainSwim       * sstep(0.25f, 0.75f, sev);
	g_fx.spots      = g_fx.gainSpots      * sstep(0.35f, 0.80f, sev);
	g_fx.tunnel     = g_fx.gainTunnel     * sstep(0.40f, 0.90f, sev);
	g_fx.blackout   = g_fx.gainBlackout   * sstep(0.75f, 1.00f, sev);
	// Sparkles are a BAND, not a ramp: phosphenes belong to the middle of the descent and
	// are gone by the time the field is black (invariant 2 - blackout suppresses them anyway).
	g_fx.sparkles   = g_fx.gainSparkles   * sstep(0.20f, 0.50f, sev) * (1.0f - sstep(0.70f, 0.95f, sev));

	// -Gz: red-out. No reserve model - retinal congestion is prompt, and it is the
	// direction humans tolerate worst (-2 to -3 G is the practical limit). An anti-G
	// suit does nothing here; it inflates to stop blood LEAVING the head.
	const float neg    = -physGzFilt;
	const float negThr = 1.0f + g_fx.gTolerance * 1.5f;
	g_fx.redout = g_fx.gainRedout * sstep(negThr, negThr + 1.8f, neg);

	// +/-Gx: globe deformation -> defocus. Eyeballs-OUT deforms at lower load than
	// eyeballs-in (which is why you launch on your back). Blur also has a genuine +Gz
	// component (vision softens before colour is fully gone), so take whichever mechanism
	// is producing more. Both thresholds ride the tolerance knob like +Gz does - it is
	// meant to be the whole pilot's robustness, not just their spine. Defaults at the
	// mid setting: 4.0 G eyeballs-out, 7.0 G eyeballs-in.
	const float gxThr  = (g_fx.feltGx < 0.0f) ? (2.5f + g_fx.gTolerance * 3.0f)
	                                          : (5.0f + g_fx.gTolerance * 4.0f);
	const float gxMag  = (float)fabs(g_fx.feltGx);
	const float gxBlur = sstep(gxThr, gxThr + 6.0f, gxMag);
	const float gzBlur = 0.60f * sstep(0.15f, 0.65f, sev);
	g_fx.blur       = g_fx.gainBlur       * (gxBlur > gzBlur ? gxBlur : gzBlur);
	g_fx.aberration = g_fx.gainAberration * sstep(gxThr + 1.5f, gxThr + 8.0f, gxMag);

	// +/-Gy: the head lolls toward the lateral load and the horizon rolls with it. The
	// SIGN is the point (left and right must tip opposite ways), so this drives tiltLean
	// rather than `tilt` - which stays the unipolar woozy SWAY, here driven by how close
	// to G-LOC you are.
	g_fx.tilt     = g_fx.gainTilt * sstep(0.30f, 0.85f, sev);
	g_fx.tiltLean = g_fx.gainTilt * sat01((float)fabs(physGyFilt) / 3.0f) * (physGyFilt < 0.0f ? -1.0f : 1.0f);
}

// Clear everything the model drives (mode changes, and when it stops owning the values).
void OroModule::ZeroDrivenEffects()
{
	g_fx.blackout = g_fx.redout = g_fx.tunnel = g_fx.spots = g_fx.greyout =
	g_fx.blur = g_fx.heartbeat = g_fx.aberration = g_fx.sparkles = g_fx.swim = g_fx.tilt = 0.0f;
	g_fx.tiltLean = 0.0f;
}
