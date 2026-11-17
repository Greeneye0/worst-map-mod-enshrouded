// Find's search results on the World Map and the minimap (asked by Find's session for the user, 2026-11-17: Find doesn't
// draw on the map itself). Find (find.dll, a hot-reloaded copy may be named find.<n>.dll) exports
//   int FindFoundTargets(float* xyz, uint32_t* entities, int max, uint32_t* generation)
// which copies what its search lights now (under its own lock, any thread): x, y, z per target in world metres (x east,
// z north), the count back (may exceed max), and a generation that changes with every new search. No export (an older
// Find, or no Find): nothing is drawn.
// Each target: the game's five-pointed star (FbUiBundle icons.genericIcons.star5) in Find's gold, as on Find's compass.

using FindFoundTargetsFn = int (*)(float* xyz, uint32_t* entities, int max, uint32_t* generation);

constexpr size_t kFindStorageIcon = 0xbbc8;     // FbUiBundle icons.genericIcons.star5 (build 2026-11-04; the user picked it, 2026-11-17)
constexpr uint32_t kFindGold = 0xFF3CC8FFu;     // Find's gold (AABBGGRR)
constexpr int kMaxFindPins = 512;

FindFoundTargetsFn g_findFoundTargets = nullptr;
uint64_t g_findTargetsTryTick = 0;

// The newest loaded Find's export, looked up again every 2 s while missing. A pointer whose module is gone (Find
// hot-reloaded) is dropped before it is called.
FindFoundTargetsFn FindTargetsExport() {
    if (g_findFoundTargets) {
        HMODULE owner = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(g_findFoundTargets), &owner) && owner)
            return g_findFoundTargets;
        g_findFoundTargets = nullptr;
        g_findTargetsTryTick = 0;
    }
    const uint64_t now = GetTickCount64();
    if (g_findTargetsTryTick != 0 && now - g_findTargetsTryTick < 2000) return nullptr;
    g_findTargetsTryTick = now;
    HMODULE mods[1024]{};
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return nullptr;
    const size_t count = std::min<size_t>(needed / sizeof(HMODULE), 1024);
    for (size_t i = count; i-- > 0;) { // the newest copy first
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(mods[i], path, MAX_PATH)) continue;
        const wchar_t* base = path;
        for (const wchar_t* p = path; *p; ++p)
            if (*p == L'\\' || *p == L'/') base = p + 1;
        if (_wcsnicmp(base, L"find.", 5) != 0) continue;
        if (auto fn = reinterpret_cast<FindFoundTargetsFn>(GetProcAddress(mods[i], "FindFoundTargets"))) {
            g_findFoundTargets = fn;
            if (g_log) g_log("Find pins: FindFoundTargets found in %ls", base);
            return fn;
        }
    }
    return nullptr;
}

// The targets now (x, z pairs in out), read once per tick for both maps
size_t ReadFindTargets(const float*& out) {
    static float s_xyz[kMaxFindPins * 3];
    static float s_xz[kMaxFindPins * 2];
    static size_t s_n = 0;
    static uint64_t s_tick = 0;
    const uint64_t now = GetTickCount64();
    if (now != s_tick) {
        s_tick = now;
        s_n = 0;
        if (FindFoundTargetsFn fn = FindTargetsExport()) {
            uint32_t gen = 0;
            const int got = fn(s_xyz, nullptr, kMaxFindPins, &gen);
            const size_t n = got <= 0 ? 0 : std::min<size_t>(static_cast<size_t>(got), kMaxFindPins);
            for (size_t i = 0; i < n; ++i) {
                const float x = s_xyz[i * 3], z = s_xyz[i * 3 + 2];
                if (!std::isfinite(x) || !std::isfinite(z) || x < 1.0f || z < 1.0f || x > 20000.0f || z > 20000.0f) continue;
                s_xz[s_n * 2] = x, s_xz[s_n * 2 + 1] = z;
                ++s_n;
            }
#ifdef UMM_DEV_BRIDGE
            static uint32_t s_lastGen = 0;
            static size_t s_lastN = 0;
            if (g_log && (gen != s_lastGen || s_n != s_lastN))
                g_log("Find pins: generation %u, %d targets (%zu drawn)", gen, got, s_n),
                      s_lastGen = gen, s_lastN = s_n;
#endif
        }
    }
    out = s_xz;
    return s_n;
}

// A dark disc with the gold storage icon on it
void DrawFindPin(void* widgetRaw, float cx, float cy, float size, float alpha) {
    const float disc = size * 1.25f;
    DrawAssetRectAbs(widgetRaw, kCirclePrimitive, cx - disc * 0.5f, cy - disc * 0.5f, disc, disc, 0xFF0A0E10u, 0.75f * alpha);
    DrawAssetRectAbs(widgetRaw, kFindStorageIcon, cx - size * 0.5f, cy - size * 0.5f, size, size, kFindGold, alpha, 1.1f);
}

void DrawFindPinsOnMap(void* widgetRaw, const float* rootRect, uint8_t* mapState) {
    if (!widgetRaw || !mapState || halls::MapActive()) return;
    const float* xz = nullptr;
    const size_t n = ReadFindTargets(xz);
    if (n == 0) return;
    float zoom = 0.0f, originX = 0.0f, originY = 0.0f, mapWorldH = 0.0f;
    if (!LevelMapView(mapState, zoom, originX, originY, mapWorldH) || !(zoom > 0.0f)) return;
    const float left = rootRect[0], top = rootRect[1];
    const float right = left + (rootRect[2] > 100.0f ? rootRect[2] : 1920.0f);
    const float bottom = top + (rootRect[3] > 100.0f ? rootRect[3] : 1080.0f);
    constexpr float kMapBannerBottom = 114.0f;
    const float size = std::max(18.0f, std::min(30.0f, 14.0f + zoom * 6.0f));
    for (size_t i = 0; i < n; ++i) {
        const float sx = originX + xz[i * 2] * zoom, sy = originY + (mapWorldH - xz[i * 2 + 1]) * zoom;
        if (sx < left - size || sx > right + size || sy < top - size || sy > bottom + size) continue;
        DrawFindPin(widgetRaw, sx, sy, size, sy < top + kMapBannerBottom ? 0.22f : 1.0f);
    }
}

// Minimap: like Town Portal's portals, a target past the edge is held at the edge, a little smaller
void DrawFindPinsOnMinimap(void* widgetRaw, float vcx, float vcy, float playerX, float playerZ, float radiusMeters,
                           float pxPerMeterX, float pxPerMeterY, float btnScale) {
    const float* xz = nullptr;
    const size_t n = ReadFindTargets(xz);
    const float clampLimit = radiusMeters * 0.91f;
    for (size_t i = 0; i < n; ++i) {
        float dxM = xz[i * 2] - playerX, dzM = xz[i * 2 + 1] - playerZ;
        MmRotate(dxM, dzM);
        bool clamped = false;
        const float maxAxis = std::max(std::abs(dxM), std::abs(dzM));
        if (maxAxis > clampLimit && maxAxis > 0.001f) {
            const float s = clampLimit / maxAxis;
            dxM *= s, dzM *= s;
            clamped = true;
        }
        DrawFindPin(widgetRaw, vcx + dxM * pxPerMeterX, vcy - dzM * pxPerMeterY, (clamped ? 13.0f : 17.0f) * btnScale,
                    clamped ? 0.8f : 1.0f);
    }
}
