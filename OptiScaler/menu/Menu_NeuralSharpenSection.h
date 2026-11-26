#pragma once

#include <algorithm>
#include <cstdint>

#include <Config.h>
#include <OptiTypes.h>
#include <menu/ScopedCollapsingHeader.h>
#include <imgui/imgui.h>

namespace MenuNeuralSharpen
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

inline void RenderRadioOptions(int32_t& sharpnessShader, ::Config* config)
{
    if (!config)
        return;

    if (ImGui::RadioButton("Neural (BaBa)", &sharpnessShader, (int32_t) SharpenShader::BabaNeural))
        config->SharpnessShader = SharpenShader::BabaNeural;
    HelpTip("Red neuronal ligera BaBa (solo luma, respeta HDR y evita fireflies).\n"
            "0.50 de Sharpness equivale a la intensidad 1.0 original.");

    ImGui::SameLine(0.0f, 6.0f);

    if (ImGui::RadioButton("OptiSharpen", &sharpnessShader, (int32_t) SharpenShader::OptiSharpen))
        config->SharpnessShader = SharpenShader::OptiSharpen;
    HelpTip("Red neuronal entrenada para OptiScaler (SDR y HDR, 6 modelos).\n"
            "0.50 de Sharpness equivale a la fuerza entrenada por defecto.");
}

inline void RenderParameters(::Config* config, bool isHdr)
{
    if (!config)
        return;

    const auto shader = config->SharpnessShader.value_or_default();
    if (shader != SharpenShader::BabaNeural && shader != SharpenShader::OptiSharpen)
        return;

    if (bool msDebug = config->MotionSharpnessDebug.value_or_default(); ImGui::Checkbox("MAS Debug", &msDebug))
        config->MotionSharpnessDebug = msDebug;
    HelpTip("Rojo: mas nitidez aplicada; Verde: nitidez reducida por movimiento.");

    const char* headerLabel =
        shader == SharpenShader::OptiSharpen ? "Parametros OptiSharpen" : "Parametros Neural (BaBa)";
    if (auto ch = ScopedCollapsingHeader(headerLabel); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        if (shader == SharpenShader::BabaNeural)
        {
            const char* models[] = { "Standard (A)", "Edge-enhancing (B)" };
            int model = std::clamp(config->BabaNeuralModel.value_or_default(), 0, 1);
            if (ImGui::Combo("Modelo BaBa", &model, models, IM_ARRAYSIZE(models)))
                config->BabaNeuralModel = model;

            float edgeResp = std::clamp(config->BabaNeuralEdgeResponse.value_or_default(), -1.0f, 1.0f);
            if (ImGui::SliderFloat("Respuesta de Bordes", &edgeResp, -1.0f, 1.0f, "%.2f"))
                config->BabaNeuralEdgeResponse = edgeResp;
            HelpTip("Negativo: suaviza bordes dentados; Positivo: realza bordes.");

            const char* linearModes[] = { "Auto (Float+HDR)", "Forzar Lineal", "Forzar Perceptual" };
            int linearMode = std::clamp(config->BabaNeuralLinearMode.value_or_default(), 0, 2);
            if (ImGui::Combo("Espacio Lineal / HDR", &linearMode, linearModes, IM_ARRAYSIZE(linearModes)))
                config->BabaNeuralLinearMode = linearMode;
            HelpTip("Auto detecta texturas FLOAT pre-tonemap y HDR10 PQ sin levantar negros.");

            const char* views[] = { "Normal", "Sharpening map", "Residual map" };
            int view = std::clamp(config->BabaNeuralDebugView.value_or_default(), 0, 2);
            if (ImGui::Combo("Vista Debug", &view, views, IM_ARRAYSIZE(views)))
                config->BabaNeuralDebugView = view;
        }
        else
        {
            const char* presets[] = { "Natural (Fiel)", "Crisp (Nitidez extra)" };
            int preset = std::clamp(config->OptiSharpenPreset.value_or_default(), 0, 1);
            if (ImGui::Combo("Perfil OptiSharpen v2", &preset, presets, IM_ARRAYSIZE(presets)))
                config->OptiSharpenPreset = preset;
            HelpTip("Usa pipelines precompiladas especializadas Natural o Crisp (OptiSharpen v2).");

            const char* models[] = { "Clean", "TAA", "TAA Wide", "TAA Wide 12", "Punch 10", "Punch 15" };
            int model = std::clamp(config->OptiSharpenModel.value_or_default(), 0, 5);
            if (ImGui::Combo("Modelo OptiSharpen", &model, models, IM_ARRAYSIZE(models)))
                config->OptiSharpenModel = model;

            float hdrLimiter = std::clamp(config->OptiSharpenHdrLimiter.value_or_default(), 0.0f, 2.0f);
            if (ImGui::SliderFloat("Limitador Anti-Halos HDR (hm16)", &hdrLimiter, 0.0f, 2.0f, "%.2f"))
                config->OptiSharpenHdrLimiter = hdrLimiter;
            HelpTip("Controla la respuesta sobre luces brillantes HDR para evitar halos/contornos oscuros.");

            const char* views[] = { "Normal", "Sharpening map", "Change magnitude" };
            int view = std::clamp(config->OptiSharpenDebugView.value_or_default(), 0, 2);
            if (ImGui::Combo("Vista Debug", &view, views, IM_ARRAYSIZE(views)))
                config->OptiSharpenDebugView = view;
        }

        if (bool clampOut = config->DAClampOutput.value_or(isHdr); ImGui::Checkbox("Clamp Output [0..1]", &clampOut))
            config->DAClampOutput = clampOut;

        if (ImGui::Button("Resetear valores Neural"))
        {
            if (shader == SharpenShader::BabaNeural)
            {
                config->BabaNeuralModel.reset();
                config->BabaNeuralEdgeResponse.reset();
                config->BabaNeuralDebugView.reset();
                config->BabaNeuralLinearMode.reset();
            }
            else
            {
                config->OptiSharpenPreset.reset();
                config->OptiSharpenModel.reset();
                config->OptiSharpenHdrLimiter.reset();
                config->OptiSharpenDebugView.reset();
            }
        }
    }
}
} // namespace MenuNeuralSharpen
