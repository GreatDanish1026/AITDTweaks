// AITDTweaks: a ReShade add-on for Alone in the Dark (2024; UE 4.27, DirectX 11) that sets the
// game's console variables from the ReShade overlay, live, with no Engine.ini to edit. See
// docs/FINDINGS.md.
//
// Saved settings (ReShade.ini, [AITDTweaks], applied at every start), by overlay tab:
//   Image     DLAA, DLSS sharpening, tonemapper sharpening
//   Detail    texture memory by video card, texture filtering, view distance, mesh, foliage
//             and character detail
//   Shadows   shadow quality, resolution, distance, cascades, small-object shadows, soft
//             (PCSS) shadows, lamp shadow sharpness, character capsule shadows
//   Lighting  ambient occlusion (more quality, more levels or GTAO), screen space global
//             illumination, reflections, volumetric fog, skin, smoke and glass lighting,
//             light shafts
//   Advanced  every console variable the settings control, with the game's value
//
// Every setting has a "Game" state (the game's own behaviour). A choice's option 0 is the
// game's; a slider is the game's until its box is ticked.
//
// What it records, next to the game executable:
//   AITDTweaks.log  the console manager scan, the layout check, and every value it forces or
//                   hands back

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "cvars.hpp"
#include "version.h"

extern "C" __declspec(dllexport) const char* NAME = AT_PRODUCT;
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Sets " AT_GAME "'s graphics from the ReShade overlay, live: shadows, ambient occlusion, global "
    "illumination, reflections, fog and detail.";

namespace {

using namespace reshade::api;

const ImVec4 kWarn(1.0f, 0.75f, 0.3f, 1.0f);
constexpr const char* kConfigSection = AT_PRODUCT;

// ---------------------------------------------------------------- logging

std::mutex g_mutex;  // present (render thread) and overlay
FILE* g_log = nullptr;

void Log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_log) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
        fflush(g_log);
    }
}

void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring dir(path, n);
    dir.resize(dir.find_last_of(L"\\/") + 1);
    std::wstring log = dir + L"" AT_PRODUCT ".log";
    std::wstring prev = dir + L"" AT_PRODUCT ".prev.log";
    // ReShade may load and unload the add-on more than once at startup, so only the first
    // load in a process starts a new log.
    if (GetEnvironmentVariableW(L"AITDT_LOG_OPEN", nullptr, 0) == 0) {
        SetEnvironmentVariableW(L"AITDT_LOG_OPEN", L"1");
        MoveFileExW(log.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    g_log = _wfopen(log.c_str(), L"a");
}

// ---------------------------------------------------------------- executable image

constexpr uint32_t kKnownTimestamp = AT_KNOWN_PE_TIMESTAMP;

const uint8_t* g_base = nullptr;
uint32_t g_imageSize = 0;
uint32_t g_timestamp = 0;
cvars::Section g_text, g_rdata;

void LogGameInfo() {
    g_base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    g_imageSize = nt->OptionalHeader.SizeOfImage;
    g_timestamp = nt->FileHeader.TimeDateStamp;
    cvars::FindSection(g_base, ".text", g_text);
    cvars::FindSection(g_base, ".rdata", g_rdata);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    Log("%s %s", AT_PRODUCT, AT_VERSION);
    Log("exe: %ls", path);
    Log("exe: PE timestamp 0x%08x (%s), image size 0x%x", g_timestamp,
        g_timestamp == kKnownTimestamp ? "the build the layouts come from" : "a different build than the layouts",
        g_imageSize);
}

std::string Where(const void* p) {
    auto b = static_cast<const uint8_t*>(p);
    if (!g_base || b < g_base || b >= g_base + g_imageSize) return "heap";
    char buf[48];
    snprintf(buf, sizeof(buf), "exe+0x%llx", static_cast<unsigned long long>(b - g_base));
    return buf;
}

// ---------------------------------------------------------------- console manager

void* g_manager = nullptr;
bool g_unsupported = false;  // the scan or the layout check failed: nothing is ever set
std::string g_status = "waiting for the game";

void Unsupported(const char* why) {
    g_unsupported = true;
    g_status = why;
    Log("cvars: %s - not changing anything", why);
}

bool IsFloat(float a, float b) { return fabsf(a - b) <= 1e-4f * std::max(1.0f, fabsf(b)); }

// Checks the vtable offsets and storage against two cvars every build has, before anything is
// set: r.ScreenPercentage (float) and r.MaxAnisotropy (int), both data cvars.
bool CheckOne(const wchar_t* name, cvars::Type want) {
    void* var = cvars::FindVariable(g_manager, name);
    if (!var) {
        Log("layout: %ls not found", name);
        return false;
    }
    void* vt = *static_cast<void**>(var);
    cvars::Type type = cvars::GetType(var);
    void* data = cvars::AsData(var, type);
    auto expect = static_cast<uint8_t*>(var) + cvars::kDataValue;
    float value = -1;
    uint32_t main = 0, render = 0;
    bool ok = cvars::InSection(g_rdata, vt) && type == want && data == expect && cvars::GetFloat(var, value) &&
              cvars::SafeCopy(&main, expect, 4) && cvars::SafeCopy(&render, expect + 4, 4);
    float mainValue = 0, renderValue = 0;
    if (want == cvars::Type::Float) {
        memcpy(&mainValue, &main, 4);
        memcpy(&renderValue, &render, 4);
    } else {
        mainValue = static_cast<float>(static_cast<int32_t>(main));
        renderValue = static_cast<float>(static_cast<int32_t>(render));
    }
    ok = ok && (IsFloat(value, mainValue) || IsFloat(value, renderValue));
    Log("layout: %ls vtable %s, type %d, data %s, GetFloat %g, stored %g/%g, flags 0x%08x", name, Where(vt).c_str(),
        static_cast<int>(type), data == expect ? "at +0x48" : "elsewhere", value, mainValue, renderValue,
        cvars::Flags(var));
    return ok;
}

bool CheckLayout() {
    bool ok = CheckOne(L"r.ScreenPercentage", cvars::Type::Float) && CheckOne(L"r.MaxAnisotropy", cvars::Type::Int);
    Log("layout: %s", ok ? "matches" : "MISMATCH");
    return ok;
}

// The singleton's address once (a scan of .text), then waits for the engine to create the
// manager and checks the layout. Called every frame until it succeeds or gives up.
bool FindManager() {
    static void** singleton = nullptr;
    static bool scanned = false;
    if (g_manager) return true;
    if (g_unsupported) return false;
    if (!scanned) {
        scanned = true;
        cvars::ManagerScan s = cvars::ScanConsoleManager(g_base);
        Log("cvars: IConsoleManager::Singleton at %s (%d of %d lookups agree), FindConsoleVariable at +0x%x (%d of %d)",
            s.singleton ? Where(s.singleton).c_str() : "?", s.votes, s.matches, s.findOffset, s.findVotes, s.findMatches);
        if (!s.singleton || s.votes < 5 || s.votes * 4 < s.matches * 3)
            return Unsupported("IConsoleManager::Singleton not found"), false;
        if (s.findOffset != static_cast<int32_t>(cvars::kFindConsoleVariable))
            return Unsupported("FindConsoleVariable is not at the expected vtable offset"), false;
        singleton = s.singleton;
    }
    void* manager = nullptr;
    if (!cvars::SafeCopy(&manager, singleton, 8) || !manager) return false;
    void* vt = nullptr;
    if (!cvars::SafeCopy(&vt, manager, 8) || !cvars::InSection(g_rdata, vt))
        return Unsupported("the console manager's vtable is not in the executable"), false;
    g_manager = manager;
    Log("cvars: manager %p, vtable %s", manager, Where(vt).c_str());
    if (!CheckLayout()) {
        g_manager = nullptr;
        return Unsupported("console variable layout check failed (see AITDTweaks.log)"), false;
    }
    g_status = "ready";
    return true;
}

// ---------------------------------------------------------------- forced values

// A cvar the saved settings force. Released (handed back to the game at its own priority)
// when no setting wants it any more.
struct Override {
    const wchar_t* cvar;
    bool wanted = false;  // a setting wants it forced this frame
    float value = 0;      // to this
    bool active = false;  // forced right now
    float applied = 0;
    void* var = nullptr;
    bool missing = false;
    bool haveGame = false;
    float game = 0;          // the game's value before we first forced it
    uint32_t gameFlags = 0;  // and its flags (priority)
    uint32_t repairs = 0;
};

std::vector<Override> g_overrides;

Override* FindOverride(const wchar_t* cvar) {
    for (Override& o : g_overrides)
        if (wcscmp(o.cvar, cvar) == 0) return &o;
    g_overrides.push_back({cvar});
    return &g_overrides.back();
}

void* Resolve(Override& o) {
    if (o.var || o.missing) return o.var;
    o.var = cvars::FindVariable(g_manager, o.cvar);
    if (!o.var) {
        o.missing = true;
        Log("override: %ls is not a cvar in this build", o.cvar);
    }
    return o.var;
}

// The game's value: what it had when we took the cvar over, or else what it has now.
bool GameValue(Override& o, float& out) {
    if (o.active && o.haveGame) {
        out = o.game;
        return true;
    }
    return g_manager && Resolve(o) && cvars::GetFloat(o.var, out);
}

bool GameValue(const wchar_t* cvar, float& out) { return GameValue(*FindOverride(cvar), out); }

std::wstring Text(float v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%g", v);
    return buf;
}

// Every frame starts with nothing wanted; each setting then asks for what it forces, so two
// settings touching the same cvar can't release each other's value.
void BeginFrame() {
    for (Override& o : g_overrides) o.wanted = false;
}

void Want(const wchar_t* cvar, float value) {
    Override* o = FindOverride(cvar);
    o->wanted = true;
    o->value = value;
}

void ApplyOverrides() {
    for (Override& o : g_overrides) {
        if (o.wanted == o.active && (!o.wanted || o.value == o.applied)) continue;
        if (!Resolve(o)) {
            o.wanted = false;
            continue;
        }
        float before = 0;
        cvars::GetFloat(o.var, before);
        if (o.wanted) {
            if (!o.active) {  // what to hand back later
                o.haveGame = cvars::GetFloat(o.var, o.game);
                o.gameFlags = cvars::Flags(o.var);
            }
            bool ok = cvars::Set(o.var, Text(o.value).c_str(), cvars::kSetByConsole) && cvars::MirrorToRenderer(o.var);
            float after = 0;
            cvars::GetFloat(o.var, after);
            Log("override: %ls = %g%s (was %g, now %g, game flags 0x%08x)", o.cvar, o.value,
                ok ? "" : " (Set or mirror failed)", before, after, o.gameFlags);
            o.active = ok;
            o.applied = o.value;
        } else {
            // Back to the game's priority first, or Set would refuse the lower one.
            cvars::Flags(o.var) = o.gameFlags;
            bool ok = cvars::Set(o.var, Text(o.game).c_str(), o.gameFlags & 0xff000000) && cvars::MirrorToRenderer(o.var);
            float after = 0;
            cvars::GetFloat(o.var, after);
            Log("override: %ls released to %g%s (was %g, now %g)", o.cvar, o.game, ok ? "" : " (Set or mirror failed)",
                before, after);
            o.active = false;
        }
    }
}

// Puts a forced value back if something changed it (the game's graphics menu, a level load).
void CheckOverrides() {
    for (Override& o : g_overrides) {
        if (!o.active) continue;
        float now = 0;
        if (cvars::GetFloat(o.var, now) && IsFloat(now, o.applied)) continue;
        if (++o.repairs <= 5)
            Log("override: %ls changed to %g behind our back; setting %g again%s", o.cvar, now, o.applied,
                o.repairs == 5 ? " (not logging further repairs)" : "");
        cvars::Set(o.var, Text(o.applied).c_str(), cvars::kSetByConsole);
        cvars::MirrorToRenderer(o.var);
    }
}

// ---------------------------------------------------------------- settings

// A setting is a choice or a slider, kept in ReShade.ini under its key.
//   choice  labels[0] is the game's own; option n forces values[n - 1].
//   slider  while its box is ticked, forces `cvar` to the slider's value and `with` along.
// A setting with a parent is shown and applied only while the parent is active and its
// choice is one of `when` (GTAO's quality under ambient occlusion, for example).
struct CvarValue {
    const wchar_t* cvar;
    float value;
};

struct Setting {
    const char* key = "";
    const char* name = "";
    const char* help = "";
    const wchar_t* shown = nullptr;  // the cvar whose game value the tooltip shows
    // choice
    std::vector<const char*> labels;
    std::vector<std::vector<CvarValue>> values;
    int byDefault = 1;
    // slider
    bool slider = false;
    float min = 0, max = 1, def = 0;
    const char* format = "%.2f";
    bool isInt = false;
    bool onByDefault = true;
    std::vector<CvarValue> with;
    // parent
    const char* parent = nullptr;
    std::vector<int> when;
    // state
    int choice = 0;
    bool on = false;
    float value = 0;
};

Setting Choice(const char* key, const char* name, const char* help, const wchar_t* shown,
               std::vector<const char*> labels, std::vector<std::vector<CvarValue>> values, int byDefault = 1) {
    Setting s;
    s.key = key, s.name = name, s.help = help, s.shown = shown;
    s.labels = std::move(labels), s.values = std::move(values), s.byDefault = byDefault;
    return s;
}

Setting Slider(const char* key, const char* name, const char* help, const wchar_t* cvar, float min, float max,
               float def, const char* format, bool onByDefault, std::vector<CvarValue> with = {}) {
    Setting s;
    s.key = key, s.name = name, s.help = help, s.shown = cvar;
    s.slider = true, s.min = min, s.max = max, s.def = def, s.format = format, s.onByDefault = onByDefault;
    s.isInt = strchr(format, 'd') != nullptr;
    s.with = std::move(with);
    return s;
}

Setting Under(Setting s, const char* parent, std::vector<int> when) {
    s.parent = parent;
    s.when = std::move(when);
    return s;
}

struct Tab {
    const char* name;
    std::vector<Setting> items;
};

Tab g_image{"Image"};
Tab g_detail{"Detail"};
Tab g_shadows{"Shadows"};
Tab g_lighting{"Lighting"};
Tab* const kTabs[] = {&g_image, &g_detail, &g_shadows, &g_lighting};

// Defaults: what the earlier Shadow/Detail Max and SSGI packs set (pakchunk901, 904), which
// this tab set replaces; the newer, untested options default to the game's.
void DefineSettings() {
    g_image.items = {
        Choice("Dlaa", "DLAA",
               "DLSS at full resolution: the sharpest, steadiest image, no upscaling. Overrides the game's DLSS mode. "
               "Costs about as much as native resolution.",
               L"r.NGX.DLAA.Enable", {"Game", "On"}, {{{L"r.NGX.DLAA.Enable", 1}}}, 0),
        Slider("DlssSharpness", "DLSS sharpening",
               "Sharpening DLSS applies to its output. 0 is none; the game's own is usually 0.",
               L"r.NGX.DLSS.Sharpness", 0, 1, 0.2f, "%.2f", false),
        Slider("TonemapSharpen", "Image sharpening",
               "The tonemapper's sharpening, applied after upscaling. Small values counter DLSS softness; high values "
               "add halos.",
               L"r.Tonemapper.Sharpen", 0, 2, 0.5f, "%.2f", false),
    };
    g_detail.items = {
        Choice("Anisotropy", "Texture filtering", "Sharper floors and walls at shallow angles. Nearly free.",
               L"r.MaxAnisotropy", {"Game", "16x"}, {{{L"r.MaxAnisotropy", 16}}}),
        Slider("ViewDistance", "View distance",
               "How far away objects are drawn, as a multiple of the game's distances. Less pop-in.",
               L"r.ViewDistanceScale", 1, 3, 2, "%.1fx", true),
        Slider("MeshDetail", "Mesh detail distance",
               "Smaller values keep detailed models further away (the game's value is 1). 0.5: twice as far.",
               L"r.StaticMeshLODDistanceScale", 0.25f, 1, 0.5f, "%.2f", true),
        Slider("FoliageDetail", "Foliage detail distance",
               "How far detailed foliage is kept, as a multiple of the game's distance.",
               L"foliage.LODDistanceScale", 1, 4, 2, "%.1fx", true),
        Choice("CharacterDetail", "Character detail",
               "Full: characters keep their most detailed model at a greater distance.", L"r.SkeletalMeshLODBias",
               {"Game", "Full"}, {{{L"r.SkeletalMeshLODBias", -1}}}),
        Choice("KeepTextures", "Keep textures loaded",
               "Keep every texture in view at full resolution instead of streaming detail in. Removes texture "
               "pop-in; uses more video memory.",
               L"r.Streaming.FullyLoadUsedTextures", {"Game", "On"}, {{{L"r.Streaming.FullyLoadUsedTextures", 1}}}, 0),
    };
    g_shadows.items = {
        Choice("ShadowFiltering", "Shadow filtering",
               "Max: the highest shadow filtering quality (5), smoother shadow edges.", L"r.ShadowQuality",
               {"Game", "Max"}, {{{L"r.ShadowQuality", 5}}}),
        Choice("ShadowResolution", "Shadow resolution",
               "Shadow map size for lamps, the flashlight and the sun or moon. Larger is crisper; 8192 costs the "
               "most video memory.",
               L"r.Shadow.MaxCSMResolution", {"Game", "4096", "8192"},
               {{{L"r.Shadow.MaxResolution", 4096}, {L"r.Shadow.MaxCSMResolution", 4096}},
                {{L"r.Shadow.MaxResolution", 8192}, {L"r.Shadow.MaxCSMResolution", 8192}}},
               2),
        Slider("ShadowDistance", "Shadow distance",
               "How far dynamic shadows are drawn, as a multiple of the game's distance.", L"r.Shadow.DistanceScale", 1,
               3, 2, "%.1fx", true),
        Slider("ShadowCascades", "Sun shadow cascades",
               "Cascades of the sun or moon's shadow map: more keep shadows detailed further from the camera, where "
               "the level's lights allow it.",
               L"r.Shadow.CSM.MaxCascades", 1, 10, 10, "%d", true),
        Choice("SmallShadows", "Small object shadows",
               "On: small and distant objects cast shadows too.", L"r.Shadow.RadiusThreshold", {"Game", "On"},
               {{{L"r.Shadow.RadiusThreshold", 0.01f}}}),
        Choice("CascadeBlend", "Cascade blending",
               "Soft: a twice-as-wide fade between shadow cascades, so the line where shadows get blurrier is harder "
               "to see.",
               L"r.Shadow.CSM.TransitionScale", {"Game", "Soft"}, {{{L"r.Shadow.CSM.TransitionScale", 2}}}, 0),
        Choice("SoftShadows", "Soft shadows (PCSS)",
               "Shadows that soften the farther they fall from what casts them, like real ones: sharp at contact, "
               "soft further away. The engine's experimental PCSS; check it in game.",
               L"r.Shadow.FilterMethod", {"Game", "On"}, {{{L"r.Shadow.FilterMethod", 1}}}, 0),
        Under(Slider("SoftShadowSize", "Softness", "The largest blur PCSS may apply, in shadow map texels.",
                     L"r.Shadow.MaxSoftKernelSize", 10, 80, 40, "%d", false),
              "SoftShadows", {1}),
        Slider("LampShadowDetail", "Lamp shadow detail",
               "Shadow map texels per screen pixel for lamps and the flashlight (the engine's default is 1.27). "
               "Higher is sharper.",
               L"r.Shadow.TexelsPerPixelSpotlight", 1, 4, 2, "%.2f", false,
               {{L"r.Shadow.TexelsPerPixelPointlight", 2}, {L"r.Shadow.TexelsPerPixelRectlight", 2}}),
        Choice("CapsuleShadows", "Character soft shadows",
               "Soft shadows characters cast from indirect light (capsule shadows), at full resolution instead of "
               "half. Only where the game uses them.",
               L"r.CapsuleShadowsFullResolution", {"Game", "Full resolution"},
               {{{L"r.CapsuleShadows", 1}, {L"r.CapsuleShadowsFullResolution", 1}}}),
    };
    g_lighting.items = {
        Choice("AmbientOcclusion", "Ambient occlusion",
               "Shading in corners, creases and under objects.\nHigh: the game's AO at full quality.\nMore levels: "
               "also deeper, smoother shading over larger areas.\nGTAO: the engine's ground-truth ambient occlusion: "
               "more accurate contact shading, none on open surfaces.",
               L"r.AmbientOcclusionLevels", {"Game", "High", "More levels", "GTAO"},
               {{{L"r.AmbientOcclusionMaxQuality", 100}},
                {{L"r.AmbientOcclusionMaxQuality", 100}, {L"r.AmbientOcclusionLevels", 3}},
                {{L"r.AmbientOcclusionMaxQuality", 100}, {L"r.AmbientOcclusion.Method", 1}}}),
        Under(Slider("AoRadius", "Radius", "How far AO looks for occluders, as a multiple of the game's radius.",
                     L"r.AmbientOcclusionRadiusScale", 0.5f, 2, 1, "%.2fx", false),
              "AmbientOcclusion", {1, 2, 3}),
        Under(Slider("GtaoAngles", "GTAO quality", "Directions GTAO samples per pixel: more is smoother, costs more.",
                     L"r.GTAO.NumAngles", 1, 8, 4, "%d", false),
              "AmbientOcclusion", {3}),
        Under(Choice("GtaoFullRes", "GTAO resolution", "Full: computed at full resolution instead of half.",
                     L"r.GTAO.Downsample", {"Game", "Full"}, {{{L"r.GTAO.Downsample", 0}}}, 0),
              "AmbientOcclusion", {3}),
        Choice("Ssgi", "Global illumination (SSGI)",
               "Screen space global illumination: light bouncing off lit surfaces into the room around them (lamp "
               "spill, colour bleeding). The game ships it switched off.",
               L"r.SSGI.Enable", {"Game", "On"}, {{{L"r.SSGI.Enable", 1}}}),
        Under(Slider("SsgiQuality", "Quality", "Rays per pixel: higher is smoother, with less noise.", L"r.SSGI.Quality",
                     1, 4, 4, "%d", true),
              "Ssgi", {1}),
        Under(Choice("SsgiFullRes", "Resolution", "Full: traced at full resolution instead of half. Sharper, costs more.",
                     L"r.SSGI.HalfRes", {"Game", "Full"}, {{{L"r.SSGI.HalfRes", 0}}}),
              "Ssgi", {1}),
        Choice("Reflections", "Reflections",
               "Max: the highest screen space reflection quality, at full resolution (polished floors, wet surfaces).",
               L"r.SSR.Quality", {"Game", "Max"}, {{{L"r.SSR.Quality", 4}, {L"r.SSR.HalfResSceneColor", 0}}}),
        Slider("ReflectionRoughness", "Rough surface reflections",
               "How rough a surface can be and still get reflections. Higher reaches wood and stone; costs more and can "
               "look noisy.",
               L"r.SSR.MaxRoughness", 0.3f, 1, 0.8f, "%.2f", false),
        Choice("VolumetricFog", "Volumetric fog",
               "Detail of the fog and the light shafts through it.\nHigh: 6 px cells. Ultra: 4 px cells and 128 depth "
               "slices, the game's cinematic values.",
               L"r.VolumetricFog.GridPixelSize", {"Game", "High", "Ultra"},
               {{{L"r.VolumetricFog.GridPixelSize", 6}},
                {{L"r.VolumetricFog.GridPixelSize", 4}, {L"r.VolumetricFog.GridSizeZ", 128},
                 {L"r.VolumetricFog.HistoryMissSupersampleCount", 16}, {L"r.VolumetricFog.Jitter", 1}}},
               2),
        Choice("Skin", "Skin scattering",
               "Light scattering under the skin of faces and hands. High: the high quality, full resolution version.",
               L"r.SSS.Quality", {"Game", "High"},
               {{{L"r.SSS.Quality", 1}, {L"r.SSS.HalfRes", 0}, {L"r.SSS.Burley.Quality", 1}}}),
        Choice("TranslucencyLighting", "Smoke and glass lighting",
               "Resolution of the lighting volume for particles, smoke and glass. High: 128 (Epic uses 64), so they "
               "pick up light and shadow more precisely.",
               L"r.TranslucencyLightingVolumeDim", {"Game", "High"}, {{{L"r.TranslucencyLightingVolumeDim", 128}}}),
        Choice("LightShafts", "Light shafts",
               "Screen space light shafts (god rays) through windows and gaps. On: the high quality version.",
               L"r.LightShaftQuality", {"Game", "On"}, {{{L"r.LightShaftQuality", 1}}}),
    };
}

Setting* FindSetting(const char* key) {
    for (Tab* t : kTabs)
        for (Setting& s : t->items)
            if (strcmp(s.key, key) == 0) return &s;
    return nullptr;
}

// Shown and applied: no parent, or the parent is and its choice is one of `when`.
bool Active(const Setting& s) {
    if (!s.parent) return true;
    const Setting* p = FindSetting(s.parent);
    return p && Active(*p) && std::find(s.when.begin(), s.when.end(), p->choice) != s.when.end();
}

std::string OnKey(const Setting& s) { return std::string(s.key) + "On"; }

void Save(const Setting& s) {
    if (s.slider) {
        reshade::set_config_value(nullptr, kConfigSection, OnKey(s).c_str(), s.on);
        reshade::set_config_value(nullptr, kConfigSection, s.key, s.value);
    } else {
        reshade::set_config_value(nullptr, kConfigSection, s.key, s.choice);
    }
}

void Reset(Setting& s) {
    s.choice = s.byDefault;
    s.on = s.onByDefault;
    s.value = s.def;
    Save(s);
}

void Load(Setting& s) {
    s.choice = s.byDefault;
    s.on = s.onByDefault;
    s.value = s.def;
    if (s.slider) {
        reshade::get_config_value(nullptr, kConfigSection, OnKey(s).c_str(), s.on);
        reshade::get_config_value(nullptr, kConfigSection, s.key, s.value);
        s.value = std::clamp(s.value, s.min, s.max);
        if (s.isInt) s.value = roundf(s.value);
    } else {
        reshade::get_config_value(nullptr, kConfigSection, s.key, s.choice);
        s.choice = std::clamp(s.choice, 0, static_cast<int>(s.labels.size()) - 1);
    }
}

void Apply(const Setting& s) {
    // Register every cvar the setting can touch, so the Advanced tab lists it.
    if (s.slider) {
        FindOverride(s.shown);
        for (const CvarValue& c : s.with) FindOverride(c.cvar);
    } else {
        for (const auto& set : s.values)
            for (const CvarValue& c : set) FindOverride(c.cvar);
    }
    if (!Active(s)) return;
    if (s.slider) {
        if (!s.on) return;
        Want(s.shown, s.value);
        for (const CvarValue& c : s.with) Want(c.cvar, s.value);
    } else if (s.choice > 0) {
        for (const CvarValue& c : s.values[s.choice - 1]) Want(c.cvar, c.value);
    }
}

// ---------------------------------------------------------------- texture pool

struct VramChoice {
    const char* label;
    int gb;      // the smallest card this row is for
    int poolMb;  // r.Streaming.PoolSize
};
const VramChoice kVram[] = {
    {"6 GB or less", 0, 1024}, {"8 GB", 8, 2048},   {"10 GB", 10, 2560},
    {"12 GB", 12, 3072},       {"16 GB", 16, 4096}, {"20 GB or more", 20, 6144},
};
constexpr int kVramFallback = 1;  // 8 GB, if the card's memory can't be read
// Saved choice: 0 the game's own, 1 Auto, else 2 + a kVram row.
constexpr int kPoolGame = 0;
constexpr int kPoolAuto = 1;

uint64_t g_vramMb = 0;  // dedicated memory of the game's adapter, from DXGI (0: unknown)
int g_texturePool = kPoolAuto;

int DetectedVramRow() {
    if (!g_vramMb) return kVramFallback;
    int gb = static_cast<int>((g_vramMb + 512) / 1024);  // a "16 GB" card reports a little under
    int row = 0;
    for (int i = 0; i < static_cast<int>(std::size(kVram)); ++i)
        if (gb >= kVram[i].gb) row = i;
    return row;
}

int VramRow() { return g_texturePool == kPoolAuto ? DetectedVramRow() : g_texturePool - 2; }

void SaveTexturePool() { reshade::set_config_value(nullptr, kConfigSection, "TexturePool", g_texturePool); }

void ApplyTexturePool() {
    FindOverride(L"r.Streaming.PoolSize");
    if (g_texturePool != kPoolGame) Want(L"r.Streaming.PoolSize", static_cast<float>(kVram[VramRow()].poolMb));
}

// ---------------------------------------------------------------- loading and applying

void InitSettings() {
    DefineSettings();
    for (Tab* t : kTabs) {
        std::string line;
        for (Setting& s : t->items) {
            Load(s);
            char item[128];
            if (s.slider)
                snprintf(item, sizeof(item), "%s %s %g", s.name, s.on ? "on" : "game", s.value);
            else
                snprintf(item, sizeof(item), "%s %s", s.name, s.labels[s.choice]);
            line += std::string(line.empty() ? "" : ", ") + item;
        }
        Log("%s: %s", t->name, line.c_str());
    }
    reshade::get_config_value(nullptr, kConfigSection, "TexturePool", g_texturePool);
    g_texturePool = std::clamp(g_texturePool, 0, static_cast<int>(std::size(kVram)) + 1);
    Log("texture pool: %s", g_texturePool == kPoolGame ? "game" : g_texturePool == kPoolAuto ? "auto" : kVram[VramRow()].label);
}

void ApplySettings() {
    BeginFrame();
    ApplyTexturePool();
    for (Tab* t : kTabs)
        for (const Setting& s : t->items) Apply(s);
}

// ---------------------------------------------------------------- events

uint64_t g_frames = 0;

// Dedicated memory of the adapter a D3D11 device runs on, in MB (0 if unknown).
uint64_t AdapterMemoryMb(ID3D11Device* d3d) {
    uint64_t mb = 0;
    IDXGIDevice* dxgiDevice = nullptr;
    if (SUCCEEDED(d3d->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice)))) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC desc = {};
            if (SUCCEEDED(adapter->GetDesc(&desc))) mb = desc.DedicatedVideoMemory >> 20;
            adapter->Release();
        }
        dxgiDevice->Release();
    }
    return mb;
}

void OnInitDevice(device* dev) {
    std::lock_guard lock(g_mutex);
    bool d3d11 = dev->get_api() == device_api::d3d11;
    Log("device created: %s", d3d11 ? "Direct3D 11" : "not Direct3D 11");
    if (!d3d11) return;
    uint64_t mb = AdapterMemoryMb(reinterpret_cast<ID3D11Device*>(dev->get_native()));
    if (mb > g_vramMb) {
        g_vramMb = mb;
        Log("device: %llu MB dedicated video memory", static_cast<unsigned long long>(mb));
    }
}

void OnPresent(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
    std::lock_guard lock(g_mutex);
    ++g_frames;
    if (!FindManager()) return;
    ApplySettings();
    ApplyOverrides();
    if (g_frames % 60 == 0) CheckOverrides();
}

// ---------------------------------------------------------------- overlay

// Each tab is one table, name | control, with the same column widths on every tab. Hovering a
// setting explains it and shows the game's own value.

void Help(const char* text, const wchar_t* cvar = nullptr) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
    if (text && *text) ImGui::TextUnformatted(text);
    float game = 0;
    if (cvar && GameValue(cvar, game)) ImGui::TextDisabled("Game value: %g  (%ls)", game, cvar);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

bool BeginSettings(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) return false;
    float name = ImGui::CalcTextSize("Global illumination (SSGI)").x;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, name + ImGui::GetFontSize() * 2.0f);
    ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 16);
    return true;
}

// One row: the name (hover for help), then the control cell. A sub-setting is indented under
// the row it belongs to.
void SettingRow(const char* name, const char* help, const wchar_t* cvar, bool sub) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (sub) ImGui::Indent(ImGui::GetFontSize());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name);
    Help(help, cvar);
    if (sub) ImGui::Unindent(ImGui::GetFontSize());
    ImGui::TableNextColumn();
}

// True if the setting changed.
bool Draw(Setting& s) {
    if (!Active(s)) return false;
    ImGui::PushID(s.key);
    SettingRow(s.name, s.help, s.shown, s.parent != nullptr);
    bool changed = false;
    if (s.slider) {
        // The box: ticked forces the slider's value; unticked leaves the game's, which the
        // slider then shows.
        changed |= ImGui::Checkbox("##on", &s.on);
        Help(s.on ? "Forced to the slider's value. Untick for the game's own." : "The game's own value. Tick to set it.",
             s.shown);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        float shown = s.value;
        if (!s.on) GameValue(s.shown, shown);
        ImGui::BeginDisabled(!s.on);
        if (s.isInt) {
            int v = static_cast<int>(lroundf(shown));
            if (ImGui::SliderInt("##v", &v, static_cast<int>(s.min), static_cast<int>(s.max), s.format) && s.on) {
                s.value = static_cast<float>(v);
                changed = true;
            }
        } else if (ImGui::SliderFloat("##v", &shown, s.min, s.max, s.format) && s.on) {
            s.value = shown;
            changed = true;
        }
        ImGui::EndDisabled();
        Help(s.help, s.shown);
    } else {
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::Combo("##c", &s.choice, s.labels.data(), static_cast<int>(s.labels.size()));
        Help(s.help, s.shown);
    }
    ImGui::PopID();
    return changed;
}

bool ResetButton(const char* label = "Reset tab") {
    ImGui::Spacing();
    float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
    return ImGui::Button(label);
}

void Note(const char* text, bool warn = false) {
    ImGui::PushTextWrapPos(0.0f);
    if (warn)
        ImGui::TextColored(kWarn, "%s", text);
    else
        ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
}

void ResetTab(Tab& t) {
    for (Setting& s : t.items) Reset(s);
    if (&t == &g_detail) {
        g_texturePool = kPoolAuto;
        SaveTexturePool();
    }
}

void DrawTab(Tab& t) {
    if (BeginSettings(t.name)) {
        if (&t == &g_detail) {
            char autoLabel[64];
            snprintf(autoLabel, sizeof(autoLabel), "Auto (%s)", kVram[DetectedVramRow()].label);
            const char* pools[2 + std::size(kVram)] = {"Game", autoLabel};
            for (size_t i = 0; i < std::size(kVram); ++i) pools[2 + i] = kVram[i].label;
            char poolHelp[256];
            snprintf(poolHelp, sizeof(poolHelp),
                     "Video memory for textures, sized to your graphics card (now %d MB), so textures load at full "
                     "resolution with less pop-in. Auto reads your card's memory.",
                     kVram[VramRow()].poolMb);
            SettingRow("Texture memory", poolHelp, L"r.Streaming.PoolSize", false);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo("##pool", &g_texturePool, pools, static_cast<int>(std::size(pools)))) SaveTexturePool();
            Help(poolHelp, L"r.Streaming.PoolSize");
        }
        for (Setting& s : t.items)
            if (Draw(s)) Save(s);
        ImGui::EndTable();
    }
    if (&t == &g_image)
        Note("The DLSS mode, frame generation, HDR, film grain, motion blur and depth of field are in the game's own "
             "graphics menu.");
    if (ResetButton()) ResetTab(t);
}

void DrawAdvanced() {
    ImGui::TextDisabled("Console variables: %s", g_status.c_str());
    ImGui::Spacing();
    Note("Every console variable the settings control: the game's own value, and what it is now.");
    if (ImGui::BeginTable("##cvars", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Console variable");
        ImGui::TableSetupColumn("Game");
        ImGui::TableSetupColumn("Now");
        ImGui::TableHeadersRow();
        for (Override& o : g_overrides) {
            ImGui::TableNextColumn();
            ImGui::Text("%ls", o.cvar);
            ImGui::TableNextColumn();
            float game = 0;
            if (o.missing)
                ImGui::TextDisabled("not in this build");
            else if (GameValue(o, game))
                ImGui::Text("%g", game);
            else
                ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            float now = 0;
            if (o.var && cvars::GetFloat(o.var, now)) {
                if (o.active)
                    ImGui::Text("%g (forced)", now);
                else
                    ImGui::TextDisabled("%g", now);
            } else {
                ImGui::TextDisabled("-");
            }
        }
        ImGui::EndTable();
    }
    if (ResetButton("Reset all settings"))
        for (Tab* t : kTabs) ResetTab(*t);
}

void DrawOverlay(effect_runtime*) {
    std::lock_guard lock(g_mutex);
    ImGui::TextDisabled(AT_PRODUCT " " AT_VERSION "  -  hover a setting for details; changes apply at once and are saved");
    if (g_unsupported) {
        Note((std::string("Not active: ") + g_status + ".").c_str(), true);
        return;
    }
    if (!g_manager) {
        Note("Waiting for the game's console variables...", true);
        return;
    }
    if (g_timestamp != kKnownTimestamp) Note("A different game build than the one analysed; the layout check passed.", true);
    if (!ImGui::BeginTabBar("##tabs")) return;
    for (Tab* t : kTabs)
        if (ImGui::BeginTabItem(t->name)) {
            ImGui::Spacing();
            DrawTab(*t);
            ImGui::EndTabItem();
        }
    if (ImGui::BeginTabItem("Advanced")) {
        ImGui::Spacing();
        DrawAdvanced();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void Install() {
    OpenLog();
    LogGameInfo();
    InitSettings();
    reshade::register_event<reshade::addon_event::init_device>(&OnInitDevice);
    reshade::register_event<reshade::addon_event::present>(&OnPresent);
    reshade::register_overlay(AT_PRODUCT, &DrawOverlay);
}

void Uninstall() {
    reshade::unregister_overlay(AT_PRODUCT, &DrawOverlay);
    if (g_log) {
        Log("unloading");
        fclose(g_log);
        g_log = nullptr;
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            Install();
            break;
        case DLL_PROCESS_DETACH:
            Uninstall();
            reshade::unregister_addon(module);
            break;
    }
    return TRUE;
}
