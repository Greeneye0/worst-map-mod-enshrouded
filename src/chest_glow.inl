// GG: everything near the player that gets the minimap's height arrow (75 m) and is not done yet - gold, silver and
// wooden chests not looted, lore notes not read, bosses not defeated, each while its row is on, and what the quests
// you're on want here (the game's own pins for them: an item to fetch, a place to reach; Main Quest / Side Quest
// rows; requested by Gadn), and with Nearby Creatures Only the live creatures of the creature rows that are on (in
// red, following them as they move) - lights up for a while: the
// game's outline highlight where it is in view, its silhouette on screen through walls and ground (Find's,
// chest_silhouette.inl; a ring with its icon where it has no model), and pulses on the map and minimap. Silver chests
// glow silver; the rest in Find's gold.
//
// The glow is the game's own outline highlight, the way Find lights the chests it finds and Quick Deposit the chests
// it filled (QuickDeposit/src/glow.cpp, trimmed from FindMod/src/nativehighlight.cpp): the render systems
// "dynamic_render_model" and "static_render_model" walk every model with the ECS iterator 'next'; their calls to it
// are redirected, and when an iteration ends the chests to light get highlight group 5 on their render instance
// through the renderer's own setter (renderer, span of model handles, span of colour bytes), and chests whose time is
// up get their old group back, unless something else (the game's interaction highlight, Quick Deposit's group 6) has
// set another since. Group 7 (Find's; groups 4 and 5 draw no outline): its colour is Find's gold while Find lights
// something, else the game's orange. The outline is hidden by walls like the chest, hence the screen markers
// (DrawMarkers: the client's view / projection matrices at +0x1d20 / +0x1d60 and the UI window's viewport, as Find's
// projection).

bool LocalPlayerXZ(uint8_t* client, float& x, float& z); // map_key_panel.cpp
bool LocalPlayerY(uint8_t* client, float& y);

namespace chest_glow {

constexpr unsigned kMax = 64;
constexpr uint8_t kGroup = 7; // HighlightColorIndex Group4: the one of the free groups the outline draws
constexpr uint32_t kGlowMs = 14000;  // as long as the map's pulses
constexpr uint32_t kFindGold = 0xff6ecdffu, kSilver = 0xffe8e4dcu, kCreatureRed = 0xff3c3cffu; // 0xAABBGGRR
constexpr unsigned kMaxCreatures = 24; // the nearest ones
constexpr float kNearMeters = 75.0f; // the chests' height-arrow range

struct Span {
    const void* data;
    uint64_t count;
};
struct Field {
    const char* text;
    uint64_t length;
};
struct System {
    uint8_t* fn;
    unsigned reads, writes, modelSlot;
};
struct Target {
    uint32_t entity;
    float position[3];
    uint64_t until;
    uint32_t colour;
    uint32_t group; // which chest / note it stands at
    float size;     // its model's size once a render pass has seen it (0: not yet)
    float dist = 0.0f;  // a note's / boss's: how far it stands from the note's spot (m, across); chests: 0
    bool follow = false; // a live creature: lit wherever it goes (no check that it still stands at its spot)
    uint8_t copyGroup = 0; // a live creature: its copy's highlight group (kGroup, or the aggro groups below)
};

// The creature copies' highlight by aggro: HighlightColorIndex Group1 (4) red while fighting, Group2 (5) amber while
// alerted, else kGroup (Find's gold / the game's orange). Groups 4 and 5 are used by no one else (Quick Deposit 6, Find
// and this mod 7); their palette colours (HighlightColorPaletteSettings group1 / group2, at [rbx + 0x58] / [rbx + 0x5c]
// where the frame builds the palette, two places) are swapped for ours at the colour conversion while copies exist.
constexpr uint8_t kFightGroup = 4, kAlertGroup = 5;
constexpr uint32_t kFightColour = 0xff2626ffu, kAlertColour = 0xff1ea8ffu; // 0xAABBGGRR
struct Applied {
    void* renderer;
    void* registry;
    uint8_t* instance;
    uint32_t handle;
    uint8_t before;
    bool haveSnapshot;
    chest_silhouette::Snapshot snapshot; // its model, for the silhouette
    uint32_t proxy = 0;                  // a live creature's copy drawn over walls (0: none)
    uint8_t* proxyInstance = nullptr;
    int64_t sourcePos[3] = {}; // the creature's model where the model systems left it (for the late placement)
    float sourceScale[3] = {};
    float sourceRot[4] = {};
    float radius = 0.0f;
};

// Live creatures through walls: a copy of the creature's render model (the renderer's own instance create / bind /
// transform / delete, as Find's x-ray experiment, FindMod/src/nativehighlight.cpp) moved toward the camera along the
// view ray and scaled down by the same factor: it projects onto the same pixels but stands in front of walls and
// ground. It shares the creature's animation binding (instance + 0x130), so it moves as the creature does, and it gets
// the outline highlight. Copies are deleted when the glow ends and before a hot reload.
struct Spatial {
    int64_t position[3];
    float rotation[4];
    float scale[3];
    uint32_t pad;
};
static_assert(sizeof(Spatial) == 56, "render instance transform");
using CreateFn = uint32_t (*)(void* renderer, const void* modelResource, const Spatial* transform);
using TransformFn = void (*)(void* renderer, const void* handles, const void* transforms, bool teleport);
using DeleteFn = void (*)(void* renderer, const void* handles);
using BindFn = void (*)(void* renderer, const void* handles, uint32_t binding);
CreateFn g_create = nullptr;
TransformFn g_transform = nullptr;
DeleteFn g_delete = nullptr;
BindFn g_bind = nullptr;
using UploadFn = void (*)(void* renderer);
UploadFn g_upload = nullptr;
// Late in the frame (the highlight palette's colour conversion, after the frame's camera is set): the copies placed
// again with that camera and the instance data sent again. Placed only when the model systems run, they trailed one
// frame behind a turning camera (2026-11-11). Find's x-ray experiment did the same.
using ColorFn = float* (*)(float* out, uint32_t colour);
ColorFn g_paletteNext = nullptr;
volatile LONG g_dropProxies = 0; // a hot reload is coming: the render hook deletes every copy
volatile LONG g_proxyCount = 0;
double g_proxySmooth = 0.6; // the step average's weight on the earlier steps (dev bridge: map action proxysmooth)
volatile LONG g_proxyPredict = 0;
// Where the copies are moved each frame (a Codex session's disassembly, 2026-11-11): the frame function stages every
// render instance's 128-byte record (UploadFn, RVA 0x115ed0 then) from one call; the palette conversion comes after it,
// too late for the frame. With g_proxyFinal the copies are placed right before that call, with the renderer's camera
// for this frame and the creature where the model systems left it, and moved nowhere else. Dev bridge: proxyfinal.
volatile LONG g_proxyFinal = 1; // dev bridge: map action proxypredict 0/1
volatile LONG g_proxyLate = 0; // dev bridge: map action proxylate 0/1
float g_proxyNear = 1.5f; // metres from the eye at the least (dev bridge: map action proxynear)
// The player's feet (game thread, each frame): the copies stand 1 m on the eye's side of the player, not right at the
// eye. The walls that hide creatures are past the player; the further from the eye the copy, the less a camera error
// moves it on screen (2026-11-11: at 1.5 m the copies still jumped when the camera stopped turning).
float g_playerPos[3] = {};
volatile LONG g_havePlayer = 0;
// Which camera places the copies: 1 the client's view matrix (client + 0x1d20, this frame's camera, as the screen
// markers use), 0 the renderer's camera (renderer + 0x40 -> +0x8d0: last frame's while the model systems run, so the
// copies trailed behind a turning camera). Dev bridge: map action proxyeye.
int g_proxyEye = 1;
std::atomic<uint8_t*> g_proxyClient{nullptr};
std::atomic<void*> g_eyeRenderer{nullptr}; // dev: the renderer for DevEyeLog
std::atomic<uint64_t> g_eyeLogUntil{0};
double g_eyeFirst[3] = {};
uint64_t g_frameNo = 0; // dev: counted at each palette conversion

bool EyeFromView(const float* v, double eye[3], double forward[3]) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(v[i])) return false;
    // x_view = sum_j v[j * 4 + i] * x_j + v[12 + i]: the eye is -R^T t, forward the view's depth row
    for (int j = 0; j < 3; ++j) {
        eye[j] = -(double(v[j * 4 + 0]) * v[12] + double(v[j * 4 + 1]) * v[13] + double(v[j * 4 + 2]) * v[14]);
        forward[j] = v[j * 4 + 2];
    }
    const double len = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    if (!(len > 0.5 && len < 2.0)) return false;
    for (int j = 0; j < 3; ++j) forward[j] /= len;
    return PlausibleWorldPos(std::array<float, 3>{float(eye[0]), float(eye[1]), float(eye[2])}.data());
}

bool EyeFromCamera(const Spatial& cam, double eye[3], double forward[3]) {
    double norm = 0;
    for (float q : cam.rotation) {
        if (!std::isfinite(q)) return false;
        norm += double(q) * q;
    }
    if (std::fabs(norm - 1.0) > 0.01) return false;
    const float x = cam.rotation[0], y = cam.rotation[1], z = cam.rotation[2], w = cam.rotation[3];
    forward[0] = 2.0 * (x * z + y * w), forward[1] = 2.0 * (y * z - x * w), forward[2] = 1.0 - 2.0 * (x * x + y * y);
    for (int j = 0; j < 3; ++j) eye[j] = cam.position[j] / 4294967296.0;
    return true;
}
bool ProxiesReady() { return g_create && g_transform && g_delete && g_bind; }

// The copy's transform: every point moved toward the eye by one factor keeps its projection.
bool PerspectiveProxy(const double eye[3], const double forward[3], Spatial& out, float modelRadius) {
    double depth = 0;
    float maxScale = 0;
    for (int j = 0; j < 3; ++j) {
        const double delta = out.position[j] / 4294967296.0 - eye[j];
        if (!std::isfinite(delta) || std::fabs(delta) > 5000 || !std::isfinite(out.scale[j])) return false;
        depth += delta * forward[j];
        maxScale = std::max(maxScale, std::fabs(out.scale[j]));
    }
    const double radius = modelRadius * maxScale;
    if (!std::isfinite(radius) || radius < 0.05 || radius > 20 || depth <= radius + 0.2) return false;
    // The copy's nearest point g_proxyNear metres from the eye. Close to the eye it clears every wall but magnifies the
    // camera's movement within the frame (Find used 0.15 m: the copy bounced whenever the player moved).
    double nearest = g_proxyNear;
    if (g_havePlayer) {
        double d2 = 0;
        for (int j = 0; j < 3; ++j) d2 += (g_playerPos[j] + (j == 1 ? 1.0 : 0.0) - eye[j]) * (g_playerPos[j] + (j == 1 ? 1.0 : 0.0) - eye[j]);
        nearest = std::max(nearest, std::min(std::sqrt(d2) - 1.0, 12.0));
    }
    const double factor = std::min(1.0, nearest / (depth - radius));
    if (!(factor > 0 && factor <= 1.0)) return false;
    for (int j = 0; j < 3; ++j) {
        const double origin = eye[j], point = out.position[j] / 4294967296.0;
        out.position[j] = static_cast<int64_t>((origin + (point - origin) * factor) * 4294967296.0);
        out.scale[j] = static_cast<float>(out.scale[j] * factor);
    }
    return true;
}

using NextFn = bool (*)(void* iter, void* view, uint32_t size);
using SetFn = void (*)(void* renderer, const Span* handles, const Span* colours);
using InstanceFn = uint8_t* (*)(void* renderer, uint32_t handle, bool valid);
using EntityComponentFn = void** (*)(void* out16, void* registry, void* typeIndex, uint32_t entityId);

NextFn g_next = nullptr;
SetFn g_set = nullptr;
InstanceFn g_instance = nullptr;
EntityComponentFn g_entityComponent = nullptr;
System g_systems[2] = {};

SRWLOCK g_lock = SRWLOCK_INIT;
Target g_targets[kMax];
unsigned g_count = 0;
Applied g_applied[kMax];
unsigned g_appliedCount = 0;
volatile LONG g_pulsing = 0;

template <class T>
T Read(const void* p, size_t off = 0) {
    T value;
    std::memcpy(&value, static_cast<const uint8_t*>(p) + off, sizeof(value));
    return value;
}

bool InImage(const void* p, size_t length) {
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto at = reinterpret_cast<uintptr_t>(p);
    return at >= base && length <= nt->OptionalHeader.SizeOfImage && at - base <= nt->OptionalHeader.SizeOfImage - length;
}

bool Named(const Field& f, const char* text) {
    const size_t n = std::strlen(text);
    return f.length == n && InImage(f.text, n) && std::memcmp(f.text, text, n) == 0;
}

// The system's definition in .rdata: {name, length}, its function at +0x10, then four spans of fields at +0x38 (two
// read, two written); ModelComponent is among the written ones.
bool DiscoverSystem(const char* name, System& out) {
    out = {};
    out.fn = SystemFunction(name);
    if (!out.fn) return false;
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (std::memcmp(sec->Name, ".rdata", 6) != 0) continue;
        for (size_t off = 0; off + 0x80 <= sec->Misc.VirtualSize; off += 8) {
            uint8_t* p = base + sec->VirtualAddress + off;
            if (!Named(Read<Field>(p), name) || Read<uint8_t*>(p, 0x10) != out.fn) continue;
            unsigned slot = 0;
            bool model = false;
            for (unsigned list = 0; list < 4; ++list) {
                auto span = Read<Span>(p, 0x38 + 0x10 * list);
                if (span.count > 32 || (span.count && !InImage(span.data, span.count * sizeof(Field)))) return false;
                for (unsigned i = 0; i < span.count; ++i, ++slot) {
                    if (Named(Read<Field>(span.data, i * sizeof(Field)), "ModelComponent")) {
                        if (model || list != 2) return false;
                        model = true;
                        out.modelSlot = slot;
                    }
                }
                (list < 2 ? out.reads : out.writes) += static_cast<unsigned>(span.count);
            }
            return model;
        }
    }
    return false;
}

// A component of an entity by id (the game's GetEntityComponent), or nullptr.
const void* Data(void* registry, uint16_t type, uint32_t entity) {
    if (!g_entityComponent || !registry) return nullptr;
    void* out[2] = {};
    return *g_entityComponent(out, registry, &type, entity);
}

void Set(void* renderer, uint32_t handle, uint8_t group) {
    Span handles{&handle, 1}, groups{&group, 1};
    g_set(renderer, &handles, &groups);
}

void RemoveProxy(Applied& a) {
    if (!a.proxy) return;
    if (g_instance(a.renderer, a.proxy, true) == a.proxyInstance) {
        Span handles{&a.proxy, 1};
        g_delete(a.renderer, &handles);
    }
    a.proxy = 0;
    a.proxyInstance = nullptr;
    InterlockedDecrement(&g_proxyCount);
}

// When the model systems run, both cameras are still the last frame's: the frame's own camera is set after them (dev
// eyelog, 2026-11-11: the early eye of frame n equals the late eye of frame n - 1). So the copies are placed with the
// camera one step ahead, its last step repeated (a turning camera orbits the player smoothly); a jump of over 3 m (a
// teleport, a cut) is not repeated. g_frameCount: the palette conversions, once a frame.
volatile LONG g_frameCount = 0;
void PredictEye(double eye[3]) {
    static LONG frame = -1;
    static double last[3] = {}, step[3] = {}, smooth[3] = {};
    static bool haveLast = false;
    const LONG now = g_frameCount;
    if (now != frame) {
        frame = now;
        double d2 = 0;
        for (int j = 0; j < 3; ++j) {
            step[j] = haveLast ? eye[j] - last[j] : 0.0;
            d2 += step[j] * step[j];
            last[j] = eye[j];
        }
        if (d2 > 9.0) step[0] = step[1] = step[2] = 0.0;
        // a running average of the steps: the raw last step follows every uneven frame and mouse report, and the
        // copies jittered with it (2026-11-11)
        for (int j = 0; j < 3; ++j) smooth[j] = g_proxySmooth * smooth[j] + (1.0 - g_proxySmooth) * step[j];
        haveLast = true;
    }
    if (!g_proxyPredict) return;
    for (int j = 0; j < 3; ++j) eye[j] = last[j] + smooth[j];
}

// The creature's copy where its model is now (created on the first call).
void UpdateProxy(Applied& a, uint8_t* source, uint32_t colourGroup) {
    auto* resource = Read<uint8_t*>(source, 0x108);
    auto* asset = resource ? Read<uint8_t*>(resource, 0x38) : nullptr;
    auto* camera = Read<uint8_t*>(a.renderer, 0x40);
    if (!asset || !camera) return RemoveProxy(a);
    Spatial transform = Read<Spatial>(source, 0x38);
    std::memcpy(a.sourcePos, transform.position, sizeof(a.sourcePos));
    std::memcpy(a.sourceScale, transform.scale, sizeof(a.sourceScale));
    std::memcpy(a.sourceRot, transform.rotation, sizeof(a.sourceRot));
    a.radius = Read<float>(asset, 0x58);
    double eye[3], forward[3];
    uint8_t* client = g_proxyClient.load(std::memory_order_relaxed);
    float view[16];
    if (client) std::memcpy(view, client + 0x1d20, sizeof(view));
    const bool haveEye = (g_proxyEye == 1 && client && EyeFromView(view, eye, forward)) ||
                         EyeFromCamera(Read<Spatial>(camera, 0x8d0), eye, forward);
    if (haveEye) PredictEye(eye);
    if (!haveEye || !PerspectiveProxy(eye, forward, transform, Read<float>(asset, 0x58))) return RemoveProxy(a);
    if (a.proxy && g_instance(a.renderer, a.proxy, true) != a.proxyInstance) {
        a.proxy = 0; // gone with its renderer (a world left)
        a.proxyInstance = nullptr;
        InterlockedDecrement(&g_proxyCount);
    }
    const bool created = !a.proxy;
    if (!a.proxy) {
        a.proxy = g_create(a.renderer, resource + 0x18, &transform);
        if (!a.proxy) return;
        a.proxyInstance = g_instance(a.renderer, a.proxy, true);
        InterlockedIncrement(&g_proxyCount);
        static int logged = 0;
        if (g_log && logged++ < 8)
            g_log("Chest glow: creature copy %08x of %08x (resource kind %u, +0x70 %u, source +0xa8 %llx, binding %u)", a.proxy,
                  a.handle, Read<uint8_t>(resource, 0x14), Read<uint32_t>(resource, 0x70),
                  static_cast<unsigned long long>(Read<uint64_t>(source, 0xa8)), Read<uint32_t>(source, 0x130));
    }
    Span handles{&a.proxy, 1}, transforms{&transform, 1};
    if (const uint32_t binding = Read<uint32_t>(source, 0x130)) g_bind(a.renderer, &handles, binding);
    if (created || !g_proxyFinal) g_transform(a.renderer, &handles, &transforms, created);
    Set(a.renderer, a.proxy, static_cast<uint8_t>(colourGroup));
}

void Restore(unsigned i) {
    RemoveProxy(g_applied[i]);
    const Applied& a = g_applied[i];
    uint8_t* current = g_instance(a.renderer, a.handle, true);
    // A group another mod set (Quick Deposit's 6, Find's 7) is not put back: that mod restores its own, and once both
    // had lit the chest it stayed lit with neither.
    if (current == a.instance && current && current[0x78] == kGroup) Set(a.renderer, a.handle, a.before == 6 || a.before == 7 ? 0 : a.before);
    g_applied[i] = g_applied[--g_appliedCount];
}

bool SamePlace(void* registry, uint32_t entity, const float* position) {
    const auto* pos = static_cast<const int64_t*>(Data(registry, g_waterTypes.position, entity));
    if (!pos) return false;
    for (int j = 0; j < 3; ++j) {
        if (std::fabs(static_cast<float>(pos[j] / 4294967296.0) - position[j]) >= 0.15f) return false;
    }
    return true;
}

// A model's largest extent in metres (its mesh's quantization box, asset + 0x40, times the instance's scale); a large
// number where it can't be read.
float ModelSize(const uint8_t* instance) {
    auto* resource = Read<uint8_t*>(instance, 0x108);
    auto* asset = resource ? Read<uint8_t*>(resource, 0x38) : nullptr;
    if (!asset) return 1e9f;
    float box[3], scale[3];
    std::memcpy(box, asset + 0x40, sizeof(box));
    std::memcpy(scale, instance + 0x38 + offsetof(chest_silhouette::Spatial, scale), sizeof(scale));
    float size = 0.0f;
    for (int j = 0; j < 3; ++j) {
        if (!std::isfinite(box[j]) || !std::isfinite(scale[j])) return 1e9f;
        size = std::max(size, std::fabs(box[j] * scale[j]));
    }
    return size > 0.0f ? size : 1e9f;
}

// At the end of a render system's iteration, on its own thread.
void Apply(void* iter, void* view, const System& system) {
    auto* state = Read<uint8_t*>(iter);
    if (!state || Read<uint16_t>(state, 0x4b8) != system.reads || Read<uint16_t>(state, 0x4ba) != system.writes) return;

    void* registry = Read<void*>(state, 0x30);
    uint8_t* record = state + 0x1c8 + system.modelSlot * 0x10;
    const uint16_t type = Read<uint16_t>(record, 0xa);
    if (type >= 1024 || Read<uint16_t>(record, 8) != 4) return;
    // Global resources come last in the view; these two layouts are 0xb8 and 0x58 bytes.
    const size_t viewSize = system.reads == 6 && system.writes == 2 ? 0xb8 : system.reads == 2 && system.writes == 1 ? 0x58 : 0;
    if (!viewSize) return;
    void* scene = Read<void*>(view, viewSize - 8);
    void* renderer = scene ? Read<void*>(scene, 0x98) : nullptr;
    if (!renderer) return;
#ifdef UMM_DEV_BRIDGE
    g_eyeRenderer.store(renderer, std::memory_order_relaxed);
#endif

    AcquireSRWLockExclusive(&g_lock);
    __try {
        const uint64_t now = GetTickCount64();
        uint32_t handles[kMax], colours[kMax];
        bool follows[kMax];
        uint8_t copyGroups[kMax];
        unsigned count = 0;
        bool active = false;
        for (unsigned i = 0; i < g_count; ++i) active |= g_targets[i].until > now;
        // Render jobs may run on views without a registry: the one whose entities sit where the chests are is the
        // render registry.
        for (int r = 0; active && !registry && r < kMaxWaterRegistries; ++r) {
            void* cand = g_waterRegistries[r].load(std::memory_order_relaxed);
            for (unsigned i = 0; cand && i < g_count && !registry; ++i) {
                const auto* model = static_cast<const uint32_t*>(Data(cand, type, g_targets[i].entity));
                if (model && *model && g_instance(renderer, *model, true) &&
                    (g_targets[i].follow || SamePlace(cand, g_targets[i].entity, g_targets[i].position))) {
                    registry = cand;
                }
            }
        }
        // One model per chest / note: the smallest of the models standing there (a chest is smaller than the scaffold
        // plank under it, a note than its table; lit too, they drew a big flat silhouette around the chest)
        uint32_t groups[kMax];
        float sizes[kMax], dists[kMax];
        for (unsigned i = 0; active && registry && i < g_count; ++i) {
            const Target& t = g_targets[i];
            if (t.until <= now) continue;
            const auto* model = static_cast<const uint32_t*>(Data(registry, type, t.entity));
            uint8_t* instance = model && *model && (t.follow || SamePlace(registry, t.entity, t.position))
                                    ? g_instance(renderer, *model, true)
                                    : nullptr;
            if (!instance) continue;
            if (std::find(handles, handles + count, *model) != handles + count) continue;
            const float size = ModelSize(instance);
            g_targets[i].size = size;
            // a better model at the same chest / note, seen in this pass or the other render system's: at a chest the
            // smaller; at a note the one closer to its spot (by more than 0.25 m), then the smaller (a note on its table)
            auto better = [&](const Target& o, float oSize, const Target& me, float mySize, bool earlier) {
                if (std::fabs(o.dist - me.dist) > 0.25f) return o.dist < me.dist;
                return oSize < mySize || (oSize == mySize && earlier);
            };
            bool smaller = false;
            for (unsigned k = 0; k < g_count && !smaller; ++k) {
                smaller = k != i && g_targets[k].group == t.group && g_targets[k].size > 0.0f &&
                          better(g_targets[k], g_targets[k].size, t, size, k < i);
            }
            if (smaller) continue;
            unsigned j = 0;
            while (j < count && groups[j] != t.group) ++j;
            if (j < count && (std::fabs(dists[j] - t.dist) > 0.25f ? dists[j] < t.dist : sizes[j] <= size)) continue;
            colours[j] = t.colour;
            follows[j] = t.follow;
            copyGroups[j] = t.copyGroup ? t.copyGroup : kGroup;
            handles[j] = *model;
            groups[j] = t.group;
            sizes[j] = size;
            dists[j] = t.dist;
            if (j == count) ++count;
        }
        for (unsigned i = 0; i < g_appliedCount;) {
            const Applied& a = g_applied[i];
            if (a.renderer == renderer && (!registry || a.registry == registry) &&
                std::find(handles, handles + count, a.handle) == handles + count) {
                Restore(i);
            } else {
                ++i;
            }
        }
        for (unsigned i = 0; i < count; ++i) {
            uint8_t* instance = g_instance(renderer, handles[i], true);
            if (!instance) continue;
            unsigned j = 0;
            while (j < g_appliedCount && !(g_applied[j].renderer == renderer && g_applied[j].handle == handles[i])) ++j;
            if (j == g_appliedCount) {
                if (j == kMax) continue;
                g_applied[g_appliedCount] = {renderer, registry, instance, handles[i], instance[0x78], false, {}};
                ++g_appliedCount;
            }
            if (g_applied[j].instance != instance) continue;
            // The game's interaction highlight, Quick Deposit's and Find's go first.
            if (instance[0x78] != 0 && instance[0x78] != kGroup) continue;
            // Silver chests get no outline highlight (group 7 is orange-gold): only their silver silhouette
            if (colours[i] != kSilver) Set(renderer, handles[i], kGroup);
            Applied& a = g_applied[j];
            a.haveSnapshot = false;
            // A live creature: its copy over walls instead of a silhouette
            if (follows[i] && ProxiesReady() && !g_dropProxies) {
                UpdateProxy(a, instance, copyGroups[i]);
                if (a.proxy) continue;
            }
            // Its model for the silhouette: resource (+0x108) -> asset (+0x38), mesh table, transform (+0x38)
            auto* resource = Read<uint8_t*>(instance, 0x108);
            auto* asset = resource ? Read<uint8_t*>(resource, 0x38) : nullptr;
            if (asset && Read<uint32_t>(asset, 0x70) > 0 && Read<uint32_t>(asset, 0x70) < 256) {
                auto* mesh = asset + 0x6c + Read<int32_t>(asset, 0x6c);
                std::memcpy(a.snapshot.asset, asset, sizeof(a.snapshot.asset));
                std::memcpy(a.snapshot.mesh, mesh, sizeof(a.snapshot.mesh));
                std::memcpy(&a.snapshot.transform, instance + 0x38, sizeof(a.snapshot.transform));
                a.snapshot.colour = colours[i];
                a.haveSnapshot = true;
            }

        }
        if (!active) g_count = 0;
        InterlockedExchange(&g_pulsing, g_appliedCount ? 1 : 0);
    } __finally {
        ReleaseSRWLockExclusive(&g_lock);
    }
}

// Before a hot reload (render thread): every copy deleted, the creatures' glow ended.
void DropProxies() {
    AcquireSRWLockExclusive(&g_lock);
    __try {
        for (unsigned i = 0; i < g_appliedCount; ++i) RemoveProxy(g_applied[i]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void SafeApply(void* iter, void* view, const System& system) {
    __try {
        Apply(iter, view, system);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_pulsing, 0);
        static volatile LONG logged = 0;
        if (!InterlockedExchange(&logged, 1) && g_log) g_log("Chest glow: render layout check failed; chests are not lit");
    }
}

// The live creatures (creature_live.inl): what model each queued entity has, once it has a render instance. On the
// render thread, at the end of a render system's iteration (the renderer and the model component's type are known
// only there).
uint64_t AssetHash(uint8_t* renderer, uint32_t handle) {
    __try {
        uint8_t* instance = g_instance(renderer, handle, true);
        auto* resource = instance ? Read<uint8_t*>(instance, 0x108) : nullptr;
        auto* asset = resource ? Read<uint8_t*>(resource, 0x38) : nullptr;
        return asset ? creature_live::Fnv64(asset, 0x80) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The entity's model handle: 0 none yet, -1 no model component (or a fault)
int64_t ModelHandle(void* registry, uint16_t type, uint32_t entity) {
    __try {
        const auto* model = static_cast<const uint32_t*>(Data(registry, type, entity));
        return model ? *model : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// (g_lock is held from here on: every read of game memory under it goes through a guarded helper)
void ResolveCreatures(void* iter, void* view, const System& system) {
    if (!creature_live::g_havePending.load(std::memory_order_acquire)) return;
    auto* state = Read<uint8_t*>(iter);
    if (!state || Read<uint16_t>(state, 0x4b8) != system.reads || Read<uint16_t>(state, 0x4ba) != system.writes) return;
    void* registry = Read<void*>(state, 0x30);
    uint8_t* record = state + 0x1c8 + system.modelSlot * 0x10;
    const uint16_t type = Read<uint16_t>(record, 0xa);
    if (type >= 1024 || Read<uint16_t>(record, 8) != 4) return;
    const size_t viewSize = system.reads == 6 && system.writes == 2 ? 0xb8 : system.reads == 2 && system.writes == 1 ? 0x58 : 0;
    if (!viewSize) return;
    void* scene = Read<void*>(view, viewSize - 8);
    auto* renderer = scene ? Read<uint8_t*>(scene, 0x98) : nullptr;
    if (!renderer) return;
    AcquireSRWLockExclusive(&creature_live::g_lock);
    size_t kept = 0;
    for (const creature_live::Pending& pnd : creature_live::g_pending) {
        if (registry && pnd.registry != registry) {
            creature_live::g_pending[kept++] = pnd;
            continue;
        }
        int ri = 0;
        while (ri < kMaxWaterRegistries && g_waterRegistries[ri].load(std::memory_order_relaxed) != pnd.registry) ++ri;
        auto it = creature_live::g_cache.find(creature_live::Key(ri, pnd.entity));
        if (it == creature_live::g_cache.end()) continue;
        const int64_t handle = ModelHandle(pnd.registry, type, pnd.entity);
        if (handle < 0) {
            it->second.model = creature_live::kNotCreature; // no model at all
            continue;
        }
        const uint64_t hash = handle ? AssetHash(renderer, static_cast<uint32_t>(handle)) : 0;
        if (!hash) {
            creature_live::g_pending[kept++] = pnd; // not drawn yet: the other render system, or a later frame
            continue;
        }
        it->second.model = creature_live::FindModel(hash);
    }
    creature_live::g_pending.resize(kept);
    creature_live::g_havePending.store(kept != 0, std::memory_order_release);
    ReleaseSRWLockExclusive(&creature_live::g_lock);
}

void SafeResolveCreatures(void* iter, void* view, const System& system) {
    __try {
        ResolveCreatures(iter, view, system);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        creature_live::g_havePending.store(false, std::memory_order_release);
    }
}

// Every copy placed again with the renderer's camera as it is now (g_lock held by the caller).
void LateCorrectInner() {
    void* uploaded[4] = {};
    unsigned uploads = 0;
    for (unsigned i = 0; i < g_appliedCount; ++i) {
        Applied& a = g_applied[i];
        if (!a.proxy) continue;
        if (g_instance(a.renderer, a.proxy, true) != a.proxyInstance || a.radius <= 0.0f) continue;
        auto* camera = Read<uint8_t*>(a.renderer, 0x40);
        if (!camera) continue;
        // the creature where the model systems left it (read back here it was another transform: the copies stood
        // off their creatures, 2026-11-11), the eye this frame is drawn with
        double eye[3], forward[3];
        Spatial transform{};
        std::memcpy(transform.position, a.sourcePos, sizeof(a.sourcePos));
        std::memcpy(transform.rotation, a.sourceRot, sizeof(a.sourceRot));
        std::memcpy(transform.scale, a.sourceScale, sizeof(a.sourceScale));
        if (!EyeFromCamera(Read<Spatial>(camera, 0x8d0), eye, forward) || !PerspectiveProxy(eye, forward, transform, a.radius))
            continue;
        Span handles{&a.proxy, 1}, transforms{&transform, 1};
        g_transform(a.renderer, &handles, &transforms, true);
        unsigned k = 0;
        while (k < uploads && uploaded[k] != a.renderer) ++k;
        if (k == uploads && uploads < 4) uploaded[uploads++] = a.renderer;
    }
    for (unsigned k = 0; k < uploads && g_upload; ++k) g_upload(uploaded[k]);
}

void LateCorrect() {
    if (!g_proxyCount || g_dropProxies || !g_proxyLate) return;
    AcquireSRWLockExclusive(&g_lock);
    __try {
        LateCorrectInner();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_proxyLate = 0;
        if (g_log) g_log("Chest glow: late copy placement faulted; off");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

#ifdef UMM_DEV_BRIDGE
// dev (bridge map action eyelog, value seconds): the eye of each camera source, at the model systems (early) and at the
// palette conversion (late), logged ten times a second, to see which one the frame is drawn with
void DevEyeLog(const char* when) {
    if (GetTickCount64() > g_eyeLogUntil.load(std::memory_order_relaxed)) return;
    static int lines = 0;
    static uint64_t window = 0;
    if (window != g_eyeLogUntil.load(std::memory_order_relaxed)) window = g_eyeLogUntil.load(std::memory_order_relaxed), lines = 0, g_eyeFirst[0] = 0.0;
    const uint64_t now = GetTickCount64();
    if (lines >= 600) return; // every frame from the first camera movement, up to 600 lines
    __try {
        double ce[3] = {}, cf[3] = {}, re[3] = {}, rf[3] = {};
        bool haveC = false, haveR = false;
        if (uint8_t* client = g_proxyClient.load(std::memory_order_relaxed)) {
            float view[16];
            std::memcpy(view, client + 0x1d20, sizeof(view));
            haveC = EyeFromView(view, ce, cf);
        }
        if (auto* renderer = static_cast<uint8_t*>(g_eyeRenderer.load(std::memory_order_relaxed)))
            if (auto* camera = Read<uint8_t*>(renderer, 0x40)) haveR = EyeFromCamera(Read<Spatial>(camera, 0x8d0), re, rf);
        if (lines == 0 && g_eyeFirst[0] == 0.0) std::memcpy(g_eyeFirst, re, sizeof(g_eyeFirst));
        if (lines == 0 && std::fabs(re[0] - g_eyeFirst[0]) + std::fabs(re[1] - g_eyeFirst[1]) + std::fabs(re[2] - g_eyeFirst[2]) < 0.01) return;
        ++lines;
        if (g_log)
            g_log("Eye %s f%llu t%llu: client %d %.3f %.3f %.3f fwd %.3f %.3f %.3f | renderer %d %.3f %.3f %.3f fwd %.3f %.3f %.3f", when,
                  static_cast<unsigned long long>(g_frameNo), static_cast<unsigned long long>(now % 100000), haveC, ce[0], ce[1], ce[2], cf[0], cf[1], cf[2], haveR, re[0],
                  re[1], re[2], rf[0], rf[1], rf[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
#endif

float* PaletteHook(float* out, uint32_t colour) {
    {
        greeneye::Lease lease; // an older generation after a hot reload only calls on
        if (lease) InterlockedIncrement(&g_frameCount);
#ifdef UMM_DEV_BRIDGE
        if (lease) DevEyeLog("late"), ++g_frameNo;
#endif
        if (lease && !g_modOff) LateCorrect();
    }
    return g_paletteNext(out, colour);
}

// Right before the frame stages the render instances: every copy placed with this frame's camera (g_lock held).
void PlaceProxiesFinalInner(void* renderer) {
    // At this point the client's view (client + 0x1d20) is already this frame's camera; the renderer's (+0x40 ->
    // +0x8d0) is still the last frame's (dev eyelog 2026-11-11: stage eye = late eye of the same frame only for the
    // client's view). The renderer's only while the client's can't be read.
    double eye[3], forward[3];
    uint8_t* client = g_proxyClient.load(std::memory_order_relaxed);
    float view[16];
    if (client) std::memcpy(view, client + 0x1d20, sizeof(view));
    if (!(client && EyeFromView(view, eye, forward))) {
        auto* camera = Read<uint8_t*>(renderer, 0x40);
        if (!camera || !EyeFromCamera(Read<Spatial>(camera, 0x8d0), eye, forward)) return;
    }
    for (unsigned i = 0; i < g_appliedCount; ++i) {
        Applied& a = g_applied[i];
        if (!a.proxy || a.renderer != renderer || a.radius <= 0.0f) continue;
        if (g_instance(a.renderer, a.proxy, true) != a.proxyInstance) continue;
        Spatial transform{};
        std::memcpy(transform.position, a.sourcePos, sizeof(a.sourcePos));
        std::memcpy(transform.rotation, a.sourceRot, sizeof(a.sourceRot));
        std::memcpy(transform.scale, a.sourceScale, sizeof(a.sourceScale));
        if (!PerspectiveProxy(eye, forward, transform, a.radius)) continue;
        Span handles{&a.proxy, 1}, transforms{&transform, 1};
        g_transform(a.renderer, &handles, &transforms, false);
    }
}

void PlaceProxiesFinal(void* renderer) {
    if (!g_proxyCount || g_dropProxies || !g_proxyFinal || !renderer) return;
    AcquireSRWLockExclusive(&g_lock);
    __try {
        PlaceProxiesFinalInner(renderer);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_proxyFinal = 0;
        if (g_log) g_log("Chest glow: placing the creature copies before staging faulted; off");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

UploadFn g_stageNext = nullptr;
void StageHook(void* renderer) {
    {
        greeneye::Lease lease; // an older generation after a hot reload only calls on
#ifdef UMM_DEV_BRIDGE
        if (lease) DevEyeLog("stage");
#endif
        if (lease && !g_modOff) PlaceProxiesFinal(renderer);
    }
    g_stageNext(renderer);
}

using ColorFn2 = float* (*)(float* out, uint32_t colour);
ColorFn2 g_fightNext[2] = {}, g_alertNext[2] = {};
template <int Site>
float* FightColourHook(float* out, uint32_t colour) {
    {
        greeneye::Lease lease;
        if (lease && g_proxyCount && !g_modOff) colour = kFightColour;
    }
    return g_fightNext[Site](out, colour);
}
template <int Site>
float* AlertColourHook(float* out, uint32_t colour) {
    {
        greeneye::Lease lease;
        if (lease && g_proxyCount && !g_modOff) colour = kAlertColour;
    }
    return g_alertNext[Site](out, colour);
}

// The lit creatures' copy groups from their aggro now (game thread, every frame: creature_live's list is read four
// times a second while the minimap shows them).
void RefreshCreatureAggro() {
    if (!g_proxyCount) return;
    AcquireSRWLockExclusive(&g_lock);
    for (unsigned i = 0; i < g_count; ++i) {
        Target& t = g_targets[i];
        if (!t.follow) continue;
        for (const creature_live::LiveCreature& c : creature_live::g_near) {
            if (c.entity != t.entity) continue;
            const int aggro = creature_live::Aggro(c);
            t.copyGroup = aggro == 2 ? kFightGroup : aggro == 1 ? kAlertGroup : kGroup;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// Which render system is iterating is told by the iterator (Apply checks its read / write counts, 6/2 and 2/1), not
// by the return address: Quick Deposit redirects the same calls, and when the loader chains the two detours the second
// one's return address is the first detour.
bool NextHook(void* iter, void* view, uint32_t size) {
    UMM_PERF(PerfGlowNext);
    greeneye::Lease lease; // an older generation after a hot reload only calls on
    const bool next = g_next(iter, view, size);
    if (!lease || next || g_modOff) return next;
    if (g_dropProxies && g_proxyCount) DropProxies();
#ifdef UMM_DEV_BRIDGE
    DevEyeLog("early");
#endif
    for (const System& system : g_systems) SafeApply(iter, view, system);
    for (const System& system : g_systems) SafeResolveCreatures(iter, view, system);
    return next;
}

void Install() {
    uint8_t* next = FindUnique("40 57 48 83 EC 20 4C 8B 01 48 8B FA 8B 41 08 4C 8B C9 49 3B 80 ?? 08 00 00");
    uint8_t* setter = FindUnique("48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 33 DB 4D 8B F0 48 8B FA "
                                 "48 8B F1 48 39 5A 08 76 46 66 66 0F 1F 84 00 00 00 00 00 48 8B 07 41 B0 01 48 8B CE 8B "
                                 "14 98 E8 ?? ?? ?? ?? 48 85 C0 74 1D 49 8B 0E 0F B6 14 0B 38 50 78");
    uint8_t* instance = FindUnique("85 D2 74 29 48 8B 81 C8 01 00 00 44 8B C2 41 81 E0 FF FF FF 00 C1 EA 18");
    uint8_t* entityComponent = FindUnique("40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B C3 48 83 C4 20 5B C3 CC CC CC "
                                          "CC CC CC CC CC CC 4C 8B D1");
    if (!next || !setter || !instance || !entityComponent ||
        !DiscoverSystem("dynamic_render_model", g_systems[0]) || !DiscoverSystem("static_render_model", g_systems[1])) {
        if (g_log) g_log("Chest glow: game code not found (next %p, set %p, instance %p, component %p); GG outlines no chests",
                         next, setter, instance, entityComponent);
        return;
    }
    g_set = reinterpret_cast<SetFn>(setter);
    g_create = reinterpret_cast<CreateFn>(FindUnique("40 55 57 41 54 41 56 41 57 48 83 EC 30"));
    g_transform = reinterpret_cast<TransformFn>(FindUnique("48 8B C4 44 88 48 20 4C 89 40 18 55"));
    g_delete = reinterpret_cast<DeleteFn>(FindUnique("48 89 6C 24 20 57 41 54 41 56 48 83 EC 30"));
    g_bind = reinterpret_cast<BindFn>(
        FindUnique("48 89 5C 24 18 48 89 6C 24 20 56 41 54 41 57 48 83 EC 20 45 33 E4"));
    g_upload = reinterpret_cast<UploadFn>(FindUnique("48 8B C4 48 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 88 "
                                                     "48 81 EC 78 01 00 00 4C 8B 71 18"));
    // the palette's group1 / group2 colours (two places each): red and amber for the creature copies
    {
        static const char* const kFight[2] = {"8B 53 58 E8 ?? ?? ?? ?? 0F 28 C6 48 8D 4C 24 70 0F 28 CE",
                                              "8B 53 58 E8 ?? ?? ?? ?? 0F 28 C6 0F 28 CE 0F 10 18"};
        static const char* const kAlert[2] = {"8B 53 5C E8 ?? ?? ?? ?? 0F 28 C6 48 8D 4C 24 70 0F 28 CE",
                                              "8B 53 5C E8 ?? ?? ?? ?? 0F 28 C6 48 8D 8D ?? ?? ?? ?? 0F 28 CE"};
        void* fightHooks[2] = {reinterpret_cast<void*>(&FightColourHook<0>), reinterpret_cast<void*>(&FightColourHook<1>)};
        void* alertHooks[2] = {reinterpret_cast<void*>(&AlertColourHook<0>), reinterpret_cast<void*>(&AlertColourHook<1>)};
        int hooked = 0;
        for (int k = 0; k < 2; ++k) {
            if (uint8_t* site = FindUnique(kFight[k]))
                hooked += (g_fightNext[k] = reinterpret_cast<ColorFn2>(jumppatch::RedirectCall(site + 3, fightHooks[k]))) != nullptr;
            if (uint8_t* site = FindUnique(kAlert[k]))
                hooked += (g_alertNext[k] = reinterpret_cast<ColorFn2>(jumppatch::RedirectCall(site + 3, alertHooks[k]))) != nullptr;
        }
        if (g_log) g_log("Chest glow: creature aggro colours %d of 4 palette calls", hooked);
    }
    // the frame function's call staging the render instances (mov rcx, r12; call; cmp qword [r12 + 0xa38], 0)
    if (uint8_t* site = FindUnique("49 8B CC E8 ?? ?? ?? ?? 49 83 BC 24 38 0A 00 00 00 74")) {
        if (g_upload && jumppatch::CallTarget(site + 3) == reinterpret_cast<void*>(g_upload))
            g_stageNext = reinterpret_cast<UploadFn>(jumppatch::RedirectCall(site + 3, reinterpret_cast<void*>(&StageHook)));
        else if (g_upload) // a previous generation's redirect: chain on it
            g_stageNext = reinterpret_cast<UploadFn>(jumppatch::RedirectCall(site + 3, reinterpret_cast<void*>(&StageHook)));
    }
    if (g_log) g_log("Chest glow: copies placed before staging %s", g_stageNext ? "ready" : "not found");
    if (!g_stageNext) g_proxyFinal = 0;
    if (uint8_t* site = FindUnique("8B 53 64 E8 ?? ?? ?? ?? 0F 28 C6 0F 28 CE 0F 28 D6 0F 10 18"))
        g_paletteNext = reinterpret_cast<ColorFn>(jumppatch::RedirectCall(site + 3, reinterpret_cast<void*>(&PaletteHook)));
    if (g_log)
        g_log("Chest glow: late copy placement %s (upload %p)", g_paletteNext ? "ready" : "not found", g_upload);
    if (g_log && !ProxiesReady())
        g_log("Chest glow: render instance functions not found (create %p, transform %p, delete %p, bind %p); creatures "
              "get silhouettes", g_create, g_transform, g_delete, g_bind);
    g_instance = reinterpret_cast<InstanceFn>(instance);
    g_entityComponent = reinterpret_cast<EntityComponentFn>(entityComponent);
    static const char* const kSystems[] = {"dynamic_render_model", "static_render_model"};
    int calls = 0, found = 0;
    g_next = reinterpret_cast<NextFn>(RedirectSystemCalls(next, kSystems, 2, reinterpret_cast<void*>(&NextHook), calls, found));
    const bool ok = g_next != nullptr;
    if (g_log) g_log(ok ? "Chest glow: ready (%d iterator calls in %d render systems)" : "Chest glow: redirect failed (%d calls, %d systems)",
                     calls, found);
    if (!ok) g_next = nullptr;
}

struct NearChest {
    float x, y, z;
    size_t chest;      // a chest (kChestSpawns), or kTotalChestCount
    size_t deposit;    // a lore note / boss (kOreDeposits), or kOreDepositCount
    uint32_t colour;
    uint32_t questRow = 0; // an active quest's pin (neither of the above): its Main Quest / Side Quest row
    bool isCreature = false; // a live creature (none of the above): where it is now is read from it
    creature_live::LiveCreature creature{};
    int creatureRow = -1;
};
std::vector<NearChest> g_markers; // what GG lit (UI thread)
uint64_t g_markersStart = 0, g_markersUntil = 0;

bool TryDetectTypes(uint8_t* reg) {
    __try {
        return reg && DetectWaterTypesInner(reg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Targets: the client registry's entities standing where the chests stand, each tagged with its chest; Apply lights
// one model per chest.
void FillTargets(uint8_t* reg, const NearChest* chests, size_t chestCount, uint64_t until) {
    AcquireSRWLockExclusive(&g_lock);
    __try {
        g_count = 0;
        const uint16_t posType = g_waterTypes.position;
        uint8_t* map = reg + 0x148;
        const uint64_t capacity = *reinterpret_cast<uint64_t*>(map + 0x10);
        auto* used = *reinterpret_cast<uint64_t**>(map + 0x08);
        auto* ids = *reinterpret_cast<uint32_t**>(map + 0x28);
        auto* records = *reinterpret_cast<uint8_t***>(reg + 0x188);
        // live creatures: their own entities
        for (size_t c = 0; c < chestCount && g_count < kMax; ++c) {
            const NearChest& n = chests[c];
            if (!n.isCreature) continue;
            Target t{n.creature.entity, {n.x, n.y, n.z}, until, n.colour, static_cast<uint32_t>(c), 0.0f, 0.0f};
            t.follow = true;
            const int aggro = creature_live::Aggro(n.creature);
            t.copyGroup = aggro == 2 ? kFightGroup : aggro == 1 ? kAlertGroup : kGroup;
            g_targets[g_count++] = t;
        }
        for (uint64_t i = 0; used && ids && records && capacity <= (1u << 22) && i < capacity && g_count < kMax; ++i) {
            if (!(used[i >> 6] >> (i & 63) & 1) || ids[i] < 0x400 || !records[i]) continue;
            float p[3];
            if (!EcsPosition(records[i], posType, p)) continue;
            for (size_t c = 0; c < chestCount; ++c) {
                const NearChest& n = chests[c];
                if (n.isCreature) continue;
                // notes / bosses: whole metres; a quest's pin stands by its item or in its place
                const float tol = n.chest < kTotalChestCount ? 0.6f : n.questRow ? 1.5f : 1.2f;
                if (std::fabs(p[0] - n.x) < tol && std::fabs(p[2] - n.z) < tol && std::fabs(p[1] - n.y) < tol + 0.6f) {
                    // a note's: the one standing closest to its spot wins, not the smallest (a bowl beside a book
                    // was smaller, and lit; 2026-11-09)
                    const float dist = n.chest < kTotalChestCount ? 0.0f : std::hypot(p[0] - n.x, p[2] - n.z);
                    g_targets[g_count++] = {ids[i], {p[0], p[1], p[2]}, until, n.colour, static_cast<uint32_t>(c), 0.0f, dist};
                    break;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_count = 0;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// The pins of the quests you're on within kNearMeters of (px, pz): the map's markers (client + 0x36ea00) whose id
// an active quest's open objective names (GetQuestFilterHashForMarkerId), quest pins and the places a quest sends
// you to alike, while that quest's row is on. Area circles (0x454e24f2) are a region, not a thing: left out.
struct QuestPin {
    float x, y, z;
    uint32_t row;
};
size_t CollectQuestPinsInner(uint8_t* client, float px, float pz, QuestPin* out, size_t cap) {
    uint8_t* base = *reinterpret_cast<uint8_t**>(client + 0x36ea00);
    const uint64_t count = *reinterpret_cast<uint64_t*>(client + 0x36ea08);
    if (!base || count == 0 || count > 10000) return 0;
    auto* entries = reinterpret_cast<FilterEntry*>(client + 0x40eaa0);
    const size_t filterCount = *reinterpret_cast<uint8_t*>(client + 0x40ebe0);
    size_t n = 0;
    for (uint64_t i = 0; i < count && n < cap; ++i) {
        const uint8_t* m = base + i * 0xA0;
        if ((*reinterpret_cast<const uint32_t*>(m) & 0xFFFF0000u) == kChestMarkerUidPrefix) continue;
        const uint32_t orig = GetMarkerOrigTypeHash(m);
        if (orig == 0u || orig == 0x454e24f2u) continue;
        const float x = static_cast<float>(*reinterpret_cast<const int64_t*>(m + 0x10) / 4294967296.0);
        const float z = static_cast<float>(*reinterpret_cast<const int64_t*>(m + 0x20) / 4294967296.0);
        if ((x - px) * (x - px) + (z - pz) * (z - pz) > kNearMeters * kNearMeters) continue;
        uint32_t id = IsSlotSaved(m) ? *reinterpret_cast<const uint32_t*>(m + 0x4c) : 0u;
        if (id == 0u) id = *reinterpret_cast<const uint32_t*>(m + 0x08);
        const uint32_t row = GetQuestFilterHashForMarkerId(id, true);
        if (row == 0u || !IsFilterEnabled(entries, filterCount, row)) continue;
        out[n++] = {x, static_cast<float>(*reinterpret_cast<const int64_t*>(m + 0x18) / 4294967296.0), z, row};
    }
    return n;
}
size_t CollectQuestPins(uint8_t* client, float px, float pz, QuestPin* out, size_t cap) {
    __try {
        return CollectQuestPinsInner(client, px, pz, out, cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The nearby unlooted chests' entities (client registry, by their position) and positions; lights them.
void LightNearbyChests(uint8_t* client) {
    float px = 0.0f, pz = 0.0f;
    if (!GetHudPlayerWorldPos(client, px, pz)) return;
    // The nearby unlooted chests from the chest list
    std::vector<NearChest> nearChests;
    for (size_t c = 0; c < kTotalChestCount; ++c) {
        const bool gold = IsGoldChest(c);
        if (IsChestCurrentlyCleared(c) || !ChestRowEnabled(c)) continue;
        const ChestSpawn& sp = kChestSpawns[c];
        const float x = sp.px1 + sp.px0 / 65536.0f, y = sp.py1 + sp.py0 / 65536.0f, z = sp.pz1 + sp.pz0 / 65536.0f;
        if ((x - px) * (x - px) + (z - pz) * (z - pz) > kNearMeters * kNearMeters) continue;
        nearChests.push_back({x, y, z, c, kOreDepositCount, gold ? kFindGold : kSilver});
    }
    // Lore notes not read and bosses not defeated, while their rows are on
    for (size_t i = 0; i < kKnowledgeSpotCount; ++i) {
        const size_t d = kKnowledgeFirstDeposit + i;
        const OreDeposit& dep = kOreDeposits[d];
        if (!g_oreEnabled[dep.ore] || KnowledgeDone(d)) continue;
        if ((dep.x - px) * (dep.x - px) + (dep.z - pz) * (dep.z - pz) > kNearMeters * kNearMeters) continue;
        nearChests.push_back({float(dep.x), float(dep.topY), float(dep.z), kTotalChestCount, d, kFindGold});
    }
    // What the quests you're on want here
    QuestPin pins[16];
    const size_t pinCount = CollectQuestPins(client, px, pz, pins, 16);
    for (size_t i = 0; i < pinCount; ++i) {
        nearChests.push_back({pins[i].x, pins[i].y, pins[i].z, kTotalChestCount, kOreDepositCount, kFindGold, pins[i].row});
    }
    // With Nearby Creatures Only: the live creatures of the creature rows that are on, the nearest first
    size_t creatureCount = 0;
    if (g_nearCreatures) {
        std::vector<creature_live::LiveCreature> live;
        creature_live::Scan(live, px, pz, kNearMeters);
        std::sort(live.begin(), live.end(), [&](const auto& a, const auto& b) {
            return (a.x - px) * (a.x - px) + (a.z - pz) * (a.z - pz) < (b.x - px) * (b.x - px) + (b.z - pz) * (b.z - pz);
        });
        for (const auto& c : live) {
            const int row = creature_live::ShownRow(c.model);
            if (row < 0 || creatureCount >= kMaxCreatures) continue;
            NearChest n{c.x, c.y, c.z, kTotalChestCount, kOreDepositCount, kCreatureRed};
            n.isCreature = true;
            n.creature = c;
            n.creatureRow = row;
            nearChests.push_back(n);
            ++creatureCount;
        }
    }
    g_markers = nearChests;
    g_markersStart = GetTickCount64();
    g_markersUntil = g_markersStart + kGlowMs;
    // The map and minimap pulse them (like Find's results)
    g_pulsingPlaces.clear();
    for (const auto& n : nearChests)
        if (!n.isCreature) g_pulsingPlaces.push_back({n.x, n.z}); // a creature moves on: its own icon shows it
    g_pulseStartTick = GetTickCount64();
    if (g_log) g_log("Chest glow: %zu chests, notes, bosses, quest pins and creatures within %.0f m (%zu quest, %zu creatures)",
                     nearChests.size(), kNearMeters, pinCount, creatureCount);
    if (!g_next || nearChests.empty()) return;
    // Their entities: an entity of the client registry standing where a chest stands
    auto* reg = static_cast<uint8_t*>(g_waterRegistry.load(std::memory_order_relaxed));
    if (!reg || !g_waterTypes.position) {
        reg = nullptr;
        for (auto& slot : g_waterRegistries) {
            auto* cand = static_cast<uint8_t*>(slot.load(std::memory_order_relaxed));
            if (TryDetectTypes(cand) || (cand && g_waterTypes.registry == cand && g_waterTypes.position)) {
                reg = cand;
                break;
            }
        }
    }
    if (!reg || !g_waterTypes.position) return;
    FillTargets(reg, nearChests.data(), nearChests.size(), GetTickCount64() + kGlowMs);
    if (g_log) g_log("Chest glow: %u lit", g_count);
}

// G ([Keys] Glow) twice within half a second while playing (not in a menu, where G sorts the inventory).
void UpdateKey(uint8_t* client, bool appFocused) {
    g_proxyClient.store(client, std::memory_order_relaxed);
    RefreshCreatureAggro();
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    if (client && LocalPlayerXZ(client, px, pz) && LocalPlayerY(client, py)) {
        g_playerPos[0] = px, g_playerPos[1] = py, g_playerPos[2] = pz;
        InterlockedExchange(&g_havePlayer, 1);
    } else {
        InterlockedExchange(&g_havePlayer, 0);
    }
    vein_surface::Tick(client);
    static bool wasDown = false;
    static uint64_t firstTick = 0;

    const bool down = appFocused && BindingDown(g_keys.glow);
    const bool edge = down && !wasDown;
    wasDown = down;
    if (!edge || !client || client[0x1cd8] != 1) return;
    const uint64_t now = GetTickCount64();
    if (firstTick && now - firstTick <= 500ULL) {
        firstTick = 0;
        LightNearbyChests(client);
        vein_sight::LightNearby(client); // and the veins of the Mining / Materials rows that are on
    } else {
        firstTick = now;
    }
}

// The lit chests on screen, through walls: a soft gold glow and a ring pulsing with the outline, the chest's icon in
// the middle, smaller with distance. Projected the way Find projects (the client's view / projection matrices and
// the UI window's viewport); behind the camera or looted since: not drawn.
void DrawMarkersInner(uint8_t* client, void* widgetRaw) {
    const uint64_t now = GetTickCount64();
    if (now >= g_markersUntil) g_markers.clear();
    auto* root = static_cast<uint8_t*>(GetUiG());
    uint8_t* window = root ? *reinterpret_cast<uint8_t**>(root + 0x10) : nullptr;
    if (!window) return;
    float view[16], proj[16], viewport[4];
    std::memcpy(view, client + 0x1d20, sizeof(view));
    std::memcpy(proj, client + 0x1d60, sizeof(proj));
    std::memcpy(viewport, window + 0x18, sizeof(viewport));
    if (viewport[2] < 1.0f || viewport[3] < 1.0f) return;
    vein_sight::Draw(client, widgetRaw, view, proj, viewport); // under the chests' rings
    if (g_markers.empty()) return;
    float px = 0.0f, pz = 0.0f;
    GetHudPlayerWorldPos(client, px, pz);
    const float elapsed = static_cast<float>(now - g_markersStart) * 0.001f;
    const float breathe = 0.5f - 0.5f * std::cos(std::fmod(elapsed, 1.2f) / 1.2f * 6.2831853f);
    const float fadeOut = std::min(1.0f, static_cast<float>(g_markersUntil - now) / 1500.0f);
    // Silhouettes of the lit models (snapshots taken by the render hook)
    chest_silhouette::Snapshot snaps[kMax];
    unsigned snapCount = 0;
    AcquireSRWLockShared(&g_lock);
    for (unsigned i = 0; i < g_appliedCount && snapCount < kMax; ++i) {
        if (g_applied[i].haveSnapshot) snaps[snapCount++] = g_applied[i].snapshot;
    }
    ReleaseSRWLockShared(&g_lock);
    chest_silhouette::Camera camera;
    std::memcpy(camera.view, view, sizeof(view));
    std::memcpy(camera.projection, proj, sizeof(proj));
    std::memcpy(camera.viewport, viewport, sizeof(viewport));
    bool drawn[kMax]{};
    chest_silhouette::Draw(widgetRaw, camera, snaps, snapCount, fadeOut, drawn);
    // A ring with its icon for what has no silhouette here (no model near it, or none decodable)
    auto hasSilhouette = [&](const NearChest& c) {
        for (unsigned i = 0; i < snapCount; ++i) {
            if (!drawn[i]) continue;
            const auto& t = snaps[i].transform;
            const float sx = float(t.position[0] / 4294967296.0), sz = float(t.position[2] / 4294967296.0);
            if (std::fabs(sx - c.x) < 1.5f && std::fabs(sz - c.z) < 1.5f) return true;
        }
        return false;
    };
    for (NearChest& c : g_markers) {
        if (c.isCreature) {
            if (ProxiesReady() && g_proxyCount) continue; // drawn by their copies
            float p[3];
            if (!creature_live::Position(c.creature, p)) continue; // gone (killed, despawned)
            c.x = p[0], c.y = p[1] + 0.6f, c.z = p[2];
        } else if (c.chest < kTotalChestCount ? IsChestCurrentlyCleared(c.chest) : !c.questRow && KnowledgeDone(c.deposit)) {
            continue;
        }
        if (hasSilhouette(c)) continue;
        const float world[3] = {c.x, c.y + 0.6f, c.z};
        const float depth = world[0] * view[2] + world[1] * view[6] + world[2] * view[10] + view[14];
        if (!std::isfinite(depth) || depth <= 0.01f) continue;
        const float inv = 1.0f / depth;
        const float vx = inv * (world[0] * view[0] + world[1] * view[4] + world[2] * view[8] + view[12]);
        const float vy = inv * (world[0] * view[1] + world[1] * view[5] + world[2] * view[9] + view[13]);
        const float sx = ((vy * proj[4] + vx * proj[0] + proj[8] + proj[12]) * 0.5f + 0.5f) * viewport[2] + viewport[0];
        const float sy = (0.5f - (vy * proj[5] + vx * proj[1] + proj[9] + proj[13]) * 0.5f) * viewport[3] + viewport[1];
        if (!std::isfinite(sx) || !std::isfinite(sy)) continue;
        const float dist = std::sqrt((c.x - px) * (c.x - px) + (c.z - pz) * (c.z - pz));
        const float size = std::max(18.0f, std::min(40.0f, 40.0f * 12.0f / std::max(12.0f, dist)));
        const float glow = size * (1.8f + 0.5f * breathe), ring = size * (1.15f + 0.25f * breathe);
        const bool gold = c.chest < kTotalChestCount && IsGoldChest(c.chest);
        const uint32_t tint = c.chest >= kTotalChestCount ? 0xffffffffu
                              : gold                         ? 0xff3cc8ffu
                              : IsWoodChest(c.chest)         ? kWoodChestTint
                                                             : 0xffe8e4dcu;
        DrawAssetRectAbs(widgetRaw, kSoftGlowPrimitive, sx - glow * 0.5f, sy - glow * 0.5f, glow, glow, c.colour,
                         (0.35f + 0.35f * breathe) * fadeOut);
        DrawAssetRectAbs(widgetRaw, kCircleOutlinePrimitive, sx - ring * 0.5f, sy - ring * 0.5f, ring, ring, 0xff5ca4c9u,
                         (0.55f + 0.45f * breathe) * fadeOut);
        DrawAssetRectAbs(widgetRaw, kCirclePrimitive, sx - size * 0.5f, sy - size * 0.5f, size, size, 0xff0a0e10u,
                         0.6f * fadeOut);
        const void* icon = c.isCreature                  ? kOreIcons[c.creatureRow]
                           : c.chest < kTotalChestCount ? GetIconRefForType(client, ChestTypeHash(c.chest))
                           : c.questRow                 ? GetIconRefForType(client, c.questRow)
                                                        : DepositIcon(c.deposit);
        if (icon) {
            DrawIconRectAbs(widgetRaw, icon, sx - size * 0.4f, sy - size * 0.4f, size * 0.8f, size * 0.8f, tint, fadeOut);
        }
    }
}

bool SafeDrawMarkersInner(uint8_t* client, void* widgetRaw) {
    __try {
        DrawMarkersInner(client, widgetRaw);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Its own HUD layer while playing (the minimap's only exists while the minimap shows).
void DrawMarkers(uint8_t* client) {
    if ((g_markers.empty() && !vein_sight::Active()) || !client || client[0x1cd8] != 1) return;
    static const UiText kLayerName = MakeUiText("chestGlowLayer");
    alignas(16) uint8_t layer[0x400]{};
    g_beginLayer(layer, &kLayerName, 1, 2, -1, 0);
    Widget anchor{};
    g_namedWidget(anchor.raw, &kLayerName);
    g_opaque(anchor.raw);
    if (!SafeDrawMarkersInner(client, anchor.raw)) g_markers.clear();
    g_endWidget(anchor.raw);
    g_endLayer(layer);
}

} // namespace chest_glow
