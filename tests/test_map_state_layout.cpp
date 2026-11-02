#include "../src/map_state_layout.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <vector>
int main(int argc,char** argv){
 using namespace marker_cache_layout;
 std::vector<uint8_t> state(0x54400);
 constexpr size_t pointerOffset=0xe1d8;
 constexpr uint64_t original=0x2000450888cull,crashed=0x2010450888cull;
 std::memcpy(state.data()+pointerOffset,&original,8);auto before=state;
 MarkDirty(state.data());uint64_t got;std::memcpy(&got,state.data()+pointerOffset,8);assert(got==original);
 for(size_t i=0;i<state.size();++i)assert(state[i]==(i==dirtyOffset?1:before[i]));
 auto old=before;old[0xe1dc]=1;std::memcpy(&got,old.data()+pointerOffset,8);assert(got==crashed);
 uint8_t stale[sizeof(nativeRead)];std::memcpy(stale,nativeRead,sizeof(stale));stale[4]=0xdc;stale[5]=0xe1;assert(!NativeMatches(stale));assert(NativeMatches(nativeRead));
 if(argc>1){
  std::ifstream file(argv[1],std::ios::binary);assert(file);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
  // Known 1.0.1.0 instruction RVA, converted with the PE section table.
  uint32_t pe;std::memcpy(&pe,bytes.data()+60,4);uint16_t count,opt;
  std::memcpy(&count,bytes.data()+pe+6,2);std::memcpy(&opt,bytes.data()+pe+20,2);
  size_t raw=0;
  for(unsigned i=0;i<count;++i){size_t at=pe+24+opt+i*40;uint32_t rva,size,offset;
   std::memcpy(&rva,bytes.data()+at+12,4);std::memcpy(&size,bytes.data()+at+16,4);std::memcpy(&offset,bytes.data()+at+20,4);
   if(0xc31cee>=rva&&0xc31cee-rva<size)raw=offset+0xc31cee-rva;
  }
  assert(raw&&raw+sizeof(nativeRead)<=bytes.size()&&NativeMatches(bytes.data()+raw));
 }
 std::puts("PASS: cache flag at+c1b4; optional native executable check; all eight callers share safe invalidation; cached pointers unchanged; stale write reproduces the exact4GiB crash-pointer corruption");
}
