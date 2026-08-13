# ORO shipped configuration

These are the files ORO ships. They are the **source copies**; the live ones live under
the Orbiter root and are what the addon actually reads:

| repo | deploys to |
|------|-----------|
| `config/bodies/*.cfg` | `<Orbiter>\Config\ORO\bodies\*.cfg` |

Deployment is a plain copy — there is no post-build step, so copy by hand after changing
one here (the same arrangement as `orofx.hlsl`).

## `bodies/` — one file per world, the AURORA scope

ORO has three settings scopes (invariant 17). These are the third:

- **GLOBAL** — `Config\ORO.cfg`, what the PILOT is.
- **PER VESSEL CLASS** — `Config\ORO\<class>.cfg`, what a HULL needs.
- **PER BODY** — `Config\ORO\bodies\<name>.cfg`, what a WORLD's aurora is. ← these

**ORO owns this tree deliberately.** An earlier design wrote an aurora block into each
body's own Orbiter `.cfg`; the user reversed that on 2026-08-07 because editing stock
config files risks breaking things nobody asked us to touch and bloats files users read
for other reasons. Owning the tree also leaves room for weather/cloud data later.

### What is in a body file

Each file carries the RANGES the dialog sliders span (so one 0..1 knob means 40-160 km at
Earth and 200-700 km at Jupiter), the magnetic-pole offset, three colours by altitude, and
where the sliders currently sit. Every value is commented with WHY it is what it is, so the
files double as documentation.

### ACTIVITY IS THE OPT-IN

There is no enable flag. `AuroraActivity = 0` — including the built-in default a world with
no file falls back to — means that world has no aurora. Turn the slider up at any world and
it gets one; press the ATMOS tab's SAVE and the file is written for you. One control that
cannot contradict itself (an explicit `AuroraEnable` was built on 2026-08-07 and removed the
same day: it gated the very flow it was meant to guard).

### Worlds shipped

Earth, Jupiter, Saturn, Uranus, Neptune, Mars, Venus, Io, Titan, Triton, Ganymede, Europa.

Two of those need a note. **Ganymede and Europa have no atmosphere in Orbiter's configs**,
and the atmosphere flag used to be what decided whether a world could glow. That was the
wrong test: Ganymede is the only moon in the solar system with its own magnetic field, so it
has genuine polar ovals, and Europa's oxygen glow is part of the evidence for its subsurface
ocean. A shipped config file is now itself the statement that a world glows, so it overrides
the flag (`OroSettings_BodyHasFile`).

### Editing by hand

Safe. Loading is forgiving per field — a missing or unreadable key falls back to ORO's
built-in default for that field alone rather than rejecting the file — and every value is
then clamped into a sane band, so no edit can produce a degenerate curtain. Colours are
COLORREF decimals (`blue<<16 | green<<8 | red`).
