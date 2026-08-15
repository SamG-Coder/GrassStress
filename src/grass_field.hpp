#pragma once

#include "environment.hpp"

#include <cstdint>

namespace grass {

constexpr std::uint32_t kTargetBlades = 10000000u;
constexpr std::uint32_t kBladesPerPatch = 100u;
constexpr std::uint32_t kTallBladesPerPatch = 22u;
constexpr int kPatchesX = 400;
constexpr int kPatchesZ = 250;
constexpr float kCell = 0.48f;
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
