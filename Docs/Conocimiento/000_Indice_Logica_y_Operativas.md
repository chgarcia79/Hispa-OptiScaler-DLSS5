---
tipo: moc
resumen: Flujos de trabajo funcionales, condiciones operativas, reglas de negocio y cÃ¡lculos matemÃ¡ticos.
fecha_creacion: 2026-09-11
fecha_modificacion: 2026-09-11
related: []
estado: activo
tags:
  - moc
  - logica
---

# 000 Indice Logica y Operativas

> [!NOTE]
> Flujos de trabajo funcionales, condiciones operativas, reglas de negocio y cÃ¡lculos matemÃ¡ticos.

---

## Notas Vinculadas

``dataview
TABLE resumen as "Resumen", fecha_modificacion as "Modificado"
FROM "Docs/Conocimiento"
WHERE tipo = "logica" OR contains(tags, "operativas")
SORT fecha_modificacion DESC
``