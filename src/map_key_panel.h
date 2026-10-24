#pragma once

#include <cstdint>

namespace unlock_map_markers {

using LogFn = void (*)(const char* fmt, ...);

// State that outlives a hot reload (Greeneye Loader host->keep): the fog mode patched into the
// game's code.
struct KeptState {
    uint32_t valid;
    int fogMode;
};

// Resolves the UI signatures and hooks the Map Screen (drawMapFilterMenu), world map input, the
// marker type lookup and the HUD (doPlayStateUi). Call once, while the mod starts: under the
// Greeneye Loader hooks can only be installed then, and they stay for the life of the process.
bool InstallMapKeyPanelHook(LogFn logger);

// Under the Greeneye Loader: the kept state, before InstallMapKeyPanelHook.
void UseKeptState(KeptState* kept);

// Before a newer generation takes over: give back the window procedure and the mouse, and close
// the Find dialog.
void PrepareMapKeyPanelForReload();

// main.cpp: the four marker unlock byte patches on (the mod's bytes) or off (the game's own).
void SetMarkerUnlockPatches(bool on);

} // namespace unlock_map_markers
