#pragma once

#include "environment.hpp"

#include <cstdint>

namespace grass {

constexpr std::uint32_t kTargetBlades = 60000000u;
constexpr std::uint32_t kBladesPerPatch = 160u;
constexpr std::uint32_t kTallBladesPerPatch = 36u;
constexpr int kPatchesX = 750;
constexpr int kPatchesZ = 500;
constexpr float kCell = 0.252f;
constexpr float kHalfX = 96.0f;
constexpr float kHalfZ = 60.0f;
constexpr float kTerrainHalfX = 102.0f;
constexpr float kTerrainHalfZ = 66.0f;

float terrainHeight(float x, float z);
dense::Vec3 terrainNormal(float x, float z);
dense::TerrainSurfaceSample sampleTerrain(float x, float z);
dense::EnvironmentMesh build(std::uint32_t seed = 5080u);
std::uint32_t countedBlades(const dense::EnvironmentMesh& mesh);

} // namespace grass
