#pragma once
#include "environment.hpp"
#include "environment_simulation.hpp"
#include "streamline_host.hpp"
#include "tree.hpp"
#include <windows.h>
#include <functional>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dense {

struct AxeMesh;
struct AxeRigidTransform;

// Column axes plus an origin describe a rigid object-to-world transform.  The
// promoted pieces use the same object space as the generated source tree, so
// physics can move either piece without baking world-space vertices every
// frame.
struct RendererRigidTransform {
    Vec3 origin{};
    Vec3 xAxis{1.0f,0.0f,0.0f};
    Vec3 yAxis{0.0f,1.0f,0.0f};
    Vec3 zAxis{0.0f,0.0f,1.0f};
};

struct DebugRenderSettings {
    // The default sward is close-mown park turf.  Density supplies the fine
    // overlapping canopy while the authored blade heights remain physical.
    float grassDensity = 3.75f;
    float bladeHeightScale = 1.00f;
    float groundNormalStrength = 1.00f;
    float groundDetailStrength = 1.00f;
    float shortGrassDrawDistance = 3.402823466e+38f;
    float tallGrassDrawDistance = 3.402823466e+38f;
};

struct HudState {
    bool visible = true;
    float titleAlpha = 1.0f;
    float hudAlpha = 1.0f;
    float fps = 0.0f;
    float frameMs = 16.0f;
    float frameMsP1 = 16.0f;
    float frameMsMax = 16.0f;
    float gpuTempC = 0.0f;
    float gpuUtil = 0.0f;
    float powerW = 0.0f;
    float vramUsedGiB = 0.0f;
    float clockMHz = 0.0f;
    std::uint32_t blades = 60000000;
    std::uint32_t patches = 0;
    float timeOfDay = 18.35f;
    bool cinematic = true;
    bool choking = false;
    float displayFps = 0.0f;
    float displayFrameMs = 16.0f;
    std::uint32_t mfgMultiplier = 1;
    std::uint32_t dlssMode = 0;
    std::uint32_t experiment = 0;
    char gpuName[64]{};
};

struct CameraView {
    Vec3 eye{};
    Vec3 forward{0.0f,0.0f,1.0f};
    // First-person locomotion can expose a lightweight world-space grass
    // collider.  The raster grass shader evaluates it per blade; no grass
    // geometry is read back or rewritten on the CPU.
    Vec3 grassInteractionPosition{};
    Vec3 grassInteractionVelocity{};
    bool grassInteractionEnabled = false;
    bool oakForestEnvironment = false;
};

struct PlayerLocalLight {
    bool enabled = false;
    // The player light is an omnidirectional warm point light by default.
    // Spotlight mode remains available to callers without changing the b0 ABI.
    bool spotlight = false;
    // A small hand-carried lamp.  The shaders use inverse-square falloff, so
    // the previous value of 60 bleached nearby bark and fluorescently lit the
    // ground at arm's length.  Four renderer-candela units retain useful
    // visibility several metres out without driving diffuse albedo into the
    // tone-mapper's white shoulder.
    float intensity = 4.0f;
    float range = 16.0f;
    float innerConeRadians = 0.19198622f; // 11 degrees
    float outerConeRadians = 0.31415927f; // 18 degrees
};

class DxrRenderer {
public:
    using WaterSampler=std::function<PersistentWaterSample(float,float)>;
    DxrRenderer();~DxrRenderer();DxrRenderer(const DxrRenderer&)=delete;DxrRenderer& operator=(const DxrRenderer&)=delete;
    bool initialize(HWND window,int width,int height);
    void resize(int width,int height);
    // Must be called before setTree(). The visual-test scene may omit this and
    // retain the original EnvironmentGenerator defaults.
    void setWorld(EnvironmentMesh world,WaterSampler waterSampler);
    void setTree(std::shared_ptr<const TreeMesh> tree);
    void setTree(const TreeMesh& tree);
    // Optional biological-owner mask for interactive cutting. Branch
    // triangles whose owner is non-zero are placed in the small editable BLAS;
    // all other wood and foliage remain in the shared immutable BLAS. Call
    // before setTree(). An empty mask preserves the legacy all-wood behavior.
    void setCuttableBranchOwners(std::vector<unsigned char> ownerMask);
    // The axe is a real indexed triangle mesh in its own BLAS.  Visibility and
    // animation only refit the TLAS; they never rebuild the shared forest.
    bool setAxeMesh(const AxeMesh& axe);
    void setAxeState(bool visible,const AxeRigidTransform& transform);
    // Shared index 0 is the identity/hero instance and indices 1..N map to
    // TreeMesh::additionalInstances[index-1].  Promotion removes exactly that
    // instance from the shared BLAS and installs independently transformable
    // standing and detached geometry.
    bool promoteTreeInstance(std::size_t sharedInstanceIndex,
                             std::shared_ptr<const TreeMesh> standing,
                             std::shared_ptr<const TreeMesh> detached,
                             const TreeInstance& source);
    bool promoteTreeInstance(std::size_t sharedInstanceIndex,
                             const TreeMesh& standing,const TreeMesh& detached,
                             const TreeInstance& source);
    void clearPromotedTree();
    bool setPromotedTreeMeshes(std::shared_ptr<const TreeMesh> standing,
                               std::shared_ptr<const TreeMesh> detached);
    bool setPromotedTreeMeshes(const TreeMesh& standing,const TreeMesh& detached);
    void setPromotedTreeTransforms(const RendererRigidTransform& standing,
                                   const RendererRigidTransform& detached);
    // Topology-stable branch articulation path.  Only the promoted detached
    // vertex range is uploaded and its BLAS is refit; all shared oaks remain
    // untouched.  Returns false when topology no longer matches, in which case
    // setPromotedTreeMeshes() is the explicit rebuild path.
    bool updateDetachedTreeMesh(std::shared_ptr<const TreeMesh> detached);
    bool updateDetachedTreeMesh(const TreeMesh& detached);
    // One burst contains six to ten independently animated solid chips in a
    // single compact mesh/BLAS. Topology changes rebuild only that chip BLAS;
    // animation updates upload vertices and refit it in place.
    bool setWoodChipMesh(std::shared_ptr<const TreeMesh> chips);
    bool updateWoodChipMesh(std::shared_ptr<const TreeMesh> chips);
    void setWoodChipTransform(const RendererRigidTransform& transform);
    void setWoodChipsVisible(bool visible);
    void clearWoodChips();
    void setHud(const HudState& hud);
    void setVsync(bool enabled);
    void setDlssQuality(DlssQuality quality);
    void setFrameGenMode(FrameGenMode mode);
    void setExperiment(std::uint32_t index);
    std::uint32_t experiment()const;
    void setOfflineAccumulate(std::uint32_t speciesPerFrame);
    bool writeDisplayPng(const wchar_t* path);
    const StreamlineStatus& streamStatus() const;
    void render(const CameraView& view,const DebugRenderSettings& settings,
                const EnvironmentCB& environment,const PlayerLocalLight& localLight);
    // Compatibility wrapper for the current orbit-camera caller. New callers
    // should provide CameraView explicitly so a player/camera transform can own
    // the local light without being reconstructed inside the renderer.
    void render(float yaw,float pitch,float distance,const DebugRenderSettings& settings,
                const EnvironmentCB& environment);
    const wchar_t* error()const;
    bool ready()const;
    std::uint32_t pathTracedBladeCount()const;
    std::uint32_t visibleNearPatches()const;
    std::uint32_t visibleFarPatches()const;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
