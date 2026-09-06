================================================================================
  O R O   -   Orbiter Realism Overhaul                      PUBLIC BETA
  Atmospheric, Physiological and Visual Immersion Suite
================================================================================

  >>> IF YOU HAVE AN EARLIER ORO BETA INSTALLED, JUST RUN THE INSTALLER. <<<
  >>> IT UPGRADES IN PLACE - your saved settings and your original-files <<<
  >>> backup are kept, and anything the addon has since moved or renamed <<<
  >>> is tidied up for you.                                              <<<

  >>> IF YOU HAVE THE OLDEST BETA (PULSE) INSTALLED, SAME ANSWER: RUN    <<<
  >>> THE ORO INSTALLER. IT WILL OFFER TO REMOVE PULSE FOR YOU.          <<<

  This addon used to be called PULSE. The ORO installer now detects PULSE -
  including a half-removed one - lists exactly what it would delete, and asks
  permission. Say yes and it removes PULSE, puts your original graphics client
  back, and then installs ORO. Say no and nothing is touched; it prints the
  manual steps instead.

  You do NOT need to find your old PULSE_beta folder, and you do not need to
  run PULSE_Uninstall.bat. We renamed the addon, so cleaning up after that
  rename is our job, not yours.

  If you would rather do it by hand: close Orbiter AND the Launchpad
  completely first, then run PULSE_Uninstall.bat, do not start Orbiter in
  between, then run ORO_Install.bat. Closing the Launchpad matters - it holds
  the graphics client file open, and PULSE's uninstaller does not check that
  its restore succeeded, so with that file locked it fails and still reports
  success. That is a bug in the version you have, and it is why the ORO
  installer now offers to do the whole thing itself.

  Why the name changed: it started as G-force effects, which is what PULSE
  meant, and it long ago grew past that. ORO is "Orbiter Realism Overhaul".
  (Also Greek: "oro" / opw = to see, to look at, to perceive - which is the
  whole subject of the addon.)

--------------------------------------------------------------------------------
  WHAT CHANGED SINCE THE BUILD YOU HAVE
--------------------------------------------------------------------------------

  If you tested the 260810 beta, thank you - both of your reports drove almost
  everything below. This build is a long way from that one.

  FIXED, AND YOU HIT THESE:

  - The crash when loading a second scenario. That was ORO handing Orbiter an
    object before the new scene existed. Gone.
  - Effects froze when you paused. Seven of them - plasma, aurora, lightning,
    plume, vapour cone, shimmer, god rays - were built once per simulation step,
    and pausing stops those steps while the screen keeps drawing. So the effect
    stayed where it was while you moved the camera. One of you worked out the
    cause unaided, from the outside, by noticing that VC shadows behaved
    correctly. You were exactly right.
  - The DeltaGlider's nose moving when you pressed Ctrl+G, and the pitch
    oscillation on autopilot. That was the FLIGHT AID, a test rig that shifts a
    vessel's centre of pressure so stock ships can hold a high angle of attack
    during reentry. It was shipped switched on, which was our mistake - it is
    now inert until you are actually in a reentry regime.
  - The engine bell was fake. Pushing its glow from 1.0 to 2.0 really did do
    nothing: the graphics client was clamping the value, so you were looking at
    the raw texture. Fixed in the client. It glows properly now, and it has a
    colour picker.
  - The lightning readout that clipped to "cells)".
  - Scenario sound not resuming after you un-muted it.
  - Dragging the panel froze the simulation.
  - Every character keypress made Windows ding.

  ALSO NEW:

  - An in-panel HELP window, per page - menus included - which follows you as
    you navigate. Several things you asked for already existed and there was
    no way to discover them - that is what this answers.
  - The panel resizes vertically, and there is a REVERT button.
  - Two effects you have never seen: crepuscular GOD RAYS, and the transonic
    VAPOUR CONE.
  - Every thruster setting is now PER ENGINE GROUP, so a vacuum-rated main and
    sea-level hovers can be described separately. Your existing tuning is
    carried into every group untouched.

  AND ONE WE OWED YOU:

  - The ORO installer now removes PULSE itself, with your permission. Sending
    you back to PULSE's uninstaller to clean up after OUR rename - using a
    script we know can fail silently and which already cost one of you a
    working Orbiter - was not a fair thing to ask.

  ONE REQUEST, AND IT MATTERS MORE THAN IT SOUNDS:

  - PLEASE SEND UNPAUSED SCREENSHOTS when commenting on how something looks.
    Most of the shots we got last time were of a paused simulation, which was
    precisely the state where our drawing was known to be wrong - so we could
    not tell a genuine complaint about the look from that bug. It is fixed now,
    but the habit is worth keeping: a paused frame is the one we trust least.

Thanks for testing. ORO is a global module that adds two families of effects:

  PHYSIOLOGY - what G-force does to the pilot: blackout, red-out, tunnel vision,
               grey-out, blur, camera shake, a heartbeat you can feel.
  ENVIRONMENT - what the world does: reentry plasma, exhaust plumes and shock
               diamonds, glowing engine bells, auroras, lightning seen from
               orbit, eclipses, transonic vapour cones, crepuscular god rays,
               and sunlight falling through the cockpit windows.

Everything is adjustable live, from one panel, while you fly.

NEW IN THIS BUILD (260906) - SHADOWS TO THE HORIZON, THE AIR, THE NIGHT:
  * CASCADED SHADOWS. A new terrain-shadow mode in the Launchpad's D3D9
    Advanced setup, "Cascaded (ORO)": ONE sun-shadow atlas for the whole
    scene. Buildings shadow the ground, hills shadow buildings, vessels shadow
    each other and their own pads, out to tens of kilometres, with crisp
    shadows on every vessel near you. Nothing blinks, nothing shimmers as the
    world turns, no tile-shaped bites - it replaces the stencil sheets and the
    per-tile maps in that mode, and the two stock modes are untouched. Section
    3 has the new Shadows box row by row.
  * BASE SPOTLIGHTS ARE HONEST IN DAYLIGHT TOO. In Cascaded mode the
    spotlight's shadow map rides in the same atlas, so a lamp no longer lights
    the ground through a hangar wall at noon.
  * FOG. A FOG page under WORLD / WEATHER: two real fog layers rendered inside
    every surface - terrain, bases, hulls, particles, runway lights, the sky -
    coloured by the sun, so it is warm at dawn, grey under a storm and dark at
    night. Shadows soften and vanish through it. The rain's gloom is fog now.
  * THE CABIN GOES DARK AT NIGHT. A stock cockpit is as bright at midnight as
    at noon. Now its fill light follows the sun - and the weather - so at
    night, or in orbit's shadow, you reach for the cockpit light the way a
    pilot would. PILOT / VIRTUAL COCKPIT: a night floor and a weather dim.
  * BASE LIGHTS on demand, with a HALO in fog: runway and taxiway lights become
    aureoles that grow with the air between you and the lamp. One pill, on
    both the RAIN and FOG pages.
  * SHADOWS HIDE BEHIND HILLS in the stock shadow modes too: stencil ground
    shadows are cut where a hill stands between them and you. The
    frame-to-frame blink this first showed at KSC is fixed at the root.
  * CABIN SOUNDS. The rain heard from the seat has its own volume, and the HULL
    DRUM was rebuilt: rain on a metal roof - dull thumps over a low panel
    rumble, no hiss.
  * TERRAIN FLATTENING NOW WORKS WITH CUBIC INTERPOLATION - the elevation
    setting most people have. Flattening (.flt) files used to flatten the
    ground you stood on but not the ground you saw; both agree now.
  * FIXED: the windscreen runners ran BACKWARD when the engine was cut on the
    runway; the client refused its vessel shader with Vessel self-shadows set
    to None (an error box at every scenario start, since 260809); a spotlight
    lit a vessel behind a building in daylight.

ALSO IN THIS BUILD - LIGHTS, SHADOWS AND THE SCULPTED PLUME:
  * SPOTLIGHT SHADOWS. Base spotlights now cast real shadows: a hangar carves
    its beam, a vessel standing in it throws its shadow on the ground, and a
    terrain ridge ends the beam. Vessels receive them day and night; the pool
    on the ground is a dusk-and-night feature in the stock shadow modes and
    works in daylight in Cascaded mode. (Off switch: the Shadows box, or
    LocalLightShadows=0 in D3D9Client.cfg. ShadowDebug=1 there is a diagnostic
    - only if I ask you for it.)
  * NOTHING DRAWS THROUGH TERRAIN ANY MORE. Runway lights and base surfaces
    hide behind hills instead of bleeding through them; the sun's glare now
    sets behind a mountain ridge instead of shining through it; and glare
    sprites respect buildings.
  * NIGHT TEXTURES FOR BASE BUILDINGS. The classic mytex.dds / mytex_n.dds
    convention now works for MESH base objects - the most common kind - so
    base authors' windows light up at dusk. Runway markings ride along free.
  * SCULPTABLE SHOCK DIAMONDS. The train's count now goes to 0 (a clean jet),
    and each cell has SHAPE (the RS-25's half-diamond at the lip, the classic
    symmetric cell, or a downstream fan), LENGTH and WIDTH (short length =
    pure Mach discs), a train OFFSET in metres, and its own COLOUR swatch that
    renders exactly what you pick. Under throttle the train grows and shrinks
    by WHOLE diamonds in sequence - never a sliced fraction of a cell.
  * FULL SHIMMER CONTROL. The heat haze has amplitude, WAVELENGTH and
    FREQUENCY, a wider per-jet offset (-2..+10 m along each jet's own flow),
    per-group strength (a weak hover haze beside a strong main haze), and it
    answers to air density on its own - thinning as you climb, gone in vacuum,
    nearly doubled on a world as dense as Venus.
  * THE STOCK DG'S WINDSCREEN IS PRE-DECLARED for the windscreen raindrops
    (Config\ORO\VesselsRainSurfaces.cfg ships with it), so the drops work on a
    plain DeltaGlider out of the box.
  * A SMALL TOOL: Script\focusall.lua. Some vessels (the Atlantis boosters and
    tank, many addon stack parts) are created unselectable, which also makes
    them untunable - per-class settings load for the FOCUSED vessel. Enable
    the LuaConsole module, open the console in the sim and type
    run('focusall') - every vessel in the scenario becomes selectable with F3
    for that session.

NEW IN 260831 - THE THRUSTER MILESTONE:
  * THE PANEL IS A MENU TREE. Three doors - WORLD / VESSEL / PILOT - then
    short pages, one subject each, with BACK and a breadcrumb. No more giant
    scrolling tabs. HELP is per page and an open help window follows you as
    you navigate; SAVE buttons go AMBER when a page holds unsaved edits.
  * PER-THRUSTER TUNING. Beside the engine-group button, a thruster selector:
    any single thruster can carry its own exhaust and particle look on top of
    its group's (see section 5). A MARK button rings every nozzle of the
    selection in-world, and CANCEL THRUST now holds exactly what you selected
    - one RCS jet can be test-fired with the ship pinned in place.
  * GIMBAL, END TO END. Plume, particles, shimmer and throat fire follow a
    gimballing engine live - and for vessels that animate their real engine
    BELLS, the bell glow can now ride the moving bell (a one-line mesh token
    for authors; see the bell glow section).
  * BOOSTERS KEEP THEIR LOOK. Vessels in a stack (SRBs, tanks) follow their
    OWN class's saved tuning even while you fly the orbiter.
  * RAINDROPS ON THE WINDSCREEN - drops bead, run and streak on the VC glass
    with real refraction, driven by the storm outside. Declare any window by
    CLICKING it (the RAINSURFACES button on the RAIN page - no mesh editing);
    vessel authors can also mark glass with a one-line RAIN 1 mesh token.
  * REFLECTIONS THAT INCLUDE YOU. A fourth Launchpad reflection mode, "Full
    Scene ORO (exp)": vessels reflect THEMSELVES and their payloads (stock
    Full Scene never could), with multi-probe environment maps and real
    planar mirrors - and planet glow now respects a closed payload bay
    (stock lights the inside of closed doors sky-blue). Experimental; the
    three stock modes are pixel-exact stock. Needs Shadows enabled.
  * SMOKE THAT KNOWS THE SUN. Diffuse particle streams darken at night, shade
    directionally (lit side bright, far side smoky), and through dawn take
    the same colours as your hull - with a Launchpad group to tune it (see
    section 5's PARTICLE LIGHTING). Stock behaviour is one dropdown away.
  * COPY STOCK and a particle TEXTURE PICKER - start your particle tuning
    from the vessel author's own stream definitions, and cycle real contrail
    textures or drop your own atlas in Textures\ORO\Particles.
  * TWIN VAPOUR CONES with full placement - position on all three axes plus
    pitch and yaw per cone.

NEW IN 260823 - THE STORM:
  * RAIN. Summon a storm at the surface: the light collapses to overcast under
    a real two-layer cloud ceiling, streaks fall, drops splash, the ground
    soaks dark, standing water pools and mirrors the ships. The RAIN page
    (WORLD / WEATHER), Earth for now.
  * RAIN LIGHTNING - flashes inside the deck and real textured BOLTS to the
    ground, each one blinking a real light over the scene and the wet ground.
  * THUNDER. Every flash sends its thunder delayed by its own distance at the
    speed of sound - up to half a minute after the light, exactly like the
    real thing. Nine real storm recordings; a STRIKE test button plants a bolt
    on your ship so you can hear the whole bolt-then-crack beat.
  * RAIN SOUND - generated rain loops that build with the storm.
  * THE STORM FROM THE COCKPIT. In a virtual cockpit the rain is outside the
    glass, cut per pixel at the window frame; the cabin stays dry, the sound
    comes through muffled, and drops drum on the hull. Vessel authors can seal
    large interiors with a tiny "rain shield" mesh - see the RAIN section.
  * The cockpit REENTRY got its own technique - a luminous sheath through the
    windows with a cabin light wash, replacing the starved geometry the VC
    used to show.

NEW SINCE THE FIRST PUBLIC FILES (if you tested the PULSE beta, 260810):
  * A CRASH FIX, and this is the one that matters. The build you are running
    can crash when you exit to the Launchpad and start a scenario again. It is
    fixed here - three separate causes, all of them the addon handing Orbiter
    something before the new scene existed. If you have been hitting that, it
    was not your machine.
  * VAPOUR CONES - the transonic shock collar, TWO of them now, independently
    tunable (VESSEL / REENTRY). Try TEST on a runway.
  * GOD RAYS - crepuscular shafts from a low sun (WORLD).
  * The patched client no longer floods your Orbiter.log with errors on every
    scenario reload. That one was a stock D3D9Client bug, not an addon bug.
  * The rename, which touches everything you can see: the Launchpad module is
    "ORO control", the panel says ORO, and the folders are Modules\ORO,
    Config\ORO, Meshes\ORO, Textures\ORO. Your tuned settings are NOT carried
    over from PULSE automatically - this package ships mine.

WHAT I AM LOOKING FOR: does it look right, and does it run at a sensible frame
rate on hardware that is not mine. Please note your GPU and your frame rate with
ORO armed and disarmed (Ctrl+G toggles everything at once).


--------------------------------------------------------------------------------
1. REQUIREMENTS
--------------------------------------------------------------------------------

* A CLEAN Orbiter 2024 install, no other addons. This matters - the beta is
  meant to isolate ORO, and some settings below are checked against stock.
* The D3D9 graphics client, which ships with Orbiter 2024.
* XRSound, which also ships with Orbiter 2024. Optional: without it ORO runs
  silently and nothing else changes.

⚠️ ORO INCLUDES A PATCHED D3D9CLIENT AND WILL REPLACE YOURS.
Stock Orbiter 2024's D3D9Client crashes the instant ANY addon registers a HUD
render callback - the exact mechanism ORO draws through - so the first patch is
a crash fix, not a feature. The rest expose things the client already renders
internally (the frame buffer, the depth buffer, its shadow map) or let an addon
switch off stock visuals it is replacing.

The installer BACKS UP your own D3D9 client and shaders before replacing them,
and ORO_Uninstall.bat puts those exact files back. You can return to a stock
install at any time, in one click, and it will not touch anything you tuned.


--------------------------------------------------------------------------------
2. INSTALL
--------------------------------------------------------------------------------

1. HAVE THE OLD PULSE BETA? Do nothing about it - the ORO installer handles it.
   It detects PULSE, including a half-removed one, lists exactly what it would
   delete, and asks. Say yes and it removes PULSE, restores your original
   graphics client - from PULSE's own backup if that still exists, otherwise
   from the pristine originals shipped in this archive - and then installs ORO.
   Say no and nothing is touched; it prints the manual steps instead.
   Your PULSE_beta folder is left alone, since it holds that backup.
   If you never installed the earlier beta, nothing here applies.
2. Close Orbiter completely (the Launchpad too).
3. Unzip this archive into your Orbiter root folder - the one that contains
   Orbiter.exe. It creates a single folder there called ORO_beta.
4. Open that folder and run ORO_Install.bat.
     - It checks you are on Orbiter 2024 - by the DATE on Orbiter.exe and
       Orbiter_ng.exe, which for 2024 is 2024-12-31 or later - and stops with
       an explanation if not. That is deliberately not a check for which files
       are present: an Orbiter 2016 with the D3D9 client added to it would
       pass that, and installing into it would break it.
     - It shows what it is about to do and waits for you to type Y.
     - It backs up your original files before replacing anything.
     - It is a plain text file. Open it in Notepad first if you want to see
       exactly what it does - you are about to let it replace your graphics
       client, so that is a reasonable thing to want.
5. Start the Orbiter Launchpad.
6. MODULES tab -> tick "ORO control". If "PULSE control" is still listed and
   ticked, UNTICK IT. Two copies of the addon will otherwise both load and
   fight over the same settings file. (If you ran step 1, it should already
   be gone.)
7. VIDEO tab -> make sure the graphics client is "D3D9Client".
8. Set the options in section 3. Several of them are NOT optional.
9. Start a scenario. In the sim press CTRL+F4 and choose "ORO control".

To confirm the install took, open Orbiter.log in the Orbiter root and look near
the top for:

    Module D3D9Client.dll ........ [Build 260906, ...]   <- patched, good
    Module D3D9Client.dll ........ [Build 241231, ...]   <- still stock

It is the BUILD number that tells them apart. Ignore the "API" number printed
beside it - that reports which SDK the DLL was compiled against, and the patched
client is built from source so it does not match the stock one. On this build it
reads API 260725, and that is correct, not a failed install.

A few lines further down ORO lists what it found in the client - patches (d),
(f), (g), (i), (k), (l), (n), (o), the fog layers (patch aa), base lights (ac)
and the VC night light (ad) should all read "available".


--------------------------------------------------------------------------------
3. REQUIRED SETTINGS  -  PLEASE DO NOT SKIP THIS
--------------------------------------------------------------------------------

These fail SILENTLY. Nothing errors; effects just quietly look wrong or do
nothing, and you would have no way to tell that a setting was the reason.

In the Launchpad, VIDEO tab -> "Advanced" / D3D9 configuration:

  Sun glare .................. ON      REQUIRED
      The depth buffer ORO reads only exists when glares are enabled. Without
      it, plasma and auroras paint straight THROUGH the hull and the cockpit
      instead of being hidden behind them.

  Post-processing ............ "Light glow"   REQUIRED
      The reentry plasma composites into the client's high-dynamic-range buffer
      BEFORE the glow pass. That is where its white-hot core comes from. Without
      it the plasma looks flat and orange, and the engine bells stay amber
      however hot they get - both delegate "white" to this pass rather than
      painting it.
      ORO CHECKS THIS ONE FOR YOU. It reads the setting at startup, writes it
      into Orbiter.log ("ORO: client post-processing (Light glow) ..."), and the
      REENTRY and BELL GLOW sections replace their captions with a warning while
      it is off. So if plasma looks hard-edged or a bell looks yellow, the panel
      will tell you whether it is a setting or a tuning question.

  Local shadows / ShadowMapMode .... 1 or higher   REQUIRED for VC shadows
      Shadow map size 2048 is a good default; 4096 if you have headroom.

  Terrain and world shadows .. "Cascaded (ORO)"   RECOMMENDED (new in 260906)
      The SHADOWS box, bottom right of the D3D9 Advanced setup dialog:
        Vessel self-shadows      The per-vessel map and its filter (stock).
                                 NONE is legal with Cascaded on: you keep
                                 every ground and building shadow and lose
                                 only cockpit shadows and the finest hull
                                 detail. Map size 1024 / 2048 / 4096.
        Terrain and world        Off / Stencil / Projected (stock) / Cascaded
        shadows                  (ORO). Cascaded replaces the stencil sheets
                                 and the per-tile maps with one atlas: nothing
                                 blinks, nothing bites, shadows reach the
                                 horizon.
        Cascade detail           1024 (48 MB) / 2048 (192 MB) / 4096 (768 MB
                                 of video memory). 4096 needs a card that
                                 takes 16384-wide textures (DX11 class) and
                                 halves itself on one that does not. 2048 is
                                 the default.
        Cascade reach            How far the far cascades go, 5 to 60 km.
        Soft far shadows         A wider filter on the distant cascades, which
                                 otherwise read pixelated up close.
        Local light shadows      Spotlights cast shadows (buildings, vessels,
                                 ridges); in Cascaded mode in daylight too.
      Greyed rows are the ones that do nothing in the selected mode.
      Keys in D3D9Client.cfg: TerrainShadowing 3, ShadowCascadeSize,
      ShadowCascadeFar, ShadowCascadeSoft, LocalLightShadows.

In the Launchpad, VISUAL EFFECTS tab:

  Particle streams ........... ON (it is on by default)   REQUIRED for the
      VESSEL > THRUSTERS > PARTICLES page. With it off, Orbiter refuses to
      create any particle stream and that whole page silently does nothing.

  Ambient light level ........ leave at the stock 20
      Raising it washes out every shadow in the sim, including ORO's.

Reference: the settings this was developed and tuned on are
ShadowMapMode 3, ShadowMapFilter 2, ShadowMapSize 4096, TerrainShadowing 3
(Cascaded), ShadowCascadeSize 2048 (4096 on a card that takes it),
ShadowCascadeFar 30000, ShadowCascadeSoft 1, LocalLightShadows 1,
PostProcess 1, SunGlare 1, AmbientLevel 20.


--------------------------------------------------------------------------------
4. FIRST FLIGHT  -  the quick tour
--------------------------------------------------------------------------------

FOUR SCENARIOS ARE PROVIDED, under Scenarios\ORO_beta in the Launchpad. They
are the quickest way to see each part of the addon:

  Habana Spaceport   A DG-S on the pad at dusk. Start here - it is the easiest
                     place to look at the engines, the bell glow and the VC
                     shadows without having to fly anything first.
  Thruster effects   The exhaust system, set up ready to look at.
  DG reentry         A DeltaGlider set up for reentry - the plasma, which is
                     the biggest effect in the addon.
  Atlantis reentry   The same, on a very different hull. Worth comparing.

Then, from any of them, press CTRL+F4 -> ORO control.

  * The green ENABLED button at the top is the master arm. CTRL+G toggles it
    from the keyboard at any time, panel open or not. If anything ever looks
    wrong, hit Ctrl+G - it hands everything back to stock instantly.
  * Take off and fly. Watch the exhaust from an external view (F1) - the plume,
    the shock diamonds at sea level, the soot at the nozzle lip.
  * VESSEL > THRUSTERS > EXHAUST, the BELL GLOW section: the engine bells heat
    up and cool down on their own as you throttle.
  * Go to orbit, then come back in steep. The reentry plasma is the PLASMA
    page (VESSEL > REENTRY) and the biggest thing in the addon.
  * Sit in the virtual cockpit (F8) at a low sun angle and roll - sunlight
    sweeps across the cabin through the canopy (PILOT > VIRTUAL COCKPIT).

  Nothing you change is permanent until you press SAVE - see section 6.


--------------------------------------------------------------------------------
5. THE PANEL, PAGE BY PAGE
--------------------------------------------------------------------------------

THE PANEL IS A MENU TREE. The main menu has three doors - WORLD (the
environment), VESSEL (the hull) and PILOT (the human) - with submenus beneath
and, at the bottom, PAGES that hold exactly one subject's sliders. The line
under the master strip is your breadcrumb (where you are), with BACK and
BACK TO MAIN beside it; both sit dim at the main menu because there is nowhere
back to go. Nothing is ever more than two clicks from anywhere, and every page
only shows the controls that belong to its subject.

Every effect has a PILL (the round toggle on the left - green is on) and a
SLIDER. Sliders show their value on the right. Each page scrolls its own
content.

=== ALWAYS VISIBLE (top of the panel) ===

  ENABLED / DISABLED   Master arm. Same as Ctrl+G. Kills every effect at once
                       and gives the sim back its stock behaviour.
  HELP                 Opens the help window for WHICHEVER PAGE YOU ARE ON -
                       menus included, so pressing it on a menu describes what
                       is behind each button, and pressing it on a page of
                       sliders explains every one of them. It never opens
                       twice: press it again elsewhere and the window you
                       already have switches to that page's text - and while
                       it is open it FOLLOWS you as you navigate, so the text
                       beside you is always about the screen in front of you.
                       Closeable and resizable, and it remembers its size -
                       but not that it was open, so it never reappears by
                       itself on a new session. The button lights green while
                       it is up, which is also how you tell it has opened
                       behind the sim window rather than not opened at all.
  SAVE                 Writes ALL settings, in all three scopes (section 6).

Every PAGE of sliders opens with its own SAVE and a REVERT beside it, in a
row that stays put however far you scroll - the way to save is never off
screen. REVERT re-reads that page's files from disk and throws away everything
you have moved since the last save - the way back from a tuning session that
went wrong.

THE SAVE BUTTONS TURN AMBER while there are unsaved edits in the files they
would write - the global one for any unsaved edit anywhere, a page's own for
its files. Green flash = written. Amber gone = safe to quit. Test buttons and
the other session-only rigs never light it, because they are not saved things.

THE PANEL RESIZES VERTICALLY. Drag its bottom edge or a corner: the width is
fixed (the sliders and the banner are built around it) but the height is yours,
down to a minimum of 500 px and up to whatever your monitor allows. Taller means
less scrolling, and REENTRY and ATMOS both run to about a thousand pixels of
content, so there is real benefit in it.
The height is remembered. It is written the moment you finish dragging, into
Config\ORO\window.cfg, and the panel reopens at that height next session - you
do not have to press SAVE and it deliberately does NOT go in with your settings,
so resizing the window can never commit tuning you had not saved. Delete that
file to get the default 800 back.
(If you had an earlier build that froze the sim while you dragged the panel: that
was ours, and it is fixed. Orbiter keeps rendering during a window drag.)


=== PILOT / G-FORCES ===  what high G does to you
    (the scripted G events live on their own page, PILOT / SCENARIOS)

   SAVE TARGET     Where this page's settings live. ALL VESSELS keeps them in the
                   global file - one pilot flies every ship, which is how ORO has
                   always worked. THIS VESSEL CLASS moves them into the hull's own
                   file, because where the crew SITS is a fact about an airframe.
                   A hull with no settings of its own falls back to your global
                   ones, so visiting an untuned ship never loses your pilot.
                   The master arm and the scenario sound always stay global.
                   The line above the SAVE button always names the file it writes.

VISION - each is a separate symptom; they layer.
  Blackout        Vision fades to black under sustained positive G.
  Red-out         The red veil of NEGATIVE G (blood forced toward the head).
  Tunnel vision   Peripheral vision closes in to a narrowing circle.
  Dark spots      Shimmering blind patches (scotomas) drifting in the field.
  Grey-out        Colour vision fades before brightness does. Full-frame.
  Blur            Vision softens and smears.
  Heartbeat       The field pulses darker with each beat, and the beat drives
                  a heartbeat sound. Also deepens the tunnel as it throbs.
  Aberration      Colour channels split apart toward the edges.
  Sparkles        "Seeing stars" - bright scintillations.
  Swim            A slow woozy warp of the periphery. Disorientation.
  BLINK           A one-shot blink, for testing.

MOTION
  Tilt / sway     The whole view rolls slowly, as your inner ear gives up.

PILOT - this is where the effects stop being a lab and start being physics.
  Effect source   LAB     - the sliders drive the effects directly. Good for
                            seeing what each one looks like.
                  PHYSICS - the felt-G model drives them, and the sliders
                            become per-effect GAINS. This is the real thing:
                            pull G and the symptoms arrive on their own.
                  IN PHYSICS A SLIDER IS A MULTIPLIER, so a row whose axis is
                  not firing reads zero however far you push it - that is not a
                  broken control. While the model is producing nothing, the
                  readout column names the axis the row is WAITING FOR instead
                  of showing a number: "+Gz" for the whole vision suite (it
                  tracks your oxygen reserve), "-Gz" for red-out, "Gx" for
                  aberration. Aberration in particular will never move during a
                  positive-G pull - it is eyeball deformation, a different axis.
  G tolerance     How much G you take before symptoms start. The readout shows
                  the threshold in G.
  Anti-G suit     Adds about 1.5 G of tolerance, positive G only.
  Position        Seated / reclined / prone / standing / couch. Decides which
                  vessel axis is your spine, so the same manoeuvre affects you
                  differently. A reclined pilot takes more G.
  G reference     Camera or vessel centre of mass. In orbit this is the whole
                  effect - your head is metres from the CoM, so rotation alone
                  produces real G at your eyes.
  Effects view    PANEL + VC - the physiology draws in any internal cockpit
                               view (the default, and what it has always done).
                  VC ONLY    - only in the virtual cockpit. For pilots who fly
                               the VC and drop to the 2D panel to work systems:
                               a panel is a flat overlay with no world behind
                               it, and some people want a clean one. The
                               heartbeat sound follows the same rule.
  FELT G readout  Live signed Gz/Gx/Gy plus your oxygen reserve. The reserve
                  goes red below 50%. At zero you black out and stay out.

SCENARIOS - one-click scripted G events (LAB mode only; they and the physics
  model would otherwise fight over the same values).
  INDUCE G-LOC / Grey-out / Red-out    Ramp up and HOLD. You stay there.
  RECOVER FROM ...                     Ramp back down from that peak.
  SOUND                                Per-scenario audio on/off. In this beta
                                       only INDUCE G-LOC has a clip.


=== VESSEL / THRUSTERS ===  engines. Two pages: EXHAUST and PARTICLES.

⚠ FIRST, THE ENGINE GROUP BUTTON, in the fixed row at the top of both pages.
Everything on either page edits ONE engine group at a time, and that button
says which. Click it to cycle through the groups this vessel actually has:
MAIN, HOVER, RETRO, USER for engines the author put in no standard group, and
RCS - all attitude thrusters as one group. A ship with only main engines has
nothing to cycle to.
  Why: one set of numbers only suits a ship whose engines all burn the same
  thing. The expansion band's high handle is the pressure an engine is RATED
  for, the Jet/Bloom swatches are its exhaust colour, and soot is the difference
  between kerosene and hydrogen. Now a vacuum-rated main and sea-level hovers
  can each have their own.
  Tune a group, cycle, tune the next. SAVE writes them all, so working on the
  hovers cannot disturb what you did to the mains.
  If you already had a tuned class file, its single set of numbers loads into
  EVERY group - nothing is lost and nothing looks different until you cycle
  and change something.
  STOCK EXHAUST and STOCK PARTICLES are the exceptions: they apply to the
  whole vessel, because the client suppresses stock exhaust per SHIP.

⚠ AND BESIDE IT, THE THRUSTER SELECTOR (Thr: < ALL >) - one step finer. At ALL
you edit the whole group, as ever. Cycle it to a single thruster and the first
slider you move gives THAT thruster its own override: it keeps its own look
from then on, saved with the class, while the rest of the group stays on the
group's numbers. Exhaust and particles override independently. CLR hands the
thruster back to its group. The status line under the row always names what
you are editing and whether it owns an override.
  MARK toggles a pulsing in-world ring at every nozzle of the selection - with
  44 RCS jets, "thruster 17" means nothing without one. Rings draw through the
  hull (deliberately - you are locating, not admiring).
  Why: some addons group thrusters loosely, or mix propellants in one group.
  This lets a single odd engine be tuned without forking the whole group.
  Vessels in a launch stack (boosters, a tank) follow their OWN class's saved
  tuning even while you fly the orbiter - tune the booster class once, and a
  fresh launch shows it on every booster with your focus elsewhere.

--- Page EXHAUST - the parts ORO draws itself ---

  Shimmer amplitude  Heat haze bending the view behind the plume - the slider is
                     the wave's amplitude. Each engine group (or overridden
                     thruster) hazes at its OWN strength in the same frame.
  Offset (m)         Slides the haze along EACH JET'S OWN flow direction,
                     -2 to +10 m (positive = downstream, for a retro as much as
                     a main). The tick marks true zero and the knob snaps to it.
  Wavelength         Ripple size, as a multiplier. High = broad and lazy,
                     low = fine frying-pan heat. 1.00 = the original look.
  Frequency          How fast the waver churns. 1.00 = original, 0 = frozen.
  The haze answers to AIR DENSITY on its own: full low down, visibly thinning
  through a climb, gone in vacuum - and up to about twice Earth strength on a
  world as dense as Venus. Nothing to set.

  PLUME EXPANSION - a rocket nozzle is built for ONE ambient pressure; the
  atmosphere decides what the jet does everywhere else. Overexpanded at sea
  level gives the narrow pinched jet with the shock-diamond train;
  underexpanded in vacuum gives the wide faint bloom.
    LAB | PHYSICS   PHYSICS lets pressure and throttle drive the shape.
                    LAB pins them so the sliders rule alone. Both are anchored
                    identical at sea level and full throttle.
    Stock preset    COPY STOCK resets this group's jet to the stock flame:
                    width/length 1.0 (the plume's base size is already the
                    vessel's own exhaust definition) with diamonds, bloom,
                    throat fire and soot off. A clean start before shaping.
    Expansion band  TWO handles on one track: the pressure range this engine is
                    built for. Drag the high handle down and you have a vacuum
                    engine that shudders and pinches at the pad.
    Width / Length      Overall jet size.
    Diamonds            How many shock cells in the train (0-12; 0 = a clean
                        jet, no cells). Successive cells are always fainter
                        than the one before, and under throttle the train grows
                        and shrinks by WHOLE diamonds in sequence - never a
                        fraction of a cell.
    Diamond bright      Their contrast.
    Diamond spacing     How far apart they sit.
    Diamond shape       Where each cell bulges (-1..+1). 0 = the classic
                        symmetric diamond; -1 = a half diamond, base toward the
                        bell (the bright converging cone an RS-25 shows at the
                        lip); +1 = the mirror.
    Diamond length      Each cell's axial size. 1 puts the tips exactly at the
                        discs; short leaves only thin bright discs (a pure
                        Mach-disc train).
    Diamond width       Each cell's radial size.
    Diamond offset      Slides the whole train along the jet, -2..+5 m
                        (negative = toward the bell).
    Bloom width/bright  The wide vacuum plume.
    Throat glow         The fire seen down inside the nozzle.
    Throat offset       Nudges it, because the visual nozzle and the engine's
                        defined exhaust point disagree on some hulls.
    Soot streaks        Dark soot shedding off the nozzle lip. 0 = off.
    Soot churn          How fast it moves. 0 freezes it.
    Jet / Bloom / Dia   Three colour swatches: the jet core, the vacuum halo,
                        and the diamonds. The diamond swatch renders EXACTLY
                        the colour you pick (white = the classic look).

  BELL GLOW - the engine bells heat and cool as real metal does, on sim time,
  whether or not you are watching.
    Bell glow      Brightness trim.
    Heat time (s)  How fast they come up to temperature.
    Cool time (s)  How long until the glow is COMPLETELY gone.
    Bell colour    Rotates the whole heat ramp onto the colour you pick, keeping
                   its shape: still dull at the bottom, still whitening at peak.
                   White = the default blackbody ramp, unchanged. Note the bell
                   reaches WHITE through the client's bloom rather than through
                   its palette, so with post-processing OFF it can only ever get
                   brighter amber - the caption under this section will tell you
                   if that is what you are looking at.
    Needs a bell mesh for the vessel class; DeltaGlider and DG-S have one.
    FOR MESH AUTHORS - GIMBALLING BELLS: if a vessel animates its real engine
    bells with the gimbal, give each bell its own group in the bell mesh and
    add a line reading GIMBAL to that group's header. The glow then rotates to
    track the live thrust direction of its engine (matched by position) and
    rides the moving bell as one piece of metal. A label's first word is the
    engine family, the rest is yours - "MAIN 1", "MAIN 2" keep mesh editors
    happy with unique names. Without the token a group stays fixed, which is
    correct for the many vessels that gimbal the thruster but never move the
    bell mesh itself.

  STOCK EXHAUST   Off = hide Orbiter's own exhaust texture, so you judge ORO's
                  plume alone. This only affects the flame BILLBOARDS. Stock's
                  exhaust PARTICLES have their own pill, on the PARTICLES
                  page - they were split deliberately, so that an addon which
                  replaces only one of them can be handled.
  CANCEL THRUST   A test stand that FOLLOWS THE SELECTION: it nulls the
                  selected group - or the one selected thruster - each engine
                  cancelled at its own position, so its push AND its twist die
                  together. Fire one RCS jet under the hold and the ship does
                  not move at all, while every other control stays live. Never
                  saved. The same switch is mirrored on the PARTICLES page -
                  one rig, two doors.

--- Page PARTICLES - Orbiter's own particle system, under your control ---

ORO draws none of these. It hands Orbiter the same settings a vessel author
sets in code, and lets you move them live. Units are the API's own.

  Stock preset   COPY STOCK loads the vessel author's own stream definition
                 into the sliders as a starting point - only the streams
                 belonging to the SELECTED ENGINE GROUP (matched by thrust
                 direction, per-thruster duplicates folded), so a DeltaGlider's
                 MAIN offers exactly its contrail and its flame puffs. Press
                 again to cycle; the status line names which one you got.
                 Slider top ends stretch automatically when a stock value (a
                 long booster-smoke lifetime, say) is beyond the preset range.
  Offset (m)     Where the particles are born, along the exhaust. Negative
                 moves the source back toward the nozzle.
  Size (m)       Particle size at birth. Note there is no width or length - a
                 particle is a round sprite, so this is its radius.
  Lifetime (s)   How long each particle lives.
  Rate (Hz)      How many are created per second.
  Speed (m/s)    How fast they leave the nozzle.
  Spread         Random spread in that velocity. 0 = a tight column.
  Growth (m/s)   How fast each particle expands as it ages.
  Atm slowdown   How much the atmosphere brakes them.
  Lighting       EMISSIVE - they glow by themselves (flame).
                 DIFFUSE  - the sun lights them (smoke, vapour). Since this
                 build DIFFUSE really means it - see PARTICLE LIGHTING below.
  Air fade       FADES IN VACUUM - Orbiter's stock behaviour, where a stream
                 fades out as the air thins. THIS IS THE DEFAULT: exhaust clouds
                 hanging in orbit look wrong, and they are wrong.
                 ALWAYS ON       - emit everywhere, including vacuum.
                 If you enable this page in orbit on the default and see nothing,
                 that is the fade doing its job - the row's own label says
                 "Air fade - in vacuum" while it is holding emission off, and
                 the caption below says so too.
  Colour A / B   TWO tints: each particle is randomly born with one or the
                 other, so white + dark grey gives a mixed smoke no single
                 colour can. On a file texture a tint REPLACES the file's
                 colour using its brightness as shading - so white really is
                 white. Set both the same for a single-colour look.
  STOCK          Use the texture's own authored colours and ignore both tints.
                 Stays green while active; the swatches grey out.
  Texture        The particle's SHAPE. Cycles ORO's synthesized atlas,
                 Orbiter's own Contrail1 / Contrail1a (the DeltaGlider's wispy
                 smoke), then any .dds you drop into Textures\ORO\Particles -
                 the folder is rescanned on every press, no restart needed.
                 Files must be 2x2 atlases of four puff variants, like
                 Contrail1.dds (a single centred image renders as corner
                 wedges); the folder's README explains. A missing file falls
                 back to the synthesized atlas.
  STOCK PARTICLES  The vessel author's own exhaust particles. Independent of
                 ORO's pill at the top of the page: run stock's, ORO's, both
                 together, or neither - every combination is legal, and SAVE
                 keeps whichever you set.
  CANCEL THRUST  The same test-stand switch as the EXHAUST page's - one rig,
                 two doors, so tuning particles does not mean a page hop.

  PARTICLE LIGHTING  (Launchpad > Video > Advanced > "Particle lighting (ORO)")
  Stock Orbiter renders DIFFUSE particle streams fully lit at any hour - a
  smoke trail at midnight glows as if it were noon. The ORO client fixes it:
  DIFFUSE particles darken on the night side (down to your Launchpad ambient
  level), cross the terminator per particle so a long trail can be lit at one
  end and dark at the other, stay lit near the engine flame, and are shaded
  DIRECTIONALLY - the sun-facing side of a smoke cloud is bright, the far
  side smoky. Through dawn and dusk the sunlit smoke follows the SAME colour
  your hull takes (it reads the client's own atmospheric sunlight at each
  particle's altitude), while the steam by the nozzles stays engine-lit white.
  EMISSIVE streams are untouched - flame is supposed to glow. The Launchpad
  dropdown picks the mode:
    Off (stock, always lit)  bit-exact stock behaviour, everything as before
    Brightness only          night/terminator darkening + directionality
    Brightness + colour      the full look (the default)
  Under the dropdown, four sliders:
    Shadow strength (diffuse)  how dark the smoke's ground shadow is. 0 = no
                               shadow, 1 = stock; a low sun can stretch a
                               launch column's shadow into a black band, and
                               this is the dial for it.
    Dawn tint lead / depth / bloom  how far ahead of the hull the smoke's
                               dawn colour runs, how deep it is, and how hard
                               the tinted sunlit side blooms. Defaults are the
                               shipped tuning; they only act near dawn/dusk in
                               colour mode - daylight is untouched at any
                               setting.

  A NOTE FOR ADDON AUTHORS: in stock D3D9 the EMISSIVE/DIFFUSE declaration was
  nearly a no-op - diffuse streams only ever differed by casting ground
  shadows - so existing addons may carry either declaration without meaning
  it. Under the ORO client, DIFFUSE now means "sun-lit": declare your streams
  honestly - EMISSIVE for anything self-luminous (flame, plasma, glowing gas),
  DIFFUSE for anything that merely reflects light (smoke, vapour, dust,
  clouds). If your addon assumed the old always-lit behaviour, your users can
  select Off in the Launchpad to get exactly the stock rendering back.


=== VESSEL / REENTRY ===  the biggest effect in the addon
    (three pages: PLASMA, VAPOUR CONES, and FLIGHT AID under VESSEL itself)

  Reentry plasma  Master pill and overall strength.
  Plasma heat     A live readout. No vessel publishes a nose radius, so the
                  heat numbers cannot be guessed - this shows what ORO
                  computed, so you can tell "too cold" from "not working".
  VC ON/OFF       Whether plasma is drawn looking out of the virtual cockpit.

  PLASMA TUNING - the look, per vessel class.
    Saturation      The whole palette. 1 = the reference look.
    Hull light      A real light source at the stagnation point, lighting the
                    vessel's own mesh. 0 removes the light entirely.
    Streak length / width / wander   The flame streaks trailing back.
    Wake churn      How FAST the wake lives - fin shimmer, spark march, the
                    drift of the striations. 1 = the standard rate, 0 freezes
                    it. Turn it up if the plasma looks like an aurora rather
                    than something being torn off a hypersonic vehicle. One
                    clock for all of it, so the wake stays coherent; note that
                    means "Spark life (s)" is seconds at churn 1.
    Fin rake (deg)  How far the streamers splay OUT from the flow direction.
                    0 lays them straight downstream. Every fin carries the same
                    angle whatever its length.
    Sparks / Spark life / Spark size Burning debris marching downstream.
    Edge light      A rim light on the silhouette. Off by default.
    Shock bright    The shock envelope wrapped around the hull.
    Shell dist      How far the glowing shell stands off the skin. This is a
                    property of the HULL, not a universal number - it differs
                    between the DeltaGlider and the Atlantis.
    Bowl dist / Bowl size X,Y,Z      Shape of the bow shock in front.
    Trail density / life / width     The luminous trail behind you.
    Trail start     Where it begins. Negative moves it upstream into the
                    fireball; the hull correctly hides the overlap.
    Tint / Fringe   Body colour and the magenta cast, as hue rotations - pick
                    a colour and you get that colour.
    Trail hot/tail  Head and tail colours of the trail.

  VAPOUR CONES - the shroud that forms as you go through Mach 1. Real air holds
  water; the flow over the hull expands, the pressure and temperature drop, and
  the water condenses. It is the shock collar you have seen in every fighter
  photograph, and it needs LOW ALTITUDE (the water is in the troposphere) as
  well as the right speed - both are read from the sim, not set by you.
  THERE ARE TWO CONES, each with the identical, completely independent set of
  controls - colours included - so a hull can carry one collar at the canopy
  and one at the tail, the way the Concorde photographs show. The single pill
  arms the effect; each cone's own Opacity is its visibility. Cone 2 ships at
  zero, so it only exists where you give it some.
    TEST            Draws them at a fixed Mach 1.15 with the speed and altitude
                    gates bypassed, so you can judge the look from a runway
                    instead of flying an ascent over and over.
  Per cone:
    Opacity         0 = no cone. Up to 1 a translucent shroud; PAST 1 the sheet
                    fills and densifies until it can hide the hull behind it -
                    the fuselage-swallowing disc of the airshow photographs.
    Size x / y      The radii, in hull sizes - x along the wing line, y
                    vertical. EQUAL VALUES = A CIRCULAR CONE. Airframe facts.
    Size z          The length, as a fraction of what the Mach angle derives.
                    1 = the physics; 0 collapses the cone to a flat collar
                    disc. It scales the derived length rather than replacing
                    it, so the shroud still stretches back as you accelerate.
    Streaks         Slim darker filaments running length-wise through the
                    vapour. The slider is the COUNT - 0 is the clean sheet.
    Streak churn    How violently they live: jitter, flare and die. 0 freezes
                    the pattern; 2 runs it doubly fast.
    Flicker (Hz)    How fast the cone breathes. Opacity and size vary together
                    on one number, because a stronger condensation event is
                    denser and bigger at the same moment. 0 freezes it.
    Vapour/Streaks  Two colour swatches - the vapour body and the filaments.
                    Defaults are the natural pair: cool white over darker grey.
    Base fill       The filled disc closing this cone's wide end - what gives
                    it a back, and at high opacity the face that hides the
                    hull. Off returns the open shell.
    Base fill offset  Slides the base's centre along the cone's axis (-1..+0.5,
                    as a fraction of the cone's depth). -1 = pushed all the way
                    in, a second inner face of the cone; 0 = flat disc; +0.5 =
                    bulged outward. The rim never moves, so base and cone stay
                    joined. Does nothing on a flat collar (Size z 0) - there is
                    no depth to offset.
    Position x/y/z  Where this cone sits, in Orbiter's own axis order: x along
                    the wing line, y vertical, z along the flight direction.
                    All bipolar, snapping to zero. z separates the two collars
                    along the hull; x and y are for vessels whose shock does
                    not stand on the centreline.
    Pitch / Yaw     Tilts this cone off the relative wind, up to 30 degrees
                    either way. Zero (the snap) rides the wind exactly, which
                    is where physics puts it - the tilt exists for hulls whose
                    geometry stands the shock off at an angle. No roll: a
                    surface of revolution has nothing to roll.
    Mach band       TWO handles: where THIS cone starts and stops existing.
                    Default 0.85 - 1.15. Drag them together for a brief flash,
                    apart for a long transonic haze - and give the two cones
                    DIFFERENT bands to have the collars appear at different
                    speeds, which is what really happens on a real airframe.
    Cone            Readout: your current Mach and the strongest visible cone -
                    or why nothing shows (subsonic, thin air, vacuum, or
                    "internal", since the cones only draw in external views).
    NOTE  The LENGTH is deliberately not a raw setting. It comes from the Mach
          angle, so the shroud stretches back on its own as you accelerate.

  FLIGHT AID - not an effect. It changes what the VESSEL does.
    CoP shift (m)   Shifts the centre of pressure so a stock vessel will hold a
                    high angle of attack instead of weathervaning nose-first.
                    This exists so you can actually SEE a reentry; without it
                    stock ships drop the nose and there is little plasma. The
                    readout shows the pitch moment it is applying.
                    Ctrl+G releases it instantly - the nose WILL drop.
                    THE SHIPPED SETTINGS FILES TURN THIS ON for the DeltaGlider
                    and the Atlantis, because the reentry scenarios need it. If
                    your ship handles differently from stock, this is why, and
                    zero (the centre, which the knob snaps to) is stock exactly.
    Applies         REENTRY ONLY (M 3+) - the default. The aid is a reentry rig,
                    so it stays out of the way at ordinary flying speeds. This
                    matters: at low speed on an autopilot, two controllers
                    closing the same pitch loop can build up an oscillation.
                    ALWAYS             - apply it at any speed. The old
                    behaviour, kept for anyone who wants it.
                    While the gate is holding the aid off, the pitch-moment
                    readout says "gated" and your current Mach rather than 0.


=== WORLD ===  the sky
    (each of these is its own page: ECLIPSE, AURORA and GOD RAYS under WORLD,
     RAIN, FOG and LIGHTNING under WORLD / WEATHER)

  ECLIPSE - models your EYE, not the light. Dark adaptation is slow opening up
  and fast closing down, which is why coming out of shadow dazzles.
    TEST            Runs a full cycle in 40 seconds; you cannot wait for a real
                    alignment.
    Dim             How much the shadow darkens things.
    Eye adaptation  How strongly your eye compensates.
    Colour loss     Colour draining as your night vision takes over.
    Readouts        "Sun obscured NN% by <body>" and "Eye response". They
                    genuinely differ: fully adapted inside totality the sun is
                    100% covered while your eye is doing nothing. That is
                    correct, not a bug. On the ground at night you will see
                    100% obscured by Earth.

  AURORA - curtains around the magnetic poles, at twelve worlds.
    TEST            Rings the point below the camera so you do not have to find
                    a polar night.
    Activity        Master strength. 0 means this world has no aurora - that is
                    also how you switch one on at a world that has none.
    Oval lat        How far from the pole the ring sits (shown in degrees).
    Fold / Rays / Breakup   Shape: the waviness, the vertical rays, and how
                    much the curtain breaks into separate arcs.
    Thickness       Sheets per curtain. More sheets brighten the edge-on view
                    the way a real curtain does, without brightening overall.
    Base / Top (km) Altitudes, in real km for the world you are at.
    Ribbons         How many concentric arcs.
    Tilt X / Y      The magnetic pole's offset from the spin axis. Earth's is
                    about 11 degrees; Uranus is 59; Io's aurora is equatorial.
    Base/Body/Top   THREE colours by altitude - which gas emits, and which of
                    its lines, depends on how deep the particles get. Earth's
                    lower border is nitrogen violet and its top is oxygen red,
                    so two colours cannot render it.

  LIGHTNING - one page, BOTH of ORO's lightning systems, under two headers.
    FROM ORBIT draws storms in a planet's cloud deck as you look down on them,
    night side only, saved PER BODY. IN THE STORM is the rain storm's own
    lightning - flashes in the deck overhead, bolts to the ground and their
    thunder, day or night, saved globally, and it needs the RAIN pill (or its
    Test) on to fire. They are independent: neither affects the other.
    The FROM ORBIT rows:
    TEST            One fast cell north of you with every gate bypassed, so you
                    can judge it from a runway in daylight.
    Activity        How many storms. 0 = none at this world.
    Brightness / Flash rate / Cell size (the km readout tells you what the
                    slider means).
    Flash colour    Default is the blue-white lightning looks like from the ISS.
    Readout         "<body> - N cells". "No storms" has three honest causes -
                    day side, clear sky, or activity 0 - and the count tells
                    you which.
    Storms form where the CLOUD actually is: ORO reads the planet's own cloud
    map. And flashes only show on the NIGHT side, which is deliberate - from
    orbit you cannot see a diffuse in-cloud flash against a sunlit deck.

  GOD RAYS - crepuscular shafts, the beams you get when a low sun is broken up
  by terrain, cloud or a hull. They need AIR to scatter in, so they do not run
  in orbit at all, and they need something to break the beam up - with the sun
  in open sky the technique can only smear the disc into a halo.
    TEST            Bypasses the air and sun-height gates (but not "the sun has
                    to be roughly on screen"), so you need not wait for a sunset.
    Strength        Master intensity.
    Reach           How far the shafts extend from the disc.
    Softness        Crisp short rays through to long soft ones.
    Sensitivity     How dim a thing may be and still cast a shaft. This is the
                    knob that separates "shafts" from "radial blur over the
                    whole sky". Like every slider here, more is more.
    Warmth          How far they redden as the sun nears the horizon.
    Shafts          Readout: strength, or why it is zero - "vacuum", "high sun",
                    "night", "behind" (sun is behind you) or "off-view".
    Best seen low, near sunrise or sunset, with terrain or cloud between you and
    the sun. An eclipse kills them, which is correct - less beam to scatter.

  RAIN - a storm you summon at the surface. The build-up ramps over about ten
  seconds: the light collapses to overcast, streaks fall, the ground soaks
  dark, water stands in pools and the ships reflect in them - and the storm
  SOUNDS: rain loops that build with it, and thunder answering every flash.
  Switching the pill OFF is instant on purpose, so you can compare the wet
  world against the dry one; the ground then dries out over a couple of
  minutes. External view AND the virtual cockpit (see below), Earth only for
  now, and only below the weather - everything fades out by about nine
  kilometres up, so a reentry begun with the pill still on gets a clean dry
  hull in space.

  THE STORM FROM THE COCKPIT: in a VIRTUAL cockpit the rain, splashes, cloud
  deck and bolts are all visible through the windows, cut per pixel at the
  frame and canopy - the inside of the cabin stays dry, with no wet sheen or
  drop sparkle on the panels. The sound follows you in: the storm drops to a
  muffled level through the hull, and raindrops DRUM on the skin instead.
  2D panel and glass-cockpit views stay dry by design. This needs Sun glare
  enabled (section 3) - without it the panel readout says "VC: SunGlare off"
  and the VC simply shows no rain rather than rain painted over the cabin.

  The page groups its sliders three ways, in order: THE STORM OUTSIDE,
  THE WINDSCREEN (drops on the glass), and SOUNDS. The storm's own LIGHTNING -
  flashes, bolts, thunder and the STRIKE test rig - lives on the LIGHTNING
  page beside this one, under its IN THE STORM header.
    TEST            The same storm as the pill, without enabling the effect.
  The storm outside:
    Gloom           How dark and grey the world goes. This is not a screen
                    filter: it collapses the SUN at the source and lifts the
                    ambient, so shadows and the warm cast go with it.
    Cloud detail    The storm deck's texture notch: 0 = plain darkened sky,
                    1/2/3 = ever finer billow detail. Snaps to whole notches.
                    The deck is two real cloud layers at two altitudes, with
                    parallax and hanging masses.
    Density         How many streaks are in the falling sheet.
    Fall speed      How fast they fall.
    Streak length   How long each streak draws.
    Streak glow     How brightly they catch the light.
    Slant (deg)     Wind - tilts the sheet up to 15 degrees either way.
    Splashes        Rings where drops land, on ground and on water. Two
                    fields: one around the camera, one around the ship.
    Wet dark        How far the wet ground darkens. 1 is the designed look,
                    2 near-black; standing water goes darker still.
    Pool size       How large the standing pools grow - and at 0, whether
                    there are any: turn it fully down for a soaked apron with
                    no standing water at all. Pools only appear once the
                    ground is properly soaked (about 70% wet), and they are
                    PINNED TO THE GROUND - drive and they stay put.
    Pool reach      How far out pools stay visible (roughly 900 m at 1). The
                    damp sheen carries on past them.
    Grain/size      Broken-water texture inside the pool reflections -
                    irregular matte patches, static in the world. Grain digs
                    them in (0 = uniform pools); Grain size coarsens them.
    Glint           Raindrop sparkle on hulls - every vessel in the scene.
    Reflection      The vessel image in the wet ground: a real mirrored
                    render, upside down at the contact points, concentrated
                    in the pools. The grey sky in the pools is always there;
                    this adds the SHIPS - hulls, nav lights and strobes,
                    contrails and particle streams, and ORO's own engine
                    plume. It is a genuine second render of the scene rather
                    than a copy of the picture, so at half resolution and
                    through the ripple, fine structure like a shock-diamond
                    train reads softer than it does in the air.
    Reflection blur How diffuse the reflection in the wet ground is. 0 is a
                    crisp mirror; raise it and the image spreads and softens
                    the way it does on a real wet apron, which scatters light
                    rather than mirroring it. A little goes a long way - it
                    should still read as the ship, just not as glass.
    Swim size/rate  The rain-pocked ripple on that reflection - how far the
                    image warps and how fast it flickers. Size 0 is a still
                    mirror.
  The windscreen (virtual cockpit only; needs Sun glare, and the glass must
  be DECLARED - click it with the RAINSURFACES button below, or mark it with
  a one-line RAIN 1 mesh token; both are described after this section):
    Glass drops     Raindrops ON the cockpit glass. Coverage: how much of the
                    pane fills with drops at full storm. The window fills
                    gradually and dries in reverse.
    Drop size       How big the drops are, as the eye sees them. Runners scale
                    with it too. 0 is no drops at all.
    Drop lens       How strongly each drop bends what is behind it - past ~1
                    the image inside a big drop genuinely inverts, like the
                    real lens a droplet is. 0 leaves drops that only glisten.
    Build up (s)    Seconds from a clean canopy to the Glass drops target at
                    full storm. The fill is the show - drops pop in one by
                    one and swell as they land.
    Runners         Loose drops that break away and run across the glass,
                    leaving a fading wet trail - straight down when parked,
                    sweeping aft with airspeed. Their speed is not a knob: it
                    follows gravity plus the real airflow.
    Runner size     Runner thickness relative to the sitting drops - a ratio,
                    so Drop size still scales both families together.
    Drop debug      A development aid; leave it at 0.
    Rain view       Which internal views get the rain. VC ONLY (default),
                    VC + PANEL, or ALL VIEWS. Outside views are always wet.
                    In the VC the rain is cut at the window frame per pixel
                    (needs Sun glare on); in the flat panel views Orbiter
                    paints the panel over it, so nothing extra is needed.
    Rain surfaces   Opens the RAINSURFACES popup - declare which mesh groups
                    take drops by CLICKING them in the sim, no mesh editing.
                    ADD goes amber; click your windscreen from the VC and the
                    group lights up green while you hold the button, then
                    joins the list. REMOVE takes one off the list (click the
                    entry, confirm). SAVE writes the shared file AND applies
                    it immediately - the drops respond without a reload.
  Sounds:
    Rain sound      The storm's sound heard from OUTSIDE: three rain loops
                    (patter / steady / downpour) crossfading as it builds.
                    1 is the designed mix against Orbiter's other ambient
                    sounds, 0 is silent. Inside a cockpit you hear a
                    DIFFERENT volume - Rain in cabin on the VIRTUAL COCKPIT
                    page, with the hull drum beside it - so the two are set
                    independently. Needs XRSound.dll (ships with Orbiter
                    2024). Thunder lives with the bolts, on the LIGHTNING page.
    Rain            Readout: the storm's build-up and ground wetness, or the
                    honest reason nothing draws - "external only", "Earth
                    only", "above the weather", "VC: SunGlare off".
    Base lights     Forces every base's NIGHT state on now - night textures,
                    runway and taxiway lights - the way an airfield lights up
                    when the visibility drops. Off is Orbiter's own behaviour
                    (on at night, off by day). The same switch sits at the
                    bottom of the FOG page: one setting, two doors.
    Lights glow     A gain on everything those lights emit; past 1 they reach
                    the Light glow post-process and bloom.
    Lights halo     The aureole round each runway light in fog - a soft disc
                    that grows with the fog between you and the lamp. 0 is a
                    plain lamp that only dims with distance.

    FOR VESSEL AUTHORS - THE RAIN SHIELD. From inside a virtual cockpit the
    rain is kept out of the cabin by a depth test that covers cockpit-sized
    interiors on its own. A vessel with a LARGER interior (a passenger cabin,
    a multi-deck flight deck) can seal it completely with a tiny authored
    mesh:  Meshes\ORO\<class>_rainshield.msh  (same class-name rule as the
    ORO per-class .cfg files; the DeltaGlider and DG-S ship with theirs).
    Every triangle in the file is a ROOF PANEL in vessel coordinates:
    rain is removed wherever a panel sits within 3.5 m directly above it, so
    a flat quad at ceiling height over the cabin footprint is usually the
    whole file. Panels may overlap freely and sit at different heights (one
    ceiling per deck); a GAP between panels is a real hole the rain falls
    through - which is exactly right for an open cargo bay. Keep the roof's
    edges at the window line: extend it past the glass and the rain just
    outside that glass is culled too. Materials, textures and normals in the
    mesh are ignored; ORO re-reads the file at every session start, so you
    can iterate on it between runs.

    DECLARING THE GLASS - FOR EVERYONE. The drops need to know which mesh
    groups ARE the glass, and the easy way is the RAINSURFACES button in the
    windscreen section: sit in the VC, press ADD, click the window (it lights
    green while you hold the button), SAVE. The picks live in
    Config\ORO\VesselsRainSurfaces.cfg - one "<mesh name> <group>" line each,
    shared by every vessel - and SAVE applies them to the running sim at
    once. The shipped file already declares the stock DeltaGlider's
    windscreen, so the DG rains out of the box.

    FOR VESSEL AUTHORS - THE WINDSCREEN GLASS TOKEN. A vessel can also opt
    in from its own mesh file: put  RAIN 1  on a line of its own directly
    before a window group's GEOM statement (Orbiter's mesh parser ignores
    unknown tokens, so the line is invisible to everything else). Both
    routes work side by side. No declaration, no drops - the rest of the
    storm is unaffected.

    Needs the ORO patched client for the wet ground, storm light, glint and
    reflections (all probe by binding and quietly stand down without it);
    the falling rain and splashes draw on any client. The lightning bolt
    imagery is derived from the free Resource Boy lightning texture pack
    (resourceboy.com) - thanks to them; the pack permits modified use in
    applications. The thunder recordings are CC0 / CC-BY 4.0 from
    freesound.org, credited in XRSound\ORO\README.txt - that credit file
    travels with any redistribution.


  FOG - a ground fog where you are, rendered by the patched client inside
  every surface: terrain, the apron, hulls, exhaust, runway lights and the
  sky all fade into the same grey. The colour is not a picker - it follows
  the sun (warm at dawn, grey under a storm, dark at night), the sun and
  every shadow weaken through it, and the cockpit interior stays clear.
    TEST            Previews the same fog with the pill off. Both build over
                    about twenty seconds; the pill off clears at once.
    Visibility (m)  How far you can see at ground level. 300 is a real fog,
                    50 is a wall, 2000 is a haze.
    Fog top (m)     How thick the layer is over the ground under your vessel.
                    Climb out of it and you look down on a blanket.
    Fade            How the top thins: 0 a solid slab with a hard ceiling,
                    1 thinned to a few percent by the top, 2 wispy.
    Brightness      A gain on the lit fog colour. 1 is the client's own.
    Sun glow        The bright lobe toward the sun. 0 removes it.
    Fog             Readout: the build-up, or why there is none (no air on
                    this world, camera at another world, too high).
    Base lights / Lights glow / Lights halo - the same three as on the RAIN
                    page (one setting, two doors).
    The RAIN page's Gloom drives a second layer - the storm's own mist, up to
    the storm deck. Needs the ORO patched client; on stock the page is inert.

=== PILOT / VIRTUAL COCKPIT ===  the cockpit

   SAVE TARGET     The same switch, for this whole page - the shadow on/off and all
                   six cam-shake knobs. Cam-shake is the reason it is here: a big
                   heavy ship should not rattle like a tiny one, and amplitude and
                   frequency describe what a HULL passes to the seat. The cabin box
                   and shadow depth were already per vessel class either way.

  VC SHADOWS      Sunlight through the canopy, sweeping across the cabin as you
                  rotate. Needs local shadows enabled (section 3).
    Cabin box (m) The size of the area the shadow map covers. The map is a
                  fixed number of texels across that box, so HALVING the box
                  doubles the resolution - and a cockpit panel is forty
                  centimetres from your eye, where that is very visible. Go as
                  low as 0.4 m if you want the crispest possible shadows.
                  The cost is real and structural: the box is also how far out
                  ORO looks for things that CAST, so shrink it far enough and a
                  vessel docked outside your window stops throwing a shadow
                  into the cabin. You cannot have both from one shadow map.
    Shadow depth  How DARK the shadows go. 0 is Orbiter's stock behaviour,
                  where a shadow only removes direct sunlight and the cabin's
                  ambient light keeps everything visible - which is why stock
                  cockpit shadows look washed out. Raising this lets the shadow
                  take the ambient with it. Lit instrument panels are never
                  dimmed, so your MFDs stay readable.

  CABIN AT NIGHT  Stock Orbiter keeps a virtual cockpit fully lit at midnight:
                  its authors fill it with a flat emissive light, and the
                  sun going down changes nothing inside. With this on, the
                  cabin goes dark as the sun sets at the camera - a twilight
                  ramp on worlds with air, a sharp flip without, and in orbit
                  the planet's shadow. MFD screens, self-lit instruments,
                  emission maps and every cockpit lamp are untouched - so
                  switching the cabin light on finally means something.
                  Needs the ORO patched client (the section greys out
                  without it).
    Night floor   What is left at full night. 0 is black without a lamp on,
                  1 is stock. The caption shows the live value.
    DIM UNDER RAIN AND FOG  A second pill: the weather takes light out of the
                  cabin too - a storm collapses the sun (RAIN's Gloom), a
                  thick fog thins it (FOG's Visibility).
    Weather dim   How much of that loss the cabin shows. 1 = a full storm at
                  noon leaves about a tenth; 2 = near black; 0 = off.
                  One compromise: a lit button painted with the same flat
                  emissive as the walls dims with the walls. A real self-lit
                  display does not.

  CABIN SOUNDS    The storm as heard from the seat.
    Rain in cabin The rain loops heard INSIDE, muffled through the hull.
                  Independent of the RAIN page's Rain sound (the outside
                  mix): 0 here is a silent cabin in a storm you can still
                  hear from an external view.
    Hull drum     Rain on the SKIN of your ship heard from inside - a metal
                  roof under rain: dull thumps over a low panel rumble, no
                  hiss - structure-borne so it is never muffled. Its own
                  volume, up to 3; 0 is silent.

  CAM-SHAKE       Buffet and the push into your seat. The STRENGTH is physics
                  driven - thrust, dynamic pressure, ground contact - so these
                  sliders shape the LOOK, not the amount. They are two separate
                  sensations and you can have either without the other.
    Seat push              The smooth sustained LEAN, opposite whichever way
                           you are being accelerated: back into the seat under
                           main thrust, down under hovers, forward under
                           deceleration. 1 = standard, 0 = no lean at all.
    X / Y / Z range (mm)   Buffet amplitude per axis - the RATTLE. All three at
                           zero leaves the seat push on its own.
    Frequency (Hz)         How fast it shakes.
    Test                   Forces full intensity so you can tune it parked.


--------------------------------------------------------------------------------
6. SAVING  -  three scopes, and this trips people up
--------------------------------------------------------------------------------

Settings are saved in three places, because they answer three different
questions. Each page's SAVE button says which files it writes - and turns
AMBER while those files have unsaved edits.

  GLOBAL      Config\ORO.cfg
              What the PILOT is: G tolerance, posture, effect enables, camera
              shake shape, scenario sound.

  PER VESSEL CLASS   Config\ORO\<class>.cfg
              What a HULL needs: all plasma tuning, the exhaust and particle
              settings, the VC cabin box and shadow depth. Size, shape and
              engine layout decide every one of these, so the DeltaGlider's
              numbers are meaningless on the Atlantis.

  PER BODY    Config\ORO\bodies\<world>.cfg
              What a WORLD is: aurora and lightning.

Two consequences worth knowing:
  * Unsaved changes are LOST when you switch to a different vessel class.
  * A vessel class with no file of its own keeps whatever is on screen rather
    than resetting, so an untuned ship inherits your last look.

The REVERT button beside each page's SAVE re-reads that page's files, so an
hour of tuning that went nowhere costs one click rather than a restart. It
follows the same two rules: a hull with no file of its own keeps what is on
screen, a world with no file goes back to the built-in defaults.

This beta ships tuned files for the DeltaGlider, DG-S, Atlantis and the ISS, and
for eleven worlds. Other vessels will work but are untuned.


--------------------------------------------------------------------------------
7. KNOWN LIMITS IN THIS BUILD
--------------------------------------------------------------------------------

* Scenario audio is one clip by design: INDUCE G-LOC. The other five scenario
  buttons are deliberately silent - the visual effect is the whole effect on
  those. Orbiter.log notes the absent files at startup; ignore those lines.
* Reentry plasma is tuned on the DeltaGlider and Atlantis. Other hulls vary;
  Shock bright 0 turns the shell off if it looks wrong on something.
* Aurora scales for the gas giants and moons are derived from physics but have
  never been checked against the limb in the sim. Tell me if one looks absurd.
* Lightning ships for Earth. Other worlds have it available but set to zero.
* Orbiter 2024 crashes on exit on some installs, after everything is saved and
  closed. It predates ORO - it is Orbiter's own shutdown path - and it costs
  nothing. If you see a crash dialog AFTER quitting, that is probably it.
* Cascaded shadows, three edges: reflection probes and planar mirrors do not
  see the atlas (they keep the per-vessel map, so a reflected hull can read a
  little brighter than the real one); where more than two hulls overlap, a hull
  pixel takes the two nearest; and Cascade detail 4096 costs 768 MB of video
  memory - drop to 2048 if the frame rate falls.
* In the STENCIL and PROJECTED terrain-shadow modes, a spotlight at dusk can
  bite a vessel's terrain shadow where the map borrow opens before the sun is
  fully down. Cascaded mode has no borrow and no bites.
* The rain is hard to see at night away from a lamp, and the lightning flash
  does not yet light the cockpit or cast shadows. Both are on the list for the
  next release.


--------------------------------------------------------------------------------
8. IF SOMETHING GOES WRONG
--------------------------------------------------------------------------------

FIRST: press CTRL+G. That disarms everything and hands the sim back its stock
behaviour, without closing anything. If the problem persists with ORO
disarmed, it is not an ORO effect.

Then please send me:
  * Orbiter.log from the Orbiter root - ORO writes a lot to it, including
    which client capabilities it found and any problem it noticed.
  * The scenario you were flying and roughly what you were doing.
  * Your GPU, and your frame rate armed vs disarmed.

To remove ORO, CLOSE ORBITER AND THE LAUNCHPAD, then run ORO_Uninstall.bat in
the ORO_beta folder. It restores your original graphics client and shaders and
deletes ORO's files.

Closing Orbiter first is not politeness - both Orbiter and the Launchpad hold
the graphics client file open, and it cannot be replaced underneath them. The
uninstaller now checks for this and refuses to run rather than trying anyway.

IT PUTS YOUR CLIENT BACK BEFORE IT REMOVES ANYTHING, and reads the file back
to confirm it arrived intact. If that check fails it stops and removes nothing,
so you are left with ORO still installed and working rather than with neither.
If it ever stops that way, close anything still running and try again.

IF ORBITER WILL NOT START after a failed uninstall - the Launchpad dies a line
or two into loading - run ORO_Uninstall.bat again. It will spot that your
graphics client is missing or damaged and offer to repair it on the spot.

IT WILL NOT DELETE ANYTHING YOU TUNED. A file is removed only if it is
byte-for-byte what ORO shipped; anything you changed or added - a vessel you
dialled in, a world you configured, a sound you recorded - is kept, and listed
at the end so you know what is still there. Those files do nothing on their own,
and they will be picked up again if you reinstall.

Thanks again for flying it.
