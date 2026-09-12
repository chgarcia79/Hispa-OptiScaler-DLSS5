---
tipo: moc
resumen: Esquemas de tablas, scripts SQL, Edge Functions (Supabase) y Stored Procedures.
fecha_creacion: 2026-09-11
fecha_modificacion: 2026-09-11
related: []
estado: activo
tags:
  - moc
  - database
  - sql
---

# 000 Indice Base Datos y Funciones

> [!NOTE]
> Esquemas de tablas, scripts SQL, Edge Functions (Supabase) y Stored Procedures.

---

## Notas Vinculadas

``dataview
TABLE resumen as "Resumen", fecha_modificacion as "Modificado"
FROM "Docs/Conocimiento"
WHERE tipo = "database" OR contains(tags, "sql")
SORT fecha_modificacion DESC
``