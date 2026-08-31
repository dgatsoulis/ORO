// ==============================================================
// OroDialog.cpp
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

// ============================================================================
// ORO - control dialog implementation (owner-drawn, dark themed)
// See OroDialog.h for the concept.
//
// LAYOUT: a fixed header (banner + master ARMED strip + NAV row), one SCROLLING
// content pane holding the CURRENT PAGE, and a fixed status line. Pages form a
// MENU TREE - WORLD / VESSEL / PILOT, submenus, then leaves of sliders.
//
// HISTORY, because it explains the shape:
//   - The panel grew to ~950 px tall by accretion.
//   - 2026-07-31 attempt 1 folded it into four TABS. Rejected on sight: the pane
//     had to be sized to the tallest tab, so every other tab showed dead space.
//   - 2026-07-31 attempt 2 was the user's call - back to one vertical strip at a
//     NARROWER 500 px, with a custom scrollbar for the overflow.
//   - 2026-08-07 the section list outgrew one strip, so the user asked for TABS
//     AGAIN - but this time each tab SCROLLS its own content, so the dead-space
//     problem that killed attempt 1 (a pane sized to the tallest tab) cannot recur.
//     Five tabs (G-FORCE / THRUSTER / REENTRY / ATMOS / VC), fixed bar below the
//     master strip; the master arm + SAVE stay fixed above it, reachable anywhere.
//   - 2026-08-29 the five tabs became a MENU TREE (his mockup): three categories
//     on a main menu, drill-down pages, breadcrumb + BACK / BACK TO MAIN in a
//     fixed nav row. Every page still scrolls its own content, so the dead-space
//     law holds; the tab-era section painters and click handlers survive intact,
//     re-anchored per leaf.
//
// SIZE IS IN PIXELS, NOT DIALOG UNITS. The .rc size is only a starting guess:
// DLU->px depends on the shell font metrics, which differ per machine/DPI (on
// the author's box 372 DLU came out ~740 px, not the 558 the standard (6,13)
// base units predict). WM_INITDIALOG therefore forces the CLIENT area to
// DLG_W x DLG_H px outright, so this file's pixel constants mean what they say.
// The layout still reads rc.right for the right-hand columns, so it degrades
// gracefully if a host ever overrides the size anyway.
//
// The row tables (g_visRows / g_motRows / g_envRows / g_shakeRows / g_envKnobs)
// are the assembly line: one line adds an effect row.
// ============================================================================

#include "OroDialog.h"
#include "OroState.h"
#include "resource.h"

// Orbiter SDK (min/max macro dance, same as OroModule.h - see note there).
#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif
#include "Orbitersdk.h"
// (commdlg.h / ChooseColor is GONE, 2026-08-09: the modal picker paused the whole
//  sim - the message loop it pumps blocks the sim thread, and closing it slammed
//  one giant dt into the physics, throwing landed vessels into the air. The
//  swatches open the custom in-panel picker now - see PaintColourPicker.)
#pragma comment(lib, "comdlg32.lib")

// ----------------------------------------------------------------------------
// Palette (COLORREF, 0x00BBGGRR via RGB()) - mirrors the approved mockup.
// ----------------------------------------------------------------------------
static const COLORREF CLR_BG        = RGB(0x10, 0x13, 0x18);  // panel background
static const COLORREF CLR_BG_HEADER = RGB(0x18, 0x1D, 0x24);  // banner fallback / armed strip
static const COLORREF CLR_LINE      = RGB(0x23, 0x29, 0x35);  // separators
static const COLORREF CLR_TEXT      = RGB(0xC7, 0xCD, 0xD6);  // primary text
static const COLORREF CLR_TEXT_DIM  = RGB(0x6B, 0x74, 0x84);  // secondary text
static const COLORREF CLR_TEXT_HI   = RGB(0xE8, 0xEB, 0xEF);  // values / emphasis
static const COLORREF CLR_TRACK     = RGB(0x1C, 0x21, 0x2B);  // slider trough / scrollbar track
static const COLORREF CLR_ACCENT    = RGB(0xE2, 0x4B, 0x4A);  // vision accent (red)
static const COLORREF CLR_PILL_ON   = RGB(0x1D, 0x9E, 0x75);  // enable pill on
static const COLORREF CLR_PILL_OFF  = RGB(0x3A, 0x41, 0x50);  // enable pill off / scrollbar thumb
// SAVE / REVERT FEEDBACK (2026-08-25). The status line sits at the very bottom of a busy
// panel and is easy to miss, so the two events are colour-coded AND the button that caused
// them lights up in the SAME hue: one event, one colour, in two places at once, which is
// what actually trains the eye to look down there. Green reads as "written to disk", amber
// as "thrown away and re-read from disk". A write FAILURE keeps CLR_ACCENT - red outranks
// both, and it is the one case where the user has to act.
static const COLORREF CLR_MSG_SAVE  = RGB(0x2E, 0xC4, 0x8D);  // saved (CLR_PILL_ON, lifted for text)
static const COLORREF CLR_MSG_REVRT = RGB(0xE0, 0xA0, 0x3C);  // reverted (amber)

// ----------------------------------------------------------------------------
// Dialog-local state
// ----------------------------------------------------------------------------
static HWND     g_hDlg = NULL;
static HINSTANCE g_hInst = NULL;          // our DLL instance, stashed by OroDlg_Open so the
                                          // HELP button can open a second Orbiter dialog
static HWND     g_hHelp = NULL;           // the HELP window (NULL = closed). Declared up
                                          // here rather than with the rest of the help
                                          // module at the bottom because the ARMED strip
                                          // paints the HELP button lit while it is open.
static HBITMAP  g_hBanner = NULL;         // user artwork, Modules\ORO\banner.bmp
static int      g_bannerW = 0, g_bannerH = 0;
static HFONT    g_fontText = NULL, g_fontSmall = NULL, g_fontBig = NULL, g_fontMono = NULL;
static HFONT    g_fontMenu = NULL;        // menu-page button labels: bold, between Text
                                          // and the banner-fallback wordmark of fontBig
static int      g_scroll = 0;             // content pane scroll offset, px (0 = top)

// ----------------------------------------------------------------------------
// NAVIGATION (2026-08-29, his mockup). The five tabs became a MENU TREE: three
// categories (WORLD / VESSEL / PILOT), submenus beneath, and LEAF pages holding
// exactly one subject's controls - which is the point: "this removes some of
// the confusion the current dialog box has". A fixed NAV ROW under the ARMED
// strip carries the breadcrumb + BACK + BACK TO MAIN. Every leaf keeps its old
// section painters, click handlers and save scopes; only where the chains
// ANCHOR changed - each re-anchors at LeafTopY(), the TabTopY re-anchoring
// pattern at finer grain (proven on the tabs, then the sub-tabs, now this).
// Re-entering a category always lands on its MENU page (his call) - automatic,
// because leaving a leaf pops it off the stack rather than parking it anywhere.
// ----------------------------------------------------------------------------
enum {
	// MENU pages first - IsMenuPage() tests <= PG_PILOT, so keep them contiguous.
	PG_MAIN = 0, PG_WORLD, PG_WEATHER, PG_VESSEL, PG_THRUSTERS, PG_REENTRY, PG_PILOT,
	// LEAF pages - one subject each, plus its scoped SAVE/REVERT row.
	PG_GFORCES, PG_SCENARIOS, PG_VC,                                  // PILOT
	PG_EXHAUST, PG_PARTICLES, PG_PLASMA, PG_VAPOUR, PG_FLIGHTAID,     // VESSEL
	PG_RAIN, PG_LIGHTNING, PG_AURORA, PG_ECLIPSE, PG_GODRAYS,         // WORLD
	PG_COUNT
};
static const char* PageName(int pg)
{
	static const char* n[PG_COUNT] = {
		"MAIN MENU", "WORLD", "WEATHER", "VESSEL", "THRUSTERS", "REENTRY", "PILOT",
		"G-FORCES", "SCENARIOS", "VIRTUAL COCKPIT",
		"EXHAUST", "PARTICLES", "PLASMA", "VAPOUR CONES", "FLIGHT AID",
		"RAIN", "LIGHTNING", "AURORA", "ECLIPSE", "GOD RAYS"
	};
	return (pg >= 0 && pg < PG_COUNT) ? n[pg] : n[0];
}
static bool IsMenuPage(int pg)    { return pg <= PG_PILOT; }
// The engine-group cycler heads EXHAUST and PARTICLES as a FIXED row: it changes
// what every control on those pages MEANS, so it may not scroll out of reach
// (the reason it used to live in the ARMED strip, kept under its new roof).
static bool PageHasGrpRow(int pg) { return pg == PG_EXHAUST || pg == PG_PARTICLES; }

// The stack IS the breadcrumb. Depth 1 = the main menu; BACK pops one level,
// BACK TO MAIN resets. Deliberately session-static and never saved: the panel
// reopens where it was within a session, and every session starts at the menu.
static int g_navStack[6] = { PG_MAIN };
static int g_navDepth    = 1;
static int  CurPage()       { return g_navStack[g_navDepth - 1]; }
static void NavPush(int pg) { if (g_navDepth < 6) { g_navStack[g_navDepth++] = pg; g_scroll = 0; } }
static void NavBack()       { if (g_navDepth > 1) { g_navDepth--; g_scroll = 0; } }
static void NavHome()       { g_navDepth = 1; g_scroll = 0; }

// Phase B: the nozzle marker's panel gate (see OroDialog.h). IsWindow makes it
// close-safe with no teardown hook - a destroyed panel answers false by itself.
bool OroDlg_ThrPageLive()
{
	return g_hDlg && IsWindow(g_hDlg)
	    && (CurPage() == PG_EXHAUST || CurPage() == PG_PARTICLES);
}

// One menu page = a column of big buttons. `target` is a PG_* page, or -1 =
// COMING SOON: drawn dim and not clickable - a roadmap signpost, his spec
// ("SNOW (Coming Soon) / WEATHER MODEL (coming soon)").
struct MenuItem { const char* label; const char* sub; int target; };
static const MenuItem MENU_MAIN[] = {
	{ "WORLD",  "the environment - weather, aurora, eclipse, god rays", PG_WORLD  },
	{ "VESSEL", "the hull - thrusters, reentry, flight aid",            PG_VESSEL },
	{ "PILOT",  "the human - g-forces, scenarios, virtual cockpit",     PG_PILOT  },
};
static const MenuItem MENU_WORLD[] = {
	{ "WEATHER",  "rain + lightning; snow and the weather model later", PG_WEATHER },
	{ "AURORA",   "curtains at the magnetic poles",                     PG_AURORA  },
	{ "ECLIPSE",  "the eye inside another body's shadow",               PG_ECLIPSE },
	{ "GOD RAYS", "shafts through the air",                             PG_GODRAYS },
};
static const MenuItem MENU_WEATHER[] = {
	{ "RAIN",          "the storm - outside, windscreen, sounds",       PG_RAIN      },
	{ "LIGHTNING",     "from orbit + inside the storm, with thunder",   PG_LIGHTNING },
	{ "SNOW",          "coming soon",                                   -1           },
	{ "WEATHER MODEL", "coming soon",                                   -1           },
};
static const MenuItem MENU_VESSEL[] = {
	{ "THRUSTERS",  "exhaust + particle streams, per engine group",     PG_THRUSTERS },
	{ "REENTRY",    "plasma + the vapour cone",                         PG_REENTRY   },
	{ "FLIGHT AID", "TEST RIG - shifts the centre of pressure",         PG_FLIGHTAID },
};
static const MenuItem MENU_THRUSTERS[] = {
	{ "EXHAUST",   "shimmer, plume, bell glow - what ORO draws",        PG_EXHAUST   },
	{ "PARTICLES", "Orbiter's own streams, configured live",            PG_PARTICLES },
};
static const MenuItem MENU_REENTRY[] = {
	{ "PLASMA",       "the fire and all its tuning",                    PG_PLASMA },
	{ "VAPOUR CONES", "transonic condensation, two of them",            PG_VAPOUR },
};
static const MenuItem MENU_PILOT[] = {
	{ "G-FORCES",        "vision + motion + the felt-G model",          PG_GFORCES   },
	{ "SCENARIOS",       "scripted G events (LAB mode)",                PG_SCENARIOS },
	{ "VIRTUAL COCKPIT", "shadows + camera shake",                      PG_VC        },
};
static const MenuItem* MenuOf(int pg, int& n)
{
	switch (pg) {
	case PG_WORLD:     n = (int)(sizeof(MENU_WORLD)     / sizeof(MenuItem)); return MENU_WORLD;
	case PG_WEATHER:   n = (int)(sizeof(MENU_WEATHER)   / sizeof(MenuItem)); return MENU_WEATHER;
	case PG_VESSEL:    n = (int)(sizeof(MENU_VESSEL)    / sizeof(MenuItem)); return MENU_VESSEL;
	case PG_THRUSTERS: n = (int)(sizeof(MENU_THRUSTERS) / sizeof(MenuItem)); return MENU_THRUSTERS;
	case PG_REENTRY:   n = (int)(sizeof(MENU_REENTRY)   / sizeof(MenuItem)); return MENU_REENTRY;
	case PG_PILOT:     n = (int)(sizeof(MENU_PILOT)     / sizeof(MenuItem)); return MENU_PILOT;
	default:           n = (int)(sizeof(MENU_MAIN)      / sizeof(MenuItem)); return MENU_MAIN;
	}
}

// (The help window is keyed by PAGE since 2026-08-29 - one table per page, menus
//  included. HelpText/HelpTitle live with the tables, above the help window code.)
static int      g_dragRow = -1;           // index of the VISION row whose slider is being dragged, -1 = none
static int      g_dragMot = -1;           // index of the MOTION row slider being dragged, -1 = none
static int      g_dragShake = -1;         // index of the CAM-SHAKE slider being dragged, -1 = none
static int      g_dragEnv = -1;           // index of the WORLD row slider being dragged, -1 = none
static int      g_dragEnvK = -1;          // index of the WORLD bipolar knob being dragged, -1 = none
static int      g_dragPlume = -1;         // index of the PLUME EXPANSION slider being dragged, -1 = none
static int      g_dragPlmBand = -1;       // EXPANSION BAND dual-slider handle: -1 none, 0 = LOW, 1 = HIGH
static int      g_dragBgl = -1;           // BELL GLOW trim slider being dragged (0 = active), -1 = none
static int      g_dragPrt = -1;           // EXHAUST PARTICLES slider: 0 Amount, 1 Size, 2 Life, -1 = none
static int      g_dragPlas = -1;          // index of the PLASMA TUNING slider being dragged, -1 = none
static int      g_dragEcl  = -1;          // index of the ECLIPSE slider being dragged, -1 = none
static int      g_dragAur  = -1;          // index of the AURORA slider being dragged, -1 = none
static int      g_dragAurRib = -1;        // AURORA Ribbons slider being dragged (0 = active), -1 = none
static int      g_dragAurK = -1;          // AURORA bipolar tilt knob being dragged, -1 = none
static int      g_dragLtg  = -1;          // LIGHTNING slider being dragged, -1 = none
static int      g_dragGry  = -1;          // GOD RAYS slider being dragged, -1 = none
static int      g_dragRain = -1;          // RAIN slider being dragged, -1 = none
static int      g_dragRlt  = -1;          // STORM-LIGHTNING slider (LIGHTNING page's
                                          // in-the-storm group) being dragged, -1 = none
static int      g_dragVcs  = -1;          // VC SHADOWS cabin-box slider being dragged, -1 = none
static int      g_dragVap  = -1;          // VAPOUR cone-1 slider being dragged, -1 = none
static int      g_dragVap2 = -1;          // VAPOUR cone-2 slider being dragged, -1 = none
static int      g_dragVapP = -1;          // VAPOUR apex knobs: -1 none; 1/2 = cone 1/2
                                          //   Position z (the original ids), 3/4 = cone 1
                                          //   x/y, 5/6 = cone 2 x/y (2026-08-30)
static int      g_dragVapR = -1;          // VAPOUR axis tilt: -1 none, 1/2 = cone 1
                                          //   pitch/yaw, 3/4 = cone 2 pitch/yaw
static int      g_dragVapBand = -1;       // cone-1 Mach-band handle: -1 none, 0 = MIN, 1 = MAX
static int      g_dragVapBand2 = -1;      // cone-2 Mach-band handle: -1 none, 0 = MIN, 1 = MAX
static int      g_dragTol = -1;           // PILOT G-tolerance slider being dragged, -1 = none
static int      g_dragCop = -1;           // FLIGHT AID CoP knob being dragged, -1 = none
static int      g_dragBar = -1;           // scrollbar thumb grab offset within the thumb, -1 = none
static DWORD    g_saveMsgUntil = 0;       // SAVE confirmation deadline (GetTickCount ms)
static char     g_noteBuf[128] = "";      // transient info line (COPY STOCK etc.) -
static DWORD    g_noteUntil = 0;          //   below save messages in priority, plain colour
// UNSAVED-EDITS TRACKING (2026-08-29, the visual pass - his yes to "the SAVE buttons
// lighting amber while unsaved edits exist"). A bitmask of ORO_SCOPE_* flags: an edit
// marks the scopes of the page it was made on (LeafSaveMask), a save/revert clears the
// scopes it wrote/re-read - which matches what saves actually DO, because a scope is a
// FILE and any save of that file commits every pending edit to keys in it.
// ⚠️ ONLY SETTING EDITS MARK (his refinement, same day: "only if a slider was changed,
// not if the test button was pressed"). The session-only controls - every TEST toggle,
// the STRIKE rig, Blink, the scenario buttons, CANCEL THRUST (never persisted, 23i) -
// raise g_clickWasEdit = false before returning, and the colour swatches defer their
// mark to the picker's OK (opening one is not yet an edit; cancelling reverts). The
// master arm never marks either: Ctrl+G outside the dialog could not mark, and
// ambering the panel for arming would be noise.
static int      g_dirtyScopes  = 0;
static bool     g_clickWasEdit = true;    // this click changes a SAVED value (default);
                                          //   session-only controls lower it
// Phase B: bell edits are GROUP-LEVEL even with a thruster selected (the bell is
// per group in v1), so they must not create a thruster override. The bell pill
// raises this; the bell sliders are identified by g_dragBgl and the tint picker
// by g_pickTarget inside MarkDirty. Reset with g_clickWasEdit at every click.
static bool     g_editGrpLevel = false;
static void     MarkDirty();              // defined after LeafSaveMask, used before it
static bool     g_saveOk = true;          // ... and what it should say
static int      g_saveMask = 0;           // ... and WHICH scopes it wrote (names the files)
static bool     g_saveWasRevert = false;  // ... and whether it was a REVERT rather than a SAVE

// PRESS FEEDBACK (2026-08-25). Clicking SAVE or REVERT writes or re-reads a file, and the
// only acknowledgement was one line of small text at the bottom of the panel. A control
// that does something irreversible-feeling and appears not to react is a control people
// press twice, so the button itself now lights up for a moment.
// ⚠️ NO TIMER OF ITS OWN: the panel already repaints on a ~10 Hz WM_TIMER (invariant 6),
// so the flash expires on its own within ~100 ms of its deadline, and the click handler
// already calls InvalidateRect, so it appears on the same frame as the press.
static DWORD    g_btnFlashUntil = 0;      // press-flash deadline (GetTickCount ms)
static int      g_btnFlashWhat  = 0;      // 1 = global SAVE, 2 = tab SAVE, 3 = tab REVERT
static const DWORD BTN_FLASH_MS = 220;    // ~2 repaint frames: clearly seen, not a state

static bool BtnFlash(int what)
{
	return g_btnFlashWhat == what && GetTickCount() < g_btnFlashUntil;
}
                                          // (same status line, opposite verb)
static int      g_lastSavedH = 0;         // client height last written to window.cfg, so a
                                          // window MOVE (which also ends in WM_EXITSIZEMOVE)
                                          // does not rewrite the file for nothing

static void ClearDrags()
{
	g_dragRow = g_dragMot = g_dragShake = g_dragEnv = g_dragEnvK = g_dragPlume = g_dragPlmBand
	          = g_dragBgl = g_dragPrt = g_dragPlas = g_dragEcl = g_dragAur = g_dragAurRib = g_dragAurK
	          = g_dragLtg = g_dragGry = g_dragRain = g_dragRlt = g_dragVcs = g_dragVap = g_dragVap2
	          = g_dragVapP = g_dragVapR = g_dragVapBand = g_dragVapBand2 = g_dragTol = g_dragCop = g_dragBar = -1;
}

// ----------------------------------------------------------------------------
// The effect row tables - THE assembly line. One entry per pill+slider row;
// paint and hit-testing loop over them, so adding an effect is one line here
// (plus its field in OroEffectState and its draw in the render callback).
// ----------------------------------------------------------------------------
// `value` is the effect value the renderer reads; `gain` is the per-effect multiplier
// the felt-G model applies to its own output. The SLIDER edits whichever of the two the
// current mode owns (LAB -> value, PHYSICS -> gain) and the readout always shows the
// value, so in physics mode you trim a gain while watching what the model produces.
// `driver` is what the felt-G model needs before this row can produce anything, in the
// same axis names the FELT G readout uses. IT EXISTS BECAUSE A GAIN TIMES ZERO IS ZERO:
// in PHYSICS mode the slider edits a multiplier on the model's output, so on an axis that
// is not firing the control moves, the number stays at 0, and the row is indistinguishable
// from a broken one. Both beta testers read it exactly that way ("aberration/swim/heartbeat
// missing in PHYSICS", "I can't change it in physics mode") - and all three were wired and
// working; aberration simply rides Gx, which a +Gz pull never touches. So while the model
// is producing nothing, the readout column names what it is waiting for instead of printing
// a zero. The row says "armed, wrong axis" rather than "dead".
struct FxRow {
	const char* label;
	bool*  enabled;
	float* value;
	float* gain;
	const char* driver;
};

// The slider's target: in PHYSICS mode the model owns `value`, so the knob edits the gain.
static float* RowKnob(const FxRow& r) { return g_fx.physicsMode ? r.gain : r.value; }

// VISION - the physiological suite (internal view).
// The drivers below are read straight off UpdatePhysics' step 5: everything the oxygen
// reserve gates says "+Gz" (that is the axis that empties it), red-out says "-Gz", and the
// two globe-deformation effects say "Gx". Blur is the one row with two mechanisms - it
// takes whichever of the Gx and +Gz terms is producing more - so it names both.
static FxRow g_visRows[] = {
	{ "Blackout",      &g_fx.blackoutEnabled, &g_fx.blackout,  &g_fx.gainBlackout,   "+Gz"   },
	{ "Red-out",       &g_fx.redoutEnabled,   &g_fx.redout,    &g_fx.gainRedout,     "-Gz"   },
	{ "Tunnel vision", &g_fx.tunnelEnabled,   &g_fx.tunnel,    &g_fx.gainTunnel,     "+Gz"   },
	{ "Dark spots",    &g_fx.spotsEnabled,    &g_fx.spots,     &g_fx.gainSpots,      "+Gz"   },
	{ "Grey-out",      &g_fx.greyoutEnabled,  &g_fx.greyout,   &g_fx.gainGreyout,    "+Gz"   },
	{ "Blur",          &g_fx.blurEnabled,     &g_fx.blur,      &g_fx.gainBlur,       "Gx+Gz" },
	{ "Heartbeat",     &g_fx.heartbeatEnabled,&g_fx.heartbeat, &g_fx.gainHeartbeat,  "+Gz"   },
	{ "Aberration",    &g_fx.aberrationEnabled,&g_fx.aberration,&g_fx.gainAberration,"Gx"    },
	{ "Sparkles",      &g_fx.sparklesEnabled, &g_fx.sparkles,  &g_fx.gainSparkles,   "+Gz"   },
	{ "Swim",          &g_fx.swimEnabled,     &g_fx.swim,      &g_fx.gainSwim,       "+Gz"   },
};
static const int NVIS = (int)(sizeof(g_visRows) / sizeof(g_visRows[0]));

// MOTION - whole-field movement. Tilt is a lab slider; the cam-shake below it is
// physics-driven (its sliders shape the LOOK, not the intensity).
static FxRow g_motRows[] = {
	// Two mechanisms again: the woozy SWAY rides the reserve (+Gz), the signed LEAN rides
	// the lateral load, and the same gain scales both.
	{ "Tilt / sway",   &g_fx.tiltEnabled,     &g_fx.tilt,      &g_fx.gainTilt,       "+Gz Gy" },
};
static const int NMOT = (int)(sizeof(g_motRows) / sizeof(g_motRows[0]));

// CAM-SHAKE subsection - live tuning knobs for the physics-driven camera shake.
// Each maps its track 0..1 to 0..vmax in the value's stored unit: X/Y/Z hold metres
// (vmax 0.010 = 10 mm, shown in mm x1000 with 0.1 precision), frequency holds Hz (0..10).
// The enable pill is g_fx.shakeEnabled (drawn in the subsection header).
// `raw` = show the stored value as-is (x1.00) rather than the mm/Hz conversions - the seat
// push is a plain gain, not a length.
struct ShakeRow { const char* label; float* value; float vmax; bool hz; bool raw = false; };
static ShakeRow g_shakeRows[] = {
	// The SEAT PUSH first, because it is the separate effect the other four are not: a
	// sustained lean opposite the felt acceleration, where the rest are a rattle. 0 = the
	// buffet alone, which is the split a beta tester asked for (see g_fx.shakePush).
	{ "Seat push",      &g_fx.shakePush, 2.0f,   false, true },
	{ "X range (mm)",   &g_fx.shakeAmpX, 0.010f, false },
	{ "Y range (mm)",   &g_fx.shakeAmpY, 0.010f, false },
	{ "Z range (mm)",   &g_fx.shakeAmpZ, 0.010f, false },
	{ "Frequency (Hz)", &g_fx.shakeFreq, 10.0f,  true  },
};
static const int NSHAKE = (int)(sizeof(g_shakeRows) / sizeof(g_shakeRows[0]));

// WORLD - environment effects (not physiological), so they render in EXTERNAL view,
// the inverse of everything above. Same pill+slider row style as VISION. This is
// where the plasma/reentry rework lands.
// (no gain column: the shimmer is a WORLD effect, outside the felt-G model, so its
// slider means the same thing in both modes - it points its gain at its own value.)
// (no driver column either: these never read zero-because-the-model-is-idle, so there is
//  nothing for the PHYSICS readout to explain.)
static FxRow g_envRows[] = {
	{ "Exhaust shimmer", &g_fx.shimmerEnabled, &g_fx.shimmer, &g_fx.shimmer, NULL },
	{ "Reentry plasma",  &g_fx.reentryEnabled, &g_fx.reentry, &g_fx.reentry, NULL },
	{ "Plume expansion", &g_fx.plumeEnabled,   &g_fx.plume,   &g_fx.plume,   NULL },
};
static const int NENV = (int)(sizeof(g_envRows) / sizeof(g_envRows[0]));

// WORLD tuning knobs - refine the row(s) above, so no pill of their own (same idea
// as the CAM-SHAKE sliders). These are BIPOLAR: the track maps 0..1 -> -vmax..+vmax with
// ZERO AT CENTRE, the fill is drawn from the centre outward and a tick marks zero.
struct EnvKnob { const char* label; float* value; float vmax; };
static EnvKnob g_envKnobs[] = {
	{ "Offset (m)", &g_fx.shimmerOfs, 1.0f },
};
static const int NENVK = (int)(sizeof(g_envKnobs) / sizeof(g_envKnobs[0]));

// PLASMA TUNING - LAB scaffolding (round 2.6.2): live multipliers wired straight
// into BuildPlasmaGeometry, so the reentry look iterates IN-SIM instead of per
// exit/rebuild/restart. Unit sliders 0..vmax like the CAM-SHAKE rows. When a value
// settles it gets BAKED into the OroReentry.cpp constants and its knob returns
// to 1.0 = neutral.
struct PlasRow { const char* label; float* value; float vmax; int dec; float vmin = 0.0f; };
                                                        // vmin: almost every row is 0..vmax;
                                                        // Trail start is the one BIPOLAR row
                                                        // (-5..+5, negative = upstream into
                                                        // the fireball - A.5)
static PlasRow g_plasRows[] = {
	{ "Saturation",     &g_fx.plasSat,         2.0f, 2 },   // palette: 1 = reference,
	                                                        // low = creamy, high = magenta
	{ "Hull light",     &g_fx.plasLight,       2.0f, 2 },   // the stagnation light: lights
	                                                        // the MESH, not our geometry
	{ "VC glow",        &g_fx.plasVCGlow,      3.0f, 2 },   // the COCKPIT sheath (screen-
	                                                        // space; the external draw list
	                                                        // starves from inside the hull).
	                                                        // 0 = off.
	{ "Cabin wash",     &g_fx.plasCabin,       1.0f, 2 },   // where the cockpit glow LANDS:
	                                                        // 0 = a directional pool that
	                                                        // follows the plasma off screen,
	                                                        // 1 = a flat glow that survives
	                                                        // looking at the instruments
	{ "Streak length",  &g_fx.plasStreakLen,  20.0f, 2 },   // x3'd, then raised to 20 when
	                                                        // the trail was abandoned
	{ "Streak width",   &g_fx.plasStreakWid,   6.0f, 2 },   // range x2'd on request
	{ "Streak wander",  &g_fx.plasWander,      3.0f, 2 },
	{ "Wake churn",     &g_fx.plasChurn,       3.0f, 2 },   // 2026-08-15: how FAST the wake
	                                                        // lives. 1 = the new baseline,
	                                                        // 0 freezes it (the Soot churn
	                                                        // idiom). See plasChurn.
	{ "Fin rake (deg)", &g_fx.plasFinRake,    45.0f, 0 },   // ... and how far the fins
	                                                        // splay off the flow axis.
	                                                        // 0 = straight downstream.
	{ "Sparks",         &g_fx.plasSpark,       6.0f, 2 },   // count multiplier
	{ "Spark life (s)", &g_fx.plasSparkLife,   3.0f, 2 },   // root->tip travel time
	{ "Spark size",     &g_fx.plasSparkSize,   4.0f, 2 },   // radius multiplier
	{ "Edge light",     &g_fx.plasComa,        2.0f, 2 },   // round 3: the old coma
	                                                        // slot drives the edge
	{ "Shock bright",   &g_fx.plasShockBright, 3.0f, 2 },   // the detached belly sheet
	{ "Shell dist",     &g_fx.plasShellDist,   0.08f, 3 },  // shell standoff - back by
	                                                        // request after the Atlantis
	                                                        // sank the baked 0.015
	{ "Bowl dist",      &g_fx.plasShockDist,   0.30f, 2 },  // envelope standoff scale;
	                                                        // 0.10 = the automatic law
	                                                        // (shell standoff is BAKED
	                                                        // at 0.015 since 2026-08-08)
	{ "Bowl size X",    &g_fx.plasBowlSX,      2.0f, 2 },   // envelope sculpting, vessel
	{ "Bowl size Y",    &g_fx.plasBowlSY,      2.0f, 2 },   //   axes, 1 = neutral (user
	{ "Bowl size Z",    &g_fx.plasBowlSZ,      2.0f, 2 },   //   request 2026-08-08)
	// THE TRAIL IS BACK (2026-08-08, take 2) - as a particle pool this time, not the
	// knot ring G10 buried. Density 0 = off (the section's usual idiom, like edge light).
	{ "Trail density",  &g_fx.plasTrail,       2.0f, 2 },   // scales 1/spacing of the sheds
	{ "Trail life (s)", &g_fx.plasTrailLife,  12.0f, 1 },   // SIM-time lifetime = the LENGTH
	                                                        //   lever (6 s ~ 45 km at entry
	                                                        //   speed - "tens of km")
	{ "Trail width",    &g_fx.plasTrailWid,    8.0f, 2 },   // ribbon width (range x2'd
	                                                        //   on request, A.2)
	{ "Trail start",    &g_fx.plasTrailStart,  5.0f, 2, -5.0f },  // root position, x hull
	                                                        //   size: + = behind (soft
	                                                        //   emergence), - = UPSTREAM
	                                                        //   into the fireball (A.5)
};
static const int NPLAS = (int)(sizeof(g_plasRows) / sizeof(g_plasRows[0]));

// PLUME EXPANSION (2026-08-09) - the THRUSTER tab's shape knobs. The REGIME is
// automatic (static pressure decides diamonds vs bloom - see OroPlume.cpp); these
// shape each end of it. Same PlasRow unit-slider shape; per vessel class like the
// shimmer. The master strength is g_envRows[2] (pill + slider above these).
static PlasRow g_plumeRows[] = {
	{ "Width",          &g_fx.plumeWidth,    3.0f, 2 },  // the silhouette knobs, first:
	{ "Length",         &g_fx.plumeLen,      3.0f, 2 },  //   ours replaces stock, so the
	                                                     //   overlay owns the jet's shape
	                                                     //   (per hull, like Shell dist)
	{ "Diamonds",       &g_fx.plumeCells,   12.0f, 0, 1.0f },  // COUNT of discs (1..12,
	                                                     //   integer readout; spacing
	                                                     //   moves them, this adds them)
	{ "Diamond bright", &g_fx.plumeDiamond,  2.0f, 2 },  // Mach-disc peak brightness
	{ "Diamond spacing",&g_fx.plumeSpacing,  3.0f, 2 },  // shock-cell length, x2 nozzle
	                                                     //   widths at 1.0
	{ "Bloom width",    &g_fx.plumeBloomWid, 2.0f, 2 },  // vacuum opening half-angle
	{ "Bloom bright",   &g_fx.plumeBloomBri, 2.0f, 2 },  // vacuum halo brightness
	{ "Throat glow",    &g_fx.plumeThroat,   4.0f, 2 },  // the fire inside the bell cup
	{ "Throat offset",  &g_fx.plumeThroatOfs,1.0f, 2 },  // [m] slide the fire out of
	                                                     //   the bell, downstream
	{ "Soot streaks",   &g_fx.plumeSoot,     2.0f, 2 },  // ablative wisps; 0 = off (the
	                                                     //   slider IS the toggle - the
	                                                     //   aurora's opt-in law)
	{ "Soot churn",     &g_fx.plumeSootRate, 3.0f, 2 },  // lifecycle speed: 0 = frozen
	                                                     //   pattern, 1 = Merlin cadence,
	                                                     //   3 = frantic shedding
};
static const int NPLM = (int)(sizeof(g_plumeRows) / sizeof(g_plumeRows[0]));

// EXHAUST PARTICLES (the THRUSTER tab's PARTICLES sub-tab, 2026-08-09). These are
// PARTICLESTREAMSPEC's own fields in PARTICLESTREAMSPEC's own units - not
// normalised multipliers - because the whole point is to hand the user the
// controls a vessel author has in code. Ranges are wide enough to cover a DG
// hover nozzle and a Shuttle SRB; the values are saved per class, which is what
// makes absolute units workable across wildly different hulls.
// vmin is used for the one signed row (Offset), exactly as the trail-start row
// uses it on the reentry tab.
static PlasRow g_prtRows[] = {
	{ "Offset (m)",     &g_fx.prtOffset,   15.0f, 1, -5.0f }, // emission point along the flow
	{ "Size (m)",       &g_fx.prtSize,     20.0f, 2,  0.1f }, // srcsize - ONE radius; there is
	                                                          //   no width/length in the API
	{ "Lifetime (s)",   &g_fx.prtLifetime, 10.0f, 2,  0.05f },
	{ "Rate (Hz)",      &g_fx.prtRate,    100.0f, 0,  1.0f }, // srcrate
	{ "Speed (m/s)",    &g_fx.prtSpeed,   400.0f, 0,  0.0f }, // v0
	{ "Spread",         &g_fx.prtSpread,    1.0f, 2,  0.0f }, // srcspread
	{ "Growth (m/s)",   &g_fx.prtGrowth,   30.0f, 1,  0.0f }, // growthrate
	{ "Atm slowdown",   &g_fx.prtSlowdown,  5.0f, 2,  0.0f },
};

// VC SHADOWS: the cabin-box half-width handed to the client. Range starts above zero
// because a degenerate ortho box is not a look, it is a bug; the top end is roughly a
// large vessel's whole forward section.
// ⚠️ THE FLOOR WAS 1.0 m AND IS 0.4 m SINCE 2026-08-15. Both beta testers independently
// found that smaller is sharper and asked for less than a metre, which is exactly what the
// 2026-08-04 texel-density analysis predicts: the map is a fixed 2048 (or 4096) texels
// across the box, so halving the box doubles the resolution on a panel forty centimetres
// from the eye. The STRUCTURAL trade is unchanged and is why this is a slider rather than a
// baked constant - the small box that makes edges sharp is exactly what stops a DOCKED
// vessel casting into your cockpit, and you cannot have both without a second map.
static const float VCS_RAD_MIN = 0.4f, VCS_RAD_MAX = 12.0f;

// ECLIPSE - the three things the observer does about it. Same unit-slider shape as
// the two tables above (PlasRow is just label/value/vmax/decimals, nothing plasma
// about it), enable pill on the section header like CAM-SHAKE. These are NOT lab
// scaffolding to be baked away later: Dim and Colour loss are taste, and Adaptation
// is how much of the physiology you want to feel - all three are the user's to keep.
static PlasRow g_eclRows[] = {
	{ "Dim",             &g_fx.eclipseDim,    1.0f, 2 },  // steady darkening while obscured
	{ "Eye adaptation",  &g_fx.eclipseAdapt,  1.0f, 2 },  // blindness in / glare out
	{ "Colour loss",     &g_fx.eclipseColour, 1.0f, 2 },  // scotopic, cool grey
};
static const int NECL = (int)(sizeof(g_eclRows) / sizeof(g_eclRows[0]));

// AURORA - the curtain look, four LIVE knobs (like the plasma tuning, wired straight
// into UpdateAurora, but these are the user's taste to KEEP, not lab scaffolding to
// bake away - the same standing as the eclipse's three). Same unit-slider shape.
static PlasRow g_aurRows[] = {
	{ "Activity",  &g_fx.auroraActivity, 1.0f, 2 },  // overall brightness/extent
	{ "Oval lat",  &g_fx.auroraReach,    1.0f, 2 },  // equatorward reach - shows the resulting
	                                                 // latitude in degrees (special-cased below)
	{ "Fold",      &g_fx.auroraFold,     1.0f, 2 },  // drapery: how far it waves
	{ "Rays",      &g_fx.auroraRays,     1.0f, 2 },  // vertical striation contrast
	{ "Breakup",   &g_fx.auroraBreakup,  1.0f, 2 },  // break the ring into disconnected bands
	{ "Thickness", &g_fx.auroraThick,    1.0f, 2 },  // 1..4 stacked sheets = an emissive VOLUME
	{ "Base (km)", &g_fx.auroraBase,     1.0f, 2 },  // base altitude - lower it to hug the limb
	{ "Top (km)",  &g_fx.auroraHeight,   1.0f, 2 },  // top altitude (both shown in km, below)
};
static const int NAUR = (int)(sizeof(g_aurRows) / sizeof(g_aurRows[0]));

// AURORA magnetic-pole offset - BIPOLAR (zero at centre = the geographic pole), because
// the SIGN is the whole point: which way the oval leans. Same row kind as the shimmer's
// Offset knob. Per body, like every other aurora number - Earth's real value is ~11 deg.
struct AurKnob { const char* label; float* value; float vmax; };
static AurKnob g_aurKnobs[] = {
	{ "Tilt X (deg)", &g_fx.auroraTiltX, 90.0f },   // toward the prime meridian
	{ "Tilt Y (deg)", &g_fx.auroraTiltY, 90.0f },   // toward 90 deg east of it
};
static const int NAURK = (int)(sizeof(g_aurKnobs) / sizeof(g_aurKnobs[0]));

// LIGHTNING - storms in the cloud deck, seen from above. Same unit-slider shape;
// all four are PER BODY (activity is the opt-in, invariant 17b), the pill is global.
static PlasRow g_ltgRows[] = {
	{ "Activity",   &g_fx.ltgActivity, 1.0f, 2 },  // how much of the world is storming
	{ "Brightness", &g_fx.ltgBright,   1.0f, 2 },  // flash intensity
	{ "Flash rate", &g_fx.ltgRate,     1.0f, 2 },  // per-cell cadence
	{ "Cell size",  &g_fx.ltgCellKm,   1.0f, 2 },  // glow radius - readout shows km
};
static const int NLTG = (int)(sizeof(g_ltgRows) / sizeof(g_ltgRows[0]));

// GOD RAYS - crepuscular shafts from the sun. Same unit-slider shape. GLOBAL scope
// (like the eclipse): these are the pilot's taste, and the physical difference between
// a thick atmosphere and a thin one is already handled by the density gate in
// OroGodRays.cpp - so there is nothing here for a per-body file to say.
static PlasRow g_gryRows[] = {
	{ "Strength",   &g_fx.grayStrength, 1.0f, 2 },  // master intensity of the shafts
	{ "Reach",      &g_fx.grayLength,   1.0f, 2 },  // how far the shafts extend from the disc
	{ "Softness",   &g_fx.grayDecay,    1.0f, 2 },  // crisp short rays -> long soft ones
	{ "Sensitivity",&g_fx.graySens,     1.0f, 2 },  // how DIM a thing may be and still cast:
	                                                //   THE knob that separates "shafts" from
	                                                //   "radial blur over the whole sky". Reads
	                                                //   upward like every other slider here -
	                                                //   it was a backwards "Threshold" for one
	                                                //   round and that was a real usability bug
	{ "Warmth",     &g_fx.grayWarm,     1.0f, 2 },  // reddening as the sun nears the horizon
};
static const int NGRY = (int)(sizeof(g_gryRows) / sizeof(g_gryRows[0]));

// RAIN (2026-08-20) - the runway slice. GLOBAL scope for v1 because there is only one
// world it can happen on; when other bodies arrive it becomes a line in the per-body cfg
// beside the aurora's, not a code change. Note there is no INTENSITY slider: the storm's
// strength is the EVENT's, and the event is what the pill and Test drive. These four are
// look knobs on top of it - his rule from the vapour cone, that the sim owns what happens
// and the user owns how it looks.
// ⚠️ ROW ORDER IS THE PAGE'S GROUPING (2026-08-29, the menu rework - his spec: "all
// the sliders that affect the exterior look go together, then a small gap with a
// header and then the interior sliders, then the sounds"). The RAIN page draws these
// as three captioned groups - THE STORM OUTSIDE / THE WINDSCREEN (VC) / SOUNDS -
// split at RAIN_EXT_N and RAIN_EXT_N + RAIN_INT_N, so a row's INDEX decides its
// group and RainRowY() opens the gaps. The three storm-lightning rows (Lightning /
// Bolt bloom / Thunder) moved to the LIGHTNING page's in-the-storm group
// (g_rltRows below): same g_fx fields, same save keys, different address.
static PlasRow g_rainRows[] = {
	// --- group 0: THE STORM OUTSIDE (visible from anywhere, VC included) --------
	{ "Gloom",      &g_fx.rainGloom,   2.0f, 2 },  // how grey and dark the world goes
	{ "Cloud detail",&g_fx.rainCloudLvl,3.0f, 0 },  // deck texture notch: 0 = plain
	                                               // gloom, 1/2/3 = 256/512/1024 texture
	{ "Density",    &g_fx.rainDensity, 2.0f, 2 },  // streaks in the sheet
	{ "Fall speed", &g_fx.rainSpeed,   2.0f, 2 },
	{ "Streak length",&g_fx.rainStreak, 2.0f, 2 },
	{ "Streak glow",&g_fx.rainStreakA, 2.0f, 2 },  // how brightly they catch the light
	{ "Slant (deg)",&g_fx.rainAngle,  15.0f, 0, -15.0f },  // BIPOLAR - the wind
	{ "Splashes",   &g_fx.rainPuddle,  2.0f, 2 },  // rings where drops land; 0 = off
	{ "Wet dark",   &g_fx.rainWetDark, 2.0f, 2 },  // how far the wet ground darkens
	                                               // (client patch s part 3); 1 = designed
	{ "Pool size",  &g_fx.rainPoolSize, 2.0f, 2 }, // standing-pool lattice scale; 1 = designed
	{ "Pool reach", &g_fx.rainPoolReach,2.0f, 2 }, // how far out pools stay visible;
	                                               // 1 = ~900 m e-fold, 0 = nearby only
	{ "Grain",      &g_fx.rainGrainOp,  2.0f, 2 }, // broken-water texture in the pool
	                                               // reflections; 0 = uniform pools
	{ "Grain size", &g_fx.rainGrainSize,2.0f, 2 }, // grain feature size; 1 = designed
	{ "Glint",      &g_fx.rainGlint,   2.0f, 2 },  // drop sparkle on hulls; 0 = off
	{ "Reflection", &g_fx.rainRefl,    2.0f, 2 },  // vessel image in the puddles; 0 = off
	{ "Reflection blur",&g_fx.rainReflBlur, 2.0f, 2 }, // how DIFFUSE that image is; 0 = a
	                                               // crisp mirror. Directly under
	                                               // Reflection because the two shape the
	                                               // SAME image - his call, 2026-08-24.
	{ "Swim size",  &g_fx.rainSwimAmp, 2.0f, 2 },  // ripple-warp amplitude on the image;
	                                               // 1 = designed, 0 = still mirror
	{ "Swim rate",  &g_fx.rainSwimRate,2.0f, 2 },  // ripple cadence; 1 = designed.
	                                               // FINDING sliders (the origin-tilt
	                                               // pattern): bake + delete when settled
	// --- group 1: THE WINDSCREEN (2026-08-26, client patch h) - VC only ---------
	// The drop rows sit together because they shape one object: how many drops, how
	// big, and how hard each one bends what is behind it. Lens at 0 leaves a drop
	// that only glistens, which is a useful A/B: it is exactly the look the
	// no-client-patch substrate could have given.
	{ "Glass drops",&g_fx.rainGlass,   2.0f, 2 },  // coverage on the canopy; 0 = clean glass
	{ "Drop size",  &g_fx.rainGlassSize,3.0f, 2 }, // angular, so it holds across viewports;
	                                               // 0 = no drops at all (his spec 08-26)
	{ "Drop lens",  &g_fx.rainGlassLens,2.0f, 2 }, // refraction; past ~1 the image inverts
	{ "Build up (s)",&g_fx.rainGlassRise,60.0f, 0, 10.0f }, // seconds from clean canopy to
	                                               // the Glass drops target at full storm
	{ "Runners",    &g_fx.rainGlassRunners,3.0f, 2 }, // loose drops carving wet trails
	                                               // down-run; speed follows the physics
	                                               // (0..3 since 2026-08-27, his spec)
	{ "Runner size",&g_fx.rainGlassRunSize,2.0f, 2, 0.4f }, // head/track thickness vs the
	                                               // drops - a RATIO; Drop size still
	                                               // scales both families together
	{ "Drop debug", &g_fx.rainGlassDbg, 2.0f, 0 }, // TEMPORARY scaffold - 0 normal,
	                                               // 1 = ignore the depth mask,
	                                               // 2 = show the mask (green=window,
	                                               //     blue=interior, plain=nothing)
	// (the Rain view cycler rides this group too, drawn after the last row)
	// --- group 2: SOUNDS --------------------------------------------------------
	{ "Rain sound", &g_fx.rainSoundVol,2.0f, 2 },  // the three generated loops crossfading
	                                               // with the envelope; 0 = silent (the
	                                               // opt-out - no pill, 17b's law)
	{ "Hull drum",  &g_fx.rainHullVol, 2.0f, 2 },  // the fourth loop - drops on the skin,
	                                               // INTERIOR ONLY; its own volume since
	                                               // 2026-08-25 (a tester's ask)
};
static const int NRAIN = (int)(sizeof(g_rainRows) / sizeof(g_rainRows[0]));
static const int RAIN_EXT_N = 18;   // rows in THE STORM OUTSIDE
static const int RAIN_INT_N = 7;    // rows in THE WINDSCREEN (the rest are SOUNDS)

// STORM LIGHTNING - the RAIN system's own flashes/bolts/thunder, shown on the
// LIGHTNING page beside the orbital system so the panel's two lightnings finally
// live under one roof with honest headers (the 2026-08-25 tester confusion, fixed
// structurally instead of by cross-referencing captions). The values are RAIN
// fields with GLOBAL save keys; only their address in the panel moved.
static PlasRow g_rltRows[] = {
	{ "Lightning",  &g_fx.rainLtg,     2.0f, 2 },  // flash rate: 0 = none, 2 = very
	                                               // often; single flashes + strobes
	{ "Bolt bloom", &g_fx.rainBoltBloom,2.0f, 2 }, // glow-pass intensity around bolts;
	                                               // 0 = crisp filament only
	{ "Thunder",    &g_fx.rainThunder, 2.0f, 2 },  // the sourced one-shot set, delayed by
	                                               // each flash's own distance; 0 = silent
};
static const int NRLT = (int)(sizeof(g_rltRows) / sizeof(g_rltRows[0]));

// THE VAPOUR CONE - transonic condensation. TWO unit sliders and one bipolar knob, and
// the short list is the design rather than an omission: the shroud's LENGTH is not here
// because it is derived from the Mach angle (invariant 25b). Give it a slider and the
// cone stops being a speed cue and becomes a decal.
// PER VESSEL CLASS: where the flow first goes supersonic and how far the shroud stands
// off are facts about a nose, not about a pilot - the shell-standoff lesson, which cost
// a day when the DG's number sank the Atlantis.
static PlasRow g_vapRows[] = {
	{ "Opacity",      &g_fx.vapStrength, 2.0f, 2 },   // 0 = off; past 1 the sheet FILLS
	                                                  // until it can hide the hull behind
	                                                  // it (his reference photos, 08-29)
	{ "Size x",       &g_fx.vapSize,     3.0f, 2 },   // wing-line radius, hull sizes -
	                                                  // the master dimension
	{ "Size y",       &g_fx.vapSizeY,    3.0f, 2 },   // vertical radius, hull sizes -
	                                                  // SAME units as x, so equal slider
	                                                  // values = a circular cone (his rule)
	{ "Size z",       &g_fx.vapSizeZ,    2.0f, 2 },   // length, RATIO of the Mach-angle
	                                                  // reach (1 = physics; 0 = flat disc)
	{ "Streaks",      &g_fx.vapStreaks,  2.0f, 2 },   // COUNT of slim darker filaments
	                                                  // (0..32); 0 = the clean sheet
	{ "Streak churn", &g_fx.vapStreakChurn, 2.0f, 2 },// how violently they move;
	                                                  // 0 = frozen (the Soot churn law)
	{ "Flicker (Hz)", &g_fx.vapFlickHz,  8.0f, 2 },   // breathing rate; 0 = frozen
};
static const int NVAP = (int)(sizeof(g_vapRows) / sizeof(g_vapRows[0]));

// THE SECOND CONE (2026-08-29, his spec): the identical row set bound to the cone-2
// fields - "completely separate tuning. Even the colors". No pill of its own: the one
// VAPOUR CONES pill arms the effect, each cone's Opacity is its own visibility, and
// cone 2 ships at 0 so it exists only where a hull is given one.
static PlasRow g_vapRows2[] = {
	{ "Opacity",      &g_fx.vapStrength2, 2.0f, 2 },
	{ "Size x",       &g_fx.vapSize2,     3.0f, 2 },
	{ "Size y",       &g_fx.vapSizeY2,    3.0f, 2 },
	{ "Size z",       &g_fx.vapSizeZ2,    2.0f, 2 },
	{ "Streaks",      &g_fx.vapStreaks2,  2.0f, 2 },
	{ "Streak churn", &g_fx.vapStreakChurn2, 2.0f, 2 },
	{ "Flicker (Hz)", &g_fx.vapFlickHz2,  8.0f, 2 },
};
// (deliberately reuses NVAP: the two tables are identical by construction, and a row
//  added to one without the other should fail to line up loudly, not silently)

// Apex station along the flow axis, in hull sizes. Bipolar for the same reason the CoP
// shift and the trail start are: the useful neutral is a place you can hit rather than a
// number you have to land on. Position x/y (2026-08-30) share the same range and units;
// Pitch/Yaw are the axis tilt, degrees, his spec: -30..+30, default 0 (the middle).
static const float VAP_POS_MAX = 2.0f;
static const float VAP_ROT_MAX = 30.0f;

// THE MACH BAND dual slider (his design, round 3). Track fraction <-> Mach over
// [0.5 .. 1.5]: the handles are where the shroud starts and stops existing, so the window
// can be tightened to a flash or loosened to a long transonic haze. Minimum gap keeps the
// two ramps (30% of the window each, inside it - invariant 23b) from degenerating.
static const float VAPB_MLO = 0.5f;
static const float VAPB_MHI = 1.5f;
static const float VAPB_GAP = 0.05f;   // Mach
// One drag rule, two bands (cone 2 since 2026-08-29): which handle moves and whose
// min/max pair it edits are parameters, the clamping law is shared.
static void VapBandDragC(float f, int handle, float& mMin, float& mMax)
{
	const float m = VAPB_MLO + f * (VAPB_MHI - VAPB_MLO);
	if (handle == 0) mMin = min(max(m, VAPB_MLO), mMax - VAPB_GAP);
	else             mMax = max(min(m, VAPB_MHI), mMin + VAPB_GAP);
}
static void VapBandDrag(float f)
{
	VapBandDragC(f, g_dragVapBand, g_fx.vapMachMin, g_fx.vapMachMax);
}

// FLIGHT AID: the CoP shift range. +-1 m covers every stock vessel we have measured
// (the DG's whole wing CoP arm is 0.3 m, Atlantis's is a few metres of airframe), and
// a bipolar knob means the neutral position is a snap-to-zero at the centre, not a
// number you have to hit.
static const float COP_MAX = 1.0f;

// Scenario buttons - one-click scripted sequences. Labels only; the timelines live in
// OroModule.cpp INDUCE_SEQ[]. Order MUST match: induce 0=G-LOC,1=Grey-out,2=Red-out,
// then recover 3=G-LOC,4=Grey-out,5=Red-out. Recover buttons map to scenario index NIND + i.
static const char* g_indNames[] = { "G-LOC", "Grey-out", "Red-out" };
static const int NIND = (int)(sizeof(g_indNames) / sizeof(g_indNames[0]));
static const char* g_recNames[] = { "G-LOC", "Grey-out", "Red-out" };
static const int NREC = (int)(sizeof(g_recNames) / sizeof(g_recNames[0]));

// ----------------------------------------------------------------------------
// Layout (all pixels). Header and status are FIXED; everything between them
// lives in the scrolling pane and is addressed in DOCUMENT coordinates (which
// equal client coordinates when g_scroll == 0).
// ----------------------------------------------------------------------------
static const int DLG_W      = 525;        // client width, LOCKED (px) - see DLG_H_MIN
                                          // ⚠️ 500 -> 525 on 2026-08-25 (+5%, his call).
                                          // The extra 25 px goes ENTIRELY to the value
                                          // column: TRACK_RPAD absorbs it so every slider
                                          // keeps its exact position and length. If DLG_W
                                          // ever moves again, move TRACK_RPAD with it.
static const int DLG_H      = 800;        // DEFAULT client height (px; 600 -> 800 on
                                          // 2026-08-02 - the pane earns its keep now
                                          // that PLASMA TUNING is a section of its own).
                                          // ⚠️ SINCE 2026-08-16 THIS IS ONLY THE DEFAULT:
                                          // the panel is vertically resizable and opens at
                                          // whatever height it was last left (stored in
                                          // Config\ORO\window.cfg). Nothing but the initial
                                          // sizing reads it - every layout helper already
                                          // derived from rc.bottom, which is what made the
                                          // whole change cheap.
static const int DLG_H_MIN  = 500;        // smallest useful client height. Below this the
                                          // ~227 px of fixed chrome (banner + arm strip +
                                          // nav row + status line; +26 more on the two
                                          // thruster pages for the group row) leaves under
                                          // 300 px of pane and the panel stops being a panel.
// WIDTH IS DELIBERATELY NOT RESIZABLE (by the USER - it is a fixed number here, and it did
// move once). The slider column was narrowed to 500 on his own call in 2026-07-30 ("no need
// to have such wide sliders"), and that judgement still stands: the widening did not make
// the sliders wider, it gave the readouts beside them room to be legible. Height remains the
// axis with the actual problem - the tallest tab is over 1000 px of document against a
// 581 px pane - so height is still the axis that opens.
// ⚠️ BANNER_H NO LONGER FOLLOWS THE ASPECT RATIO, AND THAT IS DELIBERATE. It was literally
// 500 * (130/558). Holding the ratio at 525 would mean 122 px, i.e. six more pixels of
// artwork stealing six from every tab's pane. His call was to stretch the banner instead:
// "Don't worry about the banner. We can stretch its length by 5%." So the height stays put
// and StretchBlt widens the bitmap 5% - the artwork is a wordmark on a trace graphic, which
// takes a 5% horizontal stretch without reading as distorted.
static const int BANNER_H   = 116;        // FIXED - the banner.bmp is stretched to DLG_W
static const int ARMED_H    = 40;         // master ARMED/ENABLED strip (below banner)
static const int STATUS_H   = 34;         // fixed status line at the bottom
static const int PANE_Y     = BANNER_H + ARMED_H + 1;   // top of the scrolling pane
static const int ROW_DY     = 24;         // row pitch (was 28; tightened for the 500 px panel)
static const int PILL_X     = 14, PILL_W = 26, PILL_H = 14;
static const int LABEL_X    = 46;         // row label left edge (50 -> 46 on 2026-08-25:
                                          // "you can move the start of the slider names a
                                          // few pixels to the left too". The pill ends at
                                          // PILL_X + PILL_W = 40, so 46 keeps a 6 px gap.)
static const int TRACK_X    = 158;        // slider track left edge - UNCHANGED by the 2026-
                                          // 08-25 widening, on purpose: "keep the sliders
                                          // the same size and position".
static const int TRACK_RPAD = 99;         // gap between track right edge and client right.
                                          // ⚠️ THIS IS WHAT PINS THE TRACK. At DLG_W 500 it
                                          // was 74 and the track ended at 426; at 525 it is
                                          // 99 and the track still ends at 426. The two
                                          // constants move together or the sliders resize.
static const int TRACK_H    = 8;
static const int SB_W       = 6;          // scrollbar thumb width
static const int SB_RPAD    = 5;          // gap from the scrollbar to the client right edge
static const int SEC_RPAD   = 26;         // right margin for section rules / captions

// The NAV ROW is a FIXED strip between the ARMED strip and the scrolling content
// (it replaced the tab bar, 2026-08-29): breadcrumb on the left, BACK + BACK TO
// MAIN on the right. Only the current page's content is ever painted, so all the
// document chains below coexist without interfering - each LEAF re-anchors at
// LeafTopY(), each MENU page at MenuBtnY(0).
static const int NAV_H    = 36;
// The engine-group row (EXHAUST/PARTICLES pages only) is a SECOND fixed strip:
// the cycler changes what every control below it MEANS, so it may not scroll
// out of reach. Content on those two pages starts a row lower - and everything
// derives from ContentY(), so the shift propagates through both chains free.
// Phase B grew it to TWO lines (26 -> 42): line 1 = group cycler + thruster
// cycler + MARK + CLEAR, line 2 = the selection's state ("inherits MAIN" /
// "OVERRIDE - CLEAR returns it"), which must stay visible at any scroll for the
// same reason the cycler must.
static const int THRGRP_H = 42;
// Each LEAF carries its own SAVE/REVERT row - the buttons plus a line naming the
// SCOPES they touch. ⚠️ FIXED since the visual pass (2026-08-29, his call): it sat
// at the top of the leaf's document and scrolled away with it, and "if scroll is
// needed to reach some sliders, the save/revert buttons are always visible" is the
// spec. So it is a third fixed strip, below the nav row (and the group row where
// one exists); menu pages have nothing to save and skip it.
static const int LEAFSAVE_H = 34;
static int ContentY()
{
	int y = PANE_Y + NAV_H;
	if (PageHasGrpRow(CurPage())) y += THRGRP_H;
	if (!IsMenuPage(CurPage()))   y += LEAFSAVE_H;
	return y;
}
static int LeafSaveY()                                    // FIXED save row centreline
{
	int y = PANE_Y + NAV_H;
	if (PageHasGrpRow(CurPage())) y += THRGRP_H;
	return y + LEAFSAVE_H / 2;
}
static int LeafTopY()  { return ContentY() + 10; }        // where each leaf's sections start

static int PaneBottom(const RECT& rc) { return rc.bottom - STATUS_H; }
static int PaneHeight(const RECT& rc) { return PaneBottom(rc) - ContentY(); }

// MENU pages: a column of big buttons in DOCUMENT coordinates - they scroll like
// any other content if the panel is shorter than the list, so the no-dead-space
// law cannot recur from the other direction (a menu taller than the pane).
// Metrics from the visual pass (2026-08-29, his numbers): 400 x 100, 40 px gaps.
// A fixed WIDTH centred in the client rather than symmetric margins, so the
// number he gave is the number the button is.
static const int MBTN_W = 400, MBTN_H = 100, MBTN_GAP = 40;
static int  MenuBtnY(int i) { return ContentY() + 18 + i * (MBTN_H + MBTN_GAP); }
static RECT MenuBtnRect(const RECT& rc, int i)
{
	const int x0 = (rc.right - MBTN_W) / 2;
	RECT r = { x0, MenuBtnY(i), x0 + MBTN_W, MenuBtnY(i) + MBTN_H };
	return r;
}
static int MenuBottom(int pg)
{
	int n; MenuOf(pg, n);
	return MenuBtnY(n - 1) + MBTN_H + 18;
}

// ----------------------------------------------------------------------------
// Document layout, PER TAB. Each helper is defined in terms of the one above it,
// so inserting a row shifts everything below it automatically and paint and
// hit-testing can never disagree. Sections are grouped by the TAB they live in.
// ----------------------------------------------------------------------------

// ===== LEAF: G-FORCES (PILOT) - vision + motion(tilt) + the felt-G model =====
// VISION
static int VisHdrY()      { return LeafTopY(); }                            // caption text top
static int VisRowY(int i) { return VisHdrY() + 34 + i * ROW_DY; }           // row centreline
static int BlinkCY()      { return VisRowY(NVIS - 1) + ROW_DY + 2; }
// MOTION - just the tilt sway here; the physics-driven cam-shake lives in the VC tab.
static int MotHdrY()      { return BlinkCY() + 26; }
static int MotRowY(int i) { return MotHdrY() + 34 + i * ROW_DY; }
// PILOT - the felt-G model's controls, then its live readout.
// Control rows: 0 mode, 1 tolerance, 2 G-suit, 3 posture, 4 G-reference.
// Readout rows: Gz, Gx, Gy, reserve - no controls, just numbers.
static int PilHdrY()      { return MotRowY(NMOT - 1) + 30; }               // caption text top
static int PilRowY(int i) { return PilHdrY() + 34 + i * ROW_DY; }
static const int NPILROW  = 7;   // mode / save target / tolerance / suit / posture /
                                 // G reference / effects view
static int PilReadCapY()  { return PilRowY(NPILROW - 1) + 16; }            // "F E L T   G" text top
static int PilReadY(int i){ return PilReadCapY() + 24 + i * ROW_DY; }
static const int NPILREAD = 4;
static int GforceBottom() { return PilReadY(NPILREAD - 1) + 24; }

// ===== LEAF: SCENARIOS (PILOT) - its own page since the menu rework ==========
static int ScenHdrY()     { return LeafTopY(); }                           // caption text top
static int IndCapY()      { return ScenHdrY() + 34; }
static int IndCY()        { return IndCapY() + 26; }                       // induce button centreline
static int RecCapY()      { return IndCY() + 30; }
static int RecCY()        { return RecCapY() + 26; }                       // recover button centreline
static int ScenariosBottom() { return RecCY() + 26; }

// ===== LEAF: EXHAUST (VESSEL > THRUSTERS) ==================================
// The old EXHAUST | PARTICLES sub-tabs became sibling MENU entries under
// THRUSTERS (2026-08-29) - the whole sub-tab strip died into the navigation,
// one less control kind. EXHAUST is everything ORO DRAWS (shimmer, the plume
// overlay, the bell); PARTICLES is the core's own particle streams, which ORO
// only configures. Each chain re-anchors at LeafTopY() like every other leaf.
static int ThrHdrY()      { return LeafTopY(); }                           // caption text top
static int ThrRowY()      { return ThrHdrY() + 34; }                       // shimmer row centreline
static int ThrCapY()      { return ThrRowY() + 11; }                       // caption text top
static int ThrOfsY()      { return ThrCapY() + 24; }                       // offset knob centreline
// PLUME EXPANSION (2026-08-09) - replaced the "more to come" caption it was promised in.
static int PlmHdrY()      { return ThrOfsY() + 30; }                       // section caption top
static int PlmRowY()      { return PlmHdrY() + 34; }                       // pill + master slider
static int PlmCapY()      { return PlmRowY() + 11; }                       // regime readout caption
static int PlmCopyY()     { return PlmCapY() + 24; }                       // COPY STOCK row (stock-flame preset)
static int PlmRangeY()    { return PlmCopyY() + ROW_DY; }                  // EXPANSION BAND dual slider
static int PlmSldY(int i) { return PlmRangeY() + ROW_DY + i * ROW_DY; }    // shape slider centreline
static int PlmColY()      { return PlmSldY(NPLM - 1) + ROW_DY; }           // Jet / Bloom swatch row
// The EXPANSION BAND dual slider's mapping: track fraction <-> log10(Pa) over
// [0 .. 5.5] (1 Pa .. ~316 kPa). Handles keep a minimum gap so the regime window
// stays sane; conditions beyond the handles saturate (Venus needs no track room).
static const float PLMB_LPMAX = 5.5f;
static const float PLMB_GAP   = 0.5f;    // decades
static void PlmBandDrag(float f)
{
	const float lp = f * PLMB_LPMAX;
	if (g_dragPlmBand == 0) g_fx.plumeExpLo = min(max(lp, 0.0f), g_fx.plumeExpHi - PLMB_GAP);
	else                    g_fx.plumeExpHi = max(min(lp, PLMB_LPMAX), g_fx.plumeExpLo + PLMB_GAP);
}
// Pressure readout for one handle: kPa above 1 kPa, plain Pa below.
static void PlmPressStr(float lp, char* out, size_t cap)
{
	const double P = pow(10.0, (double)lp);
	// ⚠️ THE THRESHOLDS SIT AT THE ROUNDING BOUNDARY, NOT AT THE DECADE, and that is what
	// BOUNDS the string rather than merely tidying it. Testing P against 1000 and then
	// printing "%.1fk" makes 9999 Pa render as "10.0k" - five characters, because the
	// rounding carries it into the next decade AFTER the branch has already been picked.
	// Two of those either side of a dash is eleven characters in a ten-character column,
	// which is the clipping this very row was reported for. Branching at the boundary
	// keeps every result to FOUR characters ("316k", "9.9k", "995", "9.9"), so the pair
	// can never exceed nine - and "10k" is the more honest reading of 9999 Pa anyway.
	if      (P >= 9950.0) sprintf_s(out, cap, "%.0fk", P * 1e-3);
	else if (P >= 995.0)  sprintf_s(out, cap, "%.1fk", P * 1e-3);
	else if (P >= 9.95)   sprintf_s(out, cap, "%.0f",  P);
	else                  sprintf_s(out, cap, "%.1f",  P);
}

// The thruster family's LAB | PHYSICS switch, on the PLUME EXPANSION header line
// (the PILOT section's mode-switch idea, the plasma VC button's geometry).
static RECT PlmModeBtnRect(const RECT& rc)
{
	const int cy = PlmHdrY() + 6;
	RECT r = { rc.right - SEC_RPAD - 74, cy - 11, rc.right - SEC_RPAD, cy + 11 };
	return r;
}

// BELL GLOW - the incandescent nozzle shells (Meshes\ORO\<class>_bell.msh).
static int BglHdrY()      { return PlmColY() + 30; }                       // section caption top
static int BglRowY()      { return BglHdrY() + 34; }                       // pill + strength centreline
static int BglHeatY()     { return BglRowY() + ROW_DY; }                   // Heat time (s)
static int BglCoolY()     { return BglHeatY() + ROW_DY; }                  // Cool time (s)
static int BglColY()      { return BglCoolY() + ROW_DY; }                  // Bell colour swatch
static int BglCapY()      { return BglColY() + 11; }                       // readout caption top

// STOCK EXHAUST (client patch n) - the judging pill: stock billboards + particle
// streams on (default) or suppressed, so the overlay above is judged alone.
static int StkPillY()     { return BglCapY() + 34; }                       // pill centreline
static int StkCapY()      { return StkPillY() + 11; }                      // caption text top
// CANCEL THRUST - the test-stand rig (session-only; invariant 9's family).
static int CthPillY()     { return StkCapY() + 48; }                       // pill centreline
                                                                           // (+14 for the
                                                                           // cross-reference line)
static int CthCapY()      { return CthPillY() + 11; }                      // caption text top
static int ExhaustBottom() { return CthCapY() + 38; }                      // + the cross-reference line

// ===== LEAF: PARTICLES (VESSEL > THRUSTERS) - the core's own streams ========
// One row per PARTICLESTREAMSPEC field, in the API's own units - that IS the
// feature ("give the users the controls they'd have in the code"). The four the
// user named come first; the rest are free, because the spec was always going to
// be copied wholesale. Note what is NOT here and cannot be: there is no width or
// length (a particle is a round sprite with one srcsize), and there is no colour
// field at all - the swatch drives a SYNTHESIZED TEXTURE and needs patch (l).
static int PrtHdrY()      { return LeafTopY(); }                           // caption text top
static int PrtCopyY()     { return PrtHdrY() + 34; }                       // COPY STOCK row (patch y)
static int PrtRowY(int i) { return PrtCopyY() + ROW_DY + i * ROW_DY; }     // slider centrelines
static const int NPRT     = 8;             // Offset, Size, Lifetime, Rate, Speed,
                                           //   Spread, Growth, Atm slowdown
static int PrtLightY()    { return PrtRowY(NPRT - 1) + ROW_DY; }           // EMISSIVE|DIFFUSE
static int PrtAirY()      { return PrtLightY() + ROW_DY; }                 // air-fade button
static int PrtColY()      { return PrtAirY() + ROW_DY; }                   // colour swatch
static int PrtTexY()      { return PrtColY() + ROW_DY; }                   // texture cycle (phase 1 picker)
static int PrtCapY()      { return PrtTexY() + 14; }                       // readout caption top
// STOCK PARTICLES - the analogue of the EXHAUST page's STOCK EXHAUST pill, and the
// other half of the patch-(n) split: that one kills stock's BILLBOARDS, this one
// kills stock's exhaust PARTICLE STREAMS. Two separate things on two separate pages,
// because you should not have to turn off the flame to adjust the smoke.
static int PrtStkY()      { return PrtCapY() + 34; }                       // pill centreline
static int PrtStkCapY()   { return PrtStkY() + 11; }                       // caption text top
// CANCEL THRUST - the EXHAUST page's test-stand rig, MIRRORED here (his ask): ONE
// flag behind two doors, because particle tuning is exactly when you want a held
// throttle, and reaching the switch meant remembering it lives on another page.
// Session-only as ever (23i); both pills read g_fx.cancelThrust, so they cannot
// disagree and there is no second state to sync.
static int PrtCthY()      { return PrtStkCapY() + 48; }                    // pill centreline
                                                                           // (+14 for the
                                                                           // cross-reference line)
static int PrtCthCapY()   { return PrtCthY() + 11; }                       // caption text top
static int ParticlesBottom() { return PrtCthCapY() + 38; }                 // + the cross-reference line

// ===== LEAF: PLASMA (VESSEL > REENTRY) - the fire + all its tuning ==========
static int ReeHdrY()      { return LeafTopY(); }                           // caption text top
static int ReeRowY()      { return ReeHdrY() + 34; }                       // reentry row centreline
static int ReeCapY()      { return ReeRowY() + 11; }                       // caption text top
static int ReeHeatY()     { return ReeCapY() + 24; }                       // plasma heat readout
static int PlasHdrY()     { return ReeHeatY() + 24; }                      // tuning caption top
static int PlasRowY(int i){ return PlasHdrY() + 22 + i * ROW_DY; }         // tuning row centreline
static int PlasTintY()    { return PlasRowY(NPLAS - 1) + ROW_DY; }         // plasma tint swatch row
static int PlasTrailTintY(){ return PlasTintY() + ROW_DY; }                // trail head/tail swatch row
static int PlasmaBottom() { return PlasTrailTintY() + 24; }

// ===== LEAF: VAPOUR CONE (VESSEL > REENTRY) ================================
// Filed under REENTRY because that family is per-CLASS HULL AERODYNAMICS in
// everything but its name - the scope is right, the family is right, and the
// alternative (WORLD) saves GLOBAL + BODY and would have needed a third scope.
static int VapHdrY()      { return LeafTopY(); }                           // caption text top
static int VapPillY()     { return VapHdrY() + 32; }                       // pill + Test centreline
static int VapC1CapY()    { return VapPillY() + 22; }                      // "C O N E   1" caption
static int VapRowY(int i) { return VapC1CapY() + 26 + i * ROW_DY; }        // cone-1 slider centreline
static int VapColY()      { return VapRowY(NVAP - 1) + ROW_DY; }           // cone-1 swatch pair
static int VapBaseY()     { return VapColY() + ROW_DY; }                   // cone-1 Base fill pill
// FULL PLACEMENT (2026-08-30, his fix round): Position x/y above the renamed
// Position z, in Orbiter's x-y-z order, then Pitch/Yaw. All bipolar knobs.
static int VapPosXY()     { return VapBaseY() + ROW_DY; }                  // cone-1 apex x
static int VapPosYY()     { return VapPosXY() + ROW_DY; }                  // cone-1 apex y
static int VapPosY()      { return VapPosYY() + ROW_DY; }                  // cone-1 apex z (the old knob)
static int VapPitchY()    { return VapPosY() + ROW_DY; }                   // cone-1 axis pitch
static int VapYawY()      { return VapPitchY() + ROW_DY; }                 // cone-1 axis yaw
static int VapBandY()     { return VapYawY() + ROW_DY; }                   // cone-1 Mach band
// THE SECOND CONE (2026-08-29): the identical block again, its own numbers.
static int VapC2CapY()    { return VapBandY() + ROW_DY + 6; }              // "C O N E   2" caption
static int VapRow2Y(int i){ return VapC2CapY() + 26 + i * ROW_DY; }        // cone-2 slider centreline
static int VapCol2Y()     { return VapRow2Y(NVAP - 1) + ROW_DY; }          // cone-2 swatch pair
static int VapBase2Y()    { return VapCol2Y() + ROW_DY; }                  // cone-2 Base fill pill
static int VapPosX2Y()    { return VapBase2Y() + ROW_DY; }                 // cone-2 apex x
static int VapPosY2Y()    { return VapPosX2Y() + ROW_DY; }                 // cone-2 apex y
static int VapPos2Y()     { return VapPosY2Y() + ROW_DY; }                 // cone-2 apex z (the old knob)
static int VapPitch2Y()   { return VapPos2Y() + ROW_DY; }                  // cone-2 axis pitch
static int VapYaw2Y()     { return VapPitch2Y() + ROW_DY; }                // cone-2 axis yaw
static int VapBand2Y()    { return VapYaw2Y() + ROW_DY; }                  // cone-2 Mach band
static int VapWhyY()      { return VapBand2Y() + ROW_DY + 2; }             // "Cone ..." readout
static int VapourBottom() { return VapWhyY() + 24; }

// ===== LEAF: FLIGHT AID (VESSEL) - not an effect (it changes what the VESSEL
// DOES), so it gets its own menu entry, which says the distinction louder than a
// section rule at the bottom of a tab ever did.
static int AidHdrY()      { return LeafTopY(); }                           // caption text top
static int AidWhatY()     { return AidHdrY() + 22; }                       // "this changes how the ship FLIES"
static int AidKnobY()     { return AidWhatY() + 28; }                      // knob centreline
static int AidCapY()      { return AidKnobY() + 13; }                      // caption text top
static int AidGateY()     { return AidCapY() + 28; }                       // ALWAYS | REENTRY ONLY
static int AidReadY()     { return AidGateY() + ROW_DY; }                  // moment readout centreline
static int AidBottom()    { return AidReadY() + 24; }

// ===== LEAF: ECLIPSE (WORLD) ===============================================
static int EclHdrY()      { return LeafTopY(); }                           // caption text top
static int EclPillY()     { return EclHdrY() + 32; }                       // pill + Test centreline
static int EclRowY(int i) { return EclPillY() + 24 + i * ROW_DY; }         // slider centreline
static int EclObscY()     { return EclRowY(NECL - 1) + ROW_DY + 2; }       // "Sun obscured" readout
static int EclCapY()      { return EclObscY() + 11; }                      // caption text top
static int EclEyeY()      { return EclCapY() + 24; }                       // "Eye response" readout
static int EclipseBottom(){ return EclEyeY() + 24; }

// ===== LEAF: AURORA (WORLD) ================================================
static int AurHdrY()      { return LeafTopY(); }                           // caption text top
static int AurPillY()     { return AurHdrY() + 32; }                       // pill + Test centreline
static int AurRowY(int i) { return AurPillY() + 24 + i * ROW_DY; }         // slider centreline
static int AurRibY()      { return AurRowY(NAUR - 1) + ROW_DY; }           // Ribbons slider (1..6)
static int AurKnobY(int i){ return AurRibY() + ROW_DY + i * ROW_DY; }      // bipolar tilt knobs
static int AurColY()      { return AurKnobY(NAURK - 1) + ROW_DY; }         // colour swatch row
static int AurBodyY()     { return AurColY() + ROW_DY + 2; }               // "Curtains over" readout
static int AuroraBottom() { return AurBodyY() + 24; }

// ===== LEAF: LIGHTNING (WORLD > WEATHER) - BOTH systems, one page ==========
// FROM ORBIT (the OroLightning system, per body) on top; IN THE STORM (the RAIN
// system's flashes/bolts/thunder, global) beneath - side by side under honest
// headers, which is the structural fix to the two-lightnings confusion.
static int LtgHdrY()      { return LeafTopY(); }                           // FROM ORBIT caption top
static int LtgPillY()     { return LtgHdrY() + 32; }                       // pill + Test centreline
static int LtgRowY(int i) { return LtgPillY() + 24 + i * ROW_DY; }         // slider centreline
static int LtgColY()      { return LtgRowY(NLTG - 1) + ROW_DY; }           // flash colour swatch
static int LtgBodyY()     { return LtgColY() + ROW_DY + 2; }               // "Storms over" readout
static int RltHdrY()      { return LtgBodyY() + 30; }                      // IN THE STORM caption top
static int RltCapY()      { return RltHdrY() + 22; }                       // the needs-RAIN line
static int RltRowY(int i) { return RltCapY() + 26 + i * ROW_DY; }          // slider centreline
static int RltBoltY()     { return RltRowY(NRLT - 1) + ROW_DY; }           // STRIKE test row
static int LightningBottom(){ return RltBoltY() + 24; }

// ===== LEAF: GOD RAYS (WORLD) ==============================================
static int GryHdrY()      { return LeafTopY(); }                           // caption text top
static int GryPillY()     { return GryHdrY() + 32; }                       // pill + Test centreline
static int GryRowY(int i) { return GryPillY() + 24 + i * ROW_DY; }         // slider centreline
static int GryWhyY()      { return GryRowY(NGRY - 1) + ROW_DY + 2; }       // "Shafts ..." readout
static int GodRaysBottom(){ return GryWhyY() + 24; }

// ===== LEAF: RAIN (WORLD > WEATHER) - three captioned groups ===============
// Row index decides the group (see g_rainRows); RainRowY() opens a captioned gap
// above each group, so paint and hit-testing agree about the geometry for free.
static const int RAIN_GRP = 30;            // vertical air bought by each group caption
static int RainHdrY()     { return LeafTopY(); }                           // caption text top
static int RainPillY()    { return RainHdrY() + 32; }                      // pill + Test centreline
static int RainRowY(int i)
{
	int y = RainPillY() + 24 + RAIN_GRP + i * ROW_DY;         // group-0 caption above row 0
	if (i >= RAIN_EXT_N)              y += RAIN_GRP;           // THE WINDSCREEN caption
	if (i >= RAIN_EXT_N + RAIN_INT_N) y += RAIN_GRP + ROW_DY;  // SOUNDS caption + the view row
	return y;
}
static int RainViewY()    { return RainRowY(RAIN_EXT_N + RAIN_INT_N - 1) + ROW_DY; } // view cycler
                                                                           // (closes the VC group)
static int RainWhyY()     { return RainRowY(NRAIN - 1) + ROW_DY + 2; }     // state readout
static int RainBottom()   { return RainWhyY() + 24; }

// ===== LEAF: VIRTUAL COCKPIT (PILOT) - shadows + cam-shake =================
// ⚠️ ONE leaf, deliberately, and its SAVE TARGET sits ABOVE the first section, not
// inside one: the button governs BOTH sections - the shadow on/off AND the whole
// cam-shake block - so splitting them across pages (or filing the button inside a
// section) would have a control in one place quietly deciding the fate of
// controls somewhere else. Invariant 17(c)'s placement rule, kept by the rework.
static int VcTgtY()       { return LeafTopY() + 12; }                      // save target row
static int VcsHdrY()      { return VcTgtY() + 26; }                        // caption text top
static int VcsPillY()     { return VcsHdrY() + 32; }                       // pill centreline
static int VcsRadY()      { return VcsPillY() + 24; }                      // radius slider centreline
// ORO patch (p): how much of the material AMBIENT the shadow takes with it. Stock
// self-shadowing scales the SUN alone, so a shadowed cockpit surface keeps all its
// ambient and emissive - the floor that makes VC shadows read as a grey smudge.
static int VcsDepY()      { return VcsRadY() + ROW_DY; }                   // depth slider centreline
static int VcsCapY()      { return VcsDepY() + 12; }                       // caption text top
static int CamShakeTop()  { return VcsCapY() + 30; }                       // subsection header centreline
static int ShakeRowY(int i){ return CamShakeTop() + 16 + i * ROW_DY; }
static int MotCapY()      { return ShakeRowY(NSHAKE - 1) + 14; }           // caption text top
static int VcBottom()     { return MotCapY() + 36; }                       // two caption lines

static int ContentBottom(){
	const int pg = CurPage();
	if (IsMenuPage(pg)) return MenuBottom(pg);
	switch (pg) {
	case PG_SCENARIOS: return ScenariosBottom();
	case PG_VC:        return VcBottom();
	case PG_EXHAUST:   return ExhaustBottom();
	case PG_PARTICLES: return ParticlesBottom();
	case PG_PLASMA:    return PlasmaBottom();
	case PG_VAPOUR:    return VapourBottom();
	case PG_FLIGHTAID: return AidBottom();
	case PG_RAIN:      return RainBottom();
	case PG_LIGHTNING: return LightningBottom();
	case PG_AURORA:    return AuroraBottom();
	case PG_ECLIPSE:   return EclipseBottom();
	case PG_GODRAYS:   return GodRaysBottom();
	default:           return GforceBottom();   // PG_GFORCES
	}
}
static int ContentHeight(){ return ContentBottom() - ContentY(); }

// ----------------------------------------------------------------------------
// Geometry helpers. Rows are addressed by their CENTRELINE, so every section
// reuses the same rect builders.
// ----------------------------------------------------------------------------
// An in-row BUTTON spanning the slider track's x-range. TRACK_H is 8 px - a groove,
// not something a word fits in ("I can hardly see the lighting and air fade options"),
// so a row button gets the section-header button's 22 px instead.
static RECT RowBtnRect(const RECT& rc, int cy)
{
	RECT r = { TRACK_X, cy - 11, rc.right - TRACK_RPAD, cy + 11 };
	return r;
}

static RECT TrackRectAt(const RECT& rc, int cy)
{
	RECT r;
	r.left = TRACK_X; r.right = rc.right - TRACK_RPAD;
	r.top = cy - TRACK_H / 2; r.bottom = r.top + TRACK_H;
	return r;
}

static RECT PillRectAt(int cy)
{
	RECT r;
	r.left = PILL_X; r.right = PILL_X + PILL_W;
	r.top = cy - PILL_H / 2; r.bottom = cy + PILL_H / 2;
	return r;
}

static RECT ValueRectAt(const RECT& rc, int cy)
{
	RECT r = { rc.right - TRACK_RPAD + 2, cy - 9, rc.right - SEC_RPAD, cy + 12 };
	return r;
}

// ⚠️ THE VALUE COLUMN IS 71 px - TEN Consolas characters at -12 - and DrawTextA is
// right-aligned, so a longer string renders as its own TAIL and nothing else. That is
// what turned the lightning readout into the bare word "cells)" in a beta screenshot.
// ⚠️ IT WAS 46 px (SEVEN characters) UNTIL 2026-08-25, when the panel widened 5% and every
// pixel of the gain came here. A scan of all thirty DrawValue sites found exactly three
// rows over the old budget, all of them silently losing their LEADING characters: the
// expansion band (he reported it - "13.0-93k" was rendering as "3.0-93k"), the bolt test
// ("bolt 16/16", ten), and the vapour Mach band ("0.85-1.15", nine). All three are inside
// ten now, and PlmPressStr was additionally bounded to four characters a side so the band
// cannot climb back over it. TEN IS THE BUDGET: anything longer needs the rect below.
// A NUMBER always fits; a readout that has to say a WORD ("vacuum", "sun behind", "M 1.15
// 85%", "M 0.7 - gated") does not, and every one of them was written after the warning
// comment was already in this file - which is the tell that a comment was the wrong fix.
// So: readout rows have no slider, their whole track column stands empty, and this rect
// claims it. Use it for any readout that is not purely a short number, and the class of
// bug goes away instead of each instance being caught in a screenshot.
static RECT ReadRectAt(const RECT& rc, int cy)
{
	RECT r = { TRACK_X, cy - 9, rc.right - SEC_RPAD, cy + 12 };
	return r;
}

// The master ENABLED/DISABLED toggle, in the fixed strip just below the banner.
// Mirrors g_fx.masterArmed (the same flag Ctrl+G flips) - the whole experience on/off.
static RECT ArmedBtnRect()
{
	const int cy = BANNER_H + ARMED_H / 2;
	RECT r = { PILL_X, cy - 13, PILL_X + 100, cy + 13 };
	return r;
}

// SAVE, right-aligned in the same FIXED strip. It belongs beside the master arm
// rather than at the bottom of the pane for the same reason the arm does: you must
// be able to reach it from any scroll position, and after a long tuning session the
// last thing you want is to hunt for it.
static RECT SaveBtnRect(const RECT& rc)
{
	const int cy = BANNER_H + ARMED_H / 2;
	RECT r = { rc.right - 12 - 62, cy - 13, rc.right - 12, cy + 13 };
	return r;
}

// HELP, immediately left of SAVE in the same FIXED strip (2026-08-16). It belongs beside
// the master arm and the save for the same reason they are there: it must be reachable
// from any page at any scroll position. It opens the help for whichever page is ACTIVE,
// so where you are when you press it is the question you are asking.
static RECT HelpBtnRect(const RECT& rc)
{
	const int cy = BANNER_H + ARMED_H / 2;
	RECT r = { rc.right - 12 - 62 - 8 - 56, cy - 13, rc.right - 12 - 62 - 8, cy + 13 };
	return r;
}

// THE ENGINE-GROUP CYCLER (2026-08-16; re-homed 2026-08-29). It used to live in the
// ARMED strip - the only fixed real estate there was - because it changes what every
// control on the thruster pages MEANS and so may not scroll out of reach. The menu
// rework gave those pages a fixed header row of their own (THRGRP_H, drawn by
// PaintGrpRow), which keeps the reachability argument AND puts the control beside
// the controls it governs. Phase B's per-thruster selector will live beside it.
// Phase B moved the group button to a fixed LEFT anchor: the row reads left to
// right - group, then the thruster within it, then MARK, with CLEAR at the far
// right when the selection owns this page's override family. All on LINE 1 of
// the 42px strip (centreline +13, same as the old single line); line 2 is the
// state text painted by PaintGrpRow.
static RECT ThrGrpBtnRect(const RECT& rc)
{
	const int cy = PANE_Y + NAV_H + 13;
	RECT r = { 64, cy - 11, 64 + 92, cy + 11 };
	return r;
}
static RECT ThrPrevBtnRect(const RECT& rc)
{
	const int cy = PANE_Y + NAV_H + 13;
	RECT r = { 196, cy - 10, 216, cy + 10 };
	return r;
}
static RECT ThrReadRect(const RECT& rc)
{
	const int cy = PANE_Y + NAV_H + 13;
	RECT r = { 218, cy - 10, 284, cy + 10 };
	return r;
}
static RECT ThrNextBtnRect(const RECT& rc)
{
	const int cy = PANE_Y + NAV_H + 13;
	RECT r = { 286, cy - 10, 306, cy + 10 };
	return r;
}
static RECT ThrMarkBtnRect(const RECT& rc)
{
	const int cy = PANE_Y + NAV_H + 13;
	RECT r = { 316, cy - 10, 360, cy + 10 };
	return r;
}
static RECT ThrClearBtnRect(const RECT& rc)
{
	const int cy = PANE_Y + NAV_H + 13;
	RECT r = { rc.right - SEC_RPAD - 58, cy - 10, rc.right - SEC_RPAD, cy + 10 };
	return r;
}

// The NAV row's two buttons, right-aligned: BACK TO MAIN at the edge, BACK beside
// it. The breadcrumb takes the rest of the row - it is a READOUT, not a button.
static RECT NavMainBtnRect(const RECT& rc)
{
	const int cy = PANE_Y + NAV_H / 2;
	RECT r = { rc.right - 12 - 106, cy - 12, rc.right - 12, cy + 12 };
	return r;
}
static RECT NavBackBtnRect(const RECT& rc)
{
	RECT m = NavMainBtnRect(rc);
	RECT r = { m.left - 8 - 62, m.top, m.left - 8, m.bottom };
	return r;
}

static RECT BlinkBtnRect()
{
	RECT r = { PILL_X, BlinkCY() - 12, PILL_X + 84, BlinkCY() + 12 };
	return r;
}

// The per-page SAVE button, right-aligned in each leaf's own save row.
static RECT LeafSaveBtnRect(const RECT& rc)
{
	RECT r = { rc.right - SEC_RPAD - 62, LeafSaveY() - 12, rc.right - SEC_RPAD, LeafSaveY() + 12 };
	return r;
}

// REVERT, immediately left of the page's SAVE (2026-08-15, a beta ask). Re-reads this
// page's own scopes from disk, so a tuning session that went wrong has a way back that
// is not "remember every number" or "restart Orbiter". It is the exact inverse of the
// button beside it and it costs nothing to build: the load path has existed since the
// settings landed, it was simply never given a control.
static RECT LeafRevBtnRect(const RECT& rc)
{
	RECT r = { rc.right - SEC_RPAD - 62 - 8 - 66, LeafSaveY() - 12,
	           rc.right - SEC_RPAD - 62 - 8,      LeafSaveY() + 12 };
	return r;
}

// (There is no per-world enable button: ACTIVITY is the opt-in. A world with no cfg loads
//  a zero activity and is silent; turning the first slider up is what gives it an aurora.
//  A separate toggle was built on 2026-08-07 and removed the same day - it gated the very
//  flow it was meant to guard, so raising Activity at a new world did nothing.)

// Colour swatch cell(s), laid from the track's left edge so they align with the sliders
// above. idx 0,1 sit side by side (aurora uses both, plasma uses idx 0 only).
static RECT SwatchRect(int cy, int idx)
{
	const int w = 46, gap = 12, h = 16;
	const int x = TRACK_X + idx * (w + gap);
	RECT r = { x, cy - h / 2, x + w, cy + h / 2 };
	return r;
}

// "Test" toggle in the CAM-SHAKE header - forces full-intensity shake at the tuned settings.
static RECT ShakeTestBtnRect(const RECT& rc)
{
	const int cy = CamShakeTop();
	RECT r = { rc.right - TRACK_RPAD - 78, cy - 11, rc.right - TRACK_RPAD - 6, cy + 11 };
	return r;
}

// Scenario buttons: three across, sized to the panel. 138 -> 146 with the 2026-08-25
// widening. These are laid out from the LEFT (PILL_X), so unlike everything anchored to
// SEC_RPAD they do NOT follow the new right edge - leaving them alone would have left the
// whole SCENARIOS block sitting 25 px short of every section rule beside it, which is
// exactly the kind of drift that reads as "untidy" rather than as a bug. Three at 146 plus
// two 8 px gaps runs 14..468 against rules ending at 499: a 31 px tail, against 30 before.
static const int SC_BTN_W = 146, SC_BTN_GAP = 8;
static RECT IndBtnRect(int i)
{
	const int x = PILL_X + i * (SC_BTN_W + SC_BTN_GAP);
	RECT r = { x, IndCY() - 13, x + SC_BTN_W, IndCY() + 13 };
	return r;
}
static RECT RecBtnRect(int i)
{
	const int x = PILL_X + i * (SC_BTN_W + SC_BTN_GAP);
	RECT r = { x, RecCY() - 13, x + SC_BTN_W, RecCY() + 13 };
	return r;
}
// SOUND on/off toggle for the whole scenario section (on the section header line).
static RECT ScenSoundBtnRect(const RECT& rc)
{
	const int cy = ScenHdrY() + 6;
	RECT r = { rc.right - SEC_RPAD - 84, cy - 11, rc.right - SEC_RPAD, cy + 11 };
	return r;
}

// The VC toggle (round 3.5) rides the PLASMA TUNING caption line, right-aligned
// like the scenarios' SOUND button.
static RECT PlasVCBtnRect(const RECT& rc)
{
	const int cy = PlasHdrY() + 6;
	RECT r = { rc.right - SEC_RPAD - 64, cy - 11, rc.right - SEC_RPAD, cy + 11 };
	return r;
}

// ECLIPSE Test toggle - lines up with the CAM-SHAKE one, same reason for existing:
// you cannot tune what you cannot make happen on demand. Here it matters more, since
// the real event is an orbital alignment rather than a throttle setting.
static RECT EclTestBtnRect(const RECT& rc)
{
	const int cy = EclPillY();
	RECT r = { rc.right - TRACK_RPAD - 78, cy - 11, rc.right - TRACK_RPAD - 6, cy + 11 };
	return r;
}

// AURORA Test toggle - same reason and placement as the eclipse's: the real event
// (a polar night) is not something you can produce on demand, so Test rings the
// sub-camera point instead. Lines up with the eclipse and cam-shake toggles.
static RECT AurTestBtnRect(const RECT& rc)
{
	const int cy = AurPillY();
	RECT r = { rc.right - TRACK_RPAD - 78, cy - 11, rc.right - TRACK_RPAD - 6, cy + 11 };
	return r;
}

// LIGHTNING Test toggle - a real storm needs the night side, live cloud AND a seat
// above the deck, none of which you can produce on demand; Test puts one fast cell
// near the sub-camera point with the gates bypassed.
static RECT LtgTestBtnRect(const RECT& rc)
{
	const int cy = LtgPillY();
	RECT r = { rc.right - TRACK_RPAD - 78, cy - 11, rc.right - TRACK_RPAD - 6, cy + 11 };
	return r;
}

// GOD RAYS Test toggle - the real thing needs air AND a low sun, so from orbit or at
// noon there is deliberately nothing to see. Test bypasses both gates so the look can
// be judged from anywhere.
static RECT GryTestBtnRect(const RECT& rc)
{
	const int cy = GryPillY();
	RECT r = { rc.right - TRACK_RPAD - 78, cy - 11, rc.right - TRACK_RPAD - 6, cy + 11 };
	return r;
}

// VAPOUR CONE Test toggle. The real thing needs Mach ~1 in thick air, which is a
// specific twenty seconds of a launch - so without this the only way to judge the look
// is to fly an ascent profile, badly, over and over. Test bypasses the Mach and density
// gates and pins a M 1.05 shape (broad and clearly conical, the reference-photo look).
static RECT VapTestBtnRect(const RECT& rc)
{
	const int cy = VapPillY();
	RECT r = { rc.right - TRACK_RPAD - 78, cy - 11, rc.right - TRACK_RPAD - 6, cy + 11 };
	return r;
}

// PILOT rows put their control where a slider's track would start, so the labels line
// up with every other section.
static RECT PilotBtnRect(int cy, int w)
{
	RECT r = { TRACK_X, cy - 12, TRACK_X + w, cy + 12 };
	return r;
}

static BOOL PtIn(const RECT& r, int x, int y, int slack = 0)
{
	return x >= r.left - slack && x <= r.right + slack && y >= r.top - slack && y <= r.bottom + slack;
}

// Map a mouse x on the track to 0..1 (clamped). The x-range is identical for
// every row, so this needs no row index.
static float TrackValueFromX(const RECT& rc, int x)
{
	RECT t = TrackRectAt(rc, 0);
	if (t.right <= t.left) return 0.0f;
	float v = float(x - t.left) / float(t.right - t.left);
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// Bipolar variant: track centre = 0, ends = -vmax / +vmax. Snaps to exact zero near the
// centre so the user can always get 0 back by dragging to the middle.
static float EnvKnobValueFromX(const RECT& rc, int x, float vmax)
{
	const float v = (TrackValueFromX(rc, x) * 2.0f - 1.0f) * vmax;
	return (v > -0.03f * vmax && v < 0.03f * vmax) ? 0.0f : v;
}

// ----------------------------------------------------------------------------
// Scrolling. The pane shows [g_scroll, g_scroll + paneH) of a ContentHeight()
// tall document; the scrollbar only exists when there is overflow.
// ----------------------------------------------------------------------------
static int MaxScroll(const RECT& rc)
{
	const int over = ContentHeight() - PaneHeight(rc);
	return over > 0 ? over : 0;
}

static void ClampScroll(const RECT& rc)
{
	const int mx = MaxScroll(rc);
	if (g_scroll > mx) g_scroll = mx;
	if (g_scroll < 0)  g_scroll = 0;
}

static RECT ScrollTrackRect(const RECT& rc)
{
	RECT r = { rc.right - SB_RPAD - SB_W, ContentY() + 2, rc.right - SB_RPAD, PaneBottom(rc) - 2 };
	return r;
}

// Thumb height is proportional to the visible fraction, with a floor so it stays grabbable.
static RECT ScrollThumbRect(const RECT& rc)
{
	RECT t = ScrollTrackRect(rc);
	const int trackH = t.bottom - t.top;
	const int contentH = ContentHeight();
	int thumbH = (contentH > 0) ? (int)((float)trackH * PaneHeight(rc) / contentH) : trackH;
	if (thumbH < 28) thumbH = 28;
	if (thumbH > trackH) thumbH = trackH;
	const int mx = MaxScroll(rc);
	const int travel = trackH - thumbH;
	const int off = (mx > 0) ? (int)((float)travel * g_scroll / mx + 0.5f) : 0;
	RECT r = { t.left, t.top + off, t.right, t.top + off + thumbH };
	return r;
}

// Inverse of the above: put the thumb's TOP at py and derive the scroll offset.
static void ScrollFromThumbTop(const RECT& rc, int py)
{
	RECT t = ScrollTrackRect(rc);
	RECT th = ScrollThumbRect(rc);
	const int travel = (t.bottom - t.top) - (th.bottom - th.top);
	const int mx = MaxScroll(rc);
	g_scroll = (travel > 0) ? (int)((float)(py - t.top) * mx / travel + 0.5f) : 0;
	ClampScroll(rc);
}

// ----------------------------------------------------------------------------
// Asset loading. Banner path: <OrbiterRoot>\Modules\ORO\banner.bmp, root
// derived from this DLL's own location (Modules\Plugin\ORO.dll -> two levels
// up), the MediaPlayerMFD pattern. Missing file = procedural fallback banner,
// so the dialog never depends on the artwork to function.
// ----------------------------------------------------------------------------
static void LoadBannerOnce(HINSTANCE hInst)
{
	if (g_hBanner) return;
	char path[MAX_PATH];
	if (GetModuleFileNameA(hInst, path, MAX_PATH)) {
		// strip "\ORO.dll" then "\Plugin" then append Modules\ORO\banner.bmp
		char* p = strrchr(path, '\\'); if (p) *p = '\0';
		p = strrchr(path, '\\'); if (p) *p = '\0';           // ...\Modules
		strcat_s(path, "\\ORO\\banner.bmp");
		g_hBanner = (HBITMAP)LoadImageA(NULL, path, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);
		if (g_hBanner) {
			BITMAP bm; GetObject(g_hBanner, sizeof(bm), &bm);
			g_bannerW = bm.bmWidth; g_bannerH = bm.bmHeight;
			oapiWriteLogV("ORO: banner loaded (%dx%d) from %s - drawn into %d x %d px.",
			              g_bannerW, g_bannerH, path, DLG_W, BANNER_H);
		} else {
			oapiWriteLogV("ORO: no banner.bmp (looked at %s) - using procedural header.", path);
		}
	}
}

static void CreateFontsOnce()
{
	if (g_fontText) return;
	g_fontText  = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
	g_fontSmall = CreateFontA(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
	g_fontBig   = CreateFontA(-30, 0, 0, 0, FW_BOLD,   1, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
	g_fontMenu  = CreateFontA(-17, 0, 0, 0, FW_BOLD,   0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
	g_fontMono  = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Consolas");
}

// ----------------------------------------------------------------------------
// Painting primitives. These kill the copy-paste the sections used to carry: a
// new row kind costs a geometry helper and a loop, not 40 lines of GDI.
// ----------------------------------------------------------------------------
static void FillSolid(HDC dc, const RECT& r, COLORREF c)
{
	HBRUSH b = CreateSolidBrush(c);
	FillRect(dc, &r, b);
	DeleteObject(b);
}

static void DrawPill(HDC dc, const RECT& rp, bool on)
{
	HBRUSH br = CreateSolidBrush(on ? CLR_PILL_ON : CLR_PILL_OFF);
	HGDIOBJ ob = SelectObject(dc, br);
	HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
	RoundRect(dc, rp.left, rp.top, rp.right + 1, rp.bottom + 1, PILL_H, PILL_H);
	SelectObject(dc, ob);
	SelectObject(dc, op);
	DeleteObject(br);
}

static void DrawRowLabel(HDC dc, int cy, const char* label, bool en)
{
	SelectObject(dc, g_fontText);
	SetTextColor(dc, en ? CLR_TEXT : CLR_TEXT_DIM);
	TextOutA(dc, LABEL_X, cy - 9, label, (int)strlen(label));
}

static void DrawValue(HDC dc, const RECT& rc, int cy, const char* text, bool en)
{
	SelectObject(dc, g_fontMono);
	SetTextColor(dc, en ? CLR_TEXT_HI : CLR_TEXT_DIM);
	RECT rv = ValueRectAt(rc, cy);
	DrawTextA(dc, en ? text : "-", -1, &rv, DT_RIGHT | DT_TOP | DT_SINGLELINE);
}

// Unipolar slider: trough, fill from the left, thumb riding the fill edge (the
// thumb is drawn even at 0 so there is always something to grab).
static void DrawSlider(HDC dc, const RECT& rt, float frac, bool en)
{
	FillSolid(dc, rt, CLR_TRACK);
	if (frac < 0.0f) frac = 0.0f; else if (frac > 1.0f) frac = 1.0f;
	const int tx = rt.left + (int)((rt.right - rt.left) * frac + 0.5f);
	if (en && frac > 0.0f) {
		RECT rf = rt; rf.right = tx;
		FillSolid(dc, rf, CLR_ACCENT);
	}
	if (en) {
		RECT rthumb = { tx - 2, rt.top - 4, tx + 2, rt.bottom + 4 };
		FillSolid(dc, rthumb, CLR_TEXT_HI);
	}
}

// Bipolar knob: fill runs from the CENTRE of the track toward the thumb, and a
// tick marks zero - the sign is the whole point of these.
static void DrawBipolar(HDC dc, const RECT& rt, float frac, bool en)
{
	FillSolid(dc, rt, CLR_TRACK);
	const int cx = (rt.left + rt.right) / 2;
	const int tx = rt.left + (int)((rt.right - rt.left) * frac + 0.5f);
	if (en && tx != cx) {
		RECT rf = rt;
		rf.left  = min(cx, tx);
		rf.right = max(cx, tx);
		FillSolid(dc, rf, CLR_ACCENT);
	}
	RECT rtick = { cx - 1, rt.top - 3, cx + 1, rt.bottom + 3 };
	FillSolid(dc, rtick, en ? CLR_TEXT_DIM : CLR_PILL_OFF);
	if (en) {
		RECT rthumb = { tx - 2, rt.top - 4, tx + 2, rt.bottom + 4 };
		FillSolid(dc, rthumb, CLR_TEXT_HI);
	}
}

// DUAL slider (2026-08-09, the user's design): ONE track, TWO handles, the
// segment between them filled - the PLUME EXPANSION band's min/max pressures.
// Click grabs whichever handle is nearer; the handles cannot cross.
static void DrawDualSlider(HDC dc, const RECT& rt, float fLo, float fHi, bool en)
{
	FillSolid(dc, rt, CLR_TRACK);
	if (fLo < 0.0f) fLo = 0.0f; else if (fLo > 1.0f) fLo = 1.0f;
	if (fHi < 0.0f) fHi = 0.0f; else if (fHi > 1.0f) fHi = 1.0f;
	const int xa = rt.left + (int)((rt.right - rt.left) * fLo + 0.5f);
	const int xb = rt.left + (int)((rt.right - rt.left) * fHi + 0.5f);
	if (en && xb > xa) {
		RECT rf = rt; rf.left = xa; rf.right = xb;
		FillSolid(dc, rf, CLR_ACCENT);
	}
	if (en) {
		RECT ra = { xa - 2, rt.top - 4, xa + 2, rt.bottom + 4 };
		RECT rb = { xb - 2, rt.top - 4, xb + 2, rt.bottom + 4 };
		FillSolid(dc, ra, CLR_TEXT_HI);
		FillSolid(dc, rb, CLR_TEXT_HI);
	}
}

// Rounded push-button / toggle. `on` fills it with `onClr`; otherwise it is a
// dark outlined button.
static void DrawButton(HDC dc, const RECT& rb, const char* label, bool on, COLORREF onClr)
{
	HBRUSH br = CreateSolidBrush(on ? onClr : CLR_TRACK);
	HPEN   pn = CreatePen(PS_SOLID, 1, on ? onClr : CLR_PILL_OFF);
	HGDIOBJ ob = SelectObject(dc, br);
	HGDIOBJ op = SelectObject(dc, pn);
	RoundRect(dc, rb.left, rb.top, rb.right, rb.bottom, 6, 6);
	SelectObject(dc, ob);
	SelectObject(dc, op);
	DeleteObject(br);
	DeleteObject(pn);
	SelectObject(dc, g_fontText);
	SetTextColor(dc, on ? CLR_TEXT_HI : CLR_TEXT);
	RECT rt = rb;
	DrawTextA(dc, label, -1, &rt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// A colour swatch: the chosen colour filled with a thin border, or a dark cell when the
// section is disabled. The stored DWORD is already a COLORREF (0x00BBGGRR), so it fills
// directly. A tiny corner nick hints it is clickable.
static void DrawSwatch(HDC dc, const RECT& r, DWORD colour, bool en)
{
	FillSolid(dc, r, en ? (COLORREF)(colour & 0x00FFFFFF) : CLR_TRACK);
	HPEN   pn = CreatePen(PS_SOLID, 1, en ? CLR_TEXT_DIM : CLR_PILL_OFF);
	HGDIOBJ op = SelectObject(dc, pn);
	HGDIOBJ ob = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
	Rectangle(dc, r.left, r.top, r.right, r.bottom);
	SelectObject(dc, op);
	SelectObject(dc, ob);
	DeleteObject(pn);
}

// ----------------------------------------------------------------------------
// CUSTOM COLOUR PICKER (2026-08-09) - a non-modal OVERLAY drawn inside the panel,
// replacing the Win32 ChooseColor. Two reasons, both the user's report:
// (a) ChooseColor is MODAL - it pumps its own message loop on the sim thread, so
//     Orbiter stops stepping while it is open, and on OK the sim integrates one
//     huge dt: a landed vessel gets thrown into the air ("jumps into action").
//     The overlay never blocks, so the sim keeps running - which also buys the
//     feature a modal picker can never have: LIVE PREVIEW. Every drag writes the
//     target colour immediately and the effect recolours in the running sim;
//     Cancel restores the entry value, OK (or clicking away) keeps the current.
// (b) it was a white-background Win95 element in the middle of the dark panel.
//
// Anatomy: an HSV picker - a saturation/value square for the current hue + a
// vertical hue strip - plus old/new chips, an RGB readout, OK and Cancel. The
// two gradients are DIB sections blitted in (per-pixel GDI is too slow); the SV
// square rebuilds only when the hue changes. The overlay lives in the FIXED
// layer (client coords, painted last = topmost), so it does not scroll with the
// pane; while open it eats every pane click (topmost wins), Esc cancels it
// instead of closing the dialog, and the ~10 Hz repaint timer keeps it live.
// ----------------------------------------------------------------------------
static const int PICK_W = 240, PICK_H = 226;   // overlay size
static const int PICK_SV = 150;                // SV square side
static const int PICK_HW = 16;                 // hue strip width

static float clampf01(float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }

static bool    g_pickOpen   = false;
static DWORD*  g_pickTarget = NULL;            // the g_fx colour being edited (LIVE)
static DWORD   g_pickOrig   = 0;               // entry value - Cancel restores this
static float   g_pickH = 0.0f, g_pickS = 0.0f, g_pickV = 1.0f;   // working HSV
static int     g_pickDrag   = -1;              // 0 = SV square, 1 = hue strip
static RECT    g_pickRc     = { 0, 0, 0, 0 };  // overlay rect, CLIENT coords
static HBITMAP g_pickSVDib  = NULL;            // SV square, rebuilt on hue change
static HBITMAP g_pickHueDib = NULL;            // hue strip, built once
static float   g_pickSVHue  = -1.0f;           // hue the SV dib holds (-1 = stale)

static RECT PickSVRect()     { return { g_pickRc.left + 12,  g_pickRc.top + 12,  g_pickRc.left + 12 + PICK_SV,  g_pickRc.top + 12 + PICK_SV }; }
static RECT PickHueRect()    { return { g_pickRc.left + 172, g_pickRc.top + 12,  g_pickRc.left + 172 + PICK_HW, g_pickRc.top + 12 + PICK_SV }; }
static RECT PickOkRect()     { return { g_pickRc.right - 66, g_pickRc.bottom - 30, g_pickRc.right - 12,  g_pickRc.bottom - 10 }; }
static RECT PickCancelRect() { return { g_pickRc.right - 140, g_pickRc.bottom - 30, g_pickRc.right - 72, g_pickRc.bottom - 10 }; }

// HSV <-> RGB, h in [0,360), s/v in [0,1]. Plain textbook forms; the plasma's
// 15b machinery has its own copy (different needs) - these stay dialog-local.
static void PickRgbToHsv(int r, int g, int b, float& h, float& s, float& v)
{
	const float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
	const float mx = max(rf, max(gf, bf)), mn = min(rf, min(gf, bf)), d = mx - mn;
	v = mx;
	s = (mx > 0.0f) ? d / mx : 0.0f;
	if (d <= 1e-6f)      h = 0.0f;
	else if (mx == rf)   h = 60.0f * fmodf((gf - bf) / d + 6.0f, 6.0f);
	else if (mx == gf)   h = 60.0f * ((bf - rf) / d + 2.0f);
	else                 h = 60.0f * ((rf - gf) / d + 4.0f);
}
static void PickHsvToRgb(float h, float s, float v, int& r, int& g, int& b)
{
	h = fmodf(h, 360.0f); if (h < 0.0f) h += 360.0f;
	const float c = v * s, x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f)), m = v - c;
	float rf, gf, bf;
	if      (h <  60.0f) { rf = c; gf = x; bf = 0; }
	else if (h < 120.0f) { rf = x; gf = c; bf = 0; }
	else if (h < 180.0f) { rf = 0; gf = c; bf = x; }
	else if (h < 240.0f) { rf = 0; gf = x; bf = c; }
	else if (h < 300.0f) { rf = x; gf = 0; bf = c; }
	else                 { rf = c; gf = 0; bf = x; }
	r = (int)((rf + m) * 255.0f + 0.5f);
	g = (int)((gf + m) * 255.0f + 0.5f);
	b = (int)((bf + m) * 255.0f + 0.5f);
}

// 32bpp top-down DIB the size of the two gradient controls. Pixels are written
// directly (0x00RRGGBB DWORDs); GDI per-pixel calls would repaint too slowly.
static HBITMAP PickMakeDib(int w, int h, void** bits)
{
	BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
	bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth       = w;
	bi.bmiHeader.biHeight      = -h;           // top-down
	bi.bmiHeader.biPlanes      = 1;
	bi.bmiHeader.biBitCount    = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	return CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, bits, NULL, 0);
}
static void PickEnsureDibs()
{
	void* bits = NULL;
	if (!g_pickHueDib) {
		g_pickHueDib = PickMakeDib(PICK_HW, PICK_SV, &bits);
		if (g_pickHueDib && bits) {
			DWORD* px = (DWORD*)bits;
			for (int y = 0; y < PICK_SV; y++) {
				int r, g, b;
				PickHsvToRgb(360.0f * y / (PICK_SV - 1), 1.0f, 1.0f, r, g, b);
				const DWORD c = ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
				for (int x = 0; x < PICK_HW; x++) px[y * PICK_HW + x] = c;
			}
		}
	}
	if (!g_pickSVDib) g_pickSVDib = PickMakeDib(PICK_SV, PICK_SV, &bits);
	if (g_pickSVDib && g_pickSVHue != g_pickH) {
		DIBSECTION ds;
		if (GetObjectA(g_pickSVDib, sizeof(ds), &ds) && ds.dsBm.bmBits) {
			DWORD* px = (DWORD*)ds.dsBm.bmBits;
			for (int y = 0; y < PICK_SV; y++) {
				const float v = 1.0f - (float)y / (PICK_SV - 1);
				for (int x = 0; x < PICK_SV; x++) {
					int r, g, b;
					PickHsvToRgb(g_pickH, (float)x / (PICK_SV - 1), v, r, g, b);
					px[y * PICK_SV + x] = ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
				}
			}
		}
		g_pickSVHue = g_pickH;
	}
}

// Phase B: which override family THIS picker's live preview created, if any (0 =
// none). ⚠️ THE LEAK IT PLUGS: a preview writes the FLAT buffer, and SyncOut routes
// an UN-OWNED family to the GROUP - so previewing a colour for one jet would have
// silently recoloured the whole group before OK ever created the block. Instead the
// FIRST preview tick creates the override (so the preview lands on the jet, live),
// and a CANCEL that created one takes it away again - cancel means "as you were".
static int g_pickMadeOvr = 0;

// LIVE apply: HSV -> the target COLORREF field. Runs on every drag tick - this is
// the whole point of the non-modal picker (the sim renders the new colour at once).
static void PickApply()
{
	if (!g_pickTarget) return;
	if (g_fx.thrThrSel >= 0 && g_pickTarget != &g_fx.bellTint) {
		const int fam = (CurPage() == PG_EXHAUST)   ? ORO_FAM_EXH
		              : (CurPage() == PG_PARTICLES) ? ORO_FAM_PRT : 0;
		if (fam) {
			OroThrOvr* o = OroThr_FindOvr(g_fx.thrThrSel);
			const bool owned = o && ((fam == ORO_FAM_EXH) ? o->ovrExh : o->ovrPrt);
			if (!owned && OroThr_EnsureOvr(fam)) g_pickMadeOvr = fam;
		}
	}
	int r, g, b;
	PickHsvToRgb(g_pickH, g_pickS, g_pickV, r, g, b);
	*g_pickTarget = ((DWORD)b << 16) | ((DWORD)g << 8) | (DWORD)r;   // COLORREF
}

// Open over the pane, vertically near the swatch row that was clicked (anchorDocY
// is DOCUMENT y - the openers live in pane handlers), clamped inside the pane.
static void OpenColourPicker(HWND hDlg, DWORD& target, int anchorDocY)
{
	g_clickWasEdit = false;      // opening is not yet an edit: the mark lands on the
	                             //   picker's OK (CloseColourPicker), and a cancel
	                             //   reverts the value so it never marks at all
	g_pickOpen   = true;
	g_pickTarget = &target;
	g_pickOrig   = target;
	g_pickMadeOvr = 0;                          // Phase B: fresh picker, no created block yet
	PickRgbToHsv((int)(target & 0xFF), (int)((target >> 8) & 0xFF), (int)((target >> 16) & 0xFF),
	             g_pickH, g_pickS, g_pickV);
	g_pickSVHue  = -1.0f;                       // force the SV rebuild
	g_pickDrag   = -1;
	RECT rc; GetClientRect(hDlg, &rc);
	int top = (anchorDocY - g_scroll) - PICK_H / 2;
	if (top < ContentY() + 4)               top = ContentY() + 4;
	if (top + PICK_H > PaneBottom(rc) - 4)  top = PaneBottom(rc) - 4 - PICK_H;
	const int left = (rc.right - PICK_W) / 2;
	g_pickRc = { left, top, left + PICK_W, top + PICK_H };
}

// Keep an OPEN picker inside the pane after a resize. Without this, shrinking the window
// with the picker open can push its OK/Cancel row off the bottom - and those two buttons
// are the only way out of it, so it would strand the user in a modeless overlay they
// cannot dismiss. Same clamp as OpenColourPicker, applied to the rect it already has.
static void ClampColourPicker(const RECT& rc)
{
	if (!g_pickOpen) return;
	int top = g_pickRc.top;
	if (top + PICK_H > PaneBottom(rc) - 4)  top = PaneBottom(rc) - 4 - PICK_H;
	if (top < ContentY() + 4)               top = ContentY() + 4;
	const int left = (rc.right - PICK_W) / 2;
	g_pickRc = { left, top, left + PICK_W, top + PICK_H };
}

static void CloseColourPicker(bool keep)
{
	if (!keep && g_pickTarget) *g_pickTarget = g_pickOrig;
	// Phase B: a cancelled picker whose PREVIEW created the override takes it away
	// again (see g_pickMadeOvr) - cancel means "as you were", block included. A
	// pre-existing block is never touched: g_pickMadeOvr is only set on creation.
	if (!keep && g_pickMadeOvr) OroThr_ClearOvr(g_pickMadeOvr);
	// A committed pick IS the edit the swatch click deferred (see OpenColourPicker);
	// a cancel just restored the stored colour, so there is nothing to mark.
	if (keep && g_pickTarget) MarkDirty();
	g_pickMadeOvr = 0;
	g_pickOpen = false; g_pickTarget = NULL; g_pickDrag = -1;
}

static void PaintColourPicker(HDC dc)
{
	PickEnsureDibs();
	// Panel: header-dark fill + hairline border, standing off the content below.
	FillSolid(dc, g_pickRc, CLR_BG_HEADER);
	HPEN pn = CreatePen(PS_SOLID, 1, CLR_TEXT_DIM);
	HGDIOBJ op = SelectObject(dc, pn), ob = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
	Rectangle(dc, g_pickRc.left, g_pickRc.top, g_pickRc.right, g_pickRc.bottom);
	SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pn);

	// The two gradients.
	const RECT sv = PickSVRect(), hu = PickHueRect();
	HDC mem = CreateCompatibleDC(dc);
	HGDIOBJ om = SelectObject(mem, g_pickSVDib);
	BitBlt(dc, sv.left, sv.top, PICK_SV, PICK_SV, mem, 0, 0, SRCCOPY);
	SelectObject(mem, g_pickHueDib);
	BitBlt(dc, hu.left, hu.top, PICK_HW, PICK_SV, mem, 0, 0, SRCCOPY);
	SelectObject(mem, om);
	DeleteDC(mem);

	// Markers: a ring at the S/V position (white over black so it reads on any
	// colour), a line pair across the hue strip.
	const int mx = sv.left + (int)(g_pickS * (PICK_SV - 1) + 0.5f);
	const int my = sv.top  + (int)((1.0f - g_pickV) * (PICK_SV - 1) + 0.5f);
	HPEN pb = CreatePen(PS_SOLID, 1, RGB(0, 0, 0)), pw = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
	ob = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
	op = SelectObject(dc, pw);
	Ellipse(dc, mx - 4, my - 4, mx + 5, my + 5);
	SelectObject(dc, pb);
	Ellipse(dc, mx - 5, my - 5, mx + 6, my + 6);
	const int hy = hu.top + (int)(g_pickH / 360.0f * (PICK_SV - 1) + 0.5f);
	SelectObject(dc, pw);
	MoveToEx(dc, hu.left - 2, hy, NULL); LineTo(dc, hu.right + 2, hy);
	SelectObject(dc, pb);
	MoveToEx(dc, hu.left - 2, hy + 1, NULL); LineTo(dc, hu.right + 2, hy + 1);
	SelectObject(dc, op); SelectObject(dc, ob);
	DeleteObject(pb); DeleteObject(pw);

	// Old / new chips + the RGB readout (live numbers - they move as you drag).
	const int cy0 = g_pickRc.top + 12 + PICK_SV + 10;
	RECT rOld = { g_pickRc.left + 12, cy0, g_pickRc.left + 44, cy0 + 18 };
	RECT rNew = { g_pickRc.left + 46, cy0, g_pickRc.left + 78, cy0 + 18 };
	DrawSwatch(dc, rOld, g_pickOrig, true);
	DrawSwatch(dc, rNew, g_pickTarget ? *g_pickTarget : g_pickOrig, true);
	char txt[48];
	const DWORD cur = g_pickTarget ? *g_pickTarget : g_pickOrig;
	sprintf_s(txt, "R %3d  G %3d  B %3d",
	          (int)(cur & 0xFF), (int)((cur >> 8) & 0xFF), (int)((cur >> 16) & 0xFF));
	SelectObject(dc, g_fontMono);
	SetTextColor(dc, CLR_TEXT_HI);
	TextOutA(dc, g_pickRc.left + 86, cy0 + 2, txt, (int)strlen(txt));

	DrawButton(dc, PickCancelRect(), "Cancel", false, CLR_ACCENT);
	DrawButton(dc, PickOkRect(),     "OK",     true,  CLR_PILL_ON);
}

// Mouse, while open. Returns TRUE if the event was consumed (topmost wins: while
// the picker is up it owns every click - clicking OUTSIDE dismisses it KEEPING
// the current colour, since live preview already applied it; Cancel is the undo).
static BOOL PickMouseDown(HWND hDlg, int x, int y)
{
	if (!g_pickOpen) return FALSE;
	if (!PtIn(g_pickRc, x, y)) { CloseColourPicker(true); InvalidateRect(hDlg, NULL, FALSE); return TRUE; }
	if (PtIn(PickSVRect(), x, y, 3)) {
		g_pickDrag = 0; SetCapture(hDlg);
		const RECT sv = PickSVRect();
		g_pickS = clampf01((float)(x - sv.left) / (PICK_SV - 1));
		g_pickV = clampf01(1.0f - (float)(y - sv.top) / (PICK_SV - 1));
		PickApply();
	} else if (PtIn(PickHueRect(), x, y, 3)) {
		g_pickDrag = 1; SetCapture(hDlg);
		const RECT hu = PickHueRect();
		g_pickH = 360.0f * clampf01((float)(y - hu.top) / (PICK_SV - 1));
		PickApply();
	} else if (PtIn(PickOkRect(), x, y))     CloseColourPicker(true);
	else if (PtIn(PickCancelRect(), x, y))   CloseColourPicker(false);
	InvalidateRect(hDlg, NULL, FALSE);
	return TRUE;
}
static BOOL PickMouseMove(HWND hDlg, int x, int y)
{
	if (!g_pickOpen || g_pickDrag < 0) return FALSE;
	if (g_pickDrag == 0) {
		const RECT sv = PickSVRect();
		g_pickS = clampf01((float)(x - sv.left) / (PICK_SV - 1));
		g_pickV = clampf01(1.0f - (float)(y - sv.top) / (PICK_SV - 1));
	} else {
		const RECT hu = PickHueRect();
		g_pickH = 360.0f * clampf01((float)(y - hu.top) / (PICK_SV - 1));
		g_pickSVHue = -1.0f;                    // hue moved: SV square is stale
	}
	PickApply();
	InvalidateRect(hDlg, NULL, FALSE);
	return TRUE;
}

// Small letter-spaced caption, e.g. "C A M - S H A K E".
static void DrawCaption(HDC dc, int x, int top, const char* text)
{
	SelectObject(dc, g_fontSmall);
	SetTextColor(dc, CLR_TEXT_DIM);
	TextOutA(dc, x, top, text, (int)strlen(text));
}

// Section header: caption + the hairline rule under it.
static void DrawSectionHdr(HDC dc, const RECT& rc, int top, const char* text)
{
	DrawCaption(dc, 16, top, text);
	RECT rule = { 16, top + 18, rc.right - SEC_RPAD, top + 19 };
	FillSolid(dc, rule, CLR_LINE);
}

// The same header with a right-aligned note. Used by the two felt-G sections to say what
// their sliders MEAN in the current mode - the readout already names the axis a row is
// waiting for, and this says why the number under your thumb is not the number you set.
static void DrawSectionHdrNote(HDC dc, const RECT& rc, int top, const char* text, const char* note)
{
	DrawSectionHdr(dc, rc, top, text);
	if (!note || !*note) return;
	SelectObject(dc, g_fontSmall);
	SetTextColor(dc, CLR_TEXT_DIM);
	RECT rn = { rc.right / 2, top - 1, rc.right - SEC_RPAD, top + 16 };
	DrawTextA(dc, note, -1, &rn, DT_RIGHT | DT_TOP | DT_SINGLELINE);
}

// What each LEAF's SAVE writes, and to where. The SCOPE is the whole point of the split
// (invariant 17): the pilot's settings follow the PILOT, a hull's follow the HULL, and a
// world's aurora follows the WORLD. Saying so on every page is what stops "I saved it and
// it came back wrong" - the file that will be written is named before you press the button.
// ⚠️ A PER-PAGE MASK IS A CLAIM ABOUT EVERY CONTROL ON THAT PAGE (the 2026-08-29 lesson,
// now at leaf grain): each entry below was re-derived from where its page's keys actually
// live, not inherited from the old tab. When a control moves page or a page gains a pill,
// re-check its row here.
static int LeafSaveMask(int pg)
{
	switch (pg) {
	// The effect PILLS and the LAB|PHYSICS mode on these pages (StockExhaustOn,
	// StockParticlesOn, PlumeOn, PrtOn, ReentryOn, VapourOn...) live in the GLOBAL
	// table while the tuning is per CLASS - CLASS alone silently strands every pill
	// (his 2026-08-29 report; the law, swept at last and kept swept here).
	case PG_EXHAUST: case PG_PARTICLES: case PG_PLASMA: case PG_VAPOUR:
		return ORO_SCOPE_GLOBAL | ORO_SCOPE_CLASS;
	// FLIGHT AID is the one all-CLASS page: CopShift + CopReentryOnly are both facts
	// about the hull, and nothing on the page lives anywhere else.
	case PG_FLIGHTAID:
		return ORO_SCOPE_CLASS;
	// AURORA: the rows are the world's, the pill (AuroraOn) is the pilot's.
	// LIGHTNING: the FROM-ORBIT rows + colour are the world's; its pill AND all three
	// IN-THE-STORM rows (rain fields) are GLOBAL.
	case PG_AURORA: case PG_LIGHTNING:
		return ORO_SCOPE_GLOBAL | ORO_SCOPE_BODY;
	// The eye, the pilot's taste, the (v1, one-world) storm, the scenario sound toggle.
	case PG_ECLIPSE: case PG_GODRAYS: case PG_RAIN: case PG_SCENARIOS:
		return ORO_SCOPE_GLOBAL;
	// VC: the Save target moves the shadow on/off + shake block between scopes, but the
	// cabin box and shadow depth are per class either way - both scopes, always.
	case PG_VC:
		return ORO_SCOPE_GLOBAL | ORO_SCOPE_CLASS;
	// ⚠️ G-FORCES IS THE ONE DYNAMIC ENTRY (2026-08-25, the Save target button). On THIS
	// VESSEL CLASS the pilot block goes to the hull's file - but MasterArmed and
	// ScenarioSound stay GLOBAL whatever the button says, so this page must write BOTH
	// scopes rather than swap one for the other. Returning CLASS alone would silently
	// strand those two keys, and a master-arm state that quietly stopped being saved is
	// exactly the kind of thing nobody notices until it matters.
	// ⚠️ AND THE CLASS SCOPE IS ALSO NEEDED ON THE WAY BACK OUT. Turning the button to ALL
	// VESSELS and saving must CLEAR the hull's stored block, or PilotScope=TRUE and the old
	// values sit in that file still overriding the global ones you just wrote - the "I
	// saved globally and it came back wrong" failure, from the one direction nobody tests.
	// Hence OroSettings_PilotFromClass(): include CLASS while a hull owns a block, so the
	// file gets rewritten without one. A hull that never had one is never given a file.
	default: // PG_GFORCES
		return (g_fx.pilotPerClass || OroSettings_PilotFromClass())
		       ? (ORO_SCOPE_GLOBAL | ORO_SCOPE_CLASS)
		       : ORO_SCOPE_GLOBAL;
	}
}

// An edit happened on the current page: mark its scopes as carrying unsaved values.
// See g_dirtyScopes for the granularity argument (and its deliberate imprecision).
static void MarkDirty()
{
	g_dirtyScopes |= LeafSaveMask(CurPage());
	// Phase B: the first REAL edit on a thruster-selected THRUSTERS page creates
	// that page's override family - exactly the clicks that amber (this function IS
	// the amber, so the two can never disagree), minus the bell family, which stays
	// group-level in v1: its sliders (g_dragBgl), its pill (g_editGrpLevel) and its
	// tint picker (g_pickTarget) all route their edits to the GROUP instead.
	if (g_fx.thrThrSel >= 0 && !g_editGrpLevel && g_dragBgl < 0
	    && g_pickTarget != &g_fx.bellTint) {
		if      (CurPage() == PG_EXHAUST)   OroThr_EnsureOvr(ORO_FAM_EXH);
		else if (CurPage() == PG_PARTICLES) OroThr_EnsureOvr(ORO_FAM_PRT);
	}
}

static void LeafSaveCaption(int pg, char* out, int cap)
{
	const char* cls  = OroSettings_Class();
	const char* body = OroSettings_Body();
	switch (pg) {
	case PG_EXHAUST: case PG_PARTICLES: case PG_PLASMA: case PG_VAPOUR:
		if (cls[0]) sprintf_s(out, cap, "tuning -> %s   pills + mode: global", cls);
		else        strcpy_s(out, cap, "tuning per vessel class   pills + mode: global");
		break;
	case PG_FLIGHTAID:
		if (cls[0]) sprintf_s(out, cap, "saves -> %s - a fact about this hull", cls);
		else        strcpy_s(out, cap, "saves per vessel class - none in focus yet");
		break;
	case PG_AURORA:
		if (body[0]) sprintf_s(out, cap, "aurora -> %s   pill: global", body);
		else         strcpy_s(out, cap, "aurora: no world in range   pill: global");
		break;
	case PG_LIGHTNING:
		if (body[0]) sprintf_s(out, cap, "from orbit -> %s   pill + storm rows: global", body);
		else         strcpy_s(out, cap, "from orbit: no world in range   storm rows: global");
		break;
	case PG_ECLIPSE:
		strcpy_s(out, cap, "saves globally - the same eye behind every canopy");
		break;
	case PG_GODRAYS:
		strcpy_s(out, cap, "saves globally - the pilot's taste at any world");
		break;
	case PG_RAIN:
		strcpy_s(out, cap, "saves globally - per-world files arrive with the weather model");
		break;
	case PG_SCENARIOS:
		strcpy_s(out, cap, "the sound toggle saves globally");
		break;
	case PG_VC:
		// The cabin box and shadow depth are per class either way; what the Save target
		// moves is the shadow on/off and the six cam-shake knobs.
		if      (g_fx.vcPerClass && cls[0]) sprintf_s(out, cap, "all VC settings -> %s", cls);
		else if (g_fx.vcPerClass)           strcpy_s(out, cap, "all VC settings: per class - none in focus yet");
		else if (cls[0])                    sprintf_s(out, cap, "shadows on/off + shake: global   cabin box: %s", cls);
		else                                strcpy_s(out, cap, "shadows on/off + shake: global   cabin box: per class");
		break;
	default: // PG_GFORCES
		// The caption is the promise the SAVE button has to keep, so it names the file the
		// Save target has actually selected - and says "pilot" so it is clear that the
		// master arm and the scenario sound are still going to the global file either way.
		if      (!g_fx.pilotPerClass) strcpy_s(out, cap, "saves globally - the same pilot flies every ship");
		else if (cls[0])              sprintf_s(out, cap, "global + this hull's pilot -> %s", cls);
		else                          strcpy_s(out, cap, "pilot saves per vessel class - none in focus yet");
		break;
	}
}

static void PaintLeafSave(HDC dc, const RECT& rc)
{
	char cap[128];
	LeafSaveCaption(CurPage(), cap, sizeof(cap));
	// Bounded + ellipsised rather than a bare TextOutA: the scope captions run to ~55
	// characters and REVERT moved the free space in from 400 px to ~320, which is close
	// enough that a longer class name would have overprinted the button. Clipping here is
	// cheaper than policing every caption string forever.
	{
		SelectObject(dc, g_fontSmall);
		SetTextColor(dc, CLR_TEXT_DIM);
		RECT rcap = { 16, LeafSaveY() - 6, LeafRevBtnRect(rc).left - 8, LeafSaveY() + 10 };
		DrawTextA(dc, cap, -1, &rcap, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
	}
	// DrawButton's `on` flag fills the button and brightens its label, which IS the press
	// feedback. SAVE also rides it for the UNSAVED indicator: amber while this page's
	// scopes carry edits that are not on disk (his ask, visual pass 2026-08-29), green
	// for the 220 ms press flash - which wins, and by then the save has cleared the
	// dirty bits so the button settles back to plain rather than to amber.
	const bool dirty = (g_dirtyScopes & LeafSaveMask(CurPage())) != 0;
	DrawButton(dc, LeafRevBtnRect(rc),  "REVERT", BtnFlash(3), CLR_MSG_REVRT);
	DrawButton(dc, LeafSaveBtnRect(rc), "SAVE",   BtnFlash(2) || dirty,
	           BtnFlash(2) ? CLR_MSG_SAVE : CLR_MSG_REVRT);
	// The rule is the boundary between fixed chrome and the scrolling pane now, so it
	// runs the full width like the nav row's own base line.
	RECT rule = { 0, LeafSaveY() + 16, rc.right, LeafSaveY() + 17 };
	FillSolid(dc, rule, CLR_LINE);
}

// A NAV-row button: the ordinary button when it can act, everything dimmed when it
// cannot (at the main menu there is nowhere back to go) - the click handler ignores
// it in that state, because a live-looking button that does nothing would lie.
static void DrawNavButton(HDC dc, const RECT& rb, const char* label, bool en)
{
	if (en) { DrawButton(dc, rb, label, false, CLR_PILL_ON); return; }
	HBRUSH br = CreateSolidBrush(CLR_BG_HEADER);
	HPEN   pn = CreatePen(PS_SOLID, 1, CLR_LINE);
	HGDIOBJ ob = SelectObject(dc, br);
	HGDIOBJ op = SelectObject(dc, pn);
	RoundRect(dc, rb.left, rb.top, rb.right, rb.bottom, 6, 6);
	SelectObject(dc, ob);
	SelectObject(dc, op);
	DeleteObject(br);
	DeleteObject(pn);
	SelectObject(dc, g_fontText);
	SetTextColor(dc, CLR_TEXT_DIM);
	RECT rt = rb;
	DrawTextA(dc, label, -1, &rt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// The FIXED nav row (2026-08-29, replacing the tab bar). Drawn in client coordinates
// (NOT scrolled) between the ARMED strip and the content: the breadcrumb on the left -
// dim path, bright current page, deliberately a READOUT rather than a button, so it
// answers "where am I" without inviting a press - and BACK + BACK TO MAIN on the right.
static void PaintNavBar(HDC dc, const RECT& rc)
{
	RECT bar = { 0, PANE_Y, rc.right, PANE_Y + NAV_H };
	FillSolid(dc, bar, CLR_BG_HEADER);
	SetBkMode(dc, TRANSPARENT);
	SelectObject(dc, g_fontText);
	// The path skips the root ("MAIN MENU / " on every line would be noise the BACK TO
	// MAIN button already carries) and ellipsises against the BACK button if a future
	// page name ever makes it long.
	int xp = 16;
	const int ty = PANE_Y + NAV_H / 2 - 9;
	const int xmax = NavBackBtnRect(rc).left - 10;
	SetTextColor(dc, CLR_TEXT_DIM);
	for (int i = 1; i + 1 < g_navDepth; i++) {
		char seg[40];
		sprintf_s(seg, "%s / ", PageName(g_navStack[i]));
		TextOutA(dc, xp, ty, seg, (int)strlen(seg));
		SIZE sz; GetTextExtentPoint32A(dc, seg, (int)strlen(seg), &sz);
		xp += sz.cx;
	}
	SetTextColor(dc, CLR_TEXT_HI);
	const char* cur = PageName(CurPage());
	RECT rcur = { xp, ty, xmax, ty + 18 };
	DrawTextA(dc, cur, -1, &rcur, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

	const bool deep = (g_navDepth > 1);
	DrawNavButton(dc, NavBackBtnRect(rc), "BACK", deep);
	DrawNavButton(dc, NavMainBtnRect(rc), "BACK TO MAIN", deep);
	RECT base = { 0, PANE_Y + NAV_H, rc.right, PANE_Y + NAV_H + 1 };
	FillSolid(dc, base, CLR_LINE);
}

// The engine-group row (EXHAUST/PARTICLES pages only): the cycler's home since the
// menu rework, a second FIXED strip directly under the nav row. Phase B made it
// two lines: the controls, then the SELECTION's state - see ThrGrpBtnRect.
// Paints from PUBLISHED state only (thrCnt/thrOrd/thrSelInfo, SenseMarker's) -
// paint may not call oapi, the plumeRegime discipline.
static void PaintGrpRow(HDC dc, const RECT& rc)
{
	RECT bar = { 0, PANE_Y + NAV_H + 1, rc.right, PANE_Y + NAV_H + THRGRP_H };
	FillSolid(dc, bar, CLR_BG_HEADER);
	SetBkMode(dc, TRANSPARENT);
	SelectObject(dc, g_fontSmall);
	SetTextColor(dc, CLR_TEXT_DIM);
	RECT rgl = { 16, bar.top, 60, bar.top + 26 };
	DrawTextA(dc, "Group:", -1, &rgl, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	// Greys to a plain label when the vessel has only one group - there is nothing to
	// cycle to, and a button that moves between identical states is worse than no
	// button (invariant 18b's rule). The thruster cycler applies the same rule at
	// its own grain: greyed below 2 thrusters in the group (an override on a group's
	// only thruster IS the group, so the state would be a lie).
	char gl[48];
	const int ng = OroThr_Count();
	sprintf_s(gl, ng > 1 ? "%s  (%d)" : "%s", OroThr_Name(g_fx.thrSel), ng);
	DrawButton(dc, ThrGrpBtnRect(rc), gl, ng > 1, ng > 1 ? CLR_PILL_ON : CLR_PILL_OFF);

	SetTextColor(dc, CLR_TEXT_DIM);
	RECT rtl = { 166, bar.top, 194, bar.top + 26 };
	DrawTextA(dc, "Thr:", -1, &rtl, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	const bool canCyc = (g_fx.thrCnt >= 2);
	DrawButton(dc, ThrPrevBtnRect(rc), "<", false, canCyc ? CLR_PILL_ON : CLR_PILL_OFF);
	DrawButton(dc, ThrNextBtnRect(rc), ">", false, canCyc ? CLR_PILL_ON : CLR_PILL_OFF);

	const OroThrOvr* o = (g_fx.thrThrSel >= 0) ? OroThr_FindOvr(g_fx.thrThrSel) : NULL;
	const bool onExhPage = (CurPage() == PG_EXHAUST);
	const bool famOwned  = o && (onExhPage ? o->ovrExh : o->ovrPrt);
	const bool anyOwned  = o && (o->ovrExh || o->ovrPrt);
	char rd[24];
	if (g_fx.thrThrSel < 0) strcpy_s(rd, "ALL");
	else sprintf_s(rd, "%d/%d%s", g_fx.thrOrd, g_fx.thrCnt, anyOwned ? " \x95" : "");
	SetTextColor(dc, anyOwned ? CLR_MSG_REVRT : CLR_TEXT_HI);
	RECT rrd = ThrReadRect(rc);
	DrawTextA(dc, rd, -1, &rrd, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

	// MARK: the in-world nozzle marker toggle (test-rig class - never persisted,
	// never ambers). Lit = markers drawn: the selection bright, or the whole group
	// dim at ALL.
	DrawButton(dc, ThrMarkBtnRect(rc), "MARK", g_fx.thrMarkOn, CLR_ACCENT);
	if (famOwned)
		DrawButton(dc, ThrClearBtnRect(rc), "CLEAR", false, CLR_MSG_REVRT);

	// LINE 2 - the state. Amber when this page's family is overridden, dim
	// otherwise; the below-floor warning is H5's law (a control waiting on a
	// condition must NAME the condition - a vent's silent plume sliders would
	// otherwise read as broken).
	const char* gn = OroThr_Name(g_fx.thrSel);
	const char* floorNote = (onExhPage && g_fx.thrSelBelowFloor)
	                      ? "  (below engine threshold - no plume drawn)" : "";
	char st[200];
	if (g_fx.thrThrSel < 0) {
		if (canCyc) sprintf_s(st, "editing the whole %s group (%d thrusters) - cycle Thr to tune one", gn, g_fx.thrCnt);
		else        sprintf_s(st, "editing the %s group", gn);
	} else if (famOwned) {
		sprintf_s(st, "%s - OVERRIDE (%s) - CLEAR returns it to %s%s", g_fx.thrSelInfo,
		          onExhPage ? "exhaust" : "particles", gn, floorNote);
	} else {
		sprintf_s(st, "%s - inherits %s; first change here creates an override%s",
		          g_fx.thrSelInfo, gn, floorNote);
	}
	SetTextColor(dc, famOwned ? CLR_MSG_REVRT : CLR_TEXT_DIM);
	RECT rst2 = { 16, bar.top + 24, rc.right - SEC_RPAD, bar.bottom };
	DrawTextA(dc, st, -1, &rst2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

	RECT base = { 0, PANE_Y + NAV_H + THRGRP_H, rc.right, PANE_Y + NAV_H + THRGRP_H + 1 };
	FillSolid(dc, base, CLR_LINE);
}

// A menu-page button. His gold-plate mockup re-skinned on his own instruction ("you
// decide the look"): the panel's existing button idiom scaled up - flat dark fill,
// thin border, bold centred label, a small dim sub-line saying what lives behind it.
// COMING SOON entries (target -1) sit flatter and dimmer and never light up.
static void DrawMenuButton(HDC dc, const RECT& rb, const MenuItem& it)
{
	const bool live = (it.target >= 0);
	HBRUSH br = CreateSolidBrush(live ? CLR_TRACK : CLR_BG);
	HPEN   pn = CreatePen(PS_SOLID, 1, live ? CLR_PILL_OFF : CLR_LINE);
	HGDIOBJ ob = SelectObject(dc, br);
	HGDIOBJ op = SelectObject(dc, pn);
	RoundRect(dc, rb.left, rb.top, rb.right, rb.bottom, 10, 10);
	SelectObject(dc, ob);
	SelectObject(dc, op);
	DeleteObject(br);
	DeleteObject(pn);
	SetBkMode(dc, TRANSPARENT);
	// Title + sub-line as a pair, vertically centred in the 100 px face.
	SelectObject(dc, g_fontMenu);
	SetTextColor(dc, live ? CLR_TEXT_HI : CLR_TEXT_DIM);
	RECT rt = { rb.left, rb.top + 26, rb.right, rb.top + 48 };
	DrawTextA(dc, it.label, -1, &rt, DT_CENTER | DT_TOP | DT_SINGLELINE);
	// The sub-line in the ordinary TEXT font, not the small one - it is the explanation
	// of where the door leads, and the visual pass asked for it to be easily readable.
	SelectObject(dc, g_fontText);
	SetTextColor(dc, CLR_TEXT_DIM);
	RECT rs = { rb.left + 10, rb.bottom - 42, rb.right - 10, rb.bottom - 24 };
	DrawTextA(dc, it.sub, -1, &rs, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void PaintMenuPage(HDC dc, const RECT& rc)
{
	int n; const MenuItem* it = MenuOf(CurPage(), n);
	for (int i = 0; i < n; i++) DrawMenuButton(dc, MenuBtnRect(rc, i), it[i]);
}

// One pill+slider+readout row, the shape almost every section is made of. The slider
// shows whatever the current mode lets you edit (LAB: the value, PHYSICS: the gain);
// the readout always shows the VALUE, so in physics mode you can trim a gain and watch
// the number the model is actually producing.
static void DrawFxRow(HDC dc, const RECT& rc, int cy, const FxRow& row)
{
	const bool en = *row.enabled;
	char val[16];
	DrawPill(dc, PillRectAt(cy), en);
	DrawRowLabel(dc, cy, row.label, en);
	DrawSlider(dc, TrackRectAt(rc, cy), *RowKnob(row), en);
	const int pct = (int)(*row.value * 100.0f + 0.5f);
	// PHYSICS, gain up, output still zero: the model is simply not driving this row yet, so
	// name the axis it wants instead of printing a 0 that reads as a dead control (see the
	// FxRow header). Drawn dim and in the value column's own font so it cannot be mistaken
	// for a number. A gain of zero really IS off, and prints 0 like anything else.
	if (en && g_fx.physicsMode && pct == 0 && row.driver && *row.gain > 0.0f) {
		SelectObject(dc, g_fontSmall);
		SetTextColor(dc, CLR_TEXT_DIM);
		RECT rv = ValueRectAt(rc, cy);
		rv.top -= 1;
		DrawTextA(dc, row.driver, -1, &rv, DT_RIGHT | DT_TOP | DT_SINGLELINE);
		return;
	}
	sprintf_s(val, "%d", pct);
	DrawValue(dc, rc, cy, val, en);
}

// ----------------------------------------------------------------------------
// Section painters. All coordinates are DOCUMENT coordinates - the caller has
// already offset the DC's viewport by -g_scroll.
// ----------------------------------------------------------------------------
static void PaintVision(HDC dc, const RECT& rc)
{
	DrawSectionHdrNote(dc, rc, VisHdrY(), "V I S I O N",
	                   g_fx.physicsMode ? "slider = gain on the model" : "slider = the value");
	for (int i = 0; i < NVIS; i++) DrawFxRow(dc, rc, VisRowY(i), g_visRows[i]);
	DrawButton(dc, BlinkBtnRect(), "Blink", false, CLR_ACCENT);
}

static void PaintMotion(HDC dc, const RECT& rc)
{
	DrawSectionHdrNote(dc, rc, MotHdrY(), "M O T I O N",
	                   g_fx.physicsMode ? "slider = gain on the model" : "slider = the value");
	for (int i = 0; i < NMOT; i++) DrawFxRow(dc, rc, MotRowY(i), g_motRows[i]);
}

// CAM-SHAKE lives in the VC tab (it is a virtual-cockpit motion): enable pill + Test
// toggle + the look-shaping sliders. The intensity itself is physics-driven, so the
// caption says so - these knobs only shape the look.
static void PaintCamShake(HDC dc, const RECT& rc)
{
	const bool en = g_fx.shakeEnabled;
	DrawPill(dc, PillRectAt(CamShakeTop()), en);
	DrawCaption(dc, LABEL_X, CamShakeTop() - 7, "C A M - S H A K E");
	DrawButton(dc, ShakeTestBtnRect(rc), "Test", g_fx.shakeTest, CLR_ACCENT);

	char val[16];
	for (int i = 0; i < NSHAKE; i++) {
		const ShakeRow& sr = g_shakeRows[i];
		const int   cy   = ShakeRowY(i);
		const float frac = (sr.vmax > 0.0f) ? (*sr.value / sr.vmax) : 0.0f;
		DrawRowLabel(dc, cy, sr.label, en);
		DrawSlider(dc, TrackRectAt(rc, cy), frac, en);
		if      (sr.raw) sprintf_s(val, "%.2f", *sr.value);        // a gain, not a length
		else if (sr.hz)  sprintf_s(val, "%.0f", *sr.value);
		else             sprintf_s(val, "%.1f", *sr.value * 1000.0f);   // metres -> mm (tenths)
		DrawValue(dc, rc, cy, val, en);
	}
	// The intensity is NOT a slider here - say so, it is the one physics-driven effect.
	// The second line names the split: which row is the lean and which four are the rattle.
	DrawCaption(dc, LABEL_X, MotCapY(), "intensity is physics-driven - these shape the look");
	DrawCaption(dc, LABEL_X, MotCapY() + 14, "seat push = the lean; X/Y/Z + frequency = the buffet");
}

// Per-row captions: these two effects share only their domain, so each says its own
// conditions. The shimmer needs lit engines in atmosphere and is external-only; the
// plasma is physics-driven (the slider trims it, it does not set it) and is the first
// ORO effect visible from BOTH inside and outside the ship.
static const char* g_envCaps[] = {
	"external view, in atmosphere, engines lit",
	"lights hull + cockpit - intensity is physics-driven",
};

// THRUSTER tab: exhaust shimmer + its bipolar offset knob, then the PLUME EXPANSION
// section (2026-08-09) - the pressure-dependent overlay the old caption promised.
// PARTICLES page: Orbiter's own particle streams, one row per spec field.
static void PaintParticles(HDC dc, const RECT& rc)
{
	char val[24];
	const bool pen = g_fx.prtEnabled;
	// Pill + title on one line, the CAM-SHAKE / STOCK EXHAUST idiom: the pill sits at
	// PILL_X and the caption starts at LABEL_X, clear of it. (DrawSectionHdr cannot be
	// used with a pill - its text starts at x=16, under the pill, and its rule line at
	// top+18 runs straight through one.)
	DrawPill(dc, PillRectAt(PrtHdrY() + 7), pen);
	DrawCaption(dc, LABEL_X, PrtHdrY(), "P A R T I C L E   S T R E A M S");

	// COPY STOCK (patch y): load the vessel author's own stream definition into the
	// sliders as a starting point. Successive presses cycle the vessel's streams;
	// slider top ends stretch to fit values the preset range cannot reach (the
	// long-SRB-lifetime case that asked for this).
	{
		const int nstk = OroPrt_StockSpecCount();
		const bool can = (nstk > 0) && pen;
		DrawRowLabel(dc, PrtCopyY(), "Stock preset", can);
		DrawButton(dc, RowBtnRect(rc, PrtCopyY()),
		           nstk < 0 ? "COPY STOCK - needs patch (y)" : "COPY STOCK", can, CLR_PILL_ON);
		if (nstk > 0) {
			char nb[16]; sprintf_s(nb, "%d found", nstk);
			DrawValue(dc, rc, PrtCopyY(), nb, pen);
		}
	}

	for (int i = 0; i < NPRT; i++) {
		const PlasRow& pr = g_prtRows[i];
		const int   cy   = PrtRowY(i);
		const float span = pr.vmax - pr.vmin;
		const float f    = (span > 0.0f) ? ((*pr.value - pr.vmin) / span) : 0.0f;
		DrawRowLabel(dc, cy, pr.label, pen);
		// Offset is the one signed row: a bipolar track, so "back into the bell"
		// reads as the opposite of "further downstream" rather than just a smaller
		// number (the trail-start row's lesson).
		if (i == 0) DrawBipolar(dc, TrackRectAt(rc, cy), f, pen);
		else        DrawSlider (dc, TrackRectAt(rc, cy), f, pen);
		if      (pr.dec == 0) sprintf_s(val, "%.0f", *pr.value);
		else if (pr.dec == 1) sprintf_s(val, i == 0 ? "%+.1f" : "%.1f", *pr.value);
		else                  sprintf_s(val, "%.2f", *pr.value);
		DrawValue(dc, rc, cy, val, pen);
	}

	// EMISSIVE vs DIFFUSE is the single biggest look switch in the spec - a flame
	// that glows on its own versus smoke that the sun lights - so it gets a button,
	// not a hidden flag.
	DrawRowLabel(dc, PrtLightY(), "Lighting", pen);
	DrawButton(dc, RowBtnRect(rc, PrtLightY()),
	           g_fx.prtDiffuse ? "DIFFUSE" : "EMISSIVE", pen, CLR_PILL_ON);

	// Air fade: stock's atmospheric ramp emits NOTHING in vacuum, so this is a
	// button rather than a hidden default - see prtAirFade. The label says what
	// happens, not what the flag is called. The LABEL carries the live diagnostic:
	// while the fade is actually holding emission off, the row says so, which is what
	// let the default become the physically honest one (see prtAirFade's ⚠️).
	DrawRowLabel(dc, PrtAirY(), g_fx.prtVacuum ? "Air fade - in vacuum" : "Air fade", pen);
	DrawButton(dc, RowBtnRect(rc, PrtAirY()),
	           g_fx.prtAirFade ? "FADES IN VACUUM" : "ALWAYS ON", pen, CLR_PILL_ON);

	// The colour swatch needs a synthesized texture (the spec has no colour field),
	// so it is the one control here that depends on a client patch.
	const bool tint = OroParticleTintOK();
	// TWO tints (his design): each particle is randomly born with A or B - the atlas
	// quadrants carry them, so the mix costs nothing. STOCK = the texture's own
	// authored colours; while it is on the swatches grey out (his spec), because in
	// tint mode the pick REPLACES a file's colour (luminance shading) so that white
	// genuinely means white - a multiply could only darken (the 15b lesson).
	const bool colEn = pen && tint && !g_fx.prtTexStock;
	DrawRowLabel(dc, PrtColY(), tint ? "Colour A / B" : "Colour - needs (l)", pen && tint);
	DrawSwatch(dc, SwatchRect(PrtColY(), 0), g_fx.prtColour,  colEn);
	DrawSwatch(dc, SwatchRect(PrtColY(), 1), g_fx.prtColour2, colEn);
	RECT sbtn = RowBtnRect(rc, PrtColY()); sbtn.left = SwatchRect(PrtColY(), 1).right + 12;
	DrawButton(dc, sbtn, "STOCK", pen && tint && g_fx.prtTexStock, CLR_PILL_ON);

	// TEXTURE (phase 1 of the picker, his design): the particle's SHAPE. Cycles the
	// synthesized atlas, Orbiter's two stock particle textures, then whatever .dds
	// files live in Textures\ORO\Particles. Baked through the same patch-(l) upload
	// as the tint, so the swatch keeps working on file textures (white = the file
	// exactly as authored) - which is also why it shares the (l) gate.
	DrawRowLabel(dc, PrtTexY(), tint ? "Texture" : "Texture - needs (l)", pen && tint);
	DrawButton(dc, RowBtnRect(rc, PrtTexY()),
	           g_fx.prtTexName[0] ? g_fx.prtTexName : "ORO (synthesized)", pen && tint, CLR_PILL_ON);

	DrawCaption(dc, LABEL_X, PrtCapY(),
	            // ⚠️ "no exhaust particles at all" IS A CLAIM ABOUT THE VESSEL, and `pen` is
	            // only about the group being edited. On a DG-S with HOVER and USER enabled
	            // it was flatly false, which is most of why this row was unreadable.
	            !pen ? (g_fx.stockParticles ? "off - the vessel author's streams are flying"
	                   : (OroThr_AnyPrtOn() ? "off for THIS group - another group is still streaming"
	                                        : "off - no exhaust particles at all"))
	                 : (g_fx.prtVacuum ? "streams live, but AIR FADE is holding emission off up here"
	                                   : (g_fx.prtInfo[0] ? g_fx.prtInfo : "resolving...")));
	// STOCK PARTICLES: the vessel author's own exhaust streams. Independent of the
	// EXHAUST tab's billboard pill since the patch-(n) split.
	const bool shave2 = OroStockExhaustSupported();
	DrawPill(dc, PillRectAt(PrtStkY()), shave2 && g_fx.stockParticles);
	DrawCaption(dc, LABEL_X, PrtStkY() - 7,
	            shave2 ? "S T O C K   P A R T I C L E S"
	                   : "S T O C K   P A R T I C L E S   -   n e e d s   ( n )");
	DrawCaption(dc, LABEL_X, PrtStkCapY(),
	            !shave2 ? "the running client cannot suppress - stock always emits"
	                    // vessel-wide claim again: ask every group, not the edited one
	                    : (g_fx.stockParticles
	                        ? (OroThr_AnyPrtOn() ? "stock streams + ORO's - both flying"
	                                             : "flying the vessel author's own exhaust streams")
	                        : (OroThr_AnyPrtOn() ? "suppressed - ORO's streams instead"
	                                             : "suppressed - no exhaust particles at all")));
	// ⚠️ CROSS-REFERENCE THE PARTNER PILL, because the split is invisible from either
	// side. Patch (n) was deliberately SPLIT into billboard and stream bits, on two
	// pages (invariant 23n) - the right call, and a beta tester standing on this
	// exact row asked for "a similar option on the EXHAUST sub-tab", which has had one
	// all along at the bottom of its own scroll. A control nobody can find is a control
	// that does not exist, and one line of text is the whole fix.
	DrawCaption(dc, LABEL_X, PrtStkCapY() + 14,
	            "stock BILLBOARDS have their own pill on the EXHAUST page");

	// CANCEL THRUST: the EXHAUST page's session-only test-stand rig, mirrored here -
	// ONE flag behind two doors (his ask: particle tuning wants a held throttle
	// without a page hop). Both pills read g_fx.cancelThrust, so toggling either
	// side changes both and they can never disagree. Never persisted (23i).
	// Phase B: the hold follows the SELECTION (group at ALL, one thruster otherwise),
	// force AND torque, so the caption names the live scope.
	DrawPill(dc, PillRectAt(PrtCthY()), g_fx.cancelThrust);
	DrawCaption(dc, LABEL_X, PrtCthY() - 7, "C A N C E L   T H R U S T");
	{
		char cth[110];
		if (!g_fx.cancelThrust)
			strcpy_s(cth, "test-stand: fire the selected group/thruster without going anywhere");
		else if (g_fx.thrThrSel >= 0)
			sprintf_s(cth, "hold: thr %d nulled (force + torque) - Ctrl+G or pill releases", g_fx.thrThrSel);
		else
			sprintf_s(cth, "hold: %s group nulled (force + torque) - Ctrl+G or pill releases",
			          OroThr_Name(g_fx.thrSel));
		DrawCaption(dc, LABEL_X, PrtCthCapY(), cth);
	}
	DrawCaption(dc, LABEL_X, PrtCthCapY() + 14,
	            "the same switch as the EXHAUST page - one rig, two doors");
}

// The EXHAUST page - everything ORO draws for the engines. (Its old name survives:
// this WAS the THRUSTER tab's exhaust sub-tab before the 2026-08-29 menu rework.)
static void PaintThruster(HDC dc, const RECT& rc)
{
	DrawSectionHdr(dc, rc, ThrHdrY(), "E X H A U S T");
	DrawFxRow(dc, rc, ThrRowY(), g_envRows[0]);                 // shimmer pill+slider
	DrawCaption(dc, LABEL_X, ThrCapY(), g_envCaps[0]);

	const bool ken = g_fx.shimmerEnabled;
	const EnvKnob& kn = g_envKnobs[0];                          // Offset (bipolar)
	const int cy = ThrOfsY();
	const float frac = (kn.vmax > 0.0f) ? (0.5f + 0.5f * (*kn.value / kn.vmax)) : 0.5f;
	char val[16];
	DrawRowLabel(dc, cy, kn.label, ken);
	DrawBipolar(dc, TrackRectAt(rc, cy), frac, ken);
	sprintf_s(val, "%+.2f", *kn.value);                        // signed: the sign is the point
	DrawValue(dc, rc, cy, val, ken);

	// PLUME EXPANSION. The caption is the REGIME READOUT (the reentryHeat discipline):
	// the model's pressure blend must be visible, or "no diamonds at altitude" is
	// indistinguishable from "broken". Pressure switches units below 0.1 kPa so the
	// number stays meaningful all the way up.
	DrawSectionHdr(dc, rc, PlmHdrY(), "P L U M E   E X P A N S I O N");
	// LAB | PHYSICS: in PHYSICS the four pressure/throttle curves drive the jet
	// and the sliders trim on top; in LAB the curves pin to reference and the
	// sliders rule alone. Anchored identical at (sea level, full throttle), so
	// flipping it on the test stand changes nothing until you throttle or climb.
	DrawButton(dc, PlmModeBtnRect(rc), g_fx.plumePhysics ? "PHYSICS" : "LAB",
	           g_fx.plumePhysics, CLR_PILL_ON);
	DrawFxRow(dc, rc, PlmRowY(), g_envRows[2]);                 // pill + master strength
	const bool pen = g_fx.plumeEnabled;
	char cap[80];
	if (g_fx.plumeAtmKPa >= 0.1f)
		sprintf_s(cap, "%.1f kPa - %s", g_fx.plumeAtmKPa, g_fx.plumeRegime);
	else
		sprintf_s(cap, "%.0f Pa - %s", g_fx.plumeAtmKPa * 1000.0f, g_fx.plumeRegime);
	DrawCaption(dc, LABEL_X, PlmCapY(), cap);

	// COPY STOCK: the stock-flame preset. Needs no client capability - the plume's
	// base dimensions already come from the vessel's own exhaust definitions.
	DrawRowLabel(dc, PlmCopyY(), "Stock preset", pen);
	DrawButton(dc, RowBtnRect(rc, PlmCopyY()), "COPY STOCK", pen, CLR_PILL_ON);

	// THE EXPANSION BAND (his design: one track, two handles). LOW handle = the
	// pressure at/below which the vacuum bloom is fully open; HIGH handle = full
	// overexpansion AND the pressure the engine is RATED for (the OD reference).
	// The value column shows both, in Pa (k = kPa).
	DrawRowLabel(dc, PlmRangeY(), "Expansion band", pen);
	DrawDualSlider(dc, TrackRectAt(rc, PlmRangeY()),
	               g_fx.plumeExpLo / PLMB_LPMAX, g_fx.plumeExpHi / PLMB_LPMAX, pen);
	{
		char lo[16], hi[16];
		PlmPressStr(g_fx.plumeExpLo, lo, sizeof(lo));
		PlmPressStr(g_fx.plumeExpHi, hi, sizeof(hi));
		sprintf_s(val, "%s-%s", lo, hi);
		DrawValue(dc, rc, PlmRangeY(), val, pen);
	}

	for (int i = 0; i < NPLM; i++) {
		const PlasRow& pr = g_plumeRows[i];
		const int   py   = PlmSldY(i);
		const float span = pr.vmax - pr.vmin;
		const float pf   = (span > 0.0f) ? ((*pr.value - pr.vmin) / span) : 0.0f;
		DrawRowLabel(dc, py, pr.label, pen);
		DrawSlider(dc, TrackRectAt(rc, py), pf, pen);
		// dec 0 = an integer COUNT (the Diamonds row): show what the build rounds to.
		if (pr.dec == 0) sprintf_s(val, "%.0f", floorf(*pr.value + 0.5f));
		else             sprintf_s(val, pr.dec == 1 ? "%.1f" : "%.2f", *pr.value);
		DrawValue(dc, rc, py, val, pen);
	}
	// The two colour picks: JET (core + diamond body - the diamonds whiten via the
	// fp16 bloom, never via the palette) and BLOOM (the vacuum halo).
	DrawRowLabel(dc, PlmColY(), "Jet / Bloom", pen);
	DrawSwatch(dc, SwatchRect(PlmColY(), 0), g_fx.plumeColJet,   pen);
	DrawSwatch(dc, SwatchRect(PlmColY(), 1), g_fx.plumeColBloom, pen);

	// BELL GLOW: the incandescent nozzle shells - pill + strength, then the two
	// thermal timescales (per class: they are facts about the nozzle hardware).
	// The CAPTION is the readout - which families the class's bell mesh wired,
	// or why nothing glows. An unconfigured hull can never look broken.
	DrawSectionHdr(dc, rc, BglHdrY(), "B E L L   G L O W");
	{
		const bool ben = g_fx.plumeBellOn;
		DrawPill(dc, PillRectAt(BglRowY()), ben);
		DrawRowLabel(dc, BglRowY(), "Bell glow", ben);
		DrawSlider(dc, TrackRectAt(rc, BglRowY()), g_fx.plumeBellGlow / 2.0f, ben);
		sprintf_s(val, "%.2f", g_fx.plumeBellGlow);
		DrawValue(dc, rc, BglRowY(), val, ben);

		DrawRowLabel(dc, BglHeatY(), "Heat time (s)", ben);
		DrawSlider(dc, TrackRectAt(rc, BglHeatY()), (g_fx.plumeBellHeatT - 1.0f) / 19.0f, ben);
		sprintf_s(val, "%.0f", g_fx.plumeBellHeatT);
		DrawValue(dc, rc, BglHeatY(), val, ben);

		DrawRowLabel(dc, BglCoolY(), "Cool time (s)", ben);
		DrawSlider(dc, TrackRectAt(rc, BglCoolY()), (g_fx.plumeBellCoolT - 5.0f) / 115.0f, ben);
		sprintf_s(val, "%.0f", g_fx.plumeBellCoolT);
		DrawValue(dc, rc, BglCoolY(), val, ben);

		// The bell's HUE (2026-08-15). The trim above scales r/g/b uniformly, so it could
		// only ever make the bell brighter AMBER - which is what a tester reported where
		// they expected white/red. This rotates the blackbody ramp onto the picked hue and
		// leaves its structure alone, so the bell still whitens at peak through the bloom.
		DrawRowLabel(dc, BglColY(), "Bell colour", ben);
		DrawSwatch(dc, SwatchRect(BglColY(), 0), g_fx.bellTint, ben);

		// The bell reaches WHITE through the client's bloom, not through its palette
		// (invariant 23g - the emissive is driven past the fp16 threshold on purpose), so
		// with post-processing off it can only ever read amber however hot it gets. That is
		// almost certainly what a tester saw, and it is a settings line rather than a
		// tuning fault - so the caption says which one you are looking at.
		DrawCaption(dc, LABEL_X, BglCapY(),
		            !ben ? "off - needs Meshes\\ORO\\<class>_bell.msh; heat follows thrust^1/4"
		                 : (!OroBloomOn() ? "Light glow is OFF - the bell cannot reach white, only brighter amber"
		                                  : (g_fx.plumeBellInfo[0] ? g_fx.plumeBellInfo : "resolving...")));
	}

	// STOCK EXHAUST (client patch n): pill ON = stock billboards + particle streams
	// render as always (the default); OFF = the client suppresses both on the
	// camera-target vessel, so the overlay above is judged alone. Greys out wholesale
	// without the patch (invariant 18b), VC-shadows style.
	const bool shave = OroStockExhaustSupported();
	DrawPill(dc, PillRectAt(StkPillY()), shave && g_fx.stockExhaust);
	DrawCaption(dc, LABEL_X, StkPillY() - 7,
	            shave ? "S T O C K   E X H A U S T"
	                  : "S T O C K   E X H A U S T   -   n e e d s   p a t c h  ( n )");
	// Same cross-reference as the PARTICLES page's own stock pill: this one owns the
	// BILLBOARDS only, and saying so on both sides is what makes the (n) split visible.
	DrawCaption(dc, LABEL_X, StkCapY(),
	            !shave ? "the running client cannot suppress - stock always renders"
	                   : (g_fx.stockExhaust ? "stock BILLBOARDS render under the overlay"
	                                        : "stock BILLBOARDS suppressed"));
	DrawCaption(dc, LABEL_X, StkCapY() + 14,
	            "stock PARTICLES have their own pill on the PARTICLES page");

	// CANCEL THRUST (session-only test-stand rig). Phase B scoped it to the SELECTION
	// (his requirement): the selected thruster, or the selected group at ALL, each
	// nulled at its OWN position so force and torque die together - the ship stays
	// put for slider/colour work while every out-of-scope control stays honest.
	// Deliberately never persisted (23i); the caption names the live scope.
	DrawPill(dc, PillRectAt(CthPillY()), g_fx.cancelThrust);
	DrawCaption(dc, LABEL_X, CthPillY() - 7, "C A N C E L   T H R U S T");
	{
		char cth[110];
		if (!g_fx.cancelThrust)
			strcpy_s(cth, "test-stand: fire the selected group/thruster without going anywhere");
		else if (g_fx.thrThrSel >= 0)
			sprintf_s(cth, "hold: thr %d nulled (force + torque) - Ctrl+G or pill releases", g_fx.thrThrSel);
		else
			sprintf_s(cth, "hold: %s group nulled (force + torque) - Ctrl+G or pill releases",
			          OroThr_Name(g_fx.thrSel));
		DrawCaption(dc, LABEL_X, CthCapY(), cth);
	}
	// The same switch is mirrored on the PARTICLES page (one flag, two doors) - say
	// so, the stock pills' cross-referencing rule: a twin nobody knows about reads
	// as two different controls.
	DrawCaption(dc, LABEL_X, CthCapY() + 14,
	            "the same switch appears on the PARTICLES page");
}

// REENTRY tab: the plasma pill + heat readout + PLASMA TUNING + (below) the flight aid.
static void PaintReentry(HDC dc, const RECT& rc)
{
	DrawSectionHdr(dc, rc, ReeHdrY(), "R E E N T R Y");
	DrawFxRow(dc, rc, ReeRowY(), g_envRows[1]);                 // reentry pill+slider
	// The caption doubles as the depth-clip warning (invariant 18b: a capability
	// that is dark must SAY so). With SunGlare off the client never builds the
	// scene depth buffer, the per-pixel clip silently dies everywhere, and the
	// only symptom on screen is plasma painting through the hull - which cost a
	// full confused round on 2026-08-08 before the log was read.
	// The same discipline now covers the client's BLOOM, and for the same reason: the
	// plasma composites PRE-RESOLVE into the fp16 chain specifically so that white emerges
	// from HDR accumulation (patch i, invariant 20). With post-processing off nothing ever
	// blooms, every edge stays exactly as authored, and the effect reads hard and "pointy" -
	// which is what two beta testers reported, from a settings line we could not see.
	// Precedence is deliberate: the depth warning first, because plasma painting through
	// the hull is the worse breakage of the two.
	DrawCaption(dc, LABEL_X, ReeCapY(),
	            !OroDepthClipOK() ? "depth occlusion OFF - enable Sun glare in the D3D9 video tab"
	                              : (!OroBloomOn() ? "Light glow is OFF - the plasma will read hard-edged; white comes from bloom"
	                                               : g_envCaps[1]));

	char val[16];
	// Plasma heat of the CAMERA-TARGET vessel. Same reasoning as the felt-G readout: the
	// heat thresholds are the one thing we cannot derive (no vessel exposes a nose radius),
	// so show the number - otherwise a bad threshold is indistinguishable from a bug.
	const bool ren = g_fx.reentryEnabled;
	DrawRowLabel(dc, ReeHeatY(), "Plasma heat", ren);
	sprintf_s(val, "%d%%", (int)(g_fx.reentryHeat * 100.0f + 0.5f));
	SelectObject(dc, g_fontMono);
	SetTextColor(dc, !ren ? CLR_TEXT_DIM : (g_fx.reentryHeat > 0.001f ? CLR_ACCENT : CLR_TEXT_HI));
	RECT rv = ValueRectAt(rc, ReeHeatY());
	DrawTextA(dc, ren ? val : "-", -1, &rv, DT_RIGHT | DT_TOP | DT_SINGLELINE);

	// PLASMA TUNING - the lab scaffolding rows (see g_plasRows). They follow the
	// reentry pill's enabled state; 1.00 = the baked-in look.
	DrawCaption(dc, LABEL_X, PlasHdrY(), "P L A S M A   T U N I N G");
	// VC toggle (round 3.5): also draw the plasma geometry in the VIRTUAL cockpit.
	DrawButton(dc, PlasVCBtnRect(rc), g_fx.reentryVC ? "VC ON" : "VC OFF",
	           g_fx.reentryVC, CLR_PILL_ON);
	for (int i = 0; i < NPLAS; i++) {
		const PlasRow& pr = g_plasRows[i];
		const int   cy   = PlasRowY(i);
		const float span = pr.vmax - pr.vmin;
		const float frac = (span > 0.0f) ? ((*pr.value - pr.vmin) / span) : 0.0f;
		DrawRowLabel(dc, cy, pr.label, ren);
		DrawSlider(dc, TrackRectAt(rc, cy), frac, ren);
		sprintf_s(val, pr.dec == 1 ? "%.1f" : "%.2f", *pr.value);
		DrawValue(dc, rc, cy, val, ren);
	}
	// The plasma's TWO colours, side by side. Tint is a per-channel multiply on the whole
	// palette; Fringe is the same idea aimed only at the MAGENTA CAST - the pink in the edge
	// fringe, the glow corona, the shell shoulder and the streak roots. It is weighted by
	// how far b runs past g, so orange embers and the white-hot core are untouched however
	// far it is pushed. White = the reference look for both.
	DrawRowLabel(dc, PlasTintY(), "Tint / Fringe", ren);
	DrawSwatch(dc, SwatchRect(PlasTintY(), 0), g_fx.plasmaTint,  ren);
	DrawSwatch(dc, SwatchRect(PlasTintY(), 1), g_fx.plasmaTint2, ren);
	// The TRAIL's two colours: HEAD hue and TAIL hue - the ribbon's colour journey,
	// blended along the visible length (15b's rotation law: pick it, get it; white =
	// the reference white-hot -> orange -> ember ramp).
	DrawRowLabel(dc, PlasTrailTintY(), "Trail hot/tail", ren);
	DrawSwatch(dc, SwatchRect(PlasTrailTintY(), 0), g_fx.plasTrailTint,  ren);
	DrawSwatch(dc, SwatchRect(PlasTrailTintY(), 1), g_fx.plasTrailTint2, ren);
}

// ECLIPSE - the camera inside another body's shadow. The TWO readouts are the whole
// argument for the section existing in this shape: an eclipse is rare, so without a
// number saying "the sun is 0% covered" there is no way to distinguish "nothing is
// happening in the sky" from "the effect is broken" - the same lesson the plasma heat
// readout bought. The second number (what the EYE is doing) is separate because the
// two genuinely diverge: fully adapted inside a total eclipse, the sun is 100% covered
// and the eye is doing nothing at all, which is correct and would otherwise look wrong.
static void PaintEclipse(HDC dc, const RECT& rc)
{
	DrawSectionHdr(dc, rc, EclHdrY(), "E C L I P S E");
	const bool en = g_fx.eclipseEnabled;
	char val[48];

	DrawPill(dc, PillRectAt(EclPillY()), en);
	DrawCaption(dc, LABEL_X, EclPillY() - 7, "S H A D O W   O N   T H E   S U N");
	DrawButton(dc, EclTestBtnRect(rc), "Test", g_fx.eclipseTest, CLR_ACCENT);

	for (int i = 0; i < NECL; i++) {
		const PlasRow& er = g_eclRows[i];
		const int   cy   = EclRowY(i);
		const float frac = (er.vmax > 0.0f) ? (*er.value / er.vmax) : 0.0f;
		DrawRowLabel(dc, cy, er.label, en);
		DrawSlider(dc, TrackRectAt(rc, cy), frac, en);
		sprintf_s(val, "%.2f", *er.value);
		DrawValue(dc, rc, cy, val, en);
	}

	// How much of the SUN'S DISC is covered, right now, at the camera.
	const bool active = en || g_fx.eclipseTest;
	DrawRowLabel(dc, EclObscY(), "Sun obscured", active);
	sprintf_s(val, "%d%%", (int)(g_fx.eclipseObsc * 100.0f + 0.5f));
	SelectObject(dc, g_fontMono);
	SetTextColor(dc, !active ? CLR_TEXT_DIM
	                         : (g_fx.eclipseObsc > 0.001f ? CLR_ACCENT : CLR_TEXT_HI));
	RECT rv = ValueRectAt(rc, EclObscY());
	DrawTextA(dc, active ? val : "-", -1, &rv, DT_RIGHT | DT_TOP | DT_SINGLELINE);

	// ... and by what. Naming the body is what turns a number into a sanity check.
	char cap[64];
	if (active && g_fx.eclipseBody[0]) sprintf_s(cap, "occulted by %s", g_fx.eclipseBody);
	else                               strcpy_s(cap, "nothing between you and the Sun");
	DrawCaption(dc, LABEL_X, EclCapY(), cap);

	// What the EYE is doing about it, as a percentage change from adapted-normal.
	// Negative = dimmed, positive = dazzled.
	const int pct = (int)((g_fx.eclipseGain - 1.0f) * 100.0f + (g_fx.eclipseGain >= 1.0f ? 0.5f : -0.5f));
	DrawRowLabel(dc, EclEyeY(), "Eye response", active);
	sprintf_s(val, "%+d%%", pct);
	SelectObject(dc, g_fontMono);
	SetTextColor(dc, !active ? CLR_TEXT_DIM : (pct != 0 ? CLR_ACCENT : CLR_TEXT_HI));
	RECT rv2 = ValueRectAt(rc, EclEyeY());
	DrawTextA(dc, active ? val : "-", -1, &rv2, DT_RIGHT | DT_TOP | DT_SINGLELINE);
}

// AURORA - the curtains. One readout, and it earns its place the same way the eclipse's
// and the plasma heat's do: additive curtains vanish in daylight and below the horizon,
// so without a line naming the planet there is no way to tell "washed out / wrong place"
// from "the effect is broken". Test rings the sub-camera point to sidestep both.
static void PaintAurora(HDC dc, const RECT& rc)
{
	DrawSectionHdr(dc, rc, AurHdrY(), "A U R O R A");
	const bool en = g_fx.auroraEnabled;
	char val[48];

	DrawPill(dc, PillRectAt(AurPillY()), en);
	DrawCaption(dc, LABEL_X, AurPillY() - 7, "C U R T A I N S   I N   T H E   S K Y");
	DrawButton(dc, AurTestBtnRect(rc), "Test", g_fx.auroraTest, CLR_ACCENT);

	for (int i = 0; i < NAUR; i++) {
		const PlasRow& ar = g_aurRows[i];
		const int   cy   = AurRowY(i);
		const float frac = (ar.vmax > 0.0f) ? (*ar.value / ar.vmax) : 0.0f;
		DrawRowLabel(dc, cy, ar.label, en);
		DrawSlider(dc, TrackRectAt(rc, cy), frac, en);
		// Oval lat / Base / Top show real units (degrees, km) instead of the raw 0..1, so the
		// altitude sliders can be dialled against a reference photo.
		if      (ar.value == &g_fx.auroraReach)  sprintf_s(val, "%.0f", OroAurora_OvalLatDeg());
		else if (ar.value == &g_fx.auroraBase)   sprintf_s(val, "%.0f", OroAurora_BaseAltKm());
		else if (ar.value == &g_fx.auroraHeight) sprintf_s(val, "%.0f", OroAurora_TopAltKm());
		else                                     sprintf_s(val, "%.2f", *ar.value);
		DrawValue(dc, rc, cy, val, en);
	}

	// Ribbons (1..6): integer count of concentric curtains per pole. Drawn as a unit slider
	// with an integer readout - the value the Ribbons knob writes, live.
	DrawRowLabel(dc, AurRibY(), "Ribbons", en);
	DrawSlider(dc, TrackRectAt(rc, AurRibY()), (float)(g_fx.auroraRibbons - 1) / 5.0f, en);
	sprintf_s(val, "%d", g_fx.auroraRibbons);
	DrawValue(dc, rc, AurRibY(), val, en);

	// Magnetic-pole offset: two BIPOLAR knobs, zero at centre = the geographic pole.
	for (int i = 0; i < NAURK; i++) {
		const AurKnob& kn = g_aurKnobs[i];
		const int   cy   = AurKnobY(i);
		const float frac = (kn.vmax > 0.0f) ? (0.5f + 0.5f * (*kn.value / kn.vmax)) : 0.5f;
		DrawRowLabel(dc, cy, kn.label, en);
		DrawBipolar(dc, TrackRectAt(rc, cy), frac, en);
		sprintf_s(val, "%+.0f", *kn.value);        // signed degrees: the sign is the point
		DrawValue(dc, rc, cy, val, en);
	}

	// Colours: two swatches - PRIMARY (curtain body) then SECONDARY (the edges). Click to
	// pick. These are the aurora's identity per body once Stage B stores them in the cfg.
	// Three swatches in ALTITUDE ORDER, left to right: base border, main body, diffuse top.
	// Earth reads violet / green / red across them - which is the whole reason there are
	// three rather than two.
	DrawRowLabel(dc, AurColY(), "Base/Body/Top", en);
	DrawSwatch(dc, SwatchRect(AurColY(), 0), g_fx.auroraColBase, en);
	DrawSwatch(dc, SwatchRect(AurColY(), 1), g_fx.auroraColBody, en);
	DrawSwatch(dc, SwatchRect(AurColY(), 2), g_fx.auroraColTop,  en);

	// Which planet the curtains are drawn at, right now - "" means none in range (no
	// atmospheric body near the camera), which is the honest reason for "nothing showing".
	const bool active = en || g_fx.auroraTest;
	DrawRowLabel(dc, AurBodyY(), "Curtains over", active);
	char cap[48];
	if (active && g_fx.auroraBody[0]) sprintf_s(cap, "%s", g_fx.auroraBody);
	else                              strcpy_s(cap, "-");
	SelectObject(dc, g_fontMono);
	SetTextColor(dc, !active ? CLR_TEXT_DIM
	                         : (g_fx.auroraBody[0] ? CLR_ACCENT : CLR_TEXT_HI));
	RECT rv = ReadRectAt(rc, AurBodyY());   // world names run past seven characters
	DrawTextA(dc, cap, -1, &rv, DT_RIGHT | DT_TOP | DT_SINGLELINE);
}

// LIGHTNING - storms in the cloud deck. The readout names the world AND counts the
// active cells, because "no storms" has three honest causes (daylight side, clear
// sky under you, activity 0) and a count you can see tells them from "broken".
static void PaintLightning(HDC dc, const RECT& rc)
{
	// THE PANEL'S TWO LIGHTNINGS FINALLY SHARE A PAGE (2026-08-29, the menu rework).
	// A public-beta reader's confusion (2026-08-25) was that the panel had two lightning
	// systems and said so nowhere; cross-referencing captions were the interim fix, and
	// putting both systems side by side under honest headers is the structural one.
	// FROM ORBIT is the OroLightning system: storms in a planet's cloud deck, read out
	// of its own cloud map, night side only, seen from above, PER BODY. IN THE STORM is
	// the RAIN system's: the storm you are standing in, bolts that reach the ground,
	// day or night, GLOBAL scope. Nothing about one drives the other.
	DrawSectionHdrNote(dc, rc, LtgHdrY(), "F R O M   O R B I T", "storms in the cloud deck - per world");
	const bool en = g_fx.ltgEnabled;
	char val[48];

	DrawPill(dc, PillRectAt(LtgPillY()), en);
	DrawCaption(dc, LABEL_X, LtgPillY() - 7, "S T O R M S   I N   T H E   C L O U D   D E C K");
	DrawButton(dc, LtgTestBtnRect(rc), "Test", g_fx.ltgTest, CLR_ACCENT);

	for (int i = 0; i < NLTG; i++) {
		const PlasRow& lr = g_ltgRows[i];
		const int   cy   = LtgRowY(i);
		const float frac = (lr.vmax > 0.0f) ? (*lr.value / lr.vmax) : 0.0f;
		DrawRowLabel(dc, cy, lr.label, en);
		DrawSlider(dc, TrackRectAt(rc, cy), frac, en);
		// Cell size shows the km it means (same mapping the build uses), like the
		// aurora's altitude rows.
		if (lr.value == &g_fx.ltgCellKm) sprintf_s(val, "%.0f km", OroLightning_CellKm());
		else                             sprintf_s(val, "%.2f", *lr.value);
		DrawValue(dc, rc, cy, val, en);
	}

	// Flash colour: one swatch. Default is the ISS blue-white; per body, because a
	// world's lightning is its own chemistry (Jupiter's flashes are not Earth's).
	DrawRowLabel(dc, LtgColY(), "Flash colour", en);
	DrawSwatch(dc, SwatchRect(LtgColY(), 0), g_fx.ltgColour, en);

	// The world + live cell count.
	const bool active = en || g_fx.ltgTest;
	DrawRowLabel(dc, LtgBodyY(), "Storms over", active);
	char cap[48];
	// The "cells)" bug (2026-08-10) was worked around by TRUNCATING the world name to five
	// characters, which cost the readout most of its meaning to fit a 46 px column. It has
	// the whole track column now (ReadRectAt), so it can say the world's real name and the
	// count in full. ⚠️ A beta tester reported the clipping AFTER the workaround shipped -
	// they are on 260810 and the fix landed on the 11th - so their report is stale; this
	// change is the proper version of it rather than a second fix for the same thing.
	if (active && g_fx.ltgBody[0])
		sprintf_s(cap, "%s - %d cell%s", g_fx.ltgBody, g_fx.ltgCells, g_fx.ltgCells == 1 ? "" : "s");
	else
		strcpy_s(cap, "-");
	SelectObject(dc, g_fontMono);
	SetTextColor(dc, !active ? CLR_TEXT_DIM
	                         : (g_fx.ltgCells > 0 ? CLR_ACCENT : CLR_TEXT_HI));
	RECT rv2 = ReadRectAt(rc, LtgBodyY());
	DrawTextA(dc, cap, -1, &rv2, DT_RIGHT | DT_TOP | DT_SINGLELINE);

	// IN THE STORM - the RAIN system's flashes, bolts and thunder, moved here from the
	// RAIN section (same fields, same save keys). Gated exactly as they were there: live
	// while the storm can exist (the RAIN pill or its Test), and the caption names the
	// dependency so cranking these with no rain running is never a mystery.
	const bool sen = g_fx.rainEnabled || g_fx.rainTest;
	DrawSectionHdrNote(dc, rc, RltHdrY(), "I N   T H E   S T O R M", "the RAIN storm's own - global");
	DrawCaption(dc, 16, RltCapY(), sen ? "flashes, bolts + thunder inside the storm you are in"
	                                   : "needs RAIN - enable it (or its Test) on WEATHER / RAIN");
	for (int i = 0; i < NRLT; i++) {
		const PlasRow& sr = g_rltRows[i];
		const int   cy   = RltRowY(i);
		const float frac = (sr.vmax > 0.0f) ? (*sr.value / sr.vmax) : 0.0f;
		DrawRowLabel(dc, cy, sr.label, sen);
		DrawSlider(dc, TrackRectAt(rc, cy), frac, sen);
		sprintf_s(val, "%.2f", *sr.value);
		DrawValue(dc, rc, cy, val, sen);
	}
	// The STRIKE test row: cycles the 16 baked bolt channels on the focus vessel - the
	// thunder-timing instrument (28m: "we will need it for the sounds"), kept his call.
	DrawRowLabel(dc, RltBoltY(), "Test bolt", sen);
	DrawButton(dc, RowBtnRect(rc, RltBoltY()), "STRIKE", g_fx.boltTestFire, CLR_ACCENT);
	if (g_fx.boltTestSlot >= 0) sprintf_s(val, "%d/16", g_fx.boltTestSlot + 1);
	else                        strcpy_s(val, "-");
	DrawValue(dc, rc, RltBoltY(), val, sen);
}

// GOD RAYS - crepuscular shafts from the sun. The readout carries the REASON there are
// no shafts, because this effect has more honest ways of showing nothing than any other
// in ORO: vacuum, sun behind you, sun too high, sun eclipsed. Without the line, every
// one of those reads as "it is broken".
static void PaintGodRays(HDC dc, const RECT& rc)
{
	DrawSectionHdr(dc, rc, GryHdrY(), "G O D   R A Y S");
	const bool en = g_fx.grayEnabled;
	char val[48];

	DrawPill(dc, PillRectAt(GryPillY()), en);
	DrawCaption(dc, LABEL_X, GryPillY() - 7, "S H A F T S   T H R O U G H   T H E   A I R");
	DrawButton(dc, GryTestBtnRect(rc), "Test", g_fx.grayTest, CLR_ACCENT);

	for (int i = 0; i < NGRY; i++) {
		const PlasRow& gr = g_gryRows[i];
		const int   cy   = GryRowY(i);
		const float frac = (gr.vmax > 0.0f) ? (*gr.value / gr.vmax) : 0.0f;
		DrawRowLabel(dc, cy, gr.label, en);
		DrawSlider(dc, TrackRectAt(rc, cy), frac, en);
		sprintf_s(val, "%.2f", *gr.value);
		DrawValue(dc, rc, cy, val, en);
	}

	// The live gate, as a percentage, plus why it is zero when it is.
	const bool active = en || g_fx.grayTest;
	DrawRowLabel(dc, GryWhyY(), "Shafts", active);
	if      (!active)                  strcpy_s(val, "-");
	else if (g_fx.grayVis > 0.004f)    sprintf_s(val, "%.0f%%", g_fx.grayVis * 100.0f);
	else if (g_fx.grayWhy[0])          sprintf_s(val, "%s", g_fx.grayWhy);
	else                               strcpy_s(val, "none");
	SelectObject(dc, g_fontMono);
	SetTextColor(dc, !active ? CLR_TEXT_DIM
	                         : (g_fx.grayVis > 0.004f ? CLR_ACCENT : CLR_TEXT_HI));
	RECT rv3 = ReadRectAt(rc, GryWhyY());   // grayWhy says WORDS ("sun behind", "vacuum")
	DrawTextA(dc, val, -1, &rv3, DT_RIGHT | DT_TOP | DT_SINGLELINE);
}

// RAIN Test toggle - summon a storm where you are. It RAMPS UP AND HOLDS (the INDUCE
// scenario idiom, invariant 8) rather than running a fixed cycle, because the thing you
// mostly want it for is tuning; releasing it ramps back down and the ground dries slowly.
static RECT RainTestBtnRect(const RECT& rc)
{
	const int cy = RainPillY();
	RECT r = { rc.right - TRACK_RPAD - 78, cy - 11, rc.right - TRACK_RPAD - 6, cy + 11 };
	return r;
}

// RAIN. EXTERNAL view, Earth only, below the weather - and the readout says which of
// those is stopping it, because "no rain" otherwise has four indistinguishable causes.
static void PaintRain(HDC dc, const RECT& rc)
{
	DrawSectionHdrNote(dc, rc, RainHdrY(), "R A I N", "lightning + thunder: WEATHER / LIGHTNING");
	const bool en = g_fx.rainEnabled;
	char val[48];

	DrawPill(dc, PillRectAt(RainPillY()), en);
	DrawCaption(dc, LABEL_X, RainPillY() - 7, "W E A T H E R   A T   T H E   S U R F A C E");
	DrawButton(dc, RainTestBtnRect(rc), "Test", g_fx.rainTest, CLR_ACCENT);

	for (int i = 0; i < NRAIN; i++) {
		// The three group captions, drawn in the gaps RainRowY() opens (his grouping
		// spec: exterior look together, then the interior, then the sounds).
		if (i == 0)
			DrawCaption(dc, 16, RainRowY(0) - 26,                      "T H E   S T O R M   O U T S I D E");
		else if (i == RAIN_EXT_N)
			DrawCaption(dc, 16, RainRowY(RAIN_EXT_N) - 26,             "T H E   W I N D S C R E E N   ( V C )");
		else if (i == RAIN_EXT_N + RAIN_INT_N)
			DrawCaption(dc, 16, RainRowY(RAIN_EXT_N + RAIN_INT_N) - 26, "S O U N D S");
		const PlasRow& rr = g_rainRows[i];
		const int   cy   = RainRowY(i);
		const float span = rr.vmax - rr.vmin;      // Slant is bipolar (vmin < 0)
		const float frac = (span > 0.0f) ? ((*rr.value - rr.vmin) / span) : 0.0f;
		DrawRowLabel(dc, cy, rr.label, en);
		DrawSlider(dc, TrackRectAt(rc, cy), frac, en);
		// ⚠️ HONOUR dec (2026-08-24). This hardcoded "%.2f" and ignored the row's own
		// precision, so the two INTEGER rain rows read back as fractions - Cloud detail
		// as "3.00" and Slant as "-0.67" - which is what made a tester think the notched
		// slider was not notched. In THIS table dec 0 means "an integer-valued control"
		// (the press and drag handlers both snap the store), so printing it with two
		// decimals was the readout disagreeing with the control.
		// NOTE the other row tables use dec 0 to mean display PRECISION only (Fin rake,
		// Rate, Speed are genuinely continuous), which is why this is not swept there.
		sprintf_s(val, rr.dec == 0 ? "%.0f" : "%.2f", *rr.value);
		DrawValue(dc, rc, cy, val, en);
	}

	// WHICH INTERNAL VIEWS GET THE RAIN. A cycler rather than a pill because there are
	// three states, and the same shape as the G-FORCES page's "Effects view" so the two
	// view-scope controls read alike. It closes the windscreen group; the STRIKE test
	// row moved to the LIGHTNING page with the rest of the storm-lightning family.
	static const char* RVIEW[3] = { "VC ONLY", "VC + PANEL", "ALL VIEWS" };
	const int rvm = (g_fx.rainViewMode < 0 || g_fx.rainViewMode > 2) ? 0 : g_fx.rainViewMode;
	DrawRowLabel(dc, RainViewY(), "Rain view", en);
	DrawButton(dc, RowBtnRect(rc, RainViewY()), RVIEW[rvm], false, CLR_ACCENT);

	const bool active = en || g_fx.rainTest;
	DrawRowLabel(dc, RainWhyY(), "Rain", active);
	if      (!active)              strcpy_s(val, "-");
	else if (g_fx.rainWhy[0])      sprintf_s(val, "%s", g_fx.rainWhy);
	else if (g_fx.rainI > 0.004f)  sprintf_s(val, "%s%.0f%%  wet %.0f%%",
	                                         g_fx.rainTest ? "TEST " : "",
	                                         g_fx.rainI * 100.0f, g_fx.rainWet * 100.0f);
	else                           strcpy_s(val, "dry");
	SelectObject(dc, g_fontMono);
	SetTextColor(dc, !active ? CLR_TEXT_DIM
	                         : (g_fx.rainI > 0.004f ? CLR_ACCENT : CLR_TEXT_HI));
	RECT rv4 = ReadRectAt(rc, RainWhyY());
	DrawTextA(dc, val, -1, &rv4, DT_RIGHT | DT_TOP | DT_SINGLELINE);
}

static BOOL ClickRain(HWND hDlg, const RECT& rc, int x, int y)
{
	if (PtIn(PillRectAt(RainPillY()), x, y, 4)) {
		g_fx.rainEnabled = !g_fx.rainEnabled;
		// ⚠️ TURNING THE PILL OFF ALSO CLEARS TEST (2026-08-22). Every section here
		// treats Test as an override that works with the pill off - which is right for
		// PREVIEWING, and wrong the moment someone reaches for the pill to make the
		// effect stop. He pressed it "expecting the rain effect to be completely removed"
		// and it carried on, because Test was still holding it up. An off switch that
		// does not switch the thing off is a bug however defensible the logic is.
		// Preview still works: pill off + Test on runs. What cannot happen any more is a
		// deliberate OFF being quietly outvoted.
		if (!g_fx.rainEnabled) g_fx.rainTest = false;
		return TRUE;
	}
	if (PtIn(RainTestBtnRect(rc), x, y)) {
		g_fx.rainTest = !g_fx.rainTest;
		g_clickWasEdit = false;             // a preview, not a setting
		return TRUE;
	}
	if (g_fx.rainEnabled || g_fx.rainTest) {
		if (PtIn(RowBtnRect(rc, RainViewY()), x, y, 2)) {
			g_fx.rainViewMode = (g_fx.rainViewMode + 1) % 3;
			InvalidateRect(hDlg, NULL, FALSE);
			return TRUE;
		}
		for (int i = 0; i < NRAIN; i++) {
			if (PtIn(TrackRectAt(rc, RainRowY(i)), x, y, 8)) {
				g_dragRain = i;
				SetCapture(hDlg);
				*g_rainRows[i].value = g_rainRows[i].vmin + TrackValueFromX(rc, x)
				                       * (g_rainRows[i].vmax - g_rainRows[i].vmin);
				// dec 0 = a NOTCHED row (Cloud detail): the STORE snaps to integers,
				// not just the readout - his spec. The handle lands ON the notch.
				if (g_rainRows[i].dec == 0)
					*g_rainRows[i].value = floorf(*g_rainRows[i].value + 0.5f);
				return TRUE;
			}
		}
	}
	return FALSE;
}

// VC SHADOWS - the one section where ORO draws NOTHING. Both controls drive the
// patched client's internal-pass shadow map through gcCore::SetVCShadows. It greys out
// wholesale on a client without patch (f), because a switch that cannot do anything is
// worse than no switch. There is deliberately no ShadowMapFilter row: that value is a
// D3DXMACRO compiled into D3D9Client.fx at render-window creation, so nothing can change
// it mid-session - it belongs in the Launchpad D3D9 setup, and the caption says so.
// The VC tab's SAVE TARGET - its own painter, above both sections, because it decides
// where BOTH of them are stored. The twin of the PILOT tab's button, deliberately
// identical in label and geometry: two tabs, one idea, so learning it once is enough.
static void PaintVcTarget(HDC dc, const RECT& rc)
{
	DrawRowLabel(dc, VcTgtY(), "Save target", true);
	DrawButton(dc, PilotBtnRect(VcTgtY(), 150),
	           g_fx.vcPerClass ? "THIS VESSEL CLASS" : "ALL VESSELS",
	           g_fx.vcPerClass, CLR_PILL_ON);
	RECT rule = { 16, VcTgtY() + 14, rc.right - SEC_RPAD, VcTgtY() + 15 };
	FillSolid(dc, rule, CLR_LINE);
}

static void PaintVCShadows(HDC dc, const RECT& rc)
{
	DrawSectionHdr(dc, rc, VcsHdrY(), "V C   S H A D O W S");
	const bool have = OroVCShadowsSupported();
	const bool en   = have && g_fx.vcShadows;
	char val[32];

	DrawPill(dc, PillRectAt(VcsPillY()), en);
	DrawCaption(dc, LABEL_X, VcsPillY() - 7,
	            have ? "S U N L I G H T   T H R O U G H   T H E   W I N D O W S"
	                 : "R E Q U I R E S   A   P A T C H E D   C L I E N T");

	DrawRowLabel(dc, VcsRadY(), "Cabin box (m)", en);
	DrawSlider(dc, TrackRectAt(rc, VcsRadY()), (g_fx.vcShadowRadius - VCS_RAD_MIN) / (VCS_RAD_MAX - VCS_RAD_MIN), en);
	sprintf_s(val, "%.1f", g_fx.vcShadowRadius);
	DrawValue(dc, rc, VcsRadY(), val, en);

	// Shadow depth: 0 is bit-for-bit stock, so the control can never regress the look
	// it was added to improve. EMISSIVE is deliberately never scaled - see patch (p).
	DrawRowLabel(dc, VcsDepY(), "Shadow depth", en);
	DrawSlider(dc, TrackRectAt(rc, VcsDepY()), g_fx.vcShadowDepth, en);
	sprintf_s(val, "%.2f", g_fx.vcShadowDepth);
	DrawValue(dc, rc, VcsDepY(), val, en);

	SelectObject(dc, g_fontSmall);
	SetTextColor(dc, CLR_TEXT_DIM);
	RECT rw = { 16, VcsCapY(), rc.right - SEC_RPAD, VcsCapY() + 20 };
	DrawTextA(dc, have ? "box: smaller = sharper. depth: 0 = stock, 1 = only emissive survives"
	                   : "the client renders these - ORO only sets them",
	          -1, &rw, DT_LEFT | DT_TOP | DT_SINGLELINE);
}

static void PaintScenarios(HDC dc, const RECT& rc)
{
	DrawSectionHdr(dc, rc, ScenHdrY(), "S C E N A R I O S");
	// Scenarios are LAB-only: they overwrite the effect values every frame, which is
	// exactly what the felt-G model does, and two writers on one set of fields is a fight
	// nobody wins. In PHYSICS mode they go dim and stop responding.
	if (g_fx.physicsMode) {
		DrawCaption(dc, 16, IndCapY(), "L A B   M O D E   O N L Y");
		SelectObject(dc, g_fontSmall);
		SetTextColor(dc, CLR_TEXT_DIM);
		RECT rw = { 16, IndCapY() + 20, rc.right - SEC_RPAD, IndCapY() + 40 };
		DrawTextA(dc, "the felt-G model is driving the effects", -1, &rw, DT_LEFT | DT_TOP | DT_SINGLELINE);
		return;
	}
	// SOUND on/off - one switch for every scenario clip (also mutes mid-run).
	DrawButton(dc, ScenSoundBtnRect(rc), g_fx.seqSoundEnabled ? "SOUND ON" : "SOUND OFF",
	           g_fx.seqSoundEnabled, CLR_PILL_ON);
	DrawCaption(dc, 16, IndCapY(), "I N D U C E");
	for (int i = 0; i < NIND; i++) DrawButton(dc, IndBtnRect(i), g_indNames[i], g_fx.seqActive == i, CLR_ACCENT);
	DrawCaption(dc, 16, RecCapY(), "R E C O V E R   F R O M");
	for (int i = 0; i < NREC; i++) DrawButton(dc, RecBtnRect(i), g_recNames[i], g_fx.seqActive == NIND + i, CLR_ACCENT);
}

// PILOT - the felt-G model: what it assumes about the body in the seat, and what it is
// currently measuring. The readout is not decoration: without it there is no way to tell
// a wrong number from a badly-chosen threshold.
static void PaintPilot(HDC dc, const RECT& rc)
{
	DrawSectionHdr(dc, rc, PilHdrY(), "P I L O T");
	const bool phys = g_fx.physicsMode;
	char val[32];

	// 0 - the mode itself. Green when the model is flying the effects.
	DrawRowLabel(dc, PilRowY(0), "Effect source", true);
	DrawButton(dc, PilotBtnRect(PilRowY(0), 110), phys ? "PHYSICS" : "LAB", phys, CLR_PILL_ON);

	// 1 - SAVE TARGET (2026-08-25, his ask): where this tab's settings LIVE. Directly under
	// the mode switch because the two are a pair of properties of the same block - what
	// drives it, and where it is kept. Deliberately NOT gated on `phys`: it governs the LAB
	// pills exactly as much as the model's gains.
	// ⚠️ It reads the hull you are IN, not a preference you carry: the flag is stored per
	// class, so arriving at a vessel with no pilot block of its own shows ALL VESSELS.
	DrawRowLabel(dc, PilRowY(1), "Save target", true);
	DrawButton(dc, PilotBtnRect(PilRowY(1), 150),
	           g_fx.pilotPerClass ? "THIS VESSEL CLASS" : "ALL VESSELS",
	           g_fx.pilotPerClass, CLR_PILL_ON);

	// 2 - tolerance, shown as the +Gz threshold it PRODUCES. A 0..1 abstraction here
	// would be unreadable; "4.0" is the number you can argue with.
	DrawRowLabel(dc, PilRowY(2), "G tolerance", phys);
	DrawSlider(dc, TrackRectAt(rc, PilRowY(2)), g_fx.gTolerance, phys);
	sprintf_s(val, "%.1f", OroPhys_GzThreshold());
	DrawValue(dc, rc, PilRowY(2), val, phys);

	// 3 - anti-G suit: +1.5 G on the POSITIVE threshold only (it stops blood leaving the
	// head, so it does nothing for red-out).
	DrawRowLabel(dc, PilRowY(3), "Anti-G suit", phys);
	DrawButton(dc, PilotBtnRect(PilRowY(3), 78), g_fx.gsuitOn ? "ON" : "OFF", g_fx.gsuitOn, CLR_PILL_ON);

	// 4 - posture: decides which VESSEL axis is the pilot's spine, and so which axis
	// gets the vision suite. Cycling button rather than a dropdown - one more of a row
	// kind we already have, instead of a whole new one.
	// This row and the next are exactly what the Save target above exists for: where the
	// crew SITS is a fact about the airframe, not about the person strapped into it.
	DrawRowLabel(dc, PilRowY(4), "Position", phys);
	DrawButton(dc, PilotBtnRect(PilRowY(4), 110), OroPhys_PoseName(g_fx.pilotPose), false, CLR_ACCENT);

	// 5 - where G is measured. In orbit this is the whole ball game: free-falling, the
	// CoM reads exactly zero while a spinning pilot is pinned to the seat.
	DrawRowLabel(dc, PilRowY(5), "G reference", phys);
	DrawButton(dc, PilotBtnRect(PilRowY(5), 110), g_fx.gRefCamera ? "Camera" : "Vessel CoM", false, CLR_ACCENT);

	// 6 - which internal views the physiology is allowed into. Not gated on `phys`: it
	// governs the LAB sliders and the scenarios too, and a 2D-panel pilot running an
	// INDUCE clip is exactly who would want it narrowed.
	DrawRowLabel(dc, PilRowY(6), "Effects view", true);
	DrawButton(dc, PilotBtnRect(PilRowY(6), 110), g_fx.fxVCOnly ? "VC only" : "Panel + VC",
	           g_fx.fxVCOnly, CLR_PILL_ON);

	// --- live readout ---
	DrawCaption(dc, 16, PilReadCapY(), "F E L T   G");
	struct { const char* label; float v; bool pct; } rd[NPILREAD] = {
		{ "Gz  spine",      g_fx.feltGz,   false },   // +down / -up: grey-out vs red-out
		{ "Gx  eyeballs",   g_fx.feltGx,   false },   // +in / -out: the blur axis
		{ "Gy  lateral",    g_fx.feltGy,   false },   // signed: the tilt lean
		{ "O2 reserve",     g_fx.gReserve, true  },   // what the symptoms actually track
	};
	for (int i = 0; i < NPILREAD; i++) {
		DrawRowLabel(dc, PilReadY(i), rd[i].label, phys);
		if (rd[i].pct) sprintf_s(val, "%d%%", (int)(rd[i].v * 100.0f + 0.5f));
		else           sprintf_s(val, "%+.2f", rd[i].v);
		// The reserve turns accent-red as it empties - the one number worth watching.
		SelectObject(dc, g_fontMono);
		const bool alarm = phys && ((rd[i].pct && rd[i].v < 0.5f) || (!rd[i].pct && fabs(rd[i].v) >= 4.0f));
		SetTextColor(dc, !phys ? CLR_TEXT_DIM : (alarm ? CLR_ACCENT : CLR_TEXT_HI));
		RECT rv = ValueRectAt(rc, PilReadY(i));
		DrawTextA(dc, val, -1, &rv, DT_RIGHT | DT_TOP | DT_SINGLELINE);
	}
}

// FLIGHT AID - the test rig. Moves the FOCUS vessel's effective centre of pressure so
// it can hold a high AoA through an entry without recompiling the vessel (the airfoil
// API is unreachable from a global module - see UpdateCopShift). The moment readout is
// the point of the section: it is the only way to tell "the knob is doing nothing"
// from "there is no air yet".
// THE VAPOUR CONE. The Mach readout is the whole point of the caption block: the trigger
// is a twenty-second window in the middle of an ascent, so without a number on screen
// "nothing happened" and "you were at M 0.7" look identical - the reentryHeat discipline,
// which exists because heat thresholds were unknowable a priori too.
static void PaintVapour(HDC dc, const RECT& rc)
{
	DrawSectionHdr(dc, rc, VapHdrY(), "V A P O U R   C O N E S");
	const bool en = g_fx.vapEnabled;
	char val[48];

	DrawPill(dc, PillRectAt(VapPillY()), en);
	DrawCaption(dc, LABEL_X, VapPillY() - 7, "T R A N S O N I C   C O N D E N S A T I O N");
	DrawButton(dc, VapTestBtnRect(rc), "Test", g_fx.vapTest, CLR_ACCENT);

	// TWO CONES, one painter (2026-08-29, his design): the pill arms the effect,
	// each cone's Opacity is its own visibility, and every control exists twice
	// with completely separate numbers - the colours included.
	auto drawCone = [&](const char* name, const PlasRow* rows, int capY, int rowY0,
	                    int colY, int baseY, int posXY, int posYY, int posY,
	                    int pitchY, int yawY, int bandY, DWORD cVap, DWORD cStk,
	                    bool baseOn, float posX, float posYv, float pos,
	                    float pitch, float yaw, float mLo, float mHi) {
		DrawCaption(dc, 16, capY, name);
		for (int i = 0; i < NVAP; i++) {
			const PlasRow& vr = rows[i];
			const int   cy   = rowY0 + i * ROW_DY;
			const float frac = (vr.vmax > 0.0f) ? (*vr.value / vr.vmax) : 0.0f;
			DrawRowLabel(dc, cy, vr.label, en);
			DrawSlider(dc, TrackRectAt(rc, cy), frac, en);
			sprintf_s(val, "%.2f", *vr.value);
			DrawValue(dc, rc, cy, val, en);
		}
		// The colour picks: the vapour body and the streak filaments.
		DrawRowLabel(dc, colY, "Vapour / Streaks", en);
		DrawSwatch(dc, SwatchRect(colY, 0), cVap, en);
		DrawSwatch(dc, SwatchRect(colY, 1), cStk, en);
		// BASE FILL - the disc closing this cone's wide end; off = the open loft.
		DrawPill(dc, PillRectAt(baseY), en && baseOn);
		DrawRowLabel(dc, baseY, "Base fill", en);
		// FULL PLACEMENT (2026-08-30, his fix round). Position x/y/z in Orbiter's
		// order: x/y nudge the apex along the vessel's own axes, z is the old knob
		// renamed - it still slides along the FLOW axis, which is what he tuned and
		// approved. Then the axis tilt, Pitch (+ = apex up) and Yaw (+ = right),
		// pivoting at the apex. All bipolar, snap to zero, zero = the pre-fix cone.
		auto knob = [&](int cy, const char* lbl, float v, float vmax, const char* fmt) {
			DrawRowLabel(dc, cy, lbl, en);
			DrawBipolar(dc, TrackRectAt(rc, cy), 0.5f + 0.5f * (v / vmax), en);
			sprintf_s(val, fmt, v);
			DrawValue(dc, rc, cy, val, en);
		};
		knob(posXY,  "Position x", posX,  VAP_POS_MAX, "%+.2f");
		knob(posYY,  "Position y", posYv, VAP_POS_MAX, "%+.2f");
		knob(posY,   "Position z", pos,   VAP_POS_MAX, "%+.2f");
		knob(pitchY, "Pitch",      pitch, VAP_ROT_MAX, "%+.1f");
		knob(yawY,   "Yaw",        yaw,   VAP_ROT_MAX, "%+.1f");
		// THE MACH BAND - where this cone starts and stops existing. Per cone, so the
		// collars can appear at different vehicle Mach (real: the flow goes supersonic
		// over different stations at different speeds).
		DrawRowLabel(dc, bandY, "Mach band", en);
		DrawDualSlider(dc, TrackRectAt(rc, bandY),
		               (mLo - VAPB_MLO) / (VAPB_MHI - VAPB_MLO),
		               (mHi - VAPB_MLO) / (VAPB_MHI - VAPB_MLO), en);
		sprintf_s(val, "%.2f-%.2f", mLo, mHi);
		DrawValue(dc, rc, bandY, val, en);
	};
	drawCone("C O N E   1", g_vapRows,  VapC1CapY(), VapRowY(0),  VapColY(),  VapBaseY(),
	         VapPosXY(),  VapPosYY(),  VapPosY(),  VapPitchY(),  VapYawY(),  VapBandY(),
	         g_fx.vapColour,  g_fx.vapStreakCol,  g_fx.vapBaseOn,
	         g_fx.vapPosX,  g_fx.vapPosY,  g_fx.vapPos,  g_fx.vapPitch,  g_fx.vapYaw,
	         g_fx.vapMachMin,  g_fx.vapMachMax);
	drawCone("C O N E   2", g_vapRows2, VapC2CapY(), VapRow2Y(0), VapCol2Y(), VapBase2Y(),
	         VapPosX2Y(), VapPosY2Y(), VapPos2Y(), VapPitch2Y(), VapYaw2Y(), VapBand2Y(),
	         g_fx.vapColour2, g_fx.vapStreakCol2, g_fx.vapBaseOn2,
	         g_fx.vapPosX2, g_fx.vapPosY2, g_fx.vapPos2, g_fx.vapPitch2, g_fx.vapYaw2,
	         g_fx.vapMachMin2, g_fx.vapMachMax2);

	// Live Mach + gate, and the reason it is zero when it is. "subsonic", "thin air" and
	// "vacuum" are three different things a pilot can act on; "nothing" is not.
	const bool active = en || g_fx.vapTest;
	DrawRowLabel(dc, VapWhyY(), "Cone", active);
	if      (!active)                 strcpy_s(val, "-");
	else if (g_fx.vapVis > 0.004f)    sprintf_s(val, "M %.2f  %.0f%%", g_fx.vapMach, g_fx.vapVis * 100.0f);
	else if (g_fx.vapWhy[0])          sprintf_s(val, "M %.2f  %s", g_fx.vapMach, g_fx.vapWhy);
	else                              strcpy_s(val, "none");
	SelectObject(dc, g_fontMono);
	SetTextColor(dc, !active ? CLR_TEXT_DIM
	                         : (g_fx.vapVis > 0.004f ? CLR_ACCENT : CLR_TEXT_HI));
	RECT rv4 = ReadRectAt(rc, VapWhyY());   // "M 1.15  85%" is eleven characters
	DrawTextA(dc, val, -1, &rv4, DT_RIGHT | DT_TOP | DT_SINGLELINE);
}

static void PaintFlightAid(HDC dc, const RECT& rc)
{
	DrawSectionHdr(dc, rc, AidHdrY(), "F L I G H T   A I D");
	const bool en = g_fx.masterArmed;
	char val[32];

	// ⚠️ SAY WHAT THIS SECTION IS, FIRST. It is the one part of ORO that changes what the
	// VESSEL DOES rather than what you SEE, it is the last section on the tab, and it
	// ships enabled on the DG and Atlantis - so a beta tester flew it for hours,
	// attributed the handling to "PULSE changes the aerodynamic model", and never found
	// the slider that was doing it. Everything above this line is an effect; this is not.
	SelectObject(dc, g_fontSmall);
	SetTextColor(dc, CLR_ACCENT);
	{
		RECT rwh = { 16, AidWhatY(), rc.right - SEC_RPAD, AidWhatY() + 20 };
		DrawTextA(dc, "TEST RIG - this one changes how the VESSEL FLIES, not how it looks",
		          -1, &rwh, DT_LEFT | DT_TOP | DT_SINGLELINE);
	}

	const int cy = AidKnobY();
	DrawRowLabel(dc, cy, "CoP shift (m)", en);
	DrawBipolar(dc, TrackRectAt(rc, cy), 0.5f + 0.5f * (g_fx.copShift / COP_MAX), en);
	sprintf_s(val, "%+.2f", g_fx.copShift);          // signed: the sign IS the effect
	DrawValue(dc, rc, cy, val, en);

	SelectObject(dc, g_fontSmall);
	SetTextColor(dc, CLR_TEXT_DIM);
	RECT rw = { 16, AidCapY(), rc.right - SEC_RPAD, AidCapY() + 20 };
	DrawTextA(dc, "forward (+) = less nose-down = holds AoA. 0 = vessel untouched", -1, &rw,
	          DT_LEFT | DT_TOP | DT_SINGLELINE);

	// The regime gate. Default REENTRY ONLY - see g_fx.copReentryOnly for the PIO report
	// that put it there. ALWAYS is the pre-2026-08-15 behaviour, kept as the escape hatch.
	DrawRowLabel(dc, AidGateY(), "Applies", en);
	DrawButton(dc, RowBtnRect(rc, AidGateY()),
	           g_fx.copReentryOnly ? "REENTRY ONLY (M 3+)" : "ALWAYS", g_fx.copReentryOnly, CLR_PILL_ON);

	// Live pitch moment. Greys out at zero air the same way it greys out disarmed -
	// both mean "nothing is being applied", which is exactly what you want to see. And
	// when the GATE is what is holding it off, the readout says so rather than reading 0:
	// the same rule the PHYSICS gain sliders just learned on the G-FORCE tab.
	const bool live = en && fabs(g_fx.copShift) > 0.001f;
	DrawRowLabel(dc, AidReadY(), "Pitch moment", live);
	if (live && g_fx.copGated) sprintf_s(val, "M %.1f - gated", g_fx.copMach);
	else                       sprintf_s(val, "%+.1f kNm", g_fx.copMoment);
	SelectObject(dc, g_fontMono);
	SetTextColor(dc, !live ? CLR_TEXT_DIM
	                       : (fabs(g_fx.copMoment) > 0.05f ? CLR_TEXT_HI : CLR_TEXT_DIM));
	RECT rv = ReadRectAt(rc, AidReadY());   // "M 0.7 - gated" is thirteen
	DrawTextA(dc, val, -1, &rv, DT_RIGHT | DT_TOP | DT_SINGLELINE);
}

// ----------------------------------------------------------------------------
// Painting (double-buffered: everything into a memory DC, one blit out).
// ----------------------------------------------------------------------------
static void PaintDialog(HWND hDlg, HDC dcOut)
{
	RECT rc; GetClientRect(hDlg, &rc);
	const int W = rc.right, H = rc.bottom;
	ClampScroll(rc);

	HDC dc = CreateCompatibleDC(dcOut);
	HBITMAP bb = CreateCompatibleBitmap(dcOut, W, H);
	HGDIOBJ oldbb = SelectObject(dc, bb);
	SetBkMode(dc, TRANSPARENT);

	// Background
	FillSolid(dc, rc, CLR_BG);

	// --- Banner strip (fixed) -----------------------------------------------
	RECT rBan = { 0, 0, W, BANNER_H };
	if (g_hBanner) {
		HDC src = CreateCompatibleDC(dc);
		HGDIOBJ oldsrc = SelectObject(src, g_hBanner);
		SetStretchBltMode(dc, HALFTONE);
		SetBrushOrgEx(dc, 0, 0, NULL);
		StretchBlt(dc, 0, 0, W, BANNER_H, src, 0, 0, g_bannerW, g_bannerH, SRCCOPY);
		SelectObject(src, oldsrc);
		DeleteDC(src);
	} else {
		// Procedural fallback: wordmark + a simple trace, in-theme.
		FillSolid(dc, rBan, CLR_BG_HEADER);
		SelectObject(dc, g_fontBig);
		SetTextColor(dc, CLR_TEXT_HI);
		// Lengths are computed, not counted: the wordmark shrank from ORO (5)
		// to ORO (3) in the rename, and a stale literal here would have TextOutA
		// paint whatever followed the terminator.
		static const char* WORDMARK = "ORO";
		static const char* SUBTITLE = "Orbiter Realism Overhaul";
		TextOutA(dc, 20, 26, WORDMARK, (int)strlen(WORDMARK));
		SelectObject(dc, g_fontSmall);
		SetTextColor(dc, CLR_TEXT_DIM);
		TextOutA(dc, 22, 66, SUBTITLE, (int)strlen(SUBTITLE));
		POINT tr[14] = { {210,58},{270,58},{280,58},{288,38},{297,78},{306,47},{313,58},{372,58},{390,58},{397,38},{406,78},{415,47},{422,58},{W-16,58} };
		HPEN pen = CreatePen(PS_SOLID, 2, CLR_ACCENT);
		HGDIOBJ oldpen = SelectObject(dc, pen);
		Polyline(dc, tr, 14);
		SelectObject(dc, oldpen);
		DeleteObject(pen);
	}
	RECT rSep = { 0, BANNER_H, W, BANNER_H + 1 };
	FillSolid(dc, rSep, CLR_LINE);

	// --- Master ENABLED strip (fixed; mirrors Ctrl+G) ------------------------
	RECT rStrip = { 0, BANNER_H + 1, W, BANNER_H + ARMED_H };
	FillSolid(dc, rStrip, CLR_BG_HEADER);
	{
		const bool armed = g_fx.masterArmed;
		RECT rb = ArmedBtnRect();
		HBRUSH br = CreateSolidBrush(armed ? CLR_PILL_ON : CLR_ACCENT);
		HGDIOBJ ob = SelectObject(dc, br);
		HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
		RoundRect(dc, rb.left, rb.top, rb.right, rb.bottom, 13, 13);
		SelectObject(dc, ob);
		SelectObject(dc, op);
		DeleteObject(br);
		SelectObject(dc, g_fontText);
		SetTextColor(dc, CLR_TEXT_HI);
		DrawTextA(dc, armed ? "ENABLED" : "DISABLED", -1, &rb, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		SelectObject(dc, g_fontSmall);
		SetTextColor(dc, CLR_TEXT_DIM);
		RECT rsv = SaveBtnRect(rc);
		RECT rhp = HelpBtnRect(rc);
		// (The engine-group cycler that used to time-share this strip lives in the
		// thruster pages' own fixed header row now - PaintGrpRow.)
		RECT rst = { rb.right + 10, rb.top, rhp.left - 8, rb.bottom };
		DrawTextA(dc, "Master arm - all effects (Ctrl+G)",
		          -1, &rst, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		// Lit green while the help window is up, so the button doubles as the answer to
		// "is it already open, or did it open behind the sim window?"
		DrawButton(dc, rhp, "HELP", g_hHelp != NULL, CLR_PILL_ON);
		// The global SAVE: green for the press flash, AMBER while ANY scope carries
		// unsaved edits (his ask, visual pass 2026-08-29 - see g_dirtyScopes).
		DrawButton(dc, rsv, "SAVE", BtnFlash(1) || g_dirtyScopes != 0,
		           BtnFlash(1) ? CLR_MSG_SAVE : CLR_MSG_REVRT);
	}
	RECT rSep2 = { 0, PANE_Y - 1, W, PANE_Y };
	FillSolid(dc, rSep2, CLR_LINE);

	// --- Nav row + the other fixed strips ------------------------------------
	PaintNavBar(dc, rc);
	if (PageHasGrpRow(CurPage())) PaintGrpRow(dc, rc);
	// The leaf's SAVE/REVERT row is FIXED chrome (visual pass 2026-08-29): however
	// far the page scrolls, the way to save it stays on screen.
	if (!IsMenuPage(CurPage())) PaintLeafSave(dc, rc);

	// --- The scrolling content pane -----------------------------------------
	// Clip to the pane FIRST (viewport origin still 0), then shift the origin so
	// every page can paint in document coordinates and ignore scrolling. Only the
	// CURRENT page is painted; the layout chains are disjoint.
	SaveDC(dc);
	IntersectClipRect(dc, 0, ContentY(), W, PaneBottom(rc));
	SetViewportOrgEx(dc, 0, -g_scroll, NULL);
	if (IsMenuPage(CurPage())) {
		PaintMenuPage(dc, rc);
	} else {
		switch (CurPage()) {
		case PG_SCENARIOS: PaintScenarios(dc, rc); break;
		case PG_VC:        PaintVcTarget(dc, rc); PaintVCShadows(dc, rc); PaintCamShake(dc, rc); break;
		case PG_EXHAUST:   PaintThruster(dc, rc); break;
		case PG_PARTICLES: PaintParticles(dc, rc); break;
		case PG_PLASMA:    PaintReentry(dc, rc); break;
		case PG_VAPOUR:    PaintVapour(dc, rc); break;
		case PG_FLIGHTAID: PaintFlightAid(dc, rc); break;
		case PG_RAIN:      PaintRain(dc, rc); break;
		case PG_LIGHTNING: PaintLightning(dc, rc); break;
		case PG_AURORA:    PaintAurora(dc, rc); break;
		case PG_ECLIPSE:   PaintEclipse(dc, rc); break;
		case PG_GODRAYS:   PaintGodRays(dc, rc); break;
		default:           PaintVision(dc, rc); PaintMotion(dc, rc); PaintPilot(dc, rc); break;
		}                            // default = PG_GFORCES
	}
	SetViewportOrgEx(dc, 0, 0, NULL);
	RestoreDC(dc, -1);

	// --- Scrollbar (only when there is somewhere to go) ---------------------
	if (MaxScroll(rc) > 0) {
		RECT st = ScrollTrackRect(rc);
		FillSolid(dc, st, CLR_TRACK);
		RECT sth = ScrollThumbRect(rc);
		FillSolid(dc, sth, g_dragBar >= 0 ? CLR_ACCENT : CLR_PILL_OFF);
	}

	// --- Status line (fixed) ------------------------------------------------
	RECT rSepS = { 0, H - STATUS_H, W, H - STATUS_H + 1 };
	FillSolid(dc, rSepS, CLR_LINE);
	char statusBuf[128];
	const char* status;
	bool alert = false;
	// 0 means "not a save/revert message" - fall through to the alert/resting rule at the
	// draw below. COLORREF 0 is pure black, which nothing on this panel ever draws text in,
	// so it is safe as the sentinel and keeps the other three branches untouched.
	COLORREF stClr = 0;
	if (GetTickCount() < g_saveMsgUntil) {
		// Highest priority: it is a direct answer to a click the user just made. Naming
		// the SCOPES matters - it is the difference between "saved" and "saved for this
		// hull" / "saved for this world", which is the whole point of the three-way split.
		if (!g_saveOk) {
			sprintf_s(statusBuf, "COULD NOT WRITE settings - see Orbiter.log.");
		} else {
			char what[96] = "";
			const int m = g_saveMask;
			if (m & ORO_SCOPE_GLOBAL) strcat_s(what, "global");
			if ((m & ORO_SCOPE_CLASS) && OroSettings_Class()[0]) {
				if (what[0]) strcat_s(what, " + ");
				strcat_s(what, OroSettings_Class());
			}
			if ((m & ORO_SCOPE_BODY) && OroSettings_Body()[0]) {
				if (what[0]) strcat_s(what, " + ");
				strcat_s(what, OroSettings_Body());
			}
			if (!what[0]) strcpy_s(what, "nothing yet - no vessel or world in range");
			if (g_saveWasRevert)
				sprintf_s(statusBuf, "Reverted: %s - back to the last saved values.", what);
			else
				sprintf_s(statusBuf, "Saved: %s. Reloaded automatically next time.", what);
		}
		status = statusBuf;
		alert  = !g_saveOk;
		// COLOUR-CODED TO MATCH THE BUTTON THAT WAS JUST PRESSED (see CLR_MSG_SAVE): green
		// for a write, amber for a revert, red if the write failed. It returns to the
		// resting colour on its own when this message expires a few seconds later, which is
		// exactly the moment the user is meant to stop looking down here.
		stClr  = !g_saveOk ? CLR_ACCENT : (g_saveWasRevert ? CLR_MSG_REVRT : CLR_MSG_SAVE);
	} else if (GetTickCount() < g_noteUntil) {
		// A transient info line (COPY STOCK and friends): an answer to a click, but
		// neither a save nor a revert - plain text colour on purpose, so it cannot
		// teach a false model about what was written to disk.
		status = g_noteBuf;
		stClr = CLR_TEXT;
	} else if (g_fx.seqActive >= 0) {
		if (g_fx.seqActive < NIND)
			sprintf_s(statusBuf, "INDUCING %s - ramps up and HOLDS. Recover to return.", g_indNames[g_fx.seqActive]);
		else
			sprintf_s(statusBuf, "RECOVERING FROM %s - returning to normal.", g_recNames[g_fx.seqActive - NIND]);
		status = statusBuf;
		alert = true;
	} else if (IsMenuPage(CurPage())) {
		// His spec: a general line on menu pages, the usual mode line on slider pages.
		status = "Select an option from the menu.";
	} else if (g_fx.physicsMode) {
		// Say what the sliders MEAN right now - in this mode they are gains, not values,
		// and nothing else on screen would tell you that.
		sprintf_s(statusBuf, "PHYSICS - felt G drives the effects, sliders are gains. Gz %+.2f", g_fx.feltGz);
		status = statusBuf;
		alert = (g_fx.gReserve < 0.5f);
	} else {
		status = "LAB MODE - sliders drive effects directly. Ctrl+G = master kill.";
	}
	SelectObject(dc, g_fontSmall);
	SetTextColor(dc, stClr ? stClr : (alert ? CLR_ACCENT : CLR_TEXT_DIM));
	TextOutA(dc, 16, H - 22, status, (int)strlen(status));

	// --- Colour picker overlay (fixed layer, painted LAST = topmost) --------
	if (g_pickOpen) PaintColourPicker(dc);

	// Blit out and clean up
	BitBlt(dcOut, 0, 0, W, H, dc, 0, 0, SRCCOPY);
	SelectObject(dc, oldbb);
	DeleteObject(bb);
	DeleteDC(dc);
}

// ----------------------------------------------------------------------------
// Hit-testing - one handler per section, mirroring the painters. `y` here is a
// DOCUMENT coordinate (the caller has already added g_scroll).
// ----------------------------------------------------------------------------
static BOOL ClickVision(HWND hDlg, const RECT& rc, int x, int y)
{
	if (PtIn(BlinkBtnRect(), x, y)) {
		g_fx.blinkRequest = true;   // consumed by clbkPreStep, which runs the envelope
		g_clickWasEdit = false;     // an event, not a setting
		return TRUE;
	}
	for (int i = 0; i < NVIS; i++) {
		if (PtIn(PillRectAt(VisRowY(i)), x, y, 4)) {
			*g_visRows[i].enabled = !*g_visRows[i].enabled;
			return TRUE;
		}
		if (*g_visRows[i].enabled && PtIn(TrackRectAt(rc, VisRowY(i)), x, y, 8)) {
			g_dragRow = i;
			SetCapture(hDlg);
			*RowKnob(g_visRows[i]) = TrackValueFromX(rc, x);
			return TRUE;
		}
	}
	return FALSE;
}

static BOOL ClickMotion(HWND hDlg, const RECT& rc, int x, int y)
{
	for (int i = 0; i < NMOT; i++) {
		if (PtIn(PillRectAt(MotRowY(i)), x, y, 4)) {
			*g_motRows[i].enabled = !*g_motRows[i].enabled;
			return TRUE;
		}
		if (*g_motRows[i].enabled && PtIn(TrackRectAt(rc, MotRowY(i)), x, y, 8)) {
			g_dragMot = i;
			SetCapture(hDlg);
			*RowKnob(g_motRows[i]) = TrackValueFromX(rc, x);
			return TRUE;
		}
	}
	return FALSE;
}

// CAM-SHAKE lives in the VC tab: enable pill + Test toggle + tuning sliders (track
// 0..1 -> 0..vmax).
static BOOL ClickCamShake(HWND hDlg, const RECT& rc, int x, int y)
{
	if (PtIn(PillRectAt(CamShakeTop()), x, y, 4)) {
		g_fx.shakeEnabled = !g_fx.shakeEnabled;
		if (!g_fx.shakeEnabled) g_fx.shakeTest = false;
		return TRUE;
	}
	if (PtIn(ShakeTestBtnRect(rc), x, y)) {
		g_fx.shakeTest = !g_fx.shakeTest;   // full-power preview at the tuned settings
		g_clickWasEdit = false;             // a preview, not a setting
		return TRUE;
	}
	if (g_fx.shakeEnabled) {
		for (int i = 0; i < NSHAKE; i++) {
			if (PtIn(TrackRectAt(rc, ShakeRowY(i)), x, y, 8)) {
				g_dragShake = i;
				SetCapture(hDlg);
				*g_shakeRows[i].value = TrackValueFromX(rc, x) * g_shakeRows[i].vmax;
				return TRUE;
			}
		}
	}
	return FALSE;
}

// THRUSTER tab: exhaust shimmer row (g_envRows[0]) + its bipolar offset (g_envKnobs[0]),
// then PLUME EXPANSION (g_envRows[2] + g_plumeRows + the two colour swatches).
// PARTICLES sub-tab clicks: the pill, eight spec sliders, the lighting button and
// the colour swatch. Every change is picked up by the module's settings signature
// and rebuilds the streams once - the core copied the old spec, so there is no
// other way (see OroParticles.cpp finding 1).
// Transient status-line note: an answer to a click that is neither a save nor a
// revert (COPY STOCK and friends). Plain colour at paint time, ~4.5 s.
static void SetNote(const char* fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	vsprintf_s(g_noteBuf, fmt, ap);
	va_end(ap);
	g_noteUntil = GetTickCount() + 4500;
}

// COPY STOCK (patch y) - stretch a row's top end when a stock value cannot fit, and
// return it toward the shipped range when it can. The default vmax is captured once,
// so ranges cannot ratchet up across vessels; the row's drawn fraction and its drag
// both follow vmax, so an expanded row is immediately usable.
static void PrtFitRange(int row, float v)
{
	static float vmax0[NPRT]; static bool init = false;
	if (!init) { for (int i = 0; i < NPRT; i++) vmax0[i] = g_prtRows[i].vmax; init = true; }
	g_prtRows[row].vmax = (v > vmax0[row]) ? v * 1.25f : vmax0[row];
}

// Load the vessel author's own stream definition into the sliders - a starting point
// for tuning (his ask). Successive presses cycle through the vessel's streams.
static int s_prtCopyIdx = 0;
static void ClickPrtCopyStock()
{
	const int n = OroPrt_StockSpecCount();
	// Phase B (round 2): the candidates - and these notes - answer for the SELECTION:
	// a chosen thruster narrows them to exactly its own streams (pos+dir match in
	// PrtStockForGroup), the group folds duplicates as before.
	char tgt[24];
	if (g_fx.thrThrSel >= 0) sprintf_s(tgt, "thr %d", g_fx.thrThrSel);
	else                     sprintf_s(tgt, "the %s group", OroThr_Name(g_fx.thrSel));
	if (n < 0)  { SetNote("COPY STOCK needs client patch (y) - this client cannot report stream specs."); return; }
	if (n == 0) { SetNote("No stock exhaust streams found for %s - nothing to copy.", tgt); return; }
	if (s_prtCopyIdx >= n) s_prtCopyIdx = 0;
	float sz, lf, rt, sp, sd, gr, sl; bool df, af;
	if (!OroPrt_StockSpecGet(s_prtCopyIdx, &sz, &lf, &rt, &sp, &sd, &gr, &sl, &df, &af)) return;
	// values in (floors respected), top ends stretched to fit - the long-SRB-lifetime case
	g_fx.prtSize     = max(g_prtRows[1].vmin, sz);  PrtFitRange(1, sz);
	g_fx.prtLifetime = max(g_prtRows[2].vmin, lf);  PrtFitRange(2, lf);
	g_fx.prtRate     = max(g_prtRows[3].vmin, rt);  PrtFitRange(3, rt);
	g_fx.prtSpeed    = max(g_prtRows[4].vmin, sp);  PrtFitRange(4, sp);
	g_fx.prtSpread   = max(g_prtRows[5].vmin, sd);  PrtFitRange(5, sd);
	g_fx.prtGrowth   = max(g_prtRows[6].vmin, gr);  PrtFitRange(6, gr);
	g_fx.prtSlowdown = max(g_prtRows[7].vmin, sl);  PrtFitRange(7, sl);
	g_fx.prtDiffuse  = df;
	g_fx.prtAirFade  = af;
	SetNote("Copied stock stream %d/%d for %s - %s, size %.1f m, life %.1f s.%s",
	        s_prtCopyIdx + 1, n, tgt, df ? "DIFFUSE" : "EMISSIVE", sz, lf,
	        n > 1 ? " Press again for the next." : "");
	s_prtCopyIdx = (s_prtCopyIdx + 1) % n;
}

// TEXTURE cycle (phase 1 of the picker): "" (synthesized) -> Contrail1 -> Contrail1a
// -> the user's Textures\ORO\Particles\*.dds, alphabetical. The folder is rescanned
// on every press, so dropping a file in mid-session just works; phase 2 is the
// thumbnail overlay box.
static void ClickPrtTexCycle()
{
	char names[26][48]; int n = 0;
	names[n][0] = 0; n++;                                  // the synthesized atlas
	strcpy_s(names[n++], "Contrail1");                     // stock default puffs
	strcpy_s(names[n++], "Contrail1a");                    // stock wispy smoke (the DG's)
	WIN32_FIND_DATAA fd;
	HANDLE hf = FindFirstFileA("Textures\\ORO\\Particles\\*.dds", &fd);
	if (hf != INVALID_HANDLE_VALUE) {
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			char nm[48]; strncpy_s(nm, fd.cFileName, _TRUNCATE);
			char* dot = strrchr(nm, '.'); if (dot) *dot = 0;
			if (!nm[0] || n >= 26) continue;
			// ⚠️ DEDUPE, learned from his first test: a folder COPY of Contrail1/1a
			// duplicated the built-ins, and the first-match search below then resolved
			// every press back to the built-in - the cycle orbited entries 1 and 2
			// forever and could not reach Contrail2. One name, one entry.
			bool dup = false;
			for (int i = 0; i < n && !dup; i++) dup = !_stricmp(names[i], nm);
			if (!dup) strcpy_s(names[n++], nm);
		} while (FindNextFileA(hf, &fd));
		FindClose(hf);
	}
	for (int i = 3; i < n; i++)                            // alphabetise the folder entries
		for (int j = i + 1; j < n; j++)
			if (_stricmp(names[i], names[j]) > 0) {
				char t[48]; strcpy_s(t, names[i]); strcpy_s(names[i], names[j]); strcpy_s(names[j], t);
			}
	int cur = 0;
	for (int i = 0; i < n; i++) if (!_stricmp(names[i], g_fx.prtTexName)) { cur = i; break; }
	const int nxt = (cur + 1) % n;
	strcpy_s(g_fx.prtTexName, names[nxt]);
	if (names[nxt][0])
		SetNote("Particle texture: %s (%d/%d). Files must be 2x2 atlases - see the folder README.",
		        names[nxt], nxt + 1, n);
	else
		SetNote("Particle texture: ORO synthesized (%d/%d).", nxt + 1, n);
}

// The EXHAUST page's twin: the stock-flame preset. Needs no client capability - the
// plume's base dimensions already COME from the vessel's own exhaust definitions
// (BuildPlumeModel reads lsize/wsize), so "copy stock" is the neutral multipliers
// with everything stock's flame does not have switched off.
static void ClickPlmCopyStock()
{
	// Values only, deliberately: with a thruster selected the central MarkDirty hook
	// scopes this into that thruster's exhaust override; at ALL it edits the group.
	g_fx.plumeWidth    = 1.0f;
	g_fx.plumeLen      = 1.0f;
	g_fx.plumeDiamond  = 0.0f;
	g_fx.plumeBloomBri = 0.0f;
	g_fx.plumeThroat   = 0.0f;
	g_fx.plumeSoot     = 0.0f;
	if (g_fx.thrThrSel >= 0)
		SetNote("Stock flame for thr %d: width/length 1.0 (its exhaust definition's own size); "
		        "diamonds, bloom, throat, soot off.", g_fx.thrThrSel);
	else
		SetNote("Stock flame: width/length 1.0 (the exhaust definition's own size); diamonds, bloom, throat, soot off.");
}

static BOOL ClickParticles(HWND hDlg, const RECT& rc, int x, int y)
{
	// THE TWO PILLS ARE MUTUALLY EXCLUSIVE (his design): stock ON means you are
	// flying whatever the vessel author designed; ours ON means our own streams,
	// shaped by these sliders. INDEPENDENT of the stock pill since 2026-08-29 (his
	// ruling: "some users might want both" - ORO's job is to save the combination
	// the user set and load it back, not to arbitrate). Every pairing is legal.
	if (PtIn(PillRectAt(PrtHdrY() + 7), x, y, 4)) {
		g_fx.prtEnabled = !g_fx.prtEnabled;
		return TRUE;
	}
	// Fully independent of ours (2026-08-29): stock's streams and ORO's may fly
	// together, either alone, or neither. The pill means exactly what it says, and
	// SAVE keeps whichever combination is set.
	if (OroStockExhaustSupported() && PtIn(PillRectAt(PrtStkY()), x, y, 4)) {
		g_fx.stockParticles = !g_fx.stockParticles;
		return TRUE;
	}
	// CANCEL THRUST - the EXHAUST page's test-stand rig, mirrored (one flag, two
	// doors). Always clickable like its twin (it flies the ship, so no effect-pill
	// gating applies), which is why it sits ABOVE the prtEnabled gate.
	if (PtIn(PillRectAt(PrtCthY()), x, y, 4)) {
		g_fx.cancelThrust = !g_fx.cancelThrust;
		g_clickWasEdit = false;   // session-only BY DESIGN (23i) - ambering the saves
		return TRUE;              //   for it would claim it can be saved, which it cannot
	}
	if (!g_fx.prtEnabled) return FALSE;
	// COPY STOCK (patch y): an EDIT of saved values, so it ambers the saves like a
	// slider drag (g_clickWasEdit stays true on purpose).
	if (PtIn(RowBtnRect(rc, PrtCopyY()), x, y)) { ClickPrtCopyStock(); return TRUE; }
	for (int i = 0; i < NPRT; i++) {
		if (PtIn(TrackRectAt(rc, PrtRowY(i)), x, y, 8)) {
			g_dragPrt = i;
			SetCapture(hDlg);
			*g_prtRows[i].value = g_prtRows[i].vmin
			                    + TrackValueFromX(rc, x) * (g_prtRows[i].vmax - g_prtRows[i].vmin);
			return TRUE;
		}
	}
	if (PtIn(RowBtnRect(rc, PrtLightY()), x, y)) {
		g_fx.prtDiffuse = !g_fx.prtDiffuse;
		return TRUE;
	}
	if (PtIn(RowBtnRect(rc, PrtAirY()), x, y)) {
		g_fx.prtAirFade = !g_fx.prtAirFade;
		return TRUE;
	}
	if (OroParticleTintOK()) {
		// swatches only while STOCK is off (they are greyed while it is on)
		if (!g_fx.prtTexStock && PtIn(SwatchRect(PrtColY(), 0), x, y)) {
			OpenColourPicker(hDlg, g_fx.prtColour, PrtColY());
			return TRUE;
		}
		if (!g_fx.prtTexStock && PtIn(SwatchRect(PrtColY(), 1), x, y)) {
			OpenColourPicker(hDlg, g_fx.prtColour2, PrtColY());
			return TRUE;
		}
		RECT sbtn = RowBtnRect(rc, PrtColY()); sbtn.left = SwatchRect(PrtColY(), 1).right + 12;
		if (PtIn(sbtn, x, y)) {
			g_fx.prtTexStock = !g_fx.prtTexStock;   // a saved value - ambers
			return TRUE;
		}
		// TEXTURE cycle - a saved value, so it ambers like a slider.
		if (PtIn(RowBtnRect(rc, PrtTexY()), x, y)) {
			ClickPrtTexCycle();
			return TRUE;
		}
	}
	return FALSE;
}

static BOOL ClickThruster(HWND hDlg, const RECT& rc, int x, int y)
{
	if (PtIn(PillRectAt(ThrRowY()), x, y, 4)) {
		g_fx.shimmerEnabled = !g_fx.shimmerEnabled;
		return TRUE;
	}
	if (g_fx.shimmerEnabled && PtIn(TrackRectAt(rc, ThrRowY()), x, y, 8)) {
		g_dragEnv = 0;
		SetCapture(hDlg);
		*RowKnob(g_envRows[0]) = TrackValueFromX(rc, x);
		return TRUE;
	}
	if (g_fx.shimmerEnabled && PtIn(TrackRectAt(rc, ThrOfsY()), x, y, 8)) {
		g_dragEnvK = 0;
		SetCapture(hDlg);
		*g_envKnobs[0].value = EnvKnobValueFromX(rc, x, g_envKnobs[0].vmax);
		return TRUE;
	}
	// PLUME EXPANSION.
	// The LAB | PHYSICS switch - clickable regardless of the pill (mode is a
	// statement about how the family runs, like the plasma's VC pre-arm).
	if (PtIn(PlmModeBtnRect(rc), x, y)) {
		g_fx.plumePhysics = !g_fx.plumePhysics;
		return TRUE;
	}
	if (PtIn(PillRectAt(PlmRowY()), x, y, 4)) {
		g_fx.plumeEnabled = !g_fx.plumeEnabled;
		return TRUE;
	}
	if (g_fx.plumeEnabled) {
		if (PtIn(TrackRectAt(rc, PlmRowY()), x, y, 8)) {
			g_dragEnv = 2;
			SetCapture(hDlg);
			*RowKnob(g_envRows[2]) = TrackValueFromX(rc, x);
			return TRUE;
		}
		// COPY STOCK: the stock-flame preset (an edit - it ambers the saves).
		if (PtIn(RowBtnRect(rc, PlmCopyY()), x, y)) { ClickPlmCopyStock(); return TRUE; }
		// EXPANSION BAND dual slider: grab whichever handle is nearer the click.
		if (PtIn(TrackRectAt(rc, PlmRangeY()), x, y, 8)) {
			const float f  = TrackValueFromX(rc, x);
			const float fl = g_fx.plumeExpLo / PLMB_LPMAX;
			const float fh = g_fx.plumeExpHi / PLMB_LPMAX;
			g_dragPlmBand = (fabsf(f - fl) <= fabsf(f - fh)) ? 0 : 1;
			SetCapture(hDlg);
			PlmBandDrag(f);
			return TRUE;
		}
		for (int i = 0; i < NPLM; i++) {
			if (PtIn(TrackRectAt(rc, PlmSldY(i)), x, y, 8)) {
				g_dragPlume = i;
				SetCapture(hDlg);
				*g_plumeRows[i].value = g_plumeRows[i].vmin
				                      + TrackValueFromX(rc, x) * (g_plumeRows[i].vmax - g_plumeRows[i].vmin);
				return TRUE;
			}
		}
		// Jet / Bloom swatches -> native picker (modal).
		if (PtIn(SwatchRect(PlmColY(), 0), x, y)) { OpenColourPicker(hDlg, g_fx.plumeColJet,   PlmColY()); return TRUE; }
		if (PtIn(SwatchRect(PlmColY(), 1), x, y)) { OpenColourPicker(hDlg, g_fx.plumeColBloom, PlmColY()); return TRUE; }
	}
	// BELL GLOW - the pill, then the three sliders (strength + the two thermal
	// timescales), all inert while the pill is off. Independent of the plume
	// pill: its own effect, the mesh file is the real opt-in.
	// Phase B: the whole bell family is GROUP-LEVEL (v1) - the pill flags itself so
	// MarkDirty routes it to the group instead of creating a thruster override; the
	// three sliders are identified by g_dragBgl and the tint picker by its target.
	if (PtIn(PillRectAt(BglRowY()), x, y, 4)) {
		g_fx.plumeBellOn = !g_fx.plumeBellOn;
		g_editGrpLevel = true;
		return TRUE;
	}
	if (g_fx.plumeBellOn) {
		if (PtIn(TrackRectAt(rc, BglRowY()), x, y, 8)) {
			g_dragBgl = 0;
			SetCapture(hDlg);
			g_fx.plumeBellGlow = TrackValueFromX(rc, x) * 2.0f;
			return TRUE;
		}
		if (PtIn(TrackRectAt(rc, BglHeatY()), x, y, 8)) {
			g_dragBgl = 1;
			SetCapture(hDlg);
			g_fx.plumeBellHeatT = 1.0f + TrackValueFromX(rc, x) * 19.0f;
			return TRUE;
		}
		if (PtIn(TrackRectAt(rc, BglCoolY()), x, y, 8)) {
			g_dragBgl = 2;
			SetCapture(hDlg);
			g_fx.plumeBellCoolT = 5.0f + TrackValueFromX(rc, x) * 115.0f;
			return TRUE;
		}
		if (PtIn(SwatchRect(BglColY(), 0), x, y)) { OpenColourPicker(hDlg, g_fx.bellTint, BglColY()); return TRUE; }
	}
	// STOCK EXHAUST pill (patch n) - inert without the capability (invariant 18b:
	// a switch that cannot do anything is worse than no switch).
	if (OroStockExhaustSupported() && PtIn(PillRectAt(StkPillY()), x, y, 4)) {
		g_fx.stockExhaust = !g_fx.stockExhaust;
		return TRUE;
	}
	// CANCEL THRUST pill - the test-stand rig, always clickable (it flies the ship,
	// like the flight aid, so no effect-pill gating applies).
	if (PtIn(PillRectAt(CthPillY()), x, y, 4)) {
		g_fx.cancelThrust = !g_fx.cancelThrust;
		g_clickWasEdit = false;   // session-only BY DESIGN (23i) - ambering the saves
		return TRUE;              //   for it would claim it can be saved, which it cannot
	}
	return FALSE;
}

// REENTRY tab: the plasma row (g_envRows[1]) + VC toggle + PLASMA TUNING sliders.
static BOOL ClickReentry(HWND hDlg, const RECT& rc, int x, int y)
{
	if (PtIn(PillRectAt(ReeRowY()), x, y, 4)) {
		g_fx.reentryEnabled = !g_fx.reentryEnabled;
		return TRUE;
	}
	if (g_fx.reentryEnabled && PtIn(TrackRectAt(rc, ReeRowY()), x, y, 8)) {
		g_dragEnv = 1;
		SetCapture(hDlg);
		*RowKnob(g_envRows[1]) = TrackValueFromX(rc, x);
		return TRUE;
	}
	// VC toggle - clickable regardless of the pill so it can be pre-armed.
	if (PtIn(PlasVCBtnRect(rc), x, y)) {
		g_fx.reentryVC = !g_fx.reentryVC;
		return TRUE;
	}
	// PLASMA TUNING sliders (track 0..1 -> 0..vmax, same mapping as CAM-SHAKE).
	if (g_fx.reentryEnabled) {
		for (int i = 0; i < NPLAS; i++) {
			if (PtIn(TrackRectAt(rc, PlasRowY(i)), x, y, 8)) {
				g_dragPlas = i;
				SetCapture(hDlg);
				*g_plasRows[i].value = g_plasRows[i].vmin
				                     + TrackValueFromX(rc, x) * (g_plasRows[i].vmax - g_plasRows[i].vmin);
				return TRUE;
			}
		}
		// Plasma tint swatches -> the in-panel picker (non-modal, live preview).
		if (PtIn(SwatchRect(PlasTintY(), 0), x, y)) { OpenColourPicker(hDlg, g_fx.plasmaTint,  PlasTintY());  return TRUE; }
		if (PtIn(SwatchRect(PlasTintY(), 1), x, y)) { OpenColourPicker(hDlg, g_fx.plasmaTint2, PlasTintY());  return TRUE; }
		// Trail head/tail swatches, same picker.
		if (PtIn(SwatchRect(PlasTrailTintY(), 0), x, y)) { OpenColourPicker(hDlg, g_fx.plasTrailTint,  PlasTrailTintY()); return TRUE; }
		if (PtIn(SwatchRect(PlasTrailTintY(), 1), x, y)) { OpenColourPicker(hDlg, g_fx.plasTrailTint2, PlasTrailTintY()); return TRUE; }
	}
	return FALSE;
}

// ECLIPSE. The Test toggle stays clickable regardless of the pill, like the plasma's
// VC button: it is how you find out whether the section works at all, so it must not
// be gated behind the thing you are trying to test.
static BOOL ClickEclipse(HWND hDlg, const RECT& rc, int x, int y)
{
	if (PtIn(PillRectAt(EclPillY()), x, y, 4)) {
		g_fx.eclipseEnabled = !g_fx.eclipseEnabled;
		if (!g_fx.eclipseEnabled) g_fx.eclipseTest = false;
		return TRUE;
	}
	if (PtIn(EclTestBtnRect(rc), x, y)) {
		g_fx.eclipseTest = !g_fx.eclipseTest;
		g_clickWasEdit = false;             // a preview, not a setting
		return TRUE;
	}
	if (g_fx.eclipseEnabled) {
		for (int i = 0; i < NECL; i++) {
			if (PtIn(TrackRectAt(rc, EclRowY(i)), x, y, 8)) {
				g_dragEcl = i;
				SetCapture(hDlg);
				*g_eclRows[i].value = TrackValueFromX(rc, x) * g_eclRows[i].vmax;
				return TRUE;
			}
		}
	}
	return FALSE;
}

// AURORA. Test stays clickable regardless of the pill, like the eclipse's - it is how
// you find out whether the section works at all.
static BOOL ClickAurora(HWND hDlg, const RECT& rc, int x, int y)
{
	if (PtIn(PillRectAt(AurPillY()), x, y, 4)) {
		g_fx.auroraEnabled = !g_fx.auroraEnabled;
		if (!g_fx.auroraEnabled) g_fx.auroraTest = false;
		return TRUE;
	}
	if (PtIn(AurTestBtnRect(rc), x, y)) {
		g_fx.auroraTest = !g_fx.auroraTest;
		g_clickWasEdit = false;             // a preview, not a setting
		return TRUE;
	}
	if (g_fx.auroraEnabled) {
		for (int i = 0; i < NAUR; i++) {
			if (PtIn(TrackRectAt(rc, AurRowY(i)), x, y, 8)) {
				g_dragAur = i;
				SetCapture(hDlg);
				*g_aurRows[i].value = TrackValueFromX(rc, x) * g_aurRows[i].vmax;
				return TRUE;
			}
		}
		// Ribbons slider (1..6, integer snap; drag-capable).
		if (PtIn(TrackRectAt(rc, AurRibY()), x, y, 8)) {
			g_dragAurRib = 0;
			SetCapture(hDlg);
			const int n = 1 + (int)(TrackValueFromX(rc, x) * 5.0f + 0.5f);
			g_fx.auroraRibbons = (n < 1) ? 1 : (n > 6 ? 6 : n);
			return TRUE;
		}
		// Magnetic-pole tilt knobs (bipolar, snap-to-zero at the geographic pole).
		for (int i = 0; i < NAURK; i++) {
			if (PtIn(TrackRectAt(rc, AurKnobY(i)), x, y, 8)) {
				g_dragAurK = i;
				SetCapture(hDlg);
				*g_aurKnobs[i].value = EnvKnobValueFromX(rc, x, g_aurKnobs[i].vmax);
				return TRUE;
			}
		}
		// Colour swatches -> the in-panel picker, in altitude order: base, body, top.
		if (PtIn(SwatchRect(AurColY(), 0), x, y)) { OpenColourPicker(hDlg, g_fx.auroraColBase, AurColY()); return TRUE; }
		if (PtIn(SwatchRect(AurColY(), 1), x, y)) { OpenColourPicker(hDlg, g_fx.auroraColBody, AurColY()); return TRUE; }
		if (PtIn(SwatchRect(AurColY(), 2), x, y)) { OpenColourPicker(hDlg, g_fx.auroraColTop,  AurColY()); return TRUE; }
	}
	return FALSE;
}

// LIGHTNING. Test stays clickable regardless of the pill, like the eclipse's and the
// aurora's - it is how you find out whether the section works at all.
static BOOL ClickLightning(HWND hDlg, const RECT& rc, int x, int y)
{
	if (PtIn(PillRectAt(LtgPillY()), x, y, 4)) {
		g_fx.ltgEnabled = !g_fx.ltgEnabled;
		if (!g_fx.ltgEnabled) g_fx.ltgTest = false;
		return TRUE;
	}
	if (PtIn(LtgTestBtnRect(rc), x, y)) {
		g_fx.ltgTest = !g_fx.ltgTest;
		g_clickWasEdit = false;             // a preview, not a setting
		return TRUE;
	}
	if (g_fx.ltgEnabled) {
		for (int i = 0; i < NLTG; i++) {
			if (PtIn(TrackRectAt(rc, LtgRowY(i)), x, y, 8)) {
				g_dragLtg = i;
				SetCapture(hDlg);
				*g_ltgRows[i].value = TrackValueFromX(rc, x) * g_ltgRows[i].vmax;
				return TRUE;
			}
		}
		if (PtIn(SwatchRect(LtgColY(), 0), x, y)) { OpenColourPicker(hDlg, g_fx.ltgColour, LtgColY()); return TRUE; }
	}
	// IN THE STORM - the rain system's rows, gated exactly as they were on the RAIN
	// page (live while the storm can exist: the pill or its Test).
	if (g_fx.rainEnabled || g_fx.rainTest) {
		if (PtIn(RowBtnRect(rc, RltBoltY()), x, y, 2)) {
			g_fx.boltTestFire = true;          // consumed by UpdateRain next step
			g_clickWasEdit = false;            // the test rig, not a setting
			InvalidateRect(hDlg, NULL, FALSE);
			return TRUE;
		}
		for (int i = 0; i < NRLT; i++) {
			if (PtIn(TrackRectAt(rc, RltRowY(i)), x, y, 8)) {
				g_dragRlt = i;
				SetCapture(hDlg);
				*g_rltRows[i].value = TrackValueFromX(rc, x) * g_rltRows[i].vmax;
				return TRUE;
			}
		}
	}
	return FALSE;
}

// GOD RAYS. No colour swatch: the shafts take their colour from the light that is
// actually in the frame (and warm with the Warmth knob as the sun drops), so a picker
// would be inventing a hue the sky does not have.
static BOOL ClickGodRays(HWND hDlg, const RECT& rc, int x, int y)
{
	if (PtIn(PillRectAt(GryPillY()), x, y, 4)) {
		g_fx.grayEnabled = !g_fx.grayEnabled;
		if (!g_fx.grayEnabled) g_fx.grayTest = false;
		return TRUE;
	}
	if (PtIn(GryTestBtnRect(rc), x, y)) {
		g_fx.grayTest = !g_fx.grayTest;
		g_clickWasEdit = false;             // a preview, not a setting
		return TRUE;
	}
	if (g_fx.grayEnabled) {
		for (int i = 0; i < NGRY; i++) {
			if (PtIn(TrackRectAt(rc, GryRowY(i)), x, y, 8)) {
				g_dragGry = i;
				SetCapture(hDlg);
				*g_gryRows[i].value = TrackValueFromX(rc, x) * g_gryRows[i].vmax;
				return TRUE;
			}
		}
	}
	return FALSE;
}

// VC SHADOWS. Inert on a client without patch (f) - nothing here would reach anything.
static BOOL ClickVCShadows(HWND hDlg, const RECT& rc, int x, int y)
{
	if (!OroVCShadowsSupported()) return FALSE;
	if (PtIn(PillRectAt(VcsPillY()), x, y, 4)) {
		g_fx.vcShadows = !g_fx.vcShadows;
		return TRUE;
	}
	// g_dragVcs identifies WHICH slider now that there are two: 0 = cabin box,
	// 1 = shadow depth. It used to be a bare "1 = dragging" flag.
	if (g_fx.vcShadows && PtIn(TrackRectAt(rc, VcsRadY()), x, y, 8)) {
		g_dragVcs = 0;
		SetCapture(hDlg);
		g_fx.vcShadowRadius = VCS_RAD_MIN + TrackValueFromX(rc, x) * (VCS_RAD_MAX - VCS_RAD_MIN);
		return TRUE;
	}
	if (g_fx.vcShadows && PtIn(TrackRectAt(rc, VcsDepY()), x, y, 8)) {
		g_dragVcs = 1;
		SetCapture(hDlg);
		g_fx.vcShadowDepth = TrackValueFromX(rc, x);
		return TRUE;
	}
	return FALSE;
}

// PILOT. The mode switch and the model's assumptions stay live at all times - you must
// be able to get out of PHYSICS mode without first waiting for something to finish.
static BOOL ClickPilot(HWND hDlg, const RECT& rc, int x, int y)
{
	if (PtIn(PilotBtnRect(PilRowY(0), 110), x, y)) {
		g_fx.physicsMode = !g_fx.physicsMode;
		// Entering PHYSICS, the sliders stop being values and start being gains. If they
		// were left at 0 from lab work the model would drive everything to nothing and
		// look broken - so any gain still at zero comes up at full.
		if (g_fx.physicsMode) {
			float* gains[] = { &g_fx.gainBlackout, &g_fx.gainRedout, &g_fx.gainTunnel,
			                   &g_fx.gainSpots, &g_fx.gainGreyout, &g_fx.gainBlur,
			                   &g_fx.gainHeartbeat, &g_fx.gainAberration, &g_fx.gainSparkles,
			                   &g_fx.gainSwim, &g_fx.gainTilt };
			for (int i = 0; i < (int)(sizeof(gains) / sizeof(gains[0])); i++)
				if (*gains[i] <= 0.001f) *gains[i] = 1.0f;
		}
		return TRUE;
	}
	// SAVE TARGET. Flipping it changes nothing on disk by itself - it decides where the
	// NEXT save goes, and the tab's caption above updates to name the file, so the button
	// and the caption always agree before anything is written.
	if (PtIn(PilotBtnRect(PilRowY(1), 150), x, y)) {
		g_fx.pilotPerClass = !g_fx.pilotPerClass;
		return TRUE;
	}
	if (PtIn(PilotBtnRect(PilRowY(3), 78), x, y)) {
		g_fx.gsuitOn = !g_fx.gsuitOn;
		return TRUE;
	}
	if (PtIn(PilotBtnRect(PilRowY(4), 110), x, y)) {
		const int n = OroPhys_PoseCount();
		g_fx.pilotPose = (g_fx.pilotPose + 1) % (n > 0 ? n : 1);
		return TRUE;
	}
	if (PtIn(PilotBtnRect(PilRowY(5), 110), x, y)) {
		g_fx.gRefCamera = !g_fx.gRefCamera;
		return TRUE;
	}
	if (PtIn(PilotBtnRect(PilRowY(6), 110), x, y)) {
		g_fx.fxVCOnly = !g_fx.fxVCOnly;
		return TRUE;
	}
	if (PtIn(TrackRectAt(rc, PilRowY(2)), x, y, 8)) {
		g_dragTol = 1;
		SetCapture(hDlg);
		g_fx.gTolerance = TrackValueFromX(rc, x);
		return TRUE;
	}
	return FALSE;
}

// THE VAPOUR CONE. Two unit sliders plus the bipolar apex knob, which needs its own drag
// id: g_dragVap identifies the slider, g_dragVapP the knob. The VC section's bare
// "1 = dragging" flag collided the moment a second slider arrived, and that is a bug
// worth not repeating in a section that ships with two from the start.
static BOOL ClickVapour(HWND hDlg, const RECT& rc, int x, int y)
{
	if (PtIn(PillRectAt(VapPillY()), x, y, 4)) {
		g_fx.vapEnabled = !g_fx.vapEnabled;
		if (!g_fx.vapEnabled) g_fx.vapTest = false;
		return TRUE;
	}
	if (PtIn(VapTestBtnRect(rc), x, y)) {
		g_fx.vapTest = !g_fx.vapTest;
		g_clickWasEdit = false;             // a preview, not a setting
		return TRUE;
	}
	if (g_fx.vapEnabled) {
		for (int i = 0; i < NVAP; i++) {
			if (PtIn(TrackRectAt(rc, VapRowY(i)), x, y, 8)) {
				g_dragVap = i;
				SetCapture(hDlg);
				*g_vapRows[i].value = TrackValueFromX(rc, x) * g_vapRows[i].vmax;
				return TRUE;
			}
		}
		if (PtIn(SwatchRect(VapColY(), 0), x, y)) { OpenColourPicker(hDlg, g_fx.vapColour,    VapColY()); return TRUE; }
		if (PtIn(SwatchRect(VapColY(), 1), x, y)) { OpenColourPicker(hDlg, g_fx.vapStreakCol, VapColY()); return TRUE; }
		if (PtIn(PillRectAt(VapBaseY()), x, y, 4)) {
			g_fx.vapBaseOn = !g_fx.vapBaseOn;
			return TRUE;
		}
		// FULL PLACEMENT (2026-08-30): the three position knobs in x-y-z order, then
		// the axis tilt. Each gets its own drag id - the VC section's bare-flag
		// collision, not repeated (the comment above this function's name).
		if (PtIn(TrackRectAt(rc, VapPosXY()), x, y, 8)) {
			g_dragVapP = 3;
			SetCapture(hDlg);
			g_fx.vapPosX = EnvKnobValueFromX(rc, x, VAP_POS_MAX);
			return TRUE;
		}
		if (PtIn(TrackRectAt(rc, VapPosYY()), x, y, 8)) {
			g_dragVapP = 4;
			SetCapture(hDlg);
			g_fx.vapPosY = EnvKnobValueFromX(rc, x, VAP_POS_MAX);
			return TRUE;
		}
		if (PtIn(TrackRectAt(rc, VapPosY()), x, y, 8)) {
			g_dragVapP = 1;
			SetCapture(hDlg);
			g_fx.vapPos = EnvKnobValueFromX(rc, x, VAP_POS_MAX);
			return TRUE;
		}
		if (PtIn(TrackRectAt(rc, VapPitchY()), x, y, 8)) {
			g_dragVapR = 1;
			SetCapture(hDlg);
			g_fx.vapPitch = EnvKnobValueFromX(rc, x, VAP_ROT_MAX);
			return TRUE;
		}
		if (PtIn(TrackRectAt(rc, VapYawY()), x, y, 8)) {
			g_dragVapR = 2;
			SetCapture(hDlg);
			g_fx.vapYaw = EnvKnobValueFromX(rc, x, VAP_ROT_MAX);
			return TRUE;
		}
		// The Mach band: grab whichever handle the click is nearer, the EXPANSION BAND's
		// rule exactly. Without it the two handles fight over clicks near the middle and
		// the window can only ever be widened from whichever side happens to win.
		if (PtIn(TrackRectAt(rc, VapBandY()), x, y, 8)) {
			const float f  = TrackValueFromX(rc, x);
			const float fl = (g_fx.vapMachMin - VAPB_MLO) / (VAPB_MHI - VAPB_MLO);
			const float fh = (g_fx.vapMachMax - VAPB_MLO) / (VAPB_MHI - VAPB_MLO);
			g_dragVapBand = (fabsf(f - fl) <= fabsf(f - fh)) ? 0 : 1;
			SetCapture(hDlg);
			VapBandDrag(f);
			return TRUE;
		}
		// CONE 2 - the identical set, its own state (2026-08-29).
		for (int i = 0; i < NVAP; i++) {
			if (PtIn(TrackRectAt(rc, VapRow2Y(i)), x, y, 8)) {
				g_dragVap2 = i;
				SetCapture(hDlg);
				*g_vapRows2[i].value = TrackValueFromX(rc, x) * g_vapRows2[i].vmax;
				return TRUE;
			}
		}
		if (PtIn(SwatchRect(VapCol2Y(), 0), x, y)) { OpenColourPicker(hDlg, g_fx.vapColour2,    VapCol2Y()); return TRUE; }
		if (PtIn(SwatchRect(VapCol2Y(), 1), x, y)) { OpenColourPicker(hDlg, g_fx.vapStreakCol2, VapCol2Y()); return TRUE; }
		if (PtIn(PillRectAt(VapBase2Y()), x, y, 4)) {
			g_fx.vapBaseOn2 = !g_fx.vapBaseOn2;
			return TRUE;
		}
		if (PtIn(TrackRectAt(rc, VapPosX2Y()), x, y, 8)) {
			g_dragVapP = 5;
			SetCapture(hDlg);
			g_fx.vapPosX2 = EnvKnobValueFromX(rc, x, VAP_POS_MAX);
			return TRUE;
		}
		if (PtIn(TrackRectAt(rc, VapPosY2Y()), x, y, 8)) {
			g_dragVapP = 6;
			SetCapture(hDlg);
			g_fx.vapPosY2 = EnvKnobValueFromX(rc, x, VAP_POS_MAX);
			return TRUE;
		}
		if (PtIn(TrackRectAt(rc, VapPos2Y()), x, y, 8)) {
			g_dragVapP = 2;
			SetCapture(hDlg);
			g_fx.vapPos2 = EnvKnobValueFromX(rc, x, VAP_POS_MAX);
			return TRUE;
		}
		if (PtIn(TrackRectAt(rc, VapPitch2Y()), x, y, 8)) {
			g_dragVapR = 3;
			SetCapture(hDlg);
			g_fx.vapPitch2 = EnvKnobValueFromX(rc, x, VAP_ROT_MAX);
			return TRUE;
		}
		if (PtIn(TrackRectAt(rc, VapYaw2Y()), x, y, 8)) {
			g_dragVapR = 4;
			SetCapture(hDlg);
			g_fx.vapYaw2 = EnvKnobValueFromX(rc, x, VAP_ROT_MAX);
			return TRUE;
		}
		if (PtIn(TrackRectAt(rc, VapBand2Y()), x, y, 8)) {
			const float f  = TrackValueFromX(rc, x);
			const float fl = (g_fx.vapMachMin2 - VAPB_MLO) / (VAPB_MHI - VAPB_MLO);
			const float fh = (g_fx.vapMachMax2 - VAPB_MLO) / (VAPB_MHI - VAPB_MLO);
			g_dragVapBand2 = (fabsf(f - fl) <= fabsf(f - fh)) ? 0 : 1;
			SetCapture(hDlg);
			VapBandDragC(f, g_dragVapBand2, g_fx.vapMachMin2, g_fx.vapMachMax2);
			return TRUE;
		}
	}
	return FALSE;
}

// FLIGHT AID - one bipolar knob. Snap-to-zero at the centre is what makes it safe to
// reach for mid-entry: the neutral position is a place you can hit, not a number.
static BOOL ClickFlightAid(HWND hDlg, const RECT& rc, int x, int y)
{
	if (PtIn(TrackRectAt(rc, AidKnobY()), x, y, 8)) {
		g_dragCop = 1;
		SetCapture(hDlg);
		g_fx.copShift = EnvKnobValueFromX(rc, x, COP_MAX);
		return TRUE;
	}
	if (PtIn(RowBtnRect(rc, AidGateY()), x, y)) {
		g_fx.copReentryOnly = !g_fx.copReentryOnly;
		return TRUE;
	}
	return FALSE;
}

// The scenario buttons and the SOUND toggle deliberately bypass the manual-input
// lock (you must be able to stop or mute a running scenario).
static BOOL ClickScenarios(const RECT& rc, int x, int y)
{
	if (g_fx.physicsMode) return FALSE;   // lab-only; the model owns the values
	for (int i = 0; i < NIND; i++) {
		if (PtIn(IndBtnRect(i), x, y)) {
			g_fx.seqRequest = i;        // module plays/toggles it in clbkPreStep
			g_clickWasEdit = false;     // an event, not a setting (the sound toggle
			return TRUE;                //   below IS one, and marks by default)
		}
	}
	for (int i = 0; i < NREC; i++) {
		if (PtIn(RecBtnRect(i), x, y)) {
			g_fx.seqRequest = NIND + i; // recover scenarios follow the induce ones
			g_clickWasEdit = false;
			return TRUE;
		}
	}
	if (PtIn(ScenSoundBtnRect(rc), x, y)) {
		g_fx.seqSoundEnabled = !g_fx.seqSoundEnabled;
		return TRUE;
	}
	return FALSE;
}

// ============================================================================
// THE HELP WINDOW (2026-08-16)
// ----------------------------------------------------------------------------
// A second Orbiter-managed dialog, opened from the HELP button in the fixed strip and
// showing the help for WHICHEVER TAB WAS ACTIVE when it was pressed. Same dark palette
// and same owner-drawn approach as the panel; no banner, resizable in both axes.
//
// WHY AN ORBITER DIALOG AND NOT A RAW CreateWindowEx: the same reason the panel is one.
// oapiOpenDialog windows work in fullscreen, get Orbiter's caption skinning, and - the
// lesson from this morning - are pumped by OrbiterDefDialogProc's frame timer while
// being dragged. A hand-rolled window would have to re-earn all three.
//
// TEXT LAYOUT IS MEASURED, NOT TABULATED. Every block is word-wrapped with DT_WORDBREAK
// and its height taken from DT_CALCRECT at the CURRENT width, so resizing genuinely
// reflows rather than clipping - which is the only thing that makes a resizable help
// window worth having. The same walk both measures and draws (bMeasure), so the scroll
// range can never disagree with what is on screen.
// ============================================================================

static int   g_helpTab   = 0;       // which PAGE's text it is showing - a PG_* id since the
                                    // menu rework (g_hHelp is declared with the other window
                                    // handles at the top of the file)
static int   g_helpScroll = 0;      // px
static int   g_helpDoc   = 0;       // measured document height at the last paint
static int   g_helpDragBar = -1;    // scrollbar thumb grab offset, -1 = none
static int   g_helpSavedW = 0, g_helpSavedH = 0;   // last size written to window.cfg

static const int HELP_W_DEF = 520, HELP_H_DEF = 660;
static const int HELP_W_MIN = 380, HELP_H_MIN = 300;
static const int HELP_PAD   = 20;   // left/right margin
static const int HELP_TOP   = 16;

// ----------------------------------------------------------------------------
// THE HELP WINDOW HAS ITS OWN FONTS, AND THAT IS THE POINT (2026-08-16).
// It first borrowed the panel's -13/-11 pair, which is the wrong instinct dressed up as
// consistency. Those two sizes are small for TWO reasons, and NEITHER of them applies
// here: the panel has to fit ~90 controls across five tabs, and it has to stay small
// enough that you can still see the sim you are tuning behind it. A help window fits
// nothing and hides nothing - it is read, at length, and then closed. So it is sized for
// READING and the panel keeps its own scale.
// The hierarchy is carried by weight as much as size, so the three levels stay distinct
// without the headings becoming shouty.
static HFONT g_hfTitle = NULL;      // the small accent tab name at the top
static HFONT g_hfHead  = NULL;      // section headings
static HFONT g_hfName  = NULL;      // a control's name
static HFONT g_hfBody  = NULL;      // running text - the size that actually matters

static void CreateHelpFontsOnce()
{
	if (g_hfBody) return;
	g_hfTitle = CreateFontA(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
	g_hfHead  = CreateFontA(-21, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
	g_hfName  = CreateFontA(-18, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
	g_hfBody  = CreateFontA(-17, 0, 0, 0, FW_NORMAL,   0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
}

// Block kinds. Deliberately few: a heading, a named control with its explanation, a
// running paragraph, and a spacer. Anything richer would be a layout engine.
enum { HK_H = 0, HK_ROW, HK_P, HK_GAP };
struct HelpItem { int kind; const char* a; const char* b; };

// ============================================================================
// THE HELP TEXTS, ONE PER PAGE (2026-08-29, the menu rework - his spec: pressing
// HELP on a MENU describes what each sub-menu controls; pressing it on a page of
// sliders explains each slider, pill and swatch. All tied to where you stand).
// Written against the code, not from memory: thresholds quoted are the ones in
// the sources, and the axis a row answers to is the one the readouts name.
// ============================================================================

// --- MENU: MAIN -------------------------------------------------------------
static const HelpItem HELP_M_MAIN[] = {
{ HK_H,   "THE MAIN MENU", NULL },
{ HK_P,   "ORO's whole control panel, organised as three doors. Everything behind them is "
          "an effect you can watch respond live as you drag - there is no apply button "
          "anywhere.", NULL },
{ HK_ROW, "WORLD",  "The environment: weather (rain and lightning), the aurora, the eclipse "
          "and the god rays. Things that belong to the world you are at, not to your ship." },
{ HK_ROW, "VESSEL", "The hull: the whole engine family (exhaust and particle streams), the "
          "reentry effects (plasma and the vapour cones), and the flight-aid test rig. "
          "Tuning here is saved per vessel class - a nozzle is a fact about an airframe." },
{ HK_ROW, "PILOT",  "The human: what high G does to the body in the seat, the scripted "
          "G-event scenarios, and the virtual cockpit's own shadows and camera shake." },
{ HK_GAP, NULL, NULL },
{ HK_H,   "HOW THE PANEL WORKS", NULL },
{ HK_ROW, "The nav row", "The line under the master strip. The left side is the breadcrumb - "
          "where you are. BACK steps up one level; BACK TO MAIN always lands here. Both sit "
          "dim on this page because there is nowhere back to go." },
{ HK_ROW, "The fixed strip", "The master ENABLED/DISABLED toggle (the same switch as "
          "Ctrl+G - the panic button that kills every effect at once), HELP (this window, "
          "always about the page you are on), and the global SAVE, which writes everything: "
          "your global settings, the current hull's file and the current world's file." },
{ HK_ROW, "SAVE and REVERT on every page", "Each page of controls carries its own SAVE and "
          "REVERT, fixed at the top so they never scroll away. They touch only the files "
          "that page's controls live in, and the line beside them names those files before "
          "you press anything." },
{ HK_ROW, "The amber SAVE", "A SAVE button turns AMBER while there are unsaved edits in the "
          "files it would write - the global one for any unsaved edit anywhere, a page's own "
          "for its files. Green flash = written; amber gone = you are safe to quit. Test "
          "buttons and other session-only rigs never light it, because they are not saved." },
{ HK_ROW, "Where settings live", "Three scopes, and the split is the design: what the PILOT "
          "is saves globally (the same pilot flies every ship), what a HULL needs saves per "
          "vessel class, and what a WORLD is (its aurora, its lightning) saves per body. "
          "Each page's caption says which of these it writes." },
};

// --- MENU: WORLD ------------------------------------------------------------
static const HelpItem HELP_M_WORLD[] = {
{ HK_H,   "WORLD - the environment", NULL },
{ HK_P,   "Effects that belong to the world rather than to your ship or your body. Each "
          "leaf page has a TEST toggle for the same reason: none of these can be scheduled "
          "around a tuning session. A polar night, a total eclipse, a sunset and a storm "
          "are not things you can wait for on demand.", NULL },
{ HK_ROW, "WEATHER",  "Rain and lightning today; snow and a cloud-map-driven weather model "
          "are coming. The storm you summon and stand in." },
{ HK_ROW, "AURORA",   "Ribbon curtains around each magnetic pole of the world you are at. "
          "Twelve worlds ship with settings; any world can be given an aurora and saved." },
{ HK_ROW, "ECLIPSE",  "The eye inside another body's shadow - dark adaptation, the dazzle "
          "of emergence, colour draining at night. Built as an observer, not a dimmer." },
{ HK_ROW, "GOD RAYS", "Crepuscular shafts when a low sun is broken up by terrain, cloud or "
          "a hull. Needs air, and needs something to break the beam." },
{ HK_P,   "Saving is split here: the eclipse, god rays and rain are GLOBAL (the pilot's "
          "taste), the aurora and orbital lightning are PER BODY (what a world IS). Each "
          "page's caption names its files.", NULL },
};

// --- MENU: WEATHER ----------------------------------------------------------
static const HelpItem HELP_M_WEATHER[] = {
{ HK_H,   "WEATHER", NULL },
{ HK_ROW, "RAIN",      "The whole surface storm: collapsing light, the falling sheet, "
          "splashes, soaked ground with standing pools and real reflections, the cloud deck "
          "overhead, drops on the windscreen, and the storm's own sounds. Its page groups "
          "the sliders into the storm outside, the windscreen, and the sounds." },
{ HK_ROW, "LIGHTNING", "BOTH of ORO's lightning systems, side by side on one page: storms "
          "in a planet's cloud deck seen FROM ORBIT (per world), and the flashes, bolts and "
          "thunder INSIDE the rain storm you are standing in (global). They are independent "
          "systems - the page says which rows belong to which." },
{ HK_ROW, "SNOW",          "Coming soon." },
{ HK_ROW, "WEATHER MODEL", "Coming soon: weather found from the planet's own cloud map, so "
          "a storm is something you arrive in rather than summon." },
};

// --- MENU: VESSEL -----------------------------------------------------------
static const HelpItem HELP_M_VESSEL[] = {
{ HK_H,   "VESSEL - the hull", NULL },
{ HK_P,   "Everything about the ship itself. Tuning in this category saves PER VESSEL "
          "CLASS: nozzle sizes, plasma standoffs and cone shapes are facts about an "
          "airframe, so the DeltaGlider's numbers are meaningless on the Atlantis. The "
          "enable pills stay global - which effects run is your preference.", NULL },
{ HK_ROW, "THRUSTERS", "The engine family, per engine group: the exhaust ORO draws "
          "(shimmer, the pressure-driven plume, the incandescent bells) and Orbiter's own "
          "particle streams with every setting exposed live." },
{ HK_ROW, "REENTRY",   "The fire: the plasma with all its tuning, and the transonic vapour "
          "cones." },
{ HK_ROW, "FLIGHT AID", "NOT an effect - a test rig that changes how the vessel FLIES, "
          "shifting its centre of pressure so stock ships can hold a high angle of attack "
          "through an entry. It has its own door precisely so nothing about it hides at "
          "the bottom of a page of visuals." },
};

// --- MENU: THRUSTERS --------------------------------------------------------
static const HelpItem HELP_M_THRUSTERS[] = {
{ HK_H,   "THRUSTERS", NULL },
{ HK_P,   "Two pages, different in kind rather than just in subject.", NULL },
{ HK_ROW, "EXHAUST",   "What ORO draws itself: the heat shimmer, the plume expansion system "
          "(the physics of over- and underexpansion, shock diamonds, the vacuum bloom, "
          "soot), the bell glow, and the throat fire." },
{ HK_ROW, "PARTICLES", "Orbiter's OWN particle system with its controls exposed - ORO "
          "draws no particles at all, it hands the core the same settings a vessel author "
          "writes in code, live." },
{ HK_P,   "Both pages carry a fixed row at the top with TWO cyclers: the ENGINE GROUP "
          "you are editing - main, hover, retro, ungrouped (USER) or RCS - and, beside "
          "it, Thr: ALL or ONE THRUSTER of that group. At ALL everything edits the whole "
          "group; pick a thruster and your changes become an OVERRIDE for that engine "
          "alone, with MARK ringing its nozzle in the world so you always know which one "
          "you are holding. Each page's help has the full story.", NULL },
};

// --- MENU: REENTRY ----------------------------------------------------------
static const HelpItem HELP_M_REENTRY[] = {
{ HK_H,   "REENTRY", NULL },
{ HK_ROW, "PLASMA",       "The biggest effect in the addon: the shock envelope, the flame "
          "streamers, the sparks and the trail, the stagnation light, and the cockpit's own "
          "luminous sheath - with every tuning knob, per vessel class." },
{ HK_ROW, "VAPOUR CONES", "Transonic condensation - the shroud that forms crossing Mach 1. "
          "TWO independent cones, so a hull can carry one at the canopy and one at the "
          "tail, Concorde-style." },
};

// --- MENU: PILOT ------------------------------------------------------------
static const HelpItem HELP_M_PILOT[] = {
{ HK_H,   "PILOT - the human", NULL },
{ HK_ROW, "G-FORCES",  "What high G does to the body in the seat: the vision suite "
          "(blackout, red-out, tunnel and the rest), the tilt, and the felt-G model with "
          "its LAB | PHYSICS switch - the one thing to understand before touching any "
          "slider there." },
{ HK_ROW, "SCENARIOS", "One-click scripted G events - induce a blackout, a grey-out or a "
          "red-out without flying the manoeuvre, then recover from it." },
{ HK_ROW, "VIRTUAL COCKPIT", "The cockpit itself: sunlight sweeping through the canopy "
          "(VC shadows), and the physics-driven camera shake." },
};

// --- PAGE: G-FORCES (PILOT) -------------------------------------------------
// The thresholds quoted are the ones in OroPhysics.cpp's step 5, and the axis each
// row answers to is the one the panel's own PHYSICS readout names when it is idle.
static const HelpItem HELP_GFORCES[] = {
{ HK_H,   "WHAT THIS PAGE IS", NULL },
{ HK_P,   "Everything here is what high G does to the PILOT - the effects you see because "
          "of what is happening to the body in the seat, not to the ship. They only draw in "
          "an internal cockpit view.", NULL },
{ HK_GAP, NULL, NULL },

{ HK_H,   "THE ONE THING TO UNDERSTAND FIRST", NULL },
{ HK_P,   "The PILOT section has an Effect source switch with two positions, and it changes "
          "what every slider on this page MEANS.", NULL },
{ HK_ROW, "LAB",     "The sliders ARE the effect. Drag Blackout to 0.60 and you get 60% "
                     "blackout, right now, whatever the ship is doing. This is for seeing "
                     "what each effect looks like." },
{ HK_ROW, "PHYSICS", "The felt-G model computes the effects from the vessel's real motion, "
                     "and each slider becomes a GAIN on its own effect - a multiplier, not a "
                     "value. Pull G and the symptoms arrive by themselves." },
{ HK_P,   "The consequence catches everyone: in PHYSICS, a gain times zero is still zero. If "
          "the axis a row answers to is not being loaded, that row reads 0 no matter how far "
          "you push its slider, and it looks broken when it is working perfectly. So while the "
          "model is producing nothing, the number is replaced by the axis the row is WAITING "
          "for - +Gz, -Gz or Gx. A row showing +Gz is armed and waiting for you to pull.", NULL },
{ HK_GAP, NULL, NULL },

{ HK_H,   "IN PHYSICS: WHAT THE PILL AND THE SLIDER ACTUALLY DO", NULL },
{ HK_ROW, "The pill is a DRAW gate, and nothing else",
          "The model computes every effect every frame whether or not its pill is on. The "
          "pill is checked much later, when the frame is painted. Turning it off is closing "
          "your eyes to a symptom, not curing it - and note the row's slider becomes "
          "un-draggable until you turn the pill back on." },
{ HK_ROW, "Two pills reach further than their own row",
          "Blackout suppresses the sparkles, so switching blackout OFF lets phosphenes "
          "survive where they would normally have been swallowed. And the heartbeat adds "
          "its throb to the tunnel and drives the heartbeat SOUND, so switching it off "
          "takes both of those with it." },
{ HK_ROW, "The gain is a plain multiply",
          "Set it to half and you get exactly half - the model's number, multiplied. No "
          "curve, no threshold shift." },
{ HK_ROW, "It is a VOLUME knob, not a SENSITIVITY knob",
          "Halving a gain does not make the symptom arrive later. It arrives at exactly the "
          "same point in the pull, only weaker, and it can never reach full. If you want a "
          "symptom to start LATER, that is G tolerance or the anti-G suit - those move the "
          "real threshold. The gains only scale what comes out of the far end." },
{ HK_ROW, "You can attenuate, never exaggerate",
          "Gains run 0 to 1, so in PHYSICS the model is the ceiling. This is the one real "
          "asymmetry with LAB, where the same slider spans the whole effect directly - LAB "
          "will show you a total blackout parked on the runway, PHYSICS caps you at what "
          "you have actually earned." },
{ HK_P,   "And the part that surprises people: NONE OF THIS REACHES THE PHYSIOLOGY. The "
          "oxygen reserve, the threshold, the onset-rate penalty and the fourteen-second "
          "G-LOC hold never see the pills or the gains. Set every gain to zero and you will "
          "still black out, still be incapacitated for the full fourteen seconds, and the "
          "recovery eye-flutter will still fire - the blink has no pill and no gain, it is "
          "fired straight from the G-LOC timer. The model is untouched by everything on "
          "this page except the four PILOT controls; the pills and gains live entirely on "
          "the output side.", NULL },
{ HK_GAP, NULL, NULL },

{ HK_H,   "VISION - the symptoms, in the order a real pull produces them", NULL },
{ HK_P,   "These are separate symptoms and they layer. All except red-out track your oxygen "
          "reserve, which is drained by sustained POSITIVE G - so they arrive in sequence as "
          "the reserve empties, and they do not all appear at once.", NULL },
{ HK_ROW, "Heartbeat",    "The field pulses darker with each beat, and drives the heartbeat "
                          "sound. First to appear. It also deepens the tunnel as it throbs, so "
                          "the pulse survives even when the tunnel has closed." },
{ HK_ROW, "Grey-out",     "Colour drains before brightness does. Full-frame, not a vignette." },
{ HK_ROW, "Swim",         "A slow woozy warp of the periphery. Disorientation." },
{ HK_ROW, "Dark spots",   "Shimmering blind patches drifting across the field (scotomas)." },
{ HK_ROW, "Tunnel vision","Peripheral vision closes to a narrowing circle." },
{ HK_ROW, "Sparkles",     "Seeing stars - bright scintillations. A BAND, not a ramp: they "
                          "belong to the middle of the descent and are gone by the time the "
                          "field is black." },
{ HK_ROW, "Blackout",     "Vision gone. Last to arrive, and if your reserve empties completely "
                          "you are out for fourteen seconds whatever the G does next." },
{ HK_ROW, "Red-out",      "The red veil of NEGATIVE G, blood forced toward the head. A "
                          "different mechanism entirely: no reserve, prompt onset, and an "
                          "anti-G suit does nothing for it. Humans tolerate this direction "
                          "worst - about -2 to -3 G." },
{ HK_ROW, "Blur",         "Vision softens. The one row with two causes: eyeball deformation "
                          "under fore/aft G, OR deep positive G, whichever is producing more." },
{ HK_ROW, "Aberration",   "Colour channels split toward the edges. Rides Gx alone - eyeballs "
                          "in or out. IT WILL NEVER MOVE DURING A POSITIVE-G PULL, and that is "
                          "correct, not a fault." },
{ HK_ROW, "BLINK",        "Fires a single blink. For testing." },
{ HK_GAP, NULL, NULL },

{ HK_H,   "MOTION", NULL },
{ HK_ROW, "Tilt / sway",  "The whole view rolls. Two things at once: a woozy sway as you "
                          "approach blackout, and a signed LEAN toward lateral G - your head "
                          "lolling toward the load. Left and right tip opposite ways." },
{ HK_GAP, NULL, NULL },

{ HK_H,   "PILOT - what the model assumes about the body in the seat", NULL },
{ HK_ROW, "Save target",  "Where this page's settings are KEPT. ALL VESSELS is the global "
                          "file - the same pilot flies every ship, which is how ORO has "
                          "always worked. THIS VESSEL CLASS moves them into the hull's own "
                          "file, because where the crew SITS is a fact about an airframe. A "
                          "hull with no settings of its own falls back to your global ones, "
                          "so visiting an untuned ship never loses your pilot. The master "
                          "arm and the scenario sound stay global whatever it says, and the "
                          "line above SAVE always names the file that will be written." },
{ HK_ROW, "G tolerance",  "How much G you take before symptoms begin. The readout shows the "
                          "threshold it produces in G, because a 0-to-1 abstraction would be "
                          "unreadable - 4.0 is a number you can argue with." },
{ HK_ROW, "Anti-G suit",  "Worth about 1.5 G, POSITIVE G only. It stops blood leaving the "
                          "head, so it does nothing for red-out." },
{ HK_ROW, "Position",     "Seated, reclined, prone, standing or couch. This decides which "
                          "vessel axis is your SPINE, and therefore which manoeuvre loads "
                          "you. A reclined pilot takes more G - it shortens the blood column "
                          "between heart and eye, which is why fighter seats recline." },
{ HK_ROW, "G reference",  "Camera, or the vessel's centre of mass. IN ORBIT THIS IS THE WHOLE "
                          "EFFECT: free-falling, the centre of mass feels exactly zero, while "
                          "your head sitting metres away feels every rotation. Camera is the "
                          "honest answer and the default." },
{ HK_ROW, "Effects view", "Whether the physiology draws in any internal view, or only in the "
                          "virtual cockpit. The heartbeat sound follows the same rule." },
{ HK_ROW, "FELT G",       "Live readout: signed Gz (spine), Gx (eyeballs), Gy (lateral), and "
                          "your oxygen reserve. The reserve turns red below 50% and is the one "
                          "number worth watching - the symptoms track it, not the raw G." },
{ HK_GAP, NULL, NULL },

{ HK_H,   "WHILE YOU ARE DOCKED", NULL },
{ HK_P,   "Docked, the model stops asking the simulator what forces are on your hull and works "
          "the answer out from the motion of the whole joined assembly instead. There is a good "
          "reason: a docking latch reports the force it uses to hold two ships together, and in "
          "a scenario that STARTS docked that force can be large and permanent even though "
          "nothing is moving. Read literally it puts a steady G on you that is not there - a "
          "DeltaGlider parked at the ISS reads 1.5 G of nothing, which used to tilt the "
          "horizon and leave it tilted.", NULL },
{ HK_ROW, "What you still feel", "Your own engines, fired at a dock. And ROTATION - if you are "
                          "docked to a spinning station you feel real artificial gravity, "
                          "measured from your distance to the whole assembly's centre of mass. "
                          "That is the ring's radius, so a big wheel gives you a real floor to "
                          "stand on, and it falls off as you move toward the hub." },
{ HK_ROW, "What you will not", "An acceleration applied to the assembly by somebody else - a "
                          "station reboost, or a tug pushing the stack. That is a few "
                          "thousandths of a G in practice." },
{ HK_GAP, NULL, NULL },

{ HK_H,   "SAVING", NULL },
{ HK_P,   "This page saves GLOBALLY, to Config\\ORO.cfg - the same pilot flies every ship - "
          "unless the Save target says THIS VESSEL CLASS, in which case the pilot block goes "
          "to the hull's own file too. REVERT re-reads the same files and discards anything "
          "you have moved since the last save.", NULL },
};

// --- PAGE: SCENARIOS (PILOT) ------------------------------------------------
static const HelpItem HELP_SCEN[] = {
{ HK_H,   "SCENARIOS", NULL },
{ HK_P,   "One-click scripted G events, for when you want to see an effect without flying "
          "the manoeuvre that causes it. INDUCE ramps up and HOLDS - you stay blacked out "
          "until you press the matching RECOVER, which starts from exactly where the induce "
          "ended and ramps back to normal.", NULL },
{ HK_ROW, "G-LOC",    "The full descent: heartbeat, grey-out, tunnel, blackout, then "
          "fourteen seconds of incapacitation. The only scenario with its own audio clip." },
{ HK_ROW, "Grey-out", "Colour drains, vision softens, and it holds there." },
{ HK_ROW, "Red-out",  "The negative-G red veil, held." },
{ HK_P,   "They are LAB MODE ONLY and this page greys out in PHYSICS. Both a scenario and "
          "the felt-G model write the same values every frame, and two writers on one set "
          "of numbers is a fight nobody wins - switch the Effect source on the G-FORCES "
          "page back to LAB to use these.", NULL },
{ HK_P,   "While one runs, the G-FORCES page's sliders animate to show what the scenario "
          "is doing, and they are locked against editing until it ends.", NULL },
{ HK_ROW, "SOUND",    "Per-scenario audio. It can be toggled mid-run: muting pauses the "
          "clip and unmuting picks it up where the visuals are, rather than restarting it "
          "halfway through the event it describes. This toggle is the one thing on the "
          "page that saves - globally." },
};

// --- PAGE: EXHAUST (VESSEL > THRUSTERS) -------------------------------------
static const HelpItem HELP_EXH[] = {
{ HK_H,   "WHAT THIS PAGE IS", NULL },
{ HK_P,   "Everything ORO draws for the engines itself: the heat shimmer, the "
          "pressure-driven plume, the incandescent bells and the throat fire. Orbiter's own "
          "particle streams are the sibling PARTICLES page - different in kind, because "
          "there ORO only configures.", NULL },
{ HK_P,   "This page's TUNING saves PER VESSEL CLASS. Nozzle size, engine layout and how a "
          "bell was modelled decide every number here, so the DeltaGlider's settings are "
          "meaningless on the Atlantis. The PILLS and the LAB|PHYSICS switch save GLOBALLY - "
          "which effects run is your preference, not the hull's - and the caption beside "
          "SAVE names both files.", NULL },
{ HK_GAP, NULL, NULL },

{ HK_H,   "FIRST: WHICH ENGINE GROUP ARE YOU EDITING?", NULL },
{ HK_P,   "Look at the fixed row at the top of this page. It names the engine group "
          "everything below is currently editing, and it stays put however far you "
          "scroll - because it changes what every control on the page MEANS.", NULL },
{ HK_ROW, "Click it to cycle", "It steps through the groups THIS VESSEL ACTUALLY HAS - main, "
          "hover, retro, any engines the author put in no standard group at all (shown as "
          "USER), and RCS. A ship with only main engines has nothing to cycle to, so the "
          "button sits inert and says so." },
{ HK_ROW, "RCS",              "All attitude thrusters count as ONE group, however many the "
          "ship has - they are not split by direction. They were left out of ORO until now, "
          "and that turned out to be a bug rather than a decision: switching STOCK EXHAUST "
          "off silences every jet on the hull, RCS included, so leaving them out of the "
          "groups meant nothing of ours replaced them and they simply stopped showing. They "
          "have their own plume and particle settings now, like any other group." },
{ HK_ROW, "Why it exists", "One set of numbers for every engine is right only while they all "
          "burn the same propellant. The EXPANSION BAND's high handle is the pressure an "
          "engine is RATED for; the Jet and Bloom swatches are its exhaust's colour; soot is "
          "the difference between kerosene and hydrogen. A vacuum-rated main beside "
          "sea-level hovers could not be described at all until this existed." },
{ HK_ROW, "Tune one, cycle, tune the next", "Changing anything affects only the selected "
          "group. SAVE writes the class file with every group's settings in it, so tuning "
          "the hovers can never disturb what you did to the mains. The same button governs "
          "the PARTICLES page - one selection, both pages." },
{ HK_ROW, "Your old settings are safe", "A class file written before groups existed had one "
          "set of numbers. It loads into EVERY group, so each starts exactly where your "
          "tuning already was and nothing changes until you cycle and edit." },
{ HK_ROW, "Two pills stay whole-vessel", "STOCK EXHAUST and STOCK PARTICLES are not per "
          "group and cannot be: the client suppresses stock exhaust for a SHIP, not for a "
          "thruster group. A per-group switch there would be a control that lies." },
{ HK_GAP, NULL, NULL },

{ HK_H,   "ONE THRUSTER AT A TIME - the Thr cycler", NULL },
{ HK_P,   "Beside the group button sits a second cycler: ALL, or one thruster of the "
          "selected group. At ALL you are editing the whole group, exactly as before. Pick "
          "a thruster and the page edits THAT ENGINE: it inherits its group until you "
          "change something, and the first real edit creates an OVERRIDE - a full copy of "
          "this page's settings that belongs to that thruster alone. The line under the "
          "cycler always says which of these states you are in.", NULL },
{ HK_ROW, "MARK", "The in-world finder: a pulsing ring on the selected thruster's "
          "nozzle(s), with a tick showing which way it fires. At ALL it rings every "
          "nozzle in the group dimly - the quickest answer to 'which jets are even in "
          "this group'. It shows through the hull on purpose (a far-side nozzle you "
          "cannot find defeats the point), only while the panel is open on a THRUSTERS "
          "page, and it is never saved." },
{ HK_ROW, "CLEAR", "Appears when the selected thruster owns this page's override. One "
          "press drops it and the thruster snaps back to inheriting the group, live. "
          "The EXHAUST and PARTICLES overrides are independent - clearing one page's "
          "does not touch the other's." },
{ HK_ROW, "Overrides freeze on purpose", "Once a thruster owns an override, GROUP edits "
          "stop reaching that family of its settings - the override is a full copy, and "
          "that is the point: retune the group freely and your special engine keeps its "
          "look. SAVE writes overrides into the class file; CLEAR then SAVE removes them." },
{ HK_ROW, "Docked stacks", "Every vessel in the stack answers to ITS OWN class's saved "
          "settings - groups and overrides alike. Tune the Atlantis boosters by focusing "
          "an SRB (they ship focus-disabled; the Script folder's focusall.lua makes them "
          "selectable), save, and Atlantis_SRB.cfg drives them on every later launch, "
          "whoever has focus. A stack vessel whose class has NO saved file simply follows "
          "the focus vessel's settings, as everything always did. Your thruster 3 override "
          "can never land on a docked tug's thruster 3 - different class, different file." },
{ HK_ROW, "'below engine threshold'", "Some thrusters are plumbing, not propulsion - "
          "vents and dumps modelled as weak thrusters with no exhaust definition of "
          "their own. ORO draws no plume for those however you set the sliders, and the "
          "state line says so rather than letting the sliders look broken. Their "
          "PARTICLES still work, which is usually what a vent wants anyway." },
{ HK_ROW, "The bell is per group", "Bell-glow edits always go to the GROUP, even with a "
          "thruster selected - per-thruster bells are a later question, and the section "
          "would lie if it pretended otherwise." },
{ HK_ROW, "Engines with no exhaust of their own", "Some engines are built with no visible "
          "exhaust at all - the DeltaGlider-S scramjets are the example, and on a stock "
          "install they show particles and nothing else. ORO gives those a jet anyway, sized "
          "from the hull rather than from thrust (an airbreather's rated thrust changes with "
          "the air around it, so a nozzle sized from it would swell and shrink as you fly). "
          "It is a reasonable guess, not the author's intent, so correct it with this group's "
          "Width and Length - or turn the group's plume pill off if you prefer it bare." },
{ HK_GAP, NULL, NULL },

{ HK_H,   "SHIMMER", NULL },
{ HK_ROW, "Exhaust shimmer", "Heat haze bending the view behind the plume. Needs atmosphere "
          "and lit engines, and it is EXTERNAL VIEW ONLY - from the cockpit the engines are "
          "behind you, and a screen-space warp indoors would smear the panel and the window "
          "frame along with the plume." },
{ HK_ROW, "Offset (m)", "Slides the haze along the flow axis. Bipolar - it snaps to zero at "
          "the centre. The nozzle a vessel DEFINES and the nozzle you can SEE do not always "
          "agree, and this is the fix." },
{ HK_GAP, NULL, NULL },

{ HK_H,   "PLUME EXPANSION", NULL },
{ HK_P,   "A rocket nozzle is built for ONE ambient pressure. Everywhere else the atmosphere "
          "decides what the jet does: overexpanded at sea level gives the narrow pinched jet "
          "with the shock-diamond train, underexpanded in vacuum gives the wide faint bloom. "
          "That is what this section draws - in external view and, since RCS joined the "
          "groups, through the virtual cockpit windows too, cut at the frame per pixel.", NULL },
{ HK_ROW, "LAB | PHYSICS", "PHYSICS lets pressure and throttle drive the shape and the "
          "sliders trim on top; LAB pins the curves at reference so the sliders rule alone. "
          "The two are anchored IDENTICAL at sea level and full throttle, which is what makes "
          "PHYSICS safe to leave on - it changes nothing until you throttle back or climb. "
          "The flip side is that on the pad the switch looks like it does nothing. It is "
          "working; you are simply standing at the one condition where they agree." },
{ HK_ROW, "The caption above the band", "A live readout of ambient pressure and the regime "
          "the model has picked. Without it, 'no diamonds up here' and 'broken' look the same." },
{ HK_ROW, "Expansion band", "TWO handles on one track, and the most important control in the "
          "section. The LOW handle is the pressure at or below which the vacuum bloom is "
          "fully open. The HIGH handle is full overexpansion AND the pressure the engine is "
          "RATED for. Drag the high handle down low and you have a vacuum engine, which "
          "duly shudders and pinches at sea level. The fade between them lives inside "
          "whatever window you set, so the two ramps can never overlap." },
{ HK_ROW, "Width / Length", "The jet's overall size." },
{ HK_ROW, "Diamonds", "How many shock cells in the train. A whole number." },
{ HK_ROW, "Diamond bright / spacing", "Contrast and pitch of the cells. In PHYSICS both are "
          "already being driven - spacing stretches as you climb and tightens as you throttle "
          "down - so these trim that, they do not set it." },
{ HK_ROW, "Bloom width / bright", "The wide faint vacuum halo, the other end of the regime." },
{ HK_P,   "On wet ground the jet also appears in the REFLECTION, drawn from under the "
          "water alongside the hull and its lights. It costs nothing while the ground is "
          "dry or the ship is high up, and needs the RAIN page's Reflection slider "
          "(WORLD / WEATHER / RAIN) above zero to be visible at all.", NULL },
{ HK_ROW, "Throat glow", "The fire in the bell cup, drawn as camera-facing discs so it still "
          "reads when you look straight up the nozzle. 0 turns it off." },
{ HK_ROW, "Throat offset", "Where that cup sits, 0 to 1 m. Same problem as the shimmer's "
          "offset: the visible nozzle and the defined exhaust point disagree per hull." },
{ HK_ROW, "Soot streaks", "Dark ablative streaks shedding off the lip. 0 = off. They are "
          "drawn dark OVER the jet, because soot is in the flame and must dim it." },
{ HK_ROW, "Soot churn", "How fast they move. 0 freezes them." },
{ HK_ROW, "Jet / Bloom", "Two colour swatches. The jet core reaches white through the "
          "client's bloom, not through the palette - so with post-processing off it can only "
          "get brighter, never whiter." },
{ HK_GAP, NULL, NULL },

{ HK_H,   "BELL GLOW", NULL },
{ HK_P,   "The nozzle bells heat and cool as real metal does, on SIM time, whether or not "
          "you are watching - the same discipline as the eclipse. Equilibrium follows the "
          "fourth root of throttle, so 10% thrust settles at about 56% temperature and never "
          "reaches full glow at all. It needs an author-supplied bell mesh for the vessel "
          "class; the caption under the section is the readout and says which engine families "
          "were wired, or why nothing glows.", NULL },
{ HK_ROW, "Bell glow", "Brightness trim." },
{ HK_ROW, "Heat time (s)", "How fast they come up to temperature at full throttle." },
{ HK_ROW, "Cool time (s)", "How long until the glow is COMPLETELY gone - not half gone. The "
          "shape of the fade is preserved at any length: quick off white heat, then the long "
          "dull-red ember tail." },
{ HK_ROW, "Bell colour", "Rotates the whole heat ramp onto the hue you pick and keeps its "
          "shape - still dull at the bottom, still whitening at the top. White is the default "
          "blackbody ramp, unchanged. Like the jet, the bell reaches white through the BLOOM, "
          "so with post-processing off it can only ever get brighter amber, and the caption "
          "will tell you if that is what you are looking at." },
{ HK_P,   "GIMBALLING BELLS (for mesh authors): if a vessel animates its real engine bells "
          "with the gimbal, give each bell its own group in the bell mesh and add a line "
          "reading GIMBAL to that group's header - the glow then rotates to track the live "
          "thrust direction of its engine, matched by position, so it rides the moving bell "
          "as one piece of metal. A label's first word is the engine family and the rest is "
          "yours ('MAIN 1', 'MAIN 2' keep mesh editors happy). Without the token a group "
          "stays fixed, which is correct for the many vessels that gimbal the thruster but "
          "never move the bell mesh itself.", NULL },
{ HK_GAP, NULL, NULL },

{ HK_ROW, "Stock preset / COPY STOCK", "Resets this group's jet to the stock flame: width and "
          "length 1.0 (the plume's base size already comes from the vessel's own exhaust "
          "definition), with diamonds, bloom, throat fire and soot switched off. A clean "
          "starting point before shaping." },
{ HK_H,   "THE TWO PILLS AT THE BOTTOM", NULL },
{ HK_ROW, "STOCK EXHAUST", "Orbiter's own exhaust BILLBOARDS - the flame texture. Off hides "
          "them so you can judge ORO's plume alone. It covers billboards ONLY; stock's "
          "exhaust PARTICLES have their own pill on the PARTICLES page. They were split "
          "deliberately, so an addon that replaces one of them can be handled without losing "
          "the other. Greys out if the running client cannot suppress." },
{ HK_ROW, "CANCEL THRUST", "A test stand that FOLLOWS THE SELECTION: it nulls the selected "
          "group - or the one selected thruster - each engine cancelled at its own "
          "position, so its push AND its twist die together. Fire one RCS jet under the "
          "hold and the ship moves not at all, while every other control stays live. The "
          "caption names what is being held. NEVER SAVED - a persisted thrust-cancel "
          "loaded into a launch scenario would read as 'my engines are dead'. The same "
          "switch is mirrored on the PARTICLES page - one rig, two doors." },
};

// --- PAGE: PARTICLES (VESSEL > THRUSTERS) -----------------------------------
static const HelpItem HELP_PRT[] = {
{ HK_H,   "WHAT THIS PAGE IS", NULL },
{ HK_P,   "ORO draws none of this. It hands Orbiter the same settings a vessel author writes "
          "in code and lets you move them live, in the API's own units. That is why the rows "
          "read the way they do - they are the fields of a particle stream spec, not "
          "invented knobs.", NULL },
{ HK_P,   "The fixed row above applies here exactly as on the EXHAUST page: every setting "
          "belongs to the selected GROUP - or, with the Thr cycler off ALL, to ONE "
          "THRUSTER, whose first edit here creates a particle override for that engine "
          "alone (see the EXHAUST page's help for the full override story - MARK, CLEAR, "
          "and how overrides survive group retuning). The two pages' overrides are "
          "independent: giving one jet its own particles does not freeze its plume. The "
          "tuning saves per vessel class; the pill saves globally.", NULL },
{ HK_P,   "Two consequences fall straight out of the API and surprise everyone. There is no "
          "width or length, because a particle is a ROUND sprite - Size is its radius. And "
          "there is no colour field at all: colour lives in the texture, so the swatch works "
          "by synthesizing one, and it greys out on a client that cannot accept it.", NULL },
{ HK_ROW, "Stock preset / COPY STOCK", "Loads the vessel author's own stream definition into "
          "the sliders as a starting point - only the streams belonging to the SELECTED "
          "ENGINE GROUP (matched by thrust direction), with per-thruster duplicates folded, "
          "so a DeltaGlider's MAIN offers exactly its contrail and its flame puffs. With a "
          "THRUSTER selected it narrows further, to exactly the streams the author attached "
          "to that one engine - matched by position as well as direction, no folding needed. "
          "Press again to cycle; the status line names which one you got and for what. "
          "Slider top ends stretch automatically when a stock value (a long booster-smoke "
          "lifetime, say) is beyond the preset range. Needs a patched client; greys out "
          "without it." },
{ HK_ROW, "Offset (m)", "Where particles are born along the exhaust. The one bipolar row - "
          "negative moves the source back toward the nozzle." },
{ HK_ROW, "Size (m)", "Radius at birth." },
{ HK_ROW, "Lifetime (s)", "How long each particle lives." },
{ HK_ROW, "Rate (Hz)", "How many are created per second." },
{ HK_ROW, "Speed (m/s)", "How fast they leave." },
{ HK_ROW, "Spread", "Randomness in that velocity. 0 is a tight column." },
{ HK_ROW, "Growth (m/s)", "How fast each expands as it ages." },
{ HK_ROW, "Atm slowdown", "How hard the atmosphere brakes them." },
{ HK_ROW, "Lighting", "EMISSIVE, glowing by themselves (flame), or DIFFUSE, lit by the sun "
          "(smoke and vapour). The single biggest look switch in the whole spec - try both. "
          "With the ORO client, DIFFUSE really means it: night darkening, per-particle "
          "terminator, flame-lit near the engine, and DIRECTIONAL shading - the sun-facing "
          "side of a cloud is bright, the far side smoky, per particle corner. Through dawn "
          "and dusk the sunlit smoke follows the SAME colour the hull takes, one stop ahead "
          "and bloomed, while the engine-lit steam stays its own colour. The Launchpad's "
          "'Particle lighting (ORO)' group (Video > Advanced) holds the scene-wide "
          "controls: the Off / Brightness only / full colour dropdown, the diffuse shadow "
          "strength, and three dawn-tint dials (lead, depth, bloom)." },
{ HK_ROW, "Air fade", "FADES IN VACUUM is Orbiter's own behaviour and the default: a stream "
          "thins out as the air does, which is why you do not get exhaust clouds hanging in "
          "orbit. ALWAYS ON emits everywhere. If you enable this page in orbit on the default "
          "and see nothing, that is the fade doing its job - the row's label changes to 'Air "
          "fade - in vacuum' while it is holding emission off, and the caption says so." },
{ HK_ROW, "Colour A / B", "TWO tints: each particle is randomly born with one or the other, "
          "so white + dark grey gives a mixed smoke no single colour can. On a file texture "
          "a tint REPLACES the file's colour using its brightness as shading - which is why "
          "white genuinely means white. Set both the same for a single-colour look. Needs a "
          "patched client." },
{ HK_ROW, "STOCK", "Render the texture's own authored colours and ignore both tints - the "
          "button stays green while active and the swatches grey out. The honest 'exactly "
          "as the file looks' switch." },
{ HK_ROW, "Texture", "The particle's SHAPE. Cycles through ORO's synthesized atlas, Orbiter's "
          "own Contrail1 and Contrail1a (the DeltaGlider's wispy smoke), and then any .dds "
          "you drop into Textures\\ORO\\Particles. Files MUST be 2x2 atlases of four "
          "variants, like Contrail1.dds - a single centred image renders as corner wedges "
          "(see the folder's README). The Colour swatch still applies: white shows the file "
          "exactly as authored. Saved per class; a missing file falls back to the "
          "synthesized atlas. Needs a patched client." },
{ HK_ROW, "STOCK PARTICLES", "The vessel author's own exhaust streams. Independent of "
          "ORO's pill at the top of the tab: run stock's, ORO's, both together, or neither - "
          "every combination is legal, and SAVE keeps whichever you set." },
{ HK_ROW, "CANCEL THRUST", "The same test-stand switch as the EXHAUST page's - one rig, two "
          "doors, so tuning particles does not mean a page hop to hold the ship still. It "
          "follows the selection: the chosen group, or the one chosen thruster, nulled at "
          "its own position so push and twist die together while everything else stays "
          "live. Toggling it here or there changes both. NEVER SAVED - a persisted "
          "thrust-cancel loaded into a launch scenario would read as 'my engines are "
          "dead'." },
};

// --- PAGE: PLASMA (VESSEL > REENTRY) ----------------------------------------
static const HelpItem HELP_PLAS[] = {
{ HK_H,   "WHAT THIS PAGE IS", NULL },
{ HK_P,   "The biggest effect in the addon: the reentry fire, with every knob it has.", NULL },
{ HK_P,   "The TUNING saves PER VESSEL CLASS, and that matters more here than anywhere else. "
          "How far the glowing shell should stand off the skin is a fact about a HULL - the "
          "number that fits the DeltaGlider sank the Atlantis within hours of being baked in "
          "as a constant, which is why it is a slider again. The enable pill saves GLOBALLY - "
          "which effects run is your preference, not the hull's.", NULL },
{ HK_GAP, NULL, NULL },

{ HK_H,   "THE TWO READOUTS - read these before you touch a slider", NULL },
{ HK_ROW, "Plasma heat", "What the model computed for the camera-target vessel. No vessel "
          "publishes a nose radius, so the heat thresholds cannot be derived and the number "
          "has to be visible - otherwise 'too cold' and 'not working' look identical." },
{ HK_ROW, "The caption under the pill", "Normally it describes the effect. It becomes a "
          "WARNING when something in your video settings is quietly degrading it: with Sun "
          "glare off the client never builds the depth buffer and the plasma paints straight "
          "through the hull, and with post-processing off it can never bloom to white and "
          "reads hard and pointy. Both are settings, not tuning faults, and both are silent "
          "on screen - which is exactly why the caption says them." },
{ HK_GAP, NULL, NULL },

{ HK_H,   "PLASMA TUNING - the look", NULL },
{ HK_ROW, "Saturation", "The whole palette at once. 1 is the reference look." },
{ HK_ROW, "Hull light", "A real light source at the stagnation point, lighting the vessel's "
          "OWN mesh - not our geometry. 0 removes the light entirely." },
{ HK_ROW, "VC glow", "THE COCKPIT'S OWN PLASMA, and it is a different thing from what you see "
          "outside. Everything else on this page builds detailed geometry around the hull - "
          "which is right when you are looking AT the ship, and wrong when you are sitting "
          "inside it, because from in there most of that geometry is behind your head. What a "
          "pilot actually sees is a luminous sheath filling the windows, with filaments "
          "streaming past and sudden flares. That is what this draws. It is anchored to the "
          "relative wind, so it sits where the shock really is and slides across the glass as "
          "the ship rotates. 0 turns it off. Brightness scales with the reentry heat, so this "
          "is a trim, not a level." },
{ HK_ROW, "Cabin wash", "Where the cockpit's glow LANDS, not how bright it is. Low, and it is "
          "a pool of light centred on the plasma - turn your head to the instruments and it "
          "goes with it. High, and the whole cabin lights up whatever you are looking at, "
          "which is how a reentry is actually flown: eyes down, the fire caught in the corner "
          "of your vision. The flares are timed to the ones outside the window - one event, "
          "lighting the sheath and the cabin in the same frame." },
{ HK_ROW, "Streak length / width / wander", "The flame streamers trailing back." },
{ HK_ROW, "Wake churn", "How FAST the wake lives - fin shimmer, spark march, the drift of the "
          "striations, all on one clock so the wake stays coherent. 1 is standard, 0 freezes "
          "it. Turn it up if the plasma reads more like an aurora than like something being "
          "torn off a hypersonic vehicle. Note this scales the spark march too, so 'Spark "
          "life' means seconds at churn 1." },
{ HK_ROW, "Fin rake (deg)", "How far the streamers splay OUT from the flow direction. 0 lays "
          "them dead downstream. Every streamer carries the same angle whatever its length." },
{ HK_ROW, "Sparks / Spark life / Spark size", "Burning debris marching downstream." },
{ HK_ROW, "Edge light", "A rim light picking out the windward silhouette. Off by default." },
{ HK_ROW, "Shock bright", "The shock envelope wrapped around the hull. 0 turns the shell off "
          "entirely, which is the escape hatch on a vessel where it looks wrong." },
{ HK_ROW, "Shell dist", "How far the glowing shell stands off the skin. A property of the "
          "HULL - see the note above." },
{ HK_ROW, "Bowl dist / Bowl size X, Y, Z", "Standoff and shape of the bow shock in front. "
          "The three sizes sculpt it along the vessel's own axes; 1 is neutral." },
{ HK_ROW, "Tint / Fringe", "Two colour swatches: the body hue and the magenta cast, "
          "independently. They ROTATE the hue rather than multiplying it, so you pick a "
          "colour and you get that colour - and white is the reference palette, unchanged." },
{ HK_ROW, "VC ON / OFF", "Whether the plasma also draws looking OUT of the virtual cockpit. "
          "This is the one deliberate crack in the internal/external wall. With a patched "
          "client it cuts per pixel at the window frame. What you get through the glass is "
          "the cockpit's own effect - see VC glow - and not the geometry you see from "
          "outside." },
{ HK_GAP, NULL, NULL },

{ HK_H,   "THE TRAIL", NULL },
{ HK_ROW, "Trail density", "Knot spacing, not brightness - the trail is a continuous ribbon, "
          "so there is no stacking to brighten. 0 turns it off." },
{ HK_ROW, "Trail life (s)", "The LENGTH lever. About six seconds is tens of kilometres at "
          "entry speed." },
{ HK_ROW, "Trail width", "Ribbon width." },
{ HK_ROW, "Trail start", "Where it begins, in hull sizes. BIPOLAR: negative moves it UPSTREAM "
          "into the fireball, and the hull correctly hides the overlap." },
{ HK_ROW, "Trail hot / tail", "Head and tail colours, blended along the trail's own age." },
};

// --- PAGE: VAPOUR CONES (VESSEL > REENTRY) ----------------------------------
static const HelpItem HELP_VAP[] = {
{ HK_H,   "WHAT THIS PAGE IS", NULL },
{ HK_P,   "The shroud that forms going through Mach 1. Air holds water; the flow over the "
          "hull expands, pressure and temperature drop, and the water condenses. It needs LOW "
          "ALTITUDE as well as the right speed - the water is in the troposphere, which is "
          "why every reference photograph of one is taken low and usually over the sea. Both "
          "gates are read from the sim.", NULL },
{ HK_P,   "There are TWO CONES, each with the identical, completely independent set of "
          "controls - the colours included - so a hull can carry one collar at the canopy "
          "and one at the tail the way the Concorde photographs show. The single pill arms "
          "the whole effect; each cone's own Opacity is its visibility, which is why there "
          "is no second pill. Cone 2 ships at zero - invisible until you give it some.", NULL },
{ HK_ROW, "TEST", "Pins Mach 1.15 and bypasses the speed and altitude gates, so you can judge "
          "the look from a runway instead of flying an ascent over and over. Not 1.00, "
          "because at exactly Mach 1 a cone is a near-flat collar and tells you almost "
          "nothing about its shape from three of four viewing directions." },
{ HK_GAP, NULL, NULL },
{ HK_H,   "EACH CONE'S CONTROLS", NULL },
{ HK_ROW, "Opacity", "0 is no cone at all. Up to 1 it is a translucent shroud; past 1 the "
          "sheet FILLS and densifies until, at the top, it can hide the hull behind it - "
          "the fuselage-swallowing disc of the reference photographs." },
{ HK_ROW, "Size x / Size y", "The radii, in hull sizes - x along the wing line, y vertical. "
          "EQUAL VALUES GIVE A CIRCULAR CONE; unequal, an oval one. Properties of the "
          "airframe." },
{ HK_ROW, "Size z", "The length, as a fraction of the length the Mach angle derives - 1 is "
          "the physics, 0 collapses the cone into a flat collar disc. It scales the derived "
          "length rather than replacing it, so the shroud still stretches back on its own "
          "as you accelerate - which is the whole effect." },
{ HK_ROW, "Streaks", "Slim darker filaments running length-wise through the vapour. The "
          "slider is the COUNT - up to a few dozen; 0 is the clean sheet. They paint on the "
          "surface without deforming it." },
{ HK_ROW, "Streak churn", "How violently the streaks live: they jitter, flare and die "
          "rather than parade. 0 freezes the pattern in place; 2 runs it doubly fast." },
{ HK_ROW, "Flicker (Hz)", "How fast the cone breathes. Opacity and size vary together on "
          "ONE number, because a stronger condensation event is denser AND bigger at the "
          "same instant - two clocks would give a shroud that grows while it thins, which "
          "nothing in nature does. 0 freezes it." },
{ HK_ROW, "Vapour / Streaks", "Two colour swatches: the vapour body and the streak "
          "filaments. The defaults are the natural pair - cool white vapour, darker "
          "grey-blue streaks - and the streaks BLEND toward their pick, so light streaks "
          "on dark vapour work as well as the reverse." },
{ HK_ROW, "Base fill", "The filled disc closing this cone's wide end - what gives it a "
          "back, and at high opacity the face that actually hides the hull. Off returns "
          "the open shell." },
{ HK_ROW, "Position x / y / z", "Where this cone sits, in hull sizes, in Orbiter's own axis "
          "convention: x along the wing line, y vertical, z along the flight direction. All "
          "three bipolar, snapping to zero. Per cone - z is what separates the two collars "
          "along the hull; x and y are for vessels whose shock does not stand on the "
          "centreline." },
{ HK_ROW, "Pitch / Yaw", "Tilts this cone away from the relative wind, up to 30 degrees "
          "either way - pitch nods the apex up and down, yaw swings it left and right. At "
          "zero (the default, and the knobs snap to it) the cone rides the wind exactly as "
          "before, which is where physics says it belongs; the tilt exists for hulls whose "
          "geometry stands the shock off at an angle. No roll - a surface of revolution "
          "has nothing to roll." },
{ HK_ROW, "Mach band", "Two handles: where THIS cone starts and stops existing. Drag them "
          "together for a brief flash as you punch through, apart for a long transonic "
          "haze - and give the two cones different bands to have the collars appear at "
          "different speeds, which is what really happens: the flow goes supersonic over "
          "different parts of the hull at different vehicle Mach. The fades live INSIDE "
          "whatever window you set, so a tight window is a sharp flash rather than a "
          "fade-in that has not finished before the fade-out starts." },
{ HK_GAP, NULL, NULL },
{ HK_ROW, "Cone", "Readout: your Mach and how strong the strongest visible cone is, or WHY "
          "nothing is showing - subsonic, thin air, vacuum, or internal view. The number "
          "keeps working from the cockpit even though the cones only draw externally, "
          "because the whole point is to tell you when to switch to an external view." },
{ HK_P,   "Saving: the shapes, colours and bands are PER VESSEL CLASS; the pill is "
          "global.", NULL },
};

// --- PAGE: FLIGHT AID (VESSEL) ----------------------------------------------
static const HelpItem HELP_AID[] = {
{ HK_H,   "FLIGHT AID - NOT AN EFFECT", NULL },
{ HK_P,   "Everything else in this panel changes what you SEE. THIS CHANGES HOW THE VESSEL "
          "FLIES. It is a test rig, not part of the visuals, and it is the only thing in "
          "ORO that applies a real force to your ship - which is why it has a page of its "
          "own instead of hiding at the bottom of one.", NULL },
{ HK_ROW, "CoP shift (m)", "Shifts the vessel's effective centre of pressure, which changes "
          "its PITCH TRIM: a stock ship will hold a high angle of attack instead of "
          "weathervaning nose-first. It exists so you can SEE a reentry at all - left alone, "
          "stock ships drop the nose into the flight direction and make almost no plasma. "
          "Forward (+) is less nose-down. Zero, which the knob snaps to, is the vessel "
          "flying exactly as its author coded it." },
{ HK_ROW, "It is enabled in the shipped settings", "For the DeltaGlider and the Atlantis, "
          "because the reentry scenarios need it. So if one of those handles differently "
          "from stock - trims nose-up, resists pushing the nose down - this knob is why, and "
          "zeroing it gives you the stock airframe back on the next step." },
{ HK_ROW, "Applies", "REENTRY ONLY (M 3+) is the default, and keeps the aid out of the way at "
          "ordinary flying speeds. That matters: it is a steady couple added to your pitch "
          "axis, so anything else trying to hold an attitude - an autopilot in particular - "
          "is working against it, and at low speed the two can chase each other into a "
          "growing oscillation. ALWAYS applies it at any speed if you want that." },
{ HK_ROW, "Pitch moment", "What the aid is applying, live. An aid you cannot see working is "
          "an aid you cannot tune. While the gate is holding it off it says 'gated' and your "
          "Mach, rather than a zero that would look like a broken knob." },
{ HK_P,   "One thing to know before you press it: Ctrl+G releases the aid instantly along "
          "with everything else, so disarming in the middle of an entry hands the airframe's "
          "full stability back at once and the nose WILL drop.", NULL },
{ HK_P,   "Saving: everything here is PER VESSEL CLASS - the shift and its gate are facts "
          "about the airframe.", NULL },
};

// --- PAGE: ECLIPSE (WORLD) --------------------------------------------------
static const HelpItem HELP_ECL[] = {
{ HK_H,   "ECLIPSE", NULL },
{ HK_P,   "This is built as an EYE, not as a dimmer, and that decision is what makes it work. "
          "The client already darkens terrain under a moon's shadow and already cuts a "
          "vessel's sunlight in its primary's shadow, so simply multiplying the frame down "
          "would have fought the renderer on the night side of every orbit. What no renderer "
          "models is the observer: asymmetric dark adaptation, which is why emerging from a "
          "shadow DAZZLES and entering one merely gropes.", NULL },
{ HK_ROW, "TEST", "Runs a full cycle in about forty seconds of real time - clear, first "
          "contact, eight seconds of totality, emergence." },
{ HK_ROW, "Dim", "How much the steady obscuration darkens the scene." },
{ HK_ROW, "Eye adaptation", "How strongly the eye compensates. This is the part that produces "
          "the dazzle on the way out." },
{ HK_ROW, "Colour loss", "How far colour drains toward the cool grey of night vision." },
{ HK_ROW, "The two readouts", "Sun obscured, and the eye's response. They are shown "
          "separately because they genuinely diverge: fully adapted inside totality the sun "
          "is 100% covered while the eye is doing nothing, which is correct and would look "
          "broken as a single number. Expect 'obscured 100% by Earth' standing on the ground "
          "at night - that is orbital night, and it is a real event for an eye." },
{ HK_P,   "Saves GLOBALLY - the same eye sits behind every canopy at every world.", NULL },
};

// --- PAGE: AURORA (WORLD) ---------------------------------------------------
static const HelpItem HELP_AUR[] = {
{ HK_H,   "AURORA", NULL },
{ HK_P,   "Ribbon curtains around each MAGNETIC pole of the world you are at. Twelve worlds "
          "ship with settings. There is no enable flag per world on purpose - ACTIVITY IS "
          "THE OPT-IN. An unlisted world is silent, turning Activity up at any world gives it "
          "an aurora, and saving keeps it.", NULL },
{ HK_ROW, "TEST", "Rings the point directly below the camera, so you need not fly to a pole "
          "and wait for darkness." },
{ HK_ROW, "Activity", "Master strength, and the opt-in. 0 is silent." },
{ HK_ROW, "Oval lat", "How far the oval sits from the magnetic pole. Shown in real degrees "
          "from the current world's own range, not a raw 0 to 1." },
{ HK_ROW, "Fold", "Waves the ring in and out - the draperies." },
{ HK_ROW, "Rays", "The vertical striations. 0 is a smooth sheet." },
{ HK_ROW, "Breakup", "How broken the curtain is along its length." },
{ HK_ROW, "Thickness", "Parallel sheets per curtain. This buys LIMB BRIGHTENING for free: "
          "edge-on you look through more sheets than face-on. Per-sheet brightness falls as "
          "you add them, so the knob changes the LOOK, not the total brightness." },
{ HK_ROW, "Base (km) / Top (km)", "The altitude band the curtain spans, in real kilometres "
          "from this world's own range." },
{ HK_ROW, "Ribbons", "How many concentric arcs, 1 to 6." },
{ HK_ROW, "Tilt X / Y (deg)", "Rotates the magnetic AXIS, so both ovals move together as a "
          "dipole - north one way, south exactly opposite. Physical before exotic: Earth's "
          "magnetic pole is about 11 degrees off its spin axis, Uranus's is 59, and Io's "
          "aurora is equatorial because Jupiter's field drives it rather than its own." },
{ HK_ROW, "Base / Body / Top", "Three colour swatches in ALTITUDE order, because that is what "
          "decides a real aurora's colour - how deep the particles get sets which species "
          "emits and which of its lines. Earth reads nitrogen violet at the bottom, oxygen "
          "green through the body and oxygen red at the top, and no two-colour scheme can say "
          "that, which is exactly why there are three." },
{ HK_ROW, "Curtains over", "Which world they are being drawn at. Blank means no suitable body "
          "in range, which is the honest reason for 'nothing is showing'." },
{ HK_P,   "Saves PER BODY: what a world's aurora IS belongs to the world (the pill is "
          "global). A world with no file of its own gets the built-in defaults back rather "
          "than inheriting the last world's look, because arriving somewhere new must not "
          "show you Jupiter's curtains. ACTIVITY IS THE OPT-IN - there is deliberately no "
          "per-world enable flag.", NULL },
};

// --- PAGE: LIGHTNING (WORLD > WEATHER) - both systems, one page -------------
static const HelpItem HELP_LTG[] = {
{ HK_H,   "WHAT THIS PAGE IS", NULL },
{ HK_P,   "ORO HAS TWO LIGHTNING SYSTEMS, AND THIS PAGE HOLDS BOTH, under their own "
          "headers. FROM ORBIT is storms in a planet's cloud deck as you look down on them, "
          "night side, saved per world. IN THE STORM is the rain storm's own lightning - "
          "flashes in the deck overhead and bolts to the ground, day or night, global. They "
          "are independent systems: neither's sliders affect the other, and switching one "
          "off does not quieten the other.", NULL },
{ HK_GAP, NULL, NULL },
{ HK_H,   "FROM ORBIT", NULL },
{ HK_P,   "Storms in the cloud deck, seen from above. The storms form where the CLOUD "
          "actually is - ORO reads the planet's own cloud map rather than inventing weather, "
          "so a flash never lights up a clear ocean. Earth ships enabled; other worlds have "
          "it available at zero.", NULL },
{ HK_P,   "Flashes only appear on the NIGHT side, and that is physics rather than a budget. "
          "From orbit the eye cannot pick a diffuse in-cloud flash out of a sunlit deck - the "
          "instruments that do it in daylight work at one narrow wavelength for that reason, "
          "and every astronaut photograph of lightning is taken night-side.", NULL },
{ HK_ROW, "TEST", "One fast cell a couple of hundred kilometres north of you with every gate "
          "bypassed - night, cloud cover and altitude - so it is judgeable from a runway." },
{ HK_ROW, "Activity", "How much of the world is storming. 0 is silent." },
{ HK_ROW, "Brightness", "Flash intensity." },
{ HK_ROW, "Flash rate", "How often each cell fires." },
{ HK_ROW, "Cell size", "How big a storm cell is; the readout shows what the slider means in "
          "kilometres." },
{ HK_ROW, "Flash colour", "Default is the blue-white lightning actually looks like from the "
          "ISS - the opposite pole of the palette from the reentry plasma." },
{ HK_ROW, "The readout", "The world and the live CELL COUNT. 'No storms' has three honest "
          "causes - day side, clear sky under you, or activity zero - and the count is what "
          "tells them apart from broken." },
{ HK_GAP, NULL, NULL },

{ HK_H,   "IN THE STORM", NULL },
{ HK_P,   "The RAIN storm's own lightning, and it needs the rain to exist: these rows sit "
          "grey until the RAIN pill (or its Test) is on - the page says so when that is why "
          "nothing fires. Most events light a region of the deck from within, a share become "
          "BOLTS to the ground off a baked atlas of sixteen real channels, and a rare giant "
          "strikes far out, single and brilliant. Every flash blinks a real light over the "
          "ship and the wet ground - sky, bolt and scene agree because they are one "
          "event.", NULL },
{ HK_ROW, "Lightning", "How often the storm discharges. 0 is none at all; 2 is very often. "
          "Flashes only start once the storm is properly built - lightning belongs to a "
          "real storm, not to the first drops." },
{ HK_ROW, "Bolt bloom", "The radiance around a bolt's channel: glow taps stacking under "
          "the crisp core. 0 is the bare filament; 2 wraps the channel in a storm-photo "
          "blaze." },
{ HK_ROW, "Thunder", "Every flash sends its thunder, delayed by ITS OWN distance at the "
          "speed of sound - six to twenty-six seconds after the light, which is the "
          "realism, not a miss. Close bolts CRACK, in-cloud and distant flashes rumble, "
          "the rare positive giant hits hardest, and inside the cockpit it all arrives "
          "muffled through the hull. Nine real recordings (freesound.org, credited in "
          "XRSound\\ORO\\README.txt); this is their volume, 0 = silent." },
{ HK_ROW, "Test bolt / STRIKE", "The lightning test rig: each press plants the NEXT of "
          "the sixteen atlas bolts directly on the focus vessel with a fixed, repeatable "
          "flicker, cycling 1 to 16 - the readout names the one you are looking at. It "
          "needs the storm running but ignores the Lightning rate, so the bolts can be "
          "judged on demand. The crack follows the flash by the CAMERA's distance from "
          "the strike - press it beside the ship for the whole bolt-then-thunder beat, "
          "or from kilometres out for the delayed boom." },
{ HK_P,   "Saving: the FROM ORBIT rows are per body; the storm rows and both pills are "
          "global.", NULL },
};

// --- PAGE: GOD RAYS (WORLD) -------------------------------------------------
static const HelpItem HELP_GRY[] = {
{ HK_H,   "GOD RAYS", NULL },
{ HK_P,   "Crepuscular shafts - the beams you get when a low sun is broken up by terrain, "
          "cloud or a hull. They need AIR to scatter in, so the pass does not run in orbit at "
          "all, and they need something to BREAK THE BEAM UP: with the sun in open sky the "
          "technique can only smear the disc into a halo. Best seen low, near sunrise or "
          "sunset, with something between you and the sun.", NULL },
{ HK_ROW, "TEST", "Bypasses the air and sun-height gates, though not 'the sun has to be "
          "roughly on screen'." },
{ HK_ROW, "Strength", "Master intensity." },
{ HK_ROW, "Reach", "How far the shafts extend from the disc." },
{ HK_ROW, "Softness", "Crisp short rays through to long soft ones." },
{ HK_ROW, "Sensitivity", "How dim a thing may be and still cast a shaft. This is the knob "
          "that separates 'shafts' from 'radial blur over the whole sky'." },
{ HK_ROW, "Warmth", "How far they redden as the sun nears the horizon." },
{ HK_ROW, "Shafts", "Readout: strength, or why it is zero - vacuum, high sun, night, sun "
          "behind you, or off-view. This effect has more honest ways of showing nothing than "
          "any other in ORO, and without the line every one of them reads as a fault." },
{ HK_P,   "An eclipse kills the shafts, which is correct - there is less beam left to "
          "scatter. It is the one place the two solar effects talk to each other.", NULL },
{ HK_P,   "Saves GLOBALLY - the pilot's taste; the physical difference between worlds is "
          "already handled by the density gate.", NULL },
};

// --- PAGE: RAIN (WORLD > WEATHER) -------------------------------------------
static const HelpItem HELP_RAINP[] = {
{ HK_H,   "WHAT THIS PAGE IS", NULL },
{ HK_P,   "A storm you summon at the surface. The build-up ramps over ten seconds or so - "
          "the light collapses to overcast, the first streaks fall, the ground soaks dark, "
          "water stands in pools - and switching the pill OFF is INSTANT on purpose, so you "
          "can A/B the wet world against the dry one. The ground then dries out over a "
          "couple of minutes. External view, Earth only for now, and only below the "
          "weather: everything here - including the wet ground and the hull glint - fades "
          "out by about nine kilometres up, so a reentry begun with the pill still on gets "
          "a clean dry hull in space.", NULL },
{ HK_P,   "The overcast is not a screen filter. The storm light collapses the SUN at the "
          "source and lifts the ambient, so shadows and the warm directional cast go with "
          "it - the same light the puddles then reflect. Falling rain is a sheet between "
          "you and the world in three parallax layers, which is where rain actually is; one "
          "consequence accepted knowingly is that drops pass in front of vessels, because a "
          "drop a metre from your eye really is in front of a ship fifty metres away.", NULL },
{ HK_P,   "The deck overhead is ORO's own cloud layer: two textured decks at two "
          "altitudes - a ceiling and a darker scud layer hanging under it - with real "
          "parallax, vertical relief, and no repetition. The storm also carries its own "
          "LIGHTNING - flashes in the deck and bolts to the ground - whose controls live "
          "on the LIGHTNING page beside this one, under IN THE STORM.", NULL },
{ HK_P,   "The sliders come in three groups, in the page's own order: THE STORM OUTSIDE "
          "(everything in the world, visible from any seat), THE WINDSCREEN (drops on the "
          "cockpit glass), and SOUNDS.", NULL },
{ HK_ROW, "TEST", "The same storm as the pill, without enabling the effect - a quick look." },
{ HK_GAP, NULL, NULL },
{ HK_H,   "THE STORM OUTSIDE", NULL },
{ HK_ROW, "Gloom", "How dark and grey the world goes: the sun's collapse, the deck overhead "
          "and the visibility loss all ride it." },
{ HK_ROW, "Cloud detail", "The deck texture's detail notch - 0, 1, 2, 3. Zero is the plain "
          "darkened sky with no cloud texture at all; each step up adds a layer of finer "
          "billow filigree (256, 512, 1024 texels). The slider snaps to whole notches." },
{ HK_ROW, "Density", "How many streaks are in the falling sheet." },
{ HK_ROW, "Fall speed", "How fast they fall." },
{ HK_ROW, "Streak length", "How long each streak draws." },
{ HK_ROW, "Streak glow", "How brightly the streaks catch the light." },
{ HK_ROW, "Slant (deg)", "Wind. Tilts the whole sheet up to fifteen degrees either way." },
{ HK_ROW, "Splashes", "Rings where drops land, on ground and on water. Two fields - one "
          "around the camera, one around the ship - so a chase view still sees the ground "
          "fizzing where the eye actually looks." },
{ HK_ROW, "Wet dark", "How far the wet ground darkens. 1 is the designed look; 2 is "
          "near-black; standing water darkens further still." },
{ HK_ROW, "Pool size", "How large the standing pools grow, and at 0 whether there are any: "
          "turn it fully down for a soaked apron with no standing water at all. Pools only "
          "START once the ground is properly soaked - about seventy percent wet - because "
          "soil soaks first and water stands later. Three pattern scales give sheets in one "
          "stretch and speckle in the next, and the pools are PINNED TO THE GROUND: drive "
          "and they stay put, new ones ahead, old ones behind." },
{ HK_ROW, "Pool reach", "How far out pools stay visible before blending away - roughly "
          "nine hundred metres at 1. The damp sheen carries on past them, so distant "
          "ground still reads wet without the pattern marching to the horizon." },
{ HK_ROW, "Grain / Grain size", "The broken-water texture INSIDE the pool reflections - "
          "irregular matte patches torn through the mirror, static in the world like the "
          "pools themselves. Grain is how deeply they dig (0 = uniform pools); Grain size "
          "is how coarse the patches are." },
{ HK_ROW, "Glint", "Raindrop sparkle on hulls - every vessel in the scene, not just yours. "
          "It rides the SKY light, because a sparkle hung on the sun could not exist in "
          "the weather that makes things wet." },
{ HK_ROW, "Reflection", "The vessel image in the wet ground - a real mirrored render, so "
          "the reflection is upside down at the contact points and geometrically correct, "
          "concentrated in the pools. The grey sky in the pools is always there; this "
          "slider adds the SHIPS - hulls, nav lights and strobes, contrails and particle "
          "streams, and ORO's own engine plume. It is a genuine second render of the "
          "scene, not a copy of the picture, so anything in it is seen from under the "
          "water rather than flipped: at half resolution and through the ripple, fine "
          "structure like a shock-diamond train reads softer than it does in the air, "
          "which is what a reflection in moving water does." },
{ HK_ROW, "Reflection blur", "How DIFFUSE the reflection in the wet ground is. 0 is a "
          "crisp mirror; raise it and the image spreads and softens the way it does on a "
          "real wet apron, which scatters light rather than mirroring it. A little goes a "
          "long way - the reflection should still read as the ship, just not as glass. "
          "Costs nothing at 0." },
{ HK_ROW, "Swim size / Swim rate", "The rain-pocked ripple on that reflection: how far the "
          "image warps, and how fast it flickers. Size 0 is a dead-still mirror." },
{ HK_GAP, NULL, NULL },
{ HK_H,   "THE WINDSCREEN (VC)", NULL },
{ HK_ROW, "Glass drops", "Raindrops ON the cockpit glass (virtual cockpit only). Coverage: how "
          "much of the pane fills with drops at full storm. The window fills gradually and "
          "dries in reverse. Needs Sun glare on, and a mesh whose window groups carry a "
          "RAIN 1 line - the DeltaGlider and XR2 are done; see the readme for marking any "
          "other vessel's glass." },
{ HK_ROW, "Drop size", "How big the drops are, as the eye sees them - runners scale with it "
          "too, so one knob sizes everything on the glass. 0 is no drops at all." },
{ HK_ROW, "Drop lens", "How strongly each drop bends what is behind it. Past ~1 the image "
          "inside a big drop genuinely inverts, like the real lens a droplet is. 0 leaves "
          "drops that only glisten." },
{ HK_ROW, "Build up (s)", "Seconds from a clean canopy to the Glass drops target once the "
          "storm is at full strength. The fill is the show: drops pop in one by one and "
          "swell as they land." },
{ HK_ROW, "Runners", "Loose drops that break away and run across the glass, leaving a fading "
          "wet trail - straight down when parked, sweeping aft with airspeed. Their speed is "
          "not a knob: it follows gravity plus the real airflow." },
{ HK_ROW, "Runner size", "Runner thickness relative to the sitting drops - a ratio, so Drop "
          "size still scales both families together." },
{ HK_ROW, "Drop debug", "Temporary development aid: 1 draws drops everywhere (ignores the "
          "window mask), 2 paints the mask itself - green where the client sees authored "
          "window glass, blue where it sees interior. Leave at 0." },
{ HK_ROW, "Rain view", "Which INTERNAL views the rain is drawn in. VC ONLY is the default and the strictest: in a virtual cockpit every streak is cut at the window frame per pixel, so the cabin stays dry - that needs Sun glare enabled for the depth buffer, and without it the VC stays dry rather than showing drops indoors. VC + PANEL and ALL VIEWS add the 2D panel and the glass cockpit, where no depth is needed: those panels are painted over the rain by Orbiter itself, so they hide it for free. Outside views are always wet and are not affected by this." },
{ HK_GAP, NULL, NULL },
{ HK_H,   "SOUNDS", NULL },
{ HK_ROW, "Rain sound", "The storm's sound: three rain loops (patter / steady / downpour) "
          "crossfading as the storm builds, played through XRSound. This is the volume - "
          "1 is the designed mix against Orbiter's other ambient sounds, 0 is silent. It "
          "follows the storm's own gates, so it fades out above the weather. In the "
          "virtual cockpit the storm drops to a muffled 45% and a fourth loop takes its "
          "place: raindrops drumming on the hull itself. Needs XRSound.dll - absent, "
          "the row does nothing and the rain stays visual only. (Thunder lives with the "
          "bolts, on the LIGHTNING page.)" },
{ HK_ROW, "Hull drum", "Rain drumming on the SKIN of your ship - a fourth loop that plays only from inside a virtual cockpit, and the sound you actually notice in there. It has its own volume because it is about the vessel rather than the weather: turn it down for the storm without the drumming, and the outside mix does not move. 0 is silent." },
{ HK_GAP, NULL, NULL },
{ HK_ROW, "Rain", "Readout: the storm's build-up and how wet the ground is, or the honest "
          "reason nothing is drawn - external only (and the virtual cockpit, which sees "
          "the storm through its windows and stays dry inside), Earth only, above the "
          "weather. Vessels with large interiors can seal them with an authored roof "
          "mesh, Meshes\\ORO\\<class>_rainshield.msh - see the docs." },
{ HK_P,   "Saves GLOBALLY - per-world rain files arrive with the weather model.", NULL },
};

// --- PAGE: VIRTUAL COCKPIT (PILOT) ------------------------------------------
static const HelpItem HELP_VC[] = {
{ HK_H,   "WHAT THIS PAGE IS", NULL },
{ HK_P,   "The cockpit itself: light coming into it, and the seat you are sitting in.", NULL },
{ HK_P,   "VC SHADOWS is the one section where ORO DRAWS NOTHING. It hands the patched "
          "client two numbers and the client's own shadow pass does the work - which is why "
          "the whole section greys out on a client that cannot do it. A switch that cannot "
          "do anything is worse than no switch.", NULL },
{ HK_GAP, NULL, NULL },

{ HK_H,   "VC SHADOWS", NULL },
{ HK_P,   "Sunlight through the canopy, sweeping across the cabin as the ship rotates. Needs "
          "local shadows enabled in the D3D9 video settings.", NULL },
{ HK_ROW, "Cabin box (m)", "The size of the region the shadow map covers. The map is a fixed "
          "number of texels ACROSS that box, so halving the box doubles the resolution - and "
          "a cockpit panel is forty centimetres from your eye, where that is very visible. Go "
          "as low as 0.4 m for the crispest possible shadows." },
{ HK_ROW, "... and the cost, which is structural", "The box is also how far out the client "
          "looks for things that CAST. Shrink it far enough and a vessel docked outside your "
          "window stops throwing a shadow into the cabin. You cannot have both from a single "
          "shadow map, which is why this is a slider and not a baked-in number." },
{ HK_ROW, "Shadow depth", "How DARK the shadows go. 0 is Orbiter's stock behaviour, where a "
          "shadow only removes direct sunlight and the cabin's ambient keeps everything "
          "visible - which is why stock cockpit shadows look like a faint grey smudge, and "
          "why no brightness setting anywhere fixes it. Raising this lets the shadow take the "
          "ambient share with it. LIT INSTRUMENT PANELS ARE NEVER DIMMED: a canopy frame "
          "passing over an MFD does not turn the MFD off." },
{ HK_P,   "There is deliberately no shadow-filter control. That value is compiled into the "
          "client's shaders when the render window is created, so nothing can change it "
          "mid-session and a slider for it would be a control that lies. It belongs in the "
          "Launchpad D3D9 setup.", NULL },
{ HK_GAP, NULL, NULL },

{ HK_H,   "CAM-SHAKE", NULL },
{ HK_P,   "The one effect whose STRENGTH is not a slider. It is driven by thrust, dynamic "
          "pressure and ground contact - the sliders here shape the LOOK, not the amount. "
          "The caption under the section says so for that reason.", NULL },
{ HK_P,   "It is two different sensations from two different mechanisms, and you can have "
          "either without the other.", NULL },
{ HK_ROW, "Seat push", "The smooth sustained LEAN, opposite whichever way you are being "
          "accelerated: back into the seat under main thrust, down under hovers, forward "
          "under deceleration. 1 is standard, 0 removes the lean entirely and leaves you the "
          "rattle alone." },
{ HK_ROW, "X / Y / Z range (mm)", "Buffet amplitude per axis - the RATTLE. All three at zero "
          "leaves the seat push on its own, which is the other half of the split." },
{ HK_ROW, "Frequency (Hz)", "How fast it shakes." },
{ HK_ROW, "Test", "Forces full intensity at your tuned settings so you can judge it parked, "
          "instead of having to fly something violent to see what you just changed." },
{ HK_GAP, NULL, NULL },

{ HK_H,   "SAVING", NULL },
{ HK_P,   "This page writes to TWO scopes. The cabin box and the shadow depth are always PER "
          "VESSEL CLASS, because the right value depends on how big that cockpit is and how "
          "its materials were authored, not on who is flying it. The shadow on/off and the "
          "whole cam-shake section are GLOBAL by default - but the SAVE TARGET button at "
          "the top of the page moves them into the hull's file when you want that.", NULL },
{ HK_ROW, "Save target",  "Cam-shake is the reason this is here. A big heavy ship should "
                          "not rattle and shake like a tiny one, and the amplitude and "
                          "frequency knobs describe what a HULL passes through to the seat "
                          "rather than anything about you. A hull with no settings of its "
                          "own falls back to your global ones." },
};

// One table per PAGE - menus included, so HELP always answers for exactly the
// screen the user is looking at.
static const HelpItem* HelpText(int pg, int& n)
{
#define HT(tbl) do { n = (int)(sizeof(tbl) / sizeof(tbl[0])); return tbl; } while (0)
	switch (pg) {
	case PG_WORLD:     HT(HELP_M_WORLD);
	case PG_WEATHER:   HT(HELP_M_WEATHER);
	case PG_VESSEL:    HT(HELP_M_VESSEL);
	case PG_THRUSTERS: HT(HELP_M_THRUSTERS);
	case PG_REENTRY:   HT(HELP_M_REENTRY);
	case PG_PILOT:     HT(HELP_M_PILOT);
	case PG_GFORCES:   HT(HELP_GFORCES);
	case PG_SCENARIOS: HT(HELP_SCEN);
	case PG_VC:        HT(HELP_VC);
	case PG_EXHAUST:   HT(HELP_EXH);
	case PG_PARTICLES: HT(HELP_PRT);
	case PG_PLASMA:    HT(HELP_PLAS);
	case PG_VAPOUR:    HT(HELP_VAP);
	case PG_FLIGHTAID: HT(HELP_AID);
	case PG_RAIN:      HT(HELP_RAINP);
	case PG_LIGHTNING: HT(HELP_LTG);
	case PG_AURORA:    HT(HELP_AUR);
	case PG_ECLIPSE:   HT(HELP_ECL);
	case PG_GODRAYS:   HT(HELP_GRY);
	default:           HT(HELP_M_MAIN);   // PG_MAIN
	}
#undef HT
}

// The window's title is the page's own name, letter-spaced in the panel's caption
// style. Generated rather than tabulated, so a renamed page can never leave a
// stale title behind.
static const char* HelpTitle(int pg)
{
	static char t[96];
	const char* nm = PageName(pg);
	int j = 0;
	for (int i = 0; nm[i] && j < 92; i++) {
		if (i) {
			t[j++] = ' ';
			if (nm[i] == ' ' || nm[i - 1] == ' ') t[j++] = ' ';
		}
		t[j++] = nm[i];
	}
	t[j] = 0;
	return t;
}

// One pass that MEASURES and (optionally) DRAWS. Returns the document height. Keeping
// both in one function is what guarantees the scrollbar and the text agree - two walks
// would be two chances to drift apart.
// ⚠️ THE LAYOUT IS CACHED, AND THAT IS THE SIM-FREEZE FIX (2026-08-25).
// A public-beta tester: "Orbiter stops during scrolling ORO help window." It was not the
// 2026-08-16 timer bug - this window has no WM_TIMER case, so it never ate Orbiter's
// frame pump. It was simply too expensive to repaint. Every paint called this walk TWICE
// (once to measure the document, once to draw it) and each walk ran a word-wrapping
// DT_CALCRECT over EVERY entry - about two hundred of them - then drew all of them, most
// scrolled far off screen. Dragging the scrollbar repaints as fast as the message queue
// drains, so Orbiter's own frame loop never got a turn.
//
// Text metrics only change when the WIDTH or the TAB changes, so they are measured once
// and kept. A repaint now costs the entries actually on screen, which is a dozen or so.
// The cache is keyed on both, so reflow-on-resize (the reason the measuring exists at
// all) still works - it just stops happening sixty times a second.
#define HELP_CACHE_MAX 256
static int  s_hcTab = -1, s_hcW = -1;
static int  s_hcA[HELP_CACHE_MAX];      // primary text height per entry
static int  s_hcB[HELP_CACHE_MAX];      // HK_ROW description height

static int HelpWalk(HDC dc, const RECT& rc, bool bDraw)
{
	int n = 0;
	const HelpItem* it = HelpText(g_helpTab, n);
	const int w = rc.right - HELP_PAD * 2 - SB_W - SB_RPAD;
	int y = HELP_TOP;

	const bool cacheOK = (s_hcTab == g_helpTab && s_hcW == w && n <= HELP_CACHE_MAX);
	// Measure one block, or take the kept value. `wid` matters: the HK_ROW description is
	// indented, so it wraps at a narrower width than everything else.
	auto blockH = [&](int slot, bool second, const char* s, HFONT f, int wid) -> int {
		int* cache = second ? s_hcB : s_hcA;
		if (cacheOK && slot < HELP_CACHE_MAX) return cache[slot];
		SelectObject(dc, f);
		RECT m = { 0, 0, wid, 0 };
		DrawTextA(dc, s, -1, &m, DT_WORDBREAK | DT_CALCRECT);
		const int h = m.bottom - m.top;
		if (slot < HELP_CACHE_MAX) cache[slot] = h;
		return h;
	};
	// An entry wholly above or below the window is not drawn at all - the other half of
	// the cost, and the reason a long tab is no more expensive than a short one.
	auto onScreen = [&](int top, int h) -> bool {
		const int sy = top - g_helpScroll;
		return (sy + h > 0) && (sy < rc.bottom);
	};

	// The tab name this text belongs to, so a window left open on the far side of the
	// screen still says what it is describing.
	if (bDraw) {
		SelectObject(dc, g_hfTitle);
		SetTextColor(dc, CLR_ACCENT);
		TextOutA(dc, HELP_PAD, y - g_helpScroll, HelpTitle(g_helpTab),
		         (int)strlen(HelpTitle(g_helpTab)));
	}
	y += 30;
	if (bDraw) {
		RECT rule = { HELP_PAD, y - g_helpScroll - 8, rc.right - HELP_PAD, y - g_helpScroll - 7 };
		FillSolid(dc, rule, CLR_LINE);
	}

	for (int i = 0; i < n; i++) {
		switch (it[i].kind) {
		case HK_GAP:
			y += 16;
			break;
		case HK_H: {
			y += 16;
			const int h = blockH(i, false, it[i].a, g_hfHead, w);
			if (bDraw && onScreen(y, h + 8)) {
				SelectObject(dc, g_hfHead);
				SetTextColor(dc, CLR_TEXT_HI);
				RECT r = { HELP_PAD, y - g_helpScroll, HELP_PAD + w, y - g_helpScroll + h };
				DrawTextA(dc, it[i].a, -1, &r, DT_WORDBREAK);
				RECT rule = { HELP_PAD, y - g_helpScroll + h + 6,
				              rc.right - HELP_PAD, y - g_helpScroll + h + 7 };
				FillSolid(dc, rule, CLR_LINE);
			}
			y += h + 18;
			break;
		}
		case HK_ROW: {
			// Control name on its own line, description indented under it. A two-column
			// layout would look tidier and would fall apart the moment the window is
			// narrowed, which is exactly what this window invites the user to do.
			const int hn = blockH(i, false, it[i].a, g_hfName, w);
			if (bDraw && onScreen(y, hn)) {
				SelectObject(dc, g_hfName);
				SetTextColor(dc, CLR_PILL_ON);
				RECT r = { HELP_PAD, y - g_helpScroll, HELP_PAD + w, y - g_helpScroll + hn };
				DrawTextA(dc, it[i].a, -1, &r, DT_WORDBREAK);
			}
			y += hn + 3;
			if (it[i].b) {
				const int ind = HELP_PAD + 14;
				const int hd  = blockH(i, true, it[i].b, g_hfBody, HELP_PAD + w - ind);
				if (bDraw && onScreen(y, hd)) {
					SelectObject(dc, g_hfBody);
					SetTextColor(dc, CLR_TEXT);
					RECT r = { ind, y - g_helpScroll, HELP_PAD + w, y - g_helpScroll + hd };
					DrawTextA(dc, it[i].b, -1, &r, DT_WORDBREAK);
				}
				y += hd;
			}
			y += 14;
			break;
		}
		default: {   // HK_P
			const int h = blockH(i, false, it[i].a, g_hfBody, w);
			if (bDraw && onScreen(y, h)) {
				SelectObject(dc, g_hfBody);
				// CLR_TEXT, not CLR_TEXT_DIM: dim is right for a one-line hint under a
				// slider you are already looking at, and wrong for a paragraph someone
				// has to read. Running text gets the primary colour here.
				SetTextColor(dc, CLR_TEXT);
				RECT r = { HELP_PAD, y - g_helpScroll, HELP_PAD + w, y - g_helpScroll + h };
				DrawTextA(dc, it[i].a, -1, &r, DT_WORDBREAK);
			}
			y += h + 12;
			break;
		}
		}
	}
	// Stamp the cache valid only once a full pass has actually filled it. Doing this at
	// the END means an early return (there is none today) or a short tab can never leave
	// a half-populated table looking authoritative.
	if (n <= HELP_CACHE_MAX) { s_hcTab = g_helpTab; s_hcW = w; }
	return y + HELP_TOP;
}

static int HelpMaxScroll(const RECT& rc)
{
	const int mx = g_helpDoc - rc.bottom;
	return mx > 0 ? mx : 0;
}

static void HelpClampScroll(const RECT& rc)
{
	const int mx = HelpMaxScroll(rc);
	if (g_helpScroll > mx) g_helpScroll = mx;
	if (g_helpScroll < 0)  g_helpScroll = 0;
}

static RECT HelpThumbRect(const RECT& rc)
{
	const int mx = HelpMaxScroll(rc);
	RECT tr = { rc.right - SB_RPAD - SB_W, 2, rc.right - SB_RPAD, rc.bottom - 2 };
	if (mx <= 0) return tr;
	const int trackH = tr.bottom - tr.top;
	int th = (int)((double)trackH * (double)rc.bottom / (double)g_helpDoc);
	if (th < 24) th = 24;
	const int top = tr.top + (int)((double)(trackH - th) * (double)g_helpScroll / (double)mx);
	RECT r = { tr.left, top, tr.right, top + th };
	return r;
}

static void PaintHelp(HWND hWnd, HDC dcOut)
{
	RECT rc; GetClientRect(hWnd, &rc);
	const int W = rc.right, H = rc.bottom;

	HDC dc = CreateCompatibleDC(dcOut);
	HBITMAP bmp = CreateCompatibleBitmap(dcOut, W, H);
	HGDIOBJ oldBmp = SelectObject(dc, bmp);
	SetBkMode(dc, TRANSPARENT);
	RECT full = { 0, 0, W, H };
	FillSolid(dc, full, CLR_BG);

	// Measure first (the document height depends on the CURRENT width), clamp, then draw.
	g_helpDoc = HelpWalk(dc, rc, false);
	HelpClampScroll(rc);
	HelpWalk(dc, rc, true);

	if (HelpMaxScroll(rc) > 0) {
		RECT tr = { W - SB_RPAD - SB_W, 2, W - SB_RPAD, H - 2 };
		FillSolid(dc, tr, CLR_TRACK);
		RECT th = HelpThumbRect(rc);
		FillSolid(dc, th, g_helpDragBar >= 0 ? CLR_ACCENT : CLR_PILL_OFF);
	}

	BitBlt(dcOut, 0, 0, W, H, dc, 0, 0, SRCCOPY);
	SelectObject(dc, oldBmp);
	DeleteObject(bmp);
	DeleteDC(dc);
}

static INT_PTR CALLBACK OroHelpProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg) {
	case WM_INITDIALOG: {
		g_hHelp = hWnd;
		g_helpScroll = 0;
		CreateFontsOnce();
		CreateHelpFontsOnce();               // its own, reading-sized set - see the note there
		int w = 0, h = 0;
		OroSettings_LoadHelpSize(w, h);
		if (w < HELP_W_MIN) w = HELP_W_DEF;
		if (h < HELP_H_MIN) h = HELP_H_DEF;
		RECT want = { 0, 0, w, h };
		AdjustWindowRectEx(&want, GetWindowLongA(hWnd, GWL_STYLE), FALSE, GetWindowLongA(hWnd, GWL_EXSTYLE));
		// Open beside the panel rather than on top of it - you want to read the help and
		// look at the control it describes at the same time. Clamped to the work area, or
		// a panel already near the right edge would push this one off screen entirely.
		const int wW = want.right - want.left, wH = want.bottom - want.top;
		int x = 0, y = 0;
		bool place = false;
		if (g_hDlg) {
			RECT pr; GetWindowRect(g_hDlg, &pr);
			RECT wa = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
			SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
			x = pr.right + 8; y = pr.top;
			if (x + wW > wa.right)  x = pr.left - 8 - wW;   // no room right? go left
			if (x < wa.left)        x = wa.left;            // no room either side? overlap
			if (y + wH > wa.bottom) y = wa.bottom - wH;
			if (y < wa.top)         y = wa.top;
			place = true;
		}
		SetWindowPos(hWnd, NULL, x, y, wW, wH,
		             (place ? 0 : SWP_NOMOVE) | SWP_NOZORDER | SWP_NOACTIVATE);
		RECT cr; GetClientRect(hWnd, &cr);
		g_helpSavedW = cr.right; g_helpSavedH = cr.bottom;
		return TRUE;
	}

	case WM_ERASEBKGND:
		return TRUE;                         // fully painted in WM_PAINT, no flicker

	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hWnd, &ps);
		PaintHelp(hWnd, dc);
		EndPaint(hWnd, &ps);
		return TRUE;
	}

	case WM_GETMINMAXINFO: {
		MINMAXINFO* mmi = (MINMAXINFO*)lParam;
		RECT fr = { 0, 0, HELP_W_MIN, HELP_H_MIN };
		AdjustWindowRectEx(&fr, GetWindowLongA(hWnd, GWL_STYLE), FALSE, GetWindowLongA(hWnd, GWL_EXSTYLE));
		mmi->ptMinTrackSize.x = fr.right - fr.left;
		mmi->ptMinTrackSize.y = fr.bottom - fr.top;
		return TRUE;
	}

	case WM_SIZE:
		// The text REFLOWS, so the document height changes with the width as well as the
		// height - the clamp has to happen against a freshly measured document, which the
		// next paint does. Just invalidate.
		InvalidateRect(hWnd, NULL, FALSE);
		break;

	case WM_EXITSIZEMOVE: {
		RECT cr; GetClientRect(hWnd, &cr);
		if (cr.right != g_helpSavedW || cr.bottom != g_helpSavedH) {
			g_helpSavedW = cr.right; g_helpSavedH = cr.bottom;
			OroSettings_SaveHelpSize(cr.right, cr.bottom);
		}
		break;
	}

	case WM_MOUSEWHEEL: {
		RECT rc; GetClientRect(hWnd, &rc);
		g_helpScroll -= (GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA) * 48;
		HelpClampScroll(rc);
		InvalidateRect(hWnd, NULL, FALSE);
		return TRUE;
	}

	case WM_LBUTTONDOWN: {
		RECT rc; GetClientRect(hWnd, &rc);
		const int x = (short)LOWORD(lParam), y = (short)HIWORD(lParam);   // house idiom
		if (HelpMaxScroll(rc) > 0 && x >= rc.right - SB_RPAD - SB_W - 4) {
			RECT th = HelpThumbRect(rc);
			if (y >= th.top && y <= th.bottom) g_helpDragBar = y - th.top;
			else { g_helpScroll += (y < th.top ? -1 : 1) * rc.bottom; HelpClampScroll(rc); }
			SetCapture(hWnd);
			InvalidateRect(hWnd, NULL, FALSE);
			return TRUE;
		}
		return FALSE;
	}

	case WM_MOUSEMOVE: {
		if (g_helpDragBar < 0) return FALSE;
		RECT rc; GetClientRect(hWnd, &rc);
		const int mx = HelpMaxScroll(rc);
		RECT tr = { 0, 2, 0, rc.bottom - 2 };
		RECT th = HelpThumbRect(rc);
		const int trackH = (tr.bottom - tr.top) - (th.bottom - th.top);
		if (trackH > 0 && mx > 0) {
			const int top = (short)HIWORD(lParam) - g_helpDragBar - tr.top;
			g_helpScroll = (int)((double)top * (double)mx / (double)trackH);
			HelpClampScroll(rc);
			InvalidateRect(hWnd, NULL, FALSE);
		}
		return TRUE;
	}

	case WM_LBUTTONUP:
		if (g_helpDragBar >= 0) { g_helpDragBar = -1; ReleaseCapture(); }
		return TRUE;

	// Same beep guard as the main panel - see the long note there. This window is an
	// empty template too, and being the newest thing on screen it is the one most likely
	// to be holding focus when the user reaches for the throttle.
	case WM_CHAR:
	case WM_SYSCHAR:
		return TRUE;

	case WM_COMMAND:
		if (LOWORD(wParam) == IDCANCEL) { oapiCloseDialog(hWnd); return TRUE; }
		return FALSE;

	case WM_DESTROY:
		g_hHelp = NULL;
		g_helpDragBar = -1;
		return TRUE;
	}
	return oapiDefDialogProc(hWnd, uMsg, wParam, lParam);
}

// Open, or - if it is already up - just re-point it at the current tab and raise it.
// ⚠️ NEVER A SECOND WINDOW: the button is in the fixed strip and is therefore reachable
// from every tab at every scroll position, so pressing it twice is the normal case, not
// the exceptional one. Re-pointing rather than ignoring is what makes the second press
// useful: you walk to another page, press HELP, and get that page's text in the window
// you already have open.
static void OroHelp_Open(HINSTANCE hInst, int page)
{
	const bool retarget = (g_hHelp != NULL);
	g_helpTab = page;
	if (retarget) {
		g_helpScroll = 0;                    // new subject, start at the top
		InvalidateRect(g_hHelp, NULL, FALSE);
		// NOT SetForegroundWindow: an Orbiter dialog is a CHILD of the render window, and
		// that call is for top-level windows - it would raise Orbiter, not this. HWND_TOP
		// within the parent is the correct move and works in fullscreen too.
		SetWindowPos(g_hHelp, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
		return;
	}
	oapiOpenDialogEx(hInst, IDD_ORO_HELP, OroHelpProc, DLG_CAPTIONCLOSE, NULL);
}

// THE HELP FOLLOWS THE NAVIGATION (2026-08-29, his ask): while the window is open,
// walking the panel re-points it at the page you arrive on, so the text beside you
// is always about the screen in front of you. Deliberately does NOT raise the
// window the way a HELP press does - you are working in the panel, and an
// auto-follow that shuffles z-order would fight you for the mouse.
static void OroHelp_Follow()
{
	if (!g_hHelp || g_helpTab == CurPage()) return;
	g_helpTab    = CurPage();
	g_helpScroll = 0;                        // new subject, start at the top
	InvalidateRect(g_hHelp, NULL, FALSE);
}

static void OroHelp_Close()
{
	if (g_hHelp) oapiCloseDialog(g_hHelp);   // WM_DESTROY clears g_hHelp
}

// ----------------------------------------------------------------------------
// Message handler
// ----------------------------------------------------------------------------
static INT_PTR CALLBACK OroDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg) {

	case WM_INITDIALOG: {
		g_hDlg = hDlg;
		g_scroll = 0;
		CreateFontsOnce();
		// Force the CLIENT area to exactly DLG_W x DLG_H px. The .rc size is in
		// dialog units, whose pixel size depends on the shell font metrics and so
		// varies per machine - this is what makes the layout constants in this file
		// mean the same thing everywhere.
		// The HEIGHT is whatever it was last left at (Config\ORO\window.cfg); DLG_H is
		// only the fallback for a first run. Clamped both ways: never below DLG_H_MIN,
		// and never taller than this machine's work area - a height saved on a bigger
		// monitor must not open off the bottom of a smaller one.
		{
			int h = OroSettings_LoadDlgHeight();
			const bool restored = (h > 0);
			if (!restored) h = DLG_H;
			int hMax = GetSystemMetrics(SM_CYMAXTRACK) - 80;   // leave room for the frame
			if (hMax < DLG_H_MIN) hMax = DLG_H_MIN;
			if (h < DLG_H_MIN) h = DLG_H_MIN;
			if (h > hMax)      h = hMax;

			RECT want = { 0, 0, DLG_W, h };
			AdjustWindowRectEx(&want, GetWindowLongA(hDlg, GWL_STYLE), FALSE, GetWindowLongA(hDlg, GWL_EXSTYLE));
			SetWindowPos(hDlg, NULL, 0, 0, want.right - want.left, want.bottom - want.top,
			             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
			RECT rc; GetClientRect(hDlg, &rc);
			g_lastSavedH = rc.bottom;        // so a pure MOVE never rewrites the file
			oapiWriteLogV("ORO: dialog client %d x %d px (%s; width locked, min height %d), "
			              "content %d px, scroll range %d.",
			              rc.right, rc.bottom, restored ? "height restored" : "default height",
			              DLG_H_MIN, ContentHeight(), MaxScroll(rc));
		}
		// ~10 Hz repaint: keeps the ENABLED toggle in sync with Ctrl+G and animates
		// the sliders while an INDUCE scenario drives them. Double-buffered, no flicker.
		SetTimer(hDlg, 1, 100, NULL);
		return TRUE;
	}

	case WM_ERASEBKGND:
		return TRUE;  // we paint every pixel - suppress the white flash

	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hDlg, &ps);
		PaintDialog(hDlg, dc);
		EndPaint(hDlg, &ps);
		return TRUE;
	}

	case WM_MOUSEWHEEL: {
		RECT rc; GetClientRect(hDlg, &rc);
		const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
		g_scroll -= notches * ROW_DY * 3;
		ClampScroll(rc);
		InvalidateRect(hDlg, NULL, FALSE);
		return TRUE;
	}

	case WM_LBUTTONDOWN: {
		const int x = (short)LOWORD(lParam), y = (short)HIWORD(lParam);
		RECT rc; GetClientRect(hDlg, &rc);

		// Colour picker overlay first - it is topmost, so while open it owns every
		// click (inside: the controls; outside: dismiss-keeping, and the click is
		// eaten so nothing underneath fires on the same press).
		if (PickMouseDown(hDlg, x, y)) return TRUE;

		// Master arm lives in the fixed strip - always reachable, scenario or not.
		if (PtIn(ArmedBtnRect(), x, y)) {
			g_fx.masterArmed = !g_fx.masterArmed;   // same flag Ctrl+G flips
			InvalidateRect(hDlg, NULL, FALSE);
			return TRUE;
		}
		// The engine-group cycler (the thruster pages' fixed header row). One click
		// banks the sliders into the group you were editing and brings up the next
		// one - see OroThr_Cycle.
		if (PageHasGrpRow(CurPage()) && OroThr_Count() > 1 && PtIn(ThrGrpBtnRect(rc), x, y)) {
			OroThr_Cycle();
			ClearDrags();                        // never leave a drag on a replaced value
			InvalidateRect(hDlg, NULL, FALSE);
			return TRUE;
		}
		// Phase B: the thruster cycler, MARK and CLEAR - fixed chrome beside the group
		// button. None of these go through the edit tail: cycling and MARK never amber
		// (a cursor and a test rig), and CLEAR marks the class scope ITSELF - calling
		// MarkDirty would re-create the very override it just dropped.
		if (PageHasGrpRow(CurPage())) {
			const bool canCyc = (g_fx.thrCnt >= 2);
			if (canCyc && PtIn(ThrPrevBtnRect(rc), x, y)) {
				OroThr_CycleThr(-1);
				ClearDrags();
				InvalidateRect(hDlg, NULL, FALSE);
				return TRUE;
			}
			if (canCyc && PtIn(ThrNextBtnRect(rc), x, y)) {
				OroThr_CycleThr(+1);
				ClearDrags();
				InvalidateRect(hDlg, NULL, FALSE);
				return TRUE;
			}
			if (PtIn(ThrMarkBtnRect(rc), x, y)) {
				g_fx.thrMarkOn = !g_fx.thrMarkOn;    // test-rig class: never persisted
				InvalidateRect(hDlg, NULL, FALSE);
				return TRUE;
			}
			OroThrOvr* o = (g_fx.thrThrSel >= 0) ? OroThr_FindOvr(g_fx.thrThrSel) : NULL;
			const int  fam = (CurPage() == PG_EXHAUST) ? ORO_FAM_EXH : ORO_FAM_PRT;
			const bool famOwned = o && ((fam == ORO_FAM_EXH) ? o->ovrExh : o->ovrPrt);
			if (famOwned && PtIn(ThrClearBtnRect(rc), x, y)) {
				OroThr_ClearOvr(fam);                // back to inheriting, live
				g_dirtyScopes |= LeafSaveMask(CurPage());  // dropping a block IS an edit
				ClearDrags();
				InvalidateRect(hDlg, NULL, FALSE);
				return TRUE;
			}
		}
		// HELP, ditto - and it opens the text for whichever page is ACTIVE, menus
		// included, so the question it answers is "what is all this in front of me".
		if (PtIn(HelpBtnRect(rc), x, y)) {
			OroHelp_Open(g_hInst, CurPage());
			InvalidateRect(hDlg, NULL, FALSE);   // the button lights up
			return TRUE;
		}
		// SAVE, ditto. Confirmation goes to the status line for a few seconds - a
		// button that writes a file and says nothing is a button you press twice.
		if (PtIn(SaveBtnRect(rc), x, y)) {
			g_saveMask = ORO_SCOPE_GLOBAL | ORO_SCOPE_CLASS | ORO_SCOPE_BODY;
			g_saveOk = OroSettings_Save();
			// ⚠️ THIS FLAG WAS NEVER CLEARED HERE (found and fixed 2026-08-25, while adding
			// the press feedback). The per-tab REVERT raises it and only the per-tab SAVE
			// lowered it again, so REVERT on any tab followed by THIS button reported
			// "Reverted: ..." for a write that had just succeeded - the status line saying
			// the exact opposite of what had happened, and now in a colour to match.
			g_saveWasRevert = false;
			if (g_saveOk) g_dirtyScopes = 0;   // everything is on disk - amber off
			g_saveMsgUntil = GetTickCount() + 4000;
			g_btnFlashWhat = 1; g_btnFlashUntil = GetTickCount() + BTN_FLASH_MS;
			InvalidateRect(hDlg, NULL, FALSE);
			return TRUE;
		}

		// Scrollbar: grab the thumb, or page toward a click elsewhere on the track.
		if (MaxScroll(rc) > 0 && PtIn(ScrollTrackRect(rc), x, y, 4)) {
			RECT th = ScrollThumbRect(rc);
			if (PtIn(th, x, y)) {
				g_dragBar = y - th.top;             // remember the grab point inside the thumb
			} else {
				g_scroll += (y < th.top ? -1 : 1) * PaneHeight(rc);
				ClampScroll(rc);
			}
			SetCapture(hDlg);
			InvalidateRect(hDlg, NULL, FALSE);
			return TRUE;
		}

		// Nav row: fixed strip, so test the RAW client y. Both buttons are inert at
		// the main menu (drawn dim to match). Navigation resets the scroll - every
		// page starts at its own top - and clears any drag in flight.
		if (g_navDepth > 1 && PtIn(NavBackBtnRect(rc), x, y)) {
			NavBack();
			ClearDrags();
			OroHelp_Follow();                // an open help window walks with you
			InvalidateRect(hDlg, NULL, FALSE);
			return TRUE;
		}
		if (g_navDepth > 1 && PtIn(NavMainBtnRect(rc), x, y)) {
			NavHome();
			ClearDrags();
			OroHelp_Follow();
			InvalidateRect(hDlg, NULL, FALSE);
			return TRUE;
		}

		// The leaf's SAVE/REVERT row - FIXED chrome since the visual pass, so it is
		// tested in RAW client coordinates like everything above, and reachable at any
		// scroll depth. SAVE writes only the scopes this page can have changed, and the
		// status line names the files, same as the global one.
		if (!IsMenuPage(CurPage()) && PtIn(LeafSaveBtnRect(rc), x, y)) {
			g_saveMask = LeafSaveMask(CurPage());
			g_saveOk = OroSettings_SaveScope(g_saveMask);
			g_saveWasRevert = false;
			if (g_saveOk) g_dirtyScopes &= ~g_saveMask;   // those scopes are on disk now
			g_saveMsgUntil = GetTickCount() + 4000;
			g_btnFlashWhat = 2; g_btnFlashUntil = GetTickCount() + BTN_FLASH_MS;
			InvalidateRect(hDlg, NULL, FALSE);
			return TRUE;
		}
		// REVERT - re-read this page's own scopes from disk, discarding everything moved
		// since the last save. The three loaders are the same ones the module calls on a
		// focus/world change, so a revert is exactly "arrive at this hull again".
		// ⚠️ A SCOPE WITH NO FILE MUST NOT BE TOUCHED, and the two loaders differ on that
		// by design (invariant 17a): LoadClass KEEPS the current numbers for an unconfigured
		// hull, LoadBody restores the built-in DEFAULTS. Both are the right answer to
		// "revert", because both are what you would have had if you had never touched
		// anything - so this hands the question straight to them rather than second-guessing.
		if (!IsMenuPage(CurPage()) && PtIn(LeafRevBtnRect(rc), x, y)) {
			const int m = LeafSaveMask(CurPage());
			OroSettings_Revert(m);
			g_saveMask = m;
			g_saveOk = true;             // a load has nothing to fail at: a missing file
			g_saveWasRevert = true;      // simply means "the defaults", which is a revert too
			g_dirtyScopes &= ~m;         // live == disk again for those scopes
			g_saveMsgUntil = GetTickCount() + 4000;
			g_btnFlashWhat = 3; g_btnFlashUntil = GetTickCount() + BTN_FLASH_MS;
			ClearDrags();                // never leave a drag pointing at a value we replaced
			InvalidateRect(hDlg, NULL, FALSE);
			return TRUE;
		}

		// Everything else is in the scrolling content pane: reject clicks outside it,
		// then convert client y -> document y once and dispatch to the CURRENT page.
		if (y < ContentY() || y >= PaneBottom(rc)) return FALSE;
		const int dy = y + g_scroll;

		// MENU pages: the big buttons. A COMING SOON entry eats the click and does
		// nothing - it is a signpost, not a control. Deliberately NOT behind the
		// scenario lock: you must always be able to walk to the SCENARIOS page.
		if (IsMenuPage(CurPage())) {
			int n; const MenuItem* it = MenuOf(CurPage(), n);
			for (int i = 0; i < n; i++) {
				if (PtIn(MenuBtnRect(rc, i), x, dy)) {
					if (it[i].target >= 0) {
						NavPush(it[i].target);
						ClearDrags();
						OroHelp_Follow();    // an open help window walks with you
					}
					InvalidateRect(hDlg, NULL, FALSE);
					return TRUE;
				}
			}
			return FALSE;
		}

		BOOL handled = FALSE;
		g_clickWasEdit = true;           // session-only controls lower it on their way out
		g_editGrpLevel = false;          // Phase B: bell handlers raise it (group-level)
		switch (CurPage()) {
		case PG_EXHAUST:
			handled = ClickThruster(hDlg, rc, x, dy);
			break;
		case PG_PARTICLES:
			handled = ClickParticles(hDlg, rc, x, dy);
			break;
		case PG_PLASMA:
			handled = ClickReentry(hDlg, rc, x, dy);
			break;
		case PG_VAPOUR:
			handled = ClickVapour(hDlg, rc, x, dy);
			break;
		case PG_FLIGHTAID: // flies the SHIP, not the effects - never locked
			handled = ClickFlightAid(hDlg, rc, x, dy);
			break;
		case PG_RAIN:
			handled = ClickRain(hDlg, rc, x, dy);
			break;
		case PG_LIGHTNING:
			handled = ClickLightning(hDlg, rc, x, dy);
			break;
		case PG_AURORA:
			handled = ClickAurora(hDlg, rc, x, dy);
			break;
		case PG_ECLIPSE:
			handled = ClickEclipse(hDlg, rc, x, dy);
			break;
		case PG_GODRAYS:
			handled = ClickGodRays(hDlg, rc, x, dy);
			break;
		case PG_VC:
			if (PtIn(PilotBtnRect(VcTgtY(), 150), x, dy)) {
				g_fx.vcPerClass = !g_fx.vcPerClass;   // where the NEXT save goes
				handled = TRUE;
				break;
			}
			handled = ClickVCShadows(hDlg, rc, x, dy) || ClickCamShake(hDlg, rc, x, dy);
			break;
		case PG_SCENARIOS:
			// Never behind the scenario lock: stopping or muting a running scenario is
			// exactly what this page is for.
			handled = ClickScenarios(rc, x, dy);
			break;
		default: // PG_GFORCES
			// PILOT bypasses the scenario lock by design: you must be able to leave
			// PHYSICS mode at will (stopping the scenario itself lives on SCENARIOS).
			if (ClickPilot(hDlg, rc, x, dy)) { handled = TRUE; break; }
			// While a scenario plays, the VISION/MOTION controls are LOCKED (no knob-turning);
			// the sliders still ANIMATE to show the scenario via the repaint timer. The
			// swallowed click is NOT an edit, so it returns here rather than falling into
			// the MarkDirty tail below - a locked knob must not light the amber.
			if (g_fx.seqActive >= 0) { InvalidateRect(hDlg, NULL, FALSE); return TRUE; }
			handled = ClickVision(hDlg, rc, x, dy) || ClickMotion(hDlg, rc, x, dy);
			break;
		}
		if (handled) {
			if (g_clickWasEdit) MarkDirty();   // a SAVED value changed - see g_dirtyScopes
			InvalidateRect(hDlg, NULL, FALSE);
		}
		return handled;
	}

	case WM_MOUSEMOVE: {
		RECT rc; GetClientRect(hDlg, &rc);
		const int x = (short)LOWORD(lParam), y = (short)HIWORD(lParam);
		if (PickMouseMove(hDlg, x, y)) return TRUE; // picker drags (live preview) first
		if (g_dragBar >= 0) {                       // scrollbar drag works during scenarios too
			ScrollFromThumbTop(rc, y - g_dragBar);  //   (and is NOT an edit - no dirty mark)
			InvalidateRect(hDlg, NULL, FALSE);
			return TRUE;
		}
		if (g_dragTol >= 0) {                       // tolerance is a model setting, not an
			g_fx.gTolerance = TrackValueFromX(rc, x);  // effect value - a scenario can't own it
			MarkDirty();
			InvalidateRect(hDlg, NULL, FALSE);
			return TRUE;
		}
		if (g_dragCop >= 0) {                       // ditto: this one flies the ship
			g_fx.copShift = EnvKnobValueFromX(rc, x, COP_MAX);
			MarkDirty();
			InvalidateRect(hDlg, NULL, FALSE);
			return TRUE;
		}
		if (g_fx.seqActive >= 0) return FALSE;      // scenario owns the values
		if      (g_dragRow   >= 0) *RowKnob(g_visRows[g_dragRow])  = TrackValueFromX(rc, x);
		else if (g_dragMot   >= 0) *RowKnob(g_motRows[g_dragMot])  = TrackValueFromX(rc, x);
		else if (g_dragShake >= 0) *g_shakeRows[g_dragShake].value = TrackValueFromX(rc, x) * g_shakeRows[g_dragShake].vmax;
		else if (g_dragEnv   >= 0) *RowKnob(g_envRows[g_dragEnv])  = TrackValueFromX(rc, x);
		else if (g_dragEnvK  >= 0) *g_envKnobs[g_dragEnvK].value   = EnvKnobValueFromX(rc, x, g_envKnobs[g_dragEnvK].vmax);
		else if (g_dragPlume >= 0) *g_plumeRows[g_dragPlume].value = g_plumeRows[g_dragPlume].vmin
		                                                           + TrackValueFromX(rc, x) * (g_plumeRows[g_dragPlume].vmax - g_plumeRows[g_dragPlume].vmin);
		else if (g_dragPlmBand >= 0) PlmBandDrag(TrackValueFromX(rc, x));
		else if (g_dragBgl   == 0) g_fx.plumeBellGlow  = TrackValueFromX(rc, x) * 2.0f;
		else if (g_dragBgl   == 1) g_fx.plumeBellHeatT = 1.0f + TrackValueFromX(rc, x) * 19.0f;
		else if (g_dragBgl   == 2) g_fx.plumeBellCoolT = 5.0f + TrackValueFromX(rc, x) * 115.0f;
		else if (g_dragPrt   >= 0) *g_prtRows[g_dragPrt].value = g_prtRows[g_dragPrt].vmin
		                                                       + TrackValueFromX(rc, x) * (g_prtRows[g_dragPrt].vmax - g_prtRows[g_dragPrt].vmin);
		else if (g_dragPlas  >= 0) *g_plasRows[g_dragPlas].value   = g_plasRows[g_dragPlas].vmin
		                                                           + TrackValueFromX(rc, x) * (g_plasRows[g_dragPlas].vmax - g_plasRows[g_dragPlas].vmin);
		else if (g_dragEcl   >= 0) *g_eclRows[g_dragEcl].value     = TrackValueFromX(rc, x) * g_eclRows[g_dragEcl].vmax;
		else if (g_dragAur   >= 0) *g_aurRows[g_dragAur].value     = TrackValueFromX(rc, x) * g_aurRows[g_dragAur].vmax;
		else if (g_dragAurRib >= 0) { const int n = 1 + (int)(TrackValueFromX(rc, x) * 5.0f + 0.5f); g_fx.auroraRibbons = (n < 1) ? 1 : (n > 6 ? 6 : n); }
		else if (g_dragAurK  >= 0) *g_aurKnobs[g_dragAurK].value   = EnvKnobValueFromX(rc, x, g_aurKnobs[g_dragAurK].vmax);
		else if (g_dragLtg   >= 0) *g_ltgRows[g_dragLtg].value     = TrackValueFromX(rc, x) * g_ltgRows[g_dragLtg].vmax;
		else if (g_dragRlt   >= 0) *g_rltRows[g_dragRlt].value     = TrackValueFromX(rc, x) * g_rltRows[g_dragRlt].vmax;
		else if (g_dragGry   >= 0) *g_gryRows[g_dragGry].value     = TrackValueFromX(rc, x) * g_gryRows[g_dragGry].vmax;
		else if (g_dragRain  >= 0) {
			*g_rainRows[g_dragRain].value = g_rainRows[g_dragRain].vmin
			         + TrackValueFromX(rc, x) * (g_rainRows[g_dragRain].vmax - g_rainRows[g_dragRain].vmin);
			// ⚠️ SNAP HERE TOO (2026-08-24). The PRESS handler has snapped dec-0 rain rows
			// to integers since the notches landed; this DRAG continuation never did, so
			// the control was half-snapped - click it and Cloud detail went to 3, drag it
			// and it stopped at 2.55. A public-beta tester reported it as "make Cloud
			// detail move discretely", which is not a new request: it is the behaviour the
			// press handler already implements, finished.
			// The usual lesson, third time today: the rule was applied where the evidence
			// pointed (the click) and not swept to the other path that does the same job.
			if (g_rainRows[g_dragRain].dec == 0)
				*g_rainRows[g_dragRain].value = floorf(*g_rainRows[g_dragRain].value + 0.5f);
		}
		else if (g_dragVap   >= 0) *g_vapRows[g_dragVap].value     = TrackValueFromX(rc, x) * g_vapRows[g_dragVap].vmax;
		else if (g_dragVap2  >= 0) *g_vapRows2[g_dragVap2].value  = TrackValueFromX(rc, x) * g_vapRows2[g_dragVap2].vmax;
		else if (g_dragVapP  == 2) g_fx.vapPos2  = EnvKnobValueFromX(rc, x, VAP_POS_MAX);
		else if (g_dragVapP  == 3) g_fx.vapPosX  = EnvKnobValueFromX(rc, x, VAP_POS_MAX);
		else if (g_dragVapP  == 4) g_fx.vapPosY  = EnvKnobValueFromX(rc, x, VAP_POS_MAX);
		else if (g_dragVapP  == 5) g_fx.vapPosX2 = EnvKnobValueFromX(rc, x, VAP_POS_MAX);
		else if (g_dragVapP  == 6) g_fx.vapPosY2 = EnvKnobValueFromX(rc, x, VAP_POS_MAX);
		else if (g_dragVapP  >= 0) g_fx.vapPos   = EnvKnobValueFromX(rc, x, VAP_POS_MAX);
		else if (g_dragVapR  == 1) g_fx.vapPitch  = EnvKnobValueFromX(rc, x, VAP_ROT_MAX);
		else if (g_dragVapR  == 2) g_fx.vapYaw    = EnvKnobValueFromX(rc, x, VAP_ROT_MAX);
		else if (g_dragVapR  == 3) g_fx.vapPitch2 = EnvKnobValueFromX(rc, x, VAP_ROT_MAX);
		else if (g_dragVapR  == 4) g_fx.vapYaw2   = EnvKnobValueFromX(rc, x, VAP_ROT_MAX);
		else if (g_dragVapBand >= 0) VapBandDrag(TrackValueFromX(rc, x));
		else if (g_dragVapBand2 >= 0) VapBandDragC(TrackValueFromX(rc, x), g_dragVapBand2,
		                                           g_fx.vapMachMin2, g_fx.vapMachMax2);
		else if (g_dragVcs   == 1) g_fx.vcShadowDepth  = TrackValueFromX(rc, x);
		else if (g_dragVcs   >= 0) g_fx.vcShadowRadius = VCS_RAD_MIN + TrackValueFromX(rc, x) * (VCS_RAD_MAX - VCS_RAD_MIN);
		else return FALSE;
		MarkDirty();                                // a value moved - see g_dirtyScopes
		InvalidateRect(hDlg, NULL, FALSE);
		return TRUE;
	}

	case WM_LBUTTONUP:
		if (g_pickDrag >= 0) {                      // picker drag ends; overlay stays open
			g_pickDrag = -1;
			ReleaseCapture();
			return TRUE;
		}
		if (g_dragRow >= 0 || g_dragMot >= 0 || g_dragShake >= 0 || g_dragEnv >= 0
		    || g_dragEnvK >= 0 || g_dragPlume >= 0 || g_dragPlmBand >= 0 || g_dragBgl >= 0
		    || g_dragPrt >= 0 || g_dragPlas >= 0
		    || g_dragEcl >= 0 || g_dragAur >= 0 || g_dragAurRib >= 0
		    || g_dragAurK >= 0 || g_dragLtg >= 0 || g_dragRlt >= 0 || g_dragGry >= 0
		    || g_dragRain >= 0 || g_dragVcs >= 0 || g_dragTol >= 0
		    || g_dragVap >= 0 || g_dragVap2 >= 0 || g_dragVapP >= 0 || g_dragVapR >= 0
		    || g_dragVapBand >= 0 || g_dragVapBand2 >= 0
		    || g_dragCop >= 0 || g_dragBar >= 0) {
			ClearDrags();
			ReleaseCapture();
			InvalidateRect(hDlg, NULL, FALSE);
			return TRUE;
		}
		return FALSE;

	// ⚠️ TEST THE TIMER ID. THE DIALOG-DRAG SIM FREEZE WAS THIS ONE LINE (2026-08-16).
	// OrbiterDefDialogProc ALREADY SOLVES THE FREEZE: on WM_ENTERSIZEMOVE it starts a
	// 1 ms timer with id 0xff, and its own WM_TIMER case calls g_pOrbiter->SingleFrame(),
	// so Orbiter keeps rendering right through Windows' modal move loop. That has been in
	// the core all along - and ORO was eating it. This case returned TRUE for EVERY timer
	// id, so 0xff never reached oapiDefDialogProc and SingleFrame() never ran.
	// A beta tester reported the freeze as an unavoidable cost of dragging a window. It
	// was ours, and the fix is one comparison.
	// (An earlier attempt the same day PAUSED the sim across the drag, to stop the giant
	// dt on release that once threw landed vessels into the air. That was treating a
	// symptom we had caused - and it would have held the sim stopped while Orbiter tried
	// to pump frames through it. Removed in favour of this. THE LESSON IS THE USUAL ONE:
	// read the client/core before building a workaround, because it had already been
	// solved upstream and we were the reason it did not work.)
	case WM_TIMER:
		if (wParam == 1) {                       // OUR ~10 Hz repaint, and only ours
			InvalidateRect(hDlg, NULL, FALSE);   // keeps ENABLED live vs Ctrl+G
			return TRUE;
		}
		break;                                   // anything else -> Orbiter's frame pump

	// ⚠️ EAT WM_CHAR OR WINDOWS DINGS ON EVERY KEYPRESS.
	// DefDlgProc treats a character as a possible control MNEMONIC: it searches the
	// dialog's children for a matching &accelerator and, finding none, calls
	// MessageBeep. This template has NO CHILD CONTROLS AT ALL - every pill, slider and
	// button here is painted in WM_PAINT - so the search can never succeed and EVERY
	// character keypress beeps while the panel holds focus. And it holds focus easily:
	// OrbiterDefDialogProc implements focus-follows-mouse, so merely moving the pointer
	// over the panel makes it the keyboard target.
	// The keys still reach the sim regardless (Orbiter's own input path is upstream of
	// this), which is exactly why the symptom is "my engines respond AND it dings".
	// Consuming the message removes the beep and nothing else: there is no edit control,
	// no mnemonic and no accelerator in this window that would ever want it. Escape and
	// the caption close still arrive as WM_COMMAND/IDCANCEL, which is untouched.
	case WM_CHAR:
	case WM_SYSCHAR:
		return TRUE;

	// ------------------------------------------------------------------------
	// VERTICAL RESIZE (2026-08-16). The .rc carries WS_THICKFRAME; these three cases are
	// the whole of the behaviour, because every layout helper already derived from
	// rc.bottom (PaneBottom, PaneHeight, MaxScroll, the status line, the scrollbar) - the
	// panel has been resize-ready for months without anyone noticing.
	//
	// THE WIDTH IS PINNED HERE, not in the template: setting min and max track x to the
	// same value leaves Windows nothing to drag horizontally, which is cleaner than
	// correcting the rect in WM_SIZING (that flickers as the user pulls against it).
	case WM_GETMINMAXINFO: {
		MINMAXINFO* mmi = (MINMAXINFO*)lParam;
		// Track sizes are WINDOW sizes, so run the client numbers through the frame.
		RECT fr = { 0, 0, DLG_W, DLG_H_MIN };
		AdjustWindowRectEx(&fr, GetWindowLongA(hDlg, GWL_STYLE), FALSE, GetWindowLongA(hDlg, GWL_EXSTYLE));
		mmi->ptMinTrackSize.x = mmi->ptMaxTrackSize.x = fr.right - fr.left;   // locked
		mmi->ptMinTrackSize.y = fr.bottom - fr.top;                           // floor only:
		return TRUE;                         // ptMaxTrackSize.y keeps the system default,
	}                                        // so a tall monitor is free to use its height

	case WM_SIZE: {
		// Growing the window can leave the scroll position past the new bottom (the
		// document did not change, the VIEWPORT did), so re-clamp before the next paint.
		RECT rc; GetClientRect(hDlg, &rc);
		ClampScroll(rc);
		ClampColourPicker(rc);               // its OK/Cancel row must stay reachable
		InvalidateRect(hDlg, NULL, FALSE);
		break;                               // Orbiter's dialog manager sees it too
	}

	// End of a drag - resize OR move, Windows sends it for both. Write only on a real
	// height change: a move must not touch the file, and neither must a resize that
	// ended where it started.
	case WM_EXITSIZEMOVE: {
		RECT rc; GetClientRect(hDlg, &rc);
		if (rc.bottom != g_lastSavedH && rc.bottom >= DLG_H_MIN) {
			g_lastSavedH = rc.bottom;
			OroSettings_SaveDlgHeight(rc.bottom);
		}
		break;                               // ... and Orbiter still kills its frame timer
	}

	case WM_COMMAND:
		if (LOWORD(wParam) == IDCANCEL) {  // Orbiter's caption close / Esc
			if (g_pickOpen) {              // Esc closes the picker (reverting), not the dialog
				CloseColourPicker(false);
				InvalidateRect(hDlg, NULL, FALSE);
				return TRUE;
			}
			oapiCloseDialog(hDlg);
			return TRUE;
		}
		return FALSE;

	case WM_DESTROY:
		KillTimer(hDlg, 1);
		g_hDlg = NULL;
		OroHelp_Close();                   // closing the panel from its own caption button
		                                   // must not leave the help window behind either
		ClearDrags();
		// The picker's gradient DIBs are window-lifetime resources; the open state
		// must not survive into the next dialog instance either.
		g_pickOpen = false; g_pickTarget = NULL; g_pickDrag = -1;
		if (g_pickSVDib)  { DeleteObject(g_pickSVDib);  g_pickSVDib  = NULL; }
		if (g_pickHueDib) { DeleteObject(g_pickHueDib); g_pickHueDib = NULL; }
		g_pickSVHue = -1.0f;
		return TRUE;
	}
	return oapiDefDialogProc(hDlg, uMsg, wParam, lParam);
}

// ----------------------------------------------------------------------------
// Public entry points
// ----------------------------------------------------------------------------
void OroDlg_Open(HINSTANCE hInst)
{
	g_hInst = hInst;                          // the HELP button needs it later
	if (g_hDlg) return;                       // already open
	LoadBannerOnce(hInst);
	// Orbiter-managed dialog (NOT CreateWindow - required for fullscreen).
	// DLG_CAPTIONCLOSE gives the Orbiter-skinned close button in the caption.
	oapiOpenDialogEx(hInst, IDD_ORO_CONTROL, OroDlgProc, DLG_CAPTIONCLOSE, NULL);
}

void OroDlg_Close()
{
	// The help window goes with the panel it describes. It is a second top-level window,
	// so without this it would outlive the panel and sit there orphaned - and at session
	// end it would outlive the module, which is worse than untidy.
	OroHelp_Close();
	if (g_hDlg) oapiCloseDialog(g_hDlg);      // WM_DESTROY clears g_hDlg
}
