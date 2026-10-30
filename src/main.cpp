#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <vector>

#include "patches.h"
#include "map_key_panel.h"
#include "scan.h"
#include "greeneye.h"

#define UMM_VERSION "1.3.23"

namespace {

bool g_patchesApplied = false;
uintptr_t g_imageBase = 0;
unlock_map_markers::ResolvedSite g_resolvedSites[unlock_map_markers::kNumPatchSites]{};

// The log and status file live in the mod's folder (greeneye\unlock-map-markers\). The log is
// opened per line, so a hot-reloaded generation appends to the same file.
void LogMessage(const char* fmt, ...) {
    char message[1024]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    SYSTEMTIME st{};
    GetLocalTime(&st);

    FILE* fp = nullptr;
    if (fopen_s(&fp, greeneye::DataPath("unlock_map_markers.log"), "a") == 0 && fp) {
        std::fprintf(
            fp,
            "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            message
        );
        std::fclose(fp);
    }
}

void WriteStatusFile(const char* state) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, greeneye::DataPath("UnlockMapMarkers.status"), "w") == 0 && fp) {
        std::fprintf(fp, "state=%s\n", state);
        std::fprintf(fp, "patched=%d\n", g_patchesApplied ? 1 : 0);
        std::fprintf(fp, "image_base=0x%llx\n", static_cast<unsigned long long>(g_imageBase));
        for (size_t i = 0; i < unlock_map_markers::kNumPatchSites; ++i) {
            const auto& spec = unlock_map_markers::kPatchSites[i];
            const auto& site = g_resolvedSites[i];
            const uintptr_t addr = reinterpret_cast<uintptr_t>(site.patchAddress);
            const uintptr_t rva = (addr >= g_imageBase) ? (addr - g_imageBase) : 0;
            std::fprintf(
                fp,
                "site_%zu=%s addr=0x%llx rva=0x%llx matches=%zu\n",
                i + 1,
                spec.name,
                static_cast<unsigned long long>(addr),
                static_cast<unsigned long long>(rva),
                site.matchCount
            );
        }
        std::fclose(fp);
    }
}

bool WriteProcessBytes(uint8_t* address, const uint8_t* bytes, size_t size) {
    if (!address || !bytes || size == 0) {
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("VirtualProtect failed at %p (err=%lu)", address, GetLastError());
        return false;
    }
    std::memcpy(address, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    DWORD dummy = 0;
    VirtualProtect(address, size, oldProtect, &dummy);
    return true;
}

// The byte patches sit inside functions, not where hooks go. They are matched in the live code in
// either state, so a hot-reloaded generation finds them already patched.
bool ResolveAllSites() {
    uint8_t* text = nullptr;
    size_t size = 0;
    if (!unlock_map_markers::GameText(text, size)) {
        LogMessage("ERROR: the game's .text section was not found");
        return false;
    }
    g_imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

    bool allFound = true;
    for (size_t i = 0; i < unlock_map_markers::kNumPatchSites; ++i) {
        const auto& spec = unlock_map_markers::kPatchSites[i];
        g_resolvedSites[i] = unlock_map_markers::FindPatchSite(text, size, spec);
        const auto& site = g_resolvedSites[i];
        if (site.state == unlock_map_markers::SiteMatchState::NotFound || !site.patchAddress) {
            LogMessage("ERROR: Failed to find signature for %s", spec.name);
            allFound = false;
        } else if (site.matchCount != 1) {
            LogMessage("ERROR: Signature for %s is not unique (%zu matches)", spec.name, site.matchCount);
            allFound = false;
        } else {
            const uintptr_t rva = reinterpret_cast<uintptr_t>(site.patchAddress) - g_imageBase;
            LogMessage(
                "Resolved %s at %p (RVA 0x%llx, state=%d)",
                spec.name,
                site.patchAddress,
                static_cast<unsigned long long>(rva),
                static_cast<int>(site.state)
            );
        }
    }
    return allFound;
}

bool ApplyAllPatches() {
    if (!ResolveAllSites()) {
        WriteStatusFile("error_signature_not_found");
        return false;
    }

    // Settings > Advanced > Vanilla World Map: the game's bytes stay (a hot reload finding them patched leaves that to
    // the mod's switch, which waits until no thread is in them)
    const bool vanilla = unlock_map_markers::VanillaWorldMapInConfig();
    bool allPatched = true;
    for (size_t i = 0; i < unlock_map_markers::kNumPatchSites; ++i) {
        const auto& spec = unlock_map_markers::kPatchSites[i];
        const auto& site = g_resolvedSites[i];
        if (site.state == unlock_map_markers::SiteMatchState::AlreadyPatched) {
            continue;
        }
        if (vanilla) {
            allPatched = false;
            continue;
        }
        if (!WriteProcessBytes(site.patchAddress, spec.replacementBytes, spec.patchSize)) {
            LogMessage("ERROR: Failed to write patch for %s", spec.name);
            WriteStatusFile("error_write_failed");
            return false;
        }
        LogMessage("Applied patch %zu (%s, %zu bytes) at %p", i + 1, spec.name, spec.patchSize, site.patchAddress);
    }
    g_patchesApplied = true;
    unlock_map_markers::SetWorldMapPatchedAtLoad(allPatched);
    if (vanilla) LogMessage("Vanilla World Map: the marker patches are %s; the hooks below still go in",
                            allPatched ? "on, coming off" : "left off");

    if (!unlock_map_markers::InstallMapKeyPanelHook(&LogMessage)) {
        WriteStatusFile("error_hooks_failed");
        return false;
    }

    WriteStatusFile("active");
    LogMessage("All %zu map marker unlock patches + MapKeyPanel hooks are ACTIVE.", unlock_map_markers::kNumPatchSites);
    return true;
}

} // namespace

namespace unlock_map_markers {
// The mod's switch (Ctrl + Shift + F12) puts the game's own bytes back, and the mod's again.
// Ctrl+Shift+F12 switches the patches while the game runs. The marker code they sit in runs on the game's worker
// threads, and a thread inside a site while its bytes change runs half old, half new code (two crashes reported by
// secretroff: an access violation, and the game's heap finding a block freed twice). So, as the loader writes its
// hooks: every other thread suspended, none inside or just before a site. False: one was, every try; nothing written.
// From the first SuspendThread to the last ResumeThread: no allocation, no log.
bool SetMarkerUnlockPatches(bool on) {
    struct Range {
        uintptr_t lo, hi;
    };
    Range ranges[kNumPatchSites]{};
    size_t rangeCount = 0;
    for (size_t i = 0; i < kNumPatchSites; ++i) {
        if (!g_resolvedSites[i].patchAddress) continue;
        const auto at = reinterpret_cast<uintptr_t>(g_resolvedSites[i].patchAddress);
        // Back to the start of the function it is in (sites 1 and 2 are 0x8b9 bytes into replicate_map_markers_for_ui)
        ranges[rangeCount++] = {at - 0x900, at + kPatchSites[i].patchSize};
    }
    if (rangeCount == 0) return true;
    std::vector<HANDLE> threads;
    threads.reserve(256);
    for (int attempt = 0; attempt < 20; ++attempt) {
        threads.clear();
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        const DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
        THREADENTRY32 entry{sizeof(entry)};
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
            if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                      entry.th32ThreadID))
                threads.push_back(h);
        }
        CloseHandle(snapshot);
        bool suspended[1024]{};
        const size_t n = std::min<size_t>(threads.size(), 1024);
        bool clear = threads.size() <= 1024;
        for (size_t t = 0; t < n; ++t) suspended[t] = SuspendThread(threads[t]) != static_cast<DWORD>(-1);
        for (size_t t = 0; t < n && clear; ++t) {
            if (!suspended[t]) {
                DWORD code = 0; // only a thread that has ended may refuse
                if (!GetExitCodeThread(threads[t], &code) || code == STILL_ACTIVE) clear = false;
                continue;
            }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL; // also waits until the thread has stopped
            if (!GetThreadContext(threads[t], &context)) {
                clear = false;
                continue;
            }
            for (size_t r = 0; r < rangeCount; ++r) {
                if (context.Rip >= ranges[r].lo && context.Rip < ranges[r].hi) clear = false;
            }
        }
        if (clear) {
            for (size_t i = 0; i < kNumPatchSites; ++i) {
                const auto& site = g_resolvedSites[i];
                if (!site.patchAddress) continue;
                // WriteProcessBytes without its log line
                DWORD oldProtect = 0, dummy = 0;
                if (!VirtualProtect(site.patchAddress, kPatchSites[i].patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) continue;
                std::memcpy(site.patchAddress, on ? kPatchSites[i].replacementBytes : kPatchSites[i].originalBytes,
                            kPatchSites[i].patchSize);
                FlushInstructionCache(GetCurrentProcess(), site.patchAddress, kPatchSites[i].patchSize);
                VirtualProtect(site.patchAddress, kPatchSites[i].patchSize, oldProtect, &dummy);
            }
        }
        for (size_t t = 0; t < threads.size(); ++t) {
            if (t < n && suspended[t]) ResumeThread(threads[t]);
            CloseHandle(threads[t]);
        }
        if (clear) return true;
        Sleep(1);
    }
    return false;
}
} // namespace unlock_map_markers

// Greeneye Loader (greeneye\unlock-map-markers\unlock-map-markers.dll). With the loader's API 2
// every hook goes through the loader, so the mod can be hot reloaded while the game runs.
GREENEYE_MOD_START
{
    if (!greeneye::HostOk(host) || host->side != kGreeneyeClient) {
        return false;
    }
    greeneye::UseHost(host);
    greeneye::SetDataDir(host->modDir);
    if (!greeneye::ClaimProcess(L"UnlockMapMarkers", host)) {
        return false;
    }
    if (greeneye::Host()) {
        unlock_map_markers::UseKeptState(static_cast<unlock_map_markers::KeptState*>(
            host->keep("unlock-map-markers.state", sizeof(unlock_map_markers::KeptState))));
    }
    LogMessage("The Worst Map Mod " UMM_VERSION " loaded (Greeneye Loader%s, generation %u).",
               greeneye::Host() ? "" : " without hooks API",
               greeneye::Host() ? host->generation : 1u);
    return ApplyAllPatches();
}

// Before a newer generation replaces this one: the minimap's window procedure and mouse lock are
// given back and the Find dialog closes. The new generation sets them up again when needed.
static bool HotPrepare() {
    unlock_map_markers::PrepareMapKeyPanelForReload();
    LogMessage("stopping for a reload");
    return true;
}

static bool HotReady() {
    return true;
}

static void HotResume() {
    LogMessage("the reload failed; carrying on");
}

static const GreeneyeHot kHot = {sizeof(GreeneyeHot), &HotPrepare, &HotReady, &HotResume};
GREENEYE_MOD_HOT
{
    return &kHot;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
