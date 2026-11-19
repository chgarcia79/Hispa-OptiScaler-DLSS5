#pragma once

#include <Config.h>
#include "NrNvfp4.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace DlssNr::Profiles
{
// The model a pass asks for: the runtime's presets 0..3, or one of DlssNr::Nvfp4's (4..6: Model A with the 4-bit
// kernels, or a pass-in-one model's weight set).
inline unsigned int RequestedPreset(const Config& cfg, unsigned int pass)
{
    unsigned int preset = cfg.DlssNrPreset.value_or_default();
    if (pass > 0 && pass <= 2 && pass <= std::size(cfg.DlssNrPassOverrides))
        preset = cfg.DlssNrPassOverrides[pass - 1].preset.value_or(preset);
    return preset;
}

// Whether any of the passes asks for one of DlssNr::Nvfp4's models.
inline bool WantsNvfp4(const Config& cfg, unsigned int passes)
{
    for (unsigned int pass = 0; pass < passes; ++pass)
        if (Nvfp4::IsNvfp4(RequestedPreset(cfg, pass)))
            return true;
    return false;
}

// A re-applying pass skips the network and repeats the previous network pass's change; pass 1 always runs it.
inline bool PassReapplies(const Config& cfg, unsigned int pass)
{
    return pass > 0 && pass <= std::size(cfg.DlssNrPassOverrides) &&
           cfg.DlssNrPassOverrides[pass - 1].reapply.value_or(false);
}

inline float ReapplyGain(const Config& cfg)
{
    const float gain = cfg.DlssNrReapplyGain.value_or_default();
    return std::isfinite(gain) ? std::clamp(gain, 0.0f, 2.0f) : 0.81f;
}

// Consecutive re-applying passes from `pass` (inclusive) up to `end` (exclusive).
inline unsigned int ReapplyRun(const Config& cfg, unsigned int pass, unsigned int end)
{
    unsigned int run = 0;
    while (pass + run < end && PassReapplies(cfg, pass + run))
        ++run;
    return run;
}

// The borrow tick-boxes of a network pass. NrPassBorrow::ChooseCut turns them into a cut; re-apply wins over both.
struct BorrowChoice
{
    bool transformer = false, below = false;
    unsigned int level = 32;
};

inline BorrowChoice PassBorrow(const Config& cfg, unsigned int pass)
{
    if (pass == 0 || pass > std::size(cfg.DlssNrPassOverrides) || PassReapplies(cfg, pass))
        return {};
    const auto& extra = cfg.DlssNrPassOverrides[pass - 1];
    return { extra.borrowTransformer.value_or(false), extra.borrowBelow.value_or(false),
             extra.borrowLevel.value_or(32u) };
}
} // namespace DlssNr::Profiles
