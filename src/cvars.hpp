// Finds Unreal console variables in Alone in the Dark (UE 4.27, no PDB) and reads and sets
// them. Adapted from FallenOrderTweaks (UE 4.21) and UntilDawnTweaks (UE 5); every offset
// below was read from AloneInTheDark-Win64-Shipping.exe (docs/FINDINGS.md).
//
// IConsoleManager::Singleton is found from the game's own code: cvar lookups load it with
// `mov rcx, [rip+Singleton]` shortly before `lea rdx, [rip+L"r.xxx"]`, then call
// FindConsoleVariable through the vtable. A few unrelated matches exist, so the address and
// vtable offset most matches agree on win.
//
// Vtable offsets (FConsoleManager, FConsoleVariable<T>, FConsoleVariableRef<T>):
//   IConsoleManager  +0xb0  FindConsoleVariable(const TCHAR* Name, bool bTrackFrequentCalls)
//   IConsoleObject   +0x18  GetFlags
//                    +0x30  IsVariableBool    +0x38 IsVariableInt
//                    +0x40  IsVariableFloat   +0x48 IsVariableString
//                    +0x50  AsVariableBool    +0x58 AsVariableInt   +0x60 AsVariableFloat
//                           (TConsoleVariableData<T>*, null for Ref cvars)
//   IConsoleVariable +0x80  Set(const TCHAR* Value, EConsoleVariableFlags SetBy)
//                    +0x88  GetBool  +0x90 GetInt  +0x98 GetFloat
//
// Storage:
//   data cvar  main (game thread) value +0x48, renderer's copy right after it
//              (+0x4c for int/float, +0x49 for bool)
//   Ref cvar   pointer to the global the engine reads at +0x48, main value +0x50
// Set stores the main value, then OnCVarChange copies it to the renderer's - but only on
// the game thread. Off it (ReShade's present runs on the render thread) MirrorToRenderer
// does that copy.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <map>

namespace cvars {

constexpr uint32_t kFindConsoleVariable = 0xb0;
constexpr uint32_t kIsVariableBool = 0x30;
constexpr uint32_t kIsVariableInt = 0x38;
constexpr uint32_t kIsVariableFloat = 0x40;
constexpr uint32_t kIsVariableString = 0x48;
constexpr uint32_t kAsVariableBool = 0x50;
constexpr uint32_t kAsVariableInt = 0x58;
constexpr uint32_t kAsVariableFloat = 0x60;
constexpr uint32_t kSet = 0x80;
constexpr uint32_t kGetFloat = 0x98;
constexpr uint32_t kFlags = 0x18;      // EConsoleVariableFlags; top byte = SetBy priority
constexpr uint32_t kDataValue = 0x48;  // TConsoleVariableData<T> inside FConsoleVariable<T>
constexpr uint32_t kRefGlobal = 0x48;  // FConsoleVariableRef<T>: T* to the engine global
constexpr uint32_t kRefValue = 0x50;   // and its own (main) value
// What FConsoleManager::ProcessUserConsoleInput passes to Set (UE 4: ECVF_SetByConsole), the
// highest priority there is, above the game's code (0x08000000) and its graphics menu.
constexpr uint32_t kSetByConsole = 0x09000000;

struct Section {
    const uint8_t* begin = nullptr;
    size_t size = 0;
};

inline bool FindSection(const uint8_t* base, const char* name, Section& out) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (strncmp(reinterpret_cast<const char*>(sec[i].Name), name, 8) == 0) {
            out = {base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize};
            return true;
        }
    return false;
}

inline bool InSection(const Section& s, const void* p) {
    auto b = static_cast<const uint8_t*>(p);
    return s.begin && b >= s.begin && b < s.begin + s.size;
}

// A NUL-terminated UTF-16 string that starts right after another string's terminator.
inline const uint8_t* FindWideString(const Section& s, const wchar_t* text) {
    const size_t n = (wcslen(text) + 1) * 2;
    for (size_t i = 2; i + n <= s.size; i += 2)
        if (s.begin[i - 2] == 0 && s.begin[i - 1] == 0 && memcmp(s.begin + i, text, n) == 0) return s.begin + i;
    return nullptr;
}

struct ManagerScan {
    void** singleton = nullptr;  // IConsoleManager::Singleton (the variable holding the pointer)
    int votes = 0, matches = 0;  // how many lookups agreed on it, out of how many
    int32_t findOffset = -1;     // the vtable offset those lookups call
    int findVotes = 0, findMatches = 0;
};

// Cvars the game looks up by name. Offline over these: 89 of 101 lookups load the singleton
// at RVA 0x5ebd950, 71 call +0xb0 (tools/callsites.py).
const wchar_t* const kLookupNames[] = {
    L"r.ScreenPercentage", L"r.MaxAnisotropy",     L"r.Shadow.MaxResolution", L"r.Shadow.MaxCSMResolution",
    L"r.ViewDistanceScale", L"r.SSR.Quality",      L"r.TemporalAA.Upsampling", L"r.Streaming.PoolSize",
    L"r.DetailMode",        L"r.MaterialQualityLevel", L"r.PostProcessAAQuality", L"r.VSync",
    L"r.MotionBlurQuality", L"r.DepthOfFieldQuality",  L"r.BloomQuality",         L"r.Upscale.Quality",
    L"r.SceneColorFringeQuality", L"r.Tonemapper.Sharpen", L"r.HDR.EnableHDROutput", L"t.MaxFPS",
    L"r.AmbientOcclusionLevels", L"r.SSGI.Enable", L"r.NGX.DLSS.Quality", L"r.NGX.Enable",
    L"r.DefaultFeature.AntiAliasing", L"r.ShadowQuality", L"r.VolumetricFog", L"r.SSS.Quality",
};

// Scans .text for lookups of those names: mov rcx,[rip+Singleton] within 0x40 bytes before
// `lea rdx, [rip+name]`, and the first call [rax+disp] within 0x30 bytes after it.
inline ManagerScan ScanConsoleManager(const uint8_t* base) {
    ManagerScan out;
    Section text, rdata;
    if (!FindSection(base, ".text", text) || !FindSection(base, ".rdata", rdata)) return out;
    const uint8_t* names[sizeof(kLookupNames) / sizeof(kLookupNames[0])];
    size_t nameCount = 0;
    for (const wchar_t* n : kLookupNames)
        if (const uint8_t* s = FindWideString(rdata, n)) names[nameCount++] = s;
    if (!nameCount) return out;
    std::map<void**, int> singletons;
    std::map<int32_t, int> offsets;
    for (size_t i = 0x40; i + 0x30 <= text.size; ++i) {
        const uint8_t* p = text.begin + i;
        // lea rdx, [rip+disp32] -> one of the names
        if (p[0] != 0x48 || p[1] != 0x8d || p[2] != 0x15) continue;
        int32_t disp;
        memcpy(&disp, p + 3, 4);
        const uint8_t* target = p + 7 + disp;
        bool isName = false;
        for (size_t k = 0; k < nameCount && !isName; ++k) isName = target == names[k];
        if (!isName) continue;
        for (const uint8_t* q = p - 7; q >= p - 0x40; --q) {
            if (q[0] != 0x48 || q[1] != 0x8b || q[2] != 0x0d) continue;
            memcpy(&disp, q + 3, 4);
            ++singletons[reinterpret_cast<void**>(const_cast<uint8_t*>(q + 7 + disp))];
            ++out.matches;
            break;
        }
        for (const uint8_t* c = p + 7; c + 6 <= p + 0x30; ++c) {
            if (c[0] != 0xff || (c[1] != 0x50 && c[1] != 0x90)) continue;
            int32_t off = c[2];
            if (c[1] == 0x90) memcpy(&off, c + 2, 4);
            ++offsets[off];
            ++out.findMatches;
            break;
        }
    }
    for (auto& [addr, n] : singletons)
        if (n > out.votes) out.singleton = addr, out.votes = n;
    for (auto& [off, n] : offsets)
        if (n > out.findVotes) out.findOffset = off, out.findVotes = n;
    return out;
}

using FindFn = void*(__fastcall*)(void* self, const wchar_t* name, bool track);
using PtrFn = void*(__fastcall*)(void* self);
using BoolFn = bool(__fastcall*)(void* self);
using GetFloatFn = float(__fastcall*)(void* self);
using SetFn = void(__fastcall*)(void* self, const wchar_t* value, uint32_t setBy);

template <typename Fn>
inline Fn Virtual(void* object, uint32_t offset) {
    return *reinterpret_cast<Fn*>(*reinterpret_cast<uint8_t**>(object) + offset);
}

// memcpy that reports a fault instead of raising it.
inline bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The IConsoleVariable object for <name> (null if absent or a command). Guarded: a wrong
// vtable layout faults here instead of taking the game down.
inline void* FindVariable(void* manager, const wchar_t* name) {
    __try {
        return Virtual<FindFn>(manager, kFindConsoleVariable)(manager, name, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

inline bool CallPtr(void* var, uint32_t offset, void*& out) {
    __try {
        out = Virtual<PtrFn>(var, offset)(var);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool CallBool(void* var, uint32_t offset, bool& out) {
    __try {
        out = Virtual<BoolFn>(var, offset)(var);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The engine's own GetFloat, for any numeric cvar (ints and bools convert).
inline bool GetFloat(void* var, float& out) {
    __try {
        out = Virtual<GetFloatFn>(var, kGetFloat)(var);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// IConsoleVariable::Set: refuses a SetBy priority lower than the last one (CanChange), stores
// the main value, records the priority and runs the change delegates. Follow it with
// MirrorToRenderer off the game thread. False if it faulted.
inline bool Set(void* var, const wchar_t* value, uint32_t setBy) {
    __try {
        Virtual<SetFn>(var, kSet)(var, value, setBy);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The flags word; the top byte is the priority of the last Set.
inline uint32_t& Flags(void* var) { return *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(var) + kFlags); }

enum class Type { Unknown, Bool, Int, Float, String };

// The value type, from the IsVariable* slots.
inline Type GetType(void* var) {
    bool is = false;
    if (CallBool(var, kIsVariableFloat, is) && is) return Type::Float;
    if (CallBool(var, kIsVariableInt, is) && is) return Type::Int;
    if (CallBool(var, kIsVariableBool, is) && is) return Type::Bool;
    if (CallBool(var, kIsVariableString, is) && is) return Type::String;
    return Type::Unknown;
}

// TConsoleVariableData<T>* for data cvars, null for Ref cvars.
inline void* AsData(void* var, Type t) {
    void* out = nullptr;
    switch (t) {
        case Type::Bool: CallPtr(var, kAsVariableBool, out); break;
        case Type::Int: CallPtr(var, kAsVariableInt, out); break;
        case Type::Float: CallPtr(var, kAsVariableFloat, out); break;
        default: break;
    }
    return out;
}

// Copies the main value to where the renderer reads it (what OnCVarChange does on the game
// thread). Data cvars: to the renderer's copy right after it. Ref cvars: +0x50 to the engine
// global. An aligned store, so the render thread sees old or new, never a mix.
inline bool MirrorToRenderer(void* var) {
    Type t = GetType(var);
    if (t != Type::Bool && t != Type::Int && t != Type::Float) return false;
    size_t size = t == Type::Bool ? 1 : 4;
    auto obj = static_cast<uint8_t*>(var);
    uint8_t value[4];
    if (void* data = AsData(var, t))
        return SafeCopy(value, data, size) && SafeCopy(static_cast<uint8_t*>(data) + size, value, size);
    uint8_t* global = nullptr;
    if (!SafeCopy(&global, obj + kRefGlobal, 8) || !global) return false;
    return SafeCopy(value, obj + kRefValue, size) && SafeCopy(global, value, size);
}

}  // namespace cvars
