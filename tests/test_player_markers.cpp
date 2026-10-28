#include "../src/player_markers.h"
#include <cassert>
#include <set>
#include <cstdio>
#include <iterator>

int main() {
    minimap_players::Roster roster;
    uint32_t ids[64], colors[64];
    // A player's colour depends only on their id, so every screen agrees on it.
    for (unsigned i = 0; i < 64; ++i) {
        ids[i] = 1 + i * 7919;
        colors[i] = roster.Update(ids[i], 1000, 1000).color;
        assert(colors[i] == minimap_players::ColorFor(ids[i]));
    }
    minimap_players::Roster other;
    for (int i = 63; i >= 0; --i) assert(other.Update(ids[i], 1000, 1000).color == colors[i]);
    assert(minimap_players::ColorFor(1) == 0xFF28D2FFu); // the first player in the server is yellow
    std::set<uint32_t> joined(std::begin(minimap_players::kJoinPalette), std::end(minimap_players::kJoinPalette));
    assert(joined.size() == 16);
    std::set<uint32_t> palette;
    for (unsigned slot = 0; slot < 64; ++slot) palette.insert(minimap_players::Color(slot));
    assert(palette.size() == 64);
    auto& moving = roster.Update(ids[0], 1001, 1000);
    assert(moving.dirX == 1 && moving.dirZ == 0);
    assert(roster.Update(ids[0], 1001, 1000).dirX == 1);
    assert(roster.Update(ids[0], 5000, 5000).dirX == 1); // teleport preserves heading
    roster.Retain(ids, 2);
    assert(roster.Update(ids[0], 5000, 5000).color == colors[0]);
    std::puts("PASS: colours shared by id (order independent), join-order palette, 64 distinct hues, movement/idle/teleport headings");
}
