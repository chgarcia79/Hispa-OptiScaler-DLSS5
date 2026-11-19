#pragma once

#include <Config.h>
#include <dlssnr/NrNvfp4.h>
#include <imgui/imgui.h>
#include <algorithm>
#include <cstdint>

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
    default: return 0;
    }
}

inline uint32_t ComboIndexToPreset(int idx)
{
    static constexpr uint32_t kMap[] = { 0u, 1u, 2u, 4u, 16u, 15u };
    return (idx >= 0 && idx < 6) ? kMap[idx] : 0u;
}

template <typename HelpFn> inline void RenderNvfp4PresetSelector(::Config* config, HelpFn helpMarker)
{
    static const char* kNames[] = {
        "Auto (Performance / Small FP8)",
        "Quality (Honest Tench - Maxima Fidelidad)",
        "Performance (Crazy Cuckoo Small FP8, +40% FPS)",
        "Original - NVFP4 (RTX 50 4-bit)",
        "NVFP4 2xPass-in-one (RTX 50 4-bit)",
        "NVFP4 3xPass-in-one (RTX 50 4-bit)"
    };
    const uint32_t current = config->DlssNrPreset.value_or_default();
    int idx = PresetToComboIndex(current);
    if (ImGui::Combo("Preset del modelo IA", &idx, kNames, IM_ARRAYSIZE(kNames)))
        config->DlssNrPreset = ComboIndexToPreset(idx);

    helpMarker("Modelos 0..2 embebidos en nvngx_dlssnr.dll.\n"
               "NVFP4 (4, 16, 15): Kernels 4-bit exclusivos para RTX 50 en D3D12 (OptiScaler/nvfp4).\n"
               "2xPass-in-one y 3xPass-in-one dan aspecto multi-pasada en 1 sola evaluacion.\n"
               "Sin fallback silencioso: si faltan pesos o en Vulkan, no se ejecuta Preset 1.");

    if (Nvfp4::IsNvfp4(config->DlssNrPreset.value_or_default()))
    {
        if (const auto problem = Nvfp4::Problem(); !problem.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "  NVFP4 Error: %s", problem.c_str());
        else if (Nvfp4::Running())
            ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.55f, 1.0f), "  NVFP4 4-bit Activo (%s)",
                               Nvfp4::Status().c_str());
        else
            ImGui::TextDisabled("  NVFP4: %s", Nvfp4::Status().c_str());

        if (Nvfp4::IsPassInOne(config->DlssNrPreset.value_or_default()))
        {
            if (ImGui::SmallButton("Aplicar Tuning Recomendado 2x/3xPass-in-one"))
            {
                config->DlssNrStyle = 2u; // Cinematografico (Cinematic)
                config->DlssNrLocalStructure = 0.50f;
                config->DlssNrLocalTone = 0.80f;
                config->DlssNrPasses = 1u;
            }
            helpMarker("Aplica Style = Cinematic, Local Structure = 0.50, Local Tone = 0.80 y 1 pasada.");
        }
    }
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
        "NVFP4 3xPass-in-one"
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
