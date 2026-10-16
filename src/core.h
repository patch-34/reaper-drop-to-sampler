#pragma once
/*
 * Drop To Sampler — platform-neutral core (see core.cpp).
 * Used by the platform layers to hand over drag/drop events.
 */

enum DropVisualKind { DropVisual_Insert = 0, DropVisual_Replace = 1 };

struct DropMods
{
    bool shift; // one track <-> multiple tracks
    bool ctrl;  // Layers <-> Chromatic   (macOS: Cmd)
    bool alt;   // Add <-> Replace        (macOS: Option)
};

// Drag is over TCP/MCP: update status line. Returns DropVisualKind for the overlay.
// screenX/screenY are in REAPER screen coordinates (the ones GetThingFromPoint takes).
int  Core_DragOver(const DropMods& mods, int screenX, int screenY, int fileCount);

// Drag left the window or was dropped: clear status line.
void Core_DragEnd();

// Files dropped. Paths are UTF-8.
void Core_Drop(const char* const* files, int count, int screenX, int screenY, const DropMods& mods);

bool Core_IsDebug();
void Core_DebugLog(const char* msg);
