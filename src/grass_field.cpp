#include "grass_field.hpp"

#include "math.hpp"

#include <algorithm>
#include <cmath>

namespace grass {
namespace {

float hash2(float x, float z) {
    const float n=std::sin(x*127.1f+z*311.7f)*43758.5453f;
    return n-std::floor(n);
}

float valueNoise(float x, float z) {
    const float ix=std::floor(x),iz=std::floor(z);
    const float fx=x-ix,fz=z-iz;
    const float ux=fx*fx*(3.0f-2.0f*fx);
    const float uz=fz*fz*(3.0f-2.0f*fz);
    const float a=hash2(ix,iz);
    const float b=hash2(ix+1.0f,iz);
    const float c=hash2(ix,iz+1.0f);
    const float d=hash2(ix+1.0f,iz+1.0f);
    return (a+(b-a)*ux)+(c-a+(d-b-c+a)*ux)*uz;
}

float fbm(float x, float z) {
    return valueNoise(x,z)*.57f+valueNoise(x*2.13f+8.1f,z*2.13f)*.28f+
           valueNoise(x*4.27f-3.4f,z*4.27f)*.15f;
}

std::uint32_t hashUint(std::uint32_t x) {
    x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;
    return x;
}

float randomUint(std::uint32_t x) {
    return static_cast<float>(hashUint(x)&0x00ffffffu)*(1.0f/16777216.0f);
}

void emitPathTracedBlades(dense::EnvironmentMesh& mesh) {
    mesh.grassBlades.clear();
    mesh.grassBlades.reserve(static_cast<size_t>(kPatchesX)*kPatchesZ*kBladesPerPatch);
    for(std::uint32_t patchIndex=0;patchIndex<mesh.grassPatches.size();++patchIndex) {
        const dense::GrassPatchGpu& patch=mesh.grassPatches[patchIndex];
        const std::uint32_t bladeCount=patch.packed&255u;
        const std::uint32_t tallCount=(patch.packed>>16)&255u;
        const float shortMaximum=static_cast<float>((patch.packed>>8)&255u)*.004f;
        const float tallMaximum=static_cast<float>((patch.packed>>24)&255u)*.004f;
        const dense::Vec3 normal=dense::normalize(dense::Vec3{
            patch.normalX,
            std::sqrt(std::max(0.0f,1.0f-patch.normalX*patch.normalX-patch.normalZ*patch.normalZ)),
            patch.normalZ});
        const dense::Vec3 axisX=dense::normalize(dense::Vec3{
            1.0f,-normal.x/std::max(normal.y,.25f),0.0f});
        const dense::Vec3 axisZ=dense::normalize(dense::cross(axisX,normal));
        const dense::Vec3 center{(patch.minX+patch.maxX)*.5f,patch.baseY,
                                 (patch.minZ+patch.maxZ)*.5f};
        const std::uint32_t patchRandomSeed=patch.seed&0x00ffffffu;
        for(std::uint32_t bladeIndex=0;bladeIndex<bladeCount;++bladeIndex) {
            const std::uint32_t seed=hashUint(patchRandomSeed^((bladeIndex+1u)*0x9e3779b9u));
            const float tall=bladeIndex<tallCount?1.0f:0.0f;
            const float radius=std::sqrt(randomUint(seed))* (tall>.5f?.065f:.245f);
            const float offsetAngle=randomUint(seed^0x68bc21ebu)*6.2831853f;
            const float clusterAngle=randomUint(patchRandomSeed^0x91e10da5u)*6.2831853f;
            const float clusterRadius=randomUint(patchRandomSeed^0x243f6a88u)*.095f*tall;
            const dense::Vec3 base=center
                +axisX*(std::cos(offsetAngle)*radius+std::cos(clusterAngle)*clusterRadius)
                +axisZ*(std::sin(offsetAngle)*radius+std::sin(clusterAngle)*clusterRadius);
            const float maximumHeight=shortMaximum+(tallMaximum-shortMaximum)*tall;
            const float height=maximumHeight*(.50f+.50f*randomUint(seed^0xa511e9b3u))*2.5f;
            const float reach=height*.58f+.016f;
            dense::GrassBladeGpu blade;
            blade.minX=base.x-reach;blade.minY=base.y-.03f;blade.minZ=base.z-reach;
            blade.maxX=base.x+reach;blade.maxY=base.y+height+reach*.22f;blade.maxZ=base.z+reach;
            blade.patchIndex=patchIndex;
            blade.bladeIndex=bladeIndex;
            mesh.grassBlades.push_back(blade);
        }
    }
}

} // namespace

float terrainHeight(float x, float z) {
    const float ridge=1.05f*std::sin(x*.018f)+.78f*std::sin(z*.023f+.9f);
    const float crossing=.48f*std::sin((x+z)*.039f)+
                         .32f*std::sin((x-z)*.071f+1.4f);
    const float swell=.22f*std::sin(x*.11f)*std::sin(z*.09f+0.6f);
    const float valley=-.62f*std::exp(-std::pow((z+6.0f+.08f*x)/24.0f,2.0f));
    const float micro=fbm(x*.07f+3.2f,z*.07f-1.8f)*.18f;
    return ridge+crossing+swell+valley+micro;
}

dense::Vec3 terrainNormal(float x, float z) {
    constexpr float e=.16f;
    return dense::normalize(dense::Vec3{
        terrainHeight(x-e,z)-terrainHeight(x+e,z),
        2.0f*e,
        terrainHeight(x,z-e)-terrainHeight(x,z+e)});
}

dense::TerrainSurfaceSample sampleTerrain(float x, float z) {
    dense::TerrainSurfaceSample sample;
    sample.insideBounds=std::abs(x)<=kTerrainHalfX&&std::abs(z)<=kTerrainHalfZ;
    x=dense::clamp(x,-kTerrainHalfX,kTerrainHalfX);
    z=dense::clamp(z,-kTerrainHalfZ,kTerrainHalfZ);
    sample.position={x,terrainHeight(x,z),z};
    sample.normal=terrainNormal(x,z);
    return sample;
}

std::uint32_t countedBlades(const dense::EnvironmentMesh& mesh) {
    std::uint32_t total=0;
    for(const dense::GrassPatchGpu& patch:mesh.grassPatches)
        total+=patch.packed&255u;
    return total;
}

dense::EnvironmentMesh build(std::uint32_t seed) {
    dense::EnvironmentMesh mesh;
    mesh.grassSeed=seed;

    constexpr int resolutionX=205;
    constexpr int resolutionZ=133;
    mesh.terrainVertices.reserve(static_cast<size_t>(resolutionX)*resolutionZ);
    mesh.terrainIndices.reserve(static_cast<size_t>(resolutionX-1)*(resolutionZ-1)*6);
    float minimum=1.0e9f,maximum=-1.0e9f;
    for(int iz=0;iz<resolutionZ;++iz) {
        for(int ix=0;ix<resolutionX;++ix) {
            const float x=-kTerrainHalfX+2.0f*kTerrainHalfX*
                          static_cast<float>(ix)/(resolutionX-1);
            const float z=-kTerrainHalfZ+2.0f*kTerrainHalfZ*
                          static_cast<float>(iz)/(resolutionZ-1);
            const auto surface=sampleTerrain(x,z);
            dense::MeshVertex vertex{};
            vertex.position=surface.position;
            vertex.normal=surface.normal;
            const float fertility=fbm(x*.031f,z*.031f);
            const float retention=dense::clamp(.42f+fertility*.38f-
                (1.0f-surface.normal.y)*1.4f,0.0f,1.0f);
            vertex.color=0x00284418u|
                (static_cast<std::uint32_t>(retention*255.0f+.5f)<<24);
            vertex.material=2.0f;
            vertex.u=x*.08f;
            vertex.v=z*.08f;
            mesh.terrainVertices.push_back(vertex);
            minimum=std::min(minimum,vertex.position.y);
            maximum=std::max(maximum,vertex.position.y);
        }
    }
    for(int iz=0;iz<resolutionZ-1;++iz) {
        for(int ix=0;ix<resolutionX-1;++ix) {
            const std::uint32_t a=static_cast<std::uint32_t>(iz*resolutionX+ix);
            const std::uint32_t b=a+1;
            const std::uint32_t c=a+static_cast<std::uint32_t>(resolutionX);
            const std::uint32_t d=c+1;
            if((ix+iz)&1)mesh.terrainIndices.insert(mesh.terrainIndices.end(),{a,c,b,b,c,d});
            else mesh.terrainIndices.insert(mesh.terrainIndices.end(),{a,c,d,a,d,b});
        }
    }
    mesh.minimumHeight=minimum;
    mesh.maximumHeight=maximum;

    dense::Rng rng(seed^0x9e3779b9u);
    mesh.grassPatches.reserve(static_cast<size_t>(kPatchesX)*kPatchesZ);
    constexpr float heightCodeStep=.004f;
    for(int iz=0;iz<kPatchesZ;++iz) {
        for(int ix=0;ix<kPatchesX;++ix) {
            const float x=-kHalfX+(static_cast<float>(ix)+rng.range(.12f,.88f))*kCell;
            const float z=-kHalfZ+(static_cast<float>(iz)+rng.range(.12f,.88f))*kCell;
            const auto surface=sampleTerrain(x,z);
            const dense::Vec3 normal=surface.normal;

            const float fertility=fbm(x*.033f+4.8f,z*.033f-2.1f);
            const float dryColony=dense::clamp(fbm(x*.019f-11.0f,z*.019f+7.4f)-.42f,0.0f,1.0f);
            const float lushColony=dense::clamp(fertility-.28f,0.0f,1.0f);
            const float warmCool=dense::clamp((fbm(x*.011f,z*.011f)-.5f)*1.6f,-1.0f,1.0f);
            const float moisture=dense::clamp(.56f+(fertility-.5f)*.42f-
                                              dryColony*.18f+rng.range(-.02f,.02f),0.0f,1.0f);

            const float shortHeight=rng.range(.22f,.36f);
            const float tallHeight=rng.range(.48f,.74f);
            const std::uint32_t shortCode=static_cast<std::uint32_t>(
                dense::clamp(shortHeight/heightCodeStep,1.0f,255.0f));
            const std::uint32_t tallCode=static_cast<std::uint32_t>(
                dense::clamp(tallHeight/heightCodeStep,1.0f,255.0f));
            const std::uint32_t packed=kBladesPerPatch|(shortCode<<8)|
                                       (kTallBladesPerPatch<<16)|(tallCode<<24);

            const float maxHeight=tallHeight*2.6f;
            const float slope=std::sqrt(normal.x*normal.x+normal.z*normal.z)/
                              std::max(normal.y,.25f);
            constexpr float safety=.04f;
            const float horizontalReach=.28f+maxHeight*(slope+.72f)+.02f+safety;
            const float surfaceRise=.28f*slope;
            const float lowerReach=surfaceRise+.02f*slope+safety;
            const float upperReach=surfaceRise+maxHeight*(normal.y+.72f*slope)+safety;
            const std::uint32_t retentionByte=static_cast<std::uint32_t>(
                dense::clamp((.40f+moisture*.55f)*255.0f+.5f,0.0f,255.0f));
            const std::uint32_t packedSeed=(rng.next()&0x00ffffffu)|(retentionByte<<24);
            const float baseY=surface.position.y+.008f;
            mesh.grassPatches.push_back({
                x-horizontalReach,baseY-lowerReach,z-horizontalReach,
                x+horizontalReach,baseY+upperReach,z+horizontalReach,
                packedSeed,packed,baseY,normal.x,normal.z,moisture,
                fertility,dryColony,lushColony,warmCool});
            ++mesh.tallGrassPatchCount;
        }
    }
    emitPathTracedBlades(mesh);
    return mesh;
}

} // namespace grass
