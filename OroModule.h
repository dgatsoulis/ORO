// ==============================================================
// OroModule.h
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

#pragma once

// ============================================================================
// ORO - immersive G-force effects for Orbiter (Orbiter 2024 / D3D9Client)
// ----------------------------------------------------------------------------
// The lifecycle module: a global plugin, activated from Launchpad -> Modules.
//
// STEP 2: registers a D3D9Client render callback (RENDERPROC_HUD_2ND) and paints
// a faint full-screen red wash over the live cockpit frame - proof that we can
// draw on the frame from our module. Toggle with Ctrl+G. Gated to internal
// panel/VC views (not the generic glass cockpit). Still NO physics and NO shader;
// the real effects arrive via the IPI (HLSL) pipeline in a later step.
//
// NOTE [step-2 CTD fix]: the render callback does ZERO oapi calls and only
// harmless Sketchpad batch calls. The view gate is computed on the MAIN thread in
// clbkPreStep and cached; the wash uses ColorFill(.., NULL) which fills the whole
// target with no viewport query. An earlier version called oapiGetViewportSize()
// from inside the callback (mid-frame, inside D3D9Client's own render) and CTD'd -
// it reached back into the graphics client while it was mid-render.
// ============================================================================

// Orbiter's headers rely on the Windows min/max MACROS being present (gcCoreAPI.h's
// FVECTOR4 uses the untyped max(0,r) form, which std::max cannot satisfy). So we
// deliberately do NOT define NOMINMAX anywhere in the ORO include chain.
#include "Orbitersdk.h"   // umbrella: OrbiterAPI.h + ModuleAPI.h (oapi::Module) + DrawAPI.h (Sketchpad)

class gcCore2;        // D3D9Client core interface - full definition in gcCoreAPI.h (included by the .cpp)
class gcIPInterface;  // D3D9Client image-processing (HLSL) interface - ditto
class XRSound;        // Doug Beachy's XRSound 2.0 module sound API - full def in XRSound.h (.cpp)

// Camera-offset jump [m] large enough to mean "something else moved the camera"
// (a pilot/copilot/passenger switch) rather than our own shake delta. Shared by the
// camera shake and the felt-G model, which MUST agree on how to recover the vessel's
// clean offset - see invariant 9 and CleanCameraOffset().
static const double SHAKE_RESET = 0.5;

// The star everything solar measures against (defined in OroEclipse.cpp). Shared by
// the eclipse and the god rays, so "which body is the star" is decided in exactly one
// place. Declared HERE and not in OroState.h: that header is deliberately free of
// Orbiter types and is included ahead of Orbitersdk.h in places, so an OBJHANDLE in it
// does not compile.
OBJHANDLE OroFindStar();

// Invariant 15b's colour machinery, shared (defined in OroReentry.cpp). Every ORO ramp
// that gained a colour pick works the same way: a swatch names a HUE, the ramp is ROTATED
// onto it, and grey/white is the identity so a default look is bit-for-bit unchanged.
// It is exported rather than copied because 15b(c) is a LAW - the angle is never scaled by
// the pick's saturation, because a partial rotation lands on an intermediate hue (pale blue
// came out green) instead of a paler version of the target. A second copy of that reasoning
// is a second place for it to drift.
//   OroHueRotFromPick : swatch + the family's own reference hue -> degrees to rotate.
//   OroHueRotate      : apply it to one linear rgb triple, preserving value and saturation.
//                       Values ABOVE 1.0 are preserved (the bell rides an emissive overdrive
//                       past the fp16 bloom threshold), so this normalises and re-scales.
float OroHueRotFromPick(DWORD col, float refHue);
void  OroHueRotate(float& r, float& g, float& b, float deg);

// WHICH THRUSTER GROUP A THRUSTER BELONGS TO (defined in OroPlume.cpp), shared by the
// plume, the shimmer, the particles and the bell so all four can never disagree about
// what "HOVER" means. Returns ORO_THR_MAIN/HOVER/RETRO/USER, or -1 for anything ORO
// does not touch - which is exactly the RCS: every THGROUP_ATT_* thruster classifies
// as "not ours" and is skipped everywhere.
// "USER" is any thruster in NO standard group at all. That is the definition the bell
// glow has used since 2026-08-09 (BellUserLevel), adopted wholesale rather than
// invented a second time.
int  OroThrusterGroupOf(VESSEL* v, THRUSTER_HANDLE th);
bool OroThrusterHasUser(VESSEL* v);

// ORO's lifecycle module. Derives from oapi::Module for the per-frame main-thread
// hook (clbkPreStep), the session start/end events, and keyboard input (the toggle).
class OroModule : public oapi::Module {
public:
	explicit OroModule(HINSTANCE hDLL);
	~OroModule();

	// Session begins: probe D3D9Client and register our render callback (once).
	void clbkSimulationStart(RenderMode mode) override;

	// Session ends (exit to Launchpad).
	void clbkSimulationEnd() override;

	// Per-frame, MAIN thread. STEP 2: computes the view gate (drawTint) here, where
	// oapi camera/cockpit queries are unquestionably safe. Later: the felt-G model.
	void clbkPreStep(double simt, double simdt, double mjd) override;

	// Per-frame, MAIN thread, AFTER the state update - the TRAIL's projection hook
	// (round 3, 2026-08-08). The trail is ORO's first CLOSE-RANGE WORLD-ANCHORED
	// geometry, and that class is uniquely exposed to a one-frame camera epoch
	// mismatch: clbkPreStep reads the camera BEFORE the step, the frame is rendered
	// with the camera AFTER it, and at entry speed that is ~120 m of camera travel -
	// enormous parallax on a particle a few hundred metres away, and exactly the
	// every-other-frame origin jumping the user reported (in BOTH trail
	// implementations - the knot ring had the same anchoring). Vessel-anchored
	// geometry (all the plasma) never sees this: a tracking camera keeps the vessel
	// fixed on screen, so the epoch cancels; the aurora is world-anchored but
	// hundreds of km away, where one frame of parallax is sub-pixel. Projecting in
	// POST-step reads the camera the renderer will actually use (UpdateTrailPost
	// logs a one-shot verdict on whether the camera has really advanced by then).
	void clbkPostStep(double simt, double simdt, double mjd) override;

	// Buffered keyboard: Ctrl+G toggles the master arm (the panic/quick kill).
	bool clbkProcessKeyboardBuffered(DWORD key, char kstate[256], bool simRunning) override;

	// A vessel is about to be destroyed. MANDATORY for the reentry plasma: we hold a
	// LightEmitter* belonging to that vessel, and the handle stops being valid the moment
	// this returns. Drop the light here or the next frame touches a dead vessel.
	void clbkDeleteVessel(OBJHANDLE hVessel) override;

	// Draw pass, invoked from the file-scope __gcRenderProc thunk (RENDERPROC_HUD_2ND)
	// with a Sketchpad bound to the backbuffer. Public so the thunk can forward to it
	// (mirrors the DrawOrbits sample's clbkRender). Does NO oapi calls - it only reads
	// the cached gate and issues harmless Sketchpad batch calls.
	void DrawOverlay(oapi::Sketchpad* pSkp);

	// Pre-resolve draw pass (client patch (i), RENDERPROC_PRE_RESOLVE): fires after the
	// COMPLETE scene (terrain, vessels, transparency, VC) but before the light-blur
	// resolve/tonemap and the HUD. With PostProcess=1 ("Light glow") the Sketchpad is
	// bound to the client's fp16 offscreen scene target, so additive art drawn here
	// ACCUMULATES past 1.0 and participates in the client's own threshold bloom +
	// tonemap - the compositing point the reentry plasma always needed (Firefly rework
	// step 1, 2026-08-08). Same no-oapi-calls contract as DrawOverlay.
	void DrawPreResolve(oapi::Sketchpad* pSkp);

	// Target of the GENERICPROC_SHUTDOWN thunk, so PUBLIC like the two render-proc
	// targets above. Definition and rationale live beside the private members it
	// releases - see the long comment at the bottom of this class.
	void ReleaseDeviceResources();

	// Target of the GENERICPROC_SHUTDOWN thunk, so PUBLIC as well. Hands back the
	// borrows the SCENE owns, at the top of clbkCloseSession - the only point at which
	// they are still alive. clbkSimulationEnd runs ~274 ms after the scene is deleted,
	// which made this a use-after-free in Orbiter's heap; see the registration site.
	void ReleaseSceneOwnedBorrows(bool fromShutdownProc);

private:
	gcCore2* pCore = nullptr;               // D3D9Client core; NULL if D3D9Client isn't the active client
	bool     renderProcRegistered = false;  // one-shot: the RenderProcs list survives clbkCloseSession, so register once
	bool     preResolveRegistered = false;  // one-shot, same rule, for the patch-(i) pre-resolve slot
	bool     shutdownProcRegistered = false;// one-shot, same rule (GenericProcs is a plain member vector,
	                                        // never cleared by clbkCloseSession) - see
	                                        // ReleaseSceneOwnedBorrows and its registration note
	// Latched TRUE by DrawOverlay, cleared at every session start. "The scene has
	// rendered at least one frame this session", and it is a SAFETY GATE, not a
	// diagnostic: on a scenario reload Scene::RenderMainScene returns early - before the
	// HUD stages our render proc lives in - for as long as the focus vessel has no
	// visual, which measured over a second in the user's log. Anything that hands the
	// core or the client a long-lived object must wait for this; see UpdateParticles.
	//
	// ⚠️ 2026-08-12: THERE ARE THREE CONSUMERS, NOT ONE. When 23(k) was written it was
	// applied only to UpdateParticles, because that is where the bisection pointed - but
	// the reentry hull LIGHT (Vessel::AddPointLight) and the bell glow's MESH
	// (Vessel::AddMesh) hand a vessel a long-lived object in exactly the same way, from
	// exactly the same clbkPreStep load window, and both were left unguarded. The reload
	// CTD came back on a scenario that reaches one of them. Fixing the case the evidence
	// points at is not the same as applying the rule; when a law like this is written,
	// sweep every site that breaks it.
	bool     sceneRendered = false;
	bool     lendDeferLogged = false;       // one line per session, naming the FIRST lend that had
	                                        // to wait - so if a reload ever crashes here again the
	                                        // log says which borrow was in flight instead of
	                                        // leaving it to be re-derived from a stack walk
	bool     preResolveLive = false;        // latched TRUE the first time the pre-resolve proc actually FIRES.
	                                        // Pre-(i) patched clients accept the registration but never call it,
	                                        // so DrawOverlay keeps drawing the plasma in the old HUD_2ND slot
	                                        // until this proves the new slot exists. Never reset: the client's
	                                        // RenderProcs list (and the client itself) outlives our sessions.
	bool     focusPrimed = false;           // one-shot: give the render window keyboard focus on the first frame,
	                                        // so keys don't beep at scenario start (a focus quirk, not an ORO effect)
	bool     viewGate = false;              // cached per-frame gate (internal panel/VC view), set in clbkPreStep
	bool     extGate = false;               // cached per-frame EXTERNAL-view gate - the shimmer's domain (see below)
	DWORD    viewW = 0, viewH = 0;          // viewport size, cached in clbkPreStep (policy: no oapi calls in the render path)

	// Tunnel-vision geometry: a gc triangle-strip ring (per-vertex alpha feather),
	// created once and updated in place only when the slider or viewport changes.
	// HPOLY comes from DrawAPI.h. Owned per session: deleted at simulation end
	// (the poly lives on the client's device resources).
	HPOLY    hTunnelPoly = NULL;
	float    lastTunnel = -1.0f;            // geometry cache keys
	DWORD    lastViewW = 0, lastViewH = 0;

	// Dark-spot geometry: all spots in one triangle-list HPOLY, CONSTANT vertex
	// count (inactive spots get alpha 0 - CreateTriangles updates must never
	// exceed the creation count). Updated per frame while active: the shimmer
	// animates. Session-owned like the tunnel poly.
	HPOLY    hSpotsPoly = NULL;

	// Heartbeat vignette: a soft peripheral darkening ring (feather + solid band),
	// rebuilt per frame while active because its alpha pulses. Session-owned poly.
	HPOLY    hHeartPoly = NULL;

	// Sparkle / phosphene points: all in one triangle-list HPOLY, CONSTANT vertex
	// count (inactive sparkles collapse to alpha 0), rebuilt per frame (scintillation).
	HPOLY    hSparkPoly = NULL;

	// Animation clocks (REAL-time, oapiGetSysStep - a blink must stay a 0.3 s
	// blink at 100x warp). animT feeds the spot shimmer; blinkT runs the eyelid
	// envelope, <0 = idle.
	float    animT  = 0.0f;
	double   blinkT = -1.0;

	// Heartbeat phase accumulator (0..1 within a beat, REAL time) + its current beat
	// envelope (0..1, peaks at systole). A phase accumulator (not a raw clock mod a
	// period) lets the RATE vary smoothly - the slider drives bpm - without stutter.
	double   heartPhase = 0.0;
	float    heartEnv   = 0.0f;
	double   heartPhasePrev = 0.0;   // heartPhase last frame - detects the per-beat crossing that fires the SOUND

	// INDUCE scenario player: elapsed time (REAL) into the active scripted sequence.
	double   seqT = 0.0;
	bool     seqSoundWasOn = true;   // prev g_fx.seqSoundEnabled - to catch a mid-scenario mute
	bool     seqSoundPlaying = false;// a scenario clip is loaded into the mixer (playing or PAUSED).
	                                 // The mute is a PAUSE, so un-muting can resume in place rather
	                                 // than restart a narration out of step with what it describes;
	                                 // this says whether there is anything to resume, or whether the
	                                 // scenario started silent and the clip has to be seeked instead.

	// Camera shake (felt-G): we perturb the focus vessel's camera offset. Track the
	// delta we added and the total we last wrote, so we can recover the vessel's clean
	// base offset each frame - and ADOPT a new base when it jumps (pilot/copilot switch)
	// instead of fighting it. camActive = we currently hold a perturbation.
	bool     camActive  = false;
	VECTOR3  camDelta    = { 0, 0, 0 };   // the offset we added last frame
	VECTOR3  camApplied  = { 0, 0, 0 };   // base + camDelta, what we last SetCameraOffset'd

	// Physics-driven camera shake: seat-push (felt-G) + buffet jitter, applied on the
	// MAIN thread (clbkPreStep). ReleaseCameraShake restores the vessel's clean offset.
	void UpdateCameraShake();
	void ReleaseCameraShake();
	// FLIGHT AID: a live CoP shift on the FOCUS vessel, applied as a pure force couple.
	// MAIN thread (clbkPreStep) - AddForce is a per-timestep force, so there is nothing
	// to hand back: stop calling it and the vessel is stock again on the next step.
	void UpdateCopShift();

	// CANCEL THRUST - the test-stand rig (session-only pill on the THRUSTER tab):
	// AddForce of the vessel's own total thrust, negated, at the CoM, every step.
	// Same mechanism as the CoP shift - per-timestep, so there is nothing to hand
	// back and disarm releases it on the next step. Main thread (clbkPreStep).
	void UpdateCancelThrust();

	// --- FELT-G PHYSICS (OroPhysics.cpp) ---------------------------------
	// The model that turns the lab into a simulation: real motion -> proper acceleration
	// at the PILOT'S HEAD -> pilot body axes -> physiology -> the effect values, with the
	// dialog sliders demoted to per-effect gains. MAIN THREAD (clbkPreStep) - it makes
	// VESSEL calls and must never be reached from the render path (invariant 1).
	void UpdatePhysics();
	void ZeroDrivenEffects();          // clear everything the model drives (mode changes)

	// The focus vessel's camera offset with our own shake delta removed. The physics runs
	// BEFORE UpdateCameraShake each frame, so a raw GetCameraOffset still carries last
	// frame's shake - feeding that back in would close a shake -> G -> effects -> shake
	// loop. Uses the same jump test as the shake so the two always agree (invariant 9).
	VECTOR3 CleanCameraOffset(VESSEL* v) const;

	// --- REENTRY PLASMA (OroReentry.cpp) ---------------------------------
	// ORO's first PER-VESSEL state: all vessels are tracked, so you can watch another
	// ship come down. Needs NO client patch (core Orbiter API, not gcCore) and no render
	// callback, and the emitter is VIS_ALWAYS so one source lights the hull AND the
	// cockpit - the first effect that lives in both domains (cf. invariant 10).
	//
	// !! SLOTS ARE NEVER MOVED OR COMPACTED !! LightEmitter::SetPositionRef/SetIntensityRef
	// bind the light to `pos` and `intensity` BELOW and the API requires those addresses to
	// stay valid for the light's lifetime. A std::vector here would dangle every live light
	// the moment it reallocated. hV == NULL marks a free slot, reused in place.
	static const int MAX_RENTRY = 32;
	static const int N_EMIT     = 12;        // icosahedron directions - see OroReentry.cpp
	static const int MAX_BLOB   = 8;         // wake blobs per vessel (see WakeBlob below)

	// Wake blob - a self-managed "mega-particle". Orbiter's stream particles are round,
	// unstretchable, single-texture sprites (which is why the fan reads like stock dust);
	// these are OURS: world-anchored luminous puffs the vessel visibly runs away from.
	// gvel is the velocity of the AIR at shed time (ship velocity minus airspeed), so a
	// blob rides the wind while the ship departs at kilometres per second. age < 0 = free.
	struct WakeBlob {
		VECTOR3 gpos  = { 0,0,0 };  // global position
		VECTOR3 gvel  = { 0,0,0 };  // global velocity (the wind's)
		float   age   = -1.0f;      // seconds since shed; < 0 = free slot
		float   size0 = 0.0f;       // radius at shed [m]; grows with age
		float   seed  = 0.0f;       // per-blob hash for flicker/offsets
	};

	// (TrailPt - the persistent knot ring - was REMOVED on 2026-08-02 with the whole
	//  trail. It was the only ORO state that accumulated across seconds, and that
	//  is precisely what made it fragile: everything else is recomputed each frame,
	//  so a bad sample vanishes, while a bad knot survived as a permanent spike.
	//  Long streaks carry the downstream story instead.)

	// Hull point field: a read-only SAMPLE of the vessel's own mesh (positions +
	// normals, vessel-local), taken once per slot. This is what makes the plasma
	// CONFORM to any hull with no client patch: we never render the mesh, we READ
	// it - streak roots sit on the actual skin, the glow coat covers the actual
	// windward surfaces, and dot(normal, flow) picks the hot side per point.
	static const int MAX_HULLPT = 480;       // 160 -> 480 (round 3.6.2): the shock
	                                         // sheet fuses from overlapping per-point
	                                         // patches, and 160 samples left it blotchy
	struct HullPt { VECTOR3 pos, nrm; };

	// SHOCK SHELL (round 4, the ultracode design study): a welded, DECIMATED COPY
	// of the vessel's OWN mesh triangles - the templates expose full index lists
	// (MESHGROUP.Idx), so the windward shape is READ, not guessed. Built once per
	// vessel (BuildShell, the SampleHull lazy pattern); per frame the windward
	// subset is flow-shifted by the Shock dist knob, projected per vertex and
	// drawn as connected Gouraud triangles. Coverage comes from author topology:
	// blobs (splat era) and shards (heightfield era) are impossible by construction.
	static const int SHELL_MAX_VTX = 2048;   // 4.1 doubled these to 4096/8192 on the
	static const int SHELL_MAX_TRI = 3072;   // assumption that more triangles = more
	                                         // detail = better. WRONG, and round 5.2
	                                         // rolled it back: a shell fine enough to
	                                         // resolve the DG's LANDING GEAR draws the
	                                         // landing gear, glowing, hanging under the
	                                         // belly (user, 2026-08-02). The shell is a
	                                         // SHOCK ENVELOPE - it wants the hull's
	                                         // gross shape and nothing smaller. Detail
	                                         // is not free here, it is the artifact.
	enum { SH_OPENEDGE = 1 };                // shellFlg bit: vertex on a true mesh boundary

	struct ReentryVessel {
		OBJHANDLE      hV        = NULL;      // NULL = free slot
		LightEmitter*  light     = nullptr;   // owned by the VESSEL, not by us
		double         intensity = 0.0;       // bound via SetIntensityRef - DO NOT MOVE
		VECTOR3        pos       = { 0,0,0 }; // bound via SetPositionRef  - DO NOT MOVE
		float          heat      = 0.0f;      // 0..1 Sutton-Graves stagnation heating
		int            band      = -1;        // colour band the current light was built for
		// Windward particle fan - the filament streaks. lvl[] is bound by AddParticleStream
		// and read by Orbiter every frame, so it is under the SAME no-move rule as pos and
		// intensity above, and the streams MUST be deleted before this slot dies.
		PARTICLESTREAMSPEC pspec = {};        // per-vessel (sizes scale with GetSize())
		PSTREAM_HANDLE strm[N_EMIT] = {};     // NULL if particles are off in the Launchpad
		double         lvl[N_EMIT]  = {};     // 0..1 emission level - DO NOT MOVE
		bool           streamsMade  = false;
		bool           stockOff     = false;  // did we suppress stock's billboards on it?
		WakeBlob       blob[MAX_BLOB];        // advected every frame; spawned camera-target only
		double         blobT        = 0.0;    // countdown to the next blob spawn [s, SIM time]
		HullPt         hull[MAX_HULLPT];      // skin sample, vessel-local (see above)
		int            nHull        = 0;
		bool           hullSampled  = false;  // lazy: sampled when first camera target
		// The shock shell (round 4; ~68 KB/slot, static BSS). Vessel-local, offset
		// baked per mesh; built by BuildShell when heat first crosses 0.005.
		float          shellPos[SHELL_MAX_VTX * 3];  // welded cluster positions
		float          shellNrm[SHELL_MAX_VTX * 3];  // area-weighted unit normals
		BYTE           shellFlg[SHELL_MAX_VTX];      // SH_OPENEDGE etc.
		WORD           shellIdx[SHELL_MAX_TRI * 3];  // triangle list into the above
		int            nShellV      = 0;
		int            nShellT      = 0;
		bool           shellBuilt   = false;
		// TRAIL spawn cursor (2026-08-08, the particle trail). Plain data, nothing bound -
		// the no-move rule above does not extend to these. The cursor is the ONLY per-vessel
		// trail state: particles themselves live in the GLOBAL pool below and carry no vessel
		// reference, which is what lets a trail outlive its vessel (breakup addons DELETE the
		// burning vessel and spawn a debris field - the parent's train must keep glowing).
		VECTOR3        trailLast    = { 0,0,0 }; // PLANET-RELATIVE pos at the last spawn pass
		                                         // (round 7: the cursor lives in the same
		                                         // frame as the particles, so the distance
		                                         // cadence measures the ENTRY path, not the
		                                         // 30 km/s barycentric one - which was
		                                         // shedding 4x too fast and draining the ring)
		OBJHANDLE      trailRef     = NULL;      // body trailLast is relative to; a change
		                                         // (SOI handover) re-primes the cursor
		DWORD          trailSeq     = 0;         // per-vessel shed ordinal (feeds TrailPt::seq
		                                         // - per VESSEL, so a debris field's
		                                         // interleaved sheds don't break the stride)
		DWORD          trailChain   = 0;         // this vessel's CURRENT ribbon chain id -
		                                         // a fresh id on every cursor prime, so a
		                                         // teleport/SOI/cooling gap starts a NEW
		                                         // ribbon instead of bridging the break
		double         trailAcc     = 0.0;       // metres travelled toward the next shed
		double         trailS       = 0.0;       // CUMULATIVE path metres - the meander's
		                                         // phase coordinate (round 2: shed points
		                                         // are a smooth function of path distance,
		                                         // never a fresh random roll - see SpawnTrail)
		bool           trailSeen    = false;     // trailLast valid? (false after reset/jump)
	};
	ReentryVessel rentry[MAX_RENTRY];
	double     reentryScanT      = 0.0;      // seconds since the last full vessel scan
	bool       reentryFullWarned = false;    // one-shot log if the table fills
	bool       reentryNoParticles = false;   // one-shot log: Launchpad has particles disabled
	bool       reentrySuppressChecked = false;
	bool       reentryCanSuppress = false;   // client patch (c) present? see OroReentry.cpp

	// Cockpit plasma glow: a SCREEN-SPACE effect, not a light. A point light cannot work
	// here - Orbiter's local lights have no occlusion, so a stagnation-point light at 40 deg
	// AoA shines up through the floor (tested 2026-08-01, rejected on sight). Computed on
	// the main thread and cached for the render callback (invariant 1).
	float plasmaGlow   = 0.0f;               // 0..1 intensity for the focus vessel's cockpit
	float plasmaUV[2]  = { 0.5f, 0.5f };     // where the plasma sits in screen UV
	float plasmaCol[3] = { 1.0f, 0.5f, 0.2f };

	void UpdateReentry();                    // per frame, main thread
	void ReleaseReentry();                   // hand every borrowed light back
	void ReentryForget(OBJHANDLE h);         // clbkDeleteVessel
	int  ReentryFindSlot(OBJHANDLE h) const; // NULL finds the first FREE slot
	void ReentryFreeSlot(int i, bool live);  // live = the vessel is still safe to call
	void ReentryMakeLight(int i, VESSEL* v, int band);
	void ReentryMakeStreams(int i, VESSEL* v);   // the windward particle fan
	void ReentryKillStreams(int i, VESSEL* v);   // MUST run before a slot dies (bound lvl[])

	// --- PLASMA GEOMETRY (round 2: the custom-drawn effect) ----------------
	// The KSP-reference look decomposed: a windward shock ENVELOPE, sharp FILAMENT
	// ribbons shearing downstream, and detached WAKE blobs - all as one screen-space
	// triangle list. PROJECTED on the main thread in UpdateReentry (the shimmer
	// pattern: invariant 1 bans oapi calls in the render path); the render callback's
	// external branch pushes the vertices into hPlasmaPoly and draws them ADDITIVELY
	// (client patch d) - plasma is LIGHT, it must add to the frame, not paint over it.
	// Orbiter's particle system was tried first (round 1) and plateaued at stock's
	// look for structural reasons: round camera-facing sprites, one texture, no
	// velocity stretching, no colour ramp. This replaces it as the main visual; the
	// fan survives dialled way down as an under-layer of sparks.
	// ⚠️ PLAS_MAX_TRI * 3 MUST STAY UNDER 65536 - invariant 19(d), the ceiling that CTD'd
	// the aurora on 2026-08-07. 16384 * 3 = 49152 vertices, which is under it with room;
	// the next doubling would NOT be, and would need splitting across several HPOLYs.
	static const int PLAS_MAX_TRI = 16384;   // 3072 -> 4096 (trail) -> 5120 (sheet) ->
	                                         // 7168 (dense hull field) -> 16384 (4.1's
	                                         // fidelity raise) -> 12288 (5.2's shell
	                                         // rollback) -> 8192 (the trail's 4096 handed
	                                         // back, 2026-08-02) -> 16384 (the shell became a
	                                         // 3-LAYER VOLUME, 2026-08-07). Headroom: shell
	                                         // 3072 x3 + streams ~2.9k + glows ~1.1k + sparks.
	                                         // The pool is not free - the FULL buffer is
	                                         // pushed every frame (invariant 3), so its
	                                         // size is a per-frame memcpy, not a cap.
	bool     vcGate = false;                 // round 3.5: draw the plasma in the VIRTUAL
	                                         // COCKPIT (reentryVC toggle && COCKPIT_VIRTUAL);
	                                         // computed in clbkPreStep like the other gates

	// The poly update + draw, shared by the EXTERNAL branch and the VC path (3.5).
	// Defined in OroModule.cpp - this header only forward-declares the gc types.
	// depthClip asks for the patch-(g) per-pixel clip; see the call sites for why the
	// VC passes true and the external branch does not.
	void DrawPlasmaPoly(oapi::Sketchpad* pSkp, bool depthClip);
	struct PlasVtx { float x, y; DWORD c; }; // layout == gcCore::clrVtx (asserted in .cpp)
	PlasVtx  plasVtx[PLAS_MAX_TRI * 3];
	// Per-vertex EUCLIDEAN distance from the camera, parallel to plasVtx (patch g). It is
	// length(P - campos), NOT the camera-space z the projector returns: the client's
	// GBUF_DEPTH.a is length(frg.posW) (NewMesh.hlsl, NormalDepth_PS), and mismatching the
	// two produces a clip that is subtly wrong at the frame edges rather than obviously
	// broken. Always filled, whether or not the clip is on - it costs one length() per
	// projected point and keeps the buffer honest if a branch flips.
	float    plasDepth[PLAS_MAX_TRI * 3];
	int      plasVtxN = 0;                   // vertices filled this frame (multiple of 3)
	HPOLY    hPlasmaPoly = NULL;             // device resource - released with the others
	bool     padAdditive = false;            // client carries patch (d) (gcAPIVer >= 260801)
	// The client's PostProcess (Light glow) setting, read once from D3D9Client.cfg at
	// session start - see the block in clbkSimulationStart and OroBloomOn() in OroState.h.
	// bloomKnown separates "we read a 0" from "we could not read the file at all".
	bool     bloomOn    = true;
	bool     bloomKnown = false;

	// (the SMOKE pool and the trail's gather/miter scratch lived here. Both went
	//  with the trail on 2026-08-02. The smoke was ORO's only alpha-blended layer;
	//  if a use for occluding geometry ever returns, the recipe was simply a second
	//  poly drawn with the pad's default blend state BEFORE the additive one - no
	//  client patch involved.)

	// --- THE PLASMA TRAIL, take 2 (2026-08-08) - the PARTICLE POOL ---------
	// G10 convicted CONNECTIVITY THROUGH ACCUMULATION, not accumulation itself: a bad
	// knot in a connected ribbon was a screen-crossing spike forever, because mitres,
	// cadence and despiking all read NEIGHBOUR PAIRS. This is the other architecture -
	// Firefly's own (their long trail is their smoke particle system, not shader
	// geometry): independent world-anchored particles, each with a bounded SIM-time
	// lifetime, each drawn ALONE as one soft stretched sprite. Expiry makes stale
	// state age out by construction; independence bounds any error to one size-capped
	// sprite for one lifetime. THE ONE LAW THIS ADDS: particles are NEVER joined - no
	// polyline, no mitre, no neighbour lookup, ever. The column look comes from
	// overlap + fp16 bloom, exactly like every other particle system.
	//
	// ONE GLOBAL POOL, all vessels shed into it (full-scene by design - the breakup
	// case). Ring allocation = oldest-first eviction: when a debris field floods the
	// scene every trail shortens together from its far tail, with no per-vessel quota
	// logic and no way for pressure to produce a stale artifact. Per-vessel cost is
	// only the spawn cursor in ReentryVessel; invariant 15's camera-target rule stays
	// for the ATTACHED plasma (fins/envelope) - distant vessels carry light + trail.
	// ⚠️ TRAIL_MAX * 8 tri * 3 vtx MUST STAY UNDER 65536 - invariant 19(d).
	static const int TRAIL_MAX     = 2048;   // particles. 8 tris each since round 8: a
	                                         // CAPSULE - flat-bright core between two
	                                         // stations, feathered caps and edges (every
	                                         // BORDER vertex alpha 0, the 20(b) law).
	                                         // The round-1 rhombus peaked at its centre
	                                         // and a chain of triangle profiles SUMS WITH
	                                         // RIPPLE - the column read as beads and bloom
	                                         // amplified exactly the peaks. Flat tops
	                                         // overlap ripple-free by construction. Pool
	                                         // halved to pay for it: still above the
	                                         // one-vessel fair share (TRAIL_SHARE 1600);
	                                         // a breakup shares the ring oldest-first.
	static const int TRAIL_MAX_TRI = TRAIL_MAX * 8;      // 16384 tri = 49152 verts: the
	                                         // largest safe pool without splitting HPOLYs
	struct TrailPt {
		VECTOR3 rpos;                        // position RELATIVE TO hRef's centre, in the
		                                     //   NON-ROTATING (inertial) frame - round 7's
		                                     //   law, and the ⚠️ FRAME LESSON it encodes:
		                                     //   Orbiter's "global" frame is SOLAR-SYSTEM
		                                     //   BARYCENTRIC. A particle held at a fixed
		                                     //   global position is NOT at rest over a
		                                     //   planet - the planet leaves at ~30 km/s of
		                                     //   orbital velocity (Earth: 29.8 km/s; the
		                                     //   diag7 run measured 29,906 m/s and a trail
		                                     //   recording the path around the SUN). The old
		                                     //   gvel = shipVel - airspeedVector was 98%
		                                     //   frame-keeping and 2% actual wind; round 6
		                                     //   zeroed both and the trail swung off the
		                                     //   entry path. Planet-relative storage keeps
		                                     //   the frame BY CONSTRUCTION - no velocity, no
		                                     //   integration - and drops the true-wind bend
		                                     //   (the 0.4 km/s rotating-atmosphere term)
		                                     //   that displaced round 5's train. Global
		                                     //   position is reconstructed at projection:
		                                     //   rpos + hRef's CURRENT position. The phase-2
		                                     //   smoke column re-adds wind by advecting rpos.
		OBJHANDLE hRef;                      // the body rpos is anchored to (GetSurfaceRef at
		                                     //   shed - the owner of the air; planets never
		                                     //   die, so the handle cannot dangle)
		VECTOR3 gdir;                        // shed-time flight direction (unit, global) -
		                                     //   the STRETCH AXIS. Per-particle constant:
		                                     //   the column bends because successive
		                                     //   particles shed with rotated axes, never
		                                     //   because anything reads a neighbour
		float   age     = -1.0f;             // [s] SIM time; < 0 = free slot
		float   life    = 0.0f;              // [s] this particle's own lifetime
		float   size0   = 0.0f;              // lateral half-width at shed [m]
		float   halfLen = 0.0f;              // half-length along gdir [m] (covers the shed
		                                     //   spacing so neighbours fuse by overlap)
		float   heat0   = 0.0f;              // heat at shed: colour/alpha ramp, and the
		                                     //   SOOT amount when the phase-2 smoke layer
		                                     //   lands (dark ablation column - G11 recipe)
		float   seed    = 0.0f;              // per-particle hash (drives the traveling
		                                     //   brightness lumps - a lump tied to a
		                                     //   particle is FIXED IN THE AIR, so it
		                                     //   marches down the ribbon as the vessel
		                                     //   leaves it behind: free, honest motion)
		DWORD   seq     = 0;                 // per-VESSEL shed ordinal (reserved; the
		                                     //   sprite-era LOD used it as stride key)
		DWORD   chain   = 0;                 // RIBBON CHAIN id (phase A): which vessel's
		                                     //   trail this knot belongs to. Assigned at
		                                     //   cursor prime, survives the vessel's
		                                     //   death - an orphan chain keeps drawing
		                                     //   until its knots expire (the breakup case)
	};
	TrailPt  trail[TRAIL_MAX];
	int      trailHead = 0;                  // ring cursor: next slot to (over)write
	PlasVtx  trailVtx[TRAIL_MAX_TRI * 3];
	float    trailDepth[TRAIL_MAX_TRI * 3]; // per-vertex Euclidean camera distance (patch g)
	int      trailVtxN = 0;
	HPOLY    hTrailPoly = NULL;              // device resource - released with the others
	void     SpawnTrail(int i, VESSEL* v, const VECTOR3& flowLocal);
	void     UpdateTrailPost(double simdt);  // clbkPostStep driver: gate + advect + spawn +
	                                         // gather the oapi-side emit parameters
	void     UpdateTrail(double simdt);      // age + advect + expire ONLY (round 4 moved the
	                                         // projection out - see ProjectTrail)
	void     ProjectTrail();                 // RENDER-PATH projection (round 4, patch k): the
	                                         // pool -> trailVtx with the camera the frame is
	                                         // ACTUALLY rendered with (gcCore::GetRenderCam).
	                                         // Post-step proved one step stale too - the
	                                         // round-3 diagnostic caught it in one flight.
	                                         // No oapi calls (gc calls only - the
	                                         // CopyResource precedent); falls back to the
	                                         // post-step camera on an unpatched client.
	void     EmitTrailChains();              // PHASE A (the ribbon): walk the pool per
	                                         // chain, decimate knots in SCREEN SPACE,
	                                         // thread ONE mitred feathered strip through
	                                         // them - rebuilt from scratch every frame
	                                         // (only the POSITIONS persist, and they
	                                         // expire; G10's purpose honoured, its letter
	                                         // amended now the samples are diag-proven).
	                                         // A ribbon deposits its alpha ONCE per
	                                         // pixel: no stacking, no sum to normalize -
	                                         // the whole rounds-8-13 brightness war is
	                                         // structurally impossible here.
	void     DrawTrailPoly(oapi::Sketchpad* pSkp, bool depthClip);

	void SampleHull(int i, VESSEL* v);       // read the vessel's mesh into the point field
	void BuildShell(int i, VESSEL* v);       // round 4: weld+decimate the mesh's own
	                                         // triangles into the shock shell (lazy, once)
	// Shock-shell per-frame scratch - ONE set, shared by all slots (single sim
	// thread by construction; only the camera-target vessel builds geometry).
	float shellWd[SHELL_MAX_VTX];            // windwardness per shell vertex
	float shellSx[SHELL_MAX_VTX], shellSy[SHELL_MAX_VTX];   // projected position
	float shellSz[SHELL_MAX_VTX];            // ... and its EUCLIDEAN camera distance, for the
	                                         // patch-(g) clip. Taken at the OFFSET position -
	                                         // the standoff surface is what actually draws.
	DWORD shellCol[SHELL_MAX_VTX];           // shaded colour cache (once per vertex)
	BYTE  shellNeed[SHELL_MAX_VTX], shellOk[SHELL_MAX_VTX];

	// THE HULL DEPTH MAP (round 5.1) - ORO's own coarse Z-buffer, and the only
	// thing that can tell a shell surface at the SILHOUETTE (visible, must stay -
	// graveyard G5) from one in the MIDDLE of the hull (hidden, must go). Both are
	// edge-on, so they carry the same normal and no facing term can separate them;
	// only depth can. Built per frame from the shell's UN-OFFSET triangles - the real
	// skin, leeward side included, so the top hull occludes the belly sheet.
	static const int HZ_N = 64;              // grid is per-VESSEL screen box, not per
	                                         // screen: 64 cells always span the hull
	float hullPx[SHELL_MAX_VTX], hullPy[SHELL_MAX_VTX];     // skin, projected (no standoff)
	float hullPz[SHELL_MAX_VTX];             // ... and its camera depth
	BYTE  hullOk[SHELL_MAX_VTX];
	float hullZ[HZ_N * HZ_N];                // nearest skin depth per cell (1e30 = no hull)
	void SpawnWakeBlobs(int i, VESSEL* v, const VECTOR3& flowLocal, double simdt);
	void BuildPlasmaGeometry();             // RENDER PATH since 2026-08-15: the whole draw
	                                        //   list off the render camera. Took a VESSEL*
	                                        //   for exactly four values, which UpdateReentry
	                                        //   now snapshots - see ProjCam in this header.

	bool   physWasOn   = false;        // previous g_fx.physicsMode - detects the mode edges
	float  physGzFilt  = 0.0f;         // +Gz after the cardiovascular lag - what the eye follows
	float  physGyFilt  = 0.0f;         // lateral G, smoothed, for the steady head lean
	float  physGzPrev  = 0.0f;         // last frame's raw +Gz, for the onset RATE
	float  physOnset   = 0.0f;         // smoothed +Gz onset rate [G/s] - fast pulls outrun the reflex
	double physGlocT   = -1.0;         // G-LOC incapacitation clock [s], <0 = conscious

	// --- ECLIPSE (OroEclipse.cpp) ----------------------------------------
	// Solar-disc obscuration at the CAMERA by any celestial body, driving a model
	// of the eye behind it (adaptation lag in, glare out, colour loss while dark).
	// Main thread; the render path reads only the three published numbers below.
	// The effect spans BOTH view domains - it is the light in the world, and the
	// world is there whether you are looking at it from a seat or from outside -
	// so unlike every other effect it draws in the external branch AND the
	// internal stack (cf. invariant 10, whose "decide the domain first" rule this
	// answers with "both", not by accident).
	// --- VC SHADOWS (client patch f) ---------------------------------------
	// ORO draws nothing; it just hands the patched client the two knobs its
	// internal-pass shadow map exposes. Pushed only on CHANGE - it is a client
	// state setter, not a per-frame parameter, and there is no reason to cross
	// the DLL boundary 60 times a second to say the same thing.
	bool  vcShadowSupported = false;   // client carries patch (f)?
	bool  vcShadowLastOn    = true;    // last values pushed, so we only push on change
	float vcShadowLastRad   = -1.0f;
	float vcShadowLastDep   = -1.0f;   // patch (p): the ambient bite
	void  UpdateVCShadows();

	void  UpdateEclipse();
	void  DrawEclipsePass();    // the IPI resample; self-gating, called from both branches
	float eclAdapt   = 1.0f;    // what the eye is currently set up for, 0..1
	bool  eclPrimed  = false;   // has eclAdapt been snapped to reality yet this session?
	double eclTestT  = 0.0;     // TEST cycle clock [s, REAL]
	float eclDesat   = 0.0f;    // scotopic colour loss handed to the shader, 0..1
	bool  eclActive  = false;   // is there anything to draw this frame?

	// --- GOD RAYS (OroGodRays.cpp) ---------------------------------------
	// Crepuscular shafts radiating from the sun's screen position. Shares the
	// eclipse's sun (OroFindStar) and, more importantly, shares its FRAME: the
	// client has already drawn a correctly-occluded sun disc into the backbuffer
	// by the time we capture, so the shader's light source and its occlusion both
	// arrive for free. Everything below is computed on the MAIN thread and read by
	// the render path (invariant 1).
	void  UpdateGodRays();      // MAIN thread: the light budget (air / elevation / eclipse)
	void  BuildGodRayScreen(); // RENDER PATH: where the sun is on screen (2026-08-15)
	void  DrawGodRayPass();     // the IPI resample; self-gating, called from both branches
	float grSunU     = 0.5f;    // sun position in UV - MAY LIE OUTSIDE [0,1]: the shafts
	float grSunV     = 0.5f;    //   still converge correctly on an off-screen source, and
	                            //   grFade is what retires the effect before it degrades
	float grFade     = 0.0f;    // 0..1 combined screen-proximity + elevation fade
	float grStr      = 0.0f;    // master strength AFTER the atmosphere gate
	float grTintR    = 1.0f;    // scattered-light colour; reddens as the sun sets
	float grTintG    = 1.0f;
	float grTintB    = 1.0f;
	bool  grActive   = false;   // is there anything to draw this frame?

	// --- AURORA (OroAurora.cpp) ------------------------------------------
	// The auroral curtains: additive ribbon GEOMETRY (a curtain IS a ribbon, so this
	// is the round-5 plasma machinery reused), built and PROJECTED on the MAIN thread
	// in UpdateAurora (invariant 1) into aurVtx, pushed + drawn by DrawAuroraPoly in the
	// EXTERNAL branch only. Screen-space geometry has no depth, so - like the shimmer -
	// it stays out of internal views until patch (g); the roadmap's "domain BOTH" was
	// over-optimistic for a geometry effect. No client patch: the poly draws additively
	// (patch d) with the same ALPHABLEND fallback as the plasma. Occlusion is a per-vertex
	// ray-sphere test against the planet AND the camera-target vessel (so the ship hides
	// the curtains behind it), not invariant 16's depth map. Uses PlasVtx (above).
	// ⚠️ HARD CEILING: AUR_MAX_TRI * 3 MUST STAY UNDER 65536.
	// These are drawn as ONE non-indexed DrawPrimitive off a single vertex stream, and a
	// D3D9 stream is limited to 65535 vertices. Past that CreateVertexBuffer fails, the
	// client's D3D9Triangle constructor does not check it (it calls Update() anyway), and
	// Update dereferences a NULL pVB - an instant CTD at the first draw, at ANY world,
	// regardless of how few triangles are actually filled. That is exactly what 36864
	// (110592 verts) did on 2026-08-07. 21840 * 3 = 65520 verts, which is the largest safe
	// multiple. To go bigger the poly must be split across several HPOLYs.
	static const int AUR_MAX_TRI = 21840;   // Ribbons (<=6) x Thickness sheets (<=4) x two
	                                        //   ovals x 96 seg x 8 bands x 2 tri = 73728 in
	                                        //   the absolute worst case, which this pool is
	                                        //   well under - by necessity (see above) but
	                                        //   also by design. It is not a guess: a
	                                        //   quad is skipped when all four corners are
	                                        //   dark, so the occluded far pole and the whole
	                                        //   daylit half cost nothing, and real emission
	                                        //   runs ~25-35% of the theoretical max. The FULL
	                                        //   buffer is pushed every frame (invariant 3),
	                                        //   so every triangle here is a per-frame memcpy
	                                        //   whether it draws or not - which is exactly
	                                        //   why this is not simply sized to the maximum.
	                                        //   UpdateAurora LOGS ONCE if it ever fills, so
	                                        //   a clipped curtain is diagnosable instead of
	                                        //   looking like a missing chunk (the extreme
	                                        //   combination is 6 ribbons at full thickness
	                                        //   in complete polar night).
	void    BuildVapourGeometry();          // RENDER PATH: the cone -> vapVtx (2026-08-15)
	void    UpdateAurora();                 // per frame, MAIN thread: identify the world,
	                                        //   load its settings, gather the world state
	                                        //   the build needs. No geometry (2026-08-15).
	void    BuildAuroraGeometry();          // RENDER PATH: the curtains -> aurVtx, off the
	                                        //   render camera. Here rather than in
	                                        //   clbkPreStep because clbkPreStep does not run
	                                        //   while PAUSED - see ProjCam above.
	void    DrawAuroraPoly(oapi::Sketchpad* pSkp);   // push + additive draw
	PlasVtx aurVtx[AUR_MAX_TRI * 3];
	// --- LIGHTNING (OroLightning.cpp, 2026-08-08) ------------------------
	// Flash discs in the cloud deck, seen from above. Storm cells are DETERMINISTIC
	// functions of (cloud-frame position, sim time) gated by the planet's own cloud
	// tile alpha (read from Cloud.tree - the file-scope reader lives in
	// OroLightning.cpp); the only cross-frame state is the sub-second flash-event
	// ring below, which self-expires in ~1.3 s REAL time - the blink envelope's
	// class of transient, not G10's accumulated kind. Geometry is the aurora's
	// pattern verbatim: built + projected in clbkPreStep, additive Sketchpad
	// triangles, per-vertex depth for the patch-(g) clip, full-buffer zero-padded
	// update (invariant 3). LTG_MAX_TRI * 3 = 6144 verts, far under invariant
	// 19(d)'s 65535 stream ceiling.
	static const int LTG_MAX_TRI   = 2048;  // 16 flashes x 90 tris = 1440 worst case
	static const int LTG_MAX_FLASH = 16;    // concurrent flash events (each <= ~1.3 s)
	struct LtgFlash {
		double latT = 0, lonT = 0;          // cell centre, CLOUD-TEXTURE frame [rad]
		double radM = 0;                    // glow radius [m]
		float  startAnim = -1.0f;           // animT at first stroke; < 0 = free slot
		float  seed  = 0.0f;                // stroke pattern + per-sector mottle hash
		float  amp   = 0.0f;                // cell intensity x life fade at trigger time
		float  cov0  = 0.0f;                // coverage at the centre (vertex fallback)
		bool   test  = false;               // TEST cell: gates bypassed
	};
	LtgFlash ltgFlash[LTG_MAX_FLASH];
	float    ltgPrevAnim = 0.0f;            // last frame's animT (flash-crossing detect)
	void    UpdateLightning(double simt);   // per frame, MAIN thread: cloud map, districts,
	                                        //   storm cells, flash scheduling. No geometry
	                                        //   since 2026-08-15.
	void    BuildLightningGeometry();       // RENDER PATH: the discs -> ltgVtx, off the
	                                        //   render camera (clbkPreStep is dead while
	                                        //   PAUSED - see ProjCam above).
	void    DrawLightningPoly(oapi::Sketchpad* pSkp);   // push + additive draw
	PlasVtx ltgVtx[LTG_MAX_TRI * 3];
	float   ltgDepth[LTG_MAX_TRI * 3];      // per-vertex Euclidean camera distance (patch g)
	int     ltgVtxN     = 0;                // vertices filled this frame (multiple of 3)
	HPOLY   hLightningPoly = NULL;          // device resource - released with the others
	bool    ltgActive   = false;            // geometry built this frame => draw it
	// --- TEXTURED FLASHES (client patch l, 2026-08-08) ---------------------
	// The flash is the CLOUD IMAGE lighting up: a 512x512 atlas (4x4 slots of 128^2,
	// slot i belongs permanently to ltgFlash[i] - no allocator) is baked per flash
	// from the FULL-RES cloud tile alpha, and the pad's new modulate band multiplies
	// texture x Gouraud per pixel (radial + envelope + tint stay in the vertices, the
	// cloud's own shape comes from the texture). Without the patch, or if the atlas
	// fails, the per-vertex-coverage Gouraud path below runs unchanged - same
	// dormant-degrade discipline as every other patched capability.
	struct TexVtxP { float x, y, u, v; DWORD c; }; // layout == gcCore::texVtx (asserted
	                                               //   in OroLightning.cpp)
	TexVtxP    ltgTexVtx[LTG_MAX_TRI * 3];  // the textured-mode vertex staging
	DWORD      ltgAtlasImg[512 * 512];      // CPU-side atlas image (1 MB BSS); slots are
	                                        //   rebaked in place, whole texture uploaded
	SURFHANDLE hLtgAtlas   = NULL;          // device resource - released with the others
	bool       ltgTexMode  = false;         // patch (l) bound AND the atlas exists
	bool       ltgTexTried = false;         // one-shot probe/create guard (per session)
	// Per-vertex CAMERA-SPACE depth, parallel to aurVtx (patch g). Handed to
	// CreateTrianglesDepth so the client depth-clips the curtains against the real scene
	// (cockpit / hull / terrain) instead of painting over them - which is what lets the
	// aurora into the VC through the windows, and drops the external bounding-sphere hack.
	float   aurDepth[AUR_MAX_TRI * 3];
	int     aurVtxN     = 0;                // vertices filled this frame (multiple of 3)
	HPOLY   hAuroraPoly = NULL;             // device resource - released with the others
	bool    aurActive   = false;            // geometry built this frame => draw it
	// CLIENT CAPABILITY, not an aurora property (renamed from aurDepthOK 2026-08-07 when the
	// plasma became the second consumer): the client carries patch (g) AND the depth buffer
	// is live this session (SunGlare on). Probed by BINDING, invariant 18(a).
	bool    depthClipOK = false;
	int     depthClipLogged = -1;           // -1 = never announced; else the value logged

	// --- THE PROJECTION CAMERA (2026-08-15, the pause fix) -----------------
	// ⚠️ clbkPreStep IS NOT CALLED WHILE PAUSED. Anything built there therefore FREEZES
	// while the render callback keeps drawing it, which is why a paused pan or zoom used
	// to slide the aurora, the lightning and the plasma off their subjects - the most
	// reported issue of the whole closed beta (2/2 testers, ~6 reports). The one effect
	// that always behaved is the one ORO does not draw (VC shadows, invariant 18), and
	// the TRAIL behaves because its projection already lives in the render path.
	// THE RULE THAT COMES OUT OF IT: world geometry may freeze while paused - nothing is
	// moving - but the PROJECTION may not, because the camera is still moving. So the
	// aurora, lightning and plasma builds now run in the RENDER PATH off this camera.
	// preStepCam is the fallback on a client without patch (k): one step stale, which is
	// exactly the old behaviour - degraded, not broken.
	struct ProjCam { VECTOR3 pos; MATRIX3 rot; double tanAp; };
	ProjCam preStepCam;
	bool    preStepCamValid = false;
	void    SnapPreStepCam();                                        // main thread
	bool    FillProjCam(VECTOR3& pos, MATRIX3& rot, double& tanAp);  // render path

	// ⚠️ THE COMPANION TO FillProjCam, AND IT IS NOT OPTIONAL FOR VESSEL-SCALE GEOMETRY.
	// Orbiter's "global" frame is SOLAR-SYSTEM BARYCENTRIC (invariant 21a), so a position
	// sampled in clbkPreStep is stale by one step of the whole planet's orbital motion -
	// Earth covers ~500 m at 60 fps. Pair the RENDER camera with a PRE-STEP anchor and the
	// effect lands hundreds of metres from its vessel and jitters with frame pacing, which
	// is exactly what the plume and the vapour cone did on 2026-08-15.
	// Returns the correction to ADD to any global position taken from that body at
	// pre-step. Zero on a client without patch (k2) - degraded to the old one-step lag,
	// not broken. Harmless for planet-scale geometry, which is why the aurora and the
	// lightning never showed it.
	// ⚠️⚠️ THE SECOND ARGUMENT IS THE BODY'S OWN CENTRE AT PRE-STEP - **NOT** the point
	// you are correcting. Pass the point instead and the shift comes out as
	// (centre_render - point_prestep), so point + shift == centre_render and EVERY point
	// you correct collapses onto the body's centre of mass. That is exactly what happened
	// to the plume on 2026-08-15: two nozzles became one plume sitting half inside the
	// hull, and it looked like a projection bug rather than an argument bug.
	VECTOR3 RenderEpochShift(OBJHANDLE h, const VECTOR3& bodyCentrePreStep);

	// --- THE VAPOUR CONE (OroVapour.cpp, 2026-08-11) ---------------------
	// Transonic condensation. Built + projected on the MAIN thread (invariant 1) into
	// vapVtx, drawn by DrawVapourPoly. EXTERNAL ONLY (invariant 10): the cone forms
	// around and behind the hull, so from the pilot's seat it is mostly behind you -
	// and without a domain decision up front a screen-space sheet would paint the
	// canopy frame the way the shimmer would.
	//
	// ⚠️ THE ONE THING THAT MAKES THIS DIFFERENT FROM EVERY OTHER POLY IN ORO:
	// IT DRAWS ALPHA-BLENDED, NOT ADDITIVE. Condensed water scatters and OCCLUDES;
	// additive light cannot darken, so an additive cone could only ever be a haze that
	// brightens the hull behind it. This is the first live use of the recipe graveyard
	// G11 left on the shelf when the trail's smoke layer died with it - "a second poly
	// drawn with the pad's default blend state BEFORE the additive one, no client patch
	// involved" - and that ordering is why DrawVapourPoly is called first in the
	// pre-resolve slot, ahead of the trail, the plasma and the plume.
	//
	// Vessel-anchored, so invariant 21(b)'s render-epoch problem does not apply: a
	// tracking camera cancels the epoch, which is why the plasma never needed patch (k)
	// either. Built with the pre-step camera like everything else attached to a hull.
	// VAP_MAX_TRI * 3 = 6912 verts, far under invariant 19(d)'s 65535 stream ceiling.
	static const int VAP_NA      = 64;      // angular segments around the flow axis. The
	                                        //   cone is a surface of REVOLUTION, so this is
	                                        //   the only place its smoothness comes from -
	                                        //   and it is smooth at any count by
	                                        //   construction (invariant 20e's law: nothing
	                                        //   here inherits mesh tessellation).
	static const int VAP_NR      = 9;       // rings apex -> rim, last one the feather band
	static const int VAP_MAX_TRI = VAP_NA * VAP_NR * 2 + 64;   // 1216
	void    UpdateVapour();                 // per frame, main thread - builds vapVtx
	void    DrawVapourPoly(oapi::Sketchpad* pSkp);   // push + ALPHA-BLENDED draw
	PlasVtx vapVtx[VAP_MAX_TRI * 3];
	float   vapDepth[VAP_MAX_TRI * 3];      // per-vertex camera distance (patch g). The cone
	                                        //   WRAPS the hull, so half of it is genuinely
	                                        //   behind the vessel - per-pixel depth is what
	                                        //   makes that half disappear instead of painting
	                                        //   the ship out. Without it the cone draws over
	                                        //   the hull; the caption says so.
	int     vapVtxN     = 0;                // vertices filled this frame (multiple of 3)
	HPOLY   hVapourPoly = NULL;             // device resource - released with the others
	bool    vapActive   = false;            // geometry built this frame => draw it

	// --- EXHAUST SHIMMER (the first ENVIRONMENT effect) --------------------
	// Engine plumes projected to SCREEN SPACE on the main thread, handed to PSShimmer as
	// uniforms - the render callback must make no oapi calls (invariant 1), so all the
	// vessel/camera queries and the projection happen in clbkPreStep. Each plume is a
	// screen-space CAPSULE: root (a) -> tip (b) in UV, a UV radius, and a strength.
	// MAX_PLUMES is also the shader's array size - keep them in step.
	static const int MAX_PLUMES = 6;
	struct PlumeScr {
		float ax, ay, bx, by;   // plume root -> tip, in UV (0..1)
		float rad;              // haze radius around the axis, in UV (aspect-corrected in the shader)
		float str;              // 0..1 strength (thrust curve x air density x hull visibility)
		float hpk;              // 0..1 position ALONG the plume where turbulence peaks (moves aft with thrust)
	};
	PlumeScr plumes[MAX_PLUMES] = {};
	int      plumeCount = 0;    // 0 = nothing to draw, skip the whole pass

	// Rebuild the plume table for this frame: camera-target vessel (falls back to focus),
	// main/hover/retro thrusters ONLY (no RCS puffs), strongest MAX_PLUMES kept, off-screen
	// and behind-camera plumes culled. Main thread only.
	void UpdateShimmerPlumes();

	// --- PLUME EXPANSION (pressure-dependent exhaust, 2026-08-09) ----------
	// The nozzle is expanded for ONE ambient pressure; static pressure decides what the
	// jet does about everywhere else - overexpanded at sea level = pinched narrow with
	// the shock-diamond train, underexpanded high up = the wide faint expansion bloom.
	// Our own ADDITIVE OVERLAY on the stock exhaust (never Del/AddExhaust churn - the
	// roadmap's assessed plan): screen-space ribbon layers along each plume axis, built
	// per frame in UpdatePlumeFx (clbkPreStep, invariant 1 - OroPlume.cpp), drawn in
	// the patch-(i) pre-resolve slot so the diamond cores accumulate in fp16 and BLOOM
	// to white (the Firefly law), per-vertex depth for the patch-(g) clip - which also
	// cuts a hover plume at the runway surface for free (scene depth includes terrain).
	// EXTERNAL view only (invariant 10: from a cockpit your own engines are behind you).
	// Same thruster set + strongest-6 rule as the shimmer; the two scans stay separate
	// on purpose this round (the shimmer is settled code - merge when it is revisited).
	// ⚠ PLM_MAX_TRI * 3 must stay under 65536 - invariant 19(d)'s vertex-stream ceiling.
	static const int PLM_MAX_TRI = 8192;    // 24576 verts: 6 plumes x 3 layers (sheath +
	                                        // core + diamond lozenges) x ~350 tris + slack

	// THE PLUME MODEL (2026-08-09) - one physics model, MANY consumers. Built once
	// per step in BuildPlumeModel (OroPlume.cpp, main thread): per qualifying
	// exhaust, the world-frame axis, the effective sizes, and the four PHYSICS
	// factors (expansion-ratio spacing, altitude washout, throttle->NPR coupling,
	// separation unsteadiness), all ANCHORED to 1.0 at (sea level, full throttle)
	// and pinned there wholesale in LAB mode. Consumers: (1) UpdatePlumeFx draws
	// the jet from it; (2) UpdateShimmerPlumes shapes its heat-haze capsules from
	// it, so haze and jet always agree; (3) PLANNED: the exhaust particle system
	// (pad smoke / ablative trail) will spawn from the same entries - that is the
	// whole reason this table exists as a table.
	struct PlumeModel {
		int     grp;            // ORO_THR_MAIN/HOVER/RETRO/USER - WHICH GROUP'S SETTINGS
		                        //   this plume answers to (2026-08-16). Carried from the
		                        //   candidate scan so every consumer - jet, shimmer, soot,
		                        //   colour - reads g_fx.thr[grp] instead of one shared set.
		VECTOR3 rootG, dirG;    // nozzle exit + exhaust FLOW direction, global frame
		OBJHANDLE hOwn;         // the vessel rootG was taken from - a docked STACK can
		                        //   contribute entries from several. ⚠️ rootG is a
		                        //   BARYCENTRIC position sampled at PRE-STEP, and the
		                        //   consumers now run in the render path a step later,
		                        //   where Earth has moved ~500 m at 60 fps (invariant
		                        //   21a). RenderEpochShift corrects it per entry.
		VECTOR3 ownCg;          // that vessel's own CENTRE at pre-step. Stored because
		                        //   RenderEpochShift needs the BODY CENTRE, not the point
		                        //   being corrected - passing rootG collapsed every nozzle
		                        //   onto the CoM (2026-08-15). See its declaration.
		double  level;          // thrust level 0..1
		double  wRef;           // effective nozzle width [m] (stock wsize x Width knob)
		double  w0;             // core exit radius [m] (widthF folded in)
		double  spacing;        // shock-cell length [m] (Spacing knob x physics)
		double  L;              // visible jet length [m] (regime + Length knob + throttle)
		float   dW, bW;         // regime weights (per vessel, copied per entry)
		float   decay;          // per-cell decay base (washout: 0.80 at sea level)
		float   sepW;           // 0..1 separation-flicker weight (deep overexpansion)
		float   diaF;           // diamond contrast factor (throttle coupling)
		float   puff;           // 0..1 throttle-transient bloom (physics mode; decays
		                        //   in REAL time - the flash-event class of transient)
		DWORD   exIdx;          // the vessel's exhaust index: the entry's IDENTITY
		                        //   across frames (puff tracking) and the soot
		                        //   streaks' random seed (stable however the
		                        //   strongest-6 sort reshuffles the slots)
	};
	PlumeModel plmModel[MAX_PLUMES];
	int        plmModelN = 0;               // entries this step (0 = nothing burning)
	float      plmShimStr = 0.0f;           // shimmer strength of the STRONGEST group that
	                                        //   actually contributed a capsule this frame.
	                                        //   PSShimmer has one full-frame uniform, so a
	                                        //   per-plume strength is not expressible; this
	                                        //   is the honest reduction (2026-08-16).
	double     plmRho    = 0.0;             // ambient density at the vessel (the
	                                        //   shimmer's atmosphere gate reads it)
	// Last step's (exhaust idx -> level, puff), matched BY INDEX so slot reshuffles
	// in the strongest-6 sort can never fake a transient. Vessel change resets all.
	OBJHANDLE  plmVesPrev = NULL;
	int        plmPrevN   = 0;
	DWORD      plmIdxPrev[MAX_PLUMES] = {};
	double     plmLvlPrev[MAX_PLUMES] = {};
	float      plmPuffPrev[MAX_PLUMES] = {};
	void BuildPlumeModel();                 // main thread, BEFORE both consumers
	void UpdatePlumeFx();                   // consumer 1: the jet geometry

	// --- BELL GLOW (OroBell.cpp, 2026-08-09) -----------------------------
	// Incandescent nozzle shells: the author's Meshes\ORO\<class>_bell.msh is
	// ADDED to the camera-target vessel (a borrow - removed on every exit path)
	// and its four named materials are driven through oapiSetMaterial with the
	// thermal model. Parsing, thermal state and the material driver live in
	// OroBell.cpp as file statics; these two members are what the module's
	// lifecycle paths need to see.
	void UpdateBellGlow(double simdt);      // main thread (clbkPreStep)
	void ReleaseBellGlow();                 // DelMesh + clear (safe to call twice)
	OBJHANDLE bellVessel  = NULL;           // vessel currently carrying our shell
	UINT      bellMeshIdx = (UINT)-1;       // our mesh's index on that vessel

	// ABLATIVE SOOT STREAKS (2026-08-09) - the dark layer: alpha-blended wisps
	// drawn AFTER the additive plume poly so they genuinely dim the glow they sit
	// in (soot is IN the jet; the G11 draw-dark-first recipe is for smoke BEHIND
	// content, which this deliberately is not). Own poly + buffers because the
	// blend state differs; same per-vertex depth, same full-buffer rule.
	static const int PLM_DK_MAX_TRI = 6144; // 18432 verts: 6 plumes x 16 lifecycled
	                                        // streaks x ~55 tris worst case (the dynamic
	                                        // soot rework, 2026-08-09)
	PlasVtx plmDkVtx[PLM_DK_MAX_TRI * 3];
	float   plmDkDepth[PLM_DK_MAX_TRI * 3];
	int     plmDkVtxN = 0;
	HPOLY   hPlumeDkPoly = NULL;            // device resource - released with the others
	void DrawPlumePoly(oapi::Sketchpad* pSkp, bool depthClip);
	PlasVtx plmVtx[PLM_MAX_TRI * 3];
	float   plmDepth[PLM_MAX_TRI * 3];      // per-vertex EUCLIDEAN camera distance (patch g)
	int     plmVtxN = 0;                    // 0 = nothing to draw this frame
	HPOLY   hPlumePoly = NULL;              // device resource - released with the others

	// --- EXHAUST PARTICLES (OroParticles.cpp, 2026-08-09) ----------------
	// ORO DRAWS NOTHING HERE. This is invariant 18's category - controlling
	// something we do not render - pointed at the CORE's own particle system:
	// the user gets live sliders on the PARTICLESTREAMSPEC fields a vessel author
	// would set in code. See the file header for the three source findings that
	// shaped it (the core COPIES the spec, so changes rebuild; patch (n) gates
	// ExhaustStream only, so our plain ParticleStreams survive stock suppression;
	// and the particle texture is a 2x2 atlas of four variants).
	//
	// Streams are BORROWED and handed back on every exit path (invariant 14) -
	// together with their patch-(o) exemptions, which are keyed on the stream
	// pointer and so must be cleared before the stream is freed.
	static const int PRT_MAX_STREAM = 24;
	struct PrtStream {
		PSTREAM_HANDLE  h   = NULL;         // what we must hand back
		OBJHANDLE       hV  = NULL;         // whose vessel it is on
		THRUSTER_HANDLE th  = NULL;         // which thruster it belongs to
	};
	PrtStream  prtStr[PRT_MAX_STREAM];
	int        prtStrN     = 0;
	SURFHANDLE hPrtTex[4]  = {};            // synthesized tinted 2x2 particle atlas, ONE PER
	                                        //   THRUSTER GROUP (2026-08-16). The particle
	                                        //   spec has no colour field - colour lives in
	                                        //   the texture - so per-group colour means one
	                                        //   texture per group. 4 x 256^2 is cheap; the
	                                        //   alternative was making the swatch lie.
	bool       prtTexMode  = false;         // patch (l) bound AND the texture exists
	bool       prtTexTried = false;         // one-shot probe/create guard (per session)
	void UpdateParticles(double simdt);     // main thread (clbkPreStep): rebuild on change
	void ReleaseParticles(const char* why); // hand every stream back (safe twice)
	void ForgetParticleVessel(OBJHANDLE hVessel);   // clbkDeleteVessel
	void ReleaseParticleTex();              // session teardown (device resource)

	// STOCK EXHAUST suppression (client patch n) - a BORROWED thing in invariant 14's
	// sense: while g_fx.stockExhaust is off, the stock exhaust billboards + stream
	// emission of the camera-target vessel AND EVERY VESSEL DOCKED TO IT are
	// suppressed CLIENT-SIDE, and the suppression is handed back on every exit path
	// (pill on, disarm/Ctrl+G, target change, vessel delete, simulation end,
	// destructor). Pushed on CHANGE, never per frame (invariant 18). The STACK, not
	// one vessel: Orbiter's Shuttle is four docked vessels, so suppressing only the
	// camera target would leave the SRBs' stock billboards burning under our overlay.
	static const int STACK_MAX = 8;
	void UpdateStockExhaust();              // main thread (clbkPreStep)
	void ReleaseStockExhaust();             // the hand-it-back path, safe to call twice
	OBJHANDLE hStockExSupp[STACK_MAX] = {}; // vessels currently suppressed
	int       nStockExSupp = 0;
	DWORD     stockExFlags = 0;            // GCEXH_* currently pushed (the split)

	// --- AUDIO (XRSound) ---------------------------------------------------
	// XRSound proxy for this module: created in clbkSimulationStart, deleted in
	// clbkSimulationEnd. Module sounds always play as PlaybackType::Global (see
	// XRSound.h), so we gate audibility ourselves on viewGate + masterArmed, mirroring
	// how the render callback gates the visuals. NULL / IsPresent()==false => silent
	// (every XRSound call no-ops), ORO's visuals unaffected.
	XRSound* pXRSound = nullptr;

	// --- PREMIUM (IPI / HLSL frame-resampling) pipeline --------------------
	// Grey-out and (later) blur/tilt resample the LIVE frame through the client's
	// image-processing interface, which requires the ORO client patch (backbuffer
	// capture). All of this stays inert on an unpatched client (ipiReady == false).
	gcIPInterface* pIPIGrey   = nullptr; // PSGrey shader interface (orofx.hlsl); per session
	gcIPInterface* pIPIBlur   = nullptr; // PSBlur shader interface (orofx.hlsl); per session
	gcIPInterface* pIPIChroma = nullptr; // PSChroma (chromatic aberration); per session
	gcIPInterface* pIPISwim   = nullptr; // PSSwim (peripheral UV warp); per session
	gcIPInterface* pIPITilt   = nullptr; // PSTilt (roll rotation); per session
	gcIPInterface* pIPIShimmer = nullptr;// PSShimmer (exhaust heat haze, EXTERNAL view); per session
	gcIPInterface* pIPIPlasma  = nullptr;// PSPlasma (reentry cockpit glow, INTERNAL view); per session
	gcIPInterface* pIPIEclipse = nullptr;// PSEclipse (shadow dim + dark adaptation, BOTH views); per session
	gcIPInterface* pIPIGodRay  = nullptr;// PSGodRay (crepuscular shafts, BOTH views); per session
	bool           ipiTried  = false;    // one-shot creation guard (don't retry failed compiles every frame)
	bool           ipiReady  = false;    // the client exposes backbuffer capture (patch b present)
	SURFHANDLE     hFrameTex = NULL;      // RT-texture: backbuffer copy - grey input / blur H-pass input
	SURFHANDLE     hBlurTex  = NULL;      // RT-texture: blur intermediate (H-pass output -> V-pass input)
	DWORD          texW = 0, texH = 0;    // hFrameTex/hBlurTex size; recreated on viewport change

	// Build/refresh the IPI shader + capture texture. Creation is an oapi/resource
	// operation, so it happens on the MAIN thread (clbkSimulationStart / clbkPreStep),
	// never inside the render callback.
	void EnsureIPI();
	void EnsureFrameTex();

	// ReleaseDeviceResources() is declared PUBLIC above (the shutdown thunk calls it).
	// EVERY device-bound resource we own, released in one place: the effect polys, the
	// IPI shader interfaces and the oapiCreateSurfaceEx textures. Split out of
	// clbkSimulationEnd (2026-08-10) because that callback arrives ~400 ms AFTER the
	// client has already destroyed its render window, so the client force-released our
	// surfaces first and logged `UnDeleted Surfaces Detected` every session. gcCore's
	// GENERICPROC_SHUTDOWN fires at the TOP of D3D9Client::clbkCloseSession - before the
	// render stack check, before bRunning=false, long before the surface sweep - so
	// that is where this belongs. clbkSimulationEnd still calls it for the paths the
	// client shutdown never reaches (no client, stock client, module teardown).
	// EVERY handle is nulled as it goes, so calling this twice is a no-op the second
	// time. Vessel BORROWS (streams, meshes, lights) are NOT released here - those stay
	// in clbkSimulationEnd, where the vessels are unambiguously still alive.
};
