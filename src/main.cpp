/*
 * Drop To Sampler
 * OLE drop on TCP/MCP -> insert sampler with sample loaded.
 */

#define REAPERAPI_IMPLEMENT
#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"
#include "resource.h"

#include <windows.h>
#include <windowsx.h>
#ifndef MK_ALT
#define MK_ALT 0x20
#endif
#include <ole2.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus")
using namespace Gdiplus;
#include <shellapi.h>
#include <commctrl.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "comctl32.lib")

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

static int g_winX = CW_USEDEFAULT;
static int g_winY = CW_USEDEFAULT;

static void LoadSettings()
{
    g_debug = false;
    g_samplerMode = Sampler_RS5k;
    g_recArmMode = RecArm_Auto;
    g_showSamplerMode = ShowSampler_Default;
    g_multiFilesMode = MultiFiles_MultiTracks;
    g_multiSamplersMode = MultiSamplers_Layers;
    g_existingMode = Existing_ReplaceOnSlot;
    g_winX = CW_USEDEFAULT;
    g_winY = CW_USEDEFAULT;
    if (!GetExtState) return;

    const char* v = GetExtState(kExtSection, kExtKeyDebug);
    if ((!v || !v[0]) && GetExtState)
        v = GetExtState(kExtSectionLegacy, kExtKeyDebug);
    if (v && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T'))
        g_debug = true;

    v = GetExtState(kExtSection, kExtKeySampler);
    if (v && v[0])
    {
        int m = atoi(v);
        if (m >= Sampler_RS5k && m <= Sampler_AlwaysPrompt)
            g_samplerMode = m;
    }

    v = GetExtState(kExtSection, kExtKeyRecArm);
    if (v && v[0])
    {
        int m = atoi(v);
        if (m >= RecArm_Default && m <= RecArm_Auto)
            g_recArmMode = m;
    }

    v = GetExtState(kExtSection, kExtKeyShow);
    if (v && v[0])
    {
        int m = atoi(v);
        if (m >= ShowSampler_Default && m <= ShowSampler_No)
            g_showSamplerMode = m;
    }

    v = GetExtState(kExtSection, kExtKeyMulti);
    if (v && v[0])
    {
        int m = atoi(v);
        if (m >= MultiFiles_OneTrack && m <= MultiFiles_AlwaysPrompt)
            g_multiFilesMode = m;
    }

    v = GetExtState(kExtSection, kExtKeyMultiSamplers);
    if (v && v[0])
    {
        int m = atoi(v);
        if (m >= MultiSamplers_Layers && m <= MultiSamplers_AlwaysPrompt)
            g_multiSamplersMode = m;
    }

    v = GetExtState(kExtSection, kExtKeyExisting);
    if (v && v[0])
    {
        int m = atoi(v);
        if (m >= Existing_Add && m <= Existing_ReplaceOnSlot)
            g_existingMode = m;
    }

    v = GetExtState(kExtSection, "win_x");
    if (v && v[0]) g_winX = atoi(v);
    v = GetExtState(kExtSection, "win_y");
    if (v && v[0]) g_winY = atoi(v);
}

static void SaveSettings()
{
    if (!SetExtState) return;
    SetExtState(kExtSection, kExtKeyDebug, g_debug ? "1" : "0", true);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", g_samplerMode);
    SetExtState(kExtSection, kExtKeySampler, buf, true);
    snprintf(buf, sizeof(buf), "%d", g_recArmMode);
    SetExtState(kExtSection, kExtKeyRecArm, buf, true);
    snprintf(buf, sizeof(buf), "%d", g_showSamplerMode);
    SetExtState(kExtSection, kExtKeyShow, buf, true);
    snprintf(buf, sizeof(buf), "%d", g_multiFilesMode);
    SetExtState(kExtSection, kExtKeyMulti, buf, true);
    snprintf(buf, sizeof(buf), "%d", g_multiSamplersMode);
    SetExtState(kExtSection, kExtKeyMultiSamplers, buf, true);
    snprintf(buf, sizeof(buf), "%d", g_existingMode);
    SetExtState(kExtSection, kExtKeyExisting, buf, true);
    snprintf(buf, sizeof(buf), "%d", g_winX);
    SetExtState(kExtSection, "win_x", buf, true);
    snprintf(buf, sizeof(buf), "%d", g_winY);
    SetExtState(kExtSection, "win_y", buf, true);
}

static void SaveWindowPos(HWND hwnd)
{
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return;
    g_winX = rc.left;
    g_winY = rc.top;
    if (!SetExtState) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", g_winX);
    SetExtState(kExtSection, "win_x", buf, true);
    snprintf(buf, sizeof(buf), "%d", g_winY);
    SetExtState(kExtSection, "win_y", buf, true);
}

static void DebugLog(const char* msg)
{
    if (g_debug && ShowConsoleMsg)
        ShowConsoleMsg(msg);
}

// ---------------------------------------------------------------------------
// Paths: OLE gives UTF-16; REAPER APIs and Cartridge pending_load expect UTF-8
// ---------------------------------------------------------------------------
static bool WideToUtf8(const wchar_t* wide, char* out, int outBytes)
{
    if (!wide || !out || outBytes <= 0) return false;
    out[0] = 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, outBytes, nullptr, nullptr);
    return n > 0;
}

static bool Utf8ToWide(const char* utf8, wchar_t* out, int outChars)
{
    if (!utf8 || !out || outChars <= 0) return false;
    out[0] = 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, outChars);
    return n > 0;
}



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
static HWND g_tcpHwnd = nullptr;
static HWND g_mcpHwnd = nullptr;
static bool g_dropTargetRegistered = false;
static bool g_mcpDropRegistered = false;
static bool g_oleInited = false;
static int g_timerTicks = 0;
static int g_cmdSettings = 0;
static HINSTANCE g_hInstance = nullptr;


// ---------------------------------------------------------------------------
// Drag feedback: layered PNG near cursor (no OLE cursor fight)
// ---------------------------------------------------------------------------
enum DropVisualKind { DropVisual_Insert = 0, DropVisual_Replace = 1 };
static int g_dropVisualKind = DropVisual_Insert;

static ULONG_PTR g_gdiplusToken = 0;
static bool g_gdiplusInited = false;
static HWND g_insertHwnd = nullptr;
static HBITMAP g_hbmpInsert = nullptr;
static HBITMAP g_hbmpReplace = nullptr;
static int g_insertW = 32, g_insertH = 32;
static int g_replaceW = 32, g_replaceH = 32;
static bool g_inOleDrag = false;

static HBITMAP LoadPngToPremultipliedHBitmap(const wchar_t* path, int* outW, int* outH)
{
    if (!path || !path[0])
        return nullptr;

    Bitmap src(path);
    if (src.GetLastStatus() != Ok)
        return nullptr;

    const int w = (int)src.GetWidth();
    const int h = (int)src.GetHeight();
    if (w <= 0 || h <= 0)
        return nullptr;

    // Draw into a premultiplied-alpha bitmap, then copy pixels to a DIB section
    Bitmap dst(w, h, PixelFormat32bppPARGB);
    if (dst.GetLastStatus() != Ok)
        return nullptr;
    {
        Graphics g(&dst);
        g.SetCompositingMode(CompositingModeSourceCopy);
        g.Clear(Color(0, 0, 0, 0));
        g.SetCompositingMode(CompositingModeSourceOver);
        g.DrawImage(&src, 0, 0, w, h);
        g.Flush(FlushIntentionSync);
    }

    BitmapData bd = {};
    Rect rect(0, 0, w, h);
    if (dst.LockBits(&rect, ImageLockModeRead, PixelFormat32bppPARGB, &bd) != Ok)
        return nullptr;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits)
    {
        dst.UnlockBits(&bd);
        return nullptr;
    }

    const int srcStride = bd.Stride;
    const int dstStride = w * 4;
    BYTE* srcBits = (BYTE*)bd.Scan0;
    BYTE* dstBits = (BYTE*)bits;
    for (int y = 0; y < h; ++y)
        memcpy(dstBits + y * dstStride, srcBits + y * srcStride, dstStride);

    dst.UnlockBits(&bd);

    if (outW) *outW = w;
    if (outH) *outH = h;
    return dib;
}


static void FindPngPath(const char* fileName, wchar_t* out, size_t outCount)
{
    out[0] = 0;
    HINSTANCE inst = g_hInstance ? g_hInstance : GetModuleHandleA(nullptr);

    auto tryPath = [&](const char* pathA) -> bool {
        if (GetFileAttributesA(pathA) == INVALID_FILE_ATTRIBUTES)
            return false;
        MultiByteToWideChar(CP_UTF8, 0, pathA, -1, out, (int)outCount);
        if (!out[0])
            MultiByteToWideChar(CP_ACP, 0, pathA, -1, out, (int)outCount);
        return out[0] != 0;
    };

    char pathA[MAX_PATH];

    // 1) directory of this DLL
    char mod[MAX_PATH] = {};
    if (GetModuleFileNameA(inst, mod, MAX_PATH))
    {
        char* slash = strrchr(mod, '\\');
        if (slash) *slash = 0;
        snprintf(pathA, sizeof(pathA), "%s\\%s", mod, fileName);
        if (tryPath(pathA)) return;
        snprintf(pathA, sizeof(pathA), "%s\\res\\%s", mod, fileName);
        if (tryPath(pathA)) return;
    }

    // 2) REAPER UserPlugins (where build.bat copies PNGs)
    char* appdata = getenv("APPDATA");
    if (appdata && appdata[0])
    {
        snprintf(pathA, sizeof(pathA), "%s\\REAPER\\UserPlugins\\%s", appdata, fileName);
        if (tryPath(pathA)) return;
    }
}



// Load PNG from RCDATA embedded in this DLL
static HBITMAP LoadPngFromResource(int resId, int* outW, int* outH)
{
    HINSTANCE inst = g_hInstance ? g_hInstance : GetModuleHandleA(nullptr);
    HRSRC hrs = FindResourceA(inst, MAKEINTRESOURCEA(resId), (LPCSTR)RT_RCDATA);
    if (!hrs) return nullptr;
    HGLOBAL hg = LoadResource(inst, hrs);
    if (!hg) return nullptr;
    const void* data = LockResource(hg);
    DWORD size = SizeofResource(inst, hrs);
    if (!data || !size) return nullptr;

    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!copy) return nullptr;
    void* dst = GlobalLock(copy);
    if (!dst)
    {
        GlobalFree(copy);
        return nullptr;
    }
    memcpy(dst, data, size);
    GlobalUnlock(copy);

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(copy, TRUE, &stream) != S_OK) // TRUE = free on release
    {
        GlobalFree(copy);
        return nullptr;
    }

    Bitmap* src = Bitmap::FromStream(stream);
    stream->Release();
    if (!src || src->GetLastStatus() != Ok)
    {
        delete src;
        return nullptr;
    }

    const int w = (int)src->GetWidth();
    const int h = (int)src->GetHeight();
    if (w <= 0 || h <= 0)
    {
        delete src;
        return nullptr;
    }

    Bitmap dstBmp(w, h, PixelFormat32bppPARGB);
    if (dstBmp.GetLastStatus() != Ok)
    {
        delete src;
        return nullptr;
    }
    {
        Graphics g(&dstBmp);
        g.SetCompositingMode(CompositingModeSourceCopy);
        g.Clear(Color(0, 0, 0, 0));
        g.SetCompositingMode(CompositingModeSourceOver);
        g.DrawImage(src, 0, 0, w, h);
        g.Flush(FlushIntentionSync);
    }
    delete src;

    BitmapData bd = {};
    Rect rect(0, 0, w, h);
    if (dstBmp.LockBits(&rect, ImageLockModeRead, PixelFormat32bppPARGB, &bd) != Ok)
        return nullptr;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits)
    {
        dstBmp.UnlockBits(&bd);
        return nullptr;
    }

    const int srcStride = bd.Stride;
    const int dstStride = w * 4;
    BYTE* srcBits = (BYTE*)bd.Scan0;
    BYTE* dstBits = (BYTE*)bits;
    for (int y = 0; y < h; ++y)
        memcpy(dstBits + y * dstStride, srcBits + y * srcStride, dstStride);

    dstBmp.UnlockBits(&bd);
    if (outW) *outW = w;
    if (outH) *outH = h;
    return dib;
}

static void LoadOverlayImages()
{
    if (g_hbmpInsert && g_hbmpReplace)
        return;

    if (!g_gdiplusInited)
    {
        GdiplusStartupInput in;
        if (GdiplusStartup(&g_gdiplusToken, &in, nullptr) == Ok)
            g_gdiplusInited = true;
        else
            return;
    }

    // Optional file next to DLL / UserPlugins overrides embedded default (per image)
    wchar_t wpath[MAX_PATH];
    const char* srcInsert = "embedded";
    const char* srcReplace = "embedded";

    if (!g_hbmpInsert)
    {
        FindPngPath("sampler_insert.png", wpath, MAX_PATH);
        if (wpath[0])
        {
            g_hbmpInsert = LoadPngToPremultipliedHBitmap(wpath, &g_insertW, &g_insertH);
            if (g_hbmpInsert) srcInsert = "file";
        }
        if (!g_hbmpInsert)
            g_hbmpInsert = LoadPngFromResource(IDR_PNG_INSERT, &g_insertW, &g_insertH);
    }

    if (!g_hbmpReplace)
    {
        FindPngPath("sampler_replace.png", wpath, MAX_PATH);
        if (wpath[0])
        {
            g_hbmpReplace = LoadPngToPremultipliedHBitmap(wpath, &g_replaceW, &g_replaceH);
            if (g_hbmpReplace) srcReplace = "file";
        }
        if (!g_hbmpReplace)
            g_hbmpReplace = LoadPngFromResource(IDR_PNG_REPLACE, &g_replaceW, &g_replaceH);
    }

    if (g_debug)
    {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "[Drop To Sampler] overlay insert=%s (%s %dx%d) replace=%s (%s %dx%d)\n",
            g_hbmpInsert ? "OK" : "FAIL", srcInsert, g_insertW, g_insertH,
            g_hbmpReplace ? "OK" : "FAIL", srcReplace, g_replaceW, g_replaceH);
        DebugLog(buf);
    }
}




static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void EnsureOverlayWindow()
{
    if (g_insertHwnd && IsWindow(g_insertHwnd))
        return;
    static bool reg = false;
    if (!reg)
    {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = OverlayWndProc;
        wc.hInstance = g_hInstance ? g_hInstance : GetModuleHandleA(nullptr);
        wc.lpszClassName = "DropToSamplerOverlay";
        wc.hCursor = nullptr;
        RegisterClassA(&wc);
        reg = true;
    }
    g_insertHwnd = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "DropToSamplerOverlay",
        "",
        WS_POPUP,
        0, 0, 64, 64,
        nullptr, nullptr,
        g_hInstance ? g_hInstance : GetModuleHandleA(nullptr),
        nullptr);
}

static void PaintOverlay(HBITMAP hbmp, POINT ptDst, int w, int h)
{
    if (!g_insertHwnd || !hbmp || w <= 0 || h <= 0)
        return;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, hbmp);

    SIZE size = { w, h };
    POINT ptSrc = { 0, 0 };
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    BOOL ok = UpdateLayeredWindow(g_insertHwnd, screen, &ptDst, &size, mem, &ptSrc, 0, &blend, ULW_ALPHA);
    if (!ok && g_debug)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "[Drop To Sampler] UpdateLayeredWindow failed err=%lu\n", GetLastError());
        DebugLog(buf);
    }

    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}



static void ShowOverlayAtCursor(int kind)
{
    LoadOverlayImages();
    EnsureOverlayWindow();
    if (!g_insertHwnd)
        return;

    HBITMAP hbmp = nullptr;
    int w = 32, h = 32;
    if (kind == DropVisual_Replace && g_hbmpReplace)
    {
        hbmp = g_hbmpReplace;
        w = g_replaceW;
        h = g_replaceH;
    }
    else if (g_hbmpInsert)
    {
        hbmp = g_hbmpInsert;
        w = g_insertW;
        h = g_insertH;
    }

    if (!hbmp)
    {
        // Hide previous frame (layered window can keep last image if only SW_HIDE)
        ShowWindow(g_insertHwnd, SW_HIDE);
        return;
    }

    POINT pt;
    GetCursorPos(&pt);
    pt.x += 16;
    pt.y += 16;

    SetWindowPos(g_insertHwnd, HWND_TOPMOST,
        pt.x, pt.y, w, h,
        SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
    PaintOverlay(hbmp, pt, w, h);
}



static void HideOverlay()
{
    if (g_insertHwnd && IsWindow(g_insertHwnd))
        ShowWindow(g_insertHwnd, SW_HIDE);
}

static void InitOverlaySystem()
{
    if (!g_gdiplusInited)
    {
        GdiplusStartupInput in;
        if (GdiplusStartup(&g_gdiplusToken, &in, nullptr) == Ok)
            g_gdiplusInited = true;
    }
    LoadOverlayImages();
}

static void ShutdownOverlaySystem()
{
    HideOverlay();
    if (g_insertHwnd && IsWindow(g_insertHwnd))
        DestroyWindow(g_insertHwnd);
    g_insertHwnd = nullptr;
    if (g_hbmpInsert) { DeleteObject(g_hbmpInsert); g_hbmpInsert = nullptr; }
    if (g_hbmpReplace) { DeleteObject(g_hbmpReplace); g_hbmpReplace = nullptr; }
    if (g_gdiplusInited)
    {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusInited = false;
        g_gdiplusToken = 0;
    }
}



// ---------------------------------------------------------------------------
// Dark mode helpers
// ---------------------------------------------------------------------------
static bool AppIsDark()
{
    return IsDarkMode && IsDarkMode();
}

static COLORREF ColorBg()    { return AppIsDark() ? RGB(32, 32, 32)   : GetSysColor(COLOR_WINDOW); }
static COLORREF ColorText()  { return AppIsDark() ? RGB(220, 220, 220) : GetSysColor(COLOR_WINDOWTEXT); }
static COLORREF ColorBtn()   { return AppIsDark() ? RGB(55, 55, 55)   : GetSysColor(COLOR_BTNFACE); }

static HBRUSH g_brBg = nullptr;
static HBRUSH g_brBtn = nullptr;
static int g_lastDark = -1; // -1 unknown, 0 light, 1 dark

static void EnsureBrushes()
{
    if (g_brBg) { DeleteObject(g_brBg); g_brBg = nullptr; }
    if (g_brBtn) { DeleteObject(g_brBtn); g_brBtn = nullptr; }
    g_brBg = CreateSolidBrush(ColorBg());
    g_brBtn = CreateSolidBrush(ColorBtn());
    g_lastDark = AppIsDark() ? 1 : 0;
}

static void FreeBrushes()
{
    if (g_brBg) { DeleteObject(g_brBg); g_brBg = nullptr; }
    if (g_brBtn) { DeleteObject(g_brBtn); g_brBtn = nullptr; }
    g_lastDark = -1;
}

static void ApplyDarkToChild(HWND child)
{
    typedef BOOL (WINAPI *fnAllowDark)(HWND, BOOL);
    static fnAllowDark pAllow = nullptr;
    static bool tried = false;
    if (!tried)
    {
        HMODULE uxt = LoadLibraryA("uxtheme.dll");
        if (uxt)
            pAllow = (fnAllowDark)GetProcAddress(uxt, MAKEINTRESOURCEA(133));
        tried = true;
    }
    if (pAllow)
        pAllow(child, AppIsDark() ? TRUE : FALSE);
}

// Shared dark-mode paint helpers for any of our dialogs
static LRESULT DarkCtlColor(HDC hdc)
{
    SetTextColor(hdc, ColorText());
    SetBkColor(hdc, ColorBg());
    if (!g_brBg) EnsureBrushes();
    return (LRESULT)g_brBg;
}

static BOOL CALLBACK EnumApplyDark(HWND child, LPARAM)
{
    ApplyDarkToChild(child);
    InvalidateRect(child, nullptr, TRUE);
    return TRUE;
}

static void RefreshDarkMode(HWND hwnd)
{
    EnsureBrushes();
    ApplyDarkToChild(hwnd);
    EnumChildWindows(hwnd, EnumApplyDark, 0);
    InvalidateRect(hwnd, nullptr, TRUE);
}

// Call from dialog WndProc: handles erase/ctlcolor/theme change/timer poll.
// Returns true if message was handled.
static bool HandleDarkModeMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* out)
{
    switch (msg)
    {
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        *out = DarkCtlColor((HDC)wParam);
        return true;
    case WM_ERASEBKGND:
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (!g_brBg) EnsureBrushes();
        FillRect((HDC)wParam, &rc, g_brBg);
        *out = 1;
        return true;
    }
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        RefreshDarkMode(hwnd);
        *out = 0;
        return true;
    case WM_TIMER:
        if (wParam == 42) // dark-mode poll
        {
            int d = AppIsDark() ? 1 : 0;
            if (d != g_lastDark)
                RefreshDarkMode(hwnd);
            *out = 0;
            return true;
        }
        break;
    }
    (void)lParam;
    return false;
}

// ---------------------------------------------------------------------------
// TCP window
// ---------------------------------------------------------------------------
static HWND FindTcpWindow()
{
    if (!GetMainHwnd) return nullptr;
    HWND main = GetMainHwnd();
    if (!main) return nullptr;

    HWND tcp = FindWindowExA(main, nullptr, "REAPERTCPDisplay", nullptr);
    if (tcp) return tcp;

    HWND child = GetWindow(main, GW_CHILD);
    while (child)
    {
        char cls[256] = {};
        GetClassNameA(child, cls, sizeof(cls));
        if (strstr(cls, "TCP") || strstr(cls, "tcp") || strstr(cls, "REAPERTCP"))
            return child;
        child = GetWindow(child, GW_HWNDNEXT);
    }
    return nullptr;
}

static bool WindowLooksLikeMcp(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd)) return false;
    char cls[256] = {};
    char title[256] = {};
    GetClassNameA(hwnd, cls, sizeof(cls));
    GetWindowTextA(hwnd, title, sizeof(title));
    if (strstr(cls, "REAPERMCP") || strstr(cls, "MCPDisplay"))
        return true;
    if (strstr(cls, "MCP") || strstr(cls, "mcp"))
        return true;
    // Floating / docked mixer host windows
    if (strstr(title, "Mixer") || strstr(title, "mixer"))
        return true;
    return false;
}

struct McpSearchCtx { HWND found; };

static BOOL CALLBACK EnumMcpDeep(HWND hwnd, LPARAM lParam)
{
    McpSearchCtx* ctx = (McpSearchCtx*)lParam;
    if (WindowLooksLikeMcp(hwnd))
    {
        // Prefer concrete MCP display over generic "Mixer" title
        char cls[256] = {};
        GetClassNameA(hwnd, cls, sizeof(cls));
        if (strstr(cls, "MCP") || strstr(cls, "mcp") || strstr(cls, "REAPERMCP"))
        {
            ctx->found = hwnd;
            return FALSE; // stop
        }
        if (!ctx->found)
            ctx->found = hwnd;
    }
    EnumChildWindows(hwnd, EnumMcpDeep, lParam);
    return TRUE;
}

static BOOL CALLBACK EnumTopForMcp(HWND hwnd, LPARAM lParam)
{
    EnumMcpDeep(hwnd, lParam);
    McpSearchCtx* ctx = (McpSearchCtx*)lParam;
    return ctx->found ? FALSE : TRUE;
}

static HWND FindMcpWindow()
{
    HWND main = GetMainHwnd ? GetMainHwnd() : nullptr;
    if (!main) return nullptr;

    HWND mcp = FindWindowExA(main, nullptr, "REAPERMCPDisplay", nullptr);
    if (mcp) return mcp;

    McpSearchCtx ctx = {};
    EnumChildWindows(main, EnumMcpDeep, (LPARAM)&ctx);
    if (ctx.found) return ctx.found;

    // Floating mixer is often a separate top-level window
    ctx.found = nullptr;
    EnumWindows(EnumTopForMcp, (LPARAM)&ctx);
    return ctx.found;
}

// Register OLE drop; if REAPER already owns the target, steal it.
static bool RegisterDropOnHwnd(HWND hwnd, IDropTarget* target)
{
    if (!hwnd || !target) return false;
    HRESULT hr = RegisterDragDrop(hwnd, target);
    if (hr == DRAGDROP_E_ALREADYREGISTERED)
    {
        RevokeDragDrop(hwnd);
        hr = RegisterDragDrop(hwnd, target);
    }
    return SUCCEEDED(hr);
}


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

    if (tr && (info[0] == '\0' || strstr(info, "tcp") == info || strstr(info, "mcp") == info))
        return tr;
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

    char dir[MAX_PATH] = {};
    const char* appdata = getenv("APPDATA");
    if (appdata && appdata[0])
        snprintf(dir, sizeof(dir), "%s\\Cartridge", appdata);
    else
        snprintf(dir, sizeof(dir), "Cartridge");

    if (RecursiveCreateDirectory)
        RecursiveCreateDirectory(dir, 0);
    else
        CreateDirectoryA(dir, nullptr);

    char pending[MAX_PATH];
    snprintf(pending, sizeof(pending), "%s\\pending_load.txt", dir);

    // UTF-8 path for Cartridge (same as REAPER Lua). Wide fopen for non-ASCII APPDATA.
    wchar_t wpending[MAX_PATH];
    if (!Utf8ToWide(pending, wpending, MAX_PATH))
    {
        DebugLog("[Drop To Sampler] Cartridge: pending path UTF-8 convert failed\n");
        return;
    }
    FILE* f = _wfopen(wpending, L"wb");
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

// TrackFX_GetParam signature in REAPER: double TrackFX_GetParam(MediaTrack*, int fx, int param, double* minOut, double* maxOut)
// Our call above may need adjustment - check header

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
        if (g_debug)
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
    // Existing FX: hide if Show=No or multi-track force; for replace show last rules applied by caller
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
    if (g_debug)
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

static int PromptSamplerChoice()
{
    int r = MessageBoxA(PromptParent(),
        "Use Cartridge (DsgDnB)?\n\n"
        "Yes = Cartridge (DsgDnB)\n"
        "No = ReaSamplOmatic5000 (Cockos)",
        "Drop To Sampler",
        MB_YESNOCANCEL | MB_ICONQUESTION | MB_TOPMOST);
    if (r == IDYES) return Sampler_Cartridge;
    if (r == IDNO) return Sampler_RS5k;
    return -1;
}

static int PromptMultiFilesChoice()
{
    int r = MessageBoxA(PromptParent(),
        "Place multiple media on one track?\n\n"
        "Yes = Insert multiple media on one track\n"
        "No = Create new track for each sampler",
        "Drop To Sampler",
        MB_YESNOCANCEL | MB_ICONQUESTION | MB_TOPMOST);
    if (r == IDYES) return MultiFiles_OneTrack;
    if (r == IDNO) return MultiFiles_MultiTracks;
    return -1;
}

static int PromptMultiSamplersChoice()
{
    int r = MessageBoxA(PromptParent(),
        "Place media in samplers in chromatic order?\n\n"
        "Yes = One sample per note starting from C4 (including existing samplers)\n"
        "No = Default note range for each sampler (Layers)",
        "Drop To Sampler",
        MB_YESNOCANCEL | MB_ICONQUESTION | MB_TOPMOST);
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
        if (g_debug) DebugLog("[Drop To Sampler] skip rename: multiple samples on one track\n");
        return;
    }
    if (hadSamplerBefore)
    {
        if (g_debug) DebugLog("[Drop To Sampler] skip rename: sampler already on track\n");
        return;
    }
    if (!GetSetMediaTrackInfo_String)
    {
        if (g_debug) DebugLog("[Drop To Sampler] skip rename: GetSetMediaTrackInfo_String missing\n");
        return;
    }
    if (!TrackNameIsEmpty(track))
    {
        if (g_debug) DebugLog("[Drop To Sampler] skip rename: track already has P_NAME\n");
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

static MediaTrack* InsertTrackAtEnd()
{
    if (!CountTracks || !InsertTrackAtIndex || !GetTrack) return nullptr;
    int n = CountTracks(nullptr);
    InsertTrackAtIndex(n, true);
    if (TrackList_AdjustWindows) TrackList_AdjustWindows(false);
    if (UpdateArrange) UpdateArrange();
    return GetTrack(nullptr, n);
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

static void ProcessDropFiles(IDataObject* pDataObj, int screenX, int screenY, DWORD grfKeyState)
{
    if (!pDataObj) return;

    FORMATETC fmt = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stg = {};
    if (FAILED(pDataObj->GetData(&fmt, &stg)))
    {
        DebugLog("[Drop To Sampler] drop: no CF_HDROP\n");
        return;
    }

    HDROP hDrop = (HDROP)GlobalLock(stg.hGlobal);
    if (!hDrop)
    {
        ReleaseStgMedium(&stg);
        return;
    }

    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);

    // UTF-8 paths (REAPER + Cartridge)
    char files[64][1024];
    UINT nFiles = 0;
    for (UINT i = 0; i < count && nFiles < 64; ++i)
    {
        wchar_t wpath[1024];
        wpath[0] = 0;
        UINT n = DragQueryFileW(hDrop, i, wpath, 1024);
        files[nFiles][0] = 0;
        if (n > 0 && WideToUtf8(wpath, files[nFiles], 1024) && files[nFiles][0])
            ++nFiles;
    }

    if (nFiles == 0)
    {
        GlobalUnlock(stg.hGlobal);
        ReleaseStgMedium(&stg);
        return;
    }

    bool createdNew = false;
    MediaTrack* track = ResolveTrackAtPoint(screenX, screenY, &createdNew);
    if (!track)
    {
        GlobalUnlock(stg.hGlobal);
        ReleaseStgMedium(&stg);
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
        {
            GlobalUnlock(stg.hGlobal);
            ReleaseStgMedium(&stg);
            return;
        }
    }
    else if (nFiles > 1 && multiFilesMode != MultiFiles_AlwaysPrompt)
    {
        // Shift toggles one-track <-> multi-track (not used with Always prompt)
        const bool shift = (grfKeyState & MK_SHIFT) != 0
            || (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (shift)
        {
            if (multiFilesMode == MultiFiles_OneTrack)
                multiFilesMode = MultiFiles_MultiTracks;
            else if (multiFilesMode == MultiFiles_MultiTracks)
                multiFilesMode = MultiFiles_OneTrack;
            if (g_debug)
                DebugLog(multiFilesMode == MultiFiles_MultiTracks
                    ? "[Drop To Sampler] Shift: multiple tracks\n"
                    : "[Drop To Sampler] Shift: one track\n");
        }
    }

    const bool ctrlDown = (grfKeyState & MK_CONTROL) != 0
        || (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool altDown = (grfKeyState & MK_ALT) != 0
        || (GetKeyState(VK_MENU) & 0x8000) != 0;

    int resolvedSampler = g_samplerMode;
    if (resolvedSampler == Sampler_AlwaysPrompt)
    {
        resolvedSampler = PromptSamplerChoice();
        if (resolvedSampler < 0)
        {
            GlobalUnlock(stg.hGlobal);
            ReleaseStgMedium(&stg);
            return;
        }
    }
    int samplerType = (resolvedSampler == Sampler_Cartridge) ? Sampler_Cartridge : Sampler_RS5k;

    // Existing samplers of chosen type on the drop target track
    int existingOnTrack = 0;
    if (track)
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
        if (g_debug)
            DebugLog(existingMode == Existing_Add
                ? "[Drop To Sampler] Alt: Add\n"
                : "[Drop To Sampler] Alt: Replace\n");
    }


    // Single file that will REPLACE -> no Layers/Chromatic prompt
    bool willReplaceExisting = false;
    if (nFiles == 1 && track)
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
        if (g_debug)
            DebugLog(multiSamplersMode == MultiSamplers_Chromatic
                ? "[Drop To Sampler] Ctrl: Chromatic\n"
                : "[Drop To Sampler] Ctrl: Layers\n");
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
                {
                    GlobalUnlock(stg.hGlobal);
                    ReleaseStgMedium(&stg);
                    return;
                }
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
    if (chromatic && track && !(multiFilesMode == MultiFiles_MultiTracks && nFiles > 1))
        RemapTrackSamplersChromatic(track, samplerType);

    if (Undo_BeginBlock) Undo_BeginBlock();

    MediaTrack* lastNewTrack = nullptr;
    const bool allowReplace = (nFiles == 1);

    if (g_debug && GetThingFromPoint)
    {
        char info[256] = {};
        GetThingFromPoint(screenX, screenY, info, sizeof(info));
        char buf[512];
        snprintf(buf, sizeof(buf), "[Drop To Sampler] drop hit info=%s\n", info);
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
        // Multiple samples stacked on one track → do not auto-name.
        // Single sample + empty name + no sampler before → name from file.
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

    GlobalUnlock(stg.hGlobal);
    ReleaseStgMedium(&stg);
}

// ---------------------------------------------------------------------------

// Status line under REAPER panels (Help_Set) during drag
static char g_lastDropHelp[256] = {};

static void ClearDropHelp()
{
    if (Help_Set)
        Help_Set("", true);
    g_lastDropHelp[0] = 0;
}

static void SetDropHelp(const char* msg)
{
    if (!Help_Set || !msg) return;
    // Same as first Help_Set version: temporary help, re-assert every DragOver
    // (strcmp skip removed so REAPER keeps showing it for the whole drag)
    strncpy(g_lastDropHelp, msg, sizeof(g_lastDropHelp) - 1);
    g_lastDropHelp[sizeof(g_lastDropHelp) - 1] = 0;
    Help_Set(msg, true);
}

// Preview what this drop would do (settings + modifiers + hover target)
static void UpdateDropHelp(DWORD grfKeyState, int screenX, int screenY, int fileCount)
{
    if (!Help_Set) return;
    if (fileCount < 1) fileCount = 1;
    g_dropVisualKind = DropVisual_Insert;

    const bool shift = (grfKeyState & MK_SHIFT) != 0
        || (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrl = (grfKeyState & MK_CONTROL) != 0
        || (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (grfKeyState & MK_ALT) != 0
        || (GetKeyState(VK_MENU) & 0x8000) != 0;

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
            return ctrl ? "Chromatic" : "Chromatic (Ctrl toggles)";
        return ctrl ? "Layers" : "Layers (Ctrl toggles)";
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
                g_dropVisualKind = DropVisual_Replace;
                SetDropHelp("Replace media");
                return;
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
            g_dropVisualKind = DropVisual_Replace;
            if (!alt && g_existingMode != Existing_ReplaceOnSlot)
                SetDropHelp("Replace media (Alt toggles)");
            else
                SetDropHelp("Replace media");
            return;
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
            snprintf(buf, sizeof(buf), "New samplers (%d) · %s", fileCount, placeLabel());
        else
            snprintf(buf, sizeof(buf), "New samplers (%d) · %s · %s",
                fileCount, placeLabel(), layerLabel());
        SetDropHelp(buf);
        return;
    }

    // --- Single media, Add path (also Replace-on-slot when not on slot) ---
    if (existingOnTrack >= 1)
    {
        char buf[180];
        // When base mode is Add and Alt free: layers + optional Alt hint is long; keep Layers/Chromatic only
        snprintf(buf, sizeof(buf), "New sampler · %s", layerLabel());
        SetDropHelp(buf);
        return;
    }

    // Empty track + Add mode: show Alt toggles only if Replace is meaningful? skip on empty
    if (g_existingMode == Existing_Add && !alt)
        SetDropHelp("New sampler");
    else if (g_existingMode == Existing_Replace && !alt)
        SetDropHelp("New sampler"); // nothing to replace
    else
        SetDropHelp("New sampler");
}

// IDropTarget
// ---------------------------------------------------------------------------
class TcpDropTarget : public IDropTarget
{
public:
    TcpDropTarget() : m_ref(1) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDropTarget)
        {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }

    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    static int CountHDropFiles(IDataObject* pDataObj)
    {
        if (!pDataObj) return 0;
        FORMATETC fmt = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM stg = {};
        if (FAILED(pDataObj->GetData(&fmt, &stg)) || !stg.hGlobal) return 0;
        HDROP hDrop = (HDROP)GlobalLock(stg.hGlobal);
        int n = 0;
        if (hDrop)
            n = (int)DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        if (stg.hGlobal) GlobalUnlock(stg.hGlobal);
        ReleaseStgMedium(&stg);
        return n > 0 ? n : 0;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override
    {
        m_fileCount = CountHDropFiles(pDataObj);
        if (pdwEffect) *pdwEffect = DROPEFFECT_COPY;
        g_inOleDrag = true;
        UpdateDropHelp(grfKeyState, pt.x, pt.y, m_fileCount);
        ShowOverlayAtCursor(g_dropVisualKind);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override
    {
        if (pdwEffect) *pdwEffect = DROPEFFECT_COPY;
        g_inOleDrag = true;
        UpdateDropHelp(grfKeyState, pt.x, pt.y, m_fileCount);
        ShowOverlayAtCursor(g_dropVisualKind);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        g_inOleDrag = false;
        HideOverlay();
        ClearDropHelp();
        m_fileCount = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override
    {
        g_inOleDrag = false;
        HideOverlay();
        ClearDropHelp();
        m_fileCount = 0;
        ProcessDropFiles(pDataObj, pt.x, pt.y, grfKeyState);
        if (pdwEffect) *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }

private:
    LONG m_ref;
    int m_fileCount = 0;
};

static TcpDropTarget* g_dropTarget = nullptr;

// ---------------------------------------------------------------------------
// Settings dialog
// ---------------------------------------------------------------------------
static HWND g_settingsHwnd = nullptr;
static const int IDC_DEBUG_CHECK = 1001;
static const int IDC_SAMPLER_COMBO = 1002;
static const int IDC_OK = 1003;
static const int IDC_CANCEL = 1004;
static const int IDC_LABEL_SAMPLER = 1005;
static const int IDC_LABEL_TITLE = 1006;
static const int IDC_LABEL_RECARM = 1007;
static const int IDC_RECARM_COMBO = 1008;
static const int IDC_LABEL_SHOW = 1009;
static const int IDC_SHOW_COMBO = 1010;
static const int IDC_LABEL_MULTI = 1011;
static const int IDC_MULTI_COMBO = 1012;
static const int IDC_LABEL_EXISTING = 1013;
static const int IDC_EXISTING_COMBO = 1014;
static const int IDC_LABEL_MULTISAMP = 1015;
static const int IDC_MULTISAMP_COMBO = 1016;
static const int IDC_HINT = 1017;




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
        return "Add or Replace (Alt toggles) sample when a sampler is already on the track. Replace ignored when multiple media dragged.";
    case IDC_MULTI_COMBO:
    case IDC_LABEL_MULTI:
        return "Where to place multiple dropped media. Hold Shift to toggle this behavior.";
    case IDC_MULTISAMP_COMBO:
    case IDC_LABEL_MULTISAMP:
        return "Layers: default note range. Chromatic: one note per sample starting from C4 (including existing samplers). Ctrl to toggle.";
    case IDC_DEBUG_CHECK:
        return "Log drop details to the REAPER console.";
    default:
        return "";
    }
}

static const int IDC_PREFS_HELP = 0x4eb;
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
    {
        int id = GetDlgCtrlID(child);
        hint = HintForControlId(id);
    }
    if (hint == g_shownPrefsHelp)
        return;
    g_shownPrefsHelp = hint;
    SetDlgItemTextA(prefs, IDC_PREFS_HELP, hint && hint[0] ? hint : "");
}

static HFONT GetUiFont()
{
    static HFONT font = nullptr;
    if (!font)
    {
        NONCLIENTMETRICSA ncm = {};
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            font = CreateFontIndirectA(&ncm.lfMessageFont);
        else
            font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }
    return font;
}

static void SetFontRecursive(HWND hwnd, HFONT font)
{
    SendMessageA(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
    HWND c = nullptr;
    while ((c = FindWindowExA(hwnd, c, nullptr, nullptr)) != nullptr)
        SendMessageA(c, WM_SETFONT, (WPARAM)font, TRUE);
}



#define WM_PREFS_APPLY (WM_USER * 2)
#define IDC_PREFS_APPLY_BTN 0x478

static void NotifyPrefsApplyEnabled(HWND page)
{
    HWND prefs = GetParent(page);
    if (prefs)
    {
        HWND apply = GetDlgItem(prefs, IDC_PREFS_APPLY_BTN);
        if (apply) EnableWindow(apply, TRUE);
    }
}

static void PopulateSettingsControls(HWND hwnd, bool asPrefs);

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    const bool asPrefs = GetWindowLongPtrA(hwnd, GWLP_USERDATA) != 0;

    // Custom dark painting only for standalone popup; prefs page uses native REAPER look
    if (!asPrefs)
    {
        LRESULT darkOut = 0;
        if (HandleDarkModeMsg(hwnd, msg, wParam, lParam, &darkOut))
            return darkOut;
    }

    switch (msg)
    {
    case WM_SETCURSOR:
        if (asPrefs)
        {
            UpdatePrefsHelp(hwnd);
        }
        break;

    case WM_CREATE:
    {
        CREATESTRUCTA* cs = (CREATESTRUCTA*)lParam;
        bool asPrefs = cs && cs->lpCreateParams != nullptr;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, asPrefs ? 1 : 0);
        if (!asPrefs)
        {
            EnsureBrushes();
            SetTimer(hwnd, 42, 500, nullptr);
        }
        PopulateSettingsControls(hwnd, asPrefs);
        if (!asPrefs)
            RefreshDarkMode(hwnd);
        return 0;
    }
    case WM_PREFS_APPLY:
    {
        // Apply from REAPER Preferences OK/Apply
        HWND chk = GetDlgItem(hwnd, IDC_DEBUG_CHECK);
        if (chk) g_debug = (SendMessageA(chk, BM_GETCHECK, 0, 0) == BST_CHECKED);
        HWND combo = GetDlgItem(hwnd, IDC_SAMPLER_COMBO);
        if (combo) {
            int sel = (int)SendMessageA(combo, CB_GETCURSEL, 0, 0);
            if (!g_cartridgeAvailable) g_samplerMode = Sampler_RS5k;
            else if (sel >= 0) g_samplerMode = sel;
        }
        HWND comboRa = GetDlgItem(hwnd, IDC_RECARM_COMBO);
        if (comboRa) {
            int ra = (int)SendMessageA(comboRa, CB_GETCURSEL, 0, 0);
            if (ra >= RecArm_Default && ra <= RecArm_Auto) g_recArmMode = ra;
        }
        HWND comboShow = GetDlgItem(hwnd, IDC_SHOW_COMBO);
        if (comboShow) {
            int sh = (int)SendMessageA(comboShow, CB_GETCURSEL, 0, 0);
            if (sh >= ShowSampler_Default && sh <= ShowSampler_No) g_showSamplerMode = sh;
        }
        HWND comboEx = GetDlgItem(hwnd, IDC_EXISTING_COMBO);
        if (comboEx) {
            int ex = (int)SendMessageA(comboEx, CB_GETCURSEL, 0, 0);
            if (ex >= Existing_Add && ex <= Existing_ReplaceOnSlot) g_existingMode = ex;
        }
        HWND comboMulti = GetDlgItem(hwnd, IDC_MULTI_COMBO);
        if (comboMulti) {
            int mf = (int)SendMessageA(comboMulti, CB_GETCURSEL, 0, 0);
            if (mf >= MultiFiles_OneTrack && mf <= MultiFiles_AlwaysPrompt) g_multiFilesMode = mf;
        }
        HWND comboMs = GetDlgItem(hwnd, IDC_MULTISAMP_COMBO);
        if (comboMs) {
            int ms = (int)SendMessageA(comboMs, CB_GETCURSEL, 0, 0);
            if (ms >= MultiSamplers_Layers && ms <= MultiSamplers_AlwaysPrompt) g_multiSamplersMode = ms;
        }
        SaveSettings();
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        if (asPrefs)
        {
            UpdatePrefsHelp(hwnd);
            return 0;
        }
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        HWND child = ChildWindowFromPointEx(hwnd, pt, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
        const char* hint = "";
        if (child && child != hwnd)
        {
            int id = GetDlgCtrlID(child);
            hint = HintForControlId(id);
        }
        HWND hHint = GetDlgItem(hwnd, IDC_HINT);
        if (hHint)
        {
            char cur[512] = {};
            GetWindowTextA(hHint, cur, sizeof(cur));
            if (strcmp(cur, hint) != 0)
                SetWindowTextA(hHint, hint);
        }
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wParam) == CBN_SELCHANGE || HIWORD(wParam) == BN_CLICKED)
        {
            if (LOWORD(wParam) != IDC_OK && LOWORD(wParam) != IDC_CANCEL)
                NotifyPrefsApplyEnabled(hwnd);
        }
        if (LOWORD(wParam) == IDC_OK)
        {
            HWND chk = GetDlgItem(hwnd, IDC_DEBUG_CHECK);
            g_debug = (SendMessageA(chk, BM_GETCHECK, 0, 0) == BST_CHECKED);

            HWND combo = GetDlgItem(hwnd, IDC_SAMPLER_COMBO);
            int sel = (int)SendMessageA(combo, CB_GETCURSEL, 0, 0);
            if (!g_cartridgeAvailable)
                g_samplerMode = Sampler_RS5k;
            else if (sel >= 0)
                g_samplerMode = sel;

            HWND comboRa = GetDlgItem(hwnd, IDC_RECARM_COMBO);
            int ra = (int)SendMessageA(comboRa, CB_GETCURSEL, 0, 0);
            if (ra >= RecArm_Default && ra <= RecArm_Auto)
                g_recArmMode = ra;

            HWND comboShow = GetDlgItem(hwnd, IDC_SHOW_COMBO);
            int sh = (int)SendMessageA(comboShow, CB_GETCURSEL, 0, 0);
            if (sh >= ShowSampler_Default && sh <= ShowSampler_No)
                g_showSamplerMode = sh;

            HWND comboEx = GetDlgItem(hwnd, IDC_EXISTING_COMBO);
            int ex = (int)SendMessageA(comboEx, CB_GETCURSEL, 0, 0);
            if (ex >= Existing_Add && ex <= Existing_ReplaceOnSlot)
                g_existingMode = ex;

            HWND comboMulti = GetDlgItem(hwnd, IDC_MULTI_COMBO);
            int mf = (int)SendMessageA(comboMulti, CB_GETCURSEL, 0, 0);
            if (mf >= MultiFiles_OneTrack && mf <= MultiFiles_AlwaysPrompt)
                g_multiFilesMode = mf;

            HWND comboMs = GetDlgItem(hwnd, IDC_MULTISAMP_COMBO);
            int ms = (int)SendMessageA(comboMs, CB_GETCURSEL, 0, 0);
            if (ms >= MultiSamplers_Layers && ms <= MultiSamplers_AlwaysPrompt)
                g_multiSamplersMode = ms;

            SaveSettings();
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDC_CANCEL || LOWORD(wParam) == IDCANCEL)
        {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 42);
        SaveWindowPos(hwnd);
        FreeBrushes();
        g_settingsHwnd = nullptr;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}


// --- REAPER Preferences page -----------------------------------------------
static HWND CreateSettingsPage(HWND parent, bool asPrefsChild);


// Native dialog template so REAPER dark mode themes the page like other prefs.
#pragma pack(push, 2)
struct PrefsDlgTemplate
{
    DLGTEMPLATE dlg;
    WORD menu;
    WORD windowClass;
    WCHAR title;
    WORD pointsize;
    WCHAR typeface[14]; // L"MS Shell Dlg"
};
#pragma pack(pop)

static void PopulateSettingsControls(HWND hwnd, bool asPrefs);





static INT_PTR CALLBACK PrefsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, 1); // prefs mode
        PopulateSettingsControls(hwnd, true);
        return TRUE;

    case WM_PREFS_APPLY:
    {
        HWND chk = GetDlgItem(hwnd, IDC_DEBUG_CHECK);
        if (chk) g_debug = (SendMessageA(chk, BM_GETCHECK, 0, 0) == BST_CHECKED);
        HWND combo = GetDlgItem(hwnd, IDC_SAMPLER_COMBO);
        if (combo) {
            int sel = (int)SendMessageA(combo, CB_GETCURSEL, 0, 0);
            if (!g_cartridgeAvailable) g_samplerMode = Sampler_RS5k;
            else if (sel >= 0) g_samplerMode = sel;
        }
        HWND comboRa = GetDlgItem(hwnd, IDC_RECARM_COMBO);
        if (comboRa) {
            int ra = (int)SendMessageA(comboRa, CB_GETCURSEL, 0, 0);
            if (ra >= RecArm_Default && ra <= RecArm_Auto) g_recArmMode = ra;
        }
        HWND comboShow = GetDlgItem(hwnd, IDC_SHOW_COMBO);
        if (comboShow) {
            int sh = (int)SendMessageA(comboShow, CB_GETCURSEL, 0, 0);
            if (sh >= ShowSampler_Default && sh <= ShowSampler_No) g_showSamplerMode = sh;
        }
        HWND comboEx = GetDlgItem(hwnd, IDC_EXISTING_COMBO);
        if (comboEx) {
            int ex = (int)SendMessageA(comboEx, CB_GETCURSEL, 0, 0);
            if (ex >= Existing_Add && ex <= Existing_ReplaceOnSlot) g_existingMode = ex;
        }
        HWND comboMulti = GetDlgItem(hwnd, IDC_MULTI_COMBO);
        if (comboMulti) {
            int mf = (int)SendMessageA(comboMulti, CB_GETCURSEL, 0, 0);
            if (mf >= MultiFiles_OneTrack && mf <= MultiFiles_AlwaysPrompt) g_multiFilesMode = mf;
        }
        HWND comboMs = GetDlgItem(hwnd, IDC_MULTISAMP_COMBO);
        if (comboMs) {
            int ms = (int)SendMessageA(comboMs, CB_GETCURSEL, 0, 0);
            if (ms >= MultiSamplers_Layers && ms <= MultiSamplers_AlwaysPrompt) g_multiSamplersMode = ms;
        }
        SaveSettings();
        return TRUE;
    }

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

static HWND CreatePrefsDialog(HWND parent)
{
    static PrefsDlgTemplate tmpl = {};
    tmpl.dlg.style = DS_SETFONT | DS_FIXEDSYS | WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN;
    tmpl.dlg.dwExtendedStyle = 0;
    tmpl.dlg.cdit = 0;
    tmpl.dlg.x = 0;
    tmpl.dlg.y = 0;
    tmpl.dlg.cx = 320; // dialog units
    tmpl.dlg.cy = 340;
    tmpl.menu = 0;
    tmpl.windowClass = 0;
    tmpl.title = 0;
    tmpl.pointsize = 8;
    // MS Shell Dlg (12 chars + NUL) — do not read past the string
    const wchar_t face[] = L"MS Shell Dlg";
    static_assert(sizeof(face) <= sizeof(tmpl.typeface), "typeface buffer too small");
    for (size_t i = 0; i < sizeof(face) / sizeof(face[0]); ++i)
        tmpl.typeface[i] = face[i];

    HWND hwnd = CreateDialogIndirectParamA(
        g_hInstance ? g_hInstance : GetModuleHandleA(nullptr),
        (LPCDLGTEMPLATEA)&tmpl,
        parent,
        PrefsDlgProc,
        0);
    return hwnd;
}

// Shared control population for prefs dialog and standalone window
static void PopulateSettingsControls(HWND hwnd, bool asPrefs)
{
    g_cartridgeAvailable = DetectCartridgeInstalled();
    if (!g_cartridgeAvailable && g_samplerMode != Sampler_RS5k)
        g_samplerMode = Sampler_RS5k;

    HFONT font = GetUiFont();

    const int left = 12;
    const int labelW = 370;
    const int comboW = 370;
    const int labelH = 16;
    const int rowGap = 28;
    int y = 10;

    if (!asPrefs)
    {
        CreateWindowA("STATIC", "Drop To Sampler",
            WS_CHILD | WS_VISIBLE, left, y, labelW, 16, hwnd, (HMENU)(intptr_t)IDC_LABEL_TITLE, g_hInstance, nullptr);
        y += 22;
    }

    auto addLabel = [&](const char* text, int id) {
        CreateWindowA("STATIC", text, WS_CHILD | WS_VISIBLE,
            left, y, labelW, labelH, hwnd, (HMENU)(intptr_t)id, g_hInstance, nullptr);
        y += 18;
    };
    auto addCombo = [&](int id) -> HWND {
        HWND c = CreateWindowA("COMBOBOX", "",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            left, y, comboW, 200, hwnd, (HMENU)(intptr_t)id, g_hInstance, nullptr);
        y += rowGap;
        return c;
    };

    addLabel("Sampler:", IDC_LABEL_SAMPLER);
    HWND combo = addCombo(IDC_SAMPLER_COMBO);
    SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"ReaSamplOmatic5000 (Cockos)");
    SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"Cartridge (DsgDnB)");
    SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"Always prompt");
    {
        int sel = g_samplerMode;
        if (!g_cartridgeAvailable)
        {
            SendMessageA(combo, CB_DELETESTRING, 2, 0);
            SendMessageA(combo, CB_DELETESTRING, 1, 0);
            sel = 0;
        }
        SendMessageA(combo, CB_SETCURSEL, sel, 0);
    }

    addLabel("Rec-arm for new tracks:", IDC_LABEL_RECARM);
    HWND comboRa = addCombo(IDC_RECARM_COMBO);
    SendMessageA(comboRa, CB_ADDSTRING, 0, (LPARAM)"Default");
    SendMessageA(comboRa, CB_ADDSTRING, 0, (LPARAM)"Rec-arm");
    SendMessageA(comboRa, CB_ADDSTRING, 0, (LPARAM)"Auto rec-arm");
    SendMessageA(comboRa, CB_SETCURSEL, g_recArmMode, 0);

    addLabel("Show sampler:", IDC_LABEL_SHOW);
    HWND comboShow = addCombo(IDC_SHOW_COMBO);
    SendMessageA(comboShow, CB_ADDSTRING, 0, (LPARAM)"Default");
    SendMessageA(comboShow, CB_ADDSTRING, 0, (LPARAM)"Yes");
    SendMessageA(comboShow, CB_ADDSTRING, 0, (LPARAM)"No");
    SendMessageA(comboShow, CB_SETCURSEL, g_showSamplerMode, 0);

    addLabel("If sampler exists:", IDC_LABEL_EXISTING);
    HWND comboEx = addCombo(IDC_EXISTING_COMBO);
    SendMessageA(comboEx, CB_ADDSTRING, 0, (LPARAM)"Add");
    SendMessageA(comboEx, CB_ADDSTRING, 0, (LPARAM)"Replace");
    SendMessageA(comboEx, CB_ADDSTRING, 0, (LPARAM)"Replace on sampler FX slot");
    SendMessageA(comboEx, CB_SETCURSEL, g_existingMode, 0);

    addLabel("Multiple media:", IDC_LABEL_MULTI);
    HWND comboMulti = addCombo(IDC_MULTI_COMBO);
    SendMessageA(comboMulti, CB_ADDSTRING, 0, (LPARAM)"On one track");
    SendMessageA(comboMulti, CB_ADDSTRING, 0, (LPARAM)"On multiple tracks");
    SendMessageA(comboMulti, CB_ADDSTRING, 0, (LPARAM)"Always prompt");
    SendMessageA(comboMulti, CB_SETCURSEL, g_multiFilesMode, 0);

    addLabel("Multiple samplers on one track:", IDC_LABEL_MULTISAMP);
    HWND comboMs = addCombo(IDC_MULTISAMP_COMBO);
    SendMessageA(comboMs, CB_ADDSTRING, 0, (LPARAM)"Layers");
    SendMessageA(comboMs, CB_ADDSTRING, 0, (LPARAM)"Chromatic");
    SendMessageA(comboMs, CB_ADDSTRING, 0, (LPARAM)"Always prompt");
    SendMessageA(comboMs, CB_SETCURSEL, g_multiSamplersMode, 0);

    y += 8;
    HWND chk = CreateWindowA("BUTTON", "Debug",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        left, y, 120, 22, hwnd, (HMENU)(intptr_t)IDC_DEBUG_CHECK, g_hInstance, nullptr);
    SendMessageA(chk, BM_SETCHECK, g_debug ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 28;

    if (!asPrefs)
    {
        CreateWindowA("BUTTON", "OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
            left + comboW - 200, y, 90, 26, hwnd, (HMENU)(intptr_t)IDC_OK, g_hInstance, nullptr);
        CreateWindowA("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            left + comboW - 100, y, 90, 26, hwnd, (HMENU)(intptr_t)IDC_CANCEL, g_hInstance, nullptr);
        y += 36;
        CreateWindowA("STATIC", "",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            left, y, comboW, 48, hwnd, (HMENU)(intptr_t)IDC_HINT, g_hInstance, nullptr);
    }

    SetFontRecursive(hwnd, font);
}




static HWND CreateSettingsPage(HWND parent, bool asPrefsChild)
{
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = g_hInstance ? g_hInstance : GetModuleHandleA(nullptr);
        wc.lpszClassName = "DropToSamplerSettings";
        wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1); // native prefs panel color
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassA(&wc);
        registered = true;
    }

    DWORD style = asPrefsChild
        ? (WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN)
        : (WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE);
    DWORD exStyle = asPrefsChild ? 0 : WS_EX_TOOLWINDOW;

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 400, h = 580;
    if (!asPrefsChild && g_winX != CW_USEDEFAULT)
    {
        x = g_winX;
        y = g_winY;
    }
    if (asPrefsChild)
    {
        x = 0; y = 0;
        RECT rc;
        if (parent && GetClientRect(parent, &rc))
        {
            w = rc.right - rc.left;
            h = rc.bottom - rc.top;
            if (w < 400) w = 400;
            if (h < 500) h = 500;
        }
    }

    HWND hwnd = CreateWindowExA(
        exStyle,
        "DropToSamplerSettings",
        asPrefsChild ? "" : "Drop To Sampler",
        style,
        x, y, w, h,
        parent,
        nullptr,
        g_hInstance ? g_hInstance : GetModuleHandleA(nullptr),
        asPrefsChild ? (LPVOID)1 : nullptr);

    if (hwnd && !asPrefsChild)
        g_settingsHwnd = hwnd;
    return hwnd;
}


static HWND PrefsPageCreate(HWND parent)
{
    return CreatePrefsDialog(parent);
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
    // Open our page inside REAPER Preferences (Plug-ins)
    if (ViewPrefs)
        ViewPrefs(0, "drop_to_sampler");
    else
    {
        // Fallback if ViewPrefs unavailable
        if (g_settingsHwnd && IsWindow(g_settingsHwnd))
        {
            SetForegroundWindow(g_settingsHwnd);
            return;
        }
        // create standalone (legacy)
        CreateSettingsPage(GetMainHwnd ? GetMainHwnd() : nullptr, false);
    }
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
    AppendMenuA(menu, MF_STRING, (UINT_PTR)g_cmdSettings, "Drop To Sampler");
}

// ---------------------------------------------------------------------------
// Timer
// ---------------------------------------------------------------------------
// Timer / OLE install
// ---------------------------------------------------------------------------
static void OnTimer()
{
    g_timerTicks++;
    if (g_timerTicks < 20) return;
    g_timerTicks = 0;

    if (!g_dropTarget)
    {
        g_dropTarget = new TcpDropTarget();
        OleInitialize(nullptr);
    }

    if (!g_tcpHwnd || !IsWindow(g_tcpHwnd))
    {
        if (g_dropTargetRegistered && g_tcpHwnd)
        {
            RevokeDragDrop(g_tcpHwnd);
            g_dropTargetRegistered = false;
        }
        g_tcpHwnd = FindTcpWindow();
        if (g_tcpHwnd)
        {
            if (RegisterDropOnHwnd(g_tcpHwnd, g_dropTarget))
                g_dropTargetRegistered = true;
        }
    }

    if (!g_mcpHwnd || !IsWindow(g_mcpHwnd))
    {
        if (g_mcpDropRegistered && g_mcpHwnd)
        {
            RevokeDragDrop(g_mcpHwnd);
            g_mcpDropRegistered = false;
        }
        g_mcpHwnd = FindMcpWindow();
        if (g_mcpHwnd && g_mcpHwnd != g_tcpHwnd)
        {
            if (RegisterDropOnHwnd(g_mcpHwnd, g_dropTarget))
            {
                g_mcpDropRegistered = true;
                if (g_debug)
                    DebugLog("[Drop To Sampler] MCP drop target registered\n");
            }
            else if (g_debug)
            {
                DebugLog("[Drop To Sampler] MCP RegisterDragDrop failed\n");
            }
        }
    }
}

static void Cleanup()
{
    ShutdownOverlaySystem();
    if (plugin_register)
        plugin_register("-prefpage", (void*)&g_prefsPage);
    if (g_settingsHwnd)
    {
        DestroyWindow(g_settingsHwnd);
        g_settingsHwnd = nullptr;
    }
    g_inOleDrag = false;
    HideOverlay();
    if (g_dropTargetRegistered && g_tcpHwnd)
    {
        RevokeDragDrop(g_tcpHwnd);
        g_dropTargetRegistered = false;
    }
    if (g_dropTarget)
    {
        g_dropTarget->Release();
        g_dropTarget = nullptr;
    }
    if (g_mcpDropRegistered && g_mcpHwnd)
    {
        RevokeDragDrop(g_mcpHwnd);
        g_mcpDropRegistered = false;
    }
    g_tcpHwnd = nullptr;
    g_mcpHwnd = nullptr;
    FreeBrushes();
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
    g_hInstance = hInstance;
    REAPERAPI_LoadAPI(rec->GetFunc);

    if (!GetMainHwnd)
        return 0;

    LoadSettings();
    InitOverlaySystem();

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
