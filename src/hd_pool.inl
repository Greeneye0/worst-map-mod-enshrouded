// Included in map_key_panel.cpp's private namespace. Never uses MakeUiImage or
// the game's screenshot/UI texture heap. Native evidence: evidence/gpu-pool.
namespace hdpool {
namespace native {
#include "hd_pool_native.h"
}
using hd_pool_policy::kSlots;
using hd_pool_policy::kPixels;
using hd_pool_policy::kBytes;
using hd_pool_policy::Slot;
template<class T> T& At(void* p, size_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p)+offset); }
uint8_t* g_base{};
template<class F> F Fn(const native::Entry& e) { return reinterpret_cast<F>(g_base+e.rva); }
struct Image { uint64_t context{}, id{}; };
struct GpuSlot { Image image{}; void* registry{}; uint32_t ui{}; };
// POD only: the loader retains these native resources across a hot reload.
struct State {
    void* root{}; void* scene{}; void* graphics{};
    uint32_t ticket{}, error{}, framesInFlight{};
    uint64_t frame{}, lastTick{}, allocations{}, releases{}, uploads{}, reused{}, hits{}, deferred{};
    Slot slots[kSlots]{};
    GpuSlot gpu[kSlots]{};
};
State* g_state{};
std::mutex g_lock;
std::array<std::vector<uint32_t>, kSlots> g_pixels;
bool g_enabled{};
std::atomic<bool> g_releaseRequested{false};
std::atomic<bool> g_testHalls{false};
uint64_t g_viewsFreedLast=0;
uint64_t g_budgetFirst=UINT64_MAX, g_budgetLast=UINT64_MAX;
std::atomic<bool> g_wanted{false};
// RDX is forwarded by the native scene wrapper to the model update. Ghidra
// under-infers the wrapper as one-argument; dropping RDX hides skinned clothing.
using TickFn=void(*)(void*,const void*);
using DestroyFn=void(*)(void**);
using RootDestroyFn=uint64_t(*)(void*);
using RegistryDestroyFn=void(*)(void*);
RegistryDestroyFn g_registryDestroy{};
TickFn g_tick{}; DestroyFn g_destroy{}; RootDestroyFn g_rootDestroy{};

void Release() { // only before native scene/root destruction, on its own thread
    auto& s=*g_state;
    void* context=s.root?At<void*>(s.root,0x128):nullptr;
    const uint64_t before=context?At<uint64_t>(context,0x410):0;
    for(auto& g:s.gpu) {
        if(g.ui)Fn<void(*)(void*,uint32_t)>(native::unregisterImage)(g.registry,g.ui);
        // A ticket owns the backing allocation; ImageView has a separate native
        // context slot. Mirror 0x0f3970: free the view BEFORE its backing ticket.
        if(g.image.context)Fn<void(*)(void*,Image*)>(native::freeImage)(reinterpret_cast<void*>(g.image.context),&g.image);
        g={};
    }
    const uint64_t after=context?At<uint64_t>(context,0x410):0;
    g_viewsFreedLast=before>=after?before-after:0;
    if(s.ticket && s.root) {
        Fn<void(*)(void*,uint32_t*)>(native::release)(At<void*>(s.root,0x160),&s.ticket);
        ++s.releases;
    }
    s.root=nullptr;s.scene=nullptr;s.graphics=nullptr;s.frame=0;s.lastTick=0;s.error=0;
    for(size_t i=0;i<kSlots;++i){s.slots[i]={};g_pixels[i].clear();}
    g_wanted=false;
    if(g_log)g_log("Hollow Halls GPU pool: released (allocations=%llu releases=%llu)",s.allocations,s.releases);
}
bool Allocate(void* scene) {
    auto& s=*g_state;
    s.root=At<void*>(scene,0);s.scene=scene;
    void* context=At<void*>(s.root,0x128);
    s.graphics=At<void*>(context,8);
    s.framesInFlight=At<uint32_t>(s.graphics,0x440);
    if(!s.framesInFlight || s.framesInFlight>8){s.error=1;return false;}
    alignas(16) uint8_t scope[0x180]{},plan[0x20]{},desc[0x30]{},result[16]{};
    Fn<void*(*)(void*,void*,uint8_t)>(native::scope)(scope,context,0);
    Fn<void*(*)(void*,void*,uint32_t,uint64_t,uint8_t)>(native::plan)(plan,scope,At<uint32_t>(s.root,0x270),0,1);
    At<uint32_t>(desc,4)=kPixels;At<uint32_t>(desc,8)=kPixels;At<uint32_t>(desc,12)=1;
    At<uint32_t>(desc,16)=0x1010001;desc[20]=1;desc[21]=0x25;desc[22]=0x46; // sampled + transfer destination + native persistent usage
    for(auto& g:s.gpu)Fn<void(*)(void*,Image*,const void*)>(native::image)(plan,&g.image,desc);
    Fn<void*(*)(void*,void*,uint32_t*,void*)>(native::commit)(scope,result,&s.ticket,At<void*>(s.root,0x160));
    Fn<void(*)(void*)>(native::end)(scope);
    s.error=result[0];
    if(!s.error && !s.ticket)s.error=2;
    for(const auto& g:s.gpu)if(!g.image.context)s.error=2;
    if(s.error){
        const auto error=s.error;Release();s.error=error;
        if(g_log)g_log("Hollow Halls GPU pool: allocation failed (%u); plain maps",error);
        return false;
    }
    ++s.allocations;
    if(g_log)g_log("Hollow Halls GPU pool: allocated %u reusable 256x256 images (%u bytes), %u frames in flight",
        unsigned(kSlots),unsigned(kSlots*kBytes),s.framesInFlight);
    return true;
}
bool Upload(size_t index) {
    auto& s=*g_state;auto& slot=s.slots[index];
    void* context=At<void*>(s.root,0x128);
    void* pool=At<void*>(s.graphics,0x5d0);
    if(!pool || At<uint64_t>(pool,0x10)<kBytes || g_pixels[index].size()!=size_t(kPixels)*kPixels)return false;
    alignas(16) uint8_t result[0x80]{};uint64_t access=0x400;
    Fn<void*(*)(void*,void*,const Image*,uint8_t,const uint64_t*)>(native::upload)
        (result,context,&s.gpu[index].image,7,&access);
    auto* mapped=At<uint8_t*>(result,0);
    const uint64_t bytes=At<uint64_t>(result,8), pitch=At<uint64_t>(result,0x10);
    if(!mapped || pitch<kPixels*4 || pitch>kPixels*4+65536 || bytes<(kPixels-1)*pitch+kPixels*4)return false;
    for(uint32_t y=0;y<kPixels;++y)std::memcpy(mapped+y*pitch,g_pixels[index].data()+y*kPixels,kPixels*4);
    if(slot.valid)++s.reused;
    slot.key=slot.pending;slot.pending=0;slot.queued=false;slot.valid=true;slot.readyFrame=s.frame;
    slot.lastUse=s.frame;g_pixels[index].clear();++s.uploads;
    return true;
}
void Tick(void* scene) {
    auto& s=*g_state;
    if(g_releaseRequested && s.ticket && s.scene==scene){
        const uint64_t frame=At<uint64_t>(s.graphics,0x568);
        bool idle=true;
        for(const auto& slot:s.slots)if(!hd_pool_policy::Retired(slot,frame,s.framesInFlight))idle=false;
        if(idle){Release();g_releaseRequested=false;return;}
    }
    if(s.error || !g_wanted || g_releaseRequested)return;
    if(!s.ticket && !Allocate(scene))return;
    if(s.scene!=scene)return;
    s.frame=At<uint64_t>(s.graphics,0x568);
    if(s.lastTick==s.frame)return;
    s.lastTick=s.frame;
    uint32_t uploaded=0;
    for(size_t i=0;i<kSlots && uploaded<hd_pool_policy::kUploadsPerFrame;++i)if(s.slots[i].queued) {
        if(!Upload(i)) {
            s.error=3;
            if(g_log)g_log("Hollow Halls GPU pool: upload failed; plain maps, no retry");
            return;
        }
        ++uploaded;
    }
    static uint64_t logged=0;
    if(s.uploads/100!=logged){
        logged=s.uploads/100;
        if(g_log)g_log("Hollow Halls GPU pool: uploads=%llu reused=%llu hits=%llu deferred=%llu allocations=%llu releases=%llu",
            s.uploads,s.reused,s.hits,s.deferred,s.allocations,s.releases);
    }
}
void TickGuard(void* scene) {
    __try {Tick(scene);} __except(EXCEPTION_EXECUTE_HANDLER) {
        g_state->error=GetExceptionCode();
        if(g_log)g_log("Hollow Halls GPU pool: native exception %08x; plain maps",g_state->error);
    }
}
void TickHook(void* scene,const void* description) {
    greeneye::Lease lease;
    if(lease && g_enabled){std::lock_guard lock(g_lock);TickGuard(scene);}
    g_tick(scene,description);
}
void DestroyHook(void** scene) {
    greeneye::Lease lease;
    if(lease && g_enabled){std::lock_guard lock(g_lock);if(*scene==g_state->scene)Release();}
    g_destroy(scene);
}
uint64_t RootDestroyHook(void* root) {
    greeneye::Lease lease;
    if(lease && g_enabled){std::lock_guard lock(g_lock);if(root==g_state->root)Release();}
    return g_rootDestroy(root);
}
void RegistryDestroyHook(void* registry) {
    greeneye::Lease lease;
    if(lease && g_enabled){
        std::lock_guard lock(g_lock);
        // The native destructor frees its table. Do not unregister later through
        // this pointer; surviving images can register in the replacement table.
        for(auto& g:g_state->gpu)if(g.registry==registry){g.registry=nullptr;g.ui=0;}
    }
    g_registryDestroy(registry);
}
void* Registry(void* widget) {
    auto* header=At<uint8_t*>(widget,8);
    auto* owner=header?At<uint8_t*>(header,0x50):nullptr;
    auto* layer=owner?At<uint8_t*>(owner,0x18):nullptr;
    if(!layer || At<uint32_t>(layer,8)!=4)return nullptr;
    auto* renderer=At<uint8_t*>(layer,0x6ce0);
    return renderer?At<void*>(renderer,0):nullptr;
}
// pixels null: only an image already resident under the key (keeps it from being reused), nothing queued
uint32_t Request(void* widget,const uint32_t* pixels,uint64_t key) {
    if(!g_enabled || !key)return 0;
    std::lock_guard lock(g_lock);auto& s=*g_state;
    if(s.error || g_releaseRequested)return 0;
#ifdef UMM_DEV_BRIDGE
    // Read-only measurement during actual UI construction (where g_uiCtx is valid).
    if(g_uiCtx){
        auto* ctx=static_cast<uint8_t*>(g_uiCtx());
        auto* a=ctx?At<uint8_t*>(ctx,0x10):nullptr;
        auto* b=a?At<uint8_t*>(a,0x58):nullptr;
        auto* loader=b?At<uint8_t*>(b,0xe98):nullptr;
        if(loader){g_budgetLast=At<uint64_t>(loader,0x36b8);if(g_budgetFirst==UINT64_MAX)g_budgetFirst=g_budgetLast;}
    }
#endif
    g_wanted=true;
    if(!s.ticket)return 0;
    void* registry=Registry(widget);
    if(!registry)return 0;
    const uint64_t frame=At<uint64_t>(s.graphics,0x568);
    int index=hd_pool_policy::Find(s.slots,key);
    if(index>=0){
        auto& slot=s.slots[index];slot.lastUse=frame;
        if(slot.queued || !slot.valid || frame<=slot.readyFrame)return 0;
        auto& g=s.gpu[index];
        if(g.registry && g.registry!=registry)return 0; // never use a handle in a different registry
        if(!g.ui){
            g.ui=Fn<uint32_t(*)(void*,const Image*,uint64_t,uint64_t,uint8_t)>(native::registerImage)
                (registry,&g.image,0,(uint64_t(kPixels)<<32)|kPixels,1);
            if(!g.ui){s.error=4;return 0;}
            g.registry=registry;
        }
        if(At<uint32_t>(registry,0x35304))Fn<void(*)(void*,uint32_t,const Image*)>(native::bindImage)
            (At<void*>(registry,0x10),At<uint32_t>(registry,0x35300)+(g.ui&0xfffff),&g.image);
        ++s.hits;return g.ui;
    }
    if(!pixels)return 0;
    index=hd_pool_policy::Victim(s.slots,frame,s.framesInFlight);
    if(index<0){++s.deferred;return 0;}
    auto& slot=s.slots[index];slot.pending=key;slot.queued=true;slot.lastUse=frame;
    g_pixels[index].assign(pixels,pixels+size_t(kPixels)*kPixels);
    return 0;
}
bool Available() { std::lock_guard lock(g_lock); return g_enabled && g_state && !g_state->error; }

#ifdef UMM_DEV_BRIDGE
// Explicit developer probe: bounded to 60 seconds, only draws on an open map.
// Uses the same allocation, upload, registration and draw path as real HD tiles.
std::atomic<uint64_t> g_testUntil{0};
void DrawTest(void* widget, const float* rootRect) {
    if(GetTickCount64()>=g_testUntil.load() || g_testHalls)return;
    const uint64_t generation=GetTickCount64()/750;
    static std::vector<uint32_t> pixels(size_t(kPixels)*kPixels);
    const uint64_t colour=MakePackedColor(0xffffffffu,1.0f,1.0f);
    for(uint32_t tile=0;tile<32;++tile){
        for(uint32_t y=0;y<kPixels;++y)for(uint32_t x=0;x<kPixels;++x)
            pixels[y*kPixels+x]=0xff000000u | ((x+generation*13)&255) |
                (((y+tile*17)&255)<<8) | (((tile*31+generation*23)&255)<<16);
        uint64_t key=0xf100000000000000ull | (generation<<8) | tile;
        uint32_t id=Request(widget,pixels.data(),key);
        if(id){float rect[4]={rootRect[0]+160.0f+(tile%8)*64,rootRect[1]+170.0f+(tile/8)*64,64,64};
            g_drawImage(widget,rect,&id,&colour);}
    }
}
int Control(const char* action,const char* target,char* out,uint32_t cap) {
    std::lock_guard lock(g_lock);
    if(std::strcmp(action,"hdtest")==0){
        if(std::strcmp(target,"start")==0 || std::strcmp(target,"halls")==0){
            g_testHalls=std::strcmp(target,"halls")==0;g_testUntil=GetTickCount64()+60000;
        } else if(std::strcmp(target,"stop")==0)g_testUntil=0;
        else if(std::strcmp(target,"release")==0){
            g_testUntil=0;
            if(g_state && g_state->ticket){
                g_releaseRequested=true;
                for(size_t i=0;i<kSlots;++i){g_state->slots[i].queued=false;g_state->slots[i].pending=0;g_pixels[i].clear();}
            }
        } else return -1;
    }
    const State empty{};const auto& s=g_state?*g_state:empty;
    unsigned resident=0,queued=0,registered=0;
    for(size_t i=0;i<kSlots;++i){resident+=s.slots[i].valid;queued+=s.slots[i].queued;registered+=s.gpu[i].ui!=0;}
    int n=std::snprintf(out,cap,
        "{\"enabled\":%s,\"error\":%u,\"ticket\":%u,\"capacity\":%u,\"bytes\":%u,\"resident\":%u,\"queued\":%u,\"registered\":%u,\"frame\":%llu,\"framesInFlight\":%u,\"allocations\":%llu,\"releases\":%llu,\"uploads\":%llu,\"reused\":%llu,\"hits\":%llu,\"deferred\":%llu,\"viewsFreedLast\":%llu,\"budgetFirst\":%llu,\"budgetLast\":%llu,\"testActive\":%s}",
        g_enabled?"true":"false",s.error,s.ticket,unsigned(kSlots),unsigned(kSlots*kBytes),resident,queued,registered,s.frame,s.framesInFlight,
        s.allocations,s.releases,s.uploads,s.reused,s.hits,s.deferred,g_viewsFreedLast,g_budgetFirst,g_budgetLast,GetTickCount64()<g_testUntil?"true":"false");
    return n>0 && uint32_t(n)<cap?1:-4;
}
#endif
bool Init() {
#ifdef UMM_DISABLE_HD_POOL
    if(g_log)g_log("Hollow Halls GPU pool: disabled for character-preview isolation; no pool hooks installed");
    return false;
#else
    g_base=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base+reinterpret_cast<IMAGE_DOS_HEADER*>(g_base)->e_lfanew);
    // The game build of 2026-11-03 (hd_pool_native.h)
    if(nt->FileHeader.TimeDateStamp!=0x6ae9b2ac || nt->OptionalHeader.SizeOfImage!=0x2a3f000)return false;
    const native::Entry* entries[]={&native::scope,&native::plan,&native::image,&native::commit,&native::end,
        &native::release,&native::upload,&native::registerImage,&native::unregisterImage,&native::bindImage,
        &native::sceneTick,&native::viewDestroy,&native::rootDestroy,&native::registryDestroy,&native::freeImage};
    for(auto* e:entries)if(std::memcmp(g_base+e->rva+32,e->body,sizeof(e->body)))return false;
    auto* host=greeneye::Host();
    g_state=static_cast<State*>(host->keep("halls.gpu-pool.v1",sizeof(State)));
    if(!g_state)return false;
    // Transient upload buffers belong to the old DLL. Keep only finished GPU contents.
    for(auto& slot:g_state->slots){slot.queued=false;slot.pending=0;}
    g_wanted=g_state->ticket!=0;
    g_tick=reinterpret_cast<TickFn>(host->hook(g_base+native::sceneTick.rva,reinterpret_cast<void*>(TickHook),16,nullptr,0));
    g_destroy=reinterpret_cast<DestroyFn>(host->hook(g_base+native::viewDestroy.rva,reinterpret_cast<void*>(DestroyHook),12,nullptr,0));
    g_rootDestroy=reinterpret_cast<RootDestroyFn>(host->hook(g_base+native::rootDestroy.rva,reinterpret_cast<void*>(RootDestroyHook),18,nullptr,0));
    g_registryDestroy=reinterpret_cast<RegistryDestroyFn>(host->hook(g_base+native::registryDestroy.rva,reinterpret_cast<void*>(RegistryDestroyHook),16,nullptr,0));
    g_enabled=g_tick && g_destroy && g_rootDestroy && g_registryDestroy;
    if(g_log)g_log("Hollow Halls GPU pool: %s; native UI loader bypassed",g_enabled?"ready":"hooks unavailable");
    return g_enabled;
#endif
}
} // namespace hdpool
