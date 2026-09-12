---
tipo: moc
resumen: PolÃ­ticas RLS, gestiÃ³n de JWT, CORS, encriptaciÃ³n, roles y permisos de acceso.
fecha_creacion: 2026-09-11
fecha_modificacion: 2026-09-11
related: []
estado: activo
tags:
  - moc
  - seguridad
---

# 000 Indice Seguridad y Autenticacion

> [!NOTE]
> PolÃ­ticas RLS, gestiÃ³n de JWT, CORS, encriptaciÃ³n, roles y permisos de acceso.

---

## Notas Vinculadas

``dataview
TABLE resumen as "Resumen", fecha_modificacion as "Modificado"
FROM "Docs/Conocimiento"
WHERE tipo = "seguridad" OR contains(tags, "autenticacion")
SORT fecha_modificacion DESC
``