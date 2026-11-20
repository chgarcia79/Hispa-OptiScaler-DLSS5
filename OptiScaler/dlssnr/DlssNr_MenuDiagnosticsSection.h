#pragma once

#include <Config.h>
#include <dlssnr/DlssNrFeature_Dx12.h>
#include <menu/menu_common.h>
#include <imgui/imgui.h>
#include <algorithm>

namespace DlssNr
{
template <typename HelpFn> inline void RenderDiagnosticsSection(::Config* config, HelpFn helpMarker)
{
    if (!config)
        return;

    if (auto ch = ScopedCollapsingHeader("Guardia CommandList, VRAM y DetailReuse"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        bool guard = config->DlssNrInitDiscardGuard.value_or_default();
        if (ImGui::Checkbox("Proteger CreateFeature ante CommandList descartada", &guard))
            config->DlssNrInitDiscardGuard = guard;
        helpMarker("Evita evaluar NR sobre listas reseteadas/liberadas en cargas de UE5 sin ejecutar en GPU.");

        int maxDiscards = std::clamp(config->DlssNrInitMaxDiscards.value_or_default(), 1, 10);
        if (ImGui::SliderInt("Max. reintentos consecutivos", &maxDiscards, 1, 10))
            config->DlssNrInitMaxDiscards = maxDiscards;

        const uint32_t discards = InitDiscardCount();
        if (IsPausedByInitDiscards())
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                               "  NR pausado tras %u descartes seguidos.", discards);
        else
            ImGui::TextDisabled("  Descartes detectados en sesion: %u", discards);

        if (ImGui::SmallButton("Resetear contador y reintentar NR"))
            ResetInitDiscardCounter();

        ImGui::Separator();

        bool autoFree = config->DlssNrAutoFreeVram.value_or_default();
        if (ImGui::Checkbox("Auto-liberar VRAM al apagar NR", &autoFree))
            config->DlssNrAutoFreeVram = autoFree;
        helpMarker("Libera modelos y superficies NR (~1.4 GB en 1440p) tras el retardo configurado al desactivar NR.");

        int delaySec = std::clamp(config->DlssNrAutoFreeVramDelaySec.value_or_default(), 1, 60);
        if (ImGui::SliderInt("Retardo de liberacion (s)", &delaySec, 1, 60))
            config->DlssNrAutoFreeVramDelaySec = delaySec;

        if (ImGui::SmallButton("Liberar VRAM NR ahora"))
            ReleaseVramNow();
        ImGui::SameLine();
        if (IsVramAllocated())
            ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.35f, 1.0f), "VRAM NR asignada");
        else
            ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.55f, 1.0f), "VRAM NR liberada");

        bool antiTrail = config->DlssNrDetailReuseAntiTrail.value_or_default();
        if (ImGui::Checkbox("Anti-estelas en Reuse Detail cada 3er frame", &antiTrail))
            config->DlssNrDetailReuseAntiTrail = antiTrail;
        helpMarker("Atenua arrastres y refuerza rechazo de movimiento en la segunda reproyeccion consecutiva.");
    }
}
} // namespace DlssNr
