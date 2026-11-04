#pragma once
#include "ore_surface.h"
#include <map>
namespace ore_overlay {
using VertexNormals=std::array<V,3>;
// Rebuilt only when the captured mesh changes, never for camera movement.
inline std::vector<VertexNormals> SmoothNormals(const std::vector<Triangle>& mesh){
 using Key=std::array<int64_t,3>;
 auto key=[](V p)->Key{return {int64_t(std::llround(double(p.x)*4096)),int64_t(std::llround(double(p.y)*4096)),int64_t(std::llround(double(p.z)*4096))};};
 std::map<Key,V> sums;
 for(const auto& t:mesh){auto area=Cross(t.p[1]-t.p[0],t.p[2]-t.p[0]);for(auto p:t.p){auto& n=sums[key(p)];n=n+area;}}
 std::vector<VertexNormals> result;result.reserve(mesh.size());
 for(const auto& t:mesh){VertexNormals normals;for(int i=0;i<3;++i){auto n=sums[key(t.p[i])];float length=std::sqrt(Dot(n,n));normals[i]=length>1e-8f?n*(1.f/length):t.normal;}result.push_back(normals);}
 return result;
}
// View-space camera is the origin; this also works for a rotated/translated camera.
inline bool FrontFacing(const Triangle& t,const float* view){
 auto p=(t.p[0]+t.p[1]+t.p[2])*(1.f/3);
 V q{p.x*view[0]+p.y*view[4]+p.z*view[8]+view[12],p.x*view[1]+p.y*view[5]+p.z*view[9]+view[13],p.x*view[2]+p.y*view[6]+p.z*view[10]+view[14]};
 auto n=t.normal;
 V m{n.x*view[0]+n.y*view[4]+n.z*view[8],n.x*view[1]+n.y*view[5]+n.z*view[9],n.x*view[2]+n.y*view[6]+n.z*view[10]};
 return Dot(m,q)<0;
}
// Conservative sphere/AABB rejection: retains crossing triangles even when all
// three vertices lie outside. The shader still performs exact radius clipping.
inline bool WithinReveal(const Triangle& t,V centre,float radius){
 float distance2=0;
 for(int axis=0;axis<3;++axis){
  auto component=[&](V p){return axis==0?p.x:axis==1?p.y:p.z;};
  float lo=component(t.p[0]),hi=lo;
  for(int i=1;i<3;++i){lo=std::min(lo,component(t.p[i]));hi=std::max(hi,component(t.p[i]));}
  float c=component(centre),d=c<lo?lo-c:c>hi?c-hi:0;
  distance2+=d*d;
 }
 return distance2<=radius*radius;
}
inline void StableProjection(float* p){
 // This game's symmetric perspective projection stores the temporal sampling
 // offsets in the Z column. Remove only these from our private matrix copy.
 if(p[11]==1.f && p[15]==0.f && std::abs(p[8])<.01f && std::abs(p[9])<.01f){p[8]=0;p[9]=0;}
}

}
