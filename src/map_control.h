#pragma once
#include <cstdint>

// Versioned, game-thread-only interface for the local development bridge.
struct MapControlState {
    uint32_t size = sizeof(MapControlState);
    uint32_t version = 1;
    uint32_t settingsOpen, panelExpanded, fog, pins, minimap, terrain;
};
// uiClient is supplied by the bridge's current native client (+0x1f0), since
// the renderer's global UI context is only populated during drawing.
using MapControlFn = int (*)(const char* action, const char* value, void* uiClient, MapControlState* state);
