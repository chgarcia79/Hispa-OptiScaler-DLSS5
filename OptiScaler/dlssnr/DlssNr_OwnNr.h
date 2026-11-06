#pragma once

#include <Config.h>
#include "DlssNr_GameNrRuntime.h"

namespace DlssNr
{
// OptiScaler's own NR runs while enabled, and yields while the game runs its own NR (NGX feature 18), so the picture
// is never processed twice. The saved setting in OptiScaler.ini is never rewritten.
inline constexpr const char* GameNrActiveMessage =
    "El juego tiene su propio DLSS Neural Rendering activo: el pase NR de HispaOptiScaler queda desactivado en esta "
    "sesion para evitar doble procesado. Desactiva el NR del juego para usar el de HispaOptiScaler.";

inline bool IsGameNativeNrRunning() { return GameNr::IsGameNativeNrRunning(); }

inline bool OwnNrEnabled()
{
    return Config::Instance()->DlssNrEnabled.value_or_default() && !GameNr::IsGameNativeNrRunning();
}
} // namespace DlssNr
