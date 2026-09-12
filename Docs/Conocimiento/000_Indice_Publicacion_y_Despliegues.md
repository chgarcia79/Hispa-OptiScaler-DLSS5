---
tipo: moc
resumen: ConfiguraciÃ³n de Docker, Dokploy, CI/CD, Kestrel, Proxies Inversos y paso a producciÃ³n.
fecha_creacion: 2026-09-11
fecha_modificacion: "11/09/2026 05:30:00"
related: []
estado: activo
tags:
  - moc
  - despliegues
  - devops
---

# 000 Indice Publicacion y Despliegues

> [!NOTE]
> Configuración de despliegues locales, producción, binarios y paso a producción.

---

## Notas Vinculadas del Proyecto

| Nota | Descripción | Tecnologías |
| :--- | :--- | :--- |
| [[desp_publicacion_local_desktop_hispagamecontrol]] | Directiva de publicación continua en el entorno operativo local C:\Soft\Juegos\HispaGameControl\ tras compilación limpia y estable. | .NET 9, WPF, C#, dotnet publish |

---

## Vista Dataview Dinámica

```dataview
TABLE resumen as "Resumen", fecha_modificacion as "Modificado"
FROM "Docs/Conocimiento"
WHERE tipo = "despliegue" OR tipo = "despliegues" OR contains(tags, "despliegues") OR contains(tags, "publish")
SORT fecha_modificacion DESC
```