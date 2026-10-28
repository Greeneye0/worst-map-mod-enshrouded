// Live chest state: whether a chest near the player is really empty, read from the game instead of guessed.
//
// A loot chest entity carries ecs::SceneLootNodeId (u32, the same id as the chest's placement in the scene,
// chest_loot_ids.h) and ecs::InventoryEmptyState {bool toggleVFX, bool isEmpty}: isEmpty is 1 once the chest has been
// emptied and goes back to 0 when the game refills it (seen: on a world reload). The client gets it from the server too,
// and chest loot is shared, so it is the chest's state for every player. Only the entities near the player exist.
// Read from the registries built_water.inl learns. Component type numbers change between game sessions, so both are
// recognised by their data: SceneLootNodeId is the size-4 type holding a chest's id on an entity at that chest's place;
// InventoryEmptyState the size-2 type that reads 01 00 or 01 01 on every chest found (and nothing else does).
// A chest that reads empty is marked looted; one marked looted that reads full twice in a row is unmarked, but only
// once it has been read empty this session (the game refilled it). A chest marked by hand while it still has its loot,
// or emptied some way the game doesn't flag (another mod taking the items), stays marked: the respawn rule brings it
// back. These are not told to the other players over chat: a world reload could send dozens, and players nearby read
// the same state.


namespace chest_live {

struct Types {
    void* registry = nullptr;
    uint16_t position = 0, lootId = 0, empty = 0;
};
Types g_types;
std::unordered_map<uint32_t, uint16_t> g_chestByLootId;
uint8_t g_fullReads[kTotalChestCount]{}; // reads of "full" in a row while marked looted
bool g_seenEmpty[kTotalChestCount]{};     // read empty in the game this session
uint64_t g_lastTick = 0, g_lastDetectTick = 0;

bool AtChest(const float p[3], size_t c) {
    const ChestSpawn& sp = kChestSpawns[c];
    const float dx = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f - p[0];
    const float dy = static_cast<float>(sp.py1) + static_cast<float>(sp.py0) / 65536.0f - p[1];
    const float dz = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f - p[2];
    return dx * dx + dy * dy + dz * dz < 9.0f;
}

uint16_t DetectPositionType(uint8_t* reg) {
    int counts[kEcsTypeBits]{};
    int sampled = 0;
    ForEachEcsEntity(reg, [&](uint8_t* rec) {
        if (sampled >= 3000) return;
        ++sampled;
        const auto* bits = *reinterpret_cast<const uint64_t**>(rec + 0x18);
        for (int t = 0; t < kEcsTypeBits; ++t) {
            if (!(bits[t >> 6] >> (t & 63) & 1)) continue;
            float p[3];
            if (EcsPosition(rec, static_cast<uint16_t>(t), p) && PlausibleWorldPos(p)) ++counts[t];
        }
    });
    int best = 0;
    for (int t = 1; t < kEcsTypeBits; ++t) {
        if (counts[t] > counts[best]) best = t;
    }
    return counts[best] >= 100 ? static_cast<uint16_t>(best) : 0;
}

// The chest index whose loot id this size-4 component holds, or kTotalChestCount.
size_t ChestOf(const uint8_t* c) {
    const auto it = g_chestByLootId.find(*reinterpret_cast<const uint32_t*>(c));
    return it == g_chestByLootId.end() ? kTotalChestCount : it->second;
}

// Finds both types in one registry; false while there are not enough chests near the player to be sure.
bool DetectInner(uint8_t* reg, Types& out) {
    out = Types{reg, DetectPositionType(reg), 0, 0};
    if (!out.position) return false;
    // SceneLootNodeId: votes per size-4 type holding a chest's id on an entity at that chest's place
    int votes[kEcsTypeBits]{};
    ForEachEcsEntity(reg, [&](uint8_t* rec) {
        float p[3];
        if (!EcsPosition(rec, out.position, p)) return;
        const auto* bits = *reinterpret_cast<const uint64_t**>(rec + 0x18);
        for (int t = 0; t < kEcsTypeBits; ++t) {
            if (!(bits[t >> 6] >> (t & 63) & 1)) continue;
            uint16_t size = 0;
            const uint8_t* c = EcsComponent(rec, static_cast<uint16_t>(t), &size);
            if (!c || size != 4) continue;
            const size_t chest = ChestOf(c);
            if (chest < kTotalChestCount && AtChest(p, chest)) ++votes[t];
        }
    });
    int best = 0;
    for (int t = 1; t < kEcsTypeBits; ++t) {
        if (votes[t] > votes[best]) best = t;
    }
    if (votes[best] < 3) return false;
    out.lootId = static_cast<uint16_t>(best);
    // InventoryEmptyState: the size-2 types every chest has, reading 01 00 or 01 01 on all of them
    bool ok[kEcsTypeBits];
    bool anyEmpty[kEcsTypeBits]{};
    std::fill(ok, ok + kEcsTypeBits, true);
    int chests = 0;
    ForEachEcsEntity(reg, [&](uint8_t* rec) {
        uint16_t size = 0;
        const uint8_t* id = EcsComponent(rec, out.lootId, &size);
        if (!id || size != 4 || ChestOf(id) >= kTotalChestCount) return;
        ++chests;
        for (int t = 0; t < kEcsTypeBits; ++t) {
            if (!ok[t]) continue;
            const uint8_t* c = EcsComponent(rec, static_cast<uint16_t>(t), &size);
            if (!c || size != 2 || c[0] != 1 || c[1] > 1) {
                ok[t] = false;
                continue;
            }
            anyEmpty[t] = anyEmpty[t] || c[1] == 1;
        }
    });
    int candidates = 0, withEmpty = 0, pick = 0, pickEmpty = 0;
    for (int t = 0; t < kEcsTypeBits; ++t) {
        if (!ok[t]) continue;
        ++candidates;
        pick = t;
        if (anyEmpty[t]) {
            ++withEmpty;
            pickEmpty = t;
        }
    }
    // Two such types flip together on an emptied chest (seen: one chest emptied, both read 01 01 there and 01 00 on
    // the four full ones around it), so "exactly one with an empty chest" never came. Types that read the same on every
    // chest are one answer: take either.
    if (withEmpty > 1) {
        bool agree = true;
        ForEachEcsEntity(reg, [&](uint8_t* rec) {
            uint16_t size = 0;
            const uint8_t* id = EcsComponent(rec, out.lootId, &size);
            if (!agree || !id || size != 4 || ChestOf(id) >= kTotalChestCount) return;
            const uint8_t* p = EcsComponent(rec, static_cast<uint16_t>(pickEmpty), &size);
            for (int t = 0; t < kEcsTypeBits && agree; ++t) {
                if (!ok[t] || !anyEmpty[t] || t == pickEmpty) continue;
                const uint8_t* c = EcsComponent(rec, static_cast<uint16_t>(t), &size);
                agree = c && p && c[1] == p[1];
            }
        });
        if (agree) withEmpty = 1;
    }
    static int loggedCandidates = -1;
    if (candidates != 1 && withEmpty != 1) {
        if (g_log && candidates != loggedCandidates)
            g_log("Live chests: %d chests near, %d possible empty-state types; waiting for an emptied chest", chests,
                  candidates);
        loggedCandidates = candidates;
        return false;
    }
    out.empty = static_cast<uint16_t>(candidates == 1 ? pick : pickEmpty);
    if (g_log)
        g_log("Live chests: registry %p, position type %u, loot id type %u, empty-state type %u (%d chests near)", reg,
              out.position, out.lootId, out.empty, chests);
    return true;
}

bool RegistryLearned(void* reg) {
    for (auto& slot : g_waterRegistries) {
        if (slot.load(std::memory_order_relaxed) == reg) return true;
    }
    return false;
}

// Once a second in a world: reads the chests near the player and fixes their looted marks.
void Update(uint64_t nowTick, uint32_t screen) {
    if (g_lootSection.empty() || nowTick - g_lastTick < 1000ULL) return;
    g_lastTick = nowTick;
    if (g_chestByLootId.empty()) {
        for (size_t c = 0; c < kTotalChestCount; ++c) g_chestByLootId.emplace(kChestLootNodeIds[c], static_cast<uint16_t>(c));
    }
    if (g_types.registry && !RegistryLearned(g_types.registry)) { // another world
        g_types = Types{};
        std::memset(g_fullReads, 0, sizeof(g_fullReads));
        std::memset(g_seenEmpty, 0, sizeof(g_seenEmpty));
    }
    if (!g_types.empty) {
        if (nowTick - g_lastDetectTick < 5000ULL) return;
        g_lastDetectTick = nowTick;
        for (auto& slot : g_waterRegistries) {
            auto* reg = static_cast<uint8_t*>(slot.load(std::memory_order_relaxed));
            if (!reg) continue;
            Types found;
            bool ok = false;
            __try {
                ok = DetectInner(reg, found);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
            if (ok) {
                g_types = found;
                break;
            }
        }
        if (!g_types.empty) return;
    }
    // The chest screen open: the chest being looted is changing
    if (screen == 21) return;
    uint8_t state[kTotalChestCount];
    std::memset(state, 0xff, sizeof(state)); // 0xff not near
    bool ok = true;
    __try {
        const Types t = g_types;
        ForEachEcsEntity(static_cast<uint8_t*>(t.registry), [&](uint8_t* rec) {
            uint16_t size = 0;
            const uint8_t* id = EcsComponent(rec, t.lootId, &size);
            if (!id || size != 4) return;
            const size_t c = ChestOf(id);
            if (c >= kTotalChestCount) return;
            const uint8_t* e = EcsComponent(rec, t.empty, &size);
            if (e && size == 2 && e[0] == 1 && e[1] <= 1) state[c] = e[1];
        });
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (!ok) {
        if (g_log) g_log("Live chests: registry not readable; finding it again");
        g_types = Types{};
        return;
    }
    for (size_t c = 0; c < kTotalChestCount; ++c) {
        auto& st = g_chestLootStates[c];
        if (state[c] == 1) {
            g_fullReads[c] = 0;
            g_seenEmpty[c] = true;
            st.elapsedOutOfBuildZoneSec = 0.0;
            if (!st.looted) {
                st.looted = true;
                g_chestLootDirty = true;
                g_lootSaveDirty = true;
                if (g_log) g_log("Live chests: chest %zu is empty in the game; marked looted", c);
            }
        } else if (state[c] == 0 && st.looted && g_seenEmpty[c]) {
            if (++g_fullReads[c] < 2) continue;
            g_fullReads[c] = 0;
            g_seenEmpty[c] = false;
            st.looted = false;
            st.elapsedOutOfBuildZoneSec = 0.0;
            g_chestLootDirty = true;
            g_lootSaveDirty = true;
            if (g_log) g_log("Live chests: chest %zu has loot in the game again; unmarked", c);
        } else {
            g_fullReads[c] = 0;
        }
    }
}

} // namespace chest_live
