#pragma once

// Policy and parameter sanitization for DLSS 5 Neural Rendering Depth Fade (live GUI hot-reload).

#include <Config.h>
#include "DlssNr_Common.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace DlssNr::DepthFade
{
inline float FiniteClamped(float v, float lo, float hi, float fallback)
{
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

inline uint32_t ResolveReverseZFlag(int mode, bool gameDepthInverted)
{
    const int clamped = std::clamp(mode, 0, 2);
    if (clamped == 1)
        return 1u;
    if (clamped == 2)
        return 0u;
    return gameDepthInverted ? 1u : 0u;
}

inline void PopulateResolveConstants(const Config& cfg, bool depthAvailable, bool gameDepthInverted,
                                     uint32_t depthWidth, uint32_t depthHeight, DlssNrConstants& out)
{
    out.DepthFadeEnabled = cfg.DlssNrDepthFadeEnabled.value_or_default() ? 1u : 0u;
    out.HasValidDepth = (depthAvailable && depthWidth > 0 && depthHeight > 0) ? 1u : 0u;
    out.DepthReverseZ = ResolveReverseZFlag(cfg.DlssNrDepthFadeReverseZMode.value_or_default(), gameDepthInverted);

    out.DepthNearStrength = FiniteClamped(cfg.DlssNrDepthFadeNearStrength.value_or_default(), 0.0f, 1.0f, 1.0f);
    out.DepthFarStrength = FiniteClamped(cfg.DlssNrDepthFadeFarStrength.value_or_default(), 0.0f, 1.0f, 0.25f);
    out.DepthSkyStrength = FiniteClamped(cfg.DlssNrDepthFadeSkyStrength.value_or_default(), 0.0f, 1.0f, 0.0f);

    const float nearD = FiniteClamped(cfg.DlssNrDepthFadeNearDepth.value_or_default(), 0.0001f, 0.9999f, 0.02f);
    const float farRaw = FiniteClamped(cfg.DlssNrDepthFadeFarDepth.value_or_default(), 0.0001f, 1.0f, 0.35f);
    out.DepthNear = nearD;
    out.DepthFar = std::max(farRaw, nearD + 1e-4f);

    out.DepthWidth = depthWidth;
    out.DepthHeight = depthHeight;
}

inline void ResetDefaults(Config* cfg)
{
    if (cfg == nullptr)
        return;
    cfg->DlssNrDepthFadeReverseZMode = 0;
    cfg->DlssNrDepthFadeNearStrength = 1.0f;
    cfg->DlssNrDepthFadeFarStrength = 0.25f;
    cfg->DlssNrDepthFadeSkyStrength = 0.0f;
    cfg->DlssNrDepthFadeNearDepth = 0.02f;
    cfg->DlssNrDepthFadeFarDepth = 0.35f;
}
} // namespace DlssNr::DepthFade
