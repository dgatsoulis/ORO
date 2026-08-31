// ==============================================================
// OroDialog.h
// Part of ORO - Orbiter Realism Overhaul
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2026 Dimitris "dgatsoulis" Gatsoulis
// ==============================================================

#pragma once

// ============================================================================
// ORO - the control dialog (owner-drawn, dark themed)
// ----------------------------------------------------------------------------
// Opened from Orbiter's Custom Functions list (Ctrl+F4). The dialog template
// is EMPTY; everything - banner, section headers, enable pills, sliders,
// status line - is drawn in WM_PAINT and driven by direct mouse handling.
// This is what lets the panel look like ORO rather than like Win95.
// ============================================================================

#include <windows.h>

// Open the dialog (no-op if already open). hInst = the ORO DLL instance.
void OroDlg_Open(HINSTANCE hInst);

// Close it if open (module unload / simulation end).
void OroDlg_Close();

// Phase B: is the panel open on a THRUSTERS page right now? SenseMarker publishes
// this into g_fx.thrPageLive each frame (single writer), which is what gates the
// in-world nozzle marker - a finder with the panel closed would be scaffolding
// nobody asked for. Same thread as everything else; safe from the module.
bool OroDlg_ThrPageLive();
