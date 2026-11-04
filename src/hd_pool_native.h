// Verified executable build 1273043; pinned Ghidra input, body bytes +32. Moved to the 2026-11-02 build
// (PE stamp 0x6ae4601e) by matching each function to the old one instruction for instruction; their bodies are unchanged
// apart from call targets and the registry's own fields past +0x35358 (+8), which the pool does not read.
// Moved again to the 2026-11-03 build 1285601 (PE stamp 0x6ae9b2ac) the same way; guards refreshed for the moved call targets.
// The reference build's addresses, keys for map_natives.h; they never change.
struct Entry { uint32_t rva; };
constexpr Entry scope = {0x11e8720};
constexpr Entry plan = {0x11e9240};
constexpr Entry image = {0x11e9e60};
constexpr Entry commit = {0x11f01e0};
constexpr Entry end = {0x11e9520};
constexpr Entry release = {0x11eed80};
constexpr Entry upload = {0x1032130};
constexpr Entry registerImage = {0x11a2ab0};
constexpr Entry unregisterImage = {0x11a6180};
constexpr Entry bindImage = {0x10345a0};
constexpr Entry rootDestroy = {0x1083c0};
constexpr Entry sceneTick = {0xf7890};
constexpr Entry viewDestroy = {0xf5760};
constexpr Entry registryDestroy = {0x11a5e30};
constexpr Entry freeImage = {0x102ed30};
