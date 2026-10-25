#pragma once

#include <cstddef>
#include <cstdint>

// Scanning and hooking the game the way every Greeneye mod does (EnshroudedMods/common/README.md):
// under the Greeneye Loader (API 2) through the loader, which owns the hooks and sees the code as
// it was before them; otherwise by chaining onto any hook jump another mod already wrote.
namespace unlock_map_markers {

// The game's .text section.
bool GameText(uint8_t*& start, size_t& size);

// The one match of pattern ("48 8B ?? 05", ?? = any byte) in the game's code, or nullptr if it is
// missing or occurs more than once. A function another mod hooked first is still found, by the
// bytes behind its jump.
uint8_t* FindUnique(const char* pattern);

// The game's bytes at p as they were before the loader's hooks (as they are now standalone).
void ReadCode(const void* p, void* out, size_t n);

// True if the function at target starts with expected (n bytes), or with another mod's hook jump,
// which InstallHook chains onto.
bool StartsAsExpected(const uint8_t* target, const uint8_t* expected, size_t n);

// Redirects the function at target to detour. length (12..32) is the whole instructions the jump
// replaces; code, when given, runs instead of them in the trampoline (for prologues with relative
// jumps). Returns what the detour calls on, or nullptr. Another mod's hook jump at target is
// chained, never overwritten.
void* InstallHook(uint8_t* target, void* detour, size_t length, const uint8_t* code = nullptr, size_t codeLength = 0);

// Redirects the calls to target made inside the named ECS systems (their names in .rdata) to detour, leaving the
// function itself and every other caller alone. Returns what the detour calls on (target, or the hook already
// there), or nullptr if any call could not be redirected.
void* RedirectSystemCalls(const uint8_t* target, const char* const* names, int count, void* detour, int& calls,
                          int& systems);

} // namespace unlock_map_markers
