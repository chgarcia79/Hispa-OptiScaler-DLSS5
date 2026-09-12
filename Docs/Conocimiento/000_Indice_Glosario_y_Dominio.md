---
tipo: moc
resumen: Diccionario de acrÃ³nimos, tÃ©rminos de dominio y conceptos especÃ­ficos del negocio.
fecha_creacion: 2026-09-11
fecha_modificacion: 2026-09-11
related: []
estado: activo
tags:
  - moc
  - glosario
---

# 000 Indice Glosario y Dominio

> [!NOTE]
> Diccionario de acrÃ³nimos, tÃ©rminos de dominio y conceptos especÃ­ficos del negocio.

---

## Notas Vinculadas

``dataview
TABLE resumen as "Resumen", fecha_modificacion as "Modificado"
FROM "Docs/Conocimiento"
WHERE tipo = "glosario" OR contains(tags, "dominio")
SORT fecha_modificacion DESC
``