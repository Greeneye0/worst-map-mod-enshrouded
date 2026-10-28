#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace minimap_players {
struct Marker {
    uint32_t id, color;
    unsigned slot;
    float x, z, dirX = 0, dirZ = 1;
};

// 64 distinct bright colors around the whole hue circle.
inline uint32_t Color(unsigned slot) {
    const float hue = static_cast<float>((slot * 23) % 64) * (360.0f / 64.0f) / 60.0f;
    const float c = 0.62f, m = 0.33f;
    const float x = c * (1.0f - std::abs(std::fmod(hue, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    if (hue < 1) { r = c; g = x; }
    else if (hue < 2) { r = x; g = c; }
    else if (hue < 3) { g = c; b = x; }
    else if (hue < 4) { g = x; b = c; }
    else if (hue < 5) { r = x; b = c; }
    else { r = c; b = x; }
    return 0xff000000u | (static_cast<uint32_t>((b + m) * 255) << 16) |
        (static_cast<uint32_t>((g + m) * 255) << 8) | static_cast<uint32_t>((r + m) * 255);
}

// Every player's colour, the same on every screen (the local player's own arrow too), so
// players can tell each other apart by colour. The server hands out player ids in join order
// (the first player is 1), so the first player in the server is yellow, the second blue, and
// so on; past 16 players, hues from Color(). ABGR. Town Portal colours portals with the same
// palette (TownPortal src/portal_colors.h); keep the two in step.
inline constexpr uint32_t kJoinPalette[16] = {
    0xFF28D2FFu /* yellow */,
    0xFFFF7828u /* blue */,
    0xFF3232EBu /* red */,
    0xFF50C83Cu /* green */,
    0xFFE650A0u /* purple */,
    0xFF1E82FFu /* orange */,
    0xFFDCD228u /* cyan */,
    0xFFAA5AF0u /* pink */,
    0xFFEBEBEBu /* white */,
    0xFF28E6AAu /* lime */,
    0xFF8C9614u /* teal */,
    0xFFD228D2u /* magenta */,
    0xFF325F96u /* brown */,
    0xFFA03228u /* navy */,
    0xFF288282u /* olive */,
    0xFF321E8Cu /* maroon */};
inline unsigned SlotFor(uint32_t id) { return (id * 2654435761u) % 64; }
inline uint32_t ColorFor(uint32_t id) {
    if (id >= 1 && id <= 16) return kJoinPalette[id - 1];
    return Color(SlotFor(id));
}

class Roster {
    std::vector<Marker> entries;
public:
    void Retain(const uint32_t* ids, size_t count) {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Marker& m) {
            return std::find(ids, ids + count, m.id) == ids + count;
        }), entries.end());
    }
    Marker& Update(uint32_t id, float x, float z) {
        for (auto& m : entries) if (m.id == id) {
            const float dx = x - m.x, dz = z - m.z;
            const float lengthSq = dx * dx + dz * dz;
            // Movement heading is available for remote players; keep it while stationary.
            // Ignore teleport jumps and accumulate tiny movements before updating it.
            if (lengthSq > 0.0025f && lengthSq < 100.0f) {
                const float inv = 1.0f / std::sqrt(lengthSq);
                m.dirX = dx * inv; m.dirZ = dz * inv;
                m.x = x; m.z = z;
            } else if (lengthSq >= 100.0f) { m.x = x; m.z = z; }
            return m;
        }
        // No probing for a free slot: that would depend on who this screen saw first.
        const unsigned slot = SlotFor(id);
        entries.push_back({id, ColorFor(id), slot, x, z});
        return entries.back();
    }
};
}
