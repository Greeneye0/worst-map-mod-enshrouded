// Town Portal's open portals on the world map and the minimap, each in its owner's colour.
// Town Portal (another mod in the same game process) publishes them in a small shared block,
// Local\TownPortalMarkers.<pid>: this side only opens it read-only and copies it under its seqlock.
// Without Town Portal there is no block and nothing is drawn.

#pragma pack(push, 4)
struct TownPortalMarker {   // 60 bytes (the fields as published; pack(4) adds no padding)
    float x, y, z;          // world position of the portal end (m)
    uint32_t color;         // ABGR 0xAABBGGRR, the portal's vivid colour
    uint32_t ownerPlayerId; // player-list id
    uint8_t side;           // 0 = field end, 1 = base end (at the Flame Altar)
    uint8_t isLocalOwner;
    uint8_t forming;        // 1 while still being cast
    uint8_t reserved;
    float yaw;
    char ownerName[32];     // UTF-8, may be empty
};
struct TownPortalMarkers {
    uint32_t magic; // 'TPM1'
    uint32_t version;
    volatile uint32_t sequence; // odd while being written
    uint32_t count;
    TownPortalMarker markers[16];
};
#pragma pack(pop)
static_assert(sizeof(TownPortalMarker) == 60, "Town Portal marker layout");
static_assert(sizeof(TownPortalMarkers) == 16 + 16 * 60, "Town Portal markers layout");

constexpr uint32_t kTownPortalMagic = 0x314D5054u; // 'TPM1'

struct TownPortalSource {
    HANDLE mapping = nullptr;
    const TownPortalMarkers* view = nullptr;
    uint64_t lastTryTick = 0;
};
TownPortalSource g_townPortalSource;

// Opens the block (at most once a second while it does not exist yet) and keeps it mapped
const TownPortalMarkers* TownPortalView() {
    TownPortalSource& src = g_townPortalSource;
    if (src.view) return src.view;
    const uint64_t now = GetTickCount64();
    if (src.lastTryTick != 0 && now - src.lastTryTick < 1000) return nullptr;
    src.lastTryTick = now;
    wchar_t name[64]{};
    swprintf_s(name, L"Local\\TownPortalMarkers.%lu", static_cast<unsigned long>(GetCurrentProcessId()));
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!mapping) return nullptr;
    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(TownPortalMarkers));
    if (!view) {
        CloseHandle(mapping);
        return nullptr;
    }
    src.mapping = mapping;
    src.view = static_cast<const TownPortalMarkers*>(view);
    return src.view;
}

// Before a reload: the next generation maps its own view
void ReleaseTownPortalSource() {
    TownPortalSource& src = g_townPortalSource;
    if (src.view) UnmapViewOfFile(src.view);
    if (src.mapping) CloseHandle(src.mapping);
    src = TownPortalSource{};
}

// A stable copy of the open portals (keeps the last good copy for this frame when Town Portal is mid-write).
// Returns the number of usable markers in out.
size_t ReadTownPortals(TownPortalMarker (&out)[16]) {
    const TownPortalMarkers* view = TownPortalView();
    if (!view) return 0;
    static TownPortalMarkers copy{};
    static size_t lastCount = 0;
    static uint64_t lastFrameTick = 0;
    const uint64_t now = GetTickCount64();
    // Read once per tick (the world map and the minimap may both ask in a frame)
    if (now != lastFrameTick) {
        lastFrameTick = now;
        for (int attempt = 0; attempt < 2; ++attempt) {
            const uint32_t seq0 = view->sequence;
            if (seq0 & 1u) continue;
            std::atomic_thread_fence(std::memory_order_acquire);
            TownPortalMarkers tmp;
            std::memcpy(&tmp, const_cast<const TownPortalMarkers*>(view), sizeof(tmp));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (view->sequence != seq0) continue;
            if (tmp.magic != kTownPortalMagic || tmp.version != 1 || tmp.count > 16) {
                lastCount = 0;
                break;
            }
            copy = tmp;
            lastCount = tmp.count;
            break;
        }
    }
    size_t n = 0;
    for (size_t i = 0; i < lastCount; ++i) {
        const TownPortalMarker& m = copy.markers[i];
        if (!std::isfinite(m.x) || !std::isfinite(m.z) || m.x < 1.0f || m.z < 1.0f || m.x > 20000.0f || m.z > 20000.0f)
            continue;
        out[n] = m;
        out[n].ownerName[sizeof(m.ownerName) - 1] = '\0';
        ++n;
    }
    return n;
}

// The portal's colour lifted a third of the way to white, for its rim
uint32_t TownPortalRimColor(uint32_t c) {
    auto lift = [](uint32_t ch) { return ch + (255u - ch) / 3u; };
    return 0xFF000000u | (lift((c >> 16) & 0xFFu) << 16) | (lift((c >> 8) & 0xFFu) << 8) | lift(c & 0xFFu);
}

// An upright oval ring h tall, centred on (cx, cy): dark edge, the colour faintly filled, a bright rim. The base end
// (at the Flame Altar) has a small solid core so the two ends of one portal tell apart. Forming portals breathe.
void DrawTownPortalIcon(void* widgetRaw, float cx, float cy, float h, const TownPortalMarker& m, float alpha) {
    if (m.forming) {
        const float t = static_cast<float>(GetTickCount64() % 1200u) / 1200.0f;
        alpha *= 0.45f + 0.35f * (0.5f + 0.5f * std::sin(t * 6.2831853f));
    }
    const uint32_t col = m.color | 0xFF000000u;
    const uint32_t rim = TownPortalRimColor(col);
    const float w = h * 0.74f;
    DrawAssetRectAbs(widgetRaw, kCirclePrimitive, cx - w * 0.5f - 2.0f, cy - h * 0.5f - 2.0f, w + 4.0f, h + 4.0f,
                     0xFF0A0E10u, 0.8f * alpha);
    DrawAssetRectAbs(widgetRaw, kCirclePrimitive, cx - w * 0.5f, cy - h * 0.5f, w, h, col, 0.5f * alpha, 1.1f);
    DrawAssetRectAbs(widgetRaw, kCircleOutlinePrimitive, cx - w * 0.5f, cy - h * 0.5f, w, h, rim, alpha, 1.2f);
    DrawAssetRectAbs(widgetRaw, kCircleOutlinePrimitive, cx - w * 0.5f + 1.0f, cy - h * 0.5f + 1.0f, w - 2.0f, h - 2.0f,
                     col, alpha, 1.15f);
    if (m.side == 1) {
        const float core = h * 0.3f;
        DrawAssetRectAbs(widgetRaw, kCirclePrimitive, cx - core * 0.5f, cy - core * 0.5f, core, core, rim, alpha, 1.2f);
    }
}

// World map: each portal end at its place; hovering one names its owner (no new text: the player's own name)
// The Map Key's count: open portals (a portal's two ends count once).
size_t TownPortalCount() {
    TownPortalMarker portals[16];
    const size_t n = ReadTownPortals(portals);
    size_t fieldEnds = 0;
    for (size_t i = 0; i < n; ++i) fieldEnds += portals[i].side == 0 ? 1 : 0;
    return fieldEnds ? fieldEnds : n;
}

void DrawTownPortalsOnMap(void* widgetRaw, const float* rootRect, uint8_t* mapState, bool haveCursor, float mx, float my) {
    if (!widgetRaw || !mapState) return;
    TownPortalMarker portals[16];
    const size_t n = ReadTownPortals(portals);
    if (n == 0) return;
    uint8_t* mapView = *reinterpret_cast<uint8_t**>(mapState + 8);
    if (!mapView) return;
    const float zoom = *reinterpret_cast<float*>(mapView + 0x10);
    float mapWorldH = *reinterpret_cast<float*>(mapView + 0x3c);
    if (mapWorldH <= 100.0f) mapWorldH = 10240.0f;
    if (!(zoom > 0.0f)) return;
    const float originX = *reinterpret_cast<float*>(mapView + 0x1e0) + *reinterpret_cast<float*>(mapView + 0x17c) -
                          *reinterpret_cast<float*>(mapView + 0x1e8);
    const float originY = *reinterpret_cast<float*>(mapView + 0x1e4) + *reinterpret_cast<float*>(mapView + 0x180) -
                          *reinterpret_cast<float*>(mapView + 0x1ec);
    const float left = rootRect[0], top = rootRect[1];
    const float right = left + (rootRect[2] > 100.0f ? rootRect[2] : 1920.0f);
    const float bottom = top + (rootRect[3] > 100.0f ? rootRect[3] : 1080.0f);
    constexpr float kMapBannerBottom = 114.0f; // the SOCIAL / WORLD / JOURNAL menu banner's gold edge
    const float h = std::max(16.0f, std::min(26.0f, 12.0f + zoom * 6.0f));
    const char* hoveredName = nullptr;
    float hoveredSx = 0.0f, hoveredSy = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const TownPortalMarker& m = portals[i];
        // The base end sits on the Flame Altar's icon: nudge it to the icon's top left so both show
        const float nudge = m.side == 1 ? -h * 0.55f : 0.0f;
        const float sx = originX + m.x * zoom + nudge, sy = originY + (mapWorldH - m.z) * zoom + nudge;
        if (sx < left - h || sx > right + h || sy < top - h || sy > bottom + h) continue;
        // Map Key > Regions: outside the regions shown, dimmed or not drawn
        const bool outside = OutsideFocusedRegion(m.x, m.z);
        if (outside && g_regionOutsideHide) continue;
        const bool underBanner = sy < top + kMapBannerBottom;
        DrawTownPortalIcon(widgetRaw, sx, sy, h, m, underBanner ? 0.22f : outside ? 0.3f : 1.0f);
        if (!underBanner && haveCursor && std::fabs(mx - sx) <= h * 0.5f + 2.0f && std::fabs(my - sy) <= h * 0.5f + 2.0f)
            g_hoveredDepositRow = kTownPortalTypeHash; // right-click Hide / Alt+click
        if (!underBanner && haveCursor && m.ownerName[0] && std::fabs(mx - sx) <= h * 0.5f + 2.0f &&
            std::fabs(my - sy) <= h * 0.5f + 2.0f) {
            hoveredName = m.ownerName;
            hoveredSx = sx;
            hoveredSy = sy;
        }
    }
    if (hoveredName) DrawMapHoverTitle(hoveredSx + h * 0.5f + 10.0f, hoveredSy, hoveredName);
}

// Minimap: like the other players, a portal beyond the map's edge is held at the edge, a little smaller
void DrawTownPortalsOnMinimap(void* widgetRaw, float vcx, float vcy, float playerX, float playerZ, float radiusMeters,
                              float pxPerMeterX, float pxPerMeterY, float btnScale) {
    TownPortalMarker portals[16];
    const size_t n = ReadTownPortals(portals);
    const float clampLimit = radiusMeters * 0.91f;
    for (size_t i = 0; i < n; ++i) {
        const TownPortalMarker& m = portals[i];
        if (RegionHidesAt(m.x, m.z)) continue;
        float dxM = m.x - playerX, dzM = m.z - playerZ;
        bool isClamped = false;
        const float maxAxis = std::max(std::abs(dxM), std::abs(dzM));
        if (maxAxis > clampLimit && maxAxis > 0.001f) {
            const float scale = clampLimit / maxAxis;
            dxM *= scale;
            dzM *= scale;
            isClamped = true;
        }
        const float sx = vcx + dxM * pxPerMeterX, sy = vcy - dzM * pxPerMeterY;
        DrawTownPortalIcon(widgetRaw, sx, sy, (isClamped ? 12.0f : 15.0f) * btnScale, m, isClamped ? 0.8f : 1.0f);
    }
}
