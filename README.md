# ACV Fidelity

ReShade add-on that takes Assassin's Creed Valhalla's graphics past the game's highest settings. It covers level
of detail, ambient occlusion, reflections and indirect lighting. It works inside the game's own renderer and
changes the game's own shaders. The game's art direction is kept.

## What it does

- **Level of detail.** Objects such as buildings and rocks keep their detailed models farther away. Foliage
  and the clutter near you get their own distances too. Grass, flowers and small plants appear much farther
  out instead of ending in flat terrain. A shorter fade makes the switch between detail levels less visible.
- **Ambient occlusion.** The game's ambient occlusion with more directions and steps. Contact shade under
  objects and in corners is fuller and steadier. The Fidelity presets use a visibility bitmask. Light passes
  behind grass and thin plants, so they leave fewer dark halos. Shade indoors is slightly lighter. The haze thin
  things leave over open ground and snow is reduced. The shade also reaches sunlit spots such as plant
  bases and gaps between stones. More light bounces into shade. Sky light is a little stronger by day and a
  little weaker at night. Far trees and hills keep their shade up to 1 km.
- **Screen-space reflections (Water).** The game's reflections on water. More steps
  along each ray reflect thin things such as reeds and posts sharper. Calmer water reflects objects more
  clearly. The Fidelity presets trace reflections at three quarters or full resolution instead of half. A smooth
  upscale removes the steps along reflected edges. Reflections no longer trail behind while the camera moves.
  Reflections of things on screen no longer fade out early when the camera looks down at water.
- **Screen-space reflections (Surfaces).** Glossy surfaces such as wet stone, metal and polished wood reflect
  what is around them on screen. Where that isn't on screen the game's own reflection stays. Surfaces that have
  no reflection in the game are left alone.
- **Indirect lighting.** Light from nearby surfaces bounces into shade and takes on their color. It uses the same
  view of the surroundings as the shade itself. So the two match.

Each one has presets: Game, Fidelity I, Fidelity II and Fidelity III. Game is the game's own look. Fidelity II is the
default.

The **Enabled** box at the top of the window switches everything on and off. Off gives the game's own graphics.
Your settings stay as they are. **F11** does the same while playing, at most once a second. You can pick
another key next to it. "Fidelity: On" or "Fidelity: Off" shows for a few seconds after the key.

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

The **Preset** below the toggle key sets every section at once. It shows Custom when the sections differ. The
sections start folded. Click a section's name to open it. Settings that don't apply right now are greyed out.

![The ACV Fidelity window with the sections folded](nexus/1-window.png)

**Level of detail**

| Preset | Detail distance | Clutter | Objects | Foliage | Streaming distance | Model fade |
|---|---|---|---|---|---|---|
| Game | 1.0x | 1.0x | 1.0x | 1.0x | 1.0x | 0.50 s |
| Fidelity I | 1.5x | 2.0x | 1.67x | 1.67x | 2.0x | 0.40 s |
| Fidelity II | 2.0x | 3.0x | 2.33x | 2.33x | 2.0x | 0.30 s |
| Fidelity III | 2.5x | 4.0x | 3.0x | 3.0x | 2.0x | 0.20 s |

- **Detail distance** is how far away objects keep their detailed models. Up to 2.5x.
- **Clutter distance** is how far out grass and small plants appear and how detailed they stay. Up to 4.0x.
  Grass follows it.
- **Objects distance** adds to the detail distance for rocks, props, bushes and some trees. Up to 3.0x.
- **Foliage distance** adds to it for the leaves and branches of trees and large plants. Up to 3.0x.
- **Streaming distance** is how far out the game loads detailed models and far tree lines. The tree line on the
  horizon gets fuller. It uses more video memory. Up to 2.0x. It has no effect while clutter is at 1.0x.
- **Model fade** is how long a model takes to blend into its next level of detail. A shorter fade makes the
  switch less noticeable.
- During loading screens, fast travel and respawns the game's own detail is used. Detail rises back to the
  settings in about 3 seconds after loading ends.
- Riding, loading screens, the map and menus never lower the detail. When the raven flies up fast, detail fades
  to the Fidelity I values over about 6 seconds. It stays there while the camera is in the air and goes back up
  after landing. Higher values in fast raven climbs broke geometry and crashed the GPU in testing.

**Ambient occlusion**

| Preset | Method | Bitmask thickness | Bounce light | Sky light day / night | Sunlight occlusion | Directions x steps | Max radius | Max distance | Falloff start |
|---|---|---|---|---|---|---|---|---|---|
| Game | Horizon-based | - | 1.00 | 1.00 / 1.00 | 0 | 2 x 5 | 128 px | 256 m | 0.50 |
| Fidelity I | Visibility bitmask | 0.45 m | 1.15 | 1.07 / 0.90 | 0.27 | 3 x 10 | 213 px | 512 m | 0.67 |
| Fidelity II | Visibility bitmask | 0.40 m | 1.30 | 1.13 / 0.80 | 0.32 | 4 x 20 | 299 px | 768 m | 0.83 |
| Fidelity III | Visibility bitmask | 0.35 m | 1.45 | 1.20 / 0.70 | 0.38 | 5 x 30 | 384 px | 1024 m | 1.00 |

Every Fidelity preset uses Baked shade blend 0.75, Blur width 0.5 and Blur normal edge 8. They also share Sample
spread 1.4, Far occluder fade 1.5, Min offset 1.3 px and Min visibility 0.03. Strength and Radius stay at the
game's values.

- **Strength** sets how dark the shade gets. It shows most in shade and indoors.
- **Radius** is how far from each pixel occlusion is looked for. Larger fills deep recesses but can add dark
  halos.
- **Method** picks how shade is found. Horizon-based is the game's own AO. Visibility bitmask lets light pass
  behind thin things such as grass and poles. They cast fewer halos.
- **Bitmask thickness** is how thick objects are taken to be with Visibility bitmask. Thinner lets light reach
  behind grass and thin plants. That gives fewer halos and slightly lighter shade. Thicker darkens more around
  everything. It is greyed out with Horizon-based.
- **Bounce light** raises the game's ambient light in shade.
- **Sky light (day)** and **Sky light (night)** set the sky's light in shade. The add-on reads how bright the
  sky is and blends between the two at dusk and dawn.
- **Sunlight occlusion** lets AO dim sunlight where it is strong. It shows at plant bases and in gaps between
  stones. The game keeps AO out of sunlight.
- **Baked shade blend** combines AO with the material's own baked shade instead of keeping only the darker one.
  Cracks and gaps in stone and wood show more shade.
- **Blur width** is how much the AO is smoothed. Below the game's 1.0 the blur is lighter and keeps finer shade
  in fur, rope and stone. 0 turns it off and shows a grain up close.
- **Blur normal edge** stops the blur where the surface turns. Higher keeps small creases sharp.
- **Directions** and **Steps** set how many samples each pixel takes. More gives fuller and steadier shade.
  GPU time grows in proportion.
- **Sample spread** places more steps close to each pixel where contact shade forms. Lower keeps grass and
  twigs from leaving dark rings on the ground.
- **Max radius** limits how wide the search gets on screen up close.
- **Max distance** is how far away AO still shades. Up to 1024 m.
- **Falloff start** is the share of the radius where occluders begin to count less. Higher keeps more shade
  from nearby walls and objects.
- **Far occluder fade** makes occluders far behind a surface count less. It reduces the haze grass and leaves
  put on open ground and snow.
- **Min offset** keeps the first step off the pixel itself so surfaces don't shade themselves.
- **Min visibility** limits how dark shade can get. It prevents pitch black spots in tight gaps.

**Screen-space reflections (Water)**

| Preset | Smoothing | Edge reach | Reflection angle | Steps | Hit refinement | Thickness | Far thickness | Jitter | Resolution | Smooth upscale | Softening |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Game | 0.70 | 0.70 | 35° | 128 | 0 | 2.0 | 0.018 | 1.50 | Half | 0 | 0 |
| Fidelity I | 0.75 | 0.78 | 43° | 512 | 2 | 1.6 | 0.013 | 1.25 | Three quarters | 0.33 | 0.17 |
| Fidelity II | 0.80 | 0.87 | 52° | 1024 | 3 | 1.3 | 0.009 | 1.00 | Three quarters | 0.67 | 0.33 |
| Fidelity III | 0.85 | 0.95 | 60° | 4096 | 4 | 1.0 | 0.005 | 0.50 | Full | - | - |

Every Fidelity preset also uses Noise spread 128 and Sharp in motion.

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
- **Resolution** is the size the reflections are traced at. Half is the game's. Three quarters and Full give
  sharper reflections and cost more GPU time. Full uses the game's own full-resolution path.
- **Sharp in motion** keeps reflections from trailing during camera motion. While the camera moves each frame
  shows only its own reflection.
- **Smooth upscale** reads the reflection with a smooth filter instead of the game's plain one. It removes the
  steps along reflected edges. It is not used at full resolution.
- **Softening** blends each reflection pixel with its neighbors. It hides what is left of the blocky look at
  some sharpness. It is not used at full resolution.

**Screen-space reflections (Surfaces)**

| Preset | Strength | Max roughness | Ray steps | Resolution |
|---|---|---|---|---|
| Game | 0 (the game's reflection) | - | - | - |
| Fidelity I | 0.60 | 0.55 | 48 | Half |
| Fidelity II | 0.80 | 0.70 | 64 | Three quarters |
| Fidelity III | 1.00 | 0.70 | 96 | Full |

- **Strength** is how much of the screen-space reflection replaces the game's reflection. 0 keeps the game's.
- **Max roughness** is the roughest surface that gets it. Rougher surfaces keep the game's reflection.
- **Ray steps** set how far and how finely each ray searches. More costs more GPU time.
- **Resolution** is the size the reflections are traced at. Three quarters and Full are sharper and cost more.

**Indirect lighting**

| Preset | Strength | Radius | Directions x steps |
|---|---|---|---|
| Game | 0 (off) | - | - |
| Fidelity I | 2.80 | 2 m | 2 x 12 |
| Fidelity II | 2.30 | 3 m | 2 x 20 |
| Fidelity III | 2.00 | 4 m | 2 x 32 |

- **Strength** is how strongly nearby surfaces light each other. It goes from 0.1 to 10. The Game preset turns
  it off. While on it also takes over from the game's screen-space AO in shade. Both then use the same view of
  what blocks the light.
- **Radius** is how far a surface may be to light a spot. More costs more GPU time.
- **Directions** and **Steps** set how many samples each pixel takes. More gives steadier light and costs more
  GPU time.
- It uses the Bitmask thickness from the Ambient occlusion section. So both treat objects as equally thick.
- It works only with the Visibility bitmask AO method. With Horizon-based it is off and its settings are kept.
- The presets reach farther as they go up. Their strength is set so all three give about the same amount of
  light.

All settings are saved in `acvfidelity.ini` next to `ACValhalla.exe`. The file is created on the first start.

## Performance

Measured at 1920x1080 on an RTX 3070 Ti. At 4K the cost is about four times higher. Upscaling such as DLSS
renders at a lower resolution and costs less.

- **Level of detail** costs some frame rate. It depends on how much of the world is in view.
- **Ambient occlusion** took 0.13 ms at Game.
- **Screen-space reflections (Water)** took 0.19 ms at Game and 0.80 ms at Fidelity I. Fidelity II took 1.4 ms.
  Fidelity III traces at full resolution and took 7.7 ms. The cost grows with the water on screen. A view with
  more water took 11.4 to 12.0 ms at Fidelity III. These were measured at 1920x1200.
- **Screen-space reflections (Surfaces)** took 0.49 ms at Fidelity I and 0.94 ms at Fidelity III. Fidelity II
  traces at three quarters and Fidelity III at full resolution. Measured at 1920x1200.
- **Indirect lighting** took 1.7 ms at Fidelity I, 2.7 ms at Fidelity II and 4.1 ms at Fidelity III. Measured
  at 1920x1200.

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

Ambient occlusion's Sample spread, Min offset, Min visibility and Far occluder fade follow ideas from
Intel's [XeGTAO](https://github.com/GameTechDev/XeGTAO) and BaBa's
[XeGTAO for ReShade](https://github.com/BarbatosAWLS/Reshade-Shaders/blob/main/Shaders/BaBa_XeGTAO.fx).

The visibility bitmask in Ambient occlusion and the Indirect lighting follow the paper
[Screen Space Indirect Lighting with Visibility Bitmask](https://arxiv.org/abs/2301.11376).

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
