#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <vector>

#include "scan.h"
#include "greeneye.h"
#include "jumppatch.h"

namespace unlock_map_markers {
namespace {

std::vector<int> ParsePattern(const char* pattern) {
    std::vector<int> bytes;
    for (const char* p = pattern; *p;) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        if (*p == '?') {
            bytes.push_back(-1);
            p += (p[1] == '?') ? 2 : 1;
            continue;
        }
        char hex[3] = {p[0], p[1], 0};
        bytes.push_back(static_cast<int>(std::strtoul(hex, nullptr, 16)));
        p += p[1] ? 2 : 1;
    }
    return bytes;
}

void WriteAbsJump(uint8_t* at, const void* to) {
    at[0] = 0x48; // mov rax, imm64
    at[1] = 0xB8;
    std::memcpy(at + 2, &to, 8);
    at[10] = 0xFF; // jmp rax
    at[11] = 0xE0;
}

} // namespace

bool GameText(uint8_t*& start, size_t& size) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::memcmp(sec->Name, ".text", 5) == 0) {
            start = base + sec->VirtualAddress;
            size = sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

uint8_t* FindUnique(const char* pattern) {
    // Under the Greeneye Loader: the code as it was before the loader's hooks, so a reloaded
    // generation finds the functions the first one hooked.
    if (const GreeneyeHost* host = greeneye::Host()) {
        return host->find(pattern, nullptr);
    }
    uint8_t* text = nullptr;
    size_t size = 0;
    std::vector<int> pat = ParsePattern(pattern);
    if (!GameText(text, size) || pat.empty() || size < pat.size()) return nullptr;
    uint8_t* found = nullptr;
    for (uint8_t* p = text; p + pat.size() <= text + size; ++p) {
        size_t k = 0;
        while (k < pat.size() && (pat[k] < 0 || p[k] == pat[k])) ++k;
        if (k != pat.size()) continue;
        if (found) return nullptr; // not unique
        found = p;
    }
    return found ? found : jumppatch::FindBehindHook(text, size, pat.data(), pat.size());
}

void ReadCode(const void* p, void* out, size_t n) {
    if (const GreeneyeHost* host = greeneye::Host()) {
        host->read(p, out, n);
        return;
    }
    std::memcpy(out, p, n);
}

bool StartsAsExpected(const uint8_t* target, const uint8_t* expected, size_t n) {
    uint8_t now[32]{};
    if (!target || n > sizeof(now)) return false;
    ReadCode(target, now, n);
    if (jumppatch::KindOf(now) != jumppatch::None) return true;
    return std::memcmp(now, expected, n) == 0;
}

void* InstallHook(uint8_t* target, void* detour, size_t length, const uint8_t* code, size_t codeLength) {
    // Under the Greeneye Loader (API 2) the loader owns the hook (hot reload).
    if (const GreeneyeHost* host = greeneye::Host()) {
        return host->hook(target, detour, length, code, codeLength);
    }
    // Another mod's jump is already here: point it at detour and call on through that mod.
    if (jumppatch::KindOf(target) != jumppatch::None) {
        return jumppatch::Chain(target, detour);
    }
    if (length < 12 || length > 32 || codeLength > 48) return nullptr;
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return nullptr;
    const size_t at = code ? codeLength : length;
    std::memcpy(tramp, code ? code : target, at);
    tramp[at] = 0xFF; // jmp [rip+0]; target + length
    tramp[at + 1] = 0x25;
    std::memset(tramp + at + 2, 0, 4);
    uint8_t* back = target + length;
    std::memcpy(tramp + at + 6, &back, 8);
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);

    uint8_t patch[32];
    WriteAbsJump(patch, detour);
    std::memset(patch + 12, 0x90, length - 12); // nop out the rest of the overwritten instructions
    return jumppatch::Write(target, patch, length) ? tramp : nullptr;
}

// The start of the function that contains code (one of its chunks), from the exe's unwind table: chained chunks
// lead back to the function's first chunk.
uint8_t* FunctionStart(const uint8_t* code) {
    DWORD64 image = 0;
    const RUNTIME_FUNCTION* fn = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(code), &image, nullptr);
    for (int guard = 0; fn && guard < 16; ++guard) {
        auto* unwind = reinterpret_cast<const uint8_t*>(image + fn->UnwindInfoAddress);
        if (!(unwind[0] >> 3 & 0x4)) return reinterpret_cast<uint8_t*>(image + fn->BeginAddress); // UNW_FLAG_CHAININFO
        const uint8_t codes = unwind[2];
        const size_t at = 4 + ((codes + 1) & ~1u) * 2; // the chained RUNTIME_FUNCTION after the codes
        fn = reinterpret_cast<const RUNTIME_FUNCTION*>(unwind + at);
    }
    return nullptr;
}

// Every "call target" (E8 rel32) in the game's code as it was before any hook.
int CallsTo(const uint8_t* target, uint8_t** out, int max) {
    uint8_t* text = nullptr;
    size_t size = 0;
    if (!GameText(text, size) || size < 5) return 0;
    std::vector<uint8_t> code(size);
    ReadCode(text, code.data(), size);
    int n = 0;
    for (size_t i = 0; i + 5 <= size && n < max; ++i) {
        if (code[i] != 0xE8) continue;
        int32_t d;
        std::memcpy(&d, &code[i + 1], 4);
        if (text + i + 5 + d == target) out[n++] = text + i;
    }
    return n;
}

// An ECS system's function by its name: .rdata holds {const char* name, uint64 length, function} for each.
uint8_t* SystemFunction(const char* name) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    uint8_t* text = nullptr;
    size_t textSize = 0;
    if (!GameText(text, textSize)) return nullptr;
    const size_t n = std::strlen(name), image = nt->OptionalHeader.SizeOfImage;
    uint8_t* found = nullptr;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::memcmp(sec->Name, ".rdata", 6) != 0) continue;
        for (size_t off = 0; off + 0x18 <= sec->Misc.VirtualSize; off += 8) {
            uint8_t* p = base + sec->VirtualAddress + off;
            uint64_t length;
            std::memcpy(&length, p + 8, 8);
            if (length != n) continue;
            const char* str;
            std::memcpy(&str, p, 8);
            const auto at = reinterpret_cast<uintptr_t>(str), from = reinterpret_cast<uintptr_t>(base);
            if (at < from || at - from > image - n || std::memcmp(str, name, n) != 0) continue;
            uint8_t* fn;
            std::memcpy(&fn, p + 0x10, 8);
            if (fn < text || fn >= text + textSize) continue;
            if (found && found != fn) return nullptr;
            found = fn;
        }
    }
    return found;
}

void* RedirectSystemCalls(const uint8_t* target, const char* const* names, int count, void* detour, int& calls,
                          int& systems) {
    calls = systems = 0;
    std::vector<uint8_t*> fns;
    for (int i = 0; i < count; ++i) {
        if (uint8_t* fn = SystemFunction(names[i])) fns.push_back(fn);
    }
    systems = static_cast<int>(fns.size());
    std::vector<uint8_t*> all(4096);
    const int found = CallsTo(target, all.data(), static_cast<int>(all.size()));
    int redirected = 0;
    void* original = nullptr;
    for (int i = 0; i < found; ++i) {
        uint8_t* fn = FunctionStart(all[i]);
        bool mine = false;
        for (uint8_t* f : fns) mine |= f == fn;
        if (!mine) continue;
        ++calls;
        void* previous = nullptr;
        if (const GreeneyeHost* host = greeneye::Host()) previous = host->redirectCall(all[i], detour);
        else previous = jumppatch::RedirectCall(all[i], detour);
        if (previous && !original) original = previous;
        redirected += previous != nullptr;
    }
    return calls > 0 && redirected == calls ? original : nullptr;
}

} // namespace unlock_map_markers
