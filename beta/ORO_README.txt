================================================================================
  O R O   -   Orbiter Realism Overhaul                      CLOSED BETA
  Atmospheric, Physiological and Visual Immersion Suite
================================================================================

  >>> IF YOU HAVE THE EARLIER BETA INSTALLED, UNINSTALL IT FIRST. <<<

  This addon used to be called PULSE. Before installing ORO, go to your old
  PULSE_beta folder and run PULSE_Uninstall.bat.

  This is not optional housekeeping. The ORO installer looks for ORO's files,
  and every one of them has a different name now - so it cannot see PULSE's
  files, cannot back them up, and cannot remove them. Skip this step and you
  end up with BOTH addons installed and both listed in the Launchpad, which
  will look like a bug in ORO. Section 2, step 1.

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

  - An in-panel HELP window, per tab. Several things you asked for already
    existed and there was no way to discover them - that is what this answers.
  - The panel resizes vertically, and there is a REVERT button.
  - Two effects you have never seen: crepuscular GOD RAYS, and the transonic
    VAPOUR CONE.
  - Every thruster setting is now PER ENGINE GROUP, so a vacuum-rated main and
    sea-level hovers can be described separately. Your existing tuning is
    carried into every group untouched.

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

NEW SINCE THE BUILD YOU HAVE (the PULSE beta, 260810):
  * A CRASH FIX, and this is the one that matters. The build you are running
    can crash when you exit to the Launchpad and start a scenario again. It is
    fixed here - three separate causes, all of them the addon handing Orbiter
    something before the new scene existed. If you have been hitting that, it
    was not your machine.
  * VAPOUR CONE - the transonic shock collar. REENTRY tab. Try TEST on a runway.
  * GOD RAYS - crepuscular shafts from a low sun. ATMOS tab.
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

1. UNINSTALL THE OLD PULSE BETA FIRST, if you have it.
   Go to your PULSE_beta folder and run PULSE_Uninstall.bat. Let it finish and
   read what it says - it keeps anything you tuned and tells you what it kept.
   ORO's installer cannot do this for you: it looks for ORO's files, and none
   of PULSE's files have those names any more.
   If you never installed the earlier beta, skip to 2.
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

    Module D3D9Client.dll ........ [Build 260812, ...]   <- patched, good
    Module D3D9Client.dll ........ [Build 241231, ...]   <- still stock

It is the BUILD number that tells them apart. Ignore the "API" number printed
beside it - that reports which SDK the DLL was compiled against, and the patched
client is built from source so it does not match the stock one. On this build it
reads API 260725, and that is correct, not a failed install.

A few lines further down ORO lists what it found in the client - patches (d),
(f), (g), (i), (k), (l), (n), (o) should all read "available".


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

In the Launchpad, VISUAL EFFECTS tab:

  Particle streams ........... ON (it is on by default)   REQUIRED for the
      THRUSTER > PARTICLES tab. With it off, Orbiter refuses to create any
      particle stream and that whole tab silently does nothing.

  Ambient light level ........ leave at the stock 20
      Raising it washes out every shadow in the sim, including ORO's.

Reference: the settings this was developed and tuned on are
ShadowMapMode 2, ShadowMapFilter 2, ShadowMapSize 2048, PostProcess 1,
SunGlare 1, AmbientLevel 20.


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
  * THRUSTER > EXHAUST > BELL GLOW: the engine bells heat up and cool down on
    their own as you throttle.
  * Go to orbit, then come back in steep. The reentry plasma is on the REENTRY
    tab and is the biggest thing in the addon.
  * Sit in the virtual cockpit (F8) at a low sun angle and roll - sunlight
    sweeps across the cabin through the canopy (VC tab).

  Nothing you change is permanent until you press SAVE - see section 6.


--------------------------------------------------------------------------------
5. THE PANEL, TAB BY TAB
--------------------------------------------------------------------------------

Every effect has a PILL (the round toggle on the left - green is on) and a
SLIDER. Sliders show their value on the right. The panel scrolls; each tab has
its own SAVE button explaining what it writes.

=== ALWAYS VISIBLE (top of the panel) ===

  ENABLED / DISABLED   Master arm. Same as Ctrl+G. Kills every effect at once
                       and gives the sim back its stock behaviour.
  HELP                 Opens the help window for WHICHEVER TAB YOU ARE ON, so
                       where you are when you press it is the question you are
                       asking. It never opens twice: press it again on another
                       tab and the window you already have switches to that
                       tab's text. Closeable and resizable, and it remembers its
                       size - but not that it was open, so it never reappears by
                       itself on a new session. The button lights green while it
                       is up, which is also how you tell it has opened behind
                       the sim window rather than not opened at all.
                       Everything in section 5 below is in there, in more depth.
  SAVE                 Writes ALL settings, in all three scopes (section 6).

Each tab also carries its own SAVE and a REVERT beside it. REVERT re-reads that
tab's files from disk and throws away everything you have moved since the last
save - the way back from a tuning session that went wrong.

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


=== TAB: G-FORCE ===  what high G does to you

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


=== TAB: THRUSTER ===  engines. Two sub-tabs.

⚠ FIRST, THE ENGINE GROUP BUTTON in the top strip beside SAVE. Everything on
this tab - both sub-tabs - edits ONE engine group at a time, and that button
says which. Click it to cycle through the groups this vessel actually has:
MAIN, HOVER, RETRO, and USER for engines the author put in no standard group.
RCS is never included. A ship with only main engines has nothing to cycle to.
  Why: one set of numbers only suits a ship whose engines all burn the same
  thing. The expansion band's high handle is the pressure an engine is RATED
  for, the Jet/Bloom swatches are its exhaust colour, and soot is the difference
  between kerosene and hydrogen. Now a vacuum-rated main and sea-level hovers
  can each have their own.
  Tune a group, cycle, tune the next. SAVE writes them all, so working on the
  hovers cannot disturb what you did to the mains.
  If you already had a tuned class file, its single set of numbers loads into
  ALL FOUR groups - nothing is lost and nothing looks different until you cycle
  and change something.
  STOCK EXHAUST, STOCK PARTICLES and CANCEL THRUST are the exceptions: they
  apply to the whole vessel, because the client suppresses stock exhaust per
  SHIP and cancelling thrust acts at the centre of mass.

--- Sub-tab EXHAUST - the parts ORO draws itself ---

  Exhaust shimmer   Heat haze bending the view behind the plume, in atmosphere.
  Offset (m)        Slides the haze along the plume axis.

  PLUME EXPANSION - a rocket nozzle is built for ONE ambient pressure; the
  atmosphere decides what the jet does everywhere else. Overexpanded at sea
  level gives the narrow pinched jet with the shock-diamond train;
  underexpanded in vacuum gives the wide faint bloom.
    LAB | PHYSICS   PHYSICS lets pressure and throttle drive the shape.
                    LAB pins them so the sliders rule alone. Both are anchored
                    identical at sea level and full throttle.
    Expansion band  TWO handles on one track: the pressure range this engine is
                    built for. Drag the high handle down and you have a vacuum
                    engine that shudders and pinches at the pad.
    Width / Length      Overall jet size.
    Diamonds            How many shock cells in the train (1-12).
    Diamond bright      Their contrast.
    Diamond spacing     How far apart they sit.
    Bloom width/bright  The wide vacuum plume.
    Throat glow         The fire seen down inside the nozzle.
    Throat offset       Nudges it, because the visual nozzle and the engine's
                        defined exhaust point disagree on some hulls.
    Soot streaks        Dark soot shedding off the nozzle lip. 0 = off.
    Soot churn          How fast it moves. 0 freezes it.
    Jet / Bloom         Two colour swatches.

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

  STOCK EXHAUST   Off = hide Orbiter's own exhaust texture, so you judge ORO's
                  plume alone. This only affects the flame BILLBOARDS. Stock's
                  exhaust PARTICLES have their own pill, on the PARTICLES
                  sub-tab - they were split deliberately, so that an addon which
                  replaces only one of them can be handled.
  CANCEL THRUST   A test stand: cancels the vessel's thrust so you can run the
                  engines up on the ground and look at them. Never saved.

--- Sub-tab PARTICLES - Orbiter's own particle system, under your control ---

ORO draws none of these. It hands Orbiter the same settings a vessel author
sets in code, and lets you move them live. Units are the API's own.

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
                 DIFFUSE  - the sun lights them (smoke, vapour). This is the
                 single biggest change in the whole tab; try both.
  Air fade       FADES IN VACUUM - Orbiter's stock behaviour, where a stream
                 fades out as the air thins. THIS IS THE DEFAULT: exhaust clouds
                 hanging in orbit look wrong, and they are wrong.
                 ALWAYS ON       - emit everywhere, including vacuum.
                 If you enable this tab in orbit on the default and see nothing,
                 that is the fade doing its job - the row's own label says
                 "Air fade - in vacuum" while it is holding emission off, and
                 the caption below says so too.
  Colour         Tints the particles.
  STOCK PARTICLES  The vessel author's own exhaust particles. This pill and the
                 one at the top of the tab are MUTUALLY EXCLUSIVE - stock's or
                 ORO's, never both. Turning one on turns the other off.
                 Both off is also fine: no exhaust particles at all.


=== TAB: REENTRY ===  the biggest effect in the addon

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

  VAPOUR CONE - the shroud that forms as you go through Mach 1. Real air holds
  water; the flow over the hull expands, the pressure and temperature drop, and
  the water condenses. It is the shock collar you have seen in every fighter
  photograph, and it needs LOW ALTITUDE (the water is in the troposphere) as
  well as the right speed - both are read from the sim, not set by you.
    TEST            Draws it at a fixed Mach 1.15 with the speed and altitude
                    gates bypassed, so you can judge the look from a runway
                    instead of flying an ascent over and over.
    Strength        Opacity of the shroud. 0 turns it off.
    Size            Outer radius, in hull sizes. A property of the AIRFRAME.
    Position        Where it sits along the flight direction. Bipolar, snaps to
                    zero at centre. Also a property of the airframe - it depends
                    on the shape of the nose.
    Mach band       TWO handles: where the cone starts and stops existing.
                    Default 0.85 - 1.15. Drag them together for a brief flash as
                    you punch through, apart for a long transonic haze. The
                    fade-in and fade-out live inside whatever window you set.
    Flicker (Hz)    How fast it breathes. Opacity and size vary together on one
                    number, because a stronger condensation event is denser and
                    bigger at the same moment. 0 freezes it.
    Cone            Readout: your current Mach and how strong the cone is - or
                    why it is not showing (subsonic, thin air, vacuum, or
                    "internal", since it is only drawn in external views).
    NOTE  The LENGTH is deliberately not a setting. It comes from the Mach
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


=== TAB: ATMOS ===  the sky

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

  LIGHTNING - storms in the cloud deck, seen from above.
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


=== TAB: VC ===  the cockpit

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
questions. Each tab's SAVE button says which files it writes.

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

The REVERT button beside each tab's SAVE re-reads that tab's files, so an hour
of tuning that went nowhere costs one click rather than a restart. It follows
the same two rules: a hull with no file of its own keeps what is on screen, a
world with no file goes back to the built-in defaults.

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
