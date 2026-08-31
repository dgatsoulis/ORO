# ORO — what it does

*An immersion experience for Orbiter 2024. Written 2026-08-10, the day the closed beta shipped.*

This is the first time the whole thing has been listed in one place. Everything below is
**built, flying, and tested in the sim** unless a line says otherwise.

| | |
|---|---|
| Started | 2026-07-25 |
| Shipped to beta | 2026-08-10 (16 days) |
| Distinct effects | **29** — 13 physiological, 16 environmental (the windscreen drops joined the storm; the vapour cone is now TWO independent cones) |
| Live controls | **147** sliders/knobs, **17** colour pickers, organised as a **menu tree** (WORLD / VESSEL / PILOT → 7 menus, 13 pages), all thruster settings PER ENGINE GROUP (MAIN / HOVER / RETRO / USER / **RCS**) — or **PER INDIVIDUAL THRUSTER** via layered overrides |
| Source | ~21,400 lines across 16 C++ files, plus 9 pixel shaders in one HLSL file |
| Client patches | **26** (a–y, +k2) — every one of them load-bearing |
| Worlds with auroras | 12 |
| Settings scopes | 3 — global / per vessel class / per body (+ a window-geometry file). The G-FORCES and VIRTUAL COCKPIT pages' settings can be moved between the first two per hull with a **Save target** switch |

---

## 1. PHYSIOLOGY — what the pilot's body does to the view

Internal view only (2D panel or VC). Ten of these run through a real **felt-G model**:
proper acceleration `(F−W)/m` computed *at the camera position*, resolved into pilot body
axes by posture, then through cardiovascular lag, onset rate and a cerebral-oxygen reserve.
In PHYSICS mode the sim drives them and the sliders become gains; in LAB mode you drive
them directly.

| Effect | What it is |
|---|---|
| **Blackout** | Vision loss under +Gz. Full range to black. |
| **Red-out** | Negative-G blood push. Caps at 80% so MFDs stay at the edge of readable. |
| **Tunnel vision** | Peripheral closure. The heartbeat throb *modulates* it, so the pulse survives a closed tunnel. |
| **Dark spots** | Scotoma blotches across the field. |
| **Grey-out** | Full-frame desaturation (an HLSL frame resample, not an overlay). |
| **Blur** | Eyeball-out defocus, driven by ±Gx. |
| **Chromatic aberration** | Lens fringing under load, also ±Gx. |
| **Sparkles** | Phosphenes. Suppressed by blackout — full vision loss means no sparkles. |
| **Swim** | Peripheral warp, the woozy edge of the field. |
| **Tilt / sway** | Whole-field roll sway, with a *signed* lean from ±Gy. |
| **Blink** | Involuntary lid closure. An event, not a slider. |
| **Heartbeat** | A cardiac vignette that throbs at a rate the model sets — plus a real "lub-dub" wav re-fired once per beat, so the sound tracks the picture. |
| **Camera shake** | The first physics-driven effect: seat-push and buffet from thrust, dynamic pressure and ground contact, applied by perturbing the vessel's camera offset. Its sliders shape the *look* (X/Y/Z in mm, frequency in Hz); the sim decides the intensity. |

**Scenario player** — one-click scripted G-events: INDUCE and RECOVER FROM G-LOC / Grey-out
/ Red-out. Induce ramps up and *holds* until you recover; recover starts at the matching
peak so the hand-off is seamless. Each plays its own authored clip for the scenario's exact
duration. Sliders animate live and lock while a scenario runs.

**Pilot model** — G tolerance (readout in real G), anti-G suit (+1.5 G, +Gz only), five
postures (seated / reclined / prone / standing / couch — this picks which vessel axis is
your spine), G reference (camera or vessel CoM — in orbit this *is* the whole effect), and
a live signed Gz/Gx/Gy + O₂ reserve readout that goes red under 50%.

---

## 2. ENVIRONMENT — what the world does

External view, and through the VC windows where it makes sense.

### Reentry — the big one

A complete replacement for Orbiter's two camera-facing billboards. Heat is
**Sutton-Graves** (`√ρ·v³`), which peaks higher and later than stock's `ρ·v³`.

- **Shock shell** — ORO welds and decimates *the vessel's own mesh triangles* into a
  smoothed, detail-clamped copy and lights it. Any vessel gets a heatshield for free, with
  no authoring. Authors who want control can drop in `Meshes\ORO\<class>.msh` and override
  it wholesale.
- **Shock envelope** — a lofted bow-shock surface built from two smoothed angular profiles
  of an "airstream map", so it is smooth at any range instead of showing the mesh's
  tessellation. Its gas cap fills and whitens with heat.
- **Fins and contour fins** — plasma streamers spanning real mesh edges, feathered across
  their width, spent round-robin across angular bins so mesh authoring order can never
  decide where they go.
- **Streams** — 18 mitred ribbon wakes with striations, flame-shaped origin glows and
  marching sparks. The wake *spreads* downstream while its hot core narrows.
- **The trail** — a ribbon threaded through a particle pool, planet-relative and
  epoch-exact, tens of km long, breaking cleanly on teleports and SOI handovers.
- **Turbulence, soot, sparks**, and a stagnation light that lights the actual hull mesh.
- **Colour** — two colour pickers per hull (Tint + Fringe) that rotate hue rather than
  multiplying channels, plus a real HSV saturation knob. Plus two more for the trail's head
  and tail.
- **It composites pre-bloom** into the client's fp16 chain, so white *emerges* from the
  bloom rather than being painted on.
- **Per-pixel depth clipping** in both views, so streaks cut exactly at the window frame
  instead of painting through the cockpit.
- **A separate effect for the cockpit.** Everything above is built from a point field
  sampled on the skin, which is right when you are looking *at* the ship and wrong when you
  are sitting inside it — from the seat most of that geometry is behind your head, and the
  view starves down to a handful of huge polygons. So the VC gets its own technique: a
  luminous sheath anchored to the relative wind, with filaments streaming past and abrupt
  flares. The flares light the cabin in the same frame they light the window, off one
  shared envelope. Two viewpoints, two techniques, neither compromised for the other.

21 live tuning sliders, saved per vessel class.

### The vapour cones

Prandtl–Glauert transonic condensation — the shroud that forms as the flow over the hull
expands through Mach 1, and the one famous aerodynamic visual nothing in Orbiter had.
**Two of them** since 2026-08-29, fully independent — one collar at the canopy and one at
the tail, the Concorde photograph — behind one pill, each cone's own opacity being its
visibility.

- **Its length is not a setting — it is the Mach angle.** `μ = asin(1/M)`: 90° at M 1, a
  flat collar standing across the flight path, tightening as the ship outruns its own
  pressure waves. Size x and y set the radii (equal = circular, a fact about a hull);
  Size z scales the *derived* reach rather than replacing it — 0 collapses the cone into
  the flat collar disc. So the shroud visibly *stretches back* as you accelerate, which
  is the whole effect.
- **Opacity can swallow the fuselage.** Up to 1 it is a translucent shroud; past 1 the
  sheet fills and densifies until the disc hides the hull behind it, the way the airshow
  photographs do. A **base fill** disc (per cone, its own pill) closes the wide end.
- **Slim streak filaments**, count on a slider (up to a few dozen), with a churn that
  makes them jitter, flare and die — colour and density only, never geometry, so the
  analytic surface that makes it read as a shock front survives.
- **Two colour picks per cone** — the vapour body and the streaks — so stained or
  sunset-lit vapour is one swatch away.
- **It is the one thing ORO draws that is not light.** Condensed water scatters and
  *occludes*, so unlike every other effect in the addon it draws alpha-blended and goes down
  *before* the additive layers — a cloud has to be laid down before light is added over it.
- **It rides the relative wind, not the hull axis and not the engines**, so a tail-sitter
  climbing on hover thrusters gets its cone around the axis it is actually travelling along,
  and a lifting body at 40° AoA gets one canted 40° off its nose — with no special case.
- **Limb thickening through Beer–Lambert**: edge-on you look through more water, so the rim
  reads dense and the middle stays translucent, saturating on its own instead of clipping.
- **A two-handle Mach band per cone** (0.5–1.5) sets where each lives — give the two cones
  different bands and the collars appear at different speeds, which is what really happens.
  The fade-in and fade-out are fixed fractions inside the window, so tightening it gives a
  sharp flash rather than a fade that never finishes. Plus a flicker-rate slider; opacity
  and size breathe on one number, because a stronger condensation event is denser and
  bigger at the same instant.
- **Full placement per cone** — Position x / y / z in Orbiter's own axis convention, plus
  bipolar Pitch and Yaw (±30°, snapping to zero = riding the relative wind exactly), for
  hulls whose shock stands off the centreline or at an angle. No roll — a surface of
  revolution has nothing to roll.

### The thruster system

- **Per-thruster tuning** — every exhaust and particle setting lives per engine group, and
  any INDIVIDUAL thruster can carry its own override block on top: cycle the selector to a
  thruster, move a slider, and that thruster owns its look from then on (a CLEAR button
  hands it back to the group). Sparse on disk — a class with no overrides is byte-identical
  to before. A **MARK** toggle draws a pulsing in-world ring at every nozzle so "thruster
  17 of 44" means something, and **CANCEL THRUST** follows the selection: each held engine
  is cancelled at its own position, force and torque together, so one RCS jet can be test-
  fired without the ship moving. Vessels in a stack (boosters, tanks) are resolved against
  their OWN class's saved tuning even while focus sits elsewhere.
- **Gimbal tracking, end to end** — the plume, particles, shimmer and throat fire read the
  live thruster direction every frame, so a gimballing engine's whole exhaust follows the
  nozzle with no authoring at all. For vessels that animate their real engine BELLS, the
  bell-glow mesh can opt in per group (a `GIMBAL` token): the glowing shell then rotates
  about its own derived throat pivot onto the live thrust direction and rides the moving
  bell as one piece of metal — axis, pivot and engine matching all derived from the
  geometry, nothing declared but the token itself.
- **Plume expansion** — pressure-driven, with real physics. One overexpansion number drives
  four curves: shock-cell spacing, diamond contrast, width pinch and separation flicker. The
  regime is framed by a two-handle **expansion band** you set per hull — drag the high handle
  low and you have a vacuum engine that shudders and pinches at sea level.
- **Soot** — sixteen lifecycled ablative streaks that shoot from the lip, flicker and fade,
  drawn *dark over* the jet because soot is in it.
- **Bell glow** — the nozzle heats and cools on sim time (`T_eq = throttle^¼`, closed-form
  cooling so it survives time warp) and glows incandescent: diffuse and specular forced
  black, emissive overdriven past the bloom threshold. The banding lives in the texture's
  alpha, so the dark streaks *are* the cold bell showing through. A colour pick rotates the
  whole blackbody ramp onto any hue while keeping its shape — dull at the bottom, still
  whitening at peak, because the white comes from the bloom rather than from the palette.
- **Throat fire** — camera-facing discs in the bell cup, depth-clipped by the bell walls.
- **Exhaust shimmer** — heat haze behind the plume, sharing one plume model with everything
  above so haze and jet can never disagree.
- **Exhaust particles** — the full `PARTICLESTREAMSPEC` exposed as live sliders in the API's
  own units, plus TWO colour pickers (each particle is randomly born with tint A or B — the
  atlas quadrants carry them; the API has no colour field, so ORO synthesizes the texture),
  a STOCK-colours switch, a **texture picker** (Orbiter's own Contrail textures or any 2×2
  atlas `.dds` dropped in `Textures\ORO\Particles`), emissive/diffuse lighting, an air-fade
  switch, and **COPY STOCK** — the vessel author's own stream definitions, read back through
  client patch (y), filtered to the selected engine group and offered as starting points.
- **Particle sun lighting** (client patch (x)) — stock D3D9 renders DIFFUSE particle streams
  fully lit at midnight; the ORO client darkens them on the night side, crosses the
  terminator per particle, keeps smoke flame-lit near the engine, and shades each billboard
  DIRECTIONALLY — the sun-facing side of a cloud bright, the far side smoky, per particle
  corner. Through dawn and dusk the sunlit smoke follows the SAME colour the hull takes
  (the client's own atmospheric extinction, read per particle at its own altitude), one
  stop ahead and bloomed, while engine-lit steam stays its own colour. Launchpad controls:
  the three-way mode (Off = bit-exact stock / Brightness only / Brightness + colour), a
  diffuse ground-shadow strength slider, and three dawn-tint dials (lead / depth / bloom).
  EMISSIVE streams untouched.

### Atmosphere and sky

- **Eclipses** — solar-disc obscuration at the camera against *every* body, as two
  overlapping angular discs, so penumbra ramps and annular eclipses fall out for free. Built
  as an **eye**, not a dimmer: asymmetric dark adaptation (~18 s opening up, ~1.2 s closing
  down, which is why emerging dazzles and entering merely gropes) and colour draining to the
  Purkinje grey. Because its gain converges to 1.0 it owns the transitions and never
  double-counts the renderer.
- **Auroras** — ribbon curtains around each *magnetic* pole, at **twelve worlds** (Earth,
  Venus, Mars, Jupiter, Saturn, Uranus, Neptune, Io, Europa, Ganymede, Titan, Triton).
  Three colours by altitude, because two cannot render Earth (violet nitrogen base, red
  oxygen top). Tilt knobs rotate the axis as a dipole, so Io's aurora can be equatorial —
  because Jupiter's field drives it, not its own. Thickness buys limb brightening for free.
- **Lightning** — storms read from **the planet's own cloud tiles**. ORO parses
  `Cloud.tree` directly and decodes DXT5 alpha for coverage, so flashes only happen where
  there is actually cloud. Deterministic storm districts in cloud-texture coordinates ride
  the rotating cloud layer by construction; the flash is the cloud image lighting up from a
  baked texture atlas, with multi-stroke envelopes and the occasional spider. Day kills the
  glow, which is physics, not a budget.
- **God rays** — crepuscular shafts from a low sun, and the cheapest effect in the addon
  because of an accident of frame order: D3D9Client draws its sun glare into the backbuffer
  *after* the bloom resolve and *before* the HUD stages ORO captures from. So the frame ORO
  already resamples contains a bright sun disc the client has **already occluded** against
  hull, terrain and limb. The light source and its shadowing both arrive in the pixels — no
  second pass, no depth read, no client patch. The atmosphere is a **gate, not a slider**:
  shafts are sunlight scattering off a medium, so the pass does not run in vacuum at all,
  and density and solar elevation are read from the sim rather than dialled in. Real rays
  cross the whole sky, so the falloff is linear and wide — and shafts need an *occluder*:
  with the sun in open sky the technique can only smear the disc into a halo. The eclipse
  takes the light with it, so a transit kills them for free.
- **Rain** — a summonable surface storm, and the largest single effect in the addon
  (client patch (s), seven parts). The **light collapses at the source**: the storm slider
  kills the directional sun and lifts the ambient, so shadows and the warm cast go with it
  instead of a post-process dimming the finished frame. Falling rain is a direction-aware
  **sheet in three parallax layers** (the honest per-drop field is arithmetically
  impossible — real rain runs to hundreds of drops per cubic metre); splash rings fizz in
  two world-anchored fields, one around the camera and one around the ship. The ground
  **soaks dark and then pools**: standing water pinned to the terrain itself (drive and
  the pools stay put), starting only once the ground is properly soaked, mirroring the
  grey sky at any angle, with a tight sun glint that the storm collapses and a broad
  cloud-glare that grows with it. **The ships reflect in the wet ground** — a real
  mirrored render of every vessel in range, concentrated in the pools, ripple-warped at a
  user-tuned amplitude and cadence, and carrying what comes out of the ships as well as
  the ships: nav lights and strobes, contrails and particle streams, and ORO's own
  engine plume, each drawn from under the water rather than copied off the picture.
  Hulls get wet too, across every vessel shader path:
  darkened, tightened specular, and a lifecycled **raindrop glint** riding the sky light.
  And since 08-27 the rain reaches the glass itself: **drops ON the VC windscreen** with
  real lens refraction, a gradual build-up, and runners that break loose and carve fading
  trails — any vessel opts in with a one-line `RAIN 1` token in its mesh (patch (h)),
  and the window frame masks the drops per pixel for free.
  The deck overhead is ORO's own **two-layer textured cloud ceiling** — a main deck and
  a darker scud layer hanging beneath it, with real parallax, vertical relief and no
  repetition — and the storm carries its own **lightning**: most events light a region
  of the deck from within, a share become **bolts to the ground** drawn from a baked atlas
  of sixteen real channels (re-strikes flicker down the *identical* channel; some hang
  with continuing current; a rare positive giant strikes far out, single and brilliant),
  and every flash blinks a real light across the ship and the wet ground — sky, bolt and
  scene agree because they are one scheduled event. Visibility closes down, and
  everything — including the wet ground and the glint — fades out above the weather, so
  a reentry begun with the rain still on gets a clean dry hull in space. Earth only for
  now; per-body later, beside the aurora's settings.
- **Rain sound** (2026-08-23) — the storm is audible: three seamless generated rain
  loops (`tools/raingen.py` — exactly periodic by construction, the integer-cycle law as
  audio) crossfading with the envelope, patter carrying the build-up and the downpour
  owning the full storm. Finding the play path exposed a real **XRSound engine bug**:
  module sounds never receive per-timestep state updates, so a playing module sound
  keeps its first volume forever — ORO works around it by splicing (stop, restart at
  the new volume, seek back), pushed on change.
- **Thunder** (2026-08-23) — every flash event is *heard*, delayed by its own distance
  at the speed of sound: six to twenty-six seconds after the light. Nine real storm
  recordings from freesound (CC0/CC-BY, credited in the shipped ledger), leveled into a
  close-crack / mid-boom / far-rumble hierarchy by `tools/thunderprep.py`. Close bolts
  crack, in-cloud flashes only rumble (their channel is buried in the deck), the rare
  positive giant hits hardest — and the ear is the **camera**: the STRIKE test button's
  crack follows at the view's own distance from the bolt.
- **The storm from the cockpit** (2026-08-23) — in a virtual cockpit the rain, splashes,
  deck and bolts draw through the windows, cut per pixel at the frame by the depth clip;
  the cabin stays dry (the client keeps wet sheen and drop glint off the interior in the
  cockpit pass), the storm sound arrives muffled through the hull, and a fourth generated
  loop — raindrops **drumming on the skin** — takes its place. Vessels with real
  interiors seal them with an authored **rain shield**: `Meshes\ORO\<class>_rainshield.msh`,
  every triangle a roof panel — rain is removed wherever a panel sits within 3.5 m
  directly overhead. The DeltaGlider and DG-S ship with theirs; authoring one is a
  single quad.

---

## 3. Things ORO controls but does not draw

A separate category, and a useful one — these hand knobs to the patched client or the core
rather than rendering anything.

| | |
|---|---|
| **VC shadows** | Sunlight falls through the canopy and sweeps the cabin as the ship rotates. ORO drives the client's own shadow pass. Cabin box per class; **shadow depth** lets the shadow take the ambient share with it — but never the emissive, so a lit MFD doesn't dim when a frame passes over it. |
| **Stock reentry kill** | Stock's billboards *and* every vessel's default reentry particle stream, suppressed — neither of which any documented API can turn off. |
| **Stock exhaust kill** | Billboards and exhaust streams, as two independent bits, per vessel. |
| **Night clouds** | Three stock behaviours conspired to make night cloud decks invisible from above and city lights punch through anything. Fixed in the deployed shader; tunable at runtime. |

## 4. Test rigs

| | |
|---|---|
| **Flight aid** | Shifts a vessel's centre of pressure live, so stock ships can hold a high AoA and actually make plasma worth looking at. Done as a pure couple — two equal and opposite forces — because `EditAirfoil` needs a handle only the vessel itself ever receives. Self-scales with dynamic pressure on any vessel, no per-hull tuning. Live pitch-moment readout in kN·m. |
| **Cancel thrust** | Negates the vessel's own thrust at the CoM. Deliberately session-only — a persisted thrust-cancel loaded into a launch scenario reads as "my engines are dead". |

## 5. The parts you don't see

- **The control panel** — 525-wide, height-resizable, fully owner-drawn, dark, nothing
  like a stock Orbiter dialog. Since 2026-08-29 it is a **menu tree**: a main menu of
  three categories (WORLD / VESSEL / PILOT), submenus beneath, and leaf pages holding
  exactly one subject's controls — a breadcrumb plus BACK / BACK TO MAIN in a fixed nav
  row, so nothing is ever more than two clicks from anywhere. Every page scrolls its own
  content; each leaf's SAVE/REVERT row is *fixed*, so the way to save never scrolls away;
  the SAVE buttons turn **amber** while unsaved edits exist in the files they write.
  Master arm, HELP and the global SAVE are pinned above it all. Ctrl+G is a keyboard
  panic that kills everything and hands every borrowed thing back.
- **In-panel help, per page** — pressing HELP opens the text for exactly the screen you
  are on, menus included (a menu's help describes its doors; a page's help explains every
  slider, pill and swatch on it), and an open help window *follows* you as you navigate.
- **A custom in-panel colour picker** — because the Windows one froze the sim and then
  slammed one giant timestep on close, throwing landed vessels across the map.
- **Three settings scopes** — global (what the pilot *is*), per vessel class (what a hull
  needs), per body (what a world's aurora *is*). Class files load automatically when focus
  changes class; body files when the aurora's target world changes.
- **A borrow-and-return discipline** — every light emitter, particle stream, mesh and
  suppression flag ORO takes from another vessel is handed back on *every* exit path,
  including vessel deletion mid-frame.
- **Docked, the felt-G model switches to kinematics** — a docking latch reports the force
  it uses to hold two ships together, and in a scenario that *starts* docked that force can
  be large and permanent while nothing moves (a DeltaGlider parked at the ISS reads 1.5 G
  of nothing). So while docked the model works the answer out from the motion of the whole
  joined assembly instead. Your own engines are still felt, and so is **rotation** — dock
  to a spinning station and you get real artificial gravity, measured from your distance to
  the assembly's centre of mass, which is the ring's radius.
- **Nozzles synthesised for engines that have none** — some engines are built with no
  visible exhaust at all (the DeltaGlider-S scramjets), so they show particles and no jet.
  ORO gives them one, sized from the hull rather than from thrust: an airbreather's rated
  thrust changes with the air around it, so a thrust-sized nozzle would swell and shrink
  as you fly.
- **Crash forensics** — both abort paths hooked, each logging a stack walk resolved to
  module and offset, with a linker map so an offset resolves to a symbol.

## 6. The client work

Stock D3D9Client crashes the instant any HUD render proc is registered. That was patch (a);
nineteen more followed. Several are outright bug fixes to the client, demonstrable with no
addon involved:

- `clbkCreateParticleStream` is unimplemented — so the documented core API
  `VESSEL::AddParticleStream` silently does nothing. *The stock DeltaGlider itself calls it.*
- `SetReentryTexture(NULL)`, the `bReentryFlames` Launchpad option, and every vessel's
  default reentry stream are all silently unsuppressable.
- Self-shadowing treats a half-transparent, untextured `cockpitglass` as fully opaque —
  visible in exterior views on the stock DeltaGlider.
- Planet shine has **no occlusion term at all** — Earth glow lights the inside of a
  closed payload bay (community-reported).
- The sun self-shadow map is bound in the **main scene only** — every reflection probe
  and secondary pass renders geometry fully sunlit.
- The config file died on every Launchpad close.

The rest add capability: backbuffer access, additive Sketchpad blend, per-pixel depth
clipping, textured Sketchpad triangles, CPU→texture upload, render-epoch camera and body
anchors, a pre-resolve render slot, VC shadows, surface weather — wet ground, storm
light, and a planar mirror that puts the ships in the puddles — scene depth handed to
full-frame shaders (the windscreen drops stand on it), and **real reflections**: multi-
probe environment maps with box projection, planar vessel mirrors with a curvature
warp, and planet-shine occlusion, all behind a fourth Launchpad reflection mode,
**"Full Scene ORO (exp)"** — the three stock settings stay pixel-identical stock.

And one that fixes Orbiter's own UI rather than adding anything: **patch (t) makes the menu
bar and info bars draw LAST**. The core paints the pilot's instruments and the user's chrome
in a single call, leaving an addon overlay no slot between them — so any full-frame effect
smeared Orbiter's own menus along with the world. The bars are now held back and replayed
over the top, identified by the one texture every one of them is drawn from.

Full patch text and rebuild recipe: `upstream/BUILDING.md`.

## 7. Shipping

A closed-beta installer, not a drop-in layout: `ORO_Install.bat` / `ORO_Uninstall.bat`,
plain batch on purpose — an unsigned .exe from a forum friend that replaces your graphics
client is exactly what people are trained not to run, and a batch file they can read in
Notepad is more trustworthy. It verifies Orbiter 2024 by binary timestamp, refuses to run
twice, backs up the tester's own files first, and on uninstall **deletes a file only if it
is byte-identical to what shipped** — so an evening of plasma tuning survives. Ships with
pristine stock shaders for restoring, a ~430-line readme covering every control, and four
scenarios.

---

## What isn't done

Honest list.

- **Reentry is still hand-tuned.** The knobs are found values, not driven ones. Making them
  physics-driven — you set bounds, the sim sets values — is the next big step.
- **Gas-giant aurora scales are estimates**, derived from real physics but never checked
  against the limb in-sim. Earth is the only one tuned by eye.
- **`gcAPIVer` reads 0** — a diagnostic-only bug now that every capability probes by binding,
  and the root cause IS now known: `BuildDate()` does its only work inside an `assert()`,
  so in a Release build the parse never runs and it returns 0. One line to fix in the client.
- **One unexplained abort** when the bell-glow pill was pressed, never reproduced. It's now
  instrumented; if it recurs, the log names it.
