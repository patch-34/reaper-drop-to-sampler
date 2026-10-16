#pragma once
/*
 * Drop To Sampler — platform layer interface.
 *
 * core.cpp holds everything that talks to REAPER (samplers, tracks, settings,
 * prefs page). The platform layer only:
 *   - intercepts file drags/drops on the TCP and MCP windows,
 *   - draws the drag overlay icon near the cursor,
 *   - knows OS-specific paths.
 *
 * Implementations: win/platform_win.cpp, mac/platform_mac.mm
 */

#include <cstdio>

// Modifier names shown in the status line ("Ctrl toggles" / "Cmd toggles")
#ifdef _WIN32
#define DTS_MOD_CTRL_NAME "Ctrl"
#define DTS_MOD_ALT_NAME  "Alt"
#else
#define DTS_MOD_CTRL_NAME "Cmd"
#define DTS_MOD_ALT_NAME  "Opt"
#endif

// Called once from ReaperPluginEntry after the REAPER API is loaded.
void Platform_Init(void* hInstance);

// Called from the REAPER timer (~1 s): find TCP/MCP windows, (re)install drop hooks.
void Platform_Poll();

// Called on unload: remove hooks, destroy overlay.
void Platform_Shutdown();

// Overlay near the cursor during a drag (kind = DropVisualKind from core.h)
void Platform_ShowOverlay(int kind);
void Platform_HideOverlay();

// Directory Cartridge watches for pending_load.txt (UTF-8, no trailing slash).
// Returns false if unknown on this platform.
bool Platform_GetCartridgeDir(char* out, int outSz);

// fopen with a UTF-8 path
FILE* Platform_FopenUtf8(const char* path, const char* mode);
