#pragma once
#include <array>
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>
namespace ore_overlay {
struct V {float x,y,z; V operator+(V b)const{return{x+b.x,y+b.y,z+b.z};} V operator-(V b)const{return{x-b.x,y-b.y,z-b.z};} V operator*(float t)const{return{x*t,y*t,z*t};}};
inline float Dot(V a,V b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline V Cross(V a,V b){return{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
struct Triangle {V p[3],normal;uint32_t colour;};
struct Grid {const uint16_t* samples;size_t count;unsigned n[3];V first;float step;};
// Build a private ore isosurface. The caller's native samples are read-only.
// No coarse block boxes, terrain writes or per-frame extraction.
// Material segmentation uses the same byte/density representation as cutaway.
inline bool Extract(const Grid& g,uint8_t material,V centre,float radius,
                    std::vector<Triangle>& output,size_t maxTriangles=60000,bool* budgetExceeded=nullptr){
 if(budgetExceeded)*budgetExceeded=false;
 if(!g.samples||!material||material>=128||!std::isfinite(g.step)||g.step<=0||g.step>1||
    !std::isfinite(radius)||radius<=0||radius>10)return false;
 for(float f:{g.first.x,g.first.y,g.first.z,centre.x,centre.y,centre.z})if(!std::isfinite(f))return false;
 size_t total=1;for(unsigned n:g.n){if(n<2||n>128)return false;total*=n;}
 if(total!=g.count)return false;
 std::vector<Triangle> mesh;
 constexpr unsigned corners[8][3]={{0,0,0},{1,0,0},{0,1,0},{1,1,0},{0,0,1},{1,0,1},{0,1,1},{1,1,1}};
 // Shared body diagonal and consistent face diagonals avoid cell seams.
 constexpr unsigned tetra[6][4]={{0,1,3,7},{0,3,2,7},{0,2,6,7},{0,6,4,7},{0,4,5,7},{0,5,1,7}};
 auto emit=[&](V a,V b,V c,V towardsOutside){
  V normal=Cross(b-a,c-a);float len=std::sqrt(Dot(normal,normal));
  if(len<1e-8f)return true;
  if(mesh.size()>=maxTriangles){if(budgetExceeded)*budgetExceeded=true;return false;}
  if(Dot(normal,towardsOutside)<0){std::swap(b,c);normal=normal*(-1);}
  mesh.push_back({{a,b,c},normal*(1/len)});return true;
 };
 for(unsigned z=0;z+1<g.n[2];++z)for(unsigned y=0;y+1<g.n[1];++y)for(unsigned x=0;x+1<g.n[0];++x){
  V base=g.first+V{float(x),float(y),float(z)}*g.step;
  float distance=0;const float lo[]={base.x,base.y,base.z},cc[]={centre.x,centre.y,centre.z};
  for(int i=0;i<3;++i){float d=std::max({lo[i]-cc[i],cc[i]-lo[i]-g.step,0.f});distance+=d*d;}
  if(distance>=radius*radius)continue;
  V pos[8];float density[8];
  for(unsigned i=0;i<8;++i){const auto* c=corners[i];auto v=g.samples[(x+c[0])+g.n[0]*((y+c[1])+g.n[1]*(z+c[2]))];
   pos[i]=base+V{float(c[0]),float(c[1]),float(c[2])}*g.step;
   density[i]=(v&255)==material?float(v>>8)/255.f:0.f;
  }
  for(const auto& t:tetra){unsigned in[4],out[4],ni=0,no=0;V a{},b{};
   for(auto i:t)if(density[i]>=.5f){in[ni++]=i;a=a+pos[i];}else{out[no++]=i;b=b+pos[i];}
   if(!ni||!no)continue;V direction=b*(1.f/no)-a*(1.f/ni);
   auto edge=[&](unsigned i,unsigned j){float u=(.5f-density[i])/(density[j]-density[i]);return pos[i]+(pos[j]-pos[i])*u;};
   if(ni==1){if(!emit(edge(in[0],out[0]),edge(in[0],out[1]),edge(in[0],out[2]),direction))return false;}
   else if(ni==3){if(!emit(edge(out[0],in[0]),edge(out[0],in[1]),edge(out[0],in[2]),direction))return false;}
   else {V p=edge(in[0],out[0]),q=edge(in[0],out[1]),r=edge(in[1],out[0]),s=edge(in[1],out[1]);
    if(!emit(p,q,r,direction)||!emit(q,s,r,direction))return false;
   }
  }
 }
 output.swap(mesh);return true;
}
}
