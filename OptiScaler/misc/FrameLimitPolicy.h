#pragma once

#include <algorithm>
#include <cstdint>
#include "Config.h"
#include "State.h"
#include "hooks/FpsLimitSync.h"

namespace FrameLimitPolicy
{
inline bool IsExternalFgActive(uint8_t asyncFgFramesToGenerate)
{
    const auto& state = State::Instance();
    if (state.activeFgOutput != FGOutput::NoFG)
        return false;

    if (state.fsrfgInputActive || state.dlssgDetectedInterpolationCount > 0 || asyncFgFramesToGenerate > 0)
        return true;

    return state.activeFgInput != FGInput::NoFG &&
           state.activeFgInput != FGInput::Upscaler &&
           state.dlssgLastSetMode != sl::DLSSGMode::eOff;
}

inline bool ShouldSkipSoftwareInSwapchain(bool usesDxvk, bool xellCanLimit, uint8_t asyncFgFramesToGenerate)
{
    const auto& state = State::Instance();
    return state.reflexLimitsFps ||
           state.activeFgOutput != FGOutput::NoFG ||
           usesDxvk ||
           xellCanLimit ||
           IsExternalFgActive(asyncFgFramesToGenerate);
}

inline bool ShouldSleepInFGPresent(bool willPresent, bool usesDxvk, bool xellCanLimit)
{
    const auto& state = State::Instance();
    return willPresent &&
           !state.reflexLimitsFps &&
           state.activeFgOutput != FGOutput::NoFG &&
           !usesDxvk &&
           !xellCanLimit;
}

inline uint32_t ResolveSoftwareMultiplier(bool fgActive, uint32_t optiInterpolatedCount,
                                          uint8_t asyncFgFramesToGenerate, uint32_t detectedDlssgCount,
                                          bool fsrfgInputActive)
{
    if (!fgActive || !FpsLimitSync::UseFgMultiplier())
        return 1u;

    return FpsLimitSync::ResolveFgMultiplier(true, (std::max)(optiInterpolatedCount, 1u),
                                             asyncFgFramesToGenerate, detectedDlssgCount, fsrfgInputActive);
}
} // namespace FrameLimitPolicy
