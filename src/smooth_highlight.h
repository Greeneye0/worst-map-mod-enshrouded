#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
namespace smooth_highlight {
using Point3=std::array<float,3>;
struct Point {float x,y;};
struct Vertex {float x,y;uint32_t colour;float u=0,v=0,gain=1;};
static_assert(sizeof(Vertex)==24);
// Keep mesh-derived support points, rather than a generic bounding box. At most
// 98 directions are sampled once per asset; projection remains exact each frame.
inline std::vector<Point3> Support(const std::vector<Point3>& vertices) {
    std::vector<Point3> out;
    if(vertices.empty())return out;
    for(int x=-2;x<=2;++x)for(int y=-2;y<=2;++y)for(int z=-2;z<=2;++z){
        if(std::max({std::abs(x),std::abs(y),std::abs(z)})!=2)continue;
        float best=-INFINITY;Point3 point{};
        for(const auto& v:vertices){float dot=v[0]*x+v[1]*y+v[2]*z;if(dot>best){best=dot;point=v;}}
        if(std::find(out.begin(),out.end(),point)==out.end())out.push_back(point);
    }return out;
}
inline float Cross(Point a,Point b,Point c){return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);}
inline std::vector<Point> Hull(std::vector<Point> p){
    if(p.size()<3)return {};
    std::sort(p.begin(),p.end(),[](Point a,Point b){return a.x<b.x||(a.x==b.x&&a.y<b.y);});
    p.erase(std::unique(p.begin(),p.end(),[](Point a,Point b){return a.x==b.x&&a.y==b.y;}),p.end());
    std::vector<Point> h;
    for(auto v:p){while(h.size()>1&&Cross(h[h.size()-2],h.back(),v)<=0)h.pop_back();h.push_back(v);}
    size_t lower=h.size();
    for(size_t i=p.size()-1;i-->0;){auto v=p[i];while(h.size()>lower&&Cross(h[h.size()-2],h.back(),v)<=0)h.pop_back();h.push_back(v);}
    if(!h.empty())h.pop_back();return h.size()>=3?h:std::vector<Point>{};
}
inline uint32_t Alpha(uint32_t c,float a){return (c&0xffffff)|(uint32_t(std::clamp(a,0.f,1.f)*255.f)<<24);}
struct Batch {std::vector<Vertex> vertices;std::vector<uint16_t> indices;};
// One convex silhouette: translucent fill, a thin contour and a one-pixel alpha
// fringe. Real triangles have smooth subpixel edges; no raster grid or markers.
inline bool Append(const std::vector<Point>& hull,uint32_t colour,float alpha,Batch& batch){
    size_t n=hull.size();if(n<3||n>98||batch.vertices.size()+n*3+1>60000||batch.indices.size()+n*15>60000)return false;
    Point center{};for(auto p:hull){if(!std::isfinite(p.x)||!std::isfinite(p.y))return false;center.x+=p.x/float(n);center.y+=p.y/float(n);}
    for(size_t i=0;i<n;++i)if(std::hypot(hull[i].x-hull[(i+1)%n].x,hull[i].y-hull[(i+1)%n].y)<.001f)return false;
    const uint16_t base=static_cast<uint16_t>(batch.vertices.size());
    batch.vertices.push_back({center.x,center.y,Alpha(colour,alpha*.20f)});
    for(size_t i=0;i<n;++i){
        Point prev=hull[(i+n-1)%n],p=hull[i],next=hull[(i+1)%n];
        Point a{p.x-prev.x,p.y-prev.y},b{next.x-p.x,next.y-p.y};
        float la=std::hypot(a.x,a.y),lb=std::hypot(b.x,b.y);if(la<.001f||lb<.001f)return false;
        Point na{a.y/la,-a.x/la},nb{b.y/lb,-b.x/lb};
        float den=std::max(.25f,1+na.x*nb.x+na.y*nb.y);
        Point outward{(na.x+nb.x)/den,(na.y+nb.y)/den};
        batch.vertices.push_back({p.x,p.y,Alpha(colour,alpha*.20f)});
        batch.vertices.push_back({p.x+outward.x*1.25f,p.y+outward.y*1.25f,Alpha(colour,alpha*.65f)});
        batch.vertices.push_back({p.x+outward.x*2.25f,p.y+outward.y*2.25f,Alpha(colour,0)});
    }
    auto tri=[&](unsigned a,unsigned b,unsigned c){batch.indices.push_back(uint16_t(base+a));batch.indices.push_back(uint16_t(base+c));batch.indices.push_back(uint16_t(base+b));};
    for(unsigned i=0;i<n;++i){unsigned a=1+3*i,b=1+3*((i+1)%n);tri(0,a,b);tri(a,a+1,b);tri(b,a+1,b+1);tri(a+1,a+2,b+1);tri(b+1,a+2,b+2);}
    return true;
}
template<class Spatial,class Camera>
inline bool Project(const std::vector<Point3>& support,const Spatial& t,const Camera& camera,uint32_t colour,float alpha,Batch& batch){
    double norm=0;for(float q:t.rotation){if(!std::isfinite(q))return false;norm+=q*q;}if(std::abs(norm-1)>.02)return false;
    for(float s:t.scale)if(!std::isfinite(s)||std::abs(s)>20)return false;
    std::vector<Point> points;points.reserve(support.size());
    const float qx=t.rotation[0],qy=t.rotation[1],qz=t.rotation[2],qw=t.rotation[3];
    for(auto p:support){for(int j=0;j<3;++j)p[j]*=t.scale[j];
        float cx=2*(qy*p[2]-qz*p[1]),cy=2*(qz*p[0]-qx*p[2]),cz=2*(qx*p[1]-qy*p[0]);
        float world[]={float(t.position[0]/4294967296.0)+p[0]+qw*cx+qy*cz-qz*cy,float(t.position[1]/4294967296.0)+p[1]+qw*cy+qz*cx-qx*cz,float(t.position[2]/4294967296.0)+p[2]+qw*cz+qx*cy-qy*cx};
        Point out;float z;if(!camera.Project(world,out.x,out.y,z)||std::abs(out.x)>20000||std::abs(out.y)>20000)return false;points.push_back(out);
    }
    auto hull=Hull(std::move(points));if(hull.empty())return false;
    float minx=hull[0].x,maxx=minx,miny=hull[0].y,maxy=miny;
    for(auto p:hull){minx=std::min(minx,p.x);maxx=std::max(maxx,p.x);miny=std::min(miny,p.y);maxy=std::max(maxy,p.y);}
    if(maxx<camera.viewport[0]||minx>camera.viewport[0]+camera.viewport[2]||maxy<camera.viewport[1]||miny>camera.viewport[1]+camera.viewport[3])return false;
    if(maxx-minx<1||maxy-miny<1)return false;
    return Append(hull,colour,alpha,batch);
}
}
