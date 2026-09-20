# ORO — what it does

*An immersion experience for Orbiter 2024. Written 2026-08-10, the day the closed beta shipped.*

This is the first time the whole thing has been listed in one place. Everything below is
**built, flying, and tested in the sim** unless a line says otherwise.

| | |
|---|---|
| Started | 2026-07-25 |
| Shipped to beta | 2026-08-10 (16 days) |
| Distinct effects | **33** — 13 physiological, 20 environmental (the SNOW joined 2026-09-14; the LENS FLARE and the RINGS both joined 2026-09-12; the FOG joined the weather; the windscreen drops joined the storm; the vapour cone is TWO independent cones) |
| Live controls | **211** sliders/knobs (incl. the ten snow round-3 controls, 2026-09-20, and the two cabin/drum LOOP pickers, 2026-09-18), **18** colour pickers, organised as a **menu tree** (WORLD / VESSEL / PILOT → 7 menus, 15 pages), all thruster settings PER ENGINE GROUP (MAIN / HOVER / RETRO / USER / **RCS**) — or **PER INDIVIDUAL THRUSTER** via layered overrides |
| Source | ~34300 lines across **19** .cpp and 7 headers, plus 9 pixel shaders in one HLSL file (the old line said "18 source files" for a count that included the headers - it is spelled out now) |
| Client patches | **47** (a–z, +k2, +z2, +z3, +aa–ar) — every one of them load-bearing |
| Test rigs shipped | **1** — `Script\testlights.lua`, twelve automatic cases over the local-light and shadow work, with its own scenario |
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

- **Two heat curves, chosen per vessel class** (2026-09-15, from a forum report that the
  plasma arrived minutes late on a Shuttle-class entry). CLASSIC measures that heating rate
  against two fixed thresholds and is what every vessel tuned before it flies — it is the
  default, and a hull asks for anything else only in its own cfg, so switching one vessel
  cannot move another's look. **PHYSICAL** reads the same rate as the *temperature* it holds
  the stagnation region at, with the two ends set in **kelvin**. The reason is a range
  problem rather than taste: the heating rate spans sixty times between the first visible
  glow and peak heating, and a linear ramp cannot render both ends of that — a temperature
  is its fourth root, so the same span becomes less than three. Its default onset, 800 K, is
  the **Draper point** where solids first glow red, and on a Shuttle entry the model crosses
  it 25 s after entry interface, which is when crews report the first faint glow at the nose.
  A hypersonic gate ends it below ~Mach 4 rather than letting it linger into the approach.

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
  photographs do. A **base fill** disc (per cone, its own pill) closes the wide end, and a
  **base fill offset** slides its centre along the axis — from pushed fully in (a second,
  inner face of the cone) through the flat disc to bulged outward, the rim staying joined
  to the cone throughout.
- **Slim streak filaments**, count on a slider (up to a few dozen), with a churn that
  makes them jitter, flare and die — colour and density only, never geometry, so the
  analytic surface that makes it read as a shock front survives.
- **Two colour picks per cone** — the vapour body and the streaks — so stained or
  sunset-lit vapour is one swatch away.
- **It forms by chance, and the air decides** (2026-09-07). Each pass through the Mach
  band rolls the dice once: the odds are Max chance × water (the planet's own `Mask.tree`
  water map around the vessel — open sea 1.0, deep inland 0.6, no map at all means no
  cone) × how far below a dry ceiling you are (15 km, drawn ±10% per pass) × the dynamic
  pressure's expansion term. The cone exists while the draw beats the live chance, so it
  thins climbing toward its ceiling and forms on descent where the air gets humid enough;
  an **Intermittency** gate makes it appear and die in bursts, more so on a marginal draw.
  Three global sliders and an Air readout; the shapes stay per class.
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
- **Sculptable shock diamonds** — count 0–12 (0 = a clean jet), brightness, spacing, and
  three shape controls: a bipolar **shape** knob sliding each cell's bulge from the RS-25's
  half-diamond-at-the-lip (base toward the bell) through the classic symmetric diamond to a
  downstream expansion fan, **length/width** per axis (short length = a pure Mach-disc
  train), and a **train offset** in metres. A dedicated **diamond colour swatch** renders
  exactly the colour you pick. Successive cells decay — brightest at the nozzle — and under
  throttle the train grows and shrinks by **whole diamonds in sequence**, each fading in
  complete: a shock pattern is coherent or gone, never a fraction of a cell.
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
  above so haze and jet can never disagree. Full wave control: **amplitude, wavelength and
  frequency** sliders plus a per-jet offset (−2..+10 m along each jet's *own* flow
  direction). Each engine group — or overridden thruster — hazes at its **own strength** in
  the same frame, and the whole effect answers to **air density** automatically: full low
  down, thinning through a climb, gone in vacuum, and up to twice Earth strength in an
  atmosphere as dense as Venus's.
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
- **Planetary rings** — the ring stops being a card and becomes a sheet with a real
  **optical depth**. Opacity is `1 − exp(−τ/μ)`, so it goes opaque as the view flattens
  toward the ring plane — which is the only honest kind of "thickness" available, the main
  rings being 10–30 m thick. The lit and unlit faces are different phenomena rather than one
  scaled by a constant: reflection one way, *transmission* the other, so backlit the thick B
  ring goes dark while the Cassini Division glows — the inversion in every Cassini frame.
  **The ring casts its shadow on the planet**, which Orbiter had never drawn at all, carrying
  the ring's own structure so the Division reads as a bright line inside the band, and
  broadening and narrowing with the 26.7° obliquity for free. Anything inside that shadow —
  a ship, and the sun glare seen through the sheet — is dimmed by exactly what the ring
  transmits. **Every ringed planet qualifies, addon systems included, with no new files**:
  the optical depth is read out of the ring texture the planet already ships, because the
  legacy `.tex` format's alpha channel has always been real opacity and nothing had used it.
  **And the close-up (round 2):** bring the camera toward the ring and the sheet grows its
  own texture — grooves and grain, ten and eight octaves from 2 km down to 4 m, each admitted
  by a per-pixel fade once it is big enough on screen, so the far ring is round 1 to the bit
  — standing still for a co-orbiting eye because its anchor rides Orbiter's own J2 field at
  the camera's radius. `Detail` sets how fine, `Contrast` how strong, and `Relief` reads the
  density as height and lights it, so every ridge has a lit and a shade side under Saturn's
  grazing sun. The underside of a dense ring is grey rather than black (the light a thick
  slab diffuses through itself, and planetshine), and ships cast their shadows onto the
  sheet in Cascaded mode. Underneath it, a client-side *near-field draw*: the client renders
  every planet distance-scaled on a near plane that cut the sheet 26 km out, so the sheet is
  drawn again after the hulls on a local disc with exact coordinates and crossfaded into the
  far draw at 600 km — the only way metre-scale structure can be placed to the millimetre on
  a 137,000 km mesh.
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
- **Lens flare** — ghosts, an iris starburst, an anamorphic streak and a veil, from the
  sun. **External views only, and that is physics rather than a scope cut**: a flare is
  made between the elements of a *lens* and a healthy eye has none, so through the pilot's
  eyes there is nothing to see and through a camera there is. It carries **no brightness
  rule of its own** — it reads the sun's own pixels out of the finished frame and measures
  their *contrast* against the ring of sky around them, so it dies behind a hull, dims
  through haze and fades in an eclipse without being told about any of them, and a bright
  hazy sky makes it weaker rather than stronger. (Brightness alone would have got that
  backwards, which is the lesson the 2026-09-01 sun-disc round left behind: nothing in the
  sun pipeline knows about clouds.) Its atmosphere term is the **god rays' gate inverted** —
  a shaft needs a medium to scatter in, a flare needs a concentrated source that air is
  busy smearing across the sky — so the two hand over to each other across an ascent.
  **Four optics** (CLASSIC / ANAMORPHIC / CLEAN / VINTAGE), each a different element stack,
  coating and aperture, with all six sliders keeping their meaning across every one: it is
  one effect with a lens to choose, not four effects. The ghost tables are *measured* off
  reference photographs rather than invented, and VINTAGE's near-colourless ghosts are the
  physics falling out — a ghost's colour is thin-film interference in the coating, so an
  uncoated lens has almost nothing for the Dispersion slider to spread.
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
  trails — and the window frame masks the drops per pixel for free. Declaring the glass
  needs NO mesh editing: the **RAINSURFACES picker** borrows the Debug dialog's mesh
  pick — press ADD, click your windscreen from the VC (the group holds a green
  highlight while the button is down), SAVE, and the client re-applies the shared
  `VesselsRainSurfaces.cfg` to the LIVE meshes so the drops respond without a reload.
  The shipped cfg already declares the stock DeltaGlider's windscreen; authors can
  still mark glass with a one-line `RAIN 1` mesh token (patch (h)) — both routes
  work side by side. **The glass answers to the airflow, not to a setting**: the
  runners radiate from the *stagnation point* — where the oncoming air first meets
  the airframe, which is below the glass — so they run straight down when parked and
  sweep **up** the windscreen and outward from about rotation speed onward, the way
  they do on a real canopy, on any vessel and with nothing to configure. Yaw or pitch
  moves the radiant by about that angle and no more — its direction is the airflow's,
  only its depression below the nose is the airframe's — and the runners are spread
  evenly over the pane, a runner being born where a drop lands. Past roughly
  45 m/s of **dynamic pressure** (so it stays honest at altitude, where 200 m/s in
  thin air barely disturbs a drop) the sitting drops thin out, the runners multiply to
  carry the water away, and what is left is a moving **film** that ripples the view
  rather than drawing anything of its own — one sensed number driving all three, so
  they cannot disagree about how hard the air is working.
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
- **Snow** (2026-09-14) — the rain's sheet re-particled, plus a cover that lies on the
  world. The two storms are **mutually exclusive** by design, which is what lets the snow
  have the whole geometry budget: switching one on switches the other off. Falling snow is
  the rain machinery in a second mode — near flakes drawn as soft out-of-focus discs, mid
  ones as dots, far ones as specks, each with its own hashed size, coloured as *the air*
  rather than a flat white, with a gusting **Wind** slider that streams them past a parked
  ship. The **cover** wakes client patch (aa)'s dormant `SetSnowCover`: terrain, base tiles
  and all five vessel paths lerp toward white through one helper, filling in from the
  hollows by a thresholded lattice instead of fading up uniformly. It lies on up-facing
  surfaces, never on water, and hulls shed it as dynamic pressure builds, so a parked ship
  whitens and a moving one does not. It accumulates on **simulation** time while the fall's
  envelope runs on real time — the invariant-1 split — and a **snow line** by altitude
  puts rain country below and white above. The mist runs on a **log scale**, about 1.3 km
  of visibility at 0.5 down to 100 m at 2, and in snow it takes the cloud deck with it, so
  a blizzard has no visible sky. No colour picker anywhere in it: the deck and the flakes
  take the sun's own warmth at dusk. **Round 3** (2026-09-19/20, client patch aq): a hull
  **remembers** its cover — the client keeps a per-vessel state that the airflow strips and
  only falling snow lays back, at the ground's own pace, so what the take-off roll blew off
  does not return to a parked ship until it snows again; one **Brightness** slider in two
  calibrated lanes (ground and hulls read alike); **Relief** — the lattice read as a height
  field and shaded by the sun, a lit and a shade side per drift and a lip at every patch's
  rim, centimetres on a hull and metres on the ground; **Sparkle** — a count of point
  crystals that flash as the camera moves; the share and the slope gate move the lattice's
  threshold rather than its alpha, so a hull sheds crests-first and a full cover climbs its
  steep sides; **Contrast** and **Wind from** (degrees) join the sheet, which draws some
  thirty thousand flakes across three polys at a whiteout and shows them at night. And **frost
  on the virtual cockpit's windows** (its own pill): ice grows in from each pane's own frame
  while snow falls, by a reach in metres, and melts when it stops — client patch (h) part 5
  hands the shader every declared pane's distance to its frame per pixel, from a subdivided
  copy the client draws into its depth buffer, so it fits any pane on any vessel with nothing
  to tune. The glass controls and the base lights block sit on the SNOW page too.
  **Tire marks** (2026-09-20, client patch ar) — every moving vessel's gear marks the ground
  snow it rolls over, through a one-kilometre track map the client keeps round the camera: a
  toroidal render target anchored in the planet's frame, stamped as capsules from the three
  gear points, read by the terrain, the base tiles and the plain-textured pavement as a
  lowering of the lattice's threshold (bare ground at a full mark, the hollows alone in
  between, the relief's lip for free), faded on simulation time over **Track fade** minutes
  and refilled by falling snow. **Blow-off** — snow leaves a hull as the airflow strips it:
  ORO mirrors the client's cover law and sheds the difference as world particles in the
  planet's frame, clumps and tumbling chunks drawn as the sheet's soft fans elongated into
  their motion blur; still being tuned at this build (a hard-edged residue near the hull at
  speed is open).
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

- **The fog** (2026-09-05, client patch aa) — a ground fog you summon on the FOG page,
  rendered by the client inside every surface so a ship, its apron and the horizon fade
  into ONE grey; coloured by the sun's irradiance (warm at dawn, dark at night, grey
  under a storm), the sun and every shadow weakening through it, the runway lights
  wearing a halo, the cockpit staying clear. The rain's gloom became a real layer of
  mist the same day. **And the rain is lit** — streaks and splashes take their light
  from the sky, your own lamps and the flashes, so at night with nothing on they all
  but vanish.

## 3. Things ORO controls but does not draw

A separate category, and a useful one — these hand knobs to the patched client or the core
rather than rendering anything.

| | |
|---|---|
| **VC shadows** | Sunlight falls through the canopy and sweeps the cabin as the ship rotates. ORO drives the client's own shadow pass. Cabin box per class; **shadow depth** lets the shadow take the ambient share with it — but never the emissive, so a lit MFD doesn't dim when a frame passes over it. |
| **The cabin at night** | A stock VC stays fully lit at midnight — its authors fill it with flat emissive light. ORO scales that fill (and the ambient) down as the sun sets at the camera, the horizon dipping for orbit and a twilight band on worlds with air; storms and fog dim it further by a slider of your own. MFDs, self-lit instruments, emission maps and every cockpit lamp are untouched, so the cabin light finally has a job. |
| **Fog** | Two analytic height-fog layers rendered by the client inside every surface — terrain, bases, hulls, particles, runway lights, the sky — one grey for all of them, coloured by the sun's own irradiance. Visibility, top, fade, brightness, sun glow; the rain's gloom is the second layer. Ground shadows, VC shadows and the sun all weaken through it. |
| **Base lights** | A three-state button: STOCK (Orbiter's own flip, on at night, off by day), IN WEATHER (every base's night state — night textures, runway and taxiway lights — switched on when the storm light has taken more than a quarter of the sun or the fog's visibility is under 5 km, and back to stock when it clears; a flip with hysteresis, read off the very numbers the client is rendering) and ALWAYS (forced on now). With a glow gain into the bloom and a fog halo that grows with the air between you and the lamp, applied while the lights are ORO's. One setting behind two doors (RAIN and FOG). |
| **Base trains and solar plants** | No knob at all — the client revives the three base objects that only Orbiter 2010's own renderer ever animated. The monorail cabin shuttles between its ends, the hangrail's two cabins pass each other under their girder rail, and a solar plant's panels track the sun and glint at you; all frozen or invisible under every graphics client since terrain arrived. The rails follow the terrain under an 8% grade on pylons and portals cut to the ground; everything casts and receives the cascaded shadows and takes local lights, fog and night textures. Habana's monorail and hangrail are the stock examples. |
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
thirty-five more followed. Several are outright bug fixes to the client, demonstrable with no
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
- Base rendering is **depth-blind** — runways, base tiles and runway lights all draw
  straight through terrain (a flat-planet fossil that elevation data made false), and
  the sun's glare paints over any mountain the sun is behind (its visibility kernel
  reads a depth buffer that holds vessels only). Fixed with real depth plus a
  camera-ward bias for the coplanar contest, and a terrain-marched sun-visibility
  test; base MESH objects also gain the classic `_n` night-texture pairing that stock
  only ever wired for HANGAR/TANK/LPAD blocks. Base structures also join the
  screen-space depth buffer, so glare sprites and addon geometry stop drawing
  through buildings.
- Local lights have **no shadows at all** — a spotlight beam passes straight through
  a hangar, and a vessel standing in the beam casts nothing. The patched client
  renders up to six shadow maps a frame from the strongest spot lights (lamps mounted
  together share one), an aimed map or a five-face cube for a point light, and for a
  launch pad's hemisphere floods a cube when the cells are spare or a map capped at the
  cube's face angle when they are not (buildings, vessels and terrain ridges all cast;
  terrain and every vessel shader receive, day and night, in every Terrain shadows mode).
- **Terrain flattening (`.flt` files) silently did nothing under cubic elevation
  interpolation** — the core's default. Stock flattened only a float copy of each
  elevation tile while cubic mode's file-less children interpolated from the raw array,
  so the vessel stood on the flattened height while the drawn ground kept its hills.
  Both copies are flattened now; the mesh and the physics derive from the same rounding.
- The config file died on every Launchpad close.

The rest add capability: backbuffer access, additive Sketchpad blend, per-pixel depth
clipping, textured Sketchpad triangles, CPU→texture upload, render-epoch camera and body
anchors, a pre-resolve render slot, VC shadows, surface weather — wet ground, storm
light, and a planar mirror that puts the ships in the puddles — scene depth handed to
full-frame shaders (the windscreen drops stand on it), and **real reflections**: multi-
probe environment maps with box projection, planar vessel mirrors with a curvature
warp, and planet-shine occlusion, all behind a fourth Launchpad reflection mode,
**"Full Scene ORO (exp)"** — the three stock settings stay pixel-identical stock. Then the
weather's air: two fog layers in every shader family, terrain in the depth buffer so
stencil ground shadows hide behind hills, forced base lights with a fog halo, and the
cabin going dark at night. And then **cascaded shadows** — a fourth terrain-shadow mode,
**"Cascaded (ORO)"**: one camera-fitted sun-shadow atlas (five cascades to a 5–60 km
reach, a vessel-anchored near slot, and hull boxes for the three nearest vessels, every
lattice snapped in the planet's own frame so nothing shimmers as the world turns) into
which every caster draws — vessels, base structures, terrain tiles — and from which
terrain and every vessel shader sample. Buildings shadow the ground, hills shadow
buildings, hulls shadow each other and their pads; the spotlight map rides in the same
atlas, which is what lets a lamp respect a wall in daylight. The stencil sheets and the
per-tile maps are gone in that mode; the stock modes are untouched.

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

- **Cascaded shadows have three known edges**: reflection probes and planar mirrors do
  not sample the atlas (they keep the per-vessel map), at most two overlapping hull boxes
  shade one hull pixel, and the far cascades re-render every frame (no cadence yet).
- **The rain is hard to see at night** away from a lamp, and **the lightning flash does
  not light the cockpit or cast shadows** — his list for the next release.
- **Reentry is still hand-tuned.** The knobs are found values, not driven ones. Making them
  physics-driven — you set bounds, the sim sets values — is the next big step.
- **Gas-giant aurora scales are estimates**, derived from real physics but never checked
  against the limb in-sim. Earth is the only one tuned by eye.
- **`gcAPIVer` reads 0** — a diagnostic-only bug now that every capability probes by binding,
  and the root cause IS now known: `BuildDate()` does its only work inside an `assert()`,
  so in a Release build the parse never runs and it returns 0. One line to fix in the client.
- **One unexplained abort** when the bell-glow pill was pressed, never reproduced. It's now
  instrumented; if it recurs, the log names it.
