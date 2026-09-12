---
tipo: bug
resumen: "Resolución de crash crítico en menú de ajustes por DualSense options.json, matriz de inyección DLL sin colisiones y fix de caídas FPS a 100 Hz."
fecha_creacion: "11/09/2026 03:40:00"
fecha_modificacion: "11/09/2026 03:40:00"
related: ["[[000_Indice_Bugs_y_Troubleshooting]]", "[[arq_motor_hispaoptiscaler_dlss5]]", "[[int_puredark_bridge_y_render_hooks]]"]
tecnologias: [DirectX 12, Streamline, DLSS-G, ReShade, RED4ext, Cyber Engine Tweaks, UTF-8 BOM]
archivos_claves: ["/Docs/Manuales/DOCUMENTACION_MODS_Y_FIXES_CYBERPUNK2077.md", "/Src/DESKTOP/HispaGameControl/Services/WidescreenFixService.cs", "/Src/DESKTOP/HispaGameControl/NovaOptics/NovaOptics.reds"]
rutas_claves: ["/Docs/Manuales", "/Src/DESKTOP/HispaGameControl/Services"]
prioridad_busqueda: 5
tags: [cyberpunk2077, crash, options_json, dlss5, streamline, frame_generation, reshade]
criticidad: "Critica"
estado: "Resuelto"
resolucion: "Resuelto"
---

# Resolución de Crash en Ajustes, Matriz de Inyección DLL y Fix de FPS a 100 Hz en Cyberpunk 2077

## 1. Síntomas y Diagnóstico del Crash en Menú de Ajustes

### Causa Raíz
Al acceder a la pestaña "Ajustes" de *Cyberpunk 2077*, el ejecutable sufría una terminación abrupta con violación de acceso a memoria (`EXCEPTION_ACCESS_VIOLATION` en `Cyberpunk2077.exe`).

El motor REDengine 4 valida de forma estricta el esquema del archivo de configuración `r6\config\settings\platform\pc\options.json`. Tras modificaciones previas realizadas por herramientas de corrección de campo de visión (FOV) o formateadores de texto sin soporte de codificación internacional:
- Se omitían las claves del controlador DualSense para PC en `/controls/mouseandkeyboard`:
  1. `EnableAdaptiveTriggerEffects` (`type: bool`, `order: 1`)
  2. `AdaptiveTriggersIntensity` (`type: float`, `order: 2`, `value: 1.0`, `min: 0.0`, `max: 1.0`, `step: 0.05`)
- El puntero de la interfaz C++ del menú intentaba acceder a posiciones de memoria nulas correspondientes a estos atributos faltantes.
- El archivo requería obligatoriamente codificación **UTF-8 con BOM** (`new UTF8Encoding(true)`).

### Solución Implementada
1. Reconstrucción completa de los 10 grupos oficiales y las 27 directivas de `options.json`.
2. Modificación de [`WidescreenFixService.cs`](file:///T:/DESARROLLO/GCLOUD/PROYECTOS/HispaGameControl/Src/DESKTOP/HispaGameControl/Services/WidescreenFixService.cs) para proteger este archivo contra sobrescrituras destructivas durante los ajustes de relación de aspecto Ultrawide.

---

## 2. Matriz de Inyección de DLLs sin Colisiones

La convivencia simultánea de múltiples frameworks de modding e inyectores gráficos en DirectX 12 suele provocar bloqueos si más de una librería compite por el mismo nombre de proxy (ej. `dxgi.dll`). Para *Cyberpunk 2077*, se estableció la siguiente matriz limpia de interposición:

```
[Cyberpunk2077.exe]
       ├── (Importa dxgi.dll)    ──► [Hispa-OptiScaler v10.0.0-dlss5 (dxgi.dll)]
       │                                     │
       │                                     └── (LoadReshade=true) ──► [ReShade64.dll]
       │
       ├── (Importa version.dll) ──► [Cyber Engine Tweaks - CET (version.dll)]
       │
       ├── (Importa winmm.dll)   ──► [RED4ext Framework (winmm.dll)]
       │
       └── (Llamadas DLSS 5)     ──► [nvngx.dll] ──► [nvngx_dlssnr.dll (165 MB)]
                                                 └──► [nvngx.dll_dlssnr.dll (Shim 13 KB)]
```

- **Hispa-OptiScaler** captura el pipeline de DirectX 12 como `dxgi.dll`.
- **ReShade** se descarga del rol de DLL proxy y se configura como hook subordinado mediante `LoadReshade=true` en `OptiScaler.ini`.
- **CET** y **RED4ext** quedan aislados en `version.dll` y `winmm.dll` respectivamente, sin interceptar el swapchain gráfico.

---

## 3. Diagnóstico y Solución: Caídas Periódicas de FPS a 100 Hz

### Evidencia en Logs (`OptiScaler.log`)
Durante el gameplay en monitores Ultrawide con frecuencia de refresco de 100 Hz se reportaban microtirones y caídas severas de fluidez:

```text
[streamline][warn] dlfgPresent.cpp: Frame rate over 100.00ms, reseting frame timer
[streamline][info] dlfgPresent.cpp: Flushing all worker queues ...
[streamline][info] rsync.cpp: RSYNC: Flush.
[streamline][info] dlfgPresent.cpp: DLSS-G interpolation state changed from enabled to disabled
[streamline][info] dlfgPresent.cpp: DLSS-G interpolation state changed from disabled to enabled
```

### Análisis Técnico
NVIDIA Streamline Frame Generation (`sl.dlss_g.dll`) incorpora un temporizador de seguridad de 100 milisegundos (`100.00ms`). Si el tiempo de renderizado de un frame supera o desincroniza dicho umbral (típico cuando la tasa de fotogramas coincide con el ciclo exacto del monitor o supera temporalmente los 100 Hz con VSync habilitado):
1. Streamline detecta un timeout de frame.
2. Vacia inmediatamente todas las colas de trabajo de los hilos de renderizado (`Flushing all worker queues`).
3. Desactiva la interpolación de fotogramas (`interpolation state changed to disabled`), provocando una caída instantánea del 50% de la tasa de FPS.
4. Reactiva la interpolación en el frame siguiente, generando un micro-stutter perceptible.

### Solución Definitiva
- Configurar un límite estricto de fotogramas en **96 FPS** (4 FPS por debajo de los 100 Hz de la pantalla).
- Aplicable automáticamente mediante [`NvDrsService`](file:///T:/DESARROLLO/GCLOUD/PROYECTOS/HispaGameControl/Src/DESKTOP/HispaGameControl/Services/NvDrsService.cs) o perfiles de Rivatuner Statistics Server (RTSS).
- Con el cap a 96 FPS, los intervalos de frame se estabilizan en ~10.4 ms, impidiendo que el temporizador interno de Streamline toque el límite crítico de 100 ms.

---

## 4. NovaOptics: Corrección de Autoexposición FPP en Vehículos

En Cyberpunk 2077, al conducir en perspectiva de primera persona (FPP), el algoritmo de adaptación de luminancia de REDengine sobreexpone el exterior para compensar la penumbra del interior de la cabina, cegando al jugador.

El módulo [`NovaOptics.reds`](file:///T:/DESARROLLO/GCLOUD/PROYECTOS/HispaGameControl/Src/DESKTOP/HispaGameControl/NovaOptics/NovaOptics.reds) inyecta lógica en REDscript para:
- Interceptar las transiciones de la cámara vehicular (`VehicleCameraPerspectiveChanged`).
- Aplicar un multiplicador de exposición compensado al shader de tonemapping.
- Mantener el rango dinámico HDR equilibrado entre el parabrisas y el salpicadero.

---

## 5. Resolución de Crash en Arranque (DEP 0xC0000005): SwapChain Proxy & Streamline Sync

### Causa Raíz
Durante la inicialización gráfica en resoluciones Ultrawide (3440x1440) con NVIDIA GeForce RTX 5090, Streamline v2.13 y ReShade 6.8:
1. `WrappedIDXGISwapChain4::QueryInterface` rechazaba interfaces COM extendidas no implementadas explícitamente (`IDXGISwapChainMedia`, runtime interfaces privadas del driver/motor) devolviendo `E_NOINTERFACE` y `*ppvObject = nullptr` en lugar de delegar en el swapchain real subyacente.
2. Al invocar métodos de interfaces de swapchain a través del wrapper, la llamada saltaba a una vtable nula provocando ejecución indebida (`DEP`) en `0x0000000000000000` con `RCX = 0x0` (`EXCEPTION_ACCESS_VIOLATION` `0xC0000005`).
3. En `StreamlineHooks::hkslDLSSGGetState`, cuando OptiScaler operaba con `currentFG == nullptr` (DLSS-G nativo de RTX 5090), sobreescribía `state.numFramesActuallyPresented = 1`, corrompiendo la sincronización de vallas de Streamline (`inputs-processing completion fence`).

### Solución Implementada
1. En [`OptiScaler/wrapped/wrapped_swapchain.cpp`](file:///T:/DESARROLLO/GCLOUD/PROYECTOS/HispaGameControl/OptiScaler/wrapped/wrapped_swapchain.cpp):
   - Fallback canónico en `QueryInterface`: delegación directa en `_real->QueryInterface(riid, ppvObject)` para cualquier IID desconocido.
   - Blindaje de métodos de `IDXGISwapChain1` e `IDXGISwapChain2` con verificación e inicialización perezosa (`lazy query`) de `_real1` y `_real2`.
2. En [`OptiScaler/hooks/Streamline_Hooks.cpp`](file:///T:/DESARROLLO/GCLOUD/PROYECTOS/HispaGameControl/OptiScaler/hooks/Streamline_Hooks.cpp):
   - Guarda nula en `o_slDLSSGGetState`.
   - Bypass estricto en `hkslDLSSGGetState`: no modificar `state` si `currentFG == nullptr`.
3. En `OptiScaler.ini`: `FGInput=auto` y `FGOutput=auto` para delegar la generación de fotogramas al hardware NVIDIA nativo.

---

## 6. Resolución de Crash en Inicialización Post-GetOptimalSettings: Ausencia de Runtime FidelityFX DX12

### Causa Raíz
Tras resolver el swapchain (commit `7ab73320`), el log evidenció que `ResizeBuffers` y `cloneFakeBuffers` completaron con éxito (`result: 0`). Sin embargo, el juego colapsaba inmediatamente después de la consulta de resolución:
1. Cyberpunk 2077 v2.13 incorpora integración nativa con FidelityFX (FSR 3.1) mediante carga dinámica tardía de `amd_fidelityfx_dx12.dll`.
2. Inmediatamente tras devolver la resolución óptima en `NVSDK_NGX_DLSS_GetOptimalSettingsCallback` (3440x1440 -> 2293x960), Cyberpunk consulta el ratio de escalado invocando la función `ffxQuery` (tipo de consulta `FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE` = `0x00010002`) mediante una tabla de punteros indirectos en su sección `.data` (RVA `0x035848F8`).
3. Al no desplegarse las DLLs de FidelityFX en `bin/x64` durante la instalación del paquete OptiScaler, el motor no encontraba `amd_fidelityfx_dx12.dll` y dejaba la tabla de punteros en cero (`0x0000000000000000`).
4. Al invocar la instrucción `call qword ptr [rip + 0x01838229]`, el flujo de ejecución saltaba a `0x0`, produciendo un fallo de ejecución DEP (`0xC0000005` Access Violation) en el hilo principal de renderizado (`TID 14940`).

### Solución Implementada
1. Despliegue en `J:\Steam\steamapps\common\Cyberpunk 2077\bin\x64\` del conjunto completo de librerías oficiales de FidelityFX:
   - `amd_fidelityfx_dx12.dll` (exportador de `ffxQuery`, `ffxCreateContext`, etc.)
   - `amd_fidelityfx_framegeneration_dx12.dll`
   - `amd_fidelityfx_upscaler_dx12.dll`
   - `amd_fidelityfx_vk.dll`
2. Sincronización en los 3 repositorios oficiales de paquetes de OptiScaler (`v1.0.1-dlss5`):
   - `Src/DESKTOP/HispaGameControl/Repository/OptiScaler/v1.0.1-dlss5/`
   - `C:\Soft\Juegos\HispaGameControl\Repository\OptiScaler/v1.0.1-dlss5/`
   - `K:\Librerias\HispaGameControl\Repository\OptiScaler/v1.0.1-dlss5/`
3. Actualización de `.hispa_neural_manifest.json` para auditar la integridad de las librerías FidelityFX.

---

## 7. Diccionario de Base de Datos y APIs

- [[WidescreenFixService]] [explicacion:: Servicio C# para corrección de relaciones de aspecto y blindaje de options.json]
- [[NovaOptics]] [explicacion:: Script REDscript para corrección de luminancia y exposición vehicular en Cyberpunk 2077]
- [[Streamline_DLSS_G]] [explicacion:: Módulo NVIDIA Streamline para generación de frames con temporizador crítico de 100 ms]
- [[WrappedIDXGISwapChain4]] [explicacion:: Clase envoltorio C++ COM para proxies DXGI SwapChain 1-4 en OptiScaler]
- [[StreamlineHooks]] [explicacion:: Interceptores C++ para NVIDIA Streamline v2 e integración con generadores de fotogramas]
- [[amd_fidelityfx_dx12]] [explicacion:: Runtime oficial de AMD FidelityFX DX12 que exporta ffxQuery requerido por Cyberpunk 2077]
