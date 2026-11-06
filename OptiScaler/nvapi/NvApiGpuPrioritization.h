#pragma once

// Control en vivo de NVIDIA GPU App Prioritization (CMC Yield Pct Control, QueryInterface 0xC742BB22).
// Opera exclusivamente en vivo contra nvapi64.dll sin leer ni escribir OptiScaler.ini.

#include <cstdint>
#include <string>

namespace NvApiGpuPrioritization
{
enum class Mode : int
{
    Default = 0,            // yieldPct = 20 (80% Juego FG / 20% Fondo BG)
    ForegroundPriority = 1, // yieldPct = 10 (90% Juego FG / 10% Fondo BG)
    BackgroundPriority = 2, // yieldPct = 50 (50% Juego FG / 50% Fondo BG)
    Custom = 3,             // yieldPct = 5..50 paso 5 (95%..50% Juego FG)
    Off = 4                 // yieldPct = 0 (Sin cuota CMC forzada)
};

struct LiveState
{
    bool driverAvailable = false;
    bool supported = false;
    int lastStatus = 0;
    uint32_t gpuCount = 0;
    uint32_t currentYieldPct = 20;
    uint32_t foregroundPct = 80;
    Mode activeMode = Mode::Default;
    bool elevatedUsedLastApply = false;
    uint64_t refreshSeq = 0;
    std::string statusMessage;
};

constexpr bool IsValidYieldPct(uint32_t yieldPct)
{
    return yieldPct == 0u || (yieldPct >= 5u && yieldPct <= 50u && (yieldPct % 5u) == 0u);
}

constexpr Mode ClassifyYieldPct(uint32_t yieldPct)
{
    if (yieldPct == 0u) return Mode::Off;
    if (yieldPct == 10u) return Mode::ForegroundPriority;
    if (yieldPct == 20u) return Mode::Default;
    if (yieldPct == 50u) return Mode::BackgroundPriority;
    return Mode::Custom;
}

constexpr uint32_t ModeToYieldPct(Mode mode, uint32_t customForegroundPct)
{
    if (mode == Mode::ForegroundPriority) return 10u;
    if (mode == Mode::Default) return 20u;
    if (mode == Mode::BackgroundPriority) return 50u;
    if (mode == Mode::Off) return 0u;
    const uint32_t clamped = customForegroundPct < 50u ? 50u : (customForegroundPct > 95u ? 95u : customForegroundPct);
    return 100u - (((clamped + 2u) / 5u) * 5u);
}

LiveState RefreshLiveState();
LiveState GetCachedState();
LiveState ApplyLiveYieldPct(uint32_t yieldPct);
} // namespace NvApiGpuPrioritization
