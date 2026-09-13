# Rebuilding D3D9Client for ORO (THIRTY-EIGHT local patches: a-z, +k2, +z2, +z3, +aa-ai)

ORO runs on a locally-patched D3D9Client carrying **thirty-eight** ORO patches:

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

> **THE EASY PATH (since 2026-08-13): skip step 3 entirely.** All thirty-six patches are
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
   thirty-six exist as `.patch` files** — the rest are documented as code listings in the
   per-patch sections below, because all thirty-six were developed as uncommitted
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
   the hand-added guards from the generated output — all TWENTY-ONE of them:
   `CanCaptureBackBuffer`, `CanSuppressReentry`, `CanSetVCShadows`, `CanDrawDepth`,
   `CanGetRenderCam`, `CanGetRenderObjPos`, `CanDrawTexPoly`, `CanSuppressExhaust`,
   `CanExemptStream`, `CanSetIPISceneDepth`, `CanGetExhaustStreamSpec`,
   `CanGetDevMeshName`, `CanReloadRainSurfaces`, `CanFlashMeshGroup`, `CanSetFogLayer`,
   `CanSetFogLook`, `CanSetSnowCover`, `CanSetBaseLights`, `CanSetVCNightLight`,
   `CanDeferVCHUD` (checks BOTH bound pointers) and `CanSetWaterMirror`.
   Hit for real on 2026-08-04. Note the codegen writes into the
   CLONE (`out/build/.../Orbitersdk/include/gcCoreAPI.h`); the copy ORO compiles against
   is `<Orbiter>\Orbitersdk\include\gcCoreAPI.h` and is updated BY HAND, so a client
   rebuild alone is harmless - it is copying the regenerated header over that one that
   loses the guards. Re-add all TWENTY-ONE, then verify (21 of 21) before building ORO.
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

**Part 3 - the NO-MESH-EDIT declaration route (2026-09-01).** Stock meshes are never
shipped, so an edited `deltaglider_vc.msh` cannot be the shipping answer. Beside the
in-mesh tokens, `Config\ORO\VesselsRainSurfaces.cfg` lists `<meshname> <group>` pairs
(the mesh name is the same string `clbkStoreMeshPersistent` speaks and `D3D9Mesh`
carries, so the three sides agree by construction); `RainCfgMerge` (Mesh.cpp) joins
them into the same store during the scan. Authored by ORO's in-panel RAINSURFACES
picker, which rides the STOCK `GENERICPROC_PICK_VESSEL` slot - register a generic proc
and the client's own `WM_LBUTTONDOWN` handler runs `Scene::PickScene` (VC meshes
included) and calls back with vessel + device mesh + group. Two client pieces made it
work: a NULL guard on the stock callback block (it dereferenced `pick.vObj` unchecked -
a sky click would CRASH the moment any addon enables the proc), and
`gcCore::GetDevMeshName` (guard **#12**) because `PickData.mesh` is a DEVICE handle
only the client can name.

**Part 4 - LIVE APPLICATION + the pick highlight (2026-09-02).** Every `D3D9Mesh`
copies its rain list at CONSTRUCTION, so a cfg change used to need a scenario reload.
`gcCore::ReloadRainSurfaces` (guard **#13**) re-reads the cfg, recomputes every store
entry from its kept mesh NAME (entries now persist even with zero groups, or a line
added for a previously-undeclared mesh stays unreachable all session), and hands every
LIVE mesh in `MeshCatalog` its fresh list by name - the depth pass reads it per frame,
so the picker's SAVE lands on the next one. `gcCore::FlashMeshGroup` (guard **#14**)
paints one group in the Debug dialog's own green (`eColor`, the group highlighter's
override in `D3D9Mesh::Render`), held while the mouse button that made the pick stays
physically pressed (`GetAsyncKeyState`, so a release outside the window clears it too;
`msec > 0` gives a timed variant). A flashing mesh takes the full render path for that
second - `RenderFast` has no `eColor` hook, exactly as it has none for the Debug
highlighting.

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

## Patch (z): THE BASE PASS GROWS UP - depth for bases, night textures for MESH objects, the sun behind terrain (2026-08-31/09-01)

STOCK bugs of the (q) family, reproducible with no addon: base rendering was
depth-blind - a flat-planet fossil (nothing could ever stand between camera and
runway, and depth-off dodged z-fighting with the coplanar ground) that terrain
elevation made false. His runway-through-a-mountain screenshots. The FINAL FLOWN
SHAPE, after one revert:

- **RUNWAY LIGHTS**: `BeaconArray.fx`'s technique carried `ZEnable = false` in the
  SHADER STATE BLOCK, not the C++ (the C++ only re-enables after the draw). Flipped -
  and BeaconArray.fx thereby becomes the TENTH deployed shader (staging list,
  installer loops, acceptance rows all grown the same session).
- **BASE TILES**: `Mesh.fx` `BaseTileTech`, the same state-block fossil. Flipped.
- **RUNWAY / LANDING-PAD SURFACES** (below-shadow structures): `Mesh.cpp`'s two
  `RENDER_BASEBS -> ZENABLE = 0` overrides (why HANGAR/TANK always hid correctly
  while runways bled through hills - those are above-shadow, normal path). Depth
  TEST on, WRITE off - and the coplanar contest the original disable existed for is
  settled by a CAMERA-WARD DEPTH BIAS set/cleared in `vBase::RenderSurface` around
  both draw families (constant -0.00002, about 300 ticks of a 24-bit buffer;
  slope-scale -2.0 for the grazing angles; both orders of magnitude too small to
  read through real terrain).
- ⛔ **STENCIL GROUND SHADOWS - BUILT, FLOWN, REVERTED** (his call): on sloped
  terrain a flat-projected shadow sheet CLIPS where it dips under a rise, which read
  worse than stock's smear-through. Shadows are depth-blind stock again; the honest
  fix is terrain-draped shadow geometry, parked as a real project.
- **THE SUN HIDES BEHIND TERRAIN** (`OroSunTerrainVis`, Scene.cpp): the glare's
  visibility kernel samples GBUF_DEPTH, which holds vessels and cockpit only -
  terrain never writes it - so the sprite painted over any mountain the sun was
  behind. The fix asks the TERRAIN directly: ~30 elevation samples marched toward the
  sun's azimuth on a x1.25 ladder out to 160 km (sparse far samples left
  tens-of-km gaps a whole mountain range hid in), curvature-dropped by d^2/2R,
  fading to zero as the sun's CENTRE crosses the measured ridge. Inert above 25 km
  AGL; on flat ground the ridge is the dipped sea horizon, so sunset timing improves
  for free. ⛔ The five sun-DISC experiments around it were all reverted to bit-stock
  (see the CLAUDE.md graveyard entry: nothing in the sun pipeline knows CLOUDS, so
  any brightness-keyed rule about the sun's face misreads a hazy morning as sunset).
- **`_n` NIGHT TEXTURES FOR MESH BASE OBJECTS**: the classic `mytex.dds`/`mytex_n.dds`
  pairing worked for HANGAR/TANK/LPAD blocks and never for MESH blocks (base authors'
  most-used type) - the core's base compiler wires the night layer for generic
  objects only. The core cannot be patched and does not need to be: every texture is
  loaded BY the client, so `D3D9Mesh::AttachNightTextures()` (called by vBase on its
  structure meshes) probes `<name>_n.<ext>` per day texture - quietly, via
  `TexturePath`, no log spam on a miss - and attaches hits as the night layer,
  SHARED loads (0x8) so the texture cache owns the lifetime. Runway surfaces ride
  along: a `<runwaytex>_n.dds` lights the markings at night, which stock never did.
  The day/night switch stays the stock HARD FLIP at `csun_lights` (a twilight ramp
  was built and reverted the same evening - "when someone is in a building, they
  turn on the lights at dusk": interior lights are switched by people, not faded by
  the sun).
- Plus the Advanced-setup CHECKBOX OVERLAP fix (a stock collision at 321 vs 324 DLU
  our patch-(x) +42 shift preserved faithfully): the right-column checkboxes now sit
  at an even 11-DLU pitch.

## Patch (z2): base structures join the depth-normal buffer (2026-09-02)

GBUF_DEPTH (the screen-space depth+normal buffer filled by RENDERPASS_NORMAL_DEPTH)
held VESSELS + COCKPIT only, so every consumer that asks it a visibility question was
blind to buildings: the sun and local-light GLARE visibility kernels
(`ComputeLocalLightsVisibility`) painted their sprites straight through a hangar, and
the Sketchpad depth clip (patch g) let addon geometry draw in front of structures it
should vanish behind.

**Files:** `VBase.h/.cpp` (`vBase::RenderStructureDepth` - loops the ABOVE-shadow
structure meshes through the same mesh-generic `D3D9Mesh::RenderShadowMap(pW, pVP, 1)`
path the vessels use, so patch (f) part 2's transparent-caster skip rides along and a
glass wall correctly fails to occlude), `VPlanet.h/.cpp` (`vPlanet::RenderBaseDepth` -
the proxy-body and render-flag 0x20 guards mirrored from RenderBaseStructures: a base
the user has switched off must not occlude either; note the declaration goes in the
PUBLIC section - Scene calls it, unlike RenderBaseStructures which only vPlanet::Render
ever calls), and one call in `Scene.cpp`'s NORMAL_DEPTH pass after the cockpit.

⚠️ ABOVE-SHADOW STRUCTURES ONLY, deliberately: the ground-level sets (tilemesh,
structure_bs - runways, aprons, pads) are coplanar with TERRAIN, which never writes
this buffer either. Admitting one side of that contest would make addon geometry clip
against an apron but not the grass beside it - worse than staying out entirely. A light
behind a HILL still paints through: the known terrain limit, unchanged.

Related stock finding, recorded with the patch: local-light glares ship DISABLED
(`LightsGlare = 0`) with their Advanced-setup checkbox HIDDEN
(`IDC_ELIGHTSGLARE, NOT WS_VISIBLE | WS_DISABLED`). Tested once by cfg flip: emitter
positions are authored invisible (the DG's dock light sits inside the nose mesh, its
engine light floats 10 m behind the tail), so sprites at emitter points cannot look
right addon-wide. The user's ruling: it stays hidden; bake lit glass into the mesh and
let the emitter do the lighting.


## Patch (z3): the LOCAL-LIGHT SHADOW MAP - spotlights learn what a wall is (2026-09-02/03)

Stock local lights have no occlusion term anywhere: the per-pixel light loops
(Common.hlsl for vessels, NewPlanet.hlsl for terrain) compute attenuation, cone and
N.L and nothing else, so a spotlight beam passes through a hangar and a vessel
standing in the beam casts nothing. Patch (z3) renders one perspective depth map per
frame from the strongest shadow-casting SPOT light and tests it in both receiver
families. Config key `LocalLightShadows` (default 1; 0 = bit-stock). No gcCore
surface - nothing for the codegen to touch.

Files: `Scene.h/.cpp` (the pass, the light selection, the dedicated R32F+D24X8
target, the tile-caster list, the LightOwners[] array), `Surfmgr2.cpp` (per-tile
slot match + registration), `Mesh.cpp` (per-mesh slot match at all three light-upload
sites), `D3D9Effect.h/.cpp` (three FX handles), `VBase.cpp/.h` + `VPlanet.cpp/.h`
(RenderStructureDepth/RenderBaseDepth grow an `opt`), `D3D9Config.*`, and TWO
deployed shaders: `NewPlanet.hlsl` (terrain test + the TileShdVS/PS depth pair) and
`Common.hlsl` (vessel test in both light loops - Common.hlsl thereby becomes the
ELEVENTH deployed shader; stock copy in `upstream/stock/`).

The load-bearing decisions, each bought with a flight:

- **The terrain BORROWS the tShadowMap sampler slot per tile.** The Earth config
  with `_DEVTOOLS` sits at EXACTLY ps_3_0's 16-sampler ceiling in stock (X4510 with
  a 17th - the mandatory fxc matrix must include `_DEVTOOLS`). A beam-lit tile binds
  the local map and yields its sun-map shadow for the frame.
- **Which is why the whole feature is NIGHT-GATED** (sun < ~2.5 deg above the proxy
  horizon): in daylight the yielded slot ate the vessel's own sun shadow in
  tile-shaped bites. Daylight is pixel-stock, dusk and night keep everything.
- **Casters are never view-culled.** vBase::IsVisible() is a camera test and gates
  opt 1 (GBUF_DEPTH, the camera's own buffer) only; in a light-space pass it made
  every structure shadow strobe with the view direction.
- **The emitter's own vessel does not cast.** Emitter positions are routinely
  authored inside the hull (the DG dock light sits in the nose); an honest
  self-shadow from in there blacks out the whole beam.
- **Terrain tiles cast via a self-registration list** (AddRef'd VB/IB, ~2 s TTL,
  re-anchored by camera translation) drawn one frame stale through `TileShdVS/PS` -
  the registration source is the camera's rendered tile set, so without persistence
  a camera rotation churns the casters.
- **Bias = normal-offset (~2 texels, distance-scaled) + the texel-footprint depth
  bias**: exactly the ray-depth span one PCF-widened texel covers on the receiving
  surface. Near-nothing face-on, metres at grazing, always far below a real
  caster's separation. A receiver-angle fade was tried between the two and removed:
  on a low light over flat ground the whole pool is "grazing", so a fade either
  does nothing or eats the real shadows.

REVERTED AND BURIED THE SAME ARC: a "base sun map" (coarse structures+vessels ortho
sun map bound into the TerrainShadowing-2 slots per tile, for draped building sun
shadows). Three fix rounds kept trading artifacts - the slot steal ate the vessel's
crisp shadow in tile bites, the camera-following volume fit re-quantized the grid,
and the end state cast transparent flickering building shadows. Do not rebuild by
stealing the stock per-tile slots; the receiver architecture question comes first.

## Patch (aa): THE AIR - two analytic fog layers in every shader family (2026-09-05)

`gcCore::SetFogLayer(idx, rBase, hTop, scaleH, dens)` (two slabs: base radius, top,
scale height, density in 1/m), `SetFogLook(brightness, sunGlow)`, and the dormant
`SetSnowCover(cover, lineAlt, lineWidth)` whose shader plumbing rides along for the
snow round. Guards #15-#17 (`CanSetFogLayer`, `CanSetFogLook`, `CanSetSnowCover`).

Files: `gcCore.h/.cpp`, `D3D9Effect.h/.cpp` (eFogPrm/eFogClr/eSnow handles),
`Scene.cpp` (`OroFogFrame`, `OroFogInterior`, `OroFogPushPS`, `OroFogTransmittance`,
`OroFogSunAttenuation`, `OroGroundShadowFade`), `Surfmgr2.cpp` / `Cloudmgr2.cpp` /
`HazeMgr.cpp` (the per-tile push), `VObject.cpp` (RenderSpot dims by transmittance),
`VBase.cpp` + `VVessel.cpp` (stencil shadow fade), and EIGHT deployed shaders:
`D3D9Client.fx` (the fog block + gFogSunCam), `NewPlanet.hlsl` (terrain, clouds,
horizon), `Mesh.fx` (base tiles), `PBR.fx` (main + FAST), `Vessel.fx`, `Metalness.fx`,
`Particle.fx`, `BeaconArray.fx`.

- **Optical depth is integrated analytically** along camera -> pixel, each slab
  clipped to its [base, base+top] shell; T = exp(-tau). Sun attenuation per pixel =
  the column ABOVE the pixel divided by max(sinE, floor).
- **The colour is pushed in DISPLAY space and lerped on the FINAL output** (after the
  terrain's HDR()), so every shader family converges on one grey. Colour model in
  Scene.cpp: sunLum at the camera scales a sky term, a forward lobe (pow 8) toward the
  sun, an ambient lift - warm at dawn, grey under a storm, dark at night.
- **The cockpit bracket zeroes the AIR (`OroFogInterior`) but NOT the sun attenuation**,
  and the VC shadow depth is pushed as `depth x (1 - storm) x fogSunCam`: with depth 1
  the ambient bite is sun-independent, so the bite itself has to follow the sun.
- **PBR's FAST path sits at the ps_3_0 temp ceiling** (X4505): fog is evaluated at
  its tail, sun/ambient use the uniform `gFogSunCam`.
- Stencil ground shadows fade per object: `(1 - storm) x sunAtt(pos) x T(pos)`.
- Landmines: HLSL reserves `line`; `tex2D` inside `[branch]` is X3528 (use tex2Dlod);
  the compile matrix lives in `tools/fxccheck.sh` (includes `_DEVTOOLS`, ignores X4717).

## Patch (ab): TERRAIN INTO GBUF_DEPTH + the soft stencil-shadow depth test (2026-09-05)

The (z3) tile registry serves a second consumer. At the NORMAL_DEPTH pass the tiles
the previous frame registered (stamp <= 1) draw through `TileDepthVS/PS` in
`NewPlanet.hlsl` - `float4(nx, ny, nz, length(posW))` - into the depth-normal buffer,
z-tested against the vessels and structures already in it. `Mesh.fx ShadowTechPS`
then discards a stencil-shadow fragment only where it lies BEHIND the scene by more
than `ShadowDepthTol + ShadowDepthTolK x distance` metres (VPOS lookup, tex2Dlod;
D3D9Client.cfg keys, defaults 1.0 / 0.001 - his settled values). The buffer and the
tolerances are pushed AFTER the pass and zeroed at the top of the frame, so probe
cubes never test against another camera's buffer.

Files: `Scene.h/.cpp` (LCLTILECASTER gains bs/bsRad; `pTileDepth`; the pass; the
push; `WantsTerrainDepth`), `Surfmgr2.cpp` (registration gate: light range OR
within 60 km of the depth pass), `D3D9Config.h/.cpp` (the two keys), `D3D9Effect`
(eSceneDepth/eSceneDepthPrm), `D3D9Client.fx` (the sampler + ShadowTexVS's dist),
`Mesh.fx`, `NewPlanet.hlsl`.

- **Cap 512 -> 4096**: a lunar horizon renders many hundreds of tiles a frame, and a
  refused tile reads as SKY, which the soft test treats as nothing in front.
- **THE KSC BLINK (2026-09-06, rounds 2-4): whole ground shadows blinking frame to
  frame.** Found by an instrument, not a theory: cfg key `ShadowDebug` (1 = colour each
  stencil sheet by the test's verdict - green no depth, red clipped, blue passed;
  2 = signed depth difference; both log an `ORO shadow dbg:` line once a second) showed
  red/blue flashing live and solid blue paused, and the log's per-frame camera motion
  alternating between ~150 m and 0. Orbiter steps the world on only some frames at
  high fps, and on a stepped frame the planet carries camera and terrain ~150 m through
  the global frame (30 km/s); the pass drew LAST frame's tiles re-anchored by camera
  translation alone, as if terrain were fixed in space. Fix: a registered tile is
  stored in ITS PLANET'S FRAME (origin + basis rows, doubles - the centre is ~6400 km
  off, past float's metre) and both consumers - this pass and the (z3) local-light
  map - rebuild the camera-relative matrix from the planet's current rotation and
  position at every draw (`Scene::OroTileToPlanet` / `OroTileFromPlanet`). Exact at
  any time warp (the position-only version blinked at warp, where the planet also
  rotates). `ShadowDebug` stays as a diagnostic key, default 0.

## Patch (ac): BASE LIGHTS - the night state forced, the glow, the fog halo (2026-09-05)

`gcCore::SetBaseLights(bForce, glow, halo)`, guard #18 (`CanSetBaseLights`).

Files: `gcCore.h/.cpp`, `D3D9Effect` (eBaseGlow/eBaseHalo), `VBase.cpp` (the night
flip `(csun < csun_lights) || force`; eBaseGlow set/restored around RenderSurface,
RenderStructures and RenderRunwayLights, eBaseHalo at the lights), `D3D9Client.fx`
(gBaseGlow = 1, gBaseHalo = 1), `Vessel.fx` / `PBR.fx` / `Metalness.fx` (cEmis x
gBaseGlow - 1 on vessels), `Mesh.fx` (the tile night layer), `BeaconArray.fx`.

- The glow scales what the lights EMIT; past 1 the fp16 chain carries it into the
  Light glow post-process, which is how they bloom.
- **The halo** (his runway-lights-in-fog reference): `haloK = saturate((1 - T) x
  halo)`, sprite size x (1 + 5 haloK), haze exponent lerped to 0.30, alpha
  lerp(T, sqrt(T), sat(halo)) - a lamp in fog is an aureole that grows with the
  optical depth, not a brighter lamp.

## Patch (ad): THE CABIN AT NIGHT - the VC's fill light follows the sun (2026-09-05/06)

`gcCore::SetVCNightLight(scale)`, guard #19 (`CanSetVCNightLight`). DLL-only.

What lights a stock VC at midnight is not the Launchpad ambient: it is MATERIAL
EMISSIVE - the stock DeltaGlider carries a flat 0.8 on nearly every cabin material
(the `instrument` materials are diffuse 0 / emissive 1). So the scale is applied at
both SOURCES, in the cockpit pass only: the ambient in `vVessel::Render`'s VC sun
copy (beside stock's `Color *= 0.5`), and the emissive at `Mesh.cpp`'s material push
(`OroVCNightMat`, in Render / RenderFast / RenderSimplified - every shader path by
construction). EXEMPT: MFD-screen groups (the MFD path never calls the helper),
black-diffuse DISPLAY materials (emissive is their whole picture), emission maps
(gMtrl.emission2), and every local light emitter. `Scene.cpp`'s cockpit bracket
raises `g_oroVCNightNow` for the VC draw and drops it straight after.

Files: `gcCore.h/.cpp` (+ the binder line), `Scene.cpp`, `VVessel.cpp`, `Mesh.cpp`.
Known compromise: lit buttons/labels painted with the same flat emissive as the walls
dim with the walls.

## Patch (ae): CASCADED SHADOWS - one sun-shadow atlas for the whole scene (2026-09-06)

TerrainShadowing mode 3, **"Cascaded (ORO)"**. Modes 0-2 render bit-stock (the
reflections rule: a stock setting must give stock pixels). Eleven fly-and-report rounds in
one day, every one from his screenshots.

**THE ATLAS.** One shadow target (`psCasc` + `psCascDS`, D24X8 depth) of 3 x 2 cascades:
`ShadowCascadeSize` 512..4096, default 2048 = 6144 x 4096 (192 MB with its depth); 4096 =
12288 x 8192 (768 MB), which needs 16384-wide texture caps and halves itself automatically
against `caps.MaxTextureWidth/Height`. Nine slots in half-cascade units (`ORO_CASC_SLOT[9]`,
q = cascSize/2): slot 0 a vessel-anchored, UNSNAPPED box around the focus vessel (its own
casters only); slots 1-5 camera-fitted cascades (1-3 full-size - the far one at full size
is what makes 30 km read sharp - 4-5 half-size; the mesh family samples 1-3, terrain 1-5);
slots 6-8 HULL BOXES around the three nearest other vessels (bounding radius <= 300 m,
within min(reach, 6 km)) - crisp shadows on the ShuttleA on the far pad rather than the
far cascade's blur. Row 3 is the spare row; its cell 0 receives the (z3) local-light map by
`StretchRect` after the cascade pass (round 8), so terrain samples ONE texture for sun and
spotlight shadows and the (z3) slot borrow - and with it the night gate - is gone in mode
3: a lamp respects a wall at noon. The atlas clear covers rows 0-2 only.

**THE LATTICE** (round 5, the shimmer: "fast around the edges up close, slower on the
thicker pixelated shadows farther away"). The texel snap quantised dot(g, u) from the
PLANET CENTRE in a global-frame light basis, so the planet's rotation swept the lattice at
omega x R (3.5 m/s at Brighton Beach = 130 texels/s on the near slot). Each snapped slot
keeps a PLANET-LOCAL anchor (`cascAnch[]`, `cascAnchTexel[]`) walked to the camera every
frame in WHOLE lattice steps - phase preserved, nothing pops - with the slot radius
quantised to eighth-octave steps. The (ab) lesson in another outfit: a stored
world-anchored thing lives in its body's frame.

**CASTERS**: vessels through patch (f)'s `RenderShadowMap` path (animations included),
base structures (`RenderStructureDepth` opt 0), terrain tiles (the (z3) registry through
`TileShdVS/PS`) - into every camera slot; slot 0 and the hull boxes render ONE hull each,
and the camera slots exclude the focus vessel and the hull-box vessels.

**RECEIVERS**: terrain (`NewPlanet.hlsl`: `OroCascadeShadowT`, slots 1-5, a wide tent
`OroCascTapWT` on the far slots - `ShadowCascadeSoft`, default 1 - plus four hull-box taps;
debug bands via `vCascBasis[0].w`), and every vessel path (`D3D9Client.fx`:
`OroCascadeShadow`, EXACTLY TWO lookups - the bilinear cascade and ONE hull box chosen per
pixel by `OroCascIn` containment, focus first - called from PBR_PS, MetalnessPS, the
legacy path AND, since round 12, FAST_PS - min()'d with the per-vessel map).
⚠️ **ROUND 12 (2026-09-06, the evening of the release - his open item 1, "vessel-to-base-
structure shadows"): THE FAST PATH WAS THE RECEIVER GAP.** Rounds 1-11 put the term in
PBR_PS, AdvancedPS and MetalnessPS and left `FAST_PS` alone ("it sits at the ps_3_0 temp
ceiling" - the (w) planet-shine shadow had overflowed it, X4505). But FAST is the path
EVERY mesh with no advanced texture maps takes (`Mesh.cpp` `CheckMeshStatus` ->
`RenderFast`): every base structure (`vBase::RenderStructures` -> `Render(RENDER_BASE)`),
every runway and pad surface (RENDER_BASEBS) and every plain-textured hull - the stock
DeltaGlider's texture folder carries no `_norm`/`_spec` maps at all. So in mode 3 a
hangar's walls, the pad under a vessel and a DG parked in a building's shadow stayed
SUNLIT while the terrain around them went dark. The fix is the same one-line term min()'d
into FAST's `fShadow` (which then scales dLN, fSun and the (p) ambient share exactly as
the self-shadow does), placed where FAST's shadow is computed - EARLY, with few
temporaries live - and the full fxc matrix passes in both cascade configurations.
Shader-only (PBR.fx is a deployed, runtime-compiled file): no DLL rebuild, the stamp
stays 260906, backup `PBR.fx.pre-ae12-260906`. Flown: "Everything reads smooth on my
end." Bonus nobody asked for: base structures are already casters in the atlas, so a
hangar now shadows its own interior and the VAB's faces self-shadow. Reach is the mesh
family's (slots 1-3), the same as hulls. The lesson is the list-is-a-claim trap again:
"every vessel path" was written per FILE; the technique has FOUR pixel-shader passes.

**BIAS**: receiver-plane depth from the normal, g = clamp(ln.xy / max(nl, 0.05), +-4)
(terrain 0.15 / +-6); a tap's expected depth z0 = z + dot(g, dMetres) x A.w, per-texel step
dz = g x tx x A.w, bias tx x (0.5 + 0.5 grz) x A.w (terrain 0.6 + 0.35 grz), normal offset
half a texel (0.75 on the wide tent), grz clamped at 4. Hull boxes fit with a 2 m margin
toward the sun and a 1000 m REACH past the hull (round 10: the old window ended r + 1 m
past the hull and cut ground shadows below ~15 deg sun - the sunset clipping); camera
slots fit (max(500, r), 1.0).

**CONSTANTS**: shared light basis `gCascBasis[2]` (L = -cross(U, V)), `gCascA[7]` (effect
order 0 = focus, 1-3 cascades, 4-6 hull boxes; CPU remap fxSlot = {0,1,2,3,6,7,8}),
`gCascTx[2]`; terrain `vCascBasis[3]`, `vCascA[9]`, `vCascTx[3]`, `vCascSplit`,
`vCascAtlas`. Each CASCADE carries A = (cu, cv, dot(eye, L) + zn, 1/range) and
B = (uvx, uvy, scale, texel); uv rects by arithmetic.

**THE LAUNCHPAD** (round 9): the "Shadows" group in Advanced Setup (IDs 4072-4077; the
dialog grew to 545 x 442 and Local lights moved up - his layout, nothing else touched):
Vessel self-shadows + filter (stock), map size 1024/2048/4096, "Terrain and world shadows"
Off / Stencil / Projected / Cascaded (ORO), Cascade detail 1024/2048/4096 with the MB
beside it, Cascade reach 5-60 km, Soft far shadows, Local light shadows; ORO-only rows grey
out unless Cascaded is selected. Cascades are DECOUPLED from ShadowMapMode: self-shadows
NONE + Cascaded keeps every ground and building shadow and loses only cockpit shadows and
the finer hull map (round 11: `VPlanetAtmo.cpp` adds the terrain `_SHDMAP` flag when
TerrainShadowing == 3 too, and the cascade term sits OUTSIDE the `#if SHDMAP > 0` blocks
in PBR.fx / Metalness.fx / Vessel.fx). His ruling on the user-confusion question: apply
the fix, keep both settings.

**LANDMINES, each bought:**
- THE LEGACY EFFECT COMPILER. fxc compiles fx_2_0 with the old front end, which packs
  constants differently from a standalone entry point: MetalnessPS sat at c220 inside the
  effect against c165 standalone, and 23 -> 17 -> 13 registers of cascade constants all
  failed X4550/X4507 where the standalone check passed. Bisected (variants A-H): exactly
  TWO inlined lookups fit - hence the per-pixel hull-box choice. Measure the EFFECT.
- Patch (p)'s FAST_PS line read `fShadow` OUTSIDE its `#if SHDMAP > 0` block: X3004 for
  anyone with Vessel self-shadows None, since 2026-08-09 - his round-11 test was the first
  to run that configuration in a month. `tools/fxccheck.sh` gained the SHDMAP=0
  configurations (21 in the matrix).
- Instruments from day one (the (ab) rule): `ShadowDebug` > 0 logs the cascades'
  anchor-camera distance, the nine slots' texel sizes and the hull-box count once a
  second; >= 3 dumps `ORO_cascade_atlas.dds`.

Files: `Scene.h/.cpp` (CASCADE, `FitCascade`, `RenderCascadeCasters`,
`RenderCascadeShadows`, `GetCascadeConstants`, the anchors), `Surfmgr2.cpp` (the atlas
bind, `viaAtlas`), `VPlanetAtmo.cpp`, `D3D9Effect.h/.cpp`, `D3D9Config.h/.cpp`
(`ShadowCascadeSize/Far/Soft`, `ShadowDebug` 0..4), `VideoTab.cpp`, `D3D9Client.rc`,
`resource.h`, and FIVE deployed shaders: `D3D9Client.fx`, `NewPlanet.hlsl`, `PBR.fx`,
`Metalness.fx`, `Vessel.fx`. No gcCore surface, no guards.

Known edges, parked: probes and mirrors do not sample the atlas; at most two overlapping
hull boxes shade a hull pixel; the far cascades re-render every frame.

### The moire fix (2026-09-13): the taps stop shadowing their own surface at a grazing sun

His screenshots, rolling a DeltaGlider about its z axis in orbit: a fine hatched moire
filling parts of the vessel's own shadow, sweeping as the hull rolled; a hangar wall and
the ground at a low sun the same way. His four-run bisection put it in the client's
cascade term with ORO.dll unloaded (stock `ComputeShadow` is byte-stock), i.e. in
`OroCascTapC` / `OroCascadeShadow` (D3D9Client.fx) and their terrain twins
`OroCascTapT/WT` / `OroCascadeShadowT` (NewPlanet.hlsl).

**The mechanism, in the tap's own terms.** The shadow pass rasterises front faces, so a
lit receiver compares against a quantised copy of itself and only the bias separates
them. The receiver-plane slope `g` (metres of depth per metre across the light plane,
`N.U / N.L`) makes the compare exact for a planar receiver at ANY angle - as long as it is
not clamped. It was clamped at 4 (tan 4 = 76 deg) per component, and the blind bias'
`grz` was the same tangent clamped to the same 4 (so the two "same" numbers disagreed by
up to 41% on the diagonal). Past the clamp an UPHILL tap's true depth runs away from the
extrapolated plane by `(tan - 4) x s` per texel of reach `s`; against it stood half a
texel of sin-scaled normal offset plus 2.5 texels of bias. Written out (texels of depth):
the receiver shadows itself when `1.41 (tan - 4) > 0.5 tan + 2.5`, i.e. past tan 9 (84 deg)
in the bilinear footprint's diagonal taps and past tan 13 (85.6 deg) in all of them. The
focus vessel sits in slot 0 at ~13 mm a texel (4096), so the stripes land near one texel
per pixel and read as a BEAT rather than as stripes - why the artifact looked far coarser
than its cause.

**The fix, and the arithmetic that makes it a fix rather than a tuning.** A normal offset
of `k` texels x sin(grazing) moves the receiver nearer the light than its own surface by
`k / cos` = `k x tan` texels of depth - THE SAME GROWTH THE ACNE HAS - while moving the
lookup sideways by at most `k x sin^2 <= k` texels. So the margin is `k tan + 0.5 + 0.5
min(tan, C)` against a worst tap of `1.41 (tan - C)`: at k = 1.5 the margin wins for ANY
tan and ANY clamp C (the tan coefficients are 1.5 against 1.41), and it survives ten
degrees of smooth-vs-flat normal disagreement, which no blind depth bias could. (The
2026-09-13 ledger note "normal-offset shadows want TAN, not sin" was wrong as written: a
sin-scaled offset already yields a tan-scaled DEPTH margin - the coefficient was the
fault, 0.5 against the 1.41 of the diagonal tap.) Three changes, both copies:

1. **The normal offset is 1.5 texels x sin** (was 0.5; the terrain's wide tent keeps its
   1.5x ratio, 2.25 against the old 0.75). This alone ends the acne at every angle.
2. **The slope clamp is 16 (86 deg)** (was 4), and it is ONE number now: `tn` = the true
   tangent (20 at the `nl` floor of 0.05), `grz = min(tn, C)`, and `g = ln.xy x grz / sn` -
   the clamped tangent pointed along the normal's shadow in the light plane, so the slope
   vector and the bias agree by construction. Raising C only helps: a clamp UNDER the true
   slope makes downhill taps read lit too early (a contact-shadow leak of `(tan - C) x s`
   texels of depth) as well as uphill taps read shadow (the acne); an overstated smooth
   normal errs the other way and is absorbed by the offset. The terrain had carried a
   second floor (0.15) and clamp (6) for the vector alone - gone.
3. **Both numbers are cfg keys read from SPARE LANES**, so they tune without a rebuild and
   cost the register-bound shaders nothing: `ShadowCascadeSlope` (1..32, default 16) and
   `ShadowCascadeOffset` (0..8 texels, default 1.5), hidden (no UI). Terrain: nine slots
   fill `vCascTx[0..1]` and `[2].x`, so `vCascTx[2].y` takes the clamp; `vCascBasis[2].w`
   (the L row's spare) takes the offset. Mesh family: the eighth texel lane `gCascTx[1].w`
   (seven slots) takes the clamp; `gCascBasis[1].w` - the terrain's soft-far switch, which
   the mesh taps have no tent to spend - takes the offset. `Scene::GetCascadeConstants`
   fills the terrain's lanes, `RenderCascadeShadows` overrides the mesh's two after it.

**Instrument, before tuning (the (ab) rule): `ShadowDebug 5`** blacks every receiver whose
TRUE slope exceeds the clamp and half-darkens one past half of it, the real shadow
everywhere else - set `ShadowCascadeSlope 4` with it and the black is exactly the set of
receivers the old clamp was failing on. Both copies fold it in arithmetically off literals
the shaders already held (`4.0f`, `0.5f`, `3.5f`), so no bool register and no new
constant; mode 4 (the slot bands) is gated to exactly 4 now. `D3D9Config` clamps the key
to 0..5; the per-second `ORO shadow dbg: cascades` line prints both knobs.

**Measured (tools/fxccheck.sh, 25/25 green before and after):** D3D9Client.fx registers
UNCHANGED in every configuration (his 155/224, worst shipped 195/224), slots +9 (3575 ->
3584, worst 3855 -> 3863); NewPlanet.hlsl TerrainPS Earth+DEVTOOLS c223 -> **c221** and
55 -> **53 literal registers** - the dead per-component clamp literals left - slots 2734 ->
2742, samplers 16/16 unchanged. The one shader with no headroom gained two registers.

**What to watch when it is flown:** peter-panning at contact points - the lookup now
moves up to 1.5 texels sideways at grazing (13 mm-texel hull box: 2 cm; the 50 m cascade
at 2048: ~4 cm; the 2 km cascade: ~1 m) - a vessel parked at sunset and a building under
a low sun are the judges; and, with the clamp at 16, the blind bias' `0.5 x grz` term
reaches 8 texels of DEPTH at the clamp, which is half a texel sideways. If either shows,
the keys are the levers and the shaders are runtime-compiled (edit, copy, restart).
Not touched, deliberately: front-face culling in the shadow pass - Orbiter content is
mixed single/double-sided and thin, so a culled wing's shadow would float off by the
wing's own thickness and a single-sided mesh would stop casting.

Files: `shaders/D3D9Client.fx`, `shaders/NewPlanet.hlsl` (both DEPLOYED, runtime-compiled),
`Scene.cpp` (`GetCascadeConstants`, `RenderCascadeShadows`, the debug line),
`D3D9Config.h/.cpp` (the two keys, `ShadowDebug` 0..5). No gcCore surface, no guard, no
new patch letter - it is patch (ae)'s. Client stamp `[Build 260913]` (the same day as the
(al) rebuild - the stamp cannot tell them apart); the live client before it is backed up as
`D3D9Client.dll.pre-moire-260913`.

## Patch (af): TERRAIN FLATTENING UNDER CUBIC INTERPOLATION (2026-09-06)

A stock bug of the (q)/(z) family, one call. Stock flattens each elevation tile's FLOAT
copy (`FilterElevationGraphics`) and the core's physics tiles (`clbkFilterElevation` ->
`FilterElevationPhysics`) and never the RAW INT16 file array a `SurfTile` keeps as
`elev_file`. In LINEAR mode a file-less child upsamples its parent's float copy, already
flat, so the flat is inherited down the tree. In CUBIC mode - the core's DEFAULT - a
file-less child hands the nearest ancestor's raw array to the core's spline
(`LoadElevationData` -> `ElevationGrid`) and the result is never filtered; near the ground
every tile in view is a file-less child, so the drawn terrain kept its hills while the
vessel stood on the flattened physics height. The splash code still carries the
commented-out "Terrain flattening offline due to cubic interpolation".

`FilterElevationFile` (VPlanet.cpp) filters the file array at its own level with the
physics filter's exact arithmetic (INT16, elev_res units); `ReadElevationFile` calls it
beside the float filter. Cubic children now inherit the flat at any depth, and the drawn
mesh and the physics ground derive from the SAME integer-rounded flattening. The array has
exactly one reader (the cubic branch); linear mode and flattening-off are untouched. Flown
at Antelope Valley with cubic: "Terrain flattening works with cubic interpolation."

Files: `VPlanet.h`, `VPlanet.cpp`, `Surfmgr2.cpp`. DLL-only.

## Patch (ag): THE ANIMATED BASE OBJECTS REVIVED - trains and solar plants (2026-09-07/08)

Orbiter's base definition files carry three object types that MOVE - `TRAIN1` (a
monorail cabin shuttling between two ends), `TRAIN2` (two cabins hanging under a girder
rail on legs) and `SOLARPLANT` (a panel array tracking the sun) - and all three have been
dead under every graphics client since the client split. Only the core's INLINE renderer
animates them (`Src/Orbiter/VBase.cpp` calls each object's `Update()` and the D3D7 draw
routines of the two that render themselves); a client receives a base only through
`GetBaseStructures()`: compiled meshes, copied once, every generic object sharing a texture
merged into one group. So the monorail cabin sat frozen at one end of its beam, the
hangrail's cabins floated frozen with no rail under them (the rail is inline-only), and a
solar plant was invisible (it never exported at all). The core lifts the two track ENDS to
the terrain (`Base::Setup` -> `Train::Setup`) but draws a straight chord between them.

**The revival** (`OroBaseAnim.h/.cpp`, new; hooks in `VBase.h/.cpp`, `D3D9Client.cpp`,
`CMakeLists.txt`): at `vBase` construction the client re-reads the base's own cfg for the
three block types (the patch (h) shape - the client reads the file itself; the lookup
mirrors `Planet::ScanBases`: `Config\<planet>\Base` or the planet cfg's `BEGIN_SURFBASE DIR`
entries with `PERIOD`/`CONTEXT` limiters, the base named by its file STEM or by an inner
`Name =` line - `Brighton.cfg` is "Brighton Beach"), builds its OWN meshes for the moving
and missing parts, APPENDS them to `structure_as` (so the render, the depth pass (z2), the
local-light and cascade casters (z3/ae), the night-texture flip and the bounding box all
take them with no further plumbing), animates them in `vBase::Update` on sim time, and
collapses the core's frozen exports (the cabins in the over-shadow generic mesh, the beam
in the under-shadow one) to a point 100 m underground via `EditGroup` so nothing draws
twice. The core's own tables (`cabin1`, `cabin2`, `mrail1`, the girder portal) and laws
(`Train::MoveCabin` verbatim: 1 m/s at the ends, a linear ramp over `SLOWZONE`) are copied
so a 2010 base looks as its author saw it. The trains run on sim time CAPPED at x100 real
time (his ruling) and sub-stepped at 0.1 s so end ramps and reversals are never skipped.

**Terrain** (his three questions settled it): the ground stays the ground and THE
STRUCTURE ADAPTS. Flattening a strip along the track was rejected (stock elevation tiles
are ~1.2 km per texel almost everywhere, it changes the physics ground for every vessel
nearby, and it is the wrong model - railways cross terrain on viaducts). The terrain is
sampled along the chord every 25 m through `oapiSurfaceElevation` in base-local
coordinates (`+z` east, `+x` south per `Base::Rel_EquPos`; the sphere's curvature drop
included), the rail line is the slope-limited UPPER ENVELOPE under an 8% grade (forward
and backward passes, 1-2-1 smoothed, clamped to ground + lift), pylons drop from the beam
wherever it leaves the ground by more than 0.5 m, the hangrail's portals stand at the
core's ~200 m spacing with their feet cut to the ground, and each solar panel stands at its
authored height above the ground UNDER IT with its stand's three feet cut to the ground
under each foot. Visual only, as in 2010.

**The hide matcher is SHAPE-based, and Brighton Beach is why.** The core rotates every
cabin about end 1 by the LIFTED chord's tilt (`SetCabin`: `sinth = (end2.y - end1.y) /
length` with the lifted ends). At Brighton (16.85 m over 1260 m = 0.77 deg) a hangrail
cabin hung 11 m under its girder moves ~15 cm along the track - cancelled by the chord's
foreshortening at the FAR end only - so an absolute 0.1 m x/z test found one cabin and
missed the other. The run's SHAPE is tested instead (the first vertex within 5 m across /
100 m up of the untilted reconstruction, every other vertex at its template offset within
0.5 m + 2% of the span; a cabin's y offsets included, the beam's not), and a miss LOGS its
closest candidate's residual.

**The solar plant**: the core's `SolarPlant::Activate/Update` geometry - 16 x 8 m plates
at `SCALE` on 10 x `SCALE` tripod stands, `nrow x ncol` at `SPACING`, rotated by `ROT`
about `POS`; aimed at the sun every 60 s of sim time (the long axis tilts toward it, the
short axis stays horizontal; the sun and camera arrive in BASE-LOCAL coordinates from
`vObject`'s `sundir` and `cpos` through the base's `grot`); the 2010 GLINT (a panel whose
normal points within 2.6 deg of the camera swaps to the texture's bright column) plus a
real specular term. Two departures: the tilt is capped at 75 deg (the core stood the panels
VERTICAL under a horizon-clamped sun) and the stand's apex sits just under the pivot (the
core's poked 4 x `SCALE` through the plate). Per-frame `EditGroup` with an index list moves
only the panel vertices. `solpanel.dds` is the stock texture (it is in `Config\Base.cfg`'s
generic list, with the four-column layout the block expects). No stock base carries a
SOLARPLANT block - a test block:

```
SOLARPLANT
	POS -700 0 -700
	SCALE 1
	SPACING 40 40
	GRID 4 6
	ROT 20
	TEX solpanel
END
```

**THE CORE'S SolarPlant CORRUPTS THE HEAP AT SESSION CLOSE - a stock bug, found by four
exits in a row at 0xC0000374 (ntdll, WER) the moment plants sat at three bases.**
`SolarPlant`'s constructor never initialises `ppos`, `Vtx`, `Idx`, `flash`, `ShVtx`,
`ShIdx`; `Activate()` allocates them; `~SolarPlant -> Deactivate()` `delete[]`s them
UNCONDITIONALLY (the trains guard theirs with `dyndata`). Under a graphics client a base's
objects are activated only when the client asks for its structures (`ExportBaseStructures
-> ScanObjectMeshes`), i.e. when its VISUAL is created (apparent radius > 2, or
`PreLBaseVis` at the focus planet), so a SOLARPLANT block in any base the session never
visited is destroyed with garbage pointers at `Base::~Base` - during the planetary
system's teardown, after `[Session Closed. Scene deleted.]`. The inline renderer activated
every object of every base at start, which is why nobody ever saw it. The core cannot be
patched from here, but the trigger can be pulled: `OroBaseAnim::ArmCoreSolarPlants`, at
the top of `clbkCloseSession`, scans every base cfg of every body for a SOLARPLANT block
and calls `GetBaseStructures` on those bases, which runs `Activate()` on each object (once
- `objmsh_valid`). At CLOSE rather than start, so `Base::Setup` (the terrain lift, the
mesh elevations) has run and a visited base is untouched. Flown: "Exit to launchpad
worked cleanly." A line for the orbitersim/orbiter list.

Log lines: `ORO base '<name>' - N monorail(s), N hangrail(s), N solar plant(s) with N
panel(s) revived from <cfg>`; per run `the core's frozen <cabin|beam> collapsed (the core
had lifted it X m)` or `was not found ... closest run mesh M group G vtx V, first vertex
off by (x, y, z) m, worst shape deviation N x tolerance`; at close `N solar-plant base(s)
of N activated before teardown`.

Files: `OroBaseAnim.h`, `OroBaseAnim.cpp` (new), `VBase.h`, `VBase.cpp`, `D3D9Client.cpp`,
`CMakeLists.txt`. DLL-only: no shader, no gcCore surface, no guards. Client `[Build 260908]`.

## Patch (ah): THE LIGHTS PAY FOR WHAT THEY USE - struct repack, block early-out, eviction (2026-09-08)

Step 1 of the lights-and-shadows plan (`beta/reports/260906/LIGHTS_SHADOWS_INVESTIGATION.md`
in the private repo). Three changes, output bit-identical, flown.

**1. The GPU light struct is four float4, not six.** `struct Light` in `D3D9Client.fx` and
`LightStruct` in `D3D9Util.h` (mirrored byte for byte - `ID3DXEffect::SetValue` copies the C
struct raw). The shaders read exactly position, direction, attenuation.xyz, diffuse.rgb,
cos(phi/2), the theta scale and the spot flag; `dst2`, `range`, `falloff` and `diffuse.a` were
never read by any shader. Now `position.w` = cos(phi/2) (1 for a point), `direction.w` = theta
scale (0 for a point), `attenuation.w` = type (0 point, 1 spot). `Type` and `Dst2` move to
`D3D9Light` (CPU only), with `GetRange()`, `GetCosPhi()`, `Pos3()`, `Dir3()` accessors for the
CPU consumers (Scene.cpp's local-map selection and glare code, Surfmgr2's terrain copy, which
keeps the terrain's own `LightF` layout and unpacks into it). `Common.hlsl` reads `.xyz` / `.w`.

**2. One coherent branch per block of four lights** (`LocalLightsEx`, Common.hlsl). Mesh.cpp
sorts a mesh's lights by illuminance and zero-fills the rest, so a black FIRST light of a block
means that block and every later one are empty. `gLightsEnabled` was a dead guard (it zeroed
two values the loops then overwrote); it is the outermost branch now. The client compiles with
`PREFER_FLOW_CONTROL`, so these are real branches on constant registers.

**3. `Scene::AddLocalLight` evicts the true farthest.** Stock scanned for `Dst2 > lmaxdst2` -
strictly greater than the running maximum - which matched nothing after the first eviction, so
`imax` stayed 0: the newest nearby light always replaced slot 0 and `lmaxdst2` shrank to its
distance, refusing everything farther. With more than 24 emitters near the camera the kept set
depended on registration order and camera position, and the lighting popped as the camera
moved. A stock finding for the orbitersim list.

**Measured with `tools/fxeff`** (new in the private repo: `D3DXCreateEffectFromFile` with the
client's exact flags on a NULLREF device, per technique pass the float-constant registers,
samplers and instruction count). The Windows-Kit `fxc /T fx_2_0` in `fxccheck.sh` rejected a
tester's live 8x Full + Cascaded configuration (X4507 / X4505) while that tester's client ran
it - the D3DX compiler is the arbiter for this effect, fxc is not. PBR pass, cascades + glass +
env + irradiance + glow + dev tools, shadow kernel 27:

| lights evaluated | before (6 float4) | after (4 float4) | instructions / pixel |
|---|---|---|---|
| 4 Full | 135 | 124 | 1726 -> 1744 |
| 8 Full | 163 | 140 | 1978 -> 1996 |
| 12 Full | 191 | 155 | 2231 |
| 16 Full | does not compile (X4507) | 171 | 2484 |

Declared-but-unevaluated lights cost nothing (only evaluated blocks occupy registers); each
block of four adds ~250 instructions per pixel; the +18 after is the two branches.

Files: `D3D9Util.h`, `D3D9Util.cpp`, `Scene.cpp`, `Surfmgr2.cpp`, `shaders/D3D9Client.fx`,
`shaders/Common.hlsl` (both DEPLOYED - a matched set with the DLL). No gcCore surface, no
guards. Client `[Build 260908]`, backup `D3D9Client.dll.pre-ah-260908`.

### Step 2 - the single spot map: sticky, caster-fitted, bilinear (2026-09-08, flown)

- **Sticky selection** (`Scene::RenderLocalLightShadowMap2`): the light that held the map
  last frame (`lsmap.prevLe`, the `LightEmitter*` - stable across frames where scene indices
  are not) keeps it unless a challenger scores >= 1.5x. Marg's pad: two floodlights of similar
  score had swapped the map as the camera moved.
- **Caster-fitted frustum**: the spot cone is the ceiling; `OroFitCasterSphere` (D3D9Util.h)
  widens a fitted half-angle and far distance per caster sphere that intersects the cone -
  vessels (owner excluded) and the above-shadow base structures through
  `vBase::FitLocalShadowCasters` / `vPlanet::FitBaseLocalShadowCasters` (RenderBaseDepth's
  guards). `fov = min(cone, 2 * max(1.08 * fit + 0.02, cone / 4))`, `zfar = min(range,
  1.05 * farFit)`, near re-derived. Receivers outside the map read as lit (the shaders' bounds
  test already did that). Terrain tiles are deliberately NOT in the fit (kilometre-wide
  spheres would undo it), hence the half-cone floor and its one trade: a ridge far off-axis can
  lose its shadow beyond the floor when a vessel or building is in the beam.
- **2x2 bilinear PCF** in `SampleLocalShadowV` (Common.hlsl) and `SampleLocalShadow`
  (NewPlanet.hlsl, in map or atlas-cell space - the clamp keeps taps in the cell): the four
  texels around the sample weighted by the sub-texel position. Same tap count as before.
- **`LocalLightSelfShadow`** (D3D9Config, hidden, default 0): 1 admits the emitter's own vessel
  as a caster in both the pass and the fit. A diagnostic; emitters authored inside hulls black
  out their own beam with it on.
  ⚠️ **AMENDED 2026-09-13 - as first written the flag could not do anything for the case it
  exists for.** It admits the owner at `isOwner()`, and the containment rule below (a sphere
  holding the light serves no map) rejected it again immediately. A spot merely lost the fit;
  a POINT light lost its map entirely, because `aimCasters()` doubles as the viability test
  and a light with no caster opens no cell. Since emitters are *routinely* authored inside
  their own hull - the stock DeltaGlider's main-engine light sits 11.32 m inside a 13.17 m
  bounding sphere - the flag was inert in practice. Now `ownsLight()` (the pure ownership
  test, split out of `isOwner()`) exempts the owner's own sphere from the containment skip in
  the aim, and `OroFitCasterSphere` takes `selfHull` (default false) to do the same in the
  fit; a contained sphere takes the full cone, which is what a hull wrapped around the light
  needs. The aim weight is capped at 1 so a contained hull cannot swamp the other casters.
  **With the flag at 0 the arithmetic is unchanged**: the cap is a no-op wherever the sphere
  does not contain the light, and the `d < 1e-3` guard is unreachable unless `selfHull` is set.

`tools/fxccheck.sh`'s effect rows now call `tools/fxeff` (the D3DX compiler); fxc had begun
rejecting the effect on boolean registers (X4550) that D3DX packs within the limit (b# = 16).
Files: D3D9Util.h, Scene.h/.cpp, VBase.h/.cpp, VPlanet.h/.cpp, D3D9Config.h/.cpp,
`shaders/Common.hlsl`, `shaders/NewPlanet.hlsl` (both DEPLOYED). Backup
`D3D9Client.dll.pre-ah2-260908`.

### Step 3 - 12x and 16x local lights; MESHGROUPS (2026-09-08, flown)

- **Launchpad rows** `12x Partial`, `12x Full`, `16x Partial`, `16x Full` (VideoTab.cpp;
  `LightConfiguration` 5-8). `D3D9Config::MaxLights()` returns 12 / 16 for them; the load
  clamp `max(min(4, i), 0)` (both spellings of the key) is raised to 8 - it had pinned the
  stored mode at the old maximum. `MAX_MESH_LIGHTS` in D3D9Client.h is unused; comment says so.
- **Common.hlsl** `LocalLightsEx`: blocks at offsets 8 (`LMODE >= 5`) and 12 (`LMODE >= 7`),
  nested inside the previous block's branch. Full = `LMODE` 2, 4, 6, 8 written out - the D3DX
  preprocessor rejects `%` (X1500). fxeff, PBR pass, cascades + glass + env + irradiance +
  glow + dev tools: 12x Full 156, 16x Full 172, 16x Full + kernel 35 = 180 of 224. Terrain is
  unchanged at four per tile.
- **`MESHGROUPS m first last`** in `_ecam_oro.cfg` (camera and plane blocks): `ENVCAMREC` /
  `ENVPLNREC` gain `short* pGrpMesh` (one per range, -1 = every mesh, freed with the ranges);
  the parser fills it (-1 for `GROUPS`); `vVessel::PreInitObject` stamps a range onto mesh
  `m` only when `pGrpMesh[r] >= 0`. `GROUPS` keeps its all-meshes meaning. The shipped
  `Config\GC\Atlantis_ecam_oro.cfg` carries a per-line syntax block.
- Files: VideoTab.cpp, D3D9Config.cpp, D3D9Client.h, MaterialMgr.h/.cpp, VVessel.cpp,
  `shaders/Common.hlsl` (DEPLOYED). Backups `D3D9Client.dll.pre-ah3-260908` (before the rows)
  and `.pre-meshgroups-260908`.

### Step 2b - two rules for the caster fit (2026-09-09, flown)

His SSV screenshots after step 3: ~80 cm texels on the SRB, one flood's shadow only. The pad's
stadium lights are declared with a 180-degree penumbra (`LC39.cpp`), so the CONE is a
hemisphere; the fit should have narrowed it to the stack and did not, because a mast standing
inside the MLP's bounding sphere tripped the rule "light inside a caster's sphere = full
cone". Two changes, both general:

1. `OroFitCasterSphere` (`D3D9Util.h`) returns false for a sphere that contains the light. Such
   a caster can shadow every direction, so no perspective map serves it, and it must not widen
   the map for the casters the map CAN serve.
2. Both fit loops (`Scene::RenderLocalLightShadowMap2`'s vessel loop and
   `vBase::FitLocalShadowCasters`) count only casters visible to the camera
   (`Scene::IsVisibleInCamera`). A shadow on a visible receiver lies on the light's ray to that
   receiver, so the caster that throws it is inside the fit already; casters behind the camera
   only cost texels.

Output on stock vessels unchanged (flown). Backup `D3D9Client.dll.pre-fitfix-260909`.

### Step 4 - N spot maps in the atlas (2026-09-09, flown)

Up to `LocalLightShadowMaps` (1 / 2 / 4 / 6, default 4; the Launchpad's "Spot light shadows"
combo, which replaced the "Local light shadows" checkbox - Off keeps the last count) spotlight
shadow maps a frame. `Scene::RenderLocalLightShadowMap2`:

1. **Rank** the shadow-casting spots (type 1, range >= 5 m, within 2 km, lit, cone < 180 deg) by
   `lum x range^2 / (100 + d^2)`; a light that led a map last frame (matched by emitter) ranks
   at 1.5x - the step-2 hysteresis, per map.
2. **Cluster**: walking the ranking, a light joins an existing map when it stands within 3 m of
   that map's leader and aims within 15 degrees of it, else opens a map while cells remain. A
   cluster's map renders from the centroid along the mean axis with a cone covering every
   member's; the error is the lamp spread over the caster distance.
3. **Render** each map through the scratch target (the (z3) caster set: vessels except the
   members' owners, base structures, registered terrain tiles; the caster FIT of steps 2/2b on
   the field of view only) and **copy it into its cell**, point-sampled: in TerrainShadowing 3
   the cascade atlas' spare row (`{k*q, 3q, (k+1)q, 4q}`, q = cascSize/2; the copy happens
   BEFORE `RenderCascadeShadows`, whose clear covers rows 0-2 only, so `cascLclLive` is the
   local pass's flag now); in modes 0-2 a new local atlas `ptLclAtl` of half-size cells (2x2
   up to four maps, 3x2 for six; 16 / 24 MB at a 2048 scratch); or nowhere when one map is all
   there is (the scratch is read at full size).
4. **Tell the lights**: `Lights[i].Diffuse.a = cell + 1` for every member (`UpdateLight` zeroes
   it; no shader ever read the lane). Mesh.cpp's `LightStruct` memcpy carries it to every mesh
   and Surfmgr2 writes it into the terrain light's Falloff lane - the per-slot matching in both
   is GONE.

**The far plane is the light's reach.** Step 2 fitted it to the farthest vessel or building,
which clipped a caster's shadow off the ground beyond it and kept terrain ridges (never in the
fit) out of the map. The near plane is `ORO_LCL_NF x range` (0.0075 - 0.75 m on a 100 m light,
the old floor) so the receivers need no per-map near/far.

**The receivers derive the frame** (`OroLclShadow` in Common.hlsl, `SampleLocalShadow` in
NewPlanet.hlsl) from two float4 per map - `gLclShdP[k]` = (origin, cell*10 + tan(fov/2)),
`gLclShdD[k]` = (axis, range) - plus `gLclShd` = (live maps, cells per row, first row's v, cell
texels) and `gLclAtl` = (cell w, cell h, texel u, texel v), all from `Scene::GetLocalShadowConstants`
(pushed once per frame for the mesh family in `PushLocalShadowConstants`, once per planet render
for the terrain). The basis is LookAtRH's (`x = cross(D, up)`, `y = cross(x, D)`, up = the same
axis-avoidance rule as the CPU), the depth is the caster's `1 - z/w` reconstructed as
`c (f/d - 1)`, c = NF/(1-NF), with the step-2 relative and texel-footprint biases; 2x2 bilinear
PCF inside the cell. Six maps cost 12 registers (16x Full worst: 189/224). The arrays are sized
by the `LCLMAPS` effect macro (D3D9Effect.cpp) and the `_LCL2/_LCL4/_LCL6` terrain flags
(VPlanetAtmo.cpp), so the common configurations pay for what they use.

**THE 4096 INSTRUCTION SLOTS.** A ps_3_0 shader is also bounded by the adapter's
`MaxPixelShader30InstructionSlots` - 4096 on the GTX 970 - and the first 16x build crossed it
(the shadow test inlined for sixteen lights: every vessel pass 4400-4758 slots): the DG hulls
stopped rendering at 20 fps with nothing logged, while 12x at 3946 worked. Shadow tests run in
the first two light blocks only (`ORO_LCL_SHDBLOCKS` 2, Common.hlsl - a mesh's eight strongest
lights; six maps at most exist, so a shadowed spot below eighth on a mesh is the rare case and
faint there). 16x worst = 3400. `tools/fxeff` reads the HAL cap (`-slots N` to pin one, `-asm
file` to dump a pass's disassembly), prints `ps instr-max N/CAP slots` and returns 1 above it;
`fxccheck.sh` pins `-slots 4096`; `D3D9Frame.cpp` logs `MaxPS30InstrSlots` in the startup caps
block. ONE shader for every card: a per-GPU macro would be two shaders to test.

Instrument: `ShadowDebug >= 1` writes `ORO lcl dbg:` once a second - maps, candidates, where they
live, and per map the leader, cluster size, fov, range and caster count.

Files: `Scene.h`, `Scene.cpp`, `Surfmgr2.cpp`, `Mesh.cpp` (three sites), `D3D9Util.cpp`,
`D3D9Effect.h/.cpp`, `D3D9Config.h/.cpp`, `VPlanetAtmo.cpp`, `VideoTab.cpp`, `D3D9Client.rc`,
`D3D9Frame.cpp`; `shaders/D3D9Client.fx`, `shaders/Common.hlsl`, `shaders/NewPlanet.hlsl` (all
three DEPLOYED - a matched set with the DLL). No gcCore surface. Client `[Build 260909]`,
backups `D3D9Client.dll.pre-ah4-260909` / `.pre-ah4b-260909`.


### Step 5 - point lights cast too: the aimed map and the five-face cube (2026-09-09/10, flown)

A point light has no axis, so `LocalLightShadowPoint` (the Launchpad's "Point light shadows" -
Off / Aimed / Cube, default Aimed) gives it one of two shapes:

**Aimed (1)** - a PSEUDO-SPOT. The map is aimed at the solid-angle-weighted mean direction of the
visible casters within reach (`vVessel::AimLocalShadowCasters`, `vPlanet::AimBaseLocalShadowCasters`
- the step-2/2b fit functions with the aiming pass in front), capped at `ORO_LCL_PT_HALF` (1.2 rad
half-angle) and then fitted to those casters like a spot's. A point light with nothing to cast gets
no cell at all.

**Cube (2)** - FIVE cells when five are free: faces down, +X, -X, +Z, -Z of a basis built on the
LOCAL VERTICAL (the sky face has nothing to shadow), each a 93-degree map (`ORO_LCL_CUBE_TAN` 1.05
= 90 degrees plus margin). The light carries `Diffuse.a = 100 + base cell`, and the receivers pick
the face from the dominant axis of the light-to-pixel vector in a basis rebuilt from face 0's axis
with the CPU's own rule - so one fetch serves all five faces and no matrix crosses the boundary.
The cube's pick lives behind the `_LCLCUBE` compile flag (effect macro + terrain flag), ~440 slots
that only the Cube row pays for.

**Spots rank first, always.** The spot shadows are the flagship, and ORO's own lightning flash is a
15 km point light on the focus vessel: it may borrow a spare cell for a flash's building shadows,
never displace a pad's floods. A point map's far plane is capped at `ORO_LCL_PT_RANGE` (1000 m).

FLOWN 2026-09-10 with the `testlights` rig's case 9 (a 10 m mast, four ShuttlePBs at 16 m, Cube +
6 maps): four radial shadows, the log reading `5/6 cells, 1 candidates`. Getting there cost two
diagnostic rounds in the DEPLOYED terrain shader (face-coded sector shading, then the same with the
shipped occlusion), and both said the receiver was correct; one earlier run had shown no shadows at
all and never reproduced - **an open item, unexplained**.

⚠️ **THE STANDALONE SHADERS HAVE THEIR OWN COMPILER, AND IT IS NOT fxc.** The first diagnostic
compiled clean under the Windows-Kit fxc and made his sim refuse to start: `X4507, maximum ps_3_0
constant register index (224) exceeded`. `ShaderClass` compiles through `D3DXCompileShaderFromFile`
in **d3dx9_43.dll** (the June 2010 SDK), which allocates literal constants into the same 224
registers and packs them differently; the Earth terrain shader with six maps + cube + dev tools
measures **c223 of 224** there, so ONE new literal is fatal. `tools/d3dxps.py` drives that very DLL
through ctypes and prints the highest register, the literal count, the slots and the samplers;
`fxccheck.sh`'s terrain rows run through it now. This is `tools/fxeff`'s lesson (the effect files,
2026-09-08) repeated for the standalone entry points - **the Windows-Kit fxc is not evidence about
either.** ⚠️ It also means the terrain shader has NO register headroom in that configuration: the
next thing added there must first take something out.

⚠️ **THE `cast` FIELD LIES FOR A CUBE.** `ORO lcl dbg:` prints the FACE COUNT (5) in the caster
column for a cube light, not a caster count - it is `dbgCast[k] = 5` in the cube branch. Reading it
as casters wasted a round. Give it an honest count when the point path is next touched.

Files, over step 4's: `Scene.cpp` (the aiming and cube branches, `renderCell`'s `kind`),
`VBase.h/.cpp`, `VPlanet.h/.cpp` (the two Aim* functions), `D3D9Config.h/.cpp` (the new key),
`D3D9Effect.cpp` + `VPlanetAtmo.cpp` (the `_LCLCUBE` flag), `VideoTab.cpp`, `D3D9Client.rc`,
`resource.h` (the combo, `IDC_LCLPOINT` 4078, the dialog 545x472); `shaders/Common.hlsl` and
`shaders/NewPlanet.hlsl` (the face pick, both DEPLOYED). No gcCore surface. Client `[Build 260909]`,
backups `D3D9Client.dll.pre-ah5-260909`.


---

## Patch (ai): THE RAIN REACHES THE PAVEMENT AND THE SEA, AND THE HUD STAYS READABLE (2026-09-10/11)

Four ORO asks in one client round, all of them wet-weather and all of them flown:

- **A5 - the drop glint gets a base-local scale.** `WetSparkle` (D3D9Client.fx) had one UV
  source, `tex0 * 34`, which is the mesh's own texture coordinates: right on a hull, wrong on a
  base structure whose runway texture stretches over a kilometre, where the sparkle became a
  handful of enormous blobs. New uniform `gBaseLocal` (set in `vBase`'s three draw brackets,
  cleared after) switches the lattice to a TRIPLANAR base-local UV built from the world position -
  `float2 uvS = lerp(tex0 * 34.0f, uvB * 2.0f, gBaseLocal)` - so a rain hit is the same size on a
  taxiway as on a wing. `WetSparkle` takes `camW` for it; the three call sites (PBR.fx, Vessel.fx,
  Metalness.fx) pass `frg.camW`.
- **A6 - no puddles on water, none on slopes, no grain on water.** `TerrainPS` (NewPlanet.hlsl)
  multiplies `pud` by `(1 - fMask)` (the shader's own water mask, already sampled for the stock
  water path) and by a slope gate, and lerps `gran` to 1 over water. ⚠️ THE GATE IS BUILT FROM
  CONSTANTS THE SHADER ALREADY CARRIED - `saturate(pow(saturate(dot(nvrW, vPlN)), 780.0f) * 1.6f)`
  - because the first attempt used `smoothstep(0.9945, 0.9994, ...)` and cost two new literals in
  a shader measuring **c223 of 224**. `tools/fxccheck.sh` refused it (X4507) before it reached his
  sim. TerrainPS is still at c223/224, 55 literals.
- **A7 - THE VC HUD IS DRAWN LAST.** ⚠️ **MASKING CANNOT WORK HERE AND THAT WAS ROUND 1.**
  Keeping the drop layer off glyph pixels does nothing, because a drop BESIDE a glyph still lenses
  that glyph's pixels into itself - the HUD was unreadable unless Drops lens went to 0. The fix is
  draw ORDER: `gcCore::SetDeferVCHUD(bool)` makes `D3D9Mesh::Render`/`RenderFast` hold back the
  `RENDER_VC` HUD group (`g_oroHudMesh/W/Tech`, one record per frame, cleared at the top of
  `RenderMainScene`), and `gcCore::DrawDeferredVCHUD()` replays it through `OroFlushVCHUD()` inside
  the cockpit dry/clear-air bracket. Patch (t)'s "the chrome goes last" pattern, pointed at the
  cockpit instead of the menu bar. ORO arms it only in a VC with the master armed, and calls the
  flush after its own world effects - so the drops still lens the WORLD and the numbers sit on top.
  The flush is UNCONDITIONAL after `RENDERPROC_HUD_2ND` as well, or an unarmed frame loses its HUD.
  Guard #20 `CanDeferVCHUD` checks BOTH bound pointers.
- **REFLECTIONS ON PAVEMENT AND ON OPEN WATER.**
  - `gcCore::SetWaterMirror(float)` (guard #21, clamped 0..2) is ORO's water fraction under the
    vessel, from `Mask.tree` through `OroTileTree`. It rides `gWetGrainPrm.w` and widens the wet
    block's gate in TerrainPS, so a mirrored ship appears over the sea with the RAIN EFFECT OFF.
    `Scene.cpp`'s mirror pass admits it with its own altitude window - **1500 m over water against
    250 m over wet ground** (the reflection is worth seeing from higher up at sea, and it fades
    linearly over the last 1000 m) - and **only when a vessel is within 2 km of the camera**, so an
    empty ocean costs nothing.
  - Runways, pads and taxiways are below-shadow base STRUCTURES on the vessel path, not terrain,
    so they had no wet code at all. New `WetOverlayTech` in Mesh.fx (one pass, SrcAlpha/InvSrcAlpha,
    depth test on, depth write off) draws the sky film and the mirrored vessel over them;
    `vBase::RenderWetOverlay` runs it after `structure_bs` while `g_gcSurfaceWet > 0.01`.
    ⚠️ **IT USES THE TERRAIN'S CONSTANTS, NOT THE BASE TILE'S, AND THAT IS THE WHOLE BUG STORY**:
    copying `BaseTilePS`'s UNBOUNDED sky colour (`ambE * 2.6`, which exceeds 1.0 under a storm)
    made a wet runway BRIGHTER than a dry one. The bounded `float3(0.42, 0.45, 0.49) * daylight *
    (1 + gStorm * 0.35)` is what the terrain beside it uses, and the two now agree.
  - **NO POOLS AND NO GRAIN ON PAVEMENT, DELIBERATELY.** Round 1 gave the runway the terrain's
    puddles and he called them "awful": copied from `BaseTilePS` they used a near-black darkening
    (`1 - 0.988 * gWetDark`) and a 48 m lattice period against an apron's 3-5 m. The physics settles
    it rather than the tuning - a runway is crowned and grooved precisely to shed water - so paved
    base surfaces get the film and the mirror and nothing else.
  - **The ground darkening moved into the vessel shaders.** A separate modulating pass over the
    finished pixel darkened the FOG as well, which is why zoomed-out runways read as black
    silhouettes that never blended with the weather. New uniform `gBaseGround` (set in
    `vBase::RenderSurface` only) makes all four vessel pixel paths apply
    `cDiff.rgb *= lerp(1, lerp(0.66, 1 - 0.494 * gWetDark, gBaseGround), gSurfWet)` to the ALBEDO,
    before fog - so a wet runway darkens with the apron beside it and fogs with the world.

⚠️ **AND A PRE-EXISTING TERRAIN BUG FELL OUT OF IT: the damp sky film had NO distance term.**
Only the pools faded with range, so at 20 km - where the LOD is coarse enough that normals are
essentially the sphere normal and a near-horizontal view drives the Fresnel term to 1 - every
distant pixel took the full 60% film and mountains read as a pale grey band. The pools' existing
`exp()` is hoisted into `reachF` and the film takes `filmF = sqrt(sqrt(reachF))` (four e-folds,
about 2.2 km): **zero new constants**, which is the only kind of fix that shader can accept.

Files: `shaders/D3D9Client.fx`, `shaders/PBR.fx`, `shaders/Vessel.fx`, `shaders/Metalness.fx`,
`shaders/Mesh.fx`, `shaders/NewPlanet.hlsl` (all DEPLOYED), `Mesh.h/.cpp`, `Scene.cpp`,
`Surfmgr2.cpp`, `VBase.h/.cpp`, `D3D9Effect.h/.cpp`, `gcCore.h/.cpp` + `gcCoreAPI.h` (guards #20
and #21). Client `[Build 260911]`, backup `D3D9Client.dll.pre-a567-260910`.

---

## Patch (aj): PLANETARY RINGS (2026-09-12)

His effect, discussed 2026-09-11 before a line was written (the standing rule). The plan,
the measurements and every design call are `beta/reports/260911/RINGS_PLAN.md`; this is the
patch record. Round 1 of three: **the sheet becomes physical.** Rounds 2 (the plane crossing
and the boulder swarm, ORO-side, no client patch) and 3 (the wave field) are not built.

### What it adds

`gcCore::SetRingLook(OBJHANDLE, const float* prm, int count)` and
`gcCore::SetRingProfile(OBJHANDLE, const void* bits, int w)`, guard **#28**
(`CanSetRingLook`, which checks BOTH pointers - one without the other is no ring at all),
keyed per planet like the exhaust suppression map. `prm` lanes: `[0]` blend (0 = stock
ARITHMETICALLY), `[1]` optical-depth trim, `[2]` lit-face brightness, `[3]` backlit glow,
`[4..7]` RESERVED so a later knob is an addon-side change rather than another client
rebuild (`gWetReflPrm` ran out of lanes once).

With a look and a profile pushed, the client:

- draws the ring through a new `RingTechORO` technique on its own finer carrier mesh
  (64/128/256 sections; the stock 8/12/16 mesh is untouched, so stock stays stock down to
  its 16-gon inner edge, which is short by 1,430 km at each chord midpoint);
- opacity `1 - exp(-tau/mu_view)`, which IS the honest grazing-angle "thickness" - the main
  rings are 10-30 m thick, so a uniform slab would have been a physics error;
- a reflected LIT face and a transmitted UNLIT face that **invert**: backlit, the thick B
  ring goes dark and the thin Cassini Division bright. Stock had a flat `* 0.35`;
- **the ring's shadow on the planet** (`PlanetTechPS`), which stock does not have at all;
- **every object inside that shadow shaded** by what the ring lets through
  (`vPlanet::OroRingTransmission`, called from `GetObjectAtmoParams` beside the eclipse
  term);
- **the sun glare dimmed through the rings** - the fourth ORO multiplier on that one
  `glare` value, after storm light (s), terrain (z) and fog (aa).

### Files

`shaders/Mesh.fx`, `shaders/Planet.fx`, `shaders/D3D9Client.fx` (all **DEPLOYED**),
`RingMgr.h/.cpp`, `Mesh.h/.cpp`, `VPlanet.h/.cpp`, `VPlanetAtmo.cpp`, `Scene.cpp`,
`D3D9Effect.h/.cpp`, `D3D9Util.h`, `gcCore.h/.cpp`, `gcCoreAPI.h`.
Client `[Build 260912]`; backups `D3D9Client.dll.pre-aj-260912` and
`D3D9Client.fx / Mesh.fx / Planet.fx .pre-aj-260912`.

### !! `Planet.fx` IS NOW A DEPLOYED SHADER - the twelfth

It is an `#include` of `D3D9Client.fx` and was byte-stock (modulo CRLF) until this patch, so
it had never needed deploying. Its stock copy is now in `upstream/stock/`. It must join the
staging list, both installer loops and the acceptance rows at the next release build - the
`Mesh.fx` (260823) and `Common.hlsl` (260903) misses are the precedent, and this is the third
time a file has joined the deployed set by being *included* rather than named.

### !! The constraint that did not apply, and measuring it decided the round

The terrain shader sits at c223/224 constants and 16/16 samplers. The rings are nowhere near
it: `RingTech2/P0` is **5 of 224** and `PlanetTech/P0` **7 of 224**. Saturn is not even a
terrain body - `Saturn.cfg` has no `TileFormat` key, so `tmgr_version = 1`, the legacy
`SurfaceManager`, and its globe is drawn by `PlanetTechPS` in `Planet.fx`, **not** by
`GiantPS`. After the patch: `RingTechORO/P0` 6 constants / 1 sampler / 101 instructions,
`PlanetTech/P0` 12 / 3 / 61. `tools/fxccheck.sh` 25/25, every maximum unchanged.
Both new shaders sample-then-gate - no branch around `tex2D` (the (aa) X3528 landmine).

### !! Every ringed planet already ships an optical depth

His requirement - "we cannot rely on a custom way to do this for just these two planets...
there are many fictional systems with ringed planets" - forced the finding that makes the
whole thing generic. The two ring texture formats are **opposite**:

| | `<name>_ring_<size>.dds` | `<name>_ring.tex` |
|---|---|---|
| present on | Saturn only, in stock | **every** ringed planet - the documented format |
| layout | 8192 x 1 A8R8G8B8, 14 mips | 3 concatenated DDS surfaces, 64/128/256, **DXT3** |
| brightness | 8 km/texel, excellent | 256 radial samples, coarse |
| **alpha** | **dead, all 255** | **live** - author-intended OPACITY, fed to `SrcAlpha` |

So the addon derives one profile per planet at runtime from whatever is there and uploads
it; the client never learns about file formats or filenames. Decoding the legacy alpha and
scanning radially at `u = 0.5` reproduces Saturn's B ring, Cassini Division and C ring at
their true radii, and Uranus's nine narrow rings to within 1-2%.

### !! Linear in radius - stock disagrees with itself

`RingTech2PS` samples the profile at `smoothstep(irad, orad, r)` - the cubic - but the
shipped texture is authored **linear** in radius. Measured on `Saturn_ring_8192.dds`: the B
ring's outer edge (117,507 km, the sharpest feature in the rings) sits at the linear
prediction, texel 5368, where the local peak gradient is **18.0** against a whole-profile
mean of 2.4 - and not at the smoothstep prediction, texel 5943, where it is **1.6**. Four
other features agree. Stock therefore displaces ring structure by up to **6,300 km**, more
than the Cassini Division is wide (4,543 km). ORO samples linearly, which corrects it.

### !! The uniforms live in a bracket and are CLEARED after it

`gRingPrm / gRingRad / gRingShd / gRingProf` are set once per ringed planet in
`vPlanet::Render` and cleared (`gRingPrm.x = 0`) after the near ring half - patch (p)'s
scoped-uniform lesson. One push serves the far half, the planet's own shadow pass and the
near half; without the clear every other legacy-tile planet's pass would test against
Saturn's rings. Probe and mirror passes come through the same bracket, so a reflection
agrees with the sky. The profile needs its **own** sampler (`gRingProf` / `RingProfS`):
`Planet0S` and the stock `RingS` both bind `gTex0`.

### !! A STOCK BUG IT UNCOVERED: the ring was lit by an uninitialised sun

Found by flying the new build with the ORO pill **off** and comparing against Orbiter 2016.
`RenderRings` and `RenderRings2` both push the ring mesh's own `D3D9Mesh::sunLight` into
`gSun` - but **nothing ever calls `SetSunLight` on a ring mesh** (every other mesh in the
client gets one: `Scene.cpp:5077`, `VBase`, `VVessel`, `VPlanet.cpp:1098`), and neither
`D3D9Mesh::Null()` nor any constructor initialised the member. With `gSun.Dir` at zero both
ring shading terms collapse:

- `da = dot(normalize(pp), gSun.Dir)` = 0, so `r = |pp|`, which for any ring fragment is at
  least `irad` and far past `gRadius[1]`; `smoothstep` returns 1, `sh` is 1 - **no planet
  shadow on the rings, ever**;
- the face test `dot(nrmW,CamW) * dot(nrmW,gSun.Dir) > 0` is `x * 0`, never true - **the
  rings stay fully bright seen from the dark side.**

Fixed by taking the ring's sun from `gc->GetScene()->GetSun()`, the same source
`SurfMgr.cpp:91/145` uses for the planet's own surface, so ring and globe agree by
construction; plus `Null()` zeroing `sunLight` so no mesh can push indeterminate bytes to
the GPU again. Reproducible with no addon loaded - the (q)/(z)/(af) family, and one for the
jarmonik/orbitersim list. **Indeterminate is not the same as zero**: on another machine the
same code could give a shadow in the wrong *place* rather than none, which is probably why
it survived without a clean report.

### Round 2 (2026-09-12): THE CLOSE-UP - the sheet's own texture, and the near-field draw

Eleven flights in one day, all his; `beta/reports/260911/RINGS_PLAN.md` 12.1-12.13 is the
flight-by-flight record. What the client gained:

**The look grows to 16 lanes** (`OroRingLook::prm[16]`, `SetRingLook` count <= 16, same
signature, no new guard): [4] contrast, [5]/[6] the grain anchor's along-track / radial
INTEGER parts, [7]/[10] their FRACTIONS, [8]/[9] cells per metre, [11] detail (the fade's
reach), [12] relief, [13..15] reserved. The addon pushes them EVERY frame while a ringed
planet is in range. Three float4s carry them - `gRingPrm2`, `gRingPrm3`, `gRingPrm4`
(`eRingPrm2/3/4`) - plus `gRingAxR`/`gRingAxT` (the camera's radial and along-track
directions in the ring plane, computed in the bracket because only the client knows the
render camera) and `gRingCut` (camera forward + the near-field seam depth, signed by which
draw is running).

**`RingTechOROPS` (Mesh.fx)**: grooves (1D radial value noise, ten octaves 2048 m -> 4 m)
and grain (2D, eight octaves) built from the EYE-TO-FRAGMENT vector (`-CamW * gDistScale`)
dotted with the ring-plane axes plus the addon's offsets; every octave admitted by
`RingFade` once it subtends ~4 px at Detail 1 (`fk = 290 * detail`); Hoskins hashes wrapped
every RING_NP = 4096 base cells, the cell count round the ring a multiple of that so the
anchor's phase wrap cannot pop. !! The octave coordinate is `fmod(I k, NP) + F k` with the
offset split into integer and fraction - (I + F) k is a quarter of a cell at k = 512 in
float32. Value noise returns its derivatives (Quilez's form); their weighted sum tilts the
sun-facing normal and the lit face takes the tilted Lambert term over the flat one
(`Relief`: 1.0 at zero slope, floor 0.15, cap 3). The unlit face gained the two-stream
slab's diffuse transmission (`0.5 (1/(1 + 0.75 tau/muS) - exp(-tau/muS))`) and planetshine
(`0.05 (R/r)^2`), both divided by the opacity as the single-scatter term already was. And
the world's shadows from patch (ae)'s atlas (`OroCascadeShadow`, near-field draw only).

**The near-field draw** - the part that took the day. `vPlanet::Render` draws every planet
DISTANCE-SCALED (`maxdist` is the radius + 10 km, so a camera 115,000 km out at Saturn's
rings is "far": `dist_scale` = 4.5e6 / cdist ~ 0.039) on a near plane of 1 km
(`bClearZBuffer`, external view) or `znear_for_vessels` (cockpit views, where the farthest
vessel in view sets it) - either cuts the sheet on a straight line at near / dist_scale of
real depth, ~26 km. So after the hulls `vPlanet::RenderRingsNearField` draws the sheet
again on the current frustum: `RingManager::RenderNearField` on a UNIT half-disc
(`CreateRing(0, 1, 64)`, a fan) placed by a matrix whose scale is the reach and whose
translation is the camera's foot point on the plane (`-h * normal`) - small coordinates,
millimetre positions; depth test ON, write OFF (`D3D9Mesh::bRingNearField` overrides the
technique's states after `BeginPass`); a user clip plane on 1.25 x the seam (the planar
mirror's clip-space recipe); the bracket re-armed (`PushRingBracket(-seam)`,
`UpdateEffectCamera(hObj)` for THIS planet's `gCameraPos`/`gRadius`, handed back to the
proxy after). The two draws OVERLAP over [seam, 1.2 seam] and the shader crossfades them:
the near draw takes `a (1 - s)`, the planet pass - drawn first - the exact complement
`a s / (1 - a + a s)`, so they total `a` everywhere in the band and the planet pass's hard
clip edge sits at alpha 0. `Scene::m_ringNearCut` publishes the near plane the planet pass
used; `vPlanet::Render` pushes `+seam` into `gRingCut`, the near draw `-seam`; the planet
pass leaves the shader early inside the seam. !! THE SEAM IS 600 KM (`RING_NEAR_SEAM_M`,
capped at 0.45 of the distance to the planet's surface through the pass's scale): the
giant mesh's vertices are 1e8 m, so a fragment's camera-relative position carries ~8 m of
per-frame float rounding, and the far mesh must never carry a feature that error can move -
at 600 km the finest admitted octave is a kilometre. The stock path (no look pushed) keeps
the pass's own cut with a hair of overlap; it has no crossfade.

Three facts bought by flights. `ZEnable = false` disables depth WRITES too in D3D9, so the
ring techniques' `ZWriteEnable = true` never wrote anything and the disc only ever tests
against the planet body and the hulls. The probe and custom-camera passes draw whole rings
on one 0.1 m..20,000 km frustum and need none of this (`m_ringNearCut` is 0 there). And the
grain's anchor phase is INTEGRATED on the addon side at the camera's own orbital rate -
`om * t` with `om` at the camera's radius and `t` a J2000 epoch is 63 pattern cells per
METRE of camera radius, which is what streamed on flight 3 (recorded here because the
coordinate convention the shader expects is the client's).

Files: `shaders/Mesh.fx`, `shaders/D3D9Client.fx` (deployed), `Scene.h/.cpp`,
`VPlanet.h/.cpp`, `RingMgr.h/.cpp`, `Mesh.h/.cpp`, `D3D9Effect.h/.cpp`, `D3D9Util.h`,
`gcCore.h` (doc). Backups `D3D9Client.dll.pre-aj2-260912`, `.pre-aj3-260912`.


## Patch (ak): `PostProcess` CLAMPED TO 0..1 - the dead lens-flare mode retired (2026-09-12)

ONE LINE, in `D3D9Config.cpp`'s config read. It is in the (j)/(q)/(z) family: a stock
defect, reproducible with no addon, found while building ORO's own lens flare.

**SolarLiner's lens flare is still in the 2024 client and is wired to nothing.**
`shaders/LensFlare.hlsl` is complete and is DEPLOYED to every user's
`Modules\D3D9Client\` by the CMake shader copy; `Scene::GetSunScreenVisualState()` and
`GetSunDiffColor()` are written and commented `// Lens flare code (SolarLiner)`;
`#define PP_LENSFLARE 0x2` is in `D3D9Client.h`; and `doc/D3D9Client.html` documents the
feature to users: *"The lens flare post processing will create the (infamous) lens flare
effect for the sun."* But `GetSunScreenVisualState()` has **zero callers**, and the only
`ImageProcessing` ever constructed is `LightBlur.hlsl` under `PP_DEFAULT` - there is no
`PP_LENSFLARE` branch anywhere in `Scene::Initialise`.

⚠️ **AND THE CONFIG VALUE CORRUPTED ITSELF.** The read clamped to 0..2, but
`VideoTab.cpp` fills the Launchpad combo with exactly TWO rows ("None", "Light glow"), so
a hand-set `PostProcess = 2` could not survive a visit to that page: `CB_SETCURSEL 2` on a
two-row combo fails, `CB_GETCURSEL` then returns `CB_ERR`, and the next OK wrote back
**-1**. Which is truthy, so `if (Config->PostProcess)` took the post-process branch with
`pLightBlur` NULL - allocating `GBUF_COLOR`, creating no offscreen target, and leaving the
user with **no bloom AND no flare**. This is the ShadowMapFilter 3/4 trap (see the VC
SHADOWS notes) in a second place: *a combo cannot select an index it does not hold.*

The fix is `max(0, min(1, i))`. Anyone carrying a 2 lands on Light glow, which is the
mode they actually wanted, and mode 2 becomes unreachable - which is also what ORO's
author asked for: *"I don't want people to be able to use it when setting PostProcess = 2
and see the old effect."* The 2016 code and shader stay in place, unmodified, both as the
cleanest licence position and as the record of the finding.

**ORO draws its own lens flare instead** - `PSLensFlare` in `Modules\ORO\orofx.hlsl`, one
IPI pass in the external branch, four selectable optics, tunable from the panel's GOD RAYS
page. No client capability was needed for it: the sun disc it keys off is the one
`Scene::RenderGlares` already draws into the backbuffer before the HUD stages (the same
accident of frame order that makes the god rays cheap - see patch (i)), and the occlusion
backstop is patch (h)'s `SetIPISceneDepth` on a buffer that already exists.

Files: `D3D9Config.cpp`. No shader, no header, no gcCore surface, no new guard.


## Patch (al): A REPEATING ERROR IS COLLAPSED (2026-09-13)

A stock defect of the (j)/(q)/(z)/(ak) family - reproducible with no addon loaded, and it
reaches everyone.

`LogErr` writes a line to the client's own HTML log **and to `Orbiter.log`** whenever
`uEnableLog > 0`, and `DebugLvl` **defaults to 1** in `D3D9Config.cpp`. So an error that
recurs every frame writes a line every frame into the log a user is later asked to send
in. One stale addon `SetFloat` produced **16,252 identical lines in a twenty-minute
flight**, out of a 17,134-line `Orbiter.log` - the other 882 lines being everything that
actually mattered.

There is a bound, and it is useless: `LogErr` reaches `my_ctime()`, which does `iLine++`,
so `if (iLine > LOG_MAX_LINES) return` fires - at **100,000 lines**, six times that flood.

### What it does

A message identical to the previous one is COUNTED rather than written. The count is
flushed at most once per REAL second, carrying the tally:

```
D3D9ERROR: IPInterface::SetFloat() Invalid variable name [fTime]. File[...], Entrypoint[PSGloom]
D3D9ERROR: IPInterface::SetFloat() Invalid variable name [fTime]. ...   [repeated 79 times in the last second]
```

A DIFFERENT message closes the previous one's tally first (`[repeated N more times]`), so
no count is ever dropped. **Nothing is lost** - the rate is new information, and it is
legible where 16,252 identical lines are wallpaper.

The decision has to happen BEFORE anything is written, so the function was restructured:
the message is formatted first, then compared, and one static helper (`oroWriteErrLine`)
does the actual writing to both logs. That helper escapes `ErrBuf` IN PLACE, so the raw
text is copied into `oroLastErr` before it is called.

### Scope, and why it stops there

**Errors only.** `LogWrn` is gated on `uEnableLog > 1`, above the default, so warnings
never reach an ordinary user's `Orbiter.log` at all. The informational writers are left
alone deliberately: collapsing those could hide a sequence that matters.

### What it does NOT do

It caps the damage; it does not prevent the bug. The flood that prompted it was ORO's own
stale push, live for a week and shipped in `ORO-beta-260906.zip`, and the effect was
silently missing a value it believed it was setting. `tools/ipicheck.py` is what catches
that: it reads the entry point each IPI handle is created with, brace-matches the
function, strips comments, and checks every pushed uniform is referenced in the CODE.
Both are wanted; the checker is the more important of the two.

### The build stamp

Only `Log.cpp` changed here, and `[Build ######]` is baked from `__DATE__` into
`D3D9Client.cpp.obj` - so the patched client would have shipped reporting the SAME build
number as the client without it. `D3D9Client.cpp` was force-touched and rebuilt; the
client now reads `[Build 260913]`, and the tester README's verify line follows it.

Files: `Log.cpp`. No shader, no header, no gcCore surface, no new guard.
