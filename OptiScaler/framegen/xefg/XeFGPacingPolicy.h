#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "Config.h"

namespace XeFGPacingPolicy
{
inline constexpr int64_t kMinPeriodNs = 1'000'000LL;
inline constexpr int64_t kMaxPeriodNs = 500'000'000LL;

inline bool IsFpsCapPacingActive()
{
    const auto* cfg = Config::Instance();
    return cfg->XeFGPaceWithFpsCap.value_or_default() &&
           cfg->FramerateLimit.value_or_default() > 0.0f;
}

inline uint64_t EffectiveFrameMultiplier(uint64_t countPlus1)
{
    const uint64_t safeMult = (std::max<uint64_t>)(countPlus1, 1ULL);
    if (!Config::Instance()->FpsLimitUseFgMultiplier.value_or_default())
        return 1ULL;
    return safeMult;
}

inline int64_t CappedBurstPeriodNs(uint64_t countPlus1)
{
    if (!IsFpsCapPacingActive())
        return 0;

    const double fpsCap = static_cast<double>(Config::Instance()->FramerateLimit.value_or_default());
    if (!std::isfinite(fpsCap) || fpsCap <= 0.0)
        return 0;

    const double mult = static_cast<double>(EffectiveFrameMultiplier(countPlus1));
    const double ns = (1'000'000'000.0 * mult) / fpsCap;
    if (!std::isfinite(ns) || ns <= 0.0)
        return 0;

    return std::clamp(static_cast<int64_t>(std::llround(ns)), kMinPeriodNs, kMaxPeriodNs);
}

inline int64_t ResolveBurstPeriodNs(int64_t measuredPeriodNs, uint64_t countPlus1)
{
    const int64_t capPeriodNs = CappedBurstPeriodNs(countPlus1);
    if (capPeriodNs <= 0)
        return measuredPeriodNs;
    if (measuredPeriodNs <= 0)
        return capPeriodNs;

    // When an FPS cap owns frame cadence, never pace bursts faster than the target interval.
    return (std::max)(measuredPeriodNs, capPeriodNs);
}

inline int64_t ComputeTargetStepNs(int64_t refNs, uint32_t countPlus1)
{
    const uint32_t safeCount = (std::max)(countPlus1, 1U);
    const int64_t effectiveRef = ResolveBurstPeriodNs(refNs, safeCount);
    if (effectiveRef <= 0)
        return 0;

    return std::clamp(effectiveRef / static_cast<int64_t>(safeCount), 1'000'000LL, 20'000'000LL);
}

inline double ResolveRenderTimeMs(double pacingRenderTimeMs, double fallbackDeltaMs, uint32_t countPlus1)
{
    double resolvedMs = (pacingRenderTimeMs > 0.0) ? pacingRenderTimeMs : fallbackDeltaMs;
    const int64_t capNs = CappedBurstPeriodNs(countPlus1);
    if (capNs > 0)
    {
        const double capMs = static_cast<double>(capNs) / 1'000'000.0;
        resolvedMs = (resolvedMs > 0.0) ? (std::max)(resolvedMs, capMs) : capMs;
    }
    return resolvedMs;
}
} // namespace XeFGPacingPolicy
