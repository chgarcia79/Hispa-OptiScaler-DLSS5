#pragma once

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

// Collapsing header wrapper with ImGui "Menu Width Ratchet" prevention (used-vs-ideal width split).
class ScopedCollapsingHeader
{
  public:
    explicit ScopedCollapsingHeader(const char* label, ImGuiTreeNodeFlags flags = 0)
    {
        _parentWindow = ImGui::GetCurrentWindow();
        if (_parentWindow)
        {
            _parentCursorMaxX = _parentWindow->DC.CursorMaxPos.x;
            _parentIdealMaxX = _parentWindow->DC.IdealMaxPos.x;
            _childStartX = _parentWindow->DC.CursorPos.x;
        }

        ImGui::PushID(label);
        ImGui::BeginChild("##CollapsingHeaderChild", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        _headerOpen = ImGui::CollapsingHeader(label, flags);
        _active = true;
    }

    ScopedCollapsingHeader(const ScopedCollapsingHeader&) = delete;
    ScopedCollapsingHeader& operator=(const ScopedCollapsingHeader&) = delete;

    bool IsHeaderOpen() const { return _headerOpen; }

    ~ScopedCollapsingHeader()
    {
        if (!_active)
            return;

        float usedWidth = 0.0f;
        float idealWidth = 0.0f;
        if (ImGuiWindow* childWindow = ImGui::GetCurrentWindowRead())
        {
            usedWidth = ImMax(0.0f, childWindow->DC.CursorMaxPos.x - childWindow->DC.CursorStartPos.x);
            idealWidth = ImMax(usedWidth, childWindow->DC.IdealMaxPos.x - childWindow->DC.CursorStartPos.x);
        }

        ImGui::EndChild();

        // Clamp parent width tracking back to real child contents instead of full child avail width.
        if (ImGuiWindow* parentWindow = ImGui::GetCurrentWindowRead();
            parentWindow != nullptr && parentWindow == _parentWindow)
        {
            parentWindow->DC.CursorMaxPos.x = ImMax(_parentCursorMaxX, _childStartX + usedWidth);
            parentWindow->DC.IdealMaxPos.x = ImMax(_parentIdealMaxX, _childStartX + idealWidth);
        }

        ImGui::PopID();
    }

  private:
    ImGuiWindow* _parentWindow = nullptr;
    float _parentCursorMaxX = 0.0f;
    float _parentIdealMaxX = 0.0f;
    float _childStartX = 0.0f;
    bool _active = false;
    bool _headerOpen = false;
};
