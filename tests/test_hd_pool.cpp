#include "../src/hd_pool_policy.h"
#include <array>
#include <cassert>
#include <cstdio>
using namespace hd_pool_policy;
int main() {
    std::array<Slot,kSlots> slots{};
    for(size_t i=0;i<kSlots;++i){
        const int v=Victim(slots.data(),100,3);assert(v==int(i));
        slots[v]={0,uint64_t(i),100,0,false,true};
        assert(Find(slots.data(),i)==v);
    }
    assert(Victim(slots.data(),100000,3)==-1); // outstanding uploads never overwritten
    for(size_t i=0;i<kSlots;++i)slots[i]={uint64_t(i),0,100,100,true,false};
    assert(Victim(slots.data(),100,3)==-1);
    assert(Victim(slots.data(),105,3)==-1);
    assert(Victim(slots.data(),106,3)==0);
    assert(Victim(slots.data(),99,3)==-1); // counter rollback must fail closed
    slots[0].lastUse=106;assert(Victim(slots.data(),106,3)==1);
    // Many more distinct contents than the old loader's 128-image budget;
    // slots and memory stay bounded, pinned UI contents stay untouched.
    slots={};uint64_t replacements=0;
    for(uint64_t frame=1;frame<10000;++frame){
        for(int j=0;j<4;++j){
            const uint64_t key=frame*4+j;
            int v=Victim(slots.data(),frame,8);assert(v>=0);
            if(slots[v].valid)++replacements;
            slots[v]={key,0,frame,frame,true,false};
            assert(Find(slots.data(),key)==v);
        }
        for(const auto& slot:slots)if(slot.valid&&frame-slot.lastUse<=10)assert(!Retired(slot,frame,8));
    }
    assert(replacements>39000);
    static_assert(kSlots*kBytes==24*1024*1024);
    std::puts("PASS: bounded 24 MiB pool, 39,900+ reuses, pending and in-flight guards, rollback, LRU and key zero");
}
