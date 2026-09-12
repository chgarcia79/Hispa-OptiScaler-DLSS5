---
tipo: arquitectura
resumen: "Arquitectura interna en C++20 del motor OptiScaler, proxy exports, detours de DirectX/Vulkan y pipeline de DLSS 5 Neural Rendering."
fecha_creacion: 2026-09-11
fecha_modificacion: 2026-09-12
related:
  - "[[000_Indice_Arquitectura_y_Diseno]]"
  - "[[000_Indice_Implementaciones_y_Modulos]]"
  - "[[int_puredark_bridge_y_render_hooks]]"
estado: activo
---

# Motor HispaOptiScaler y Pipeline DLSS 5 Neural Rendering

## 1. Visión General
HispaOptiScaler es una solución de interposición gráfica (*middleware*) en C++20 que desacopla la API de escalado que solicita el videojuego (Input) del backend de escalado y generación de fotogramas que ejecuta la GPU (Output).

Ubicación del código fuente: [Src/LIBRERIAS/HispaOptiScaler/](file:///T:/DESARROLLO/GCLOUD/PROYECTOS/HispaGameControl/Src/LIBRERIAS/HispaOptiScaler)

```
[ Videojuego ] 
       │ (Llamadas estándar NVNGX / DXGI / D3D12 / Vulkan)
       ▼
[ Proxy DLL / Thunking ] (dxgi.dll / version.dll / winmm.dll / nvngx.dll)
       │
       ├─► [ Hooking VMT / Detours ] ──► Captura de SwapChain, CommandQueue y Barreras
       ├─► [ GPU Spoofing ] ────────────► Falsificación de Vendor/Device ID (RTX 4090, Arc, Radeon)
       │
       ▼
[ Pipeline de Escalado ] ──────────────► DLSS-SR / FSR 3.1 / FSR 4 / XeSS 2.1
       │
       ▼
[ DLSS 5 Neural Rendering ] ───────────► nvngx.dll_dlssnr.dll (Forwarder Shim)
       │                                  │
       │                                  ▼
       │                               nvngx_dlssnr.dll (Modelo Neural NVIDIA RTX 50)
       ▼
[ Composición de Ratio / Shader ] ─────► RenoDX Tone Mapping (Espacio OkLab, Gamut AP1)
       │
       ▼
[ Presentación Final ] ────────────────► SwapChain -> Pantalla
```

---

## 2. Mecanismos de Interposición y Hooking

### 2.1. Proxy Exports y Carga Dinámica
- **Archivos clave:** `Source.def`, `dllmain.cpp`, `NtdllProxy.cpp`.
- Permite a la DLL actuar como `dxgi.dll`, `d3d12.dll`, `version.dll`, `winmm.dll`, `dbghelp.dll`, etc.
- Carga de forma segura la DLL original del sistema desde `System32` o `plugins/` utilizando llamadas `NtdllProxy::LoadLibraryExW_Ldr` para evitar bloqueos por recursión de carga.

### 2.2. Interposición de DirectX 12 y Auto-Fixing de Barreras
- **Archivos clave:** `D3D12_Hooks.cpp`, `CommandBuffer_StateTracker.h`.
- **Resource Barrier Auto-Fixing:** Detecta y corrige estados inválidos de recursos enviados por motores como Unreal Engine (que generaban artefactos multicolores *"rainbow artifacts"* en GPUs AMD/Intel), forzando transiciones correctas en `ColorResourceBarrier`, `MotionVectorResourceBarrier` y `DepthResourceBarrier`.
- **Restauración de Signatures:** Restaura firmas raíz gráficas y de cómputo tras la inyección de compute shaders.

### 2.3. Puentes Multi-API (Dx11-on-12 y Vulkan-on-12)
- **Dx11-on-12:** Contexto en segundo plano D3D12 que comparte texturas DX11 mediante NT Shared Handles (`D3D11_RESOURCE_MISC_SHARED_NTHANDLE`), sincronizadas vía GPU Fences o eventos CPU.
- **Vulkan-on-12:** Intercambio de memoria compartida POSIX/Win32 (`VK_KHR_external_memory_win32`) para habilitar upscalers D3D12 en juegos Vulkan (como *Indiana Jones*).

---

## 3. DLSS 5 Neural Rendering (DLSS-NR)

### 3.1. Arquitectura del Modelo
- Utiliza la biblioteca oficial `nvngx_dlssnr.dll` (~165 MB, Feature ID 18 de NGX).
- Se ejecuta inmediatamente después del reescalado y antes de la generación de fotogramas.

### 3.2. Bypass de Verificación con Forwarder DLL
- **Problema de NVIDIA:** `nvngx_dlssnr.dll` analiza la dirección de retorno del llamador mediante `RtlPcToFileHeader`. Si el módulo llamante no se llama exactamente `nvngx.dll`, devuelve error `0xBAD0000B FAIL_PlatformError`.
- **Solución implementada:** Se compila un proyecto dedicado (`dlssnr_forwarder.vcxproj`) que genera una mini-DLL llamada obligatoriamente `nvngx.dll_dlssnr.dll` (~13 KB). Esta DLL expone:
  - `dlssnr_call_create`
  - `dlssnr_call_evaluate`
  - `dlssnr_call_set_extras`
- Todas las peticiones de OptiScaler pasan por este puente, burlando la validación sin modificar el binario original de NVIDIA.

### 3.3. Composición de Ratio y Shaders (RenoDX)
- En lugar de una suma directa, se aplica una composición de ratio entre la luminancia original del upscaler y la codificación devuelta por la red neural.
- El shader HLSL (`dlssnr.hlsl`, precompilado con `fxc.exe -O3`) opera en espacio de color **OkLab** con limitación de gamut **AP1** y parámetros ajustables: `TransferStrength`, `ColourStrength`, `WhitePointScale`, `MaxRatio` y `WorkingScale`.

### 3.4. Pipeline Conmutable Pre-SR / Post-SR y Ajuste de Piel
- **Bifurcación Pre-SR vs Post-SR:** Permite conmutar la evaluación de `DlssNrFeature` antes del reescalado (Pre-SR, para mitigar cuellos de botella de Ray Tracing a resolución base) o después (Post-SR, nativo a 3440×3440 para máxima nitidez). Por defecto opera en **Post-SR (`RunBeforeSR = false`)**.
- **Ajuste Piel vs Escenario (Skin vs Scenery):** Detección automática en espacio cromático OkLab (`AutoSkinMask`) con atenuación de contraste local (`SkinStructure`) para prevenir sobre-enfoque o efecto "piel plástica" en rostros humanos, preservando el detalle arquitectónico del entorno (`LocalStructure`).

---

## 4. Upstream OptiScaler v0.9.4 y Estabilidad de 1% Lows

### 4.1. Mejoras del Motor Base v0.9.4
- **Waitable Object Swapchain Simulation:** Simulación de objetos en espera en la cadena de intercambio DX12 para sincronizar la cola de comandos de presentación. Elimina la fluctuación errática de frametimes y eleva drásticamente los **1% Lows** y 0.1% Lows.
- **Protección ResizeBuffers1:** Corrección de cuelgues durante cambios dinámicos de resolución y transiciones fullscreen/borderless.
- **Manual Input Polling:** Sondeo directo de teclas y comandos (`UpdateManualInput`) para garantizar el control del OSD incluso en videojuegos con captura exclusiva de periféricos o *Raw Input* agresivo.
- **FSR 4.1.1 e INT8 (FFX 2.3):** Incorporación de soporte para cuantización INT8 en núcleos tensores/vectoriales, reduciendo latencia de escalado.

### 4.2. Versionado SemVer y Convivencia en HispaGameControl
- Coexistencia normalizada y estandarizada bajo SemVer:
  - `v1.0.0-dlss5`: Versión inicial de referencia (previamente denominada v10.0.0-dlss5).
  - `v1.0.1-dlss5`: Versión predeterminada basada en upstream estable 0.9.4 con soporte DLSS-NR completo, estabilidad de 1% lows y FSR 4.1.1.
- Mapeo transparente en C# (`GameInfo.NormalizeOptiScalerVersionString`) para garantizar retrocompatibilidad automática con bases de datos SQLite y perfiles que contengan nombres anteriores (`v10.0.0-dlss5`, `v0.9.4-dlss5`).
- Despliegue automático vía `DlssSwapperService`, gestión en `MainViewModel` y empaquetado de extracción en `ExtraerLibrerias.ps1`.

### 4.3. Compatibilidad con ReShade y Ciclo de Vida COM (`WrappedIDXGISwapChain4`)
- **Diagnóstico Exhaustivo del Crash en Cyberpunk 2077:** Al encadenar `OptiScaler.dll` (como `dxgi.dll`) con ReShade, tras interceptar `ResizeBuffers`, el juego o el hook de ReShade ejecutaba `IDXGISwapChain3::SetColorSpace1(ColorSpace = 0)`. El crash dump minidump (`Cyberpunk2077.dmp`, TID 6896) reveló una violación DEP en `0x0` con el puntero `this` (`RCX`) exactamente en **`0x0000000000000000`**.
- **Causa Raíz:** `WrappedIDXGISwapChain4::QueryInterface` rechazaba indebidamente las interfaces `IDXGISwapChain1` hasta `IDXGISwapChain4` devolviendo `E_NOINTERFACE` y fijando `*ppvObject = nullptr` cuando los punteros subyacentes `_real1..4` aún no estaban resueltos o existía intermediación de proxies (p. ej. Streamline `sl.interposer.dll` o ReShade). Al asumir Cyberpunk que en DX12 la interfaz `IDXGISwapChain3` está siempre soportada, invocaba `SetColorSpace1` sobre el puntero nulo entregado.
- **Corrección Canónica COM (Commit `bced3855`):**
  - **`QueryInterface` Canónico:** `WrappedIDXGISwapChain4` devuelve siempre `S_OK` con su propio puntero `static_cast<IDXGISwapChainX*>(this)` para todas las interfaces COM de swapchain (1 al 4) que implementa.
  - **Lazy Query Subyacente:** Resolución perezosa de `_real1..4` ante cualquier llamada entrante.
  - **Guardas Nulas y Fallbacks:** En `CheckColorSpaceSupport`, `SetColorSpace1` (con soporte para `SkipColorSpace`), `ResizeBuffers1` (con fallback a `ResizeBuffers` ordinario si `_real3` es nulo), `GetCurrentBackBufferIndex` y `SetHDRMetaData`.
  - **Liberación Ordenada:** Destructor `~WrappedIDXGISwapChain4()` para prevenir Use-After-Free.

### 4.4. Gobernanza y Pipeline de CI/CD Exclusivo en GitHub Actions
- Por norma determinista del proyecto (`[COMPILACION_MOTOR_OPTISCALER]`), queda prohibida la compilación local con MSBuild/VS debido a la ausencia de los submódulos pesados de C++ (`spdlog`, `vulkan-headers`, `detours`, etc.).
- La compilación se delega enteramente a **GitHub Actions** (`Build Hispa-OptiScaler-DLSS5`), promediando ~6 minutos de compilación Release x64 limpia. Los binarios se extraen automáticamente de la release `nightly` y se sincronizan a los repositorios de distribución y al juego.
