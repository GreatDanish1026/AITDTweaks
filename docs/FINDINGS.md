# Findings

Notes from reverse-engineering Alone in the Dark (2024) for AITDTweaks.

## The game

- Unreal Engine 4.27, DirectX 11 (`DefaultGraphicsRHI_DX11`),
  `AloneInTheDark\Binaries\Win64\AloneInTheDark-Win64-Shipping.exe` (Steam app 1310410, PE
  timestamp `0x66549727`). No PDB, no DRM wrapper: the exe on disk is the code that runs.
- Paks: `pakchunk0/1-WindowsNoEditor` (.pak with IoStore .utoc/.ucas), unencrypted. Config files
  are in `pakchunk0-WindowsNoEditor.pak` under `AloneInTheDark/Config/`.
- User config: `%LOCALAPPDATA%\AloneInTheDark\Saved\Config\WindowsNoEditor\`. The game rewrites
  `Engine.ini` there, which dropped the earlier mod installers' merges.
- Shipped with `r.RayTracing=False` and `r.GenerateMeshDistanceFields=False`, so hardware ray
  tracing, distance field AO and distance field shadows can't be switched on at run time.
- Shipped DLSS plugin: `nvngx_dlss.dll` 3.1.1. `r.NGX.DLSS.Preset` only accepts presets A-F.
- The game's "Graphics Mode" (Quality/Performance) only applies on consoles
  (`DefaultOFGraphicsModeDeveloperSettings.ini`).

## Console variables

Found with `tools/callsites.py`, `tools/findmgr.py` and `tools/cvvt427.py` (run against the
exe) and llvm-objdump.

### IConsoleManager::Singleton

Game code looks cvars up as `mov rcx, [rip+Singleton]` ... `lea rdx, [rip+L"r.xxx"]` ...
`call [rax+0xb0]`. Over 30 cvar names: 89 of 101 lookups load the singleton at RVA
`0x5ebd950`, 71 call vtable offset `0xb0` (FindConsoleVariable). At run time the add-on's scan
saw 82 of 82 agree. The FConsoleManager constructor (RVA `0x19804f0`) installs the vtable at RVA
`0x4c7fcd0`; the singleton is stored at RVA `0x19805d4`.

### Console object vtables (UE 4.27)

Same layout as UE 4.26/5 (UntilDawnTweaks), except FindConsoleVariable is at `+0xb0`:

| Offset | Slot |
|---|---|
| 0x18 / 0x20 | GetFlags / SetFlags (`[rcx+0x18]`) |
| 0x28 | AsVariable |
| 0x30-0x48 | IsVariableBool / Int / Float / String |
| 0x50-0x68 | AsVariableBool / Int / Float / String (`&this+0x48` in data cvars, null in Ref cvars) |
| 0x70 | AsCommand |
| 0x78 | Release |
| 0x80 | Set(Value, SetBy) |
| 0x88 / 0x90 / 0x98 | GetBool / GetInt / GetFloat |

Vtables the Register functions install (RVAs): bool `0x4c80158`, int `0x4c80210`, float
`0x4c802c8`, bool Ref `0x4c80438`, int Ref `0x4c804f0`, float Ref `0x4c805a8`, string Ref
`0x4c80660`.

### Storage

- Data cvars: `TConsoleVariableData<T>` at `+0x48`: game-thread value, the renderer's copy right
  after it (`+0x4c` for int/float, `+0x49` for bool).
- Ref cvars: pointer to the engine global at `+0x48`, own value at `+0x50` (their GetInt is
  `mov eax, [rcx+0x50]`).

As in the other UE 4 games, Set only copies the value to the renderer on the game thread, so the
add-on copies it itself after each Set (`cvars::MirrorToRenderer`).

### Priority

Flags seen at run time: most render cvars at `0x01` (scalability), the SSGI cvars at `0x03`
(project settings). The add-on sets at `0x09000000` (console), the highest there is.

## Earlier pak mods (removed)

Before the add-on, never-released pak mods (pakchunk900-906: atmosphere maps, a shadow/detail
config, surface materials, post-process reflection quality 70 to 100, SSGI config, Emily and
Edward 4K textures and materials) were tried. The two config-only ones (901 shadow/detail, 904
SSGI) became the Cinematic preset's values. All of them were deleted from the game on 2026-10-31;
the game is stock apart from this add-on.

## First run (0.1.0)

Layout check passed; every default applied on the first frame, none was changed back during a
short session at the menu. Visual results of each setting still to be checked in game.

## 0.2.0: the shared Tweaks design

0.2.0 replaces 0.1.0's render-thread setting with TrueColorsTweaks' design (UE 4.25, same
layout): every Set runs on the game thread from a vtable copy of the console manager whose
CallAllConsoleVariableSinks slot (`+0x50`, RVA `0x196ebe0`; 6 call sites, `+0x58` RegisterSink 16
sites, `tools/sinkcalls.py`) runs the add-on first. IsVariable* slots are found by reading their
code (IsVariableBool at `+0x30`). Options are combos with a Game choice, presets Ultra+ and
Cinematic, Measure preset costs / each option, F8, a probe file and an Advanced tab, as in the
other Tweaks add-ons. With nothing saved every option starts at Game (0.2.0's first build picked
Cinematic on the first start instead; dropped with the old paks).

First run 2026-10-31: 3 of 3 singleton matches, layout check passed, all 20 Cinematic values
taken, none put back during the menu.

### The game's values (title screen and menu, 4K, menu at its highest)

| Cvar | Game | Set by |
|---|---|---|
| r.Shadow.MaxResolution / MaxCSMResolution | 2048 | 0x01 |
| r.Shadow.DistanceScale, r.ViewDistanceScale | 1 | 0x01 |
| r.Shadow.TexelsPerPixelSpotlight | 2.546 | default |
| r.Shadow.TexelsPerPixelPointlight / Rectlight | 1.273 | default |
| r.SSR.Quality | 3 | 0x01 |
| r.SSR.MaxRoughness | -1 (post-process value) | default |
| r.SSGI.Enable / Quality / HalfRes | 0 / 3 / 1 | 0x03 / 0x01 / 0x03 |
| r.AmbientOcclusionLevels / MaxQuality | -1 / 100 | 0x01 |
| r.VolumetricFog.GridPixelSize / GridSizeZ | 8 / 128 | 0x01 |
| r.TranslucencyLightingVolumeDim | 64 | 0x01 |
| r.Streaming.PoolSize | 3000 | 0x01 |
| r.Tonemapper.Quality / Sharpen | 5 / 0 | 0x01 / default |
| r.BloomQuality / LensFlareQuality | 5 / 2 | 0x01 |
| r.NGX.DLSS.Enable / Quality / DLAA.Enable | 1 / 1 / 0 | 0x07 |
| r.HDR.EnableHDROutput / OutputDevice / ColorGamut | 1 / 5 / 2 | 0x02 / 0x05 / 0x05 |

Already at the top in the menu (not offered): r.ShadowQuality 5, r.Shadow.CSM.MaxCascades 10,
r.Shadow.RadiusThreshold 0.01, r.SSS.* full quality, r.LightShaftQuality 1, r.MaxAnisotropy 16,
r.CapsuleShadows 1, fog depth slices 128.

### HDR

The game's HDR is Unreal's ACES output for a 1000-nit display in scRGB (OutputDevice 5, FP16
swap chain, colour space scRGB). UE 4.27 has no peak-luminance cvar (only EnableHDROutput,
OutputDevice, ColorGamut, UI.CompositeMode and UI.Level), so the highest built-in target is
2000 nits (OutputDevice 6). 0.2.0 briefly offered that as an HDR tab; it was removed before
release because HDR is left to RenoDX, which replaces the game's tone mapping and sets any peak.
