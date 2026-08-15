#include "grass_field.hpp"

#include <cstdio>
#include <cstdlib>

int main() {
    const dense::EnvironmentMesh mesh=grass::build(5080);
    const std::uint32_t blades=grass::countedBlades(mesh);
    if(mesh.grassPatches.size()!=static_cast<size_t>(grass::kPatchesX)*grass::kPatchesZ){
        std::fprintf(stderr,"patch count %zu, expected %d\n",
                     mesh.grassPatches.size(),grass::kPatchesX*grass::kPatchesZ);
        return 1;
    }
    if(blades!=grass::kTargetBlades){
        std::fprintf(stderr,"blade count %u, expected %u\n",blades,grass::kTargetBlades);
        return 1;
    }
    if(mesh.grassBlades.size()!=grass::kTargetBlades){
        std::fprintf(stderr,"path-traced blade AABBs %zu, expected %u\n",
                     mesh.grassBlades.size(),grass::kTargetBlades);
        return 1;
    }
    if(mesh.terrainVertices.empty()||mesh.terrainIndices.empty()){
        std::fprintf(stderr,"terrain mesh is empty\n");
        return 1;
    }
    const auto origin=grass::sampleTerrain(0.0f,0.0f);
    if(!origin.insideBounds){
        std::fprintf(stderr,"origin is outside the meadow\n");
        return 1;
    }
    std::printf("OK: %u patches, %u blades\n",
                static_cast<unsigned>(mesh.grassPatches.size()),blades);
    return 0;
}
