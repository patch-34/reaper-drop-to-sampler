/*
 * Drop To Sampler — Windows platform layer.
 * OLE IDropTarget on TCP/MCP windows + layered PNG overlay (GDI+).
 */

#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"
#include "../resource.h"
#include "../core.h"
#include "../platform.h"

#include <windows.h>
#ifndef MK_ALT
#define MK_ALT 0x20
#endif
#include <ole2.h>
#include <gdiplus.h>
using namespace Gdiplus;
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static HINSTANCE g_hInstance = nullptr;
static HWND g_tcpHwnd = nullptr;
static HWND g_mcpHwnd = nullptr;
static bool g_dropTargetRegistered = false;
static bool g_mcpDropRegistered = false;

// ---------------------------------------------------------------------------
// Paths
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

bool Platform_GetCartridgeDir(char* out, int outSz)
{
    if (!out || outSz <= 0) return false;
    // %APPDATA% as UTF-8 (getenv would give the ANSI code page)
    wchar_t wappdata[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", wappdata, MAX_PATH);
    char appdata[MAX_PATH * 3] = {};
    if (n > 0 && n < MAX_PATH && WideToUtf8(wappdata, appdata, sizeof(appdata)) && appdata[0])
        snprintf(out, outSz, "%s\\Cartridge", appdata);
    else
        snprintf(out, outSz, "Cartridge");
    return true;
}

FILE* Platform_FopenUtf8(const char* path, const char* mode)
{
    wchar_t wpath[MAX_PATH * 2];
    wchar_t wmode[16];
    if (!Utf8ToWide(path, wpath, MAX_PATH * 2) || !Utf8ToWide(mode, wmode, 16))
        return nullptr;
    return _wfopen(wpath, wmode);
}

// ---------------------------------------------------------------------------
// Drag feedback: layered PNG near cursor (no OLE cursor fight)
// ---------------------------------------------------------------------------
static ULONG_PTR g_gdiplusToken = 0;
static bool g_gdiplusInited = false;
static HWND g_insertHwnd = nullptr;
static HBITMAP g_hbmpInsert = nullptr;
static HBITMAP g_hbmpReplace = nullptr;
static int g_insertW = 32, g_insertH = 32;
static int g_replaceW = 32, g_replaceH = 32;

// Draw a GDI+ bitmap into a premultiplied-alpha DIB section
static HBITMAP BitmapToPremultipliedDib(Bitmap* src, int* outW, int* outH)
{
    if (!src || src->GetLastStatus() != Ok)
        return nullptr;

    const int w = (int)src->GetWidth();
    const int h = (int)src->GetHeight();
    if (w <= 0 || h <= 0)
        return nullptr;

    Bitmap dst(w, h, PixelFormat32bppPARGB);
    if (dst.GetLastStatus() != Ok)
        return nullptr;
    {
        Graphics g(&dst);
        g.SetCompositingMode(CompositingModeSourceCopy);
        g.Clear(Color(0, 0, 0, 0));
        g.SetCompositingMode(CompositingModeSourceOver);
        g.DrawImage(src, 0, 0, w, h);
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

static HBITMAP LoadPngToPremultipliedHBitmap(const wchar_t* path, int* outW, int* outH)
{
    if (!path || !path[0])
        return nullptr;
    Bitmap src(path);
    return BitmapToPremultipliedDib(&src, outW, outH);
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
    HBITMAP dib = BitmapToPremultipliedDib(src, outW, outH);
    delete src;
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

    if (Core_IsDebug())
    {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "[Drop To Sampler] overlay insert=%s (%s %dx%d) replace=%s (%s %dx%d)\n",
            g_hbmpInsert ? "OK" : "FAIL", srcInsert, g_insertW, g_insertH,
            g_hbmpReplace ? "OK" : "FAIL", srcReplace, g_replaceW, g_replaceH);
        Core_DebugLog(buf);
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
    if (!ok && Core_IsDebug())
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "[Drop To Sampler] UpdateLayeredWindow failed err=%lu\n", GetLastError());
        Core_DebugLog(buf);
    }

    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void Platform_ShowOverlay(int kind)
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

void Platform_HideOverlay()
{
    if (g_insertHwnd && IsWindow(g_insertHwnd))
        ShowWindow(g_insertHwnd, SW_HIDE);
}

static void ShutdownOverlaySystem()
{
    Platform_HideOverlay();
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
// TCP / MCP windows
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
// IDropTarget
// ---------------------------------------------------------------------------
static DropMods ModsFromKeyState(DWORD grfKeyState)
{
    DropMods m;
    m.shift = (grfKeyState & MK_SHIFT) != 0 || (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    m.ctrl  = (grfKeyState & MK_CONTROL) != 0 || (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    m.alt   = (grfKeyState & MK_ALT) != 0 || (GetKeyState(VK_MENU) & 0x8000) != 0;
    return m;
}

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
        int kind = Core_DragOver(ModsFromKeyState(grfKeyState), pt.x, pt.y, m_fileCount);
        Platform_ShowOverlay(kind);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override
    {
        if (pdwEffect) *pdwEffect = DROPEFFECT_COPY;
        int kind = Core_DragOver(ModsFromKeyState(grfKeyState), pt.x, pt.y, m_fileCount);
        Platform_ShowOverlay(kind);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        Platform_HideOverlay();
        Core_DragEnd();
        m_fileCount = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override
    {
        Platform_HideOverlay();
        Core_DragEnd();
        m_fileCount = 0;
        ProcessDrop(pDataObj, pt.x, pt.y, grfKeyState);
        if (pdwEffect) *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }

private:
    static void ProcessDrop(IDataObject* pDataObj, int x, int y, DWORD grfKeyState)
    {
        if (!pDataObj) return;
        FORMATETC fmt = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM stg = {};
        if (FAILED(pDataObj->GetData(&fmt, &stg)))
        {
            Core_DebugLog("[Drop To Sampler] drop: no CF_HDROP\n");
            return;
        }
        HDROP hDrop = (HDROP)GlobalLock(stg.hGlobal);
        if (!hDrop)
        {
            ReleaseStgMedium(&stg);
            return;
        }

        // UTF-8 paths (REAPER + Cartridge)
        static char files[64][1024];
        const char* ptrs[64];
        int nFiles = 0;
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count && nFiles < 64; ++i)
        {
            wchar_t wpath[1024];
            wpath[0] = 0;
            UINT n = DragQueryFileW(hDrop, i, wpath, 1024);
            files[nFiles][0] = 0;
            if (n > 0 && WideToUtf8(wpath, files[nFiles], 1024) && files[nFiles][0])
            {
                ptrs[nFiles] = files[nFiles];
                ++nFiles;
            }
        }
        GlobalUnlock(stg.hGlobal);
        ReleaseStgMedium(&stg);

        if (nFiles > 0)
            Core_Drop(ptrs, nFiles, x, y, ModsFromKeyState(grfKeyState));
    }

    LONG m_ref;
    int m_fileCount = 0;
};

static TcpDropTarget* g_dropTarget = nullptr;

// ---------------------------------------------------------------------------
// Platform interface
// ---------------------------------------------------------------------------
void Platform_Init(void* hInstance)
{
    g_hInstance = (HINSTANCE)hInstance;
    if (!g_gdiplusInited)
    {
        GdiplusStartupInput in;
        if (GdiplusStartup(&g_gdiplusToken, &in, nullptr) == Ok)
            g_gdiplusInited = true;
    }
    LoadOverlayImages();
}

void Platform_Poll()
{
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
                Core_DebugLog("[Drop To Sampler] MCP drop target registered\n");
            }
            else
            {
                Core_DebugLog("[Drop To Sampler] MCP RegisterDragDrop failed\n");
            }
        }
    }
}

void Platform_Shutdown()
{
    ShutdownOverlaySystem();
    if (g_dropTargetRegistered && g_tcpHwnd)
    {
        RevokeDragDrop(g_tcpHwnd);
        g_dropTargetRegistered = false;
    }
    if (g_mcpDropRegistered && g_mcpHwnd)
    {
        RevokeDragDrop(g_mcpHwnd);
        g_mcpDropRegistered = false;
    }
    if (g_dropTarget)
    {
        g_dropTarget->Release();
        g_dropTarget = nullptr;
    }
    g_tcpHwnd = nullptr;
    g_mcpHwnd = nullptr;
}
