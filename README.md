# Auto Rate

Geometry Dash (Geode) editor mod. It analyses your whole level, finds **free color channel IDs**
(it scans every object's save string and never reuses a channel the level already uses),
then adds a themed decoration pass.

## What it adds

- Glow lines along the top and bottom faces of blocks (merged into long strips, not one object per block)
- Crown spikes on platforms and hanging icicles under them (non-solid, behind the player)
- Glow copies behind your existing hazards
- A background city skyline with lit windows
- Background saws / glowing diamonds placed only in empty areas (never over gameplay)
- Color triggers (background, ground, line, 4 free channels) and pulse triggers
- Areas you already decorated heavily get less new decoration

Styles: Hell, Heaven, Gold, Neon, Blue. Quality: Fast, Normal, Ultra.
Work is time-sliced, so the editor stays responsive while it runs.

## Use

Editor -> pause menu -> **Auto Rate** button (bottom right). Options are in the mod settings.
Undo with Ctrl+Z (may take several presses).

## Build

Needs the Geode SDK. Either build locally (`geode build`) or push to GitHub:
the workflow in `.github/workflows/build.yml` builds Windows / macOS / Android and uploads a `.geode` file
under Actions -> latest run -> Artifacts.

Before the first build, edit `mod.json`: `id` and `developer`, and copy the `geode` / `gd` versions from a fresh
`geode new` project if they are out of date.

## Tuning

All style colors are in the `STYLES` table at the top of `src/main.cpp`.
Object IDs used: 1 (block), 8 (spike), 88 (saw), 899 (color trigger), 1006 (pulse trigger).
