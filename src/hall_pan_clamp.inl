// Included in map_key_panel.cpp's private namespace, after halls.inl.
// The dungeon map pans only over its hall's box, through the game's own pan clamp (FUN_140d7ffd0, the World Map view's
// update): it writes the view's bounds as the whole world ({0, 0, +0x38, +0x3c} into mapView +0x1c8..+0x1d4, x, y,
// width, height on the map's canvas), calls FUN_140a412f0 and then clamps the view to those bounds. The call is sent
// through a small stub that, while a dungeon map shows, writes the hall's box over the bounds first and then goes on to
// the game's function; the game's clamp does the rest, so nothing fights the drag. Moving the view from the mod each
// frame did fight it. The stub (and the box it reads) lives in memory of its own next to the game's code, kept across
// hot reloads; with the switch off it only forwards the call.
namespace hall_pan {

struct StubData {
    uint32_t on;      // +0: a dungeon map shows
    float rect[4];    // +4: the bounds on the canvas: x, y (south from the world's north edge), width, height
    uint64_t mapView; // +24 (0x18): the view it is for (the clamp's RSI)
    float lo[2];      // +0x20: the bounds' corners (x, y), for the stub's own clamp of the view's anchors
    float hi[2];      // +0x28
};
static_assert(sizeof(StubData) == 48, "stub data layout");

StubData* g_data = nullptr;

// The stub's page: allocated near the call (a rel32 call has to reach it), once per game run (kept)
uint8_t* NearPage(uint8_t* close) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t base = reinterpret_cast<uintptr_t>(close) & ~(gran - 1);
    for (uintptr_t step = gran; step < (1ull << 30); step += gran) {
        for (int sign : {-1, 1}) {
            const uintptr_t at = sign < 0 ? base - step : base + step;
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(at), 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
                return static_cast<uint8_t*>(p);
        }
    }
    return nullptr;
}

// The overworld's terrain off while a dungeon map shows: the World Map draw (FUN_140c26610) calls FUN_140d67fc0 for the
// map canvas, which draws the world's edge frame (FUN_140cf58b0: its edge strip and corners, sized by the zoom) and the
// terrain. The hall's map is the mod's own, so none of it is wanted; the game draws it under the translucent menu bars,
// where the mod can't paint over it (the bars come after). The call goes through a stub in the pan stub's page (+0x80)
// that returns at once while the switch is on; the game's own target is kept at +0x108 (see Install)
void InstallTerrainSkip(uint8_t* page) {
    // mov rdx, r13 / lea rcx, [rbp+0x150] / call FUN_140d67fc0 / mov dl, 0x81 / lea rcx, [rbp+...]
    uint8_t* hit = FindUnique("49 8B D5 48 8D 8D 50 01 00 00 E8 ?? ?? ?? ?? B2 81 48 8D 8D");
    if (!hit) {
        if (g_log) g_log("Dungeon map: the World Map's terrain call was not found; the overworld shows under the menu bars");
        return;
    }
    uint8_t* call = hit + 10;
    uint8_t original[5];
    ReadCode(call, original, sizeof(original));
    int32_t rel;
    std::memcpy(&rel, original + 1, 4);
    uint8_t* target = call + 5 + rel;
    auto* gameTarget = reinterpret_cast<uint64_t*>(page + 0x108);
    if (!(target >= page && target < page + 4096)) *gameTarget = reinterpret_cast<uint64_t>(target);
    if (!*gameTarget) return;
    uint8_t* code = page + 0x80;
    // mov rax, data / cmp dword [rax], 0 / je go / ret / go: mov rax, FUN_140d67fc0 / jmp rax
    uint8_t stub[] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
                      0x83, 0x38, 0x00,
                      0x74, 0x01,
                      0xC3,
                      0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
                      0xFF, 0xE0};
    const uint64_t dataAddr = reinterpret_cast<uint64_t>(g_data);
    std::memcpy(stub + 2, &dataAddr, 8);
    std::memcpy(stub + 18, gameTarget, 8);
    std::memcpy(code, stub, sizeof(stub));
    FlushInstructionCache(GetCurrentProcess(), code, sizeof(stub));
    const int64_t toStub = reinterpret_cast<int64_t>(code) - reinterpret_cast<int64_t>(call + 5);
    if (toStub > INT32_MAX || toStub < INT32_MIN) return;
    uint8_t patch[5] = {0xE8};
    const int32_t rel32 = static_cast<int32_t>(toStub);
    std::memcpy(patch + 1, &rel32, 4);
    if (std::memcmp(call, patch, 5) != 0) {
        DWORD old = 0;
        if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old)) return;
        std::memcpy(call, patch, 5);
        VirtualProtect(call, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), call, 5);
    }
    if (g_log) g_log("Dungeon map: the overworld's terrain and edge frame stay off under a dungeon map");
}

bool Install() {
    // movups [rsi+0x1c8], xmm2 / call FUN_140a412f0 / movss xmm0, [rsi+0x1c8] (+0x1b8 before the game's 2026-11-02 patch)
    uint8_t* hit = FindUnique("0F 11 96 C8 01 00 00 E8 ?? ?? ?? ?? F3 0F 10 86 C8 01 00 00");
    if (!hit) {
        if (g_log) g_log("Dungeon map panning: the game's pan clamp was not found; panning is free");
        return false;
    }
    uint8_t* call = hit + 7;
    uint8_t original[5];
    ReadCode(call, original, sizeof(original));
    int32_t rel;
    std::memcpy(&rel, original + 1, 4);
    uint8_t* target = call + 5 + rel; // FUN_140a412f0, or the stub an earlier generation put there
    auto* kept = static_cast<uint8_t**>(greeneye::Host() ? greeneye::Host()->keep("halls.pan-stub.v1", sizeof(uint8_t*)) : nullptr);
    uint8_t* page = kept && *kept ? *kept : NearPage(call);
    if (!page) {
        if (g_log) g_log("Dungeon map panning: no memory near the game's code; panning is free");
        return false;
    }
    if (kept) *kept = page;
    // The game's own target, kept in the page (+0x100) from the first install: once the call points at the stub,
    // reading it gives the stub, and a stub going on to itself spun the game forever (2026-11-01)
    auto* gameTarget = reinterpret_cast<uint64_t*>(page + 0x100);
    const bool intoPage = target >= page && target < page + 4096;
    if (!intoPage) *gameTarget = reinterpret_cast<uint64_t>(target);
    if (!*gameTarget) {
        if (g_log) g_log("Dungeon map panning: the game's own call target is unknown; panning is free");
        return false;
    }
    target = reinterpret_cast<uint8_t*>(*gameTarget);
    g_data = reinterpret_cast<StubData*>(page);
    g_data->on = 0; // its bounds are written by the first Update
    // Code at +0x200 (the terrain stub is at +0x80, the game's targets at +0x100 / +0x108)
    uint8_t* code = page + 0x200;
    // mov rax, data / cmp dword [rax], 0 / je skip / cmp rsi, [rax+0x18] / jne skip /
    // mov r11, [rax+4] / mov [rsi+0x1c8], r11 / mov r11, [rax+0xc] / mov [rsi+0x1d0], r11 /
    // the view's position (+0x20 / +0x24) and the anchor it is drawn from (+0x1b0 / +0x1b4) clamped to the bounds:
    // in one input mode the game copies the position to the anchor before its own clamp, which then bounds the
    // position only (Codex's trace of FUN_140d7ffd0, 2026-11-01) /
    // skip: mov rax, FUN_140a412f0 / jmp rax        (rax, r11, xmm4: volatile, no arguments in them)
    std::vector<uint8_t> stub = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0x83, 0x38, 0x00,
                                 0x0F, 0x84, 0, 0, 0, 0,
                                 0x48, 0x3B, 0x70, 0x18,
                                 0x0F, 0x85, 0, 0, 0, 0,
                                 0x4C, 0x8B, 0x58, 0x04,
                                 0x4C, 0x89, 0x9E, 0xC8, 0x01, 0x00, 0x00,
                                 0x4C, 0x8B, 0x58, 0x0C,
                                 0x4C, 0x89, 0x9E, 0xD0, 0x01, 0x00, 0x00};
    auto sse = [&](uint8_t op, uint8_t modrm, uint32_t disp) { // F3 0F op xmm4, [base+disp32]
        stub.insert(stub.end(), {0xF3, 0x0F, op, modrm});
        for (int i = 0; i < 4; ++i) stub.push_back(static_cast<uint8_t>(disp >> (8 * i)));
    };
    for (uint32_t field : {0x20u, 0x24u, 0x1b0u, 0x1b4u}) {
        const uint32_t axis = (field & 4) ? 4u : 0u;
        sse(0x10, 0xA6, field);                                      // movss xmm4, [rsi+field]
        sse(0x5F, 0xA0, static_cast<uint32_t>(offsetof(StubData, lo)) + axis); // maxss xmm4, [rax+lo]
        sse(0x5D, 0xA0, static_cast<uint32_t>(offsetof(StubData, hi)) + axis); // minss xmm4, [rax+hi]
        sse(0x11, 0xA6, field);                                      // movss [rsi+field], xmm4
    }
    const size_t skip = stub.size();
    stub.insert(stub.end(), {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0});
    const int32_t toSkipJe = static_cast<int32_t>(skip - 19), toSkipJne = static_cast<int32_t>(skip - 29);
    std::memcpy(stub.data() + 15, &toSkipJe, 4);
    std::memcpy(stub.data() + 25, &toSkipJne, 4);
    const uint64_t dataAddr = reinterpret_cast<uint64_t>(g_data), targetAddr = reinterpret_cast<uint64_t>(target);
    std::memcpy(stub.data() + 2, &dataAddr, 8);
    std::memcpy(stub.data() + skip + 2, &targetAddr, 8);
    std::memcpy(code, stub.data(), stub.size());
    FlushInstructionCache(GetCurrentProcess(), code, stub.size());
    const int64_t toStub = reinterpret_cast<int64_t>(code) - reinterpret_cast<int64_t>(call + 5);
    if (toStub > INT32_MAX || toStub < INT32_MIN) return false;
    uint8_t patch[5] = {0xE8};
    const int32_t rel32 = static_cast<int32_t>(toStub);
    std::memcpy(patch + 1, &rel32, 4);
    uint8_t now[5];
    std::memcpy(now, call, 5);
    if (std::memcmp(now, patch, 5) != 0) {
        DWORD old = 0;
        if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
        std::memcpy(call, patch, 5);
        VirtualProtect(call, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), call, 5);
    }
    if (g_log) g_log("Dungeon map panning: the game's pan clamp holds the dungeon map to its hall");
    InstallTerrainSkip(page);
    return true;
}

// Each World Map frame: the bounds for the hall's box while a dungeon map shows, else the switch off. A canvas point c
// shows at screen (+0x1f0 + pivot +0x18c) + (c - anchor) * zoom (the anchor: +0x1b0, what the game clamps), so the
// anchor that puts the box's west edge at the screen's left is minX + (P - left) / zoom, and so on (Codex's formula).
// Zoomed in, the screen's edges stop at the box's; with the whole box on the screen, it moves only as far as it stays
// on it (never a single point: the zoom reads the bounds too, and a point held it still)
void Update(uint8_t* mapState, const float* rootRect) {
    if (!g_data) return;
    float minX, minZ, maxX, maxZ;
    uint8_t* mapView = mapState ? *reinterpret_cast<uint8_t**>(mapState + 8) : nullptr;
    if (!mapView || !rootRect || !halls::MapBounds(minX, minZ, maxX, maxZ)) {
        g_data->on = 0;
        return;
    }
    auto at = [&](int off) { return *reinterpret_cast<float*>(mapView + off); };
    float mapWorldH = at(0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
    const float zoom = at(0x10), px = at(0x1f0) + at(0x18c), py = at(0x1f4) + at(0x190);
    if (!(zoom > 0.0001f) || !std::isfinite(px) || !std::isfinite(py)) {
        g_data->on = 0;
        return;
    }
    const float left = rootRect[0], right = left + (rootRect[2] > 100.0f ? rootRect[2] : 1920.0f);
    const float top = rootRect[1] + 88.0f; // below the menu banner, above the key hints
    const float bottom = rootRect[1] + (rootRect[3] > 100.0f ? rootRect[3] : 1080.0f) - 64.0f;
    // The game first keeps the anchor on the world's canvas (0 .. +0x38 / +0x3c), so in the halls' corner one side's
    // stop comes early (2026-11-01: the box's right edge stopped at 3/4 of the screen, its left went to the edge). With
    // the whole box on the screen, its play is then the same both ways from the middle: as far as the short side
    // allows. Zoomed in, the screen's edges still stop at the box's
    auto range = [&](float lo, float hi, float p, float screenLo, float screenHi, int axis) {
        const float a = lo + (p - screenLo) / zoom, b = hi + (p - screenHi) / zoom;
        const float canvas = at(axis ? 0x3c : 0x38) > 100.0f ? at(axis ? 0x3c : 0x38) : 10240.0f;
        // A fifth of the screen's slack past the box's edges: with the box about the screen's size, edge to edge left
        // no room to pan at all (2026-11-01, 3x zoom), and its east side stayed under the Map Key
        const float slack = 0.2f * (screenHi - screenLo) / zoom;
        float from = std::max(0.0f, std::min(a, b) - slack), to = std::min(canvas, std::max(a, b) + slack);
        if (a > b) { // the box fits: centred, and an even play each way
            const float mid = (a + b) * 0.5f, play = std::max(0.0f, std::min(mid - from, to - mid));
            from = mid - play, to = mid + play;
        }
        if (to - from < 1.0f) from = (from + to) * 0.5f - 0.5f, to = from + 1.0f;
        g_data->lo[axis] = from;
        g_data->hi[axis] = to;
    };
    range(minX, maxX, px, left, right, 0);
    range(mapWorldH - maxZ, mapWorldH - minZ, py, top, bottom, 1);
    g_data->rect[0] = g_data->lo[0];
    g_data->rect[1] = g_data->lo[1];
    g_data->rect[2] = g_data->hi[0] - g_data->lo[0];
    g_data->rect[3] = g_data->hi[1] - g_data->lo[1];
    g_data->mapView = reinterpret_cast<uint64_t>(mapView);
    g_data->on = 1;
}
void Off() {
    if (g_data) g_data->on = 0;
}

} // namespace hall_pan
