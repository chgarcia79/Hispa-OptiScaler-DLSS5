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

    if (auto ch = ScopedCollapsingHeader("Guardia CommandList Inicializacion NR (UE5)"); ch.IsHeaderOpen())
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
    }
}
} // namespace DlssNr
