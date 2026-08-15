#include "dxr_renderer.hpp"
#include "axe.hpp"
#include "environment.hpp"
#include "ground_texture.hpp"
#include "math.hpp"
#include "optix_denoise.hpp"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dense {
namespace {
template<class T>void release(T*&p){if(p){p->Release();p=nullptr;}}
constexpr UINT64 alignUp(UINT64 value,UINT64 alignment){return(value+alignment-1)&~(alignment-1);}
D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type){D3D12_HEAP_PROPERTIES h{};h.Type=type;h.CPUPageProperty=D3D12_CPU_PAGE_PROPERTY_UNKNOWN;h.MemoryPoolPreference=D3D12_MEMORY_POOL_UNKNOWN;h.CreationNodeMask=1;h.VisibleNodeMask=1;return h;}
D3D12_RESOURCE_DESC bufferDesc(UINT64 bytes,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.Format=DXGI_FORMAT_UNKNOWN;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;return d;}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource*r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=r;b.Transition.StateBefore=before;b.Transition.StateAfter=after;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;return b;}
float fade(float t){return t*t*t*(t*(t*6-15)+10);}
float smoothRange(float low,float high,float value){const float t=clamp((value-low)/(high-low),0,1);return t*t*(3-2*t);}
uint32_t gridHash(int x,int y){uint32_t h=static_cast<uint32_t>(x)*0x8da6b343u^static_cast<uint32_t>(y)*0xd8163841u^0xcb1ab31fu;h^=h>>13;h*=0x85ebca6bu;h^=h>>16;return h;}

struct InstanceGeometryGpu {
    uint32_t indexBase{};
    uint32_t vertexBase{};
    uint32_t visualInstance{};
    uint32_t flags{};
};
static_assert(sizeof(InstanceGeometryGpu)==16);

struct TriangleMeshRange {
    UINT vertexBase{};
    UINT vertexCount{};
    UINT indexBase{};
    UINT indexCount{};
    bool valid()const{return vertexCount&&indexCount;}
};

void appendTreePart(const std::vector<MeshVertex>&partVertices,
                    const std::vector<uint32_t>&partIndices,
                    std::vector<MeshVertex>&vertices,
                    std::vector<uint32_t>&indices,TriangleMeshRange&range){
    range.vertexBase=static_cast<UINT>(vertices.size());
    range.indexBase=static_cast<UINT>(indices.size());
    vertices.insert(vertices.end(),partVertices.begin(),partVertices.end());
    indices.insert(indices.end(),partIndices.begin(),partIndices.end());
    range.vertexCount=static_cast<UINT>(vertices.size())-range.vertexBase;
    range.indexCount=static_cast<UINT>(indices.size())-range.indexBase;
}

bool isCuttableOwner(const std::vector<unsigned char>&mask,uint32_t owner){
    return mask.empty()||(owner<mask.size()&&mask[owner]!=0);
}

bool appendBranchPartition(const TreeMesh&mesh,bool cuttable,
                           const std::vector<unsigned char>&ownerMask,
                           std::vector<MeshVertex>&vertices,
                           std::vector<uint32_t>&indices,TriangleMeshRange&range){
    range={static_cast<UINT>(vertices.size()),0,
           static_cast<UINT>(indices.size()),0};
    if(ownerMask.empty()){
        if(cuttable)appendTreePart(mesh.branchVertices,mesh.branchIndices,
                                  vertices,indices,range);
        return true;
    }
    if(mesh.branchIndices.size()%3!=0||
       mesh.branchTriangleOwners.size()!=mesh.branchIndices.size()/3)return false;
    constexpr uint32_t unmapped=std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t>remap(mesh.branchVertices.size(),unmapped);
    for(size_t triangle=0;triangle<mesh.branchIndices.size()/3;++triangle){
        if(isCuttableOwner(ownerMask,mesh.branchTriangleOwners[triangle])!=cuttable)continue;
        for(size_t corner=0;corner<3;++corner){
            const uint32_t source=mesh.branchIndices[triangle*3+corner];
            if(source>=mesh.branchVertices.size())return false;
            uint32_t&mapped=remap[source];
            if(mapped==unmapped){
                mapped=static_cast<uint32_t>(vertices.size()-range.vertexBase);
                vertices.push_back(mesh.branchVertices[source]);
            }
            indices.push_back(mapped);
        }
    }
    range.vertexCount=static_cast<UINT>(vertices.size())-range.vertexBase;
    range.indexCount=static_cast<UINT>(indices.size())-range.indexBase;
    return true;
}

void appendHiddenTriangle(std::vector<MeshVertex>&vertices,
                          std::vector<uint32_t>&indices,TriangleMeshRange&range){
    range.vertexBase=static_cast<UINT>(vertices.size());
    range.indexBase=static_cast<UINT>(indices.size());
    constexpr float hidden=-4096.0f;
    vertices.insert(vertices.end(),{
        {{0,hidden,0},{0,1,0},0xff000000u,3.0f,0,0},
        {{1,hidden,0},{0,1,0},0xff000000u,3.0f,0,0},
        {{0,hidden,1},{0,1,0},0xff000000u,3.0f,0,0}});
    indices.insert(indices.end(),{0,1,2});
    range.vertexCount=3;range.indexCount=3;
}

RendererRigidTransform rendererTransform(const AxeRigidTransform&source){
    return {source.origin,source.xAxis,source.yAxis,source.zAxis};
}
float tileNoise(float u,float v,int cells){const float x=u*cells,y=v*cells;const int ix=static_cast<int>(std::floor(x)),iy=static_cast<int>(std::floor(y));const float fx=fade(x-ix),fy=fade(y-iy);auto sample=[&](int px,int py){px=(px%cells+cells)%cells;py=(py%cells+cells)%cells;return static_cast<float>(gridHash(px,py)>>8)*(1.0f/16777216.0f);};const float a=sample(ix,iy),b=sample(ix+1,iy),c=sample(ix,iy+1),d=sample(ix+1,iy+1);return(a+(b-a)*fx)+((c+(d-c)*fx)-(a+(b-a)*fx))*fy;}
std::vector<uint32_t> makeOakBarkNormal(UINT width,UINT height){
    const size_t count=static_cast<size_t>(width)*height;
    std::vector<float> heightField(count),cavityField(count),toneField(count);
    std::vector<float> fissures(count),shoulders(count),crossChecks(count);
    constexpr float physicalWidth=1.15f,physicalHeight=2.40f;
    const float texelWidth=physicalWidth/width,texelHeight=physicalHeight/height;
    const auto hash01=[](uint32_t seed){
        seed^=seed>>16;seed*=0x7feb352du;seed^=seed>>15;
        seed*=0x846ca68bu;seed^=seed>>16;
        return static_cast<float>(seed>>8)*(1.0f/16777216.0f);
    };
    const auto wrap=[](int value,int extent){
        value%=extent;return value<0?value+extent:value;
    };
    const auto periodic=[](float value){
        value-=std::floor(value);return value;
    };

    // Stamp a physical-width vertical sample into a single texel row.  The
    // wider shoulder is deliberately irregular and shallow; the core is the
    // only deeply recessed part of the fissure.  This avoids both engraved
    // hairlines and the broad, inflated grooves of the previous bark.
    const auto stampRow=[&](float centreU,UINT y,float coreWidth,
                            float strength,float crossAmount){
        centreU=periodic(centreU);
        const float centre=centreU*width-.5f;
        // The shoulder reaches into a significant fraction of the 2--8 cm
        // plate spacing.  That gives each plate a raised, chipped crown
        // instead of leaving a flat clay field between engraved lines.
        const float outerWidth=coreWidth*2.8f+.0085f;
        const int radius=static_cast<int>(std::ceil(outerWidth/texelWidth))+1;
        const int centrePixel=static_cast<int>(std::floor(centre));
        for(int offset=-radius;offset<=radius;++offset){
            const float distance=std::abs((centrePixel+offset+.5f)-centre)*texelWidth;
            if(distance>=outerWidth)continue;
            const float core=1-smoothRange(coreWidth,coreWidth*1.85f+.0005f,distance);
            const float shoulder=1-smoothRange(coreWidth*1.35f,outerWidth,distance);
            const size_t index=static_cast<size_t>(y)*width+
                               wrap(centrePixel+offset,static_cast<int>(width));
            fissures[index]=std::max(fissures[index],core*strength);
            shoulders[index]=std::max(shoulders[index],shoulder*strength);
            crossChecks[index]=std::max(crossChecks[index],core*crossAmount);
        }
    };
    const auto stampDisc=[&](float centreU,float centreV,float coreWidth,
                             float strength,float crossAmount){
        centreU=periodic(centreU);centreV=periodic(centreV);
        const float centreX=centreU*width-.5f,centreY=centreV*height-.5f;
        const float outerWidth=coreWidth*2.35f+.0045f;
        const int radiusX=static_cast<int>(std::ceil(outerWidth/texelWidth))+1;
        const int radiusY=static_cast<int>(std::ceil(outerWidth/texelHeight))+1;
        const int pixelX=static_cast<int>(std::floor(centreX));
        const int pixelY=static_cast<int>(std::floor(centreY));
        for(int oy=-radiusY;oy<=radiusY;++oy)for(int ox=-radiusX;ox<=radiusX;++ox){
            const float dx=(pixelX+ox+.5f-centreX)*texelWidth;
            const float dy=(pixelY+oy+.5f-centreY)*texelHeight;
            const float distance=std::sqrt(dx*dx+dy*dy);
            if(distance>=outerWidth)continue;
            const float core=1-smoothRange(coreWidth,coreWidth*1.72f+.00035f,distance);
            const float shoulder=1-smoothRange(coreWidth*1.25f,outerWidth,distance);
            const size_t index=static_cast<size_t>(wrap(pixelY+oy,
                static_cast<int>(height)))*width+wrap(pixelX+ox,static_cast<int>(width));
            fissures[index]=std::max(fissures[index],core*strength);
            shoulders[index]=std::max(shoulders[index],shoulder*strength);
            crossChecks[index]=std::max(crossChecks[index],core*crossAmount);
        }
    };

    struct BarkTrack{float base,width,wander,phase;uint32_t seed;bool persistent;};
    constexpr int trackCount=27;
    std::vector<float> gaps(trackCount);float gapSum=0;
    for(int i=0;i<trackCount;++i){
        // A broad distribution of spacings is essential: equal phase spacing
        // immediately reads as a machined comb on cylindrical trunks.
        const float random=hash01(0x41c64e6du+static_cast<uint32_t>(i)*0x9e3779b9u);
        gaps[i]=.55f+1.15f*std::pow(random,1.35f);gapSum+=gaps[i];
    }
    std::vector<BarkTrack> tracks;tracks.reserve(trackCount);
    float cursor=.037f;
    for(int i=0;i<trackCount;++i){
        const uint32_t seed=0xa511e9b3u+static_cast<uint32_t>(i)*0x85ebca6bu;
        const float r0=hash01(seed),r1=hash01(seed^0x68bc21ebu);
        const float r2=hash01(seed^0x02e5be93u);
        tracks.push_back({periodic(cursor/gapSum),.00125f+.0046f*r0*r0,
                          .013f+.024f*r1,.13f+.74f*r2,seed,(i%9)==0});
        cursor+=gaps[i];
    }

    const auto trackCentre=[&](const BarkTrack&track,float v){
        // Periodic value noise gives a wandering path without the obvious
        // wavelength and mirrored turns of a sinusoid.  Both samples tile in
        // V, so the runtime texture remains seamless along branch length.
        const float low=tileNoise(periodic(track.base+track.phase),v,7)-.5f;
        const float mid=tileNoise(periodic(track.base*.61f+track.phase+.271f),v,23)-.5f;
        const float jag=tileNoise(periodic(track.base*.37f+track.phase+.613f),v,53)-.5f;
        const float curl=std::sin(2*pi*(v*(2+(track.seed%3))+
                                       track.phase+low*.12f));
        return periodic(track.base+(low*.50f+mid*.34f+jag*.16f)*track.wander/
                         physicalWidth+curl*.0024f/physicalWidth);
    };

    for(UINT y=0;y<height;++y){
        const float v=(y+.5f)/height;
        for(size_t i=0;i<tracks.size();++i){
            const BarkTrack&track=tracks[i];
            const float centre=trackCentre(track,v);
            const float widthNoise=tileNoise(periodic(track.base+.419f),v,13);
            const float activityNoise=tileNoise(periodic(track.base*.77f+.193f),v,7);
            // A second, shorter-period gate prevents the non-primary tracks
            // from surviving as metre-long etched lines.  Primary fissures
            // remain connected, but even those taper through quiet sections.
            const float segmentNoise=tileNoise(
                periodic(track.base*1.31f+track.phase*.43f+.487f),v,13);
            const float segmentGate=smoothRange(.28f,.65f,segmentNoise);
            const float activity=track.persistent?
                (.24f+.76f*activityNoise)*(.38f+.62f*segmentGate):
                smoothRange(.26f,.68f,activityNoise)*segmentGate;
            if(activity>.025f){
                const float width=track.width*(.62f+(1.55f-.62f)*widthNoise);
                stampRow(centre,y,width,activity,0);
            }
        }

        // Short daughter fissures peel away from a parent and merge back at a
        // different height.  Only a handful exist per tile: enough to create
        // natural splits and irregular plate faces without returning to a
        // closed Voronoi/fish-scale pattern.
        for(int fork=0;fork<17;++fork){
            const uint32_t seed=0x7f4a7c15u+static_cast<uint32_t>(fork)*0x9e3779b9u;
            const BarkTrack&parent=tracks[(fork*5+2)%tracks.size()];
            const float start=hash01(seed^0x31f14a4du);
            const float span=.09f+.25f*hash01(seed^0x94d049bbu);
            const float relative=periodic(v-start);
            if(relative>=span)continue;
            const float t=relative/span;
            const float envelope=std::pow(std::sin(pi*t),.82f);
            const float side=hash01(seed^0x5bd1e995u)<.5f?-1.0f:1.0f;
            const float separation=(.012f+.047f*hash01(seed^0x27d4eb2du))*
                                   envelope/physicalWidth;
            const float centre=periodic(trackCentre(parent,v)+side*separation+
                (tileNoise(parent.base+.37f,v,17)-.5f)*.006f/physicalWidth);
            const float strength=envelope*(.50f+.38f*hash01(seed^0x165667b1u));
            stampRow(centre,y,parent.width*.72f,strength,0);
        }

        // Fine high-order fissures are narrow, intermittent and independently
        // placed.  They supply close-up complexity without adding another
        // evenly spaced global stripe family.
        for(int fineTrack=0;fineTrack<31;++fineTrack){
            const uint32_t seed=0xd1b54a35u+static_cast<uint32_t>(fineTrack)*0x94d049bbu;
            const float base=hash01(seed);
            const float gate=tileNoise(periodic(base+.281f),v,13);
            const float activity=smoothRange(.52f,.72f,gate)*(.22f+.30f*hash01(seed^0x33u));
            if(activity<=.02f)continue;
            const float wander=(tileNoise(periodic(base+.417f),v,19)-.5f)*
                               .025f/physicalWidth;
            stampRow(base+wander,y,.00065f+.00075f*hash01(seed^0xa5u),
                     activity,0);
        }
    }

    // Build cross-checks as graph edges between neighbouring longitudinal
    // tracks. Random isolated dashes looked like scratches on clay; these
    // strokes actually terminate in fissures and therefore form the dense,
    // interlocking elongated plates visible on mature oak. Nearly half are
    // blind checks, leaving the network open rather than a regular grid.
    for(int gap=0;gap<trackCount;++gap){
        const BarkTrack&left=tracks[gap];
        const BarkTrack&right=tracks[(gap+1)%trackCount];
        const uint32_t gapSeed=0x243f6a88u+static_cast<uint32_t>(gap)*0x9e3779b9u;
        const int checkCount=7+static_cast<int>(hash01(gapSeed^0x51ed270bu)*7);
        for(int check=0;check<checkCount;++check){
            const uint32_t seed=gapSeed+static_cast<uint32_t>(check)*0x85ebca6bu;
            if(hash01(seed^0x7f4a7c15u)<.11f)continue;
            // Independent positions intentionally permit clusters and long
            // quiet spans. Stratifying one check per band made the previous
            // result read as staggered brickwork despite random phases.
            const float v0=hash01(seed^0x8aed2a6bu);
            const float verticalDrift=(hash01(seed^0xc2b2ae35u)-.5f)*.018f/
                                      physicalHeight;
            const float v1=periodic(v0+verticalDrift);
            const float leftU=trackCentre(left,v0),rightU=trackCentre(right,v1);
            const float expectedDelta=periodic(right.base-left.base);
            float delta=rightU-leftU;
            delta+=std::round(expectedDelta-delta);
            const bool blind=hash01(seed^0x27d4eb2fu)<.68f;
            const bool fromRight=hash01(seed^0x165667b1u)<.5f;
            const float extent=blind?( .22f+.56f*hash01(seed^0xd3a2646cu)):1.0f;
            const float width=.00070f+.00185f*hash01(seed^0x94d049bbu);
            const int steps=std::max(5,static_cast<int>(std::ceil(
                std::abs(delta)*physicalWidth/texelWidth*.70f)));
            for(int step=0;step<=steps;++step){
                const float t=static_cast<float>(step)/steps;
                const float pathT=fromRight?1-t*extent:t*extent;
                const float envelope=std::pow(std::sin(pi*clamp(t,.001f,.999f)),.25f);
                const float bend=std::sin(pi*pathT)*
                    (hash01(seed^0x5bd1e995u)-.5f)*.010f/physicalWidth;
                const float u=leftU+delta*pathT+bend;
                const float v=periodic(v0+(v1-v0)*pathT+
                    std::sin(pi*pathT)*(hash01(seed^0x31f14a4du)-.5f)*
                    .008f/physicalHeight);
                stampDisc(u,v,width,(.40f+.25f*envelope),.57f*envelope);
            }
        }
    }

    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){
        const float u=(x+.5f)/width,v=(y+.5f)/height;
        const float broad=tileNoise(u,v,5),medium=tileNoise(u,v,17);
        const float fine=tileNoise(u,v,67),fineOffset=tileNoise(u+.193f,v-.127f,67);
        const float chip=tileNoise(u+.071f,v+.219f,43);
        const float grain=tileNoise(u,v,193);
        const size_t index=static_cast<size_t>(y)*width+x;
        const float fissure=clamp(fissures[index],0.0f,1.0f);
        const float shoulder=clamp(shoulders[index],0.0f,1.0f);
        const float cross=clamp(crossChecks[index],0.0f,1.0f);
        const float ridgeCrown=std::pow(clamp(1-shoulder,0.0f,1.0f),.68f)*
                               (.78f+(1.16f-.78f)*medium);
        const float pore=std::pow(clamp(.43f-grain,0.0f,1.0f)*1.75f,2.1f);
        // A shallow, broken two-dimensional microfracture relief roughens the
        // plate faces. It is height-only: keeping it out of the albedo/tone
        // channel prevents the white salt-and-pepper sparkle seen under the
        // local player light.
        const float microDistance=std::abs(fine-fineOffset);
        const float microFracture=(1-smoothRange(.018f,.072f,microDistance))*
                                  smoothRange(.34f,.68f,medium);
        // Plate faces carry millimetre-scale granular relief of their own.
        // This is intentionally normal-only (not tone) so close lighting
        // reveals fractured cork without returning to white speckle.
        heightField[index]=.0017f*(broad-.5f)+.00125f*(medium-.5f)+
                           .00108f*(fine-.5f)+.00068f*(chip-.5f)+
                           .00031f*(grain-.5f)+.0038f*(ridgeCrown-.48f)-.0062f*fissure-
                           .0020f*shoulder-.0017f*cross-.00038f*pore-
                           .00042f*microFracture;
        cavityField[index]=clamp(.82f*fissure+.15f*shoulder+.28f*cross+
                                  .055f*pore,0.0f,1.0f);
        toneField[index]=clamp(.34f+.078f*(broad-.5f)+.062f*(medium-.5f)+
                                .060f*(ridgeCrown-.5f)-.095f*fissure-
                                .036f*cross,0.0f,1.0f);
    }
    std::vector<uint32_t> pixels(count);auto encode=[](float value){return static_cast<uint32_t>(clamp((value*.5f+.5f)*255.0f,0,255));};
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const size_t left=static_cast<size_t>(y)*width+(x+width-1)%width,right=static_cast<size_t>(y)*width+(x+1)%width,down=static_cast<size_t>((y+height-1)%height)*width+x,up=static_cast<size_t>((y+1)%height)*width+x,index=static_cast<size_t>(y)*width+x;const float dhdx=(heightField[right]-heightField[left])/(2*texelWidth),dhdy=(heightField[up]-heightField[down])/(2*texelHeight);const float inverse=1/std::sqrt(dhdx*dhdx*.48f*.48f+dhdy*dhdy*.62f*.62f+1);const float nx=-dhdx*.48f*inverse,ny=-dhdy*.62f*inverse;const uint32_t r=encode(nx),g=encode(ny),b=static_cast<uint32_t>(clamp(cavityField[index]*255.0f,0,255)),a=static_cast<uint32_t>(clamp(toneField[index]*255.0f,0,255));pixels[index]=r|(g<<8)|(b<<16)|(a<<24);}
    return pixels;
}
std::vector<std::vector<uint32_t>> makeNormalMipChain(std::vector<uint32_t> top,UINT width,UINT height,UINT levels){
    std::vector<std::vector<uint32_t>> result;result.reserve(levels);result.push_back(std::move(top));
    UINT previousWidth=width,previousHeight=height;
    for(UINT level=1;level<levels;++level){
        const UINT currentWidth=std::max(1u,previousWidth/2),currentHeight=std::max(1u,previousHeight/2);std::vector<uint32_t> current(static_cast<size_t>(currentWidth)*currentHeight);const auto&previous=result.back();
        for(UINT y=0;y<currentHeight;++y)for(UINT x=0;x<currentWidth;++x){
            float nx=0,ny=0,nz=0,cavity=0,maxCavity=0,tone=0;
            for(UINT oy=0;oy<2;++oy)for(UINT ox=0;ox<2;++ox){const uint32_t packed=previous[static_cast<size_t>(std::min(previousHeight-1,y*2+oy))*previousWidth+std::min(previousWidth-1,x*2+ox)];const float sampleX=((packed&255)/255.0f)*2-1,sampleY=(((packed>>8)&255)/255.0f)*2-1,sampleZ=std::sqrt(std::max(0.0f,1-sampleX*sampleX-sampleY*sampleY)),sampleCavity=((packed>>16)&255)/255.0f;nx+=sampleX;ny+=sampleY;nz+=sampleZ;cavity+=sampleCavity;maxCavity=std::max(maxCavity,sampleCavity);tone+=((packed>>24)&255)/255.0f;}
            const float inverse=1/std::sqrt(nx*nx+ny*ny+nz*nz);nx*=inverse;ny*=inverse;cavity=level<=5?cavity*.1625f+maxCavity*.35f:cavity*.25f;tone*=.25f;auto encode=[](float value){return static_cast<uint32_t>(clamp((value*.5f+.5f)*255.0f,0,255));};const uint32_t r=encode(nx),g=encode(ny),b=static_cast<uint32_t>(clamp(cavity*255.0f,0,255)),a=static_cast<uint32_t>(clamp(tone*255.0f,0,255));current[static_cast<size_t>(y)*currentWidth+x]=r|(g<<8)|(b<<16)|(a<<24);
        }
        result.push_back(std::move(current));previousWidth=currentWidth;previousHeight=currentHeight;
    }
    return result;
}
}

struct DxrRenderer::Impl{
    HWND window{};int width=1,height=1,renderWidth=1,renderHeight=1;std::wstring lastError;bool initialized=false;bool deviceLost=false;UINT frameIndex=0;
    StreamlineHost sl;
    CameraBasis prevCamera{};bool havePrevCamera{};
    ID3D12Resource*linearDepth{};ID3D12Resource*motionVectors{};ID3D12Resource*normalRough{};
    ID3D12Resource*diffuseAlbedo{};ID3D12Resource*specularAlbedo{};
    ID3D12Resource*uiColor{};ID3D12Resource*dlssOutput{};
    ID3D12RootSignature*presentRoot{};ID3D12PipelineState*presentPipeline{};
    ID3D12Resource*presentBuffer{};void*presentMapped{};
    ID3D12RootSignature*denoiseRoot{};ID3D12PipelineState*denoisePipeline{};
    ID3D12Resource*denoisePing{};ID3D12Resource*denoiseCb{};void*denoiseCbMapped{};
    ID3D12Resource*readback{};UINT64 readbackPitch{};
    ID3D12Resource*hdrReadback{};UINT64 hdrReadbackPitch{};
    ID3D12Resource*lastHdr{};
    ID3D12CommandQueue*copyQueue{};ID3D12CommandAllocator*copyAlloc{};
    ID3D12GraphicsCommandList*copyList{};ID3D12Fence*copyFence{};
    HANDLE copyFenceEvent{};UINT64 copyFenceValue{};
    UINT offlineSpp{};
    UINT lastBack{};
    UINT experiment{};
    bool upscaleThisFrame{};
    CameraView lastView{};PlayerLocalLight lastLocalLight{};
    bool haveLastView=false,haveLastLocalLight=false;
    DebugRenderSettings lastDebugSettings{};bool haveLastDebugSettings=false;
    EnvironmentCB lastEnvironment{};bool haveLastEnvironment=false;
    EnvironmentMesh environment{};
    IDXGIFactory6*factory{};IDXGISwapChain3*swap{};ID3D12Device5*device{};ID3D12CommandQueue*queue{};ID3D12CommandAllocator*allocator{};ID3D12GraphicsCommandList4*list{};
    ID3D12CommandAllocator*allocators[2]{};ID3D12GraphicsCommandList4*lists[2]{};
    UINT64 inflightFence[2]{};UINT inflightSlot{};UINT submittedFrames{};
    void*cameraMappedBase{};void*environmentMappedBase{};void*hudMappedBase{};
    void*presentMappedBase{};void*denoiseCbMappedBase{};void*visibleGrassMappedBase{};
    UINT64 visibleGrassStride{};
    D3D12_GPU_VIRTUAL_ADDRESS cameraGpu{};D3D12_GPU_VIRTUAL_ADDRESS environmentGpu{};
    D3D12_GPU_VIRTUAL_ADDRESS hudGpu{};D3D12_GPU_VIRTUAL_ADDRESS presentGpu{};
    D3D12_GPU_VIRTUAL_ADDRESS denoiseGpu{};D3D12_GPU_VIRTUAL_ADDRESS visibleGrassGpu{};
    ID3D12Fence*fence{};HANDLE fenceEvent{};UINT64 fenceValue{};ID3D12DescriptorHeap*rtvHeap{};ID3D12DescriptorHeap*dsvHeap{};ID3D12DescriptorHeap*gpuHeap{};UINT rtvSize{},srvSize{};ID3D12Resource*backBuffers[2]{};
    ID3D12RootSignature*root{};ID3D12StateObject*state{};ID3D12StateObjectProperties*stateProps{};
    ID3D12RootSignature*grassRoot{};ID3D12PipelineState*grassPipeline{};
    ID3D12RootSignature*hudRoot{};ID3D12PipelineState*hudPipeline{};
    ID3D12RootSignature*treeWindRoot{};ID3D12PipelineState*treeWindPipeline{};
    ID3D12Resource*hudBuffer{};void*hudMapped{};
    HudState hud{};bool vsync{};
    ID3D12Resource*output{};ID3D12Resource*accumulation{};ID3D12Resource*grassDepth{};ID3D12Resource*barkNormal{};ID3D12Resource*groundAlbedo{};ID3D12Resource*groundNormal{};ID3D12Resource*cameraBuffer{};void*cameraMapped{};ID3D12Resource*environmentBuffer{};void*environmentMapped{};
    ID3D12Resource*vertexBuffer{};ID3D12Resource*baseTreeVertexBuffer{};
    ID3D12Resource*standingVertexBuffer{};ID3D12Resource*standingIndexBuffer{};
    ID3D12Resource*detachedVertexBuffer{};ID3D12Resource*detachedIndexBuffer{};
    ID3D12Resource*axeVertexBuffer{};ID3D12Resource*axeIndexBuffer{};
    ID3D12Resource*indexBuffer{};ID3D12Resource*blas{};ID3D12Resource*leafBlas{};
    ID3D12Resource*staticBlas{};
    ID3D12Resource*standingBlas{};ID3D12Resource*detachedBlas{};ID3D12Resource*axeBlas{};
    ID3D12Resource*chipBlas{};
    ID3D12Resource*tlas{};ID3D12Resource*blasScratch{};ID3D12Resource*leafBlasScratch{};
    ID3D12Resource*staticBlasScratch{};
    ID3D12Resource*standingBlasScratch{};ID3D12Resource*detachedBlasScratch{};
    ID3D12Resource*axeBlasScratch{};
    ID3D12Resource*chipBlasScratch{};
    ID3D12Resource*tlasScratch{};ID3D12Resource*instanceBuffer{};
    ID3D12Resource*instanceGeometryBuffer{};
    ID3D12Resource*grassBuffer{};ID3D12Resource*grassBladeBuffer{};ID3D12Resource*visibleGrassBuffer{};
    UINT grassBladeCount{};
    ID3D12Resource*visibleGrassUploadBuffer{};
    ID3D12Resource*grassBlas{};ID3D12Resource*grassBlasScratch{};
    struct GrassBlasChunk{
        ID3D12Resource*blas{};
        ID3D12Resource*scratch{};
        UINT bladeBase{};
        UINT bladeCount{};
    };
    std::vector<GrassBlasChunk>grassBlasChunks;
    void*visibleGrassMapped{};bool visibleGrassGpuReady{};
    ID3D12Resource*raygenTable{};ID3D12Resource*missTable{};ID3D12Resource*hitTable{};
    UINT vertexCount{},indexCount{},treeVertexCount{},treeIndexCount{},grassPatchCount{};
    UINT tlasInstanceCount{2};
    float treeHeight=1.0f;bool treeWindWasActive=false;bool hasDynamicTree=true;
    UINT visibleNearGrassPatchCount{},visibleFarGrassPatchCount{};
    struct GrassChunk{
        std::vector<GrassPatchGpu>patches;
        uint64_t lastUse{};
    };
    std::unordered_map<uint64_t,GrassChunk>grassChunkCache;
    uint64_t grassStreamEpoch{};
    std::vector<GrassPatchGpu>streamedGrassPatches;
    std::vector<std::pair<float,UINT>>grassNearOrder;
    std::vector<std::pair<float,UINT>>grassFarOrder;
    bool grassStreamValid{};
    WaterSampler waterSampler;
    bool customWorld{};
    float grassStreamCenterX=std::numeric_limits<float>::quiet_NaN();
    float grassStreamCenterZ=std::numeric_limits<float>::quiet_NaN();
    float grassStreamRadius{};

    std::shared_ptr<const TreeMesh> sourceTree{};
    std::vector<unsigned char>cuttableBranchOwners;
    std::shared_ptr<const TreeMesh> promotedStanding{},promotedDetached{};
    std::shared_ptr<const TreeMesh>woodChipMesh{};
    AxeMesh axeMesh{};
    TreeInstance promotedSource{};
    std::size_t promotedSharedIndex=std::numeric_limits<std::size_t>::max();
    RendererRigidTransform standingTransform{},detachedTransform{},axeTransform{},
                           woodChipTransform{};
    bool promotedTreeActive{},axeGeometryReady{},axeVisible{};
    bool woodChipGeometryReady{},woodChipsVisible{};
    bool promotedCanopyDetached{};
    TriangleMeshRange sharedBranchRange{},sharedLeafRange{},environmentRange{},
                      standingRange{},detachedRange{},axeRange{},chipRange{};
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC>sceneInstances;
    std::vector<InstanceGeometryGpu>instanceGeometry;
    void*instanceMapped{};
    UINT standingInstanceSlot=UINT_MAX,detachedInstanceSlot=UINT_MAX,
         promotedLeafInstanceSlot=UINT_MAX,
         axeInstanceSlot=UINT_MAX,chipInstanceSlot=UINT_MAX;
    static constexpr UINT woodChipVertexCapacity=1024;
    static constexpr UINT woodChipIndexCapacity=1024;
    UINT woodChipVertexBase{},woodChipIndexBase{};
    bool woodChipArenaReady{};
    std::vector<uint32_t>woodChipTopologyIndices;

    ~Impl(){wait();sl.shutdown();for(GrassBlasChunk&chunk:grassBlasChunks){release(chunk.scratch);release(chunk.blas);}grassBlasChunks.clear();if(cameraBuffer&&cameraMapped)cameraBuffer->Unmap(0,nullptr);if(environmentBuffer&&environmentMapped)environmentBuffer->Unmap(0,nullptr);if(hudBuffer&&hudMapped)hudBuffer->Unmap(0,nullptr);if(presentBuffer&&presentMapped)presentBuffer->Unmap(0,nullptr);if(denoiseCb&&denoiseCbMapped)denoiseCb->Unmap(0,nullptr);if(visibleGrassUploadBuffer&&visibleGrassMapped)visibleGrassUploadBuffer->Unmap(0,nullptr);if(instanceBuffer&&instanceMapped)instanceBuffer->Unmap(0,nullptr);release(hitTable);release(missTable);release(raygenTable);release(instanceGeometryBuffer);release(instanceBuffer);release(tlasScratch);release(chipBlasScratch);release(axeBlasScratch);release(detachedBlasScratch);release(standingBlasScratch);release(grassBlasScratch);release(staticBlasScratch);release(leafBlasScratch);release(blasScratch);release(tlas);release(chipBlas);release(axeBlas);release(detachedBlas);release(standingBlas);release(grassBlas);release(staticBlas);release(leafBlas);release(blas);release(axeIndexBuffer);release(axeVertexBuffer);release(detachedIndexBuffer);release(detachedVertexBuffer);release(standingIndexBuffer);release(standingVertexBuffer);release(visibleGrassBuffer);release(visibleGrassUploadBuffer);release(grassBladeBuffer);release(grassBuffer);release(indexBuffer);release(baseTreeVertexBuffer);release(vertexBuffer);release(environmentBuffer);release(cameraBuffer);release(hudBuffer);release(hdrReadback);release(readback);release(denoiseCb);release(denoisePing);release(presentBuffer);release(copyList);release(copyAlloc);release(copyFence);release(copyQueue);if(copyFenceEvent){CloseHandle(copyFenceEvent);copyFenceEvent=nullptr;}release(groundNormal);release(groundAlbedo);release(barkNormal);release(uiColor);release(dlssOutput);release(specularAlbedo);release(diffuseAlbedo);release(normalRough);release(motionVectors);release(linearDepth);release(grassDepth);release(accumulation);release(output);release(denoisePipeline);release(denoiseRoot);release(presentPipeline);release(presentRoot);release(treeWindPipeline);release(treeWindRoot);release(hudPipeline);release(hudRoot);release(grassPipeline);release(grassRoot);release(stateProps);release(state);release(root);for(auto&b:backBuffers)release(b);release(gpuHeap);release(dsvHeap);release(rtvHeap);list=nullptr;allocator=nullptr;release(lists[0]);release(lists[1]);release(allocators[0]);release(allocators[1]);release(fence);release(queue);release(swap);release(device);release(factory);if(fenceEvent)CloseHandle(fenceEvent);}
    bool fail(HRESULT hr,const wchar_t*message){wchar_t text[320];wsprintfW(text,L"%s (HRESULT 0x%08X)",message,static_cast<unsigned>(hr));lastError=text;return false;}
    void wait(){if(!queue||!fence)return;const UINT64 value=++fenceValue;if(SUCCEEDED(queue->Signal(fence,value))&&fence->GetCompletedValue()<value){fence->SetEventOnCompletion(value,fenceEvent);WaitForSingleObject(fenceEvent,INFINITE);}inflightFence[0]=inflightFence[1]=fence->GetCompletedValue();}
    void waitSlot(UINT slot){if(!fence||!inflightFence[slot])return;if(fence->GetCompletedValue()<inflightFence[slot]){fence->SetEventOnCompletion(inflightFence[slot],fenceEvent);WaitForSingleObject(fenceEvent,INFINITE);}}
    void bindInFlight(UINT slot){
        inflightSlot=slot&1u;
        allocator=allocators[inflightSlot]?allocators[inflightSlot]:allocator;
        list=lists[inflightSlot]?lists[inflightSlot]:list;
        cameraMapped=cameraMappedBase?static_cast<char*>(cameraMappedBase)+inflightSlot*256:cameraMapped;
        environmentMapped=environmentMappedBase?static_cast<char*>(environmentMappedBase)+inflightSlot*256:environmentMapped;
        hudMapped=hudMappedBase?static_cast<char*>(hudMappedBase)+inflightSlot*256:hudMapped;
        presentMapped=presentMappedBase?static_cast<char*>(presentMappedBase)+inflightSlot*256:presentMapped;
        denoiseCbMapped=denoiseCbMappedBase?static_cast<char*>(denoiseCbMappedBase)+inflightSlot*256:denoiseCbMapped;
        if(cameraBuffer)cameraGpu=cameraBuffer->GetGPUVirtualAddress()+inflightSlot*256ull;
        if(environmentBuffer)environmentGpu=environmentBuffer->GetGPUVirtualAddress()+inflightSlot*256ull;
        if(hudBuffer)hudGpu=hudBuffer->GetGPUVirtualAddress()+inflightSlot*256ull;
        if(presentBuffer)presentGpu=presentBuffer->GetGPUVirtualAddress()+inflightSlot*256ull;
        if(denoiseCb)denoiseGpu=denoiseCb->GetGPUVirtualAddress()+inflightSlot*256ull;
        if(visibleGrassMappedBase&&visibleGrassStride){
            visibleGrassMapped=static_cast<char*>(visibleGrassMappedBase)+inflightSlot*visibleGrassStride;
            if(visibleGrassUploadBuffer)
                visibleGrassGpu=visibleGrassUploadBuffer->GetGPUVirtualAddress()+inflightSlot*visibleGrassStride;
        }
    }
    void recycleInFlight(){
        // One in-flight frame only. Alternating command lists plus NGX/DLSS
        // removed the device after a few presents.
        wait();
        bindInFlight(0);
    }
    bool begin(){wait();bindInFlight(0);if(FAILED(allocator->Reset()))return false;if(FAILED(list->Reset(allocator,nullptr)))return false;return true;}
    bool beginRecording(){if(!allocator||!list)return false;if(FAILED(allocator->Reset()))return false;if(FAILED(list->Reset(allocator,nullptr)))return false;return true;}
    bool execute(){if(FAILED(list->Close()))return false;ID3D12CommandList*commands[]={list};queue->ExecuteCommandLists(1,commands);wait();return true;}
    bool executeAsync(){
        if(!execute())return false;
        ++submittedFrames;
        return true;
    }
    void createDlssFeature(){
        if(sl.status().quality==DlssQuality::Off)return;
        if(!begin())return;
        const bool ok=sl.ensureFeature(list);
        execute();
        FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a");
        if(boot){
            const auto&st=sl.status();
            fprintf(boot,"dlss_feature %d label %s %ux%u->%ux%u\n",
                    ok?1:0,st.label,st.renderWidth,st.renderHeight,
                    st.displayWidth,st.displayHeight);
            fclose(boot);
        }
    }
    ID3D12Resource*makeBuffer(UINT64 bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){ID3D12Resource*r{};auto h=heap(type);auto d=bufferDesc(std::max<UINT64>(bytes,256),flags);if(FAILED(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&r))))return nullptr;return r;}
    template<class T>ID3D12Resource*upload(const std::vector<T>&data){ID3D12Resource*r=makeBuffer(data.size()*sizeof(T),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);if(!r)return nullptr;void*mapped{};if(FAILED(r->Map(0,nullptr,&mapped))){release(r);return nullptr;}if(!data.empty())std::memcpy(mapped,data.data(),data.size()*sizeof(T));r->Unmap(0,nullptr);return r;}
    template<class T>ID3D12Resource*uploadDefault(const std::vector<T>&data){const UINT64 copyBytes=data.size()*sizeof(T),bytes=std::max<UINT64>(copyBytes,256);ID3D12Resource*destination=makeBuffer(bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST),*staging=makeBuffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);if(!destination||!staging){release(destination);release(staging);return nullptr;}void*mapped{};if(FAILED(staging->Map(0,nullptr,&mapped))){release(destination);release(staging);return nullptr;}if(copyBytes)std::memcpy(mapped,data.data(),copyBytes);staging->Unmap(0,nullptr);if(!begin()){release(destination);release(staging);return nullptr;}if(copyBytes)list->CopyBufferRegion(destination,0,staging,0,copyBytes);auto barrier=transition(destination,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_GENERIC_READ);list->ResourceBarrier(1,&barrier);if(!execute()){release(destination);release(staging);return nullptr;}release(staging);return destination;}
    template<class T>ID3D12Resource*uploadDefaultUav(const std::vector<T>&data){const UINT64 copyBytes=data.size()*sizeof(T),bytes=std::max<UINT64>(copyBytes,256);ID3D12Resource*destination=makeBuffer(bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),*staging=makeBuffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);if(!destination||!staging){release(destination);release(staging);return nullptr;}void*mapped{};if(FAILED(staging->Map(0,nullptr,&mapped))){release(destination);release(staging);return nullptr;}if(copyBytes)std::memcpy(mapped,data.data(),copyBytes);staging->Unmap(0,nullptr);if(!begin()){release(destination);release(staging);return nullptr;}if(copyBytes)list->CopyBufferRegion(destination,0,staging,0,copyBytes);auto barrier=transition(destination,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&barrier);if(!execute()){release(destination);release(staging);return nullptr;}release(staging);return destination;}
    uint32_t terrainRetentionByte(float worldX,float worldZ)const{
        constexpr int resolution=EnvironmentGenerator::terrainResolution;
        constexpr int centre=(resolution-1)/2;
        constexpr float gridExponent=2.05f;
        if(environment.terrainVertices.size()<static_cast<size_t>(resolution)*resolution)
            return 0;
        const auto gridPosition=[](float world){
            const float normalized=clamp(
                world/EnvironmentGenerator::terrainHalfExtent,-1.0f,1.0f);
            const float uniform=std::copysign(
                std::pow(std::abs(normalized),1.0f/gridExponent),normalized);
            return centre+uniform*centre;
        };
        const float gridX=gridPosition(worldX),gridZ=gridPosition(worldZ);
        const int cellX=std::clamp(static_cast<int>(std::floor(gridX)),0,resolution-2);
        const int cellZ=std::clamp(static_cast<int>(std::floor(gridZ)),0,resolution-2);
        const auto gridCoordinate=[](int coordinate){
            const float centred=static_cast<float>(coordinate-centre)/centre;
            return std::copysign(EnvironmentGenerator::terrainHalfExtent*
                                 std::pow(std::abs(centred),gridExponent),centred);
        };
        const float x0=gridCoordinate(cellX),x1=gridCoordinate(cellX+1);
        const float z0=gridCoordinate(cellZ),z1=gridCoordinate(cellZ+1);
        // The terrain triangles are planar in emitted world space.  The
        // inverse-grid fractional coordinate is nonlinear and therefore is
        // not a valid barycentric weight except at the vertices themselves.
        const float u=clamp((worldX-x0)/(x1-x0),0.0f,1.0f);
        const float v=clamp((worldZ-z0)/(z1-z0),0.0f,1.0f);
        const auto retention=[&](int x,int z){
            const uint32_t color=environment.terrainVertices[
                static_cast<size_t>(z)*resolution+x].color;
            return static_cast<float>(color>>24);
        };
        const float a=retention(cellX,cellZ),b=retention(cellX+1,cellZ);
        const float c=retention(cellX,cellZ+1),d=retention(cellX+1,cellZ+1);
        float value{};
        // Match EnvironmentGenerator's alternating triangle diagonal exactly;
        // puddle edges therefore agree with both the emitted terrain and the
        // grass shader instead of being bilinearly smeared across a ridge.
        if(((cellX+cellZ)&1)==0){
            value=v>=u?a*(1-v)+c*(v-u)+d*u:
                       a*(1-u)+d*v+b*(u-v);
        }else{
            value=u+v<=1?a*(1-u-v)+c*v+b*u:
                          b*(1-v)+c*(1-u)+d*(u+v-1);
        }
        return static_cast<uint32_t>(clamp(value+0.5f,0.0f,255.0f));
    }
    void rebuildGrassStream(const Vec3&eye,float drawDistance){
        if(customWorld){
            streamedGrassPatches=environment.grassPatches;
            grassStreamCenterX=eye.x;grassStreamCenterZ=eye.z;
            grassStreamRadius=drawDistance+22.0f;grassStreamValid=true;
            return;
        }
        constexpr int chunkCells=32;
        constexpr float snap=12.0f,guardBand=22.0f;
        const float centreX=std::round(eye.x/snap)*snap;
        const float centreZ=std::round(eye.z/snap)*snap;
        const float radius=drawDistance+guardBand;
        streamedGrassPatches.clear();
        const float cell=EnvironmentGenerator::grassCellSize;
        const size_t expectedPatches=std::min<size_t>(grassPatchCount,
            static_cast<size_t>(pi*radius*radius/(cell*cell)*.86f)+2048u);
        if(streamedGrassPatches.capacity()<expectedPatches)
            streamedGrassPatches.reserve(expectedPatches);
        const int minimumX=static_cast<int>(std::floor((centreX-radius)/cell));
        const int maximumX=static_cast<int>(std::ceil((centreX+radius)/cell));
        const int minimumZ=static_cast<int>(std::floor((centreZ-radius)/cell));
        const int maximumZ=static_cast<int>(std::ceil((centreZ+radius)/cell));
        const auto floorDivide=[](int value){
            int quotient=value/chunkCells;
            if(value%chunkCells<0)--quotient;
            return quotient;
        };
        const auto chunkKey=[](int x,int z){
            return (static_cast<uint64_t>(static_cast<uint32_t>(x))<<32)|
                   static_cast<uint32_t>(z);
        };
        const int minimumChunkX=floorDivide(minimumX);
        const int maximumChunkX=floorDivide(maximumX);
        const int minimumChunkZ=floorDivide(minimumZ);
        const int maximumChunkZ=floorDivide(maximumZ);
        const float radiusSquared=radius*radius;
        const float chunkWorldSize=chunkCells*cell;
        ++grassStreamEpoch;
        size_t activeChunkCount=0;
        for(int chunkZ=minimumChunkZ;chunkZ<=maximumChunkZ;++chunkZ){
            for(int chunkX=minimumChunkX;chunkX<=maximumChunkX;++chunkX){
                const float chunkMinimumX=chunkX*chunkWorldSize;
                const float chunkMaximumX=chunkMinimumX+chunkWorldSize;
                const float chunkMinimumZ=chunkZ*chunkWorldSize;
                const float chunkMaximumZ=chunkMinimumZ+chunkWorldSize;
                const float closestX=clamp(centreX,chunkMinimumX,chunkMaximumX);
                const float closestZ=clamp(centreZ,chunkMinimumZ,chunkMaximumZ);
                const float chunkDeltaX=closestX-centreX;
                const float chunkDeltaZ=closestZ-centreZ;
                if(chunkDeltaX*chunkDeltaX+chunkDeltaZ*chunkDeltaZ>radiusSquared)
                    continue;
                ++activeChunkCount;
                const uint64_t key=chunkKey(chunkX,chunkZ);
                auto [iterator,inserted]=grassChunkCache.try_emplace(key);
                GrassChunk&chunk=iterator->second;
                if(inserted){
                    chunk.patches.reserve(chunkCells*chunkCells*3/4);
                    const int firstCellX=chunkX*chunkCells;
                    const int firstCellZ=chunkZ*chunkCells;
                    for(int localZ=0;localZ<chunkCells;++localZ){
                        for(int localX=0;localX<chunkCells;++localX){
                            GrassPatchGpu patch;
                            if(EnvironmentGenerator::makeGrassPatch(
                                firstCellX+localX,firstCellZ+localZ,
                                environment.grassSeed,patch)){
                                const float patchX=(patch.minX+patch.maxX)*.5f;
                                const float patchZ=(patch.minZ+patch.maxZ)*.5f;
                                const uint32_t retention=terrainRetentionByte(patchX,patchZ);
                                patch.seed=(patch.seed&0x00ffffffu)|(retention<<24);
                                chunk.patches.push_back(patch);
                            }
                        }
                    }
                }
                chunk.lastUse=grassStreamEpoch;
                for(const GrassPatchGpu&patch:chunk.patches){
                    const float patchX=(patch.minX+patch.maxX)*.5f-centreX;
                    const float patchZ=(patch.minZ+patch.maxZ)*.5f-centreZ;
                    if(patchX*patchX+patchZ*patchZ>radiusSquared)continue;
                    streamedGrassPatches.push_back(patch);
                    if(streamedGrassPatches.size()>=grassPatchCount)break;
                }
                if(streamedGrassPatches.size()>=grassPatchCount)break;
            }
            if(streamedGrassPatches.size()>=grassPatchCount)break;
        }
        // Keep roughly one maximum-range neighbourhood plus a small travel
        // history.  Cache eviction never affects blade identity because every
        // chunk is rebuilt solely from its absolute integer coordinates.
        const size_t cacheLimit=std::max<size_t>(256,activeChunkCount+128);
        if(grassChunkCache.size()>cacheLimit){
            std::vector<std::pair<uint64_t,uint64_t>>evictionCandidates;
            evictionCandidates.reserve(grassChunkCache.size()-activeChunkCount);
            for(const auto&entry:grassChunkCache)
                if(entry.second.lastUse!=grassStreamEpoch)
                    evictionCandidates.push_back({entry.second.lastUse,entry.first});
            std::sort(evictionCandidates.begin(),evictionCandidates.end());
            size_t removeCount=grassChunkCache.size()-cacheLimit;
            for(const auto&candidate:evictionCandidates){
                if(removeCount==0)break;
                removeCount-=grassChunkCache.erase(candidate.second);
            }
        }
        grassStreamCenterX=centreX;grassStreamCenterZ=centreZ;
        grassStreamRadius=radius;
        grassStreamValid=true;
    }
    std::pair<UINT,UINT> compactVisibleGrass(
        const Vec3&eye,const Vec3&forward,const Vec3&right,const Vec3&up,
        float tanHalf,float aspect,const DebugRenderSettings&settings){
        if(!visibleGrassMapped)return {};
        auto*visible=static_cast<GrassPatchGpu*>(visibleGrassMapped);
        UINT nearCount=0,farCount=0;
        if(!grassStreamValid)
            rebuildGrassStream(eye,std::numeric_limits<float>::max());
        grassNearOrder.clear();
        grassFarOrder.clear();
        if(grassPatchCount==0)return {};
        const UINT patchTotal=static_cast<UINT>(streamedGrassPatches.size());
        for(UINT index=0;index<patchTotal;++index){
            const GrassPatchGpu&patch=streamedGrassPatches[index];
            const Vec3 center{(patch.minX+patch.maxX)*.5f,patch.baseY,
                              (patch.minZ+patch.maxZ)*.5f};
            const Vec3 delta=center-eye;
            const float radiusX=(patch.maxX-patch.minX)*.5f;
            const float radiusY=std::max(patch.maxY-patch.baseY,
                                         patch.baseY-patch.minY);
            const float radiusZ=(patch.maxZ-patch.minZ)*.5f;
            const float interactionCullMargin=2.25f*
                clamp(settings.bladeHeightScale,.35f,2.5f);
            const float radius=std::sqrt(radiusX*radiusX+radiusY*radiusY+
                                         radiusZ*radiusZ)+interactionCullMargin;
            const float viewDepth=dot(delta,forward);
            if(viewDepth+radius<=-1.25f)continue;
            const float projectedDepth=std::max(viewDepth,.02f);
            if(std::abs(dot(delta,right))>projectedDepth*tanHalf*aspect+radius)continue;
            if(std::abs(dot(delta,up))>projectedDepth*tanHalf+radius)continue;
            if(nearCount>=grassPatchCount)break;
            visible[nearCount++]=patch;
        }
        return {nearCount,farCount};
    }
    bool recordVisibleGrassUpload(bool grassEnabled){
        const UINT visibleCount=visibleNearGrassPatchCount+visibleFarGrassPatchCount;
        if(!grassEnabled||visibleCount==0)return true;
        if(!visibleGrassUploadBuffer||!visibleGrassBuffer||!visibleGrassMapped)return false;
        const UINT64 srcBase=inflightSlot*visibleGrassStride;
        if(visibleGrassGpuReady){
            auto toCopy=transition(visibleGrassBuffer,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
            list->ResourceBarrier(1,&toCopy);
        }
        const UINT64 patchBytes=sizeof(GrassPatchGpu);
        if(visibleNearGrassPatchCount){
            const UINT64 nearBytes=static_cast<UINT64>(visibleNearGrassPatchCount)*patchBytes;
            list->CopyBufferRegion(visibleGrassBuffer,0,visibleGrassUploadBuffer,srcBase,nearBytes);
        }
        if(visibleFarGrassPatchCount){
            const UINT64 farOffset=static_cast<UINT64>(
                grassPatchCount-visibleFarGrassPatchCount)*patchBytes;
            const UINT64 farBytes=static_cast<UINT64>(visibleFarGrassPatchCount)*patchBytes;
            list->CopyBufferRegion(visibleGrassBuffer,farOffset,
                                   visibleGrassUploadBuffer,srcBase+farOffset,farBytes);
        }
        auto toVertex=transition(visibleGrassBuffer,D3D12_RESOURCE_STATE_COPY_DEST,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1,&toVertex);
        visibleGrassGpu=visibleGrassBuffer->GetGPUVirtualAddress();
        visibleGrassGpuReady=true;
        return true;
    }
    bool createBackBuffers(){D3D12_CPU_DESCRIPTOR_HANDLE handle=rtvHeap->GetCPUDescriptorHandleForHeapStart();for(UINT n=0;n<2;++n){HRESULT hr=swap->GetBuffer(n,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&backBuffers[n]));if(FAILED(hr))return fail(hr,L"DXR swap-chain buffer creation failed");device->CreateRenderTargetView(backBuffers[n],nullptr,handle);handle.ptr+=rtvSize;}return true;}
    ID3D12Resource*makeTexture2D(UINT w,UINT h,DXGI_FORMAT format,D3D12_RESOURCE_FLAGS flags,
                                 D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE*clear=nullptr){
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
        d.DepthOrArraySize=1;d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;
        d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;d.Flags=flags;
        auto heapProps=heap(D3D12_HEAP_TYPE_DEFAULT);
        ID3D12Resource*resource{};
        if(FAILED(device->CreateCommittedResource(&heapProps,D3D12_HEAP_FLAG_NONE,&d,state,clear,
            __uuidof(ID3D12Resource),reinterpret_cast<void**>(&resource))))
            return nullptr;
        return resource;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE heapCpu(UINT index)const{
        D3D12_CPU_DESCRIPTOR_HANDLE handle=gpuHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr+=static_cast<SIZE_T>(index)*srvSize;
        return handle;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu(UINT index)const{
        D3D12_CPU_DESCRIPTOR_HANDLE handle=rtvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr+=static_cast<SIZE_T>(index)*rtvSize;
        return handle;
    }
    bool createOutputs(){
        release(output);release(accumulation);release(grassDepth);
        release(linearDepth);release(motionVectors);release(normalRough);
        release(diffuseAlbedo);release(specularAlbedo);release(uiColor);release(dlssOutput);
        release(denoisePing);release(readback);release(hdrReadback);
        lastHdr=nullptr;
        renderWidth=std::max(1,renderWidth);renderHeight=std::max(1,renderHeight);
        const UINT rw=static_cast<UINT>(renderWidth),rh=static_cast<UINT>(renderHeight);
        const UINT dw=static_cast<UINT>(std::max(1,width)),dh=static_cast<UINT>(std::max(1,height));
        output=makeTexture2D(rw,rh,DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if(!output)return fail(E_FAIL,L"DXR output texture creation failed");
        accumulation=makeTexture2D(rw,rh,DXGI_FORMAT_R32G32B32A32_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if(!accumulation)return fail(E_FAIL,L"DXR accumulation texture creation failed");
        D3D12_CLEAR_VALUE depthClear{};depthClear.Format=DXGI_FORMAT_D32_FLOAT;depthClear.DepthStencil.Depth=1.0f;
        grassDepth=makeTexture2D(rw,rh,DXGI_FORMAT_D32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,&depthClear);
        if(!grassDepth)return fail(E_FAIL,L"Grass depth texture creation failed");
        linearDepth=makeTexture2D(rw,rh,DXGI_FORMAT_R32_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        motionVectors=makeTexture2D(rw,rh,DXGI_FORMAT_R16G16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        normalRough=makeTexture2D(rw,rh,DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        diffuseAlbedo=makeTexture2D(rw,rh,DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        specularAlbedo=makeTexture2D(rw,rh,DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if(!linearDepth||!motionVectors||!normalRough||!diffuseAlbedo||!specularAlbedo)
            return fail(E_FAIL,L"DLSS G-buffer texture creation failed");
        D3D12_CLEAR_VALUE uiClear{};uiClear.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        uiColor=makeTexture2D(dw,dh,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RENDER_TARGET,&uiClear);
        dlssOutput=makeTexture2D(dw,dh,DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if(!uiColor||!dlssOutput)return fail(E_FAIL,L"DLSS display target creation failed");
        denoisePing=makeTexture2D(rw,rh,DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if(!denoisePing)return fail(E_FAIL,L"SM denoise target creation failed");
        D3D12_RESOURCE_DESC backDesc{};
        backDesc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        backDesc.Width=dw;backDesc.Height=dh;backDesc.DepthOrArraySize=1;backDesc.MipLevels=1;
        backDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;backDesc.SampleDesc.Count=1;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot{};UINT rows=0;UINT64 rowBytes=0,total=0;
        device->GetCopyableFootprints(&backDesc,0,1,0,&foot,&rows,&rowBytes,&total);
        readbackPitch=foot.Footprint.RowPitch;
        readback=makeBuffer(total,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        if(!readback)return fail(E_FAIL,L"PNG readback buffer creation failed");
        D3D12_RESOURCE_DESC hdrDesc{};
        hdrDesc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        hdrDesc.Width=rw;hdrDesc.Height=rh;hdrDesc.DepthOrArraySize=1;hdrDesc.MipLevels=1;
        hdrDesc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;hdrDesc.SampleDesc.Count=1;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT hdrFoot{};UINT hdrRows=0;UINT64 hdrRowBytes=0,hdrTotal=0;
        device->GetCopyableFootprints(&hdrDesc,0,1,0,&hdrFoot,&hdrRows,&hdrRowBytes,&hdrTotal);
        hdrReadbackPitch=hdrFoot.Footprint.RowPitch;
        hdrReadback=makeBuffer(hdrTotal,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        if(!hdrReadback)return fail(E_FAIL,L"HDR readback buffer creation failed");

        D3D12_DEPTH_STENCIL_VIEW_DESC depthView{};depthView.Format=DXGI_FORMAT_D32_FLOAT;
        depthView.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(grassDepth,&depthView,dsvHeap->GetCPUDescriptorHandleForHeapStart());
        device->CreateRenderTargetView(output,nullptr,rtvCpu(2));
        device->CreateRenderTargetView(uiColor,nullptr,rtvCpu(3));
        device->CreateRenderTargetView(dlssOutput,nullptr,rtvCpu(4));

        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        u.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;device->CreateUnorderedAccessView(output,nullptr,&u,heapCpu(0));
        u.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;device->CreateUnorderedAccessView(accumulation,nullptr,&u,heapCpu(1));
        u.Format=DXGI_FORMAT_R32_FLOAT;device->CreateUnorderedAccessView(linearDepth,nullptr,&u,heapCpu(2));
        u.Format=DXGI_FORMAT_R16G16_FLOAT;device->CreateUnorderedAccessView(motionVectors,nullptr,&u,heapCpu(3));
        u.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;device->CreateUnorderedAccessView(normalRough,nullptr,&u,heapCpu(4));
        device->CreateUnorderedAccessView(diffuseAlbedo,nullptr,&u,heapCpu(5));
        device->CreateUnorderedAccessView(specularAlbedo,nullptr,&u,heapCpu(6));

        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=1;
        view.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;device->CreateShaderResourceView(accumulation,&view,heapCpu(10));
        view.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;device->CreateShaderResourceView(output,&view,heapCpu(11));
        view.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;device->CreateShaderResourceView(output,&view,heapCpu(12));
        view.Format=DXGI_FORMAT_R32_FLOAT;device->CreateShaderResourceView(linearDepth,&view,heapCpu(13));
        view.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;device->CreateShaderResourceView(normalRough,&view,heapCpu(14));
        D3D12_UNORDERED_ACCESS_VIEW_DESC du{};du.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        du.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(denoisePing,nullptr,&du,heapCpu(15));
        frameIndex=0;havePrevCamera=false;return true;
    }
    bool createBarkNormal(){
        constexpr UINT textureWidth=2048,textureHeight=2048,mipLevels=12;const auto pixels=makeNormalMipChain(makeOakBarkNormal(textureWidth,textureHeight),textureWidth,textureHeight,mipLevels);D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=textureWidth;d.Height=textureHeight;d.DepthOrArraySize=1;d.MipLevels=mipLevels;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;auto defaultHeap=heap(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr=device->CreateCommittedResource(&defaultHeap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&barkNormal));if(FAILED(hr))return fail(hr,L"Runtime oak bark normal texture creation failed");
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipLevels);std::vector<UINT> rows(mipLevels);std::vector<UINT64> rowBytes(mipLevels);UINT64 uploadBytes{};device->GetCopyableFootprints(&d,0,mipLevels,0,footprints.data(),rows.data(),rowBytes.data(),&uploadBytes);ID3D12Resource*uploadBuffer=makeBuffer(uploadBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);if(!uploadBuffer)return fail(E_OUTOFMEMORY,L"Runtime oak bark upload allocation failed");
        void*mapped{};if(FAILED(uploadBuffer->Map(0,nullptr,&mapped))){release(uploadBuffer);return fail(E_FAIL,L"Runtime oak bark upload mapping failed");}for(UINT level=0;level<mipLevels;++level){const UINT levelWidth=std::max(1u,textureWidth>>level),levelHeight=std::max(1u,textureHeight>>level);for(UINT y=0;y<levelHeight;++y)std::memcpy(static_cast<char*>(mapped)+footprints[level].Offset+static_cast<size_t>(y)*footprints[level].Footprint.RowPitch,pixels[level].data()+static_cast<size_t>(y)*levelWidth,levelWidth*sizeof(uint32_t));}uploadBuffer->Unmap(0,nullptr);
        if(!begin()){release(uploadBuffer);return false;}for(UINT level=0;level<mipLevels;++level){D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=barkNormal;destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;destination.SubresourceIndex=level;D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=uploadBuffer;source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint=footprints[level];list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);}auto barrier=transition(barkNormal,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&barrier);if(!execute()){release(uploadBuffer);return false;}release(uploadBuffer);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Format=d.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=mipLevels;device->CreateShaderResourceView(barkNormal,&view,heapCpu(7));return true;
    }
    bool createGroundTextureArray(const std::vector<GroundTextureMip>&mips,
                                  ID3D12Resource*&resource,UINT descriptorIndex,
                                  const wchar_t*failureMessage){
        if(mips.empty()||mips.front().width!=GroundTextureAtlas::atlasWidth||
           mips.front().height!=GroundTextureAtlas::atlasHeight)return fail(E_INVALIDARG,failureMessage);
        const UINT mipLevels=static_cast<UINT>(mips.size()),arraySize=GroundTextureAtlas::tileCount;
        const UINT topTileWidth=mips.front().width/arraySize,topTileHeight=mips.front().height;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=topTileWidth;d.Height=topTileHeight;d.DepthOrArraySize=static_cast<UINT16>(arraySize);d.MipLevels=static_cast<UINT16>(mipLevels);d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;auto defaultHeap=heap(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr=device->CreateCommittedResource(&defaultHeap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&resource));if(FAILED(hr))return fail(hr,failureMessage);
        const UINT subresourceCount=mipLevels*arraySize;std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresourceCount);std::vector<UINT> rows(subresourceCount);std::vector<UINT64> rowBytes(subresourceCount);UINT64 uploadBytes{};device->GetCopyableFootprints(&d,0,subresourceCount,0,footprints.data(),rows.data(),rowBytes.data(),&uploadBytes);ID3D12Resource*uploadBuffer=makeBuffer(uploadBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);if(!uploadBuffer){release(resource);return fail(E_OUTOFMEMORY,failureMessage);}
        void*mapped{};if(FAILED(uploadBuffer->Map(0,nullptr,&mapped))){release(uploadBuffer);release(resource);return fail(E_FAIL,failureMessage);}for(UINT tile=0;tile<arraySize;++tile)for(UINT level=0;level<mipLevels;++level){const auto&mip=mips[level];const UINT tileWidth=mip.width/arraySize,tileHeight=mip.height,originX=tile*tileWidth,originY=0,subresource=level+tile*mipLevels;if(tileWidth==0||tileHeight==0||mip.width!=tileWidth*arraySize||mip.pixels.size()!=static_cast<size_t>(mip.width)*mip.height){uploadBuffer->Unmap(0,nullptr);release(uploadBuffer);release(resource);return fail(E_INVALIDARG,failureMessage);}for(UINT y=0;y<tileHeight;++y)std::memcpy(static_cast<char*>(mapped)+footprints[subresource].Offset+static_cast<size_t>(y)*footprints[subresource].Footprint.RowPitch,mip.pixels.data()+static_cast<size_t>(originY+y)*mip.width+originX,static_cast<size_t>(tileWidth)*sizeof(uint32_t));}uploadBuffer->Unmap(0,nullptr);
        if(!begin()){release(uploadBuffer);release(resource);return false;}for(UINT subresource=0;subresource<subresourceCount;++subresource){D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=resource;destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;destination.SubresourceIndex=subresource;D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=uploadBuffer;source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint=footprints[subresource];list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);}auto barrier=transition(resource,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&barrier);if(!execute()){release(uploadBuffer);release(resource);return false;}release(uploadBuffer);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Format=d.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;view.Texture2DArray.MipLevels=mipLevels;view.Texture2DArray.ArraySize=arraySize;auto cpu=gpuHeap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=descriptorIndex*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);device->CreateShaderResourceView(resource,&view,cpu);return true;
    }
    bool createGroundMaterials(){
        const GroundTextureAtlas atlas=makeGroundTextureAtlas();
        if(!createGroundTextureArray(atlas.albedoRoughness,groundAlbedo,8,L"Runtime ground albedo texture creation failed"))return false;
        if(!createGroundTextureArray(atlas.normalHeightCavity,groundNormal,9,L"Runtime ground normal texture creation failed")){release(groundAlbedo);return false;}
        return true;
    }
    std::vector<char>loadShader(const wchar_t*name){wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);auto path=std::filesystem::path(exe).parent_path().parent_path()/L"shaders"/name;std::ifstream stream(path,std::ios::binary|std::ios::ate);if(!stream)return{};const auto size=stream.tellg();stream.seekg(0);std::vector<char>data(static_cast<size_t>(size));stream.read(data.data(),size);return data;}
    std::vector<char>loadDxil(){return loadShader(L"raytracing.dxil");}
    bool createPipeline(){D3D12_DESCRIPTOR_RANGE ranges[3]{};ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;ranges[0].NumDescriptors=7;ranges[0].BaseShaderRegister=0;ranges[0].OffsetInDescriptorsFromTableStart=0;ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[1].NumDescriptors=1;ranges[1].BaseShaderRegister=3;ranges[1].OffsetInDescriptorsFromTableStart=7;ranges[2].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[2].NumDescriptors=2;ranges[2].BaseShaderRegister=5;ranges[2].OffsetInDescriptorsFromTableStart=8;
        D3D12_ROOT_PARAMETER params[15]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable.NumDescriptorRanges=3;params[0].DescriptorTable.pDescriptorRanges=ranges;for(int p=1;p<=3;++p){params[p].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[p].Descriptor.ShaderRegister=static_cast<UINT>(p-1);}params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[4].Descriptor.ShaderRegister=0;params[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[5].Descriptor.ShaderRegister=4;params[6].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[6].Descriptor.ShaderRegister=1;params[7].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[7].Descriptor.ShaderRegister=7;for(int p=8;p<14;++p){params[p].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[p].Descriptor.ShaderRegister=static_cast<UINT>(p);}params[14].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[14].Descriptor.ShaderRegister=14;
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_ANISOTROPIC;sampler.AddressU=D3D12_TEXTURE_ADDRESS_MODE_WRAP;sampler.AddressV=D3D12_TEXTURE_ADDRESS_MODE_WRAP;sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;sampler.MaxAnisotropy=8;sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MinLOD=0;sampler.MaxLOD=10;sampler.ShaderRegister=0;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=15;rs.pParameters=params;rs.NumStaticSamplers=1;rs.pStaticSamplers=&sampler;ID3DBlob*blob{},*errors{};HRESULT hr=D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors);if(FAILED(hr)){release(errors);return fail(hr,L"DXR root-signature serialization failed");}hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),__uuidof(ID3D12RootSignature),reinterpret_cast<void**>(&root));release(blob);release(errors);if(FAILED(hr))return fail(hr,L"DXR root-signature creation failed");
        auto dxil=loadDxil();if(dxil.empty()){lastError=L"Compiled raytracing.dxil was not found beside the build output.";return false;}
        const wchar_t*exports[]={L"RayGen",L"RadianceMiss",L"VisibilityMiss",L"RadianceHit",L"VisibilityHit",L"GrassIntersection",L"GrassRadianceHit"};D3D12_EXPORT_DESC exportDescs[7]{};for(int n=0;n<7;++n)exportDescs[n].Name=exports[n];D3D12_DXIL_LIBRARY_DESC library{};library.DXILLibrary={dxil.data(),dxil.size()};library.NumExports=7;library.pExports=exportDescs;
        D3D12_HIT_GROUP_DESC hit0{};hit0.HitGroupExport=L"RadianceHitGroup";hit0.ClosestHitShaderImport=L"RadianceHit";hit0.Type=D3D12_HIT_GROUP_TYPE_TRIANGLES;D3D12_HIT_GROUP_DESC hit1{};hit1.HitGroupExport=L"VisibilityHitGroup";hit1.ClosestHitShaderImport=L"VisibilityHit";hit1.Type=D3D12_HIT_GROUP_TYPE_TRIANGLES;
        D3D12_HIT_GROUP_DESC hit2{};hit2.HitGroupExport=L"GrassRadianceHitGroup";hit2.IntersectionShaderImport=L"GrassIntersection";hit2.ClosestHitShaderImport=L"GrassRadianceHit";hit2.Type=D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;D3D12_HIT_GROUP_DESC hit3{};hit3.HitGroupExport=L"GrassVisibilityHitGroup";hit3.IntersectionShaderImport=L"GrassIntersection";hit3.Type=D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{56,8};D3D12_GLOBAL_ROOT_SIGNATURE global{root};D3D12_RAYTRACING_PIPELINE_CONFIG pipeline{3};D3D12_STATE_SUBOBJECT subs[9]{};subs[0]={D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&library};subs[1]={D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&hit0};subs[2]={D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&hit1};subs[3]={D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&hit2};subs[4]={D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&hit3};subs[5]={D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,&shaderConfig};const wchar_t*associations[]={L"RayGen",L"RadianceMiss",L"VisibilityMiss",L"RadianceHitGroup",L"VisibilityHitGroup",L"GrassRadianceHitGroup",L"GrassVisibilityHitGroup"};D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association{&subs[5],7,associations};subs[6]={D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,&association};subs[7]={D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&global};subs[8]={D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,&pipeline};D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,9,subs};hr=device->CreateStateObject(&desc,__uuidof(ID3D12StateObject),reinterpret_cast<void**>(&state));if(FAILED(hr))return fail(hr,L"DXR state-object creation failed");hr=state->QueryInterface(__uuidof(ID3D12StateObjectProperties),reinterpret_cast<void**>(&stateProps));if(FAILED(hr))return fail(hr,L"DXR state-object properties unavailable");return createShaderTables();}

    bool createGrassPipeline(){
        D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=2;range.BaseShaderRegister=0;range.OffsetInDescriptorsFromTableStart=0;
        D3D12_DESCRIPTOR_RANGE gbufferRange{};gbufferRange.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;gbufferRange.NumDescriptors=5;gbufferRange.BaseShaderRegister=0;gbufferRange.OffsetInDescriptorsFromTableStart=0;
        D3D12_ROOT_PARAMETER params[7]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[0].Descriptor.ShaderRegister=0;params[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[1].Descriptor.ShaderRegister=2;params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
        params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[2].DescriptorTable.NumDescriptorRanges=1;params[2].DescriptorTable.pDescriptorRanges=&range;params[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        params[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[3].Constants.ShaderRegister=2;params[3].Constants.Num32BitValues=2;
        params[3].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
        params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[4].Descriptor.ShaderRegister=1;params[4].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        params[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[5].Descriptor.ShaderRegister=3;
        params[5].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        params[6].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[6].DescriptorTable.NumDescriptorRanges=1;
        params[6].DescriptorTable.pDescriptorRanges=&gbufferRange;
        params[6].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=7;signature.pParameters=params;signature.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob*blob{},*errors{};HRESULT hr=D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors);if(FAILED(hr)){release(errors);return fail(hr,L"Grass root-signature serialization failed");}
        hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),__uuidof(ID3D12RootSignature),reinterpret_cast<void**>(&grassRoot));release(blob);release(errors);if(FAILED(hr))return fail(hr,L"Grass root-signature creation failed");
        const auto vertexShader=loadShader(L"grass_overlay_vs.dxil"),pixelShader=loadShader(L"grass_overlay_ps.dxil");if(vertexShader.empty()||pixelShader.empty()){lastError=L"Compiled grass overlay shaders were not found beside the build output.";return false;}
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};desc.pRootSignature=grassRoot;desc.VS={vertexShader.data(),vertexShader.size()};desc.PS={pixelShader.data(),pixelShader.size()};
        auto&target=desc.BlendState.RenderTarget[0];target.BlendEnable=TRUE;target.LogicOpEnable=FALSE;target.SrcBlend=D3D12_BLEND_SRC_ALPHA;target.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;target.BlendOp=D3D12_BLEND_OP_ADD;target.SrcBlendAlpha=D3D12_BLEND_ONE;target.DestBlendAlpha=D3D12_BLEND_ZERO;target.BlendOpAlpha=D3D12_BLEND_OP_ADD;target.LogicOp=D3D12_LOGIC_OP_NOOP;target.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask=UINT_MAX;desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;desc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;desc.RasterizerState.DepthClipEnable=TRUE;
        desc.DepthStencilState.DepthEnable=TRUE;desc.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;desc.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;desc.DepthStencilState.StencilEnable=FALSE;
        desc.InputLayout={nullptr,0};desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;desc.NumRenderTargets=1;desc.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.DSVFormat=DXGI_FORMAT_D32_FLOAT;desc.SampleDesc.Count=1;
        hr=device->CreateGraphicsPipelineState(&desc,__uuidof(ID3D12PipelineState),reinterpret_cast<void**>(&grassPipeline));if(FAILED(hr))return fail(hr,L"Grass graphics pipeline creation failed");return true;
    }
    bool createPresentPipeline(){
        D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;range.BaseShaderRegister=0;
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges=1;params[0].DescriptorTable.pDescriptorRanges=&range;
        params[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[1].Descriptor.ShaderRegister=0;
        params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler.AddressU=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.AddressV=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.ShaderRegister=0;
        sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=2;signature.pParameters=params;
        signature.NumStaticSamplers=1;signature.pStaticSamplers=&sampler;
        ID3DBlob*blob{},*errors{};
        HRESULT hr=D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors);
        if(FAILED(hr)){release(errors);return fail(hr,L"Present root-signature serialization failed");}
        hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),
            __uuidof(ID3D12RootSignature),reinterpret_cast<void**>(&presentRoot));
        release(blob);release(errors);
        if(FAILED(hr))return fail(hr,L"Present root-signature creation failed");
        const auto vertexShader=loadShader(L"present_tonemap_vs.dxil");
        const auto pixelShader=loadShader(L"present_tonemap_ps.dxil");
        if(vertexShader.empty()||pixelShader.empty()){
            lastError=L"Compiled present tonemap shaders were not found beside the build output.";
            return false;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature=presentRoot;
        desc.VS={vertexShader.data(),vertexShader.size()};
        desc.PS={pixelShader.data(),pixelShader.size()};
        auto&target=desc.BlendState.RenderTarget[0];
        target.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask=UINT_MAX;
        desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        desc.DepthStencilState.DepthEnable=FALSE;
        desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets=1;desc.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1;
        hr=device->CreateGraphicsPipelineState(&desc,__uuidof(ID3D12PipelineState),
            reinterpret_cast<void**>(&presentPipeline));
        if(FAILED(hr))return fail(hr,L"Present pipeline creation failed");
        return true;
    }
    bool createDenoisePipeline(){
        D3D12_DESCRIPTOR_RANGE srvs{};srvs.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvs.NumDescriptors=3;srvs.BaseShaderRegister=0;srvs.OffsetInDescriptorsFromTableStart=0;
        D3D12_DESCRIPTOR_RANGE uavs{};uavs.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavs.NumDescriptors=1;uavs.BaseShaderRegister=0;uavs.OffsetInDescriptorsFromTableStart=0;
        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges=1;params[0].DescriptorTable.pDescriptorRanges=&srvs;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges=1;params[1].DescriptorTable.pDescriptorRanges=&uavs;
        params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[2].Descriptor.ShaderRegister=0;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter=D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler.AddressU=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.NumParameters=3;signature.pParameters=params;
        ID3DBlob*blob{},*errors{};
        HRESULT hr=D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors);
        if(FAILED(hr)){release(errors);return fail(hr,L"Denoise root-signature serialization failed");}
        hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),
            __uuidof(ID3D12RootSignature),reinterpret_cast<void**>(&denoiseRoot));
        release(blob);release(errors);
        if(FAILED(hr))return fail(hr,L"Denoise root-signature creation failed");
        const auto cs=loadShader(L"sm_denoise.dxil");
        if(cs.empty()){lastError=L"Compiled sm_denoise.dxil was not found.";return false;}
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature=denoiseRoot;
        desc.CS={cs.data(),cs.size()};
        hr=device->CreateComputePipelineState(&desc,__uuidof(ID3D12PipelineState),
            reinterpret_cast<void**>(&denoisePipeline));
        if(FAILED(hr))return fail(hr,L"Denoise compute pipeline creation failed");
        return true;
    }
    bool createHudPipeline(){
        D3D12_ROOT_PARAMETER params[1]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister=0;
        params[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.NumParameters=1;signature.pParameters=params;
        ID3DBlob*blob{},*errors{};
        HRESULT hr=D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors);
        if(FAILED(hr)){release(errors);return fail(hr,L"HUD root-signature serialization failed");}
        hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),
            __uuidof(ID3D12RootSignature),reinterpret_cast<void**>(&hudRoot));
        release(blob);release(errors);
        if(FAILED(hr))return fail(hr,L"HUD root-signature creation failed");
        const auto vertexShader=loadShader(L"hud_overlay_vs.dxil");
        const auto pixelShader=loadShader(L"hud_overlay_ps.dxil");
        if(vertexShader.empty()||pixelShader.empty()){
            lastError=L"Compiled HUD overlay shaders were not found beside the build output.";
            return false;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature=hudRoot;
        desc.VS={vertexShader.data(),vertexShader.size()};
        desc.PS={pixelShader.data(),pixelShader.size()};
        auto&target=desc.BlendState.RenderTarget[0];
        target.BlendEnable=TRUE;target.SrcBlend=D3D12_BLEND_SRC_ALPHA;
        target.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;target.BlendOp=D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha=D3D12_BLEND_ONE;target.DestBlendAlpha=D3D12_BLEND_ZERO;
        target.BlendOpAlpha=D3D12_BLEND_OP_ADD;
        target.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask=UINT_MAX;
        desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        desc.DepthStencilState.DepthEnable=FALSE;
        desc.InputLayout={nullptr,0};
        desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets=1;desc.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1;
        hr=device->CreateGraphicsPipelineState(&desc,__uuidof(ID3D12PipelineState),
            reinterpret_cast<void**>(&hudPipeline));
        if(FAILED(hr))return fail(hr,L"HUD graphics pipeline creation failed");
        return true;
    }
    bool createTreeWindPipeline(){
        D3D12_ROOT_PARAMETER params[4]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[0].Descriptor.ShaderRegister=0;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor.ShaderRegister=0;
        params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[2].Constants.ShaderRegister=0;params[2].Constants.Num32BitValues=4;
        params[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[3].Descriptor.ShaderRegister=1;
        D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=4;
        signature.pParameters=params;
        ID3DBlob*blob{},*errors{};
        HRESULT hr=D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,
                                               &blob,&errors);
        if(FAILED(hr)){release(errors);return fail(hr,L"Tree-wind root-signature serialization failed");}
        hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),
            __uuidof(ID3D12RootSignature),reinterpret_cast<void**>(&treeWindRoot));
        release(blob);release(errors);
        if(FAILED(hr))return fail(hr,L"Tree-wind root-signature creation failed");
        const auto shader=loadShader(L"tree_wind.dxil");
        if(shader.empty()){lastError=L"Compiled tree_wind.dxil was not found beside the build output.";return false;}
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=treeWindRoot;
        desc.CS={shader.data(),shader.size()};
        hr=device->CreateComputePipelineState(&desc,__uuidof(ID3D12PipelineState),
                                              reinterpret_cast<void**>(&treeWindPipeline));
        if(FAILED(hr))return fail(hr,L"Tree-wind compute pipeline creation failed");
        return true;
    }
    ID3D12Resource*shaderTable(const std::vector<const void*>&ids){const UINT stride=D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;ID3D12Resource*r=makeBuffer(alignUp(ids.size()*stride,D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);if(!r)return nullptr;void*m{};r->Map(0,nullptr,&m);for(size_t i=0;i<ids.size();++i)std::memcpy(static_cast<char*>(m)+i*stride,ids[i],stride);r->Unmap(0,nullptr);return r;}
    bool createShaderTables(){const void*rg=stateProps->GetShaderIdentifier(L"RayGen"),*rm=stateProps->GetShaderIdentifier(L"RadianceMiss"),*vm=stateProps->GetShaderIdentifier(L"VisibilityMiss"),*rh=stateProps->GetShaderIdentifier(L"RadianceHitGroup"),*vh=stateProps->GetShaderIdentifier(L"VisibilityHitGroup"),*grh=stateProps->GetShaderIdentifier(L"GrassRadianceHitGroup"),*gvh=stateProps->GetShaderIdentifier(L"GrassVisibilityHitGroup");if(!rg||!rm||!vm||!rh||!vh||!grh||!gvh){lastError=L"DXR shader identifier lookup failed.";return false;}raygenTable=shaderTable({rg});missTable=shaderTable({rm,vm});hitTable=shaderTable({rh,vh,grh,gvh});return raygenTable&&missTable&&hitTable;}
    bool buildBottomLevel(const D3D12_RAYTRACING_GEOMETRY_DESC&geometry,
                          ID3D12Resource*&scratch,ID3D12Resource*&result,
                          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags=
                              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE){
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
        input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        input.Flags=flags;
        input.NumDescs=1;input.pGeometryDescs=&geometry;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&input,&info);
        scratch=makeBuffer(std::max(info.ScratchDataSizeInBytes,info.UpdateScratchDataSizeInBytes),D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        result=makeBuffer(info.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if(!scratch||!result)return false;
        if(!begin())return false;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs=input;
        build.ScratchAccelerationStructureData=scratch->GetGPUVirtualAddress();
        build.DestAccelerationStructureData=result->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&build,0,nullptr);
        D3D12_RESOURCE_BARRIER uav{};uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource=result;list->ResourceBarrier(1,&uav);
        return execute();
    }

    D3D12_RAYTRACING_GEOMETRY_DESC geometryFor(const TriangleMeshRange&range)const{
        D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
        geometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geometry.Triangles.VertexBuffer.StartAddress=vertexBuffer->GetGPUVirtualAddress()+
            static_cast<UINT64>(range.vertexBase)*sizeof(MeshVertex);
        geometry.Triangles.VertexBuffer.StrideInBytes=sizeof(MeshVertex);
        geometry.Triangles.VertexCount=range.vertexCount;
        geometry.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
        geometry.Triangles.IndexBuffer=indexBuffer->GetGPUVirtualAddress()+
            static_cast<UINT64>(range.indexBase)*sizeof(uint32_t);
        geometry.Triangles.IndexCount=range.indexCount;
        geometry.Triangles.IndexFormat=DXGI_FORMAT_R32_UINT;
        return geometry;
    }
    D3D12_RAYTRACING_GEOMETRY_DESC geometryForResources(
        ID3D12Resource*vertices,ID3D12Resource*indices,
        const TriangleMeshRange&range)const{
        D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
        geometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geometry.Triangles.VertexBuffer.StartAddress=vertices->GetGPUVirtualAddress();
        geometry.Triangles.VertexBuffer.StrideInBytes=sizeof(MeshVertex);
        geometry.Triangles.VertexCount=range.vertexCount;
        geometry.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
        geometry.Triangles.IndexBuffer=indices->GetGPUVirtualAddress();
        geometry.Triangles.IndexCount=range.indexCount;
        geometry.Triangles.IndexFormat=DXGI_FORMAT_R32_UINT;
        return geometry;
    }

    static void writeTransform(D3D12_RAYTRACING_INSTANCE_DESC&instance,
                               const RendererRigidTransform&transform){
        instance.Transform[0][0]=transform.xAxis.x;
        instance.Transform[0][1]=transform.yAxis.x;
        instance.Transform[0][2]=transform.zAxis.x;
        instance.Transform[0][3]=transform.origin.x;
        instance.Transform[1][0]=transform.xAxis.y;
        instance.Transform[1][1]=transform.yAxis.y;
        instance.Transform[1][2]=transform.zAxis.y;
        instance.Transform[1][3]=transform.origin.y;
        instance.Transform[2][0]=transform.xAxis.z;
        instance.Transform[2][1]=transform.yAxis.z;
        instance.Transform[2][2]=transform.zAxis.z;
        instance.Transform[2][3]=transform.origin.z;
    }

    static RendererRigidTransform instanceTransform(const TreeInstance&tree){
        const float cosine=std::cos(tree.yaw)*tree.scale;
        const float sine=std::sin(tree.yaw)*tree.scale;
        return {{tree.position.x,tree.position.y,tree.position.z},
                {cosine,0,-sine},{0,tree.scale,0},{sine,0,cosine}};
    }

    bool rebuildTopLevel(bool rebuildBuffers=true){
        if(!blas||!leafBlas||!staticBlas)return false;
        wait();
        std::vector<D3D12_RAYTRACING_INSTANCE_DESC>instances;
        std::vector<InstanceGeometryGpu>metadata;
        const TreeMesh&source=sourceTree?*sourceTree:TreeMesh{};
        instances.reserve((source.additionalInstances.size()+1)*2+6);
        metadata.reserve((source.additionalInstances.size()+1)*2+6);
        auto add=[&](ID3D12Resource*acceleration,const TriangleMeshRange&range,
                     const RendererRigidTransform&transform,uint32_t visualInstance,
                     bool enabled=true,UINT*slot=nullptr,uint32_t geometrySlot=0){
            if(!acceleration||!range.valid())return;
            D3D12_RAYTRACING_INSTANCE_DESC instance{};
            writeTransform(instance,transform);
            instance.InstanceID=static_cast<UINT>(metadata.size());
            instance.InstanceMask=enabled?0x1:0;
            instance.InstanceContributionToHitGroupIndex=0;
            instance.AccelerationStructure=acceleration->GetGPUVirtualAddress();
            instances.push_back(instance);
            metadata.push_back(geometrySlot==0?
                InstanceGeometryGpu{range.indexBase,range.vertexBase,visualInstance,0}:
                InstanceGeometryGpu{0,0,visualInstance,geometrySlot});
            if(slot)*slot=static_cast<UINT>(instances.size()-1);
        };
        if(!(promotedTreeActive&&promotedSharedIndex==0))
            add(blas,sharedBranchRange,RendererRigidTransform{},0);
        if(!(promotedTreeActive&&promotedSharedIndex==0))
            add(leafBlas,sharedLeafRange,RendererRigidTransform{},0);
        for(std::size_t i=0;i<source.additionalInstances.size();++i){
            const std::size_t sharedIndex=i+1;
            if(!(promotedTreeActive&&promotedSharedIndex==sharedIndex)){
                const RendererRigidTransform transform=instanceTransform(source.additionalInstances[i]);
                add(blas,sharedBranchRange,transform,static_cast<uint32_t>(sharedIndex));
                add(leafBlas,sharedLeafRange,transform,static_cast<uint32_t>(sharedIndex));
            }
        }
        // Static environment retains a stable visual identifier used by
        // foliage wind shading; geometry addressing no longer relies on it.
        add(staticBlas,environmentRange,RendererRigidTransform{},0x80000000u);
        standingInstanceSlot=detachedInstanceSlot=promotedLeafInstanceSlot=
            axeInstanceSlot=chipInstanceSlot=UINT_MAX;
        if(promotedTreeActive){
            const uint32_t visualInstance=static_cast<uint32_t>(promotedSharedIndex);
            add(standingBlas,standingRange,standingTransform,visualInstance,true,
                &standingInstanceSlot,1);
            add(detachedBlas,detachedRange,detachedTransform,visualInstance,true,
                &detachedInstanceSlot,2);
            // The four-million-triangle canopy stays in its original shared
            // BLAS.  Promotion gives only this one instance an independent
            // transform, so falling never uploads or refits leaf geometry.
            // Before structural separation the original leaf inventory still
            // belongs to the standing trunk.  Once a detached wood mesh exists,
            // move the shared canopy rigidly with that falling body.
            const RendererRigidTransform& canopyTransform=promotedCanopyDetached?
                detachedTransform:standingTransform;
            add(leafBlas,sharedLeafRange,canopyTransform,visualInstance,true,
                &promotedLeafInstanceSlot);
        }
        add(axeBlas,axeRange,axeTransform,0x40000003u,
            axeVisible&&axeGeometryReady,&axeInstanceSlot,3);
        // Chip indices are local to a reserved range in the shared scene
        // buffers, so the existing global-geometry shader path resolves them
        // without another root SRV or shader-table variant.
        add(chipBlas,chipRange,woodChipTransform,0x40000004u,
            woodChipsVisible&&woodChipGeometryReady,&chipInstanceSlot);
        for(const GrassBlasChunk&chunk:grassBlasChunks){
            if(!chunk.blas||!chunk.bladeCount)continue;
            D3D12_RAYTRACING_INSTANCE_DESC grassInstance{};
            writeTransform(grassInstance,RendererRigidTransform{});
            grassInstance.InstanceID=chunk.bladeBase;
            grassInstance.InstanceMask=0x2;
            grassInstance.InstanceContributionToHitGroupIndex=2;
            grassInstance.AccelerationStructure=chunk.blas->GetGPUVirtualAddress();
            instances.push_back(grassInstance);
            metadata.push_back({0,0,chunk.bladeBase,0});
        }
        if(instances.empty())return false;

        release(instanceGeometryBuffer);
        if(instanceBuffer&&instanceMapped)instanceBuffer->Unmap(0,nullptr);
        instanceMapped=nullptr;release(instanceBuffer);
        instanceBuffer=makeBuffer(instances.size()*sizeof(instances.front()),
            D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        instanceGeometryBuffer=uploadDefault(metadata);
        if(!instanceBuffer||!instanceGeometryBuffer||
           FAILED(instanceBuffer->Map(0,nullptr,&instanceMapped)))return false;
        std::memcpy(instanceMapped,instances.data(),
                    instances.size()*sizeof(instances.front()));
        sceneInstances=instances;instanceGeometry=metadata;
        tlasInstanceCount=static_cast<UINT>(instances.size());

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
        input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        input.Flags=static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);
        input.NumDescs=tlasInstanceCount;input.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
        input.InstanceDescs=instanceBuffer->GetGPUVirtualAddress();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&input,&info);
        if(rebuildBuffers||!tlas||!tlasScratch){
            release(tlasScratch);release(tlas);
            tlasScratch=makeBuffer(std::max(info.ScratchDataSizeInBytes,
                info.UpdateScratchDataSizeInBytes),D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            tlas=makeBuffer(info.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        }
        if(!tlasScratch||!tlas||!begin())return false;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs=input;
        build.ScratchAccelerationStructureData=tlasScratch->GetGPUVirtualAddress();
        build.DestAccelerationStructureData=tlas->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&build,0,nullptr);
        D3D12_RESOURCE_BARRIER uav{};uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource=tlas;list->ResourceBarrier(1,&uav);
        return execute();
    }

    bool refitTopLevelTransforms(){
        if(!tlas||!tlasScratch||!instanceBuffer||!instanceMapped||
           sceneInstances.empty())return false;
        if(standingInstanceSlot<sceneInstances.size())
            writeTransform(sceneInstances[standingInstanceSlot],standingTransform);
        if(detachedInstanceSlot<sceneInstances.size())
            writeTransform(sceneInstances[detachedInstanceSlot],detachedTransform);
        if(promotedLeafInstanceSlot<sceneInstances.size())
            writeTransform(sceneInstances[promotedLeafInstanceSlot],
                           promotedCanopyDetached?detachedTransform:standingTransform);
        if(axeInstanceSlot<sceneInstances.size()){
            writeTransform(sceneInstances[axeInstanceSlot],axeTransform);
            sceneInstances[axeInstanceSlot].InstanceMask=
                axeVisible&&axeGeometryReady?0x1:0;
        }
        if(chipInstanceSlot<sceneInstances.size()){
            writeTransform(sceneInstances[chipInstanceSlot],woodChipTransform);
            sceneInstances[chipInstanceSlot].InstanceMask=
                woodChipsVisible&&woodChipGeometryReady?0x1:0;
        }
        wait();
        std::memcpy(instanceMapped,sceneInstances.data(),
                    sceneInstances.size()*sizeof(sceneInstances.front()));
        if(!begin())return false;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
        input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        input.Flags=static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE);
        input.NumDescs=tlasInstanceCount;input.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
        input.InstanceDescs=instanceBuffer->GetGPUVirtualAddress();
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs=input;
        build.SourceAccelerationStructureData=tlas->GetGPUVirtualAddress();
        build.DestAccelerationStructureData=tlas->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData=tlasScratch->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&build,0,nullptr);
        D3D12_RESOURCE_BARRIER uav{};uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource=tlas;list->ResourceBarrier(1,&uav);
        return execute();
    }

    static bool validWoodChipMesh(const TreeMesh&mesh){
        if(mesh.branchVertices.empty()||mesh.branchIndices.empty()||
           mesh.branchVertices.size()>woodChipVertexCapacity||
           mesh.branchIndices.size()>woodChipIndexCapacity||
           mesh.branchIndices.size()%3!=0||
           !mesh.leafVertices.empty()||!mesh.leafIndices.empty()||
           !mesh.additionalInstances.empty())return false;
        for(uint32_t index:mesh.branchIndices)
            if(index>=mesh.branchVertices.size())return false;
        for(const MeshVertex&vertex:mesh.branchVertices){
            const Vec3&position=vertex.position;
            const Vec3&normal=vertex.normal;
            if(!std::isfinite(position.x)||!std::isfinite(position.y)||
               !std::isfinite(position.z)||!std::isfinite(normal.x)||
               !std::isfinite(normal.y)||!std::isfinite(normal.z)||
               !std::isfinite(vertex.material)||!std::isfinite(vertex.u)||
               !std::isfinite(vertex.v))return false;
        }
        return true;
    }

    bool uploadWoodChipData(const TreeMesh&mesh,bool uploadIndices){
        if(!woodChipArenaReady||!vertexBuffer||!indexBuffer)return false;
        const UINT64 vertexBytes=static_cast<UINT64>(mesh.branchVertices.size())*
                                 sizeof(MeshVertex);
        const UINT64 indexBytes=static_cast<UINT64>(mesh.branchIndices.size())*
                                sizeof(uint32_t);
        ID3D12Resource*vertexStaging=makeBuffer(vertexBytes,D3D12_HEAP_TYPE_UPLOAD,
                                                D3D12_RESOURCE_STATE_GENERIC_READ);
        ID3D12Resource*indexStaging=uploadIndices?
            makeBuffer(indexBytes,D3D12_HEAP_TYPE_UPLOAD,
                       D3D12_RESOURCE_STATE_GENERIC_READ):nullptr;
        if(!vertexStaging||(uploadIndices&&!indexStaging)){
            release(indexStaging);release(vertexStaging);return false;
        }
        void*mapped{};
        if(FAILED(vertexStaging->Map(0,nullptr,&mapped))){
            release(indexStaging);release(vertexStaging);return false;
        }
        std::memcpy(mapped,mesh.branchVertices.data(),static_cast<size_t>(vertexBytes));
        vertexStaging->Unmap(0,nullptr);
        if(uploadIndices){
            mapped=nullptr;
            if(FAILED(indexStaging->Map(0,nullptr,&mapped))){
                release(indexStaging);release(vertexStaging);return false;
            }
            std::memcpy(mapped,mesh.branchIndices.data(),static_cast<size_t>(indexBytes));
            indexStaging->Unmap(0,nullptr);
        }
        if(!begin()){
            release(indexStaging);release(vertexStaging);return false;
        }
        auto vertexToCopy=transition(vertexBuffer,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1,&vertexToCopy);
        list->CopyBufferRegion(vertexBuffer,
            static_cast<UINT64>(woodChipVertexBase)*sizeof(MeshVertex),
            vertexStaging,0,vertexBytes);
        auto vertexToRead=transition(vertexBuffer,D3D12_RESOURCE_STATE_COPY_DEST,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1,&vertexToRead);
        if(uploadIndices){
            auto indexToCopy=transition(indexBuffer,D3D12_RESOURCE_STATE_GENERIC_READ,
                                        D3D12_RESOURCE_STATE_COPY_DEST);
            list->ResourceBarrier(1,&indexToCopy);
            list->CopyBufferRegion(indexBuffer,
                static_cast<UINT64>(woodChipIndexBase)*sizeof(uint32_t),
                indexStaging,0,indexBytes);
            auto indexToRead=transition(indexBuffer,D3D12_RESOURCE_STATE_COPY_DEST,
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
            list->ResourceBarrier(1,&indexToRead);
        }
        const bool ok=execute();
        release(indexStaging);release(vertexStaging);
        return ok;
    }

    bool installWoodChipMesh(std::shared_ptr<const TreeMesh>mesh){
        if(!mesh||!validWoodChipMesh(*mesh))return false;
        woodChipMesh=std::move(mesh);
        woodChipTopologyIndices=woodChipMesh->branchIndices;
        woodChipsVisible=true;
        if(!initialized||!woodChipArenaReady||!vertexBuffer||!indexBuffer){
            woodChipGeometryReady=false;return true;
        }
        if(!uploadWoodChipData(*woodChipMesh,true))return false;
        wait();release(chipBlasScratch);release(chipBlas);
        chipRange={woodChipVertexBase,
                   static_cast<UINT>(woodChipMesh->branchVertices.size()),
                   woodChipIndexBase,
                   static_cast<UINT>(woodChipMesh->branchIndices.size())};
        constexpr auto flags=static_cast<
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD|
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);
        if(!buildBottomLevel(geometryFor(chipRange),chipBlasScratch,chipBlas,flags)){
            release(chipBlasScratch);release(chipBlas);chipRange={};
            woodChipGeometryReady=false;
            if(blas&&leafBlas&&staticBlas)rebuildTopLevel();
            return false;
        }
        woodChipGeometryReady=true;
        return rebuildTopLevel();
    }

    bool refitWoodChipMesh(std::shared_ptr<const TreeMesh>mesh){
        if(!mesh||!validWoodChipMesh(*mesh)||!woodChipGeometryReady||
           !chipBlas||!chipBlasScratch||!woodChipMesh||!tlas||!tlasScratch||
           !instanceBuffer||!instanceMapped||sceneInstances.empty()||
           mesh->branchVertices.size()!=chipRange.vertexCount||
           mesh->branchIndices!=woodChipTopologyIndices)return false;
        const UINT64 vertexBytes=static_cast<UINT64>(mesh->branchVertices.size())*
                                 sizeof(MeshVertex);
        ID3D12Resource*staging=makeBuffer(vertexBytes,D3D12_HEAP_TYPE_UPLOAD,
                                          D3D12_RESOURCE_STATE_GENERIC_READ);
        if(!staging)return false;
        void*mapped{};
        if(FAILED(staging->Map(0,nullptr,&mapped))){release(staging);return false;}
        std::memcpy(mapped,mesh->branchVertices.data(),static_cast<size_t>(vertexBytes));
        staging->Unmap(0,nullptr);
        wait();
        std::memcpy(instanceMapped,sceneInstances.data(),
                    sceneInstances.size()*sizeof(sceneInstances.front()));
        if(!begin()){release(staging);return false;}
        auto toCopy=transition(vertexBuffer,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1,&toCopy);
        list->CopyBufferRegion(vertexBuffer,
            static_cast<UINT64>(woodChipVertexBase)*sizeof(MeshVertex),
            staging,0,vertexBytes);
        auto toRead=transition(vertexBuffer,D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1,&toRead);
        D3D12_RAYTRACING_GEOMETRY_DESC geometry=geometryFor(chipRange);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
        input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        input.Flags=static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE);
        input.NumDescs=1;input.pGeometryDescs=&geometry;
        if(!begin())return false;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs=input;
        build.SourceAccelerationStructureData=chipBlas->GetGPUVirtualAddress();
        build.DestAccelerationStructureData=chipBlas->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData=chipBlasScratch->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&build,0,nullptr);
        D3D12_RESOURCE_BARRIER uav{};uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource=chipBlas;list->ResourceBarrier(1,&uav);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topInput{};
        topInput.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        topInput.Flags=static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE);
        topInput.NumDescs=tlasInstanceCount;
        topInput.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
        topInput.InstanceDescs=instanceBuffer->GetGPUVirtualAddress();
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topBuild{};
        topBuild.Inputs=topInput;
        topBuild.SourceAccelerationStructureData=tlas->GetGPUVirtualAddress();
        topBuild.DestAccelerationStructureData=tlas->GetGPUVirtualAddress();
        topBuild.ScratchAccelerationStructureData=tlasScratch->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&topBuild,0,nullptr);
        uav.UAV.pResource=tlas;list->ResourceBarrier(1,&uav);
        const bool ok=execute();release(staging);
        if(!ok)return false;
        woodChipMesh=std::move(mesh);
        return true;
    }

    bool rebuildUniqueGeometry(){
        if(!vertexBuffer||!indexBuffer)return false;
        wait();release(axeBlasScratch);release(detachedBlasScratch);
        release(standingBlasScratch);release(axeBlas);release(detachedBlas);
        release(standingBlas);release(axeIndexBuffer);release(axeVertexBuffer);
        release(detachedIndexBuffer);release(detachedVertexBuffer);
        release(standingIndexBuffer);release(standingVertexBuffer);
        standingRange={};detachedRange={};axeRange={};
        constexpr auto flags=static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);
        auto buildTree=[&](const std::shared_ptr<const TreeMesh>&mesh,
                           ID3D12Resource*&vertices,ID3D12Resource*&indices,
                           ID3D12Resource*&scratch,ID3D12Resource*&result,
                           TriangleMeshRange&range){
            if(!mesh)return true;
            std::vector<MeshVertex>flatVertices;std::vector<uint32_t>flatIndices;
            if(!appendBranchPartition(*mesh,true,cuttableBranchOwners,flatVertices,
                                      flatIndices,range))return false;
            if(!range.valid())return true;
            vertices=uploadDefaultUav(flatVertices);indices=uploadDefault(flatIndices);
            return vertices&&indices&&buildBottomLevel(
                geometryForResources(vertices,indices,range),scratch,result,flags);
        };
        if(!buildTree(promotedStanding,standingVertexBuffer,standingIndexBuffer,
            standingBlasScratch,standingBlas,standingRange))return false;
        if(!buildTree(promotedDetached,detachedVertexBuffer,detachedIndexBuffer,
            detachedBlasScratch,detachedBlas,detachedRange))return false;
        if(!axeMesh.vertices.empty()&&!axeMesh.indices.empty()){
            axeRange={0,static_cast<UINT>(axeMesh.vertices.size()),0,
                      static_cast<UINT>(axeMesh.indices.size())};
            axeVertexBuffer=uploadDefault(axeMesh.vertices);
            axeIndexBuffer=uploadDefault(axeMesh.indices);
            if(!axeVertexBuffer||!axeIndexBuffer||!buildBottomLevel(
                geometryForResources(axeVertexBuffer,axeIndexBuffer,axeRange),
                axeBlasScratch,axeBlas))return false;
        }
        axeGeometryReady=axeRange.valid()&&axeBlas;
        return rebuildTopLevel();
    }

    bool buildAcceleration(const TreeMesh&tree){
        wait();release(instanceGeometryBuffer);
        if(instanceBuffer&&instanceMapped)instanceBuffer->Unmap(0,nullptr);
        instanceMapped=nullptr;release(instanceBuffer);release(tlasScratch);release(grassBlasScratch);
        release(chipBlasScratch);release(axeBlasScratch);
        release(detachedBlasScratch);release(standingBlasScratch);
        release(staticBlasScratch);release(leafBlasScratch);release(blasScratch);
        release(tlas);release(grassBlas);
        release(chipBlas);release(axeBlas);release(detachedBlas);
        release(standingBlas);release(staticBlas);release(blas);
        release(leafBlas);
        release(axeIndexBuffer);release(axeVertexBuffer);
        release(detachedIndexBuffer);release(detachedVertexBuffer);
        release(standingIndexBuffer);release(standingVertexBuffer);
        if(visibleGrassUploadBuffer&&visibleGrassMapped)
            visibleGrassUploadBuffer->Unmap(0,nullptr);
        visibleGrassMapped=nullptr;release(visibleGrassBuffer);
        release(visibleGrassUploadBuffer);visibleGrassGpuReady=false;
        for(GrassBlasChunk&chunk:grassBlasChunks){
            release(chunk.scratch);release(chunk.blas);
        }
        grassBlasChunks.clear();
        release(grassBladeBuffer);release(grassBuffer);release(indexBuffer);release(baseTreeVertexBuffer);release(vertexBuffer);
        woodChipGeometryReady=false;woodChipArenaReady=false;chipRange={};
        chipInstanceSlot=UINT_MAX;
        grassChunkCache.clear();grassStreamEpoch=0;
        grassChunkCache.reserve(1024);
        streamedGrassPatches.clear();
        grassStreamValid=false;
        grassStreamCenterX=std::numeric_limits<float>::quiet_NaN();
        grassStreamCenterZ=std::numeric_limits<float>::quiet_NaN();
        grassStreamRadius=0;
        if(environment.terrainVertices.empty())environment=EnvironmentGenerator{}.build();

        hasDynamicTree=!tree.branchIndices.empty()||!tree.leafIndices.empty();
        std::vector<MeshVertex>treeVertices;
        std::vector<uint32_t>treeIndices;
        if(!appendBranchPartition(tree,true,cuttableBranchOwners,treeVertices,
                                  treeIndices,sharedBranchRange)){
            lastError=L"DXR cuttable branch ownership partition is invalid.";return false;
        }
        if(!sharedBranchRange.valid())appendHiddenTriangle(
            treeVertices,treeIndices,sharedBranchRange);
        // The shared immutable BLAS combines all non-cuttable wood with the
        // full canopy. It is built once and then merely instanced/transformed.
        if(!appendBranchPartition(tree,false,cuttableBranchOwners,treeVertices,
                                  treeIndices,sharedLeafRange)){
            lastError=L"DXR immutable branch ownership partition is invalid.";return false;
        }
        const uint32_t immutableLeafBase=sharedLeafRange.vertexCount;
        treeVertices.insert(treeVertices.end(),tree.leafVertices.begin(),
                            tree.leafVertices.end());
        for(uint32_t index:tree.leafIndices)
            treeIndices.push_back(immutableLeafBase+index);
        sharedLeafRange.vertexCount=static_cast<UINT>(treeVertices.size())-
                                    sharedLeafRange.vertexBase;
        sharedLeafRange.indexCount=static_cast<UINT>(treeIndices.size())-
                                   sharedLeafRange.indexBase;
        // The generated-world mode has no hero tree, but DXR still keeps a
        // separately refittable instance for the legacy wind path. A tiny
        // hidden triangle makes that BLAS structurally valid without adding
        // a visible object or special-casing the shader table/TLAS layout.
        if(!sharedLeafRange.valid())appendHiddenTriangle(
            treeVertices,treeIndices,sharedLeafRange);
        std::vector<MeshVertex>vertices=treeVertices;
        std::vector<uint32_t>indices=treeIndices;
        vertices.reserve(tree.branchVertices.size()+tree.leafVertices.size()+
                         environment.terrainVertices.size()+environment.riverVertices.size()+
                         environment.detailVertices.size()+woodChipVertexCapacity);
        indices.reserve(tree.branchIndices.size()+tree.leafIndices.size()+
                         environment.terrainIndices.size()+environment.riverIndices.size()+
                         environment.detailIndices.size()+woodChipIndexCapacity);
        treeVertexCount=static_cast<UINT>(treeVertices.size());
        treeIndexCount=static_cast<UINT>(treeIndices.size());treeHeight=.5f;
        for(const MeshVertex&vertex:treeVertices)treeHeight=std::max(treeHeight,vertex.position.y);
        environmentRange.vertexBase=static_cast<UINT>(vertices.size());
        environmentRange.indexBase=static_cast<UINT>(indices.size());
        const uint32_t terrainBase=0;
        vertices.insert(vertices.end(),environment.terrainVertices.begin(),
                        environment.terrainVertices.end());
        for(uint32_t index:environment.terrainIndices)indices.push_back(terrainBase+index);
        const uint32_t riverBase=static_cast<uint32_t>(environment.terrainVertices.size());
        vertices.insert(vertices.end(),environment.riverVertices.begin(),
                        environment.riverVertices.end());
        for(uint32_t index:environment.riverIndices)indices.push_back(riverBase+index);
        const uint32_t detailBase=riverBase+
            static_cast<uint32_t>(environment.riverVertices.size());
        vertices.insert(vertices.end(),environment.detailVertices.begin(),
                        environment.detailVertices.end());
        for(uint32_t index:environment.detailIndices)indices.push_back(detailBase+index);
        environmentRange.vertexCount=static_cast<UINT>(vertices.size())-
                                     environmentRange.vertexBase;
        environmentRange.indexCount=static_cast<UINT>(indices.size())-
                                    environmentRange.indexBase;
        // Reserve a stable subrange for transient chip bursts. This prevents
        // per-impact relocation of the very large forest/environment buffers
        // and lets animation touch only a few kilobytes.
        woodChipVertexBase=static_cast<UINT>(vertices.size());
        woodChipIndexBase=static_cast<UINT>(indices.size());
        vertices.resize(vertices.size()+woodChipVertexCapacity);
        indices.resize(indices.size()+woodChipIndexCapacity);
        woodChipArenaReady=true;
        standingRange={};detachedRange={};axeRange={};
        vertexCount=static_cast<UINT>(vertices.size());indexCount=static_cast<UINT>(indices.size());
        baseTreeVertexBuffer=uploadDefault(treeVertices);
        vertexBuffer=uploadDefaultUav(vertices);indexBuffer=uploadDefault(indices);
        grassPatchCount=static_cast<UINT>(environment.grassPatches.size());
        grassBladeCount=static_cast<UINT>(environment.grassBlades.size());
        if(grassBladeCount==0){
            for(const GrassPatchGpu&patch:environment.grassPatches)
                grassBladeCount+=patch.packed&255u;
        }
        grassBuffer=uploadDefault(environment.grassPatches);
        std::vector<GrassBladeGpu> packedBlades=environment.grassBlades;
        grassBlasChunks.clear();
        if(!packedBlades.empty()){
            constexpr int cellsX=30,cellsZ=20;
            const float originX=-96.0f,originZ=-60.0f;
            const float cellW=192.0f/cellsX,cellH=120.0f/cellsZ;
            std::vector<std::vector<GrassBladeGpu>> bins(
                static_cast<size_t>(cellsX*cellsZ));
            for(const GrassBladeGpu&blade:packedBlades){
                const float x=(blade.minX+blade.maxX)*.5f;
                const float z=(blade.minZ+blade.maxZ)*.5f;
                int cx=static_cast<int>(std::floor((x-originX)/cellW));
                int cz=static_cast<int>(std::floor((z-originZ)/cellH));
                cx=std::clamp(cx,0,cellsX-1);
                cz=std::clamp(cz,0,cellsZ-1);
                bins[static_cast<size_t>(cz*cellsX+cx)].push_back(blade);
            }
            packedBlades.clear();
            packedBlades.reserve(environment.grassBlades.size());
            for(auto&bin:bins){
                if(bin.empty())continue;
                GrassBlasChunk chunk;
                chunk.bladeBase=static_cast<UINT>(packedBlades.size());
                chunk.bladeCount=static_cast<UINT>(bin.size());
                packedBlades.insert(packedBlades.end(),bin.begin(),bin.end());
                grassBlasChunks.push_back(chunk);
            }
        }
        grassBladeCount=static_cast<UINT>(packedBlades.size());
        grassBladeBuffer=packedBlades.empty()?nullptr:uploadDefault(packedBlades);
        const UINT64 visibleGrassBytes=std::max<UINT64>(
            static_cast<UINT64>(grassPatchCount)*sizeof(GrassPatchGpu),256);
        visibleGrassStride=visibleGrassBytes;
        visibleGrassUploadBuffer=makeBuffer(
            visibleGrassBytes*2,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        visibleGrassBuffer=makeBuffer(
            visibleGrassBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
        if(visibleGrassUploadBuffer&&
           FAILED(visibleGrassUploadBuffer->Map(0,nullptr,&visibleGrassMappedBase))){
            release(visibleGrassUploadBuffer);visibleGrassMappedBase=nullptr;
        }
        visibleGrassMapped=visibleGrassMappedBase;
        if(!baseTreeVertexBuffer||!vertexBuffer||!indexBuffer||!grassBuffer||
           !visibleGrassBuffer||!visibleGrassUploadBuffer||
           (!environment.grassBlades.empty()&&!grassBladeBuffer)){
            lastError=L"DXR scene geometry upload to GPU-local memory failed.";return false;
        }

        // Vertex buffers contain absolute concatenated ranges, but each BLAS
        // consumes local indices relative to its own range.  HLSL adds the
        // matching vertexBase from InstanceGeometryTable at hit time.

        D3D12_RAYTRACING_GEOMETRY_DESC treeGeometry=geometryFor(sharedBranchRange);
        D3D12_RAYTRACING_GEOMETRY_DESC leafGeometry=geometryFor(sharedLeafRange);
        constexpr auto updateFlags=static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);
        if(!buildBottomLevel(treeGeometry,blasScratch,blas,updateFlags))return false;
        if(!buildBottomLevel(leafGeometry,leafBlasScratch,leafBlas,updateFlags))return false;

        D3D12_RAYTRACING_GEOMETRY_DESC staticGeometry=geometryFor(environmentRange);
        if(!buildBottomLevel(staticGeometry,staticBlasScratch,staticBlas))return false;
        if(grassBladeBuffer&&!grassBlasChunks.empty()){
            for(GrassBlasChunk&chunk:grassBlasChunks){
                D3D12_RAYTRACING_GEOMETRY_DESC grassGeometry{};
                grassGeometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
                grassGeometry.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
                grassGeometry.AABBs.AABBCount=chunk.bladeCount;
                grassGeometry.AABBs.AABBs.StartAddress=grassBladeBuffer->GetGPUVirtualAddress()+
                    static_cast<UINT64>(chunk.bladeBase)*sizeof(GrassBladeGpu);
                grassGeometry.AABBs.AABBs.StrideInBytes=sizeof(GrassBladeGpu);
                if(!buildBottomLevel(grassGeometry,chunk.scratch,chunk.blas)){
                    lastError=L"DXR grass-blade chunk acceleration structure build failed.";
                    return false;
                }
            }
        }
        if((promotedTreeActive||!axeMesh.vertices.empty())&&!rebuildUniqueGeometry())return false;
        if(!promotedTreeActive&&axeMesh.vertices.empty()&&!rebuildTopLevel())return false;
        if(woodChipMesh&&!installWoodChipMesh(woodChipMesh))return false;
        frameIndex=0;treeWindWasActive=false;return true;
    }
    bool recordTreeWind(const EnvironmentCB&environmentConstants){
        if(!hasDynamicTree)return true;
        const bool active=environmentConstants.windSpeed>.001f&&
                          environmentConstants.windStrength>.001f;
        if(!active&&!treeWindWasActive)return true;
        if(!treeWindPipeline||!treeWindRoot||!baseTreeVertexBuffer||!vertexBuffer||
           !blas||!leafBlas||!tlas||!instanceBuffer||treeVertexCount==0)return false;

        auto toUav=transition(vertexBuffer,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1,&toUav);
        list->SetComputeRootSignature(treeWindRoot);
        list->SetPipelineState(treeWindPipeline);
        list->SetComputeRootShaderResourceView(0,baseTreeVertexBuffer->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(1,vertexBuffer->GetGPUVirtualAddress());
        struct WindConstants{UINT count;float height;UINT padding[2];}
            constants{treeVertexCount,treeHeight,{0,0}};
        static_assert(sizeof(WindConstants)==16);
        list->SetComputeRoot32BitConstants(2,4,&constants,0);
        list->SetComputeRootConstantBufferView(3,environmentGpu?environmentGpu:environmentBuffer->GetGPUVirtualAddress());
        list->Dispatch((treeVertexCount+255u)/256u,1,1);
        D3D12_RESOURCE_BARRIER vertexUav{};vertexUav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        vertexUav.UAV.pResource=vertexBuffer;list->ResourceBarrier(1,&vertexUav);
        auto toAcceleration=transition(vertexBuffer,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1,&toAcceleration);

        D3D12_RAYTRACING_GEOMETRY_DESC treeGeometry=geometryFor(sharedBranchRange);
        D3D12_RAYTRACING_GEOMETRY_DESC leafGeometry=geometryFor(sharedLeafRange);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottomInput{};
        bottomInput.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bottomInput.Flags=static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE);
        bottomInput.NumDescs=1;bottomInput.pGeometryDescs=&treeGeometry;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bottomBuild{};
        bottomBuild.Inputs=bottomInput;
        bottomBuild.SourceAccelerationStructureData=blas->GetGPUVirtualAddress();
        bottomBuild.DestAccelerationStructureData=blas->GetGPUVirtualAddress();
        bottomBuild.ScratchAccelerationStructureData=blasScratch->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&bottomBuild,0,nullptr);
        D3D12_RESOURCE_BARRIER blasUav{};blasUav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        blasUav.UAV.pResource=blas;list->ResourceBarrier(1,&blasUav);

        // Leaves occupy a second immutable/shared BLAS. The same wind compute
        // writes their portion of the combined vertex buffer, so refit that
        // BLAS as well without ever rebuilding or duplicating its topology.
        bottomInput.pGeometryDescs=&leafGeometry;
        bottomBuild.Inputs=bottomInput;
        bottomBuild.SourceAccelerationStructureData=leafBlas->GetGPUVirtualAddress();
        bottomBuild.DestAccelerationStructureData=leafBlas->GetGPUVirtualAddress();
        bottomBuild.ScratchAccelerationStructureData=leafBlasScratch->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&bottomBuild,0,nullptr);
        blasUav.UAV.pResource=leafBlas;list->ResourceBarrier(1,&blasUav);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topInput{};
        topInput.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        topInput.Flags=static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE);
        topInput.NumDescs=tlasInstanceCount;
        topInput.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
        topInput.InstanceDescs=instanceBuffer->GetGPUVirtualAddress();
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topBuild{};
        topBuild.Inputs=topInput;
        topBuild.SourceAccelerationStructureData=tlas->GetGPUVirtualAddress();
        topBuild.DestAccelerationStructureData=tlas->GetGPUVirtualAddress();
        topBuild.ScratchAccelerationStructureData=tlasScratch->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&topBuild,0,nullptr);
        D3D12_RESOURCE_BARRIER tlasUav{};tlasUav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        tlasUav.UAV.pResource=tlas;list->ResourceBarrier(1,&tlasUav);
        treeWindWasActive=active;
        return true;
    }
};

DxrRenderer::DxrRenderer():impl_(std::make_unique<Impl>()){}DxrRenderer::~DxrRenderer()=default;
bool DxrRenderer::initialize(HWND window,int width,int height){auto&i=*impl_;i.window=window;i.width=std::max(1,width);i.height=std::max(1,height);i.renderWidth=i.width;i.renderHeight=i.height;
    CreateDirectoryW(L"C:\\StressTest\\video",nullptr);
    {
        FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a");
        if(boot){fputs("starting\n",boot);fclose(boot);}
    }
    wchar_t exePath[MAX_PATH]{};GetModuleFileNameW(nullptr,exePath,MAX_PATH);
    if(wchar_t*slash=wcsrchr(exePath,L'\\'))*slash=0;
    {
        FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a");
        if(boot){fputs("create_factory\n",boot);fclose(boot);}
    }
    HRESULT hr=CreateDXGIFactory1(__uuidof(IDXGIFactory6),reinterpret_cast<void**>(&i.factory));if(FAILED(hr))return i.fail(hr,L"DXGI factory creation failed");
    {
        FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a");
        if(boot){fputs("create_device\n",boot);fclose(boot);}
    }
    IDXGIAdapter1*adapter{};
    for(UINT n=0;i.factory->EnumAdapterByGpuPreference(n,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,__uuidof(IDXGIAdapter1),reinterpret_cast<void**>(&adapter))!=DXGI_ERROR_NOT_FOUND;++n){
        DXGI_ADAPTER_DESC1 d{};adapter->GetDesc1(&d);
        if(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE){release(adapter);continue;}
        HRESULT deviceHr=E_FAIL;
        for(int attempt=0;attempt<4&&!i.device;++attempt){
            if(attempt)Sleep(400);
            deviceHr=D3D12CreateDevice(adapter,D3D_FEATURE_LEVEL_12_1,__uuidof(ID3D12Device5),reinterpret_cast<void**>(&i.device));
            FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a");
            if(boot){fprintf(boot,"create_device_try %u %d hr 0x%08X\n",n,attempt,static_cast<unsigned>(deviceHr));fclose(boot);}
            MSG pump{};
            while(PeekMessageW(&pump,nullptr,0,0,PM_REMOVE)){
                TranslateMessage(&pump);DispatchMessageW(&pump);
            }
        }
        if(i.device){release(adapter);break;}
        release(adapter);
    }
    if(!i.device){i.lastError=L"No DXR-capable DirectX 12 device was found.";return false;}
    {
        FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a");
        if(boot){fputs("device_ok\n",boot);fclose(boot);}
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};if(FAILED(i.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options,sizeof(options)))||options.RaytracingTier<D3D12_RAYTRACING_TIER_1_1){i.lastError=L"The selected GPU does not expose DXR 1.1 inline ray queries.";return false;}
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;hr=i.device->CreateCommandQueue(&q,__uuidof(ID3D12CommandQueue),reinterpret_cast<void**>(&i.queue));if(FAILED(hr))return i.fail(hr,L"DXR command queue creation failed");
    for(UINT slot=0;slot<2;++slot){
        hr=i.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,__uuidof(ID3D12CommandAllocator),reinterpret_cast<void**>(&i.allocators[slot]));
        if(FAILED(hr))return i.fail(hr,L"DXR command allocator creation failed");
        hr=i.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,i.allocators[slot],nullptr,__uuidof(ID3D12GraphicsCommandList4),reinterpret_cast<void**>(&i.lists[slot]));
        if(FAILED(hr))return i.fail(hr,L"DXR command-list creation failed");
        i.lists[slot]->Close();
    }
    i.allocator=i.allocators[0];i.list=i.lists[0];
    DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=i.width;sd.Height=i.height;sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;sd.SampleDesc.Count=1;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;IDXGISwapChain1*base{};hr=i.factory->CreateSwapChainForHwnd(i.queue,window,&sd,nullptr,nullptr,&base);if(FAILED(hr))return i.fail(hr,L"DXR swap chain creation failed");hr=base->QueryInterface(__uuidof(IDXGISwapChain3),reinterpret_cast<void**>(&i.swap));release(base);if(FAILED(hr))return i.fail(hr,L"DXR swap-chain interface unavailable");
    D3D12_DESCRIPTOR_HEAP_DESC rh{};rh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rh.NumDescriptors=8;i.device->CreateDescriptorHeap(&rh,__uuidof(ID3D12DescriptorHeap),reinterpret_cast<void**>(&i.rtvHeap));i.rtvSize=i.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);D3D12_DESCRIPTOR_HEAP_DESC dh{};dh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;dh.NumDescriptors=1;i.device->CreateDescriptorHeap(&dh,__uuidof(ID3D12DescriptorHeap),reinterpret_cast<void**>(&i.dsvHeap));D3D12_DESCRIPTOR_HEAP_DESC gh{};gh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;gh.NumDescriptors=16;gh.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;i.device->CreateDescriptorHeap(&gh,__uuidof(ID3D12DescriptorHeap),reinterpret_cast<void**>(&i.gpuHeap));i.srvSize=i.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);i.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,__uuidof(ID3D12Fence),reinterpret_cast<void**>(&i.fence));i.fenceEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    D3D12_COMMAND_QUEUE_DESC copyQ{};copyQ.Type=D3D12_COMMAND_LIST_TYPE_COPY;
    i.device->CreateCommandQueue(&copyQ,__uuidof(ID3D12CommandQueue),reinterpret_cast<void**>(&i.copyQueue));
    if(i.copyQueue){
        i.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,__uuidof(ID3D12CommandAllocator),reinterpret_cast<void**>(&i.copyAlloc));
        i.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_COPY,i.copyAlloc,nullptr,__uuidof(ID3D12GraphicsCommandList),reinterpret_cast<void**>(&i.copyList));
        if(i.copyList)i.copyList->Close();
        i.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,__uuidof(ID3D12Fence),reinterpret_cast<void**>(&i.copyFence));
        i.copyFenceEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    }
    {
        FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a");
        if(boot){fputs("copy_queue_ready\n",boot);fclose(boot);}
    }
    // Load NGX only after a live D3D12 device and queue exist. Event Viewer
    // showed 0xC0000005 in _nvngx.dll when it was loaded before device create.
    const bool slStarted=i.sl.startup(exePath);
    {
        FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a");
        if(boot){fprintf(boot,"streamline_startup %d\n",slStarted?1:0);fclose(boot);}
    }
    i.sl.setDevice(i.device);
    i.sl.configure(static_cast<std::uint32_t>(i.width),static_cast<std::uint32_t>(i.height),
                   i.sl.quality(),i.sl.frameGen());
    if(i.sl.status().frameGen!=FrameGenMode::Off){
        IDXGISwapChain*swapBase=i.swap;
        if(i.sl.upgradeSwapChain(&swapBase)&&swapBase){
            IDXGISwapChain3*upgraded{};
            if(SUCCEEDED(swapBase->QueryInterface(__uuidof(IDXGISwapChain3),reinterpret_cast<void**>(&upgraded)))&&upgraded){
                i.swap->Release();
                i.swap=upgraded;
            }
        }
    }
    {
        FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a");
        if(boot){
            const auto&st=i.sl.status();
            fprintf(boot,"sl_loaded %d rr %d dlss %d fg %d render %ux%u label %s\n",
                    st.loaded?1:0,st.rayReconstruction?1:0,st.dlss?1:0,
                    st.frameGeneration?1:0,st.renderWidth,st.renderHeight,st.label);
            fclose(boot);
        }
    }
    if(i.sl.upscaleActive()&&i.sl.status().renderWidth&&i.sl.status().renderHeight){
        i.renderWidth=static_cast<int>(i.sl.status().renderWidth);
        i.renderHeight=static_cast<int>(i.sl.status().renderHeight);
    }else{
        i.renderWidth=i.width;i.renderHeight=i.height;
    }
    auto bootMark=[&](const char*m){
        FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a");
        if(boot){fputs(m,boot);fputc('\n',boot);fclose(boot);}
    };
    bootMark("create_backbuffers");
    if(!i.createBackBuffers())return false;
    bootMark("create_outputs");
    if(!i.createOutputs())return false;
    bootMark("create_bark");
    if(!i.createBarkNormal())return false;
    bootMark("create_ground");
    if(!i.createGroundMaterials())return false;
    bootMark("create_pipeline");
    if(!i.createPipeline())return false;
    bootMark("create_grass");
    if(!i.createGrassPipeline())return false;
    bootMark("create_hud");
    if(!i.createHudPipeline())return false;
    bootMark("create_present");
    if(!i.createPresentPipeline())return false;
    bootMark("create_wind");
    if(!i.createTreeWindPipeline())return false;
    i.cameraBuffer=i.makeBuffer(512,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    i.environmentBuffer=i.makeBuffer(512,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    i.hudBuffer=i.makeBuffer(512,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    i.presentBuffer=i.makeBuffer(512,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    i.denoiseCb=i.makeBuffer(512,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    if(!i.cameraBuffer||!i.environmentBuffer||!i.hudBuffer||!i.presentBuffer||!i.denoiseCb||
       FAILED(i.cameraBuffer->Map(0,nullptr,&i.cameraMappedBase))||
       FAILED(i.environmentBuffer->Map(0,nullptr,&i.environmentMappedBase))||
       FAILED(i.hudBuffer->Map(0,nullptr,&i.hudMappedBase))||
       FAILED(i.presentBuffer->Map(0,nullptr,&i.presentMappedBase))||
       FAILED(i.denoiseCb->Map(0,nullptr,&i.denoiseCbMappedBase)))return false;
    i.cameraMapped=i.cameraMappedBase;i.environmentMapped=i.environmentMappedBase;
    i.hudMapped=i.hudMappedBase;i.presentMapped=i.presentMappedBase;
    i.denoiseCbMapped=i.denoiseCbMappedBase;i.bindInFlight(0);
    bootMark("create_dlss");
    i.createDlssFeature();
    i.initialized=true;
    bootMark("initialize_ok");
    return true;
}
void DxrRenderer::resize(int width,int height){auto&i=*impl_;if(!i.initialized||i.deviceLost||width<=0||height<=0)return;if(width==i.width&&height==i.height)return;i.wait();i.width=width;i.height=height;for(auto&b:i.backBuffers)release(b);if(SUCCEEDED(i.swap->ResizeBuffers(0,width,height,DXGI_FORMAT_UNKNOWN,0))){i.createBackBuffers();i.sl.configure(static_cast<std::uint32_t>(i.width),static_cast<std::uint32_t>(i.height),i.sl.quality(),i.sl.frameGen());if(i.sl.upscaleActive()&&i.sl.status().renderWidth&&i.sl.status().renderHeight){i.renderWidth=static_cast<int>(i.sl.status().renderWidth);i.renderHeight=static_cast<int>(i.sl.status().renderHeight);}else{i.renderWidth=i.width;i.renderHeight=i.height;}i.createOutputs();i.createDlssFeature();}}
void DxrRenderer::setTree(std::shared_ptr<const TreeMesh>tree){
    if(!tree)tree=std::make_shared<TreeMesh>();
    impl_->sourceTree=std::move(tree);
    if(impl_->initialized&&!impl_->buildAcceleration(*impl_->sourceTree))
        MessageBoxW(impl_->window,impl_->lastError.c_str(),
                    L"Dense Trees DXR geometry error",MB_ICONERROR);
}
void DxrRenderer::setTree(const TreeMesh&tree){
    setTree(std::make_shared<TreeMesh>(tree));
}

void DxrRenderer::setCuttableBranchOwners(std::vector<unsigned char>ownerMask){
    auto&i=*impl_;i.cuttableBranchOwners=std::move(ownerMask);
}

bool DxrRenderer::setAxeMesh(const AxeMesh&axe){
    auto&i=*impl_;i.axeMesh=axe;
    if(!i.initialized)return true;
    if(!i.vertexBuffer||!i.indexBuffer)return true;
    return i.rebuildUniqueGeometry();
}

void DxrRenderer::setAxeState(bool visible,const AxeRigidTransform&transform){
    auto&i=*impl_;i.axeVisible=visible;i.axeTransform=rendererTransform(transform);
    if(i.initialized&&i.blas&&i.axeGeometryReady&&!i.refitTopLevelTransforms())
        i.lastError=L"DXR axe TLAS update failed.";
}

bool DxrRenderer::promoteTreeInstance(std::size_t sharedInstanceIndex,
                                      std::shared_ptr<const TreeMesh>standing,
                                      std::shared_ptr<const TreeMesh>detached,
                                      const TreeInstance&source){
    auto&i=*impl_;
    if(!i.sourceTree||sharedInstanceIndex>i.sourceTree->additionalInstances.size())return false;
    i.promotedTreeActive=true;i.promotedSharedIndex=sharedInstanceIndex;
    i.promotedSource=source;i.promotedStanding=std::move(standing);
    i.promotedDetached=std::move(detached);
    i.promotedCanopyDetached=i.promotedDetached&&
        !i.promotedDetached->branchIndices.empty();
    const RendererRigidTransform sourceTransform=Impl::instanceTransform(source);
    i.standingTransform=sourceTransform;i.detachedTransform=sourceTransform;
    return !i.initialized||i.rebuildUniqueGeometry();
}

bool DxrRenderer::promoteTreeInstance(std::size_t sharedInstanceIndex,
                                      const TreeMesh&standing,
                                      const TreeMesh&detached,
                                      const TreeInstance&source){
    return promoteTreeInstance(sharedInstanceIndex,
        std::make_shared<TreeMesh>(standing),std::make_shared<TreeMesh>(detached),source);
}

void DxrRenderer::clearPromotedTree(){
    auto&i=*impl_;i.promotedTreeActive=false;
    i.promotedSharedIndex=std::numeric_limits<std::size_t>::max();
    i.promotedStanding={};i.promotedDetached={};i.promotedCanopyDetached=false;
    if(i.initialized&&i.blas&&!i.rebuildUniqueGeometry())
        i.lastError=L"DXR promoted-tree removal failed.";
}

bool DxrRenderer::setPromotedTreeMeshes(std::shared_ptr<const TreeMesh>standing,
                                        std::shared_ptr<const TreeMesh>detached){
    auto&i=*impl_;if(!i.promotedTreeActive)return false;
    i.promotedStanding=std::move(standing);i.promotedDetached=std::move(detached);
    i.promotedCanopyDetached=i.promotedDetached&&
        !i.promotedDetached->branchIndices.empty();
    return !i.initialized||i.rebuildUniqueGeometry();
}

bool DxrRenderer::setPromotedTreeMeshes(const TreeMesh&standing,
                                        const TreeMesh&detached){
    return setPromotedTreeMeshes(std::make_shared<TreeMesh>(standing),
                                 std::make_shared<TreeMesh>(detached));
}

void DxrRenderer::setPromotedTreeTransforms(
    const RendererRigidTransform&standing,
    const RendererRigidTransform&detached){
    auto&i=*impl_;i.standingTransform=standing;i.detachedTransform=detached;
    if(i.initialized&&i.promotedTreeActive&&!i.refitTopLevelTransforms())
        i.lastError=L"DXR promoted-tree TLAS update failed.";
}

bool DxrRenderer::updateDetachedTreeMesh(std::shared_ptr<const TreeMesh>detachedMesh){
    auto&i=*impl_;
    if(!detachedMesh)return false;
    const TreeMesh&detached=*detachedMesh;
    if(!i.initialized||!i.promotedTreeActive||!i.detachedBlas||
       !i.promotedDetached||
       detached.branchIndices.size()%3!=0||
       detached.branchTriangleOwners.size()!=detached.branchIndices.size()/3)return false;
    std::vector<MeshVertex>vertices;std::vector<uint32_t>indices;
    TriangleMeshRange range;
    if(!appendBranchPartition(detached,true,i.cuttableBranchOwners,vertices,
                              indices,range)||
       range.vertexCount!=i.detachedRange.vertexCount||
       range.indexCount!=i.detachedRange.indexCount)return false;
    const TreeMesh&previous=*i.promotedDetached;
    std::vector<MeshVertex>previousVertices;std::vector<uint32_t>previousIndices;
    TriangleMeshRange previousRange;
    if(!appendBranchPartition(previous,true,i.cuttableBranchOwners,previousVertices,
                              previousIndices,previousRange)||
       indices!=previousIndices)return false;
    i.wait();
    const UINT64 bytes=static_cast<UINT64>(vertices.size())*sizeof(MeshVertex);
    ID3D12Resource*staging=i.makeBuffer(bytes,D3D12_HEAP_TYPE_UPLOAD,
                                       D3D12_RESOURCE_STATE_GENERIC_READ);
    if(!staging)return false;
    void*mapped{};
    if(FAILED(staging->Map(0,nullptr,&mapped))){release(staging);return false;}
    std::memcpy(mapped,vertices.data(),static_cast<size_t>(bytes));staging->Unmap(0,nullptr);
    if(!i.begin()){release(staging);return false;}
    auto toCopy=transition(i.detachedVertexBuffer,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_COPY_DEST);
    i.list->ResourceBarrier(1,&toCopy);
    i.list->CopyBufferRegion(i.detachedVertexBuffer,0,staging,0,bytes);
    auto toAcceleration=transition(i.detachedVertexBuffer,D3D12_RESOURCE_STATE_COPY_DEST,
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    i.list->ResourceBarrier(1,&toAcceleration);
    D3D12_RAYTRACING_GEOMETRY_DESC geometry=i.geometryForResources(
        i.detachedVertexBuffer,i.detachedIndexBuffer,i.detachedRange);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
    input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    input.Flags=static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD|
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE);
    input.NumDescs=1;input.pGeometryDescs=&geometry;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs=input;
    build.SourceAccelerationStructureData=i.detachedBlas->GetGPUVirtualAddress();
    build.DestAccelerationStructureData=i.detachedBlas->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData=i.detachedBlasScratch->GetGPUVirtualAddress();
    i.list->BuildRaytracingAccelerationStructure(&build,0,nullptr);
    D3D12_RESOURCE_BARRIER uav{};uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource=i.detachedBlas;i.list->ResourceBarrier(1,&uav);
    const bool ok=i.execute();release(staging);
    if(ok)i.promotedDetached=std::move(detachedMesh);
    return ok;
}

bool DxrRenderer::updateDetachedTreeMesh(const TreeMesh&detached){
    return updateDetachedTreeMesh(std::make_shared<TreeMesh>(detached));
}

bool DxrRenderer::setWoodChipMesh(std::shared_ptr<const TreeMesh>chips){
    auto&i=*impl_;
    return i.installWoodChipMesh(std::move(chips));
}

bool DxrRenderer::updateWoodChipMesh(std::shared_ptr<const TreeMesh>chips){
    auto&i=*impl_;
    if(!chips||!Impl::validWoodChipMesh(*chips)||!i.woodChipMesh||
       chips->branchVertices.size()!=i.woodChipMesh->branchVertices.size()||
       chips->branchIndices!=i.woodChipTopologyIndices)return false;
    if(!i.initialized||!i.woodChipGeometryReady){
        i.woodChipMesh=std::move(chips);return true;
    }
    return i.refitWoodChipMesh(std::move(chips));
}

void DxrRenderer::setWoodChipTransform(const RendererRigidTransform&transform){
    auto&i=*impl_;i.woodChipTransform=transform;
    if(i.initialized&&i.woodChipGeometryReady&&
       !i.refitTopLevelTransforms())
        i.lastError=L"DXR wood-chip TLAS transform update failed.";
}

void DxrRenderer::setWoodChipsVisible(bool visible){
    auto&i=*impl_;i.woodChipsVisible=visible;
    if(i.initialized&&i.woodChipGeometryReady&&
       !i.refitTopLevelTransforms())
        i.lastError=L"DXR wood-chip visibility update failed.";
}

void DxrRenderer::clearWoodChips(){
    auto&i=*impl_;i.woodChipMesh={};i.woodChipTopologyIndices.clear();
    i.woodChipGeometryReady=false;i.woodChipsVisible=false;i.chipRange={};
    if(!i.initialized||!i.chipBlas)return;
    i.wait();release(i.chipBlasScratch);release(i.chipBlas);
    i.chipInstanceSlot=UINT_MAX;
    if(i.blas&&i.leafBlas&&i.staticBlas&&!i.rebuildTopLevel())
        i.lastError=L"DXR wood-chip removal failed.";
}

void DxrRenderer::setWorld(EnvironmentMesh world,WaterSampler waterSampler){
    auto&i=*impl_;
    i.environment=std::move(world);
    i.waterSampler=std::move(waterSampler);
    i.customWorld=true;
}
void DxrRenderer::render(const CameraView&requestedView,
                         const DebugRenderSettings&settings,
                         const EnvironmentCB&environment,
                         const PlayerLocalLight&requestedLocalLight){
    auto&i=*impl_;if(!i.initialized||i.deviceLost||!i.tlas)return;
    if(i.submittedFrames<4){
        FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a");
        if(boot){fprintf(boot,"render_begin %u tlas %p cam %p\n",
                         i.submittedFrames,(void*)i.tlas,(void*)i.cameraMapped);fclose(boot);}
    }
    i.sl.beginFrame(i.frameIndex);
    i.sl.markerSimulationStart();
    const auto finiteVec=[](const Vec3&value){
        return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z);
    };
    Vec3 eye=finiteVec(requestedView.eye)?requestedView.eye:Vec3{0.0f,4.1f,-14.0f};
    Vec3 forward=requestedView.forward;
    if(!finiteVec(forward)||lengthSq(forward)<1e-8f)forward={0.0f,0.0f,1.0f};
    else forward=normalize(forward);
    Vec3 right=cross({0.0f,1.0f,0.0f},forward);
    if(lengthSq(right)<1e-8f)right=cross({0.0f,0.0f,1.0f},forward);
    if(lengthSq(right)<1e-8f)right={1.0f,0.0f,0.0f};
    else right=normalize(right);
    const Vec3 up=normalize(cross(forward,right));

    // Persistent river water is a volume, not a collision plane.  The
    // first-person controller continues to follow the carved bed; this state
    // only tells the ray-generation path which participating medium contains
    // the camera.  Querying the same authored cross-section that builds the
    // mesh keeps the transition at the visible waterline.
    const PersistentWaterSample water=i.waterSampler?
        i.waterSampler(eye.x,eye.z):EnvironmentGenerator::persistentWater(eye.x,eye.z);
    const bool cameraUnderwater=water.inside&&eye.y<water.surfaceHeight;
    const float cameraImmersion=cameraUnderwater?
        water.surfaceHeight-eye.y:0.0f;

    PlayerLocalLight localLight=requestedLocalLight;
    localLight.intensity=std::isfinite(localLight.intensity)?
        clamp(localLight.intensity,0.0f,2048.0f):60.0f;
    localLight.range=std::isfinite(localLight.range)?
        clamp(localLight.range,.25f,128.0f):22.0f;
    localLight.innerConeRadians=std::isfinite(localLight.innerConeRadians)?
        clamp(localLight.innerConeRadians,0.0f,1.54f):.19198622f;
    localLight.outerConeRadians=std::isfinite(localLight.outerConeRadians)?
        clamp(localLight.outerConeRadians,localLight.innerConeRadians+.001f,1.56f):
        std::max(localLight.innerConeRadians+.001f,.31415927f);
    CameraView view=requestedView;
    view.eye=eye;view.forward=forward;
    if(!view.grassInteractionEnabled||
       !finiteVec(view.grassInteractionPosition)||
       !finiteVec(view.grassInteractionVelocity)){
        view.grassInteractionEnabled=false;
        view.grassInteractionPosition={};view.grassInteractionVelocity={};
    }else{
        // Defensive cap only; the first-person controller normally supplies
        // at most its 5.5 m/s sprint velocity.
        const float interactionSpeed=length(view.grassInteractionVelocity);
        if(interactionSpeed>15.0f)
            view.grassInteractionVelocity=
                view.grassInteractionVelocity*(15.0f/interactionSpeed);
    }
    const auto changed=[](float a,float b){return std::abs(a-b)>.0001f;};
    const bool viewChanged=!i.haveLastView||
        changed(view.eye.x,i.lastView.eye.x)||changed(view.eye.y,i.lastView.eye.y)||
        changed(view.eye.z,i.lastView.eye.z)||
        changed(view.forward.x,i.lastView.forward.x)||
        changed(view.forward.y,i.lastView.forward.y)||
        changed(view.forward.z,i.lastView.forward.z);
    const bool localLightChanged=!i.haveLastLocalLight||
        localLight.enabled!=i.lastLocalLight.enabled||
        localLight.spotlight!=i.lastLocalLight.spotlight||
        changed(localLight.intensity,i.lastLocalLight.intensity)||
        changed(localLight.range,i.lastLocalLight.range)||
        changed(localLight.innerConeRadians,i.lastLocalLight.innerConeRadians)||
        changed(localLight.outerConeRadians,i.lastLocalLight.outerConeRadians);
    const bool debugChanged=!i.haveLastDebugSettings||
        std::abs(settings.grassDensity-i.lastDebugSettings.grassDensity)>.0001f||
        std::abs(settings.bladeHeightScale-i.lastDebugSettings.bladeHeightScale)>.0001f||
        std::abs(settings.groundNormalStrength-i.lastDebugSettings.groundNormalStrength)>.0001f||
        std::abs(settings.groundDetailStrength-i.lastDebugSettings.groundDetailStrength)>.0001f||
        std::abs(settings.shortGrassDrawDistance-i.lastDebugSettings.shortGrassDrawDistance)>.0001f||
        std::abs(settings.tallGrassDrawDistance-i.lastDebugSettings.tallGrassDrawDistance)>.0001f;
    EnvironmentCB visualEnvironment=environment;
    EnvironmentCB previousVisualEnvironment=i.lastEnvironment;
    // Total time and dt are not visible when every time-driven effect is
    // disabled. Ignoring those two clock fields lets a paused scene converge.
    visualEnvironment.time=previousVisualEnvironment.time=0.0f;
    visualEnvironment.deltaTime=previousVisualEnvironment.deltaTime=0.0f;
    const bool environmentChanged=!i.haveLastEnvironment||
        std::memcmp(&visualEnvironment,&previousVisualEnvironment,
                    sizeof(visualEnvironment))!=0;
    const bool useDlss=i.sl.upscaleActive();
    const bool hardCut=!i.haveLastView||
        lengthSq(eye-i.lastView.eye)>9.0f;
    // Cinematic motion and wind must not zero the temporal index. That forced
    // DLSS Reset every frame and left the 1-spp path tracer raw-noisy.
    if(!useDlss&&(hardCut||localLightChanged||debugChanged||environmentChanged)){
        i.frameIndex=0;
    }
    i.lastView=view;i.lastLocalLight=localLight;
    i.lastDebugSettings=settings;i.lastEnvironment=environment;
    i.haveLastView=i.haveLastLocalLight=true;
    i.haveLastDebugSettings=i.haveLastEnvironment=true;
    const bool animatedEnvironment=environment.windSpeed>.001f||
        environment.rainIntensity>.001f||environment.lightningFlash>.001f||
        environmentChanged;
    const UINT temporalFrames=i.offlineSpp?i.offlineSpp:(useDlss?1u:(animatedEnvironment?1u:8u));
    const UINT shaderFrame=temporalFrames>1u?i.frameIndex:0u;
    const float tanHalf=std::tan(52*pi/360);
    const float grassDensity=clamp(settings.grassDensity,0.0f,6.0f);
    const UINT packedBlades=i.environment.grassPatches.empty()?80u:
        std::min<UINT>(i.environment.grassPatches.front().packed&255u,160u);
    const UINT nearGrassStride=static_cast<UINT>(clamp(
        std::ceil(std::max(1.0f,grassDensity)*static_cast<float>(packedBlades)),1.0f,160.0f));
    const UINT farGrassStride=0;
    const float renderAspect=static_cast<float>(i.renderWidth)/
        static_cast<float>(std::max(1,i.renderHeight));
    i.recycleInFlight();
    const auto visibleGrass=grassDensity>.001f?
        i.compactVisibleGrass(eye,forward,right,up,tanHalf,renderAspect,settings):
        std::pair<UINT,UINT>{};
    i.visibleNearGrassPatchCount=visibleGrass.first;
    i.visibleFarGrassPatchCount=visibleGrass.second;
    float jitterX=0,jitterY=0;
    copyHaltonJitter(i.frameIndex,&jitterX,&jitterY);
    if(!useDlss){jitterX=0;jitterY=0;}
    CameraBasis currentCamera{};
    currentCamera.eye[0]=eye.x;currentCamera.eye[1]=eye.y;currentCamera.eye[2]=eye.z;
    currentCamera.forward[0]=forward.x;currentCamera.forward[1]=forward.y;currentCamera.forward[2]=forward.z;
    currentCamera.right[0]=right.x;currentCamera.right[1]=right.y;currentCamera.right[2]=right.z;
    currentCamera.up[0]=up.x;currentCamera.up[1]=up.y;currentCamera.up[2]=up.z;
    currentCamera.tanHalf=tanHalf;currentCamera.aspect=renderAspect;
    currentCamera.jitter[0]=jitterX;currentCamera.jitter[1]=jitterY;
    currentCamera.time=environment.time;
    const CameraBasis prevCamera=i.havePrevCamera?i.prevCamera:currentCamera;
    const bool resetHistory=hardCut||!i.havePrevCamera;
    i.sl.markerSimulationEnd();
    i.sl.setCamera(currentCamera,prevCamera,resetHistory);
    i.sl.markerRenderStart();
    struct Camera{
        float eye[3],tanHalf;float forward[3],aspect;float right[3];UINT frame;
        float up[3];UINT maxFrames;
        float exposure,localLightIntensity,localLightRange,localLightInnerCos;
        UINT resolution[2];UINT environmentIndexOffset;float localLightOuterCos;
        float grassSettings[4];float groundSettings[4];float grassInteraction[4];
        float waterState[4];
        float sceneSettings[4];
        float jitter[2];float prevJitter[2];
        float prevEye[3];float prevTanHalf;
        float prevForward[3];float prevAspect;
        float prevRight[3];float prevTime;
        float prevUp[3];float gbufferWrite;
    }c{{eye.x,eye.y,eye.z},tanHalf,
       {forward.x,forward.y,forward.z},renderAspect,
       {right.x,right.y,right.z},shaderFrame,{up.x,up.y,up.z},temporalFrames,
        1.28f,localLight.enabled?localLight.intensity:0.0f,localLight.range,
        localLight.spotlight?std::cos(localLight.innerConeRadians):-1.0f,
        {static_cast<UINT>(i.renderWidth),static_cast<UINT>(i.renderHeight)},
        i.treeIndexCount,
        localLight.spotlight?std::cos(localLight.outerConeRadians):-1.0f,
         {settings.grassDensity,settings.bladeHeightScale,settings.shortGrassDrawDistance,
         settings.tallGrassDrawDistance},
         {settings.groundNormalStrength,settings.groundDetailStrength,
          static_cast<float>(nearGrassStride),view.grassInteractionEnabled?1.0f:0.0f},
         {view.grassInteractionPosition.x,view.grassInteractionPosition.z,
          view.grassInteractionVelocity.x,view.grassInteractionVelocity.z},
         {cameraUnderwater?1.0f:0.0f,water.surfaceHeight,cameraImmersion,
          i.customWorld?(water.inside?-2.0f:-1.0f):
                        (water.inside?1.0f:0.0f)},
         {view.oakForestEnvironment?1.0f:0.0f,static_cast<float>(i.experiment),0,0},
         {jitterX,jitterY},{prevCamera.jitter[0],prevCamera.jitter[1]},
         {prevCamera.eye[0],prevCamera.eye[1],prevCamera.eye[2]},prevCamera.tanHalf,
         {prevCamera.forward[0],prevCamera.forward[1],prevCamera.forward[2]},prevCamera.aspect,
         {prevCamera.right[0],prevCamera.right[1],prevCamera.right[2]},prevCamera.time,
         {prevCamera.up[0],prevCamera.up[1],prevCamera.up[2]},1.0f};
    static_assert(sizeof(Camera)==256);
    static_assert(offsetof(Camera,localLightIntensity)==68);
    static_assert(offsetof(Camera,resolution)==80);
    static_assert(offsetof(Camera,grassSettings)==96);
    static_assert(offsetof(Camera,grassInteraction)==128);
    static_assert(offsetof(Camera,waterState)==144);
    static_assert(offsetof(Camera,sceneSettings)==160);
    static_assert(offsetof(Camera,jitter)==176);
    // The mapped constants are single-buffered.  begin() waits for the prior
    // submission before we overwrite them, preventing the previous frame's
    // ray/compute work from observing partially updated camera or wind data.
    if(!i.cameraMapped||!i.environmentMapped)return;
    if(!i.beginRecording())return;
    if(i.submittedFrames<4){
        FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a");
        if(boot){fputs("render_recorded_cb\n",boot);fclose(boot);}
    }
    std::memcpy(i.cameraMapped,&c,sizeof(c));
    std::memcpy(i.environmentMapped,&environment,sizeof(environment));
    if(!i.recordVisibleGrassUpload(settings.grassDensity>.001f)){
        i.lastError=L"GPU-local visible-grass upload failed.";
        i.list->Close();return;
    }
    if(!i.recordTreeWind(environment)){
        i.lastError=L"GPU tree-wind deformation or acceleration refit failed.";
        i.list->Close();return;
    }
    ID3D12DescriptorHeap*heaps[]={i.gpuHeap};i.list->SetDescriptorHeaps(1,heaps);
    i.list->SetComputeRootSignature(i.root);i.list->SetPipelineState1(i.state);
    i.list->SetComputeRootDescriptorTable(0,i.gpuHeap->GetGPUDescriptorHandleForHeapStart());
    i.list->SetComputeRootShaderResourceView(1,i.tlas->GetGPUVirtualAddress());
    i.list->SetComputeRootShaderResourceView(2,i.vertexBuffer->GetGPUVirtualAddress());
    i.list->SetComputeRootShaderResourceView(3,i.indexBuffer->GetGPUVirtualAddress());
    i.list->SetComputeRootConstantBufferView(4,i.cameraGpu?i.cameraGpu:i.cameraBuffer->GetGPUVirtualAddress());
    i.list->SetComputeRootShaderResourceView(5,(i.grassBuffer?i.grassBuffer:i.vertexBuffer)->GetGPUVirtualAddress());
    i.list->SetComputeRootConstantBufferView(6,i.environmentGpu?i.environmentGpu:i.environmentBuffer->GetGPUVirtualAddress());
    i.list->SetComputeRootShaderResourceView(7,(i.instanceGeometryBuffer?i.instanceGeometryBuffer:i.vertexBuffer)->GetGPUVirtualAddress());
    const D3D12_GPU_VIRTUAL_ADDRESS fallbackVertices=i.vertexBuffer->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS fallbackIndices=i.indexBuffer->GetGPUVirtualAddress();
    i.list->SetComputeRootShaderResourceView(8,i.standingVertexBuffer?
        i.standingVertexBuffer->GetGPUVirtualAddress():fallbackVertices);
    i.list->SetComputeRootShaderResourceView(9,i.standingIndexBuffer?
        i.standingIndexBuffer->GetGPUVirtualAddress():fallbackIndices);
    i.list->SetComputeRootShaderResourceView(10,i.detachedVertexBuffer?
        i.detachedVertexBuffer->GetGPUVirtualAddress():fallbackVertices);
    i.list->SetComputeRootShaderResourceView(11,i.detachedIndexBuffer?
        i.detachedIndexBuffer->GetGPUVirtualAddress():fallbackIndices);
    i.list->SetComputeRootShaderResourceView(12,i.axeVertexBuffer?
        i.axeVertexBuffer->GetGPUVirtualAddress():fallbackVertices);
    i.list->SetComputeRootShaderResourceView(13,i.axeIndexBuffer?
        i.axeIndexBuffer->GetGPUVirtualAddress():fallbackIndices);
    i.list->SetComputeRootShaderResourceView(14,i.grassBladeBuffer?
        i.grassBladeBuffer->GetGPUVirtualAddress():i.grassBuffer->GetGPUVirtualAddress());
    D3D12_DISPATCH_RAYS_DESC rays{};
    rays.RayGenerationShaderRecord={i.raygenTable->GetGPUVirtualAddress(),
                                    D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES};
    rays.MissShaderTable={i.missTable->GetGPUVirtualAddress(),
                          D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES*2,
                          D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES};
    rays.HitGroupTable={i.hitTable->GetGPUVirtualAddress(),
                        D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES*4,
                        D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES};
    rays.Width=static_cast<UINT>(i.renderWidth);rays.Height=static_cast<UINT>(i.renderHeight);rays.Depth=1;
    if(!i.raygenTable||!i.missTable||!i.hitTable||!i.tlas)return;
    if(i.submittedFrames<4){
        FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a");
        if(boot){fprintf(boot,"render_dispatch %dx%d\n",i.renderWidth,i.renderHeight);fclose(boot);}
    }
    i.list->DispatchRays(&rays);
    if(i.submittedFrames<4){
        FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a");
        if(boot){fputs("render_after_rays\n",boot);fclose(boot);}
    }
    const UINT back=i.swap->GetCurrentBackBufferIndex();
    i.lastBack=back;
    D3D12_RESOURCE_BARRIER afterRays[]={
        transition(i.output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_RENDER_TARGET),
        transition(i.accumulation,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
    i.list->ResourceBarrier(2,afterRays);

    D3D12_CPU_DESCRIPTOR_HANDLE grassTarget=i.rtvCpu(2);
    const D3D12_CPU_DESCRIPTOR_HANDLE depthTarget=i.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    i.list->OMSetRenderTargets(1,&grassTarget,FALSE,&depthTarget);
    i.list->ClearDepthStencilView(depthTarget,D3D12_CLEAR_FLAG_DEPTH,1.0f,0,0,nullptr);
    D3D12_VIEWPORT viewport{0,0,static_cast<float>(i.renderWidth),static_cast<float>(i.renderHeight),0,1};
    D3D12_RECT scissor{0,0,i.renderWidth,i.renderHeight};i.list->RSSetViewports(1,&viewport);i.list->RSSetScissorRects(1,&scissor);
    if(i.grassPipeline&&i.grassRoot&&settings.grassDensity>.001f){
        i.list->SetGraphicsRootSignature(i.grassRoot);i.list->SetPipelineState(i.grassPipeline);
        i.list->SetGraphicsRootConstantBufferView(0,i.cameraGpu?i.cameraGpu:i.cameraBuffer->GetGPUVirtualAddress());
        i.list->SetGraphicsRootShaderResourceView(1,i.visibleGrassGpu?i.visibleGrassGpu:i.visibleGrassBuffer->GetGPUVirtualAddress());
        D3D12_GPU_DESCRIPTOR_HANDLE grassTextures=i.gpuHeap->GetGPUDescriptorHandleForHeapStart();
        grassTextures.ptr+=10ull*i.srvSize;i.list->SetGraphicsRootDescriptorTable(2,grassTextures);
        D3D12_GPU_DESCRIPTOR_HANDLE grassGbuffers=i.gpuHeap->GetGPUDescriptorHandleForHeapStart();
        grassGbuffers.ptr+=2ull*i.srvSize;i.list->SetGraphicsRootDescriptorTable(6,grassGbuffers);
        i.list->SetGraphicsRootConstantBufferView(4,i.environmentGpu?i.environmentGpu:i.environmentBuffer->GetGPUVirtualAddress());
        i.list->SetGraphicsRootShaderResourceView(5,i.tlas->GetGPUVirtualAddress());
        i.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if(i.visibleNearGrassPatchCount){
            const UINT drawConstants[]={0u,nearGrassStride};
            i.list->SetGraphicsRoot32BitConstants(3,2,drawConstants,0);
            i.list->DrawInstanced(12,i.visibleNearGrassPatchCount*nearGrassStride,0,0);
        }
        if(i.visibleFarGrassPatchCount){
            const UINT drawConstants[]={i.grassPatchCount-i.visibleFarGrassPatchCount,
                                        farGrassStride};
            i.list->SetGraphicsRoot32BitConstants(3,2,drawConstants,0);
            i.list->DrawInstanced(12,i.visibleFarGrassPatchCount*farGrassStride,0,0);
        }
    }
    D3D12_RESOURCE_BARRIER afterGrass[]={
        transition(i.output,D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(i.linearDepth,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(i.motionVectors,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(i.normalRough,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(i.diffuseAlbedo,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(i.specularAlbedo,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    };
    i.list->ResourceBarrier(6,afterGrass);

    ID3D12Resource*ptHdr=i.output;
    i.lastHdr=i.output;

    ID3D12Resource*displayHdr=ptHdr;
    D3D12_RESOURCE_STATES displayHdrState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    i.upscaleThisFrame=false;
    if(useDlss && i.sl.status().dlss){
        i.upscaleThisFrame=i.sl.evaluateUpscale(
            i.list,ptHdr,i.linearDepth,i.motionVectors,i.normalRough,
            i.diffuseAlbedo,i.specularAlbedo,i.dlssOutput,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if(i.upscaleThisFrame){
            displayHdr=i.dlssOutput;
            displayHdrState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC presentView{};
    presentView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    presentView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    presentView.Texture2D.MipLevels=1;
    presentView.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    if(displayHdrState!=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE){
        auto toSrv=transition(displayHdr,displayHdrState,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        i.list->ResourceBarrier(1,&toSrv);
        displayHdrState=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    i.device->CreateShaderResourceView(displayHdr,&presentView,i.heapCpu(12));

    D3D12_CPU_DESCRIPTOR_HANDLE backTarget=i.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    backTarget.ptr+=static_cast<SIZE_T>(back)*i.rtvSize;
    auto toBack=transition(i.backBuffers[back],D3D12_RESOURCE_STATE_PRESENT,
                           D3D12_RESOURCE_STATE_RENDER_TARGET);
    i.list->ResourceBarrier(1,&toBack);
    i.list->OMSetRenderTargets(1,&backTarget,FALSE,nullptr);
    const float backClear[4]{0.02f,0.03f,0.04f,1};
    i.list->ClearRenderTargetView(backTarget,backClear,0,nullptr);
    D3D12_VIEWPORT displayViewport{0,0,static_cast<float>(i.width),static_cast<float>(i.height),0,1};
    D3D12_RECT displayScissor{0,0,i.width,i.height};
    i.list->RSSetViewports(1,&displayViewport);i.list->RSSetScissorRects(1,&displayScissor);
    if(i.presentPipeline&&i.presentRoot&&i.presentMapped){
        struct PresentConstants{float resolution[2];float grain;float pad;}presentConstants{
            {static_cast<float>(i.width),static_cast<float>(i.height)},
            static_cast<float>(i.frameIndex),i.upscaleThisFrame?1.0f:0.0f};
        std::memcpy(i.presentMapped,&presentConstants,sizeof(presentConstants));
        i.list->SetGraphicsRootSignature(i.presentRoot);
        i.list->SetPipelineState(i.presentPipeline);
        D3D12_GPU_DESCRIPTOR_HANDLE presentSrv=i.gpuHeap->GetGPUDescriptorHandleForHeapStart();
        presentSrv.ptr+=12ull*i.srvSize;
        i.list->SetGraphicsRootDescriptorTable(0,presentSrv);
        i.list->SetGraphicsRootConstantBufferView(1,i.presentGpu?i.presentGpu:i.presentBuffer->GetGPUVirtualAddress());
        i.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        i.list->DrawInstanced(3,1,0,0);
    }

    const float uiClearColor[4]{0,0,0,0};
    i.list->ClearRenderTargetView(i.rtvCpu(3),uiClearColor,0,nullptr);
    if(i.hudPipeline&&i.hudRoot&&i.hudMapped&&i.hud.visible){
        struct HudConstants{
            float resolution[2];float fps;float frameMs;
            UINT blades;UINT patches;UINT width;UINT height;
            float titleAlpha;float hudAlpha;float timeOfDay;float cinematic;
            float tempC;float util;float powerW;float frameMsP1;
            float vramGiB;float pad0;float pad1;float pad2;
            float displayFps;float displayMs;UINT mfgMul;UINT dlssMode;
            UINT gpuChars[16];
        }hudConstants{
            {static_cast<float>(i.width),static_cast<float>(i.height)},
            i.hud.fps,i.hud.frameMs,
            i.hud.blades?i.hud.blades:i.grassBladeCount,
            i.hud.patches?i.hud.patches:i.grassPatchCount,
            static_cast<UINT>(i.width),static_cast<UINT>(i.height),
            i.hud.titleAlpha,i.hud.hudAlpha,i.hud.timeOfDay,
            i.hud.cinematic?1.0f:0.0f,
            i.hud.gpuTempC,i.hud.gpuUtil,i.hud.powerW,i.hud.frameMsP1,
            i.hud.vramUsedGiB,i.hud.clockMHz,i.hud.choking?1.0f:0.0f,
            static_cast<float>(i.hud.experiment?i.hud.experiment:i.experiment),
            i.hud.displayFps,i.hud.displayFrameMs,i.hud.mfgMultiplier,i.hud.dlssMode,{}
        };
        for(int word=0;word<16;++word){
            UINT packed=0;
            for(int byte=0;byte<4;++byte){
                const char ch=i.hud.gpuName[word*4+byte];
                if(!ch)break;
                packed|=static_cast<UINT>(static_cast<unsigned char>(ch))<<(byte*8);
            }
            hudConstants.gpuChars[word]=packed;
        }
        std::memcpy(i.hudMapped,&hudConstants,sizeof(hudConstants));
        i.list->SetGraphicsRootSignature(i.hudRoot);
        i.list->SetPipelineState(i.hudPipeline);
        i.list->SetGraphicsRootConstantBufferView(0,i.hudGpu?i.hudGpu:i.hudBuffer->GetGPUVirtualAddress());
        i.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        i.list->DrawInstanced(3,1,0,0);
        D3D12_CPU_DESCRIPTOR_HANDLE uiTarget=i.rtvCpu(3);
        i.list->OMSetRenderTargets(1,&uiTarget,FALSE,nullptr);
        i.list->DrawInstanced(3,1,0,0);
        i.list->OMSetRenderTargets(1,&backTarget,FALSE,nullptr);
    }

    i.sl.tagFrameGeneration(displayHdr,i.uiColor,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_RESOURCE_BARRIER restore[]={
        transition(i.output,displayHdr==i.output?displayHdrState:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transition(i.accumulation,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transition(i.linearDepth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transition(i.motionVectors,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transition(i.normalRough,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transition(i.diffuseAlbedo,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transition(i.specularAlbedo,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transition(i.backBuffers[back],D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PRESENT)
    };
    i.list->ResourceBarrier(8,restore);
    if(displayHdr==i.dlssOutput){
        auto resetDlss=transition(i.dlssOutput,displayHdrState,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        i.list->ResourceBarrier(1,&resetDlss);
    }
    i.sl.markerRenderEnd();
    if(i.executeAsync()){
        i.sl.markerPresentStart();
        HRESULT presentHr=i.swap->Present(i.vsync?1u:0u,0);
        i.sl.markerPresentEnd();
        i.sl.queryPresentedFrames();
        i.frameIndex=std::min<UINT>(i.frameIndex+1,1023);
        i.prevCamera=currentCamera;
        i.havePrevCamera=true;
        if(i.submittedFrames<=4){
            FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a");
            if(boot){fprintf(boot,"render_present %u hr 0x%08X\n",
                             i.submittedFrames,static_cast<unsigned>(presentHr));fclose(boot);}
        }
        if(FAILED(presentHr)){
            HRESULT reason=i.device->GetDeviceRemovedReason();
            FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a");
            if(boot){fprintf(boot,"device_removed present=0x%08X reason=0x%08X\n",
                             static_cast<unsigned>(presentHr),
                             static_cast<unsigned>(reason));fclose(boot);}
            i.lastError=L"Present failed (device removed).";
            i.deviceLost=true;
            return;
        }
        if((i.submittedFrames%30u)==0u){
            FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a");
            if(boot){fprintf(boot,"render_ok %u\n",i.submittedFrames);fclose(boot);}
        }
    }
}

void DxrRenderer::setHud(const HudState&hud){
    impl_->hud=hud;
}

void DxrRenderer::setVsync(bool enabled){
    impl_->vsync=enabled;
}

void DxrRenderer::setDlssQuality(DlssQuality quality){
    auto&i=*impl_;
    if(!i.initialized){i.sl.setQuality(quality);return;}
    i.wait();
    i.sl.configure(static_cast<std::uint32_t>(i.width),static_cast<std::uint32_t>(i.height),
                   quality,i.sl.frameGen());
    if(i.sl.upscaleActive()&&i.sl.status().renderWidth&&i.sl.status().renderHeight){
        i.renderWidth=static_cast<int>(i.sl.status().renderWidth);
        i.renderHeight=static_cast<int>(i.sl.status().renderHeight);
    }else{
        i.renderWidth=i.width;i.renderHeight=i.height;
    }
    i.createOutputs();
    i.createDlssFeature();
}

void DxrRenderer::setExperiment(std::uint32_t index){
    impl_->experiment=index%8u;
}

std::uint32_t DxrRenderer::experiment()const{
    return impl_->experiment;
}

void DxrRenderer::setFrameGenMode(FrameGenMode mode){
    auto&i=*impl_;
    if(!i.initialized){i.sl.setFrameGen(mode);return;}
    i.wait();
    i.sl.configure(static_cast<std::uint32_t>(i.width),static_cast<std::uint32_t>(i.height),
                   i.sl.quality(),mode);
}

const StreamlineStatus&DxrRenderer::streamStatus()const{
    return impl_->sl.status();
}

void DxrRenderer::setOfflineAccumulate(std::uint32_t samplesPerFrame){
    auto&i=*impl_;
    i.offlineSpp=samplesPerFrame;
    i.frameIndex=0;
}

namespace {
float halfToFloat(std::uint16_t h){
    const unsigned s=(h>>15)&1u;
    unsigned e=(h>>10)&31u;
    unsigned m=h&1023u;
    unsigned out=0;
    if(e==0){
        if(m==0)out=s<<31;
        else{
            e=1;
            while((m&0x400u)==0){m<<=1;--e;}
            m&=1023u;
            out=(s<<31)|((e+127-15)<<23)|(m<<13);
        }
    }else if(e==31)out=(s<<31)|0x7f800000u|(m<<13);
    else out=(s<<31)|((e+127-15)<<23)|(m<<13);
    float value=0;
    std::memcpy(&value,&out,sizeof(value));
    return value;
}

bool encodePngRgba(const wchar_t* path,const std::uint8_t* rgba,UINT width,UINT height,UINT stride){
    if(!path||!rgba||!width||!height)return false;
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    IWICImagingFactory* factory{};
    HRESULT hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,
        __uuidof(IWICImagingFactory),reinterpret_cast<void**>(&factory));
    if(FAILED(hr)||!factory)return false;
    IWICStream* stream{};
    IWICBitmapEncoder* encoder{};
    IWICBitmapFrameEncode* frame{};
    bool ok=false;
    if(SUCCEEDED(factory->CreateStream(&stream))&&
       SUCCEEDED(stream->InitializeFromFilename(path,GENERIC_WRITE))&&
       SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))&&
       SUCCEEDED(encoder->Initialize(stream,WICBitmapEncoderNoCache))&&
       SUCCEEDED(encoder->CreateNewFrame(&frame,nullptr))&&
       SUCCEEDED(frame->Initialize(nullptr))&&
       SUCCEEDED(frame->SetSize(width,height))){
        WICPixelFormatGUID format=GUID_WICPixelFormat32bppRGBA;
        if(SUCCEEDED(frame->SetPixelFormat(&format))&&
           SUCCEEDED(frame->WritePixels(height,stride,stride*height,const_cast<BYTE*>(rgba)))&&
           SUCCEEDED(frame->Commit())&&
           SUCCEEDED(encoder->Commit()))
            ok=true;
    }
    if(frame)frame->Release();
    if(encoder)encoder->Release();
    if(stream)stream->Release();
    factory->Release();
    return ok;
}

}

bool DxrRenderer::writeDisplayPng(const wchar_t* path){
    auto&i=*impl_;
    if(!path||!i.initialized||!i.swap||!i.readback)return false;
    i.wait();

    std::vector<float> hdr;
    const UINT hdrW=static_cast<UINT>(i.renderWidth);
    const UINT hdrH=static_cast<UINT>(i.renderHeight);
    if(i.lastHdr&&i.hdrReadback&&hdrW&&hdrH){
        if(!i.begin())return false;
        auto toCopy=transition(i.lastHdr,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);
        i.list->ResourceBarrier(1,&toCopy);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource=i.hdrReadback;
        dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset=0;
        dst.PlacedFootprint.Footprint.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        dst.PlacedFootprint.Footprint.Width=hdrW;
        dst.PlacedFootprint.Footprint.Height=hdrH;
        dst.PlacedFootprint.Footprint.Depth=1;
        dst.PlacedFootprint.Footprint.RowPitch=static_cast<UINT>(i.hdrReadbackPitch);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource=i.lastHdr;
        src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        i.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        auto toUav=transition(i.lastHdr,D3D12_RESOURCE_STATE_COPY_SOURCE,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        i.list->ResourceBarrier(1,&toUav);
        if(!i.execute())return false;
        void* mapped{};
        const D3D12_RANGE range{0,i.hdrReadbackPitch*hdrH};
        if(SUCCEEDED(i.hdrReadback->Map(0,&range,&mapped))&&mapped){
            hdr.resize(static_cast<size_t>(hdrW)*hdrH*4);
            for(UINT y=0;y<hdrH;++y){
                const auto* row=reinterpret_cast<const std::uint16_t*>(
                    static_cast<const char*>(mapped)+static_cast<size_t>(y)*i.hdrReadbackPitch);
                for(UINT x=0;x<hdrW;++x){
                    const size_t o=(static_cast<size_t>(y)*hdrW+x)*4;
                    hdr[o+0]=halfToFloat(row[x*4+0]);
                    hdr[o+1]=halfToFloat(row[x*4+1]);
                    hdr[o+2]=halfToFloat(row[x*4+2]);
                    hdr[o+3]=halfToFloat(row[x*4+3]);
                }
            }
            i.hdrReadback->Unmap(0,nullptr);
        }
    }

    float hdrPeak=0;
    if(!hdr.empty()){
        for(size_t n=0;n<hdr.size();n+=4)
            hdrPeak=std::max(hdrPeak,std::max(hdr[n],std::max(hdr[n+1],hdr[n+2])));
    }
    std::vector<float> denoised;
    bool usedTensor=false;
    if(!hdr.empty()&&hdrPeak>1e-4f){
        denoised=hdr;
        usedTensor=tensorDenoiseRgba(hdr.data(),denoised.data(),hdrW,hdrH);
    }
    if(FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a")){
        fprintf(boot,"png_hdr peak %.5f tensor %d %s\n",
                hdrPeak,usedTensor?1:0,tensorDenoiseStatus().label);
        fclose(boot);
    }
    // Tensor denoise already ran above to occupy the unused tensor cores.
    // The PNG is the presented backbuffer so the video matches the live look.

    ID3D12Resource* back=i.backBuffers[i.lastBack];
    if(!back)return false;
    const UINT dw=static_cast<UINT>(i.width);
    const UINT dh=static_cast<UINT>(i.height);
    if(!i.begin())return false;
    const bool useCopyEngine=i.copyQueue&&i.copyAlloc&&i.copyList;
    auto toShare=transition(back,D3D12_RESOURCE_STATE_PRESENT,
                            useCopyEngine?D3D12_RESOURCE_STATE_COMMON:D3D12_RESOURCE_STATE_COPY_SOURCE);
    i.list->ResourceBarrier(1,&toShare);
    if(!useCopyEngine){
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource=i.readback;
        dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width=dw;
        dst.PlacedFootprint.Footprint.Height=dh;
        dst.PlacedFootprint.Footprint.Depth=1;
        dst.PlacedFootprint.Footprint.RowPitch=static_cast<UINT>(i.readbackPitch);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource=back;
        src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        i.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        auto toPresent=transition(back,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);
        i.list->ResourceBarrier(1,&toPresent);
        if(!i.execute())return false;
    }else{
        if(!i.execute())return false;
        if(i.copyQueue&&i.copyFence&&i.copyFenceEvent){
            const UINT64 value=++i.copyFenceValue;
            if(SUCCEEDED(i.copyQueue->Signal(i.copyFence,value))&&i.copyFence->GetCompletedValue()<value){
                i.copyFence->SetEventOnCompletion(value,i.copyFenceEvent);
                WaitForSingleObject(i.copyFenceEvent,INFINITE);
            }
        }
        if(FAILED(i.copyAlloc->Reset())||FAILED(i.copyList->Reset(i.copyAlloc,nullptr)))
            return false;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource=i.readback;
        dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width=dw;
        dst.PlacedFootprint.Footprint.Height=dh;
        dst.PlacedFootprint.Footprint.Depth=1;
        dst.PlacedFootprint.Footprint.RowPitch=static_cast<UINT>(i.readbackPitch);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource=back;
        src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        i.copyList->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        if(FAILED(i.copyList->Close()))return false;
        ID3D12CommandList* lists[]={i.copyList};
        i.copyQueue->ExecuteCommandLists(1,lists);
        if(i.copyQueue&&i.copyFence&&i.copyFenceEvent){
            const UINT64 value=++i.copyFenceValue;
            if(SUCCEEDED(i.copyQueue->Signal(i.copyFence,value))&&i.copyFence->GetCompletedValue()<value){
                i.copyFence->SetEventOnCompletion(value,i.copyFenceEvent);
                WaitForSingleObject(i.copyFenceEvent,INFINITE);
            }
        }
        if(!i.begin())return false;
        auto toPresent=transition(back,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PRESENT);
        i.list->ResourceBarrier(1,&toPresent);
        if(!i.execute())return false;
    }

    void* mapped{};
    const D3D12_RANGE range{0,i.readbackPitch*dh};
    if(FAILED(i.readback->Map(0,&range,&mapped))||!mapped)return false;
    std::vector<std::uint8_t> rgba(static_cast<size_t>(dw)*dh*4);
    for(UINT y=0;y<dh;++y){
        const auto* row=static_cast<const std::uint8_t*>(mapped)+static_cast<size_t>(y)*i.readbackPitch;
        std::memcpy(rgba.data()+static_cast<size_t>(y)*dw*4,row,static_cast<size_t>(dw)*4);
    }
    i.readback->Unmap(0,nullptr);
    return encodePngRgba(path,rgba.data(),dw,dh,dw*4);
}

void DxrRenderer::render(float yaw,float pitch,float distance,
                         const DebugRenderSettings&settings,
                         const EnvironmentCB&environment){
    const Vec3 target{0.0f,4.1f,0.0f};
    Vec3 eye=target+Vec3{std::sin(yaw)*std::cos(pitch)*distance,
                         std::sin(pitch)*distance,
                         -std::cos(yaw)*std::cos(pitch)*distance};
    eye.y=std::max(eye.y,EnvironmentGenerator::terrainHeight(eye.x,eye.z)+.34f);
    render(CameraView{eye,normalize(target-eye)},settings,environment,PlayerLocalLight{});
}

const wchar_t*DxrRenderer::error()const{return impl_->lastError.c_str();}bool DxrRenderer::ready()const{return impl_->initialized&&!impl_->deviceLost;}
std::uint32_t DxrRenderer::pathTracedBladeCount()const{return impl_->grassBladeCount;}
std::uint32_t DxrRenderer::visibleNearPatches()const{return impl_->visibleNearGrassPatchCount;}
std::uint32_t DxrRenderer::visibleFarPatches()const{return impl_->visibleFarGrassPatchCount;}
}
