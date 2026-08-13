# ORO — Orbiter Realism Overhaul

*Atmospheric, Physiological and Visual Immersion Suite*

An addon for **Orbiter 2024** that adds realistic atmospheric, physiological and visual
effects — reentry plasma, auroras, lightning seen from orbit, god rays, eclipses, exhaust
plumes and shock diamonds, transonic vapour cones, and what high G does to a pilot's vision.

*Ορώ* is Greek for *to see, to perceive* — which suits an addon whose whole subject is what
the pilot perceives.

> ### ⚠️ Read this before installing
>
> ORO **replaces your D3D9Client graphics client** with a patched build. It has to: many of
> these effects are impossible through the stock client's API, and one of them —
> registering a HUD render proc — crashes the stock client outright.
>
> The installer backs up your original files first, the uninstaller restores them, and
> pristine stock copies ship in the package as a second safety net. The full source of the
> patched client is published at
> **[dgatsoulis/orbiter-oro](https://github.com/dgatsoulis/orbiter-oro/tree/oro-patches)**.
>
> Install onto a **clean Orbiter 2024**, and read the included `ORO_README.txt` first.
> This is beta software.

## What it does

Two families of effect, plus some things that are neither.

**Physiology** — what the pilot's body does to what the pilot sees. Blackout, red-out, grey-out,
tunnel vision, dark spots, blink, blur, chromatic aberration, sparkles, peripheral swim, camera
shake, and a heartbeat you can hear. Driven by a real felt-G model — proper acceleration at the
head position, pilot body axes by posture, cardiovascular lag, and a cerebral-oxygen reserve —
or by sliders, if you would rather drive it yourself.

**Environment** — reentry plasma built from the vessel's own mesh, its luminous trail, auroras
at twelve worlds, lightning storms read from the planet's real cloud map, god rays, eclipses
modelled as an eye rather than a dimmer, pressure-driven exhaust plumes with shock diamonds,
incandescent engine bells, and Prandtl–Glauert vapour cones.

**And things ORO controls but does not draw** — shadows in the virtual cockpit, and Orbiter's
own particle streams put under live control.

A full inventory is in **[FEATURES.md](FEATURES.md)**.

Effects are physics-driven wherever the simulator already knows the answer; the sliders are
there to control the *look*, not to tell the sim things it can work out for itself.

## Requirements

- **Orbiter 2024** (the 2024 release specifically — the installer checks)
- **D3D9Client** — ships with Orbiter 2024; ORO replaces it with the patched build
- **XRSound** — ships with Orbiter 2024; optional, ORO is silent without it
- 32-bit. ORO is a global module built Release/Win32, matching D3D9Client's architecture.

## Installing

Download the release package, extract it into your Orbiter root folder, and run
`ORO_Install.bat`. It verifies you are on Orbiter 2024, backs up the files it is about to
replace, copies the payload, and verifies the result.

`ORO_Uninstall.bat` reverses it — and deletes a file only when it is byte-identical to what
was shipped, so anything you tuned or added is kept rather than thrown away.

Then open Orbiter, enable **ORO control** in the Modules tab, and press **Ctrl+F4** in the
simulator for the control panel. `Ctrl+G` is the master kill switch.

**Section 3 of `ORO_README.txt` lists required video settings.** Do not skip it — several
effects fail silently without them.

## Building

Visual Studio 2022, toolset v143, C++17, **Release | Win32 only**.

```
MSBuild ORO.vcxproj /p:Configuration=Release /p:Platform=Win32
```

Output goes to `Modules\Plugin\ORO.dll`. You will also need the patched D3D9Client — see
[`upstream/BUILDING.md`](upstream/BUILDING.md) for the full recipe, or just clone the
[`oro-patches`](https://github.com/dgatsoulis/orbiter-oro/tree/oro-patches) branch where the
patches are already applied.

## Licence

ORO is dual licensed under **GPL v3** and **LGPL v3** — the same licence as D3D9Client, the
client it extends. See [`LICENSE`](LICENSE) and [`COPYING.LESSER`](COPYING.LESSER).

The patched D3D9Client is published separately at
[dgatsoulis/orbiter-oro](https://github.com/dgatsoulis/orbiter-oro); those changes are
modifications of Jarmo Nikkanen's and Martin Schweiger's GPL/LGPL code and carry that licence.

## Credits

- **Orbiter** — Martin Schweiger. MIT licensed.
- **D3D9Client** — Jarmo Nikkanen and Martin Schweiger. ORO would not exist without it, and
  seventeen small patches to it are what make these effects possible.
- **XRSound** — Douglas Beachy. MIT licensed.

Built by **Dimitris "dgatsoulis" Gatsoulis**, with Claude.
