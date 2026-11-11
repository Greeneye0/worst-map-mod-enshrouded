// Included in desk.inl (namespace desk). The map desk's props the mod draws itself (tools/resources/desk_props.py
// lifts them out of the painting; data/desk/props/):
//  - the lantern: its sprite (props/lantern_R_C.png, 256 px tiles at the hi scale) over a soft blue light on the desk
//    that pulses a little and fades out before the map; a faint light in its glass breathes with it.
//  - the compass: a 3D one, built each frame from triangles (ui_mesh, lit per vertex; its dial the one texture,
//    props/compass_card.png): case, bezel, recessed dial on a gimbal, a needle that holds north, the glass and the bail.
//    The table's moves shake it in proportion to their jerk: its on-screen acceleration tips the dial plate against
//    the move (a damped pendulum) and swings the needle off north (a lightly damped spring), and both settle again.
// props.txt: "lantern X Y W H cols C rows R" (its box, desk.png px) and "compass CX CY R" (the bowl's top face).

struct Props {
    bool lantern = false, compass = false;
    float lan[4]{};
    int lanCols = 0, lanRows = 0;
    std::vector<std::vector<uint32_t>> lanTiles; // [row * cols + col], premultiplied (empty: clear)
    float comp[3]{};
    std::vector<uint32_t> card; // 256 x 256, premultiplied
    uint64_t hash = 0;          // of their pixels: in the image pool's keys (the pool outlives a reload, and a key it
                                // holds keeps its old pixels: a re-cut lantern showed old tiles beside new ones)
};
Props g_props;

void LoadProps(const std::string& dir) {
    g_props = Props{};
    const std::string pdir = dir + "props\\";
    FILE* f = nullptr;
    if (fopen_s(&f, (pdir + "props.txt").c_str(), "r") != 0 || !f) return;
    char line[160];
    while (std::fgets(line, sizeof(line), f)) {
        float a, b, c, d;
        int cols, rows;
        if (sscanf_s(line, "lantern %f %f %f %f cols %d rows %d", &a, &b, &c, &d, &cols, &rows) == 6 && cols > 0 && rows > 0 &&
            cols * rows <= 16) {
            g_props.lan[0] = a, g_props.lan[1] = b, g_props.lan[2] = c, g_props.lan[3] = d;
            g_props.lanCols = cols, g_props.lanRows = rows;
            g_props.lanTiles.assign(static_cast<size_t>(cols) * rows, {});
            bool all = true;
            for (int r = 0; r < rows; ++r)
                for (int cc = 0; cc < cols; ++cc) {
                    char name[40];
                    std::snprintf(name, sizeof(name), "lantern_%d_%d.png", r, cc);
                    std::vector<uint32_t> px;
                    int w = 0, h = 0;
                    if (halls::DecodePng(Wide(pdir + name), px, w, h) && w == kTile && h == kTile) {
                        Premultiply(px);
                        g_props.lanTiles[static_cast<size_t>(r) * cols + cc] = std::move(px);
                    } else {
                        all = false;
                    }
                }
            g_props.lantern = all;
        } else if (sscanf_s(line, "compass %f %f %f", &a, &b, &c) == 3 && c > 1.0f) {
            g_props.comp[0] = a, g_props.comp[1] = b, g_props.comp[2] = c;
            int w = 0, h = 0;
            if (halls::DecodePng(Wide(pdir + "compass_card.png"), g_props.card, w, h) && w == kTile && h == kTile) {
                Premultiply(g_props.card);
                g_props.compass = true;
            }
        }
    }
    std::fclose(f);
    uint64_t h = 0xcbf29ce484222325ull;
    for (const auto& t : g_props.lanTiles)
        for (size_t i = 0; i < t.size(); i += 61) h = (h ^ t[i]) * 0x100000001b3ull;
    for (size_t i = 0; i < g_props.card.size(); i += 61) h = (h ^ g_props.card[i]) * 0x100000001b3ull;
    g_props.hash = h;
    if (g_log) g_log("Map desk: props: lantern %d, compass %d", g_props.lantern ? 1 : 0, g_props.compass ? 1 : 0);
}

// ---- the lantern
void DrawLantern(void* widget, float dx, float dy, float s, const float* clip, float t) {
    if (!g_props.lantern) return;
    const float* L = g_props.lan;
    const float gx = dx + (L[0] + L[2] * 0.5f) * s, gy = dy + (L[1] + L[3] * 0.56f) * s; // its glass's middle
    // the light on the desk: a slow breath and a faint flicker; it reaches ~0.85 lantern widths round the glass, short
    // of the map's frame (~1 lantern width away)
    const float breath = 0.5f + 0.5f * std::sin(t * 1.15f);
    const float flicker = 0.04f * std::sin(t * 13.7f) * std::sin(t * 7.3f + 1.1f);
    const float a = 0.30f + 0.10f * breath + flicker;
    const float r1 = L[2] * 0.95f * s, r2 = L[2] * 0.62f * s;
    DrawAssetRectAbs(widget, kSoftGlowPrimitive, gx - r1, gy - r1 * 0.82f, r1 * 2.0f, r1 * 1.64f, 0xffff9440u, a * 0.55f * g_deskDim);
    DrawAssetRectAbs(widget, kSoftGlowPrimitive, gx - r2, gy - r2 * 0.82f, r2 * 2.0f, r2 * 1.64f, 0xffffb46au, a * 0.7f * g_deskDim);
    const float tile = static_cast<float>(kTile) / static_cast<float>(g_hiScale); // a sprite tile in desk.png px
    for (int r = 0; r < g_props.lanRows; ++r)
        for (int c = 0; c < g_props.lanCols; ++c) {
            const auto& px = g_props.lanTiles[static_cast<size_t>(r) * g_props.lanCols + c];
            const uint64_t key = (g_keyBase ^ g_props.hash ^ 0x6c616e7465726e00ull ^ static_cast<uint64_t>(r * 16 + c + 1) * 0x9e3779b97f4a7c15ull) | 1;
            DrawTile(widget, px.data(), key, dx, dy, s, L[0] + c * tile, L[1] + r * tile, tile, clip);
        }
    // the light in the glass, breathing with the glow
    const float r3 = L[2] * 0.30f * s;
    DrawAssetRectAbs(widget, kSoftGlowPrimitive, gx - r3, gy - r3, r3 * 2.0f, r3 * 2.0f, 0xffffe0b0u, (0.10f + 0.16f * breath + flicker) * g_deskDim);
}

// ---- the compass
struct V3 {
    float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Norm(V3 a) {
    const float l = std::sqrt(Dot(a, a));
    return l > 1e-6f ? a * (1.0f / l) : V3{0, 0, 1};
}

struct CompassState {
    bool have = false;
    LARGE_INTEGER last{};
    float px = 0, py = 0, vx = 0, vy = 0;   // the bowl on screen (UI px) and its speed
    float scale = 0;                        // the desk's scale last frame (a zoom moves the bowl too: not felt)
    float sx = 0, sy = 0, ax = 0, ay = 0;   // its speed and acceleration smoothed (a drag's frame-to-frame steps are noisy)
    float cx = 0, cy = 0;                   // the cursor last frame (a drag is felt even where the map can't move on)
    uint64_t zoomTick = 0;                  // the last zoom change (the map settling into its bounds after it: not felt)
    float peakA = 0, peakPhi = 0;           // dev: the hardest shove and swing since the last log line
    uint64_t peakLog = 0;
    float tx = 0, ty = 0, tvx = 0, tvy = 0; // the dial's tilt (slope across, slope along) and its rate
    float phi = 0, phiV = 0;                // the needle off north (radians) and its rate
};
CompassState g_compass;

// The desk seen a little from its near side: a point (u across, v down the desk, z up; desk.png px from the bowl's
// middle) on screen. The painting's round bowl shows 0.91 as tall as wide: the view's pitch.
constexpr float kCosP = 0.91f, kSinP = 0.415f;

void DrawCompass(void* widget, float dx, float dy, float s) {
    if (!g_props.compass || !ui_mesh::Ready()) return;
    const float R = g_props.comp[2];
    const float ox = dx + g_props.comp[0] * s, oy = dy + g_props.comp[1] * s;
    // physics: the bowl's acceleration on screen, in desk px/s^2 (zooming in moves it too: also felt)
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    CompassState& c = g_compass;
    float dt = c.have ? static_cast<float>(now.QuadPart - c.last.QuadPart) / static_cast<float>(freq.QuadPart) : 0.0f;
    if (!c.have || dt > 0.1f || dt <= 0.0f) { // first frame, or the map was away: at rest where it is
        c = CompassState{};
        c.have = true, c.last = now, c.px = ox, c.py = oy, c.scale = s;
        dt = 0.0f;
    }
    if (dt > 0.0f) {
        // only the table being moved shakes it: while the zoom changes, the bowl's new place is just where it is (no
        // speed carried over, so a zoom's end isn't a jolt either)
        const uint64_t nowMs = GetTickCount64();
        if (std::fabs(s - c.scale) > 1e-4f * s) c.zoomTick = nowMs;
        // a zoom, and the map settling into its bounds for 0.4 s after it (the Map Desk's pan clamp re-centres it)
        const bool zooming = nowMs - c.zoomTick < 400;
        // the speed in desk px/s: of the cursor while it drags the map (the map stops at its bounds, the drag goes on:
        // a hard pull is felt where the table can't move), else of the table; smoothed over ~60 ms, and its change
        // over ~40 ms: the drag's per-frame jitter left out, a real shove kept
        const bool dragging = g_mapCursorIn && KeyIsDown(VK_LBUTTON);
        float vx = (ox - c.px) / dt / s, vy = (oy - c.py) / dt / s;
        if (dragging && c.cx != 0.0f) vx = (g_mapCursor[0] - c.cx) / dt / s, vy = (g_mapCursor[1] - c.cy) / dt / s;
        c.cx = g_mapCursor[0], c.cy = g_mapCursor[1];
        if (zooming) vx = vy = 0.0f;
        const float kv = 1.0f - std::exp(-dt / 0.06f), ka = 1.0f - std::exp(-dt / 0.04f);
        const float psx = c.sx, psy = c.sy;
        if (zooming) c.sx = c.sy = 0.0f;
        else c.sx += (vx - c.sx) * kv, c.sy += (vy - c.sy) * kv;
        c.ax += ((zooming ? 0.0f : (c.sx - psx) / dt) - c.ax) * ka;
        c.ay += ((zooming ? 0.0f : (c.sy - psy) / dt) - c.ay) * ka;
        const float ax = c.ax, ay = c.ay;
        c.px = ox, c.py = oy, c.vx = vx, c.vy = vy, c.last = now, c.scale = s;
#ifdef UMM_DEV_BRIDGE
        c.peakA = std::max(c.peakA, std::hypot(ax, ay));
        c.peakPhi = std::max(c.peakPhi, std::fabs(c.phi));
        if (GetTickCount64() - c.peakLog > 700) {
            if (c.peakA > 50.0f && g_log) g_log("Compass: peak shove %.0f desk px/s^2, needle %.2f rad, tilt %.2f/%.2f", c.peakA, c.peakPhi, c.tx, c.ty);
            c.peakA = c.peakPhi = 0.0f, c.peakLog = GetTickCount64();
        }
#endif
        for (int step = 0; step < 4; ++step) { // a few small steps: stiff springs stay stable at low frame rates
            const float h = dt / 4.0f;
            // the dial on its gimbal: a pendulum tipping against the move
            constexpr float kW = 2.0f * 3.14159f * 1.3f, kZ = 0.32f, kTip = 2.5e-3f; // (measured: a normal pan ~5000, a hard one ~130000)
            c.tvx += (-kW * kW * c.tx - 2.0f * kZ * kW * c.tvx - ax * kTip) * h;
            c.tvy += (-kW * kW * c.ty - 2.0f * kZ * kW * c.tvy - ay * kTip) * h;
            c.tx = std::clamp(c.tx + c.tvx * h, -0.28f, 0.28f);
            c.ty = std::clamp(c.ty + c.tvy * h, -0.28f, 0.28f);
            // the needle: north is up the desk; a push across it swings it, the dial's rocking stirs it, it settles
            constexpr float kWn = 2.0f * 3.14159f * 0.75f, kZn = 0.11f, kPush = 3.2e-3f, kStir = 0.8f; // ~15 degrees / ~50
            const float nx = std::sin(c.phi), ny = -std::cos(c.phi);
            const float torque = kPush * (nx * ay - ny * ax) + kStir * (c.tvx * ny - c.tvy * nx);
            c.phiV += (-kWn * kWn * c.phi - 2.0f * kZn * kWn * c.phiV + torque) * h;
            c.phi = std::clamp(c.phi + c.phiV * h, -1.6f, 1.6f);
        }
    }
    // geometry
    const V3 light = Norm({-0.6f, -0.7f, 0.55f}), view = Norm({0.0f, kSinP, kCosP}); // low, from up and left
    auto proj = [&](V3 p, float& x, float& y) {
        x = ox + p.x * s;
        y = oy + (p.y * kCosP - p.z * kSinP) * s;
    };
    auto shade = [&](V3 n, float r, float g, float b, float spec, float alpha = 1.0f) -> uint32_t {
        n = Norm(n);
        const float diff = std::max(0.0f, Dot(n, light));
        const V3 h = Norm(light + view);
        const float sp = spec * std::pow(std::max(0.0f, Dot(n, h)), 28.0f);
        auto ch = [&](float base) { return static_cast<uint32_t>(std::clamp(base * (0.22f + 1.05f * diff) + 255.0f * sp, 0.0f, 255.0f)); };
        return (static_cast<uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f) << 24) | (ch(b) << 16) | (ch(g) << 8) | ch(r);
    };
    smooth_highlight::Batch back, front;
    auto vert = [&](smooth_highlight::Batch& b, V3 p, uint32_t col, float u = 0, float v = 0) {
        float x, y;
        proj(p, x, y);
        auto dimc = [&](uint32_t c, int sh) { return static_cast<uint32_t>(((c >> sh) & 0xff) * g_deskDim) << sh; };
        col = (col & 0xff000000u) | dimc(col, 16) | dimc(col, 8) | dimc(col, 0);
        b.vertices.push_back({x, y, col, u, v, 1.0f});
        return static_cast<uint16_t>(b.vertices.size() - 1);
    };
    auto quad = [&](smooth_highlight::Batch& b, uint16_t a0, uint16_t a1, uint16_t a2, uint16_t a3) {
        b.indices.insert(b.indices.end(), {a0, a1, a2, a0, a2, a3});
    };
    constexpr int kSeg = 48;
    constexpr float kPi = 3.14159265f;
    const float H = R * 0.30f, inner = R * 0.84f, cardZ = H - R * 0.08f;
    const float gr = 196, gg = 140, gb = 56; // the painting's gold (lit up to its highlights)
    // the soft shadow under it (down and right of the light)
    DrawAssetRectAbs(widget, kSoftGlowPrimitive, ox - R * 1.25f * s + R * 0.18f * s, oy - R * 1.05f * s + R * 0.22f * s,
                     R * 2.5f * s, R * 2.2f * s, 0xff000000u, 0.6f);
    // the bail: a ring lying north of the bowl on a short stem
    {
        const float bc = -(R + R * 0.34f), br = R * 0.26f, bw = R * 0.075f;
        for (int i = 0; i < 24; ++i) {
            const float a0 = i * 2 * kPi / 24, a1 = (i + 1) * 2 * kPi / 24;
            auto pt = [&](float a, float r) { return V3{r * std::sin(a), bc - r * std::cos(a), H * 0.55f}; };
            const V3 n0{std::sin(a0), -std::cos(a0), 0.9f}, n1{std::sin(a1), -std::cos(a1), 0.9f};
            const uint16_t q0 = vert(back, pt(a0, br + bw), shade(n0, gr, gg, gb, 0.9f)), q1 = vert(back, pt(a1, br + bw), shade(n1, gr, gg, gb, 0.9f));
            const uint16_t q2 = vert(back, pt(a1, br - bw), shade(n1 * -1.0f + V3{0, 0, 1.8f}, gr, gg, gb, 0.9f));
            const uint16_t q3 = vert(back, pt(a0, br - bw), shade(n0 * -1.0f + V3{0, 0, 1.8f}, gr, gg, gb, 0.9f));
            quad(back, q0, q1, q2, q3);
        }
        const uint16_t s0 = vert(back, {-R * 0.07f, -R * 0.97f, H * 0.8f}, shade({-0.6f, 0, 1}, gr, gg, gb, 0.8f));
        const uint16_t s1 = vert(back, {R * 0.07f, -R * 0.97f, H * 0.8f}, shade({0.6f, 0, 1}, gr, gg, gb, 0.8f));
        const uint16_t s2 = vert(back, {R * 0.07f, bc + br + bw * 0.5f, H * 0.6f}, shade({0.6f, 0, 1}, gr, gg, gb, 0.8f));
        const uint16_t s3 = vert(back, {-R * 0.07f, bc + br + bw * 0.5f, H * 0.6f}, shade({-0.6f, 0, 1}, gr, gg, gb, 0.8f));
        quad(back, s0, s1, s2, s3);
    }
    // the case's wall (its near half shows) and the bezel's top
    for (int i = 0; i < kSeg; ++i) {
        const float a0 = i * 2 * kPi / kSeg, a1 = (i + 1) * 2 * kPi / kSeg;
        const V3 n0{std::sin(a0), -std::cos(a0), 0.15f}, n1{std::sin(a1), -std::cos(a1), 0.15f};
        if (std::cos(a0) < 0.25f || std::cos(a1) < 0.25f) { // facing us (down the desk)
            auto w = [&](float a, float z) { return V3{R * std::sin(a), -R * std::cos(a), z}; };
            quad(back, vert(back, w(a0, 0), shade(n0, gr * 0.8f, gg * 0.8f, gb * 0.8f, 0.6f)), vert(back, w(a1, 0), shade(n1, gr * 0.8f, gg * 0.8f, gb * 0.8f, 0.6f)),
                 vert(back, w(a1, H), shade(n1, gr, gg, gb, 0.6f)), vert(back, w(a0, H), shade(n0, gr, gg, gb, 0.6f)));
        }
        // the bezel: a rounded ring from the rim (outer, sloping out) to the dial's edge (inner, sloping in)
        auto b = [&](float a, float r, float z) { return V3{r * std::sin(a), -r * std::cos(a), z}; };
        const float mid = (R + inner) * 0.5f;
        const V3 out0{std::sin(a0) * 0.7f, -std::cos(a0) * 0.7f, 1}, out1{std::sin(a1) * 0.7f, -std::cos(a1) * 0.7f, 1};
        const V3 in0{-std::sin(a0) * 0.7f, std::cos(a0) * 0.7f, 1}, in1{-std::sin(a1) * 0.7f, std::cos(a1) * 0.7f, 1};
        quad(back, vert(back, b(a0, R, H), shade(out0, gr, gg, gb, 1.0f)), vert(back, b(a1, R, H), shade(out1, gr, gg, gb, 1.0f)),
             vert(back, b(a1, mid, H + R * 0.05f), shade({0, 0, 1}, gr, gg, gb, 1.0f)), vert(back, b(a0, mid, H + R * 0.05f), shade({0, 0, 1}, gr, gg, gb, 1.0f)));
        quad(back, vert(back, b(a0, mid, H + R * 0.05f), shade({0, 0, 1}, gr, gg, gb, 1.0f)), vert(back, b(a1, mid, H + R * 0.05f), shade({0, 0, 1}, gr, gg, gb, 1.0f)),
             vert(back, b(a1, inner, H), shade(in1, gr, gg, gb, 1.0f)), vert(back, b(a0, inner, H), shade(in0, gr, gg, gb, 1.0f)));
        // the recess's dark wall down to the dial
        quad(back, vert(back, b(a0, inner, H), 0xff1a2028u), vert(back, b(a1, inner, H), 0xff1a2028u),
             vert(back, b(a1, inner * 0.985f, cardZ), 0xff10151au), vert(back, b(a0, inner * 0.985f, cardZ), 0xff10151au));
    }
    ui_mesh::Draw(widget, back);
    // the dial on its gimbal (tipped by c.tx / c.ty), textured
    auto tipped = [&](float u, float v, float z) { return V3{u, v, z + u * c.tx + v * c.ty}; };
    const float cr = inner * 0.985f;
    const uint64_t cardKey = (g_keyBase ^ g_props.hash ^ 0x636f6d7061737300ull) | 1;
    const uint32_t cardId = hdpool::Request(widget, g_props.card.data(), cardKey);
    {
        smooth_highlight::Batch dial;
        // lit as a plate: brighter tipped toward the light
        const V3 n = Norm({-c.tx, -c.ty, 1});
        const uint32_t lit = shade(n, 255, 255, 255, 0.0f);
        const uint16_t mid = vert(dial, tipped(0, 0, cardZ), cardId ? lit : 0xff1c1a18u, 0.5f, 0.5f);
        for (int i = 0; i <= kSeg; ++i) {
            const float a = i * 2 * kPi / kSeg;
            vert(dial, tipped(cr * std::sin(a), -cr * std::cos(a), cardZ), cardId ? lit : 0xff1c1a18u, 0.5f + 0.5f * std::sin(a), 0.5f - 0.5f * std::cos(a));
            if (i) dial.indices.insert(dial.indices.end(), {mid, static_cast<uint16_t>(mid + i), static_cast<uint16_t>(mid + i + 1)});
        }
        ui_mesh::Draw(widget, dial, cardId);
    }
    // the needle: two halves on a ridge (each side its own facet: lit and shaded), north red, south steel
    {
        const float len = cr * 0.80f, wide = cr * 0.10f, nz = cardZ + R * 0.07f, ridge = R * 0.035f;
        const float ax = std::sin(c.phi), ay = -std::cos(c.phi); // north on the desk
        const float px = -ay, py = ax;                         // across it
        auto half = [&](float dir, float r, float g, float b) {
            const V3 tip = tipped(ax * len * dir, ay * len * dir, nz), mid = tipped(0, 0, nz + ridge);
            const V3 left = tipped(px * wide, py * wide, nz), right = tipped(-px * wide, -py * wide, nz);
            const V3 nl{px - ax * dir * 0.2f, py - ay * dir * 0.2f, 1.4f}, nr{-px - ax * dir * 0.2f, -py - ay * dir * 0.2f, 1.4f};
            const uint32_t cl = shade(nl, r, g, b, 1.2f), cr2 = shade(nr, r, g, b, 1.2f);
            const uint16_t t0 = vert(front, tip, cl), m0 = vert(front, mid, cl), l0 = vert(front, left, cl);
            const uint16_t t1 = vert(front, tip, cr2), m1 = vert(front, mid, cr2), r1 = vert(front, right, cr2);
            front.indices.insert(front.indices.end(), {t0, m0, l0, t1, r1, m1});
        };
        half(1.0f, 205, 48, 34);   // north: red
        half(-1.0f, 190, 186, 176); // south: steel
        // the pivot's cap
        const uint16_t cm = vert(front, tipped(0, 0, nz + ridge * 1.6f), shade({0, 0, 1}, gr, gg, gb, 1.5f));
        for (int i = 0; i <= 12; ++i) {
            const float a = i * 2 * kPi / 12, r = wide * 0.75f;
            vert(front, tipped(r * std::sin(a), -r * std::cos(a), nz + ridge), shade({std::sin(a), -std::cos(a), 1.2f}, gr, gg, gb, 1.2f));
            if (i) front.indices.insert(front.indices.end(), {cm, static_cast<uint16_t>(cm + i), static_cast<uint16_t>(cm + i + 1)});
        }
    }
    // the glass: a faint sheen and a bright crescent toward the light
    {
        const float gz = H + R * 0.03f;
        for (int i = 0; i < kSeg; ++i) {
            const float a0 = i * 2 * kPi / kSeg, a1 = (i + 1) * 2 * kPi / kSeg;
            auto g = [&](float a, float r) { return V3{r * std::sin(a), -r * std::cos(a), gz}; };
            // brightest up and left (toward the light), nothing on the far side
            auto glow = [&](float a) { return std::pow(std::max(0.0f, std::cos(a + 0.75f)), 3.0f); };
            const uint32_t c0 = (static_cast<uint32_t>(150 * glow(a0)) << 24) | 0xffffff, c1 = (static_cast<uint32_t>(150 * glow(a1)) << 24) | 0xffffff;
            quad(front, vert(front, g(a0, inner * 0.92f), c0), vert(front, g(a1, inner * 0.92f), c1),
                 vert(front, g(a1, inner * 0.78f), c1 & 0x00ffffffu), vert(front, g(a0, inner * 0.78f), c0 & 0x00ffffffu));
        }
    }
    ui_mesh::Draw(widget, front);
}

void DrawProps(void* widget, float dx, float dy, float s, const float* clip) {
    static const uint64_t start = GetTickCount64();
    const float t = static_cast<float>(GetTickCount64() - start) / 1000.0f;
    DrawLantern(widget, dx, dy, s, clip, t);
    DrawCompass(widget, dx, dy, s);
}
