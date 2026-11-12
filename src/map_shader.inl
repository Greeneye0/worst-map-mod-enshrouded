// The map's terrain shader, made to work turned (Settings > Minimap Rotation > Facing Up).
//
// game38_ui/map_renderer.gpc's MapRenderer fragment shader (the World Map's and minimap's terrain) sizes its contour
// lines with dFdx(uv.x * tiles) * 1024: the map's x per screen pixel along the screen's x. Turned a quarter, that is 0,
// and past it negative, so the lines went and the map read as cloud. fwidth (|dFdx| + |dFdy|) is the same number on
// a map that isn't turned (dFdy of it is 0 then) and stays right turned: one SPIR-V opcode, OpDPdx (207) to
// OpFwidth (209), same operands.
//
// The game makes the map's shader modules at start in FUN_140fe3970(device, out, span {SPIR-V, bytes}).
// The hook hands that one module the changed copy (only the map shader's exact bytes, by size and hash; both its
// rgba and bgra variants are these bytes), so it takes a game start to apply.
namespace map_shader {

constexpr uint64_t kBytes = 18612;
constexpr uint64_t kFnv = 0xd8f171462f3aa77eull; // FNV-1a 64 of the 18612 bytes (build of 2026-11-02)
constexpr size_t kWord = 1785;                   // the OpDPdx instruction
constexpr uint32_t kDPdx = 0x000400CFu, kFwidth = 0x000400D1u;

std::atomic<int> g_swapped{0};

uint64_t Fnv(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// The code at *codeSlot (sizeSlot its bytes) when it is the map shader, else null
const uint8_t* MapCode(const uint8_t* const* codeSlot, const uint64_t* sizeSlot) {
    __try {
        const uint8_t* code = *codeSlot;
        if (*sizeSlot != kBytes || !code) return nullptr;
        if (*reinterpret_cast<const uint32_t*>(code + kWord * 4) != kDPdx || Fnv(code, kBytes) != kFnv) return nullptr;
        return code;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The game has three makers of shader modules, each with the code and its size at its own place:
// A FUN_140fd3b90(device, desc): +0x10 code, +0x18 bytes; B FUN_140fe2c10(device, desc): +8 code, +0x10 bytes;
// C FUN_140fe3970(device, out, span): span {code, bytes}
using MakeFn = void* (*)(void*, void*, void*);
MakeFn g_orig[3] = {};
std::atomic<int> g_calls[3];

template <int K, size_t CodeAt, size_t SizeAt, int Arg>
void* Make(void* a, void* b, void* c) {
    greeneye::Lease lease;
    ++g_calls[K];
    uint8_t* desc = static_cast<uint8_t*>(Arg == 1 ? b : c);
    auto** codeSlot = reinterpret_cast<const uint8_t**>(desc + CodeAt);
    const uint8_t* code = lease && desc ? MapCode(codeSlot, reinterpret_cast<const uint64_t*>(desc + SizeAt)) : nullptr;
    if (!code) return g_orig[K](a, b, c);
    // The changed copy for this one call (the driver copies the code into its module)
    static uint32_t patched[kBytes / 4];
    std::memcpy(patched, code, kBytes);
    patched[kWord] = kFwidth;
    *codeSlot = reinterpret_cast<const uint8_t*>(patched);
    void* result = g_orig[K](a, b, c);
    *codeSlot = code;
    const int n = ++g_swapped;
    if (g_log) g_log("Map shader: the terrain's contour lines made to work turned (maker %c, module %d)", 'A' + K, n);
    return result;
}

void Install() {
    struct Site {
        const char* pattern;
        size_t length; // whole instructions
        void* detour;
    };
    // Only C makes the map's modules (the game start of 2026-11-12: A 793 modules, B 0, C 1090, the map shader by C)
    const Site sites[1] = {
        // sub rsp, 88h; mov r9, [r8]; test r9, r9
        {"48 81 EC 88 00 00 00 4D 8B 08 4D 85 C9 0F 84 ?? ?? ?? ?? 49 8B 40 08 0F 57 C0 4C 8B 81 40 12 00 00 0F 11 44 24 38 48 "
         "89 44 24 38 48 8B 81 80 07",
         13, reinterpret_cast<void*>(&Make<2, 0x00, 0x08, 2>)},
    };
    uint8_t* fn = FindUnique(sites[0].pattern);
    if (fn) g_orig[2] = reinterpret_cast<MakeFn>(InstallHook(fn, sites[0].detour, sites[0].length));
    if (g_log)
        g_log("Map shader: %s", g_orig[2] ? "hook ready (applies at game start)"
                                         : "not found in this game build; the turned minimap loses its contour lines");
}

#ifdef UMM_DEV_BRIDGE
// dev: how many modules each maker made since the hooks went in (logged once a minute or so with the minimap)
void LogCalls() {
    static uint64_t last = 0;
    if (!g_log || GetTickCount64() - last < 60000) return;
    last = GetTickCount64();
    g_log("Map shader: modules made A %d, B %d, C %d; map shader swapped %d", g_calls[0].load(), g_calls[1].load(),
          g_calls[2].load(), g_swapped.load());
}
#endif

// The terrain shader was changed at this game's start (else the turned minimap has no contour lines past ~70 deg)
bool Turnable() { return g_swapped.load() > 0; }

} // namespace map_shader
