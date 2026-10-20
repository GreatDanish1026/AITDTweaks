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

## Earlier pak mods folded into the overlay

The pak mods built before the add-on (never released) included two that only carried config:

- `pakchunk901-ShadowDetailMax` (a `DefaultScalability.ini`): shadows, view distance, detail,
  volumetric fog and reflections. Now the Detail, Shadows and Lighting defaults.
- `pakchunk904-SSGIQuality` (a `DefaultEngine.ini`): `r.SSGI.Enable 1`, `Quality 4`, `HalfRes 0`.
  Now the Lighting tab's global illumination.

Both paks were moved out of the game to `AloneInTheDark\_AITDTweaks_retired_paks`. The asset
mods stay paks: atmosphere maps (900), surface materials (902), post-process reflection quality
70 to 100 (903: `r.SSR.Quality` can only cap the post-process value, not raise it), and the
Emily and Edward textures and materials (905, 906).

## First run (0.1.0)

Layout check passed; every default applied on the first frame, none was changed back during a
short session at the menu. Visual results of each setting still to be checked in game.
