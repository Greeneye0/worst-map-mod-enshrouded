#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
namespace ore_upload {
// FUN_140fc6920: ring head + live bytes determine the next aligned range.
// Do not advance to another ring; leave substantial space for later game passes.
inline bool HasRoom(uint64_t capacity,uint64_t head,uint64_t live,
                    uint64_t bytes,uint64_t alignment,uint64_t reserve){
 if(!capacity||head>=capacity||live>capacity||!bytes||!alignment||
    (alignment&(alignment-1))||alignment>capacity)return false;
 const auto free=capacity-live;
 const auto offset=head>=free?head-free:head+live;
 const auto padding=(alignment-offset%alignment)%alignment;
 if(padding>capacity-offset)return false;
 const auto aligned=offset+padding;
 const auto costPadding=bytes>capacity-aligned?capacity-offset:padding;
 return costPadding<=free&&reserve<=free-costPadding&&
        bytes<=free-costPadding-reserve;
}
// Simulate the actual batches, including ring-wrap padding, before emitting
// any commands. A single aggregate allocation would charge an excessive gap.
inline bool HasBatchRoom(uint64_t capacity,uint64_t head,uint64_t live,
 uint64_t triangles,uint64_t alignment,uint64_t reserve){
 while(triangles){
  auto n=std::min<uint64_t>(4096,triangles),bytes=n*96;
  if(!HasRoom(capacity,head,live,bytes,alignment,reserve))return false;
  auto offset=(head+live)%capacity;
  auto padding=(alignment-offset%alignment)%alignment;
  auto cost=bytes>capacity-offset-padding?capacity-offset:padding;
  live+=cost+bytes;triangles-=n;
 }
 return HasRoom(capacity,head,live,65536,alignment,reserve);
}
// Verified against FUN_140148980: allocation at rsp+40, returned pointer at rsp+60.
struct NativeRange {
 uint64_t context,handle,offset,bytes;
 float* mapped;
 uint64_t remaining;
};
static_assert(sizeof(NativeRange)==48);
static_assert(offsetof(NativeRange,mapped)==0x20);
static_assert(offsetof(NativeRange,offset)==0x10);
static_assert(offsetof(NativeRange,bytes)==0x18);
inline float* Mapped(const NativeRange& r){return r.context?r.mapped:nullptr;}

// Preserve order and whole triangles, bounded by native allocator chunk size.
// No bind/draw command is emitted for an allocation that failed.
template<class Allocate,class Fill,class Emit>
size_t Batches(size_t total,uint64_t chunkBytes,Allocate allocate,Fill fill,Emit emit){
 const size_t limit=size_t(std::min<uint64_t>(4096,chunkBytes/(3*32)));
 if(!limit)return 0;
 size_t done=0;
 while(done<total){
  const size_t n=std::min(limit,total-done);
  auto* mapped=allocate(n);if(!mapped)break;
  fill(mapped,done,n);emit(n);done+=n;
 }
 return done;
}
}
