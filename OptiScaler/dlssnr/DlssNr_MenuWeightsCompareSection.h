#pragma once

#include <Config.h>
#include <dlssnr/NrNvfp4.h>
#include <dlssnr/DlssNr_Weights.h>
#include <menu/menu_common.h>
#include <imgui/imgui.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace DlssNr
{
template <typename HelpFn> inline void RenderWeightsCompareSection(::Config* config, HelpFn helpMarker)
{
    if (!config)
        return;

    if (auto ch = ScopedCollapsingHeader("Pesos A/B (nr_weights) y Comparador Interactivo A/B"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        static std::vector<std::string> cachedSets = Weights::ListAvailableSets();
        if (ImGui::SmallButton("Refrescar carpetas nr_weights"))
            cachedSets = Weights::ListAvailableSets();

        auto renderWeightCombo = [&](const char* label, CustomOptional<std::string>* opt)
        {
            std::string cur = opt->value_or_default();
            std::vector<const char*> items;
            items.reserve(cachedSets.size() + 1);
            items.push_back("Integrados en DLL (Defecto)");
            int selected = 0;
            for (size_t i = 0; i < cachedSets.size(); ++i)
            {
                items.push_back(cachedSets[i].c_str());
                if (cachedSets[i] == cur)
                    selected = static_cast<int>(i + 1);
            }
            if (ImGui::Combo(label, &selected, items.data(), static_cast<int>(items.size())))
            {
                if (selected == 0)
                    opt->reset();
                else
                    *opt = cachedSets[static_cast<size_t>(selected - 1)];
            }
        };

        renderWeightCombo("Carpeta Pesos - Modelo A (Original)", &config->DlssNrWeightsSetA);
        helpMarker("Subcarpeta de OptiScaler/nr_weights para WEIGHTS_HT / WEIGHTS_PL.");
        renderWeightCombo("Carpeta Pesos - Modelo B (Optimized)", &config->DlssNrWeightsSetB);
        helpMarker("Subcarpeta de OptiScaler/nr_weights para WEIGHTS_CC_SMALL_FP8.");

        if (Weights::g_needsNewForwarder.load())
        {
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f),
                               "  Aviso: actualiza nvngx.dll_dlssnr.dll para selector dual A/B.");
        }

        ImGui::Separator();

        static const char* kCompareModes[] = {
            "Desactivado",
            "Mitad y Mitad (50/50)",
            "Cortinilla Deslizante (Wipe)"
        };
        int cmpMode = std::clamp(static_cast<int>(config->DlssNrCompare.value_or_default()), 0, 2);
        if (ImGui::Combo("Modo Comparacion", &cmpMode, kCompareModes, IM_ARRAYSIZE(kCompareModes)))
            config->DlssNrCompare = static_cast<uint32_t>(cmpMode);

        if (cmpMode != 0)
        {
            if (cmpMode == 2)
            {
                float pct = std::clamp(config->DlssNrCompareSplit.value_or_default(), 0.0f, 1.0f) * 100.0f;
                if (ImGui::SliderFloat("Posicion Cortinilla (%)", &pct, 0.0f, 100.0f, "%.0f%%"))
                    config->DlssNrCompareSplit = std::clamp(pct / 100.0f, 0.0f, 1.0f);
                if (ImGui::SmallButton("25%"))
                    config->DlssNrCompareSplit = 0.25f;
                ImGui::SameLine();
                if (ImGui::SmallButton("50%"))
                    config->DlssNrCompareSplit = 0.50f;
                ImGui::SameLine();
                if (ImGui::SmallButton("75%"))
                    config->DlssNrCompareSplit = 0.75f;
            }

            static constexpr uint32_t kOtherMap[] = { 0u, 25u, 27u, 26u, 28u, 4u, 21u, 22u };
            static const char* kOtherNames[] = {
                "Imagen Original (Sin NR)",
                "INT8 Original",
                "INT8 Original - Alternate training",
                "INT8 2xPass-in-one",
                "INT8 2xPass-in-one - Alternate training",
                "NVFP4 Original",
                "NVFP4 2xPass-in-one",
                "NVFP4 3xPass-in-one"
            };
            const uint32_t curOther = config->DlssNrCompareOtherModel.value_or_default();
            int otherIdx = 0;
            for (int i = 0; i < IM_ARRAYSIZE(kOtherMap); ++i)
            {
                if (kOtherMap[i] == curOther)
                {
                    otherIdx = i;
                    break;
                }
            }
            if (ImGui::Combo("Comparar contra (Otro lado)", &otherIdx, kOtherNames, IM_ARRAYSIZE(kOtherNames)))
                config->DlssNrCompareOtherModel = kOtherMap[otherIdx];
            helpMarker("Enfrenta dos modelos NR distintos en pantalla en tiempo real (DX12).");

            if (const auto note = Nvfp4::ComparisonNote(); !note.empty())
                ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f), "  %s", note.c_str());

            bool showTags = config->DlssNrCompareTags.value_or_default();
            if (ImGui::Checkbox("Mostrar etiquetas de modelo en pantalla", &showTags))
                config->DlssNrCompareTags = showTags;

            bool swapSides = config->DlssNrCompareSwap.value_or_default();
            if (ImGui::Checkbox("Intercambiar Izquierda / Derecha", &swapSides))
                config->DlssNrCompareSwap = swapSides;

            float tagScale = std::clamp(config->DlssNrTagScale.value_or_default(), 0.5f, 3.0f);
            if (ImGui::SliderFloat("Tamano de Etiquetas", &tagScale, 0.5f, 3.0f, "%.2fx"))
                config->DlssNrTagScale = tagScale;
        }
    }
}
} // namespace DlssNr
