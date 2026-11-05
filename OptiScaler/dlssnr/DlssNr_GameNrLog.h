#pragma once

#include "DlssNr_GameNr.h"

#include <SysUtils.h>
#include <Config.h>
#include <format>
#include <mutex>
#include <optional>
#include <string>

namespace DlssNr::GameNr
{
inline std::mutex& SummaryMutex()
{
    static std::mutex m;
    return m;
}

inline std::string& StoredSummary()
{
    static std::string s;
    return s;
}

inline std::string LastGameSummary()
{
    std::lock_guard<std::mutex> lock(SummaryMutex());
    return StoredSummary();
}

template <class T> inline std::optional<T> ReadParam(const NVSDK_NGX_Parameter* p, const char* key)
{
    T value {};
    return (p && p->Get(key, &value) == NVSDK_NGX_Result_Success) ? std::optional<T>(value) : std::nullopt;
}

inline std::string FormatParam(std::optional<unsigned int> v) { return v ? std::to_string(*v) : "unset"; }
inline std::string FormatParam(std::optional<float> v) { return v ? std::format("{:.2f}", *v) : "unset"; }

inline void LogGameSettingsAndYieldOwnNr(const NVSDK_NGX_Parameter* params)
{
    const auto preset = ReadParam<unsigned int>(params, "DLSSNR.Hint.Render.Preset");
    const auto w = ReadParam<unsigned int>(params, "DLSSNR.Width");
    const auto h = ReadParam<unsigned int>(params, "DLSSNR.Height");
    {
        std::lock_guard<std::mutex> lock(SummaryMutex());
        StoredSummary() = (w && h) ? std::format("Preset {} ({}) | {}x{}", FormatParam(preset), ModelName(preset), *w, *h)
                                   : std::format("Preset {} ({})", FormatParam(preset), ModelName(preset));
    }

    LOG_INFO("DLSS-NR (game): model settings: Preset={} -> {}, Style={}, Intensity={}, LocalStructure={}, LocalTone={}, "
             "SkinStructure={}, AutoMask={}, Size={}x{}",
             FormatParam(preset), ModelName(preset), FormatParam(ReadParam<unsigned int>(params, "DLSSNR.Style")),
             FormatParam(ReadParam<float>(params, "DLSSNR.Intensity")),
             FormatParam(ReadParam<float>(params, "DLSSNR.LocalStructureStrength")),
             FormatParam(ReadParam<float>(params, "DLSSNR.LocalToneStrength")),
             FormatParam(ReadParam<float>(params, "DLSSNR.SkinStructureStrength")),
             FormatParam(ReadParam<unsigned int>(params, "DLSSNR.UseAutoMask")), FormatParam(w), FormatParam(h));

    if (Config::Instance()->DlssNrEnabled.value_or_default())
        LOG_INFO("DLSS-NR (game): native game NR active; turning OptiScaler's NR off for this session (volatile)");
    Config::Instance()->DlssNrEnabled.set_volatile_value(false);
}
} // namespace DlssNr::GameNr
