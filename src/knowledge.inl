// Read lore notes and defeated bosses: the local player's knowledge (the ids the game sets when a note is read or a
// boss is killed, ore_deposits.h kKnowledgeSpots) from the save system's player slot.
//
// Save2 (FUN_140dc4a30 builds it) keeps a knowledge container per player slot at Save2 + 0xbcd28 + slot * 0x5620
// (FUN_140dd1780): +0 u8 1, +8 keys {u32* ptr, u64 count, u64 capacity 11000, fn, allocator}, +0x30 states {u32* ptr,
// u64 count, ...}; a key is known when its state is not 0. The local host player is slot 0. Save2 + 0x1136d8 is a
// container of the same layout with this character's own knowledge, filled on a client too (from the server's save,
// so a note read there shows once the server saves); a client's player slots stay empty. Known = both together.
// Locating Save2: a static initializer (0x140837e7b, "xchg [rip+x], eax; lea rax, ...; movdqa [rip+x], xmm0; ...;
// mov byte [rip+x], cl") fills a block of globals; its last store is to block + 0x10, the allocator is at block + 8
// and the Save2 pointer at block + 0x20. The pointer is used only while it validates: Save2 + 0x119aa0 holds that
// allocator, Save2 + 0x50 is set, the container byte is 1 and its capacity 11000. Otherwise notes and bosses show
// untracked.

uint8_t* g_save2Block = nullptr; // the static block: +8 allocator, +0x20 Save2
std::unordered_set<uint32_t> g_knownKnowledge;
bool g_knowledgeValid = false;
// Whether this game hosts the world: 1 hosting (or single player), 0 a player on someone else's server, -1 not known
// yet. A host's slot 0 has the player's keys. A client gets its knowledge replicated from the server into
// R = *(Save2 + 0x1192a0), a hash set at R + 0x40058 (+0x54 count), and its slot 0 stays empty; R keeps the last
// server's data after leaving it, so a client is only taken for one after 60 s of R filled and slot 0 empty.
int g_worldHostState = -1;
uint32_t g_knowledgeReplicated = 0;
uint64_t g_knowledgeSlotKeys = 0;
bool g_worldHostRecheck = false; // set when another world starts: decide again from scratch

void FindSave2Block() {
    uint8_t* p = FindUnique("87 05 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 66 0F 7F 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 0F 57 C0 "
                            "48 8D 05 ?? ?? ?? ?? 48 89 0D ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? "
                            "48 89 05 ?? ?? ?? ?? 88 0D");
    if (!p) {
        if (g_log) g_log("Lore notes / bosses: the save system was not found; read notes and defeated bosses are not tracked");
        return;
    }
    int32_t disp = 0;
    ReadCode(p + 0x44, &disp, 4);
    g_save2Block = p + 0x48 + disp - 0x10; // the byte store's target is block + 0x10
}

// Adds a knowledge container's known ids to out; false if it does not validate.
bool ReadKnowledgeContainer(const uint8_t* c, std::unordered_set<uint32_t>& out) {
    if (c[0] != 1) return false;
    const auto* keys = *reinterpret_cast<const uint32_t* const*>(c + 0x08);
    const uint64_t count = *reinterpret_cast<const uint64_t*>(c + 0x10);
    const uint64_t capacity = *reinterpret_cast<const uint64_t*>(c + 0x18);
    const auto* states = *reinterpret_cast<const uint32_t* const*>(c + 0x30);
    const uint64_t stateCount = *reinterpret_cast<const uint64_t*>(c + 0x38);
    if (capacity != 11000 || count > capacity || stateCount != count || (count && (!keys || !states))) return false;
    for (uint64_t i = 0; i < count; ++i) {
        if (states[i] != 0) out.insert(keys[i]);
    }
    return true;
}

bool ReadKnowledgeInner(std::unordered_set<uint32_t>& out) {
    uint8_t* save2 = *reinterpret_cast<uint8_t**>(g_save2Block + 0x20);
    void* allocator = *reinterpret_cast<void**>(g_save2Block + 0x08);
    if (!save2 || !allocator) return false;
    if (*reinterpret_cast<void**>(save2 + 0x119aa0) != allocator || *reinterpret_cast<uint64_t*>(save2 + 0x50) == 0) return false;
    out.clear();
    const bool own = ReadKnowledgeContainer(save2 + 0x1136d8, out);  // this character's knowledge (host or client)
    const bool slot = ReadKnowledgeContainer(save2 + 0xbcd28, out);  // slot 0, the local host player (live)
    const uint8_t* replica = *reinterpret_cast<uint8_t**>(save2 + 0x1192a0);
    const uint32_t replicated = replica ? *reinterpret_cast<const uint32_t*>(replica + 0x40058 + 0x54) : 0;
    const uint64_t slotKeys = *reinterpret_cast<const uint64_t*>(save2 + 0xbcd28 + 0x10);
    g_knowledgeReplicated = replicated <= 11000 ? replicated : 0;
    g_knowledgeSlotKeys = slot ? slotKeys : 0;
    return own || slot;
}

// Once a second: the known set, or untracked if the save system is not there or does not validate.
void UpdateKnowledge() {
    static uint64_t lastTick = 0;
    const uint64_t now = GetTickCount64();
    if (now - lastTick < 1000ULL) return;
    lastTick = now;
    bool ok = false;
    if (g_save2Block) {
        __try {
            ok = ReadKnowledgeInner(g_knownKnowledge);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    static size_t loggedCount = SIZE_MAX;
    if (ok && g_knownKnowledge.size() != loggedCount && g_log) {
        g_log("Lore notes / bosses: %zu known", g_knownKnowledge.size()); // also when a note is read or a boss beaten
        loggedCount = g_knownKnowledge.size();
    } else if (!ok && g_knowledgeValid && g_log) {
        g_log("Lore notes / bosses: knowledge not readable (untracked)");
        loggedCount = SIZE_MAX;
    }
    g_knowledgeValid = ok;
    static uint64_t clientSince = 0;
    if (g_worldHostRecheck) {
        g_worldHostRecheck = false;
        g_worldHostState = -1;
        clientSince = 0;
    }
    if (!ok) {
        g_worldHostState = -1;
        clientSince = 0;
    } else if (g_knowledgeSlotKeys > 0) {
        g_worldHostState = 1;
        clientSince = 0;
    } else if (g_knowledgeReplicated > 0) {
        if (!clientSince) clientSince = now;
        if (now - clientSince >= 60000ULL) g_worldHostState = 0;
    } else {
        clientSince = 0;
    }
    static int loggedHost = -2;
    if (g_worldHostState != loggedHost && g_worldHostState != -1 && g_log) {
        g_log("%s (player slot %llu keys, replicated %u)",
              g_worldHostState ? "World: hosted by this game" : "World: on a server (not hosted by this game)",
              static_cast<unsigned long long>(g_knowledgeSlotKeys), g_knowledgeReplicated);
        loggedHost = g_worldHostState;
    }
    if (!ok) g_knownKnowledge.clear();
}

// A Flame or Night Sanctum marker near (x, z) that this character has visited (sanctum_spots.h; false while untracked).
bool SanctumVisitedNear(float x, float z) {
    if (!g_knowledgeValid) return false;
    for (const SanctumSpot& sp : kSanctumSpots) {
        if (std::fabs(sp.x - x) > 6.0f || std::fabs(sp.z - z) > 6.0f) continue;
        for (uint32_t id : sp.id) {
            if (id != 0 && g_knownKnowledge.count(id) != 0) return true;
        }
    }
    return false;
}

bool IsKnowledgeDeposit(size_t d) {
    return d >= kKnowledgeFirstDeposit && d < kKnowledgeFirstDeposit + kKnowledgeSpotCount;
}

// A lore note read / a boss defeated (false while untracked).
bool KnowledgeDone(size_t d) {
    if (!g_knowledgeValid || !IsKnowledgeDeposit(d)) return false;
    const uint32_t id = kKnowledgeSpots[d - kKnowledgeFirstDeposit].knowledge;
    return id != 0 && g_knownKnowledge.count(id) != 0;
}

// The note's or boss's own name (journal entry / health bar) in the game's language, else the row's name.
const char* KnowledgeSpotName(size_t d) {
    static char names[kKnowledgeSpotCount][96]{};
    static uint8_t state[kKnowledgeSpotCount]{}; // 0 not tried, 1 have it, 2 none
    static int builtFor = -1;
    const int lang = static_cast<int>(umm_lang::Current());
    if (builtFor != lang) {
        std::memset(state, 0, sizeof(state));
        builtFor = lang;
    }
    if (!IsKnowledgeDeposit(d)) return "";
    const size_t i = d - kKnowledgeFirstDeposit;
    if (state[i] == 0 && kKnowledgeSpots[i].nameLoca != 0u && GetUiClient()) {
        state[i] = ReadLocaText(kKnowledgeSpots[i].nameLoca, names[i], sizeof(names[i])) ? 1 : 2;
    }
    return state[i] == 1 ? names[i] : OreName(kOreDeposits[d].ore);
}

// A row's count: for Lore Notes and Bosses the ones not read / defeated yet.
int KnowledgeRemaining(size_t ore) {
    int n = 0;
    for (size_t i = 0; i < kKnowledgeSpotCount; ++i) {
        const size_t d = kKnowledgeFirstDeposit + i;
        if (kOreDeposits[d].ore == ore && !KnowledgeDone(d)) ++n;
    }
    return n;
}
