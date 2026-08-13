# Rebuilding D3D9Client for ORO (SEVENTEEN local patches: a-g, i-q)

ORO runs on a locally-patched D3D9Client carrying **seventeen** ORO patches:

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

> **THE EASY PATH (since 2026-08-13): skip step 3 entirely.** All seventeen patches are
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
   seventeen exist as `.patch` files** — the rest are documented as code listings in the
   per-patch sections below, because all seventeen were developed as uncommitted
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

## Version marker

⚠️ **THE STAMP RE-BAKES FROM `D3D9Client.cpp`, NOT `D3D9Util.cpp` — this file said Util
until 2026-08-12 and it was WRONG.** `D3D9Util.cpp`'s `BuildDate()` *looks* like the source
(it parses `__DATE__` by name) but it feeds `SystemSpecs`, not the loader line, and in
Release it is dead code anyway (see patch (q)'s addendum). The `[Build ######]` line comes
from the SDK's module-version glue, which bakes `__DATE__` into `D3D9Client.cpp.obj`.
**Force-touch `D3D9Client.cpp` after any client patch, then VERIFY** by scanning the built
DLL for a `"Mmm DD YYYY"` string and confirming it is today's — the check takes seconds and
is what caught this. With (q) the stamp is **`260812`**.

The patched client logs `[Build YYMMDD, API YYMMDD]` (compile date) in Orbiter.log vs the
stock `[Build 241231, API 241231]`. Patch (a) alone was `260725`; +patch (b) was `260726`;
the client with a+b+c+d is **`260801`**; with (f) it is **`260804`**; with (g) it is
**`260807`**; with (i)+(j) it is **`260808`**; **(k) is ALSO `260808`** - the stamp cannot
distinguish it from (i)+(j) (same-day builds; the stamp re-bakes only when D3D9Util.cpp
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
