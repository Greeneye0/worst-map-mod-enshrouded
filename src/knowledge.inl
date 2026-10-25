// Read lore notes and defeated bosses: the local player's knowledge (the ids the game sets when a note is read or a
// boss is killed, ore_deposits.h kKnowledgeSpots) from the save system's player slot.
//
// Save2 (FUN_140dc4a30 builds it) keeps a knowledge container per player slot at Save2 + 0xbcd28 + slot * 0x5620
// (FUN_140dd1780): +0 u8 1, +8 keys {u32* ptr, u64 count, u64 capacity 11000, fn, allocator}, +0x30 states {u32* ptr,
// u64 count, ...}; a key is known when its state is not 0. The local host player is slot 0.
// Locating Save2: a static initializer (0x140837e7b, "xchg [rip+x], eax; lea rax, ...; movdqa [rip+x], xmm0; ...;
// mov byte [rip+x], cl") fills a block of globals; its last store is to block + 0x10, the allocator is at block + 8
// and the Save2 pointer at block + 0x20. The pointer is used only while it validates: Save2 + 0x119aa0 holds that
// allocator, Save2 + 0x50 is set, the container byte is 1 and its capacity 11000. Otherwise (a client on someone
// else's server has no Save2) notes and bosses show untracked.

uint8_t* g_save2Block = nullptr; // the static block: +8 allocator, +0x20 Save2
std::unordered_set<uint32_t> g_knownKnowledge;
bool g_knowledgeValid = false;

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

bool ReadKnowledgeInner(std::unordered_set<uint32_t>& out) {
    uint8_t* save2 = *reinterpret_cast<uint8_t**>(g_save2Block + 0x20);
    void* allocator = *reinterpret_cast<void**>(g_save2Block + 0x08);
    if (!save2 || !allocator) return false;
    if (*reinterpret_cast<void**>(save2 + 0x119aa0) != allocator || *reinterpret_cast<uint64_t*>(save2 + 0x50) == 0) return false;
    uint8_t* c = save2 + 0xbcd28; // slot 0, the local host player
    if (c[0] != 1) return false;
    const auto* keys = *reinterpret_cast<const uint32_t**>(c + 0x08);
    const uint64_t count = *reinterpret_cast<uint64_t*>(c + 0x10);
    const uint64_t capacity = *reinterpret_cast<uint64_t*>(c + 0x18);
    const auto* states = *reinterpret_cast<const uint32_t**>(c + 0x30);
    const uint64_t stateCount = *reinterpret_cast<uint64_t*>(c + 0x38);
    if (!keys || !states || capacity != 11000 || count > capacity || stateCount != count) return false;
    out.clear();
    for (uint64_t i = 0; i < count; ++i) {
        if (states[i] != 0) out.insert(keys[i]);
    }
    return true;
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
    if (ok != g_knowledgeValid && g_log) {
        g_log(ok ? "Lore notes / bosses: %zu known" : "Lore notes / bosses: knowledge not readable (untracked)%s",
              ok ? g_knownKnowledge.size() : 0, "");
    }
    g_knowledgeValid = ok;
    if (!ok) g_knownKnowledge.clear();
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
