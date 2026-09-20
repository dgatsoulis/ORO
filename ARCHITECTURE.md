# ORO — architecture

A map of the code, for anyone reading it, forking it or extending it.
`README.md` says what ORO is and how to install it; `FEATURES.md` lists what it does;
this file explains how it is put together and — more usefully — **what will break if you
change the wrong thing.**

Most of what follows was bought with a failed build and a test flight. Where a rule looks
arbitrary, it usually is not.

---

## 1. What ORO is, structurally

A 32-bit Orbiter 2024 **global module** (`oapi::Module`) that draws through the
D3D9Client's `gcCore` interface. It is not a vessel, it owns no scenario state, and it
adds nothing to the simulation — it reads the world and draws over the frame.

It draws in exactly three ways:

| | |
|---|---|
| **Sketchpad triangle lists** | Screen-space geometry, projected on the CPU. The plasma, the aurora, lightning, rain, the plume, the rings' own geometry. Drawn additively (or alpha-blended for the vapour cone), optionally clipped per pixel against the scene depth. |
| **IPI pixel shaders** | Full-frame resamples: `Modules\ORO\orofx.hlsl`, one entry point per effect. Capture the backbuffer, run a pixel shader, write it back. Blur, grey-out, aberration, swim, tilt, shimmer, plasma wash, eclipse, god rays, lens flare, rain. |
| **Nothing at all** | A large and growing category: ORO sets a value and the *client* does the drawing. VC shadows, the wet ground, fog, base lights, the rings' shadow, the cascade shadows. See §6. |

Everything else in the addon exists to decide *what* to draw and *where*.

---

## 2. The hard dependency: a patched D3D9Client

**ORO does not work on a stock client.** Stock Orbiter 2024's D3D9Client crashes the
instant any HUD render proc is registered, which is the first thing ORO does. Forty-five
patches are applied to the client; the patch set, a full rebuild recipe and the landmines
are in **`upstream/BUILDING.md`**, with the combined diff in
`upstream/ORO-D3D9Client-all-patches.patch`.

The patches fall into three groups, and the distinction matters when you read them:

- **capabilities** ORO needs and the client did not expose (backbuffer capture, a
  depth-clipped Sketchpad path, textured triangles, the render-epoch camera);
- **suppression** — letting an addon turn *stock* behaviour off (reentry billboards,
  exhaust streams), because the core offers no way to do it;
- **plain stock bug fixes** with no ORO involvement at all, several reproducible with no
  addon loaded.

⚠️ **Probe a capability by BINDING, never by version number.** Every optional feature
tests the bound function pointer (`gc->CanSetVCShadows()`, `CanDrawDepth()`, …). A build
stamp can be stale and an argument can be lost in a call; a null pointer cannot lie. When
a capability is absent the feature must go quiet **and say so in the panel** — a
capability that is dark and silent is indistinguishable from a bug.

---

## 3. The execution model — read this before changing anything

Three places ORO code runs. Confusing them is the single largest source of bugs in this
project's history.

### `clbkPreStep` — the main thread
May call `oapi*` freely. Runs once per simulation step.

⚠️ **It does not run while the simulation is PAUSED.** Anything built here freezes while
the render callback keeps drawing it. Pausing is what people do in order to *look* at
something, so a frozen build shows up immediately as an effect that "follows the camera".

### `clbkProcessKeyboardImmediate` — every frame, paused included
A keyboard callback only by address: the core calls it each frame whether or not the
simulation is running, on the main thread, before the frame is rendered. ORO uses it to
**sense** the world. It consumes nothing and always returns false.

**The law that follows:** *what senses the world runs every frame; what evolves over time
does not.* View gates, viewport size, which planet you are at, how much air there is —
sensed every frame. Envelopes, soak, thermal state, animation phase — main thread only,
because paused means no time passes and nothing should get wetter.

⚠️ Sensing must be **idempotent**, because it is called from both places.

### The render callback — no `oapi` calls, ever
Registered on the client's HUD and pre-resolve render-proc slots. Calling
`oapiGetViewportSize` from inside it once crashed the client; that is why the rule exists.

⚠️ **All screen-space projection happens here**, using the camera the frame is *actually*
being rendered with — which is not the camera `clbkPreStep` saw. Effects are therefore
**split**: a main-thread half that does the `oapi` work and snapshots what it found, and a
render-path half that projects it. Get this wrong and the effect lags the world by a frame
or freezes under pause.

⚠️ **The render path cannot log.** `oapiWriteLogV` is an `oapi` call. Record a number or
raise a flag; let the next main-thread pass write the line.

### Frames and epochs

⚠️ Orbiter's "global" frame is **solar-system barycentric**. A position held fixed in it
recedes from Earth at 29.8 km/s. Anything anchored to the world stores **planet-relative**
coordinates and rebuilds from the planet's current rotation and position at draw time.

⚠️ The renderer runs a step of body state *ahead* of the module callbacks. World-anchored
geometry must take both the camera and its anchors from the renderer
(`gcCore::GetRenderCam`, `GetRenderObjPos`); mixing a render-epoch camera with a pre-step
anchor puts an effect hundreds of metres from its vessel. Vessel-anchored geometry never
sees this, because a tracking camera cancels the epoch.

---

## 4. File map

| file | owns |
|---|---|
| `OroModule.{h,cpp}` | Module lifecycle, the render callback and draw order, the IPI pipeline, settings persistence, thruster-group plumbing. The largest file and the hub. |
| `OroState.h` | `struct OroEffectState g_fx` — every setting and readout, in one struct. Single-threaded by construction; no mutex. |
| `OroDialog.{h,cpp}` | The whole control panel: a menu tree of pages, hand-drawn. See §7. |
| `OroPhysics.cpp` | The felt-G model: proper acceleration at the camera, pilot body axes, cardiovascular lag, oxygen reserve. |
| `OroReentry.cpp` | Reentry plasma: per-vessel slot table, heat model, stock suppression, hull sampling, the fin/envelope geometry, the trail, the VC glow. The oldest code here. |
| `OroPlume.cpp`, `OroBell.cpp`, `OroParticles.cpp` | The thruster family: the shared plume model and its consumers, the bell-glow thermal model, and configuration of Orbiter's *own* particle streams. |
| `OroRain.cpp` | The surface storm: sheet, splashes, cloud deck, windscreen drops, storm lightning, and the wet-world values pushed to the client. |
| `OroAurora.cpp`, `OroLightning.cpp`, `OroEclipse.cpp`, `OroGodRays.cpp`, `OroFog.cpp`, `OroRings.cpp`, `OroVapour.cpp` | One environment effect each. `OroGodRays.cpp` also owns the shared solar sense and the lens flare. |
| `OroTree.{h,cpp}` | Shared reader for Orbiter's tile archives (`Cloud.tree`, `Mask.tree`): decode, cache, sample. |
| `OroDDS.{h,cpp}`, `OroRingProfile.{h,cpp}` | DDS/DXT decoding, and deriving a ring's optical-depth profile from the texture a planet already ships. |
| `orofx.hlsl` | Every IPI pixel shader, one file. **Deployed by hand** to `Modules\ORO\`; there is no post-build copy step. |
| `upstream/` | The client patches and `BUILDING.md`. Not ORO code. |
| `tools/` | Verification and asset generation — see §8. |

---

## 5. The laws

Numbered so they can be cited. Each one prevents a specific failure.

1. **No `oapi` calls in the render callback.** §3.
2. **Sketchpad geometry is updated with the FULL creation count, every frame, unused tail
   zero-padded.** The client locks with `D3DLOCK_DISCARD` (fresh uninitialised VRAM) and
   always draws the creation count. An unwritten tail is random triangles strobing across
   the screen.
3. ⚠️ **A vertex stream caps at 65535 vertices.** `MAX_TRI * 3` must stay under it. Past
   that, buffer creation fails, the client does not check, and the first draw dereferences
   null. To go bigger, split across several polys.
4. **Animations use real time** (`oapiGetSysStep`), never simulation time — a 0.3 s blink
   stays 0.3 s at 100× time acceleration. Physical objects that *advect* are the deliberate
   exception.
5. **Colours are `0xAABBGGRR`.**
6. **Nothing accumulates across frames** unless it is expiring, deterministic and measured.
   Geometry is rebuilt every frame, so a bad sample vanishes on the next one; a bad
   *stored* sample lives forever. This rule was written after a persistent trail spent
   fifteen rounds producing spikes nobody could remove.
7. **Anything borrowed from a vessel is handed back on every exit path** — lights,
   particle streams, meshes, suppression flags, camera offsets. Including
   `clbkDeleteVessel`, where the handle dies when the call returns.
8. ⚠️ **Nothing may hand the core or the client a long-lived object until the scene has
   rendered a frame.** `clbkPreStep` runs during scenario load, when the world is half
   built; lending an object there crashes on the *next* load. Reading state is fine;
   lending is not. Gate at the lend, not around the whole update.
9. ⚠️ **Scene-owned borrows go back in `GENERICPROC_SHUTDOWN`, not `clbkSimulationEnd`** —
   the latter arrives after the scene is destroyed. The test: is this thing destroyed *with*
   the scene? Particle streams yes; lights, meshes and vessel flags no.
10. **Decide an effect's DOMAIN first** — external, internal, or both — and gate it there.
11. **Bind an effect to the physical quantity it depends on**, not to the convention that
    usually coincides with it. Ask the atmosphere which way the air is moving rather than
    assuming the hull's +Z; a tail-sitter then works with no special case.
12. ⚠️ **Two effects that share a value must not share a gate.** A value computed inside
    one effect's pill becomes stale the moment that pill is off, and the second effect dies
    for a reason no user can guess.
13. ⚠️ **A lattice that must not seam needs whole cycles per unit of a wrapping
    coordinate.** Tile UVs agree with their neighbours only modulo 1; an azimuth wraps at
    2π. A fractional multiplier seams visibly.
14. ⚠️ **A hand-counted constant is a claim about a table.** Several here (group row
    counts, capability-guard lists, patch counts) are hand-maintained and are used to place
    things. Count the table; do not trust the number.
15. **A comment explaining why something exists is evidence that it is not dead.**

16. ⚠️ **Nothing is written to the log per frame — ever.** Everything goes through
    `OroLog(level, ...)` in `OroLog.h`; the level is a ceiling set by `Debug` in
    `Config\ORO.cfg` (0 necessary / 1 concise, the default / 2 verbose). Anything inside a
    function that can run per frame uses `ORO_LOG_EVERY`, which keeps a timer per call
    site off a REAL-time tick — a throttle on simulation time fires every frame at 100×
    time acceleration, which is the same bug wearing a hat. `tools/logaudit.py` enforces
    both, and refuses a raw `oapiWriteLog` outside the one entry point.
    **This law was bought:** a shader stopped using a uniform the host went on pushing, and
    the client dutifully logged the failed call once a frame — 16,252 of a 17,134-line
    `Orbiter.log`, shipped in a release. Note what a log level would *not* have stopped:
    that flood was the CLIENT's line about OUR call, so the defence for it is
    `tools/ipicheck.py`, which checks every pushed uniform against what its entry point
    actually references.

---

## 6. Things ORO controls but does not draw

A large part of the addon renders nothing: it hands the patched client a value and the
client's own passes do the work. VC shadows, the wet and mirroring ground, storm light,
fog, base lights, cabin dimming, the ring shadow, and the cascaded shadow atlas all work
this way.

The rules are the same as for anything borrowed: **probe by binding; grey the control out
when the capability is absent; push on CHANGE, not per frame** (it is client *state*, not a
frame parameter); and hand the stock behaviour back when the master switch is off.

---

## 7. The control panel

`OroDialog.cpp` paints everything itself into one memory bitmap and blits it once. There
are no child controls; mouse handling is direct.

- **Sized in pixels, laid out in document coordinates.** The `.rc` template's size is a
  throwaway — the real size is forced in code, because DLU→pixel conversion varies by
  machine and DPI. Pages scroll, so paint and hit-testing must share one coordinate space:
  painters never see scrolling, and hit-testing adds the scroll offset exactly once.
- **Row tables are the assembly line.** One entry per control; painters and hit-testing
  loop over them, so adding a slider is one line plus its field in `OroEffectState`.
- ⚠️ **Row tables reference state by ADDRESS**, and so do the settings tables. A field that
  looks unused in the render path may still be live in a table — and removing a *table
  entry* while keeping the field compiles cleanly and silently stops that setting
  persisting.
- **The panel scales itself** on large displays by stretching the finished bitmap. One
  function returns the layout rect; nothing else may ask the window for its size.

### Settings scopes

Three, and the split is deliberate: **global** (`Config\ORO.cfg`) is what the *pilot* is;
**per vessel class** (`Config\ORO\<class>.cfg`) is what a *hull* needs; **per body**
(`Config\ORO\bodies\<name>.cfg`) is what a *world* is. Two blocks may move between the
first two per hull.

Their rules for a missing value differ, and each is deliberate: a class with no look
settings **keeps** the current numbers; a world with no file gets the **built-in
defaults**; a hull with no movable block falls back to the **global** values.

---

## 8. Verifying a change

There is no automated test suite. What exists instead is a set of tools that prove
specific things, and the discipline of knowing which proof a change needs.

| tool | proves |
|---|---|
| `tools/fxccheck.sh` | Every client shader compiles in every configuration the client uses. **Run it on every shader edit.** Run it from a POSIX shell where `$TEMP` is set. |
| `tools/d3dxps.py` | One standalone shader entry point compiles **through the client's own compiler**, with its register, sampler and instruction-slot counts. `-asm` dumps the disassembly; `-members` walks the constant table and prints, per struct member, the registers allocated, the registers the code reads, and the packed byte offset the upload walks. |
| `tools/structlayout.py` | The 32-bit byte layout of the client's shader-mirrored C++ structs (`ConstParams`, `ShaderParams`, `LightF`), from a probe built with the client's own compiler. Run it after changing any of them. |
| `tools/d3dxsetvalue.py` | What `SetValue` actually lands in the registers for a struct upload: an indexed buffer through the real D3DX walk on a null device. Its "packed walk" column must match `structlayout.py`'s offsets, or the shader reads the wrong floats. |
| `tools/fxeff` | The same for the client's effect files, per pass. |
| `tools/luachk` | Lua 5.1 syntax for anything in `Script\`. |
| `tools/ipicheck.py` | Every uniform the host pushes is one its entry point actually references. A push to a name the shader does not use is **not a no-op** — the compiler strips the unused uniform out of the constant table, the call fails, and the client logs an error *every frame*. |
| `tools/logaudit.py` | Nothing can log per frame (law 16), and nothing logs outside `OroLog`. |
| `tools/scnstate.py` | `--check` validates the scenario-authoring frame maths against states Orbiter itself wrote, before any of it reaches a scenario file. |
| `tools/scenarios.py` | The scenarios, their launchpad pages and the folder pages, all from one table, so a description can never drift from the scenario beside it. It keeps a manifest and **refuses to overwrite a file a human has edited since it was written** (`--force` overrides). |
| `tools/scnfreeze.py` | Once a scenario has been flown and re-saved, its state is Orbiter's, not the generator's: this captures everything after `END_DESC` into `scenarios.frozen.json`, and `--check` proves the frozen data still matches the live tree. The generator then emits the prose and replays the frozen tail verbatim. |
| `tools/setuphelpcheck.py` | Every control and group box of the client's Advanced Setup dialog has a help entry (the "?" / F1 / Help text lives in the client's `SetupHelp.cpp`). It reads the dialog out of the resource script, so a control added later cannot ship silent, and it insists the table stays ASCII. |

⚠️ **The Windows SDK's `fxc` is not evidence about this client.** The client compiles
through `d3dx9_43.dll`, which allocates literal constants differently and has twice
rejected a shader `fxc` had just passed — and once accepted one `fxc` rejected. Use the
tools above.

⚠️ **`ps_3_0` has three separate budgets**: 224 constant registers, 16 samplers, and an
instruction-slot cap the *card* reports (4096 on the hardware this was developed against,
though the specification allows as little as 512). A shader can pass the first two, compile
cleanly, and then silently fail to run on the device. The tools check all three.

**A change that cannot alter behaviour should be proved so, not tested.** A comment edit
should leave the binary identical; a shader comment should leave the disassembly identical.
Where that proof is available, it is worth more than a test flight.

**And where a change replaces one implementation with another, run both.** Keep the old
one in the build, drive both from the same inputs, compare, and log a verdict — then delete
the old one once it reports clean. Reading two functions can prove the *algorithms*
equivalent; it cannot prove the *plumbing* — that the replacement is handed the same file,
the same level, the same units — and the plumbing is where a migration actually goes wrong.

It is also worth feeding such a harness deliberately illegal inputs. When the lightning's
tile reader was replaced by the shared one, 296 of 297 samples matched to the bit and the
one that did not was a latitude of 180°, which is not a latitude. That disagreement was a
real out-of-bounds read: a bilinear index clamped at the top but not the bottom, present in
**three** copies of the same routine, two of which were live. No caller could reach it, and
no flight would ever have found it.

---

## 9. Adding an effect

Roughly the order that has worked:

1. **Decide the domain** (law 10) and what physical quantity drives it (law 11).
2. **Read the client first.** Several effects here turned out to be mostly already
   rendered by the client — the eclipse, the VC shadows, the god rays. Grepping
   `OVP/D3D9Client` before designing has changed the design more than once, always for the
   smaller.
3. **Split it** main-thread / render-path from the start (§3). Retrofitting that split is
   much harder than building it in.
4. Add the state to `OroEffectState`, a row to the page's table, an entry to the settings
   table, and the draw to the callback in the right place in the order.
5. **Give it a readout that says why it is doing nothing.** Most effects here have several
   honest ways to show nothing — wrong world, no air, sun behind you, wrong view. Without
   a line naming the reason, every one of them reads as a fault.

---

## 10. The graveyard

Approaches that were built, flown and buried. They are recorded because each looks
reasonable on paper, and because the reasons generalise.

- **Round sprites as a luminous medium.** Orbiter's particle streams, and later ORO's own
  additive sprites, for reentry plasma and its trail. Disconnected sprites cannot read as a
  continuous medium at any density, and the per-pixel brightness of an additive sprite
  chain is a projective sum that flares without bound where perspective compresses it. No
  scalar computed *inside* one sprite can normalise a sum over many. **Connectivity is the
  fix**: a ribbon deposits its alpha once per pixel and the whole problem class disappears.
- **Splats on mesh-sampled points.** Mesh vertices cluster at authored detail — nosecones,
  engine bells — so point-sampled effects clump there however many samples you take.
- **Screen-space silhouette rings** around a vessel: they read as a bubble, and head-on
  they become a ring around nothing.
- **Heightfields from sparse unstructured samples**: jagged shards, for three independently
  verified reasons.
- **Anything lofted from mesh triangles** shows its tessellation at close range. Surfaces of
  revolution and analytic profiles are smooth at any resolution by construction.
- **Hard alpha clamps** flatten Gouraud gradients into cutouts; use a soft exponential
  ceiling.
- **A partial anchor is a partial decal.** An effect tracking the world at 42% is tracking
  the *camera* at 58%, and viewers notice. If a compromise is needed it belongs in what the
  effect looks like — its reach, its profile — never in how faithfully it is anchored.
- **Local lights as a stand-in for area light.** Orbiter's point lights have no occlusion
  and no shape: one standing in for a curtain hundreds of kilometres wide lights the whole
  hull evenly, from nowhere in particular.
- **Physically-correct rules keyed to brightness, in the sun pipeline.** Nothing there
  knows about clouds, so every such rule misreads a hazy morning as a sunset. Key on what
  the finished frame *contains* instead.

---

## 11. Licence and provenance

Dual GPL v3 / LGPL v3, matching D3D9Client, whose headers and licence files are reproduced
unmodified. Orbiter's core SDK is MIT; XRSound is MIT and is a soft dependency — every
call no-ops if it is absent.

The client patches in `upstream/` are modifications of jarmonik's D3D9Client, published
under the same dual licence so that the binary ORO ships can be rebuilt from source.
