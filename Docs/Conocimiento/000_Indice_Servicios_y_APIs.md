---
tipo: moc
resumen: "Servicios internos de la aplicación, utilidades de bajo nivel y controladores de subsistemas."
fecha_creacion: "11/09/2026 01:00:00"
fecha_modificacion: "11/09/2026 03:45:00"
related: []
estado: activo
tags:
  - moc
  - apis
  - servicios
---

# 000 Indice Servicios y APIs

> [!NOTE]
> Servicios internos de la aplicación, utilidades de bajo nivel y controladores de subsistemas.

---

## Notas Vinculadas del Proyecto

| Nota | Descripción | Tecnologías |
| :--- | :--- | :--- |
| [[int_nvdrs_nvidia_profile_inspector]] | Servicio C# para manipulación de DRS (Driver Profile Settings) de NVIDIA vía reflexión. | C#, .NET 9, NVIDIA DRS |
| [[int_scanners_launchers_y_savegames]] | Servicios de descubrimiento de juegos, escáner PE para proxy DLL y backups de partidas. | C#, .NET 9, Win32 PE, JSON |
| [[int_gestor_librerias_graficas_dlss_fsr_xess]] | Servicio C# para swapper de librerías DLL y preservación de backups originales. | C#, .NET 9, FileStream |

---

## Vista Dataview Dinámica

```dataview
TABLE resumen as "Resumen", fecha_modificacion as "Modificado"
FROM "Docs/Conocimiento"
WHERE tipo = "api" OR tipo = "apis" OR tipo = "servicio" OR contains(tags, "servicios") OR contains(tags, "api")
SORT fecha_modificacion DESC
```