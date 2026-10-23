#pragma once
// Greeneye Loader mod API (see GreeneyeLoader/ and common/README.md). Shared by the loader and
// every mod; plain C data only crosses between them.
//
// The loader (dbgcore.dll, or renamed to umpdc.dll / cfgmgr32.dll) reads greeneye\<id>\mod.json,
// loads greeneye\<id>\<dll> and calls its exported GreeneyeModStart once, before the game's own
// code runs where it can. A mod keeps working under its old proxy name too (standalone), so the
// helpers below also cover that case: the data folder is then the game folder.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <cwchar>

constexpr uint32_t kGreeneyeApiVersion = 2;

enum GreeneyeSide : int { kGreeneyeClient = 1, kGreeneyeServer = 2 };

// Passed by the loader. Only ever grows by appending fields; check size before reading new ones.
struct GreeneyeHost {
    // API 1
    uint32_t size;                 // sizeof(GreeneyeHost) as the loader knows it
    uint32_t apiVersion;           // kGreeneyeApiVersion
    int side;                      // GreeneyeSide
    const wchar_t* modDir;         // ...\greeneye\<id>\ (trailing backslash)
    const wchar_t* gameDir;        // folder of the exe (trailing backslash)
    void (*log)(const char* line); // a line into greeneye\loader.log

    // API 2: the loader owns every hook, so a mod can be reloaded while the game runs (hot
    // reload, only with greeneye\hotreload.enable; see GreeneyeLoader/src/hot.cpp). A mod
    // installs its hooks with hook / redirectCall, never by patching itself, and every detour
    // takes a lease (greeneye::Lease) before it touches the mod's state.
    uint32_t generation;           // 1 at game start, 2, 3... for each reload of this mod
    bool hotReload;                // reloads can happen in this process
    void* token;                   // this generation, for enter
    // Lease: true while this generation is the active one (call on to the original when not).
    // Nested calls on one thread are fine. Always pair with leave.
    bool (*enter)(void* token);
    void (*leave)();
    // Unique match of pattern ("48 8B ?? 05") in the exe's code as it was before any of the
    // loader's hooks (also behind another mod's hook); module must be null.
    uint8_t* (*find)(const char* pattern, void* module);
    // The bytes at p as they were before the loader's hooks.
    void (*read)(const void* p, void* out, size_t n);
    // InstallHook: redirect the function at target (first len bytes, whole instructions; the
    // trampoline runs prologue instead of them when given) to detour. Returns what to call on.
    // In a reload, only the targets the first generation hooked are allowed.
    void* (*hook)(uint8_t* target, void* detour, size_t len, const uint8_t* prologue, size_t prologueLen);
    // The "call rel32" at call goes to detour instead; returns what it called before.
    void* (*redirectCall)(uint8_t* call, void* detour);
    // A safe point offered by the mod (no lease held): pending reloads may happen here.
    void (*poll)();
    // Zeroed memory of size bytes, the same for every generation of this mod (state that must
    // survive a reload). The first call with a key decides the size.
    void* (*keep)(const char* key, size_t size);
};

// Exported by every mod: start the mod, return true if it is running.
using GreeneyeModStartFn = bool (*)(const GreeneyeHost* host);
#define GREENEYE_MOD_START extern "C" __declspec(dllexport) bool GreeneyeModStart(const GreeneyeHost* host)

// Optionally exported (GreeneyeModHot) for hot reload: before a newer generation starts, the old
// one is asked to prepare (stop starting new work; false: not now, ask again next frame) until
// ready; if the newer one then fails to start, the old one gets resume.
struct GreeneyeHot {
    uint32_t size; // sizeof(GreeneyeHot)
    bool (*prepare)();
    bool (*ready)();
    void (*resume)();
};
using GreeneyeModHotFn = const GreeneyeHot* (*)();
#define GREENEYE_MOD_HOT extern "C" __declspec(dllexport) const GreeneyeHot* GreeneyeModHot()

namespace greeneye {

// One running copy of a mod per process: the standalone proxy and the loader copy may both be
// installed; whichever starts first wins. The handle stays open for the life of the process.
inline bool ClaimProcess(const wchar_t* mod)
{
    wchar_t name[96];
    swprintf_s(name, L"Local\\Greeneye-%s-%lu", mod, GetCurrentProcessId());
    HANDLE claim = CreateMutexW(nullptr, FALSE, name);
    if (!claim)
        return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(claim);
        return false;
    }
    return true;
}

// The folder this mod keeps its settings and logs in: the mod's own folder under the loader,
// the game folder (the exe's) when standalone.
inline wchar_t* DataDirBuffer()
{
    static wchar_t dir[MAX_PATH] = L"";
    return dir;
}

inline void SetDataDir(const wchar_t* dir)
{
    wcsncpy_s(DataDirBuffer(), MAX_PATH, dir, _TRUNCATE);
}

// The exe's folder, with a trailing backslash.
inline void GameDir(wchar_t* out, size_t count)
{
    out[0] = 0;
    if (!GetModuleFileNameW(nullptr, out, static_cast<DWORD>(count)))
        return;
    if (wchar_t* slash = wcsrchr(out, L'\\'))
        slash[1] = 0;
}

// Standalone mode: data stays in the game folder, as before the loader.
inline void UseGameDirForData()
{
    wchar_t dir[MAX_PATH];
    GameDir(dir, MAX_PATH);
    SetDataDir(dir);
}

// Absolute path of file in the data folder; the game folder if no folder was set (an ini path
// must be absolute: GetPrivateProfile* looks for a bare name in the Windows folder).
inline const char* DataPath(const char* file)
{
    static thread_local char path[MAX_PATH * 3];
    char dir[MAX_PATH * 3] = "";
    wchar_t game[MAX_PATH];
    const wchar_t* from = DataDirBuffer();
    if (!*from) {
        GameDir(game, MAX_PATH);
        from = game;
    }
    WideCharToMultiByte(CP_ACP, 0, from, -1, dir, sizeof(dir), nullptr, nullptr);
    snprintf(path, sizeof(path), "%s%s", dir, file);
    return path;
}

// Under the loader: move file from the game folder into the mod folder once, so existing
// settings carry over. Does nothing if the mod folder already has it or the game folder has not.
inline void Migrate(const char* file)
{
    wchar_t game[MAX_PATH], from[MAX_PATH], to[MAX_PATH], wfile[MAX_PATH];
    GameDir(game, MAX_PATH);
    MultiByteToWideChar(CP_ACP, 0, file, -1, wfile, MAX_PATH);
    swprintf_s(from, L"%s%s", game, wfile);
    swprintf_s(to, L"%s%s", DataDirBuffer(), wfile);
    if (_wcsicmp(from, to) == 0 || GetFileAttributesW(to) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(from) == INVALID_FILE_ATTRIBUTES)
        return;
    MoveFileExW(from, to, MOVEFILE_COPY_ALLOWED);
}

// True if this DLL is loaded under its standalone proxy name (e.g. L"wsock32.dll").
inline bool LoadedAs(HINSTANCE self, const wchar_t* proxyName)
{
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(self, path, MAX_PATH))
        return false;
    const wchar_t* file = wcsrchr(path, L'\\');
    return _wcsicmp(file ? file + 1 : path, proxyName) == 0;
}

// The host is usable by this mod (right API version, big enough).
inline bool HostOk(const GreeneyeHost* host)
{
    return host && host->apiVersion >= 1 && host->size >= offsetof(GreeneyeHost, generation) && host->modDir;
}

// The loader offers API 2 (hooks through the loader, leases, reloads).
inline bool HostHasHooks(const GreeneyeHost* host)
{
    return HostOk(host) && host->apiVersion >= 2 && host->size >= sizeof(GreeneyeHost) && host->hook && host->enter;
}

// This module's host, once the mod has started under the loader with API 2 (null standalone).
inline const GreeneyeHost*& Host()
{
    static const GreeneyeHost* host = nullptr;
    return host;
}

inline void UseHost(const GreeneyeHost* host)
{
    Host() = HostHasHooks(host) ? host : nullptr;
}

// A reload (generation 2 and later) belongs to the process the first generation claimed.
inline bool ClaimProcess(const wchar_t* mod, const GreeneyeHost* host)
{
    if (HostHasHooks(host) && host->generation > 1)
        return true;
    return ClaimProcess(mod);
}

// Held by a detour for as long as it runs the mod's code: a reload waits for every lease to end.
// Standalone (no host) it is always active.
class Lease {
public:
    Lease() : host_(Host()), active_(!host_ || host_->enter(host_->token)) {}
    ~Lease()
    {
        if (host_)
            host_->leave();
    }
    explicit operator bool() const { return active_; }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

private:
    const GreeneyeHost* host_;
    bool active_;
};

} // namespace greeneye
