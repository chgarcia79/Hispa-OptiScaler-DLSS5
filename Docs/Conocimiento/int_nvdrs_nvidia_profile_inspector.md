---
tipo: integracion
resumen: "Integración de bajo nivel con el Driver de NVIDIA mediante reflexión C# sobre NVIDIA Profile Inspector y manipulación en memoria de DWORDs DRS."
fecha_creacion: "11/09/2026 03:40:00"
fecha_modificacion: "11/09/2026 03:40:00"
related: ["[[000_Indice_Integraciones_Externas]]", "[[000_Indice_Servicios_y_APIs]]", "[[arq_arquitectura_hispagamecontrol_wpf]]"]
tecnologias: [C#, .NET 9, NVIDIA DRS, NVAPI, Reflection]
proveedor_externo: "NVIDIA Profile Inspector"
archivos_claves: ["/Src/DESKTOP/HispaGameControl/Services/NvDrsService.cs"]
rutas_claves: ["/Src/DESKTOP/HispaGameControl/Services"]
prioridad_busqueda: 5
tags: [nvidia, drs, profile_inspector, nvapi, dlss, reflex]
criticidad: "Alta"
resolucion: "Resuelto"
---

# Integración de Bajo Nivel con NVIDIA Driver Settings (DRS) vía NvDrsService

## 1. Visión General
El servicio [`NvDrsService`](file:///T:/DESARROLLO/GCLOUD/PROYECTOS/HispaGameControl/Src/DESKTOP/HispaGameControl/Services/NvDrsService.cs) proporciona control programático directo sobre los perfiles de configuración de drivers NVIDIA (Driver Profile Settings / DRS). A diferencia de los enfoques tradicionales basados en ejecutar la CLI de herramientas externas o requerir la interacción con el panel de control de NVIDIA, HispaGameControl carga y manipula el motor de `nvidiaProfileInspector.exe` mediante reflexión dinámica en C# (`System.Reflection`) o llamadas directas a las APIs nativas de NVAPI (`nvapi64.dll`).

Esto permite consultar, crear perfiles por ejecutable y modificar configuraciones avanzadas del hardware en microsegundos y sin ventanas emergentes.

---

## 2. IDs de Configuración DRS Críticos

`NvDrsService` mapea las constantes de bajo nivel del driver gráfico para DLSS, Frame Generation, sincronización vertical y gestión energética:

| Categoría | Nombre Constante en C# | Setting ID (Hex) | Tipo / Función |
| :--- | :--- | :--- | :--- |
| **DLSS-SR** | `ID_DLSS_SR_OVERRIDE_DLL` | `0x10E41E01` | Inyección de DLL personalizada de Super Resolution |
| **DLSS-SR** | `ID_DLSS_SR_MODEL_PROFILE`| `0x00634291` | Perfil de red neuronal DLSS (A, B, C, D, E, F) |
| **DLSS-SR** | `ID_DLSS_SR_PRESET`       | `0x10E41DF3` | Preset de renderizado SR |
| **DLSS-SR** | `ID_DLSS_SR_RATIO`        | `0x10E41DF5` | Escala porcentual personalizada (Ultra Quality / DLDSR) |
| **DLSS-SR** | `ID_DLSS_SR_QUALITY_LEVEL`| `0x10AFB768` | Nivel de calidad por defecto |
| **DLSS-RR** | `ID_DLSS_RR_OVERRIDE_DLL` | `0x10E41E02` | Inyección de Ray Reconstruction (`nvngx_dlssd.dll`) |
| **DLSS-RR** | `ID_DLSS_RR_PRESET`       | `0x10E41DF7` | Preset de Ray Reconstruction |
| **DLSS-RR** | `ID_DLSS_RR_RATIO`        | `0x10C7D4A2` | Escala de RR |
| **DLSS-NR** | `ID_DLSS_NR_OVERRIDE_DLL` | `0x10E41E04` | Inyección de Neural Rendering DLSS 5 (`nvngx_dlssnr.dll`) |
| **DLSS-NR** | `ID_DLSS_NR_PRESET`       | `0x10E41DF8` | Preset de DLSS 5 Neural Rendering |
| **DLSS-FG** | `ID_DLSS_FG_OVERRIDE_DLL` | `0x10E41E03` | Inyección de Frame Generation (`nvngx_dlssg.dll`) |
| **Sync**    | `ID_VSYNC_MODE`           | `0x108343C0` | Forzar VSync (Fast Sync, On, Off) |
| **Sync**    | `ID_GSYNC_MODE`           | `0x10AA9E8A` | Activar VRR / G-SYNC por juego |
| **Power**   | `ID_POWER_MANAGEMENT`     | `0x1057EB71` | Preferir rendimiento máximo (0x01) |
| **Latencia**| `ID_REFLEX_MODE`          | `0x108343C1` | NVIDIA Reflex Low Latency (On + Boost) |

---

## 3. Arquitectura de Invocación por Reflexión

Para evitar dependencias nativas rígidas o fallos de compilación si la versión del binario de Profile Inspector varía:

1. **Localización del Binario**: El servicio localiza `nvidiaProfileInspector.exe` en la carpeta de herramientas embebidas del proyecto.
2. **Carga en AppDomain / Assembly**:
   ```csharp
   Assembly inspectorAssembly = Assembly.LoadFrom(inspectorPath);
   Type drsSessionType = inspectorAssembly.GetType("nvidiaProfileInspector.DrsService");
   ```
3. **Inicialización de Sesión DRS**:
   - Se instancia el gestor de sesión `DrsSession`.
   - Se invoca `FindProfile(gameTitle)` o `CreateProfile(gameTitle, exePath)`.
4. **Escritura Transaccional de DWORDs**:
   - Cada parámetro se asigna mediante `SetSetting(settingId, dwordValue)`.
   - Se invoca `SaveSettings()` para persistir los cambios en la caché del driver NVIDIA (`C:\ProgramData\NVIDIA Corporation\Drs\nvdrssel.bin`).
5. **Liberación de Manijas**: La sesión se cierra liberando el lock del driver para permitir que el juego tome las directivas de inmediato sin reiniciar el sistema operativo.

---

## 4. Perfiles Preconfigurados en HispaGameControl

El modelo [`NvProfileData`](file:///T:/DESARROLLO/GCLOUD/PROYECTOS/HispaGameControl/Src/DESKTOP/HispaGameControl/Services/NvDrsService.cs#L8-L40) soporta los siguientes estados de sincronización y rendimiento:

- **Modo Máximo Rendimiento**: Desactiva el ahorro de energía PCI Express, forzando clocks máximos en GPUs Ada Lovelace y Blackwell.
- **Sincronización VRR Óptima**: Activa G-SYNC en modo ventana y pantalla completa (`0x00000001`), fuerza VSync a nivel driver (`0x108343C0 = 0x6108343C`) y habilita Reflex On + Boost para eliminar el lag de entrada derivado de buffers de cola.
- **Sobrescritura Forzada de Modelos Neuronales**: Configura `ID_DLSS_SR_PRESET = 0x00000006` (Preset F/J) para reducir ghosting en simuladores de carreras y shooters en primera persona.

---

## 5. Diccionario de Base de Datos y APIs

- [[NvDrsService]] [explicacion:: Servicio C# para lectura y escritura transaccional de DWORDs del driver NVIDIA DRS en memoria]
- [[NvProfileData]] [explicacion:: Modelo DTO que encapsula el estado de sincronización vertical, G-Sync, perfiles energéticos y presets DLSS]
- [[nvidiaProfileInspector]] [explicacion:: Ensamblado .NET externo inspeccionado vía Reflection para ejecutar comandos DRS sin CLI]
