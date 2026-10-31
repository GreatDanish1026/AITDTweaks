# AITDTweaks 1.0.0

Graphics add-on for **Alone in the Dark** (2024, PC, Steam), for ReShade with add-on support.
Raises the game's lighting, shadow and detail settings beyond what its menu offers, live, from an
in-game overlay.

- **Lighting**: screen-space bounce light (Unreal's SSGI, shipped switched off), more ambient
  occlusion levels or GTAO, the highest reflection quality and reflections on rougher surfaces,
  finer volumetric fog, finer lighting on smoke, dust and glass.
- **Shadows**: 4096 / 8192 shadow maps (the game uses 2048), longer shadow distance, sharper
  shadows from bulbs and candles, softer cascade blending, contact-hardening shadows,
  full-resolution character shadows.
- **Detail**: texture memory beyond the game's 3 GB, textures kept fully loaded, further view
  distance, the most detailed models, plants and characters.
- **Image**: frame rate limit above the menu's 144 (240, 360 or unlimited), DLAA, DLSS and
  tonemapper sharpening, film grain and vignette, lens flares, bloom.
- **Quality presets**: *Ultra+* (screen-space bounce light at half resolution plus everything
  that costs little; about 35% slower) and *Cinematic* (Ultra+ plus the finest fog, 8192 sun
  shadows, rougher reflections and twice the view distance; about 55-60% slower). Measured on an
  RTX 5080 at 4K with DLSS Quality in one scene. **Measure preset costs** and **Measure each option**
  check them on your PC (uncap the frame rate first).
- **F8** switches every tweak off and on, to compare.

Every option has a **Game** choice that shows the game's own value, and its tooltip says what the
choice costs. The menu's highest setting already maxes shadow filtering, cascades, AO quality,
skin, light shafts and 16x filtering, so those aren't offered.

## Not included

- **HDR**: use [RenoDX](https://github.com/clshortfuse/renodx), which replaces the game's tone
  mapping and sets any peak brightness. Unreal 4.27 itself stops at 1000 or 2000 nits.
- **Ray tracing, distance field shadows and AO**: the game ships without them compiled in, so
  no setting can switch them on.
- **Newer DLSS models**: the game's DLSS preset setting stops at preset F; swap in a newer
  `nvngx_dlss.dll` and pick the preset in the NVIDIA app.

## Install

1. Install ReShade 6.8 or later with full add-on support for `AloneInTheDark-Win64-Shipping.exe`,
   in `AloneInTheDark\AloneInTheDark\Binaries\Win64`.
2. Copy `AITDTweaks.addon64` into that folder.
3. Start the game, open ReShade (Home key), Add-ons tab, AITDTweaks.

Settings are saved in `ReShade.ini` under `[AITDTweaks]`. Log: `AITDTweaks.log` next to the game's
exe (and `AITDTweaks-probe.txt`, every setting's value). Tested with the Steam version (exe
timestamp 0x66549727). Uninstall: delete `AITDTweaks.addon64`.

## Build

`.\build.ps1` (Visual Studio 2022 C++ tools); `-Install` copies the add-on into the game folder
(game closed), `-Package` makes the release zip.

## How it works

Everything goes through the game's console variables, set on the game thread (from the console
manager's once-per-frame sinks call), at console priority; letting go puts the game's value and
priority back. `docs/FINDINGS.md` has the UE 4.27 layout and the game's values.
