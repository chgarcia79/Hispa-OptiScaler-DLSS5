---
tipo: moc
resumen: "Scripts de automatización, mantenimiento, empaquetado y utilidades del sistema."
fecha_creacion: "11/09/2026 01:00:00"
fecha_modificacion: "11/09/2026 04:47:00"
related: []
estado: activo
tags:
  - moc
  - scripts
  - operaciones
---

# 000 Indice Operaciones y Scripts

> [!NOTE]
> Scripts de automatización, mantenimiento, empaquetado y utilidades del sistema.

---

## Notas Vinculadas del Proyecto

| Nota | Descripción | Tecnologías |
| :--- | :--- | :--- |
| [[script_reconstruccion_librerias_libscomp]] | Reconstrucción automatizada de librerías fragmentadas en Libscomp (DLSS-NR y Streamline) para evasión de límites de Git. | PowerShell, FileStream, ZipArchive |
| [[script_nvidia_profile_manager_skill]] | Suite en PowerShell y skill para exportación, importación, edición y control de versiones Git de perfiles NVIDIA (.nip). | PowerShell, XML, NVIDIA DRS, Git |

---

## Vista Dataview Dinámica

```dataview
TABLE resumen as "Resumen", fecha_modificacion as "Modificado"
FROM "Docs/Conocimiento"
WHERE tipo = "script" OR contains(tags, "operaciones") OR contains(tags, "scripts")
SORT fecha_modificacion DESC
```