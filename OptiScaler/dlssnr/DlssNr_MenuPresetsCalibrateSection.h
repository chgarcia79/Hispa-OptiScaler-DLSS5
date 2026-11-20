#pragma once

#include <Config.h>
#include <dlssnr/DlssNr_Calibrate.h>
#include <dlssnr/DlssNr_Presets.h>
#include <menu/menu_common.h>
#include <imgui/imgui.h>
#include <cstring>
#include <string>
#include <vector>

namespace DlssNr
{
template <typename HelpFn> inline void RenderPresetsCalibrateSection(::Config* config, HelpFn helpMarker)
{
    if (!config)
        return;

    if (auto ch = ScopedCollapsingHeader("Presets NR y Auto-Calibrador (DX12)"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        static char presetName[128] = "mi_preset";
        static std::string statusMsg;

        const auto list = Presets::List();
        static int selIdx = 0;
        if (selIdx >= static_cast<int>(list.size()))
            selIdx = 0;

        if (!list.empty())
        {
            std::vector<const char*> names;
            names.reserve(list.size());
            for (const auto& s : list)
                names.push_back(s.c_str());

            if (ImGui::Combo("Preset guardado (nr-presets)", &selIdx, names.data(), static_cast<int>(names.size())))
                std::strncpy(presetName, list[static_cast<size_t>(selIdx)].c_str(), sizeof(presetName) - 1);

            if (ImGui::SmallButton("Cargar"))
            {
                const auto err = Presets::Load(list[static_cast<size_t>(selIdx)], *config);
                statusMsg = err.empty() ? ("Cargado: " + list[static_cast<size_t>(selIdx)]) : err;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Renombrar"))
            {
                const auto err = Presets::Rename(list[static_cast<size_t>(selIdx)], presetName);
                statusMsg = err.empty() ? ("Renombrado a: " + std::string(presetName)) : err;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Eliminar"))
            {
                const auto err = Presets::Delete(list[static_cast<size_t>(selIdx)]);
                statusMsg = err.empty() ? ("Eliminado: " + list[static_cast<size_t>(selIdx)]) : err;
            }
        }
        else
        {
            ImGui::TextDisabled("  Sin presets guardados en nr-presets/.");
        }

        ImGui::InputText("Nombre preset", presetName, sizeof(presetName));
        if (ImGui::SmallButton("Guardar / Sobrescribir preset"))
        {
            const auto err = Presets::Save(presetName, *config);
            statusMsg = err.empty() ? ("Guardado: " + std::string(presetName)) : err;
        }
        if (!statusMsg.empty())
            ImGui::TextDisabled("  %s", statusMsg.c_str());

        ImGui::Separator();

        bool calEnabled = config->DlssNrCalibratorEnabled.value_or_default();
        if (ImGui::Checkbox("Habilitar Auto-Calibrador DX12", &calEnabled))
            config->DlssNrCalibratorEnabled = calEnabled;
        helpMarker("Congela un frame en DX12 y ajusta sliders de la pasada 1 para igualar un preset objetivo.");

        ImGui::BeginDisabled(!calEnabled);
        bool skinOpt = config->DlssNrCalibratorSkinOpt.value_or_default();
        if (ImGui::Checkbox("Optimizar Piel, Entorno y Highlight Guard", &skinOpt))
            config->DlssNrCalibratorSkinOpt = skinOpt;

        const auto report = Calibrate::Read();
        if (report.active)
        {
            if (ImGui::SmallButton("Detener calibracion"))
                Calibrate::Cancel();
        }
        else if (!list.empty())
        {
            if (ImGui::SmallButton("Calibrar hacia preset seleccionado"))
                Calibrate::Start(list[static_cast<size_t>(selIdx)], 0u, skinOpt);
        }
        if (!report.text.empty())
            ImGui::TextWrapped("  Estado calibrador: %s", report.text.c_str());
        ImGui::EndDisabled();
    }
}
} // namespace DlssNr
