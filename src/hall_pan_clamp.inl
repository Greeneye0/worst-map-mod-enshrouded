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
    uint32_t on;      // +0: 1 a dungeon map shows, 2 the overworld on the Map Desk (its terrain drawn)
    float rect[4];    // +4: the bounds on the canvas: x, y (south from the world's north edge), width, height
    uint64_t mapView; // +24 (0x18): the view it is for (the clamp's RSI)
    float lo[2];      // +0x20: the bounds' corners (x, y), for the stub's own clamp of the view's anchors
    float hi[2];      // +0x28
};
static_assert(sizeof(StubData) == 48, "stub data layout");

StubData* g_data = nullptr;
// The Map Desk's ease back: how far past the resting bounds they still reach (canvas units, x / y), the last time the
// map was dragged further out, and the last frame
float g_extra[2] = {}, g_prevOver[2] = {};
uint64_t g_lastOut = 0, g_lastUpdate = 0;

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
    // mov rax, data / cmp dword [rax], 1 / jne go / ret / go: mov rax, FUN_140d67fc0 / jmp rax (1: a dungeon map; 2, the
    // overworld held on the Map Desk, keeps its terrain)
    uint8_t stub[] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
                      0x83, 0x38, 0x01,
                      0x75, 0x01,
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

// The view's centre off the world's canvas, on the Map Desk: before the clamp call above, FUN_140d7ffd0 holds the view's
// position (+0x20 / +0x24) to [0, +0x38 / +0x3c] (maxss xmm1/xmm0 with xmm9 = 0, minss with xmm2 / xmm3), so the
// world's edge never went past mid-screen. Those four go through a stub (page +0x300) that skips them while the flag at
// page +0x40 is set for this view (the overworld on the Map Desk: the mod's bounds at the clamp call then hold it);
// otherwise it clamps as the game does. The bytes between (movups xmm2, [rsp+0x40] / shufps) go along.
uint32_t* g_canvasFree = nullptr; // page +0x40
void InstallCanvasFree(uint8_t* page) {
    // movss xmm1,[rsi+20] / movss xmm0,[rsi+24] / maxss xmm1,xmm9 / maxss xmm0,xmm9 / minss xmm1,xmm2 /
    // movups xmm2,[rsp+40] / minss xmm0,xmm3 / shufps xmm2,xmm2,d2 / movss [rsi+20],xmm1
    const uint8_t original[27] = {0xF3, 0x41, 0x0F, 0x5F, 0xC9, 0xF3, 0x41, 0x0F, 0x5F, 0xC1, 0xF3, 0x0F, 0x5D, 0xCA,
                                  0x0F, 0x10, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x5D, 0xC3, 0x0F, 0xC6, 0xD2, 0xD2};
    uint8_t* hit = FindUnique("F3 0F 10 4E 20 F3 0F 10 46 24 F3 41 0F 5F C9 F3 41 0F 5F C1 F3 0F 5D CA 0F 10 54 24 40 "
                              "F3 0F 5D C3 0F C6 D2 D2 F3 0F 11 4E 20");
    if (!hit) hit = FindUnique("F3 0F 10 4E 20 F3 0F 10 46 24 E9 ?? ?? ?? ?? 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 "
                               "90 90 90 90 90 90 90 F3 0F 11 4E 20"); // an earlier generation's patch
    if (!hit) {
        if (g_log) g_log("Map desk panning: the game's canvas clamp was not found; the map's edge stops mid-screen");
        return;
    }
    uint8_t* site = hit + 10;
    uint8_t* back = site + sizeof(original);
    g_canvasFree = reinterpret_cast<uint32_t*>(page + 0x40);
    *g_canvasFree = 0;
    uint8_t* code = page + 0x300;
    std::vector<uint8_t> s = {0x50,                                   // push rax
                              0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,     // mov rax, page
                              0x83, 0x78, 0x40, 0x00,                 // cmp dword [rax+0x40], 0
                              0x74, 0x0A,                             // je clamp
                              0x48, 0x3B, 0x70, 0x18,                 // cmp rsi, [rax+0x18]
                              0x75, 0x04,                             // jne clamp
                              0x58,                                   // pop rax
                              0xEB, 0x15,                             // jmp tail
                              0x90,                                   // (pad)
                              // clamp:
                              0x58,                                   // pop rax
                              0xF3, 0x41, 0x0F, 0x5F, 0xC9,           // maxss xmm1, xmm9
                              0xF3, 0x41, 0x0F, 0x5F, 0xC1,           // maxss xmm0, xmm9
                              0xF3, 0x0F, 0x5D, 0xCA,                 // minss xmm1, xmm2
                              0xF3, 0x0F, 0x5D, 0xC3,                 // minss xmm0, xmm3
                              0x90, 0x90,
                              // tail:
                              0x0F, 0x10, 0x54, 0x24, 0x40,           // movups xmm2, [rsp+0x40]
                              0x0F, 0xC6, 0xD2, 0xD2,                 // shufps xmm2, xmm2, 0xd2
                              0xFF, 0x25, 0, 0, 0, 0,                 // jmp [rip]
                              0, 0, 0, 0, 0, 0, 0, 0};
    const uint64_t pageAddr = reinterpret_cast<uint64_t>(page), backAddr = reinterpret_cast<uint64_t>(back);
    std::memcpy(s.data() + 3, &pageAddr, 8);
    std::memcpy(s.data() + s.size() - 8, &backAddr, 8);
    // the jumps' targets: "jmp tail" from after it (offset 26) to tail; "je clamp" from 17 to clamp (27)
    const size_t clampAt = 27, tailAt = s.size() - 23;
    s[16] = static_cast<uint8_t>(clampAt - 17);
    s[22] = static_cast<uint8_t>(clampAt - 23);
    s[25] = static_cast<uint8_t>(tailAt - 26);
    std::memcpy(code, s.data(), s.size());
    FlushInstructionCache(GetCurrentProcess(), code, s.size());
    const int64_t toStub = reinterpret_cast<int64_t>(code) - reinterpret_cast<int64_t>(site + 5);
    if (toStub > INT32_MAX || toStub < INT32_MIN) return;
    uint8_t patch[sizeof(original)];
    std::memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    const int32_t rel32 = static_cast<int32_t>(toStub);
    std::memcpy(patch + 1, &rel32, 4);
    if (std::memcmp(site, patch, sizeof(patch)) != 0) {
        DWORD old = 0;
        if (!VirtualProtect(site, sizeof(patch), PAGE_EXECUTE_READWRITE, &old)) return;
        std::memcpy(site, patch, sizeof(patch));
        VirtualProtect(site, sizeof(patch), old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, sizeof(patch));
    }
    if (g_log) g_log("Map desk panning: the map can be dragged out over the desk");
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
    InstallCanvasFree(page);
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
    auto at = [&](int off) { return *reinterpret_cast<float*>(mapView + off); };
    // The overworld map on the Map Desk: the same rule for the whole world (the game's own keeps only the view's middle
    // on the map, so zoomed out it slid until its edge reached mid-screen, stopping at a different place every zoom)
    const bool hall = mapView && rootRect && halls::MapBounds(minX, minZ, maxX, maxZ);
    const bool deskWorld = !hall && mapView && rootRect && g_mapDesk && !halls::MapActive();
    if (!hall && !deskWorld) {
        g_data->on = 0;
        if (g_canvasFree) *g_canvasFree = 0;
        return;
    }
    float mapWorldH = at(0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
    if (deskWorld) {
        minX = 0.0f, minZ = 0.0f;
        maxX = at(0x38) > 100.0f ? at(0x38) : 10240.0f;
        maxZ = mapWorldH;
    }
    const float zoom = at(0x10), px = at(0x1f0) + at(0x18c), py = at(0x1f4) + at(0x190);
    if (!(zoom > 0.0001f) || !std::isfinite(px) || !std::isfinite(py)) {
        g_data->on = 0;
        if (g_canvasFree) *g_canvasFree = 0;
        return;
    }
    const float left = rootRect[0], right = left + (rootRect[2] > 100.0f ? rootRect[2] : 1920.0f);
    const float top = rootRect[1] + 88.0f; // below the menu banner, above the key hints
    const float bottom = rootRect[1] + (rootRect[3] > 100.0f ? rootRect[3] : 1080.0f) - 64.0f;
    // The game first keeps the anchor on the world's canvas (0 .. +0x38 / +0x3c), so in the halls' corner one side's
    // stop comes early (2026-11-01: the box's right edge stopped at 3/4 of the screen, its left went to the edge). With
    // the whole box on the screen, its play is then the same both ways from the middle: as far as the short side
    // allows. Zoomed in, the screen's edges still stop at the box's
    const uint64_t now = GetTickCount64();
    const float dt = std::clamp(static_cast<float>(now - g_lastUpdate) / 1000.0f, 0.0f, 0.1f);
    g_lastUpdate = now;
    const float decay = std::exp(-dt * 2.0f); // a slow float back: about a second and a half to settle
    const bool mouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (g_canvasFree) *g_canvasFree = deskWorld ? 1u : 0u;
    auto range = [&](float lo, float hi, float p, float screenLo, float screenHi, int axis) {
        const float a = lo + (p - screenLo) / zoom, b = hi + (p - screenHi) / zoom;
        const float canvas = at(axis ? 0x3c : 0x38) > 100.0f ? at(axis ? 0x3c : 0x38) : 10240.0f;
        // A fifth of the screen's slack past the box's edges: with the box about the screen's size, edge to edge left
        // no room to pan at all (2026-11-01, 3x zoom), and its east side stayed under the Map Key
        // The overworld on the Map Desk: half a screen, so a place at the world's edge can be brought to the middle
        // (2026-11-12, zoom 3: a fifth stopped it short)
        auto bounds = [&](float share, bool onCanvas, float& from, float& to) {
            const float slack = share * (screenHi - screenLo) / zoom;
            from = std::min(a, b) - slack, to = std::max(a, b) + slack;
            if (onCanvas) from = std::max(0.0f, from), to = std::min(canvas, to);
            if (a > b) { // the box fits: centred, and an even play each way
                const float mid = (a + b) * 0.5f, play = std::max(0.0f, std::min(mid - from, to - mid));
                from = mid - play, to = mid + play;
            }
            if (to - from < 1.0f) from = (from + to) * 0.5f - 0.5f, to = from + 1.0f;
        };
        float from = 0.0f, to = 0.0f;
        bounds(deskWorld ? 0.5f : 0.2f, true, from, to);
        if (deskWorld && g_canvasFree) {
            // Out over the desk while you drag (up to most of a screen past the edge); still for a moment, the bounds
            // ease back in and the game's clamp brings the map back with them
            float hardFrom = 0.0f, hardTo = 0.0f;
            bounds(0.9f, false, hardFrom, hardTo);
            const float anchor = at(axis ? 0x1b4 : 0x1b0);
            const float over = std::max({0.0f, from - anchor, anchor - to}); // past the resting bounds now
            // dragging: the mouse held, or the map moved further out in the last second (the ease back only moves it in)
            if (mouseDown || over > g_prevOver[axis] + 0.01f) g_lastOut = now;
            g_prevOver[axis] = over;
            const bool active = now - g_lastOut < 1000;
            if (active) g_extra[axis] = over;
            else g_extra[axis] = std::min(g_extra[axis], over) * decay;
            if (active) from = hardFrom, to = hardTo;
            else {
                from = std::max(hardFrom, from - g_extra[axis]), to = std::min(hardTo, to + g_extra[axis]);
                // The view's other copies of where it is (the pan's target +0x30, the smoothing's +0x1a8) float in with
                // it: a click while it floated took the map back out to where the drag had left them (2026-11-12)
                float& target = *reinterpret_cast<float*>(mapView + 0x30 + axis * 4);
                if (std::isfinite(target)) target = std::clamp(target, from, to);
                // +0x1a8 kept the last drag's spot even inside the bounds (y 8843 with the map at 6530, zoomed out), and
                // the next click went on from there: with no drag going, it is where the map is
                if (!mouseDown) *reinterpret_cast<float*>(mapView + 0x1a8 + axis * 4) = at(axis ? 0x1b4 : 0x1b0);
            }
        }
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
    g_data->on = hall ? 1 : 2;
}
void Off() {
    if (g_data) g_data->on = 0;
    if (g_canvasFree) *g_canvasFree = 0;
}

} // namespace hall_pan
