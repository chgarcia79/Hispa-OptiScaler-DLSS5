---
tipo: arquitectura
resumen: "Arquitectura modular en C# .NET 9 WPF de HispaGameControl, servicios desacoplados, inspección PE e integración con SQLite."
fecha_creacion: 2026-09-11
fecha_modificacion: 2026-09-11
related:
  - "[[000_Indice_Arquitectura_y_Diseno]]"
  - "[[000_Indice_Servicios_y_APIs]]"
  - "[[arq_motor_hispaoptiscaler_dlss5]]"
estado: activo
---

# Arquitectura General de HispaGameControl (C# WPF .NET 9)

## 1. Visión General y Patrón de Diseño
HispaGameControl está implementado como una aplicación de escritorio moderna sobre **.NET 9 Windows (WPF)** con arquitectura desacoplada basada en servicios (`Services/`), módulos (`Modules/`), vistas MVVM (`ViewModels/`, `Views/`) y persistencia dual (SQLite + manifiestos transaccionales JSON).

Ubicación del código fuente: [Src/DESKTOP/HispaGameControl/](file:///T:/DESARROLLO/GCLOUD/PROYECTOS/HispaGameControl/Src/DESKTOP/HispaGameControl)

### Capas del Sistema
```
┌─────────────────────────────────────────────────────────────┐
│                    Capa de Presentación                     │
│        MainWindow.xaml, FilterChipsBar, FooterStatusBar     │
│        Diálogos: GameDetailsDialog, VersionManagerDialog    │
├─────────────────────────────────────────────────────────────┤
│                  Capa de Lógica / ViewModels                │
│                 MainViewModel, RelayCommand                 │
├──────────────────────────────┬──────────────────────────────┤
│       Capa de Módulos        │      Capa de Servicios       │
│  DlssModule, OptiScalerModule│  PeHeaderReaderService       │
│  RenoDXModule, HispaOverlay  │  DlssSwapperService          │
│  (Implementan IModModule)    │  ModManagerService           │
│                              │  NvDrsService, SteamScanner  │
├──────────────────────────────┴──────────────────────────────┤
│                Capa de Acceso a Datos / I/O                 │
│    SQLite (dlss_swapper.db), JSON Manifests, BinaryReader   │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. Servicios Principales (`Services/`)

### 2.1. Inspección Quirúrgica de Encabezados PE (`PeHeaderReaderService.cs`)
- **Objetivo:** Analizar ejecutables Windows (PE32 y PE32+) en tiempo récord (~1 ms) sin cargar la DLL ni ejecutar procesos.
- **Implementación:** Lee `e_lfanew` (offset 0x3C), valida la firma `IMAGE_NT_SIGNATURE` y recorre el `IMAGE_DATA_DIRECTORY` para mapear el `Import Directory` a través de los `Section Headers`. Extrae descriptores `IMAGE_IMPORT_DESCRIPTOR` para obtener la lista de DLLs importadas.
- **Resolución ASI Proxy:** Clasifica la DLL de inyección óptima para cada juego (`dinput8.dll`, `version.dll`, `winmm.dll`, `bink2w64.dll` o `dxgi.dll`).

### 2.2. Orquestador de Intercambio Gráfico (`DlssSwapperService.cs`)
- **Gestión de librerías:** Escanea repositorios locales de DLSS, DLSS-G, DLSS-D, DLSS-NR y Streamline.
- **Cadena de inyección transaccional:** Registra `.hispa_neural_manifest.json`, crea respaldos `.orig`, renombra `dxgi.dll` a `ReShade64.dll` si ReShade está presente, y despliega el forwarder `nvngx.dll_dlssnr.dll` junto con el modelo neural.

### 2.3. Parches Binarios en Memoria (`HexPatcherService.cs`)
- Modifica strings y textos dentro de binarios compilados (`OptiScaler.dll`) aplicando búsqueda de patrones y relleno con espacios (`0x20`) para no alterar los offsets de sección PE, localizando menús y opciones en vivo al español.

### 2.4. Diagnóstico y Telemetría de Driver (`NvDrsService.cs` y `NvidiaDiagnosticsService.cs`)
- **Reflexión DRS:** Carga dinámicamente `nvidiaProfileInspector.exe` mediante `Assembly.LoadFrom` para leer y escribir constantes DRS del driver de NVIDIA (`0x10E41DF3`, `0x10E41DF8`, etc.) directamente en memoria sin scripts externos.
- **WMI:** Consulta `Win32_VideoController` para extraer versión real del driver, VRAM dedicada, ReBAR y estado de VSync/G-Sync.

---

## 3. Diccionario de Base de Datos y Persistencia
- [[dlss_swapper.db]] [explicacion:: Base de datos SQLite local de DLSS Swapper consumida por HistoryService mediante Microsoft.Data.Sqlite]
- [[History]] [explicacion:: Tabla SQLite con el histórico de eventos de intercambio de librerías (GameId, EventType, ResourceType, Version, Created)]
- [[Games]] [explicacion:: Tabla SQLite de juegos catalogados con ID y título oficial]
- [[.mods_manifest.json]] [explicacion:: Manifiesto JSON atómico por juego para rollback limpio de mods sin colisiones]
- [[.hispa_neural_manifest.json]] [explicacion:: Manifiesto de archivos inyectados por el pack DLSS5/OptiScaler para restauración vanilla]
