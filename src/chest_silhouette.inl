// The GG targets' silhouettes on screen, through walls and ground: Find's (FindMod/src/silhouette.cpp, mesh / raster /
// runs headers), drawn with the map mod's solid rectangles.
//
// While the render hook lights a chest it takes a snapshot of its model: the render instance's resource (+0x108) ->
// asset (+0x38, 0x160 bytes; mesh table at asset + 0x6c + rel, 56 bytes) and its transform (instance + 0x38). The
// mesh's vertex and index data are read from the game's files (enshrouded.kfc's directory -> enshrouded_NNN.dat) and
// decoded (quantized PNTCT positions). Each frame the triangles are projected with the camera, rasterized into a
// coarse grid of the chest's own (no world depth: walls never hide it), shaded by facing, outlined with two halo bands
// and merged into rectangles. Every rectangle is one UI quad in the game's per-frame dynamic buffer, which crashes the
// game when exhausted, so each chest coarsens its grid until it fits its share of kFrameRects (at most kChestRects).

namespace chest_silhouette {

struct Spatial {
    int64_t position[3];
    float rotation[4];
    float scale[3];
    uint32_t pad;
};
static_assert(sizeof(Spatial) == 56, "render instance transform");

struct Snapshot {
    unsigned char asset[0x160], mesh[56];
    Spatial transform;
    uint32_t colour; // 0xAABBGGRR
};

struct Mesh {
    std::vector<std::array<float, 3>> vertices;
    std::vector<uint16_t> indices;
};

template <class T>
T Rd(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// Quantized PNTCT: a planar 8-byte position stream; x in bits 11..31 of word 0, y across both words, z in word 1 bits
// 0..20.
bool Decode(const std::vector<uint8_t>& data, const uint8_t* asset, const uint8_t* mesh, Mesh& out) {
    out = {};
    const uint32_t n = Rd<uint32_t>(mesh + 0x14), count = Rd<uint32_t>(mesh + 0x1c);
    const uint64_t vo = uint64_t(Rd<uint32_t>(asset + 0x140)) + Rd<uint32_t>(mesh + 0x28);
    const uint64_t io = uint64_t(Rd<uint32_t>(asset + 0x130)) + uint64_t(Rd<uint32_t>(mesh + 0x18)) * 2;
    if (mesh[0x34] != 4 || mesh[0x35] != 24 || mesh[0x37] != 1 || n < 3 || n > 20000 || !count || count > 120000 ||
        count % 3 || vo + n * 8ull > data.size() || io + count * 2ull > data.size()) {
        return false;
    }
    const uint64_t vb = Rd<uint32_t>(asset + 0x140), ib = Rd<uint32_t>(asset + 0x130);
    if (vo + n * 8ull > vb + Rd<uint32_t>(asset + 0x144) || io + count * 2ull > ib + Rd<uint32_t>(asset + 0x134)) return false;
    float scale[3], offset[3];
    std::memcpy(scale, asset + 0x40, 12);
    std::memcpy(offset, asset + 0x4c, 12);
    for (int j = 0; j < 3; ++j) {
        if (!std::isfinite(scale[j]) || scale[j] <= 0 || scale[j] > 50 || !std::isfinite(offset[j])) return false;
    }
    out.vertices.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t a = Rd<uint32_t>(data.data() + vo + i * 8), b = Rd<uint32_t>(data.data() + vo + i * 8 + 4);
        const uint32_t q[3] = {a >> 11, ((a << 10) | (b >> 22)) & 0x1fffff, b & 0x1fffff};
        std::array<float, 3> v{};
        for (int j = 0; j < 3; ++j) v[j] = offset[j] + scale[j] * (float(q[j]) / 2097151.f);
        out.vertices.push_back(v);
    }
    out.indices.resize(count);
    std::memcpy(out.indices.data(), data.data() + io, count * 2ull);
    for (auto i : out.indices) {
        if (i >= n) {
            out = {};
            return false;
        }
    }
    return true;
}

std::wstring GameDir() {
    wchar_t p[32768];
    GetModuleFileNameW(nullptr, p, 32768);
    std::wstring s(p);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? s : s.substr(0, slash);
}

bool ReadFileAt(const std::wstring& path, uint64_t at, uint32_t n, std::vector<uint8_t>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{}, pos{};
    bool ok = GetFileSizeEx(f, &size) && at <= uint64_t(size.QuadPart) && n <= uint64_t(size.QuadPart) - at;
    if (ok) {
        pos.QuadPart = static_cast<LONGLONG>(at);
        out.resize(n);
        DWORD got = 0;
        ok = SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) && ReadFile(f, out.data(), n, &got, nullptr) && got == n;
    }
    CloseHandle(f);
    return ok;
}

// A content blob by its 16-byte key (u32 size first): enshrouded.kfc's directory, then its .dat file.
bool Content(const uint8_t* hash, std::vector<uint8_t>& data) {
    static std::vector<uint8_t> directory;
    static bool attempted = false;
    if (!attempted) {
        attempted = true;
        const std::wstring p = GameDir() + L"\\enshrouded.kfc";
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) {
            const uint64_t n = (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
            if (n >= 0x60 && n < 64ull * 1024 * 1024) ReadFileAt(p, 0, static_cast<uint32_t>(n), directory);
        }
    }
    if (directory.size() < 0x60 || std::memcmp(directory.data(), "KFC4", 4)) return false;
    const auto* d = directory.data();
    const uint64_t keys = 0x48ull + Rd<uint32_t>(d + 0x48), values = 0x50ull + Rd<uint32_t>(d + 0x50);
    const uint32_t count = Rd<uint32_t>(d + 0x4c), n = Rd<uint32_t>(hash);
    if (count != Rd<uint32_t>(d + 0x54) || keys + uint64_t(count) * 16 > directory.size() ||
        values + uint64_t(count) * 16 > directory.size() || n > 16 * 1024 * 1024) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (std::memcmp(d + keys + uint64_t(i) * 16, hash, 16)) continue;
        const uint64_t entry = Rd<uint64_t>(d + values + uint64_t(i) * 16);
        if (Rd<uint64_t>(d + values + uint64_t(i) * 16 + 8)) return false;
        wchar_t filename[64];
        swprintf_s(filename, L"\\enshrouded_%03u.dat", unsigned(entry >> 48));
        return ReadFileAt(GameDir() + filename, entry & 0xffffffffffffull, n, data);
    }
    return false;
}

struct Camera {
    float view[16]{}, projection[16]{}, viewport[4]{};
    bool Project(const float world[3], float& x, float& y, float& depth) const {
        depth = world[0] * view[2] + world[1] * view[6] + world[2] * view[10] + view[14];
        if (!std::isfinite(depth) || depth <= .01f) return false;
        const float inverse = 1.f / depth;
        const float vx = inverse * (world[0] * view[0] + world[1] * view[4] + world[2] * view[8] + view[12]);
        const float vy = inverse * (world[0] * view[1] + world[1] * view[5] + world[2] * view[9] + view[13]);
        x = ((vy * projection[4] + vx * projection[0] + inverse * depth * projection[8] + projection[12]) * .5f + .5f) *
                viewport[2] + viewport[0];
        y = (.5f - (vy * projection[5] + vx * projection[1] + inverse * depth * projection[9] + projection[13]) * .5f) *
                viewport[3] + viewport[1];
        return std::isfinite(x) && std::isfinite(y);
    }
};

struct SurfacePixel {
    float inverseDepth = 0;
    std::array<float, 3> normal{};
    unsigned char shade = 0;
};

// The depth buffer belongs only to the chest: world walls never take part.
void RasterSurface(std::vector<SurfacePixel>& buffer, int width, int height, const std::array<float, 3> p[3],
                   const std::array<float, 3>& normal, unsigned char shade) {
    const float area = (p[1][1] - p[2][1]) * (p[0][0] - p[2][0]) + (p[2][0] - p[1][0]) * (p[0][1] - p[2][1]);
    if (!std::isfinite(area) || std::fabs(area) < 1e-6f) return;
    const int x0 = std::max(0, int(std::floor(std::min({p[0][0], p[1][0], p[2][0]}))));
    const int x1 = std::min(width - 1, int(std::ceil(std::max({p[0][0], p[1][0], p[2][0]}))));
    const int y0 = std::max(0, int(std::floor(std::min({p[0][1], p[1][1], p[2][1]}))));
    const int y1 = std::min(height - 1, int(std::ceil(std::max({p[0][1], p[1][1], p[2][1]}))));
    const float ax = (p[1][1] - p[2][1]) / area, bx = (p[2][1] - p[0][1]) / area;
    const float izStep = ax * (p[0][2] - p[2][2]) + bx * (p[1][2] - p[2][2]);
    for (int y = y0; y <= y1; ++y) {
        const float scan = y + .5f;
        float lo = 1e20f, hi = -1e20f;
        int hits = 0;
        for (int j = 0; j < 3; ++j) {
            const auto& a = p[j];
            const auto& b = p[(j + 1) % 3];
            if ((a[1] <= scan && b[1] > scan) || (b[1] <= scan && a[1] > scan)) {
                const float x = a[0] + (scan - a[1]) * (b[0] - a[0]) / (b[1] - a[1]);
                lo = std::min(lo, x);
                hi = std::max(hi, x);
                ++hits;
            }
        }
        if (hits != 2) continue;
        const int left = std::max(x0, int(std::ceil(lo - .5f))), right = std::min(x1, int(std::floor(hi - .5f)));
        const float a = ((p[1][1] - p[2][1]) * (left + .5f - p[2][0]) + (p[2][0] - p[1][0]) * (scan - p[2][1])) / area;
        const float b = ((p[2][1] - p[0][1]) * (left + .5f - p[2][0]) + (p[0][0] - p[2][0]) * (scan - p[2][1])) / area;
        float iz = a * p[0][2] + b * p[1][2] + (1 - a - b) * p[2][2];
        for (int x = left; x <= right; ++x, iz += izStep) {
            auto& dest = buffer[y * width + x];
            if (iz > dest.inverseDepth) dest = {iz, normal, shade};
        }
    }
}

bool SurfaceCrease(const SurfacePixel& a, const SurfacePixel& b) {
    if (a.inverseDepth <= 0 || b.inverseDepth <= 0) return false;
    float dot = 0;
    for (int j = 0; j < 3; ++j) dot += a.normal[j] * b.normal[j];
    return std::fabs(dot) < .82f;
}

struct RasterRect {
    int x, y, w, h, kind;
};

// Merges identical runs on consecutive rows; every labelled pixel is covered exactly once.
void MergeRuns(const std::vector<uint8_t>& labels, int width, int height, std::vector<RasterRect>& out) {
    out.clear();
    std::vector<size_t> previous, current;
    for (int y = 0; y < height; ++y) {
        current.clear();
        size_t cursor = 0;
        for (int x = 0; x < width;) {
            const int start = x, k = labels[y * width + x++];
            while (x < width && labels[y * width + x] == k) ++x;
            if (!k) continue;
            while (cursor < previous.size() && out[previous[cursor]].x < start) ++cursor;
            size_t id = out.size();
            if (cursor < previous.size()) {
                const auto candidate = previous[cursor];
                auto& r = out[candidate];
                if (r.x == start && r.w == x - start && r.kind == k) {
                    ++r.h;
                    id = candidate;
                }
            }
            if (id == out.size()) out.push_back({start, y, x - start, 1, k});
            current.push_back(id);
        }
        previous.swap(current);
    }
}

struct Strip {
    float x, y, w, h;
};
struct Face {
    std::array<float, 3> normal{};
    unsigned char shade = 0;
    bool valid = false;
};
struct Prepared {
    const Mesh* mesh = nullptr;
    Spatial transform{};
    Camera camera{};
    bool imageValid = false;
    std::vector<std::array<float, 3>> worlds, points;
    std::vector<Face> faces;
    std::array<std::vector<Strip>, 7> strips;
};

constexpr unsigned kMaxShown = 64;
Prepared g_prepared[kMaxShown];
std::map<std::string, Mesh> g_meshes;
struct Scratch {
    std::vector<uint8_t> mask, horizontal, expanded, labels;
    std::vector<SurfacePixel> surface;
    std::vector<RasterRect> rectangles;
};
Scratch g_scratch;
constexpr size_t kChestRects = 3000, kMinChestRects = 800, kFrameRects = 16000;

size_t Count(const Prepared& prepared) {
    size_t n = 0;
    for (int k = 1; k <= 6; ++k) n += prepared.strips[k].size();
    return n;
}

void Submit(void* widget, const Snapshot& s, const Prepared& prepared, size_t* budget, float alpha) {
    const auto& strips = prepared.strips;
    if (budget) {
        const size_t n = Count(prepared);
        if (n > *budget) return;
        *budget -= n;
    }
    const uint32_t colour = s.colour;
    const float pulse = .5f - .5f * std::cos(float(GetTickCount64() % 1800) / 1800.f * 6.283185307f);
    auto draw = [&](const std::vector<Strip>& v, float a) {
        for (const auto& r : v) DrawSolidRectAbs(widget, r.x, r.y, r.w, r.h, colour, alpha * a);
    };
    for (int k = 1; k <= 4; ++k) draw(strips[k], .08f + k * .025f + (.08f + k * .025f) * pulse);
    draw(strips[6], .08f + .10f * pulse);
    draw(strips[5], .45f + .40f * pulse);
}

void DrawOne(void* widget, const Snapshot& s, const Mesh& mesh, const Camera& camera, Prepared& prepared, size_t* budget,
             size_t cap, float alpha) {
    if (mesh.vertices.empty()) return;
    const bool sameGeometry = prepared.mesh == &mesh && !std::memcmp(&prepared.transform, &s.transform, sizeof(s.transform));
    if (sameGeometry && prepared.imageValid && !std::memcmp(&prepared.camera, &camera, sizeof(camera))) {
        Submit(widget, s, prepared, budget, alpha);
        return;
    }
    prepared.imageValid = false;
    auto& points = prepared.points;
    auto& worlds = prepared.worlds;
    auto& faces = prepared.faces;
    points.clear();
    points.reserve(mesh.vertices.size());
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    const auto& t = s.transform;
    double qnorm = 0;
    for (float q : t.rotation) {
        if (!std::isfinite(q)) return;
        qnorm += q * q;
    }
    if (std::fabs(qnorm - 1) > 0.02) return;
    for (float scale : t.scale) {
        if (!std::isfinite(scale) || std::fabs(scale) > 20) return;
    }
    if (!sameGeometry) {
        worlds.clear();
        worlds.reserve(mesh.vertices.size());
        const float qx = t.rotation[0], qy = t.rotation[1], qz = t.rotation[2], qw = t.rotation[3];
        for (auto p : mesh.vertices) {
            for (int j = 0; j < 3; ++j) p[j] *= t.scale[j];
            const float cx = 2 * (qy * p[2] - qz * p[1]), cy = 2 * (qz * p[0] - qx * p[2]), cz = 2 * (qx * p[1] - qy * p[0]);
            worlds.push_back({float(t.position[0] / 4294967296.0) + p[0] + qw * cx + qy * cz - qz * cy,
                              float(t.position[1] / 4294967296.0) + p[1] + qw * cy + qz * cx - qx * cz,
                              float(t.position[2] / 4294967296.0) + p[2] + qw * cz + qx * cy - qy * cx});
        }
        faces.assign(mesh.indices.size() / 3, {});
        for (size_t i = 0; i < mesh.indices.size(); i += 3) {
            const auto p = worlds[mesh.indices[i]], q = worlds[mesh.indices[i + 1]], r = worlds[mesh.indices[i + 2]];
            std::array<float, 3> a{}, b{};
            for (int j = 0; j < 3; ++j) {
                a[j] = q[j] - p[j];
                b[j] = r[j] - p[j];
            }
            auto& f = faces[i / 3];
            f.normal = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
            auto& n = f.normal;
            const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (length < 1e-8f) continue;
            for (float& v : n) v /= length;
            const float light = std::fabs(n[0] * .36f + n[1] * .80f + n[2] * -.48f);
            f.shade = static_cast<unsigned char>(std::min(3, int(light * 4)));
            f.valid = true;
        }
        prepared.mesh = &mesh;
        prepared.transform = s.transform;
    }
    for (const auto& world : worlds) {
        float x, y, z;
        if (!camera.Project(world.data(), x, y, z) || std::fabs(x) > 20000 || std::fabs(y) > 20000) return;
        points.push_back({x, y, 1.f / z});
        minx = std::min(minx, x);
        maxx = std::max(maxx, x);
        miny = std::min(miny, y);
        maxy = std::max(maxy, y);
    }
    if (maxx < camera.viewport[0] - 10 || maxy < camera.viewport[1] - 10 ||
        minx > camera.viewport[0] + camera.viewport[2] + 10 || miny > camera.viewport[1] + camera.viewport[3] + 10) {
        return;
    }
    float step = std::max(1.f, std::max(maxx - minx, maxy - miny) / 250.f);
    if (maxx - minx < 1 || maxy - miny < 1 || step > 20) return;
    const float originx = minx, originy = miny;
    auto& rectangles = g_scratch.rectangles;
    for (int attempt = 0;; ++attempt) {
        minx = originx;
        miny = originy;
        const int w = int(std::ceil((maxx - minx) / step)) + 7, h = int(std::ceil((maxy - miny) / step)) + 7;
        minx -= 3 * step;
        miny -= 3 * step;
        auto& mask = g_scratch.mask;
        mask.assign(size_t(w) * h, 0);
        auto& surface = g_scratch.surface;
        surface.assign(size_t(w) * h, {});
        for (size_t i = 0; i < mesh.indices.size(); i += 3) {
            const auto& face = faces[i / 3];
            if (!face.valid) continue;
            std::array<float, 3> p[3];
            for (int j = 0; j < 3; ++j) {
                const auto v = points[mesh.indices[i + j]];
                p[j] = {(v[0] - minx) / step, (v[1] - miny) / step, v[2]};
            }
            RasterSurface(surface, w, h, p, face.normal, face.shade);
        }
        for (int i = 0; i < w * h; ++i) mask[i] = surface[i].inverseDepth > 0 ? 1 : 0;
        // Two exterior contour bands soften the outline (a separable 5x5 dilation).
        auto& horizontal = g_scratch.horizontal;
        auto& expanded = g_scratch.expanded;
        auto& labels = g_scratch.labels;
        horizontal.assign(size_t(w) * h, 0);
        expanded.assign(size_t(w) * h, 0);
        labels.assign(size_t(w) * h, 0);
        for (int y = 0; y < h; ++y) {
            for (int x = 2; x < w - 2; ++x) {
                horizontal[y * w + x] = mask[y * w + x - 2] | mask[y * w + x - 1] | mask[y * w + x] | mask[y * w + x + 1] |
                                        mask[y * w + x + 2];
            }
        }
        for (int y = 2; y < h - 2; ++y) {
            for (int x = 2; x < w - 2; ++x) {
                expanded[y * w + x] = horizontal[(y - 2) * w + x] | horizontal[(y - 1) * w + x] | horizontal[y * w + x] |
                                      horizontal[(y + 1) * w + x] | horizontal[(y + 2) * w + x];
            }
        }
        for (int y = 1; y < h - 1; ++y) {
            for (int x = 1; x < w - 1; ++x) {
                const int at = y * w + x;
                if (!mask[at]) {
                    labels[at] = expanded[at] ? 6 : 0;
                    continue;
                }
                if (!mask[at - 1] || !mask[at + 1] || !mask[at - w] || !mask[at + w] ||
                    SurfaceCrease(surface[at], surface[at + 1]) || SurfaceCrease(surface[at], surface[at + w])) {
                    labels[at] = 5;
                } else {
                    labels[at] = static_cast<uint8_t>(1 + surface[at].shade);
                }
            }
        }
        MergeRuns(labels, w, h, rectangles);
        if (rectangles.size() <= cap) break;
        if (attempt >= 4) return;
        step *= std::max(1.25f, std::sqrt(float(rectangles.size()) / cap) * 1.1f);
    }
    auto& strips = prepared.strips;
    for (auto& v : strips) v.clear();
    for (const auto& r : rectangles) strips[r.kind].push_back({minx + r.x * step, miny + r.y * step, r.w * step, r.h * step});
    prepared.camera = camera;
    prepared.imageValid = true;
    Submit(widget, s, prepared, budget, alpha);
}

// Draws these chests' silhouettes; false for each that has no decodable mesh (the caller marks those another way).
void Draw(void* widget, const Camera& camera, const Snapshot* snapshots, unsigned count, float alpha, bool* drawn) {
    count = std::min(count, kMaxShown);
    size_t budget = kFrameRects;
    const size_t cap = count ? std::clamp(kFrameRects / count, kMinChestRects, kChestRects) : kChestRects;
    for (unsigned i = 0; i < count; ++i) {
        const auto& s = snapshots[i];
        std::string key(reinterpret_cast<const char*>(s.asset + 0x110), 16);
        key.append(reinterpret_cast<const char*>(s.mesh), 56);
        auto it = g_meshes.find(key);
        if (it == g_meshes.end()) {
            if (g_meshes.size() >= 64) continue;
            Mesh mesh;
            std::vector<uint8_t> content;
            const bool ok = Content(s.asset + 0x110, content) && Decode(content, s.asset, s.mesh, mesh);
            if (g_log) g_log("Chest glow: silhouette mesh %s (%zu vertices, %zu triangles)", ok ? "decoded" : "not decoded",
                             mesh.vertices.size(), mesh.indices.size() / 3);
            it = g_meshes.emplace(key, std::move(mesh)).first;
        }
        if (drawn) drawn[i] = !it->second.vertices.empty();
        DrawOne(widget, s, it->second, camera, g_prepared[i], &budget, cap, alpha);
    }
}

} // namespace chest_silhouette
