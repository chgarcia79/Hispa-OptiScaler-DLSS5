---
tipo: script
resumen: "Skill y conjunto de scripts en PowerShell para exportación, importación, edición y control de versiones Git de perfiles NVIDIA (.nip) mediante NVIDIA Profile Inspector."
fecha_creacion: "2026-09-11 04:47:00"
fecha_modificacion: "2026-09-11 05:30:00"
related: ["[[000_Indice_Operaciones_y_Scripts]]", "[[000_Indice_IA_Prompts_y_Agentes]]", "[[int_nvdrs_nvidia_profile_inspector]]"]
tecnologias: [PowerShell, XML, NVIDIA DRS, Git, REST API GitHub, .NET Reflection]
proveedor_externo: "Orbmu2k/nvidiaProfileInspector"
archivos_claves:
  - .agent/skills/nvidia-profile-manager/SKILL.md
  - Resources/Profiles/Nvidia/README.md
  - Resources/Profiles/Nvidia/FullBackups/Nvidia_All_Customized_Profiles.nip
rutas_claves:
  - .agent/skills/nvidia-profile-manager
  - Resources/Profiles/Nvidia
prioridad_busqueda: 5
tags: [nvidia, profiles, backup, git, nip, restore, drs, reflection]
criticidad: "Media"
resolucion: "Resuelto"
---

# Gestión Automatizada de Perfiles NVIDIA (.nip) con nvidia-profile-manager

## 1. Visión General
Esta skill y su suite de utilidades en PowerShell resuelven el reseteo recurrente de configuraciones personalizadas del driver de NVIDIA tras cada actualización del software (`nvdrssel.bin`). Centraliza el almacenamiento en `Resources/Profiles/Nvidia/` con nombres de archivo estables para facilitar el seguimiento de diferencias (*diffs*) en Git y GitHub.

---

## 2. Componentes de la Suite

### 1. Diccionario Oficial de Parámetros
* **Ruta:** `.agent/skills/nvidia-profile-manager/references/CustomSettingNames.xml`
* **Origen:** Repositorio upstream de *Orbmu2k*.
* **Función:** Define los nombres amigables, IDs hexadecimales, categorías y valores permitidos para cada setting del driver.

### 2. Scripts Operativos
* **`Find-NvidiaInspector.ps1`:** Localización desacoplada de `nvidiaProfileInspector.exe` priorizando `C:\Soft\Juegos\nvidiaProfileInspector\nvidiaProfileInspector.exe`, seguido de rutas relativas, `PATH`, descargas o variables de entorno.
* **`Backup-NvidiaProfiles.ps1`:** Exportación individual por juego (`Games/<Juego>.nip`), volcado completo del driver (`FullBackups/Nvidia_All_Profiles.nip`) o todos los perfiles modificados (`-Customized` -> `FullBackups/Nvidia_All_Customized_Profiles.nip`).
* **`Restore-NvidiaProfiles.ps1`:** Inyección silenciosa de configuraciones en caliente hacia el driver.
* **`Check-SettingNamesUpdate.ps1`:** Comparación de hash SHA256 contra la API de GitHub para alertar de nuevos flags de NVIDIA.
* **`Set-NvidiaProfileSetting.ps1`:** Manipulación quirúrgica de XML para añadir, editar o eliminar parámetros (reseteo a defaults).

---

## 3. Arquitectura de Invocación por Reflection (.NET)
Para eludir el control de cuentas de usuario (UAC) de Windows que `nvidiaProfileInspector.exe` exige al lanzarse como proceso independiente, los scripts de la skill implementan carga en memoria mediante reflexión:

```powershell
$asm = [System.Reflection.Assembly]::LoadFrom($exePath)
$importType = $asm.GetType("nvidiaProfileInspector.Common.DrsImportService")
$import = [Activator]::CreateInstance($importType, $meta, $drs, $scanner, $decrypter)

# Exportación directa en memoria
$import.ExportProfiles($list, $targetFile, $false)

# Importación directa al driver
$import.ImportProfiles($sourceFile)
```

### Ventajas Técnicas:
1. **Cero UAC Popups:** No solicita elevación de administrador ni interrumpe la sesión.
2. **Ultra-Rápido:** Exporta o importa en menos de 1 segundo directamente contra NVAPI.
3. **Fallback CLI:** Si la reflexión encontrara una versión con firmas incompatibles, recurre automáticamente a `-silent -import` o `-export`.

---

## 4. Inventario de Perfiles Respaldados en Git (`Resources/Profiles/Nvidia/`)

| Perfil / Juego | Ubicación | Ajustes Guardados |
| :--- | :--- | :---: |
| **Cyberpunk 2077** | `Games/Cyberpunk 2077.nip` | 24 |
| **Crimson Desert** | `Games/Crimson Desert.nip` | 20 |
| **Avatar: Frontiers of Pandora** | `Games/Avatar_ Frontiers of Pandora.nip` | 16 |
| **STAR WARS Jedi: Survivor** | `Games/STAR WARS Jedi_ Survivor.nip` | 10 |
| **Assassin's Creed Valhalla** | `Games/Assassin's Creed Valhalla.nip` | 7 |
| **Shadow of the Tomb Raider** | `Games/Shadow of the Tomb Raider.nip` | 4 |
| **God of War** | `Games/God of War.nip` | 1 |
| **Valve Steam WebHelper** | `Games/Valve Steam WebHelper.nip` | 1 |
| **Base Profile (Global)** | `FullBackups/Base Profile.nip` | 5 |
| **Bundle Completo Modificados** | `FullBackups/Nvidia_All_Customized_Profiles.nip` | Consolidado |

---

## 5. Relación con NvDrsService
Mientras que [`NvDrsService.cs`](file:///T:/DESARROLLO/GCLOUD/PROYECTOS/HispaGameControl/Src/DESKTOP/HispaGameControl/Services/NvDrsService.cs) proporciona integración en tiempo de ejecución C# para la app WPF, esta skill actúa como la capa de mantenimiento y orquestación externa para el agente y el desarrollador.
