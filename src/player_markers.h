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

// 64 distinct bright colors, leaving the local player's gold hue out of the range.
inline uint32_t Color(unsigned slot) {
    const float hue = std::fmod(85.0f + static_cast<float>((slot * 23) % 64) * (300.0f / 64.0f), 360.0f) / 60.0f;
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
        unsigned slot = (id * 2654435761u) % 64;
        for (unsigned tries = 0; tries < 64; ++tries) {
            if (std::none_of(entries.begin(), entries.end(), [&](const Marker& m) { return m.slot == slot; })) break;
            slot = (slot + 1) % 64;
        }
        entries.push_back({id, Color(slot), slot, x, z});
        return entries.back();
    }
};
}
