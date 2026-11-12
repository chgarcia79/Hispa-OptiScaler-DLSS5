#pragma once

#include <algorithm>
#include <cstdint>

#include <Config.h>
#include <framegen/xefg/XeFGPacing.h>
#include <hooks/FG_Hooks.h>
#include <hooks/FpsLimitSync.h>
#include <hooks/Reflex_Hooks.h>

#include <imgui/imgui.h>

namespace MenuDev10LivePacing
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

// Renders live FrameGen pacing, secondary swapchain filtering, FPS sync, and Reflex latency controls.
inline void RenderSection(::Config* config, float menuResScale)
{
    if (!config)
        return;

    ImGui::PushID("Dev10LivePacing");
    ImGui::Spacing();
    ImGui::SeparatorText("Pacing, SwapChains y Latencia (Live v1.0.4-dev10)");

    bool skipForeign = config->FGSkipForeignSwapChain.value_or_default();
    if (ImGui::Checkbox("Ignorar SwapChains Secundarias en FGPresent", &skipForeign))
        config->FGSkipForeignSwapChain = skipForeign;
    HelpTip("Evita que ventanas secundarias, overlays, webviews o reproductores de video\n"
            "activen Frame Generation o disparen cambios de pacing.");

    const uint64_t skippedForeign = FGHooks::SkippedForeignPresentCount();
    if (skippedForeign > 0)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.55f, 1.0f), "(%llu omitidas)",
                           static_cast<unsigned long long>(skippedForeign));
    }
    else
    {
        ImGui::TextDisabled("  SwapChains secundarias omitidas: 0");
    }

    bool xePaceCap = config->XeFGPaceWithFpsCap.value_or_default();
    if (ImGui::Checkbox("XeMFG: Medir Render-Time en 2X y Pacing por FPS Cap", &xePaceCap))
    {
        config->XeFGPaceWithFpsCap = xePaceCap;
        FpsLimitSync::NotifyConfigChanged();
    }
    HelpTip("Estabiliza XeMFG 3X/4X tomando la cadencia objetivo desde el cap de FPS activo\n"
            "y midiendo tiempo puro de renderizado en 2X.");

    const double xeRenderMs = XeFGPacing::RenderTimeMs();
    if (xeRenderMs > 0.0)
    {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.0f, 1.0f), "  XeMFG Render-Time medido: %.2f ms", xeRenderMs);
    }
    else
    {
        ImGui::TextDisabled("  XeMFG Render-Time medido: inactivo");
    }

    bool singleReflexOwner = config->FpsLimitSingleReflexOwner.value_or_default();
    if (ImGui::Checkbox("Sincronizar Limite FPS en Dueno Unico Reflex / XeLL", &singleReflexOwner))
    {
        config->FpsLimitSingleReflexOwner = singleReflexOwner;
        FpsLimitSync::NotifyConfigChanged();
    }
    HelpTip("Evita la doble limitacion cuando Reflex y XeLL coinciden al mismo tiempo,\n"
            "asignando el limite unicamente a la ruta baja en latencia activa.");

    bool useFgMult = config->FpsLimitUseFgMultiplier.value_or_default();
    if (ImGui::Checkbox("Aplicar Multiplicador FG Real en Limitador Software", &useFgMult))
    {
        config->FpsLimitUseFgMultiplier = useFgMult;
        FpsLimitSync::NotifyConfigChanged();
    }
    HelpTip("Mantiene el techo efectivo en FPS finales de pantalla dividiendo la espera\n"
            "por el factor real de fotogramas generados (2X..6X).");

    bool showLatency = config->ShowReflexInputLatency.value_or_default();
    if (ImGui::Checkbox("Mostrar Input Latency (ms) en Tiempos Reflex", &showLatency))
        config->ShowReflexInputLatency = showLatency;
    HelpTip("Muestra en la seccion Reflex la latencia entrada->presentacion promediada en ms.");

    ImGui::BeginDisabled(!showLatency);
    ImGui::PushItemWidth(160.0f * (std::max)(menuResScale, 0.5f));
    int avgSamples = std::clamp(config->ReflexLatencyAvgSamples.value_or_default(), 1, 32);
    if (ImGui::SliderInt("Muestras Promedio Input Latency", &avgSamples, 1, 32))
        config->ReflexLatencyAvgSamples = std::clamp(avgSamples, 1, 32);
    ImGui::PopItemWidth();
    HelpTip("Numero de fotogramas de la ventana deslizante para promediar Input Latency (1..32).");

    if (const auto latencyMs = ReflexHooks::inputLatencyMs())
    {
        ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.55f, 1.0f), "  Input Latency actual: %.1f ms", *latencyMs);
    }
    else
    {
        ImGui::TextDisabled("  Input Latency actual: esperando marcadores Reflex");
    }
    ImGui::EndDisabled();

    ImGui::PopID();
}
} // namespace MenuDev10LivePacing
