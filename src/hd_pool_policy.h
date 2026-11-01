// Pure scheduling policy shared with the offline stress test.
#pragma once
#include <cstdint>
#include <cstddef>
namespace hd_pool_policy {
constexpr size_t kSlots = 96;
constexpr uint32_t kPixels = 256, kBytes = kPixels * kPixels * 4;
constexpr uint32_t kUploadsPerFrame = 4;
struct Slot {
    uint64_t key{}, pending{}, lastUse{}, readyFrame{};
    bool valid{}, queued{};
};
inline bool Retired(const Slot& s, uint64_t frame, uint32_t framesInFlight) {
    // The native uploader provides GPU synchronization. This guard additionally
    // protects UI commands built on the CPU before they reach the renderer.
    return !s.queued && (!s.valid || (frame > s.lastUse && frame - s.lastUse > framesInFlight + 2));
}
inline int Find(const Slot* slots, uint64_t key) {
    for (size_t i=0;i<kSlots;++i)
        if ((slots[i].valid && slots[i].key==key) || (slots[i].queued && slots[i].pending==key)) return int(i);
    return -1;
}
inline int Victim(const Slot* slots, uint64_t frame, uint32_t framesInFlight) {
    int best=-1;
    for (size_t i=0;i<kSlots;++i) if(Retired(slots[i],frame,framesInFlight)) {
        if(!slots[i].valid)return int(i);
        if(best<0 || slots[i].lastUse<slots[best].lastUse)best=int(i);
    }
    return best;
}
}
