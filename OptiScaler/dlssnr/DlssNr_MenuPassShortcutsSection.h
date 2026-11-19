#pragma once

#include <Config.h>
#include <dlssnr/NrPassBorrow.h>
#include <menu/ScopedCollapsingHeader.h>
#include <imgui/imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace DlssNr
{
template <typename HelpFn> inline void RenderSpatialBlock(::Config* config, HelpFn helpMarker)
{
    bool sp = config->DlssNrSpatialCompression.value_or_default();
    if (ImGui::Checkbox("Compresion Espacial Periferica (Foveation)", &sp))
        config->DlssNrSpatialCompression = sp;
    helpMarker("Reduce resolucion en bordes conservando el centro a escala completa.");
    if (!sp)
        return;

    auto sliderPct = [&](const char* lbl, CustomOptional<float>& opt, float lo, float hi)
    {
        float v = std::clamp(opt.value_or_default(), lo, hi);
        if (ImGui::SliderFloat(lbl, &v, lo, hi, "%.1f%%"))
            opt = v;
    };
    sliderPct("Centro X (%)", config->DlssNrSpatialCenterX, 10.0f, 100.0f);
    sliderPct("Centro Y (%)", config->DlssNrSpatialCenterY, 10.0f, 100.0f);
    sliderPct("Zona Trabajo X (%)", config->DlssNrSpatialWorkX, 20.0f, 100.0f);
    sliderPct("Zona Trabajo Y (%)", config->DlssNrSpatialWorkY, 20.0f, 100.0f);
    sliderPct("Offset X (%)", config->DlssNrSpatialOffsetX, -50.0f, 50.0f);
    sliderPct("Offset Y (%)", config->DlssNrSpatialOffsetY, -50.0f, 50.0f);
    sliderPct("Shift X (%)", config->DlssNrSpatialShiftX, -50.0f, 50.0f);
    sliderPct("Shift Y (%)", config->DlssNrSpatialShiftY, -50.0f, 50.0f);

    bool showC = config->DlssNrSpatialShowCenter.value_or_default();
    if (ImGui::Checkbox("Mostrar guia Centro", &showC))
        config->DlssNrSpatialShowCenter = showC;
    ImGui::SameLine(0.0f, 12.0f);
    bool showW = config->DlssNrSpatialShowWork.value_or_default();
    if (ImGui::Checkbox("Mostrar guia Trabajo", &showW))
        config->DlssNrSpatialShowWork = showW;
}

template <typename HelpFn> inline void RenderPassShortcutsSection(::Config* config, HelpFn helpMarker)
{
    if (!config)
        return;

    if (auto ch = ScopedCollapsingHeader("Atajos Multi-Pass (Re-apply / Borrow) y Foveation"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();
        RenderSpatialBlock(config, helpMarker);
        ImGui::Spacing();

        const uint32_t totalPasses = std::clamp(config->DlssNrPasses.value_or_default(), 1u, 10u);
        if (totalPasses < 2)
        {
            ImGui::TextDisabled("Activa 2 o mas pasadas para configurar Re-apply y PassBorrow.");
            return;
        }

        float gain = std::clamp(config->DlssNrReapplyGain.value_or_default(), 0.0f, 2.0f);
        if (ImGui::SliderFloat("Ganancia Re-apply (ReapplyGain)", &gain, 0.0f, 2.0f, "%.2f"))
            config->DlssNrReapplyGain = gain;
        helpMarker("Ganancia de cada pasada Re-apply: out = saturate(out_m + r*gain*(out_m - in_m)).");

        char borrowStatus[160] {};
        NrPassBorrow::Status(borrowStatus, sizeof(borrowStatus));
        ImGui::TextDisabled("PassBorrow: %s", borrowStatus);

        for (uint32_t passNum = 2; passNum <= totalPasses; ++passNum)
        {
            auto& ov = config->DlssNrPassOverrides[passNum - 2];
            char nodeLabel[48];
            std::snprintf(nodeLabel, sizeof(nodeLabel), "Pasada %u - Atajos y Perfil", passNum);
            if (!ImGui::TreeNode(nodeLabel))
                continue;

            bool reapply = ov.reapply.value_or(false);
            if (ImGui::Checkbox("Re-aplicar cambio anterior (Compute Re-apply)", &reapply))
                ov.reapply = reapply;

            ImGui::BeginDisabled(reapply);
            bool bt = ov.borrowTransformer.value_or(false);
            if (ImGui::Checkbox("Reutilizar Transformer de Pasada 1", &bt))
                ov.borrowTransformer = bt;

            bool bb = ov.borrowBelow.value_or(false);
            if (ImGui::Checkbox("Reutilizar capas <= 1/N de Pasada 1", &bb))
                ov.borrowBelow = bb;

            static constexpr uint32_t kLevels[] = { 32u, 16u, 8u, 4u, 2u };
            static const char* kLevelNames[] = { "1/32 y menores", "1/16 y menores", "1/8 y menores",
                                                 "1/4 y menores", "1/2 y menores" };
            int lvlIdx = 0;
            const uint32_t curLvl = ov.borrowLevel.value_or(32u);
            for (int k = 0; k < 5; ++k)
                if (kLevels[k] == curLvl)
                    lvlIdx = k;
            if (ImGui::Combo("Nivel corte Borrow", &lvlIdx, kLevelNames, IM_ARRAYSIZE(kLevelNames)))
                ov.borrowLevel = kLevels[lvlIdx];
            ImGui::EndDisabled();

            bool indepScale = ov.workingScale.has_value();
            if (ImGui::Checkbox("Escala de trabajo independiente", &indepScale))
            {
                if (indepScale)
                    ov.workingScale = config->DlssNrWorkingScale.value_or_default();
                else
                    ov.workingScale.reset();
            }
            if (indepScale)
            {
                int pct = (int) std::lround(ov.workingScale.value_or(1.0f) * 100.0f);
                if (ImGui::SliderInt("Working Scale (%)", &pct, 25, 200, "%d%%"))
                    ov.workingScale = static_cast<float>(pct) / 100.0f;
            }

            float pInt = ov.intensity.value_or(config->DlssNrIntensity.value_or_default());
            if (ImGui::SliderFloat("Intensity (Pasada)", &pInt, 0.0f, 2.0f, "%.2f"))
                ov.intensity = pInt;
            float pStr = ov.structure.value_or(config->DlssNrLocalStructure.value_or_default());
            if (ImGui::SliderFloat("Local Structure (Pasada)", &pStr, 0.0f, 2.0f, "%.2f"))
                ov.structure = pStr;
            float pTone = ov.tone.value_or(0.0f);
            if (ImGui::SliderFloat("Local Tone (Pasada)", &pTone, 0.0f, 2.0f, "%.2f"))
                ov.tone = pTone;

            ImGui::TreePop();
        }
    }
}
} // namespace DlssNr
