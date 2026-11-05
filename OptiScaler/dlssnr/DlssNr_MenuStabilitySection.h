#pragma once

// Controles de estabilidad DX12, guiado de bordes del residual y convivencia con NR nativo del juego (v1.0.4-dev9).

#include "DlssNrFeature_Dx12.h"
#include "DlssNr_GameNrLog.h"
#include "DlssNr_GameNrRuntime.h"

#include <Config.h>
#include <imgui/imgui.h>
#include <algorithm>
#include <string>

namespace DlssNr
{
inline void RenderGameNativeNrBanner()
{
    if (!DlssNr::GameNr::IsGameNativeNrRunning())
        return;

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.25f, 1.0f));
    ImGui::TextWrapped("DLSS 5 NR nativo del juego activo — NR propio en pausa automatica");
    ImGui::PopStyleColor();

    const std::string summary = DlssNr::GameNr::LastGameSummary();
    if (!summary.empty())
        ImGui::TextDisabled("Modelo nativo detectado: %s", summary.c_str());
    ImGui::Spacing();
}

inline void RenderDx12StabilityTelemetry(bool vulkan)
{
    if (vulkan)
        return;

    const StabilityTelemetry tel = GetStabilityTelemetry();
    if (tel.mainWidth != 0 && tel.mainHeight != 0)
    {
        ImGui::TextDisabled("Principal: %ux%u | Secundarias omitidas: %u (evals: %llu) | Flips HDR absorbidos: %llu",
                            tel.mainWidth, tel.mainHeight, tel.activeSecondary,
                            (unsigned long long) tel.skippedSecondaryEvaluates,
                            (unsigned long long) tel.absorbedFormatFlips);
    }
    else
    {
        ImGui::TextDisabled("Principal: en espera | Secundarias omitidas: %u | Flips HDR absorbidos: %llu",
                            tel.activeSecondary, (unsigned long long) tel.absorbedFormatFlips);
    }

    if (tel.stoodDownForRebuilds)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                           "Guardia anti-reconstruccion activa: esperando a que el motor estabilice formato/resolucion.");
    }
}

inline void RenderDev9StabilitySection(Config* config, void (*helpMarker)(const char*))
{
    if (!ImGui::TreeNodeEx("Estabilidad, Bordes y Convivencia Nativa (v1.0.4-dev9)", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    bool edgeGuiding = config->DlssNrResidualEdgeGuiding.value_or_default();
    if (ImGui::Checkbox("Guiado de bordes en residual NVIDIA (Anti-sangrado en siluetas)", &edgeGuiding))
    {
        config->DlssNrResidualEdgeGuiding = edgeGuiding;
        config->DlssNrQualityProfile = 7u;
    }
    helpMarker("Pondera bilateralmente los 4 taps del residual reducido frente a la guia de resolucion completa\n"
               "para impedir que las ediciones neuronales crucen siluetas y aristas de alto contraste.");

    ImGui::BeginDisabled(!edgeGuiding);
    float edgeStrengthPct = std::clamp(config->DlssNrResidualEdgeStrength.value_or_default(), 0.0f, 1.0f) * 100.0f;
    if (ImGui::SliderFloat("Fuerza guiado de bordes residual (%)", &edgeStrengthPct, 0.0f, 100.0f, "%.0f%%"))
    {
        config->DlssNrResidualEdgeStrength = std::clamp(edgeStrengthPct / 100.0f, 0.0f, 1.0f);
        config->DlssNrQualityProfile = 7u;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##ResidualEdgeStrength"))
    {
        config->DlssNrResidualEdgeStrength = 1.0f;
        config->DlssNrQualityProfile = 7u;
    }
    ImGui::EndDisabled();
    helpMarker("Intensidad de mezcla entre el residual bilineal clasico (0%%) y el guiado de siluetas (100%%).");

    bool unlockGameNr = config->DlssNrUnlockGameNR.value_or_default();
    if (ImGui::Checkbox("Permitir y priorizar DLSS 5 NR nativo del juego (NBA 2K27 / Metro Redux)", &unlockGameNr))
        config->DlssNrUnlockGameNR = unlockGameNr;
    helpMarker("Permite que juegos con integracion oficial NGX Feature 18 ejecuten su propio DLSS 5 NR,\n"
               "pausando automaticamente el NR de HispaOptiScaler mientras el nativo este activo.");

    bool preloadNr = config->DlssNrPreload.value_or_default();
    if (ImGui::Checkbox("Precargar motor DLSS-NR al iniciar el juego (Evita tiron inicial)", &preloadNr))
        config->DlssNrPreload = preloadNr;
    helpMarker("Inicializa nvngx_dlssnr.dll durante el arranque NGX D3D12 para eliminar el tiron del primer fotograma.");

    bool keepFlip = config->DlssNrKeepModelOnFormatFlip.value_or_default();
    if (ImGui::Checkbox("Conservar modelo IA en cambios HDR RGBA16F / R11G11B10F (Sin tirones)", &keepFlip))
        config->DlssNrKeepModelOnFormatFlip = keepFlip;
    helpMarker("Mantiene las superficies internas del modelo en RGBA16F cuando el motor alterna entre RGBA16F\n"
               "y R11G11B10F (pantallas de carga / UI), evitando reconstrucciones del modelo por fotograma.");

    bool isolateMain = config->DlssNrIsolateMainFeature.value_or_default();
    if (ImGui::Checkbox("Aislar NR solo en camara principal (Ignorar mirillas PiP / espejos / UI 3D)", &isolateMain))
        config->DlssNrIsolateMainFeature = isolateMain;
    helpMarker("Ejecuta DLSS-NR unicamente sobre la camara principal de mayor resolucion y omite instancias\n"
               "DLSS secundarias (mirillas Picture-in-Picture, espejos o retratos 3D en menus).");

    bool gpuSafeRetire = config->DlssNrGpuSafeRetire.value_or_default();
    if (ImGui::Checkbox("Liberacion diferida GPU-Safe con Fence (Anti-cuelgues RE4 / UE5)", &gpuSafeRetire))
        config->DlssNrGpuSafeRetire = gpuSafeRetire;
    helpMarker("Retiene los recursos y features NR retirados hasta que ID3D12Fence confirme que la GPU\n"
               "ha completado todas las listas de comandos en vuelo (imprescindible con Frame Generation).");

    ImGui::TreePop();
}
} // namespace DlssNr
