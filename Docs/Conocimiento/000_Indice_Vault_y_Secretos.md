---
tipo: moc
resumen: Mapa referencial de credenciales y secretos (sin exponer contraseÃ±as ni tokens).
fecha_creacion: 2026-09-11
fecha_modificacion: 2026-09-11
related: []
estado: activo
tags:
  - moc
  - vault
  - secretos
---

# 000 Indice Vault y Secretos

> [!NOTE]
> Mapa referencial de credenciales y secretos (sin exponer contraseÃ±as ni tokens).

---

## Notas Vinculadas

``dataview
TABLE resumen as "Resumen", fecha_modificacion as "Modificado"
FROM "Docs/Conocimiento"
WHERE tipo = "vault" OR contains(tags, "secretos")
SORT fecha_modificacion DESC
``