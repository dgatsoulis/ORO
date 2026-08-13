# ORO closed beta — packaging manifest

Everything that goes in the zip, where it comes from, and where it lands.

**The archive is no longer laid out in place - it carries an INSTALLER.** The tester
unzips into their Orbiter root, which creates one folder, and runs one file. That is
what makes the client swap reversible: the installer backs up *their* original files
before replacing them, and the uninstaller puts those exact files back.

```
<zip root>
+-- ORO_beta\                     <- unzips into <OrbiterRoot>\ORO_beta\
    +-- ORO_Install.bat           <- the tester runs this
    +-- ORO_Uninstall.bat
    +-- ORO_README.txt
    +-- payload\                    <- mirrors OrbiterRoot; copied in on install
    +-- stock\                      <- pristine 2024 originals (restore fallback)
    +-- backup\                     <- NOT shipped; the installer creates it
```

The installer refuses to run unless it finds an Orbiter 2024 install one level up, and
refuses to run twice. The uninstaller restores the client and its six shaders, then
deletes only files that are **byte-identical to what was shipped** - anything the tester
added or tuned is kept and listed. Both were tested end to end against a mock install.

Sections 1-5 below all go under `ORO_beta\payload\`, at the path in the "Goes to"
column (which is relative to `<OrbiterRoot>`).

---

## 1. The two DLLs

| Source | Goes to | Note |
|---|---|---|
| `Modules\Plugin\ORO.dll` | `Modules\Plugin\ORO.dll` | ours |
| `Modules\Plugin\D3D9Client.dll` | `Modules\Plugin\D3D9Client.dll` | ⚠️ **PATCHED — overwrites stock** |

The patched client logs `[Build 260812]`; stock logs `[Build 241231]`. That one log line
is the fastest way to confirm an install took.

## 2. The six deployed shaders — ⚠️ ALL OVERWRITE STOCK

They are compiled at RUN TIME, so they must always match the DLL. Never ship a patched
DLL with stock shaders or vice versa.

| Source | Goes to |
|---|---|
| `Modules\D3D9Client\D3D9Client.fx` | same path | 
| `Modules\D3D9Client\Vessel.fx` | same path |
| `Modules\D3D9Client\PBR.fx` | same path |
| `Modules\D3D9Client\Metalness.fx` | same path |
| `Modules\D3D9Client\Sketchpad.fx` | same path |
| `Modules\D3D9Client\NewPlanet.hlsl` | same path |

## 3. ORO's own runtime assets

| Source | Goes to |
|---|---|
| `Modules\ORO\banner.bmp` | same path |
| `Modules\ORO\orofx.hlsl` | same path |
| `Modules\ORO\sounds\heartbeat.wav` | same path |
| `Modules\ORO\sounds\Induce_gloc.wav` | same path |

`orofx.hlsl` is **not optional** — it is compiled at session start and every premium
effect (grey-out, blur, aberration, swim, tilt, shimmer, plasma, eclipse) lives in it.

## 4. Meshes and textures (author-shipped, not generated)

| Source | Goes to | What it is |
|---|---|---|
| `Meshes\ORO\DG-S.msh` | same path | heatshield override, DG-S |
| `Meshes\ORO\DeltaGlider.msh` | same path | heatshield override, DeltaGlider - a COPY of DG-S.msh (his call: the same shell suits both) |
| `Meshes\ORO\DG-S_bell.msh` | same path | bell-glow shell, DG-S |
| `Meshes\ORO\DeltaGlider_bell.msh` | same path | bell-glow shell, DeltaGlider |
| `Textures\ORO\bell_glow.dds` | same path | the bell's banding mask |

## 5. Settings — ship these, they are the tuned look

A tester on built-in defaults sees an **untuned** addon, which is not what you want them
judging. These carry your numbers.

| Source | Goes to |
|---|---|
| `Config\ORO.cfg` | same path |
| `Config\ORO\Atlantis.cfg` | same path |
| `Config\ORO\DeltaGlider.cfg` | same path |
| `Config\ORO\DG-S.cfg` | same path |
| `Config\ORO\ProjectAlpha_ISS.cfg` | same path |
| `Config\ORO\bodies\*.cfg` (12 files) | same path |

## 6. Scenarios - four, to point testers at the effects

His own, and every vessel in them is stock (ISS, ProjectAlpha_ISS, Mir, Wheel,
DeltaGlider, DG-S, Atlantis, ShuttleA, ShuttlePB - all verified present in a stock
Config\Vessels), so they load on the clean install testers were told to use.

| Source | Goes to |
|---|---|
| `Scenarios\ORO_beta\Atlantis reentry.scn` | same path |
| `Scenarios\ORO_beta\DG reentry.scn` | same path |
| `Scenarios\ORO_beta\Habana Spaceport.scn` | same path |
| `Scenarios\ORO_beta\Thruster effects.scn` | same path |

The uninstaller removes this folder under the same rule as everything else - shipped
and unchanged goes, anything a tester saved into it stays.

## 7. The restore bundle

The FALLBACK only - used if a tester loses the backup the installer made.

| Source | Goes to (in the zip) |
|---|---|
| `Orbitersdk\samples\ORO\upstream\stock\*.fx`, `*.hlsl` (6) | `ORO_beta\stock\Modules\D3D9Client\` |
| `Modules\Plugin\D3D9Client.dll.orig-241231` | `ORO_beta\stock\Modules\Plugin\D3D9Client.dll` |
| `Orbitersdk\samples\ORO\upstream\stock\RESTORE.txt` | `ORO_beta\stock\RESTORE.txt` (manual fallback) |

## 8. The readme

| Source | Goes to (in the zip) |
|---|---|
| `Orbitersdk\samples\ORO\beta\ORO_README.txt` | `ORO_beta\ORO_README.txt` |
| `Orbitersdk\samples\ORO\beta\ORO_Install.bat` | `ORO_beta\ORO_Install.bat` |
| `Orbitersdk\samples\ORO\beta\ORO_Uninstall.bat` | `ORO_beta\ORO_Uninstall.bat` |

---

## ⚠️ DO NOT SHIP

- **`Config\ORO\XR2Ravenstar.cfg`** — the XR2 is Doug Beachy's addon, not stock (no XR
  vessel exists in the Orbiter source tree). On a clean install it is dead weight, and it
  hints at addons the tester was told not to install.
- **`Modules\ORO\sounds\breathing.wav` / `breathing2.wav`** — verified unreferenced
  anywhere in the source. A standalone breathing loop was rejected long ago; breathing
  lives inside the scenario clips. Dead files in a first impression are noise.
- **`Textures\ORO\bell_glow.tga`** — the authoring source. Only the `.dds` is loaded.
- **`Meshes\ORO\DeltaGlider2.msh`** — a copy of `DG-S.msh` under a DELIBERATELY dead
  name: the override lookup is `Meshes\ORO\<class>.msh`, so renaming it to
  "DeltaGlider2" switches the override off and makes the DeltaGlider build its shock
  shell from the raw mesh. That is how the author A/B'd the two. `DeltaGlider.msh` is
  the live one and is what ships; keep this one locally as the off-switch if you ever
  want that comparison again.
- **`Modules\Plugin\D3D9Client.dll.pre-*` / `.working-*`** — your intermediate build
  backups. Only `.orig-241231` matters, and it goes in `ORO_beta\stock`.
- **`D3D9Client.cfg`** and **`Orbiter_NG.cfg`** — these are the *tester's* settings.
  Overwriting them would stamp your video mode and key bindings on their machine. The
  readme tells them what to set instead.
- Anything under `Orbitersdk\` — source, not runtime.

## Scenario audio — decided

**One clip is the design.** `Induce_gloc.wav` is the only scenario clip and the other five
buttons are silent by choice (decided 2026-08-13, not a shortfall). Do not re-open this as
a packaging gap; the readme states it plainly. The loader still accepts the other names if
one is ever dropped in, so nothing has to change if that decision is ever revisited.

## The built archive

**CURRENT: `beta/dist/ORO-beta-260812.zip`** — 3.75 MB, 55 entries, built 2026-08-12.
The first package under the ORO name, and **the next thing testers receive.**

⚠️ **Two older zips sit beside it and BOTH keep their PULSE names — they are history,
not typos.** Do not "fix" them in a rename sweep:
- `PULSE-beta-260810.zip` — **what testers are actually running**, and it has the
  **reload CTD** in it (exit to Launchpad, start a scenario again, crash). He has told
  them about it.
- `PULSE-beta-260812.zip` — built, acceptance-tested, and **deliberately never shipped**
  (his call: one ORO package rather than an install-then-uninstall dance). Its verified
  contents are what this package was rebuilt from.

So testers go straight from 260810 to ORO, which is why the readme leads with
**"run the old PULSE_Uninstall.bat FIRST"** and why `ORO_Install.bat` REFUSES to run
while `Modules\Plugin\PULSE.dll` is present — the ORO installer cannot see, back up or
remove PULSE's files, since none of them have those names any more.

`beta/dist/` is gitignored (build output, and it carries a 920 KB third-party binary).
Rebuild from the staging tree beside it with
`Compress-Archive -Path beta\dist\ORO_beta -DestinationPath <zip> -Force`.

The ORO staging tree was **built explicitly from the live install** by
`beta/build_staging.ps1` (file list = sections 1–8 below, DO-NOT-SHIP list
enforced), rather than by copying and renaming the PULSE tree — so nothing stale or
old-named can survive by accident. Rebuilding it that way is preferable to patching it.

⚠️ **REFRESH THE STAGING TREE FIRST — the zip is built from it, not from the live install.**
Do not eyeball which files changed; hash every staged payload file against its live source
under `<OrbiterRoot>` and copy the ones that differ. On the 260812 rebuild that found six,
and **two of them would not have been guessed**: `banner.bmp` (same size, different bytes —
he had re-cut the artwork) and `D3D9Client.dll` (same size, different bytes — patch (q) is a
one-line change). A size comparison would have shipped both stale.

## After building the zip — verify

**The mechanical half is automated: `beta/acceptance.ps1`** — 8 cases, 44
assertions, run against the SHIPPED zip extracted into a mock Orbiter 2024 tree.
Not-Orbiter refusal, 2016-dated refusal, **old-PULSE-installed refusal**, cancel,
install, double-install, tuned-file preservation + uninstall, double-uninstall.
Last run 2026-08-12 on `ORO-beta-260812.zip`: **44 passed, 0 failed.**

⚠️ **Two traps it is built to avoid, both bought with real time:**
- **Drive the .bat by FULL PATH.** A bare name after `cd /d` silently fails to resolve,
  the installer never runs, and every "the tree is untouched" assertion then passes
  TRIVIALLY — indistinguishable from a real pass. Every case therefore asserts on a
  SIGNATURE STRING proving the installer actually ran.
- **Read the output, not just the exit state.** On the first ORO run one case failed on
  the assertion `output must not contain "** "` — which the SUCCESS screen legitimately
  trips with `***  PLEASE READ THE README`. The product was fine; the test was wrong.
  A failing assertion is a claim about the test as much as about the code.

Then the part only a human can do:

1. Unzip into a scratch copy of a **clean** Orbiter 2024, run `ORO_Install.bat`,
   then start a scenario.
2. `Orbiter.log` should show `Module D3D9Client.dll ... [Build 260812, API 260725]` and,
   ⚠️ **it is the BUILD that discriminates, not the API** — the patched client is compiled
   from the clone, so its API number tracks the clone's SDK (260725) and does NOT match
   stock's 241231. Both docs said 241231 until 2026-08-12, which would have had a tester
   reading a correct install as a failed one. `ORO.dll` itself reads `API 241231`, since it
   builds against the installed `Orbitersdk`. And,
   a few lines down, ORO reporting each capability it found — patches (d), (f), (g),
   (i), (k), (l), (n), (o) all "available".
3. `ORO: vessel class DG-S - loaded NN of NN settings` proves the config tree arrived.
4. `ORO bell: ORO\DG-S_bell -> bell: MAIN` proves the meshes arrived.
5. Ctrl+F4 → "ORO control" opens the panel; the banner is artwork, not the procedural
   fallback (that would mean `banner.bmp` is missing).
6. Run `ORO_Uninstall.bat`, then confirm Orbiter still starts on the stock client
   (`[Build 241231]` in the log) with no ORO files left behind.
