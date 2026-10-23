#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "../src/patches.h"

namespace {

bool TestExecutable(const char* exePath) {
    std::ifstream file(exePath, std::ios::binary | std::ios::ate);
    if (!file) {
        std::fprintf(stderr, "FAIL: Could not open %s\n", exePath);
        return false;
    }

    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        std::fprintf(stderr, "FAIL: Could not read %s\n", exePath);
        return false;
    }

    std::printf("Loaded %s (%zu bytes)\n", exePath, buffer.size());

    for (size_t i = 0; i < unlock_map_markers::kNumPatchSites; ++i) {
        const auto& spec = unlock_map_markers::kPatchSites[i];
        auto site = unlock_map_markers::FindPatchSite(buffer.data(), buffer.size(), spec);
        if (site.matchCount != 1 || site.state != unlock_map_markers::SiteMatchState::Unpatched) {
            std::fprintf(
                stderr,
                "FAIL: Site %zu (%s) matchCount=%zu state=%d\n",
                i + 1,
                spec.name,
                site.matchCount,
                static_cast<int>(site.state)
            );
            return false;
        }
        const size_t fileOffset = static_cast<size_t>(site.patchAddress - buffer.data());
        std::printf("  PASS: Site %zu (%s) unique unpatched match at file offset 0x%zx\n", i + 1, spec.name, fileOffset);

        std::memcpy(site.patchAddress, spec.replacementBytes, spec.patchSize);

        auto patchedSite = unlock_map_markers::FindPatchSite(buffer.data(), buffer.size(), spec);
        if (patchedSite.matchCount != 1 ||
            patchedSite.state != unlock_map_markers::SiteMatchState::AlreadyPatched ||
            patchedSite.patchAddress != site.patchAddress) {
            std::fprintf(stderr, "FAIL: Site %zu (%s) did not match as AlreadyPatched after patching\n", i + 1, spec.name);
            return false;
        }

        std::memcpy(site.patchAddress, spec.originalBytes, spec.patchSize);
        auto restoredSite = unlock_map_markers::FindPatchSite(buffer.data(), buffer.size(), spec);
        if (restoredSite.matchCount != 1 ||
            restoredSite.state != unlock_map_markers::SiteMatchState::Unpatched ||
            restoredSite.patchAddress != site.patchAddress) {
            std::fprintf(stderr, "FAIL: Site %zu (%s) did not match as Unpatched after restore\n", i + 1, spec.name);
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    if (!TestExecutable("C:\\temp\\enshrouded-re\\bin\\enshrouded.exe")) {
        return 1;
    }
    if (!TestExecutable("C:\\Program Files (x86)\\Steam\\steamapps\\common\\Enshrouded\\enshrouded.exe")) {
        return 1;
    }
    std::printf("ALL %zu PATCH TESTS PASSED ON BOTH EXECUTABLES.\n", unlock_map_markers::kNumPatchSites);
    return 0;
}
