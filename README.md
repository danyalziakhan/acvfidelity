# ACV Fidelity

ReShade add-on that takes three parts of Assassin's Creed Valhalla's graphics past the game's highest
settings: level of detail, ambient occlusion and screen-space reflections. It works inside the game's own
renderer and changes the game's own shaders. The game's art direction is kept.

## What it does

- **Level of detail.** Objects such as buildings and rocks keep their detailed models farther away. Foliage
  and the clutter near you get their own distances too. Grass, flowers and small plants appear much farther
  out instead of ending in flat terrain. A shorter fade makes the switch between detail levels less visible.
- **Ambient occlusion.** The game's ambient occlusion with more directions and steps. Contact shade under
  objects and in corners is fuller and steadier. Thin things such as grass and poles cast much less false shade.
  The haze they leave over open ground and snow is reduced. The shade also reaches sunlit spots such as plant
  bases and gaps between stones. More light bounces into shade. Sky light is a little stronger by day and a
  little weaker at night. Far trees and hills keep their shade up to 1 km.
- **Screen-space reflections.** The game's reflections on water and other flat shiny surfaces. More steps
  along each ray reflect thin things such as reeds and posts sharper. Calmer water reflects objects more
  clearly. A smooth upscale removes the steps along reflected edges. Reflections no longer lag behind their
  objects when the camera turns. Reflections of things on screen no longer fade out early when the camera looks
  down at water. The game's other reflections are not changed.

Each one has presets: Game, Fidelity I, Fidelity II and Fidelity III. Game is the game's own look. Fidelity II is the
default.

## Requirements

- ReShade 6.8 or newer with add-on support.
- The current version of the game from Steam, Ubisoft Connect or Ubisoft+.

## Installing

1. Install ReShade 6.8 or newer with add-on support.
2. Copy `acvfidelity.addon64` next to `ACValhalla.exe` in the game folder.
3. Start the game and open the ReShade overlay to find the **ACV Fidelity** window.

## Settings

Open the ReShade overlay to find the **ACV Fidelity** window. Every change applies right away. No restart is
needed.

![The ACV Fidelity window with the default Fidelity II presets](nexus/1-window.png)

**Level of detail**

| Preset | Detail distance | Objects | Foliage | Clutter | Model fade |
|---|---|---|---|---|---|
| Game | 1.0x | 1.0x | 1.0x | 1.0x | 0.50 s |
| Fidelity I | 1.5x | 1.67x | 1.67x | 2.0x | 0.40 s |
| Fidelity II | 2.0x | 2.33x | 2.33x | 3.0x | 0.30 s |
| Fidelity III | 2.5x | 3.0x | 3.0x | 4.0x | 0.20 s |

- **Detail distance** is how far away objects keep their detailed models. Up to 2.5x.
- **Objects distance** adds to it for rocks, props, bushes and some trees. Up to 3.0x.
- **Foliage distance** adds to it for the leaves and branches of trees and large plants. Up to 3.0x.
- **Clutter distance** is how far out grass and small plants appear and how detailed they stay. Up to 4.0x.
  Grass follows it up to 3.0x or about 250 m.
- **Model fade** is how long a model takes to blend into its next level of detail. A shorter fade makes the
  switch less noticeable.
- During loading screens, fast travel and respawns the game's own detail is used. Detail rises back to the
  settings a few seconds after loading ends.

**Ambient occlusion**

| Preset | Bounce light | Sky light day / night | Sunlight occlusion | Directions x steps | Max radius | Max distance | Falloff start | Thin objects |
|---|---|---|---|---|---|---|---|---|
| Game | 1.00 | 1.00 / 1.00 | 0 | 2 x 5 | 128 px | 256 m | 0.50 | 0 |
| Fidelity I | 1.25 | 1.07 / 0.90 | 0.17 | 3 x 10 | 213 px | 512 m | 0.67 | 0.0020 |
| Fidelity II | 1.50 | 1.13 / 0.80 | 0.33 | 4 x 20 | 299 px | 768 m | 0.83 | 0.0040 |
| Fidelity III | 1.75 | 1.20 / 0.70 | 0.50 | 5 x 30 | 384 px | 1024 m | 1.00 | 0.0060 |

Baked shade blend is 0.5 in Fidelity I and 1.0 in II and III. Every Fidelity preset also uses Sample spread 1.4,
Thin compensation 2, Min offset 1.3 px and Min visibility 0.03. Strength and Radius stay at the game's values.

- **Strength** sets how dark the shade gets. It shows most in shade and indoors.
- **Radius** is how far from each pixel occlusion is looked for. Larger fills deep recesses but can add dark
  halos.
- **Bounce light** is the strength of the light that bounces off nearby surfaces into shade.
- **Sky light (day)** and **Sky light (night)** set the sky's light in shade. The add-on reads how bright the
  sky is and blends between the two at dusk and dawn.
- **Sunlight occlusion** lets AO dim sunlight where it is strong. It shows at plant bases and in gaps between
  stones. The game keeps AO out of sunlight.
- **Baked shade blend** combines AO with the material's own baked shade instead of keeping only the darker one.
  Cracks and gaps in stone and wood show more shade.
- **Directions** and **Steps** set how many samples each pixel takes. More gives fuller and steadier shade.
  GPU time grows in proportion.
- **Sample spread** places more steps close to each pixel where contact shade forms. Lower keeps grass and
  twigs from leaving dark rings on the ground.
- **Max radius** limits how wide the search gets on screen up close.
- **Max distance** is how far away AO still shades. Up to 1024 m.
- **Falloff start** is the share of the radius where occluders begin to count less. Higher keeps more shade
  from nearby walls and objects.
- **Thin objects** lets shade fade behind thin things such as grass and poles. Wider objects such as pots and
  rocks keep their full shade.
- **Thin compensation** makes occluders far behind a surface count less. It reduces the haze grass and leaves
  put on open ground and snow.
- **Min offset** keeps the first step off the pixel itself so surfaces don't shade themselves.
- **Min visibility** limits how dark shade can get. It prevents pitch black spots in tight gaps.

**Screen-space reflections**

| Preset | Smoothing | Edge reach | Reflection angle | Steps | Hit refinement | Thickness | Far thickness | Jitter | Smooth upscale | Softening |
|---|---|---|---|---|---|---|---|---|---|---|
| Game | 0.70 | 0.70 | 35° | 128 | 0 | 2.0 | 0.018 | 1.50 | 0 | 0 |
| Fidelity I | 0.73 | 0.78 | 43° | 512 | 2 | 1.6 | 0.013 | 1.25 | 0.33 | 0.17 |
| Fidelity II | 0.77 | 0.87 | 52° | 1024 | 3 | 1.3 | 0.009 | 1.00 | 0.67 | 0.33 |
| Fidelity III | 0.80 | 0.95 | 60° | 4096 | 4 | 1.0 | 0.005 | 0.50 | 1.00 | 0.50 |

Every Fidelity preset also uses Noise spread 128 and Motion fix 0.5.

- **Smoothing** calms the water before it reflects. Higher reflects objects more clearly but shows fewer
  ripples and less glitter.
- **Edge reach** is how close to the top of the screen reflections reach before fading. Higher shows more of
  tall things in the water, such as trees and masts.
- **Reflection angle** is how steeply the camera can look down before reflections fade out. The game fades them
  between 25 and 35 degrees.
- **Steps** are the samples along each ray. More gives crisper reflections of thin things. GPU time grows in
  proportion.
- **Hit refinement** checks the depth a few more times to find the exact point a ray hits.
- **Thickness** is how deep behind a surface a ray still counts as a hit. Higher fills holes in reflections.
  More steps need less of it.
- **Far thickness** is the same for rays that travel far. Lower finds reflections on far water.
- **Jitter** turns banding into fine noise that the game's anti-aliasing smooths. More steps need less of it.
- **Noise spread** spreads that noise more finely over the pixels.
- **Smooth upscale** reads the half-resolution reflection with a smooth filter instead of the game's plain one.
  It removes the 2-pixel steps along reflected edges.
- **Softening** blends each reflection pixel with its neighbors. It hides what is left of the blocky look at
  some sharpness.
- **Motion fix** stops reflections from lagging behind their objects when the camera turns.

All settings are saved in `acvfidelity.ini` next to `ACValhalla.exe`. The file is created on the first start.

## Performance

Measured at 1920x1080 on an RTX 3070 Ti. At 4K the cost is about four times higher. Upscaling such as DLSS
renders at a lower resolution and costs less.

- **Level of detail** costs some frame rate. It depends on how much of the world is in view.
- **Ambient occlusion** took 0.13 ms at Game and about 1.3 ms at Fidelity III. Fidelity I and II
  took about 0.33 ms and 0.75 ms.
- **Screen-space reflections** at Fidelity III took 0.63 ms to 2.14 ms depending on the view.
  Lower presets cost far less.

## Good to know

- It works well with [ACV DLSS](https://www.nexusmods.com/assassinscreedvalhalla/mods/428).
- ACV Fidelity writes `acvfidelity.log` next to `ACValhalla.exe`. It starts fresh each time the game starts.
  Attach it together with `acvfidelity.ini` when you report a problem.
- For a detailed log, set `Debug=1` under `[Diagnostics]` in `acvfidelity.ini`.
- ReShade's **Generic Depth** add-on can crash the game on a fast travel or a loading screen. ACV Fidelity
  doesn't need it. Turn it off in the **Add-ons** tab unless an effect you use needs ReShade's depth. If it
  stays on, ACV Fidelity sets `DepthCopyBeforeClears=2` under `[DEPTH]` in `ReShade.ini` to stop the crash.

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

Ambient occlusion's Sample spread, Min offset, Min visibility and Thin compensation follow ideas from
Intel's [XeGTAO](https://github.com/GameTechDev/XeGTAO) and BaBa's
[XeGTAO for ReShade](https://github.com/BarbatosAWLS/Reshade-Shaders/blob/main/Shaders/BaBa_XeGTAO.fx).

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
reflection and lighting passes and the values that control them. I traced
where the game splits its level of detail into categories. I measured
the cost of every setting. And I tested every change in game many times over
before keeping it.

## License

MIT
