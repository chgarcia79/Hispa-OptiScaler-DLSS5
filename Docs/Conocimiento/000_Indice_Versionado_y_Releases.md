---
tipo: moc
resumen: Changelogs, Release Notes, historial cronolÃ³gico de versiones y migraciones.
fecha_creacion: 2026-09-11
fecha_modificacion: 2026-09-11
related: []
estado: activo
tags:
  - moc
  - releases
---

# 000 Indice Versionado y Releases

> [!NOTE]
> Changelogs, Release Notes, historial cronolÃ³gico de versiones y migraciones.

---

## Notas Vinculadas

``dataview
TABLE resumen as "Resumen", fecha_modificacion as "Modificado"
FROM "Docs/Conocimiento"
WHERE tipo = "release" OR contains(tags, "changelog")
SORT fecha_modificacion DESC
``