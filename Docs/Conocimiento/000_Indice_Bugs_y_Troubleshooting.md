---
tipo: moc
resumen: "Registro de incidencias críticas resueltas, post-mortems y soluciones a cuelgues o incompatibilidades."
fecha_creacion: "11/09/2026 01:00:00"
fecha_modificacion: "11/09/2026 03:45:00"
related: []
estado: activo
tags:
  - moc
  - bugs
  - troubleshooting
---

# 000 Indice Bugs y Troubleshooting

> [!NOTE]
> Registro de incidencias críticas resueltas, post-mortems y soluciones a cuelgues o incompatibilidades.

---

## Notas Vinculadas del Proyecto

| Nota | Descripción | Severidad | Estado |
| :--- | :--- | :--- | :--- |
| [[bug_cyberpunk2077_optimizacion_dlss5_y_crash_settings]] | Resolución de crash en menú de opciones por options.json DualSense, matriz de DLLs y solución a caídas de FPS a 100 Hz con Streamline. | Crítica | Resuelto |

---

## Vista Dataview Dinámica

```dataview
TABLE resumen as "Resumen", criticidad as "Criticidad", estado as "Estado", fecha_modificacion as "Modificado"
FROM "Docs/Conocimiento"
WHERE tipo = "bug" OR contains(tags, "troubleshooting") OR contains(tags, "bug")
SORT fecha_modificacion DESC
```