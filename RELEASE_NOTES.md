# ORO release notes

Newest release on top. Each section lists what was added or fixed since the previous
public release.

Installing, or upgrading from any earlier ORO beta:
1. Close Orbiter and the Launchpad completely.
2. Unzip the file in your Orbiter folder (the one that contains Orbiter.exe). It creates
   an `ORO_beta` folder there.
3. Open that `ORO_beta` folder and run `ORO_Install.bat`.

It upgrades in place and keeps your settings and your original-files backup.

---

## ORO beta 260906

The patched D3D9Client in this build logs `[Build 260906]`. Everything below is new since
`ORO-beta-260831`.

### NEW

**Cascaded shadows** (Launchpad, D3D9 Advanced setup, "Terrain and world shadows" = Cascaded (ORO))
- One sun-shadow atlas for the whole scene: buildings shadow the ground, hills shadow buildings, vessels shadow each other and their pads, out to the cascade reach (5-60 km)
- Crisp shadows on your own vessel and the three nearest vessels through dedicated hull maps
- No frame-to-frame blink, no shimmer as the world turns, no tile-shaped bites
- Cascade detail 1024 / 2048 / 4096 (48 / 192 / 768 MB of video memory); 4096 needs a DX11-class card and falls back automatically
- Soft far shadows option for the distant cascades
- Vessel self-shadows can be set to None with Cascaded on: ground and building shadows stay, only cockpit shadows and the finest hull detail go
- Spotlight shadows ride in the same atlas, so in daylight a lamp no longer lights the ground through a hangar wall
- The two stock terrain-shadow modes (Stencil, Projected) are unchanged
- Greyed rows in the new Shadows box are the ones that do nothing in the selected mode

**Fog** (WORLD / WEATHER / FOG)
- Two fog layers rendered inside every surface: terrain, bases, hulls, particles, runway lights, the sky
- Fog colour follows the sun: warm at dawn, grey under a storm, dark at night
- Ground shadows, VC shadows and the sun itself weaken through it
- Visibility, top, fade, brightness and sun glow sliders
- The rain's gloom is now a real layer of mist

**The cabin at night** (PILOT / VIRTUAL COCKPIT)
- A virtual cockpit's fill light follows the sun at the camera; in orbit's shadow or at night the cabin goes dark and the cockpit light has a job
- Night floor slider, and a Weather dim slider so storms and fog darken it further
- MFDs, self-lit displays, emission maps and every cockpit lamp are untouched
- Rain streaks and splashes are lit by daylight, the vessel's own lights and the storm's flashes

**Cabin sounds** (PILOT / VIRTUAL COCKPIT)
- Rain in cabin: the storm heard from the seat, with its own volume, independent of the outside mix
- Hull drum rebuilt as rain on a metal roof: dull thumps over a low panel rumble, no hiss

**Base lights** (one setting on both the RAIN and FOG pages)
- Force every base's night state on: night textures, runway and taxiway lights
- Glow gain into the bloom, and a fog halo that grows with the air between you and the lamp

**Spotlight shadows** (patched client)
- Base spotlights cast real shadows: a hangar carves its beam, a vessel in the beam throws a shadow, a ridge ends it
- Vessels receive them day and night; the ground pool is dusk-and-night in the stock shadow modes and daylight too in Cascaded mode
- Off switch: the Shadows box, or LocalLightShadows = 0 in D3D9Client.cfg

**Night textures for base buildings**
- The mytex.dds / mytex_n.dds convention now works for MESH base objects; runway markings ride along

**Sculptable shock diamonds** (VESSEL / THRUSTERS / EXHAUST)
- Diamond count goes down to 0 (a clean jet)
- Diamond shape (-1 half diamond at the lip, 0 classic, +1 downstream fan), length, width and a train offset in metres
- Diamond colour swatch renders exactly what you pick
- Under throttle the train grows and shrinks by whole diamonds, never a sliced cell

**Full shimmer control** (VESSEL / THRUSTERS / EXHAUST)
- Amplitude, wavelength and frequency sliders; frequency 0 freezes the ripple
- Per-jet offset widened to -2..+10 m along each jet's own flow
- Per-group strength: a weak hover haze beside a strong main haze
- Shimmer answers to air density: thinning as you climb, gone in vacuum, nearly doubled on Venus

**Vapour cones** (VESSEL / REENTRY / VAPOUR CONES)
- Base fill offset per cone (-1..+0.5): the base disc becomes an inner cone or a bulge

**Windscreen rain surfaces** (WORLD / WEATHER / RAIN)
- RAINSURFACES button: click a window in the sim to declare it a rain glass, no mesh editing; the picked group holds a green highlight while the button is down
- Config\ORO\VesselsRainSurfaces.cfg ships with the stock DeltaGlider's windscreen pre-declared
- Changes apply live on SAVE

**Tools**
- Script\focusall.lua makes unselectable vessels (boosters, tanks, stack parts) selectable for tuning: run('focusall') from the Lua console

### FIXED
- Windscreen runners ran backward when the engine was cut while still rolling
- Ground shadows at KSC blinked frame to frame at high frame rates and at time warp
- The vessel shader refused to compile with Vessel self-shadows set to None (an error box at every scenario start, since build 260809)
- A spotlight lit a vessel behind a building in daylight
- A slider could stay stuck to the mouse after releasing over a shimmer wave slider
- Bipolar sliders with asymmetric ranges drew their zero tick in the wrong place
- The "Enable terrain flattening" and "Enable sun glare" checkboxes overlapped in the D3D9 Advanced setup

### CLIENT (stock D3D9Client bugs fixed in the patched client)
- Terrain flattening (.flt files) did nothing under cubic elevation interpolation, the core's default; it works in both modes now, and the drawn ground matches the ground you land on
- Runway lights and base surfaces drew through hills; they obey depth now
- The sun's glare shone through mountains; it sets behind the ridge now
- Glare sprites and addon geometry drew through buildings; base structures are in the depth buffer now
- Stencil ground shadows drew through hills; they are cut behind terrain now (D3D9Client.cfg ShadowDepthTol / ShadowDepthTolK, defaults 1.0 / 0.001)

### TECHNICAL
- The patched client logs [Build 260906]: thirty-five patches (a-z, k2, z2, z3, aa-af), eleven deployed shaders, all matched to the DLL
- New D3D9Client.cfg keys: TerrainShadowing 3, ShadowCascadeSize, ShadowCascadeFar, ShadowCascadeSoft, LocalLightShadows, ShadowDepthTol, ShadowDepthTolK, ShadowDebug (diagnostic, default 0)
- New ORO settings: the FOG page, BaseLightsOn / BaseLightsGlow / BaseLightsHalo, the VIRTUAL COCKPIT page's night and cabin-sound keys, the diamond and shimmer keys, VapourBaseOfs / Vapour2BaseOfs - old settings files load with the defaults
- The in-panel HELP and the README cover every new control

### KNOWN ISSUES
- Reflection probes and planar mirrors do not sample the cascade atlas; they keep the per-vessel shadow map
- Where more than two hulls overlap, a hull pixel in Cascaded mode takes the two nearest
- In the Stencil and Projected modes a spotlight at dusk can bite a vessel's terrain shadow; Cascaded mode has no bites
- The rain is hard to see at night away from a lamp, and the lightning flash does not light the cockpit or cast shadows - next release
- Cascade detail 4096 costs 768 MB of video memory; drop to 2048 if the frame rate falls
