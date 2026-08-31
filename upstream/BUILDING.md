# Rebuilding D3D9Client for ORO (TWENTY-SIX local patches: a-y, +k2)

ORO runs on a locally-patched D3D9Client carrying **twenty-six** ORO patches:

- **(a) `D3D9Client-HUD-renderproc-CTD-fix.patch`** - the crash fix. Stock Orbiter 2024
  clients CTD the moment any `RENDERPROC_HUD_1ST/2ND` callback is registered
  (`MakeRenderProcCall` passes NULL view/proj matrices for the HUD stages and
  `D3D9Pad::SetViewProj` dereferences them unchecked). Tested 2026-07-25.
- **(b) `D3D9Client-gcCore-backbuffer-access.patch`** - exposes the live frame to
  addons so ORO can run its own HLSL over it (grey-out / blur / tilt). See
  "Patch (b)" below. Tested 2026-07-26.
- **(c) `D3D9Client-reentry-suppression.patch`** - lets an addon hide the client's
  built-in reentry billboards per vessel, so it can draw its own. See "Patch (c)"
  below. Tested 2026-08-01.
- **(d) `D3D9Client-sketchpad-additive-blend.patch`** - adds an ADDITIVE blend state
  (value 0x5) to the Sketchpad. One file (`D3D9Pad.cpp`), ~15 lines: the API offered
  ALPHABLEND/COPY variants only, although the device uses `D3DBLEND_ONE` internally
  all over the client (exhaust, beacons, base tiles). An effect drawing LIGHT - the
  ORO reentry plasma - needs src.rgb*src.a ADDED to the frame, not lerped over it.
  Implemented in `D3D9Pad::Flush()` after `BeginPass` (overrides the pass defaults;
  explicit restore after `EndPass` since the effect runs `D3DXFX_DONOTSAVESTATE`, and
  the pass re-establishes InvSrcAlpha on the next `BeginPass` anyway). The addon
  passes the raw value `(BlendState)0x5` - no SDK header (DrawAPI.h) change needed,
  so the patch surface stays client-only. Gate on `gcAPIVer >= 260801` (patches c
  and d share that build stamp; ORO falls back to ALPHABLEND without it - the
  plasma tints instead of glowing, degraded but not broken). Tested 2026-08-01.
- **(e) `D3D9Client-reentry-particle-suppression.patch`** - extends (c)'s per-vessel
  suppression to the reentry PARTICLE streams. See "Patch (e)" below. Built
  2026-08-01 after stock "puffs" survived (c) at ~89 km.

This documents the local rebuild that produced all five patches.
- **(f) `D3D9Client-vc-shadow-map.patch`** - SHADOWS IN THE VIRTUAL COCKPIT. Three
  parts across `Scene.cpp` + `Mesh.cpp`, plus a `gcCore::SetVCShadows(bool,float)`
  entry point. See "Patch (f)" below. Tested 2026-08-04.
- **(i) the PRE-RESOLVE render-proc slot** - `RENDERPROC_PRE_RESOLVE 0x0006`, the
  reentry plasma's HDR compositing point (Firefly rework step 1). See "Patch (i)"
  below. Built 2026-08-08. ((h) stays RESERVED for the assessed-but-unbuilt
  "scene depth to IPI" patch, which would hand the scene depth to the IPI
  post-process path rather than only to the Sketchpad path that (g) serves - so the
  letters in the docs and the code comments never collide.)
- **(j) the ATOMIC CFG WRITE** - fixes a STOCK Orbiter 2024 bug: video settings
  were lost on EVERY normal exit (0-byte D3D9Client.cfg). See "Patch (j)" below.
  Diagnosed with a raw-Win32 tracer + automated Launchpad open/close harness and
  verified fixed the same way, 2026-08-08.
- **(n) per-vessel STOCK EXHAUST suppression** - `gcCore::SuppressExhaust`, the
  (c)+(e) story told again for the ENGINE exhaust: billboards gated in
  `vVessel::RenderExhaust`, exhaust-stream EMISSION gated in `ExhaustStream::Update`.
  Built 2026-08-09 for the plume-expansion overlay's judging pill. See "Patch (n)"
  below. SPLIT 2026-08-09 into `GCEXH_BILLBOARD` / `GCEXH_STREAM` flags - see the
  "Patch (n) addendum". ((k), (l), (m) are documented in their own sections only -
  this top list stopped being maintained per-patch after (j) and resumed here.)
- **(o) the STREAM EXEMPTION LATCH** - `gcCore::ExemptNewStreams`, so an addon can
  REPLACE a vessel's stock exhaust streams rather than only add to them. Built
  2026-08-09 for the PARTICLES sub-tab. Carries two findings worth reading before
  touching particle streams at all: `clbkCreateParticleStream` is UNIMPLEMENTED in
  this client, and a set-of-stream-pointers exemption silently misses because
  `D3D9ParticleStream` has two base classes. See "Patch (o)" below.
- **(p) VC SHADOW DEPTH** - `gcCore::SetVCShadows` gains a third argument. Stock
  self-shadowing scales the SUN term only, so a shadowed virtual-cockpit surface keeps
  all its material ambient and emissive and the shadow reads as a faint grey smudge no
  external setting can deepen. (p) lets the shadow take the AMBIENT with it, by a
  user-set fraction, in the COCKPIT PASS ONLY. Emissive is never scaled. Built
  2026-08-09. ⚠️ Touches FOUR DEPLOYED SHADERS - see "Patch (p)" below.

## The bug (for the upstream report)

- Repro: any module registers an EMPTY callback via
  `gcCore->RegisterRenderProc(proc, RENDERPROC_HUD_2ND, p)` -> CTD on the first rendered
  frame, before the callback body is entered.
- Cause chain: `Scene::RenderMainScene` HUD stage calls
  `MakeRenderProcCall(pSketch, RENDERPROC_HUD_2ND, NULL, NULL)` (Scene.cpp:2124);
  `MakeRenderProcCall` unconditionally calls `pSkp2->SetViewProj(pV, pP)`
  (D3D9Client.cpp:2765); `D3D9Pad::SetViewProj` does `mV = *pV` with no NULL check
  (D3D9Pad.cpp:342). Present in tag `2024` (shipped Build 241231) AND in `main` at the
  time of writing.
- Fix: move `SetViewProj` inside the existing `EXTERIOR || PLANETARIUM` branch (HUD
  procs draw in the ortho defaults `LoadDefaults()` already establishes - `vmode = ORTHO`,
  identity matrices, D3D9Pad.cpp:283/306-309), plus a defensive NULL-guard in
  `SetViewProj` itself. Verified: the same module that instantly CTD'd on the stock
  client runs cleanly on the patched one, callback invoked (ORO step 2, full visual
  confirmation: full-frame Sketchpad wash in VC/panel views).
- Secondary observation (unattributed): in some scenarios the first ~1 s of a session
  logs failing `pDevice->Clear` calls (D3DERR_INVALIDCALL) at Scene.cpp:1249 (the
  "scene not yet initialized" early-return) with a black screen, then self-heals.
  Also seen on the STOCK client (with a HUD proc registered, before the CTD), so it is
  not caused by this patch. Not yet reproduced without ORO loaded; attribution
  experiment: run the same scenario with ORO disabled on the patched client.

## Build recipe (mirrors .github/workflows/reusable-build.yml)

> **THE EASY PATH (since 2026-08-13): skip step 3 entirely.** All twenty-six patches are
> published, already applied, on the `oro-patches` branch of
> <https://github.com/dgatsoulis/orbiter-oro> (branched from tag `2024`). Clone that
> instead of upstream and there is nothing to apply:
>
> `git clone --branch oro-patches https://github.com/dgatsoulis/orbiter-oro.git`
>
> Steps 1 and 3 below are the from-scratch route, kept because it documents WHAT each
> patch does and in what order they have to go on — which is what anyone reviewing or
> cherry-picking them needs, and what the fork's single commit cannot show.

Workspace used: `C:\OrbiterDev\` (deletable; everything here recreates it).

1. `git clone --depth 1 --branch 2024 https://github.com/orbitersim/orbiter.git C:\OrbiterDev\orbiter`
   (tag `2024` = shipped Build 241231; the CI path `D:\a\orbiter\orbiter\...` in crash
   logs proves the shipped DLL comes from this tree.)
2. DXSDK June 2010: download `https://download.microsoft.com/download/a/e/7/ae743f1f-632b-4809-87a9-aa1bb3458e31/DXSDK_Jun10.exe`
   (~600 MB), then `7z x DXSDK_Jun10.exe DXSDK/Include DXSDK/Lib` into `C:\OrbiterDev\`.
3. Apply the ORO patches, **with `git apply`** (see note). ⚠️ **Only SEVEN of the
   twenty-six exist as `.patch` files** — the rest are documented as code listings in the
   per-patch sections below, because all twenty-six were developed as uncommitted
   working-tree changes and a per-file diff would carry the earlier ones too. The
   complete, verified set is `ORO-D3D9Client-all-patches.patch` (every patch, against
   tag `2024`), or just use the fork above. The individual files are:
   `D3D9Client-HUD-renderproc-CTD-fix.patch`,
   `D3D9Client-gcCore-backbuffer-access.patch`,
   `D3D9Client-reentry-suppression.patch`,
   `D3D9Client-sketchpad-additive-blend.patch`, then
   `D3D9Client-reentry-particle-suppression.patch`. (a) and (b) touch disjoint files;
   (c) shares `gcCore.h`/`gcCore.cpp` with (b) and was generated on top of it, so
   **apply (b) before (c)**. (d) touches `D3D9Pad.cpp`, which patch (a) also touches -
   it was generated on top of (a), so **apply (a) before (d)**. (e) touches only
   `Particle.cpp` (applies in any order) but **references (c)'s
   `gcIsReentrySuppressed` symbol - the client only LINKS with both present**.
   USE `git apply`, NOT `patch`: the repo checks out CRLF while the patches carry CRLF
   context, and GNU `patch` trips over the combination (this cost a confused verification
   pass on 2026-08-01). To confirm a patch is already applied without touching anything:
   `git apply --check --reverse <patch>`.
4. Local build tweaks (NOT part of the upstream patch - see LANDMINES below):
   a. Comment out `add_dependencies(${OrbiterTgt} orbiter_lua)` in
      `Src/Module/LuaScript/LuaInterpreter/CMakeLists.txt` (needs hhc.exe otherwise).
   b. Create stub dir `Extern/irrKlang/x86/irrKlang-1.6.0/{bin/win32-visualStudio,lib/Win32-visualStudio,include}`
      (dead download URL otherwise; XRSound is configured but never built).
5. Configure + build (from a VS2022 x86 env; VS-bundled CMake+Ninja):
   `cmake . --preset windows-x86-release -DORBITER_MAKE_DOC=OFF -DDXSDK_DIR:PATH=C:\OrbiterDev\DXSDK`
   `cmake --build out\build\windows-x86-release --target D3D9Client --parallel`
   (see `C:\OrbiterDev\build_d3d9.bat` for the exact env setup)
6. Deploy: back up `<Orbiter>\Modules\Plugin\D3D9Client.dll` as
   `D3D9Client.dll.orig-241231`, then copy
   `out\build\windows-x86-release\Modules\Plugin\D3D9Client.dll` over it.
   Rollback = rename the backup back.

## LANDMINES hit on the way (each cost one failed configure/build)

1. `-DORBITER_BUILD_XRSOUND=OFF` BREAKS CONFIGURE: `LuaInterpreter`'s CMakeLists
   hard-references the XRSound targets (`XRSound_lib` in target_link_libraries and
   add_dependencies), so the OFF path cannot generate. Build CI-faithful (XRSound ON)
   and stub irrKlang instead.
2. `OrbiterTgt` depends on `orbiter_lua` (the Lua CHM doc target), which needs
   `hhc.exe` (HTML Help Workshop) at BUILD time - present on CI runners, usually not
   on dev machines. Cut the edge locally (tweak 4a); D3D9Client is unaffected.
3. **`gcCoreAPI.h` REGENERATION DROPS THE HAND-ADDED GUARDS.** The build runs the
   codegen over `gcCore.h` on every configure, so any patch touching that header wipes
   `CanCaptureBackBuffer`, `CanSuppressReentry`, `CanSetVCShadows`, `CanDrawDepth`,
   `CanGetRenderCam`, `CanGetRenderObjPos`, `CanDrawTexPoly` and `CanSuppressExhaust`
   from the generated output. Hit for real on 2026-08-04. Note the codegen writes into the
   CLONE (`out/build/.../Orbitersdk/include/gcCoreAPI.h`); the copy ORO compiles against
   is `<Orbiter>\Orbitersdk\include\gcCoreAPI.h` and is updated BY HAND, so a client
   rebuild alone is harmless - it is copying the regenerated header over that one that
   loses the guards. Re-add all EIGHT, then verify (8 of 8) before building ORO.
3b. **⚠️ THE CODEGEN ALSO MIS-WIRES MULTI-POINTER OVERLOADS, AND IT FAILS SILENTLY.**
   This one cost most of a day on 2026-08-06. The generated wrapper for
   `CreateTrianglesDepth` called `pCreateTriangles(hPoly, pt, npt, flags)` - the WRONG
   bound pointer, silently dropping the `pDepth` argument. Everything compiled, linked and
   ran; the depth array simply never reached the client, so the depth clip did nothing and
   looked like a shader bug. Three rounds of probe shaders went into the shader before the
   header was suspected. **After ANY regeneration, read the wrapper body of every new
   function and check it calls its OWN `p<Name>` pointer with ALL its arguments.** A
   missing guard is loud (compile error); a mis-wired wrapper is not.
4. **The `[Build]` stamp only refreshes when `D3D9Util.cpp` recompiles**, because
   `BuildDate()` bakes `__DATE__`. Patch a different file, rebuild, redeploy, and the log
   still shows the OLD stamp - you cannot tell the new DLL is live. Force-touch
   `D3D9Util.cpp` after every client patch. It still cannot distinguish two builds made
   on the SAME DAY; fall back to a file hash for that.
5. The pinned irrKlang URL (`ambiera.at/downloads/irrKlang-32bit-1.6.0.zip`) is DEAD
   (404; ambiera.com variant also 404s - site restructured). The gate is just
   `if(NOT EXISTS Extern/irrKlang/x86)`, so a stub directory satisfies configure and
   FetchContent never runs (tweak 4b). CI survives on its action cache.

## Patch (b): gcCore backbuffer access (the ORO premium / IPI pipeline)

Grey-out, and later blur/tilt/image-space-shake, resample the LIVE frame through the
client's image-processing interface (`gcIPInterface`, HLSL). That needs two things the
stock gcCore never exposed - both trivially present INSIDE the client, just not reachable
from an addon:

- **`gcCore::GetBackBufferHandle()`** - returns the client's existing
  `D3D9Client::GetBackBufferHandle()`. Used as the shader OUTPUT target and as the
  copy SOURCE.
- **`gcCore::CopyResource(tgt, src)`** - a device `StretchRect` issued directly, with
  NO `Begin/EndScene` wrapper. This matters: the pre-existing `StretchRectInScene`
  wraps `g_client->BeginScene()`, which returns `D3DERR_INVALIDCALL` when we are already
  mid-scene (a render callback runs between the device's Begin/EndScene), so it silently
  no-ops AND corrupts the client's `bRendering` flag. `CopyResource` is safe mid-callback.

ORO then: capture backbuffer -> a render-target TEXTURE (`oapiCreateSurfaceEx`,
`X8R8G8B8` - matches the backbuffer so `StretchRect` is a plain copy / MSAA resolve) ->
`gcIPInterface` samples it, writes the transformed frame back to the backbuffer via
`Execute(0, /*bInScene=*/true, Rect)` (which saves & restores the render target itself).

**The patch touches 3 client files** (`gcCore.h`, `gcCore.cpp`, `D3D9Util.cpp`), all in
`D3D9Client-gcCore-backbuffer-access.patch`. `D3D9Util.cpp` is only force-touched so
`BuildDate()`'s `__DATE__` re-bakes (marker refresh).

**Consumer side (`Orbitersdk/include/gcCoreAPI.h`)** is mostly AUTO-GENERATED: the
`INTERFACE_BUILDER`/`gc_interface` codegen produces the `GetBackBufferHandle`/`CopyResource`
wrappers, function pointers and bind calls from the patched `gcCore.h` at build time, and
the regenerated header lands in `out/build/.../Orbitersdk/include/gcCoreAPI.h`. **Redeploy
that regenerated header** over `Z:\Orbiter-2024\Orbitersdk\include\gcCoreAPI.h` (ORO
compiles against it). ONE hand-add the codegen does NOT produce - append it to the
deployed header (it null-checks the two bound pointers so ORO stays dormant on a stock
client):

```cpp
	bool CanCaptureBackBuffer()
	{
		return (pGetBackBufferHandle != NULL) && (pCopyResource != NULL);
	}

	bool CanSuppressReentry()      // patch (c)
	{
		return (pSuppressReentry != NULL);
	}
```

(FOLLOW-UP: fold this into the deferred "ship story" by gating on
`GetSystemSpecs().gcAPIVer >= 260726` instead, so nothing hand-edits a generated header.)

## Patch (c): per-vessel reentry suppression

**The bug (for the upstream report - this is the best-evidenced of the three).**
`VESSEL::SetReentryTexture(NULL)` is documented as suppressing a vessel's reentry flames
(`VesselAPI.h:4875`). The INLINE renderer honours it - `Src/Orbiter/Vvessel.cpp:671` gates
the reentry trail on `vessel->reentry.do_render` **and** the user's
`CfgVisualPrm.bReentryFlames` Launchpad option. `vVessel::RenderReentry`
(`OVP/D3D9Client/VVessel.cpp:1718`) honours **neither**: it gates only on its own globally
loaded `defreentrytex`, so under D3D9Client both the documented API and the user's own
visual-effects setting are silently ignored. Grep confirms neither symbol appears anywhere
in the client. Consequence: an addon replacing the reentry visuals cannot turn the stock
ones off, and a user who dislikes reentry flames cannot switch them off either.

**Why the fix is a new gcCore call and not "honour do_render".** `do_render` lives in
`Src/Orbiter/Vessel.h`, which is core-internal and unreachable across the client DLL
boundary - a client cannot read it without a new core API. Fixing it properly is jarmonik's
call (expose a getter, or have the core pass the flag to the client). Patch (c) is the
client-side equivalent we can ship: `gcCore::SuppressReentry(OBJHANDLE, bool)` keeps an
opt-in set inside the client and `RenderReentry` early-returns for members of it.

**Touches 3 files:** `gcCore.h` (the `gc_interface` declaration - codegen turns this into
the addon-side wrapper), `gcCore.cpp` (bind entry, the set, and the
`gcIsReentrySuppressed` query), and `VVessel.cpp` (one `extern` + one `if`).

**The addon MUST clear its suppressions** on unload / disarm, or the vessel keeps its flames
hidden for the rest of the session. ORO does this in `ReentryFreeSlot`, which every exit
path funnels through, and it calls `SuppressReentry(h, false)` even when the vessel is
already gone - the suppression lives in the CLIENT's list, keyed by a handle that Orbiter
will eventually reuse.

**Shader:** ORO's pixel shaders live in `Modules\ORO\orofx.hlsl` (source-of-truth
copy vendored in the ORO repo root). The vertex stage is the stock
`Modules\D3D9Client\IPI.hlsl:VSMain`, auto-selected by `ImageProcessing`. The shader is
recompiled from file on every session start - iterate on it with NO rebuild.

## Patch (e): reentry particle-stream suppression

**The bug it closes.** Patch (c) silences the client's reentry BILLBOARDS, but the
reentry "puffs" survived it (user report, ~89 km / M27, 2026-08-01): those are reentry
PARTICLE STREAMS, a separate render path. The core gives **every vessel** a default one
(`Vessel::SetDefaultReentryStream`, `Src/Orbiter/Vessel.cpp:2345` - spec `ATM_PLOG 2e8`,
emitting from ~84 km at entry speed) and vessels may add their own - stock Atlantis does
(`Atlantis.cpp:147`, `ATM_PLIN 6e7..12e7`, emitting from ~95 km). There is no core API to
disable another vessel's streams, so the switch belongs in the client, keyed off the same
per-vessel suppression list as (c).

**The fix.** One file (`Particle.cpp`), 2 lines + comments: `ReentryStream::Update` gates
its EMISSION branch on `!gcIsReentrySuppressed(hRef)`. Emission only - particles already
in flight advect and expire naturally, and the suppressed branch falls into the existing
`else t0 = simt`, so lifting suppression cannot release a backlog burst. Exhaust and
custom (non-reentry) streams are untouched. No new gcCore API: the addon-side
`SuppressReentry(h, true/false)` from (c) now covers both paths, so no ORO-side change
is required to ADOPT (e) - but ORO must call SuppressReentry EARLY enough; see
`StockReentryWants` in `OroReentry.cpp` (the tracking envelope must lead stock's
earliest particle threshold, `0.5*rho^0.6*v^3 >= ~6e7`, not ORO's own heat threshold).

## Patch (f): shadows in the virtual cockpit

**The bug it closes.** The VC never received shadows, and almost nothing was missing to
make it work. `Scene::RenderMainScene` renders the focus vessel's shadow map, then sets
`smap.pShadowMap = NULL` before the "remaining vessels" loop - and the internal pass
(`vFocus->Render(pDevice, true)`) runs after that. `vVessel::Render` binds the map only
while `shd->pShadowMap` is non-NULL, so `gShadowsEnabled` was false for every VC draw and
`Common.hlsl`'s `ComputeShadow()` early-returned "fully lit" for the whole cabin. An
ordering consequence, not an absent feature - note `RenderShadowMap` even carries a
`bInternal` parameter, threaded all the way to `vVessel::Render`, that nothing ever
passes as true.

**The fix, three parts.**

1. `Scene.cpp` - re-render the focus vessel's shadow map at the top of the cockpit stage.
   Re-render rather than stash the earlier one: `smap` carries the light's MATRICES as
   well as the texture, and at `ShadowMapMode >= 2` the Intersect loop can leave those
   describing a different vessel while reusing the same per-LOD render target.
2. `Mesh.cpp` - in `D3D9Mesh::RenderShadowMap`'s group loop, skip groups whose material
   diffuse alpha is below 0.9. A group escapes the map only via `UsrFlag 0x1/0x2` and
   softens only through OIT (needs `UsrFlag 0x20` AND a texture), so a mesh setting none
   of those casts fully opaque however transparent its material is. **`deltaglider.msh`
   contains ZERO `FLAG` lines while `cockpitglass` carries diffuse alpha 0.5 and no
   texture** - the canopy was a solid occluder, so no sunlight could reach the cabin at
   all. THIS PART IS VISIBLE WITHOUT ORO: the DG's canopy shadows its own fuselage in
   exterior views. Strongest single item in the whole upstream report.
   ⚠️ **PART 2 SHIPPED WITH AN `opt != 1` GUARD FOR TWELVE DAYS AND THAT GUARD WAS A BUG
   (fixed 2026-08-20).** `RenderShadowMap` serves TWO passes - the shadow map (`opt` 0)
   and `RENDERPASS_NORMAL_DEPTH` (`opt` 1, which fills `psgBuffer[GBUF_DEPTH]` via
   `SHADER_NORMAL_DEPTH`). The skip was written for the pass the evidence pointed at and
   applied there only, so the same 0.5-alpha canopy that stopped casting a solid shadow
   kept writing **fully opaque depth**. Everything that asks that buffer a visibility
   question - SSAO, the sun/local-light checks, and **patch (g)'s per-pixel clip for addon
   geometry** - was told the windscreen is a wall at ~1 m.
   **How it surfaced:** ORO's reentry plasma disappeared whenever the pilot looked
   straight ahead from the DG's VC while the exterior view showed the ship engulfed. The
   plasma was drawing correctly and being clipped behind its own canopy; the aurora and
   the lightning lost the same pixels through the same window. Verified in the mesh:
   `cockpitglass` 0.500, `visor` 0.500, `HUD_glass` 0.400, all under the threshold.
   **The fix is deleting `opt != 1 &&`** - the reasoning ("a material that is clearly not
   opaque does not block") was always pass-independent, and is if anything more obviously
   right for a depth buffer than for a shadow map.
   ⚠️ **AND THE PATTERN IS THE POINT: a rule was written and applied only where the
   evidence pointed.** Third time in this project - invariant 23(k) gated one lend site
   out of four, and 25(e) was not swept onto the CoP shift until a tester flew into a PIO.
   When a law like this lands, grep every site it governs in the same session.
3. `Scene.cpp` - fit the internal-pass map to the CABIN, not the hull. The exterior box
   is the vessel bounding sphere (~20 m on a DG, ~1 cm/texel at 2048), which stair-steps
   on a panel 40 cm from the eye. Centre on the camera (the ORIGIN of that space -
   `vObject::mWorld` carries the camera-relative translation) at a ~2.2 m half-width.

**Landmine in part 3.** Do NOT pass exactly `(0,0,0)`: `RenderShadowMap` computes
`rsmax = viewh*rad/(tanap*|pos|)` and then `lod = log2f(size/(rsmax*1.5f))`, so a zero
`|pos|` gives `log2f(0)` = -inf and `int(round(-inf))` - undefined behaviour feeding the
`psShmRT[]` index. Use a centimetre offset.

**Addon-side API.** `gcCore::SetVCShadows(bool bEnable, float radius)` (+ hand-added
`CanSetVCShadows()` guard) lets an addon A/B the pass and set the box per vessel. Both
client-side defaults reproduce the built-in behaviour, so a client nobody calls behaves
exactly as if the entry point did not exist. **Probe by BINDING, not by build date** -
the guard null-checks the bound pointer, which no stale stamp can fool.

**NOT exposed, deliberately: `ShadowMapFilter`.** It is a `D3DXMACRO` (`SHDMAP`,
`KERNEL_SIZE`) compiled into `D3D9Client.fx` by `D3D9Effect::D3D9TechInit` at
`clbkCreateRenderWindow`, not a shader uniform - nothing can change it mid-session.
Related trap for anyone hand-editing it: `VideoTab.cpp` only `CB_ADDSTRING`s three
filter entries (values 3 and 4, the 35-tap kernels, are commented out), so a config value
above 2 leaves the combo unselected and the next OK on that page writes back
`CB_ERR` = **-1**.

## Patch (g): a depth-clipped Sketchpad draw path

**What it buys:** screen-space Sketchpad geometry can be occluded by the real scene, per
PIXEL. That is what puts the AURORA behind the cockpit in the VC and behind the hull in
external view, and it is the groundwork for the reentry plasma's "streaks render inside the
cockpit" problem. Before it, `gcCore` triangles had no Z at all and simply painted over
everything.

**The insight is that nothing new had to be RENDERED.** The client already fills
`ptgBuffer[GBUF_DEPTH]` (an `A16B16G16R16F` render-target texture, commented in the source
as a shader-readable depth buffer) during `RENDERPASS_NORMAL_DEPTH`, and that pass
explicitly includes the cockpit (`if (oapiCameraInternal() && vFocus) vFocus->Render(...)`).
So (g) is the same shape as (b): hand out a handle to a texture that already exists.

**Files:** `Scene.h`, `Scene.cpp`, `D3D9Pad.h`, `D3D9Pad.cpp`, `D3D9Pad2.cpp`, `gcCore.h`,
`gcCore.cpp`, `shaders/Sketchpad.fx`.

**`D3D9Client-sketchpad-depth-clip.patch` covers only `D3D9Pad2.cpp` + `Sketchpad.fx`** -
the two files (g) touches EXCLUSIVELY. The other six are shared with (b)/(d)/(f), and all
seven patches live in the tree as uncommitted working-tree changes, so a per-file `git diff`
there would carry the earlier patches' hunks too. Those six hunks are small and listed
below; apply them by hand after (a)-(f).

1. **`Scene.h`** - expose the buffer:
   `LPDIRECT3DTEXTURE9 GetDepthTexture() const { return ptgBuffer[GBUF_DEPTH]; }`
2. **`Scene.cpp`** - publish it to a file-scope pointer the Sketchpad can reach:
   declare `extern LPDIRECT3DTEXTURE9 g_gcSceneDepth;`, assign it after the gbuffer is
   created in the Scene constructor, and NULL it at teardown.
3. **`D3D9Pad.h`** - `static D3DXHANDLE eDepthClip, eDepthTex;` and give the
   `D3D9Triangle` constructor / `Update` a trailing `const float *pDepth = NULL`.
4. **`D3D9Pad.cpp`** - define `LPDIRECT3DTEXTURE9 g_gcSceneDepth = NULL;`, bind the two
   handles in `GlobalInit`, and in `Flush`:
   `bool bDepthClip = ((dwBlendState & 0x100) != 0) && (g_gcSceneDepth != NULL);`
   then set `eDepthClip`, and `eDepthTex` when it is on.
5. **`gcCore.h` / `gcCore.cpp`** - `CreateTrianglesDepth()` and `HasDepthBuffer()`.

**⚠️ THE `0x100` BLEND BIT IS THE SAME NO-SDK-HEADER TRICK AS PATCH (d)'s `0x5`.** There is
no API constant for "depth-clip this poly", so the flag rides an unused bit of the blend
state. Anything that masks or validates `dwBlendState` will silently eat it.

**⚠️ THE DEPTH ENCODING IS EUCLIDEAN DISTANCE, NOT CAMERA-SPACE Z.** `GBUF_DEPTH.a` is
`length(frg.posW)` (see `NewMesh.hlsl`, `NormalDepth_PS`). ORO must publish
`length(P - campos)` per vertex to match. Getting this wrong produces a clip that is subtly
wrong at the edges of the frame rather than obviously broken - which is exactly how it
presented.

**⚠️ THE UV IS `sc.xy / gTarget.zw`.** `vTarget.xy` holds `2/Width, 2/Height` (NDC line
widths), NOT `1/size`. Using `.xy` gives a half-screen offset.

**Gate:** the buffer only exists when `Config->bGlares || bLocalGlares` (`SunGlare`, which
defaults to 1). `gcCore::HasDepthBuffer()` reports it; ORO degrades rather than assuming.
Capability is probed by BINDING (`CanDrawDepth()` null-checks the bound pointer), the
lesson from patch (f) - no stale build stamp or lost argument can fool it.

**Still open:** RETIRED 2026-08-08 - the VC plasma moved onto this path (commit 67af7a3)
and both consumers now share it.

## Patch (i): the pre-resolve render-proc slot

**What it buys:** a Sketchpad slot that fires after the COMPLETE scene (terrain, vessels,
transparency, VC) but BEFORE the LightBlur resolve/tonemap and the HUD. With
`PostProcess=1` ("Light glow" in the Launchpad video tab) the top render target at that
point is the client's **fp16 offscreen scene buffer** (`A16B16G16R16F`, created in the
Scene ctor when `pLightBlur` exists), so ADDITIVE draws accumulate past 1.0 and the
client's own threshold bloom (`GFXThreshold`, default 1.1 - only >1.0 colors bloom) plus
the soft tonemap `HDRtoLDR(hdr) = hdr*(1+hdr^4)^-0.25` process them like any other bright
scene content. This is the compositing point the reentry plasma needed: Firefly's whole
white-out is HDR accumulation + bloom, never an authored white, and ORO's old HUD_2ND
slot lands AFTER the tonemap in a clamped 8-bit target (which is where the round-5.5
"red pins at 255" behaviour actually came from). With `PostProcess=0` the slot degrades
gracefully: same draws, plain backbuffer, under the HUD.

**Files:** `gcCore.h` (the `RENDERPROC_PRE_RESOLVE 0x0006` define), `Scene.cpp` (~15
lines in `RenderMainScene`, inserted at the "End Of Main Scene Rendering" comment, before
the "Copy Offscreen render target to backbuffer" block):

    {
        D3D9Pad *pSketch = GetPooledSketchpad(SKETCHPAD_2D_OVERLAY);
        if (pSketch) {
            gc->MakeRenderProcCall(pSketch, RENDERPROC_PRE_RESOLVE, NULL, NULL);
            pSketch->EndDrawing(); // SKETCHPAD_2D_OVERLAY
        }
    }

NULL matrices are safe (that is patch (a)); the pad stays in its ortho pixel-space
defaults, the same contract as the HUD stages, so CPU-projected geometry draws unchanged.
`RegisterRenderProc` appends ANY non-zero id, so an addon can register on a pre-(i)
client without error - the proc simply never fires. ORO exploits that: it latches a
`preResolveLive` flag inside the callback and keeps drawing in the old HUD_2ND slot until
the first real invocation proves the new slot exists (a BEHAVIOURAL capability probe -
even less foolable than probing by binding, and consistent with the gcAPIVer lesson).

**Hand-add to the SDK header after any regeneration:** `RENDERPROC_PRE_RESOLVE` is
defined in `gcCore.h`, so a regeneration CARRIES it - but the hand-maintained copy at
`Z:\Orbiter-2024\Orbitersdk\include\gcCoreAPI.h` must gain it by hand, like the four
`Can*` guards.

## Patch (j): the atomic cfg write (a STOCK 2024 bug, found while chasing "my settings don't save")

**Symptom (user-visible for weeks, invisible in any log):** every Launchpad video
setting reverts to defaults on the next launch. `D3D9Client.cfg` is left **0 bytes**
with an mtime matching the moment the Launchpad closed.

**Root cause, measured (not inferred):** `D3D9Config::WriteParams()` runs from
`~D3D9Config` at `ExitModule` time, and writes through `oapiOpenFile(FILE_OUT)` -
which is `new std::ofstream` INSIDE orbiter.exe. At that point in application
teardown the core's C++ static objects (iostream/locale machinery) are already
destroyed: the C-level open underneath still works and TRUNCATES the cfg, then the
C++ layer calls through a dead runtime pointer. A raw-Win32 tracer (immune to CRT
state) captured it: the ENTER line prints, `oapiOpenFile` never returns, and an SEH
guard logs `EXCEPTION 0xC0000005 at 0x20646162` - **the instruction pointer is
ASCII text ("bad ")**, i.e. execution jumped into string bytes through a clobbered
pointer. The loader swallows the exception, so the process still "exits cleanly"
and nobody ever sees a crash. The session-start write (`clbkCreateRenderWindow`)
works fine - statics alive - which is why settings HOLD during a session and die at
the NEXT Launchpad close.

**Why nobody correlated it:** the crash is silent, the file is rewritten-to-empty
rather than deleted, and the readback failure happens one launch LATER.

**Fix (D3D9Config.cpp only):** WriteParams now formats all ~74 items into a buffer
and writes with plain kernel32 I/O - `CreateFileA` on `D3D9Client.cfg.new`,
`WriteFile`, `FlushFileBuffers`, then **atomic** `MoveFileExA(.new -> .cfg,
REPLACE_EXISTING | WRITE_THROUGH)`. Safe at any teardown stage (no CRT stream
objects), and no failure mode can leave a truncated file: if anything fails, the
previous cfg survives. Output format is byte-compatible with the stock writer
(`Item = value`, CRLF, %g float formatting). The stock body is kept `#if 0`'d
beside it with a DO-NOT-CALL-AT-EXIT note.

**Verified end-to-end with an automated harness** (launch `Orbiter_ng.exe`, wait,
`CloseMainWindow()` on the `Modules\Server\Orbiter.exe` process, inspect): first
cycle wrote a full 1448-byte cfg at exit (first successful exit-write this install
ever made); second cycle round-tripped planted sentinels (`ShadowMapMode 2`,
`ShadowMapFilter 2`, `PostProcess 1`) intact through read-at-start /
rewrite-at-exit.

**Upstream-report note:** this reproduces on an UNPATCHED stock client - it is not
caused by any ORO patch, and it silently destroys jarmonik's own users' settings.
Strongest candidate in the whole report after (c)+(e) and (f) part 2. A general
LANDMINE it teaches: **never do oapi file I/O from ExitModule / module destructors
on Orbiter 2024** - ORO itself saves only from the SAVE button (mid-session) and
must stay that way.

## Patch (k): the render-camera snapshot (the trail's origin-jump fix)

**Symptom:** ORO's reentry TRAIL - its first CLOSE-RANGE WORLD-ANCHORED geometry -
visibly jumped position frame-to-frame ("shimmers wildly... every other frame the origin
jumps to a position near but not in the vessel", 2026-08-08, and the SAME jumps afflicted
the abandoned 2026-08-02 knot-ring trail).

**Root cause, measured in two steps:** module `clbkPreStep` AND `clbkPostStep` both run
before Orbiter updates the camera for the frame, so anything a module projects there uses
a camera one full step stale (~120 m of travel at entry speed - enormous parallax on a
particle a few hundred metres away). Proved empirically with an ORO-side one-shot
diagnostic (2026-08-08): `camera does NOT advance by post-step (300 fast frames)`.
Vessel-anchored overlays never see this (a tracking camera holds the vessel still on
screen, so the epoch cancels); far world geometry (the aurora, hundreds of km out) sees
sub-pixel parallax. Only close world-anchored geometry is exposed, and the trail is the
first ORO has ever drawn.

**Fix (4 small edits):** expose the camera the scene is ACTUALLY rendering with, readable
from inside a render callback.
- `Scene.h`: `MATRIX3 grot` added to `struct CAMERA` (the double-precision rotation -
  `mView` holds the same rotation demoted to float), plus inline
  `Scene::GetRenderCam(VECTOR3* p, MATRIX3* r, double* t)` returning `Camera.pos`,
  `Camera.grot`, `tan(Camera.aperture)`.
- `Scene.cpp` (`UpdateCameraFromOrbiter`): one line - `Camera.grot = grot;` right after
  `oapiCameraRotationMatrix(&grot)`. Refreshed at the top of every scene render pass, so
  it is current whenever any render proc fires.
- `gcCore.h`: `gc_interface bool GetRenderCam(VECTOR3* pos, MATRIX3* rot, double* tanAp);`
  (doc comment in the header states the pre/post-step staleness rationale).
- `gcCore.cpp`: the implementation (`g_client->GetScene()` guard, then
  `pScene->GetRenderCam(...)`, `true`) + one binder line in `gcBindCoreMethod`:
  `if (strcmp(name,"GetRenderCam")==0) *ppFnc = &gcCore2::GetRenderCam;`

**SDK side (hand-edits to `Z:\Orbiter-2024\Orbitersdk\include\gcCoreAPI.h`, NOT codegen):**
member pointer `bool(__cdecl * pGetRenderCam)(VECTOR3*, MATRIX3*, double*)`, the bind call,
the wrapper `GetRenderCam(...)` (VERIFY it calls `pGetRenderCam` with ALL THREE arguments -
the codegen mis-wire landmine), and the **FIFTH hand-added guard** `CanGetRenderCam()`
(null-checks the bound pointer, invariant-18a style).

**Consumer:** `OroModule::ProjectTrail()` - the trail's projection runs in the RENDER
CALLBACK now (a gc call mid-render is the established CopyResource precedent), state
updates stay in `clbkPostStep`. On a client without (k) the trail falls back to the
post-step camera - degraded (the old jitter), not broken - and logs it one-shot.

**Patch (k2), same session: `gcCore::GetRenderObjPos(OBJHANDLE, VECTOR3*)` - the
render-epoch ANCHOR.** With the camera exact, the trail still sat offset and jittered:
the reconstruction anchor ("body position + planet-relative offset") used the body
position read in `clbkPostStep`, and the RENDERER runs a step of body state ahead of
that hook - measured in-sim as ~2 km of Earth's barycentric motion per frame at 10x
warp. (Related frame lesson, bought the same day: Orbiter's global frame is SOLAR-
SYSTEM BARYCENTRIC - a "stationary" global position recedes from Earth at 29.8 km/s,
so any absolute-position bookkeeping must be planet-relative or it records the path
around the Sun.) The getter returns `vObject::GlobalPos()` for any object with a
visual - refreshed at render start in the same breath as the camera, so it IS the
frame's number. Two edits: the implementation + binder line in `gcCore.cpp` (uses
`Scene::GetVisObject`), the declaration in `gcCore.h`. SDK side identical in shape to
(k): pointer `pGetRenderObjPos`, bind call, wrapper (verify BOTH arguments), and the
SIXTH hand-added guard `CanGetRenderObjPos()`. Consumer: `ProjectTrail` overwrites the
post-step anchors with the renderer's values each frame; falls back to post-step
anchors (degraded, not broken) without (k2). After (k)+(k2), camera, anchor and drawn
vessel all come from ONE epoch source - the renderer - and no ORO-side assumption
about Orbiter's frame ordering remains in the trail pipeline.

## Patch (l): textured, depth-clippable Sketchpad triangles + CPU texture upload

**What it buys:** `gcCore::CreateTrianglesTex` - a triangle poly whose fragments are
**texture × per-vertex Gouraud colour**, with the same optional per-vertex depth (and the
same `0x100` clip bit) as patch (g) - plus `gcCore::UpdateTexture2D`, which fills an
`oapiCreateSurfaceEx(OAPISURFACE_TEXTURE | OAPISURFACE_NOMIPMAPS)` surface from CPU bytes
(no public oapi route can do that at runtime). Consumer: ORO's lightning, whose flash is
a baked cloud-alpha texture - and the planned textured smoke sprites inherit the substrate.

**The insight is that the pad's vertex format already had everything.** `SkpVtx` carries
`nx/ny` (the blit paths' texcoord channel, in TEXELS - the shader multiplies by `gSize`),
the spare `l` that (g) already uses for depth, and a per-vertex `fnc` function switch. What
the stock pad LACKED was a modulate mode: its texture path REPLACES colour with texture
(`sw[TSW] > 0.8`), because blits never needed anything else.

**Files:** `gcCore.h`, `gcCore.cpp`, `D3D9Pad.h`, `D3D9Pad.cpp`, `D3D9Pad2.cpp`,
`shaders/Sketchpad.fx` - ALL shared with earlier patches, so no standalone .patch file;
the hunks:

1. **`Sketchpad.fx`** - one line in `SketchpadPS`'s colour-source selection, AFTER the pen
   and texture lines: `if (frg.sw[TSW] > 0.06f && frg.sw[TSW] < 0.14f) c = t * frg.color;`
   ⚠️ **The band 0.4-0.6 is TAKEN** - `SKPSW_PENCOLOR` is byte `0x80` = 0.502. The modulate
   byte is `0x1A` (0.102); 0x00/0x80/0xFF all miss the 0.06-0.14 test. An unbound texture
   leaves `t = 1` → plain Gouraud, so the mode degrades instead of breaking. The (g) depth
   clip runs LATER in the same PS, so textured + clipped + additive compose for free.
2. **`D3D9Pad.h`** - `#define SKPSW_TEXMODUL 0x0000001A`; `D3D9Triangle` gains
   `LPDIRECT3DTEXTURE9 pTex = NULL`, `SetTex/GetTex`, and
   `UpdateTex(const gcCore::texVtx*, int, const float* pDepth = NULL)`.
3. **`D3D9Pad2.cpp`** - `D3D9Triangle::UpdateTex`: like `Update` but `nx/ny = u/v` and
   `fnc = SKPSW_CENTER | SKPSW_TEXMODUL`.
4. **`D3D9Pad.cpp`** - in `DrawPoly`, BEFORE `Topology(TRIANGLE)`: if the poly is a
   triangle poly with a texture, `bColorKey = false;` then `TexChangeNative(pTex)` - the
   pad's own `SetupDevice` machinery then applies `gTex0/gTexEn/gSize` exactly as for a
   blit. Do NOT set effect uniforms directly; use the pad's state path.
5. **`gcCore.h`** - the `texVtx` struct (`FVECTOR2 pos; float u, v; DWORD color;` - u,v in
   TEXELS), `CreateTrianglesTex(HPOLY, const texVtx*, const float* pDepth, int npt, DWORD
   flags, SURFHANDLE hTex)`, `UpdateTexture2D(SURFHANDLE, const void*, int w, int h)`.
6. **`gcCore.cpp`** - the two implementations (`UpdateTexture2D` = SYSMEM staging texture
   with the DESTINATION's format + full mip count, `LockRect`/memcpy/`UpdateTexture`;
   destination must NOT be a rendertarget) + the two `gcBindCoreMethod` entries.

**Contract:** the texture SURFHANDLE must outlive the poly (no ref held). Upload is
main-thread only (resource op). Guard: `CanDrawTexPoly()` - checks BOTH bound pointers, so
a half-present client reports unsupported.

⚠️ **gcCoreAPI.h wrappers for `CreateTrianglesTex` are exactly the multi-pointer class the
codegen mis-wired for (g)** (landmine 3). The SDK copy is hand-maintained; after ANY
regeneration read the wrapper body and check it calls `pCreateTrianglesTex` with ALL SIX
arguments.

## Patch (m): night clouds (shader + one gate; the fix ORO's lightning exposed)

**What it buys:** clouds exist on the night side, seen from above - as dark veils that
dim and blot the city lights beneath them, the way real decks read from orbit (night
storms are conspicuous as HOLES in the city-light field). Stock loses the phenomenon
entirely, through THREE independent behaviours that also make each other undiagnosable:

1. `CloudPS` (from-above branch) returns `alpha = cTex.a * cAmb.a * cAmb.a`, where
   `AmbientApprox().a` is a twilight ramp with NO floor - past the terminator the layer
   renders at alpha EXACTLY 0 (drawn, but invisible).
2. `Surfmgr2.cpp` binds cloud tiles to surface tiles only where `sdist < PI05 + rad`
   (day side - shadows were their only consumer), while night lights switch on at
   `sdist > 1.35` - so the terrain shader is blind to cloud exactly where lights render.
3. Orbital night lights are `cMsk.rgb * CamSpace * 4.0` - **4× overbright** into the fp16
   chain, then bloomed. Even a 74%-alpha veil leaves a 4× source at ~1.04× = still
   saturated; no overlay can dim them. (Diagnosed against a live session: cloud map
   coverage 0.74 over Dallas, lights crisp.)

**Files:** `shaders/NewPlanet.hlsl` (both changes below), `Surfmgr2.cpp` (one line).

1. **`NewPlanet.hlsl`, `CloudPS`** - the veil: `#define ORO_NIGHT_CLOUD 0.45f` and the
   from-above return becomes
   `fNight = ORO_NIGHT_CLOUD + (1 - ORO_NIGHT_CLOUD) * cAmb.a * cAmb.a;` →
   `alpha = cTex.a * fNight`. A LERP, not a max: day side is EXACTLY 1 (unchanged look),
   no slope kink at the terminator. RGB stays sun-lit (≈ black at night) - the veil is
   darkness, which is correct.
2. **`NewPlanet.hlsl`, terrain `cNgt` block** - the blot, inside `_NIGHTLIGHTS` +
   `_CLOUDSHD`: `fCld = (vUVCld.x < 1.0 ? fChA : fChB)` (the day-shadow sample, already
   computed) → `cNgt *= 1 - fCld*0.92; cNgt2 *= 1 - fCld*0.92;`. The 8% leak is
   deliberate - the diffuse glow real cities show through cloud.
3. **`Surfmgr2.cpp`** - `has_shadows = (render_shadows && (sdist < (PI05 + rad) ||
   has_lights));` - bind the cloud tiles for night tiles too. Day side: nothing changes
   (the shadow term only bites where sun terms are nonzero).

**Deploy note (the landmine):** `NewPlanet.hlsl` is a DEPLOYED, runtime-compiled file -
copy it to `Modules\D3D9Client\` like `Sketchpad.fx`, or the DLL and its shaders skew.
`ORO_NIGHT_CLOUD` in the DEPLOYED copy is the user's live knob (edit + restart session,
no rebuild); stock backup saved beside it as `NewPlanet.hlsl.pre-m`. The client's shader
CACHE (`Cache/D3D9Client/Shaders/*.bin`) is mtime-checked against the source, so edits
invalidate it correctly - no manual cache clearing needed.

**Upstream-report material:** behaviours 1-3 are visible in STOCK with no addon involved -
fly the terminator at night with cloud data present: decks vanish, city lights shine
through overcast. Same class as the patch-(f) part-2 canopy finding.

## Patch (n): per-vessel STOCK EXHAUST suppression (billboards + streams)

**What it buys.** ORO's plume-expansion overlay (2026-08-09) draws its own exhaust
visuals on top of stock's. To JUDGE the overlay - and eventually to replace stock's
fixed-at-every-altitude billboard outright - the stock exhaust must be switchable per
vessel, and it is not: `GetExhaustSpec` copies out (no EditExhaust exists), a
Del/AddExhaust rewrite churns indices the vessel's own code may hold (retractable
engines, damage models - the roadmap's assessed verdict), and exhaust PARTICLE
streams are worse: `DelExhaustStream` needs the `PSTREAM_HANDLE` only the creating
vessel ever received, and nothing enumerates them (the airfoil story again). So the
render side is gated in the client, exactly like (c)+(e) for the reentry family.

**Touches 4 files, all mirroring (c)/(e):**
1. `gcCore.h` - the `gc_interface` declaration `SuppressExhaust(OBJHANDLE, bool)`,
   beside `SuppressReentry`.
2. `gcCore.cpp` - the binder line (`"SuppressExhaust"` -> `&gcCore2::SuppressExhaust`),
   a second `std::set<OBJHANDLE> g_gcExhaustSuppressed`, the setter, and the
   `gcIsExhaustSuppressed` query (extern'd where used - the no-header-churn pattern).
3. `VVessel.cpp` - one `extern` + one `if` at the top of `vVessel::RenderExhaust`
   (`return true` = "nothing to do"); `ExhaustLength` stays 0 for a suppressed vessel.
   The gate lives in vVessel so BOTH Scene call sites (main pass, custom-camera pass)
   are covered.
4. `Particle.cpp` - one `extern` + `!gcIsExhaustSuppressed(hRef)` in
   `ExhaustStream::Update`'s emission condition. EMISSION only, exactly (e)'s rule:
   in-flight particles expire naturally, and the suppressed branch falls into the
   existing `else t0 = simt`, so lifting suppression cannot release a backlog burst.
   Covers exhaust smoke AND contrails (both are `ExhaustStream`); reentry and custom
   streams untouched.

**Addon side:** hand-add to the SDK header (landmine 3): the `pSuppressExhaust`
pointer, its constructor bind, the `SuppressExhaust` wrapper (calls its OWN pointer
with BOTH arguments - landmine 3b), and the EIGHTH guard `CanSuppressExhaust()`.
ORO consumes it as the THRUSTER tab's STOCK EXHAUST pill: pushed on CHANGE only,
camera-target vessel, returned on every exit path (pill on, disarm/Ctrl+G, target
switch, `clbkDeleteVessel`, simulation end, destructor - `ReleaseStockExhaust` in
`OroPlume.cpp`). On an UNPATCHED client the binder logs
`ERROR:gcCoreAPI: Function [SuppressExhaust] failed to bind` once at init - harmless,
the guard null-checks, the pill greys out.

**Upstream-report note:** unlike (c), this one is NOT "your client ignores a
documented API" - the core never had an exhaust-hiding API at all. It is a
capability request, same class as (b)/(g): the client is the only place the gate
can live, because only it renders another vessel's exhaust.

## Patch (o): a LATCH marking new streams exempt from patch (n)

**What it buys:** `gcCore::ExemptNewStreams(bool)` - raise it, create your exhaust
streams, lower it, and those streams keep emitting on a vessel whose stock exhaust
streams you are suppressing with (n). Without it an addon cannot REPLACE the stock
streams, only add to them: the replacement is suppressed alongside the thing it
replaces.

⚠️ **Two findings from the client made this necessary, and both are worth knowing.**

**1. `AddParticleStream` is dead under D3D9Client.** `clbkCreateParticleStream` is
unimplemented - it logs `UnImplemented Feature Used clbkCreateParticleStream` and
returns NULL (`D3D9Client.cpp:1383`), so `VESSEL::AddParticleStream` yields nothing.
Only the exhaust and reentry factories are real. The obvious dodge - "create a plain
ParticleStream, which (n)'s `ExhaustStream::Update` gate never sees" - therefore does
not exist: **every usable stream is an ExhaustStream, and every ExhaustStream is
gated.** ORO's PARTICLES tab was written against `AddParticleStream` first and would
have produced absolutely nothing, with no error visible in the sim.

**2. A set of stream POINTERS does not work, and this cost a fly-and-report round.**
The first version of (o) kept `std::set<PSTREAM_HANDLE>` and tested
`gcIsStreamExempt(this)` in the gate. The exemptions were registered (ORO's caption
confirmed the binding) and the lookup still missed, because
`class D3D9ParticleStream : public oapi::ParticleStream, public D3D9Effect` -
**two base classes.** The addon holds a `ParticleStream*`; the gate runs with an
`ExhaustStream*`; matching them is a bet on base-subobject offsets.
**A LATCH read at CONSTRUCTION avoids the question entirely** - each stream stamps a
plain `bool bExempt` member - and it also means the addon has nothing to clean up,
because a deleted stream takes its exemption with it.

**Files:** `gcCore.h`, `gcCore.cpp`, `Particle.h`, `Particle.cpp`; the hunks:

1. **`gcCore.h`** - `gc_interface void ExemptNewStreams(bool bExempt);`
2. **`gcCore.cpp`** - `static bool g_gcExemptLatch;`, the setter
   `gcCore::ExemptNewStreams`, the reader `bool gcExemptLatch()`, and the bind line
   `if (strcmp(name,"ExemptNewStreams")==0) *ppFnc = &gcCore2::ExemptNewStreams;`
3. **`Particle.h`** - `ExhaustStream` gains a private `bool bExempt;`
4. **`Particle.cpp`** - `extern bool gcExemptLatch();` above the constructors, both
   `ExhaustStream::ExhaustStream` bodies gain `bExempt = gcExemptLatch();`, and the
   emission gate becomes
   `... && (!gcIsExhaustStreamSuppressed(hRef) || bExempt) && ...`.

**SDK header:** `gcCoreAPI.h` gains `pExemptNewStreams`, its bind, the HAND-WRITTEN
wrapper, and the **NINTH hand-added guard** `CanExemptStream()`.

**Contract:** lower the latch after creating - ORO does it unconditionally after its
creation loop, so an early `continue` cannot leave it raised.

## Patch (n) addendum: the BILLBOARD / STREAM split (2026-08-09)

(n) originally suppressed billboards and exhaust particle streams under ONE bool. That
is wrong whenever an addon replaces only one of them - you had to turn off the flame to
adjust the smoke. The signature is now `SuppressExhaust(OBJHANDLE, DWORD flags)` over
`GCEXH_BILLBOARD 0x1` and `GCEXH_STREAM 0x2` (defined in `gcCore.h` and mirrored in
`gcCoreAPI.h`), backed by a `std::map<OBJHANDLE,DWORD>` instead of a set, and asked two
separate questions: `gcIsExhaustSuppressed` (billboards, `vVessel::RenderExhaust`) and
`gcIsExhaustStreamSuppressed` (emission, `ExhaustStream::Update`). ORO drives the two
bits from two separate pills on two separate sub-tabs.

## Patch (p): VC SHADOW DEPTH - let the shadow take the AMBIENT too

**What it buys:** a third argument on `gcCore::SetVCShadows(bool, float radius, float
depth)`, and with it the only lever that can actually make a virtual-cockpit shadow
look dark.

**The problem, which is not obvious and is not tunable from outside.** Self-shadowing
multiplies the SUN term and nothing else:

```hlsl
cSun.rgb *= ComputeShadow(frg.shdH, dLN, sc);
cTex.rgb *= saturate(Base + gMtrl.diffuse.rgb * Light_fx(cDiffLocal + cSun * dLN));
//                   ^^^^ = gMtrl.ambient*gSun.Ambient + gMtrl.emissive
```

So a fully shadowed pixel still receives the material's **ambient** and **emissive**.
On an exterior hull the sun dominates and the shadow reads fine; a VC is authored with
generous ambient and emissive so the panels stay readable, so the same shadow lands on
a bright floor and reads as a faint grey smudge. Lowering the Launchpad `AmbientLevel`
does not reach `gMtrl.ambient`, and nothing else outside the shader does either.

**(p) scales the AMBIENT share by the shadow factor, by a user-set fraction.**
⚠️ **EMISSIVE IS DELIBERATELY NEVER SCALED** (the author's call): a lit instrument panel
does not care what is between it and the sun, and dimming MFDs as a canopy shadow
sweeps across them reads as a bug, not as realism.

**Files:**
1. **`shaders/D3D9Client.fx`** - `uniform extern float gVCShdDepth;` beside `gMix`.
   One declaration serves all three vessel shaders: `D3D9Client.fx` includes
   `Vessel.fx`, which includes `Common.hlsl`, `PBR.fx` AND `Metalness.fx`, so they are
   one effect with one parameter table.
2. **`shaders/Vessel.fx`** - capture the shadow factor, then
   `Base -= (gMtrl.ambient.rgb*gSun.Ambient) * ((1-fShd) * gVCShdDepth);`
   Base is (ambient + emissive), so subtracting at most the ambient part can never go
   negative and never eats the emissive.
3. **`shaders/PBR.fx`** - two sites (the baked-diffuse path and the second lighting
   path, which already had `fShadow` in hand); `fAmbShd = lerp(1, fShd, gVCShdDepth)`.
4. **`shaders/Metalness.fx`** - same `fAmbShd`, applied to its richer `cAmbient`.
5. **`D3D9Effect.h/.cpp`** - the `eVCShdDepth` handle + `GetParameterByName`.
6. **`Scene.cpp`** - set the uniform from `g_gcVCShadowDep` immediately before
   `vFocus->Render(pDevice, true)` (the cockpit stage) and back to 0.0f immediately
   after. **This gate is load-bearing**: without it every exterior vessel's
   self-shadowing changes too, which is not the addon's call to make.
7. **`gcCore.h/.cpp`** - the third argument and `g_gcVCShadowDep`, clamped to 0..1
   (outside that range the subtraction either brightens the shadow or goes negative).

**SDK header:** `gcCoreAPI.h` - the wrapper and `pSetVCShadows` both gain the argument.
No new guard: `CanSetVCShadows` already covers it, and depth is meaningless without (f).

⚠️ **FOUR SHADERS ARE DEPLOYED FILES.** `D3D9Client.fx`, `Vessel.fx`, `PBR.fx` and
`Metalness.fx` are compiled at RUNTIME from `Modules\D3D9Client\`, so copy all four
after editing the source tree or the DLL and its shaders skew. Compile-check first -
the client's own macro set is what makes it build standalone:
`fxc /T fx_2_0 /DANISOTROPY_MACRO=4 /DLMODE=1 /DMAX_LIGHTS=12 /DSHDMAP=3 /DKERNEL_SIZE=27 /DKERNEL_WEIGHT=0.0285 D3D9Client.fx`

**Default 0.0 = bit-for-bit stock**, so the patch is inert until an addon asks for it.

## Patch (q): the reload Clear storm (a STOCK 2024 bug, second of its kind after (j))

**One line, no API, nothing in ORO depends on it.** It exists so the log is trustworthy,
which started to matter once beta testers began reading their own logs.

**Symptom:** ~30 `D3D9ERROR ... Scene.cpp Line:1258 ... pDevice->Clear(...)` lines
(`D3DERR_INVALIDCALL`, -2005530516) on EVERY scenario reload. **Reproducible with no addon
loaded at all** - it is the "focus vessel has no visual yet" path, which every reload takes.

**Cause:** `Scene::RenderMainScene`'s `!UpdateCamVis()` early-out asks for
`D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL` before any depth-stencil surface is
bound, so the whole call fails - which also means the black loading screen the line is
there to paint never gets painted.

**Fix** (`OVP/D3D9Client/Scene.cpp`, in `RenderMainScene`):
```cpp
    if (!UpdateCamVis()) {
        if (SUCCEEDED(gc->BeginScene())) {
-           HR(pDevice->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0L));
+           HR(pDevice->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0L));
            gc->EndScene();
        }
        return; // Scene not yet properly inilialized, return
    }
```

**Verified NOT ours before changing it:** line 1258 is outside all nine ORO hunks in that
file, and the only patch anywhere in that call chain is (k)'s one-line `Camera.grot = grot;`.

⚠️ **A SECOND STOCK BUG FOUND THE SAME DAY, NOT FIXED HERE — `BuildDate()` is dead code in
Release.** `D3D9Util.cpp` parses `__DATE__` inside an `assert`, and `NDEBUG` removes an
assert ALONG WITH ITS ARGUMENT, so the parse never runs and the function returns
uninitialised locals. Confirmed by inspecting the built object file: no date string is
emitted into `D3D9Util.cpp.obj` at all. This is very likely the real cause of
`gcCore::GetSystemSpecs().gcAPIVer` reading 0 - a number that was never computed explains
it more simply than one lost crossing the call boundary. Worth reporting upstream with (q).

## Patch (r): the EMISSIVE OVERDRIVE (a stock limitation, and it is SHADER-ONLY)

`PBR.fx` + `Vessel.fx`, two lines each. **No DLL rebuild — both are runtime-compiled
deployed files, so this is edit, copy, restart Orbiter.**

**The problem.** Both vessel shaders fold the material emissive into the light term and
then clamp it:

```
float3 diffBaked = Light_fx(gMtrl.diffuse.rgb * (...) + gMtrl.emissive.rgb + ...);
cDiff.rgb *= diffBaked;                       // PBR.fx
```

...and `Light_fx()` in `Common.hlsl` is `return saturate(x);` — a plain clamp (the soft
rolloff beside it is commented out). So **everything a material emissive carries past 1.0
is discarded**, and because the term MULTIPLIES the texture, a surface can never be
brighter than its own texture. Three consequences, all of which ORO hit at once:

- ORO's bell glow drives emissive to `1.45 x trim x 2.2`, i.e. ~3.19 at trim 1.0. Every
  channel clamps to 1.0, so **trim 0.3 and trim 2.0 render identically** and the slider is
  dead across most of its range.
- All three channels clamping together means the result is **untinted** — the heat ramp's
  colour, and the per-class Bell colour pick, are erased at high heat. What you see is the
  raw texture.
- Nothing ever exceeds 1.0, so **it can never reach the light-glow (bloom) pass**. Invariant
  23(g) claimed "the halo IS the glow"; that was false in this shader path, and this patch
  is what makes it true.

**The fix.** Re-apply the excess as an ADDITIVE term, modulating the albedo:

```
float3 cAlbedo = cDiff.rgb;                   // before the lighting multiply
cDiff.rgb *= diffBaked;
cDiff.rgb += cAlbedo * max(gMtrl.emissive.rgb - 1.0f, 0.0f);
```

⚠️ **STOCK CONTENT CANNOT MOVE.** `max()` is exactly zero for any emissive at or below 1.0,
which is everything an authored mesh carries — so this is a no-op for the entire stock
install and only a material *deliberately* driven past 1.0 sees any change. Modulating by
the albedo (rather than adding flat) keeps a texture's dark areas dark, so ORO's bell
banding survives the overdrive instead of being washed out.

**Which shader actually matters:** `Mesh.cpp` sets `Grp[g].Shader = SHADER_PBR` for every
group by default, so a plain textured mesh takes **PBR.fx** — `Vessel.fx` is
`SHADER_LEGACY` and only runs where a mesh asks for it. Both are patched so behaviour does
not depend on which path a mesh happens to take. **`Metalness.fx` is deliberately NOT
patched**: it routes emissive through `LightFXSq` with squared terms, it only engages when
a texture carries a metalness map, and nothing in ORO uses it.

**Report this upstream with (q) and the `BuildDate()` bug** — it is a limitation rather
than a crash, but it means no addon can make anything glow past its own texture, which is
a real constraint on any future heat/incandescence effect.

## Version marker

⚠️ **THE STAMP RE-BAKES FROM `D3D9Client.cpp`, NOT `D3D9Util.cpp` — this file said Util
until 2026-08-12 and it was WRONG.** `D3D9Util.cpp`'s `BuildDate()` *looks* like the source
(it parses `__DATE__` by name) but it feeds `SystemSpecs`, not the loader line, and in
Release it is dead code anyway (see patch (q)'s addendum). The `[Build ######]` line comes
from the SDK's module-version glue, which bakes `__DATE__` into `D3D9Client.cpp.obj`.
**Force-touch `D3D9Client.cpp` after any client patch, then VERIFY** by scanning the built
DLL for a `"Mmm DD YYYY"` string and confirming it is today's — the check takes seconds and
is what caught this. With (q) the stamp is **`260812`**; with (s) it is **`260820`**; with
**(t) it is `260824`**. (t) needs no stamp either: it changes no API, and its effect is
visible at a glance - the menu bar stays crisp under a full-frame effect.

The patched client logs `[Build YYMMDD, API YYMMDD]` (compile date) in Orbiter.log vs the
stock `[Build 241231, API 241231]`. Patch (a) alone was `260725`; +patch (b) was `260726`;
the client with a+b+c+d is **`260801`**; with (f) it is **`260804`**; with (g) it is
**`260807`**; with (i)+(j) it is **`260808`**; **(k) is ALSO `260808`** - the stamp cannot
distinguish it from (i)+(j) (same-day builds; the stamp re-bakes only when D3D9Client.cpp
recompiles), but unlike (e) it needs no stamp: **`CanGetRenderCam()` probes it by binding**.
With (l)+(m) the stamp is **`260809`**; (l) probes by binding (`CanDrawTexPoly()`), and (m)
has no binding to probe - it is a look change with no API - but its shader half announces
itself: `ORO_NIGHT_CLOUD` greps in the deployed `NewPlanet.hlsl`. **(n) is ALSO `260809`**
(same-day build, the stamp cannot tell it from (l)+(m)) - but like (k) it needs no stamp:
`CanSuppressExhaust()` probes it by binding, and ORO logs
`stock exhaust suppression (patch n) available` at session start.
(e) was built the SAME DAY as a+b+c+d - `260801` does
NOT distinguish a client with (e) from one without it. If the puffs ever reappear on a
supposedly-patched client, suspect a stale DLL and rebuild - there is no runtime probe for (e).
`gcCore::GetSystemSpecs().gcAPIVer` returns the same number - ORO can version-gate on it
once a fixed client ships officially (see the patch-(b) follow-up above).

## Patch (s): SURFACE WEATHER - wet ground, storm light, wet hulls, the planar mirror (2026-08-22)

The largest patch in the set, in SEVEN parts, built for ORO's rain. One gcCore surface:
`SetSurfaceWetness(k)`, `SetStormLight(k)`, `SetWetDarkness(k)`, `SetWetGlint(k)` and
`SetWetReflection(gain, swimAmp, swimRate, poolSize, poolReach)` - all clamped 0..2,
all probed BY BINDING (`CanSetSurfaceWetness` etc. in the hand-maintained gcCoreAPI.h),
all defaulting to stock behaviour so an unpatched or disarmed client cannot move.

- **Part 1, wetness** (`gSurfWet`): the ground darkens and takes a Fresnel sheen in BOTH
  ground shaders - `Mesh.fx` BaseTilePS (runways) AND `NewPlanet.hlsl` TerrainPS (the
  apron a vessel actually parks on; the first build patched only the tiles and the
  readback said "stored perfectly" while the pixels never saw it). Hulls wet in every
  vessel path: PBR.fx, its FAST_PS, Vessel.fx, Metalness.fx, NewMesh.hlsl - the second
  DeltaGlider stayed dry until a path-tint diagnostic named Metalness as the unpatched one.
- **Part 2, storm light** (`gStorm`): the directional sun collapses AT THE SOURCE and the
  ambient lifts, in the ground shaders, cloud shadows, the sun glint, vessel shadows and
  the glare pass - an overcast, not a brightness knob. Plus exponential storm fog.
- **Part 3, wet darkening gain** (`gWetDark`): how far wet albedo drops; user slider.
- **Part 4, the rain clock** (`gWetTime`): a PAUSE-GATED accumulator fed per frame from
  Scene.cpp - the drop glint danced on a paused sim when it rode raw system time.
- **Part 5, the drop glint** (`gWetGlint` + `WetSparkle()` in D3D9Client.fx): a
  lifecycled sparkle field on every wet hull, applied AFTER the light bake and scaled by
  the SKY ambient - a sparkle hung on the sun cannot exist in the weather that wets things.
- **Part 6, the planar mirror**: before the main scene, vessels within 1.5 km re-render
  through a ground-mirrored camera into a half-res RT (`ptWetRefl`), which both ground
  shaders sample at each wet pixel's own screen position. THREE landmines, each one round:
  the mirror flips winding and `Mesh.cpp` re-sets cull per GROUP, so a cull-mode override
  dies - the fix is a DOUBLE mirror (reflection matrix x clip-space X flip, undone by
  `ruv.x = 1 - ruv.x` at sample time); meshes trust the STORED gVP (set once per camera
  update), so the mirrored camera must go through `D3D9Effect::SetViewProjMatrix` and be
  restored after; and the sampler must be NULL-unbound before the RT push or the device
  refuses. `gWetReflPrm` = (1/W, 1/H, gain, live); `gWetSwimPrm` = (swim amp, swim rate,
  pool size, pool reach) - the ripple warp and the standing-pool controls.
- **Part 7, standing pools** (shader-only, in the two ground wet blocks): a three-scale
  sine lattice in the water-microtexture UV mapping (LOD-continuous by construction, so
  pools are pinned to the ground). WARNING: EVERY SINUSOID COMPLETES AN INTEGER NUMBER OF
  CYCLES PER UV UNIT (TAU x quantized count): tile UVs agree with their neighbours only
  MODULO 1, so any fractional-cycle lattice jumps phase at tile boundaries - the seam is
  visible and was screenshotted. Integer cycles make a mod-1 jump land on the same value.

**Added 2026-08-23**: `SetWetReflection` grew to five arguments (swim amplitude/rate +
pool size/reach, packed as `gWetSwimPrm`), a sixth setter `SetWetGrain(opacity, size)`
(`gWetGrainPrm` - the pool-grain value noise in both ground shaders), the planar
mirror PLANE anchored to the ground under the FOCUS VESSEL rather than under the
camera (terrain undulation made the reflection bob as the view orbited), and the
stencil ground-shadow storm fade corrected: `RenderGroundShadow`'s parameter is an
INVERSE alpha (drawn opacity = 1 - depth), so the original `depth *= (1-storm)` was
driving non-focus vessels' shadows to FULL BLACK under storm - the fade must push
depth toward one. Patch (l) also gained a WRAP sampler state around textured-poly
draws (D3D9Pad2.cpp, D3D9Triangle::Draw) - the pad's CLAMP is right for blits and
wrong for a world-tiling texture like the rain's cloud deck; restored after each draw.

Files: gcCore.h/.cpp, D3D9Effect.h/.cpp, Scene.h/.cpp, Surfmgr2.cpp, VVessel.cpp, plus
the DEPLOYED shaders D3D9Client.fx, Mesh.fx, PBR.fx, Vessel.fx, Metalness.fx,
NewPlanet.hlsl. The shader halves are runtime-compiled - edit, copy, restart - but the
C++ half needs the DLL rebuild. WARNING: NewPlanet.hlsl also carries patch (m)'s tuned
`ORO_NIGHT_CLOUD 0.5f` - verify it on every deploy.
⚠️ **THE DEPLOYED SET IS EIGHT FILES NOW** (with Sketchpad.fx from patches d/g/l and
NewMesh.hlsl from patch (h) part 2), and
`Mesh.fx` is the one every ship-list forgot: it joined with (s)'s base-tile ground work
(a wet RUNWAY is a base tile), was live in the sim from 2026-08-22, and was absent from
the staging list, the stock restore bundle and the installer's backup loops until the
260823 release audit hash-compared clone against deployed. **Audit all eight on every
release: the clone, the deployed copies and `upstream/stock` must agree.**

**EXTENDED 2026-08-23 - THE COCKPIT INTERIOR IS DRY.** The VC is rendered with the same
vessel shaders, so the wet-hull work put drop glint and wet sheen on the instrument
panel. Scene.cpp's cockpit-pass bracket (the same one patch (p) uses for shadow depth)
now zeroes `gWetGlint` AND `gSurfWet` around `vFocus->Render(pDevice, true)` and
restores them after - the interior is dry while every hull seen THROUGH the window
keeps its full wet look. DLL-only; no shader change.

**EXTENDED 2026-08-24 - BEACONS IN THE REFLECTION.** A public-beta tester asked for nav
lights and strobes in the wet-ground mirror. They were never being lost or clipped: the
mirror pass only ever called `vVessel::Render`, and beacons are not part of it - the main
scene draws them in a separate later loop. One extra loop in the pass, and NO camera
plumbing, which is the point worth recording: `vObject::RenderSpot` builds its billboard
from the object's CAMERA-RELATIVE POSITION and draws through the same `gVP` the pass
already overrides, so a MATRIX-ONLY mirror carries it. The blob ends up facing the real
camera rather than the mirrored one - a tilt of ~11 deg for a camera 3 m up and a vessel
30 m away, i.e. a round blob at 98% width, which a soft spot absorbs completely. A second
loop rather than one, matching the main scene, so a beacon composites over every hull in
the reflection and not just its own; strobe phase is `fmod(simt, period)`, so both passes
agree within the frame. ⚠️ `vVessel::RenderExhaust` has the same billboard property and
would drop in the same way, but its first line is `gcIsExhaustSuppressed()` - patch (n) -
so it draws NOTHING for any user running ORO's own plume. And ORO's own plume can never
arrive by this route at all: it is screen-space Sketchpad geometry drawn to the backbuffer
after the scene, so reflecting it needs a render-proc slot INSIDE this pass plus a second
geometry build against the mirrored camera. Same shape of problem as patch (t) - a missing
slot, not a missing calculation. ✅ **BUILT 2026-08-25 as patch (u)**, and the "second
geometry build" turned out to need no second CAMERA path at all - see below.

**EXTENDED AGAIN 2026-08-25 - EXHAUST, PARTICLES, REFLECTION BLUR AND POOLS-OFF.**
Three more, all following from the beacon note above. **Stock exhaust and particle streams
join the mirror pass**: `vVessel::RenderExhaust` orients from `cdir` (the camera's position
in VESSEL frame) and `D3D9ParticleStream::RenderDiffuse` builds every sprite from
`p->pos - camera_gpos`, so both are CAMERA-RELATIVE like `RenderSpot` and both draw through
the gVP the pass already overrides - a matrix-only mirror carries them. Order matches the
main scene (exhausts, beacons, streams), because these are additive layers and the order
they accumulate in is the look. ⚠️ `RenderExhaust`'s first line is `gcIsExhaustSuppressed()`,
patch (n), so it draws nothing for anyone running ORO's own plume; what it restores in
practice is the CONTRAIL. (⚠️ ORO's own jet arrived a day later, and needed a slot rather
than a loop - see patch (u).) Streams go in whole - they are scene-owned and carry no cheap
distance handle - exactly as the main scene and `RenderSecondaryScene` do, and this only
runs while the ground is wet and the camera is under 250 m AGL.
**`SetWetReflection` gains a SIXTH argument, fBlur** - how diffuse the reflected image is,
riding `gWetGrainPrm.z` because `gWetReflPrm`'s four channels (1/W, 1/H, gain, live) are all
taken. That vector is a TRANSPORT, not a grouping; the signpost is in Scene.cpp, since
anyone hunting the blur will look in gWetReflPrm first. Both ground shaders widen the
existing 3-tap vertical smear, and **at 0 the taps and weights are exactly as shipped** with
the widening behind a uniform-driven branch, so it costs nothing while the slider is down.
⚠️ **AND IT WAS HALF-WIRED ON THE FIRST BUILD, IN THE PLACE THIS FILE ALREADY WARNS ABOUT.**
The blur was pushed only through Scene.cpp's `D3D9Effect` path, which serves the BASE TILES,
while the ground a vessel parks on is TERRAIN, pushed from `Surfmgr2.cpp`. Right value,
wrong shader - the same sweep patch (s)'s own round 1 records. **ANY NEW WET PARAMETER MUST
BE PUSHED IN BOTH PLACES OR IT IS SILENTLY HALF-WIRED**; the rule is now written at the
Surfmgr2 block itself. Swept afterwards: exactly two push sites, two sampling files.
**`SetWetPoolSize` may now go NEGATIVE, to -0.1** - the only one of the six wet setters with
a meaning below its range. Pool size only ever set the lattice SCALE and the shaders floor
it at `1/max(0.35, z)`, so a slider at zero still produced a fine mesh of small pools; the
ground shaders now fade standing water out across -0.1..0, which lets the addon offer a wet
apron with no pools while the user's slider still reads a plain 0..2.

## Patch (t): THE CHROME GOES LAST - Orbiter's menu bar stops being an effect surface (2026-08-24)

Reported by a public-beta tester and by the author independently: with a full-frame effect
running - blur, swim, chromatic aberration, a colour wash, and equally the rain sheet or
the plasma - Orbiter's own menu bar and info bars are smeared along with the world, which
makes the UI hard to read and to use. DLL-only, three files, no shader change, no new API.

**THE CAUSE IS AN ORDERING ONE, AND IT IS THE CORE'S, NOT THE CLIENT'S.**
`Scene::RenderMainScene` runs the overlay stage as

```
PushRenderTarget(backbuffer, RENDERPASS_MAINOVERLAY)
    RENDERPROC_HUD_1ST
    gc->Render2DOverlay()        // -> the core's Pane::Render()
    RENDERPROC_HUD_2ND
```

and `Pane::Render()` (Src/Orbiter/Pane.cpp) draws the PILOT'S INSTRUMENTS (HUD, 2D panel,
glass-cockpit MFDs) and then `mibar->Render()` - the USER'S CHROME - in one uninterruptible
core call. So an addon overlay has no slot between the two: it must draw under the pilot's
instruments or over the user's menu bar. ORO draws at HUD_2ND, hence the smear.

⚠️ **THE OBVIOUS FIX IS NOT AVAILABLE.** Splitting that call means editing `Pane::Render`
or `GraphicsClient::Render2DOverlay`, both of which live in the CORE and compile into
Orbiter.exe. ORO patches the client only; shipping a patched Orbiter executable is a
different distribution proposition entirely and was rejected. The client cannot reorder
what it cannot see.

⚠️ **AND THE CHEAP FIX TRADES ONE WRONGNESS FOR ANOTHER.** Moving the addon draw to
RENDERPROC_HUD_1ST does clean the chrome, because the pane then draws after it - but then
EVERYTHING the pane draws becomes immune to the effects. In the VC that costs nothing (the
VC's HUD is rendered into the VC's own HUD surface in `Pane::Update` and is part of the 3D
scene, so it keeps receiving the effects; in VC mode the pane's overlay contains only the
bars). In 2D-panel and glass-cockpit modes it is a visible regression: a blackout would
darken the outside view through the windows while the panel stayed lit.

**SO THE CHROME IS DEFERRED INSTEAD, AND IT IS IDENTIFIED BY SURFACE IDENTITY.** This is
the part that makes the patch small and safe. Every bar - the menu bar, the warp
mini-readout, the action flag, and both auxiliary info bars - is drawn from ONE surface,
loaded as `"main_menu_tgt.dds"` at `MenuInfoBar.cpp:429`, and `MenuInfoBar` is that file's
only consumer in the entire core (`ExtraInfoBar` takes `infoTgt = mibar->menuTgt`). No
guessing from screen position, draw order, or which overload was called:

- `D3D9Client::clbkLoadSurface` tags the handle when the filename matches; `clbkReleaseSurface`
  clears the tag at the point of `delete`, so a later allocation landing on the same address
  can never be mistaken for the chrome.
- `D3D9Client::clbkRender2DPanel` captures a matching draw instead of executing it, but only
  while armed. Unarmed - i.e. everywhere else in the client - nothing changes at all.
- `Scene.cpp` arms with `ChromeDeferBegin()` before `Render2DOverlay()` and replays with
  `ChromeDeferFlush()` after the HUD_2ND call, still inside the same render-target bracket
  (which runs to the `PopRenderTargets()` ~180 lines later, so there is room).

**THREE DETAILS THAT ARE LOAD-BEARING:**

1. ⚠️ **THE TRANSFORM IS COPIED BY VALUE.** `MenuInfoBar::Render` reuses ONE `transf` and
   rewrites it between its own draws (it undoes the x-squeeze for the mini-readout and the
   flag, then puts it back). `clbkRender2DPanel` reads it through a pointer, so a deferral
   that stored the pointer would replay all three bars with whichever transform happened to
   be there last.
2. ⚠️ **THE FLUSH IS UNCONDITIONAL, OUTSIDE THE `if (pSketch)` GUARD.** Put it inside and a
   frame that fails to get a pooled sketchpad loses its menu bar - a far worse failure than
   the one being fixed. For the same reason `ChromeDeferBegin()` clears the queue rather
   than trusting the previous frame to have emptied it, and a full queue falls through and
   draws immediately instead of dropping a bar.
3. `ChromeDeferFlush()` lowers the arm flag FIRST, because the replay re-enters
   `clbkRender2DPanel` and would otherwise capture the same draws forever.

**WHAT ELSE THIS CHANGES:** Orbiter's bars now sit on top of ANY addon's HUD_2ND drawing in
this client, not just ORO's. That is the intended default - the chrome is Orbiter's own UI -
but it is a behaviour difference a third-party addon could notice. ORO itself needed no
change whatsoever, which is why the fix cannot regress any approved look.

**FOR THE VULKAN REQUIREMENTS DOCUMENT:** the underlying request is one line - *an addon
overlay needs a slot between the pilot's instruments and the user's chrome.* Today the two
available slots straddle both, and an addon that draws over the world necessarily draws
over the UI.

## Patch (u): A RENDER-PROC SLOT INSIDE THE WET MIRROR - the reflection gets ORO's plume (2026-08-25)

His ask, straight after the exhaust/particle extension landed: the reflection shows the
hull and now the contrail, but not ORO's own jet. Three files, DLL-only, no shader change,
one new render-proc id. It is one of the smallest patches in the set and the one with the
highest ratio of comment to code, because every part of it is an argument about *why no
new API was needed*.

**WHY A SLOT AND NOT A LOOP.** Everything the mirror pass gained in the two previous
rounds - meshes, exhaust billboards, beacons, particle streams - is geometry the CLIENT
draws, oriented from a camera-relative position and pushed through the view-projection the
pass already overrides. One loop each, no plumbing. ORO's plume is none of those things:
it is screen-space Sketchpad triangles projected on the CPU and drawn AFTER the whole
scene, in patch (i)'s pre-resolve slot, by which time this pass is long finished. There is
no loop to add. It needs to be invited in.

```cpp
// gcCore.h
#define RENDERPROC_WET_MIRROR   0x0007
```

**THE THREE PARTS**

1. **`gcCore.h`** - the new id, with a doc comment that carries the three facts a consumer
   cannot guess: the bound target is the HALF-RES reflection texture and not the
   backbuffer; `GetRenderCam` reports the MIRRORED camera for the duration; and scene
   depth belongs to the main camera and must not be used.

2. **`Scene.h`** - `bMirrorCam` / `mirrorCamPos` / `mirrorCamRot`, and `GetRenderCam`
   returns those while the flag is up. Patch (k)'s accessor becomes a substitution rather
   than gaining a sibling. Aperture is untouched: a mirror does not change the field of
   view.

3. **`Scene.cpp`** - the slot itself, after the streams loop and before
   `PopRenderTargets`, so it composites over everything else in the reflection exactly as
   the pre-resolve slot composites over the main scene.

```cpp
VECTOR3 mp = Camera.pos - up * (2.0 * planeAGL);
MATRIX3 mr = Camera.grot;
for (int j = 0; j < 3; j++) {
    VECTOR3 v = _V(mr.data[j], mr.data[3 + j], mr.data[6 + j]);   // column j
    v -= up * (2.0 * dotp(up, v));
    if (j == 0) v = -v;                    // handedness + the X flip
    mr.data[j] = v.x; mr.data[3 + j] = v.y; mr.data[6 + j] = v.z;
}
mirrorCamPos = mp; mirrorCamRot = mr; bMirrorCam = true;
D3D9Pad *pSkpM = GetPooledSketchpad(SKETCHPAD_2D_OVERLAY);
if (pSkpM) { gc->MakeRenderProcCall(pSkpM, RENDERPROC_WET_MIRROR, NULL, NULL); pSkpM->EndDrawing(); }
bMirrorCam = false;
```

**⚠️ THE MIRRORED CAMERA IS A REAL CAMERA, AND THAT IS THE WHOLE PATCH.** The obvious
design - hand the addon the reflection matrix - would have meant a second projection path
on the addon side, kept in sync with the first by hand, forever. It is unnecessary,
because a planar reflection of a camera *is* a camera. Reflect the eye point and the three
basis vectors through the plane and you get a view in which a real point P lands exactly
where the main camera sees P's virtual image - the same identity the matrix trick in patch
(s) part 6 already relies on, stated in camera terms instead of matrix terms.

**⚠️ THE REPORTED CAMERA IS A PURE MIRROR, AND THE HANDEDNESS IS THE CONSUMER'S TO
RECONCILE.** The reflection flips handedness (`det = -1`), which is why the pass mirrors a
second time in clip space to keep the meshes' winding legal - and that second mirror is
what leaves the RT holding a horizontally flipped image, which the ground shaders undo
when they sample it. CPU-projected geometry gets no such flip, so a consumer has to mirror
its own screen X to land in the same convention. Negating the reported camera's RIGHT
column does both jobs at once - restores a proper right-handed rotation AND puts screen X
in the RT's convention - so it is still a three-line reconciliation rather than a second
projection path. It is deliberately NOT folded into the reported basis: the camera then
describes the pass's real geometry, and the single place that has to know about the RT's
flip is the code putting pixels into it. ORO does it in `FillProjCam`, its one camera
entry point.

**⚠️ THE VIEWPORT COMES OUT OF THE SKETCHPAD, AND NO NEW API WAS NEEDED FOR THAT EITHER.**
The reflection target is half resolution, and an addon must never assume that: hardcoding
"half" couples it to a client implementation detail that could be retuned for performance
at any time. It does not have to. `D3D9Pad::BeginDrawing()` binds to
`gc->GetTopRenderTarget()`, so a pooled pad taken *inside* the push describes the
reflection texture - its ortho matrix, its scissor, and the stock SDK virtual
`Sketchpad::GetRenderSurfaceSize()`, which D3D9Pad answers from `tgt_desc`. The addon asks
the pad it was handed. Change this target's resolution and every consumer follows with no
addon edit.

**⚠️ SCENE DEPTH IS THE MAIN CAMERA'S.** `ptgBuffer[GBUF_DEPTH]` was filled in
`RENDERPASS_NORMAL_DEPTH` from the real view, so patch (g)'s per-pixel clip is meaningless
in here - at every pixel it describes different geometry than the one being drawn, and
would cut the reflection against a scene that is not there. The doc comment says so;
nothing in the client can enforce it. Consumers draw unclipped and accept painting over
the mirrored hulls, which is invisible in practice: this image only ever reaches the eye
through the puddle lattice, rippled, Fresnel-masked and blurred.
⚠️ **BUT "ACCEPT PAINTING OVER THE HULLS" UNDERSTATES WHAT LOSING THE CLIP COSTS, AND
IT COST A ROUND.** A per-pixel clip is not only an occluder - some geometry is AUTHORED
around it and is malformed without it. ORO's throat fire is drawn UPSTREAM of the nozzle
on purpose, as the fire seen through the bell mouth, with patch (g) carving it against the
bell walls; unclipped it does not merely fail to be hidden, it makes the jet visibly start
AHEAD of the engine. Anything entering this slot has to ask which of its parts exist only
because something else was cutting them, and drop those - not just tolerate the overlap.
(The same question applies wherever the clip can be absent, e.g. a user with SunGlare off.)

**⚠️ AND A FOURTH PART, WHICH IS WHAT ACTUALLY MADE IT WORK: `D3D9Pad.cpp` GAINS A
`0x200` "WRITE COVERAGE ALPHA" BLEND BIT.** Both of the pad's blended paths set
`COLORWRITEENABLE 0x7` - RGB only, alpha masked off. On the BACKBUFFER that is exactly
right: Sketchpad art there is light laid on a finished frame and has no business touching
its alpha. **In an offscreen target whose alpha is a MASK it is exactly wrong.** The
wet-mirror RT is cleared to alpha 0 and both ground shaders read `cVes.a` as "is anything
reflected at this pixel", so RGB-only geometry lands in a region the shader still reads as
empty. The first flight showed the symptom precisely: the jet survived only where it
overlapped the hull's own alpha - a stub at the tail - which reads as a broken effect
rather than a missing one, and cost a round being mistaken for a projection error.
The bit is opt-in (no existing caller changes) and rides the same no-SDK-header trick as
(d)'s `0x5` and (g)'s `0x100`. Alpha gets SEPARATE blend factors, and the linearity is the
point: letting alpha ride the colour factors gives `a = src.a*src.a + dst.a`, a SQUARED
coverage that all but deletes the soft outer sheath (0.15 -> 0.02) and reflects only the
hot core. `ONE/ONE` sums coverage the way the colour sums light. A device without
`D3DPMISCCAPS_SEPARATEALPHABLEND` degrades to exactly that squared coverage - dim, not
broken - so it is set plainly rather than through `HR()`.
**THE GENERAL RULE FOR ANY FUTURE OFFSCREEN SLOT: ask what the target's ALPHA MEANS before
drawing into it.** A render proc that only ever saw the backbuffer has never had to.

**COST.** One extra CPU geometry build (~1000 triangles for ORO) and one half-res draw,
and only while `g_gcSurfaceWet > 0.01` and the camera is 1..250 m AGL - the same gate the
pass itself already runs behind. Nothing at all in the dry, and nothing in orbit.

**WHAT IT DOES NOT DO.** It does not run when the pass does not: dry ground, above 250 m,
or a client without the patch. Registration always succeeds (`RegisterRenderProc` is a
plain list append that accepts any non-zero id), so an addon cannot probe for this one by
binding the way it probes `CanSetVCShadows` - it can only latch the first real invocation
and know that "not seen yet" is not the same as "not supported". ORO logs it that way
deliberately.

**FOR THE VULKAN REQUIREMENTS DOCUMENT:** the general form is *any pass that re-renders
the scene from a different viewpoint should be able to invite addon overlays into it, and
should report its own camera and its own target size through the same interfaces the main
pass uses.* Both halves of that were free here because the client already had the two
accessors; designed in rather than patched in, it costs nothing at all.



## Patch (h): scene depth into IPI + the `RAIN 1` glass token (2026-08-27)

Two parts, one purpose: raindrops ON the VC glass that refract the world and stop at
the window frame.

**Part 1 - `gcCore::SetIPISceneDepth(gcIPInterface*, const char* name, DWORD flags)`**
(gcCore.h/.cpp, ~3 lines): binds `ptgBuffer[GBUF_DEPTH]` into a user IPI shader by
sampler name. The resource has existed since patch (g)'s work and already includes the
cockpit; this is the same shape as (b) and (g) - hand out a handle to a texture that is
already there. Probe by binding: `CanSetIPISceneDepth` is hand-added guard **#10** in
`gcCoreAPI.h` (re-add after any codegen regeneration, as ever).

**Part 2 - the authored glass mask.** Mesh groups whose .msh carries a `RAIN 1` line
before their GEOM are written into the NORMAL_DEPTH pass with their distance NEGATED
(`NewMesh.hlsl`, deployed shader #8): the SIGN of the per-pixel depth is the window
mask, and occlusion by seats/frames/helmets is the pass's own z-buffer, free. The core's
mesh parser skips unknown tokens (verified in source), so the CLIENT reads them itself:
`RainGlassStoreScan` (Mesh.cpp) parses the mesh FILE at `clbkStoreMeshPersistent` - the
one place a mesh's filename is ever spoken - into a `map<MESHHANDLE, groups>` the mesh
constructors consult. ANY vessel works with one line in its mesh; nothing to configure.
Two landmines, both hit: the scan must run BEFORE `meshmgr->StoreMesh` (the stored
template's constructor reads the map), and the membership test in `RenderShadowMap`
(opt==1, the depth pass) must sit ABOVE the `UsrFlag 0x1/0x2` skips - the DG canopy
carries FLAG 1 and would otherwise never enter the mask.

## Patch (v): REFLECTIONS - multi-probe env maps, planar vessel mirrors, and the "Full Scene ORO (exp)" mode (2026-08-28)

Community-requested (DaveS): stock "Full Scene" reflections show planet and sky but
never the vessel itself or its payload. Three parts.

**Part 1 - MULTI-PROBE ENV MAPS.** `MAX_ENVCAM 4` (MaterialMgr): `BEGIN_CAMERA n` is
un-parked (stock parsed the index and then forced `camera = 0; // For now just one
camera`), each probe carries its own LPOS/flags/omit lists plus `GROUPS a b` ranges of
mesh groups that sample it, and `BOX cx cy cz hx hy hz` - a proxy volume for
box-projected (parallax-corrected) sampling: `EnvDir()` in D3D9Client.fx re-aims the
cube lookup at the box intersection, so a probe INSIDE the geometry it reflects stops
painting that geometry magnified. `DO_NOT_OMIT_FOCUS` (a stock key) puts the vessel
itself in its own probe. `vVessel::RenderENVMap` cycles probes through the same
round-robin that served the one; `D3D9Mesh::Render` swaps cubes per group.

**Part 2 - PLANAR VESSEL MIRRORS.** For flat near-mirrors (the shuttle's radiators)
where probe parallax always shows: `BEGIN_PLANE n` blocks (POS/NRM in the mesh BASE
pose; `GRPREF mesh grp` ties the plane to an animated group so an opening door carries
its mirror; `GROUPS` = receivers; `RDIST` below). Scene renders the vessel list -
SELF INCLUDED, the whole point - through a camera mirrored about the plane into a
half-res RT, with the wet mirror's clip-space double-flip keeping every group's culling
legal, and the clip plane transformed into CLIP SPACE (inverse-transpose of the VP;
with shaders active D3D9 clip planes live there). Receiver groups sample the RT at
their own screen position and the probe cube fills every pixel the mirror pass left
empty (alpha = coverage) - exact where declared, probe elsewhere. The CURVATURE WARP:
instead of the pixel's own screen position (exact only for a flat mirror), follow the
pixel's TRUE reflected ray - curved normal, normal map and all - an assumed distance
RDIST, mirror that point through the declared plane, and project it with the scene
camera. Law of reflection: for a flat on-plane pixel this degenerates EXACTLY to the
flat sampling, independent of RDIST - the warp exists only where the surface actually
curves. `RDIST 0` is a sentinel: the flat mirror verbatim, kept as an exact A/B.

**Part 3 - THE FOURTH REFLECTION MODE, and the two-file law.** All of the above lives
behind `EnvMapMode 3` = "Full Scene ORO (exp)" in the Launchpad combo. The three stock
settings are PIXEL-EXACT stock: consumer-side gates everywhere, zero extra passes, and
stock's own cfg reader clamps `min(2, i)` so a saved mode 3 degrades to Full Scene on
a stock client for free.
WARNING - THE FINDING THAT FORCED THE FILE SPLIT: stock's `_ecam.cfg` parser forces any
`BEGIN_CAMERA n` to camera 0 AND clears its flags, so an experimental camera block in
the shared file would RECONFIGURE a stock client's probe (move it into the bay, un-omit
the payload). Experimental content therefore lives in `<class>_ecam_oro.cfg` - a
filename stock never builds, so nothing in it can reach a stock install BY CONSTRUCTION.
`<class>_ecam.cfg` is parsed with stock grammar, byte-exact semantics, in every mode.
(Also fixed while in there: `fgets2` cuts `;` comments and returns the EMPTY remainder
as a valid line, so every comment line in a camera file spammed "Invalid Line" - ours
skips empties; stock still has the bug.)

## Patch (w): PLANET-SHINE SHADOWS - Earth glow learns what a closed door is (2026-08-29)

Community-reported (DaveS, stock behaviour): planet shine has NO occlusion term of any
kind. The glow sites in PBR.fx/Metalness.fx add `gAtmColor * f(angle)` to every
planet-facing surface - the sun term gets `ComputeShadow()`, the glow term gets
nothing - so a closed payload bay flying bay-to-Earth glows sky-blue inside.

**The fix:** the focus vessel's ATTACHMENT ASSEMBLY (climb the attachment tree to its
root, collect everything below - orbiter + berthed payloads, so focusing the payload
changes nothing) renders into a depth map along the PLANET direction, reusing
`RenderShadowMap` wholesale. The result is copied out by `StretchRect` to a dedicated
R32F target and the sun's `smap` struct restored - the shared LOD targets are repainted
by later passes, the same reuse trap patch (f) documented. `PShineShadow()`
(D3D9Client.fx; orthographic, so no w-divide; 2-tap PCF - planet light is a huge area
source) attenuates the glow sites for assembly members. The FAST_PS variant keeps the
stock unshadowed glow: it is a per-mesh opt-in chosen for cheapness and sits at the
ps_3_0 temp-register ceiling - adding the call overflows it (X4505, caught by the
mandatory fxc compile-check).

**Part 2 - A MIRROR MUST NOT CREATE LIGHT.** First flight: the bay went black but its
REFLECTIONS stayed lit. Two causes, one class: the planet-shine bind was main-scene
gated (fixed: the pass renders at the top of the frame, before the env-cube turn and
both mirror passes, and assembly members bind it in EVERY pass - the world frame is
camera-centred and constant within a frame, only view matrices change, so the lookup
is valid everywhere); and - the deeper stock finding - THE SUN SELF-SHADOW MAP IS
MAIN-SCENE-ONLY in stock (`vVessel::Render`), so every secondary render (probe cubes,
both mirror passes) draws geometry fully sunlit. Stock never noticed because stock
probes exclude the vessel itself; an interior probe with DO_NOT_OMIT_FOCUS is the
first thing that ever rendered a vessel's own closed bay into a cube. Fix: after the
main scene renders the focus sun map, copy it out (same recipe), and secondary passes
bind the copy for assembly members - `ComputeShadow` runs verbatim, one frame stale,
bounded.

**Known floor, accepted:** a faint stable residual remains in the reflections - real
light leaking through places the bay mesh is not watertight from the inside. An
authoring fact of the mesh, not a lighting bug (same family as shadows leaking through
non-watertight hulls).

**FOR THE VULKAN REQUIREMENTS DOCUMENT:** planet shine (and any ambient-class light)
needs an occlusion input, and shadow maps should be bindable in EVERY pass that renders
vessels - both invisible in stock only because nothing ever rendered a vessel's own
interior from a secondary viewpoint before.

## Patch (x): PARTICLE SUN LIGHTING - diffuse smoke learns what night is (2026-08-30)

**The stock finding:** `Particle.fx`'s diffuse vertex shader hardcodes `light = 1.0f`
with the original N.L term COMMENTED OUT, so every DIFFUSE particle stream renders
fully daylight-lit on the night side of a planet - a midnight smoke trail glows as if
at noon (a tester report, with photographs). The disable was probably deliberate:
normal-based lighting misbehaves on camera-facing billboards. The fix is POSITION-based
instead - "does sunlight reach this particle?".

**Files:** `Particle.cpp` (the whole model), `shaders/Particle.fx` (the NINTH deployed
ORO shader - one line in the VS reads the value, the `light` interpolant becomes
float3), `D3D9Config.{h,cpp}` (+`ParticleLight`, persisted 0..2, default 2),
`VideoTab.cpp` + `D3D9Client.rc` + `resource.h` (the Launchpad "Particle lighting
(ORO)" group: the Advanced Setup's right-column checkboxes moved down, a "Diffuse sun
light" dropdown above them - Off (stock, always lit) / Brightness only / Brightness +
colour).

**The model, all CPU-side per particle, carried in the vertex normal channel the
shader no longer reads as a normal** (those vertices were already rewritten every
frame; zero new buffers):
- **Sun visibility:** sun-at-the-global-origin (RenderGroundShadow's own long-standing
  convention), sin(elevation) against the ALTITUDE-DEPRESSED horizon, smoothstepped over
  a twilight band (sin -1..+5 deg) onto the user's Launchpad-ambient floor (the exact
  vVessel::ModLighting scaling). Per particle, so a long trail straddles the terminator;
  the depressed horizon keeps an orbital trail lit past the ground terminator.
- **The flame:** young smoke near the source is lit by the engine - `max()` with an
  inverse-square glow reaching ~10 src-sizes, scaled by the stream's own level. It can
  only LIFT, so daylight is untouched; engine shutdown kills it.
- **The dawn/dusk tint** rides the TRUE sun elevation (0..+9 deg - the air path the
  light crossed; the visibility coordinate would whiten high smoke that photographs
  gold, the STS-108 lesson), three smoothstepped stops: RED (1,.30,.10) at the shadow
  edge -> GOLD (1,.62,.18) -> white. ⚠️ The g:b RATIO is the hue - ~2:1 reads SALMON.
  Two buried experiments, both REVERTED on flight: a head/tail hue lerp from
  vPlanet::GetObjectAtmoParams (normalized low-sun colour = saturated full-brightness
  red smeared by the age lerp), and an altitude-thinning + 0.6 softener ("goes straight
  to yellow"). The tunables live at the top of Particle.cpp.
- **Fully sunlit evaluates to exactly 1.0** and EMISSIVE streams never enter the path,
  so daylight and every self-luminous effect are bit-identical to stock. Mode 0 skips
  the whole block - bit-exact stock for the scene.

⚠️ **RESOURCE-ID LANDMINE:** the first build defined `IDC_PRTLIGHT = 3036`, which
COLLIDED with `IDC_TILECOUNT` - the tiles combo's 600/1200/2400 landed in the new
dropdown and the tiles combo went empty. resource.h has multiple ID ranges; always
take max+1 FILE-WIDE (4067).

**STAGE 2 (2026-08-30/31, DLL-only - the FX file's per-vertex `light` interpolant
already carries whatever the CPU writes, so none of this touched the shader):**
- **Directional shading, per billboard CORNER:** pseudo-normal = the corner's offset
  from the particle centre + a view-direction bulge (`ORO_PRT_DIR_ZC` - the billboard
  as the front hemisphere of a sphere), wrap-Lambert against the sun
  (`(dot+W)/(1+W)`, `ORO_PRT_DIR_WRAP`). The sun-facing side of a cloud is bright,
  the far side smoky. Orientation-proof: the offsets are read off the vertices the
  atlas rotation just wrote.
- **Two-light split:** cool sky ambient (`ORO_PRT_AMB_*`, colour mode + atmosphere
  only) + tinted directional sun + white omnidirectional engine flame. The flame ADDS
  (was `max()` - the hard max had a kink exactly at the flame->sun handover, reported
  as an abrupt white-to-orange cutoff) and its falloff gained a LORENTZIAN tail past
  the reach, value- and slope-continuous at the boundary.
- **The dawn tint is the HULL'S OWN** (round 4/5): per particle,
  `vPlanet::SunLightColor(-sinel, alt)` - the exact extinction curve
  GetObjectAtmoParams feeds vessel sunlight - normalized to hue (magnitude stays with
  the twilight ramp), deepened and read slightly AHEAD of the true sun. The three-stop
  band survives as the fallback when the proxy body has no planet visual. Where the
  hue is deep the sun term overdrives past 1.0 into the fp16 chain (the diffuse PS
  multiplies light through unclamped), so Light glow BLOOMS the tinted smoke; white
  daylight and engine-lit steam overdrive by exactly nothing.
- **Four more Launchpad controls** (`D3D9Config` + `VideoTab` + rc, IDs 4068-4071,
  the max+1 rule): "Shadow strength (diffuse)" (`ParticleShadow` 0..1 - scales
  RenderGroundShadow's alpha including its 0.1 floor; a low sun stretched a smoke
  column's shadow into a near-black band), and the three dawn-tint dials
  `ParticleTintLead` (0..0.20 sin-el), `ParticleTintSat` (1..4), `ParticleTintBloom`
  (1..3). Defaults are the author's settled tuning (0.10 / 1.6 / 2.34). The setup
  dialog grew 42 DLU (378 -> 420) for the three rows - IDC_FLATS and everything
  below moved down with it.

**FOR THE VULKAN REQUIREMENTS DOCUMENT:** particle lighting wants a position-based
sun-visibility input per particle (or per stream segment); billboard normals are the
wrong basis and a hardcoded 1.0 is the wrong fix.

## Patch (y): STREAM-SPEC READBACK - the stock definitions an addon cannot otherwise see (2026-08-30)

**Why:** the core COPIES a `PARTICLESTREAMSPEC` at stream construction and exposes no
getter, so "start my sliders from what the vessel author shipped" (ORO's COPY STOCK
buttons) is impossible through the SDK. The client's scene holds every live stream
with all its derived spec fields - the only place the definition can be read back.

**Files:** `Particle.h/.cpp` (`OroGetSpec` inverts SetSpecs exactly - lifetime out of
`ipht2`, amax out of `afac`; `OroIsStockExhaust` = belongs to the vessel AND not
created under patch (o)'s exemption latch, so ORO's own replacement streams and
reentry streams are excluded), `Scene.h/.cpp` (the walker over `pstream[]`),
`gcCore.h/.cpp`:

    gc_interface int GetExhaustStreamSpec(OBJHANDLE hVessel, int idx,
                     PARTICLESTREAMSPEC* out, VECTOR3* pos, VECTOR3* dir);

Returns the stock-stream count; fills the outputs when idx is valid. `pos`/`dir` are
the stream's attach point and THRUST direction, VESSEL frame - they are pointers into
the vessel's own thruster storage, which is what lets the consumer classify a stream
by ENGINE GROUP (ORO matches dir against the selected group's thruster directions and
folds byte-identical specs, so a DeltaGlider's MAIN offers exactly two candidates:
the contrail and the flame puffs). Read-only - nothing is lent, so the 23(k)
load-window rule does not apply. Guard #11 in the hand-maintained gcCoreAPI.h:
`CanGetExhaustStreamSpec` (probe by binding, as ever - and re-add it after any codegen
regeneration, with the wrapper verified to pass ALL FIVE arguments).
