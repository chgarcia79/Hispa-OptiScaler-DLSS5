#pragma once

// Native Blend mode for DLSS 5 Neural Rendering: direct linear lerp(original, modelDecoded, effectiveStrength)
// with in-memory snapshot/restore of conflicting composition controls when toggled live in the GUI.

#include <Config.h>
#include <shaders/dlssnr/DlssNr_Common.h>
#include <mutex>

namespace DlssNr::NativeBlend
{
struct CompositionSnapshot
{
    bool saved = false;
    float colourStrength = 1.0f;
    float maxRatio = 2.0f;
    float exposureBalance = 1.0f;
    bool antiStarvation = false;
    float haloSuppression = 0.0f;
    float haloBrightSuppression = 0.0f;
};

inline CompositionSnapshot& SnapshotState()
{
    static CompositionSnapshot s_snapshot;
    return s_snapshot;
}

inline std::mutex& SnapshotMutex()
{
    static std::mutex s_mutex;
    return s_mutex;
}

inline bool HasSnapshot()
{
    std::lock_guard<std::mutex> lock(SnapshotMutex());
    return SnapshotState().saved;
}

inline void ApplyToggle(Config* cfg, bool enable)
{
    if (cfg == nullptr)
        return;

    std::lock_guard<std::mutex> lock(SnapshotMutex());
    auto& snap = SnapshotState();

    if (enable)
    {
        if (!snap.saved)
        {
            snap.saved = true;
            snap.colourStrength = cfg->DlssNrColourStrength.value_or_default();
            snap.maxRatio = cfg->DlssNrMaxRatio.value_or_default();
            snap.exposureBalance = cfg->DlssNrExposureBalance.value_or_default();
            snap.antiStarvation = cfg->DlssNrAntiStarvation.value_or_default();
            snap.haloSuppression = cfg->DlssNrHaloSuppression.value_or_default();
            snap.haloBrightSuppression = cfg->DlssNrHaloBrightSuppression.value_or_default();
        }
        cfg->DlssNrNativeBlend = true;
        cfg->DlssNrColourStrength = 1.0f;
        cfg->DlssNrExposureBalance = 1.0f;
        cfg->DlssNrAntiStarvation = false;
        cfg->DlssNrHaloSuppression = 0.0f;
        cfg->DlssNrHaloBrightSuppression = 0.0f;
    }
    else
    {
        cfg->DlssNrNativeBlend = false;
        if (snap.saved)
        {
            cfg->DlssNrColourStrength = snap.colourStrength;
            cfg->DlssNrMaxRatio = snap.maxRatio;
            cfg->DlssNrExposureBalance = snap.exposureBalance;
            cfg->DlssNrAntiStarvation = snap.antiStarvation;
            cfg->DlssNrHaloSuppression = snap.haloSuppression;
            cfg->DlssNrHaloBrightSuppression = snap.haloBrightSuppression;
            snap.saved = false;
        }
    }
}

inline void PopulateResolveConstants(const Config& cfg, DlssNrConstants& out)
{
    out.NativeBlend = cfg.DlssNrNativeBlend.value_or_default() ? 1u : 0u;
}
} // namespace DlssNr::NativeBlend
