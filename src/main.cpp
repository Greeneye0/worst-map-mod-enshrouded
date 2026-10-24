#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>

#include "patches.h"
#include "map_key_panel.h"
#include "scan.h"
#include "greeneye.h"

#define UMM_VERSION "1.2.0"

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

    for (size_t i = 0; i < unlock_map_markers::kNumPatchSites; ++i) {
        const auto& spec = unlock_map_markers::kPatchSites[i];
        const auto& site = g_resolvedSites[i];
        if (site.state == unlock_map_markers::SiteMatchState::AlreadyPatched) {
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

    if (!unlock_map_markers::InstallMapKeyPanelHook(&LogMessage)) {
        WriteStatusFile("error_hooks_failed");
        return false;
    }

    WriteStatusFile("active");
    LogMessage("All %zu map marker unlock patches + MapKeyPanel hooks are ACTIVE.", unlock_map_markers::kNumPatchSites);
    return true;
}

} // namespace

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
