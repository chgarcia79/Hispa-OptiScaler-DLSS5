---
tipo: integracion
resumen: "Mecanismo de orquestación, swapping e inyección de librerías gráficas (DLSS, FSR, XeSS, Streamline) en DlssSwapperService."
fecha_creacion: 2026-09-11
fecha_modificacion: 2026-09-11
related:
  - "[[000_Indice_Integraciones_Externas]]"
  - "[[000_Indice_Servicios_y_APIs]]"
  - "[[arq_arquitectura_hispagamecontrol_wpf]]"
  - "[[script_reconstruccion_librerias_libscomp]]"
estado: activo
---

# Gestor de Librerías Gráficas: DLSS, FSR, XeSS y Streamline

## 1. Visión General
El servicio `DlssSwapperService` actúa como el motor central de gestión de paquetes gráficos. Permite identificar, descargar, verificar y reemplazar en caliente las DLLs de escalado y generación de fotogramas sin riesgo de dañar la instalación vanilla del juego.

Ubicación del código fuente: [Src/DESKTOP/HispaGameControl/Services/DlssSwapperService.cs](file:///T:/DESARROLLO/GCLOUD/PROYECTOS/HispaGameControl/Src/DESKTOP/HispaGameControl/Services/DlssSwapperService.cs)

---

## 2. Mapa de Librerías Gestionadas

| Tecnología | Archivo DLL Principal | Propósito y Versiones Clave |
|---|---|---|
| **DLSS-SR** | `nvngx_dlss.dll` | Reescalado Super Resolution (2.5.1, 3.7.10, 310.9.1) |
| **DLSS-D** | `nvngx_dlssd.dll` | Reconstrucción de rayos Ray Reconstruction (3.5+) |
| **DLSS-G** | `nvngx_dlssg.dll` | Generación de fotogramas Frame Generation (RTX 40+) |
| **DLSS-NR** | `nvngx_dlssnr.dll` | Renderizado Neural DLSS 5 (RTX 50+, NGX Feature 18, ~165 MB) |
| **Streamline** | `sl.interposer.dll`, `sl.common.dll` | Framework modular de NVIDIA (2.13, 2.14, 2.14.1) |
| **AMD FSR** | `amd_fidelityfx_dx12.dll` / `ffx_fsr31_dx12.dll` | FidelityFX Super Resolution (2.2, 3.1.2 a 3.1.5, 4.0.2) |
| **Intel XeSS** | `libxess.dll`, `libxell.dll`, `libxess_fg.dll` | Escalado de red neuronal Intel (1.3.1, 2.0.2, 2.1) |

---

## 3. Protocolo de Inyección y Swapping

### 3.1. Cadena de Inyección de OptiScaler (`ApplyHispaOptiScalerPack`)
1. **Limpieza previa:** Invoca `CleanAllInjectedMods` para retirar shims obsoletos.
2. **Registro de Manifiesto:** Genera `.hispa_neural_manifest.json` y almacena copias de seguridad de cualquier binario preexistente con extensión `.orig`.
3. **Parcheo en Memoria:** Pasa `OptiScaler.dll` por `HexPatcherService.PatchOptiScalerFast` para renombrar cadenas del OSD y traducir menús en memoria.
4. **Encadenamiento con ReShade:** Si se detecta un `dxgi.dll` previo perteneciente a ReShade, se renombra atómicamente a `ReShade64.dll`, se activa `LoadReshade=true` en `OptiScaler.ini` y se posiciona OptiScaler como el `dxgi.dll` receptor primario.
5. **Inyección de Shims:** Despliega el forwarder `nvngx.dll_dlssnr.dll` (~13 KB) y la subcarpeta `OptiScaler\streamline\` con las librerías oficiales de Streamline y Frame Generation.
6. **Despliegue del Modelo Neural:** Copia `nvngx_dlssnr.dll` si está disponible.

### 3.2. Preservación de Streamline Nativo (`EnsureNativeStreamlineRestored`)
Para evitar que juegos con soporte nativo de Streamline (como Cyberpunk 2077 o Horizon Forbidden West) crasheen al inicio si se eliminan sus dependencias:
- El servicio consulta la IAT vía `PeHeaderReaderService` para comprobar si el ejecutable importa `sl.interposer.dll`.
- Si se detecta soporte nativo, durante la desinstalación de mods **nunca elimina** las DLLs de Streamline, o las reensambla inmediatamente desde el repositorio `Repository/Streamline/`.
