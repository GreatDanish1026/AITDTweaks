# AITDTweaks

A ReShade add-on for **Alone in the Dark** (2024, PC) that sets the game's graphics from the
ReShade overlay, live: no Engine.ini, no launcher.

The overlay has one tab per area. Every setting has a **Game** state (the game's own
behaviour): a choice's first option, or a slider whose box is unticked. Hovering a setting
explains it and shows the game's value. Changes apply at once.

- **Image**: DLAA, DLSS sharpening, image sharpening.
- **Detail**: texture memory sized to your video card, 16x texture filtering, view distance,
  mesh and foliage detail distance, character detail, keeping textures fully loaded.
- **Shadows**: filtering, resolution up to 8192, shadow distance, sun cascades, small object
  shadows, cascade blending, soft (PCSS) shadows, lamp and flashlight shadow detail, character
  capsule shadows.
- **Lighting**: ambient occlusion (full quality, more levels, or the engine's **GTAO**),
  **screen space global illumination** (shipped switched off), reflections, volumetric fog,
  skin scattering, smoke and glass lighting, light shafts.
- **Advanced**: every console variable the settings control, the game's value and the current
  one, and a reset for everything.

Settings are kept in `ReShade.ini` under `[AITDTweaks]` and applied every time the game starts.

## Install

1. Install [ReShade](https://reshade.me) 6.8 or newer, **"with full add-on support"**, for
   `AloneInTheDark\Binaries\Win64\AloneInTheDark-Win64-Shipping.exe` (DirectX 10/11/12).
2. Copy `AITDTweaks.addon64` into that same folder.
3. Start the game, press **Home**, and open the **AITDTweaks** tab.

Uninstall: delete `AITDTweaks.addon64`. Values go back to the game's at the next start.

## Notes

- If the game build differs from the one analysed, the add-on checks the console variable
  layout first and changes nothing if it doesn't match (the overlay says so).
- It writes `AITDTweaks.log` next to the game executable.
- The DLSS mode, HDR, film grain, motion blur and depth of field are in the game's own menu.

## Building

Visual Studio 2022+ with the C++ x64 tools, then `.\build.ps1` (`-Install` copies it to the
game; the game must be closed). See [docs/FINDINGS.md](docs/FINDINGS.md) for how it works.

MIT License. ReShade and Dear ImGui headers: see THIRD_PARTY_NOTICES.txt.
