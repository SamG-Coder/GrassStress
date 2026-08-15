#pragma once

#include "tree.hpp"

#include <cstdint>
#include <vector>

namespace dense {

// These are intent-space directions, not canned screen-space effects.  Every
// pose below is a rigid transform in world space and the sharpened edge is
// exposed explicitly for swept collision against tree geometry.
enum class AxeSwingDirection : std::uint8_t {
    LeftToRight,
    RightToLeft,
    Overhead
};

struct AxeChargeInput {
    AxeSwingDirection direction{AxeSwingDirection::Overhead};
    // Zero is the neutral first-person carry. One is a complete shoulder
    // backswing. Values supplied by callers are always clamped by the API.
    float charge{};
};

struct AxeCameraFrame {
    Vec3 eye{};
    Vec3 right{1,0,0};
    Vec3 up{0,1,0};
    Vec3 forward{0,0,1};
};

struct AxeRigidTransform {
    Vec3 origin{};
    Vec3 xAxis{1,0,0};
    Vec3 yAxis{0,1,0};
    Vec3 zAxis{0,0,1};

    Vec3 transformPoint(Vec3 point) const;
    Vec3 transformVector(Vec3 vector) const;
};

struct AxeMesh {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    // Local-space line along the actual sharpened bit. Collision code sweeps
    // this line, not the handle, head centre, cursor, or a camera ray.
    Vec3 bladeEdgeStart{};
    Vec3 bladeEdgeEnd{};
    Vec3 gripPoint{};
};

struct AxePose {
    AxeRigidTransform transform{};
    Vec3 bladeEdgeStart{};
    Vec3 bladeEdgeEnd{};
    Vec3 headCentre{};
    // Outward from the poll through the sharpened bit. During the powered
    // impact this leads the head velocity for all three swing directions.
    Vec3 bitDirection{1,0,0};
    float normalizedTime{};
};

// One closed ruled interval of the moving blade. A collision implementation
// can test both endpoint capsules and the two triangles joining the old and
// new edge, retaining the earliest time of impact.
struct AxeBladeSweep {
    Vec3 previousStart{};
    Vec3 previousEnd{};
    Vec3 currentStart{};
    Vec3 currentEnd{};
    float previousTime{};
    float currentTime{};
};

AxeMesh buildAxeMesh();
AxeSwingDirection classifyAxeSwing(float dragX,float dragY);
AxeChargeInput chargeAxeSwing(float dragX,float dragY,
                              float fullChargePixels=130.0f);
AxePose restAxePose(const AxeCameraFrame& camera,const AxeMesh& mesh);
AxePose previewAxeSwing(AxeChargeInput input,const AxeCameraFrame& camera,
                        const AxeMesh& mesh);
AxePose sampleAxeRelease(AxeSwingDirection direction,float charge,
                         float normalizedTime,const AxeCameraFrame& camera,
                         const AxeMesh& mesh);
// Compatibility sampler: a complete backswing released at full power.
AxePose sampleAxeSwing(AxeSwingDirection direction,float normalizedTime,
                       const AxeCameraFrame& camera,const AxeMesh& mesh);

class AxeSwingAnimation {
public:
    explicit AxeSwingAnimation(const AxeMesh& mesh);

    // Call every frame while LMB is held. The rendered pose responds
    // continuously to both drag direction and magnitude.
    void setCharge(AxeChargeInput input,const AxeCameraFrame& camera);
    void setCharge(float dragX,float dragY,const AxeCameraFrame& camera,
                   float fullChargePixels=130.0f);
    // Continues from the currently rendered charged pose without a reset.
    void release(const AxeCameraFrame& camera);
    // Full-charge compatibility entry point.
    void start(AxeSwingDirection direction,const AxeCameraFrame& camera);
    // Rewinds the rendered head to the exact continuous collision time and
    // arrests it briefly in the wood. The remaining follow-through resumes
    // after the hit-stop; collision remains driven by the same swept edge.
    void registerImpact(float normalizedTime,float holdSeconds=.060f);
    void cancel();
    std::vector<AxeBladeSweep> advance(float elapsedSeconds,
                                       const AxeCameraFrame& camera);
    AxePose pose(const AxeCameraFrame& camera) const;

    bool active() const { return active_; }
    bool charging() const { return charging_; }
    float charge() const { return charge_; }
    float normalizedTime() const { return normalizedTime_; }
    AxeSwingDirection direction() const { return direction_; }
    float swingDuration() const { return duration(direction_,releaseCharge_); }
    static float duration(AxeSwingDirection direction,float charge=1.0f);
    static constexpr float impactNormalizedTime() { return .28f; }

private:
    const AxeMesh* mesh_{};
    AxeSwingDirection direction_{AxeSwingDirection::Overhead};
    AxeCameraFrame releaseCamera_{};
    float charge_{};
    float releaseCharge_{1.0f};
    float normalizedTime_{};
    bool active_{};
    bool charging_{};
    float impactHoldSeconds_{};
    AxePose previousPose_{};
};

} // namespace dense
