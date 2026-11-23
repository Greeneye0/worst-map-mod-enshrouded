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
// A chest blown up (barrels beside it, GillzaSVD 1.3.54) never reads empty: the server's destructible system dissolves
// the entity and it is gone (re atlas loot.md). So a chest seen this session that is gone while it should still be sent
// to the player is marked looted too: within 90 m and 10 m closer than a chest of its own kind that is still there,
// 3 reads in a row, not just after a teleport. The server sends an entity by its 2D distance to the player, 100 m or
// 200 m by its template, scaled down under load (memory: entity relevance, send radius factor); a chest of the same kind
// has the same template, so one still there farther away means this one would be sent too. It comes back like an
// emptied one: read full twice after the area reset.


namespace chest_live {

struct Types {
    void* registry = nullptr;
    uint16_t position = 0, lootId = 0, empty = 0;
};
Types g_types;
std::unordered_map<uint32_t, uint16_t> g_chestByLootId;
uint8_t g_fullReads[kTotalChestCount]{}; // reads of "full" in a row while marked looted
bool g_seenEmpty[kTotalChestCount]{};     // read empty in the game this session
bool g_seenHere[kTotalChestCount]{};      // its entity read this session (full or empty)
uint8_t g_goneReads[kTotalChestCount]{};  // reads in a row it was gone while in the player's reach
float g_lastPlayerXZ[2] = {};
#ifdef UMM_DEV_BRIDGE
long g_devHideChest = -1; // dev (map op "chestgone" <index>): that chest's entity read as gone, as if blown up (-1 none)
#endif
uint64_t g_settleUntil = 0; // after a teleport (or a world's first read): the new place's entities are still coming
uint64_t g_lastTick = 0, g_lastDetectTick = 0;

// The chest's place, across the ground (x, z)
float ChestDistXZ(size_t c, float px, float pz) {
    const ChestSpawn& sp = kChestSpawns[c];
    const float dx = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f - px;
    const float dz = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f - pz;
    return std::sqrt(dx * dx + dz * dz);
}

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

// The clams' loot ids aren't in the mod's data (chest_loot_ids.h has 0): a loot entity standing exactly on a clam's
// place (within half a metre) is that clam, and its id is kept for the session. Ids already looked at and not a clam's
// aren't looked at again.
std::unordered_set<uint32_t> g_notClamIds;
size_t LearnClam(uint8_t* rec, const uint8_t* id, uint16_t positionType) {
    const uint32_t v = *reinterpret_cast<const uint32_t*>(id);
    if (v == 0 || g_notClamIds.count(v)) return kTotalChestCount;
    float p[3];
    size_t found = kTotalChestCount;
    // (the nearest within 1 m: the closest two clams are 2.5 m apart, and a chest can settle onto the ground a little)
    if (EcsPosition(rec, positionType, p) && p[0] > 3000.0f && p[0] < 6100.0f && p[2] > 4900.0f && p[2] < 7100.0f) {
        float best = 1.0f;
        for (size_t c = kClamFirst; c < kClamFirst + kClamCount; ++c) {
            const ChestSpawn& sp = kChestSpawns[c];
            const float dx = static_cast<float>(sp.px1) + static_cast<float>(sp.px0) / 65536.0f - p[0];
            const float dy = static_cast<float>(sp.py1) + static_cast<float>(sp.py0) / 65536.0f - p[1];
            const float dz = static_cast<float>(sp.pz1) + static_cast<float>(sp.pz0) / 65536.0f - p[2];
            if (dx * dx + dy * dy + dz * dz < best) {
                best = dx * dx + dy * dy + dz * dz;
                found = c;
            }
        }
    }
    if (found == kTotalChestCount) {
        if (g_notClamIds.size() < 200000) g_notClamIds.insert(v);
        return found;
    }
    g_chestByLootId.emplace(v, static_cast<uint16_t>(found));
    if (g_log) g_log("Live chests: clam %zu has loot id %08x", found - kClamFirst, v);
    return found;
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
void Update(uint64_t nowTick, uint32_t screen, uint8_t* client) {
    if (g_lootSection.empty() || nowTick - g_lastTick < 1000ULL) return;
    g_lastTick = nowTick;
    if (g_chestByLootId.empty()) {
        for (size_t c = 0; c < kTotalChestCount; ++c)
            if (kChestLootNodeIds[c]) g_chestByLootId.emplace(kChestLootNodeIds[c], static_cast<uint16_t>(c));
    }
    if (g_types.registry && !RegistryLearned(g_types.registry)) { // another world
        g_types = Types{};
        g_notClamIds.clear();
        std::memset(g_fullReads, 0, sizeof(g_fullReads));
        std::memset(g_seenEmpty, 0, sizeof(g_seenEmpty));
        std::memset(g_seenHere, 0, sizeof(g_seenHere));
        std::memset(g_goneReads, 0, sizeof(g_goneReads));
    }
    // Until an emptied chest is near, the empty-state type isn't known; the chests' id type is, and with it which chests
    // are there (a blown-up one gone): read with that while the empty state is looked for again every 5 s
    if (!g_types.empty && nowTick - g_lastDetectTick >= 5000ULL) {
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
                found = Types{};
            }
            if (ok) {
                g_types = found;
                break;
            }
            if (found.lootId && found.position && !g_types.lootId) {
                g_types = found; // (empty 0: presence only)
                if (g_log) g_log("Live chests: chests' id type %u found; their empty state not yet", found.lootId);
            }
        }
    }
    if (!g_types.lootId || !g_types.position) return;
    // The chest screen open: the chest being looted is changing
    if (screen == 21) return;
    uint8_t state[kTotalChestCount];
    std::memset(state, 0xff, sizeof(state)); // 0xff not near
    bool present[kTotalChestCount]{};        // its entity is there (whatever its empty state reads)
    float px = 0.0f, pz = 0.0f;
    const bool havePlayer = GetHudPlayerWorldPos(client, px, pz);
    bool ok = true;
    __try {
        const Types t = g_types;
        ForEachEcsEntity(static_cast<uint8_t*>(t.registry), [&](uint8_t* rec) {
            uint16_t size = 0;
            const uint8_t* id = EcsComponent(rec, t.lootId, &size);
            if (!id || size != 4) return;
            size_t c = ChestOf(id);
            if (c >= kTotalChestCount) c = LearnClam(rec, id, t.position);
            if (c >= kTotalChestCount) return;
#ifdef UMM_DEV_BRIDGE
            if (static_cast<long>(c) == g_devHideChest) return;
#endif
            present[c] = true;
            const uint8_t* e = t.empty ? EcsComponent(rec, t.empty, &size) : nullptr;
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
    if (havePlayer) {
        const float mx = px - g_lastPlayerXZ[0], mz = pz - g_lastPlayerXZ[1];
        if (mx * mx + mz * mz > 40.0f * 40.0f) g_settleUntil = nowTick + 10000ULL; // a teleport, not a run or a glide
        g_lastPlayerXZ[0] = px, g_lastPlayerXZ[1] = pz;
    }
    const bool settled = nowTick >= g_settleUntil;
    // per kind of chest, the farthest one still sent to the player, across the ground (within 150 m: not an old place's)
    constexpr size_t kKinds = 11;
    float reach[kKinds]{};
    if (havePlayer) {
        for (size_t c = 0; c < kTotalChestCount; ++c) {
            if (!present[c] || kChestSpawns[c].subtype >= kKinds) continue;
            const float d = ChestDistXZ(c, px, pz);
            if (d < 150.0f) reach[kChestSpawns[c].subtype] = std::max(reach[kChestSpawns[c].subtype], d);
        }
    }
    for (size_t c = 0; c < kTotalChestCount; ++c) {
        auto& st = g_chestLootStates[c];
        if (present[c]) {
            g_seenHere[c] = true;
            g_goneReads[c] = 0;
        } else if (g_seenHere[c] && !st.looted && havePlayer && settled && kChestSpawns[c].subtype < kKinds) {
            // gone: blown up, if it's well inside the distance chests of its kind are still sent at
            const float d = ChestDistXZ(c, px, pz);
            const float r = reach[kChestSpawns[c].subtype];
            if (d + 10.0f < r && d < 90.0f) {
                if (++g_goneReads[c] >= 3) {
                    g_goneReads[c] = 0;
                    g_seenHere[c] = false;
                    g_seenEmpty[c] = true; // read full again after the area reset: unmarked as an emptied one is
                    g_fullReads[c] = 0;
                    st.looted = true;
                    st.elapsedOutOfBuildZoneSec = 0.0;
                    g_chestLootDirty = true;
                    g_lootSaveDirty = true;
                    if (g_log)
                        g_log("Live chests: chest %zu is gone %.0f m away (its kind still sent at %.0f m); destroyed, marked looted",
                              c, d, r);
                }
            } else {
                g_goneReads[c] = 0;
            }
        } else {
            g_goneReads[c] = 0;
        }
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
        } else if (state[c] == 0 && st.looted && g_seenEmpty[c] && !g_chestsStayLooted) {
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
