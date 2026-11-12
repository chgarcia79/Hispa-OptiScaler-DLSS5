#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include <Config.h>
#include <dlssnr/DlssNrFeature_Dx12.h>
#include <imgui/imgui.h>

namespace DlssNr
{
// Live controls for Native Blend, Depth Fade distance masking, Z-polarity override, and live mask preview.
inline void RenderDev10DepthFadeSection(Config* config, void (*helpMarker)(const char*))
{
    if (!config)
        return;

    if (!ImGui::TreeNodeEx("Mezcla Nativa y Atenuacion por Profundidad (Depth Fade v1.0.4-dev10)",
                           ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::PushID("Dev10DepthFade");

    bool nativeBlend = config->DlssNrNativeBlend.value_or_default();
    if (ImGui::Checkbox("Modo Mezcla Nativa (Native Blend - Salida pura del modelo NVIDIA)", &nativeBlend))
    {
        config->DlssNrNativeBlend = nativeBlend;
        config->DlssNrQualityProfile = 7u;
    }
    helpMarker("Delega la mezcla directamente en el kernel nativo de NVIDIA cuando la escena lo permite\n"
               "(desactiva ajustes custom de contraste para obtener la salida pura del modelo).");

    bool depthFade = config->DlssNrDepthFadeEnabled.value_or_default();
    if (ImGui::Checkbox("Atenuacion por Distancia (Depth Fade - Suavizar fondo y niebla)", &depthFade))
    {
        config->DlssNrDepthFadeEnabled = depthFade;
        config->DlssNrQualityProfile = 7u;
    }
    helpMarker("Modula la intensidad del detalle neural entre primer plano (Near), fondo lejano (Far)\n"
               "y cielo/horizonte (Sky), evitando sobre-enfocar niebla volumetrica o cielo.");

    static uint32_t s_prevDebugView = 0u;
    const uint32_t currentDebug = config->DlssNrDebugView.value_or_default();
    if (currentDebug != 4u)
        s_prevDebugView = currentDebug;

    bool showMask = (currentDebug == 4u);
    if (ImGui::Checkbox("Ver Mascara Depth Fade en Vivo (Blanco=100%, Negro=0%)", &showMask))
        config->DlssNrDebugView = showMask ? 4u : ((s_prevDebugView == 4u) ? 0u : s_prevDebugView);
    helpMarker("Previsualiza en tiempo real el peso calculado de Depth Fade sobre la imagen\n"
               "(Blanco = 100% efecto, Negro = 0%). Al desmarcar restaura la vista normal.");

    ImGui::BeginDisabled(!depthFade);

    const StabilityTelemetry tel = GetStabilityTelemetry();
    const std::string autoPolarLabel = std::string("Auto segun flag NGX del juego (") +
                                       (tel.gameDepthInverted ? "Reverse-Z" : "Normal-Z") + ")";
    const char* revZModes[] = {
        autoPolarLabel.c_str(),
        "Forzar Reverse-Z [UE5/DX12]",
        "Forzar Normal-Z"
    };
    int revZMode = std::clamp((int) config->DlssNrDepthFadeReverseZMode.value_or_default(), 0, 2);
    if (ImGui::Combo("Polaridad Z de Profundidad", &revZMode, revZModes, IM_ARRAYSIZE(revZModes)))
    {
        config->DlssNrDepthFadeReverseZMode = (uint32_t) std::clamp(revZMode, 0, 2);
        config->DlssNrQualityProfile = 7u;
    }
    helpMarker("Controla si Depth = 0 es el fondo infinito (Reverse-Z) o el primer plano (Normal-Z).\n"
               "Auto lee en vivo el indicador DepthInverted enviado por el juego via NGX.");

    auto percentSlider = [&](const char* label, const char* idSuffix, auto& field, float defaultVal, const char* tip)
    {
        float pct = field.value_or_default() * 100.0f;
        if (ImGui::SliderFloat(label, &pct, 0.0f, 100.0f, "%.0f%%"))
        {
            field = std::clamp(pct / 100.0f, 0.0f, 1.0f);
            config->DlssNrQualityProfile = 7u;
        }
        ImGui::SameLine();
        ImGui::PushID(idSuffix);
        if (ImGui::SmallButton("Reset"))
        {
            field = defaultVal;
            config->DlssNrQualityProfile = 7u;
        }
        ImGui::PopID();
        helpMarker(tip);
    };

    auto depthSlider = [&](const char* label, const char* idSuffix, auto& field, float defaultVal, const char* tip)
    {
        float depthVal = std::clamp(field.value_or_default(), 0.0001f, 1.0f);
        if (ImGui::SliderFloat(label, &depthVal, 0.0001f, 1.0f, "%.4f", ImGuiSliderFlags_Logarithmic))
        {
            field = std::clamp(depthVal, 0.0001f, 1.0f);
            config->DlssNrQualityProfile = 7u;
        }
        ImGui::SameLine();
        ImGui::PushID(idSuffix);
        if (ImGui::SmallButton("Reset"))
        {
            field = defaultVal;
            config->DlssNrQualityProfile = 7u;
        }
        ImGui::PopID();
        helpMarker(tip);
    };

    percentSlider("Fuerza Cerca / Primer Plano (%)", "near_str", config->DlssNrDepthFadeNearStrength, 1.0f,
                  "Peso neural aplicado en objetos cercanos a camara (por defecto 100%).");
    percentSlider("Fuerza Lejos / Fondo (%)", "far_str", config->DlssNrDepthFadeFarStrength, 0.25f,
                  "Peso neural aplicado tras alcanzar la profundidad lejana (por defecto 25%).");
    percentSlider("Fuerza Cielo / Horizonte (%)", "sky_str", config->DlssNrDepthFadeSkyStrength, 0.0f,
                  "Peso aplicado en plano infinito / cielo (por defecto 0% para evitar grano en nubes).");

    depthSlider("Inicio Atenuacion (Near Depth)", "near_z", config->DlssNrDepthFadeNearDepth, 0.02f,
                "Profundidad normalizada donde empieza la transicion desde Fuerza Cerca hacia Lejos.");
    depthSlider("Fin Atenuacion (Far Depth)", "far_z", config->DlssNrDepthFadeFarDepth, 0.35f,
                "Profundidad normalizada donde la atenuacion alcanza por completo Fuerza Lejos.");

    ImGui::EndDisabled();

    ImGui::PopID();
    ImGui::TreePop();
}
} // namespace DlssNr
