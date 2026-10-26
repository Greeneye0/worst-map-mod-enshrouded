#pragma once

#include <cstdint>
#include <cstring>

namespace unlock_map_markers {

struct PatchSiteSpec {
    const char* name;
    const uint8_t* signature;
    size_t signatureSize;
    size_t patchOffset;
    const uint8_t* originalBytes;
    const uint8_t* replacementBytes;
    size_t patchSize;
};

// Site 1: replicate_map_markers_for_ui (RVA 0x310d90)
// Original at +0x0d: 0f 84 60 06 00 00 (je 0x140311403 when MapMarkerVisibilityState->visibility == 0)
// Patch at +0x0d:    90 90 90 90 90 90 (NOP 6 bytes so hidden markers are replicated to the UI)
inline constexpr uint8_t kSite1Signature[32] = {
    0x48, 0x8b, 0x44, 0x24, 0x58,       // mov rax, [rsp + 0x58]
    0x48, 0x85, 0xc0,                   // test rax, rax
    0x74, 0x09,                         // je +9
    0x80, 0x38, 0x00,                   // cmp byte ptr [rax], 0
    0x0f, 0x84, 0x60, 0x06, 0x00, 0x00, // je +0x660
    0x48, 0x8b, 0x44, 0x24, 0x50,       // mov rax, [rsp + 0x50]
    0x48, 0x8b, 0x50, 0x10,             // mov rdx, [rax + 0x10]
    0x4c, 0x8b, 0x40, 0x18              // mov r8, [rax + 0x18]
};
inline constexpr uint8_t kSite1Original[6] = {
    0x0f, 0x84, 0x60, 0x06, 0x00, 0x00
};
inline constexpr uint8_t kSite1Replacement[6] = {
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90
};

// Site 2: replicate_map_markers_for_ui (RVA 0x3113c9..0x311403, 58 bytes)
// Note: ui_marker->visibility ([rsi + 0x34]) is initialized to 2 (Visible/Discovered) at RVA 0x311048.
// Preserves [rsi + 0x30] (typeHash) for all markers while writing [rsi + 0x40] = lastChange
// and keeping [rsi + 0x34] = 2 (or 3 if Checked), so map_key_panel.cpp can render locked
// Fast Travel Points and Ancient Spires with their grayed-out locked state per character.
inline constexpr uint8_t kSite2Signature[66] = {
    0x48, 0x8b, 0x44, 0x24, 0x58,                   // mov rax, [rsp + 0x58]
    0x48, 0x85, 0xc0,                               // test rax, rax
    0x74, 0x15,                                     // je +0x15
    0x48, 0x8b, 0x40, 0x08,                         // mov rax, [rax + 8]
    0x48, 0x89, 0x46, 0x40,                         // mov [rsi + 0x40], rax
    0x48, 0x8b, 0x44, 0x24, 0x58,                   // mov rax, [rsp + 0x58]
    0x0f, 0xb6, 0x08,                               // movzx ecx, byte ptr [rax]
    0x88, 0x4e, 0x34,                               // mov [rsi + 0x34], cl
    0xeb, 0x1b,                                     // jmp +0x1b
    0x48, 0x8b, 0x44, 0x24, 0x70,                   // mov rax, [rsp + 0x70]
    0x48, 0x85, 0xc0,                               // test rax, rax
    0x74, 0x09,                                     // je +9
    0x48, 0x8b, 0x00,                               // mov rax, [rax]
    0x48, 0x89, 0x46, 0x40,                         // mov [rsi + 0x40], rax
    0xeb, 0x08,                                     // jmp +8
    0x48, 0xc7, 0x46, 0x40, 0x00, 0x00, 0x00, 0x00, // mov qword ptr [rsi + 0x40], 0
    0x41, 0xb8, 0x70, 0x00, 0x00, 0x00,             // mov r8d, 0x70
    0x48, 0x8d                                      // lea rdx, ...
};
inline constexpr uint8_t kSite2Original[58] = {
    0x48, 0x8b, 0x44, 0x24, 0x58,
    0x48, 0x85, 0xc0,
    0x74, 0x15,
    0x48, 0x8b, 0x40, 0x08,
    0x48, 0x89, 0x46, 0x40,
    0x48, 0x8b, 0x44, 0x24, 0x58,
    0x0f, 0xb6, 0x08,
    0x88, 0x4e, 0x34,
    0xeb, 0x1b,
    0x48, 0x8b, 0x44, 0x24, 0x70,
    0x48, 0x85, 0xc0,
    0x74, 0x09,
    0x48, 0x8b, 0x00,
    0x48, 0x89, 0x46, 0x40,
    0xeb, 0x08,
    0x48, 0xc7, 0x46, 0x40, 0x00, 0x00, 0x00, 0x00
};
// Previous patch bytes (for seamless live-sync upgrade of an already-patched process)
inline constexpr uint8_t kSite2LegacyV1[58] = {
    0x31, 0xc9,                         // xor ecx, ecx
    0x48, 0x8b, 0x44, 0x24, 0x58,       // mov rax, [rsp + 0x58]
    0x48, 0x85, 0xc0,                   // test rax, rax
    0x74, 0x1d,                         // je .no_vis (+0x29)
    0x48, 0x8b, 0x48, 0x08,             // mov rcx, [rax + 8]
    0x0f, 0xb6, 0x10,                   // movzx edx, byte ptr [rax]
    0x80, 0xfa, 0x03,                   // cmp dl, 3
    0x75, 0x03,                         // jne .check_spire (+0x1b)
    0x88, 0x56, 0x34,                   // mov [rsi + 0x34], dl
    0x84, 0xd2,                         // test dl, dl
    0x75, 0x17,                         // jne .done (+0x36)
    0x38, 0x56, 0x48,                   // cmp [rsi + 0x48], dl
    0x74, 0x12,                         // je .done (+0x36)
    0x89, 0x56, 0x30,                   // mov [rsi + 0x30], edx
    0xeb, 0x0d,                         // jmp .done (+0x36)
    0x48, 0x8b, 0x44, 0x24, 0x70,       // .no_vis: mov rax, [rsp + 0x70]
    0x48, 0x85, 0xc0,                   // test rax, rax
    0x74, 0x03,                         // je .done (+0x36)
    0x48, 0x8b, 0x08,                   // mov rcx, [rax]
    0x48, 0x89, 0x4e, 0x40              // .done: mov [rsi + 0x40], rcx
};
// Also keeps the character's own visibility byte (0 not discovered, 2 discovered, 3 checked) as 0x10 | it at
// [rsi + 0x35], a byte the game never writes (0x10: not discovered; 0xff: a marker without one; anything else, stale
// or zero, means nothing), so undiscovered places can be drawn faded.
inline constexpr uint8_t kSite2Replacement[58] = {
    0x48, 0x8b, 0x44, 0x24, 0x58,       // mov rax, [rsp + 0x58]
    0x48, 0x85, 0xc0,                   // test rax, rax
    0x74, 0x1b,                         // je .no_vis (+0x25)
    0x48, 0x8b, 0x48, 0x08,             // mov rcx, [rax + 8]
    0x48, 0x89, 0x4e, 0x40,             // mov [rsi + 0x40], rcx
    0x0f, 0xb6, 0x08,                   // movzx ecx, byte ptr [rax]
    0x80, 0xf9, 0x03,                   // cmp cl, 3
    0x75, 0x03,                         // jne +3
    0x88, 0x4e, 0x34,                   // mov [rsi + 0x34], cl
    0x80, 0xc9, 0x10,                   // or cl, 0x10
    0x88, 0x4e, 0x35,                   // mov [rsi + 0x35], cl
    0xeb, 0x15,                         // jmp .done (+0x3a)
    0xc6, 0x46, 0x35, 0xff,             // .no_vis: mov byte ptr [rsi + 0x35], 0xff
    0x48, 0x8b, 0x44, 0x24, 0x70,       // mov rax, [rsp + 0x70]
    0x48, 0x85, 0xc0,                   // test rax, rax
    0x74, 0x03,                         // je +3 (rax 0: lastChange 0)
    0x48, 0x8b, 0x00,                   // mov rax, [rax]
    0x48, 0x89, 0x46, 0x40              // mov [rsi + 0x40], rax
};
// The patch before 1.3.13 (no visibility byte), replaced when found in a running game
inline constexpr uint8_t kSite2LegacyV2[58] = {
    0x48, 0x8b, 0x44, 0x24, 0x58,
    0x48, 0x85, 0xc0,
    0x74, 0x15,
    0x48, 0x8b, 0x48, 0x08,
    0x48, 0x89, 0x4e, 0x40,
    0x0f, 0xb6, 0x08,
    0x80, 0xf9, 0x03,
    0x75, 0x03,
    0x88, 0x4e, 0x34,
    0xeb, 0x1b,
    0x48, 0x8b, 0x44, 0x24, 0x70,
    0x48, 0x85, 0xc0,
    0x74, 0x09,
    0x48, 0x8b, 0x00,
    0x48, 0x89, 0x46, 0x40,
    0xeb, 0x08,
    0x48, 0xc7, 0x46, 0x40, 0x00, 0x00, 0x00, 0x00
};

// Site 3: MapMarkerType lookup lore-rumor downgrade (RVA 0xd2001e)
// Original at +0x0e: 0f 44 dd (cmove ebx, ebp — downgrades real POI icon to Question Mark 0x8ba7c122 if referenced by uncompleted lore rumor)
// Patch at +0x0e:    90 90 90 (NOP 3 bytes so real POIs always display their actual icon)
inline constexpr uint8_t kSite3Signature[28] = {
    0xe9, 0x70, 0xff, 0xff, 0xff,       // jmp 0x140d1e4f5
    0x41, 0x38, 0x78, 0xcc,             // cmp byte ptr [r8 - 0x34], dil
    0xbd, 0x22, 0xc1, 0xa7, 0x8b,       // mov ebp, 0x8ba7c122
    0x0f, 0x44, 0xdd,                   // cmove ebx, ebp
    0xe9, 0x64, 0xff, 0xff, 0xff,       // jmp 0x140d1e4fa
    0x48, 0x8b, 0x49, 0x38,             // mov rcx, [rcx + 0x38]
    0x80, 0x7e                          // cmp byte ptr [rsi + ...
};
inline constexpr uint8_t kSite3Original[3] = {
    0x0f, 0x44, 0xdd
};
inline constexpr uint8_t kSite3Replacement[3] = {
    0x90, 0x90, 0x90
};

// Site 4: MapMarkerType lookup quest-rumor downgrade (RVA 0xd1fe88)
// Original at +0x03: 75 c7 (jne — downgrades real POI icon to Quest Question Mark 0xc52e92e5/0xa7cadc08 if referenced by uncompleted quest)
// Patch at +0x03:    90 90 (NOP 2 bytes so real POIs keep their real icon even when targeted by an active quest)
inline constexpr uint8_t kSite4Signature[18] = {
    0x48, 0x85, 0xc0,                   // test rax, rax
    0x75, 0xc7,                         // jne -0x39
    0x44, 0x8b, 0x5e, 0x08,             // mov r11d, [rsi + 8]
    0x45, 0x85, 0xdb,                   // test r11d, r11d
    0x0f, 0x84, 0x0d, 0x01, 0x00, 0x00  // je +0x10d
};
inline constexpr uint8_t kSite4Original[2] = {
    0x75, 0xc7
};
inline constexpr uint8_t kSite4Replacement[2] = {
    0x90, 0x90
};

inline constexpr size_t kNumPatchSites = 4;

inline constexpr PatchSiteSpec kPatchSites[kNumPatchSites] = {
    {
        "replicate_map_markers_for_ui_skip_check",
        kSite1Signature,
        sizeof(kSite1Signature),
        0x0d,
        kSite1Original,
        kSite1Replacement,
        sizeof(kSite1Original)
    },
    {
        "replicate_map_markers_for_ui_visibility_and_spire_lock",
        kSite2Signature,
        sizeof(kSite2Signature),
        0x00,
        kSite2Original,
        kSite2Replacement,
        sizeof(kSite2Original)
    },
    {
        "map_marker_type_lore_rumor_downgrade",
        kSite3Signature,
        sizeof(kSite3Signature),
        0x0e,
        kSite3Original,
        kSite3Replacement,
        sizeof(kSite3Original)
    },
    {
        "map_marker_type_quest_rumor_downgrade",
        kSite4Signature,
        sizeof(kSite4Signature),
        0x03,
        kSite4Original,
        kSite4Replacement,
        sizeof(kSite4Original)
    }
};

enum class SiteMatchState {
    NotFound = 0,
    Unpatched = 1,
    AlreadyPatched = 2
};

struct ResolvedSite {
    uint8_t* signatureBase = nullptr;
    uint8_t* patchAddress = nullptr;
    SiteMatchState state = SiteMatchState::NotFound;
    size_t matchCount = 0;
};

inline bool MatchSiteAt(const uint8_t* candidate, const PatchSiteSpec& spec, SiteMatchState* outState) {
    if (spec.patchOffset > 0 && std::memcmp(candidate, spec.signature, spec.patchOffset) != 0) {
        return false;
    }
    const size_t suffixOffset = spec.patchOffset + spec.patchSize;
    if (suffixOffset < spec.signatureSize) {
        const size_t suffixSize = spec.signatureSize - suffixOffset;
        if (std::memcmp(candidate + suffixOffset, spec.signature + suffixOffset, suffixSize) != 0) {
            return false;
        }
    }
    if (std::memcmp(candidate + spec.patchOffset, spec.originalBytes, spec.patchSize) == 0) {
        *outState = SiteMatchState::Unpatched;
        return true;
    }
    if (spec.patchSize == sizeof(kSite2LegacyV1) &&
        (std::memcmp(candidate + spec.patchOffset, kSite2LegacyV1, sizeof(kSite2LegacyV1)) == 0 ||
         std::memcmp(candidate + spec.patchOffset, kSite2LegacyV2, sizeof(kSite2LegacyV2)) == 0)) {
        *outState = SiteMatchState::Unpatched;
        return true;
    }
    if (std::memcmp(candidate + spec.patchOffset, spec.replacementBytes, spec.patchSize) == 0) {
        *outState = SiteMatchState::AlreadyPatched;
        return true;
    }
    return false;
}

inline ResolvedSite FindPatchSite(uint8_t* buffer, size_t bufferSize, const PatchSiteSpec& spec) {
    ResolvedSite result{};
    if (!buffer || bufferSize < spec.signatureSize) {
        return result;
    }
    const size_t limit = bufferSize - spec.signatureSize;
    for (size_t i = 0; i <= limit; ++i) {
        SiteMatchState st = SiteMatchState::NotFound;
        if (MatchSiteAt(buffer + i, spec, &st)) {
            if (result.matchCount == 0) {
                result.signatureBase = buffer + i;
                result.patchAddress = buffer + i + spec.patchOffset;
                result.state = st;
            }
            ++result.matchCount;
        }
    }
    return result;
}

} // namespace unlock_map_markers
