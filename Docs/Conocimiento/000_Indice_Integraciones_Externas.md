---
tipo: moc
resumen: "Comunicación con APIs, inyectores gráficos, bridges nativos y servicios de terceros."
fecha_creacion: "11/09/2026 01:00:00"
fecha_modificacion: "11/09/2026 03:45:00"
related: []
estado: activo
tags:
  - moc
  - integraciones
  - drivers
  - mods
---

# 000 Indice Integraciones Externas

> [!NOTE]
> Comunicación con APIs, inyectores gráficos, bridges nativos y servicios de terceros.

---

## Notas Vinculadas del Proyecto

| Nota | Descripción | Tecnologías |
| :--- | :--- | :--- |
| [[int_puredark_bridge_y_render_hooks]] | Bridge de PureDark para DLSS 5, hooking NVNGX y bypass criptográfico de licencias. | C++, DirectX 12, MinHook, Crypt32 |
| [[int_hispaoverlay_native_imgui]] | Módulo C++ nativo de overlay con ImGui, hooking VMT en DirectX 11/12 y desbloqueador de cursor Win32. | C++, ImGui, MinHook, Win32 |
| [[int_gestor_librerias_graficas_dlss_fsr_xess]] | Swapper de DLLs de escalado gráfico (DLSS, FSR, XeSS) con backup atómico y hot-swapping. | C#, .NET 9, SQLite, FileStream |
| [[int_nvdrs_nvidia_profile_inspector.md]] | Integración con Driver de NVIDIA mediante reflexión sobre NVIDIA Profile Inspector y manipulación DRS. | C#, .NET 9, NVIDIA DRS, NVAPI |
| [[int_scanners_launchers_y_savegames.md]] | Scanners multi-launcher (Steam, Epic, Ubisoft, GOG), parser PE para proxy ASI y backup de saves. | C#, .NET 9, Win32 PE, VDF, JSON |

---

## Vista Dataview Dinámica

```dataview
TABLE resumen as "Resumen", fecha_modificacion as "Modificado"
FROM "Docs/Conocimiento"
WHERE tipo = "integracion" OR tipo = "integraciones" OR contains(tags, "mcp") OR contains(tags, "integraciones") OR contains(tags, "integracion")
SORT fecha_modificacion DESC
```