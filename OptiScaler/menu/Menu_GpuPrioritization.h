#pragma once

// Seccion independiente ImGui para NVIDIA GPU App Prioritization (NvAPI_SYS_CmcYieldPctControl 0xC742BB22, struct 0x0001001C).
// Opera 100% fuera de OptiScaler.ini y Save INI; sincroniza bidireccionalmente en vivo con el driver y HispaGameControl.

#include <nvapi/NvApiGpuPrioritization.h>
#include <menu/menu_common.h>
#include <imgui/imgui.h>
#include <algorithm>

namespace MenuGpuPrioritization
{
inline void HelpTip(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

inline void RenderSection(float menuResScale)
{
    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("GPU App Prioritization (RTX 50 - Driver Live)"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        static uint64_t s_syncedSeq = 0;
        static NvApiGpuPrioritization::Mode s_mode = NvApiGpuPrioritization::Mode::Default;
        static int s_customFgPct = 80;

        NvApiGpuPrioritization::LiveState state = NvApiGpuPrioritization::GetCachedState();
        if (state.refreshSeq != s_syncedSeq)
        {
            s_syncedSeq = state.refreshSeq;
            s_mode = state.activeMode;
            s_customFgPct = state.currentYieldPct == 0u ? 80 : (int) std::clamp(state.foregroundPct, 50u, 95u);
        }

        if (state.supported)
            ImGui::TextColored(ImVec4(0.35f, 0.92f, 0.55f, 1.0f), "%s", state.statusMessage.c_str());
        else
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.32f, 1.0f), "%s", state.statusMessage.c_str());

        ImGui::SameLine();
        if (ImGui::SmallButton("Refrescar##GpuPrio"))
        {
            state = NvApiGpuPrioritization::RefreshLiveState();
            s_syncedSeq = state.refreshSeq;
            s_mode = state.activeMode;
            s_customFgPct = state.currentYieldPct == 0u ? 80 : (int) std::clamp(state.foregroundPct, 50u, 95u);
        }
        HelpTip("Consulta en vivo NvAPI_SYS_CmcYieldPctControl (0xC742BB22, struct 0x0001001C).\n"
                "Detecta automaticamente el modo configurado en NVIDIA App o en HispaGameControl.\n"
                "Si el juego corre sin Administrador (NVAPI_INVALID_USER_PRIVILEGE -137), eleva puntualmente al aplicar.");

        ImGui::TextDisabled("Ajuste global en caliente del driver NVIDIA (independiente de OptiScaler.ini)");
        ImGui::Spacing();

        ImGui::BeginDisabled(!state.supported);

        auto modeRadio = [&](const char* label, NvApiGpuPrioritization::Mode m, int presetFg)
        {
            if (ImGui::RadioButton(label, s_mode == m))
            {
                s_mode = m;
                if (presetFg > 0)
                    s_customFgPct = presetFg;
            }
        };

        modeRadio("Foreground 90%", NvApiGpuPrioritization::Mode::ForegroundPriority, 90);
        ImGui::SameLine();
        modeRadio("Default 80%", NvApiGpuPrioritization::Mode::Default, 80);
        ImGui::SameLine();
        modeRadio("Background 50%", NvApiGpuPrioritization::Mode::BackgroundPriority, 50);
        ImGui::SameLine();
        modeRadio("Custom", NvApiGpuPrioritization::Mode::Custom, 0);
        ImGui::SameLine();
        modeRadio("Off", NvApiGpuPrioritization::Mode::Off, 0);

        ImGui::BeginDisabled(s_mode == NvApiGpuPrioritization::Mode::Off);
        ImGui::PushItemWidth(220.0f * menuResScale);
        if (ImGui::SliderInt("Prioridad Juego Primer Plano (%)", &s_customFgPct, 50, 95, "%d%%"))
        {
            s_customFgPct = ((std::clamp(s_customFgPct, 50, 95) + 2) / 5) * 5;
            if (s_customFgPct == 90)
                s_mode = NvApiGpuPrioritization::Mode::ForegroundPriority;
            else if (s_customFgPct == 80)
                s_mode = NvApiGpuPrioritization::Mode::Default;
            else if (s_customFgPct == 50)
                s_mode = NvApiGpuPrioritization::Mode::BackgroundPriority;
            else
                s_mode = NvApiGpuPrioritization::Mode::Custom;
        }
        ImGui::PopItemWidth();
        ImGui::EndDisabled();

        const uint32_t targetYield = NvApiGpuPrioritization::ModeToYieldPct(s_mode, (uint32_t) s_customFgPct);
        const bool pendingChange = (targetYield != state.currentYieldPct);
        if (targetYield == 0u)
            ImGui::TextDisabled("Seleccionado: Sin cuota forzada CMC (0%% reserva fondo)");
        else
            ImGui::TextDisabled("Seleccionado: %u%% Juego Primer Plano / %u%% Tareas Segundo Plano", 100u - targetYield, targetYield);

        if (pendingChange)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.56f, 0.34f, 1.0f));
        if (ImGui::Button("Aplicar al Driver (Live)##GpuPrio"))
        {
            state = NvApiGpuPrioritization::ApplyLiveYieldPct(targetYield);
            s_syncedSeq = state.refreshSeq;
            s_mode = state.activeMode;
            s_customFgPct = state.currentYieldPct == 0u ? 80 : (int) std::clamp(state.foregroundPct, 50u, 95u);
        }
        if (pendingChange)
            ImGui::PopStyleColor();

        ImGui::EndDisabled();
        ImGui::Spacing();
    }
}
} // namespace MenuGpuPrioritization
