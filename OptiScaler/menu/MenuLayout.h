#pragma once

#include <algorithm>
#include <optional>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

namespace MenuLayout
{
// Computes local cursor X for a right-aligned footer item without collapsing below minGap.
inline float RightAlignedX(float cursorLocalX, float availWidth, float itemWidth, float minGap = 8.0f)
{
    return cursorLocalX + (std::max)(minGap, availWidth - itemWidth);
}

// Restores parent CursorMaxPos/IdealMaxPos after drawing a right-aligned widget so AlwaysAutoResize can shrink.
class ScopedRightAlignExtent
{
  public:
    ScopedRightAlignExtent(float cursorLocalX, float itemWidth, float minGap = 8.0f)
        : _window(ImGui::GetCurrentWindowRead())
    {
        if (_window)
        {
            _savedCursorMaxX = _window->DC.CursorMaxPos.x;
            _savedIdealMaxX = _window->DC.IdealMaxPos.x;
            _minEndX = _window->Pos.x - _window->Scroll.x + cursorLocalX + minGap + itemWidth;
        }
    }

    ScopedRightAlignExtent(const ScopedRightAlignExtent&) = delete;
    ScopedRightAlignExtent& operator=(const ScopedRightAlignExtent&) = delete;

    ~ScopedRightAlignExtent()
    {
        if (_window)
        {
            _window->DC.CursorMaxPos.x = (std::max)(_savedCursorMaxX, _minEndX);
            _window->DC.IdealMaxPos.x = (std::max)(_savedIdealMaxX, _minEndX);
        }
    }

  private:
    ImGuiWindow* _window = nullptr;
    float _savedCursorMaxX = 0.0f;
    float _savedIdealMaxX = 0.0f;
    float _minEndX = 0.0f;
};

inline float StatusWrapPosX(float cursorLocalX, float menuScale)
{
    return cursorLocalX + 420.0f * (std::max)(menuScale, 0.5f);
}

inline float StatusWrapPosX(float menuScale)
{
    return StatusWrapPosX(ImGui::GetCursorPosX(), menuScale);
}

// Bounds Frame Generation status notices to 420 px * MenuScale inside StretchSame table columns.
class ScopedStatusWrap
{
  public:
    explicit ScopedStatusWrap(float menuScale) { ImGui::PushTextWrapPos(StatusWrapPosX(menuScale)); }

    ScopedStatusWrap(const ScopedStatusWrap&) = delete;
    ScopedStatusWrap& operator=(const ScopedStatusWrap&) = delete;

    ~ScopedStatusWrap() { ImGui::PopTextWrapPos(); }
};

inline bool ShowsDlssgRatioCombo(const std::optional<int>& dlssgMfgMax, bool dlssgInputOrOutput)
{
    return dlssgMfgMax.has_value() && dlssgMfgMax.value() >= 1 && !dlssgInputOrOutput;
}

inline bool ShowsDlssgDmfgControls(bool gameDmfgSupported, bool dlssgInputOrOutput)
{
    return gameDmfgSupported && !dlssgInputOrOutput;
}
} // namespace MenuLayout
