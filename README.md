# ACV Fidelity

ReShade add-on that takes three parts of Assassin's Creed Valhalla's graphics past the game's highest
settings: level of detail, ambient occlusion and screen-space reflections. It works inside the game's own
renderer and changes the game's own shaders. The game's art direction is kept.

## What it does

- **Level of detail.** Objects such as buildings and rocks keep their detailed models farther away.
- **Ambient occlusion.** The game's ambient occlusion with more search directions and steps and a wider
  radius. Corners, eaves and bushes get fuller shading.
- **Screen-space reflections.** The game's reflections on water and other flat shiny surfaces. More steps
  along each ray reflect thin things such as reeds and posts sharper. Water flickers less. The game's other
  reflections are not changed.

Each one has presets: Game, Fidelity I, Fidelity II and Fidelity III. Game is the game's own look. Fidelity II is the
default.

## Requirements

- ReShade 6.8 or newer with add-on support.
- The current version of the game from Steam, Ubisoft Connect or Ubisoft+. The add-on recognizes the game's
  shaders by their exact contents. A section shows "Not active" if a game update changes them.

## Installing

1. Install ReShade 6.8 or newer with add-on support.
2. Copy `acvfidelity.addon64` next to `ACValhalla.exe`. That's where ReShade's DLL is.
3. Start the game and open the ReShade overlay to find the **ACV Fidelity** window.

## Settings

Open the ReShade overlay to find the **ACV Fidelity** window. Every change applies right away. No restart is
needed.

![The ACV Fidelity window with the default Fidelity II presets](nexus/1-window.png)

**Level of detail**

- **Preset:** Game 1.0x, Fidelity I 1.5x, Fidelity II 2.0x, Fidelity III 2.5x.
- **Detail distance:** from 1.0x up to 2.5x. 1.0x is the game's own. It costs some frame rate. Testing showed no
  visible difference beyond 2.5x. Higher values only brought geometry glitches, stretched textures and crashes.

**Ambient occlusion**

| Preset | Directions x steps | Radius | Max radius | GPU time |
|---|---|---|---|---|
| Game | 2 x 5 | 2 m | 128 px | 0.14 ms |
| Fidelity I | 4 x 10 | 2.25 m | 192 px | 0.53 ms |
| Fidelity II | 6 x 20 | 2.5 m | 384 px | 1.39 ms |
| Fidelity III | 8 x 30 | 2.75 m | 512 px | 2.49 ms |

GPU time was measured at 1920x1080 on an RTX 3070 Ti. At 4K it is about four times higher. Upscaling such as
DLSS renders at a lower resolution and costs less.

- **Directions** and **Steps** decide how many samples each pixel takes. More finds occlusion from more sides
  and catches thin occluders such as grass. GPU time grows in proportion.
- **Strength** is 100% in every preset. Higher is darker. The game's lighting applies ambient occlusion only to
  ambient light and lifts it in direct sun. So it shows most in shade and indoors.
- **Radius** is how far around each pixel occluders are searched in meters. The presets raise it only a little.
  Deep recesses and the space under roofs get darker while open areas stay free of halos. The samples still sit
  far closer together than the game's. That gives finer contact shading.
- **Max radius** limits how wide the search gets on screen up close. It's in pixels at half resolution.

**Screen-space reflections**

| Preset | Smoothing | Thickness | Steps | Jitter |
|---|---|---|---|---|
| Game | 0.70 | 2.0 | 128 | 1.50 |
| Fidelity I | 0.72 | 1.6 | 512 | 1.25 |
| Fidelity II | 0.75 | 1.3 | 1024 | 1.00 |
| Fidelity III | 0.78 | 1.0 | 4096 | 0.50 |

- **Smoothing** pulls the water's surface toward flat before reflecting. Higher reduces flicker on water but
  shows less ripple detail.
- **Thickness** is how deep behind a surface a ray still counts as a hit. Higher fills holes in reflections
  when the steps are far apart. With more steps a lower value looks more natural. So the presets lower it as
  the steps rise.
- **Steps** are the samples along each ray. More gives crisper reflections of thin things. GPU time grows in
  proportion.
- **Jitter** shifts where each pixel's steps land. It turns the banding of widely spaced steps into fine noise
  that the game's anti-aliasing smooths. Closer steps leave less banding to hide. So the presets lower it as the
  steps rise.

Changing a value shows the preset as Custom. The **Game** preset sets a section back to the game's own values.

All settings are saved in `acvfidelity.ini` next to `ACValhalla.exe`. The file is created on the first start.

## Good to know

- It works well with [ACV DLSS](https://www.nexusmods.com/assassinscreedvalhalla/mods/428).
- Dense grass can look darker at the higher ambient occlusion presets. Lower the Strength or pick a lower preset
  to soften it.
- ACV Fidelity writes `acvfidelity.log` next to `ACValhalla.exe`. It starts fresh each time the game starts.
  Attach it together with `acvfidelity.ini` when you report a problem.
- For a detailed log, set `Debug=1` under `[Diagnostics]` in `acvfidelity.ini`.

## Building

Visual Studio 2022 and CMake. Clone the dependencies into `deps` first from the repository root:

```
git clone --depth 1 --branch v6.8.0 https://github.com/crosire/reshade.git deps\reshade
```

ReShade passes its own ImGui functions to add-ons. So the ImGui commit has to match the one ReShade pins. For
v6.8.0 it's `3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c`:

```
git clone https://github.com/ocornut/imgui.git deps\imgui
```
```
git -C deps\imgui checkout 3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c
```

Then:

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```
```
cmake --build build --config Release
```

Output: `build\Release\acvfidelity.addon64`. The default `acvfidelity.ini` is built into the add-on from the
repository's copy.

## Acknowledgements

The location and values of the game's LOD factor come from the
[Valhalla LOD Fix](https://www.nexusmods.com/assassinscreedvalhalla/mods/368). I set the 2.5x maximum from my
own stability testing at different values.

## Third-party licenses

- [ReShade](https://github.com/crosire/reshade), BSD 3-Clause, copyright Patrick Mours. The add-on is built
  against its add-on API headers.
- [Dear ImGui](https://github.com/ocornut/imgui), MIT, copyright Omar Cornut.

## Development note

AI assistance was used during development. It helped review code, find bugs,
refine the implementation and write documentation. All changes were reviewed
and tested before being included.

This is not a mere "vibe-coded" mod. I spent countless hours and days on it.
I captured and disassembled the game's shaders to find its ambient occlusion,
reflection and lighting passes and the values that control them. I measured
the cost of every setting. And I tested every change in game many times over
before keeping it.

## License

MIT
