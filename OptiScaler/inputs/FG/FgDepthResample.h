#pragma once

#include <cstdint>

// XeFG rejects motion vectors and depth of different sizes ("motion vector and depth resource resolutions must
// match"). On the upscaler path, display-resolution motion vectors are tagged at the target size while depth is
// tagged at the render size, so depth is resampled to the motion-vector size for XeFG when XeFGDepthResample is on.
namespace FgDepthResample
{
inline bool Needed(bool enabled, bool xefgOutput, bool lowResMotionVectors, uint32_t depthWidth, uint32_t depthHeight,
                   uint32_t motionWidth, uint32_t motionHeight)
{
    if (!enabled || !xefgOutput || lowResMotionVectors)
        return false;
    if (depthWidth == 0 || depthHeight == 0 || motionWidth == 0 || motionHeight == 0)
        return false;
    return depthWidth != motionWidth || depthHeight != motionHeight;
}
} // namespace FgDepthResample
