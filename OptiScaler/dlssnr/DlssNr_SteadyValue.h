#pragma once

// Time-windowed GPU ms reading stabilizer for DLSS 5 Neural Rendering overlay display.

#include <chrono>
#include <cstdint>
#include <optional>

namespace DlssNr
{
class SteadyValue
{
  public:
    void Reset()
    {
        _hasValue = false;
        _held = _sum = 0.0;
        _count = 0;
        _windowStart = {};
    }

    double Update(double rawValue, uint32_t windowMs)
    {
        const auto now = std::chrono::steady_clock::now();
        if (windowMs == 0 || !_hasValue)
        {
            _hasValue = true;
            _held = rawValue;
            _sum = 0.0;
            _count = 0;
            _windowStart = now;
            return _held;
        }

        _sum += rawValue;
        ++_count;
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - _windowStart).count();
        if (elapsedMs >= static_cast<long long>(windowMs) && _count > 0)
        {
            _held = _sum / static_cast<double>(_count);
            _sum = 0.0;
            _count = 0;
            _windowStart = now;
        }
        return _held;
    }

    std::optional<double> UpdateOptional(std::optional<double> rawValue, uint32_t windowMs)
    {
        if (!rawValue.has_value())
            return _hasValue ? std::optional<double>(_held) : std::nullopt;
        return Update(rawValue.value(), windowMs);
    }

  private:
    bool _hasValue = false;
    double _held = 0.0;
    double _sum = 0.0;
    uint32_t _count = 0;
    std::chrono::steady_clock::time_point _windowStart {};
};
} // namespace DlssNr
