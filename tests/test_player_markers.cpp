#include "../src/player_markers.h"
#include <cassert>
#include <set>
#include <cstdio>

int main() {
    minimap_players::Roster roster;
    uint32_t ids[64], colors[64];
    std::set<uint32_t> unique;
    // All IDs collide in their initial palette slot: allocation must still be unique.
    for (unsigned i = 0; i < 64; ++i) {
        ids[i] = 1 + i * 64;
        colors[i] = roster.Update(ids[i], 1000, 1000).color;
        assert(unique.insert(colors[i]).second);
    }
    for (int i = 63; i >= 0; --i) assert(roster.Update(ids[i], 1000, 1000).color == colors[i]);
    auto& moving = roster.Update(ids[0], 1001, 1000);
    assert(moving.dirX == 1 && moving.dirZ == 0);
    assert(roster.Update(ids[0], 1001, 1000).dirX == 1);
    assert(roster.Update(ids[0], 5000, 5000).dirX == 1); // teleport preserves heading
    roster.Retain(ids, 2);
    assert(roster.Update(ids[0], 5000, 5000).color == colors[0]);
    assert(roster.Update(9999, 0, 0).color != colors[1]);
    std::puts("PASS: 64 unique colors, stable ordering, movement/idle/teleport headings, roster churn");
}
