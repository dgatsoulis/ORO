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

## ORO beta 260913

The patched D3D9Client in this build logs `[Build 260913]`. Everything below is new since
`ORO-beta-260906`.

### NEW

**Rain on the windscreen follows the airflow** (WORLD / WEATHER / RAIN, THE WINDSCREEN)
- Runners used to radiate from the middle of the front window, so half of them swept down it. They now radiate from the STAGNATION POINT - the spot on the nose where the oncoming air first meets the airframe, which is below the glass - so parked they still run straight down, and from about rotation speed onward they sweep UP the windscreen and outward, the way they do on a real canopy
- Nothing to set: the point is where the airframe and the airflow put it, so it is right on any vessel
- **Standing drops thin out as you accelerate and the runners multiply to carry the water away** - at speed the glass stops being a field of drops
- **A new Water film slider**: past about 45 m/s of dynamic pressure what is left on the glass is a moving sheet rather than drops, and it ripples what you see through it. 0 turns it off; when it arrives is the dynamic pressure, not a setting, so it is honest at altitude - 200 m/s in thin air barely disturbs a drop
- Fly a takeoff roll in rain to see all three: they change together, because they are all the airflow beating gravity at the same moment

**Lens flare is back** (WORLD / GOD RAYS, second group)
- The sun throws ghosts, an iris starburst and a veil across the frame again - the effect Orbiter 2016 had and Orbiter 2024 lost, asked for on the forum thread
- **External views only, on purpose**: a flare is made inside a lens, and a healthy eye has no lens elements, so there is none in any of the three cockpit views. Outside, the camera is a camera
- **Four lenses to choose from**, and every slider means the same thing in all four: CLASSIC (a warm stills optic, a few large varied ghosts), ANAMORPHIC (the cine look - cool blue-white, a long horizontal streak through the sun, many fine rays, a long chain of small ghosts), CLEAN (a modern multi-coated lens: a crisp star, two faint reflections and almost nothing else) and VINTAGE (uncoated - veiling glare washing the whole frame, lifted blacks, many soft colourless ghosts and a few soft blades)
- Intensity, Size, Ghosts, Rays, Dispersion and Air fade, plus a pill and a Test button; saved globally
- It is crispest in vacuum and thins out as you descend: an atmosphere spreads the sun's light across the sky and kills the contrast a flare lives on. That makes it the god rays' opposite, and the two hand over to each other as you climb
- It goes out behind a hull, a mountain or a building, dims through cloud and haze and fades in an eclipse, with nothing to set for any of it - it reads the real sun out of the frame rather than guessing from a brightness model
- Dispersion sets how far the ghost colours spread from white. Those colours come from the lens coating, which is why VINTAGE barely responds to it - an uncoated lens has no coating colours to spread

**Planetary rings** (WORLD / RINGS)
- Saturn's and Uranus's rings become a real sheet instead of a flat card: they go opaque as you look along them edge-on, and the lit and backlit faces are different things - with the sun behind them the thick B ring goes dark while the thin Cassini Division glows, the way it does in every Cassini photograph
- **The rings cast their shadow on the planet**, which Orbiter has never drawn. The shadow carries the ring's own structure, so the Cassini Division shows as a bright line across the band, and it broadens and narrows with Saturn's seasons on its own
- A ship inside that shadow is shaded by however much light the ring lets through, and the sun itself dims when you look at it through the rings - deeply behind the B ring, barely at all through a gap
- Ring density, Ring brightness and Backlit glow, saved per planet; the pill is off for stock behaviour exactly, so one click is the before-and-after
- **Any ringed planet works, including addon ones, with no extra files.** ORO reads the optical depth out of the ring texture the planet already ships, so a fictional system's rings get the same treatment as Saturn's
- Two scenarios in `6 The sky` fly it
- Addon authors: drop a `<Planet>_ring_oro.dds` in Textures to hand-author a ring profile instead of the derived one
- **The close-up.** Bring the camera toward the ring and the sheet grows its own texture: grooves and grain, each octave arriving as it gets big enough on screen, down to structure a few metres across when you are on the plane. It follows the camera, not the ship, and it stands still for a ship in a circular orbit, as the real material would
- Three rows under THE CLOSE-UP: Detail (how fine), Contrast (how strong) and Relief - the texture read as height and lit by the sun, so every ridge has a lit side and a shade side
- The underside of a dense ring is grey, not black: the light a thick ring diffuses through itself, and planetshine, both under the Backlit glow slider
- Ships and bases cast their shadows onto the ring sheet (Cascaded (ORO) shadow mode)
- `Rings - the crossing` puts you in the cockpit inside the B ring, 1,945 m above the ring plane and falling through it at 32 m/s, a minute from the crossing and over the LIT face; `Rings - the wide view` does the same in the thin Cassini Division, starting wide and under the unlit face, with the ring's shadow lying across Saturn's southern hemisphere

**The control panel sizes itself to your screen**
- On a 4K display the panel is drawn at double size, larger still above that; at 1080p and 1440p it is exactly what it was
- The help window comes with it, and its text reflows as well as growing - widening the window still fits more words per line
- `DialogScale = 150` in `Config\ORO\window.cfg` overrides the automatic size (100 to 300, where 100 is the old panel); the file lists the key in its own header
- The remembered panel height is measured before the scaling, so it means the same amount of panel whatever size you are on

**Sixteen scenarios that explain themselves**
- The old `ORO_beta` scenario folder is replaced by `ORO`, with six folders - The pilot, Reentry, Engines, Weather, Night and lights, The sky - and sixteen scenarios in them
- **Click one and the description panel tells you what it is, what to look for, and exactly which ORO pill and which Launchpad rows it needs.** Click a folder and you get a page about that whole family. Four of the old seven had no description at all
- They cover the things that had none: pulling G until you grey out, the plasma from inside the cockpit and from outside, a transonic run for the vapour cone, a climb from the ground to vacuum, RCS and payload-bay reflections, a storm you stand in, rain on the glass at speed, fog at first light, a base at night with the monorail running, an orbital night pass with auroras and storms, sunrise for the god rays and the lens flare, an annular eclipse low over the Edwards lakebed, and two rides through Saturn's rings
- Upgrading keeps anything of yours: the installer removes only the old scenarios that are byte-identical to ones it shipped, names anything you edited, and leaves the folder alone if something of yours is still in it

**You choose how much ORO writes to Orbiter.log**
- `Debug` in `Config\ORO.cfg`: 0 = only what is necessary, 1 = concise (the default, and what a useful report looks like), 2 = verbose for hunting something
- **Nothing ORO writes is per frame at any level** - a diagnostic writes at most one line per real second. If you ever see a log growing by thousands of lines in one flight, that is a bug in its own right

**The wet look reaches runways, pads and taxiways** (WORLD / WEATHER / RAIN)
- Paved base surfaces darken with the ground beside them, carry the storm's sky sheen, and mirror the ship standing on them, exactly as terrain does; until now they stayed dry-looking while the dirt around them soaked
- They get no standing pools on purpose: a runway is crowned and grooved to shed water, so puddles on one would be a defect rather than weather
- Rain-hit sparkle on base objects is sized in metres now, so a taxiway glitters like a wing instead of showing a few enormous blots

**Reflections on open water, with the rain off** (WORLD / WEATHER / RAIN, "Water mirror")
- Fly over sea and your ship is mirrored in it whether or not a storm is running; the slider scales it from 0 to twice the default, and the readout shows how much water is under you
- Water is read from the planet's own water map, so coastlines and lakes are where they really are
- It reaches from the deck to 1500 m, fading out over the last kilometre, and only while a vessel is near the camera

**Splash size** (WORLD / WEATHER / RAIN)
- Sets how big the rain rings on the ground are, 0 to 2, where 1 is what they have always been; 0 turns them off
- Small addon vessels no longer stand among splashes wider than they are

**The HUD stays readable through the rain** (virtual cockpit)
- Raindrops still sit on the glass and still lens the world behind them, but the HUD is drawn after them, so the numbers and lines stay crisp at any Drops lens setting
- Nothing to set: it happens whenever ORO is armed in a virtual cockpit

**Vapour cones form by chance** (VESSEL / REENTRY / VAPOUR CONES)
- A cone no longer appears on every transonic pass: each entry into the Mach band rolls the dice once, with odds that fall with altitude, rise over the sea and fall inland (read from the planet's own water map around you), and follow the dynamic pressure
- Max chance, Dry ceiling (default 15 km, drawn ±10% on every pass) and Intermittency sliders, global; the cone shapes stay per vessel class
- Intermittency makes the cone appear and die in bursts on top of its flicker; a marginal draw flutters more than a certain one
- The Air readout shows the geography, the live chance and this pass's verdict; Test still shows the cone

**Base trains and solar plants live again** (any base whose cfg carries TRAIN1, TRAIN2 or SOLARPLANT blocks - Habana's monorail and hangrail among the stock bases)
- The monorail cabin shuttles between its ends and the hangrail's two cabins pass each other under their girder rail, on sim time, as they did in Orbiter 2010; both had stood frozen since terrain arrived
- The rails follow the terrain: the beam climbs and descends under an 8% grade, bridges a dip on pylons, and the hangrail's portals stand with their feet on the ground
- Solar plants render at last (they never did under a graphics client): each panel tracks the sun, glints when you look down its normal, and stands on its own ground
- Time warp is capped at x100 for the trains so they never become a blur
- Cabins, rails and panels cast and receive the cascaded shadows, take local lights and fog, and use the mytex_n night textures
  - Base authors: the blocks are the 2010 ones (END1 / END2 / MAXSPEED / SLOWZONE / HEIGHT / TEX for the trains; POS / SCALE / SPACING / GRID / ROT / TEX for a plant); `solpanel` is the stock plant texture

**More local lights** (Launchpad, D3D9 Advanced setup, Local lights)
- 12x and 16x rows join 4x and 8x: up to sixteen lamps light a mesh at once (pads and stations with many floods)
- A mesh evaluates only the lights that reach it, so the higher rows cost nothing where few lamps are present; terrain keeps its four strongest per tile

**Point lights cast shadows too** (Launchpad, D3D9 Advanced setup, Shadows box: Point light shadows - Off / Aimed / Cube, default Aimed)
- A point light has no direction, so Aimed points one shadow map at whatever is standing in the light, and Cube gives it five (down and the four sides), which shadows in every direction at once
- Cube costs five of your shadow maps, so it wants the 6 spot maps row to share with a spotlight
- Spotlights always get their maps first: a floodlit pad never loses a shadow to a passing light

**A lights and shadows test scenario** (Scenarios / ORO / 5 Night and lights / Lights and shadows)
- Load it and a twelve-case tour runs itself: one spotlight, two, four and six, a cluster, more lights than maps, a stadium flood, a point light in a ring of vessels, sixteen lights at once, and the same scene by day
- Each case says on screen what you should see and which setting it needs; T skips ahead, R goes back, Ctrl+P pauses
- Everything it spawns is removed at the end, and it works at whatever settings you have - the title card warns about anything a case wanted and did not get
- If something looks wrong, note the case number and send a screenshot: that is all we need to find it

**Several spotlights cast shadows at once** (Launchpad, D3D9 Advanced setup, Shadows box: Spot light shadows - Off / 1 / 2 / 4 / 6 spot maps, default 4)
- Up to six spotlights cast shadows in the same frame instead of one, each from its own map: a pad with several floods shows every one of them, and a shadow no longer jumps from lamp to lamp as the camera moves
- Lamps mounted together share one map
- A vessel's shadow in a beam reaches the ground beyond it, and a ridge beyond it ends the beam
- In Cascaded mode the maps live in the sun's shadow atlas at no extra memory; the other modes add a small local atlas (16 or 24 MB)

**Reflection probes and mirrors per mesh** (`Config\GC\<class>_ecam_oro.cfg`)
- `MESHGROUPS m first last` assigns a probe or mirror to groups of one mesh - multi-mesh vessels can now target a single part (a request from the SSV team)
- The shipped Atlantis file carries a syntax reference for every line

### FIXED
- **ORO filled Orbiter.log with one error per frame whenever rain was live** - 16,252 identical lines in a twenty-minute flight, out of a 17,134-line log. A shader stopped using a value that ORO went on sending it, which the client correctly reported every single frame. Fixed, and the log is a few hundred lines again. It shipped in 260906, so any log from that build is worth discarding
- **The planet cast no shadow on its own rings, and the rings stayed fully bright seen from the dark side** - both since Orbiter 2024, both visible in 2016. The ring mesh was being lit by a sun direction nothing had ever set; the rings take the same sun the planet's own surface does now. Visible with ORO's ring pill off, so it is a fix to stock behaviour rather than part of the effect
- The ring sheet vanished along a straight line as the camera closed on it, in external and cockpit views alike, leaving black sky where the ring should be: the client draws every planet with a near plane that cut the sheet about 26 km out. The sheet is now drawn again close to the camera and blended into the far draw, so it runs continuously from the horizon to under the ship
- Puddles formed on the sea surface and on cliff faces; they are now limited to ground that could actually hold water, and the pool grain no longer appears over water
- Distant terrain washed out to a pale grey band during rain: the damp sheen on the ground had no distance falloff, so mountains 20 km away took the full effect; it now fades over about 2 km like the puddles do
- Zoomed out, runways and pads read as black silhouettes that never blended into the fog; the wet darkening is applied to the surface itself now, so it fogs with the rest of the world
- Cascaded mode: base buildings, runways, pads and every plain-textured hull (the stock DeltaGlider included) received no world shadows and stayed sunlit inside a hangar's shadow; they take them now, and a hangar shadows its own interior
- **Cascaded mode: the fine hatched moiré inside a vessel's own shadow at a grazing sun is gone** - a hull rolling in orbit showed it best, and hangar walls and terrain under a low sun had it too. It was shadow acne: the receiver's slope correction stopped growing at 76 degrees while the surface's depth per shadow texel kept growing, so past about 84 degrees every surface shadowed itself. The lookup is now offset along the surface normal by 1.5 texels (a margin that grows with the same tangent the acne does) and the slope clamp sits at 86 degrees. Contact shadows stay attached - the lookup moves sideways, never in depth
- Exiting a scenario crashed to the desktop (heap corruption) whenever a SOLARPLANT block existed in a base you had not visited that session - an Orbiter core bug (its SolarPlant frees pointers it never set unless the base was activated); the client now activates those bases at session close, so the exit is clean
- Lighting at a pad with more than 24 lamps reshuffled as the camera moved: the client's scene light list evicted the wrong light when full (a stock bug); it evicts the farthest now
- Spotlight shadows on a vessel at a lit pad: the shadow map is now fitted to the vessels and buildings in the beam instead of the whole cone (many times finer), the light that holds it no longer changes as the camera moves, and the edges are filtered smooth instead of stepped; a mast or building standing around the lamp no longer blows the fit open, and only casters in view are fitted
- Setting `PostProcess = 2` by hand in D3D9Client.cfg left you with no bloom and no lens flare, and then silently rewrote itself to an invalid value the next time you opened the Advanced setup page. The setting is limited to its two real values now, and anyone carrying a 2 lands on Light glow

### CLIENT (stock D3D9Client bugs fixed in the patched client)
- **A repeating error no longer floods the log.** Any error that recurs every frame wrote a line every frame, into the client's own log and into Orbiter.log - and the client's `DebugLvl` defaults to 1, so it reached everyone, not just someone debugging. Identical messages are counted now and flushed once a second with the tally (`[repeated 79 times in the last second]`), so a storm reads as a headline instead of burying the rest of the file. Nothing is lost - the rate is information the repeated lines never carried

### TECHNICAL
- The ring profile is derived at load from whatever ring texture a planet ships - the legacy `.tex` carries a real optical depth in its alpha channel, which is what makes the effect work on any ringed planet without new files
- `Planet.fx` joins the deployed shaders (twelve now) and is restored by the uninstaller like the rest
- Local lights are packed into four constant registers each instead of six, and a mesh evaluates only the lights that actually reach it; 8x Full now costs no more than 4x Full did unless eight lights land on a mesh. This is the room the next lighting work builds in
- `tools/fxeff`: an offline shader check using the client's own compiler, so register budgets are measured rather than estimated
- Hidden key `LocalLightSelfShadow` in D3D9Client.cfg (default 0): the emitter's own vessel casts into its own shadow map - spot or point, and including a light authored inside the hull, which is the usual case. A diagnostic for vessel authors; with it on an interior emitter blacks out its own beam, which is why it ships off
- `tools/ipicheck.py`: every value ORO pushes to a shader is checked against what that shader actually uses, so a stale push cannot survive an edit again
- `tools/logaudit.py`: every log line in the project is checked for a rate guard, and raw logging outside the one entry point is refused
- The scenarios and their launchpad pages are generated from one table (`tools/scenarios.py`), so a scenario and its description cannot drift apart. States were computed by `tools/scnstate.py`, which validates itself against states Orbiter wrote; once a scenario has been flown and saved, `tools/scnfreeze.py` captures that state and the flown version wins from then on
- Shader size is checked against the graphics card's ps_3_0 instruction-slot cap (4096 on NVIDIA): the spotlight shadow tests are limited to a mesh's eight strongest lights so every Local lights row fits, `tools/fxeff` fails a shader above the cap, and the client logs the card's cap at startup (`MaxPS30InstrSlots` in Orbiter.log)
- Hidden keys `ShadowCascadeSlope` (default 16) and `ShadowCascadeOffset` (default 1.5) in D3D9Client.cfg tune the cascade taps' slope clamp and normal offset without a rebuild - they ride spare lanes of constants the shaders already carried, so the terrain shader that sits at the ps_3_0 register ceiling paid nothing. `ShadowDebug 5` blacks every surface whose grazing slope exceeds the clamp, the instrument the moiré fix is judged with

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
