---
tipo: moc
resumen: "Estructura de clases, patrones de diseño de software, inyección de dependencias y decisiones de infraestructura."
fecha_creacion: "11/09/2026 01:00:00"
fecha_modificacion: "11/09/2026 03:45:00"
related: []
estado: activo
tags:
  - moc
  - arquitectura
---

# 000 Indice Arquitectura y Diseno

> [!NOTE]
> Estructura de clases, patrones de diseño de software, inyección de dependencias y decisiones de infraestructura.

---

## Notas Vinculadas del Proyecto

| Nota | Descripción | Tecnologías |
| :--- | :--- | :--- |
| [[arq_arquitectura_hispagamecontrol_wpf]] | Arquitectura MVVM C# WPF en .NET 9, inyección DI, persistencia SQLite y módulos de escalado. | C#, .NET 9, WPF, SQLite |
| [[arq_motor_hispaoptiscaler_dlss5]] | Motor C++ de interposición gráfica (DirectX/Vulkan), spoofing de GPU y soporte DLSS 5 Neural Rendering. | C++, DirectX 11/12, Vulkan, DLSS-NR |

---

## Vista Dataview Dinámica

```dataview
TABLE resumen as "Resumen", fecha_modificacion as "Modificado"
FROM "Docs/Conocimiento"
WHERE tipo = "arquitectura" OR contains(tags, "arquitectura")
SORT fecha_modificacion DESC
```