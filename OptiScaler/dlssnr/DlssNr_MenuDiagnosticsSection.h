#pragma once

#include <Config.h>
#include <dlssnr/DlssNrFeature_Dx12.h>
#include <menu/menu_common.h>
#include <imgui/imgui.h>
#include <algorithm>
#include <cstdio>

namespace DlssNr
{
template <typename HelpFn> inline void RenderDiagnosticsSection(::Config* config, HelpFn helpMarker)
{
    if (!config)
        return;

    if (auto ch = ScopedCollapsingHeader("Guardia CommandList, VRAM y Blindajes (v1.0.4-dev15)"); ch.IsHeaderOpen())
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

        const float remSec = GetAutoFreeRemainingSec();
        if (autoFree && IsVramAllocated() && remSec >= 0.0f)
        {
            const float frac = std::clamp(1.0f - (remSec / static_cast<float>(delaySec)), 0.0f, 1.0f);
            char vramOverlay[96] {};
            std::snprintf(vramOverlay, sizeof(vramOverlay), "Liberando VRAM NR en %.1fs / %ds", remSec, delaySec);
            ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), vramOverlay);
        }

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

        ImGui::Separator();

        bool defRel = config->DeferredFeatureRelease.value_or_default();
        if (ImGui::Checkbox("Retiro Diferido Seguro de Features D3D12", &defRel))
            config->DeferredFeatureRelease = defRel;
        helpMarker("Retiene features D3D12 liberadas hasta completar los Presents en vuelo.");

        bool slNative = config->DlssNrStreamlineNativeBridge.value_or_default();
        if (ImGui::Checkbox("Puente DLSS-NR Nativo en Juegos Streamline", &slNative))
            config->DlssNrStreamlineNativeBridge = slNative;
        helpMarker("Publica callbacks NR para juegos Streamline (Where Winds Meet, etc.).");

        bool disOta = config->StreamlineDisableOta.value_or_default();
        if (ImGui::Checkbox("Bloquear Descargas OTA de Streamline", &disOta))
            config->StreamlineDisableOta = disOta;
        helpMarker("Evita que Streamline descargue plugins OTA paralelos y provoque crash 1E7 al iniciar.");

        bool skipNvApp = config->StreamlineSkipNvidiaAppOverride.value_or_default();
        if (ImGui::Checkbox("Ignorar Override de Plugins de NVIDIA App (0x10E41E06)", &skipNvApp))
            config->StreamlineSkipNvidiaAppOverride = skipNvApp;
        helpMarker("Responde 0 en memoria a la clave DRS 0x10E41E06 sin tocar el perfil global del driver.");

        bool lockWin = config->OverlayLockPrimaryWindow.value_or_default();
        if (ImGui::Checkbox("Anclar Overlay a Ventana Principal (Anti-Crash Pop-out)", &lockWin))
            config->OverlayLockPrimaryWindow = lockWin;
        helpMarker("Evita crashes y recursion WndProc cuando el juego abre ventanas secundarias (MSFS 2024).");
    }
}
} // namespace DlssNr
