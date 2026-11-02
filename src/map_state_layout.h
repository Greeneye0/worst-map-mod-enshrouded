#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
namespace marker_cache_layout {
// Build1283531: map filter menu reads its marker-cache dirty flag at+c1b4.
// The old+e1dc now overlaps a cached type pointer: writing 1 there can add4GiB.
inline constexpr size_t dirtyOffset=0xc1b4;
inline constexpr size_t nativeReadOffset=0xb1e;
inline constexpr uint8_t nativeRead[]{0x41,0x0f,0xb6,0x9f,0xb4,0xc1,0,0};
inline void MarkDirty(uint8_t* state){if(state)state[dirtyOffset]=1;}
inline bool NativeMatches(const uint8_t* bytes){return std::memcmp(bytes,nativeRead,sizeof(nativeRead))==0;}
}
