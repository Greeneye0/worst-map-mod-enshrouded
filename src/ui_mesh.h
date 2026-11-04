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
// Build 1273043: verified against native quad/rotated-image emission in Ghidra. RVAs of the 2026-11-02 build (the three
// functions are byte for byte the old ones there).
// No hooks or retained resources: use the native per-frame allocator and batch.
// The three game functions, found by signature (tools/patchcheck/gensig.py, at least 64 bytes with struct offsets
// literal): a patch that only moves code needs no update, and a layout change turns smooth drawing off.
inline uint8_t* g_allocate{},*g_submit{},*g_set{};
inline bool Ready(){
    static const bool valid=[](){
        auto host=greeneye::Host();
        if(!host||!host->find)return false;
        g_allocate=host->find("48 8B C4 53 56 41 56 48 81 EC C0 00 00 00 4D 8B F1 48 8B DA 48 8B F1 49 81 F8 00 00 01 00 0F 87 ?? ?? ?? ?? 49 81 F9 00 00 01 00 0F 87 ?? ?? ?? ??",nullptr);
        g_submit=host->find("40 53 48 83 EC 20 B8 FF FF 00 00 4C 8B C2 48 8B D9 66 39 81 2A 52 03 00 0F 84 ?? ?? ?? ?? 66 83 B9 28 52 03 00 00 0F 84 ?? ?? ?? ?? 80 B9 D2 52 03 00 00",nullptr);
        g_set=host->find("85 D2 75 ?? 48 8B 11 8B 92 E0 53 03 00 39 91 1C 52 03 00 74 ?? 89 91 1C 52 03 00 C6 81 D2 52 03 00 00 44 38 81 2E 52 03 00 74 ??",nullptr);
        return g_allocate&&g_submit&&g_set;
    }();return valid;
}
inline bool Draw(void* widget,const smooth_highlight::Batch& batch){
    if(batch.vertices.empty()||batch.indices.empty()||batch.vertices.size()>60000||batch.indices.size()>60000||!Ready())return false;
    auto handle=Read<void*>(widget,8);if(!handle)return false;
    auto state=Read<void*>(handle,0x50);if(!state)return false;
    state=Read<void*>(state,0x18);if(!state||Read<uint32_t>(state,8)!=4)return false;
    auto renderer=Read<void*>(state,0x6ce0);if(!renderer)return false;
    auto set=reinterpret_cast<void(*)(void*,uint32_t,uint8_t)>(g_set);
    auto allocate=reinterpret_cast<bool(*)(Allocation*,void*,uint64_t,uint64_t)>(g_allocate);
    auto submit=reinterpret_cast<void(*)(void*,const Allocation*)>(g_submit);
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
