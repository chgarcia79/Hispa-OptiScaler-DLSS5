---
tipo: moc
resumen: Sistemas de clientes, bases de cÃ³digo de terceros, ingenierÃ­a inversa o scraping.
fecha_creacion: 2026-09-11
fecha_modificacion: 2026-09-11
related: []
estado: activo
tags:
  - moc
  - externos
---

# 000 Indice Proyectos Externos y Terceros

> [!NOTE]
> Sistemas de clientes, bases de cÃ³digo de terceros, ingenierÃ­a inversa o scraping.

---

## Notas Vinculadas

``dataview
TABLE resumen as "Resumen", fecha_modificacion as "Modificado"
FROM "Docs/Conocimiento"
WHERE tipo = "externo" OR contains(tags, "terceros")
SORT fecha_modificacion DESC
``