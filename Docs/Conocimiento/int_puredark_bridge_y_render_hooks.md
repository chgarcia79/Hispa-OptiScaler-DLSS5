---
tipo: integracion
resumen: "Integración del puente PureDark Bridge, intercepción de llamadas NVNGX y redirección de Reflex con Fakenvapi."
fecha_creacion: 2026-09-11
fecha_modificacion: 2026-09-11
related:
  - "[[000_Indice_Integraciones_Externas]]"
  - "[[arq_motor_hispaoptiscaler_dlss5]]"
  - "[[int_gestor_librerias_graficas_dlss_fsr_xess]]"
estado: activo
---

# PureDark Bridge y Ganchos de Renderizado NVNGX

## 1. Contexto del PureDark Bridge
PureDark desarrolló mods de inyección de DLSS y Frame Generation para títulos sin soporte nativo (*The Elder Scrolls V: Skyrim*, *Fallout 4*, *Starfield*, *Elden Ring*, *The Last of Us Part I*). Estos mods capturan los búferes del motor gráfico y emiten llamadas directas a la API de NVIDIA NGX (`NVSDK_NGX_*`).

Para que OptiScaler pueda actuar como backend de estos mods:
- En [Src/LIBRERIAS/HispaOptiScaler/OptiScaler/inputs/NVNGX.cpp](file:///T:/DESARROLLO/GCLOUD/PROYECTOS/HispaGameControl/Src/LIBRERIAS/HispaOptiScaler/OptiScaler/inputs/NVNGX.cpp), OptiScaler implementa el conjunto completo de funciones exportadas de `nvngx.dll`.
- En `NVSDK_NGX_UpdateFeature`, OptiScaler intercepta las peticiones de los mods de PureDark cuando consultan o modifican características no convencionales (distintas de `NVSDK_NGX_Feature_SuperSampling`), devolviendo `NVSDK_NGX_Result_Success` de manera segura en lugar de abortar la ejecución.

---

## 2. Bypass de Verificación de Firma Criptográfica (Artur Method)
A partir de la versión DLSS 3.7+, NVIDIA implementó chequeos estrictos de firma digital en sus binarios para impedir la interposición no autorizada.

OptiScaler integra la técnica creada por **Artur** (autor de *DLSS Enabler*):
- **Archivos:** `Crypt32_Hooks.h`, `Wintrust_Hooks.h`, `Kernel_Hooks.cpp`.
- **Mecanismo:** Intercepta llamadas de `WinVerifyTrust` y APIs criptográficas de Windows (`CryptVerifyCertificateSignature`), validando virtualmente las llamadas provenientes de los módulos de DLSS para que acepten la librería de interposición sin emitir errores de integridad.

---

## 3. Redirección de Reflex y Baja Latencia (`Fakenvapi`)
- Cuando un juego invoca funciones de latencia de NVIDIA Reflex (`NvAPI_D3D_SetLatencyMarker`, `NvAPI_D3D_Sleep`), OptiScaler las desvía mediante `fakenvapi.ini` hacia tecnologías abiertas:
  1. **AMD Anti-Lag 2:** Para GPUs Radeon RDNA1 en adelante.
  2. **LatencyFlex (LFX):** Implementación agnóstica de hardware para cualquier GPU.
  3. **Intel XeLL:** Extensión de baja latencia para GPUs Intel Arc.
