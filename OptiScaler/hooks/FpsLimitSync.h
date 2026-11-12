#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include "Config.h"
#include "State.h"

enum class FpsLimitBackend : uint8_t
{
    None = 0,
    ReflexD3D,
    ReflexXeFG,
    ReflexVulkan,
    GameXeLL
};

namespace FpsLimitSync
{
inline std::atomic<uint64_t> g_revision { 1 };
inline std::atomic<FpsLimitBackend> g_activeBackend { FpsLimitBackend::None };

inline void NotifyConfigChanged() { g_revision.fetch_add(1, std::memory_order_release); }
inline uint64_t Revision() { return g_revision.load(std::memory_order_acquire); }
inline FpsLimitBackend ActiveBackend() { return g_activeBackend.load(std::memory_order_acquire); }
inline void SetActiveBackend(FpsLimitBackend backend) { g_activeBackend.store(backend, std::memory_order_release); }

inline void ReleaseBackend(FpsLimitBackend expected)
{
    FpsLimitBackend current = expected;
    g_activeBackend.compare_exchange_strong(current, FpsLimitBackend::None, std::memory_order_acq_rel);
}

inline bool SingleOwnerEnabled() { return Config::Instance()->FpsLimitSingleReflexOwner.value_or_default(); }
inline bool UseFgMultiplier() { return Config::Instance()->FpsLimitUseFgMultiplier.value_or_default(); }

inline float ConfiguredFpsLimit()
{
    const float limit = Config::Instance()->FramerateLimit.value_or_default();
    return (std::isfinite(limit) && limit > 0.0f) ? limit : 0.0f;
}

inline uint32_t FpsToIntervalUs(float fps)
{
    if (!std::isfinite(fps) || fps <= 0.0f)
        return 0u;
    const double us = 1'000'000.0 / static_cast<double>(fps);
    if (!std::isfinite(us) || us <= 0.0)
        return 0u;
    return static_cast<uint32_t>(std::clamp<int64_t>(std::llround(us), 1LL, 100'000'000LL));
}

inline FpsLimitBackend ResolveReflexRoute(bool isVulkan, bool xefgOutputActive, bool hasD3dSleepDev, bool hasVkSleepDev)
{
    if (isVulkan)
        return hasVkSleepDev ? FpsLimitBackend::ReflexVulkan : FpsLimitBackend::None;
    if (xefgOutputActive)
        return FpsLimitBackend::ReflexXeFG;
    if (hasD3dSleepDev)
        return FpsLimitBackend::ReflexD3D;
    return FpsLimitBackend::None;
}

inline bool ShouldReflexOwn(bool canReflexLimit, bool gameSendingMarkers, bool optiFgActive, bool gameXellCanLimit)
{
    if (!canReflexLimit)
        return false;
    if (SingleOwnerEnabled() && gameXellCanLimit && !gameSendingMarkers && !optiFgActive)
        return false;
    return true;
}

inline bool ShouldGameXellOwn(bool gameXellActive, bool reflexLimitsFps)
{
    if (!gameXellActive)
        return false;
    if (SingleOwnerEnabled() && reflexLimitsFps)
        return false;
    return true;
}

inline uint32_t ResolveFgMultiplier(bool optiFgActive, uint32_t optiInterpolatedCount,
                                      uint8_t asyncFgFramesToGenerate, uint32_t detectedDlssgCount,
                                      bool fsrfgInputActive)
{
    if (optiFgActive)
        return 1u + (std::max)(optiInterpolatedCount, 1u);
    const uint32_t extCount = (std::max)(static_cast<uint32_t>(asyncFgFramesToGenerate), detectedDlssgCount);
    if (extCount > 0u)
        return 1u + extCount;
    return fsrfgInputActive ? 2u : 1u;
}

inline float ComputeEffectiveReflexFps(float baseFps, uint32_t fgMultiplier, bool usingXellBackend)
{
    if (!std::isfinite(baseFps) || baseFps <= 0.0f)
        return 0.0f;
    // XeLL scales internally by its configured FG count when XeLL owns sleep cadence.
    if (usingXellBackend || !UseFgMultiplier() || fgMultiplier <= 1u)
        return baseFps;
    return baseFps / static_cast<float>(fgMultiplier);
}
} // namespace FpsLimitSync
