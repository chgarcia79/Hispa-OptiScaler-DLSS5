#pragma once

#include <Config.h>
#include <dlssnr/DlssNr_Calibrate.h>
#include <dlssnr/DlssNr_Capture.h>

namespace DlssNr::CalibrateBridge
{
inline void Tick(capture::FrameCapture& frames)
{
    auto* cfg = Config::Instance();
    if (cfg == nullptr)
        return;
    if (!cfg->DlssNrCalibratorEnabled.value_or_default())
    {
        if (DlssNr::Calibrate::State().active)
            DlssNr::Calibrate::Cancel();
    }
    DlssNr::Calibrate::Tick(*cfg, frames);
}

inline void Composed()
{
    auto* cfg = Config::Instance();
    if (cfg != nullptr && cfg->DlssNrCalibratorEnabled.value_or_default())
        DlssNr::Calibrate::Composed();
}
} // namespace DlssNr::CalibrateBridge
