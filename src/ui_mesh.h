#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "greeneye.h"
#include "smooth_highlight.h"
#include <cstring>
namespace ui_mesh {
template<class T>T Read(void* p,size_t offset){T v;memcpy(&v,static_cast<uint8_t*>(p)+offset,sizeof(v));return v;}
struct Allocation {uint16_t buffer,count,first,pad; smooth_highlight::Vertex* vertices;uint16_t* indices;uint16_t base;uint8_t padding[6];};
static_assert(sizeof(Allocation)==32);
// Build 1273043: verified against native quad/rotated-image emission in Ghidra.
// No hooks or retained resources: use the native per-frame allocator and batch.
inline bool Ready(){
    static const bool valid=[](){
        auto base=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
        struct Check{size_t rva;uint8_t bytes[24];};
        const Check checks[]={
            {0x11833e0,{0x38,0x03,0,0,0x49,0x81,0xf9,0,0,1,0,0x0f,0x87,0x2b,3,0,0,0x48,0x8b,0x8a,0xc0,0x52,3,0}},
            {0x118c8e0,{0xb9,0x28,0x52,3,0,0,0x0f,0x84,0xe0,0,0,0,0x80,0xb9,0xd2,0x52,3,0,0,0x74,0x48,0x4c,0x8b,0x49}},
            {0x11b3020,{0,0,0x44,0x38,0x81,0x2e,0x52,3,0,0x74,0x0e,0x44,0x88,0x81,0x2e,0x52,3,0,0xc6,0x81,0xd2,0x52,3,0}}};
        for(auto& c:checks){uint8_t bytes[24];auto host=greeneye::Host();if(host)host->read(base+c.rva+32,bytes,24);else memcpy(bytes,base+c.rva+32,24);if(memcmp(bytes,c.bytes,24))return false;}return true;
    }();return valid;
}
inline bool Draw(void* widget,const smooth_highlight::Batch& batch){
    if(batch.vertices.empty()||batch.indices.empty()||batch.vertices.size()>60000||batch.indices.size()>60000||!Ready())return false;
    auto handle=Read<void*>(widget,8);if(!handle)return false;
    auto state=Read<void*>(handle,0x50);if(!state)return false;
    state=Read<void*>(state,0x18);if(!state||Read<uint32_t>(state,8)!=4)return false;
    auto renderer=Read<void*>(state,0x6ce0);if(!renderer)return false;
    auto base=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto set=reinterpret_cast<void(*)(void*,uint32_t,uint8_t)>(base+0x11b3020);
    auto allocate=reinterpret_cast<bool(*)(Allocation*,void*,uint64_t,uint64_t)>(base+0x11833e0);
    auto submit=reinterpret_cast<void(*)(void*,const Allocation*)>(base+0x118c8e0);
    set(renderer,0,0);Allocation allocation{};
    if(!allocate(&allocation,renderer,batch.vertices.size(),batch.indices.size())||!allocation.vertices||!allocation.indices)return false;
    if(uint64_t(allocation.base)+batch.vertices.size()>65536)return false;
    float opacity=1;auto count=Read<uint64_t>(renderer,0xc0);
    if(count){auto stack=Read<float*>(renderer,0xb8);if(stack)opacity=std::clamp(stack[count-1],0.f,1.f);}
    for(size_t i=0;i<batch.vertices.size();++i){auto v=batch.vertices[i];v.colour=(v.colour&0xffffff)|(uint32_t((v.colour>>24)*opacity)<<24);allocation.vertices[i]=v;}
    for(size_t i=0;i<batch.indices.size();++i)allocation.indices[i]=uint16_t(allocation.base+batch.indices[i]);
    submit(renderer,&allocation);return true;
}
}
