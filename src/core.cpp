/*
 * Drop To Sampler
 * Drop media on TCP/MCP -> insert sampler with sample loaded.
 *
 * Platform-neutral core: everything that talks to REAPER.
 * Builds on Windows (Win32) and macOS (SWELL, provided by REAPER).
 * OS-specific drag interception lives in win/ and mac/ (see platform.h).
 */

#if defined(_WIN32) && defined(UNICODE)
#error "Build without UNICODE: core.cpp uses the ANSI/UTF-8 Win32 API names"
#endif

#define REAPERAPI_IMPLEMENT
#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"
#include "resource.h"
#include "core.h"
#include "platform.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// Small Win32/SWELL compatibility helpers
// ---------------------------------------------------------------------------
static int CtrlIdOf(HWND hwnd)
{
#ifdef _WIN32
    return GetDlgCtrlID(hwnd);
#else
    return (int)GetWindowLong(hwnd, GWL_ID);
#endif
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
enum SamplerMode
{
    Sampler_RS5k = 0,
    Sampler_Cartridge = 1,
    Sampler_AlwaysPrompt = 2
};

enum RecArmMode
{
    RecArm_Default = 0,  // leave as preferences
    RecArm_On = 1,       // force record arm
    RecArm_Auto = 2      // auto-recarm when track selected (B_AUTO_RECARM)
};

enum ShowSamplerMode
{
    ShowSampler_Default = 0, // leave as preferences
    ShowSampler_Yes = 1,
    ShowSampler_No = 2
};

enum MultiFilesMode
{
    MultiFiles_OneTrack = 0,      // all samplers on the same track
    MultiFiles_MultiTracks = 1,   // one new track per file
    MultiFiles_AlwaysPrompt = 2
};

enum MultiSamplersMode
{
    MultiSamplers_Layers = 0,        // default: just stack FX
    MultiSamplers_Chromatic = 1,     // one MIDI key per sampler, base C4=60
    MultiSamplers_AlwaysPrompt = 2
};

enum ExistingSamplerMode
{
    Existing_Add = 0,           // always add new FX
    Existing_Replace = 1,       // replace if exactly one matching sampler on track
    Existing_ReplaceOnSlot = 2  // replace only when drop hits that FX slot on TCP
};

static const char* kExtSection = "DropToSampler";
static const char* kExtSectionLegacy = "DropMediaTcpMcpSampler";
static const char* kExtKeyDebug = "debug";
static const char* kExtKeySampler = "sampler";
static const char* kExtKeyRecArm = "recarm";
static const char* kExtKeyShow = "show_sampler";
static const char* kExtKeyMulti = "multi_files";
static const char* kExtKeyMultiSamplers = "multi_samplers";
static const char* kExtKeyExisting = "existing_sampler";

static bool g_debug = false;
static int g_samplerMode = Sampler_RS5k; // default RS5k
static int g_recArmMode = RecArm_Auto;
static int g_showSamplerMode = ShowSampler_Default;
static int g_multiFilesMode = MultiFiles_MultiTracks;
static int g_multiSamplersMode = MultiSamplers_Layers;
static int g_existingMode = Existing_ReplaceOnSlot;
static int g_dropExistingMode = Existing_Add; // per-drop (modifiers)
static bool g_cartridgeAvailable = false;

static int ReadIntSetting(const char* key, int lo, int hi, int def)
{
    const char* v = GetExtState ? GetExtState(kExtSection, key) : nullptr;
    if (v && v[0])
    {
        int m = atoi(v);
        if (m >= lo && m <= hi)
            return m;
    }
    return def;
}

static void LoadSettings()
{
    g_debug = false;
    g_samplerMode = Sampler_RS5k;
    g_recArmMode = RecArm_Auto;
    g_showSamplerMode = ShowSampler_Default;
    g_multiFilesMode = MultiFiles_MultiTracks;
    g_multiSamplersMode = MultiSamplers_Layers;
    g_existingMode = Existing_ReplaceOnSlot;
    if (!GetExtState) return;

    const char* v = GetExtState(kExtSection, kExtKeyDebug);
    if (!v || !v[0])
        v = GetExtState(kExtSectionLegacy, kExtKeyDebug);
    if (v && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T'))
        g_debug = true;

    g_samplerMode       = ReadIntSetting(kExtKeySampler,       Sampler_RS5k,         Sampler_AlwaysPrompt,       g_samplerMode);
    g_recArmMode        = ReadIntSetting(kExtKeyRecArm,        RecArm_Default,       RecArm_Auto,                g_recArmMode);
    g_showSamplerMode   = ReadIntSetting(kExtKeyShow,          ShowSampler_Default,  ShowSampler_No,             g_showSamplerMode);
    g_multiFilesMode    = ReadIntSetting(kExtKeyMulti,         MultiFiles_OneTrack,  MultiFiles_AlwaysPrompt,    g_multiFilesMode);
    g_multiSamplersMode = ReadIntSetting(kExtKeyMultiSamplers, MultiSamplers_Layers, MultiSamplers_AlwaysPrompt, g_multiSamplersMode);
    g_existingMode      = ReadIntSetting(kExtKeyExisting,      Existing_Add,         Existing_ReplaceOnSlot,     g_existingMode);
}

static void SaveIntSetting(const char* key, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    SetExtState(kExtSection, key, buf, true);
}

static void SaveSettings()
{
    if (!SetExtState) return;
    SetExtState(kExtSection, kExtKeyDebug, g_debug ? "1" : "0", true);
    SaveIntSetting(kExtKeySampler, g_samplerMode);
    SaveIntSetting(kExtKeyRecArm, g_recArmMode);
    SaveIntSetting(kExtKeyShow, g_showSamplerMode);
    SaveIntSetting(kExtKeyMulti, g_multiFilesMode);
    SaveIntSetting(kExtKeyMultiSamplers, g_multiSamplersMode);
    SaveIntSetting(kExtKeyExisting, g_existingMode);
}

bool Core_IsDebug() { return g_debug; }

void Core_DebugLog(const char* msg)
{
    if (g_debug && ShowConsoleMsg)
        ShowConsoleMsg(msg);
}

static void DebugLog(const char* msg) { Core_DebugLog(msg); }


static void ApplyRecArmForNewTrack(MediaTrack* track)
{
    if (!track || !SetMediaTrackInfo_Value) return;
    if (g_recArmMode == RecArm_Default) return;

    // Input: MIDI -> All MIDI Inputs (4096 + 2016 = 6112, common REAPER value)
    SetMediaTrackInfo_Value(track, "I_RECINPUT", 6112.0);

    if (g_recArmMode == RecArm_On)
    {
        SetMediaTrackInfo_Value(track, "I_RECARM", 1.0);
    }
    else if (g_recArmMode == RecArm_Auto)
    {
        // Auto-recarm when track is selected (does not force arm immediately)
        SetMediaTrackInfo_Value(track, "B_AUTO_RECARM", 1.0);
    }
}

// forceHide: always hide (multi-track mode, or intermediate FX on same track)
// isLastOnTrack: only the last sampler on a track may follow Show sampler setting
static void ApplyShowSampler(MediaTrack* track, int fx, bool forceHide, bool isLastOnTrack)
{
    if (!track || fx < 0 || !TrackFX_Show) return;

    auto hide = [&]() {
        TrackFX_Show(track, fx, 0); // hide chain
        TrackFX_Show(track, fx, 2); // hide floating
    };

    if (forceHide || !isLastOnTrack || g_showSamplerMode == ShowSampler_No)
    {
        hide();
        return;
    }

    if (g_showSamplerMode == ShowSampler_Yes)
    {
        // 3 = show floating FX window
        TrackFX_Show(track, fx, 3);
    }
    // ShowSampler_Default: leave to REAPER preferences
}


// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static reaper_plugin_info_t* g_rec = nullptr;
static int g_timerTicks = 0;
static int g_cmdSettings = 0;
static HINSTANCE g_hInstance = nullptr;


// ---------------------------------------------------------------------------
// Resolve track under screen point
// ---------------------------------------------------------------------------
static MediaTrack* ResolveTrackAtPoint(int screenX, int screenY, bool* createdNew)
{
    if (createdNew) *createdNew = false;

    char info[256] = {};
    MediaTrack* tr = nullptr;

    if (GetThingFromPoint)
        tr = GetThingFromPoint(screenX, screenY, info, sizeof(info));
    else if (GetTrackFromPoint)
        tr = GetTrackFromPoint(screenX, screenY, nullptr);

    if (tr)
        return tr;

    if (!CountTracks || !InsertTrackAtIndex || !GetTrack)
        return nullptr;

    int n = CountTracks(nullptr);
    InsertTrackAtIndex(n, true);
    if (TrackList_AdjustWindows) TrackList_AdjustWindows(false);
    if (UpdateArrange) UpdateArrange();

    if (createdNew) *createdNew = true;
    return GetTrack(nullptr, n);
}

// ---------------------------------------------------------------------------
// Detect Cartridge plugin (add temporarily, then remove)
// ---------------------------------------------------------------------------
static const char* kCartridgeNames[] = {
    "Cartridge (DsgDnB)",
    "Cartridge",
    "VST3: Cartridge (DsgDnB)",
    "VST3: Cartridge",
    "VST: Cartridge (DsgDnB)",
    "VST: Cartridge",
    "AU: Cartridge (DsgDnB)",
    "AU: Cartridge",
    nullptr
};

// --- Cartridge detection ---------------------------------------------------
// Real probe only: TrackFX_AddByName under PreventUIRefresh (may create a temp track).
static bool DetectCartridgeInstalled()
{
    if (!TrackFX_AddByName || !TrackFX_Delete || !CountTracks || !GetTrack || !InsertTrackAtIndex)
        return false;

    if (PreventUIRefresh) PreventUIRefresh(1);

    bool created = false;
    MediaTrack* tr = nullptr;
    int n = CountTracks(nullptr);
    if (n <= 0)
    {
        InsertTrackAtIndex(0, true);
        tr = GetTrack(nullptr, 0);
        created = true;
    }
    else
    {
        tr = GetTrack(nullptr, 0);
    }

    bool found = false;
    if (tr)
    {
        for (int i = 0; kCartridgeNames[i]; ++i)
        {
            int fx = TrackFX_AddByName(tr, kCartridgeNames[i], false, 1);
            if (fx >= 0)
            {
                found = true;
                TrackFX_Delete(tr, fx);
                break;
            }
        }
    }

    if (created && tr && DeleteTrack)
        DeleteTrack(tr);

    if (PreventUIRefresh) PreventUIRefresh(-1);
    return found;
}

// ---------------------------------------------------------------------------
// Load sample into Cartridge
// Based on: DSG_Cartridge create track from Media Explorer
// (DsgDnB Cartridge script - pending_load.txt + "Load Trigger" param toggle)
// ---------------------------------------------------------------------------
static void CartridgeTriggerLoad(MediaTrack* track, int fx_idx, const char* sample_path)
{
    if (!track || !sample_path || !sample_path[0]) return;

    char dir[1024] = {};
    if (!Platform_GetCartridgeDir(dir, sizeof(dir)))
    {
        DebugLog("[Drop To Sampler] Cartridge: pending_load directory unknown on this platform\n");
        return;
    }

    if (RecursiveCreateDirectory)
        RecursiveCreateDirectory(dir, 0);

    char pending[1100];
#ifdef _WIN32
    snprintf(pending, sizeof(pending), "%s\\pending_load.txt", dir);
#else
    snprintf(pending, sizeof(pending), "%s/pending_load.txt", dir);
#endif

    FILE* f = Platform_FopenUtf8(pending, "wb");
    if (f)
    {
        fputs(sample_path, f); // sample_path is UTF-8
        fclose(f);
    }
    else
    {
        DebugLog("[Drop To Sampler] Cartridge: failed to write pending_load.txt\n");
        return;
    }

    if (!TrackFX_GetNumParams || !TrackFX_GetParamName || !TrackFX_GetParam || !TrackFX_SetParam)
        return;

    int nparams = TrackFX_GetNumParams(track, fx_idx);
    for (int i = 0; i < nparams; ++i)
    {
        char name[256] = {};
        if (!TrackFX_GetParamName(track, fx_idx, i, name, sizeof(name)))
            continue;
        if (strcmp(name, "Load Trigger") == 0)
        {
            double val = TrackFX_GetParam(track, fx_idx, i, nullptr, nullptr);
            TrackFX_SetParam(track, fx_idx, i, val < 0.5 ? 1.0 : 0.0);
            DebugLog("[Drop To Sampler] Cartridge: Load Trigger toggled\n");
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Add sampler + load file
// ---------------------------------------------------------------------------

static bool FxNameIsRS5k(const char* name)
{
    if (!name) return false;
    // Match common display names
    if (strstr(name, "ReaSamplOmatic") || strstr(name, "ReaSamplomatic") || strstr(name, "RS5K") || strstr(name, "RS5k"))
        return true;
    return false;
}

static bool FxNameIsCartridge(const char* name)
{
    if (!name) return false;
    return strstr(name, "Cartridge") != nullptr;
}

static bool FxNameMatchesSamplerType(const char* name, int samplerType)
{
    if (samplerType == Sampler_Cartridge)
        return FxNameIsCartridge(name);
    return FxNameIsRS5k(name);
}

// Returns FX index or -1. If outCount given, writes how many matching FX on track.
static int FindSamplerFx(MediaTrack* track, int samplerType, int* outCount)
{
    if (outCount) *outCount = 0;
    if (!track || !TrackFX_GetCount || !TrackFX_GetFXName) return -1;

    int n = TrackFX_GetCount(track);
    int first = -1;
    int cnt = 0;
    for (int i = 0; i < n; ++i)
    {
        char name[256] = {};
        TrackFX_GetFXName(track, i, name, sizeof(name));
        if (!FxNameMatchesSamplerType(name, samplerType))
            continue;
        if (first < 0) first = i;
        ++cnt;
    }
    if (outCount) *outCount = cnt;
    return first;
}

// REAPER TCP FX inserts (optional empty slots), GetThingFromPoint examples:
//   "tcp.fxlist 0"        - UI slot 0 empty (no fx:) -> not a real FX
//   "tcp.fxlist 0 fx:0"   - UI slot 0 -> track FX index 0
//   "tcp.fxlist 2 fx:1"   - UI slot 2 -> track FX index 1 (empty slot in between)
// Use "fx:N" as the real TrackFX index. No "fx:" means empty slot.
static int ParseFxIndexFromHitInfo(const char* info)
{
    if (!info || !info[0]) return -1;

    const char* fx = strstr(info, "fx:");
    if (fx)
    {
        fx += 3;
        if (*fx >= '0' && *fx <= '9')
            return atoi(fx);
    }
    return -1;
}

static int HitTestSamplerSlot(int screenX, int screenY, MediaTrack* expectedTrack, int samplerType)
{
    if (!GetThingFromPoint) return -1;
    char info[256] = {};
    MediaTrack* tr = GetThingFromPoint(screenX, screenY, info, sizeof(info));
    if (expectedTrack && tr && tr != expectedTrack)
        return -1;

    if (!strstr(info, "tcp.fxlist") && !strstr(info, "mcp.fxlist"))
        return -1;

    int fx = ParseFxIndexFromHitInfo(info);
    if (fx < 0)
    {
        DebugLog("[Drop To Sampler] FX list slot empty (no fx:N) - will add\n");
        return -1;
    }
    if (!tr || !TrackFX_GetFXName) return -1;

    char name[256] = {};
    TrackFX_GetFXName(tr, fx, name, sizeof(name));
    if (!FxNameMatchesSamplerType(name, samplerType))
    {
        if (g_debug)
        {
            char buf[512];
            snprintf(buf, sizeof(buf), "[Drop To Sampler] FX #%d is '%s', not target sampler - will add\n", fx, name);
            DebugLog(buf);
        }
        return -1;
    }

    if (g_debug)
    {
        char buf[512];
        snprintf(buf, sizeof(buf), "[Drop To Sampler] replace FX #%d (%s) info=%s\n", fx, name, info);
        DebugLog(buf);
    }
    return fx;
}

static void LoadSampleIntoExistingFx(MediaTrack* track, int fx, int samplerType, const char* file)
{
    if (!track || fx < 0 || !file) return;
    if (samplerType == Sampler_Cartridge)
        CartridgeTriggerLoad(track, fx, file);
    else if (TrackFX_SetNamedConfigParm)
    {
        TrackFX_SetNamedConfigParm(track, fx, "FILE0", file);
        TrackFX_SetNamedConfigParm(track, fx, "DONE", "");
    }
}


// Find FX param index by name (substring match, case-sensitive as REAPER reports it).
static int FindFxParamByName(MediaTrack* track, int fx, const char* needle)
{
    if (!track || fx < 0 || !needle || !TrackFX_GetNumParams || !TrackFX_GetParamName)
        return -1;
    int n = TrackFX_GetNumParams(track, fx);
    for (int i = 0; i < n; ++i)
    {
        char name[256] = {};
        if (!TrackFX_GetParamName(track, fx, i, name, sizeof(name)))
            continue;
        if (strstr(name, needle))
            return i;
    }
    return -1;
}

// Chromatic mapping: one MIDI key per sampler. Base C4 = 60.
// RS5k: Note start/end (3/4), Pitch start (5)=0.5 original pitch.
// Cartridge: find "MIDI Note Min/Max" and "Root Note" by name (indices vary).
static void ApplyChromaticNote(MediaTrack* track, int fx, int samplerType, int note)
{
    if (!track || fx < 0) return;
    if (note < 0) note = 0;
    if (note > 127) note = 127;

    if (!TrackFX_SetParamNormalized) return;

    if (samplerType == Sampler_Cartridge)
    {
        double n127 = (double)note / 127.0;
        // Root Note range measured as A0(21)=0 .. C8(108)=1
        double root = (double)(note - 21) / 87.0;
        if (root < 0.0) root = 0.0;
        if (root > 1.0) root = 1.0;

        int pMin = FindFxParamByName(track, fx, "MIDI Note Min");
        int pMax = FindFxParamByName(track, fx, "MIDI Note Max");
        int pRoot = FindFxParamByName(track, fx, "Root Note");

        // Fallback to previously observed indices if names not found
        if (pMin < 0) pMin = 51;
        if (pMax < 0) pMax = 52;
        if (pRoot < 0) pRoot = 47;

        TrackFX_SetParamNormalized(track, fx, pMin, n127);
        TrackFX_SetParamNormalized(track, fx, pMax, n127);
        TrackFX_SetParamNormalized(track, fx, pRoot, root);

        if (g_debug)
        {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "[Drop To Sampler] Cartridge chromatic note %d (min=%d max=%d root=%d)\n",
                note, pMin, pMax, pRoot);
            DebugLog(buf);
        }
    }
    else
    {
        double n127 = (double)note / 127.0;
        TrackFX_SetParamNormalized(track, fx, 3, n127); // Note start
        TrackFX_SetParamNormalized(track, fx, 4, n127); // Note end
        TrackFX_SetParamNormalized(track, fx, 5, 0.5);  // Pitch start = original
        if (g_debug)
        {
            char buf[128];
            snprintf(buf, sizeof(buf), "[Drop To Sampler] RS5k chromatic note %d on FX #%d\n", note, fx);
            DebugLog(buf);
        }
    }
}


static bool GetSamplerNoteRange(MediaTrack* track, int fx, int samplerType, int* outStart, int* outEnd)
{
    if (!track || fx < 0 || !TrackFX_GetParamNormalized) return false;
    double a = 0, b = 0;
    if (samplerType == Sampler_Cartridge)
    {
        int pMin = FindFxParamByName(track, fx, "MIDI Note Min");
        int pMax = FindFxParamByName(track, fx, "MIDI Note Max");
        if (pMin < 0) pMin = 51;
        if (pMax < 0) pMax = 52;
        a = TrackFX_GetParamNormalized(track, fx, pMin);
        b = TrackFX_GetParamNormalized(track, fx, pMax);
    }
    else
    {
        a = TrackFX_GetParamNormalized(track, fx, 3);
        b = TrackFX_GetParamNormalized(track, fx, 4);
    }
    int ns = (int)(a * 127.0 + 0.5);
    int ne = (int)(b * 127.0 + 0.5);
    if (ns < 0) ns = 0;
    if (ne < 0) ne = 0;
    if (ns > 127) ns = 127;
    if (ne > 127) ne = 127;
    if (outStart) *outStart = ns;
    if (outEnd) *outEnd = ne;
    return true;
}

// Next free chromatic key. Only single-key samplers (start==end) occupy a step.
// Full-range layers do not bump the base - first chromatic stays at C4=60.
static int NextChromaticNote(MediaTrack* track, int samplerType)
{
    int next = 60;
    if (!track || !TrackFX_GetCount || !TrackFX_GetFXName)
        return next;

    int n = TrackFX_GetCount(track);
    for (int i = 0; i < n; ++i)
    {
        char name[256] = {};
        TrackFX_GetFXName(track, i, name, sizeof(name));
        if (!FxNameMatchesSamplerType(name, samplerType))
            continue;

        int ns = 0, ne = 0;
        if (!GetSamplerNoteRange(track, i, samplerType, &ns, &ne))
            continue;
        if (ns == ne)
        {
            if (ns + 1 > next)
                next = ns + 1;
        }
    }
    if (next > 127) next = 127;
    return next;
}

// Re-map all matching samplers on the track to consecutive keys from C4 (60).
static void RemapTrackSamplersChromatic(MediaTrack* track, int samplerType)
{
    if (!track || !TrackFX_GetCount || !TrackFX_GetFXName) return;
    int note = 60;
    int n = TrackFX_GetCount(track);
    for (int i = 0; i < n; ++i)
    {
        char name[256] = {};
        TrackFX_GetFXName(track, i, name, sizeof(name));
        if (!FxNameMatchesSamplerType(name, samplerType))
            continue;
        ApplyChromaticNote(track, i, samplerType, note);
        if (note < 127) ++note;
    }
    DebugLog("[Drop To Sampler] remapped existing samplers to chromatic from C4\n");
}


static int AddFxByNames(MediaTrack* track, const char* const* names)
{
    if (!TrackFX_AddByName) return -1;
    // instantiate <= -1000 selects insert position: -1000 = first, -1000-N = after N existing FX
    // Append at end of chain so order matches normal "add FX" behaviour.
    int pos = -1000;
    if (TrackFX_GetCount)
        pos = -1000 - TrackFX_GetCount(track);
    for (int i = 0; names[i]; ++i)
    {
        int fx = TrackFX_AddByName(track, names[i], false, pos);
        if (fx >= 0) return fx;
    }
    return -1;
}

static void AddRS5kWithFile(MediaTrack* track, const char* file, bool forceHide, bool isLastOnTrack, int chromaticNote)
{
    static const char* names[] = {
        "ReaSamplOmatic5000",
        "ReaSamplomatic5000",
        "RS5K",
        nullptr
    };
    int fx = AddFxByNames(track, names);
    if (fx < 0)
    {
        DebugLog("[Drop To Sampler] RS5k AddByName failed\n");
        return;
    }
    if (TrackFX_SetNamedConfigParm)
    {
        TrackFX_SetNamedConfigParm(track, fx, "FILE0", file);
        TrackFX_SetNamedConfigParm(track, fx, "DONE", "");
    }
    if (chromaticNote >= 0)
        ApplyChromaticNote(track, fx, Sampler_RS5k, chromaticNote);
    ApplyShowSampler(track, fx, forceHide, isLastOnTrack);
    if (g_debug)
    {
        char buf[1024];
        snprintf(buf, sizeof(buf), "[Drop To Sampler] RS5k #%d FILE0=%s\n", fx, file);
        DebugLog(buf);
    }
}

static void AddCartridgeWithFile(MediaTrack* track, const char* file, bool forceHide, bool isLastOnTrack, int chromaticNote)
{
    int fx = AddFxByNames(track, kCartridgeNames);
    if (fx < 0)
    {
        DebugLog("[Drop To Sampler] Cartridge AddByName failed\n");
        return;
    }
    CartridgeTriggerLoad(track, fx, file);
    if (chromaticNote >= 0)
        ApplyChromaticNote(track, fx, Sampler_Cartridge, chromaticNote);
    ApplyShowSampler(track, fx, forceHide, isLastOnTrack);
    if (g_debug)
    {
        char buf[1024];
        snprintf(buf, sizeof(buf), "[Drop To Sampler] Cartridge #%d file=%s\n", fx, file);
        DebugLog(buf);
    }
}

static HWND PromptParent()
{
    return GetMainHwnd ? GetMainHwnd() : nullptr;
}

static int PromptYesNoCancel(const char* text)
{
    return MessageBox(PromptParent(), text, "Drop To Sampler",
        MB_YESNOCANCEL | MB_ICONQUESTION | MB_TOPMOST);
}

static int PromptSamplerChoice()
{
    int r = PromptYesNoCancel(
        "Use Cartridge (DsgDnB)?\n\n"
        "Yes = Cartridge (DsgDnB)\n"
        "No = ReaSamplOmatic5000 (Cockos)");
    if (r == IDYES) return Sampler_Cartridge;
    if (r == IDNO) return Sampler_RS5k;
    return -1;
}

static int PromptMultiFilesChoice()
{
    int r = PromptYesNoCancel(
        "Place multiple media on one track?\n\n"
        "Yes = Insert multiple media on one track\n"
        "No = Create new track for each sampler");
    if (r == IDYES) return MultiFiles_OneTrack;
    if (r == IDNO) return MultiFiles_MultiTracks;
    return -1;
}

static int PromptMultiSamplersChoice()
{
    int r = PromptYesNoCancel(
        "Place media in samplers in chromatic order?\n\n"
        "Yes = One sample per note starting from C4 (including existing samplers)\n"
        "No = Default note range for each sampler (Layers)");
    if (r == IDYES) return MultiSamplers_Chromatic;
    if (r == IDNO) return MultiSamplers_Layers;
    return -1;
}

// resolvedSamplerMode: if >= 0, use it (no prompt). If -1, use g_samplerMode and prompt when AlwaysPrompt.
static void AddSamplerWithFile(MediaTrack* track, const char* file, bool forceHide, bool isLastOnTrack,
                                bool allowReplace, int screenX, int screenY, bool chromatic,
                                int resolvedSamplerMode)
{
    if (!track || !file || !file[0]) return;

    int mode = resolvedSamplerMode;
    if (mode < 0)
    {
        mode = g_samplerMode;
        if (mode == Sampler_AlwaysPrompt)
        {
            mode = PromptSamplerChoice();
            if (mode < 0) return;
        }
    }

    int samplerType = (mode == Sampler_Cartridge) ? Sampler_Cartridge : Sampler_RS5k;

    int replaceFx = -1;
    if (allowReplace && g_dropExistingMode != Existing_Add)
    {
        if (g_dropExistingMode == Existing_ReplaceOnSlot)
        {
            replaceFx = HitTestSamplerSlot(screenX, screenY, track, samplerType);
        }
        else if (g_dropExistingMode == Existing_Replace)
        {
            int cnt = 0;
            int first = FindSamplerFx(track, samplerType, &cnt);
            if (cnt == 1)
                replaceFx = first;
        }
    }

    if (replaceFx >= 0)
    {
        if (samplerType == Sampler_Cartridge && !g_cartridgeAvailable)
            g_cartridgeAvailable = DetectCartridgeInstalled();

        LoadSampleIntoExistingFx(track, replaceFx, samplerType, file);
        ApplyShowSampler(track, replaceFx, forceHide, isLastOnTrack);
        if (g_debug)
        {
            char buf[1024];
            snprintf(buf, sizeof(buf), "[Drop To Sampler] replaced FX #%d with %s\n", replaceFx, file);
            DebugLog(buf);
        }
        return;
    }

    int chromNote = -1;
    if (chromatic)
        chromNote = NextChromaticNote(track, samplerType);

    if (samplerType == Sampler_Cartridge)
    {
        if (!g_cartridgeAvailable)
            g_cartridgeAvailable = DetectCartridgeInstalled();
        if (!g_cartridgeAvailable)
        {
            DebugLog("[Drop To Sampler] Cartridge not available, falling back to RS5k\n");
            AddRS5kWithFile(track, file, forceHide, isLastOnTrack, chromNote);
            return;
        }
        AddCartridgeWithFile(track, file, forceHide, isLastOnTrack, chromNote);
    }
    else
    {
        AddRS5kWithFile(track, file, forceHide, isLastOnTrack, chromNote);
    }
}


// Name track after sample only when: empty name, no sampler yet, single sample on this track
static bool TrackNameIsEmpty(MediaTrack* track)
{
    // GetTrackName returns "Track N" for unnamed tracks — not empty.
    // Only P_NAME is empty when the track has no custom name.
    if (!track || !GetSetMediaTrackInfo_String) return false;
    char name[512] = {};
    GetSetMediaTrackInfo_String(track, "P_NAME", name, false);
    for (char* p = name; *p; ++p)
    {
        if (*p != ' ' && *p != '\t')
            return false;
    }
    return true;
}

static void SampleBaseNameFromPath(const char* path, char* out, int outSz)
{
    if (!out || outSz <= 0) return;
    out[0] = 0;
    if (!path || !path[0]) return;

    const char* base = path;
    for (const char* p = path; *p; ++p)
    {
        if (*p == '\\' || *p == '/')
            base = p + 1;
    }
    // copy without extension
    int j = 0;
    for (const char* p = base; *p && j < outSz - 1; ++p)
    {
        if (*p == '.')
        {
            // keep dots in the middle; only strip final extension
            const char* rest = p + 1;
            bool moreDot = false;
            for (const char* q = rest; *q; ++q)
                if (*q == '.') { moreDot = true; break; }
            if (!moreDot)
                break; // this is the last extension
        }
        out[j++] = *p;
    }
    out[j] = 0;
}

static void MaybeNameTrackAfterSample(MediaTrack* track, const char* file,
    bool hadSamplerBefore, bool multipleSamplesOnThisTrack)
{
    if (!track || !file || !file[0]) return;
    if (multipleSamplesOnThisTrack)
    {
        DebugLog("[Drop To Sampler] skip rename: multiple samples on one track\n");
        return;
    }
    if (hadSamplerBefore)
    {
        DebugLog("[Drop To Sampler] skip rename: sampler already on track\n");
        return;
    }
    if (!GetSetMediaTrackInfo_String)
    {
        DebugLog("[Drop To Sampler] skip rename: GetSetMediaTrackInfo_String missing\n");
        return;
    }
    if (!TrackNameIsEmpty(track))
    {
        DebugLog("[Drop To Sampler] skip rename: track already has P_NAME\n");
        return;
    }

    char base[512];
    SampleBaseNameFromPath(file, base, (int)sizeof(base));
    if (!base[0]) return;

    GetSetMediaTrackInfo_String(track, "P_NAME", base, true);
    if (g_debug)
    {
        char buf[640];
        snprintf(buf, sizeof(buf), "[Drop To Sampler] track renamed to \"%s\"\n", base);
        DebugLog(buf);
    }
}

static void SelectOnlyTrack(MediaTrack* track)
{
    if (!track || !CountTracks || !GetTrack || !SetTrackSelected) return;
    int n = CountTracks(nullptr);
    for (int i = 0; i < n; ++i)
    {
        MediaTrack* t = GetTrack(nullptr, i);
        if (t) SetTrackSelected(t, t == track);
    }
}

// 0-based track index, or -1
static int GetTrackIndex0(MediaTrack* track)
{
    if (!track) return -1;
    if (GetMediaTrackInfo_Value)
    {
        double v = GetMediaTrackInfo_Value(track, "IP_TRACKNUMBER");
        // 1-based track number; 0 = not found; -1 = master
        if (v >= 1.0)
            return (int)v - 1;
    }
    if (CountTracks && GetTrack)
    {
        int n = CountTracks(nullptr);
        for (int i = 0; i < n; ++i)
            if (GetTrack(nullptr, i) == track)
                return i;
    }
    return -1;
}

// Insert a new track at 0-based index (existing tracks at/after idx shift down)
static MediaTrack* InsertTrackAt(int idx0)
{
    if (!CountTracks || !InsertTrackAtIndex || !GetTrack) return nullptr;
    int n = CountTracks(nullptr);
    if (idx0 < 0) idx0 = 0;
    if (idx0 > n) idx0 = n;
    InsertTrackAtIndex(idx0, true);
    if (TrackList_AdjustWindows) TrackList_AdjustWindows(false);
    if (UpdateArrange) UpdateArrange();
    return GetTrack(nullptr, idx0);
}

// ---------------------------------------------------------------------------
// Drop
// ---------------------------------------------------------------------------
void Core_Drop(const char* const* inFiles, int inCount, int screenX, int screenY, const DropMods& mods)
{
    if (!inFiles || inCount <= 0) return;

    // Same cap as the original Windows build
    const char* files[64];
    UINT nFiles = 0;
    for (int i = 0; i < inCount && nFiles < 64; ++i)
    {
        if (inFiles[i] && inFiles[i][0])
            files[nFiles++] = inFiles[i];
    }
    if (nFiles == 0) return;

    bool createdNew = false;
    MediaTrack* track = ResolveTrackAtPoint(screenX, screenY, &createdNew);
    if (!track)
    {
        DebugLog("[Drop To Sampler] could not resolve track\n");
        return;
    }

    // Resolve prompts in order:
    // 1) Multiple files (if needed)
    // 2) Sampler type (if needed)
    // 3) Multiple samplers Layers/Chromatic only when stacking on one track
    int multiFilesMode = g_multiFilesMode;
    if (nFiles > 1 && multiFilesMode == MultiFiles_AlwaysPrompt)
    {
        multiFilesMode = PromptMultiFilesChoice();
        if (multiFilesMode < 0)
            return;
    }
    else if (nFiles > 1 && multiFilesMode != MultiFiles_AlwaysPrompt)
    {
        // Shift toggles one-track <-> multi-track (not used with Always prompt)
        if (mods.shift)
        {
            if (multiFilesMode == MultiFiles_OneTrack)
                multiFilesMode = MultiFiles_MultiTracks;
            else if (multiFilesMode == MultiFiles_MultiTracks)
                multiFilesMode = MultiFiles_OneTrack;
            DebugLog(multiFilesMode == MultiFiles_MultiTracks
                ? "[Drop To Sampler] Shift: multiple tracks\n"
                : "[Drop To Sampler] Shift: one track\n");
        }
    }

    const bool ctrlDown = mods.ctrl;
    const bool altDown = mods.alt;

    int resolvedSampler = g_samplerMode;
    if (resolvedSampler == Sampler_AlwaysPrompt)
    {
        resolvedSampler = PromptSamplerChoice();
        if (resolvedSampler < 0)
            return;
    }
    int samplerType = (resolvedSampler == Sampler_Cartridge) ? Sampler_Cartridge : Sampler_RS5k;

    // Existing samplers of chosen type on the drop target track
    int existingOnTrack = 0;
    FindSamplerFx(track, samplerType, &existingOnTrack);

    // Effective "If sampler exists" mode:
    // Replace-on-slot: Alt ignored (slot -> replace, else add).
    // Add/Replace: Alt toggles Add <-> Replace.
    int existingMode = g_existingMode;
    if (altDown && g_existingMode != Existing_ReplaceOnSlot)
    {
        if (existingMode == Existing_Add)
            existingMode = Existing_Replace;
        else
            existingMode = Existing_Add;
        DebugLog(existingMode == Existing_Add
            ? "[Drop To Sampler] " DTS_MOD_ALT_NAME ": Add\n"
            : "[Drop To Sampler] " DTS_MOD_ALT_NAME ": Replace\n");
    }


    // Single file that will REPLACE -> no Layers/Chromatic prompt
    bool willReplaceExisting = false;
    if (nFiles == 1)
    {
        if (existingMode == Existing_ReplaceOnSlot)
        {
            if (HitTestSamplerSlot(screenX, screenY, track, samplerType) >= 0)
                willReplaceExisting = true;
        }
        else if (existingMode == Existing_Replace)
        {
            int cnt = 0;
            FindSamplerFx(track, samplerType, &cnt);
            if (cnt == 1)
                willReplaceExisting = true;
        }
    }

    int multiSamplersMode = g_multiSamplersMode;
    // Ctrl toggles Layers <-> Chromatic when not Always prompt
    if (ctrlDown && multiSamplersMode != MultiSamplers_AlwaysPrompt)
    {
        if (multiSamplersMode == MultiSamplers_Layers)
            multiSamplersMode = MultiSamplers_Chromatic;
        else if (multiSamplersMode == MultiSamplers_Chromatic)
            multiSamplersMode = MultiSamplers_Layers;
        DebugLog(multiSamplersMode == MultiSamplers_Chromatic
            ? "[Drop To Sampler] " DTS_MOD_CTRL_NAME ": Chromatic\n"
            : "[Drop To Sampler] " DTS_MOD_CTRL_NAME ": Layers\n");
    }

    // Multi-track layout only applies when dropping several files; a single file can still use Chromatic
    if ((multiFilesMode == MultiFiles_MultiTracks && nFiles > 1) || willReplaceExisting)
    {
        multiSamplersMode = MultiSamplers_Layers;
    }
    else
    {
        bool willStack = (nFiles > 1) || (nFiles == 1 && existingOnTrack >= 1);
        if (multiSamplersMode == MultiSamplers_AlwaysPrompt)
        {
            if (willStack)
            {
                multiSamplersMode = PromptMultiSamplersChoice();
                if (multiSamplersMode < 0)
                    return;
            }
            else
            {
                multiSamplersMode = MultiSamplers_Layers;
            }
        }
    }

    // Chromatic only when stacking; replace / empty track -> full range
    bool chromatic = false;
    if (!willReplaceExisting && multiSamplersMode == MultiSamplers_Chromatic)
    {
        if (nFiles > 1 && multiFilesMode == MultiFiles_OneTrack)
            chromatic = true;
        else if (nFiles == 1 && existingOnTrack >= 1)
            chromatic = true;
    }

    g_dropExistingMode = existingMode;

    // Chromatic: rebuild existing samplers on the target track from C4, then new ones continue
    if (chromatic && !(multiFilesMode == MultiFiles_MultiTracks && nFiles > 1))
        RemapTrackSamplersChromatic(track, samplerType);

    if (Undo_BeginBlock) Undo_BeginBlock();

    MediaTrack* lastNewTrack = nullptr;
    const bool allowReplace = (nFiles == 1);

    if (g_debug && GetThingFromPoint)
    {
        char info[256] = {};
        GetThingFromPoint(screenX, screenY, info, sizeof(info));
        char buf[512];
        snprintf(buf, sizeof(buf), "[Drop To Sampler] drop hit info=%s (x=%d y=%d)\n", info, screenX, screenY);
        DebugLog(buf);
    }

    if (multiFilesMode == MultiFiles_MultiTracks && nFiles > 1)
    {
        // Drop target is only an anchor: insert N new tracks AFTER it (or at end if empty area).
        // Example: drop 3 files on "2-Drums" with "3-Synth" below ->
        //   2-Drums (unchanged), 3-SampleA, 4-SampleB, 5-SampleC, 6-Synth
        int insertAt;
        if (createdNew)
        {
            // Empty TCP area: ResolveTrackAtPoint already appended one track for the drop.
            // Reuse it as the first new track, then append the rest at end.
            insertAt = GetTrackIndex0(track);
            if (insertAt < 0)
                insertAt = CountTracks ? CountTracks(nullptr) - 1 : 0;

            for (UINT i = 0; i < nFiles; ++i)
            {
                MediaTrack* tr = nullptr;
                if (i == 0)
                    tr = track;
                else
                    tr = InsertTrackAt(insertAt + (int)i);

                if (!tr) continue;
                ApplyRecArmForNewTrack(tr);
                lastNewTrack = tr;
                AddSamplerWithFile(tr, files[i], true, true, false, screenX, screenY, chromatic, resolvedSampler);
                MaybeNameTrackAfterSample(tr, files[i], false, false);
            }
        }
        else
        {
            // Dropped on an existing track: do not put samples on it; insert after it.
            int anchor = GetTrackIndex0(track);
            if (anchor < 0)
                anchor = (CountTracks ? CountTracks(nullptr) : 1) - 1;
            insertAt = anchor + 1;

            for (UINT i = 0; i < nFiles; ++i)
            {
                MediaTrack* tr = InsertTrackAt(insertAt + (int)i);
                if (!tr) continue;
                ApplyRecArmForNewTrack(tr);
                lastNewTrack = tr;
                AddSamplerWithFile(tr, files[i], true, true, false, screenX, screenY, chromatic, resolvedSampler);
                MaybeNameTrackAfterSample(tr, files[i], false, false);
            }
        }
    }
    else
    {
        if (createdNew)
        {
            ApplyRecArmForNewTrack(track);
            lastNewTrack = track;
        }
        // Multiple samples stacked on one track -> do not auto-name.
        // Single sample + empty name + no sampler before -> name from file.
        const bool multiOnOne = (nFiles > 1);
        const bool hadSampler = (existingOnTrack >= 1);
        for (UINT i = 0; i < nFiles; ++i)
        {
            bool isLast = (i + 1 == nFiles);
            AddSamplerWithFile(track, files[i], false, isLast, allowReplace, screenX, screenY, chromatic, resolvedSampler);
        }
        if (!multiOnOne && nFiles == 1)
            MaybeNameTrackAfterSample(track, files[0], hadSampler, false);
    }

    if (lastNewTrack)
        SelectOnlyTrack(lastNewTrack);

    if (Undo_EndBlock)
        Undo_EndBlock("Insert sampler from TCP drop", -1);
}

// ---------------------------------------------------------------------------
// Drag feedback: status line under REAPER panels (Help_Set)
// ---------------------------------------------------------------------------
static void SetDropHelp(const char* msg)
{
    if (!Help_Set || !msg) return;
    // Temporary help, re-asserted on every drag-over so REAPER keeps showing it
    Help_Set(msg, true);
}

void Core_DragEnd()
{
    if (Help_Set)
        Help_Set("", true);
}

// Preview what this drop would do (settings + modifiers + hover target)
int Core_DragOver(const DropMods& mods, int screenX, int screenY, int fileCount)
{
    if (!Help_Set) return DropVisual_Insert;
    if (fileCount < 1) fileCount = 1;

    const bool shift = mods.shift;
    const bool ctrl = mods.ctrl;
    const bool alt = mods.alt;

    const bool multiFiles = (fileCount > 1);

    int multiFilesMode = g_multiFilesMode;
    if (multiFiles && multiFilesMode != MultiFiles_AlwaysPrompt && shift)
    {
        if (multiFilesMode == MultiFiles_OneTrack)
            multiFilesMode = MultiFiles_MultiTracks;
        else if (multiFilesMode == MultiFiles_MultiTracks)
            multiFilesMode = MultiFiles_OneTrack;
    }

    // Replace-on-slot: Alt ignored. Slot hit -> replace, else -> Add.
    // Add/Replace: Alt toggles. Multiple media: always Add.
    int existingMode = g_existingMode;
    if (multiFiles)
        existingMode = Existing_Add;
    else if (g_existingMode == Existing_ReplaceOnSlot)
        existingMode = Existing_ReplaceOnSlot;
    else if (alt)
        existingMode = (g_existingMode == Existing_Add) ? Existing_Replace : Existing_Add;

    int multiSamplersMode = g_multiSamplersMode;
    if (ctrl && multiSamplersMode != MultiSamplers_AlwaysPrompt)
    {
        if (multiSamplersMode == MultiSamplers_Layers)
            multiSamplersMode = MultiSamplers_Chromatic;
        else if (multiSamplersMode == MultiSamplers_Chromatic)
            multiSamplersMode = MultiSamplers_Layers;
    }

    int samplerType = (g_samplerMode == Sampler_Cartridge) ? Sampler_Cartridge : Sampler_RS5k;

    MediaTrack* track = nullptr;
    char info[256] = {};
    if (GetThingFromPoint)
        track = GetThingFromPoint(screenX, screenY, info, sizeof(info));

    bool overFxSlot = (strstr(info, "tcp.fxlist") != nullptr || strstr(info, "mcp.fxlist") != nullptr);
    bool slotHasFx = overFxSlot && (strstr(info, "fx:") != nullptr);
    bool onTrackPanel = (strstr(info, "tcp") == info || strstr(info, "mcp") == info);

    auto layerLabel = [&]() -> const char* {
        if (multiSamplersMode == MultiSamplers_AlwaysPrompt)
            return "Ask: Layers/Chromatic";
        if (multiSamplersMode == MultiSamplers_Chromatic)
            return ctrl ? "Chromatic" : "Chromatic (" DTS_MOD_CTRL_NAME " toggles)";
        return ctrl ? "Layers" : "Layers (" DTS_MOD_CTRL_NAME " toggles)";
    };

    auto placeLabel = [&]() -> const char* {
        if (multiFilesMode == MultiFiles_AlwaysPrompt)
            return "Ask: one / multiple tracks";
        if (multiFilesMode == MultiFiles_MultiTracks)
            return shift ? "On multiple tracks" : "On multiple tracks (Shift toggles)";
        return shift ? "On one track" : "On one track (Shift toggles)";
    };

    // --- Replace on FX slot ---
    if (!multiFiles && existingMode == Existing_ReplaceOnSlot && slotHasFx && track)
    {
        int fx = ParseFxIndexFromHitInfo(info);
        if (fx >= 0 && TrackFX_GetFXName)
        {
            char name[256] = {};
            TrackFX_GetFXName(track, fx, name, sizeof(name));
            if (FxNameMatchesSamplerType(name, samplerType)
                || (g_samplerMode == Sampler_AlwaysPrompt
                    && (FxNameIsRS5k(name) || FxNameIsCartridge(name))))
            {
                SetDropHelp("Replace media");
                return DropVisual_Replace;
            }
        }
    }

    // --- Replace (settings Replace, or Alt from Add) ---
    if (!multiFiles && existingMode == Existing_Replace && track && onTrackPanel)
    {
        int cnt = 0;
        FindSamplerFx(track, samplerType, &cnt);
        if (cnt == 1)
        {
            // "(Alt toggles)" only when key not held and base mode is Replace or Add (toggle available)
            if (!alt && g_existingMode != Existing_ReplaceOnSlot)
                SetDropHelp("Replace media (" DTS_MOD_ALT_NAME " toggles)");
            else
                SetDropHelp("Replace media");
            return DropVisual_Replace;
        }
    }

    int existingOnTrack = 0;
    if (track && onTrackPanel)
        FindSamplerFx(track, samplerType, &existingOnTrack);

    // --- Multiple media ---
    if (multiFiles)
    {
        char buf[220];
        if (multiFilesMode == MultiFiles_MultiTracks)
            snprintf(buf, sizeof(buf), "New samplers (%d) \xC2\xB7 %s", fileCount, placeLabel());
        else
            snprintf(buf, sizeof(buf), "New samplers (%d) \xC2\xB7 %s \xC2\xB7 %s",
                fileCount, placeLabel(), layerLabel());
        SetDropHelp(buf);
        return DropVisual_Insert;
    }

    // --- Single media, Add path (also Replace-on-slot when not on slot) ---
    if (existingOnTrack >= 1)
    {
        char buf[180];
        snprintf(buf, sizeof(buf), "New sampler \xC2\xB7 %s", layerLabel());
        SetDropHelp(buf);
        return DropVisual_Insert;
    }

    SetDropHelp("New sampler");
    return DropVisual_Insert;
}

// ---------------------------------------------------------------------------
// Preferences page (Preferences -> Plug-ins -> Drop To Sampler)
// ---------------------------------------------------------------------------
static const char* HintForControlId(int id)
{
    switch (id)
    {
    case IDC_SAMPLER_COMBO:
    case IDC_LABEL_SAMPLER:
        return "Which sampler to insert. Cartridge by DsgDnB can be used if installed.";
    case IDC_RECARM_COMBO:
    case IDC_LABEL_RECARM:
        return "Set record-arm or auto record-arm for newly created tracks. Also sets input to All MIDI inputs if enabled.";
    case IDC_SHOW_COMBO:
    case IDC_LABEL_SHOW:
        return "Show floating FX window after insert. Default follows REAPER preferences.";
    case IDC_EXISTING_COMBO:
    case IDC_LABEL_EXISTING:
        return "Add or Replace (" DTS_MOD_ALT_NAME " toggles) sample when a sampler is already on the track. Replace ignored when multiple media dragged.";
    case IDC_MULTI_COMBO:
    case IDC_LABEL_MULTI:
        return "Where to place multiple dropped media. Hold Shift to toggle this behavior.";
    case IDC_MULTISAMP_COMBO:
    case IDC_LABEL_MULTISAMP:
        return "Layers: default note range. Chromatic: one note per sample starting from C4 (including existing samplers). " DTS_MOD_CTRL_NAME " to toggle.";
    case IDC_DEBUG_CHECK:
        return "Log drop details to the REAPER console.";
    default:
        return "";
    }
}

// REAPER Preferences window: help text control and Apply button IDs
static const int IDC_PREFS_HELP = 0x4eb;
static const int IDC_PREFS_APPLY_BTN = 0x478;
#define WM_PREFS_APPLY (WM_USER * 2)

static const char* g_shownPrefsHelp = nullptr;

static void UpdatePrefsHelp(HWND page)
{
    if (!IsWindowVisible(page))
    {
        g_shownPrefsHelp = nullptr;
        return;
    }
    HWND prefs = GetParent(page);
    if (!prefs) return;

    POINT pt;
    GetCursorPos(&pt);
    HWND child = WindowFromPoint(pt);
    // walk up to a direct child of page
    while (child && GetParent(child) != page && child != page)
        child = GetParent(child);

    const char* hint = "";
    if (child && child != page)
        hint = HintForControlId(CtrlIdOf(child));
    if (hint == g_shownPrefsHelp)
        return;
    g_shownPrefsHelp = hint;
    SetDlgItemText(prefs, IDC_PREFS_HELP, hint && hint[0] ? hint : "");
}

static void NotifyPrefsApplyEnabled(HWND page)
{
    HWND prefs = GetParent(page);
    if (prefs)
    {
        HWND apply = GetDlgItem(prefs, IDC_PREFS_APPLY_BTN);
        if (apply) EnableWindow(apply, TRUE);
    }
}

static void ComboAdd(HWND dlg, int id, const char* text)
{
    SendDlgItemMessage(dlg, id, CB_ADDSTRING, 0, (LPARAM)text);
}

static void PopulateSettingsControls(HWND hwnd)
{
    g_cartridgeAvailable = DetectCartridgeInstalled();
    if (!g_cartridgeAvailable && g_samplerMode != Sampler_RS5k)
        g_samplerMode = Sampler_RS5k;

    ComboAdd(hwnd, IDC_SAMPLER_COMBO, "ReaSamplOmatic5000 (Cockos)");
    if (g_cartridgeAvailable)
    {
        ComboAdd(hwnd, IDC_SAMPLER_COMBO, "Cartridge (DsgDnB)");
        ComboAdd(hwnd, IDC_SAMPLER_COMBO, "Always prompt");
    }
    SendDlgItemMessage(hwnd, IDC_SAMPLER_COMBO, CB_SETCURSEL, g_cartridgeAvailable ? g_samplerMode : 0, 0);

    ComboAdd(hwnd, IDC_RECARM_COMBO, "Default");
    ComboAdd(hwnd, IDC_RECARM_COMBO, "Rec-arm");
    ComboAdd(hwnd, IDC_RECARM_COMBO, "Auto rec-arm");
    SendDlgItemMessage(hwnd, IDC_RECARM_COMBO, CB_SETCURSEL, g_recArmMode, 0);

    ComboAdd(hwnd, IDC_SHOW_COMBO, "Default");
    ComboAdd(hwnd, IDC_SHOW_COMBO, "Yes");
    ComboAdd(hwnd, IDC_SHOW_COMBO, "No");
    SendDlgItemMessage(hwnd, IDC_SHOW_COMBO, CB_SETCURSEL, g_showSamplerMode, 0);

    ComboAdd(hwnd, IDC_EXISTING_COMBO, "Add");
    ComboAdd(hwnd, IDC_EXISTING_COMBO, "Replace");
    ComboAdd(hwnd, IDC_EXISTING_COMBO, "Replace on sampler FX slot");
    SendDlgItemMessage(hwnd, IDC_EXISTING_COMBO, CB_SETCURSEL, g_existingMode, 0);

    ComboAdd(hwnd, IDC_MULTI_COMBO, "On one track");
    ComboAdd(hwnd, IDC_MULTI_COMBO, "On multiple tracks");
    ComboAdd(hwnd, IDC_MULTI_COMBO, "Always prompt");
    SendDlgItemMessage(hwnd, IDC_MULTI_COMBO, CB_SETCURSEL, g_multiFilesMode, 0);

    ComboAdd(hwnd, IDC_MULTISAMP_COMBO, "Layers");
    ComboAdd(hwnd, IDC_MULTISAMP_COMBO, "Chromatic");
    ComboAdd(hwnd, IDC_MULTISAMP_COMBO, "Always prompt");
    SendDlgItemMessage(hwnd, IDC_MULTISAMP_COMBO, CB_SETCURSEL, g_multiSamplersMode, 0);

    CheckDlgButton(hwnd, IDC_DEBUG_CHECK, g_debug ? BST_CHECKED : BST_UNCHECKED);
}

static int ComboSel(HWND dlg, int id)
{
    return (int)SendDlgItemMessage(dlg, id, CB_GETCURSEL, 0, 0);
}

static void ApplySettingsFromControls(HWND hwnd)
{
    g_debug = IsDlgButtonChecked(hwnd, IDC_DEBUG_CHECK) == BST_CHECKED;

    int sel = ComboSel(hwnd, IDC_SAMPLER_COMBO);
    if (!g_cartridgeAvailable) g_samplerMode = Sampler_RS5k;
    else if (sel >= Sampler_RS5k && sel <= Sampler_AlwaysPrompt) g_samplerMode = sel;

    int ra = ComboSel(hwnd, IDC_RECARM_COMBO);
    if (ra >= RecArm_Default && ra <= RecArm_Auto) g_recArmMode = ra;

    int sh = ComboSel(hwnd, IDC_SHOW_COMBO);
    if (sh >= ShowSampler_Default && sh <= ShowSampler_No) g_showSamplerMode = sh;

    int ex = ComboSel(hwnd, IDC_EXISTING_COMBO);
    if (ex >= Existing_Add && ex <= Existing_ReplaceOnSlot) g_existingMode = ex;

    int mf = ComboSel(hwnd, IDC_MULTI_COMBO);
    if (mf >= MultiFiles_OneTrack && mf <= MultiFiles_AlwaysPrompt) g_multiFilesMode = mf;

    int ms = ComboSel(hwnd, IDC_MULTISAMP_COMBO);
    if (ms >= MultiSamplers_Layers && ms <= MultiSamplers_AlwaysPrompt) g_multiSamplersMode = ms;

    SaveSettings();
}

static INT_PTR CALLBACK PrefsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (msg)
    {
    case WM_INITDIALOG:
        PopulateSettingsControls(hwnd);
        return TRUE;

    case WM_PREFS_APPLY:
        ApplySettingsFromControls(hwnd);
        return TRUE;

    case WM_COMMAND:
        if (HIWORD(wParam) == CBN_SELCHANGE || HIWORD(wParam) == BN_CLICKED)
            NotifyPrefsApplyEnabled(hwnd);
        return TRUE;

    case WM_SETCURSOR:
        UpdatePrefsHelp(hwnd);
        break;

    case WM_MOUSEMOVE:
        UpdatePrefsHelp(hwnd);
        return TRUE;
    }
    return FALSE;
}

static HWND PrefsPageCreate(HWND parent)
{
    return CreateDialog(g_hInstance, MAKEINTRESOURCE(IDD_PREFS), parent, PrefsDlgProc);
}

static prefs_page_register_t g_prefsPage = {
    "drop_to_sampler",
    "Drop To Sampler",
    PrefsPageCreate,
    0x9a, // parent: Plug-ins
    "",
    0,
    nullptr,
    nullptr,
    {}
};

static void OpenSettingsWindow()
{
    // Our page lives inside REAPER Preferences (Plug-ins)
    if (ViewPrefs)
        ViewPrefs(0, "drop_to_sampler");
}

// ---------------------------------------------------------------------------
// Actions / menu
// ---------------------------------------------------------------------------
static bool HookCommand2(KbdSectionInfo* sec, int command, int val, int valhw, int relmode, HWND hwnd)
{
    (void)sec; (void)val; (void)valhw; (void)relmode; (void)hwnd;
    if (g_cmdSettings != 0 && command == g_cmdSettings)
    {
        OpenSettingsWindow();
        return true;
    }
    return false;
}

static void HookCustomMenu(const char* menuidstr, void* menuPtr, int flag)
{
    if (flag != 0 || !menuidstr || !menuPtr || g_cmdSettings <= 0)
        return;
    if (strcmp(menuidstr, "Main extensions") != 0)
        return;

    HMENU menu = (HMENU)menuPtr;
    InsertMenu(menu, GetMenuItemCount(menu), MF_BYPOSITION | MF_STRING, (UINT_PTR)g_cmdSettings, "Drop To Sampler");
}

// ---------------------------------------------------------------------------
// Timer: let the platform layer (re)attach drop hooks every ~20 ticks
// ---------------------------------------------------------------------------
static void OnTimer()
{
    g_timerTicks++;
    if (g_timerTicks < 20) return;
    g_timerTicks = 0;
    Platform_Poll();
}

static void Cleanup()
{
    if (plugin_register)
    {
        plugin_register("-timer", (void*)OnTimer);
        plugin_register("-hookcommand2", (void*)HookCommand2);
        plugin_register("-hookcustommenu", (void*)HookCustomMenu);
        plugin_register("-prefpage", (void*)&g_prefsPage);
    }
    Platform_Shutdown();
    g_rec = nullptr;
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------
extern "C" REAPER_PLUGIN_DLL_EXPORT int ReaperPluginEntry(
    REAPER_PLUGIN_HINSTANCE hInstance, reaper_plugin_info_t* rec)
{
    if (!rec)
    {
        Cleanup();
        return 0;
    }

    if (rec->caller_version != REAPER_PLUGIN_VERSION)
        return 0;

    g_rec = rec;
    g_hInstance = (HINSTANCE)hInstance;
    REAPERAPI_LoadAPI(rec->GetFunc);

    if (!GetMainHwnd)
        return 0;

    LoadSettings();
    Platform_Init((void*)hInstance);

    custom_action_register_t action = {};
    action.uniqueSectionId = 0;
    action.idStr = "DROP_TO_SAMPLER_SETTINGS";
    action.name = "Drop To Sampler";
    action.extra = nullptr;
    g_cmdSettings = (int)(intptr_t)rec->Register("custom_action", &action);

    rec->Register("hookcommand2", (void*)HookCommand2);

    if (AddExtensionsMainMenu)
        AddExtensionsMainMenu();
    rec->Register("hookcustommenu", (void*)HookCustomMenu);

    rec->Register("prefpage", (void*)&g_prefsPage);

    rec->Register("timer", (void*)OnTimer);
    return 1;
}
