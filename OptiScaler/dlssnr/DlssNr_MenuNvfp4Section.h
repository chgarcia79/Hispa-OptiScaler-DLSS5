#pragma once

#include <Config.h>
#include <dlssnr/NrNvfp4.h>
#include <dlssnr/DlssNr_Weights.h>
#include <menu/menu_common.h>
#include <imgui/imgui.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace DlssNr
{
inline int PresetToComboIndex(uint32_t preset)
{
    switch (preset)
    {
    case 1: return 1;
    case 2: return 2;
    case 4: return 3;
    case 16: return 4;
    case 15: return 5;
    case 17: return 6;
    case 18: return 7;
    case 19: return 8;
    case 20: return 9;
    default: return 0;
    }
}

inline uint32_t ComboIndexToPreset(int idx)
{
    static constexpr uint32_t kMap[] = { 0u, 1u, 2u, 4u, 16u, 15u, 17u, 18u, 19u, 20u };
    return (idx >= 0 && idx < 10) ? kMap[idx] : 0u;
}

template <typename HelpFn> inline void RenderNvfp4PresetSelector(::Config* config, HelpFn helpMarker)
{
    bool fp8Enabled = config->DlssNrFp8Enabled.value_or_default();
    if (ImGui::Checkbox("Habilitar modelos FP8 (RTX 40/50 DX12)", &fp8Enabled))
        config->DlssNrFp8Enabled = fp8Enabled;
    helpMarker("Permite usar los presets FP8 2x/3xPass-in-one y v2 (OptiScaler/nvfp4/fp8).");

    static const char* kNames[] = {
        "Auto (Performance / Small FP8)",
        "Quality (Honest Tench - Maxima Fidelidad)",
        "Performance (Crazy Cuckoo Small FP8, +40% FPS)",
        "Original - NVFP4 (RTX 50 4-bit)",
        "NVFP4 2xPass-in-one (RTX 50 4-bit)",
        "NVFP4 3xPass-in-one (RTX 50 4-bit)",
        "FP8 2xPass-in-one (RTX 40/50)",
        "FP8 3xPass-in-one (RTX 40/50)",
        "FP8 2xPass-in-one v2 (RTX 40/50)",
        "FP8 3xPass-in-one v2 (RTX 40/50)"
    };
    const uint32_t current = config->DlssNrPreset.value_or_default();
    int idx = PresetToComboIndex(current);
    if (ImGui::Combo("Preset del modelo IA", &idx, kNames, IM_ARRAYSIZE(kNames)))
        config->DlssNrPreset = ComboIndexToPreset(idx);

    helpMarker("Modelos 0..2 embebidos en nvngx_dlssnr.dll.\n"
               "NVFP4 (4, 16, 15): Kernels 4-bit para RTX 50 en D3D12 (OptiScaler/nvfp4).\n"
               "FP8 (17..20): Pesos v1/v2 en OptiScaler/nvfp4/fp8 para RTX 40 y RTX 50 en D3D12.\n"
               "Sin fallback silencioso: si faltan pesos o en Vulkan, no se ejecuta Preset 1.");

    const uint32_t activePreset = config->DlssNrPreset.value_or_default();
    if (Nvfp4::IsNvfp4(activePreset))
    {
        if (const auto problem = Nvfp4::Problem(); !problem.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "  Error Modelo: %s", problem.c_str());
        else if (Nvfp4::Running())
            ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.55f, 1.0f), "  %s Activo (%s)",
                               Nvfp4::IsFp8(activePreset) ? "FP8" : "NVFP4 4-bit", Nvfp4::Status().c_str());
        else
            ImGui::TextDisabled("  Estado: %s", Nvfp4::Status().c_str());

        if (Nvfp4::IsFp8(activePreset))
            ImGui::TextDisabled("  Ruta pesos FP8: OptiScaler/nvfp4/fp8");

        if (Nvfp4::IsPassInOne(activePreset) || Nvfp4::IsFp8(activePreset))
        {
            if (ImGui::SmallButton("Aplicar ajuste recomendado 2x/3x (Cinematic, Struct 0.50, Tone 0.80)"))
            {
                config->DlssNrStyle = 2u;
                config->DlssNrLocalStructure = 0.50f;
                config->DlssNrLocalTone = 0.80f;
                config->DlssNrPasses = 1u;
            }
            helpMarker("Aplica Style = Cinematic, Local Structure = 0.50, Local Tone = 0.80 y 1 pasada.");
        }
    }

    const auto sets = Weights::ListAvailableSets();
    std::string curSet = config->DlssNrWeightsSet.value_or_default();
    std::vector<const char*> items;
    items.reserve(sets.size() + 1);
    items.push_back("Integrados en DLL (Defecto)");
    int selected = 0;
    for (size_t i = 0; i < sets.size(); ++i)
    {
        items.push_back(sets[i].c_str());
        if (sets[i] == curSet)
            selected = static_cast<int>(i + 1);
    }
    if (ImGui::Combo("Carpeta de pesos NR (OptiScaler/nr_weights)", &selected, items.data(),
                     static_cast<int>(items.size())))
    {
        if (selected == 0)
            config->DlssNrWeightsSet.reset();
        else
            config->DlssNrWeightsSet = sets[static_cast<size_t>(selected - 1)];
    }
    helpMarker("Selecciona una subcarpeta de OptiScaler/nr_weights/ para sobrescribir pesos al recrear la feature.");
}

inline void RenderNvfp4PassPresetCombo(const char* label, ::Config* config, uint32_t passIndex,
                                       CustomOptional<uint32_t>* legacyOpt)
{
    static const char* kPassNames[] = {
        "Auto (Heredar Pasada 1)",
        "Quality (Honest Tench)",
        "Performance (Small FP8)",
        "Original - NVFP4 (4-bit)",
        "NVFP4 2xPass-in-one",
        "NVFP4 3xPass-in-one",
        "FP8 2xPass-in-one",
        "FP8 3xPass-in-one",
        "FP8 2xPass-in-one v2",
        "FP8 3xPass-in-one v2"
    };
    const uint32_t cur = (passIndex < std::size(config->DlssNrPassOverrides) &&
                          config->DlssNrPassOverrides[passIndex].preset.has_value())
                             ? config->DlssNrPassOverrides[passIndex].preset.value()
                             : legacyOpt->value_or_default();
    int idx = PresetToComboIndex(cur);
    if (ImGui::Combo(label, &idx, kPassNames, IM_ARRAYSIZE(kPassNames)))
    {
        const uint32_t presetVal = ComboIndexToPreset(idx);
        *legacyOpt = presetVal;
        if (passIndex < std::size(config->DlssNrPassOverrides))
        {
            if (presetVal == 0)
                config->DlssNrPassOverrides[passIndex].preset.reset();
            else
                config->DlssNrPassOverrides[passIndex].preset = presetVal;
        }
    }
}
} // namespace DlssNr
