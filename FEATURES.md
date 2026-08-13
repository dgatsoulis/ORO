# ORO — what it does

*An immersion experience for Orbiter 2024. Written 2026-08-10, the day the closed beta shipped.*

This is the first time the whole thing has been listed in one place. Everything below is
**built, flying, and tested in the sim** unless a line says otherwise.

| | |
|---|---|
| Started | 2026-07-25 |
| Shipped to beta | 2026-08-10 (16 days) |
| Distinct effects | **25** — 13 physiological, 12 environmental |
| Live controls | **84** sliders/knobs, **11** colour pickers, 5 tabs + 2 sub-tabs |
| Source | ~15,400 lines across 15 C++ files, plus 8 pixel shaders in one HLSL file |
| Client patches | **17** (a–g, i–q) — every one of them load-bearing |
| Worlds with auroras | 12 |
| Settings scopes | 3 — global / per vessel class / per body |

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

19 live tuning sliders, saved per vessel class.

### The vapour cone

Prandtl–Glauert transonic condensation — the shroud that forms as the flow over the hull
expands through Mach 1, and the one famous aerodynamic visual nothing in Orbiter had.

- **Its length is not a setting — it is the Mach angle.** `μ = asin(1/M)`: 90° at M 1, a
  flat collar standing across the flight path, tightening as the ship outruns its own
  pressure waves. You set the outer radius (a fact about a hull); the axial reach falls out.
  So the shroud visibly *stretches back* as you accelerate, which is the whole effect.
- **It is the one thing ORO draws that is not light.** Condensed water scatters and
  *occludes*, so unlike every other effect in the addon it draws alpha-blended and goes down
  *before* the additive layers — a cloud has to be laid down before light is added over it.
- **It rides the relative wind, not the hull axis and not the engines**, so a tail-sitter
  climbing on hover thrusters gets its cone around the axis it is actually travelling along,
  and a lifting body at 40° AoA gets one canted 40° off its nose — with no special case.
- **Limb thickening through Beer–Lambert**: edge-on you look through more water, so the rim
  reads dense and the middle stays translucent, saturating on its own instead of clipping.
- **A two-handle Mach band** (0.5–1.5) sets where it lives, with the fade-in and fade-out as
  fixed fractions inside the window so tightening it gives a sharp flash rather than a
  fade that never finishes. Plus a flicker-rate slider; opacity and size breathe on one
  number, because a stronger condensation event is denser and bigger at the same instant.

### The thruster system

- **Plume expansion** — pressure-driven, with real physics. One overexpansion number drives
  four curves: shock-cell spacing, diamond contrast, width pinch and separation flicker. The
  regime is framed by a two-handle **expansion band** you set per hull — drag the high handle
  low and you have a vacuum engine that shudders and pinches at sea level.
- **Soot** — sixteen lifecycled ablative streaks that shoot from the lip, flicker and fade,
  drawn *dark over* the jet because soot is in it.
- **Bell glow** — the nozzle heats and cools on sim time (`T_eq = throttle^¼`, closed-form
  cooling so it survives time warp) and glows incandescent: diffuse and specular forced
  black, emissive overdriven past the bloom threshold. The banding lives in the texture's
  alpha, so the dark streaks *are* the cold bell showing through.
- **Throat fire** — camera-facing discs in the bell cup, depth-clipped by the bell walls.
- **Exhaust shimmer** — heat haze behind the plume, sharing one plume model with everything
  above so haze and jet can never disagree.
- **Exhaust particles** — the full `PARTICLESTREAMSPEC` exposed as live sliders in the API's
  own units, plus a colour picker (the API has no colour field, so ORO synthesizes the
  texture), emissive/diffuse lighting and an air-fade switch.

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

- **The control panel** — 500×800, fully owner-drawn, dark, nothing like a stock Orbiter
  dialog. Five tabs, each scrolling its own content; the thruster tab has two sub-tabs.
  Hand-drawn scrollbar, live 10 Hz repaint, master arm and SAVE pinned outside the scroll
  pane so they're always reachable. Ctrl+G is a keyboard panic that kills everything and
  hands every borrowed thing back.
- **A custom in-panel colour picker** — because the Windows one froze the sim and then
  slammed one giant timestep on close, throwing landed vessels across the map.
- **Three settings scopes** — global (what the pilot *is*), per vessel class (what a hull
  needs), per body (what a world's aurora *is*). Class files load automatically when focus
  changes class; body files when the aurora's target world changes.
- **A borrow-and-return discipline** — every light emitter, particle stream, mesh and
  suppression flag ORO takes from another vessel is handed back on *every* exit path,
  including vessel deletion mid-frame.
- **Crash forensics** — both abort paths hooked, each logging a stack walk resolved to
  module and offset, with a linker map so an offset resolves to a symbol.

## 6. The client work

Stock D3D9Client crashes the instant any HUD render proc is registered. That was patch (a);
fifteen more followed. Several are outright bug fixes to the client, demonstrable with no
addon involved:

- `clbkCreateParticleStream` is unimplemented — so the documented core API
  `VESSEL::AddParticleStream` silently does nothing. *The stock DeltaGlider itself calls it.*
- `SetReentryTexture(NULL)`, the `bReentryFlames` Launchpad option, and every vessel's
  default reentry stream are all silently unsuppressable.
- Self-shadowing treats a half-transparent, untextured `cockpitglass` as fully opaque —
  visible in exterior views on the stock DeltaGlider.
- The config file died on every Launchpad close.

The rest add capability: backbuffer access, additive Sketchpad blend, per-pixel depth
clipping, textured Sketchpad triangles, CPU→texture upload, render-epoch camera and body
anchors, a pre-resolve render slot, and VC shadows.

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
- **The aurora doesn't light the hull yet** — a small, self-contained job.
- **Gas-giant aurora scales are estimates**, derived from real physics but never checked
  against the limb in-sim. Earth is the only one tuned by eye.
- **Reentry has no sound at all.**
- **`gcAPIVer` reads 0** — a diagnostic-only bug now that every capability probes by binding,
  but the root cause was never found.
- **One unexplained abort** when the bell-glow pill was pressed, never reproduced. It's now
  instrumented; if it recurs, the log names it.
