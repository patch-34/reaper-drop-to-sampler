/*
 * Drop To Sampler — macOS platform layer.
 *
 * REAPER on macOS draws its windows with SWELL: every HWND is an NSView
 * (REAPER's SWELL_hwndChild class). File drags arrive through the Cocoa
 * NSDraggingDestination methods of the deepest view registered for file
 * types; SWELL turns a drop into WM_DROPFILES for REAPER.
 *
 * There is no OLE here, so instead of replacing the window's IDropTarget we
 * swizzle the four dragging methods on REAPER's SWELL view class and hit-test
 * every drag: if the cursor is over the TCP or MCP (GetThingFromPoint says
 * "tcp..." / "mcp..."), the drag is ours — status line, overlay, sampler
 * insert, and REAPER never sees it. Anywhere else the original SWELL methods
 * run untouched. This works for the docked and floating mixer alike and does
 * not depend on finding windows by class name.
 */

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <string>
#include <vector>

#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"
#include "../core.h"
#include "../platform.h"
#include "../res/overlay_png.h"

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
static bool DirExists(const char* path)
{
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

FILE* Platform_FopenUtf8(const char* path, const char* mode)
{
    return fopen(path, mode); // macOS paths are UTF-8 already
}

// Cartridge on Windows watches %APPDATA%\Cartridge (JUCE userApplicationDataDirectory).
// The macOS location is not documented, so:
//   1) ExtState DropToSampler/cartridge_dir, if set (escape hatch, no rebuild needed)
//   2) the first existing of ~/Library/Application Support/Cartridge, ~/Library/Cartridge
//   3) ~/Library/Cartridge (what JUCE's userApplicationDataDirectory + "Cartridge" gives on macOS)
bool Platform_GetCartridgeDir(char* out, int outSz)
{
    if (!out || outSz <= 0) return false;
    out[0] = 0;

    if (GetExtState)
    {
        const char* v = GetExtState("DropToSampler", "cartridge_dir");
        if (v && v[0])
        {
            snprintf(out, outSz, "%s", v);
            size_t n = strlen(out);
            while (n > 1 && out[n - 1] == '/') out[--n] = 0;
            return true;
        }
    }

    const char* home = getenv("HOME");
    if (!home || !home[0]) return false;

    char cand[2][1024];
    snprintf(cand[0], sizeof(cand[0]), "%s/Library/Application Support/Cartridge", home);
    snprintf(cand[1], sizeof(cand[1]), "%s/Library/Cartridge", home);
    for (auto& c : cand)
    {
        if (DirExists(c))
        {
            snprintf(out, outSz, "%s", c);
            return true;
        }
    }
    snprintf(out, outSz, "%s", cand[1]);
    return true;
}

// ---------------------------------------------------------------------------
// Overlay: borderless, click-through panel with the PNG near the cursor
// ---------------------------------------------------------------------------
static NSPanel* g_overlay = nil;
static NSImageView* g_overlayView = nil;
static NSImage* g_imgInsert = nil;
static NSImage* g_imgReplace = nil;
static int g_overlayKind = -1;

static std::string ModuleDir()
{
    Dl_info info = {};
    if (dladdr((const void*)&Platform_Init, &info) && info.dli_fname)
    {
        std::string p = info.dli_fname;
        size_t slash = p.rfind('/');
        if (slash != std::string::npos) return p.substr(0, slash);
    }
    return std::string();
}

// Optional PNG next to the dylib or in REAPER/UserPlugins overrides the embedded one
static NSImage* LoadOverlayImage(const char* fileName, const unsigned char* data, size_t size, const char** srcOut)
{
    std::vector<std::string> paths;
    std::string mod = ModuleDir();
    if (!mod.empty())
    {
        paths.push_back(mod + "/" + fileName);
        paths.push_back(mod + "/res/" + fileName);
    }
    if (GetResourcePath)
    {
        const char* rp = GetResourcePath();
        if (rp && rp[0]) paths.push_back(std::string(rp) + "/UserPlugins/" + fileName);
    }
    for (const std::string& p : paths)
    {
        NSImage* img = [[NSImage alloc] initWithContentsOfFile:[NSString stringWithUTF8String:p.c_str()]];
        if (img)
        {
            *srcOut = "file";
            return img;
        }
    }
    *srcOut = "embedded";
    return [[NSImage alloc] initWithData:[NSData dataWithBytes:data length:size]];
}

static void EnsureOverlay()
{
    if (!g_imgInsert || !g_imgReplace)
    {
        const char* srcI = "";
        const char* srcR = "";
        if (!g_imgInsert)
            g_imgInsert = LoadOverlayImage("sampler_insert.png", k_sampler_insert_png, sizeof(k_sampler_insert_png), &srcI);
        if (!g_imgReplace)
            g_imgReplace = LoadOverlayImage("sampler_replace.png", k_sampler_replace_png, sizeof(k_sampler_replace_png), &srcR);
        if (Core_IsDebug())
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "[Drop To Sampler] overlay insert=%s (%s) replace=%s (%s)\n",
                g_imgInsert ? "OK" : "FAIL", srcI, g_imgReplace ? "OK" : "FAIL", srcR);
            Core_DebugLog(buf);
        }
    }
    if (g_overlay) return;

    g_overlay = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 32, 32)
                                           styleMask:(NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel)
                                             backing:NSBackingStoreBuffered
                                               defer:YES];
    [g_overlay setReleasedWhenClosed:NO];
    [g_overlay setOpaque:NO];
    [g_overlay setBackgroundColor:[NSColor clearColor]];
    [g_overlay setHasShadow:NO];
    [g_overlay setIgnoresMouseEvents:YES];
    [g_overlay setHidesOnDeactivate:NO];
    [g_overlay setLevel:NSPopUpMenuWindowLevel];
    [g_overlay setCollectionBehavior:(NSWindowCollectionBehaviorCanJoinAllSpaces |
                                      NSWindowCollectionBehaviorTransient |
                                      NSWindowCollectionBehaviorIgnoresCycle)];

    g_overlayView = [[NSImageView alloc] initWithFrame:NSMakeRect(0, 0, 32, 32)];
    [g_overlayView setImageScaling:NSImageScaleNone];
    [g_overlayView setImageFrameStyle:NSImageFrameNone];
    [g_overlay setContentView:g_overlayView];
}

void Platform_ShowOverlay(int kind)
{
    EnsureOverlay();
    NSImage* img = (kind == DropVisual_Replace && g_imgReplace) ? g_imgReplace : g_imgInsert;
    if (!g_overlay || !img)
    {
        Platform_HideOverlay();
        return;
    }

    if (kind != g_overlayKind)
    {
        [g_overlayView setImage:img];
        g_overlayKind = kind;
    }

    // Same placement as Windows: 16 px right of and below the cursor (Cocoa y grows upward)
    NSSize sz = [img size];
    NSPoint m = [NSEvent mouseLocation];
    NSRect frame = NSMakeRect(m.x + 16.0, m.y - 16.0 - sz.height, sz.width, sz.height);
    [g_overlay setFrame:frame display:NO];
    [g_overlayView setFrame:NSMakeRect(0, 0, sz.width, sz.height)];
    if (![g_overlay isVisible])
        [g_overlay orderFrontRegardless];
    else
        [g_overlayView setNeedsDisplay:YES];
}

void Platform_HideOverlay()
{
    if (g_overlay && [g_overlay isVisible])
        [g_overlay orderOut:nil];
}

// ---------------------------------------------------------------------------
// Drag state
// ---------------------------------------------------------------------------
static bool g_dragIsOurs = false;   // current drag is over TCP/MCP and handled by us
static int g_dragFileCount = 0;

static DropMods CurrentMods()
{
    NSEventModifierFlags f = [NSEvent modifierFlags];
    DropMods m;
    m.shift = (f & NSEventModifierFlagShift) != 0;
    // "Ctrl" role: Cmd (REAPER/SWELL convention) or the physical Control key
    m.ctrl = (f & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) != 0;
    m.alt = (f & NSEventModifierFlagOption) != 0;
    return m;
}

// Drag location in REAPER/SWELL screen coordinates (Cocoa global, y up — what GetCursorPos returns)
static void DragScreenPoint(NSView* view, id<NSDraggingInfo> sender, int* x, int* y)
{
    NSPoint p = [sender draggingLocation]; // window base coordinates
    NSWindow* w = [view window];
    if (w)
    {
        NSRect r = [w convertRectToScreen:NSMakeRect(p.x, p.y, 0, 0)];
        p = r.origin;
    }
    else
    {
        p = [NSEvent mouseLocation];
    }
    *x = (int)floor(p.x + 0.5);
    *y = (int)floor(p.y + 0.5);
}

static std::vector<std::string> DraggedFiles(id<NSDraggingInfo> sender)
{
    std::vector<std::string> out;
    NSPasteboard* pb = [sender draggingPasteboard];

    NSArray* urls = [pb readObjectsForClasses:@[ [NSURL class] ]
                                      options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
    for (NSURL* u in urls)
    {
        const char* s = [[u path] UTF8String];
        if (s && s[0]) out.push_back(s);
    }
    if (out.empty())
    {
        // Legacy list (SWELL_InitiateDragDropOfFileList, i.e. REAPER's Media Explorer)
        id list = [pb propertyListForType:NSFilenamesPboardType];
        if ([list isKindOfClass:[NSArray class]])
        {
            for (id s in (NSArray*)list)
            {
                if ([s isKindOfClass:[NSString class]])
                {
                    const char* c = [(NSString*)s UTF8String];
                    if (c && c[0]) out.push_back(c);
                }
            }
        }
    }
    return out;
}

static int DraggedFileCount(id<NSDraggingInfo> sender)
{
    NSPasteboard* pb = [sender draggingPasteboard];
    NSArray* urls = [pb readObjectsForClasses:@[ [NSURL class] ]
                                      options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
    if ([urls count]) return (int)[urls count];
    id list = [pb propertyListForType:NSFilenamesPboardType];
    if ([list isKindOfClass:[NSArray class]]) return (int)[(NSArray*)list count];
    return 0;
}

// Is the point over the track control panel or the mixer?
static bool PointIsTcpOrMcp(int x, int y, char* infoOut, int infoSz)
{
    if (infoOut && infoSz > 0) infoOut[0] = 0;
    if (!GetThingFromPoint) return false;
    char info[256] = {};
    GetThingFromPoint(x, y, info, sizeof(info));
    if (infoOut) snprintf(infoOut, infoSz, "%s", info);
    return strncmp(info, "tcp", 3) == 0 || strncmp(info, "mcp", 3) == 0;
}

// Pick an operation the source allows (Cmd/Option change the source mask during a drag)
static NSDragOperation PickOperation(id<NSDraggingInfo> sender)
{
    NSDragOperation mask = [sender draggingSourceOperationMask];
    if (mask & NSDragOperationCopy) return NSDragOperationCopy;
    if (mask & NSDragOperationGeneric) return NSDragOperationGeneric;
    if (mask & NSDragOperationLink) return NSDragOperationLink;
    if (mask & NSDragOperationMove) return NSDragOperationMove;
    return NSDragOperationCopy;
}

static void EndOurDrag()
{
    if (!g_dragIsOurs) return;
    g_dragIsOurs = false;
    g_dragFileCount = 0;
    Platform_HideOverlay();
    Core_DragEnd();
}

// ---------------------------------------------------------------------------
// Method swizzling on REAPER's SWELL view class
//
// Only classes that themselves implement SWELL's drop handling are hooked, with
// method_setImplementation (never class_addMethod), and never two classes from
// the same inheritance chain. So every hooked call has exactly one original to
// forward to, and a [super ...] call from an unhooked subclass cannot recurse.
// ---------------------------------------------------------------------------
typedef NSDragOperation (*DragOpIMP)(id, SEL, id);
typedef void (*DragVoidIMP)(id, SEL, id);
typedef BOOL (*DragBoolIMP)(id, SEL, id);

struct HookedClass
{
    Class cls;
    IMP origEntered, origUpdated, origExited, origPerform; // nullptr = not hooked on this class
};

static std::vector<HookedClass> g_hooked;

static const HookedClass* HookFor(id self)
{
    for (Class c = object_getClass(self); c; c = class_getSuperclass(c))
        for (const HookedClass& h : g_hooked)
            if (h.cls == c) return &h;
    return nullptr;
}

static NSDragOperation CallOrigOp(IMP imp, id self, SEL sel, id sender)
{
    return imp ? ((DragOpIMP)imp)(self, sel, sender) : NSDragOperationNone;
}

// Hover: ownership is decided on every update, so a drag can cross TCP <-> arrange inside one view tree
static NSDragOperation HandleHover(id self, SEL sel, id<NSDraggingInfo> sender, bool entering)
{
    const HookedClass* h = HookFor(self);
    IMP origEnter = h ? h->origEntered : nullptr;
    IMP origUpdate = h ? h->origUpdated : nullptr;
    IMP origExit = h ? h->origExited : nullptr;

    int x = 0, y = 0;
    DragScreenPoint((NSView*)self, sender, &x, &y);
    char info[256];
    const bool over = PointIsTcpOrMcp(x, y, info, sizeof(info));

    if (over)
    {
        if (!g_dragIsOurs)
        {
            // Taking over from REAPER's own handling (e.g. moved from arrange onto TCP): let it clean up
            if (!entering && origExit)
                ((DragVoidIMP)origExit)(self, @selector(draggingExited:), sender);
            g_dragIsOurs = true;
            g_dragFileCount = DraggedFileCount(sender);
            if (Core_IsDebug())
            {
                char buf[400];
                snprintf(buf, sizeof(buf), "[Drop To Sampler] drag over '%s' (%d file(s)) view=%s x=%d y=%d\n",
                    info, g_dragFileCount, class_getName(object_getClass(self)), x, y);
                Core_DebugLog(buf);
            }
        }
        if (g_dragFileCount <= 0)
        {
            EndOurDrag();
            return NSDragOperationNone;
        }
        int kind = Core_DragOver(CurrentMods(), x, y, g_dragFileCount);
        Platform_ShowOverlay(kind);
        return PickOperation(sender);
    }

    if (g_dragIsOurs)
    {
        // Moved off TCP/MCP: hand the drag back to REAPER
        EndOurDrag();
        return CallOrigOp(origEnter, self, @selector(draggingEntered:), sender);
    }
    return CallOrigOp(entering ? origEnter : origUpdate, self, sel, sender);
}

static NSDragOperation Hook_draggingEntered(id self, SEL sel, id sender)
{
    return HandleHover(self, sel, sender, true);
}

static NSDragOperation Hook_draggingUpdated(id self, SEL sel, id sender)
{
    return HandleHover(self, sel, sender, false);
}

static void Hook_draggingExited(id self, SEL sel, id sender)
{
    if (g_dragIsOurs)
    {
        EndOurDrag();
        return;
    }
    const HookedClass* h = HookFor(self);
    if (h && h->origExited)
        ((DragVoidIMP)h->origExited)(self, sel, sender);
}

static BOOL Hook_performDragOperation(id self, SEL sel, id sender)
{
    int x = 0, y = 0;
    DragScreenPoint((NSView*)self, sender, &x, &y);

    if (g_dragIsOurs || PointIsTcpOrMcp(x, y, nullptr, 0))
    {
        EndOurDrag();
        const std::vector<std::string> files = DraggedFiles(sender);
        if (files.empty())
            return NO;
        const DropMods mods = CurrentMods();

        // Run after Cocoa has finished the drag session: Core_Drop may show modal
        // prompts, which should not run inside the drag loop.
        dispatch_async(dispatch_get_main_queue(), ^{
            std::vector<const char*> p;
            p.reserve(files.size());
            for (const std::string& f : files) p.push_back(f.c_str());
            Core_Drop(p.data(), (int)p.size(), x, y, mods);
        });
        return YES; // REAPER never sees this drop (no WM_DROPFILES -> no media item)
    }

    const HookedClass* h = HookFor(self);
    if (h && h->origPerform)
        return ((DragBoolIMP)h->origPerform)(self, sel, sender);
    return NO;
}

static bool ClassDefinesMethod(Class cls, SEL sel)
{
    unsigned int n = 0;
    Method* list = class_copyMethodList(cls, &n);
    bool found = false;
    for (unsigned int i = 0; i < n && !found; ++i)
        if (method_getName(list[i]) == sel) found = true;
    free(list);
    return found;
}

static IMP HookOne(Class cls, SEL sel, IMP newImp)
{
    if (!ClassDefinesMethod(cls, sel)) return nullptr;
    Method m = class_getInstanceMethod(cls, sel);
    if (!m) return nullptr;
    return method_setImplementation(m, newImp);
}

static void UnhookOne(Class cls, SEL sel, IMP orig)
{
    if (!orig) return;
    Method m = class_getInstanceMethod(cls, sel);
    if (m) method_setImplementation(m, orig);
}

// Most-derived class (from the view up) that implements SWELL's drop handling
static Class SwellDropClassFor(NSView* view)
{
    for (Class c = object_getClass(view); c && c != [NSView class]; c = class_getSuperclass(c))
    {
        if (ClassDefinesMethod(c, @selector(swellExtendedDragOp:retGlob:)) ||
            ClassDefinesMethod(c, @selector(performDragOperation:)) ||
            ClassDefinesMethod(c, @selector(draggingUpdated:)))
            return c;
    }
    return nil;
}

static void HookClass(Class cls, const char* what)
{
    if (!cls) return;
    for (const HookedClass& h : g_hooked)
    {
        if (h.cls == cls) return;
        if ([cls isSubclassOfClass:h.cls] || [h.cls isSubclassOfClass:cls])
        {
            if (Core_IsDebug())
            {
                char buf[300];
                snprintf(buf, sizeof(buf), "[Drop To Sampler] %s view class %s is related to hooked %s - not hooking twice\n",
                    what, class_getName(cls), class_getName(h.cls));
                Core_DebugLog(buf);
            }
            return;
        }
    }

    HookedClass h = {};
    h.cls = cls;
    h.origEntered = HookOne(cls, @selector(draggingEntered:), (IMP)Hook_draggingEntered);
    h.origUpdated = HookOne(cls, @selector(draggingUpdated:), (IMP)Hook_draggingUpdated);
    h.origExited  = HookOne(cls, @selector(draggingExited:), (IMP)Hook_draggingExited);
    h.origPerform = HookOne(cls, @selector(performDragOperation:), (IMP)Hook_performDragOperation);
    g_hooked.push_back(h);

    if (Core_IsDebug())
    {
        char buf[300];
        snprintf(buf, sizeof(buf), "[Drop To Sampler] drag hooks on %s (from %s view): entered=%d updated=%d exited=%d perform=%d\n",
            class_getName(cls), what, h.origEntered != nullptr, h.origUpdated != nullptr,
            h.origExited != nullptr, h.origPerform != nullptr);
        Core_DebugLog(buf);
    }
}

static void UnhookAll()
{
    for (const HookedClass& h : g_hooked)
    {
        UnhookOne(h.cls, @selector(draggingEntered:), h.origEntered);
        UnhookOne(h.cls, @selector(draggingUpdated:), h.origUpdated);
        UnhookOne(h.cls, @selector(draggingExited:), h.origExited);
        UnhookOne(h.cls, @selector(performDragOperation:), h.origPerform);
    }
    g_hooked.clear();
}

// ---------------------------------------------------------------------------
// TCP / MCP views: make sure the class is hooked and the view accepts file drags
// ---------------------------------------------------------------------------
static HWND FindChildByClassDeep(HWND parent, const char* cls)
{
    if (!parent) return nullptr;
    HWND h = FindWindowEx(parent, nullptr, cls, nullptr);
    if (h) return h;
    for (HWND c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
    {
        HWND r = FindChildByClassDeep(c, cls);
        if (r) return r;
    }
    return nullptr;
}

static void EnsureViewAcceptsFiles(NSView* v, const char* what)
{
    if (!v || ![v isKindOfClass:[NSView class]]) return;
    if ([[v registeredDraggedTypes] count]) return;
    // REAPER did not register this view for drags: register it so Cocoa delivers file drags here
    [v registerForDraggedTypes:@[ NSPasteboardTypeFileURL, NSFilenamesPboardType ]];
    if (Core_IsDebug())
    {
        char buf[200];
        snprintf(buf, sizeof(buf), "[Drop To Sampler] registered %s view for file drags\n", what);
        Core_DebugLog(buf);
    }
}

static void HookView(HWND hwnd, const char* what)
{
    if (!hwnd) return;
    NSView* v = (__bridge NSView*)hwnd;
    if (![v isKindOfClass:[NSView class]]) return;
    HookClass(SwellDropClassFor(v), what);
    if (strcmp(what, "main") != 0)
        EnsureViewAcceptsFiles(v, what);
}

static HWND FindMcpView(HWND main)
{
    // Mixer: docked (inside main or a docker window) or floating (own NSWindow)
    HWND mcp = FindChildByClassDeep(main, "REAPERMCPDisplay");
    if (mcp) return mcp;
    for (NSWindow* w in [NSApp windows])
    {
        NSView* cv = [w contentView];
        if (!cv) continue;
        mcp = FindChildByClassDeep((__bridge HWND)cv, "REAPERMCPDisplay");
        if (mcp) return mcp;
    }
    return nullptr;
}

void Platform_Init(void* hInstance)
{
    (void)hInstance;
    EnsureOverlay();
}

void Platform_Poll()
{
    if (!GetMainHwnd) return;
    HWND main = GetMainHwnd();
    if (!main) return;

    // REAPER's main view: its class is the SWELL view class REAPER's windows share
    static bool s_mainHooked = false;
    if (!s_mainHooked)
    {
        HookView(main, "main");
        s_mainHooked = true;
    }

    static HWND s_tcp = nullptr, s_mcp = nullptr;
    if (!s_tcp || !IsWindow(s_tcp))
    {
        s_tcp = FindChildByClassDeep(main, "REAPERTCPDisplay");
        if (s_tcp) HookView(s_tcp, "TCP");
    }
    if (!s_mcp || !IsWindow(s_mcp))
    {
        s_mcp = FindMcpView(main);
        if (s_mcp) HookView(s_mcp, "MCP");
    }

    static int s_reported = -1;
    const int state = (s_tcp ? 1 : 0) | (s_mcp ? 2 : 0);
    if (Core_IsDebug() && state != s_reported)
    {
        s_reported = state;
        char buf[256];
        snprintf(buf, sizeof(buf), "[Drop To Sampler] macOS: TCP view %s, MCP view %s\n",
            s_tcp ? "found" : "not found (hit-test only)", s_mcp ? "found" : "not found (hit-test only)");
        Core_DebugLog(buf);
    }
}

void Platform_Shutdown()
{
    EndOurDrag();
    UnhookAll();
    if (g_overlay)
    {
        [g_overlay orderOut:nil];
        [g_overlay close];
    }
    g_overlay = nil;
    g_overlayView = nil;
    g_imgInsert = nil;
    g_imgReplace = nil;
    g_overlayKind = -1;
}
