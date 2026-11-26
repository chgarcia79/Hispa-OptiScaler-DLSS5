#pragma once

#include <Config.h>
#include <dlssnr/NrNvfp4.h>
#include <menu/menu_common.h>
#include <imgui/imgui.h>
#include <algorithm>
#include <cstdint>

namespace DlssNr
{
inline int PresetToComboIndex(uint32_t preset)
{
    const uint32_t cur = Nvfp4::Current(preset);
    for (int i = 0; i < static_cast<int>(std::size(Nvfp4::kModels)); ++i)
    {
        if (Nvfp4::kModels[i].preset == cur)
            return i;
    }
    return 0;
}

inline uint32_t ComboIndexToPreset(int idx)
{
    if (idx >= 0 && idx < static_cast<int>(std::size(Nvfp4::kModels)))
        return Nvfp4::kModels[idx].preset;
    return 0u;
}

template <typename HelpFn> inline void RenderNvfp4PresetSelector(::Config* config, HelpFn helpMarker)
{
    bool fp8Enabled = config->DlssNrFp8Enabled.value_or_default();
    if (ImGui::Checkbox("Habilitar modelos FP8 (DX12 / Vulkan)", &fp8Enabled))
        config->DlssNrFp8Enabled = fp8Enabled;
    helpMarker("Permite usar FP8 2x/3xPass-in-one (OptiScaler/nvfp4/fp8) en RTX 40/50.");

    bool int8Enabled = config->DlssNrInt8Enabled.value_or_default();
    if (ImGui::Checkbox("Habilitar modelos INT8 (DX12 / Vulkan)", &int8Enabled))
        config->DlssNrInt8Enabled = int8Enabled;
    helpMarker("Permite usar los 4 modelos propios INT8 (Original, 2xPass-in-one y Alternate training) sin runtime NR.");

    bool vkModels = config->DlssNrVulkanModelsEnabled.value_or_default();
    if (ImGui::Checkbox("Habilitar Modelos NR en Vulkan Nativo", &vkModels))
        config->DlssNrVulkanModelsEnabled = vkModels;
    helpMarker("Ejecuta INT8/NVFP4/FP8 en Vulkan nativo (requiere activar [DlssNr] Enabled=true antes de arrancar).");

    static const char* kNames[] = {
        "Automatic (Performance / Small FP8)",
        "FP8 Original (Honest Tench)",
        "FP8 Optimized (Crazy Cuckoo)",
        "FP8 2xPass-in-one",
        "FP8 3xPass-in-one",
        "INT8 Original",
        "INT8 Original - Alternate training",
        "INT8 2xPass-in-one",
        "INT8 2xPass-in-one - Alternate training",
        "NVFP4 Original (RTX 50 4-bit)",
        "NVFP4 2xPass-in-one (RTX 50 4-bit)",
        "NVFP4 3xPass-in-one (RTX 50 4-bit)"
    };
    const uint32_t current = config->DlssNrPreset.value_or_default();
    int idx = PresetToComboIndex(current);
    if (ImGui::Combo("Preset del modelo IA", &idx, kNames, IM_ARRAYSIZE(kNames)))
        config->DlssNrPreset = ComboIndexToPreset(idx);

    helpMarker("Ordenado por precision de pesos: FP8 -> INT8 -> NVFP4.\n"
               "INT8 y NVFP4 corren en el host propio (DX12 y Vulkan nativo) sin depender de nvngx_dlssnr.dll.\n"
               "Sin fallback silencioso: si faltan pesos o kernels, se detiene el pase e informa abajo.");

    const uint32_t activePreset = config->DlssNrPreset.value_or_default();
    if (Nvfp4::IsNvfp4(activePreset))
    {
        if (const auto problem = Nvfp4::Problem(); !problem.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "  Error Modelo: %s", problem.c_str());
        else if (Nvfp4::Running())
            ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.55f, 1.0f), "  %s Activo (%s)",
                               Nvfp4::IsInt8(activePreset) ? "INT8" : (Nvfp4::IsFp8(activePreset) ? "FP8" : "NVFP4 4-bit"),
                               Nvfp4::ModelName(activePreset));
        else
            ImGui::TextDisabled("  Estado: %s", Nvfp4::Status().c_str());

        if (Nvfp4::IsPassInOne(activePreset) || Nvfp4::IsFp8(activePreset) || activePreset == 26u || activePreset == 28u)
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
}

inline void RenderNvfp4PassPresetCombo(const char* label, ::Config* config, uint32_t passIndex,
                                       CustomOptional<uint32_t>* legacyOpt)
{
    static const char* kPassNames[] = {
        "Auto (Heredar Pasada 1)",
        "FP8 Original",
        "FP8 Optimized",
        "FP8 2xPass-in-one",
        "FP8 3xPass-in-one",
        "INT8 Original",
        "INT8 Original - Alternate training",
        "INT8 2xPass-in-one",
        "INT8 2xPass-in-one - Alternate training",
        "NVFP4 Original",
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
