---
tipo: integracion
resumen: "Scanners de launchers (Steam, Epic, Ubisoft, GOG), detección PE x86/x64 para proxy ASI y gestión unificada de partidas guardadas."
fecha_creacion: "11/09/2026 03:40:00"
fecha_modificacion: "11/09/2026 03:40:00"
related: ["[[000_Indice_Integraciones_Externas]]", "[[000_Indice_Servicios_y_APIs]]", "[[arq_arquitectura_hispagamecontrol_wpf]]"]
tecnologias: [C#, .NET 9, Win32 PE, VDF Parsing, JSON, ZipArchive]
proveedor_externo: "Steam, Epic Games, Ubisoft Connect, GOG Galaxy"
archivos_claves: ["/Src/DESKTOP/HispaGameControl/Services/LauncherScanners.cs", "/Src/DESKTOP/HispaGameControl/Services/SteamScanner.cs", "/Src/DESKTOP/HispaGameControl/Services/PeHeaderReaderService.cs", "/Src/DESKTOP/HispaGameControl/Services/GameSaveManagerService.cs"]
rutas_claves: ["/Src/DESKTOP/HispaGameControl/Services"]
prioridad_busqueda: 4
tags: [scanners, steam, epic, pe_header, savegames, backup]
criticidad: "Media"
resolucion: "Resuelto"
---

# Scanners de Launchers, Análisis Binario PE e Infraestructura de SaveGames

## 1. Visión General
HispaGameControl desacopla la detección e integración de juegos en Windows a través de tres componentes especializados:
1. **Scanners de Plataformas**: Detección no invasiva de instalaciones locales en Steam, Epic Games Launcher, Ubisoft Connect y GOG Galaxy.
2. **Lector de Cabeceras PE (`PeHeaderReaderService`)**: Análisis estático en disco de binarios Win32/x64 en menos de 1 milisegundo para seleccionar la DLL proxy óptima.
3. **Gestor Universal de Partidas Guardadas (`GameSaveManagerService`)**: Detección de carpetas de guardado, normalización de rutas de slots y copias de seguridad comprimidas en ZIP con versionado temporal.

---

## 2. Detección Multi-Launcher (`LauncherScanners` y `SteamScanner`)

### Steam
- **Ruta Base**: Detectada en el registro de Windows (`HKCU\Software\Valve\Steam` -> `SteamPath`).
- **Librerías Secundarias**: Parsea `steamapps/libraryfolders.vdf` para extraer todos los montajes y discos configurados por el usuario.
- **Identificación de Juegos**: Localiza los archivos `appmanifest_<appid>.acf`. Extrae metadatos clave: `installdir`, `name`, `StateFlags` y `buildid`.
- **Verificación Física**: [`LauncherScanners.CheckIfGameInstalled`](file:///T:/DESARROLLO/GCLOUD/PROYECTOS/HispaGameControl/Src/DESKTOP/HispaGameControl/Services/LauncherScanners.cs#L11-L60) verifica que el directorio exista y contenga al menos un binario `.exe` con tamaño superior a cero bytes.

### Epic Games Store
- Parsea los manifiestos JSON ubicados en `%ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\*.item`.
- Extrae `DisplayName`, `InstallLocation`, `LaunchExecutable`, `AppName` y `CatalogItemId`.

### Ubisoft Connect & GOG Galaxy
- **Ubisoft**: Consulta la clave de registro `HKLM\SOFTWARE\WOW6432Node\Ubisoft\Launcher\Installs` extrayendo el `InstallDir`.
- **GOG**: Lee `HKLM\SOFTWARE\WOW6432Node\GOG.com\Games` identificando `gameID`, `exe` y `path`.

---

## 3. Análisis Binario PE y Selección de DLL Proxy (`PeHeaderReaderService`)

Para inyectar mods o librerías de escalado como OptiScaler u Overlay sin modificar el ejecutable original del juego, se utiliza el patrón de **DLL Proxy / DLL Hijacking**. 

[`PeHeaderReaderService`](file:///T:/DESARROLLO/GCLOUD/PROYECTOS/HispaGameControl/Src/DESKTOP/HispaGameControl/Services/PeHeaderReaderService.cs) lee directamente la cabecera Portable Executable (PE) del juego en disco:

```
[DOS Header: 0x3C (e_lfanew)]
       │
       ▼
[PE Signature: 0x00004550 "PE\0\0"]
       │
       ▼
[File Header: Machine (x86 / x64), NumSections]
       │
       ▼
[Optional Header: Magic (0x010B = PE32, 0x020B = PE32+)]
       │
       ▼
[Data Directory[1]: Import Table RVA & Size]
       │
       ▼
[Section Headers: RVA -> Raw File Offset Mapping]
       │
       ▼
[Import Directory Table: IMAGE_IMPORT_DESCRIPTOR Array]
       │
       ▼
[DLL Names Extraction: string parsing en disco]
```

### Resolución de la DLL Proxy Óptima
El servicio compara la lista de DLLs importadas contra la lista prioritaria `KnownProxyDlls`:
1. `dxgi.dll` (Prioritaria en juegos modernos DirectX 11/12).
2. `version.dll` (Ideal para evitar colisiones con capas gráficas).
3. `winmm.dll` (Común en motores heredados o inyectores secundarios).
4. `dinput8.dll` (Estándar histórico en modding de Capcom y DirectX 9/11).
5. `bink2w64.dll` (Utilizada en juegos que reproducen cinemáticas con Bink Video).
6. `dsound.dll` y `xlive.dll` (Compatibilidad retro).

Si el binario importa `dxgi.dll`, el sistema recomienda o renombra automáticamente el inyector a `dxgi.dll`. Si ya existe otro mod ocupando dicho nombre, recurre automáticamente a la siguiente DLL importada compatible.

---

## 4. Gestión Unificada de SaveGames (`GameSaveManagerService`)

### Normalización de Rutas y Heurística de Slots
Muchos juegos almacenan partidas en subcarpetas dinámicas según el perfil de Steam o slots de autoguardado. [`NormalizeGameSaveRoot`](file:///T:/DESARROLLO/GCLOUD/PROYECTOS/HispaGameControl/Src/DESKTOP/HispaGameControl/Services/GameSaveManagerService.cs#L52-L65) aplica expresiones regulares para truncar subcarpetas volátiles:

```csharp
if (Regex.IsMatch(dirName, @"^(?:AutoSave|ManualSave|QuickSave|Save|Slot|Point|Checkpoint|ACV\[)[\-_0-9A-Za-z]*$", RegexOptions.IgnoreCase))
```
Esto asegura que la raíz de backup capture la colección completa de partidas del jugador y no un único slot transitorio.

### Almacenamiento y Restauración
- Las copias de seguridad se empaquetan en `SaveGames/Backups/<GameTitle>_<yyyyMMdd_HHmmss>.zip`.
- Se genera un archivo de metadatos `save_config.json` con hash de verificación, tamaño total y fecha de captura.
- La restauración incluye reemplazo atómico con backup de contingencia previo para evitar pérdida de progreso si el proceso se interrumpe.

---

## 5. Diccionario de Base de Datos y APIs

- [[LauncherScanners]] [explicacion:: Escáner unificado para verificar presencia de ejecutables en Steam, Epic, Ubisoft y GOG]
- [[SteamScanner]] [explicacion:: Parser especializado de VDF y ACF de Steam para descubrir librerías y aplicaciones]
- [[PeHeaderReaderService]] [explicacion:: Analizador de binarios PE x86/x64 que extrae la IAT y selecciona la DLL proxy óptima]
- [[GameSaveManagerService]] [explicacion:: Gestor de copias de seguridad, compresión ZIP y normalización de rutas de guardado]
