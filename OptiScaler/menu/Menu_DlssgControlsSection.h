#pragma once

#include <Config.h>
#include <State.h>
#include <framegen/dlssg/DlssgPresetPolicy.h>
#include <menu/menu_common.h>
#include <imgui/imgui.h>
#include <algorithm>
#include <string>

namespace MenuDlssgControls
{
inline void HelpTip(const char* desc)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

inline void RenderSection(::Config* config)
{
    if (!config)
        return;

    ImGui::PushID("DlssgControlsSection");
    ImGui::Spacing();
    ImGui::SeparatorText("DLSS-G Preset y Techo MFG (v1.0.4-dev15)");

    static const char* kPresetNames[] = {
        "Auto (Juego / Driver)",
        "Preset A (Modelo Base)",
        "Preset B (Estabilidad)",
        "Preset C (Performance 310.9.16+)"
    };
    static const char* kPresetValues[] = { "auto", "a", "b", "c" };

    const std::string curStr = config->DlssgPreset.value_or("auto");
    const auto parsed = DlssgPresetPolicy::Parse(curStr);
    int idx = 0;
    if (parsed.has_value())
        idx = static_cast<int>(*parsed);

    if (ImGui::Combo("Preset Modelo DLSS-G", &idx, kPresetNames, IM_ARRAYSIZE(kPresetNames)))
    {
        if (idx >= 0 && idx < 4)
            config->DlssgPreset = std::string(kPresetValues[idx]);
    }
    HelpTip("Responde en memoria a la lectura DRS sin modificar el perfil global de NVIDIA.\n"
            "Requiere reiniciar el juego para que DLSS-G tome el nuevo preset.");

    const auto& applied = State::Instance().dlssgAppliedPreset;
    if (!applied.empty())
    {
        ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.55f, 1.0f), "  Preset %s activo en sesion", applied.c_str());
    }

    int maxCeiling = std::clamp(config->DlssgMaxFramesCeiling.value_or_default(), 2, 6);
    if (ImGui::SliderInt("Techo Maximo Multiplicador MFG", &maxCeiling, 2, 6, "%dX"))
        config->DlssgMaxFramesCeiling = maxCeiling;
    HelpTip("Limita el multiplicador maximo MFG ofrecido al juego (2X..6X).");

    ImGui::PopID();
}
} // namespace MenuDlssgControls
